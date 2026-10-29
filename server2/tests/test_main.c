/*
 * Host-side tests for the reconstruction core. Build and run with `make test`.
 * Set DUMP_PGM=1 to write the reconstructed frames of the smooth-pattern test as .pgm files.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/recon/recon.h"
#include "../src/recon/scan.h"
#include "../src/hw/sim.h"

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* ---------------------------------------------------------------- scan */

static void test_quantise(void)
{
    printf("test_quantise\n");
    double targets[] = { 1000.0, 3.0, 1.0, 2345.6, 0.5 };
    for (size_t i = 0; i < sizeof(targets) / sizeof(*targets); i++) {
        wf_t wf = { .shape = WF_SINE, .freq = targets[i] };
        int  rc = scan_quantise(&wf, 256, WFM2_MAX_LEN);
        CHECK(rc == 0, "quantise %.3f failed", targets[i]);
        CHECK(wf.len % 2 == 0 && wf.len <= WFM2_MAX_LEN && wf.dec >= 1, "bad len/dec");
        CHECK(fabs(wf.freq_actual - targets[i]) / targets[i] < 1e-4,
              "f=%.4f actual=%.6f (len %u dec %u)", targets[i], wf.freq_actual, wf.len, wf.dec);
    }
    wf_t exact = { .shape = WF_SINE, .freq = 1000.0 };
    scan_quantise(&exact, 256, WFM2_MAX_LEN);
    CHECK(exact.freq_actual == 1000.0, "1 kHz should be exact, got %.6f", exact.freq_actual);
    wf_t too_fast = { .shape = WF_SINE, .freq = 1e6 };
    CHECK(scan_quantise(&too_fast, 256, WFM2_MAX_LEN) == -1, "1 MHz with len>=256 must fail");
}

static void test_fill(void)
{
    printf("test_fill\n");
    int16_t buf[1000];
    wf_t saw = { .shape = WF_SAWTOOTH, .rise = 0.9, .amp = 10000, .offset = 500, .len = 1000 };
    scan_fill(&saw, buf);
    CHECK(buf[0] == 500 - 10000, "sawtooth start %d", buf[0]);
    CHECK(abs(buf[899] - (500 + 10000)) < 30, "sawtooth top %d", buf[899]);
    int mono = 1;
    for (int i = 1; i < 900; i++)
        mono &= buf[i] >= buf[i - 1];
    CHECK(mono, "sawtooth rise not monotonic");

    wf_t clip = { .shape = WF_SINE, .amp = 40000, .len = 1000 };
    scan_fill(&clip, buf);
    CHECK(buf[250] == 32767 && buf[750] == -32768, "clamping %d %d", buf[250], buf[750]);
}

/* ---------------------------------------------------------------- recon */

static double smooth_pattern(double u, double w)
{
    return 8000.0 + 6000.0 * sin(2.0 * M_PI * 1.5 * u) * cos(2.0 * M_PI * 2.0 * w);
}

typedef struct {
    int      n_frames;
    double   rms_err;        /* of the first complete frame */
    double   fb_diff;        /* mean |forward - backward| (BIDIR_BOTH) */
    double   empty_frac;
    uint64_t used;
    recon_cfg_t cfg;
    recon_fcal_t fcal;
    const char *dump_name;
} result_t;

static int autocal;     /* run_scan: start from wrong fast gains and let the recon fit them */

static void on_frame(void *ctx, const frame_t *f)
{
    result_t *res = ctx;
    if (res->n_frames++ > 0)
        return;                                   /* evaluate the first frame only */

    size_t   npix = (size_t)f->width * f->height;
    double   se = 0.0, fb = 0.0;
    uint64_t n = 0, nfb = 0;

    for (uint16_t r = 0; r < f->height; r++) {
        for (uint16_t c = 0; c < f->width; c++) {
            size_t i = (size_t)r * f->width + c;
            double ref = smooth_pattern((r + 0.5) / f->height, (c + 0.5) / f->width);
            if (!isnan(f->data[i])) {
                double d = f->data[i] - ref;
                se += d * d;
                n++;
            }
            if (f->n_images == 2 && !isnan(f->data[i]) && !isnan(f->data[npix + i])) {
                fb += fabs(f->data[i] - f->data[npix + i]);
                nfb++;
            }
        }
    }
    res->rms_err = n ? sqrt(se / n) : INFINITY;
    res->fb_diff = nfb ? fb / nfb : INFINITY;
    res->empty_frac = (double)f->n_empty / ((double)npix * f->n_images);
    res->used = f->n_used;

    if (res->dump_name && getenv("DUMP_PGM")) {
        FILE *fp = fopen(res->dump_name, "wb");
        if (fp) {
            fprintf(fp, "P5\n%u %u\n255\n", f->width, (unsigned)f->height * f->n_images);
            for (size_t i = 0; i < npix * f->n_images; i++) {
                double v = isnan(f->data[i]) ? 0.0 : f->data[i] / 16383.0 * 255.0;
                fputc((int)fmin(255.0, fmax(0.0, v)), fp);
            }
            fclose(fp);
            printf("  wrote %s\n", res->dump_name);
        }
    }
}

