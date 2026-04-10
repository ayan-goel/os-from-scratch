/*
 * init.c — the first user process.
 *
 * Forks and execs three workload programs (cpu_bound, io_bound, hello),
 * then reaps all children via wait(). This is the Phase 1 completion
 * milestone: multiple concurrent user processes, timer preemption,
 * fork/exec/wait all working together.
 */

#include "ulib.h"

static void spawn(const char *name) {
    int pid = fork();
    if (pid == 0) {
        exec(name);
        puts("init: exec failed for ");
        puts(name);
        puts("\n");
        exit(99);
    }
    puts("init: spawned ");
    puts(name);
    puts(" pid=");
    puthex((unsigned long)pid);
    puts("\n");
}

__attribute__((section(".text.boot")))
void _start(void) {
    puts("init: starting\n");

    spawn("cpu_bound");
    spawn("io_bound");
    spawn("hello");

    /* Reap all children. */
    int status;
    int w;
    while ((w = wait(&status)) > 0) {
        puts("init: reaped pid=");
        puthex((unsigned long)w);
        puts(" status=");
        puthex((unsigned long)status);
        puts("\n");
    }

    puts("init: all children done\n");
    exit(0);
}
