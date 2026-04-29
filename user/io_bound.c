/*
 * io_bound.c — I/O-bound (sleep-heavy) workload.
 *
 * Prints a line, sleeps, repeats. Rounds and sleep length picked so
 * the process sits in SLEEPING for seconds of wall time — enough for
 * the scheduler to accumulate dozens of wake/sleep decisions. See
 * SPEC.md §Phase 3.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    for (int round = 0; round < 30; round++) {
        puts("io_bound pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");
        sleep(10);   /* ~100 ms per round × 30 rounds = ~3 s wall time */
    }

    exit(0);
}
