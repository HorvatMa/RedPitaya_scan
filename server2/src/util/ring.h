#ifndef RING_H
#define RING_H

#include <stdatomic.h>
#include <stddef.h>

#include "../common.h"

/* Single-producer / single-consumer ring of records (acq thread -> recon thread). */
typedef struct {
    record_t      *buf;
    size_t         mask;
    atomic_size_t  head;    /* written by the producer */
    atomic_size_t  tail;    /* written by the consumer */
} ring_t;

int    ring_init(ring_t *r, size_t capacity_pow2);
void   ring_free(ring_t *r);
void   ring_reset(ring_t *r);                         /* only while both threads are stopped */
size_t ring_used(ring_t *r);
size_t ring_capacity(const ring_t *r);

/* All or nothing: returns n, or 0 if there is not enough free space. */
size_t ring_write(ring_t *r, const record_t *src, size_t n);
/* Returns the number of records copied (0 if empty). */
size_t ring_read(ring_t *r, record_t *dst, size_t max);

#endif