/* Standard scan: slow sawtooth 3 Hz, fast sine 1 kHz, acq_dec 50 (as in image_test_2.py). */
static result_t run_scan(bidir_t bidir, double delay, double frames, const char *dump)
{
    static int16_t w1[WFM1_MAX_LEN], w2[WFM2_MAX_LEN];

    wf_t slow = { .shape = WF_SAWTOOTH, .rise = 0.975, .amp = 8000, .offset = -500, .freq = 3.0 };
    wf_t fast = { .shape = WF_SINE, .amp = 6000, .offset = 1000, .freq = 1000.0 };
    scan_quantise(&slow, 256, WFM1_MAX_LEN);
    scan_quantise(&fast, 256, WFM2_MAX_LEN);
    scan_fill(&slow, w1);
    scan_fill(&fast, w2);

    const uint32_t acq_dec = 50;
    const double   rate = FPGA_CLK_HZ / acq_dec;
    double x_lo, x_hi, y_lo, y_hi;
    scan_range(&slow, &x_lo, &x_hi);
    scan_range(&fast, &y_lo, &y_hi);
    double cmd_lo = y_lo, cmd_hi = y_hi;
    double margin = 0.02 * (y_hi - y_lo);
    y_lo += margin;
    y_hi -= margin;

    result_t res = { .dump_name = dump };
    recon_cfg_t *c = &res.cfg;
    recon_auto_size(rate, slow.freq_actual, fast.freq_actual, slow.rise, 1920, 1080,
                    &c->width, &c->height);
    c->x_lo = x_lo; c->x_hi = x_hi;
    c->y_lo = y_lo; c->y_hi = y_hi;
    c->fast_gain = 1.25;            /* sensor = (mirror - 300) / 1.25 in the simulator */
    c->fast_offset = 300.0;
    c->delay = delay;
    c->v_scale = 1.0 / (acq_dec - 1);
    c->bidir = bidir;
    c->dir_window = 4;
    if (autocal) {
        c->fast_gain = 10.0;            /* far off: a cap from this gain would shrink the width */
        c->width_auto = 1;
        c->sensor_step = 4.0;
        c->fast_offset = 0.0;
        c->fast_auto = 1;
        c->fast_cycles = fast.freq_actual / rate;
        c->fast_cmd_lo = cmd_lo;
        c->fast_cmd_hi = cmd_hi;
    }

    sim_cfg_t sc = {
        .wfm1 = w1, .len1 = slow.len, .dec1 = slow.dec,
        .wfm2 = w2, .len2 = fast.len, .dec2 = fast.dec,
        .acq_dec = acq_dec, .fast_lag_cycles = 3000.0,
        .sensor_gain = 1.25, .sensor_offset = 300.0, .noise = 0.0,
        .x_lo = x_lo, .x_hi = x_hi, .y_lo = y_lo, .y_hi = y_hi,
        .pattern = smooth_pattern, .seed = 7,
    };
    sim_t sim;
    sim_init(&sim, &sc);

    recon_t *r = recon_create(c, on_frame, &res);
    if (!r) {
        res.rms_err = INFINITY;
        return res;
    }
    record_t buf[4096];
    uint64_t total = (uint64_t)(frames * rate / slow.freq_actual);
    for (uint64_t done = 0; done < total; done += 4096) {
        sim_next(&sim, buf, 4096);
        recon_push(r, buf, 4096);
    }
    recon_fast_cal(r, &res.fcal);
    recon_destroy(r);
    return res;
}

