/*
 * Simulated FPGA: produces records in real time from the configured waveforms with the
 * FPGA-faithful simulator (sim.c). The "sample" is fixed in command space, so changing the
 * scan amplitude/offset zooms into it like a real specimen.
 */
#include "hw.h"
#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hw {
    int16_t   wfm1[WFM1_MAX_LEN], wfm2[WFM2_MAX_LEN];
    sim_t     sim;
    int       configured, running;
    double    rate;             /* records per second */
    double    t0;               /* start time, s */
    uint64_t  produced;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/*
 * Test specimen in absolute DAC counts (fx: fast, sy: slow), with structure on every scale:
 * a dot lattice (period 128 counts), grid lines every 1024 counts, and a slow gradient.
 * The default scan (+-~370 counts) then shows a few dots; zooming reveals single dots.
 */
static double specimen(double fx, double sy)
{
    double gx = fx / 128.0, gy = sy / 128.0;
    double dx = gx - floor(gx) - 0.5, dy = gy - floor(gy) - 0.5;
    double size = 0.12 + 0.08 * sin(0.37 * floor(gx)) * cos(0.23 * floor(gy));  /* varying dots */
    double v = 3000.0 + 0.02 * (fx + sy);
    v += 7000.0 * exp(-(dx * dx + dy * dy) / (2.0 * size * size));
    double lx = fabs(fx / 1024.0 - floor(fx / 1024.0 + 0.5)) * 1024.0;
    double ly = fabs(sy / 1024.0 - floor(sy / 1024.0 + 0.5)) * 1024.0;
    if (lx < 6.0 || ly < 6.0)
        v += 4000.0;
    return v;
}

/* sim pattern(u, w) is called with u = slow, w = fast normalised to the ranges below */
static double pattern(double slow, double fast)
{
    return specimen(-32768.0 + 65535.0 * fast, -32768.0 + 65535.0 * slow);
}

hw_t *hw_open(void)
{
    return calloc(1, sizeof(hw_t));
}

void hw_close(hw_t *hw)
{
    free(hw);
}

const char *hw_name(const hw_t *hw)
{
    (void)hw;
    return "fake";
}

int hw_configure(hw_t *hw, const hw_setup_t *s)
{
    if (hw->running || s->len1 == 0 || s->len1 > WFM1_MAX_LEN || s->len2 == 0 ||
        s->len2 > WFM2_MAX_LEN || s->dec1 == 0 || s->dec2 == 0 || s->acq_dec < 2)
        return -1;
    memcpy(hw->wfm1, s->wfm1, s->len1 * sizeof(int16_t));
    memcpy(hw->wfm2, s->wfm2, s->len2 * sizeof(int16_t));

    sim_cfg_t c = {
        .wfm1 = hw->wfm1, .len1 = s->len1, .dec1 = s->dec1,
        .wfm2 = hw->wfm2, .len2 = s->len2, .dec2 = s->dec2,
        .acq_dec = s->acq_dec,
        .fast_lag_cycles = 2000.0,       /* galvo lag; the sensor reports the real position */
        .sensor_gain = s->sim_sensor_gain > 0 ? s->sim_sensor_gain : 1.0,
        .sensor_offset = 0.0, .noise = 3.0,
        .sensor_lsb = SENSOR_LSB,               /* 14-bit ADC << 2 in input_selector.v */
        .x_lo = -32768, .x_hi = 32767, .y_lo = -32768, .y_hi = 32767,
        .pattern = pattern, .seed = 12345,
    };
    /* RPSIPM_FAKE_EXT="freq_hz amp offset" (mirror counts): the fast galvo follows an external
       sine generator instead of OUT2 (tests of fast.source external) */
    const char *ext = getenv("RPSIPM_FAKE_EXT");
    if (ext && sscanf(ext, "%lf %lf %lf", &c.ext_freq, &c.ext_amp, &c.ext_offset) != 3)
        c.ext_amp = 0.0;
    sim_init(&hw->sim, &c);
    hw->rate = FPGA_CLK_HZ / s->acq_dec;
    hw->configured = 1;
    return 0;
}

int hw_start(hw_t *hw)
{
    if (!hw->configured)
        return -1;
    sim_init(&hw->sim, &hw->sim.cfg);        /* generators restart at t = 0 */
    hw->t0 = now_s();
    hw->produced = 0;
    hw->running = 1;
    return 0;
}

void hw_stop(hw_t *hw)
{
    hw->running = 0;
}

long hw_read(hw_t *hw, record_t *dst, size_t max, int timeout_ms)
{
    if (!hw->running)
        return -1;
    double deadline = now_s() + timeout_ms / 1000.0;
    for (;;) {
        uint64_t due = (uint64_t)((now_s() - hw->t0) * hw->rate);
        if (due > hw->produced) {
            size_t n = due - hw->produced;
            if (n > max)
                n = max;
            sim_next(&hw->sim, dst, n);
            hw->produced += n;
            return (long)n;
        }
        if (now_s() >= deadline)
            return 0;
        struct timespec ts = { 0, 2000000 };     /* FPGA delivers chunks every few ms too */
        nanosleep(&ts, NULL);
    }
}
