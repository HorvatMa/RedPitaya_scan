#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <stdint.h>

#include "hw/hw.h"

#define FRAME_FLAG_DATA_LOST 0x1

/* A finished frame as handed to the network sender. */
typedef struct {
    uint32_t frame_id;          /* assigned at reconstruction, restarts at 0 every run */
    uint16_t width, height;
    uint8_t  n_images;
    uint16_t flags;
    uint64_t t_start_ns, t_end_ns;
    uint32_t n_records, n_discarded, n_empty;
    float    slow_freq, fast_freq;
    float    roi[4];
    float   *data;              /* n_images * height * width */
} app_frame_t;

typedef struct app app_t;

app_t *app_create(hw_t *hw);
void   app_destroy(app_t *a);

/* Executes one protocol command line; writes the reply ("OK ..." / "ERR <code> ...") into out. */
void   app_command(app_t *a, const char *line, char *out, size_t outlen);

/* Stops a running scan (also used when the client disconnects). */
void   app_stop(app_t *a);

/*
 * Sender side: waits up to timeout_ms for the newest finished frame. The frame stays valid
 * until app_frame_release(). Returns NULL on timeout.
 */
const app_frame_t *app_frame_wait(app_t *a, int timeout_ms);
void   app_frame_release(app_t *a, const app_frame_t *f, int sent);

/* STATUS payload (key=value lines). */
void   app_status(app_t *a, char *buf, size_t len);

#endif
