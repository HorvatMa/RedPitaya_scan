#include "recon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* History ring: records are processed HIST/2 samples late so the delay may be +-RECON_MAX_DELAY. */
#define HIST      256
#define HIST_MASK (HIST - 1)
#define LAG       (RECON_MAX_DELAY + 2)

#define CAL_RECORDS   262144.0  /* calibration length scale, records          */
#define CAL_MIN_AMP   16.0      /* smaller measured sweep (sensor counts) = no signal */

/* external fast drive: frequency measurement phases, seconds */
#define MEAS_SETTLE   0.02
#define MEAS_RANGE    0.05      /* min/max -> crossing level and hysteresis */
#define MEAS_CROSS    0.2       /* rising crossings -> frequency            */

typedef enum { SLOW_RISING, SLOW_FALLING } slow_state_t;

struct recon {
    recon_cfg_t cfg;
    frame_cb_t  cb;
    void       *ctx;

    /* history of positions (y already calibrated) and detector values; float is plenty
       for 16-bit data and much faster than double on the Cortex-A9 */
    float       hx[HIST], hy[HIST], hv[HIST];
    uint64_t    n_in;              /* records received so far */

    /* constant fractional delay: position(j - delay) = h[j+dk]*wa + h[j+dk+1]*wb */
    int64_t      dk;
    float        wa, wb;

    /* slow-axis turn detection */
    slow_state_t slow;
    float        slow_ext;         /* running max (rising) or min (falling) */
    float        slow_hyst;
    int          synced;           /* seen at least one turn: frames are complete */

    int          fast_dir;         /* +1 / -1 */

    float        x_lo, y_lo;
    float        sx, sy;           /* rows per count, columns per count */
    float        fgain, foff, vscale;
    int          n_img;
    size_t       npix;             /* width * height */
    float       *sum;
    uint32_t    *cnt;
    float       *out;

    uint32_t     frame_id;
    uint64_t     f_records, f_used;

    /* fast-axis auto-calibration: least-squares fit y = a sin + b cos + c over whole periods */
    recon_fcal_t fcal;
    int          calibrating;      /* records go to the calibration, not the image */
    int          measuring;        /* external drive: frequency not known yet */
    uint64_t     m_n, m_settle, m_range, m_cross, m_first, m_last;
    unsigned     m_count;
    int          m_high;
    double       m_min, m_max, m_mid, m_hyst;
    uint64_t     cal_n, cal_skip, cal_len;
    double       cal_sy, cal_ss, cal_sc;
    double       rot_c, rot_s, rot_dc, rot_ds;   /* phase rotator */
};

/* Width cap: a column narrower than one sensor step can never be hit reliably. It needs the
   sensor scale, so it is not applied while the calibration is pending. */
static void apply_fast_mapping(recon_t *r)
{
    const recon_cfg_t *c = &r->cfg;
    r->fgain = (float)r->fcal.gain;
    r->foff = (float)r->fcal.offset;
    r->fcal.levels = c->sensor_step > 0.0
                   ? (c->y_hi - c->y_lo) / (c->sensor_step * fabs(r->fcal.gain)) : 0.0;
    r->fcal.width = c->width;
    if (c->width_auto && r->fcal.state != FCAL_PENDING && r->fcal.levels >= 1.0 &&
        r->fcal.levels < c->width)
        r->fcal.width = (uint16_t)r->fcal.levels;
    r->npix = (size_t)r->fcal.width * c->height;
    r->sy = (float)(r->fcal.width / (c->y_hi - c->y_lo));
}

