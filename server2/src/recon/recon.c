#include "recon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* History ring: records are processed HIST/2 samples late so the delay may be +-RECON_MAX_DELAY. */
#define HIST      256
#define HIST_MASK (HIST - 1)
#define LAG       (RECON_MAX_DELAY + 2)

typedef enum { SLOW_RISING, SLOW_FALLING } slow_state_t;

struct recon {
    recon_cfg_t cfg;
    frame_cb_t  cb;
    void       *ctx;

    /* history of positions (y already calibrated) and detector values */
    double      hx[HIST], hy[HIST], hv[HIST];
    uint64_t    n_in;              /* records received so far */

    /* slow-axis turn detection */
    slow_state_t slow;
    double       slow_ext;         /* running max (rising) or min (falling) */
    double       slow_hyst;
    int          synced;           /* seen at least one turn: frames are complete */

    int          fast_dir;         /* +1 / -1 */

    double       sx, sy;           /* rows per count, columns per count */
    int          n_img;
    size_t       npix;             /* width * height */
    double      *sum;
    uint32_t    *cnt;
    float       *out;

    uint32_t     frame_id;
    uint64_t     f_records, f_used;
};

recon_t *recon_create(const recon_cfg_t *cfg, frame_cb_t cb, void *ctx)
{
    if (!cfg || !cb || cfg->width == 0 || cfg->height == 0 ||
        !(cfg->x_hi > cfg->x_lo) || !(cfg->y_hi > cfg->y_lo) ||
        fabs(cfg->delay) > RECON_MAX_DELAY || cfg->dir_window == 0 ||
        cfg->dir_window > RECON_MAX_DELAY)
        return NULL;

    recon_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->cfg = *cfg;
    r->cb = cb;
    r->ctx = ctx;
    r->n_img = cfg->bidir == BIDIR_BOTH ? 2 : 1;
    r->npix = (size_t)cfg->width * cfg->height;
    r->sx = cfg->height / (cfg->x_hi - cfg->x_lo);
    r->sy = cfg->width / (cfg->y_hi - cfg->y_lo);
    r->slow_hyst = fmax(0.5, 0.001 * (cfg->x_hi - cfg->x_lo));   /* slow command is exact (integer counts) */
    r->fast_dir = 1;

    size_t n = r->npix * r->n_img;
    r->sum = calloc(n, sizeof(*r->sum));
    r->cnt = calloc(n, sizeof(*r->cnt));
    r->out = malloc(n * sizeof(*r->out));
    if (!r->sum || !r->cnt || !r->out) {
        recon_destroy(r);
        return NULL;
    }
    return r;
}

void recon_destroy(recon_t *r)
{
    if (!r)
        return;
    free(r->sum);
    free(r->cnt);
    free(r->out);
    free(r);
}

static void clear_accum(recon_t *r)
{
    size_t n = r->npix * r->n_img;
    memset(r->sum, 0, n * sizeof(*r->sum));
    memset(r->cnt, 0, n * sizeof(*r->cnt));
    r->f_records = 0;
    r->f_used = 0;
}

void recon_resync(recon_t *r)
{
    clear_accum(r);
    r->synced = 0;
    r->n_in = 0;
}

static void finish_frame(recon_t *r, int slow_dir)
{
    size_t   n = r->npix * r->n_img;
    uint32_t empty = 0;

    for (size_t i = 0; i < n; i++) {
        if (r->cnt[i]) {
            r->out[i] = (float)(r->sum[i] / r->cnt[i]);
        } else {
            r->out[i] = NAN;
            empty++;
        }
    }

    frame_t f = {
        .frame_id = r->frame_id++,
        .width = r->cfg.width,
        .height = r->cfg.height,
        .n_images = (uint8_t)r->n_img,
        .slow_dir = (int8_t)slow_dir,
        .n_records = r->f_records,
        .n_used = r->f_used,
        .n_empty = empty,
        .data = r->out,
    };
    r->cb(r->ctx, &f);
    clear_accum(r);
}