static void test_recon_sum(void)
{
    printf("test_recon_sum\n");
    result_t res = run_scan(BIDIR_SUM, 0.5, 3.2, "frame_sum.pgm");
    printf("  %ux%u, frames %d, rms %.1f counts, empty %.1f %%, used %llu\n",
           res.cfg.width, res.cfg.height, res.n_frames, res.rms_err, 100 * res.empty_frac,
           (unsigned long long)res.used);
    CHECK(res.n_frames >= 2, "expected >= 2 complete frames, got %d", res.n_frames);
    CHECK(res.rms_err < 0.02 * 16383, "rms error %.1f too large", res.rms_err);
    CHECK(res.empty_frac < 0.10, "too many empty pixels: %.1f %%", 100 * res.empty_frac);
}

static void test_fast_autocal(void)
{
    printf("test_fast_autocal\n");
    autocal = 1;
    result_t res = run_scan(BIDIR_SUM, 0.5, 3.2, NULL);
    autocal = 0;
    printf("  state %d, gain %.4f (true 1.25), offset %.1f (true 300), sensor amp %.1f mid %.1f, "
           "frames %d, rms %.1f, empty %.1f %%\n", res.fcal.state, res.fcal.gain, res.fcal.offset,
           res.fcal.sensor_amp, res.fcal.sensor_mid, res.n_frames, res.rms_err, 100 * res.empty_frac);
    CHECK(res.fcal.state == FCAL_DONE, "calibration not done (state %d)", res.fcal.state);
    CHECK(fabs(res.fcal.gain / 1.25 - 1.0) < 0.005, "gain %.4f, expected 1.25", res.fcal.gain);
    CHECK(fabs(res.fcal.offset - 300.0) < 20.0, "offset %.1f, expected 300", res.fcal.offset);
    CHECK(res.fcal.width == res.cfg.width, "width %u, expected %u (cap from the fitted gain)",
          res.fcal.width, res.cfg.width);
    CHECK(res.n_frames >= 2, "expected >= 2 complete frames, got %d", res.n_frames);
    CHECK(res.rms_err < 0.02 * 16383, "rms error %.1f too large", res.rms_err);
    CHECK(res.empty_frac < 0.10, "too many empty pixels: %.1f %%", 100 * res.empty_frac);
}

/* Fast mirror driven by an external generator (OUT2 unused): frequency, scale and size are
   all taken from the sensor at start. */
