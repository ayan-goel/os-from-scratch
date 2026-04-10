/*
 * io_bound.c — I/O-bound (sleep-heavy) workload.
 *
 * Prints a line, sleeps for 5 timer ticks (~50ms at 10ms quantum),
 * repeats. Tests that sleep + wake + preemption all cooperate.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    for (int round = 0; round < 5; round++) {
        puts("io_bound pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");
        sleep(5);   /* ~50 ms */
    }

    exit(0);
}
