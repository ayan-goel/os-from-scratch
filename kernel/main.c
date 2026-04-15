/*
 * main.c — kernel C entry point and global services (panic).
 *
 * kmain() is called from entry.S after the stack is set up and BSS is cleared.
 * Each phase adds initialization calls here in dependency order.
 */

#include "defs.h"
#include "arch/riscv.h"
#include "dev/uart.h"
#include "dev/clint.h"
#include "trap.h"
#include "mm/pmem.h"
#include "mm/vm.h"
#include "mm/kalloc.h"
#include "proc.h"
#include "fs.h"
#include "io.h"
#include "shell.h"
#include "tui.h"

/* ── panic ──────────────────────────────────────────────────────────────────
 * Print a message and halt the CPU permanently.
 * Declared in defs.h so any file can call it without a circular include.
 */
void panic(const char *msg) {
    /* Disable machine interrupts so we don't re-enter from a timer tick. */
    CSR_CLEAR(mstatus, MSTATUS_MIE);

    uart_puts("\nPANIC: ");
    uart_puts(msg);
    uart_puts("\n");

    for (;;)
        __asm__ volatile("wfi");
}

/*
 * Embedded user programs and the ramfs table live in kernel/fs.c. kmain
 * just calls fs_init before spawning init.
 */

/* ── kmain ───────────────────────────────────────────────────────────────── */
void kmain(unsigned int hart_id, unsigned long dtb_ptr) {
    (void)hart_id;
    (void)dtb_ptr;

    /* 1. Bring up the UART so we can print from here on. */
    uart_init();
    uart_puts("hello from the kernel\n");

    /* 2. Arm the CLINT timer so it starts counting toward the first interrupt. */
    clint_init();

    /* 3. Point mtvec at our trap vector (direct mode, bit[1:0] = 0). */
    CSR_WRITE(mtvec, (uint64_t)machine_trap_vector);

    /*
     * 3a. Establish the mscratch protocol sentinel.
     *
     * trapvec.S atomically swaps sp with mscratch on every trap entry.
     * When mscratch is 0, trapvec interprets the swap result as "trap from
     * kernel mode" and swaps back before saving registers. We hardcode 0
     * here even though CSRs come up zero on reset — making the invariant
     * explicit is worth one instruction. T6.3+ will switch mscratch to
     * the kernel stack top whenever we drop into user mode, and back to 0
     * whenever we return to pure kernel context.
     */
    CSR_WRITE(mscratch, 0);

    /*
     * 3b. Configure PMP0 to allow U-mode access to all physical memory.
     *
     * RISC-V spec: if any PMP entry is implemented and an S/U-mode access
     * doesn't match any entry, the access is denied. QEMU implements PMP.
     * Without this setup, the first instruction fetch in U-mode (i.e., the
     * very first user instruction after mret) takes a PMP instruction
     * access fault (mcause=1).
     *
     * We're not using PMP as a real security boundary — Sv39 page tables
     * handle that. This entry is "PMP0 covers all of memory, all perms"
     * via NAPOT encoding with pmpaddr0 = all-ones (represents a 2^(XLEN+2)
     * byte region starting at 0).
     *
     *   pmpcfg0 low byte = pmp0cfg = A(NAPOT=11)<<3 | X(1)<<2 | W(1)<<1 | R(1)
     *                              = 0x18 | 0x04 | 0x02 | 0x01 = 0x1F
     */
    CSR_WRITE(pmpaddr0, ~0ULL);
    CSR_WRITE(pmpcfg0,  0x1FULL);

    /* 4. Enable machine timer interrupts in the mie register. */
    CSR_SET(mie, MIE_MTIE);

    /* 5. Enable global machine interrupts in mstatus. */
    CSR_SET(mstatus, MSTATUS_MIE);

    uart_puts("trap infrastructure ready\n");

    /* 6. Physical memory allocator. */
    pmem_init();

    /* 7. Virtual memory — build kernel page table and enable Sv39 paging. */
    vm_init();

    /* 8. Kernel heap. */
    kalloc_init();

    /* 9. Process table. */
    proc_init();

    /* 10. Initialize the ramfs and the global output ring. */
    fs_init();
    io_init();

    /* 11. Spawn the init process. It fork/execs the workloads.
     *     Init's output lands in output_ring (via sys_write), where
     *     the TUI (spawned next) renders it into the shell panel. */
    {
        const inode_t *init = fs_lookup("init");
        KASSERT(init != NULL, "kmain: init binary not found");
        proc_exec_static(init->data, init->size, USER_TEXT_BASE, "init");
    }

    /* 12. Spawn the kernel-mode shell thread. */
    proc_spawn_fn(shell_thread, "shell");

    /* 13. Spawn the TUI renderer. Takes over the terminal once it
     *     draws its first frame. */
    proc_spawn_fn(tui_thread, "tui");

    /* Enter the scheduler. Never returns. */
    scheduler();
}
