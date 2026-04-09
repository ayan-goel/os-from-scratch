#ifndef PROC_H
#define PROC_H

#include "defs.h"
#include "mm/vm.h"

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

#define NPROC       64
#define KSTACK_SIZE 4096   /* 4 KB kernel stack per process */

typedef struct proc {
    proc_state_t  state;
    int           pid;
    struct proc  *parent;
    pagetable_t   pagetable;
    uint64_t      kstack;      /* physical address of top of kernel stack */
    context_t     context;     /* saved kernel register context */
    uint64_t      wake_tick;   /* for SLEEPING: tick at which to wake */
    char          name[16];

    /* T5: kernel-function entry point (replaced by ELF entry in T6). */
    void (*init_fn)(void);

    /*
     * Scheduler-specific accounting fields are added as each scheduler phase
     * needs them. See SPEC.md §4 Phase 3/5 for the staging plan.
     */
} proc_t;

/* ── Globals ─────────────────────────────────────────────────────────────── */

extern proc_t  proc_table[NPROC];
extern proc_t *current;           /* process currently running, or NULL */

/* ── API ─────────────────────────────────────────────────────────────────── */

void    proc_init(void);          /* zero table, allocate kstacks */
proc_t *proc_alloc(void);         /* find UNUSED slot → EMBRYO */
void    proc_free(proc_t *p);     /* ZOMBIE → UNUSED; free kstack + pagetable */
void    sched(void);              /* yield CPU back to scheduler */
void    scheduler(void) __attribute__((noreturn));

/* T5: spawn a kernel-function process (replaced by proc_exec in T6). */
proc_t *proc_spawn_fn(void (*fn)(void), const char *name);

/* Defined in switch.S */
void switch_context(context_t *old, context_t *new);

#endif /* PROC_H */
