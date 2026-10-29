#ifndef SIM_H
#define SIM_H

#include <stddef.h>
#include <stdint.h>

#include "../common.h"

/* Detector level (0..16383) as a function of normalised mirror position u, w in [0,1]. */
typedef double (*sim_pattern_t)(double u, double w);

/*
 * Emulates the deployed FPGA: two table generators, a decimated sampler, and an integrator.
 * Positions follow the commands exactly (slow axis); the fast mirror lags its command by
 * fast_lag_cycles and the sensor reports the mirror position scaled as
 * sensor = (mirror - sensor_offset) / sensor_gain.
 */
typedef struct {
    const int16_t *wfm1; uint32_t len1, dec1;   /* slow, OUT1 */
    const int16_t *wfm2; uint32_t len2, dec2;   /* fast, OUT2 */
    uint32_t acq_dec;
    double   fast_lag_cycles;
    double   sensor_gain, sensor_offset;
    double   noise;                             /* sensor noise, counts rms */
    unsigned sensor_lsb;                        /* sensor reads in multiples of this (FPGA: 4) */
    double   x_lo, x_hi, y_lo, y_hi;            /* range used to normalise the pattern */
    double   ext_amp, ext_offset, ext_freq;     /* ext_amp > 0: the fast mirror follows an external
                                                   sine (counts, Hz) instead of OUT2 */
    sim_pattern_t pattern;
    uint32_t seed;
} sim_cfg_t;

typedef struct {
    sim_cfg_t cfg;
    uint64_t  k;          /* next record index */
    uint32_t  rng;
} sim_t;

void sim_init(sim_t *s, const sim_cfg_t *cfg);
void sim_next(sim_t *s, record_t *out, size_t n);

#endif
