/*
 * bursty.c — long idle, then sudden compute. Repeats.
 *
 * Six cycles. Each cycle: sleep(30) (~300 ms of idle) followed by
 * a longer compute burst (~5M iterations, dozens of ticks). Models
 * an interactive process that sits idle most of the time but occasionally
 * does real work — a harder pattern to schedule fairly than mixed.
 *
 * Expected accounting: moderate cpu_ticks (bursts-dominated), sleep_calls == 6,
 * per-burst length mostly set by the quantum.
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    for (int round = 0; round < 6; round++) {
        sleep(30);

        volatile int x = 0;
        for (int i = 0; i < 5000000; i++)
            x += i;

        puts("bursty pid=");
        puthex((unsigned long)pid);
        puts(" round=");
        puthex((unsigned long)round);
        puts("\n");
    }

    exit(0);
}
