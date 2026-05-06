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

/* ── V1: Exponentially-Weighted Burst Prediction ─────────────────── */
/*
 * V1 is the first online-learning policy: each process maintains an
 * exponentially-weighted moving average of its recent burst lengths,
 * and the scheduler always picks the RUNNABLE proc with the smallest
 * estimate (approximate SRTF).
 *
 * Update rule:  τ_{n+1} = α · t_n + (1-α) · τ_n   with α = 1/2
 *               implemented as  τ' = (t + τ) >> 1
 *
 * One shift + one add per burst completion. No allocation. No FP. The
 * estimate is in the same units as last_burst (10 ms ticks), so a CPU-
 * bound process settles around τ=1 (preempted every quantum) while an
 * I/O-bound process whose bursts are sub-tick stays at τ=0 — pick_next
 * always favors the I/O proc. Tick resolution is the floor here;
 * sub-tick discrimination would need rdcycle (deferred to Phase 6).
 */

static struct proc *v1_pick_next(void) {
    /* Cursor used as a tiebreaker only — when multiple procs share the
     * minimum τ, advance fairly through them rather than always picking
     * the lowest-index slot. */
    static int v1_cursor = 0;

    struct proc *best = NULL;
    int          best_idx = -1;
    uint32_t     best_tau = 0;

    for (int i = 0; i < NPROC; i++) {
        int idx = (v1_cursor + i) % NPROC;
        struct proc *p = &proc_table[idx];
        if (p->state != RUNNABLE)
            continue;
        /* Kernel threads run only when no user work is RUNNABLE — same
         * trick as MLFQ. Treat them as having maximum τ. */
        uint32_t tau = (p->init_fn != 0) ? 0xFFFFFFFFu : p->burst_estimate;
        if (best == NULL || tau < best_tau) {
            best = p;
            best_idx = idx;
            best_tau = tau;
        }
    }
    if (best != NULL)
        v1_cursor = (best_idx + 1) % NPROC;
    return best;
}

static void v1_on_burst_end(struct proc *p) {
    if (p->state == ZOMBIE)
        return;   /* exiting; estimate irrelevant */
    /* α = 1/2 EWMA at 8× scale: τ_new = ((t<<3) + τ) >> 1.
     * The 8× scaling keeps 3 fractional bits across the right-shift,
     * so a 1-tick burst nudges τ from 0 → 4 → 6 → 7 → 8 across
     * successive bursts instead of collapsing to 0 immediately. */
    p->burst_estimate = (((uint32_t)p->last_burst << 3) + p->burst_estimate + 1) >> 1;
}

static int  v1_should_preempt(struct proc *p) { (void)p; return 1; }
static void v1_on_periodic(void)              { }
static void v1_on_proc_init(struct proc *p)   { p->burst_estimate = 0; }
static void v1_on_activate(void)              { }

sched_policy_t sched_v1 = {
    .name           = "v1",
    .pick_next      = v1_pick_next,
    .on_burst_end   = v1_on_burst_end,
    .should_preempt = v1_should_preempt,
    .on_periodic    = v1_on_periodic,
    .on_proc_init   = v1_on_proc_init,
    .on_activate    = v1_on_activate,
};

/* ── V2: Online Process Classifier + Policy Map ──────────────────── */
/*
 * V2 augments V1's burst estimate with two more features computed
 * online:
 *
 *   - burst_variance: β=1/4 EW estimate of (last_burst - burst_estimate)²
 *     (kept for V3's context vector; the classifier itself only uses
 *     burst_estimate, voluntary_yields, sleep_calls, involuntary_preempts)
 *
 *   - voluntary_yield_ratio: fixed-point in 1/256 units, computed at
 *     classify time. yields / (yields + preempts + 1)
 *
 * The classifier is a hand-coded threshold tree — structurally a Naive
 * Bayes model with equal priors and hard thresholds, the simplest
 * thing that discriminates the four classes Phase 6's workloads
 * exhibit. The tree is intentionally conservative (the four conditions
 * are independent so misclassification is bounded) and has no tuning
 * knobs — every threshold has a 1-line justification.
 *
 * Pick rule: scan classes 0..3 in priority order; within each class,
 * advance a single shared cursor for round-robin fairness. This is
 * structurally identical to MLFQ's pick_next but the level mapping is
 * data-driven (per-proc class) instead of demotion-driven (per-proc
 * level).
 */

