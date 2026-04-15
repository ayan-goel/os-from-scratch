#ifndef RING_H
#define RING_H

#include "defs.h"

/*
 * ring.h — simple byte ring buffer.
 *
 * Fixed-capacity circular buffer over a caller-provided backing array.
 * Single-producer / single-consumer is safe on a single-core kernel with
 * no preemption of kernel threads — which is our current model. If we
 * ever add SMP or preempt kernel threads, these need to grow locks.
 *
 * Used for:
 *   - UART RX buffer (shell keyboard input)
 *   - Process output buffer (TUI shell panel content)
 */

typedef struct {
    uint8_t  *buf;
    uint64_t  cap;   /* capacity in bytes (must be power of 2 for cheap mask) */
    uint64_t  head; /* write index (producer) */
    uint64_t  tail; /* read  index (consumer) */
} ring_t;

/* Initialize a ring over `backing` of size `cap`. cap must be a power of 2. */
void ring_init(ring_t *r, uint8_t *backing, uint64_t cap);

/* True if no bytes available to read. */
int ring_empty(const ring_t *r);

/* Number of bytes currently available to read. */
uint64_t ring_size(const ring_t *r);

/*
 * Push one byte. If the ring is full, the oldest byte is dropped
 * (the tail advances). This is intentional for our use cases —
 * keyboard input during a long command can drop; process stdout
 * during a slow TUI render can drop old scrollback. Callers that
 * care can check ring_size first.
 */
void ring_push(ring_t *r, uint8_t byte);

/*
 * Push a NUL-terminated string. Uses ring_push for each byte.
 */
void ring_puts(ring_t *r, const char *s);

/* Push an unsigned decimal integer (no leading zeros). */
void ring_putdec(ring_t *r, uint64_t val);

/* Push "0x" followed by 16 hex digits. */
void ring_puthex64(ring_t *r, uint64_t val);

/*
 * Pop one byte. Returns 1 on success and writes to *out; returns 0
 * if the ring was empty (*out untouched).
 */
int ring_pop(ring_t *r, uint8_t *out);

/*
 * Copy up to `max` of the most recent bytes into `out`. Returns the
 * number of bytes actually copied (<= max and <= ring_size). Does
 * NOT modify the ring — this is a peek at the tail, used by the TUI
 * renderer to draw the most recent scrollback each frame.
 */
uint64_t ring_read_tail(const ring_t *r, uint8_t *out, uint64_t max);

/* Drop all contents (clear, for `cmd_clear` in Phase 2.5). */
void ring_reset(ring_t *r);

#endif /* RING_H */
