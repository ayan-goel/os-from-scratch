/*
 * clint.c — CLINT timer driver for the QEMU virt machine.
 *
 * The CLINT fires a machine timer interrupt whenever mtime >= mtimecmp[0].
 * We rearm it in the interrupt handler by setting mtimecmp[0] = mtime + interval.
 */

#include "dev/clint.h"

void clint_init(void) {
    /* Arm the first interrupt TIMER_INTERVAL ticks from now. */
    clint_set_timer();
}

void clint_set_timer(void) {
    /*
     * Writing mtimecmp[0] clears the pending timer interrupt flag and schedules
     * the next one. The write is to a 64-bit MMIO register; on RV32 this would
     * require two writes with a temporary max value between them, but on RV64
     * a single 64-bit store is atomic relative to the interrupt controller.
     */
    CLINT_MTIMECMP(0) = CLINT_MTIME + TIMER_INTERVAL;
}
