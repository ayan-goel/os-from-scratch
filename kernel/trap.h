#ifndef TRAP_H
#define TRAP_H

#include "defs.h"

/*
 * trap_frame_t — the saved register state pushed onto the kernel stack by
 * machine_trap_vector in trap.S.
 *
 * Layout (8-byte slots, matching the sd/ld sequence in trap.S):
 *   slot 0  = x1  (ra)
 *   slot 1  = x2  (sp)   — the value BEFORE the trap vector allocated its frame
 *   slot 2  = x3  (gp)
 *   ...
 *   slot 30 = x31 (t6)
 *
 * x0 is always zero and is not saved.
 */
typedef struct {
    uint64_t regs[31]; /* x1 ... x31 */
} trap_frame_t;

/* Index helpers (register xN maps to regs[N-1]). */
#define REG_RA   0   /* x1  */
#define REG_SP   1   /* x2  */
#define REG_GP   2   /* x3  */
#define REG_TP   3   /* x4  */
#define REG_T0   4   /* x5  */
#define REG_T1   5   /* x6  */
#define REG_T2   6   /* x7  */
#define REG_S0   7   /* x8  */
#define REG_S1   8   /* x9  */
#define REG_A0   9   /* x10 */
#define REG_A1   10  /* x11 */
#define REG_A2   11  /* x12 */
#define REG_A3   12  /* x13 */
#define REG_A4   13  /* x14 */
#define REG_A5   14  /* x15 */
#define REG_A6   15  /* x16 */
#define REG_A7   16  /* x17 */
#define REG_S2   17  /* x18 */
#define REG_S3   18  /* x19 */
#define REG_S4   19  /* x20 */
#define REG_S5   20  /* x21 */
#define REG_S6   21  /* x22 */
#define REG_S7   22  /* x23 */
#define REG_S8   23  /* x24 */
#define REG_S9   24  /* x25 */
#define REG_S10  25  /* x26 */
#define REG_S11  26  /* x27 */
#define REG_T3   27  /* x28 */
#define REG_T4   28  /* x29 */
#define REG_T5   29  /* x30 */
#define REG_T6   30  /* x31 */

/* Entry point called from trap.S. */
void trap_handler(trap_frame_t *frame);

/* Symbol exported from trap.S; written to mtvec in kmain. */
void machine_trap_vector(void);

#endif /* TRAP_H */
