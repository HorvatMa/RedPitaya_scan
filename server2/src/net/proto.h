#ifndef PROTO_H
#define PROTO_H

/* Protocol v1, see documentation/PROTOCOL.md. All fields little-endian. */

#include <stddef.h>
#include <stdint.h>

#include "../app.h"

#define PROTO_MAGIC     0x49535052u     /* "RPSI" */
#define PROTO_VERSION   1
#define PROTO_HDR_SIZE  12
#define PROTO_FHDR_SIZE 64
#define PROTO_MAX_CMD   4096            /* longest accepted command payload */

enum { MSG_CMD = 1, MSG_REPLY = 2, MSG_FRAME = 3, MSG_STATUS = 4 };

void proto_pack_hdr(uint8_t out[PROTO_HDR_SIZE], uint16_t type, uint32_t length);

/* Returns 0 and fills type/length, or -1 for a bad magic/version. */
int  proto_parse_hdr(const uint8_t in[PROTO_HDR_SIZE], uint16_t *type, uint32_t *length);

void proto_pack_frame_hdr(uint8_t out[PROTO_FHDR_SIZE], const app_frame_t *f);

#endif
