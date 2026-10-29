#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "hw/hw.h"
#include "recon/recon.h"
#include "recon/scan.h"

/* Pixel formats on the wire (frame header dtype). Integer formats are scaled per frame. */
typedef enum { PIX_FLOAT32 = 0, PIX_UINT16 = 1, PIX_UINT8 = 2 } pixel_t;

#define COUNTS_PER_VOLT 32767.0
#define EXT_FAST_RANGE  16384.0   /* external fast drive: virtual +- range the measured sweep maps onto */   /* RP connector: +-1 V <-> +-32767 (to be verified on hardware) */

/* User-facing settings (protocol keys). Voltages are at the galvo/driver side. */
typedef struct {
    wf_shape_t slow_shape;  double slow_rise, slow_freq, slow_amp, slow_offset;
    wf_shape_t fast_shape;  double fast_freq, fast_amp, fast_offset;
    int        fast_external;            /* fast.source: 0 internal (OUT2), 1 external generator */
    unsigned   acq_dec, int_dec;
    unsigned   img_w, img_h;             /* 0 = auto */
    bidir_t    bidir;
    pixel_t    dtype;                    /* wire format of the pixels */
    double     roi[4];                   /* x0 x1 y0 y1, fractions of the full range */
    double     cal_delay, cal_fast_gain, cal_fast_offset;
    unsigned   cal_fast_auto;            /* 1: fit the fast sensor at start (overrides the two above) */
    double     hw_slow_gain, hw_fast_gain, hw_sensor_gain;   /* galvo V per RP V */
} config_t;

/* Everything computed from a config_t for one run. */
typedef struct {
    wf_t        slow, fast;
    int16_t     wfm1[WFM1_MAX_LEN], wfm2[WFM2_MAX_LEN];
    hw_setup_t  hw;
    recon_cfg_t recon;
    double      rate;                    /* records per second */
    double      sensor_levels;           /* distinct fast-sensor values across the image */
} derived_t;

void config_defaults(config_t *c);

/* Returns 0, or -1 with a message in err. Does not check cross-key consistency. */
int  config_set(config_t *c, const char *key, const char *value, char *err, size_t errlen);

/* Keys that may change while running (applied at the next frame). */
int  config_is_live(const char *key);

/* Builds waveforms, FPGA setup and reconstruction config. Returns 0, or -1 with err. */
int  config_derive(const config_t *c, derived_t *d, char *err, size_t errlen);

/* key=value lines; derived values (actual frequencies, sizes) are appended when d != NULL. */
void config_format(const config_t *c, const derived_t *d, char *buf, size_t len);

#endif
