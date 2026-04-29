#ifndef PROC_H
#define PROC_H

#include "defs.h"
#include "mm/vm.h"
#include "trap.h"

/* ── Process state ───────────────────────────────────────────────────────── */

typedef enum {
    UNUSED   = 0,   /* slot is free */
    EMBRYO   = 1,   /* being created */
    RUNNABLE = 2,   /* ready to run */
    RUNNING  = 3,   /* currently on CPU */
    SLEEPING = 4,   /* blocked, waiting for wake_tick */
    ZOMBIE   = 5,   /* exited, waiting for parent to reap */
} proc_state_t;

/* ── Saved kernel context ────────────────────────────────────────────────── */

/*
 * context_t holds the callee-saved registers that switch_context() must
 * preserve across a context switch (RISC-V ABI: ra, sp, s0-s11).
 *
 * ra is special: switch_context saves the return address of the caller, so
 * when this context is resumed the CPU jumps back to wherever sched() was
 * called. For a brand-new process, ra is pre-loaded with proc_return_to_user
 * (T6) so the first "resume" enters user mode.
 */
typedef struct {
    uint64_t ra;   /* return address (where execution resumes) */
    uint64_t sp;   /* stack pointer */
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
} context_t;

/* ── Process descriptor ──────────────────────────────────────────────────── */

#define NPROC              64
#define KSTACK_SIZE        4096              /* 4 KB kernel stack per process */
#define USER_TEXT_BASE     0x40000000UL      /* user program text VA */
#define USER_STACK_OFFSET  0x00100000UL      /* user stack: 1 MB above text */

typedef struct proc {
    proc_state_t  state;
    int           pid;
    struct proc  *parent;
    pagetable_t   pagetable;
    uint64_t      kstack;      /* physical address of top of kernel stack */
    trap_frame_t *tf;          /* T6.2+: trap frame storage on this proc's kstack */
    context_t     context;     /* saved kernel register context */
    uint64_t      user_pc;     /* T6.4+: initial mepc for first U-mode entry */
    uint64_t      wake_tick;   /* for SLEEPING: tick at which to wake */
    int           exit_status; /* T7.4: stored by sys_exit, read by sys_wait */
    char          name[16];

    /* T5: kernel-function entry point (replaced by ELF entry in T6). */
    void (*init_fn)(void);

    /* T2 (Phase 3): per-process CPU accounting.
     * Ticks are 10 ms each; uint32 overflows after ~500 days — fine. */
    uint32_t cpu_ticks;       /* timer ticks charged to this proc while RUNNING */
    uint32_t start_tick;      /* value of `ticks` at proc_alloc time */
    uint32_t first_run_tick;  /* tick of first RUNNABLE→RUNNING edge, 0 if never */

    /* T3 (Phase 3): burst + scheduling-event counters. */
    uint32_t burst_start_tick;    /* set on RUNNABLE→RUNNING; zero when not running */
    uint32_t burst_sum;           /* sum of all completed-burst lengths (ticks) */
    uint32_t burst_count;         /* number of completed bursts */
    uint32_t last_burst;          /* most recent burst length (ticks) */
    uint32_t voluntary_yields;    /* sys_yield / kernel_yield calls */
    uint32_t involuntary_preempts;/* timer-driven preemptions */
    uint32_t sleep_calls;         /* sys_sleep / kernel_sleep calls */

    /*
     * Scheduler-specific accounting fields are added as each scheduler phase
     * needs them. See SPEC.md §4 Phase 3/5 for the staging plan.
     */
} proc_t;

/* ── Globals ─────────────────────────────────────────────────────────────── */

extern proc_t  proc_table[NPROC];
extern proc_t *current;           /* process currently running, or NULL */

/* T5 (Phase 3): total scheduling decisions across the kernel's lifetime.
 * Incremented in sched(); read by `stats` and the TUI sched panel. */
extern uint64_t sched_total_decisions;

/* ── API ─────────────────────────────────────────────────────────────────── */

void    proc_init(void);          /* zero table, allocate kstacks */
proc_t *proc_alloc(void);         /* find UNUSED slot → EMBRYO */
void    proc_free(proc_t *p);     /* ZOMBIE → UNUSED; free kstack + pagetable */
void    sched(void);              /* yield CPU back to scheduler */
void    scheduler(void) __attribute__((noreturn));

/*
 * P2.3: find a process by pid. Returns NULL if no slot has that pid,
 * or the slot is UNUSED. O(NPROC) linear scan.
 */
proc_t *proc_find_by_pid(int pid);

/* T5: spawn a kernel-function process (kept for unit tests; not used in prod). */
proc_t *proc_spawn_fn(void (*fn)(void), const char *name);

/* T7.2: fork — duplicate the current process. Returns child pid to parent,
 * 0 to child, or -1 on failure. */
int proc_fork(void);

/* T7.3: exec — replace current process image with a named binary.
 * Returns -1 on failure (binary not found); does not return on success.
 * The binary is looked up in the ramfs via fs_lookup (kernel/fs.h). */
int proc_exec(const char *name, trap_frame_t *frame);

/*
 * T6.4: Load a raw (objcopy -O binary) user program into a fresh proc.
 *
 *   bin      — pointer to the raw binary bytes (embedded in kernel image)
 *   size     — length of the binary in bytes
 *   entry_va — user virtual address where the binary's first byte lands;
 *              also the initial mepc. Must be page-aligned.
 *   name     — process name (copied into p->name, truncated to 15 chars)
 *
 * Allocates text pages, copies the binary, allocates a user stack, sets
 * up the trap frame with user sp and entry point, and arms context.ra =
 * proc_return_to_user so the first schedule of this proc drops to U-mode.
 */
proc_t *proc_exec_static(const void *bin, uint64_t size,
                         uint64_t entry_va, const char *name);

/*
 * T6.4: First-time trampoline into user mode. Entered via context.ra on
 * the first scheduling of a freshly-exec'd proc. Sets mepc/mstatus/mscratch
 * and tail-calls user_return (asm) to install satp and mret. Never returns.
 */
void proc_return_to_user(void) __attribute__((noreturn));

/* Defined in switch.S */
void switch_context(context_t *old, context_t *new);

/* Defined in arch/trapvec.S — final leg of first user-mode entry.
 *   a0 = trap frame pointer
 *   a1 = satp value (MAKE_SATP of p->pagetable)
 * Never returns. */
void user_return(trap_frame_t *tf, uint64_t satp) __attribute__((noreturn));

#endif /* PROC_H */
