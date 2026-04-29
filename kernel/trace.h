#ifndef TRACE_H
#define TRACE_H

#include "defs.h"

/*
 * trace.h — lock-free scheduling event ring for Phase 3 instrumentation.
 *
 * Every scheduling-relevant transition emits one 8-byte record into a
 * static ring buffer. The shell's `trace` command dumps the tail of
 * that ring as ASCII lines; tools/parse_trace.py parses the dump into
 * CSVs and Gantt charts.
 *
 * Event types are a uint8 enum. pid is stored as uint8 — enough for
 * a single Phase 3 experiment run (pids don't exceed 255 unless we do
 * 256+ fork/execs, which no current workload does).
 */

typedef enum {
    EV_SPAWN   = 1,   /* proc_alloc returned a new slot */
    EV_EXIT    = 2,   /* sys_exit about to sched away as ZOMBIE */
    EV_RUN     = 3,   /* scheduler entering RUNNING (before switch_context) */
    EV_YIELD   = 4,   /* voluntary sys_yield / kernel_yield */
    EV_PREEMPT = 5,   /* timer-driven preemption of a user proc */
    EV_SLEEP   = 6,   /* sys_sleep / kernel_sleep */
    EV_WAKE    = 7,   /* timer handler wakes a sleeping proc */
} trace_event_type_t;

typedef struct {
    uint32_t tick;    /* global tick counter snapshot */
    uint8_t  type;    /* trace_event_type_t */
    uint8_t  pid;     /* proc->pid, truncated to 8 bits */
    uint16_t _pad;    /* reserved */
} trace_event_t;

/* 16 K entries × 8 bytes = 128 KB. Static array in .bss. */
#define TRACE_RING_SIZE 16384

/* Write one event to the ring. O(1), no allocation. Safe from traps
 * because emit uses a monotonic write index and a bounded-size array. */
void trace_emit(trace_event_type_t type, int pid);

/*
 * Read interface for the `trace` command. Returns the total number of
 * events ever written (may exceed TRACE_RING_SIZE). Out-parameter
 * `out_start` is the index of the oldest still-resident entry
 * (max(0, total - TRACE_RING_SIZE)); `out_count` is how many entries
 * are still resident (min(total, TRACE_RING_SIZE)).
 */
uint64_t trace_total(uint64_t *out_start, uint64_t *out_count);

/*
 * Fetch the entry at absolute index `abs_idx`. Returns 1 if the entry
 * is still resident in the ring (i.e. has not been overwritten), 0 if
 * the caller asked for an entry that's been wrapped past. On success
 * `*out` is filled with the event.
 */
int trace_get(uint64_t abs_idx, trace_event_t *out);

/* Reset the ring to empty. Used by `trace clear`. */
void trace_reset(void);

#endif /* TRACE_H */
