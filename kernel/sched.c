/*
 * sched.c — scheduler policy registry + round-robin implementation.
 *
 * Round-robin is the original Phase 1 scheduler, reorganized as the
 * first instance of the sched_policy_t interface. All hooks here
 * reproduce the pre-Phase-4 behavior bit-for-bit:
 *
 *   - pick_next walks proc_table once per "sweep" (the loop in
 *     scheduler()). On each call it returns the next RUNNABLE slot
 *     after the previous; when the sweep ends it returns NULL,
 *     scheduler() does a wfi, and a fresh sweep starts on the next
 *     wake.
 *
 *   - should_preempt always returns 1 — every timer tick that finds a
 *     user process RUNNING preempts it.
 *
 *   - on_burst_end / on_periodic / on_proc_init / on_activate are
 *     no-ops. RR has no per-process state beyond what's already in
 *     proc_t.
 */

#include "sched.h"
#include "proc.h"
#include "trace.h"

extern volatile uint64_t ticks;   /* defined in trap.c */

/* ── Round-robin implementation ──────────────────────────────────── */

/*
 * rr_cursor — index of the next slot to consider in the current sweep.
 *
 * Reset to 0 when pick_next exhausts the table; advances by one on
 * each successful pick. This preserves the pre-refactor behavior of
 * `for (i = 0; i < NPROC; i++)` — every sweep visits each slot once
 * in order, so a single RUNNABLE proc with low pid doesn't starve the
 * higher-pid procs in its same sweep.
 */
static int rr_cursor = 0;

static struct proc *rr_pick_next(void) {
    while (rr_cursor < NPROC) {
        struct proc *p = &proc_table[rr_cursor++];
        if (p->state == RUNNABLE)
            return p;
    }
    rr_cursor = 0;
    return NULL;
}

static void rr_on_burst_end(struct proc *p)   { (void)p; }
static int  rr_should_preempt(struct proc *p) { (void)p; return 1; }
static void rr_on_periodic(void)              { }
static void rr_on_proc_init(struct proc *p)   { (void)p; }
static void rr_on_activate(void)              { rr_cursor = 0; }

sched_policy_t sched_rr = {
    .name           = "round-robin",
    .pick_next      = rr_pick_next,
    .on_burst_end   = rr_on_burst_end,
    .should_preempt = rr_should_preempt,
    .on_periodic    = rr_on_periodic,
    .on_proc_init   = rr_on_proc_init,
    .on_activate    = rr_on_activate,
};

/* ── MLFQ implementation ─────────────────────────────────────────── */

const uint16_t mlfq_allotment[MLFQ_LEVELS] = { 1, 2, 4, 8 };

/*
 * mlfq_cursor[level] — RR-style cursor within each priority queue, so
 * two procs at the same level take turns rather than the lower-pid one
 * starving the higher-pid one. Reset to 0 at proc_alloc, on_activate,
 * and after each cursor-exhausting sweep at that level.
 */
static int mlfq_cursor[MLFQ_LEVELS];

/* Tick of the most recent boost; on_periodic compares against this. */
static uint64_t mlfq_last_boost = 0;

/*
 * mlfq_pick_next — scan from level 0 upward. At each level, scan the
 * proc table starting at this level's cursor; the first RUNNABLE proc
 * whose mlfq_level matches is returned. The cursor advances past it
 * so the next call at the same level picks a different proc.
 *
 * If a level has no RUNNABLE procs, fall through to the next level.
 * If all levels exhaust, return NULL (sweep done; scheduler() wfi's
 * iff no proc ran during this whole sweep — same idle semantics as RR).
 *
 * Cost: O(MLFQ_LEVELS × NPROC) worst case = 4 × 64 = 256 ops. At
 * 100 Hz scheduling that's negligible.
 */
static struct proc *mlfq_pick_next(void) {
    for (int lv = 0; lv < MLFQ_LEVELS; lv++) {
        for (int i = 0; i < NPROC; i++) {
            int idx = (mlfq_cursor[lv] + i) % NPROC;
            struct proc *p = &proc_table[idx];
            if (p->state != RUNNABLE)
                continue;
            /*
             * Kernel threads (shell, tui — init_fn != 0) are
             * infrastructure: they yield in tight loops and would
             * otherwise dominate level 0, starving every user proc.
             * Treat them as if pinned to MLFQ_LEVELS-1 so they only
             * run when no user work is available. We don't actually
             * mutate p->mlfq_level — leaving it untouched lets RR
             * still see the same struct after a hot-swap.
             */
            int effective_level = (p->init_fn != 0)
                ? (MLFQ_LEVELS - 1)
                : p->mlfq_level;
            if (effective_level != lv)
                continue;
            mlfq_cursor[lv] = (idx + 1) % NPROC;
            return p;
        }
        /* Whole table scanned at this level — none RUNNABLE here. Try
         * the next level. Don't reset cursor: the next sweep starts
         * fresh from where we left off, preserving fair rotation. */
    }
    return NULL;
}

