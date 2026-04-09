/*
 * entry.S — kernel entry point.
 *
 * QEMU virt drops us here in machine mode at 0x80000000 with:
 *   - a0 = hart ID
 *   - a1 = pointer to device tree blob (DTB)
 *   - interrupts disabled
 *   - no stack yet
 *
 * We set up a stack then jump to kmain in C.
 * Only hart 0 proceeds; all others spin indefinitely.
 */

    .section .text.entry
    .global _entry

_entry:
    /* Park all harts except hart 0. */
    csrr    t0, mhartid
    bnez    t0, _hang

    /* Set up the stack. Stack grows downward; _stack_top is defined below. */
    la      sp, _stack_top

    /* Clear BSS before entering C. */
    la      t0, _bss_start
    la      t1, _bss_end
_bss_clear:
    beq     t0, t1, _bss_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       _bss_clear
_bss_done:

    /* Jump to C. a0 (hart ID) and a1 (DTB pointer) are already in the right registers. */
    call    kmain

    /* kmain should never return. If it does, hang. */
_hang:
    wfi
    j       _hang

    /* 16 KB stack. */
    .section .bss
    .align 12                   /* 4096-byte aligned */
    .space 16384                /* 16 KB */
_stack_top:
