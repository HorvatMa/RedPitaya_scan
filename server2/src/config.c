#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { K_DOUBLE, K_UINT, K_SIZE, K_SLOW_SHAPE, K_FAST_SHAPE, K_BIDIR, K_ROI } kind_t;

typedef struct {
    const char *key;
    kind_t      kind;
    size_t      off;
    double      lo, hi;
    int         live;
} keydef_t;

#define F(field) offsetof(config_t, field)

static const keydef_t KEYS[] = {
    { "slow.shape",      K_SLOW_SHAPE, F(slow_shape),      0, 0, 0 },
    { "slow.rise",       K_DOUBLE,     F(slow_rise),       0.5, 1.0, 0 },
    { "slow.freq",       K_DOUBLE,     F(slow_freq),       1e-3, 1e5, 0 },
    { "slow.amp",        K_DOUBLE,     F(slow_amp),        0, 1e3, 0 },
    { "slow.offset",     K_DOUBLE,     F(slow_offset),     -1e3, 1e3, 0 },
    { "fast.shape",      K_FAST_SHAPE, F(fast_shape),      0, 0, 0 },
    { "fast.freq",       K_DOUBLE,     F(fast_freq),       1e-3, 1e5, 0 },
    { "fast.amp",        K_DOUBLE,     F(fast_amp),        0, 1e3, 0 },
    { "fast.offset",     K_DOUBLE,     F(fast_offset),     -1e3, 1e3, 0 },
    { "acq.dec",         K_UINT,       F(acq_dec),         2, 1e9, 0 },
    { "acq.int_dec",     K_UINT,       F(int_dec),         1, 1e9, 0 },
    { "img.width",       K_SIZE,       F(img_w),           1, 1920, 0 },
    { "img.height",      K_SIZE,       F(img_h),           1, 1080, 0 },
    { "img.bidir",       K_BIDIR,      F(bidir),           0, 0, 0 },
    { "img.roi",         K_ROI,        F(roi),             0, 1, 1 },
    { "cal.delay",       K_DOUBLE,     F(cal_delay),       -RECON_MAX_DELAY, RECON_MAX_DELAY, 0 },
    { "cal.fast_gain",   K_DOUBLE,     F(cal_fast_gain),   1e-3, 1e3, 0 },
    { "cal.fast_offset", K_DOUBLE,     F(cal_fast_offset), -1e3, 1e3, 0 },
    { "hw.slow_gain",    K_DOUBLE,     F(hw_slow_gain),    1e-3, 1e3, 0 },
    { "hw.fast_gain",    K_DOUBLE,     F(hw_fast_gain),    1e-3, 1e3, 0 },
    { "hw.sensor_gain",  K_DOUBLE,     F(hw_sensor_gain),  1e-3, 1e3, 0 },
};
#define NKEYS (sizeof(KEYS) / sizeof(*KEYS))

static const char *SLOW_SHAPES[] = { [WF_SAWTOOTH] = "sawtooth", [WF_TRIANGLE] = "triangle" };
static const char *FAST_SHAPES[] = { [WF_SINE] = "sine", [WF_TRIANGLE] = "triangle" };
static const char *BIDIR[] = { [BIDIR_SUM] = "sum", [BIDIR_FORWARD] = "forward",
                               [BIDIR_BACKWARD] = "backward", [BIDIR_BOTH] = "both" };

void config_defaults(config_t *c)
{
    /* scan values from client/src/image_test_2.py (galvo-side volts, gains 10.75 / 20) */
    *c = (config_t){
        .slow_shape = WF_SAWTOOTH, .slow_rise = 0.975, .slow_freq = 3.0,
        .slow_amp = 0.1217, .slow_offset = 2.286,
        .fast_shape = WF_SINE, .fast_freq = 1000.0, .fast_amp = 0.12, .fast_offset = -1.02,
        .acq_dec = 50, .int_dec = 1,
        .img_w = 0, .img_h = 0, .bidir = BIDIR_SUM,
        .roi = { 0.0, 1.0, 0.02, 0.98 },
        .cal_delay = 0.5, .cal_fast_gain = 1.0, .cal_fast_offset = 0.0,
        .hw_slow_gain = 10.75, .hw_fast_gain = 10.75, .hw_sensor_gain = 20.0,
    };
}

static void seterr(char *err, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

static const keydef_t *find_key(const char *key)
{
    for (size_t i = 0; i < NKEYS; i++)
        if (strcmp(KEYS[i].key, key) == 0)
            return &KEYS[i];
    return NULL;
}

int config_is_live(const char *key)
{
    const keydef_t *k = find_key(key);
    return k && k->live;
}

static int parse_double(const char *s, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    while (*end == ' ' || *end == '\t')
        end++;
    if (errno || end == s || *end || !isfinite(v))
        return -1;
    *out = v;
    return 0;
}

static int parse_enum(const char *s, const char **names, int n)
{
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(s, names[i]) == 0)
            return i;
    return -1;
}

