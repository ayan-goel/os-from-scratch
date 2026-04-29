/*
 * clint.c — CLINT timer driver for the QEMU virt machine.
 *
 * The CLINT fires a machine timer interrupt whenever mtime >= mtimecmp[0].
 * We rearm it in the interrupt handler by setting mtimecmp[0] = mtime + interval.
 */

#include "dev/clint.h"

/*
 * Scheduling quantum in CLINT ticks (10 MHz clock → 100 ns/tick).
 * 100000 = 10 ms. The shell's `quantum <ms>` command writes here.
 * Volatile so the rearm sees concurrent updates from other CPU threads
 * (today single-CPU, but cheap insurance).
 */
volatile uint64_t timer_interval = 100000ULL;

void clint_init(void) {
    /* Arm the first interrupt `timer_interval` ticks from now. */
    clint_set_timer();
}

void clint_set_timer(void) {
    /*
     * Writing mtimecmp[0] clears the pending timer interrupt flag and schedules
     * the next one. The write is to a 64-bit MMIO register; on RV32 this would
     * require two writes with a temporary max value between them, but on RV64
     * a single 64-bit store is atomic relative to the interrupt controller.
     *
     * Read `timer_interval` once into a local so a concurrent write to the
     * variable can't split the quantum across the read.
     */
    uint64_t q = timer_interval;
    CLINT_MTIMECMP(0) = CLINT_MTIME + q;
}
