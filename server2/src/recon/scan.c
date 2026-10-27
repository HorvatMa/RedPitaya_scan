#include "scan.h"
#include "../common.h"

#include <math.h>

int scan_quantise(wf_t *wf, uint32_t min_len, uint32_t max_len)
{
    if (wf->freq <= 0.0 || min_len < 2 || max_len < min_len)
        return -1;

    double cycles = FPGA_CLK_HZ / wf->freq;          /* len * dec wanted */
    uint32_t dec0 = (uint32_t)ceil(cycles / max_len);
    if (dec0 < 1)
        dec0 = 1;

    double   best_err = INFINITY;
    uint32_t best_len = 0, best_dec = 0;

    /* Scan dividers from the smallest usable one; len shrinks as dec grows. */
    for (uint32_t dec = dec0; dec < dec0 + 4096; dec++) {
        double l = cycles / dec;
        if (l < min_len)
            break;
        for (int k = -1; k <= 1; k++) {
            int64_t len = 2 * (int64_t)floor(l / 2.0) + 2 * k;
            if (len < (int64_t)min_len || len > (int64_t)max_len)
                continue;
            double err = fabs(FPGA_CLK_HZ / ((double)len * dec) - wf->freq);
            if (err < best_err - 1e-12 || (fabs(err - best_err) <= 1e-12 && (uint32_t)len > best_len)) {
                best_err = err;
                best_len = (uint32_t)len;
                best_dec = dec;
            }
        }
    }

    if (best_len == 0)
        return -1;
    wf->len = best_len;
    wf->dec = best_dec;
    wf->freq_actual = FPGA_CLK_HZ / ((double)best_len * best_dec);
    return 0;
}

static double shape_at(const wf_t *wf, double phase)   /* phase in [0,1) -> [-1,1] */
{
    switch (wf->shape) {
    case WF_SINE:
        return sin(2.0 * M_PI * phase);
    case WF_TRIANGLE:
        return phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
    case WF_SAWTOOTH:
    default: {
        double r = (wf->rise > 0.0 && wf->rise <= 1.0) ? wf->rise : 1.0;
        if (phase < r)
            return 2.0 * phase / r - 1.0;
        return 1.0 - 2.0 * (phase - r) / (1.0 - r);
    }
    }
}

void scan_fill(const wf_t *wf, int16_t *out)
{
    for (uint32_t i = 0; i < wf->len; i++) {
        double v = wf->offset + wf->amp * shape_at(wf, (double)i / wf->len);
        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        out[i] = (int16_t)lround(v);
    }
}

void scan_range(const wf_t *wf, double *lo, double *hi)
{
    *lo = wf->offset - wf->amp;
    *hi = wf->offset + wf->amp;
}
