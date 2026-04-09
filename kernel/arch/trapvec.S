/*
 * trap.S — machine-mode trap vector.
 *
 * mtvec is set to machine_trap_vector with mode=0 (direct): all machine-mode
 * traps jump here regardless of cause.
 *
 * Register save layout (31 slots × 8 bytes = 248 bytes below original sp):
 *
 *   sp+  0 : x1  (ra)
 *   sp+  8 : x2  (sp)  ← original sp before this frame was allocated
 *   sp+ 16 : x3  (gp)
 *   sp+ 24 : x4  (tp)
 *   sp+ 32 : x5  (t0)
 *   sp+ 40 : x6  (t1)
 *   sp+ 48 : x7  (t2)
 *   sp+ 56 : x8  (s0)
 *   sp+ 64 : x9  (s1)
 *   sp+ 72 : x10 (a0)
 *   sp+ 80 : x11 (a1)
 *   sp+ 88 : x12 (a2)
 *   sp+ 96 : x13 (a3)
 *   sp+104 : x14 (a4)
 *   sp+112 : x15 (a5)
 *   sp+120 : x16 (a6)
 *   sp+128 : x17 (a7)
 *   sp+136 : x18 (s2)
 *   sp+144 : x19 (s3)
 *   sp+152 : x20 (s4)
 *   sp+160 : x21 (s5)
 *   sp+168 : x22 (s6)
 *   sp+176 : x23 (s7)
 *   sp+184 : x24 (s8)
 *   sp+192 : x25 (s9)
 *   sp+200 : x26 (s10)
 *   sp+208 : x27 (s11)
 *   sp+216 : x28 (t3)
 *   sp+224 : x29 (t4)
 *   sp+232 : x30 (t5)
 *   sp+240 : x31 (t6)
 *
 * Phase 1 note: we are always in machine mode (M-mode) in Phases 1–5. sp is
 * always the kernel stack and is valid on trap entry. The mscratch-based
 * kernel/user stack swap needed for user mode (Phase T6) is added later.
 */

    .section .text
    .align 4                    /* mtvec BASE must be 4-byte aligned (mode bits in [1:0]) */
    .global machine_trap_vector

machine_trap_vector:
    /* ── Allocate frame (248 bytes) ─────────────────────────────────────── */
    addi    sp, sp, -(31 * 8)

    /* ── Save x5 (t0) first — we need it as scratch to compute original sp ── */
    sd      x5,  (4 * 8)(sp)

    /* ── Compute and save original sp (= current sp + 248) ─────────────── */
    addi    x5, sp, (31 * 8)
    sd      x5,  (1 * 8)(sp)

    /* ── Save all other registers ───────────────────────────────────────── */
    sd      x1,  (0 * 8)(sp)   /* ra  */
    /* x2 (sp) already saved above */
    sd      x3,  (2 * 8)(sp)   /* gp  */
    sd      x4,  (3 * 8)(sp)   /* tp  */
    /* x5 (t0) already saved above */
    sd      x6,  (5 * 8)(sp)   /* t1  */
    sd      x7,  (6 * 8)(sp)   /* t2  */
    sd      x8,  (7 * 8)(sp)   /* s0  */
    sd      x9,  (8 * 8)(sp)   /* s1  */
    sd      x10, (9 * 8)(sp)   /* a0  */
    sd      x11, (10 * 8)(sp)  /* a1  */
    sd      x12, (11 * 8)(sp)  /* a2  */
    sd      x13, (12 * 8)(sp)  /* a3  */
    sd      x14, (13 * 8)(sp)  /* a4  */
    sd      x15, (14 * 8)(sp)  /* a5  */
    sd      x16, (15 * 8)(sp)  /* a6  */
    sd      x17, (16 * 8)(sp)  /* a7  */
    sd      x18, (17 * 8)(sp)  /* s2  */
    sd      x19, (18 * 8)(sp)  /* s3  */
    sd      x20, (19 * 8)(sp)  /* s4  */
    sd      x21, (20 * 8)(sp)  /* s5  */
    sd      x22, (21 * 8)(sp)  /* s6  */
    sd      x23, (22 * 8)(sp)  /* s7  */
    sd      x24, (23 * 8)(sp)  /* s8  */
    sd      x25, (24 * 8)(sp)  /* s9  */
    sd      x26, (25 * 8)(sp)  /* s10 */
    sd      x27, (26 * 8)(sp)  /* s11 */
    sd      x28, (27 * 8)(sp)  /* t3  */
    sd      x29, (28 * 8)(sp)  /* t4  */
    sd      x30, (29 * 8)(sp)  /* t5  */
    sd      x31, (30 * 8)(sp)  /* t6  */

    /* ── Call C trap handler with pointer to frame ──────────────────────── */
    mv      a0, sp
    call    trap_handler

    /* ── Restore all registers (sp restored last) ───────────────────────── */
    ld      x1,  (0 * 8)(sp)
    /* skip x2 — restored below after all others */
    ld      x3,  (2 * 8)(sp)
    ld      x4,  (3 * 8)(sp)
    ld      x5,  (4 * 8)(sp)
    ld      x6,  (5 * 8)(sp)
    ld      x7,  (6 * 8)(sp)
    ld      x8,  (7 * 8)(sp)
    ld      x9,  (8 * 8)(sp)
    ld      x10, (9 * 8)(sp)
    ld      x11, (10 * 8)(sp)
    ld      x12, (11 * 8)(sp)
    ld      x13, (12 * 8)(sp)
    ld      x14, (13 * 8)(sp)
    ld      x15, (14 * 8)(sp)
    ld      x16, (15 * 8)(sp)
    ld      x17, (16 * 8)(sp)
    ld      x18, (17 * 8)(sp)
    ld      x19, (18 * 8)(sp)
    ld      x20, (19 * 8)(sp)
    ld      x21, (20 * 8)(sp)
    ld      x22, (21 * 8)(sp)
    ld      x23, (22 * 8)(sp)
    ld      x24, (23 * 8)(sp)
    ld      x25, (24 * 8)(sp)
    ld      x26, (25 * 8)(sp)
    ld      x27, (26 * 8)(sp)
    ld      x28, (27 * 8)(sp)
    ld      x29, (28 * 8)(sp)
    ld      x30, (29 * 8)(sp)
    ld      x31, (30 * 8)(sp)
    ld      x2,  (1 * 8)(sp)   /* sp — restore last; frame is now gone */

    mret