static proc_class_t v2_classify(struct proc *p) {
    /* Voluntary-yield ratio in 1/256 fixed-point. The +1 in the
     * denominator avoids a divide-by-zero on freshly-spawned procs
     * and biases toward "interactive" until the first burst. */
    uint32_t denom = p->voluntary_yields + p->involuntary_preempts + 1;
    uint32_t yield_ratio = (p->voluntary_yields * 256) / denom;

    /*
     * INTERACTIVE: yields way more than it gets preempted (yield_ratio
     * > 50%) AND has short bursts. The shell/tui kernel threads would
     * match this — but the pick_next override pins kernel threads to
     * BATCH regardless of class. burst_estimate is stored at 8× scale
     * (see proc.h), so "≤ 1 tick" is "≤ 8" in stored units.
     */
    if (yield_ratio > 128 && p->burst_estimate <= 8)
        return PCLASS_INTERACTIVE;

    /*
     * IO_BOUND: has slept at least twice and recent bursts are short.
     * The "twice" threshold filters one-off sleeps from genuinely
     * sleep-driven workloads.
     */
    if (p->sleep_calls >= 2 && p->burst_estimate <= 8)
        return PCLASS_IO_BOUND;

    /*
     * CPU_BOUND: at least one preempt (so the scheduler has actually
     * observed it running) AND fewer sleeps than preempts (otherwise
     * it would have been classified as I/O above).
     */
    if (p->involuntary_preempts > 0 &&
        p->sleep_calls < p->involuntary_preempts)
        return PCLASS_CPU_BOUND;

    /* Fallback. New procs without enough history land here. */
    return PCLASS_BATCH;
}

static void v2_on_burst_end(struct proc *p) {
    if (p->state == ZOMBIE)
        return;

    /* Update burst_variance: standard EW variance with β=1/4.
     *   σ²_new = (1/4)·diff² + (3/4)·σ²_old
     * Compute in uint64 to handle (uint32_t)² overflow. */
    int64_t diff = (int64_t)p->last_burst - (int64_t)p->burst_estimate;
    uint64_t diff_sq = (uint64_t)(diff * diff);
    uint64_t new_var = (diff_sq >> 2) + (((uint64_t)p->burst_variance * 3) >> 2);
    p->burst_variance = (uint32_t)(new_var > 0xFFFFFFFFu ? 0xFFFFFFFFu : new_var);

    /* Update burst_estimate: same 8×-scale EWMA as V1. */
    p->burst_estimate = (((uint32_t)p->last_burst << 3) + p->burst_estimate + 1) >> 1;

    /* Reclassify. Cheap — at most 2 comparisons per condition. */
    p->proc_class = (uint8_t)v2_classify(p);

    /* Reset the class-quantum counter so the next burst at this class
     * gets a fresh slice. This is the "allotment per burst" semantics
     * (vs MLFQ's "allotment per level") — procs that voluntarily yield
     * mid-allotment don't lose the rest of their slice on next run. */
    p->mlfq_used_in_level = 0;
}

static struct proc *v2_pick_next(void) {
    /* Single shared cursor across all classes — within each class we
     * advance round-robin starting from the cursor. After picking a
     * proc, advance the cursor past it. */
    static int v2_cursor = 0;

