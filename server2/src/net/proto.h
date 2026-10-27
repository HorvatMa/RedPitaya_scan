#ifndef PROTO_H
#define PROTO_H

/* Protocol v1, see documentation/PROTOCOL.md. All fields little-endian. */

#include <stddef.h>
#include <stdint.h>

#include "../app.h"

#define PROTO_MAGIC     0x49535052u     /* "RPSI" */
#define PROTO_VERSION   1
#define PROTO_HDR_SIZE  12
#define PROTO_FHDR_SIZE 72
#define PROTO_MAX_CMD   4096            /* longest accepted command payload */

enum { MSG_CMD = 1, MSG_REPLY = 2, MSG_FRAME = 3, MSG_STATUS = 4 };

void proto_pack_hdr(uint8_t out[PROTO_HDR_SIZE], uint16_t type, uint32_t length);

/* Returns 0 and fills type/length, or -1 for a bad magic/version. */
int  proto_parse_hdr(const uint8_t in[PROTO_HDR_SIZE], uint16_t *type, uint32_t *length);

/* dtype and the value range [lo, hi] describe the encoded pixels (see encode below). */
void proto_pack_frame_hdr(uint8_t out[PROTO_FHDR_SIZE], const app_frame_t *f,
                          uint8_t dtype, float lo, float hi);

/*
 * Bytes per pixel of a dtype (0 = float32, 1 = uint16, 2 = uint8), or 0 if unknown.
 * Integer encoding: code 0 = empty (NaN); finite v -> 1 + round((v - lo) / (hi - lo) * (max - 1)),
 * max = 65535 or 255, with lo/hi the finite min/max of the frame.
 */
size_t proto_pixel_size(uint8_t dtype);

/* Finite min/max of n floats (both NaN if there are none). */
void   proto_minmax(const float *v, size_t n, float *lo, float *hi);

/* Encode n floats into dst (proto_pixel_size(dtype) * n bytes, little-endian). */
void   proto_encode(const float *src, size_t n, uint8_t dtype, float lo, float hi, void *dst);

#endif
