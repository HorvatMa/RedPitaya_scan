/* Throughput of recon_push() on pre-generated records. Run on the board to check the CPU budget. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/recon/recon.h"
#include "../src/recon/scan.h"
#include "../src/hw/sim.h"

static double pattern(double u, double w) { return 4000.0 + 3000.0 * u * w; }
static void   on_frame(void *ctx, const frame_t *f) { (void)f; (*(int *)ctx)++; }

int main(int argc, char **argv)
{
    size_t   n = argc > 1 ? strtoul(argv[1], NULL, 10) : 10000000;
    uint16_t width = argc > 2 ? (uint16_t)atoi(argv[2]) : 1920;
    uint16_t height = argc > 3 ? (uint16_t)atoi(argv[3]) : 1080;
    static int16_t w1[WFM1_MAX_LEN], w2[WFM2_MAX_LEN];
    wf_t slow = { .shape = WF_SAWTOOTH, .rise = 0.975, .amp = 8000, .freq = 3.0 };
    wf_t fast = { .shape = WF_SINE, .amp = 6000, .freq = 1000.0 };
    scan_quantise(&slow, 256, WFM1_MAX_LEN);
    scan_quantise(&fast, 256, WFM2_MAX_LEN);
    scan_fill(&slow, w1);
    scan_fill(&fast, w2);

    sim_cfg_t sc = { .wfm1 = w1, .len1 = slow.len, .dec1 = slow.dec, .wfm2 = w2, .len2 = fast.len,
                     .dec2 = fast.dec, .acq_dec = 50, .sensor_gain = 1, .x_lo = -8000, .x_hi = 8000,
                     .y_lo = -6000, .y_hi = 6000, .pattern = pattern, .seed = 1 };
    sim_t sim;
    sim_init(&sim, &sc);
    record_t *rec = malloc(n * sizeof(*rec));
    if (!rec)
        return 1;
    sim_next(&sim, rec, n);

    recon_cfg_t c = { .width = width, .height = height, .x_lo = -8000, .x_hi = 8000, .y_lo = -5800,
                      .y_hi = 5800, .fast_gain = 1, .delay = 0.5, .v_scale = 1.0 / 49,
                      .bidir = BIDIR_SUM, .dir_window = 4 };
    int frames = 0;
    recon_t *r = recon_create(&c, on_frame, &frames);
    if (!r)
        return 1;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < n; i += 4096)
        recon_push(r, rec + i, n - i < 4096 ? n - i : 4096);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double s = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
    printf("%zu records in %.3f s = %.2f Mrec/s (%d frames, %ux%u)\n", n, s, n / s / 1e6, frames,
           width, height);
    printf("acq.dec=50 needs 2.50 Mrec/s\n");
    recon_destroy(r);
    free(rec);
    return 0;
}
