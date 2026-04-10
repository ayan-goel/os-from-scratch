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

    for (int round = 0; round < 5; round++) {
        /* Busy work — burn CPU cycles without yielding. */
        volatile int x = 0;
        for (int i = 0; i < 100000; i++)
            x += i;

        puts("cpu_bound pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");
    }

    exit(0);
}
