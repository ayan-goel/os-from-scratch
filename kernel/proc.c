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
#include "fs.h"
#include "trace.h"
#include "mm/pmem.h"
#include "mm/vm.h"
#include "dev/uart.h"
#include "defs.h"
#include "arch/riscv.h"

extern volatile uint64_t ticks;   /* defined in trap.c */

proc_t  proc_table[NPROC];
proc_t *current = NULL;

/*
 * T5 (Phase 3): global count of scheduling decisions.
 *
 * Incremented on every sched() call, including preempt, yield, sleep, and
 * exit. Used by the `stats` command and the live TUI scheduler panel.
 * Per-process burst_count disappears when slots are freed; this global
 * persists for the whole run.
 */
uint64_t sched_total_decisions = 0;

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
        p->state      = EMBRYO;
        p->pid        = next_pid++;
        p->start_tick = (uint32_t)ticks;

        /* Allocate a kernel stack for this process. */
        void *kstack_page = pmem_alloc();
        if (kstack_page == NULL)
            panic("proc_alloc: out of memory for kernel stack");
        p->kstack = (uint64_t)kstack_page + KSTACK_SIZE; /* top of stack */

        /*
         * Per-process pagetable (T6.2+).
         *
         * Each process gets its own root page that mirrors the kernel's
         * mappings. T6.3+ will install user-VA mappings into this root.
         * In T6.2 the clone is still functionally identical to the kernel
         * pagetable, but we pre-allocate it now so the T6.3 transition is
         * a one-line change in proc_exec_static.
         */
        p->pagetable = vm_clone_kernel();
        KASSERT(p->pagetable != kernel_pagetable,
                "proc_alloc: clone returned the kernel root itself");
        KASSERT(vm_pa_of(p->pagetable, PHYS_BASE) ==
                vm_pa_of(kernel_pagetable, PHYS_BASE),
                "proc_alloc: clone doesn't see kernel RAM");

        return p;
    }
    return NULL; /* no free slots */
}

proc_t *proc_find_by_pid(int pid) {
    for (int i = 0; i < NPROC; i++) {
        proc_t *p = &proc_table[i];
        if (p->state != UNUSED && p->pid == pid)
            return p;
    }
    return NULL;
}