    for (int cls = 0; cls < PCLASS_COUNT; cls++) {
        for (int i = 0; i < NPROC; i++) {
            int idx = (v2_cursor + i) % NPROC;
            struct proc *p = &proc_table[idx];
            if (p->state != RUNNABLE)
                continue;
            /* Kernel threads → BATCH regardless of stored class. */
            int eff = (p->init_fn != 0) ? PCLASS_BATCH : p->proc_class;
            if (eff != cls)
                continue;
            v2_cursor = (idx + 1) % NPROC;
            return p;
        }
    }
    return NULL;
}

/*
 * V2's class → quantum mapping. The classifier is only useful if its
 * class assignments lead to different scheduling behavior; the quantum
 * is what closes the loop:
 *
 *   INTERACTIVE  1 tick  — wake-and-yield pattern; long quantum wasted
 *   IO_BOUND     1 tick  — sub-tick bursts; quantum doesn't matter
 *   CPU_BOUND    5 ticks — 50 ms; amortize ctx-switch cost over compute
 *   BATCH       10 ticks — runs only when idle; cheapest scheduling
 *
 * Reuses mlfq_used_in_level as the allotment counter. Reset to 0 in
 * v2_on_burst_end so the next burst at this class gets a fresh slice.
 * Kernel threads always preempt every tick so they yield back fast.
 */
static int v2_should_preempt(struct proc *p) {
    if (p->init_fn != 0)
        return 1;

    p->mlfq_used_in_level++;
    uint16_t allot;
    switch (p->proc_class) {
    case PCLASS_CPU_BOUND: allot = 5;  break;
    case PCLASS_BATCH:     allot = 10; break;
    default:               allot = 1;  break;   /* INT, IO */
    }
    if (p->mlfq_used_in_level >= allot) {
        p->mlfq_used_in_level = 0;
        return 1;
    }
    return 0;
}
static void v2_on_periodic(void)              { }
static void v2_on_proc_init(struct proc *p) {
    p->burst_estimate = 0;
    p->burst_variance = 0;
    /* Optimistic default — new procs are treated as INTERACTIVE until
     * their first burst contradicts that. */
    p->proc_class = PCLASS_INTERACTIVE;
}
static void v2_on_activate(void) {
    /* Reclassify everyone the first time we see them so a hot-swap
     * from another policy doesn't run with stale class assignments.
     * Also zero mlfq_used_in_level so the dual-use counter starts
     * fresh — a swap from MLFQ may have left it mid-allotment. */
    for (int i = 0; i < NPROC; i++) {
        if (proc_table[i].state != UNUSED) {
            proc_table[i].proc_class = (uint8_t)v2_classify(&proc_table[i]);
            proc_table[i].mlfq_used_in_level = 0;
        }
    }
}

sched_policy_t sched_v2 = {
    .name           = "v2",
    .pick_next      = v2_pick_next,
    .on_burst_end   = v2_on_burst_end,
    .should_preempt = v2_should_preempt,
    .on_periodic    = v2_on_periodic,
    .on_proc_init   = v2_on_proc_init,
    .on_activate    = v2_on_activate,
};

