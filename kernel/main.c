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

/* ── Embedded user programs ──────────────────────────────────────────────── */

/*
 * User programs are compiled from user/ sources, linked at 0x40000000, stripped
 * to raw binaries, and re-wrapped as ELF .o files by objcopy. The symbols
 * are mangled from the input filename (e.g. user/hello.bin → _binary_user_hello_bin_*).
 */
extern const unsigned char _binary_user_init_bin_start[];
extern const unsigned char _binary_user_init_bin_end[];
extern const unsigned char _binary_user_hello_bin_start[];
extern const unsigned char _binary_user_hello_bin_end[];
extern const unsigned char _binary_user_cpu_bound_bin_start[];
extern const unsigned char _binary_user_cpu_bound_bin_end[];
extern const unsigned char _binary_user_io_bound_bin_start[];
extern const unsigned char _binary_user_io_bound_bin_end[];

/* ── Binary lookup table ────────────────────────────────────────────────── */

static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static binary_entry_t binary_table[] = {
    { "init",      _binary_user_init_bin_start,      0 },
    { "hello",     _binary_user_hello_bin_start,     0 },
    { "cpu_bound", _binary_user_cpu_bound_bin_start, 0 },
    { "io_bound",  _binary_user_io_bound_bin_start,  0 },
};
#define NBINARIES (sizeof(binary_table) / sizeof(binary_table[0]))

const binary_entry_t *binary_lookup(const char *name) {
    for (uint64_t i = 0; i < NBINARIES; i++) {
        if (kstrcmp(binary_table[i].name, name) == 0)
            return &binary_table[i];
    }
    return NULL;
}

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

    /* 10. Fill in binary table sizes (can't use pointer arithmetic in
     * static initializers for extern symbols). */
    binary_table[0].size = (uint64_t)(_binary_user_init_bin_end      - _binary_user_init_bin_start);
    binary_table[1].size = (uint64_t)(_binary_user_hello_bin_end     - _binary_user_hello_bin_start);
    binary_table[2].size = (uint64_t)(_binary_user_cpu_bound_bin_end - _binary_user_cpu_bound_bin_start);
    binary_table[3].size = (uint64_t)(_binary_user_io_bound_bin_end  - _binary_user_io_bound_bin_start);

    /* 11. Spawn the init process. It fork/execs the workloads. */
    {
        const binary_entry_t *init = binary_lookup("init");
        KASSERT(init != NULL, "kmain: init binary not found");
        proc_exec_static(init->data, init->size, USER_TEXT_BASE, "init");
    }

    /* Enter the scheduler. Never returns. */
    scheduler();
}
