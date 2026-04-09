/*
 * trap.c — machine-mode trap handler.
 *
 * Called from machine_trap_vector in trap.S with a pointer to the saved
 * register frame on the kernel stack. Dispatches on mcause.
 */

#include "trap.h"
#include "dev/clint.h"
#include "dev/uart.h"
#include "defs.h"
#include "arch/riscv.h"

/* Global tick counter, incremented on every timer interrupt. */
volatile uint64_t ticks = 0;

/*
 * trap_handler — dispatch on mcause.
 *
 * The frame pointer is currently unused for T1 (we read CSRs directly).
 * It becomes essential in T6 when we need to read/write user registers
 * (syscall arguments, return values).
 */
void trap_handler(trap_frame_t *frame) {
    (void)frame;  /* unused until T6 */

    uint64_t mcause, mepc, mtval;
    CSR_READ(mcause, mcause);
    CSR_READ(mepc,   mepc);
    CSR_READ(mtval,  mtval);

    if (mcause == MCAUSE_TIMER_INTERRUPT) {
        /* Rearm the timer for the next interval. */
        clint_set_timer();
        ticks++;

        /*
         * Per-tick printing was useful while bringing up the trap vector
         * in T1; it floods the terminal once the scheduler is idle, so
         * it's off by default. Define TRAP_DEBUG_TICK in CFLAGS to bring
         * it back during low-level timer debugging.
         *
         * In T8 this branch becomes the preemption hook: on every Nth
         * tick, set current->state = RUNNABLE and call sched().
         */
#ifdef TRAP_DEBUG_TICK
        uart_puts("tick\n");
#endif
        return;
    }

    if (mcause == MCAUSE_ILLEGAL_INSTR) {
        uart_puts("TRAP: illegal instruction at mepc=");
        uart_puthex64(mepc);
        uart_puts(" mtval=");
        uart_puthex64(mtval);
        uart_puts("\n");
        /* Kill the process (T6+). For now, hang so we can inspect the state. */
        panic("illegal instruction");
    }

    if (mcause == MCAUSE_ECALL_UMODE) {
        /*
         * Syscall from user mode. Full dispatch is implemented in T6.
         * For now, just advance mepc past the ecall instruction (4 bytes)
         * and return, so the kernel doesn't loop on the same ecall.
         */
        CSR_WRITE(mepc, mepc + 4);
        return;
    }

    if (mcause == MCAUSE_ECALL_MMODE) {
        /*
         * ecall from machine mode — should not happen in normal operation.
         * Advance past it and return rather than panicking, so it's easy
         * to trigger deliberately in tests.
         */
        uart_puts("TRAP: ecall from M-mode at mepc=");
        uart_puthex64(mepc);
        uart_puts("\n");
        CSR_WRITE(mepc, mepc + 4);
        return;
    }

    /* Unknown / unhandled trap: print cause and halt. */
    uart_puts("TRAP: unhandled mcause=");
    uart_puthex64(mcause);
    uart_puts(" mepc=");
    uart_puthex64(mepc);
    uart_puts(" mtval=");
    uart_puthex64(mtval);
    uart_puts("\n");
    panic("unhandled trap");
}