static int names_list(const char **names, int n, char *buf, size_t len)
{
    size_t pos = 0;
    for (int i = 0; i < n && pos < len; i++)
        if (names[i])
            pos += snprintf(buf + pos, len - pos, "%s%s", pos ? "|" : "", names[i]);
    return 0;
}

int config_set(config_t *c, const char *key, const char *value, char *err, size_t errlen)
{
    const keydef_t *k = find_key(key);
    if (!k) {
        seterr(err, errlen, "unknown key %s", key);
        return -1;
    }
    char *p = (char *)c + k->off;
    double v;
    char   opts[64];

    switch (k->kind) {
    case K_DOUBLE:
        if (parse_double(value, &v) || v < k->lo || v > k->hi) {
            seterr(err, errlen, "%s must be a number in [%g, %g]", key, k->lo, k->hi);
            return -1;
        }
        *(double *)p = v;
        return 0;
    case K_UINT:
    case K_SIZE:
        if (k->kind == K_SIZE && strcmp(value, "auto") == 0) {
            *(unsigned *)p = 0;
            return 0;
        }
        if (parse_double(value, &v) || v != floor(v) || v < k->lo || v > k->hi) {
            seterr(err, errlen, "%s must be an integer in [%g, %g]%s", key, k->lo, k->hi,
                   k->kind == K_SIZE ? " or auto" : "");
            return -1;
        }
        *(unsigned *)p = (unsigned)v;
        return 0;
    case K_SLOW_SHAPE:
    case K_FAST_SHAPE:
    case K_BIDIR: {
        const char **names = k->kind == K_SLOW_SHAPE ? SLOW_SHAPES : k->kind == K_FAST_SHAPE ? FAST_SHAPES : BIDIR;
        int n = k->kind == K_SLOW_SHAPE ? 2 : k->kind == K_FAST_SHAPE ? 3 : 4;
        int i = parse_enum(value, names, n);
        if (i < 0) {
            names_list(names, n, opts, sizeof(opts));
            seterr(err, errlen, "%s must be one of %s", key, opts);
            return -1;
        }
        *(int *)p = i;
        return 0;
    }
    case K_ROI: {
        double r[4];
        char   extra;
        if (sscanf(value, "%lf %lf %lf %lf %c", &r[0], &r[1], &r[2], &r[3], &extra) != 4 ||
            !(r[0] >= 0 && r[0] < r[1] && r[1] <= 1 && r[2] >= 0 && r[2] < r[3] && r[3] <= 1)) {
            seterr(err, errlen, "img.roi must be 'x0 x1 y0 y1' with 0 <= x0 < x1 <= 1, 0 <= y0 < y1 <= 1");
            return -1;
        }
        memcpy(p, r, sizeof(r));
        return 0;
    }
    }
    return -1;
}

