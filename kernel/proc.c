/*
 * proc.c — process management and round-robin scheduler.
 *
 * The scheduler runs in its own context (the initial kmain stack becomes the
 * scheduler stack after the first switch_context call). It loops over the
 * process table looking for RUNNABLE processes and switches to each one.
 *
 * Each process runs on its own kernel stack. When it calls sched() (either
 * voluntarily via yield/sleep, or from the timer interrupt handler), it
 * switches back to the scheduler context. The scheduler then picks the next
 * RUNNABLE process.
 *
 * In Phase 1 (T5), all "processes" are kernel functions — there is no user
 * mode yet. proc_run_fn() is used to wrap a kernel function as a process.
 * User mode exec comes in T6.
 */

#include "proc.h"
#include "mm/pmem.h"
#include "mm/vm.h"
#include "dev/uart.h"
#include "defs.h"

proc_t  proc_table[NPROC];
proc_t *current = NULL;

/* The scheduler's own saved context. */
static context_t scheduler_context;

/* Next PID to assign (monotonically increasing). */
static int next_pid = 1;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void memzero_struct(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++)
        b[i] = 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void proc_init(void) {
    for (int i = 0; i < NPROC; i++) {
        memzero_struct(&proc_table[i], sizeof(proc_t));
        proc_table[i].state = UNUSED;
    }
    uart_puts("proc: initialized (");
    uart_puthex64(NPROC);
    uart_puts(" slots)\n");
}

proc_t *proc_alloc(void) {
    for (int i = 0; i < NPROC; i++) {
        proc_t *p = &proc_table[i];
        if (p->state != UNUSED)
            continue;

        memzero_struct(p, sizeof(proc_t));
        p->state = EMBRYO;
        p->pid   = next_pid++;

        /* Allocate a kernel stack for this process. */
        void *kstack_page = pmem_alloc();
        if (kstack_page == NULL)
            panic("proc_alloc: out of memory for kernel stack");
        p->kstack = (uint64_t)kstack_page + KSTACK_SIZE; /* top of stack */

        /* Inherit the kernel page table. Process-specific user mappings
         * are added by proc_exec (T6). For now, use the kernel pagetable
         * directly (no per-process pagetable yet in T5). */
        p->pagetable = kernel_pagetable;

        return p;
    }
    return NULL; /* no free slots */
}

void proc_free(proc_t *p) {
    /* Free the kernel stack page. kstack points to the top; subtract KSTACK_SIZE. */
    pmem_free((void *)(p->kstack - KSTACK_SIZE));

    /* In T6+ we'll free the process's own page table here.
     * For T5, pagetable == kernel_pagetable so we must not free it. */

    memzero_struct(p, sizeof(proc_t));
    p->state = UNUSED;
}

/*
 * sched — switch from the current process back to the scheduler.
 *
 * The caller must set p->state to RUNNABLE, SLEEPING, or ZOMBIE before
 * calling sched(). This function does not return until the scheduler
 * reschedules this process (at which point it returns normally).
 */
void sched(void) {
    proc_t *p = current;
    current = NULL;
    switch_context(&p->context, &scheduler_context);
    /* Execution resumes here next time this process is scheduled. */
}

/*
 * proc_entry_trampoline — called when a new kernel-mode process first runs.
 *
 * switch_context jumps here (via the new process's context.ra). The process
 * function pointer is stored in s0 (callee-saved, so switch_context preserved
 * it). We call the function, then call exit if it returns.
 *
 * This is the T5 mechanism for kernel-function processes. T6 replaces this
 * with proc_return_to_user for real user-mode processes.
 */
static void proc_entry_trampoline(void) {
    /*
     * The function to call is stored in current->init_fn, set by proc_spawn_fn.
     * We read it from the struct rather than a register because the compiler
     * uses s0 as its frame pointer in the function prologue, clobbering any
     * value we might have loaded into s0 via the context setup.
     */
    void (*fn)(void) = current->init_fn;
    fn();

    /* If the function returns, mark the process as zombie and reschedule. */
    current->state = ZOMBIE;
    sched();
    panic("proc_entry_trampoline: returned from sched after zombie");
}

/*
 * proc_spawn_fn — create a kernel-mode process that runs fn().
 *
 * Used in T5 to test context switching without user mode. T6 replaces this
 * with proc_exec which loads a real ELF binary.
 */
proc_t *proc_spawn_fn(void (*fn)(void), const char *name) {
    proc_t *p = proc_alloc();
    if (p == NULL)
        panic("proc_spawn_fn: no free process slots");

    /* Copy the name (up to 15 chars). */
    int i = 0;
    while (name[i] && i < 15) { p->name[i] = name[i]; i++; }
    p->name[i] = '\0';

    /*
     * Set up the initial kernel context so that when switch_context first
     * jumps into this process, it lands in proc_entry_trampoline with:
     *   s0 = fn  (the function to call)
     *   sp = top of kernel stack
     */
    p->init_fn     = fn;
    p->context.ra  = (uint64_t)proc_entry_trampoline;
    p->context.sp  = p->kstack;   /* top of kernel stack */

    p->state = RUNNABLE;
    return p;
}

/*
 * scheduler — the main scheduling loop. Runs in the kmain stack context.
 * Never returns.
 *
 * This is a simple round-robin: scan the table in order, run each RUNNABLE
 * process for one scheduling quantum. The timer interrupt calls sched() to
 * preempt the running process and return here.
 *
 * In T5, there is no quantum: the process runs until it calls sched() itself
 * (voluntary yield). Timer-driven preemption is wired in T8.
 */
void scheduler(void) {
    uart_puts("scheduler: starting\n");

    for (;;) {
        int found = 0;
        for (int i = 0; i < NPROC; i++) {
            proc_t *p = &proc_table[i];
            if (p->state != RUNNABLE)
                continue;

            found = 1;
            p->state = RUNNING;
            current  = p;
            switch_context(&scheduler_context, &p->context);
            /*
             * The process called sched() and we're back here.
             * current is already NULL (set by sched before switching).
             */
        }

        if (!found) {
            /* Nothing to run — idle. */
            __asm__ volatile("wfi");
        }
    }
}
