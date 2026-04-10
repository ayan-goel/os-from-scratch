/*
 * trapvec.S — machine-mode trap vector with mscratch-based kernel stack swap.
 *
 * mtvec is set to machine_trap_vector with mode=0 (direct): all machine-mode
 * traps jump here regardless of cause.
 *
 * ── mscratch protocol ───────────────────────────────────────────────────────
 *
 *   When running kernel code:   mscratch = 0                 (sentinel)
 *   When running user code:     mscratch = p->kstack         (kernel stack top
 *                                                              for this proc)
 *
 * On trap entry, we atomically swap sp with mscratch:
 *
 *   From kernel: mscratch was 0, so the swap puts 0 into sp. We detect
 *                this, swap back (restoring sp and mscratch=0), then save
 *                state on the current kernel stack.
 *
 *   From user:   mscratch held the kernel stack top, so the swap puts the
 *                kernel stack top into sp and the user sp into mscratch.
 *                We allocate the trap frame, move user sp from mscratch
 *                into the frame, then reset mscratch to 0 for nested-trap
 *                handling. On exit, we restore mscratch = kernel stack top
 *                before loading user sp and mret'ing.
 *
 * Why atomic csrrw: on trap entry every register holds live state that
 * must be preserved. csrrw gives us an atomic read+write to one CSR in
 * exchange for one register's worth of work — no separate scratch needed.
 *
 * ── Register save layout ────────────────────────────────────────────────────
 *
 * 32 slots × 8 bytes = 256 bytes below the kernel sp. Layout matches
 * trap_frame_t in kernel/trap.h; the _Static_assert there pins the size.
 * Slot 31 (offset 248) is padding that exists only so sp stays 16-byte
 * aligned at the `call trap_handler` below.
 *
 *   sp+  0 : x1  (ra)
 *   sp+  8 : x2  (sp)  ← original sp (kernel or user, depending on path)
 *   sp+ 16 : x3  (gp)
 *   sp+ 24 : x4  (tp)
 *   sp+ 32 : x5  (t0)
 *   ...
 *   sp+240 : x31 (t6)
 *   sp+248 : _pad (unused)
 */

    .section .text
    .align 4                    /* mtvec BASE must be 4-byte aligned */
    .global machine_trap_vector

machine_trap_vector:
    /* ── Atomic swap sp <-> mscratch ─────────────────────────────────────
     * After this one instruction:
     *   sp       = (old mscratch)  — either 0 (from kernel) or kstack top (from user)
     *   mscratch = (old sp)        — either kernel sp or user sp
     */
    csrrw   sp, mscratch, sp

    /* If sp is nonzero, the sentinel said we came from user mode. */
    bnez    sp, _from_user

    /* ── From kernel ──────────────────────────────────────────────────────
     * sp is 0 (was the sentinel). Swap back:
     *   sp       = original kernel sp
     *   mscratch = 0  (from the 0 that was in sp above)
     */
    csrrw   sp, mscratch, sp

    /* Allocate 256-byte trap frame on the kernel stack. */
    addi    sp, sp, -(32 * 8)

    /* Save x5 (t0) first — scratch to compute original sp. */
    sd      x5,  (4 * 8)(sp)

    /* Compute and save original kernel sp (= current sp + 256). */
    addi    x5, sp, (32 * 8)
    sd      x5,  (1 * 8)(sp)

    /* Save remaining GPRs. */
    sd      x1,  (0 * 8)(sp)   /* ra  */
    sd      x3,  (2 * 8)(sp)   /* gp  */
    sd      x4,  (3 * 8)(sp)   /* tp  */
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

    /* sp is now 16-byte aligned (256-byte frame + 16-aligned start). */
    mv      a0, sp
    call    trap_handler

    /* Restore GPRs (sp last). */
    ld      x1,  (0 * 8)(sp)
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
    ld      x2,  (1 * 8)(sp)   /* sp restored last */

    mret