/* ── V3: Contextual Bandit Scheduler ─────────────────────────────── */
/*
 * The most ambitious of the Phase 5 policies: every scheduling
 * decision is treated as a bandit problem. For each RUNNABLE proc we
 * compute an 8-feature context vector, score it with a learned weight
 * vector via dot product, add a UCB exploration bonus, and pick the
 * highest-scoring proc. After the proc runs, we observe a reward and
 * update the weights with a small gradient step.
 *
 * SPEC.md §V3 mandates this comment state the cost bound and the
 * fixed-point scale factors. Three deviations from spec, each
 * deliberate:
 *
 *   (1) SPEC: "O(1) per decision". REALITY: O(NPROC). Both pick_next
 *       and RR's pick_next sweep proc_table once. The bandit's per-
 *       slot cost is bigger (≈8 integer multiplies + 8 adds for the
 *       dot product) but still constant per slot. Phase 5 doesn't
 *       maintain a sorted RUNNABLE list, so achieving true O(1)
 *       would require restructuring proc_table — a Phase 6+ change.
 *
 *   (2) SPEC: "weights scaled by 2^16, gradient updates are integer
 *       additions". REALITY: weights are raw int32; updates are
 *       (reward · ctx_i) >> V3_LR_BITS where V3_LR_BITS=10.
 *       Equivalent in spirit (fixed-point integer math, no FP) but
 *       scaled differently — a 2^16 weight scale would need ctx
 *       features at smaller scale to avoid int32 overflow on the
 *       dot product accumulator. Our scale was chosen so weights
 *       converge to small int values (single digits) on our test
 *       workloads; the LR_BITS shift acts as the implicit scale.
 *
 *   (3) SPEC: "reward = negative turnaround at exit/checkpoint".
 *       REALITY: reward fires on every burst, not just exit. Per-
 *       burst feedback is denser (every burst is a learning signal,
 *       not just the few-per-run exit events) so the bandit
 *       converges meaningfully on long-running procs. The trade-off
 *       is documented in notes/phase5-learning-scheduler.md.
 *
 * Scale factors (all integer / fixed-point — no FP):
 *   - Weights stored as int32. Initial values 0. Updates are integer
 *     additions of (reward · ctx_i) >> V3_LR_BITS.
 *   - Context features are raw (unscaled) integer values; ranges:
 *       ctx[0] burst_estimate          0..hundreds   (ticks)
 *       ctx[1] burst_variance          0..millions   (ticks²)
 *       ctx[2] voluntary_yield_ratio   0..256        (1/256 fixed-point)
 *       ctx[3] io_rate                 0..hundreds   (sleeps per 100 cpu ticks)
 *       ctx[4] time_since_last_run     0..thousands  (ticks)
 *       ctx[5] proc_class              0..3
 *       ctx[6] age                     0..thousands  (ticks since spawn)
 *       ctx[7] reserved                0
 *   - Score = Σ w_i · ctx_i  (int64 to avoid overflow on the
 *     accumulation).
 *   - UCB bonus = V3_UCB_C / (visits + 1). Procs picked rarely get a
 *     large bonus so the bandit doesn't permanently lock out untried
 *     options; common procs see a vanishing bonus.
 *
 * Cost analysis (per scheduling decision):
 *   - One sweep over NPROC slots = 64 iterations
 *   - For each RUNNABLE proc: one v3_context() (≈ 8 integer ops) +
 *     one 8-element dot product (8 multiplies + 8 adds) + UCB bonus
 *     (1 division — int32, no FP)
 *   - Per-decision worst case: 64 · (8 + 8·2 + 1) ≈ 1600 integer ops
 *   - In practice (≤5 RUNNABLE): ~125 integer ops per decision
 *   - O(NPROC) — same complexity class as RR's pick_next; constant
 *     factor ~10× larger but still bounded
 *
 * Reward signal (per burst):
 *   reward = -(int32_t)last_burst         // long bursts are bad
 *          + (state == SLEEPING ? 1 : 0)  // voluntary sleeps are good
 *   The negative-burst-length component pushes the bandit toward
 *   choices that produce short bursts (typical of I/O-bound workloads
 *   or interactive ones); the sleep bonus rewards yielding behavior.
 *
 * Per-proc state: v3_visits (uint32_t = 4 bytes).
 * Global state:   v3_weights[8] (int32_t × 8 = 32 bytes).
 */

#define V3_NUM_FEATURES   8
#define V3_LR_BITS        10           /* learning rate = 1/1024 */
#define V3_UCB_C          1000000      /* UCB constant */

static int32_t v3_weights[V3_NUM_FEATURES];

/* Short labels for the TUI WEIGHTS row. Each MUST be exactly 9 chars
 * (padded with trailing spaces) since the renderer writes a fixed-
 * width field via uart_write_raw. Order matches ctx[] slots filled
 * by v3_context. */
