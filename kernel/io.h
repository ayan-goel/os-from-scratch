#ifndef IO_H
#define IO_H

#include "defs.h"
#include "ring.h"

/*
 * io.h — shared kernel I/O state and helpers.
 *
 * Holds the global `output_ring` that is the single source of truth
 * for everything the user should see in the TUI shell panel. Both
 * user processes (via sys_write → syscall.c) and kernel code (shell
 * command results, boot messages after tui_start) push to it; the
 * TUI renderer reads the tail on every frame and draws it into the
 * output panel.
 *
 * Also defines kernel-thread helpers (kernel_sleep, kernel_yield)
 * that the shell and tui threads use to hand the CPU back to the
 * scheduler without going through the syscall path.
 */

#define OUTPUT_RING_SIZE 4096

extern ring_t output_ring;

/* Called once by kmain before any ring consumers run. */
void io_init(void);

/* Convenience writers that push into output_ring. */
void out_putc(char c);
void out_puts(const char *s);
void out_putdec(uint64_t v);
void out_puthex64(uint64_t v);

/*
 * kernel_yield — set current kernel thread to RUNNABLE and call sched().
 * When the scheduler picks us again, we return here normally.
 */
void kernel_yield(void);

/*
 * kernel_sleep(ticks) — set SLEEPING with wake_tick = ticks + arg, call
 * sched(). The timer handler's sleeper-wake pass wakes us when the
 * deadline is reached.
 */
void kernel_sleep(uint64_t ticks_to_sleep);

#endif /* IO_H */
