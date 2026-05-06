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
#include "sched.h"
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

    /* " os-from-scratch  sched:<name>  uptime:MM:SS" — the policy
     * name comes from active_sched live, so `sched mlfq` flips the
     * header on the next frame. */
    tui_write(" os-from-scratch");
    write_str_field("", 2);  /* spacer */

    tui_write("sched:");
    tui_write(active_sched->name);
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

/*
 * Burst-estimate bar (Phase 5 T3): under V1/V2/V3 the rightmost
 * column of each proc row visualizes burst_estimate as 0..8 filled
 * '#' chars (1 char per tick, saturating). Under RR/MLFQ the column
 * is blank — same layout, no flicker, no policy-specific row width.
 */
static int policy_shows_burst_bar(void) {
    return active_sched == &sched_v1
        || active_sched == &sched_v2
        || active_sched == &sched_bandit;
}

/*
 * CLASS column (Phase 5 T4): under V2/V3 each proc row shows a 3-char
 * label for proc_class (INT/IO /CPU/BAT). V1 has no class concept,
 * RR/MLFQ leave it blank. Kernel threads are pinned to BAT at pick
 * time; mirroring that here keeps the panel and the picker in sync.
 */
static int policy_shows_class(void) {
    return active_sched == &sched_v2 || active_sched == &sched_bandit;
}

static const char *class_tag(proc_t *p) {
    if (p->init_fn != 0)
        return "BAT";
    switch (p->proc_class) {
    case PCLASS_INTERACTIVE: return "INT";
    case PCLASS_IO_BOUND:    return "IO ";
    case PCLASS_CPU_BOUND:   return "CPU";
    case PCLASS_BATCH:       return "BAT";
    default:                 return "???";
    }
}

static void draw_burst_bar(uint32_t estimate) {
    /* burst_estimate is stored at 8× scale (see proc.h V1 comment).
     * Map (estimate >> 3) ticks → 0..8 filled chars, saturating. */
    int filled = (int)(estimate >> 3);
    if (filled > 8) filled = 8;
    if (filled < 0) filled = 0;
    char buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = (i < filled) ? '#' : ' ';
    uart_write_raw(buf, 8);
}

static void tui_draw_procs(void) {
    /*
     * Layout (39 cols total):
     *   1  pad
     *   4  pid
     *   2  pad
     *  10  name (was 14; shrunk to fit CLASS column)
     *   1  pad
     *   3  state
     *   1  pad
     *   3  CLASS  ("INT"/"IO "/"CPU"/"BAT" under V2/V3; "   " otherwise)
     *   1  pad
     *   8  burst bar (under V1/V2/V3; "        " otherwise)
     *   5  pad
     */
    int show_bar   = policy_shows_burst_bar();
    int show_class = policy_shows_class();

    ansi_goto(3, 1);
    /* Header. */
    uart_write_raw("  PID  NAME       ST ", 21);
    if (show_class) uart_write_raw("CLS ", 4); else uart_write_raw("    ", 4);
    if (show_bar)   uart_write_raw("TAU      ", 9);
    else            uart_write_raw("         ", 9);
    /* 21+4+9 = 34. Pad to 39. */
    clear_to(5);

    int row = 4;
    for (int i = 0; i < NPROC && row <= 12; i++) {
        proc_t *p = &proc_table[i];
        if (p->state == UNUSED)
            continue;

        ansi_goto(row, 1);

        uart_write_raw(" ", 1);
        write_int_field((uint64_t)p->pid, 4);
        uart_write_raw("  ", 2);

        /* name (max 10 chars) */
        char nbuf[11];
        int nl = 0;
        while (nl < 10 && p->name[nl]) { nbuf[nl] = p->name[nl]; nl++; }
        nbuf[nl] = '\0';
        write_str_field(nbuf, 10);
        uart_write_raw(" ", 1);

        write_str_field(state_tag(p->state), 3);
        uart_write_raw(" ", 1);

        if (show_class) {
            uart_write_raw(class_tag(p), 3);
        } else {
            uart_write_raw("   ", 3);
        }
        uart_write_raw(" ", 1);

        if (show_bar)
            draw_burst_bar(p->burst_estimate);
        else
            uart_write_raw("        ", 8);

        /* 1+4+2+10+1+3+1+3+1+8 = 34. Pad to 39 = 5. */
        clear_to(5);
        row++;
    }

    /* Blank the remaining proc rows (stale entries from larger frames). */
    while (row <= 12) {
        ansi_goto(row, 1);
        clear_to(39);
        row++;
    }
}