static void test_fast_external(void)
{
    printf("test_fast_external\n");
    static int16_t w1[WFM1_MAX_LEN], w2[WFM2_MAX_LEN];
    wf_t slow = { .shape = WF_SAWTOOTH, .rise = 0.975, .amp = 8000, .offset = -500, .freq = 3.0 };
    wf_t fast = { .shape = WF_SINE, .amp = 0, .offset = 0, .freq = 1000.0 };   /* OUT2 idle */
    scan_quantise(&slow, 256, WFM1_MAX_LEN);
    scan_quantise(&fast, 256, WFM2_MAX_LEN);
    scan_fill(&slow, w1);
    scan_fill(&fast, w2);

    const uint32_t acq_dec = 50;
    const double   rate = FPGA_CLK_HZ / acq_dec, f_ext = 731.3, amp = 7000, off = -1200;
    double x_lo, x_hi;
    scan_range(&slow, &x_lo, &x_hi);

    result_t res = { 0 };
    recon_cfg_t *c = &res.cfg;
    *c = (recon_cfg_t){
        .x_lo = x_lo, .x_hi = x_hi, .y_lo = -0.96 * 16384, .y_hi = 0.96 * 16384,
        .fast_gain = 1.0, .delay = 0.5, .v_scale = 1.0 / (acq_dec - 1), .bidir = BIDIR_SUM,
        .dir_window = 4, .fast_auto = 1, .fast_external = 1,
        .fast_cmd_lo = -16384, .fast_cmd_hi = 16384,
        .sample_rate = rate, .slow_freq = slow.freq_actual, .slow_frac = slow.rise,
        .roi_h = 1.0, .roi_w = 0.96, .width_auto = 1, .sensor_step = 4.0,
    };
    sim_cfg_t sc = {
        .wfm1 = w1, .len1 = slow.len, .dec1 = slow.dec,
        .wfm2 = w2, .len2 = fast.len, .dec2 = fast.dec,
        .acq_dec = acq_dec, .sensor_gain = 1.25, .sensor_offset = 300.0, .noise = 2.0,
        .x_lo = x_lo, .x_hi = x_hi, .y_lo = off - 0.96 * amp, .y_hi = off + 0.96 * amp,
        .ext_amp = amp, .ext_offset = off, .ext_freq = f_ext,
        .pattern = smooth_pattern, .seed = 11,
    };
    sim_t sim;
    sim_init(&sim, &sc);
    recon_t *r = recon_create(c, on_frame, &res);
    CHECK(r != NULL, "external config rejected");
    if (!r)
        return;
    record_t buf[4096];
    uint64_t total = (uint64_t)(3.5 * rate / slow.freq_actual);
    for (uint64_t done = 0; done < total; done += 4096) {
        sim_next(&sim, buf, 4096);
        recon_push(r, buf, 4096);
    }
    recon_fast_cal(r, &res.fcal);
    recon_destroy(r);

    uint16_t aw, ah;
    recon_auto_size(rate, slow.freq_actual, f_ext, slow.rise, 1920, 1080, &aw, &ah);
    printf("  state %d, %.3f Hz (true %.1f), sensor amp %.1f (true %.1f) mid %.1f (true %.1f), "
           "width %u (auto %u), frames %d, rms %.1f, empty %.1f %%\n", res.fcal.state,
           res.fcal.fast_hz, f_ext, res.fcal.sensor_amp, amp / 1.25, res.fcal.sensor_mid,
           (off - 300) / 1.25, res.fcal.width, (unsigned)(aw * 0.96), res.n_frames, res.rms_err,
           100 * res.empty_frac);
    CHECK(res.fcal.state == FCAL_DONE, "calibration not done (state %d)", res.fcal.state);
    CHECK(fabs(res.fcal.fast_hz / f_ext - 1.0) < 1e-4, "frequency %.4f, expected %.1f",
          res.fcal.fast_hz, f_ext);
    CHECK(fabs(res.fcal.sensor_amp / (amp / 1.25) - 1.0) < 0.003, "amplitude %.1f", res.fcal.sensor_amp);
    CHECK(fabs(res.fcal.sensor_mid - (off - 300) / 1.25) < 5.0, "centre %.1f", res.fcal.sensor_mid);
    CHECK(res.fcal.width == (uint16_t)(aw * 0.96), "width %u", res.fcal.width);
    CHECK(res.n_frames >= 2, "expected >= 2 complete frames, got %d", res.n_frames);
    CHECK(res.rms_err < 0.02 * 16383, "rms error %.1f too large", res.rms_err);
    CHECK(res.empty_frac < 0.15, "too many empty pixels: %.1f %%", 100 * res.empty_frac);
}

static void test_delay_alignment(void)
{
    printf("test_delay_alignment\n");
    double delays[] = { 0.0, 0.5, 2.0, -2.0 };
    double diff[4];
    for (int i = 0; i < 4; i++) {
        result_t res = run_scan(BIDIR_BOTH, delays[i], 2.2, NULL);   /* first complete frame ends ~2 periods in */
        CHECK(res.n_frames >= 1, "no complete frame for delay %.1f", delays[i]);
        diff[i] = res.fb_diff;
        printf("  delay %+.1f: mean |fwd-bwd| = %.1f counts\n", delays[i], diff[i]);
    }
    CHECK(diff[1] <= diff[0] && diff[1] < diff[2] && diff[1] < diff[3],
          "delay 0.5 should align forward/backward best");
}

static void test_invalid_cfg(void)
{
    printf("test_invalid_cfg\n");
    recon_cfg_t c = { .width = 10, .height = 10, .x_lo = 0, .x_hi = 1, .y_lo = 0, .y_hi = 1,
                      .fast_gain = 1, .v_scale = 1, .dir_window = 4 };
    result_t res = { 0 };
    recon_t *r = recon_create(&c, on_frame, &res);
    CHECK(r != NULL, "valid config rejected");
    recon_destroy(r);
    c.delay = RECON_MAX_DELAY + 1;
    CHECK(recon_create(&c, on_frame, &res) == NULL, "delay out of range accepted");
    c.delay = 0;
    c.x_hi = c.x_lo;
    CHECK(recon_create(&c, on_frame, &res) == NULL, "empty x range accepted");
}

int main(void)
{
    test_quantise();
    test_fill();
    test_invalid_cfg();
    test_recon_sum();
    test_fast_autocal();
    test_fast_external();
    test_delay_alignment();
    printf(failures ? "%d FAILURE(S)\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
