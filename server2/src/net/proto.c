#include "proto.h"

#include <math.h>
#include <string.h>

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
    return p + 4;
}

static uint8_t *put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
    return p + 8;
}

static uint8_t *putf(uint8_t *p, float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return put32(p, v);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void proto_pack_hdr(uint8_t out[PROTO_HDR_SIZE], uint16_t type, uint32_t length)
{
    uint8_t *p = put32(out, PROTO_MAGIC);
    p = put16(p, PROTO_VERSION);
    p = put16(p, type);
    put32(p, length);
}

int proto_parse_hdr(const uint8_t in[PROTO_HDR_SIZE], uint16_t *type, uint32_t *length)
{
    if (get32(in) != PROTO_MAGIC || (in[4] | in[5] << 8) != PROTO_VERSION)
        return -1;
    *type = (uint16_t)(in[6] | in[7] << 8);
    *length = get32(in + 8);
    return 0;
}

void proto_pack_frame_hdr(uint8_t out[PROTO_FHDR_SIZE], const app_frame_t *f,
                          uint8_t dtype, float lo, float hi)
{
    uint8_t *p = put32(out, f->frame_id);
    p = put16(p, f->width);
    p = put16(p, f->height);
    *p++ = dtype;
    *p++ = f->n_images;
    p = put16(p, f->flags);
    p = put64(p, f->t_start_ns);
    p = put64(p, f->t_end_ns);
    p = put32(p, f->n_records);
    p = put32(p, f->n_discarded);
    p = put32(p, f->n_empty);
    p = putf(p, f->slow_freq);
    p = putf(p, f->fast_freq);
    for (int i = 0; i < 4; i++)
        p = putf(p, f->roi[i]);
    p = putf(p, lo);
    putf(p, hi);
}

size_t proto_pixel_size(uint8_t dtype)
{
    return dtype == 0 ? 4 : dtype == 1 ? 2 : dtype == 2 ? 1 : 0;
}

void proto_minmax(const float *v, size_t n, float *lo, float *hi)
{
    float a = INFINITY, b = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < a)
            a = v[i];
        if (v[i] > b)
            b = v[i];                  /* NaN compares false: skipped */
    }
    *lo = a <= b ? a : NAN;
    *hi = a <= b ? b : NAN;
}

void proto_encode(const float *src, size_t n, uint8_t dtype, float lo, float hi, void *dst)
{
    if (dtype == 0) {
        memcpy(dst, src, n * sizeof(float));
        return;
    }
    float maxc = dtype == 1 ? 65534.0f : 254.0f;            /* codes 1..max */
    float k = hi > lo ? maxc / (hi - lo) : 0.0f;
    for (size_t i = 0; i < n; i++) {
        float v = src[i];
        unsigned code = v == v ? 1u + (unsigned)((v - lo) * k + 0.5f) : 0u;   /* v != v: NaN */
        if (dtype == 1)
            ((uint16_t *)dst)[i] = (uint16_t)code;
        else
            ((uint8_t *)dst)[i] = (uint8_t)code;
    }
}
