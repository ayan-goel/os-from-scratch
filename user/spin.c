/*
 * spin.c — infinite busy loop.
 *
 * Never exits voluntarily; the shell's `kill <pid>` marks it ZOMBIE
 * and the scheduler reaps it. Used as a long-running CPU hog for
 * Phase 3 measurement runs and for stress-testing preemption.
 *
 * No output: printing would dominate the workload. The point is to
 * saturate one scheduler slot until killed.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    volatile unsigned long x = 0;
    for (;;) {
        x++;
    }
}
