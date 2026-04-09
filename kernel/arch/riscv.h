#ifndef ARCH_RISCV_H
#define ARCH_RISCV_H

#include "defs.h"

/*
 * arch/riscv.h — RISC-V CSR access macros and privilege-mode bit constants.
 *
 * Included only by files that actually read/write CSRs or test privilege
 * bits (main.c for boot, trap.c for dispatch, vm.c for satp). Keeps
 * architecture coupling out of architecture-neutral headers.
 */

/* ── CSR access macros ──────────────────────────────────────────────────────
 * The CSR name is stringified into the instruction, so each call site is a
 * single instruction with no branching. The "rK" constraint allows either a
 * register or a 5-bit unsigned immediate (csrs/csrc accept uimm5).
 */
#define CSR_READ(csr, val) \
    __asm__ volatile("csrr %0, " #csr : "=r"(val) :: "memory")

#define CSR_WRITE(csr, val) \
    __asm__ volatile("csrw " #csr ", %0" :: "rK"(val) : "memory")

#define CSR_SET(csr, bits) \
    __asm__ volatile("csrs " #csr ", %0" :: "rK"(bits) : "memory")

#define CSR_CLEAR(csr, bits) \
    __asm__ volatile("csrc " #csr ", %0" :: "rK"(bits) : "memory")

/* ── mstatus / mie bit positions ────────────────────────────────────────────
 * Used when enabling/disabling interrupts and when dropping to user mode.
 */
#define MSTATUS_MIE   (1ULL << 3)   /* machine global interrupt enable */
#define MSTATUS_MPIE  (1ULL << 7)   /* machine previous interrupt enable */
#define MSTATUS_MPP_M (3ULL << 11)  /* MPP field = machine mode */
#define MSTATUS_MPP_U (0ULL << 11)  /* MPP field = user mode */

#define MIE_MSIE  (1ULL << 3)   /* machine software interrupt enable */
#define MIE_MTIE  (1ULL << 7)   /* machine timer interrupt enable */
#define MIE_MEIE  (1ULL << 11)  /* machine external interrupt enable */

/* ── mcause values ──────────────────────────────────────────────────────────
 * Bit 63 is set for interrupts. Lower bits are the cause code.
 */
#define MCAUSE_INTERRUPT       (1ULL << 63)

#define MCAUSE_ILLEGAL_INSTR   2ULL
#define MCAUSE_ECALL_UMODE     8ULL
#define MCAUSE_ECALL_MMODE     11ULL

#define MCAUSE_TIMER_INTERRUPT (MCAUSE_INTERRUPT | 7ULL)

#endif /* ARCH_RISCV_H */