int config_derive(const config_t *c, derived_t *d, char *err, size_t errlen)
{
    memset(&d->hw, 0, sizeof(d->hw));

    /* galvo volts -> DAC counts */
    d->slow = (wf_t){ .shape = c->slow_shape, .rise = c->slow_rise, .freq = c->slow_freq,
                      .amp = c->slow_amp / c->hw_slow_gain * COUNTS_PER_VOLT,
                      .offset = c->slow_offset / c->hw_slow_gain * COUNTS_PER_VOLT };
    d->fast = (wf_t){ .shape = c->fast_shape, .freq = c->fast_freq,
                      .amp = c->fast_amp / c->hw_fast_gain * COUNTS_PER_VOLT,
                      .offset = c->fast_offset / c->hw_fast_gain * COUNTS_PER_VOLT };
    const struct { const char *name; wf_t *w; double gain; } ax[2] = {
        { "slow", &d->slow, c->hw_slow_gain }, { "fast", &d->fast, c->hw_fast_gain } };
    for (int i = 0; i < 2; i++) {
        if (fabs(ax[i].w->offset) + ax[i].w->amp > 32767.0) {
            seterr(err, errlen, "%s offset+amp exceed the DAC range (+-%.4g V at gain %g)",
                   ax[i].name, ax[i].gain, ax[i].gain);
            return -1;
        }
        if (ax[i].w->amp < 1.0) {
            seterr(err, errlen, "%s amplitude is below one DAC count", ax[i].name);
            return -1;
        }
    }
    if (scan_quantise(&d->slow, 256, WFM1_MAX_LEN) || scan_quantise(&d->fast, 256, WFM2_MAX_LEN)) {
        seterr(err, errlen, "frequency not reachable (needs 125 MHz / f >= 256 cycles)");
        return -1;
    }
    scan_fill(&d->slow, d->wfm1);
    scan_fill(&d->fast, d->wfm2);

    /* reconstruction geometry */
    d->rate = FPGA_CLK_HZ / c->acq_dec;
    double sensor_to_cmd = c->cal_fast_gain * c->hw_sensor_gain / c->hw_fast_gain;
    double xlo, xhi, ylo, yhi;
    scan_range(&d->slow, &xlo, &xhi);
    scan_range(&d->fast, &ylo, &yhi);

    recon_cfg_t *r = &d->recon;
    *r = (recon_cfg_t){
        .x_lo = xlo + c->roi[0] * (xhi - xlo), .x_hi = xlo + c->roi[1] * (xhi - xlo),
        .y_lo = ylo + c->roi[2] * (yhi - ylo), .y_hi = ylo + c->roi[3] * (yhi - ylo),
        .fast_gain = sensor_to_cmd,
        .fast_offset = c->cal_fast_offset / c->hw_fast_gain * COUNTS_PER_VOLT,
        .delay = c->cal_delay,
        .v_scale = (double)c->int_dec / (c->acq_dec - 1),
        .bidir = c->bidir,
        .slow_bidir = c->slow_shape == WF_TRIANGLE,
        .dir_window = 4,
    };
    uint16_t aw, ah;
    double frac = c->slow_shape == WF_TRIANGLE ? 0.5 : c->slow_rise;
    recon_auto_size(d->rate, d->slow.freq_actual, d->fast.freq_actual, frac, 1920, 1080, &aw, &ah);
    /* a column narrower than one sensor step can never be hit reliably */
    double sensor_levels = (r->y_hi - r->y_lo) / (SENSOR_LSB * sensor_to_cmd);
    d->sensor_levels = sensor_levels;
    double w = c->img_w ? c->img_w : fmax(1.0, fmin(aw * (c->roi[3] - c->roi[2]), sensor_levels));
    double h = c->img_h ? c->img_h : fmax(1.0, ah * (c->roi[1] - c->roi[0]));
    r->width = (uint16_t)w;
    r->height = (uint16_t)h;

    d->hw = (hw_setup_t){
        .wfm1 = d->wfm1, .len1 = d->slow.len, .dec1 = d->slow.dec,
        .wfm2 = d->wfm2, .len2 = d->fast.len, .dec2 = d->fast.dec,
        .acq_dec = c->acq_dec, .int_dec = c->int_dec,
        .sim_sensor_gain = sensor_to_cmd,
    };
    return 0;
}

static size_t put(char *buf, size_t len, size_t pos, const char *fmt, ...)
{
    if (pos >= len)
        return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, len - pos, fmt, ap);
    va_end(ap);
    return n > 0 ? pos + (size_t)n : pos;
}

void config_format(const config_t *c, const derived_t *d, char *buf, size_t len)
{
    size_t pos = 0;
    buf[0] = 0;
    for (size_t i = 0; i < NKEYS; i++) {
        const keydef_t *k = &KEYS[i];
        const char *p = (const char *)c + k->off;
        switch (k->kind) {
        case K_DOUBLE: pos = put(buf, len, pos, "%s=%.10g\n", k->key, *(const double *)p); break;
        case K_UINT:   pos = put(buf, len, pos, "%s=%u\n", k->key, *(const unsigned *)p); break;
        case K_SIZE:
            if (*(const unsigned *)p)
                pos = put(buf, len, pos, "%s=%u\n", k->key, *(const unsigned *)p);
            else
                pos = put(buf, len, pos, "%s=auto\n", k->key);
            break;
        case K_SLOW_SHAPE: pos = put(buf, len, pos, "%s=%s\n", k->key, SLOW_SHAPES[*(const int *)p]); break;
        case K_FAST_SHAPE: pos = put(buf, len, pos, "%s=%s\n", k->key, FAST_SHAPES[*(const int *)p]); break;
        case K_BIDIR:      pos = put(buf, len, pos, "%s=%s\n", k->key, BIDIR[*(const int *)p]); break;
        case K_ROI: {
            const double *r = (const double *)p;
            pos = put(buf, len, pos, "%s=%.6g %.6g %.6g %.6g\n", k->key, r[0], r[1], r[2], r[3]);
            break;
        }
        }
    }
    if (d) {
        pos = put(buf, len, pos,
                  "slow.freq_actual=%.10g\nfast.freq_actual=%.10g\nslow.len=%u\nslow.dec=%u\n"
                  "fast.len=%u\nfast.dec=%u\nacq.rate=%.10g\nimg.width_actual=%u\nimg.height_actual=%u\n"
                  "fast.sensor_levels=%.0f\n",
                  d->slow.freq_actual, d->fast.freq_actual, d->slow.len, d->slow.dec,
                  d->fast.len, d->fast.dec, d->rate, d->recon.width, d->recon.height,
                  d->sensor_levels);
    }
    if (pos > 0 && pos < len && buf[pos - 1] == '\n')
        buf[pos - 1] = 0;
}
