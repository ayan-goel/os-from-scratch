/*
 * trap.c — machine-mode trap handler.
 *
 * Called from machine_trap_vector in trapvec.S with a pointer to the saved
 * register frame on the kernel stack. Dispatches on mcause.
 */

#include "trap.h"
#include "syscall.h"
#include "shell.h"
#include "proc.h"
#include "trace.h"
#include "mm/vm.h"
#include "dev/clint.h"
#include "dev/uart.h"
#include "defs.h"
#include "arch/riscv.h"

/* Global tick counter, incremented on every timer interrupt. */
volatile uint64_t ticks = 0;

/*
 * Detect whether the trap came from user mode by checking mstatus.MPP.
 * MPP is bits [12:11]; value 0 = user, 3 = machine.
 */
static int from_user(void) {
    uint64_t mstatus;
    CSR_READ(mstatus, mstatus);
    return ((mstatus >> 11) & 3) == 0;
}

/*
 * restore_user_csrs — set up CSRs for mret back to user mode.
 *
 * Called after any path that may have called sched() (yield, sleep,
 * preemption, wait). During the switch-away period, other processes'
 * traps clobber mepc, mstatus, mscratch, and satp. We must restore
 * all four from the current process's state before trapvec's mret.
 */
static void restore_user_csrs(trap_frame_t *frame) {
    CSR_WRITE(mepc, frame->epc);
    uint64_t ms;
    CSR_READ(mstatus, ms);
    ms &= ~MSTATUS_MPP_M;
    ms |= MSTATUS_MPP_U;
    ms |= MSTATUS_MPIE;
    ms &= ~MSTATUS_MIE;
    CSR_WRITE(mstatus, ms);
    CSR_WRITE(mscratch, current->kstack);
    CSR_WRITE(satp, MAKE_SATP(current->pagetable));
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

void trap_handler(trap_frame_t *frame) {
    /*
     * mscratch protocol invariant (established in T6.1).
     */
    uint64_t ms;
    CSR_READ(mscratch, ms);
    KASSERT(ms == 0, "trap: mscratch non-zero in kernel trap handler");

    uint64_t mcause, mepc, mtval;
    CSR_READ(mcause, mcause);
    CSR_READ(mepc,   mepc);
    CSR_READ(mtval,  mtval);

    /*
     * If the trap came from user mode, save mepc into the frame so
     * that a yield/sleep that switches away and back restores the
     * correct user PC even if another process's trap clobbers the
     * hardware mepc register in between. The _from_user restore path
     * in trapvec.S doesn't manage mepc — we restore it from the frame
     * before returning from this handler.
     */
    int is_user = from_user();
    if (is_user)
        frame->epc = mepc;

    if (mcause == MCAUSE_TIMER_INTERRUPT) {
        clint_set_timer();
        ticks++;

        /* T2 (Phase 3): charge this tick to whoever was RUNNING. Counts
         * both user processes and kernel threads (shell, tui). */
        if (current != NULL && current->state == RUNNING)
            current->cpu_ticks++;

        /* T6.5: wake sleeping processes whose wake_tick has arrived. */
        for (int i = 0; i < NPROC; i++) {
            proc_t *p = &proc_table[i];
            if (p->state == SLEEPING && ticks >= p->wake_tick) {
                p->state = RUNNABLE;
                /* Skip kernel-thread wakes (tui) — we don't trace
                 * their RUN or SLEEP either; matching events only. */
                if (p->init_fn == 0)
                    trace_emit(EV_WAKE, p->pid);
            }
        }

        /* P2.1: drain the UART RX FIFO into the shell's input ring.
         * We're in M-mode with interrupts disabled and no one else is
         * racing us for the UART, so a simple loop is safe. */
        int c;
        while ((c = uart_getc()) >= 0)
            shell_rx_push((uint8_t)c);

#ifdef TRAP_DEBUG_TICK
        uart_puts("tick\n");
#endif
        /* Preempt the current user process if one is running. */
        if (is_user && current != NULL && current->state == RUNNING) {
            current->involuntary_preempts++;
            current->state = RUNNABLE;
            trace_emit(EV_PREEMPT, current->pid);
            sched();
            /* Returned from scheduler — this process was rescheduled. */
            restore_user_csrs(frame);
            return;
        }

        /* Non-preemption return: just restore user mepc if needed. */
        if (is_user)
            CSR_WRITE(mepc, frame->epc);
        return;
    }

    if (mcause == MCAUSE_ILLEGAL_INSTR) {
        uart_puts("TRAP: illegal instruction at mepc=");
        uart_puthex64(mepc);
        uart_puts(" mtval=");
        uart_puthex64(mtval);
        uart_puts("\n");
        if (is_user && current != NULL) {
            uart_puts("killed pid ");
            uart_puthex64((uint64_t)current->pid);
            uart_puts("\n");
            current->state = ZOMBIE;
            sched();
            /* sched() returns here when we're next scheduled — but
             * we're zombie, so the scheduler shouldn't pick us.
             * If it does, panic. */
            panic("zombie rescheduled after illegal instruction");
        }
        panic("illegal instruction in M-mode");
    }

    if (mcause == MCAUSE_ECALL_UMODE) {
        /*
         * Advance mepc past the ecall (4 bytes) BEFORE dispatch. This
         * way sys_exit (which doesn't return) and sys_yield (which may
         * sched() away and come back much later) both have the correct
         * resume PC.
         *
         * Save the advanced value into frame->epc so that if the
         * process yields and is rescheduled, we can restore the right
         * mepc before mret.
         */
        frame->epc = mepc + 4;
        CSR_WRITE(mepc, frame->epc);
        syscall_dispatch(frame);
        /*
         * On return from dispatch (for non-exit syscalls), the process
         * may have yielded (sched → switch_context → scheduler → ...
         * → switch_context back here). During that time, other processes'
         * traps clobbered the hardware mepc and mstatus. Restore both:
         *
         *   mepc    = frame->epc (the user PC after the ecall)
         *   mstatus = MPP=U, MPIE=1, MIE=0 (interrupts re-enabled on mret)
         */
        restore_user_csrs(frame);
        return;
    }

    if (mcause == MCAUSE_ECALL_MMODE) {
        uart_puts("TRAP: ecall from M-mode at mepc=");
        uart_puthex64(mepc);
        uart_puts("\n");
        CSR_WRITE(mepc, mepc + 4);
        return;
    }

    /* Unknown / unhandled trap. */
    uart_puts("TRAP: unhandled mcause=");
    uart_puthex64(mcause);
    uart_puts(" mepc=");
    uart_puthex64(mepc);
    uart_puts(" mtval=");
    uart_puthex64(mtval);
    uart_puts("\n");
    panic("unhandled trap");
}
