#ifndef SYSCALL_H
#define SYSCALL_H

#include "defs.h"
#include "trap.h"

/*
 * syscall.h — user→kernel syscall interface.
 *
 * Syscall numbers are stable across the project; user programs in user/
 * must use the same values. Keep this file in sync with user/ulib.h.
 *
 * ABI:
 *   a7       = syscall number (SYS_*)
 *   a0..a5   = up to 6 arguments
 *   a0       = return value (or -errno on failure)
 *
 * The ecall instruction in user mode traps to M-mode. trap.c routes
 * MCAUSE_ECALL_UMODE to syscall_dispatch() with the user trap frame.
 * syscall_dispatch reads a7, calls the handler, and writes the return
 * value into frame->regs[REG_A0] so the user sees it on mret.
 *
 * mepc advancement (to skip past the ecall instruction) happens in
 * trap.c, not here — the dispatcher stays ignorant of the trap vector
 * convention.
 */

#define SYS_EXIT    1
#define SYS_WRITE   2
#define SYS_GETPID  3
#define SYS_YIELD   4
#define SYS_SLEEP   5
#define SYS_FORK    6
#define SYS_EXEC    7
#define SYS_WAIT    8

/* Dispatch the syscall encoded in frame->regs[REG_A7] with args from
 * REG_A0..REG_A5. Writes the return value into REG_A0 on success. */
void syscall_dispatch(trap_frame_t *frame);

#endif /* SYSCALL_H */
