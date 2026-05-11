/*
 * flipper.c — Phase 6 adversarial workload: I/O-bound, then CPU-bound.
 *
 * Phase A (I/O-bound): 10 short sleeps. Under V1/V2, this trains the
 * burst estimate toward zero and the classifier toward IO_BOUND. Under
 * MLFQ, the process never burns a full quantum so it stays at the top
 * priority level. Under RR, observable behavior is identical to any
 * other sleep-driven workload.
 *
 * Phase B (CPU-bound): 5 rounds of a tight arithmetic loop. Same shape
 * as cpu_bound.c. After this transition the policy must adapt — the
 * "adaptation speed" metric (tools/adaptation.py) measures how many
 * bursts elapse before each policy notices the new pattern.
 *
 * The phase boundary is trace-locatable: the last EV_SLEEP record from
 * this pid is followed by the first EV_PREEMPT record from the same
 * pid (since CPU work without sleeps gets timer-preempted instead).
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    /* Phase A: sleep loop. Each sleep(1) yields for 1 tick (~10 ms). */
    for (int round = 0; round < 10; round++) {
        puts("flipper pid=");
        puthex((unsigned long)pid);
        puts(" phaseA round=");
        puthex((unsigned long)round);
        puts("\n");
        sleep(1);
    }

    /* Marker so the trace + log clearly show the phase change. */
    puts("flipper pid=");
    puthex((unsigned long)pid);
    puts(" -- PHASE_CHANGE --\n");

    /* Phase B: CPU-bound. Same loop shape as cpu_bound.c, sized so the
     * scheduler observes several full preempts per round. */
    for (int round = 0; round < 5; round++) {
        volatile int x = 0;
        for (int i = 0; i < 10000000; i++)
            x += i;

        puts("flipper pid=");
        puthex((unsigned long)pid);
        puts(" phaseB round=");
        puthex((unsigned long)round);
        puts("\n");
    }

    exit(0);
}
