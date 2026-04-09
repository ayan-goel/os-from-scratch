/*
 * switch.S — kernel context switch.
 *
 * void switch_context(context_t *old, context_t *new);
 *
 * Saves the calling context's callee-saved registers into *old, then loads
 * the callee-saved registers from *new and returns into the new context.
 *
 * "Returns into the new context" means: after loading, `ret` jumps to
 * new->ra, which is wherever that context last called switch_context (its
 * scheduler loop or sched() call), or proc_return_to_user for a brand-new
 * process.
 *
 * Callee-saved registers per RISC-V ABI: ra, sp, s0-s11.
 * Caller-saved registers (a0-a7, t0-t6) are the caller's responsibility and
 * are NOT saved here.
 *
 * context_t layout (must match proc.h exactly — offsets in bytes):
 *   0:  ra
 *   8:  sp
 *   16: s0
 *   24: s1
 *   32: s2
 *   40: s3
 *   48: s4
 *   56: s5
 *   64: s6
 *   72: s7
 *   80: s8
 *   88: s9
 *   96: s10
 *   104: s11
 */

    .section .text
    .global switch_context

switch_context:
    /* a0 = context_t *old, a1 = context_t *new */

    /* ── Save old context ─────────────────────────────────────────────── */
    sd    ra,    0(a0)
    sd    sp,    8(a0)
    sd    s0,   16(a0)
    sd    s1,   24(a0)
    sd    s2,   32(a0)
    sd    s3,   40(a0)
    sd    s4,   48(a0)
    sd    s5,   56(a0)
    sd    s6,   64(a0)
    sd    s7,   72(a0)
    sd    s8,   80(a0)
    sd    s9,   88(a0)
    sd    s10,  96(a0)
    sd    s11, 104(a0)

    /* ── Load new context ─────────────────────────────────────────────── */
    ld    ra,    0(a1)
    ld    sp,    8(a1)
    ld    s0,   16(a1)
    ld    s1,   24(a1)
    ld    s2,   32(a1)
    ld    s3,   40(a1)
    ld    s4,   48(a1)
    ld    s5,   56(a1)
    ld    s6,   64(a1)
    ld    s7,   72(a1)
    ld    s8,   80(a1)
    ld    s9,   88(a1)
    ld    s10,  96(a1)
    ld    s11, 104(a1)

    ret   /* jumps to new->ra */
