#ifndef RECON_H
#define RECON_H

#include <stddef.h>
#include <stdint.h>

#include "../common.h"

#define RECON_MAX_DELAY 63      /* |delay| limit in samples */

typedef enum { BIDIR_SUM, BIDIR_FORWARD, BIDIR_BACKWARD, BIDIR_BOTH } bidir_t;

typedef struct {
    uint16_t width, height;
    double   x_lo, x_hi;        /* slow command range mapped onto rows, counts      */
    double   y_lo, y_hi;        /* fast range mapped onto columns, command counts    */
    double   fast_gain;         /* y_cal = fast_gain * y + fast_offset               */
    double   fast_offset;
    double   delay;             /* v[i] is paired with position(i - delay), samples  */
    double   v_scale;           /* pixel value = v * v_scale                         */
    bidir_t  bidir;
    int      slow_bidir;        /* 1: both slow directions are image (triangle scan) */
    unsigned dir_window;        /* samples used for the fast-direction estimate      */
} recon_cfg_t;

typedef struct {
    uint32_t     frame_id;
    uint16_t     width, height;
    uint8_t      n_images;      /* 2 for BIDIR_BOTH: [0] forward, [1] backward */
    int8_t       slow_dir;      /* +1 rising, -1 falling slow sweep            */
    uint64_t     n_records;     /* records seen during this frame              */
    uint64_t     n_used;        /* records binned into pixels                  */
    uint32_t     n_empty;       /* pixels without samples (value NaN)          */
    const float *data;          /* n_images * height * width, row-major        */
} frame_t;

typedef void (*frame_cb_t)(void *ctx, const frame_t *frame);

typedef struct recon recon_t;

/* Returns NULL on invalid config or allocation failure. */
recon_t *recon_create(const recon_cfg_t *cfg, frame_cb_t cb, void *ctx);
void     recon_destroy(recon_t *r);

/* Feed records in acquisition order. Calls cb for every completed frame. */
void     recon_push(recon_t *r, const record_t *rec, size_t n);

/* Drop the partial frame and wait for the next slow turn-around (e.g. after data loss). */
void     recon_resync(recon_t *r);

/*
 * Suggested image size for a sine fast axis: about two samples per column in the
 * centre of the sweep, and one row per fast period (one forward + one backward sweep)
 * during the slow image sweep.
 */
void     recon_auto_size(double sample_rate, double f_slow, double f_fast,
                         double slow_image_fraction, uint16_t max_w, uint16_t max_h,
                         uint16_t *w, uint16_t *h);

#endif
