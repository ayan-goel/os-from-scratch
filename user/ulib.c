/*
 * ulib.c — user-mode syscall wrappers.
 *
 * Each wrapper stuffs the syscall number into a7, the arguments into
 * a0..a5, and executes `ecall`. The kernel reads these out of the trap
 * frame via syscall_dispatch in kernel/syscall.c and writes the return
 * value back into a0.
 */

#include "ulib.h"

/* ── Raw syscall helpers ─────────────────────────────────────────────────── */

static long syscall0(long num) {
    register long _num __asm__("a7") = num;
    register long _a0  __asm__("a0");
    __asm__ volatile("ecall"
                     : "=r"(_a0)
                     : "r"(_num)
                     : "memory");
    return _a0;
}

static long syscall1(long num, long a0) {
    register long _num __asm__("a7") = num;
    register long _a0  __asm__("a0") = a0;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(_num)
                     : "memory");
    return _a0;
}

static long syscall3(long num, long a0, long a1, long a2) {
    register long _num __asm__("a7") = num;
    register long _a0  __asm__("a0") = a0;
    register long _a1  __asm__("a1") = a1;
    register long _a2  __asm__("a2") = a2;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(_num), "r"(_a1), "r"(_a2)
                     : "memory");
    return _a0;
}

/* ── Wrappers ────────────────────────────────────────────────────────────── */

void exit(int status) {
    syscall1(SYS_EXIT, (long)status);
    for (;;) { }
}

long write(int fd, const void *buf, long len) {
    return syscall3(SYS_WRITE, (long)fd, (long)buf, len);
}

int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

void yield(void) {
    syscall0(SYS_YIELD);
}

void sleep(long ticks) {
    syscall1(SYS_SLEEP, ticks);
}

int fork(void) {
    return (int)syscall0(SYS_FORK);
}

int exec(const char *name) {
    return (int)syscall1(SYS_EXEC, (long)name);
}

int wait(int *status) {
    return (int)syscall1(SYS_WAIT, (long)status);
}

/* ── Convenience ─────────────────────────────────────────────────────────── */

static long strlen(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n;
}

void puts(const char *s) {
    write(1, s, strlen(s));
}

void puthex(unsigned long val) {
    char buf[19]; /* "0x" + 16 hex + NUL */
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xF;
        buf[17 - i] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    buf[18] = '\0';
    write(1, buf, 18);
}
