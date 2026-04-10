/*
 * syscall.c — user→kernel syscall dispatch table.
 *
 * Each sys_* handler reads its arguments out of the trap frame and writes
 * the return value back into REG_A0. Handlers that never return (sys_exit)
 * mark the process ZOMBIE and call sched().
 */

#include "syscall.h"
#include "proc.h"
#include "trap.h"
#include "dev/uart.h"
#include "defs.h"

extern volatile uint64_t ticks;   /* from trap.c */

/* ── sys_exit ─────────────────────────────────────────────────────────────── */

static void sys_exit(int64_t status) {
    uart_puts("sys_exit: pid=");
    uart_puthex64((uint64_t)current->pid);
    uart_puts(" status=");
    uart_puthex64((uint64_t)status);
    uart_puts("\n");

    current->exit_status = (int)status;
    current->state = ZOMBIE;

    /* Wake parent if it's blocked in wait(). */
    if (current->parent != NULL && current->parent->state == SLEEPING)
        current->parent->state = RUNNABLE;

    sched();
    panic("sys_exit: returned from sched after zombie");
}

/* ── sys_write ────────────────────────────────────────────────────────────── */

/*
 * sys_write(fd, buf, len)
 *
 * Writes len bytes from user buffer at virtual address `buf` to fd.
 * Only fd=1 (stdout / UART) is supported. Returns the number of bytes
 * written, or -1 on bad fd.
 *
 * Security note: we must translate the user VA to a kernel-accessible PA
 * before reading. In our identity-mapped kernel, the user pages are
 * identity-mapped too (the PA is the VA for kernel access), but we still
 * use vm_pa_of to verify the page is actually mapped in the process's
 * page table — this catches a malicious buf pointer that points into
 * kernel-only memory.
 *
 * For simplicity, we handle one byte at a time through vm_pa_of.
 * A real implementation would walk pages once and memcpy, but the
 * per-byte approach is correct and fast enough for UART-speed output.
 */
static int64_t sys_write(uint64_t fd, uint64_t buf_va, uint64_t len) {
    if (fd != 1)
        return -1;

    for (uint64_t i = 0; i < len; i++) {
        uint64_t pa = vm_pa_of(current->pagetable, buf_va + i);
        if (pa == 0)
            return -1;   /* unmapped user address */
        char c = *(char *)pa;
        uart_putc(c);
    }
    return (int64_t)len;
}

/* ── sys_getpid ───────────────────────────────────────────────────────────── */

static int64_t sys_getpid(void) {
    return (int64_t)current->pid;
}

/* ── sys_yield ────────────────────────────────────────────────────────────── */

/*
 * sys_yield — voluntarily give up the CPU.
 *
 * The process stays RUNNABLE and will be picked up by the scheduler on
 * the next scan. sched() switches to the scheduler context; when the
 * scheduler switches back, sched() returns, which returns to
 * syscall_dispatch, which returns to trap_handler, which returns to
 * trapvec.S's _from_user restore + mret.
 */
static void sys_yield(void) {
    current->state = RUNNABLE;
    sched();
}

/* ── sys_sleep ────────────────────────────────────────────────────────────── */

/*
 * sys_sleep(ticks_to_sleep) — block until `ticks_to_sleep` timer ticks pass.
 *
 * The timer handler in trap.c wakes sleeping processes whose wake_tick
 * has arrived by setting them RUNNABLE.
 */
static void sys_sleep(uint64_t ticks_to_sleep) {
    current->wake_tick = ticks + ticks_to_sleep;
    current->state = SLEEPING;
    sched();
}

/* ── sys_wait ─────────────────────────────────────────────────────────────── */

/*
 * sys_wait(status_uva) — wait for a child process to exit.
 *
 * If status_uva is non-zero, the child's exit status is written to the
 * user address. Returns the child's pid, or -1 if no children exist.
 * Blocks (SLEEPING) if children exist but none are ZOMBIE yet.
 */
static int64_t sys_wait(uint64_t status_uva) {
    for (;;) {
        int has_children = 0;
        for (int i = 0; i < NPROC; i++) {
            proc_t *p = &proc_table[i];
            if (p->parent != current)
                continue;
            has_children = 1;
            if (p->state == ZOMBIE) {
                int child_pid = p->pid;
                /* Copy exit status to user space if pointer is non-NULL. */
                if (status_uva != 0) {
                    uint64_t pa = vm_pa_of(current->pagetable, status_uva);
                    if (pa != 0)
                        *(int *)pa = p->exit_status;
                }
                proc_free(p);
                return (int64_t)child_pid;
            }
        }
        if (!has_children)
            return -1;

        /* Block until a child exits and wakes us. */
        current->state = SLEEPING;
        sched();
        /* Woken by sys_exit of a child — loop back to scan. */
    }
}

/* ── Dispatch ─────────────────────────────────────────────────────────────── */

void syscall_dispatch(trap_frame_t *frame) {
    uint64_t num = frame->regs[REG_A7];

    switch (num) {
    case SYS_EXIT:
        sys_exit((int64_t)frame->regs[REG_A0]);
        break;   /* unreachable */

    case SYS_WRITE:
        frame->regs[REG_A0] = (uint64_t)sys_write(
            frame->regs[REG_A0],
            frame->regs[REG_A1],
            frame->regs[REG_A2]);
        break;

    case SYS_GETPID:
        frame->regs[REG_A0] = (uint64_t)sys_getpid();
        break;

    case SYS_YIELD:
        sys_yield();
        frame->regs[REG_A0] = 0;
        break;

    case SYS_SLEEP:
        sys_sleep(frame->regs[REG_A0]);
        frame->regs[REG_A0] = 0;
        break;

    case SYS_FORK:
        frame->regs[REG_A0] = (uint64_t)proc_fork();
        break;

    case SYS_WAIT:
        frame->regs[REG_A0] = (uint64_t)sys_wait(frame->regs[REG_A0]);
        break;

    case SYS_EXEC: {
        /* Read the name string from user space into a kernel buffer. */
        char name[16];
        uint64_t uva = frame->regs[REG_A0];
        int i;
        for (i = 0; i < 15; i++) {
            uint64_t pa = vm_pa_of(current->pagetable, uva + (uint64_t)i);
            if (pa == 0) { frame->regs[REG_A0] = (uint64_t)-1; return; }
            name[i] = *(char *)pa;
            if (name[i] == '\0') break;
        }
        name[15] = '\0';
        frame->regs[REG_A0] = (uint64_t)proc_exec(name, frame);
        break;
    }

    default:
        uart_puts("syscall: unknown number ");
        uart_puthex64(num);
        uart_puts(" from pid ");
        uart_puthex64((uint64_t)current->pid);
        uart_puts("\n");
        frame->regs[REG_A0] = (uint64_t)-1;
        break;
    }
}
