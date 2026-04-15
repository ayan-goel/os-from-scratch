#ifndef TUI_H
#define TUI_H

/*
 * tui.h — ANSI dashboard renderer.
 *
 * The TUI runs as a kernel-function process (see proc_spawn_fn). It
 * writes ANSI cursor-positioning escapes directly to the UART to draw
 * a 4-panel dashboard: header, process panel, scheduler panel, and
 * shell output / input panel. Reads the global output_ring for the
 * shell panel contents and shell_current_input() for the live input
 * line.
 */

void tui_thread(void);

#endif /* TUI_H */