void proc_free(proc_t *p) {
    /* Free the kernel stack page. kstack points to the top; subtract KSTACK_SIZE. */
    pmem_free((void *)(p->kstack - KSTACK_SIZE));

    /* Release the cloned pagetable. vm_free_clone enforces the T6.2
     * invariant that no root entry has diverged from the kernel root
     * yet — that assertion will start firing the moment T6.3 starts
     * adding user mappings, at which point we'll wire the recursive
     * teardown path. */
    vm_free_clone(p->pagetable);

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

    /* T3: close the running burst before leaving the CPU. */
    uint32_t burst = (uint32_t)ticks - p->burst_start_tick;
    p->last_burst = burst;
    p->burst_sum += burst;
    p->burst_count++;

    /* T5: count the scheduling decision globally so stats can report
     * overall throughput even after processes exit and free their slots. */
    sched_total_decisions++;

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

/* ── fork (T7.2) ──────────────────────────────────────────────────────────── */

/*
 * proc_fork — duplicate the current process.
 *
 * Allocates a new slot, deep-copies the user address space, copies the
 * trap frame (so the child resumes at the instruction after the ecall),
 * and marks the child RUNNABLE.
 *
 * Returns: child pid to parent, 0 to child (via trap frame), -1 on error.
 */
int proc_fork(void) {
    proc_t *child = proc_alloc();
    if (child == NULL)
        return -1;

    /* Copy the user address space page-by-page. */
    if (vm_copy_user_pages(child->pagetable, current->pagetable) < 0) {
        proc_free(child);
        return -1;
    }

    /* Carve the child's trap frame from the top of its kernel stack. */
    child->tf = (trap_frame_t *)(child->kstack - sizeof(trap_frame_t));

    /* Copy the parent's trap frame into the child's. */
    {
        uint64_t *dst = (uint64_t *)child->tf;
        uint64_t *src = (uint64_t *)current->tf;
        for (uint64_t i = 0; i < sizeof(trap_frame_t) / 8; i++)
            dst[i] = src[i];
    }

    /* fork returns 0 to the child. */
    child->tf->regs[REG_A0] = 0;

    /* Child resumes at the instruction after the ecall (already advanced
     * by trap.c before syscall_dispatch). Using tf->epc, NOT user_pc
     * (which is the original entry point from exec). */
    child->user_pc = current->tf->epc;

    /* Metadata. */
    child->parent = current;
    for (int i = 0; i < 16; i++)
        child->name[i] = current->name[i];

    /* First schedule of the child enters proc_return_to_user. */
    child->context.ra = (uint64_t)proc_return_to_user;
    child->context.sp = (uint64_t)child->tf;

    child->state = RUNNABLE;
    trace_emit(EV_SPAWN, child->pid);
    return child->pid;
}

/* ── exec (T7.3) ──────────────────────────────────────────────────────────── */

/*
 * proc_exec — replace the current process's image with a new binary.
 *
 * Tears down old user mappings, loads the named binary, resets the trap
 * frame for a fresh start, and updates frame->epc so the ecall return
 * path in trap.c jumps to the new entry point instead of the old code.
 *
 * Returns -1 if the binary is not found. On success, does not return to
 * the original code — but does return to syscall_dispatch, which returns
 * to trap_handler, which mrets to the new entry point.
 */
int proc_exec(const char *name, trap_frame_t *frame) {
    const inode_t *bin = fs_lookup(name);
    if (bin == NULL || bin->type != FT_BINARY)
        return -1;

    /* Tear down old user mappings (text + stack pages). */
    vm_teardown_user(current->pagetable);

    /* Load new binary — same logic as proc_exec_static's inner body. */
    uint64_t text_pages = (bin->size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t pg = 0; pg < text_pages; pg++) {
        void *phys = pmem_alloc();
        if (phys == NULL)
            panic("proc_exec: oom mapping text");

        uint64_t off = pg * PAGE_SIZE;
        uint64_t to_copy = bin->size - off;
        if (to_copy > PAGE_SIZE) to_copy = PAGE_SIZE;

        const uint8_t *src = bin->data + off;
        uint8_t       *dst = (uint8_t *)phys;
        for (uint64_t j = 0; j < to_copy; j++)       dst[j] = src[j];
        for (uint64_t j = to_copy; j < PAGE_SIZE; j++) dst[j] = 0;

        if (vm_map(current->pagetable, USER_TEXT_BASE + off, (uint64_t)phys,
                   PAGE_SIZE, PTE_USER_RX) < 0)
            panic("proc_exec: vm_map text failed");
    }

    /* Allocate and map a fresh user stack. */
    void *stack_phys = pmem_alloc();
    if (stack_phys == NULL)
        panic("proc_exec: oom user stack");
    {
        uint8_t *sp = (uint8_t *)stack_phys;
        for (uint64_t j = 0; j < PAGE_SIZE; j++) sp[j] = 0;
    }

    uint64_t user_stack_va = USER_TEXT_BASE + USER_STACK_OFFSET;
    if (vm_map(current->pagetable, user_stack_va, (uint64_t)stack_phys,
               PAGE_SIZE, PTE_USER_RW) < 0)
        panic("proc_exec: vm_map stack failed");

    /* Zero the trap frame and set up the new user state.
     * frame and current->tf alias the same memory. */
    {
        uint64_t *words = (uint64_t *)current->tf;
        for (uint64_t j = 0; j < sizeof(trap_frame_t) / 8; j++)
            words[j] = 0;
    }
    current->tf->regs[REG_SP] = user_stack_va + PAGE_SIZE;
    current->user_pc = USER_TEXT_BASE;

    /* Update frame->epc so the ecall return path in trap.c writes the
     * new entry point to mepc. The new binary starts at _start. */
    frame->epc = USER_TEXT_BASE;

    /* Copy the new name. */
    {
        int i = 0;
        while (name[i] && i < 15) { current->name[i] = name[i]; i++; }
        current->name[i] = '\0';
    }

    return 0;  /* trap.c writes this into frame->regs[REG_A0], but the
                * new binary won't see it — it starts fresh at _start. */
}

/* ── User-mode first entry (T6.4+) ─────────────────────────────────────── */

/*
 * proc_return_to_user — first-time entry into user mode.
 *
 * Called via context.ra on the first schedule of a fresh exec'd proc.
 * By the time we're here:
 *   - current points to the proc
 *   - sp points somewhere on the proc's kernel stack (context.sp was set
 *     by proc_exec_static to p->tf; the prologue pushed below that)
 *   - mscratch is 0 (sentinel — we're still in kernel context)
 *
 * Sets up the CSRs the hardware needs to mret into user mode, then
 * tail-calls user_return (asm) to do the GPR restore and mret. user_return
 * does not return, so this function is marked noreturn.
 *
 * Subsequent trap→handler→mret cycles go through trapvec.S's _from_user
 * path directly; this trampoline runs exactly once per proc.
 */
void proc_return_to_user(void) {
    proc_t *p = current;
    KASSERT(p != NULL, "proc_return_to_user: current is NULL");
    KASSERT(p->tf != NULL, "proc_return_to_user: no trap frame");

    /*
     * CRITICAL SECTION: disable interrupts before modifying mepc/mstatus.
     *
     * The scheduler runs with MIE=1, so a timer interrupt can fire at any
     * point in this function. If an interrupt fires AFTER we write mepc
     * (the user entry point) but BEFORE the mret in user_return, the
     * hardware's trap-entry sequence overwrites mepc with the interrupted
     * PC (a kernel address). The timer handler's mret then returns to
     * proc_return_to_user with the original MPP=U intact (it gets saved
     * by the hardware and restored by mret). When user_return's mret
     * finally executes, mepc still holds the stale kernel PC, so the CPU
     * jumps to a kernel address in U-mode → instruction access fault.
     *
     * Fix: clear MIE now. MPIE=1 ensures mret re-enables interrupts
     * atomically at the privilege transition. Between here and the mret,
     * no interrupts can fire, so mepc and mscratch stay exactly as we
     * set them.
     */
    CSR_CLEAR(mstatus, MSTATUS_MIE);

    /* Where mret will jump. Now safe from clobbering. */
    CSR_WRITE(mepc, p->user_pc);

    /*
     * mstatus: MPP = U (mret returns to user mode), MPIE = 1 (mret
     * re-enables interrupts). MIE stays 0 (already cleared above).
     */
    uint64_t mstatus;
    CSR_READ(mstatus, mstatus);
    mstatus &= ~MSTATUS_MPP_M;   /* clear MPP field (bits [12:11]) */
    mstatus |= MSTATUS_MPP_U;    /* MPP = 00 (user) */
    mstatus |= MSTATUS_MPIE;     /* MIE ← MPIE on mret → interrupts on */
    CSR_WRITE(mstatus, mstatus);

    /* Arm the mscratch swap so the next trap from user lands on the
     * kernel stack. p->kstack holds the top address. */
    CSR_WRITE(mscratch, p->kstack);

    /* Final leg in asm: install satp, restore user GPRs, mret. */
    user_return(p->tf, MAKE_SATP(p->pagetable));

    /* Unreachable. */
    panic("proc_return_to_user: user_return returned");
}

/*
 * proc_exec_static — load a raw (non-ELF) user binary into a fresh proc.
 *
 * Memory layout installed in the proc's pagetable:
 *   [entry_va,               entry_va + text_pages*PAGE_SIZE)    R+X+U
 *   [entry_va + USER_STACK_OFF, entry_va + USER_STACK_OFF + PAGE_SIZE) R+W+U
 *
 * We place the text and stack in the same 1 GB root-entry slot so one
 * clone-on-write (or in our case, no CoW at all since we chose entry_va
 * in an unused root slot) suffices. The user stack is a single page,
 * initial sp at its top.
 */

proc_t *proc_exec_static(const void *bin, uint64_t size,
                         uint64_t entry_va, const char *name) {
    KASSERT((entry_va & (PAGE_SIZE - 1)) == 0,
            "proc_exec_static: entry_va not page-aligned");

    proc_t *p = proc_alloc();
    if (p == NULL)
        panic("proc_exec_static: no free slots");

    /* Copy the name (up to 15 chars + NUL). */
    int i = 0;
    while (name[i] && i < 15) { p->name[i] = name[i]; i++; }
    p->name[i] = '\0';

    /* Map and populate the text pages. */
    uint64_t text_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t pg = 0; pg < text_pages; pg++) {
        void *phys = pmem_alloc();
        if (phys == NULL)
            panic("proc_exec_static: oom mapping text");

        /* Copy this page's slice of the binary; zero any trailing bytes. */
        uint64_t off = pg * PAGE_SIZE;
        uint64_t to_copy = size - off;
        if (to_copy > PAGE_SIZE) to_copy = PAGE_SIZE;

        const uint8_t *src = (const uint8_t *)bin + off;
        uint8_t       *dst = (uint8_t *)phys;
        for (uint64_t j = 0; j < to_copy; j++)       dst[j] = src[j];
        for (uint64_t j = to_copy; j < PAGE_SIZE; j++) dst[j] = 0;

        if (vm_map(p->pagetable, entry_va + off, (uint64_t)phys,
                   PAGE_SIZE, PTE_USER_RX) < 0)
            panic("proc_exec_static: vm_map text failed");
    }

    /* Allocate and map the user stack (one page, initially zeroed). */
    void *stack_phys = pmem_alloc();
    if (stack_phys == NULL)
        panic("proc_exec_static: oom user stack");
    {
        uint8_t *sp = (uint8_t *)stack_phys;
        for (uint64_t j = 0; j < PAGE_SIZE; j++) sp[j] = 0;
    }

    uint64_t user_stack_va = entry_va + USER_STACK_OFFSET;
    if (vm_map(p->pagetable, user_stack_va, (uint64_t)stack_phys,
               PAGE_SIZE, PTE_USER_RW) < 0)
        panic("proc_exec_static: vm_map stack failed");

    /* Carve the trap frame out of the top of the kernel stack. The frame
     * size (256 bytes, see trap.h) leaves p->kstack - 256 == 16-aligned. */
    p->tf = (trap_frame_t *)(p->kstack - sizeof(trap_frame_t));

    /* Zero the frame so user regs start clean. */
    {
        uint64_t *words = (uint64_t *)p->tf;
        for (uint64_t j = 0; j < sizeof(trap_frame_t) / 8; j++)
            words[j] = 0;
    }

    /* User sp starts at the top of the user stack page. */
    p->tf->regs[REG_SP] = user_stack_va + PAGE_SIZE;

    /* Record the user entry point; proc_return_to_user writes it to mepc. */
    p->user_pc = entry_va;

    /*
     * Initial kernel context. When the scheduler first schedules this
     * proc, switch_context loads context.sp/ra and rets into
     * proc_return_to_user.
     *
     * context.sp = p->tf so the C prologue pushes below the trap frame
     * (not into it). p->tf is 16-byte aligned, so sp is ABI-compliant.
     */
    p->context.ra = (uint64_t)proc_return_to_user;
    p->context.sp = (uint64_t)p->tf;

    p->state = RUNNABLE;
    trace_emit(EV_SPAWN, p->pid);
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
        /*
         * Enable interrupts while scanning for runnable processes.
         * Trap entry (from user or kernel) clears MIE. After every
         * process yields/sleeps/exits, the scheduler inherits MIE=0
         * from the last switch_context. Without re-enabling MIE here,
         * timer interrupts can't fire, sleeping processes never wake,
         * and the scheduler deadlocks.
         *
         * Disable MIE before switch_context into a process so the
         * process's trap handler starts with a known MIE state.
         */
        CSR_SET(mstatus, MSTATUS_MIE);

        int found = 0;
        for (int i = 0; i < NPROC; i++) {
            proc_t *p = &proc_table[i];

            /* Reap parentless zombies. */
            if (p->state == ZOMBIE && p->parent == NULL) {
                proc_free(p);
                continue;
            }

            if (p->state != RUNNABLE)
                continue;

            found = 1;
            p->state = RUNNING;
            if (p->first_run_tick == 0)
                p->first_run_tick = (uint32_t)ticks;
            p->burst_start_tick = (uint32_t)ticks;   /* T3: open burst */
            /*
             * T6: trace RUN only for user processes. The shell yields
             * at ~1.7M/s; tracing its picks would flood the 16K ring
             * inside a few ms. Kernel threads (init_fn != NULL) are
             * infrastructure, not subjects of scheduler measurement.
             */
            if (p->init_fn == 0)
                trace_emit(EV_RUN, p->pid);
            current  = p;
            CSR_CLEAR(mstatus, MSTATUS_MIE);
            switch_context(&scheduler_context, &p->context);
            /* Back from sched(). current is NULL. Re-enable MIE
             * for the next iteration's scan + wfi. */
            CSR_SET(mstatus, MSTATUS_MIE);
        }

        if (!found) {
            /* Idle — MIE already set above. wfi wakes on timer. */
            __asm__ volatile("wfi");
        }
    }
}
