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

    /* Phase 4/5: per-policy allotment state.
     *
     * mlfq_level / mlfq_demote_count are MLFQ-only. mlfq_used_in_level
     * is reused by V2 as its class-quantum counter (V2's class →
     * quantum mapping needs a per-proc counter and MLFQ is inactive
     * when V2 is, so dual-use is safe). Both policies reset it to 0
     * at activate/burst-end so a hot-swap doesn't leak stale values. */
    uint8_t  mlfq_level;          /* MLFQ: 0 = highest, MLFQ_LEVELS-1 = lowest */
    uint16_t mlfq_used_in_level;  /* MLFQ: ticks at current level; V2: ticks at current class */
    uint64_t mlfq_demote_count;   /* MLFQ: lifetime demotions (for stats + trace) */

    /* Phase 5 V1: exponentially-weighted burst-length estimate.
     * τ_{n+1} = ((t_n << 3) + τ_n) >> 1  with α=1/2 — one shift + one
     * add per burst completion. Stored at 8× scale (3 fractional bits)
     * to keep ≥1 bit of precision after the right-shift; without this,
     * 1-tick bursts collapse to τ=0 forever (tick-resolution floor).
     * Convert back to ticks via (burst_estimate >> 3).
     *
     * v1_pick_next picks the RUNNABLE proc with the smallest τ
     * (approximate SRTF). V2/V3 use this same field as a feature.
     * Untouched under RR/MLFQ. */
    uint32_t burst_estimate;

    /* Phase 5 V2: online classification into one of 4 behavioral classes.
     * burst_variance is a β=1/4 EW estimate of (last_burst - τ)² —
     * tracked but unused by the classifier (kept for V3's context vec).
     * proc_class is updated on every burst end via a hand-coded threshold
     * tree; v2_pick_next prioritizes classes 0..3 in order. */
    uint32_t burst_variance;
    uint8_t  proc_class;

    /* Phase 5 V3: contextual bandit visit count. Used in the UCB
     * exploration bonus (rare-procs get a higher score so the bandit
     * doesn't permanently lock out untried options). */
    uint32_t v3_visits;

    /* Phase 7 V2: ring of recent burst end reasons.
     *
     * Phase 6 found V2's classifier sticks at IO_BOUND on phase changes
     * because it reads lifetime sleep_calls / voluntary_yields /
     * involuntary_preempts. After 10 sleeps a workload is permanently
     * IO_BOUND even if its behavior flips. Fix: classify from the last
     * K=10 bursts only. v2_classify scans the most-recent K'=5 entries
     * (window-within-window) so adaptation completes inside ~3 cpu
     * preempts on flipper, matching MLFQ's demote ladder.
     *
     * Encoding: one byte per entry. 0=SLEEP, 1=YIELD, 2=PREEMPT, 3=EXIT,
     * 4=OTHER. cursor advances on each v2_on_burst_end write; fill is
     * capped at sizeof(burst_window) so the classifier can detect a
     * partly-filled ring on freshly-spawned procs.
     *
     * v2_last_yields / v2_last_preempts are snapshots of the lifetime
     * counters at the last v2_on_burst_end call — used to detect which
     * counter ticked during the just-closed burst (yield vs preempt
     * leave the same RUNNABLE state, so state alone can't tell us).
     *
     * Untouched under RR/MLFQ/V1/V3 — only V2 reads or writes these. */
    uint8_t burst_window[10];
    uint8_t burst_window_cursor;
    uint8_t burst_window_fill;
    uint32_t v2_last_yields;
    uint32_t v2_last_preempts;

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