/*
 * mlfq_should_preempt — increments the running proc's used-allotment
 * counter (called once per timer tick the proc spent RUNNING) and
 * returns 1 iff the level's allotment is exhausted.
 *
 * The actual demotion happens in on_burst_end (which fires after
 * sched() closes the burst), not here — keeps preempt logic and
 * level-state mutation in separate hooks that are easier to reason
 * about.
 */
static int mlfq_should_preempt(struct proc *p) {
    p->mlfq_used_in_level++;
    return p->mlfq_used_in_level >= mlfq_allotment[p->mlfq_level];
}

/*
 * mlfq_on_burst_end — called from sched() right after the burst is
 * closed. p->state encodes WHY the burst ended:
 *
 *   RUNNABLE  — preempted by timer (allotment expired). Demote one
 *               level (clamped at MLFQ_LEVELS-1) and reset the
 *               allotment counter for the new level.
 *   SLEEPING  — voluntary sleep. Stay at level; reset allotment so
 *               the next time this proc runs at this level it gets a
 *               fresh quantum.
 *   ZOMBIE    — exiting. No-op; the slot will be freed.
 */
static void mlfq_on_burst_end(struct proc *p) {
    if (p->state == RUNNABLE) {
        if (p->mlfq_level < MLFQ_LEVELS - 1) {
            p->mlfq_level++;
            p->mlfq_demote_count++;
            /* Trace only user-proc demotions (matches the kernel-thread
             * filter on EV_RUN). Kernel threads can't be demoted in
             * practice — they yield voluntarily and never exhaust the
             * level-0 allotment — but the guard documents intent. */
            if (p->init_fn == 0)
                trace_emit(EV_DEMOTE, p->pid);
        }
        p->mlfq_used_in_level = 0;
    } else if (p->state == SLEEPING) {
        p->mlfq_used_in_level = 0;
    }
    /* ZOMBIE: leave state alone. */
}

/* New proc starts at the top priority with a fresh allotment. */
static void mlfq_on_proc_init(struct proc *p) {
    p->mlfq_level = 0;
    p->mlfq_used_in_level = 0;
}

/*
 * Hot-swap entry: reset every existing proc to (level=0, used=0) and
 * zero all level cursors. Procs created under the previous policy
 * inherited (0, 0) from memzero in proc_alloc anyway, but cursors may
 * have advanced under MLFQ before a swap-back-and-forth, so re-zeroing
 * is the safe default.
 */
static void mlfq_on_activate(void) {
    for (int i = 0; i < NPROC; i++) {
        if (proc_table[i].state != UNUSED) {
            proc_table[i].mlfq_level = 0;
            proc_table[i].mlfq_used_in_level = 0;
        }
    }
    for (int lv = 0; lv < MLFQ_LEVELS; lv++)
        mlfq_cursor[lv] = 0;
    /* Reset boost timer so the first boost after a hot-swap fires a
     * full BOOST_TICKS later, not immediately (which would happen if
     * mlfq_last_boost stayed at its boot-time 0 while `ticks` had
     * accumulated). */
    mlfq_last_boost = ticks;
}

/*
 * Periodic priority boost: every MLFQ_BOOST_TICKS, lift every non-
 * UNUSED proc back to level 0 with a fresh allotment. This is the
 * starvation mitigation; without it, a long-lived cpu_bound that
 * demoted to level 3 stays stuck behind any io_bound at level 0
 * forever.
 *
 * One EV_BOOST trace record per fire (with pid=0 as a global-event
 * sentinel) so parse_trace.py can mark boost moments on the Gantt.
 *
 * Cost: NPROC iterations + one trace emit, all with interrupts
 * disabled. NPROC=64 → ~100 ns total. Negligible.
 */
static void mlfq_on_periodic(void) {
    if (ticks - mlfq_last_boost < MLFQ_BOOST_TICKS)
        return;
    mlfq_last_boost = ticks;

    for (int i = 0; i < NPROC; i++) {
        if (proc_table[i].state != UNUSED) {
            proc_table[i].mlfq_level = 0;
            proc_table[i].mlfq_used_in_level = 0;
        }
    }
    trace_emit(EV_BOOST, 0);
}

sched_policy_t sched_mlfq = {
    .name           = "mlfq",
    .pick_next      = mlfq_pick_next,
    .on_burst_end   = mlfq_on_burst_end,
    .should_preempt = mlfq_should_preempt,
    .on_periodic    = mlfq_on_periodic,
    .on_proc_init   = mlfq_on_proc_init,
    .on_activate    = mlfq_on_activate,
};

/* ── Active-policy globals + name lookup ─────────────────────────── */

sched_policy_t * volatile active_sched = &sched_rr;

static int sched_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

sched_policy_t *sched_policy_by_name(const char *name) {
    if (sched_streq(name, "rr"))           return &sched_rr;
    if (sched_streq(name, "round-robin"))  return &sched_rr;
    if (sched_streq(name, "mlfq"))         return &sched_mlfq;
    return NULL;
}