/* Image size (auto from the fast frequency when 0) and accumulation buffers. */
static int alloc_image(recon_t *r)
{
    recon_cfg_t *c = &r->cfg;
    if (c->width == 0 || c->height == 0) {
        uint16_t aw, ah;
        recon_auto_size(c->sample_rate, c->slow_freq, c->fast_cycles * c->sample_rate, c->slow_frac,
                        1920, 1080, &aw, &ah);
        if (c->width == 0)
            c->width = (uint16_t)fmax(1.0, aw * c->roi_w);
        if (c->height == 0)
            c->height = (uint16_t)fmax(1.0, ah * c->roi_h);
    }
    r->sx = (float)(c->height / (c->x_hi - c->x_lo));
    size_t n = (size_t)c->width * c->height * r->n_img;
    r->sum = calloc(n, sizeof(*r->sum));
    r->cnt = calloc(n, sizeof(*r->cnt));
    r->out = malloc(n * sizeof(*r->out));
    return r->sum && r->cnt && r->out ? 0 : -1;
}

/* Start the sine fit at the known frequency: settle `settle` records, then 2..20 whole periods
   (CAL_RECORDS, ~0.1 s at 2.5 MS/s, bounds both unless a single period is longer). */
static void fit_init(recon_t *r, double settle)
{
    double cyc = r->cfg.fast_cycles, per = 1.0 / cyc;
    double nper = fmin(20.0, fmax(2.0, floor(CAL_RECORDS / per)));
    r->cal_n = 0;
    r->cal_skip = (uint64_t)settle;
    r->cal_len = (uint64_t)llround(nper * per);
    r->cal_sy = r->cal_ss = r->cal_sc = 0.0;
    r->rot_c = 1.0;
    r->rot_s = 0.0;
    r->rot_dc = cos(2.0 * M_PI * cyc);
    r->rot_ds = sin(2.0 * M_PI * cyc);
}

static void meas_init(recon_t *r)
{
    const recon_cfg_t *c = &r->cfg;
    r->measuring = 1;
    r->m_n = 0;
    r->m_settle = (uint64_t)(MEAS_SETTLE * c->sample_rate);
    r->m_range = (uint64_t)(MEAS_RANGE * c->sample_rate);
    r->m_cross = (uint64_t)(MEAS_CROSS * c->sample_rate);
    r->m_min = INFINITY;
    r->m_max = -INFINITY;
    r->m_count = 0;
}

