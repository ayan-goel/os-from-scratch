/*
 * forker.c — fork/exec churn workload.
 *
 * Forks four children in a row. Each child runs a small compute loop
 * and exits. Parent reaps them all via wait(). Exercises the proc_alloc /
 * proc_free path and pagetable clone / teardown under load.
 *
 * Expected accounting: parent has multiple bursts (one per wait wake),
 * each child has a short cpu-bound run with a handful of preemptions.
 */

#include "ulib.h"

static void child_work(int pid) {
    volatile int x = 0;
    for (int i = 0; i < 2000000; i++)
        x += i;

    puts("forker child pid=");
    puthex((unsigned long)pid);
    puts("\n");
    exit(0);
}

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();
    puts("forker parent pid=");
    puthex((unsigned long)pid);
    puts("\n");

    for (int i = 0; i < 4; i++) {
        int cpid = fork();
        if (cpid == 0)
            child_work(getpid());
    }

    int status;
    while (wait(&status) > 0) {
        /* drain all children */
    }

    exit(0);
}
