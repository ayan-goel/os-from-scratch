/*
 * hello.c — second user program. Tests write, getpid, and yield.
 *
 * Prints "hello from pid N", yields 3 times (printing each time),
 * then exits with status 0.
 *
 * Expected QEMU output when two copies run concurrently:
 *   hello from pid 1, round 0
 *   hello from pid 2, round 0
 *   hello from pid 1, round 1
 *   hello from pid 2, round 1
 *   ... etc
 */

#include "ulib.h"

__attribute__((section(".text.boot")))
void _start(void) {
    int pid = getpid();

    for (int i = 0; i < 3; i++) {
        puts("hello from pid ");
        puthex((unsigned long)pid);
        puts(", round ");
        puthex((unsigned long)i);
        puts("\n");
        yield();
    }

    exit(0);
}
