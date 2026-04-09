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

/* ── T5 test processes ───────────────────────────────────────────────────── */

/* Two kernel-function processes that voluntarily yield to each other.
 * They print their name + a counter, yield, and repeat. Removed in T8 when
 * real user-mode processes replace them. */

static volatile int yield_count_a = 0;
static volatile int yield_count_b = 0;

static void proc_a(void) {
    for (int i = 0; i < 5; i++) {
        uart_puts("proc_a: tick ");
        uart_puthex64(i);
        uart_puts("\n");
        yield_count_a++;
        current->state = RUNNABLE;
        sched();
    }
    uart_puts("proc_a: done\n");
    current->state = ZOMBIE;
    sched();
}

static void proc_b(void) {
    for (int i = 0; i < 5; i++) {
        uart_puts("proc_b: tick ");
        uart_puthex64(i);
        uart_puts("\n");
        yield_count_b++;
        current->state = RUNNABLE;
        sched();
    }
    uart_puts("proc_b: done\n");
    current->state = ZOMBIE;
    sched();
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

    /* T5 verification: spawn 2 kernel-function processes, watch them interleave. */
    proc_spawn_fn(proc_a, "proc_a");
    proc_spawn_fn(proc_b, "proc_b");

    /* Enter the scheduler. Never returns. */
    scheduler();
}