/* Linear interpolation in the history ring at fractional index t (absolute record index). */
static double hist_at(const double *h, double t)
{
    double   fl = floor(t);
    double   fr = t - fl;
    uint64_t i = (uint64_t)fl;
    return h[i & HIST_MASK] * (1.0 - fr) + h[(i + 1) & HIST_MASK] * fr;
}

/* Process the record at absolute index j (at least LAG records behind the newest). */
static void process(recon_t *r, uint64_t j)
{
    const recon_cfg_t *c = &r->cfg;
    double t = (double)j - c->delay;
    double x = hist_at(r->hx, t);
    double y = hist_at(r->hy, t);
    double v = r->hv[j & HIST_MASK];

    /* slow-axis turn detection with hysteresis on the running extreme */
    int accumulate;
    if (r->slow == SLOW_RISING) {
        if (x > r->slow_ext) {
            r->slow_ext = x;
        } else if (r->slow_ext - x > r->slow_hyst) {
            if (r->synced)
                finish_frame(r, +1);
            else
                clear_accum(r);
            r->synced = 1;
            r->slow = SLOW_FALLING;
            r->slow_ext = x;
        }
    } else {
        if (x < r->slow_ext) {
            r->slow_ext = x;
        } else if (x - r->slow_ext > r->slow_hyst) {
            if (c->slow_bidir && r->synced)
                finish_frame(r, -1);
            else
                clear_accum(r);
            r->synced = 1;
            r->slow = SLOW_RISING;
            r->slow_ext = x;
        }
    }
    accumulate = r->synced && (r->slow == SLOW_RISING || c->slow_bidir);
    r->f_records++;
    if (!accumulate)
        return;

    /* fast-axis direction from the (delayed) calibrated sensor signal */
    double dy = y - hist_at(r->hy, t - c->dir_window);
    if (dy > 0.0)
        r->fast_dir = 1;
    else if (dy < 0.0)
        r->fast_dir = -1;

    int img = 0;
    switch (c->bidir) {
    case BIDIR_FORWARD:  if (r->fast_dir < 0) return; break;
    case BIDIR_BACKWARD: if (r->fast_dir > 0) return; break;
    case BIDIR_BOTH:     img = r->fast_dir < 0; break;
    case BIDIR_SUM:      break;
    }

    double fr = (x - c->x_lo) * r->sx;
    double fc = (y - c->y_lo) * r->sy;
    if (fr < 0.0 || fc < 0.0 || fr >= c->height || fc >= c->width)
        return;

    size_t idx = (size_t)img * r->npix + (size_t)fr * c->width + (size_t)fc;
    r->sum[idx] += v;
    r->cnt[idx]++;
    r->f_used++;
}

void recon_push(recon_t *r, const record_t *rec, size_t n)
{
    const recon_cfg_t *c = &r->cfg;

    for (size_t k = 0; k < n; k++) {
        uint64_t i = r->n_in++;
        r->hx[i & HIST_MASK] = rec[k].x;
        r->hy[i & HIST_MASK] = c->fast_gain * rec[k].y + c->fast_offset;
        r->hv[i & HIST_MASK] = rec[k].v * c->v_scale;

        /* wait for a full history ring so every interpolation index is valid */
        if (i == HIST) {
            r->slow = SLOW_RISING;
            r->slow_ext = r->hx[(i - LAG) & HIST_MASK];
        }
        if (i >= HIST)
            process(r, i - LAG);
    }
}

void recon_auto_size(double sample_rate, double f_slow, double f_fast,
                     double slow_image_fraction, uint16_t max_w, uint16_t max_h,
                     uint16_t *w, uint16_t *h)
{
    /* two samples per column at the sweep centre (where the sine is fastest) */
    double ww = sample_rate / (2.0 * M_PI * f_fast);
    /* one row per full fast period: lines are tilted, so a row per half-sweep leaves the edges half empty */
    double hh = f_fast * slow_image_fraction / f_slow;
    ww = fmin(fmax(ww, 1.0), max_w);
    hh = fmin(fmax(hh, 1.0), max_h);
    *w = (uint16_t)ww;
    *h = (uint16_t)hh;
}
