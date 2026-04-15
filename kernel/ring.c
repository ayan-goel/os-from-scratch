/*
 * ring.c — byte ring buffer implementation.
 *
 * Capacity must be a power of 2 so we can wrap with a bitmask instead
 * of a modulo. The head/tail indices are free-running uint64_ts —
 * they never wrap in practice (2^64 bytes of push would take longer
 * than the universe has been around), and masking with cap-1 on
 * access gives the actual buffer slot.
 *
 * Full condition: head - tail == cap.
 * Empty condition: head == tail.
 * Size: head - tail.
 */

#include "ring.h"
#include "defs.h"

void ring_init(ring_t *r, uint8_t *backing, uint64_t cap) {
    /* Panic if cap isn't a power of 2 — required for the mask wrap. */
    KASSERT((cap & (cap - 1)) == 0, "ring_init: cap not power of 2");
    r->buf  = backing;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
}

int ring_empty(const ring_t *r) {
    return r->head == r->tail;
}

uint64_t ring_size(const ring_t *r) {
    return r->head - r->tail;
}

void ring_push(ring_t *r, uint8_t byte) {
    /* If full, drop the oldest byte so the newest always wins. */
    if (ring_size(r) == r->cap)
        r->tail++;
    r->buf[r->head & (r->cap - 1)] = byte;
    r->head++;
}

void ring_puts(ring_t *r, const char *s) {
    while (*s) {
        ring_push(r, (uint8_t)*s);
        s++;
    }
}

void ring_putdec(ring_t *r, uint64_t val) {
    char buf[21];
    int i = 0;
    if (val == 0) {
        ring_push(r, '0');
        return;
    }
    while (val > 0 && i < 20) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0)
        ring_push(r, (uint8_t)buf[--i]);
}

void ring_puthex64(ring_t *r, uint64_t val) {
    ring_push(r, '0');
    ring_push(r, 'x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        int n = (int)((val >> shift) & 0xF);
        ring_push(r, (uint8_t)(n < 10 ? '0' + n : 'a' + (n - 10)));
    }
}

int ring_pop(ring_t *r, uint8_t *out) {
    if (ring_empty(r))
        return 0;
    *out = r->buf[r->tail & (r->cap - 1)];
    r->tail++;
    return 1;
}

uint64_t ring_read_tail(const ring_t *r, uint8_t *out, uint64_t max) {
    uint64_t avail = ring_size(r);
    uint64_t n = avail < max ? avail : max;
    uint64_t start = r->head - n;  /* most recent n bytes start here */
    for (uint64_t i = 0; i < n; i++)
        out[i] = r->buf[(start + i) & (r->cap - 1)];
    return n;
}

void ring_reset(ring_t *r) {
    r->head = 0;
    r->tail = 0;
}