static const char *v3_feature_labels[V3_NUM_FEATURES] = {
    "burst_est",   /* 9 */
    "burst_var",   /* 9 */
    "yield_rt ",   /* 9 (padded) */
    "io_rate  ",   /* 9 (padded) */
    "since_run",   /* 9 */
    "class    ",   /* 9 (padded) */
    "age      ",   /* 9 (padded) */
    "(rsvd)   ",   /* 9 (padded) */
};

int v3_top_feature(void) {
    int best = 0;
    int32_t best_abs = 0;
    for (int i = 0; i < V3_NUM_FEATURES; i++) {
        int32_t v = v3_weights[i];
        int32_t a = v < 0 ? -v : v;
        if (a > best_abs) {
            best_abs = a;
            best = i;
        }
    }
    return best;
}

int32_t v3_weight(int idx) {
    if (idx < 0 || idx >= V3_NUM_FEATURES) return 0;
    return v3_weights[idx];
}

const char *v3_feature_name(int idx) {
    if (idx < 0 || idx >= V3_NUM_FEATURES) return "?       ";
    return v3_feature_labels[idx];
}

extern volatile uint64_t ticks;

static void v3_context(struct proc *p, int32_t *ctx) {
    ctx[0] = (int32_t)p->burst_estimate;
    ctx[1] = (int32_t)p->burst_variance;

    /* yield_ratio in 1/256 fixed-point */
    uint32_t denom = p->voluntary_yields + p->involuntary_preempts + 1;
    ctx[2] = (int32_t)((p->voluntary_yields * 256) / denom);

    /* io_rate: sleeps per 100 cpu ticks (≈ sleeps/sec at 100 Hz tick) */
    ctx[3] = (int32_t)((p->sleep_calls * 100) / (p->cpu_ticks + 1));

    /* time since burst started, or 0 if not currently RUNNING */
    uint32_t now = (uint32_t)ticks;
    ctx[4] = (int32_t)(p->burst_start_tick == 0 ? 0
                       : now - p->burst_start_tick);

    ctx[5] = (int32_t)p->proc_class;
    ctx[6] = (int32_t)(now - p->start_tick);
    ctx[7] = 0;
}

static int64_t v3_score(struct proc *p) {
    int32_t ctx[V3_NUM_FEATURES];
    v3_context(p, ctx);
    int64_t score = 0;
    for (int i = 0; i < V3_NUM_FEATURES; i++)
        score += (int64_t)v3_weights[i] * (int64_t)ctx[i];
    /* UCB exploration bonus: c / (visits + 1) */
    score += V3_UCB_C / ((int64_t)p->v3_visits + 1);
    return score;
}

/*
 * v3_pick_next — sweep semantics matching RR/MLFQ/V2. Each "sweep"
 * walks the table once, returning RUNNABLE procs one at a time, then
 * returns NULL so scheduler() can wfi.
 *
 * Within a sweep:
 *   - On the FIRST call of a sweep, score every RUNNABLE user proc and
 *     return the highest-scoring one.
 *   - Subsequent calls in the same sweep return remaining RUNNABLE procs
 *     in cursor order (kernel threads + lower-scoring users).
 *   - When the cursor reaches NPROC, return NULL and reset.
 *
 * The "score the user procs once per sweep" pattern matches the spec's
 * "select which process to run" semantics: the bandit picks the best
 * user proc when one is available; otherwise the scheduler falls
 * through to the kernel thread that yields next.
 *
 * Crucially this returns NULL at the end of a sweep so scheduler() can
 * wfi — without this the kernel busy-loops 1.7M/sec on shell-yield-
 * pick-shell-yield, starving the timer ISR's UART drain and wedging
 * input forever (observed empirically; see notes/phase5).
 */
static int v3_cursor = 0;
static int v3_picked_user_this_sweep = 0;

