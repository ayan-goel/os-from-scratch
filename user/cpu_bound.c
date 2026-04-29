/*
 * cpu_bound.c — CPU-intensive workload.
 *
 * Tight arithmetic loop with no voluntary yields. Relies entirely on
 * timer preemption for scheduling. Prints progress every round.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    /* Inner count bumped ~100x vs Phase 1/2 so a single instance burns
     * multiple seconds of CPU time — enough for ≥500 scheduler decisions
     * at a 10 ms quantum. See SPEC.md §Phase 3. */
    for (int round = 0; round < 5; round++) {
        volatile int x = 0;
        for (int i = 0; i < 10000000; i++)
            x += i;

        puts("cpu_bound pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");
    }

    exit(0);
}