recon_t *recon_create(const recon_cfg_t *cfg, frame_cb_t cb, void *ctx)
{
    int measure = cfg && cfg->fast_external && cfg->fast_auto && cfg->fast_cycles == 0.0;
    if (!cfg || !cb || ((cfg->width == 0 || cfg->height == 0) && !cfg->fast_external) ||
        !(cfg->x_hi > cfg->x_lo) || !(cfg->y_hi > cfg->y_lo) ||
        fabs(cfg->delay) > RECON_MAX_DELAY || cfg->dir_window == 0 ||
        cfg->dir_window > RECON_MAX_DELAY || cfg->fast_gain == 0.0 ||
        (cfg->width_auto && !(cfg->sensor_step > 0.0)) ||
        (cfg->fast_external && !(cfg->sample_rate > 0.0 && cfg->slow_freq > 0.0 &&
                                 cfg->roi_w > 0.0 && cfg->roi_h > 0.0)) ||
        (cfg->fast_external && cfg->fast_cycles == 0.0 && !cfg->fast_auto) ||
        (cfg->fast_auto && !measure && !(cfg->fast_cycles > 0.0 && cfg->fast_cycles < 0.5)) ||
        (cfg->fast_auto && !(cfg->fast_cmd_hi > cfg->fast_cmd_lo)))
        return NULL;

    recon_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->cfg = *cfg;
    r->cb = cb;
    r->ctx = ctx;
    r->n_img = cfg->bidir == BIDIR_BOTH ? 2 : 1;
    r->x_lo = (float)cfg->x_lo;
    r->y_lo = (float)cfg->y_lo;
    r->fcal = (recon_fcal_t){ .state = cfg->fast_auto ? FCAL_PENDING : FCAL_MANUAL,
                              .gain = cfg->fast_gain, .offset = cfg->fast_offset,
                              .width = cfg->width };
    r->calibrating = cfg->fast_auto;
    if (measure)
        meas_init(r);
    else if (cfg->fast_auto)
        fit_init(r, fmax(1.0 / cfg->fast_cycles, fmin(4.0 / cfg->fast_cycles, CAL_RECORDS)));
    r->vscale = (float)cfg->v_scale;
    r->slow_hyst = (float)fmax(0.5, 0.001 * (cfg->x_hi - cfg->x_lo));   /* slow command is exact */
    r->fast_dir = 1;
    double fl = floor(-cfg->delay);
    r->dk = (int64_t)fl;
    r->wb = (float)(-cfg->delay - fl);
    r->wa = 1.0f - r->wb;

    if (!measure && alloc_image(r)) {          /* external: sized once the frequency is known */
        recon_destroy(r);
        return NULL;
    }
    apply_fast_mapping(r);
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
    r->f_records = 0;
    r->f_used = 0;
    if (!r->sum)
        return;                                    /* external drive, not sized yet */
    memset(r->sum, 0, n * sizeof(*r->sum));
    memset(r->cnt, 0, n * sizeof(*r->cnt));
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
            r->out[i] = r->sum[i] / r->cnt[i];
        } else {
            r->out[i] = NAN;
            empty++;
        }
    }

    frame_t f = {
        .frame_id = r->frame_id++,
        .width = r->fcal.width,
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

/* Process the record at absolute index j (at least LAG records behind the newest). */
static void process(recon_t *r, uint64_t j)
{
    const recon_cfg_t *c = &r->cfg;
    uint64_t i0 = j + (uint64_t)r->dk;               /* wraps correctly for negative dk */
    float x = r->hx[i0 & HIST_MASK] * r->wa + r->hx[(i0 + 1) & HIST_MASK] * r->wb;
    float y = r->hy[i0 & HIST_MASK] * r->wa + r->hy[(i0 + 1) & HIST_MASK] * r->wb;
    float v = r->hv[j & HIST_MASK];

    /* slow-axis turn detection with hysteresis on the running extreme */
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
    r->f_records++;
    if (!r->synced || !(r->slow == SLOW_RISING || c->slow_bidir))
        return;

    /* fast-axis direction from the (delayed) calibrated sensor signal */
    float dy = y - r->hy[(i0 - c->dir_window) & HIST_MASK];
    if (dy > 0.0f)
        r->fast_dir = 1;
    else if (dy < 0.0f)
        r->fast_dir = -1;

    int img = 0;
    switch (c->bidir) {
    case BIDIR_FORWARD:  if (r->fast_dir < 0) return; break;
    case BIDIR_BACKWARD: if (r->fast_dir > 0) return; break;
    case BIDIR_BOTH:     img = r->fast_dir < 0; break;
    case BIDIR_SUM:      break;
    }

    float fr = (x - r->x_lo) * r->sx;
    float fc = (y - r->y_lo) * r->sy;
    if (!(fr >= 0.0f && fc >= 0.0f && fr < c->height && fc < r->fcal.width))
        return;

    size_t idx = (size_t)img * r->npix + (size_t)fr * r->fcal.width + (size_t)fc;
    r->sum[idx] += v;
    r->cnt[idx]++;
    r->f_used++;
}

void recon_fast_cal(const recon_t *r, recon_fcal_t *out)
{
    *out = r->fcal;
    out->fast_hz = r->cfg.fast_cycles * r->cfg.sample_rate;
}

/* External drive: find the frequency from rising crossings of the sweep centre (with
   hysteresis). Returns 1 when it is known (fit started), -1 when there is no usable sweep. */
static int measure(recon_t *r, int16_t y)
{
    uint64_t i = r->m_n++;
    if (i < r->m_settle)
        return 0;
    i -= r->m_settle;
    if (i < r->m_range) {
        r->m_min = fmin(r->m_min, y);
        r->m_max = fmax(r->m_max, y);
        if (i + 1 == r->m_range) {
            r->m_mid = 0.5 * (r->m_max + r->m_min);
            r->m_hyst = 0.25 * (r->m_max - r->m_min);
            r->m_high = y > r->m_mid;
            r->fcal.sensor_amp = 0.5 * (r->m_max - r->m_min);
            r->fcal.sensor_mid = r->m_mid;
            if (r->fcal.sensor_amp < CAL_MIN_AMP)
                return -1;
        }
        return 0;
    }
    i -= r->m_range;
    if (!r->m_high && y > r->m_mid + r->m_hyst) {
        r->m_high = 1;
        if (r->m_count++ == 0)
            r->m_first = i;
        r->m_last = i;
    } else if (r->m_high && y < r->m_mid - r->m_hyst) {
        r->m_high = 0;
    }
    if (i + 1 < r->m_cross)
        return 0;
    if (r->m_count < 3)
        return -1;                                     /* < 2 periods in MEAS_CROSS */
    double cyc = (r->m_count - 1) / (double)(r->m_last - r->m_first);
    if (!(cyc < 0.25))
        return -1;
    r->cfg.fast_cycles = cyc;
    r->measuring = 0;
    fit_init(r, 0.0);
    return 1;
}

/* One calibration record; returns 1 when the fit is finished (mapping updated). */
static int calibrate(recon_t *r, int16_t y)
{
    uint64_t i = r->cal_n++;
    if (i < r->cal_skip)
        return 0;
    double s = r->rot_s, c = r->rot_c;
    r->cal_sy += y;
    r->cal_ss += y * s;
    r->cal_sc += y * c;
    r->rot_c = c * r->rot_dc - s * r->rot_ds;
    r->rot_s = s * r->rot_dc + c * r->rot_ds;
    if (((i - r->cal_skip) & 4095) == 4095) {      /* keep the rotator on the unit circle */
        double m = 1.0 / sqrt(r->rot_c * r->rot_c + r->rot_s * r->rot_s);
        r->rot_c *= m;
        r->rot_s *= m;
    }
    if (i + 1 < r->cal_skip + r->cal_len)
        return 0;

    double n = (double)r->cal_len;
    double amp = 2.0 / n * hypot(r->cal_ss, r->cal_sc);    /* fundamental */
    if (r->cfg.fast_triangle)
        amp *= M_PI * M_PI / 8.0;                          /* triangle peak from its fundamental */
    double mid = r->cal_sy / n;
    r->fcal.sensor_amp = amp;
    r->fcal.sensor_mid = mid;
    if (amp < CAL_MIN_AMP) {
        r->fcal.state = FCAL_FAILED;
        if (r->cfg.fast_external) {                        /* nothing to image without it: retry */
            meas_init(r);
            return 0;
        }
        r->calibrating = 0;                                /* no sweep on the sensor: keep manual */
        apply_fast_mapping(r);
        return 1;
    }
    double cmd_amp = 0.5 * (r->cfg.fast_cmd_hi - r->cfg.fast_cmd_lo);
    double cmd_mid = 0.5 * (r->cfg.fast_cmd_hi + r->cfg.fast_cmd_lo);
    r->fcal.gain = copysign(cmd_amp / amp, r->cfg.fast_gain);
    r->fcal.offset = cmd_mid - r->fcal.gain * mid;
    r->fcal.state = FCAL_DONE;
    r->calibrating = 0;
    if (!r->sum && alloc_image(r)) {
        r->fcal.state = FCAL_FAILED;                       /* out of memory: no frames */
        r->calibrating = 1;
        meas_init(r);
        return 0;
    }
    apply_fast_mapping(r);
    return 1;
}

void recon_push(recon_t *r, const record_t *rec, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (r->calibrating) {
            if (r->measuring) {
                if (measure(r, rec[k].y) < 0) {
                    r->fcal.state = FCAL_FAILED;           /* no sweep yet: keep trying */
                    meas_init(r);
                }
            } else if (calibrate(r, rec[k].y)) {
                recon_resync(r);                           /* start over with the new mapping */
            }
            continue;
        }
        uint64_t i = r->n_in++;
        r->hx[i & HIST_MASK] = rec[k].x;
        r->hy[i & HIST_MASK] = r->fgain * rec[k].y + r->foff;
        r->hv[i & HIST_MASK] = (float)rec[k].v * r->vscale;

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
