#ifndef SCAN_H
#define SCAN_H

#include <stddef.h>
#include <stdint.h>

typedef enum { WF_SAWTOOTH, WF_TRIANGLE, WF_SINE } wf_shape_t;

/* One generator channel. Amplitude/offset are in DAC counts (int16 range). */
typedef struct {
    wf_shape_t shape;
    double     rise;          /* sawtooth: fraction of the period spent rising (0..1] */
    double     amp;           /* peak amplitude, counts */
    double     offset;        /* counts */
    double     freq;          /* requested frequency, Hz */

    /* filled by scan_quantise() */
    uint32_t   len;           /* waveform table length */
    uint32_t   dec;           /* generator step period in FPGA cycles */
    double     freq_actual;   /* 125 MHz / (len * dec) */
} wf_t;

/*
 * Pick an even table length len in [min_len, max_len] and a divider dec >= 1 so that
 * 125 MHz / (len * dec) is as close as possible to wf->freq. Longer tables win ties.
 * Returns 0 on success, -1 if the frequency is out of reach.
 */
int scan_quantise(wf_t *wf, uint32_t min_len, uint32_t max_len);

/* Fill out[0..wf->len) with the waveform; values are clamped to int16. */
void scan_fill(const wf_t *wf, int16_t *out);

/* Lowest and highest value of the rising part (sawtooth) or of the whole waveform. */
void scan_range(const wf_t *wf, double *lo, double *hi);

#endif