/*
 * Rolling 1-second window for ctx-switch rate.
 *
 * Sampled once per TUI frame (~50 ms at 20 fps). The buffer holds 20
 * slots (≈ 1 second). Each frame, we record (decisions, ticks) at the
 * write head and compute the rate as (newest - oldest) decisions over
 * (newest - oldest) ticks, scaled to per-second (100 ticks/sec).
 *
 * Until the buffer fills with real samples, the head and tail refer to
 * the same boot-time zero — rate stays 0 for the first second of life.
 */
#define SCHED_WINDOW 20
static uint64_t win_dec  [SCHED_WINDOW];
static uint64_t win_ticks[SCHED_WINDOW];
static int      win_head = 0;
static int      win_filled = 0;

static void tui_draw_sched(void) {
    /* Sample the rolling window. */
    win_dec  [win_head] = sched_total_decisions;
    win_ticks[win_head] = ticks;
    int oldest = (win_head + 1) % SCHED_WINDOW;
    if (!win_filled && win_head == SCHED_WINDOW - 1)
        win_filled = 1;
    int tail = win_filled ? oldest : 0;

    uint64_t dec_delta  = win_dec  [win_head] - win_dec  [tail];
    uint64_t tick_delta = win_ticks[win_head] - win_ticks[tail];
    uint64_t cps = (tick_delta > 0) ? (dec_delta * 100 / tick_delta) : 0;
    win_head = (win_head + 1) % SCHED_WINDOW;

    /* Rows 3-12, cols 41-80. Width = 40. */
    ansi_goto(3, 41);
    tui_write(" SCHEDULER");
    clear_to(40 - 10);

    ansi_goto(4, 41);
    tui_write("   algorithm : ");
    const char *name = active_sched->name;
    int name_chars = 0;
    while (name[name_chars]) name_chars++;
    uart_write_raw(name, (uint64_t)name_chars);
    /* Clear to width 40. Used so far: 15 (label) + name_chars. */
    clear_to(40 - 15 - name_chars);

    ansi_goto(5, 41);
    tui_write("   quantum   : ");
    write_int_field(timer_interval / 10000, 4);
    tui_write(" ms");
    clear_to(40 - 24);

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

    ansi_goto(8, 41);
    tui_write("   decisions : ");
    write_int_field(sched_total_decisions, 10);
    clear_to(40 - 25);

    ansi_goto(9, 41);
    tui_write("   ctx sw/s  : ");
    write_int_field(cps, 10);
    clear_to(40 - 25);

    /* Row 10: V3 WEIGHTS row (Phase 5 T5) — top-weighted feature
     * index, label, and current value. Visible only under sched_bandit;
     * blank otherwise. Format:  "   weights : f5 class     -1234" */
    ansi_goto(10, 41);
    if (active_sched == &sched_bandit) {
        int top = v3_top_feature();
        int32_t w = v3_weight(top);
        tui_write("   weights : f");
        write_int_field((uint64_t)top, 1);
        uart_write_raw(" ", 1);
        uart_write_raw(v3_feature_name(top), 9);
        uart_write_raw(" ", 1);
        if (w < 0) {
            uart_write_raw("-", 1);
            write_int_field((uint64_t)(-w), 7);
        } else {
            uart_write_raw(" ", 1);
            write_int_field((uint64_t)w, 7);
        }
        /* 14 + 1 + 1 + 9 + 1 + 1 + 7 = 34. Pad to 40 = 6. */
        clear_to(6);
    } else {
        clear_to(40);
    }

    /* Blank rows 11-12. */
    for (int r = 11; r <= 12; r++) {
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
