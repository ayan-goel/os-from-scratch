#ifndef SHELL_H
#define SHELL_H

#include "defs.h"

/*
 * shell.h — kernel-mode shell.
 *
 * The shell is a kernel-function process (spawned via proc_spawn_fn)
 * that reads characters from the UART via an RX ring buffer and
 * dispatches commands to C handlers. Because it runs in M-mode with
 * direct access to the kernel, commands can manipulate proc_table,
 * the ramfs, and the scheduler directly — no syscalls involved.
 *
 * See notes/phase2-shell-fs-tui.md for design decisions.
 */

/*
 * shell_thread — kernel-function entry point for the shell process.
 * Passed to proc_spawn_fn in kmain.
 */
void shell_thread(void);

/*
 * shell_rx_push — called from the timer interrupt handler every tick
 * to deliver a newly-read UART byte into the RX ring buffer. The
 * shell thread pops bytes from the same ring in shell_readline.
 *
 * Safe to call from trap context (interrupts already disabled).
 */
void shell_rx_push(uint8_t byte);

/*
 * shell_current_input — used by the TUI renderer to draw the live
 * input line. Returns a pointer to a NUL-terminated buffer holding
 * whatever the user has typed so far (or an empty string). The
 * buffer is owned by the shell thread; callers must only read.
 */
const char *shell_current_input(void);

#endif /* SHELL_H */
