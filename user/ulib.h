#ifndef ULIB_H
#define ULIB_H

/*
 * ulib.h — user-mode syscall wrappers.
 *
 * Syscall numbers must match kernel/syscall.h exactly. Keep them in sync.
 * User programs use only this header; no kernel headers are accessible.
 *
 * We use `long` for sizes/returns because lp64d guarantees long = 64 bits,
 * and we can't include <stdint.h>.
 */

#define SYS_EXIT    1
#define SYS_WRITE   2
#define SYS_GETPID  3
#define SYS_YIELD   4
#define SYS_SLEEP   5
#define SYS_FORK    6
#define SYS_EXEC    7
#define SYS_WAIT    8

void exit(int status) __attribute__((noreturn));
long write(int fd, const void *buf, long len);
int  getpid(void);
void yield(void);
void sleep(long ticks);
int  fork(void);
int  exec(const char *name);
int  wait(int *status);

/* Convenience: write a NUL-terminated string to stdout. */
void puts(const char *s);

/* Convenience: write a 64-bit hex value to stdout. */
void puthex(unsigned long val);

#endif /* ULIB_H */
