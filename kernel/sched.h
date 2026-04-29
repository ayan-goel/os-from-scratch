#ifndef SCHED_H
#define SCHED_H

#include "defs.h"

/*
 * sched.h — pluggable scheduler policy interface.
 *
 * Each scheduler is a sched_policy_t — a function-pointer table whose
 * hooks fire at the four points where scheduling decisions happen:
 *
 *   - pick_next      — scheduler() asks "what runs next?" (returns NULL
 *                      when the current sweep is exhausted; scheduler()
 *                      then wfi's until the next interrupt)
 *   - on_burst_end   — sched() informs the policy that a burst just
 *                      closed; policy reads p->state to learn why
 *                      (RUNNABLE = preempted, SLEEPING = voluntary,
 *                      ZOMBIE = exit)
 *   - should_preempt — timer ISR asks "should I preempt the running
 *                      user proc?". RR always returns 1; MLFQ tracks
 *                      per-level allotments and returns 1 only when
 *                      exhausted.
 *   - on_periodic    — timer ISR ticks the policy once per tick; used
 *                      by MLFQ for the periodic priority boost.
 *   - on_proc_init   — proc_alloc just produced an EMBRYO; policy
 *                      seeds per-proc state (e.g. MLFQ level).
 *   - on_activate    — `sched <name>` just made this policy active;
 *                      seed per-proc state for processes that already
 *                      exist (since they missed on_proc_init).
 *
 * Hot-swap semantics: shell disables MIE, writes active_sched, calls
 * on_activate, re-enables MIE. Single-CPU makes the pointer write
 * atomic.
 */

struct proc;   /* forward decl — kernel/proc.h pulls in real definition */

typedef struct sched_policy {
    const char *name;
    struct proc *(*pick_next)(void);
    void (*on_burst_end)(struct proc *p);
    int  (*should_preempt)(struct proc *p);
    void (*on_periodic)(void);
    void (*on_proc_init)(struct proc *p);
    void (*on_activate)(void);
} sched_policy_t;

/* Active policy. Read everywhere; written only by cmd_sched in shell.c
 * (with interrupts disabled). Pointer is volatile so the timer ISR
 * sees the new value on the next tick rather than a cached load. */
extern sched_policy_t * volatile active_sched;

/* Built-in policies. Phase 4 ships RR (always available) and MLFQ
 * (added in T3). Phase 5 will add v1 / v2 / bandit. */
extern sched_policy_t sched_rr;
extern sched_policy_t sched_mlfq;

/*
 * MLFQ tunables — exposed in the header so notes/tools can reason
 * about the configuration without grepping the implementation. See
 * D3 in tasks/plan.md.
 *
 *   MLFQ_LEVELS        — number of priority queues (4 means levels 0..3)
 *   mlfq_allotment[k]  — ticks the level-k allotment lasts; doubles
 *                        each level: {1, 2, 4, 8} at the default.
 *   MLFQ_BOOST_TICKS   — period of the priority boost (every N ticks
 *                        every proc resets to level 0). 100 ticks = 1 s
 *                        at the default 10 ms tick.
 */
#define MLFQ_LEVELS       4
#define MLFQ_BOOST_TICKS  100
extern const uint16_t mlfq_allotment[MLFQ_LEVELS];

/*
 * Look up a policy by name. Accepts "rr" / "round-robin" for RR,
 * "mlfq" for MLFQ. Returns NULL if no match. cmd_sched uses this.
 */
sched_policy_t *sched_policy_by_name(const char *name);

#endif /* SCHED_H */
