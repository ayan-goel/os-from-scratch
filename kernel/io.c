/*
 * io.c — shared kernel I/O state and kernel-thread helpers.
 */

#include "io.h"
#include "ring.h"
#include "proc.h"
#include "defs.h"

/* ticks is defined in trap.c and is the global timer tick counter. */
extern volatile uint64_t ticks;

/* ── Global output ring ──────────────────────────────────────────── */

static uint8_t output_buf[OUTPUT_RING_SIZE];
ring_t output_ring;

void io_init(void) {
    ring_init(&output_ring, output_buf, OUTPUT_RING_SIZE);
}

/* ── Convenience writers ─────────────────────────────────────────── */

void out_putc(char c) {
    ring_push(&output_ring, (uint8_t)c);
}

void out_puts(const char *s) {
    /* Translate \n to \r\n so lines display cleanly when the ring
     * bytes are emitted directly to a raw-mode terminal during
     * testing / early boot. The TUI renderer strips CRs when drawing
     * into the shell panel, so the only cost is one extra byte per
     * newline in the ring. */
    while (*s) {
        if (*s == '\n')
            ring_push(&output_ring, '\r');
        ring_push(&output_ring, (uint8_t)*s);
        s++;
    }
}

void out_putdec(uint64_t v) {
    ring_putdec(&output_ring, v);
}

void out_puthex64(uint64_t v) {
    ring_puthex64(&output_ring, v);
}

/* ── Kernel-thread helpers ───────────────────────────────────────── */

void kernel_yield(void) {
    current->state = RUNNABLE;
    sched();
}

void kernel_sleep(uint64_t ticks_to_sleep) {
    current->wake_tick = ticks + ticks_to_sleep;
    current->state = SLEEPING;
    sched();
}