_from_user:
    /* ── From user ─────────────────────────────────────────────────────────
     * sp       = p->kstack (kernel stack top; was in mscratch)
     * mscratch = user sp    (was in sp; came from the swap)
     *
     * Allocate the trap frame at the top of the kernel stack, save user
     * registers, pull user sp out of mscratch into frame slot 1, then
     * reset mscratch to 0 so that any nested kernel trap takes the
     * from-kernel path.
     */
    addi    sp, sp, -(32 * 8)

    /* Save x5 (t0) first — need it as scratch for mscratch read. */
    sd      x5,  (4 * 8)(sp)

    /* Pull user sp out of mscratch into slot 1 (x2 / original sp). */
    csrr    x5, mscratch
    sd      x5,  (1 * 8)(sp)

    /* Reset mscratch to the kernel sentinel. */
    csrw    mscratch, zero

    /* Save remaining GPRs. */
    sd      x1,  (0 * 8)(sp)
    sd      x3,  (2 * 8)(sp)
    sd      x4,  (3 * 8)(sp)
    sd      x6,  (5 * 8)(sp)
    sd      x7,  (6 * 8)(sp)
    sd      x8,  (7 * 8)(sp)
    sd      x9,  (8 * 8)(sp)
    sd      x10, (9 * 8)(sp)
    sd      x11, (10 * 8)(sp)
    sd      x12, (11 * 8)(sp)
    sd      x13, (12 * 8)(sp)
    sd      x14, (13 * 8)(sp)
    sd      x15, (14 * 8)(sp)
    sd      x16, (15 * 8)(sp)
    sd      x17, (16 * 8)(sp)
    sd      x18, (17 * 8)(sp)
    sd      x19, (18 * 8)(sp)
    sd      x20, (19 * 8)(sp)
    sd      x21, (20 * 8)(sp)
    sd      x22, (21 * 8)(sp)
    sd      x23, (22 * 8)(sp)
    sd      x24, (23 * 8)(sp)
    sd      x25, (24 * 8)(sp)
    sd      x26, (25 * 8)(sp)
    sd      x27, (26 * 8)(sp)
    sd      x28, (27 * 8)(sp)
    sd      x29, (28 * 8)(sp)
    sd      x30, (29 * 8)(sp)
    sd      x31, (30 * 8)(sp)

    /* Dispatch. */
    mv      a0, sp
    call    trap_handler

    /* ── Restore user state and mret ────────────────────────────────────────
     * Before touching sp, set mscratch = kstack_top = sp + 256 so the
     * next trap from user mode finds the kernel stack. Use x5 as scratch
     * (it will be restored from the frame below).
     */
    addi    x5, sp, (32 * 8)
    csrw    mscratch, x5

    /* Restore all GPRs except sp (x2). */
    ld      x1,  (0 * 8)(sp)
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

    /* Restore sp last; after this sp = user sp and the frame is gone. */
    ld      x2,  (1 * 8)(sp)

    mret

/* ── user_return ────────────────────────────────────────────────────────────
 *
 * Final leg of transitioning to user mode for the FIRST time a process runs
 * (after exec). Subsequent trap returns go through the _from_user mret path
 * above. This helper exists so that proc_return_to_user can write the CSRs
 * in C and hand off the GPR restore + mret to assembly.
 *
 * Arguments:
 *   a0 = pointer to the process's trap frame (already populated with user regs)
 *   a1 = satp value (MAKE_SATP(p->pagetable))
 *
 * Caller must have already written:
 *   mepc     = user entry point
 *   mstatus  = MPP=U, MPIE=1, MIE=0
 *   mscratch = p->kstack (kernel stack top for this proc)
 *
 * user_return never returns.
 */
    .global user_return

user_return:
    /* Install the process's pagetable. */
    csrw    satp, a1
    sfence.vma zero, zero

    /* Point sp at the trap frame so the restore sequence works. */
    mv      sp, a0

    /* Restore all GPRs from the frame (sp restored last; a0/a1 will be
     * clobbered by their own frame slots, which is fine — user expects
     * them at whatever proc_exec_static set up). */
    ld      x1,  (0 * 8)(sp)
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

    /* Restore x2 (sp) last; after this sp = user sp. */
    ld      x2,  (1 * 8)(sp)

    mret
