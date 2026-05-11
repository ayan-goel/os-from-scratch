/*
 * wolf.c — Phase 6 adversarial workload: classical MLFQ gaming.
 *
 * Phase A (priority gaming): yield repeatedly via sleep(1). Each yield
 * is a voluntary deschedule that never burns a full quantum, so MLFQ
 * (which only demotes on quantum exhaustion) leaves wolf at the top
 * priority level. Wolf accumulates `voluntary_yields` heavily, which
 * V2 should catch via the classifier (high yield_ratio → INTERACTIVE
 * → 1-tick quantum) — the SPEC's "at least one workload where a
 * learning scheduler wins" story.
 *
 * Phase B (burst): a long tight loop. Under MLFQ, wolf is still at
 * the top queue and will get a generous quantum before being demoted
 * — that is the MLFQ vulnerability being demonstrated. Under V2,
 * wolf was classified INTERACTIVE on the basis of yield ratio so the
 * burst gets a 1-tick quantum and is preempted aggressively.
 *
 * The phase boundary is the last SLEEP record from this pid; the
 * first subsequent PREEMPT (or YIELD-with-state-RUNNING transition)
 * is the start of Phase B.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    /* Phase A: yield 100 times. sleep(1) returns after the next tick,
     * so this never burns a full quantum even on RR. */
    for (int round = 0; round < 100; round++) {
        if ((round & 0xf) == 0) {
            puts("wolf pid=");
            puthex((unsigned long)pid);
            puts(" phaseA yields=");
            puthex((unsigned long)round);
            puts("\n");
        }
        sleep(1);
    }

    /* Marker so the trace + log clearly show the phase change. */
    puts("wolf pid=");
    puthex((unsigned long)pid);
    puts(" -- PHASE_CHANGE --\n");

    /* Phase B: long burst. Sized larger than cpu_bound to expose the
     * "wolf holds top queue under MLFQ" failure mode visibly. */
    for (int round = 0; round < 5; round++) {
        volatile int x = 0;
        for (int i = 0; i < 10000000; i++)
            x += i;

        puts("wolf pid=");
        puthex((unsigned long)pid);
        puts(" phaseB round=");
        puthex((unsigned long)round);
        puts("\n");
    }

    exit(0);
}
