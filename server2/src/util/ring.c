#include "ring.h"

#include <stdlib.h>
#include <string.h>

int ring_init(ring_t *r, size_t capacity_pow2)
{
    if (capacity_pow2 < 2 || (capacity_pow2 & (capacity_pow2 - 1)))
        return -1;
    r->buf = malloc(capacity_pow2 * sizeof(*r->buf));
    if (!r->buf)
        return -1;
    r->mask = capacity_pow2 - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

void ring_free(ring_t *r)
{
    free(r->buf);
    r->buf = NULL;
}

void ring_reset(ring_t *r)
{
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}

size_t ring_capacity(const ring_t *r)
{
    return r->mask + 1;
}

size_t ring_used(ring_t *r)
{
    return atomic_load_explicit(&r->head, memory_order_acquire) -
           atomic_load_explicit(&r->tail, memory_order_acquire);
}

/* copy n records between a linear buffer and the ring starting at index pos (wraps) */
static void ring_copy(ring_t *r, size_t pos, record_t *lin, size_t n, int to_ring)
{
    size_t i = pos & r->mask;
    size_t first = n < r->mask + 1 - i ? n : r->mask + 1 - i;
    if (to_ring) {
        memcpy(r->buf + i, lin, first * sizeof(*lin));
        memcpy(r->buf, lin + first, (n - first) * sizeof(*lin));
    } else {
        memcpy(lin, r->buf + i, first * sizeof(*lin));
        memcpy(lin + first, r->buf, (n - first) * sizeof(*lin));
    }
}

size_t ring_write(ring_t *r, const record_t *src, size_t n)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (n > r->mask + 1 - (head - tail))
        return 0;
    ring_copy(r, head, (record_t *)src, n, 1);
    atomic_store_explicit(&r->head, head + n, memory_order_release);
    return n;
}

size_t ring_read(ring_t *r, record_t *dst, size_t max)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t n = head - tail;
    if (n > max)
        n = max;
    if (n == 0)
        return 0;
    ring_copy(r, tail, dst, n, 0);
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}
