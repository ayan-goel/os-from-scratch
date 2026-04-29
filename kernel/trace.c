/*
 * trace.c — kernel scheduling event ring buffer.
 *
 * Single-writer design: emit is called from trap_handler and from
 * sched() callers (sys_yield, sys_sleep, sys_exit, kernel_yield,
 * kernel_sleep, proc_alloc). All of those paths execute with
 * interrupts disabled or in the context of a single CPU, so no
 * locking is needed.
 *
 * The ring stores events by absolute index and uses a modulo mask to
 * locate the slot. When the write index passes the ring size, older
 * events get overwritten. Readers (the `trace` command) clamp to the
 * resident window.
 */

#include "trace.h"

extern volatile uint64_t ticks;   /* defined in trap.c */

static trace_event_t ring[TRACE_RING_SIZE];
static uint64_t      next_idx = 0;   /* absolute write position */

void trace_emit(trace_event_type_t type, int pid) {
    trace_event_t *e = &ring[next_idx % TRACE_RING_SIZE];
    e->tick = (uint32_t)ticks;
    e->type = (uint8_t)type;
    e->pid  = (uint8_t)pid;
    e->_pad = 0;
    next_idx++;
}

uint64_t trace_total(uint64_t *out_start, uint64_t *out_count) {
    uint64_t total = next_idx;
    uint64_t count = total < TRACE_RING_SIZE ? total : TRACE_RING_SIZE;
    uint64_t start = total - count;
    if (out_start) *out_start = start;
    if (out_count) *out_count = count;
    return total;
}

int trace_get(uint64_t abs_idx, trace_event_t *out) {
    uint64_t start;
    uint64_t count;
    trace_total(&start, &count);
    if (abs_idx < start || abs_idx >= start + count)
        return 0;
    *out = ring[abs_idx % TRACE_RING_SIZE];
    return 1;
}

void trace_reset(void) {
    next_idx = 0;
}
