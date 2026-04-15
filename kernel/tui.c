/*
 * tui.c — ANSI terminal dashboard.
 *
 * Layout (80 cols x 24 rows):
 *
 *   row  1   header: "os-from-scratch  sched:round-robin  uptime:MM:SS"
 *   row  2   top border ================================================
 *   rows 3-12  process panel (cols 1-39) | scheduler panel (cols 41-80)
 *   row 13   middle border --------------
 *   rows 14-22 shell output panel (9 lines, tail of output_ring)
 *   row 23   bottom border --------------
 *   row 24   input line: "$ " + shell_current_input()
 *
 * Rendering strategy: draw the static borders once at init time, then
 * on every frame overwrite just the dynamic regions using
 * cursor-positioning escapes. No full-screen clears — this is what
 * gives the non-flickering effect.
 *
 * All output goes DIRECT to the UART (bypassing the output ring)
 * because the TUI IS the terminal output. Process stdout and shell
 * command output land in the ring; the TUI pulls them out and renders
 * them into the shell panel.
 */

#include "tui.h"
#include "io.h"
#include "shell.h"
#include "ring.h"
#include "proc.h"
#include "dev/uart.h"
#include "dev/clint.h"
#include "defs.h"

extern volatile uint64_t ticks;

/* ── ANSI escape constants ───────────────────────────────────────── */

#define ANSI_CLEAR       "\033[2J"
#define ANSI_HOME        "\033[H"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"

static void tui_write(const char *s) {
    /* emit raw bytes until the NUL */
    uint64_t n = 0;
    while (s[n]) n++;
    uart_write_raw(s, n);
}

/*
 * ansi_goto(row, col) — 1-based cursor positioning.
 * Emits "\033[<row>;<col>H".
 */
static void ansi_goto(int row, int col) {
    char buf[16];
    int i = 0;
    buf[i++] = '\033';
    buf[i++] = '[';

    /* row */
    if (row >= 10) buf[i++] = (char)('0' + row / 10);
    buf[i++] = (char)('0' + row % 10);

    buf[i++] = ';';

    /* col */
    if (col >= 10) buf[i++] = (char)('0' + col / 10);
    buf[i++] = (char)('0' + col % 10);

    buf[i++] = 'H';
    uart_write_raw(buf, (uint64_t)i);
}

/* Write a fixed-width decimal field, right-padded with spaces. */
static void write_int_field(uint64_t v, int width) {
    char buf[24];
    int i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        char tmp[24];
        int j = 0;
        while (v > 0 && j < 24) { tmp[j++] = (char)('0' + v % 10); v /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    }
    while (i < width) buf[i++] = ' ';
    uart_write_raw(buf, (uint64_t)i);
}

/* Write a left-aligned string padded to `width` with spaces. */
static void write_str_field(const char *s, int width) {
    int i = 0;
    while (i < width && s[i]) {
        uart_write_raw(&s[i], 1);
        i++;
    }
    char space = ' ';
    while (i < width) {
        uart_write_raw(&space, 1);
        i++;
    }
}

static const char *state_tag(proc_state_t s) {
    switch (s) {
    case UNUSED:   return "---";
    case EMBRYO:   return "EMB";
    case RUNNABLE: return "RDY";
    case RUNNING:  return "RUN";
    case SLEEPING: return "SLP";
    case ZOMBIE:   return "ZMB";
    default:       return "???";
    }
}

/* Fill a region with N spaces to clear it before drawing. */
static void clear_to(int count) {
    static const char spaces[] = "                                                                                ";
    /* 80 spaces above; if count > 80 we split. */
    while (count > 80) {
        uart_write_raw(spaces, 80);
        count -= 80;
    }
    if (count > 0)
        uart_write_raw(spaces, (uint64_t)count);
}

/* ── Static frame ────────────────────────────────────────────────── */

static void tui_draw_static_frame(void) {
    tui_write(ANSI_CLEAR);
    tui_write(ANSI_HIDE_CURSOR);
    tui_write(ANSI_HOME);

    /* Row 2: top border */
    ansi_goto(2, 1);
    static const char eq80[] =
        "================================================================================";
    uart_write_raw(eq80, 80);

    /* Rows 3..12: column divider at col 40 */
    for (int r = 3; r <= 12; r++) {
        ansi_goto(r, 40);
        uart_write_raw("|", 1);
    }

    /* Row 13: middle border */
    ansi_goto(13, 1);
    static const char dash80[] =
        "--------------------------------------------------------------------------------";
    uart_write_raw(dash80, 80);

    /* Row 23: bottom border */
    ansi_goto(23, 1);
    uart_write_raw(dash80, 80);
}

/* ── Dynamic regions ─────────────────────────────────────────────── */

static void tui_draw_header(void) {
    ansi_goto(1, 1);

    /* " os-from-scratch  sched:round-robin  uptime:MM:SS" */
    tui_write(" os-from-scratch");
    write_str_field("", 2);  /* spacer */

    tui_write("sched:round-robin");
    write_str_field("", 2);

    tui_write("uptime:");
    uint64_t total_s = ticks / 100;  /* 100 ticks/sec */
    uint64_t mm = total_s / 60;
    uint64_t ss = total_s % 60;

    char tbuf[6];
    tbuf[0] = (char)('0' + (mm / 10) % 10);
    tbuf[1] = (char)('0' + mm % 10);
    tbuf[2] = ':';
    tbuf[3] = (char)('0' + ss / 10);
    tbuf[4] = (char)('0' + ss % 10);
    tbuf[5] = ' ';
    uart_write_raw(tbuf, 6);

    /* Clear the rest of the line. Rough estimate of used columns:
     * 1 + 16 + 2 + 17 + 2 + 7 + 5 + 1 = 51. Pad to 80. */
    clear_to(80 - 51);
}

