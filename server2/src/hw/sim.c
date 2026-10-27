#include "sim.h"

#include <math.h>

void sim_init(sim_t *s, const sim_cfg_t *cfg)
{
    s->cfg = *cfg;
    s->k = 0;
    s->rng = cfg->seed ? cfg->seed : 1;
}

static double rnd_gauss(uint32_t *st)
{
    /* xorshift32 + Box-Muller; good enough for test noise */
    double u[2];
    for (int i = 0; i < 2; i++) {
        *st ^= *st << 13;
        *st ^= *st >> 17;
        *st ^= *st << 5;
        u[i] = (*st + 1.0) / 4294967297.0;
    }
    return sqrt(-2.0 * log(u[0])) * cos(2.0 * M_PI * u[1]);
}

/* Table value at FPGA cycle t (generators start together at t = 0), linear between steps. */
static double gen_at(const int16_t *w, uint32_t len, uint32_t dec, double t)
{
    if (t < 0.0)
        t = 0.0;
    double   step = t / dec;
    uint64_t i = (uint64_t)step;
    double   fr = step - i;
    /* the hardware holds each value; interpolate only for the mirror model */
    return w[i % len] * (1.0 - fr) + w[(i + 1) % len] * fr;
}

static int16_t gen_held(const int16_t *w, uint32_t len, uint32_t dec, uint64_t t)
{
    return w[(t / dec) % len];
}

void sim_next(sim_t *s, record_t *out, size_t n)
{
    const sim_cfg_t *c = &s->cfg;

    for (size_t j = 0; j < n; j++, s->k++) {
        uint64_t t = (s->k + 1) * (uint64_t)c->acq_dec;   /* trig cycle of this record */

        /* recorded values at the trig cycle */
        int16_t x_cmd = gen_held(c->wfm1, c->len1, c->dec1, t);
        double  mirror = gen_at(c->wfm2, c->len2, c->dec2, t - c->fast_lag_cycles);
        double  sensor = (mirror - c->sensor_offset) / c->sensor_gain + c->noise * rnd_gauss(&s->rng);

        /* integral over the previous acq_dec-1 cycles, midpoint rule on the true positions */
        double tm = t - c->acq_dec / 2.0;
        double xm = gen_at(c->wfm1, c->len1, c->dec1, tm);
        double ym = gen_at(c->wfm2, c->len2, c->dec2, tm - c->fast_lag_cycles);
        double u = (xm - c->x_lo) / (c->x_hi - c->x_lo);
        double w = (ym - c->y_lo) / (c->y_hi - c->y_lo);
        double level = c->pattern(u, w);

        out[j].x = x_cmd;
        if (c->sensor_lsb > 1)
            sensor = c->sensor_lsb * floor(sensor / c->sensor_lsb + 0.5);
        out[j].y = (int16_t)lround(fmax(-32768.0, fmin(32767.0, sensor)));
        out[j].v = (uint32_t)lround(level * (c->acq_dec - 1));
    }
}
