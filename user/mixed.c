/*
 * mixed.c — alternating compute / sleep workload.
 *
 * Ten rounds. Each round: a short compute burst (~1M iterations,
 * a few ticks of CPU time at most) followed by sleep(5). Exercises
 * both preemption and sleep/wake paths in a single process.
 *
 * Expected accounting: nonzero cpu_ticks, sleep_calls == 10,
 * involuntary_preempts likely in single digits.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    for (int round = 0; round < 10; round++) {
        volatile int x = 0;
        for (int i = 0; i < 1000000; i++)
            x += i;

        puts("mixed pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");

        sleep(5);
    }

    exit(0);
}