static void tui_draw_procs(void) {
    /* Rows 3-12, cols 1-39. Row 3 is the header row. */
    ansi_goto(3, 1);
    tui_write("  PID  NAME            ST ");
    clear_to(39 - 26);  /* pad to col 39 */

    int row = 4;
    for (int i = 0; i < NPROC && row <= 12; i++) {
        proc_t *p = &proc_table[i];
        if (p->state == UNUSED)
            continue;

        ansi_goto(row, 1);

        /* " " + 4-wide pid + "  " + 14-wide name + "  " + 3-wide state + pad to 39 */
        uart_write_raw(" ", 1);
        write_int_field((uint64_t)p->pid, 4);
        uart_write_raw("  ", 2);

        /* name (max 14 chars) */
        char nbuf[15];
        int nl = 0;
        while (nl < 14 && p->name[nl]) { nbuf[nl] = p->name[nl]; nl++; }
        nbuf[nl] = '\0';
        write_str_field(nbuf, 14);
        uart_write_raw("  ", 2);

        write_str_field(state_tag(p->state), 3);

        /* Pad to col 39. 1+4+2+14+2+3 = 26. 39-26 = 13. */
        clear_to(13);
        row++;
    }

    /* Blank the remaining proc rows (stale entries from larger frames). */
    while (row <= 12) {
        ansi_goto(row, 1);
        clear_to(39);
        row++;
    }
}

static void tui_draw_sched(void) {
    /* Rows 3-12, cols 41-80. Width = 40. */
    ansi_goto(3, 41);
    tui_write(" SCHEDULER");
    clear_to(40 - 10);

    ansi_goto(4, 41);
    tui_write("   algorithm : round-robin");
    clear_to(40 - 26);

    ansi_goto(5, 41);
    tui_write("   quantum   : 10 ms");
    clear_to(40 - 20);

    ansi_goto(6, 41);
    tui_write("   ticks     : ");
    write_int_field(ticks, 10);
    clear_to(40 - 25);

    /* Count RUNNABLE + RUNNING processes. */
    int ready = 0;
    for (int i = 0; i < NPROC; i++) {
        proc_state_t s = proc_table[i].state;
        if (s == RUNNABLE || s == RUNNING)
            ready++;
    }

    ansi_goto(7, 41);
    tui_write("   runnable  : ");
    write_int_field((uint64_t)ready, 10);
    clear_to(40 - 25);

    /* Blank rows 8-12. */
    for (int r = 8; r <= 12; r++) {
        ansi_goto(r, 41);
        clear_to(40);
    }
}

/* Render the output panel — rows 14-22. Reads the tail of output_ring
 * and splits on newlines into up to 9 lines. */
static void tui_draw_output(void) {
    /* Read the last 720 bytes (80 cols × 9 rows) from the ring. */
    static uint8_t scratch[720];
    uint64_t n = ring_read_tail(&output_ring, scratch, sizeof(scratch));

    /* Walk bytes to find the last 9 newline-separated lines. */
    const int ROWS = 9;
    const int COLS = 80;
    const char *lines[ROWS];
    int lens[ROWS];
    for (int i = 0; i < ROWS; i++) { lines[i] = ""; lens[i] = 0; }

    /* Scan forward, splitting on \n. Overwrite older lines as we go. */
    int cur_row = 0;
    const char *cur = (const char *)scratch;
    int cur_len = 0;

    for (uint64_t i = 0; i < n; i++) {
        char c = (char)scratch[i];
        if (c == '\r') continue;
        if (c == '\n') {
            lines[cur_row] = cur;
            lens[cur_row] = cur_len;
            cur_row = (cur_row + 1) % ROWS;
            cur = (const char *)&scratch[i + 1];
            cur_len = 0;
        } else {
            cur_len++;
        }
    }
    /* Final (possibly unterminated) line. */
    if (cur_len > 0) {
        lines[cur_row] = cur;
        lens[cur_row] = cur_len;
        cur_row = (cur_row + 1) % ROWS;
    }

    /* cur_row now points at the OLDEST kept line. Draw in order. */
    for (int r = 0; r < ROWS; r++) {
        int idx = (cur_row + r) % ROWS;
        ansi_goto(14 + r, 1);

        int write_len = lens[idx];
        if (write_len > COLS) write_len = COLS;
        if (write_len > 0)
            uart_write_raw(lines[idx], (uint64_t)write_len);
        clear_to(COLS - write_len);
    }
}

static void tui_draw_input(void) {
    ansi_goto(24, 1);
    uart_write_raw("$ ", 2);

    const char *s = shell_current_input();
    int len = 0;
    while (s[len] && len < 76) len++;
    if (len > 0)
        uart_write_raw(s, (uint64_t)len);

    /* Erase to end of line. */
    clear_to(78 - len);
}

/* ── Entry point ─────────────────────────────────────────────────── */

void tui_thread(void) {
    /* Short delay so the kernel boot messages that were emitted via
     * direct uart_puts can scroll off before we clear the screen. */
    kernel_sleep(20);

    tui_draw_static_frame();

    for (;;) {
        tui_draw_header();
        tui_draw_procs();
        tui_draw_sched();
        tui_draw_output();
        tui_draw_input();

        /* Park cursor at the end of the input line so it blinks there. */
        ansi_goto(24, 3 + (int)(shell_current_input()[0] ? 0 : 0));
        /* (Actual cursor position updated implicitly by draw_input.) */

        kernel_sleep(5);   /* 50 ms → 20 fps */
    }
}