static struct proc *v3_pick_next(void) {
    /* Once per sweep: score user procs and pick the best. */
    if (v3_cursor == 0)
        v3_picked_user_this_sweep = 0;

    /* If we haven't yet picked the best user proc this sweep, do so. */
    if (!v3_picked_user_this_sweep) {
        struct proc *best = NULL;
        int          best_idx = -1;
        int64_t      best_score = 0;
        for (int i = 0; i < NPROC; i++) {
            struct proc *p = &proc_table[i];
            if (p->state != RUNNABLE) continue;
            if (p->init_fn != 0) continue;
            int64_t s = v3_score(p);
            if (best == NULL || s > best_score) {
                best = p;
                best_idx = i;
                best_score = s;
            }
        }
        v3_picked_user_this_sweep = 1;
        if (best != NULL) {
            best->v3_visits++;
            /* Skip the cursor past this slot so the rest-of-sweep
             * pass doesn't re-pick it. */
            if (best_idx >= v3_cursor)
                v3_cursor = best_idx + 1;
            return best;
        }
    }

    /* Rest of sweep: cursor RR over remaining RUNNABLE slots
     * (typically just the kernel threads). */
    while (v3_cursor < NPROC) {
        struct proc *p = &proc_table[v3_cursor++];
        if (p->state == RUNNABLE)
            return p;
    }

    /* Sweep done — let scheduler() wfi. */
    v3_cursor = 0;
    return NULL;
}

static void v3_on_burst_end(struct proc *p) {
    if (p->state == ZOMBIE)
        return;

    /* Reward shaping: short bursts good, voluntary yields bonus. */
    int32_t reward = -(int32_t)p->last_burst;
    if (p->state == SLEEPING)
        reward += 1;

    /* Gradient step: w_i += (reward · ctx_i) >> V3_LR_BITS.
     * Skip if proc is a kernel thread (we never picked it via the
     * bandit so the update would corrupt learning). */
    if (p->init_fn == 0) {
        int32_t ctx[V3_NUM_FEATURES];
        v3_context(p, ctx);
        for (int i = 0; i < V3_NUM_FEATURES; i++) {
            v3_weights[i] += (int32_t)(((int64_t)reward * (int64_t)ctx[i]) >> V3_LR_BITS);
        }
    }

    /* Always keep V1's burst_estimate fresh — it's a feature. Same
     * 8×-scale EWMA as V1/V2. */
    p->burst_estimate = (((uint32_t)p->last_burst << 3) + p->burst_estimate + 1) >> 1;
}

static int  v3_should_preempt(struct proc *p) { (void)p; return 1; }
static void v3_on_periodic(void)              { }

static void v3_on_proc_init(struct proc *p) {
    p->burst_estimate = 0;
    p->burst_variance = 0;
    p->proc_class = PCLASS_INTERACTIVE;
    p->v3_visits = 0;
}

static void v3_on_activate(void) {
    /* Don't reset weights on hot-swap — let the bandit accumulate
     * learning across activations. Reset visit counts to zero so the
     * UCB bonus re-fires for procs the bandit hasn't seen this
     * activation. */
    for (int i = 0; i < NPROC; i++) {
        if (proc_table[i].state != UNUSED)
            proc_table[i].v3_visits = 0;
    }
}

sched_policy_t sched_bandit = {
    .name           = "bandit",
    .pick_next      = v3_pick_next,
    .on_burst_end   = v3_on_burst_end,
    .should_preempt = v3_should_preempt,
    .on_periodic    = v3_on_periodic,
    .on_proc_init   = v3_on_proc_init,
    .on_activate    = v3_on_activate,
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
    if (sched_streq(name, "v1"))           return &sched_v1;
    if (sched_streq(name, "v2"))           return &sched_v2;
    if (sched_streq(name, "v3"))           return &sched_bandit;
    if (sched_streq(name, "bandit"))       return &sched_bandit;
    return NULL;
}
