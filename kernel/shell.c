/*
 * shell.c — kernel-mode shell for os-from-scratch.
 *
 * Runs as a kernel-function process (see proc_spawn_fn). Reads
 * characters from an RX ring buffer filled by the timer handler,
 * parses simple commands, and dispatches to C handlers. Because the
 * shell runs in M-mode, commands can touch kernel state directly —
 * no syscalls are involved.
 *
 * Output goes through the global output_ring (io.h), which the TUI
 * renderer draws into the shell panel on every frame. The shell does
 * NOT echo typed characters; the TUI renders the live input line
 * from shell_current_input() on each frame.
 */

#include "shell.h"
#include "ring.h"
#include "io.h"
#include "fs.h"
#include "proc.h"
#include "dev/clint.h"
#include "defs.h"

extern volatile uint64_t ticks;   /* from trap.c */

/* ── RX ring buffer ──────────────────────────────────────────────── */

#define RX_RING_SIZE 256
static uint8_t rx_buf[RX_RING_SIZE];
static ring_t  rx_ring;
static int     rx_ring_ready = 0;

void shell_rx_push(uint8_t byte) {
    if (!rx_ring_ready)
        return;
    ring_push(&rx_ring, byte);
}

/* ── Live input buffer (read by TUI) ─────────────────────────────── */

#define LINE_BUF 64
static char     input_buf[LINE_BUF];
static uint64_t input_len = 0;

const char *shell_current_input(void) {
    return input_buf;
}

/* ── String helpers ──────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int katoi(const char *s) {
    if (*s == '\0') return -1;
    int val = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

static void split_arg(char *arg, char **arg2) {
    char *p = arg;
    while (*p && *p != ' ') p++;
    if (*p == ' ') {
        *p++ = '\0';
        while (*p == ' ') p++;
    }
    *arg2 = p;
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

/* ── Commands ────────────────────────────────────────────────────── */

static void cmd_help(void) {
    out_puts("commands:\n");
    out_puts("  help              list commands\n");
    out_puts("  ls                list files in the ramfs\n");
    out_puts("  cat <file>        print a text file\n");
    out_puts("  ps                list all processes\n");
    out_puts("  run <name> [n]    spawn n copies of program name\n");
    out_puts("  kill <pid>        terminate process\n");
    out_puts("  stats             print scheduler statistics\n");
    out_puts("  sched <name>      hot-swap scheduler (stub)\n");
    out_puts("  quantum [ms]      show/set timer quantum (stub)\n");
    out_puts("  clear             clear the shell output panel\n");
}

static void cmd_clear(void) {
    ring_reset(&output_ring);
}

static void cmd_stats(void) {
    int ready = 0, running = 0, sleeping = 0, zombie = 0;
    for (int i = 0; i < NPROC; i++) {
        switch (proc_table[i].state) {
        case RUNNABLE: ready++;    break;
        case RUNNING:  running++;  break;
        case SLEEPING: sleeping++; break;
        case ZOMBIE:   zombie++;   break;
        default: break;
        }
    }
    out_puts("scheduler: round-robin\n");
    out_puts("  quantum:  "); out_putdec(TIMER_INTERVAL / 10000); out_puts(" ms\n");
    out_puts("  ticks:    "); out_putdec(ticks); out_putc('\n');
    out_puts("  running:  "); out_putdec((uint64_t)running);  out_putc('\n');
    out_puts("  runnable: "); out_putdec((uint64_t)ready);    out_putc('\n');
    out_puts("  sleeping: "); out_putdec((uint64_t)sleeping); out_putc('\n');
    out_puts("  zombie:   "); out_putdec((uint64_t)zombie);   out_putc('\n');
}

static void cmd_sched(const char *arg) {
    (void)arg;
    out_puts("sched: hot-swap not implemented until Phase 4 (MLFQ)\n");
}

static void cmd_quantum(const char *arg) {
    if (*arg == '\0') {
        out_puts("quantum: "); out_putdec(TIMER_INTERVAL / 10000);
        out_puts(" ms (read-only until Phase 4)\n");
        return;
    }
    out_puts("quantum: dynamic adjust not implemented until Phase 4\n");
}

static void cmd_ls(void) {
    for (uint64_t i = 0; i < NRAMFS; i++) {
        out_putc('[');
        out_putc(ramfs[i].type == FT_BINARY ? 'b' : 't');
        out_puts("] ");
        out_puts(ramfs[i].name);
        out_putc('\n');
    }
}

static void cmd_cat(const char *arg) {
    if (*arg == '\0') {
        out_puts("usage: cat <file>\n");
        return;
    }

    const inode_t *in = fs_lookup(arg);
    if (in == NULL) {
        out_puts("cat: ");
        out_puts(arg);
        out_puts(": not found\n");
        return;
    }
    if (in->type != FT_TEXT) {
        out_puts("cat: ");
        out_puts(arg);
        out_puts(": binary file\n");
        return;
    }

    /* Walk bytes (out_puts would stop at an embedded NUL). */
    for (uint64_t i = 0; i < in->size; i++)
        out_putc((char)in->data[i]);
}

static void cmd_ps(void) {
    out_puts("  PID  NAME             STATE\n");
    for (int i = 0; i < NPROC; i++) {
        proc_t *p = &proc_table[i];
        if (p->state == UNUSED)
            continue;

        int pid = p->pid;
        int digits = 1;
        {
            int n = pid;
            while (n >= 10) { n /= 10; digits++; }
        }
        for (int s = 0; s < 5 - digits; s++) out_putc(' ');
        out_putdec((uint64_t)pid);
        out_puts("  ");

        int name_len = 0;
        while (name_len < 15 && p->name[name_len]) {
            out_putc(p->name[name_len]);
            name_len++;
        }
        for (int s = name_len; s < 15; s++) out_putc(' ');
        out_puts("  ");

        out_puts(state_tag(p->state));
        out_putc('\n');
    }
}

static void cmd_run(char *arg) {
    if (*arg == '\0') {
        out_puts("usage: run <name> [n]\n");
        return;
    }

    char *count_str;
    split_arg(arg, &count_str);

    int n = 1;
    if (*count_str) {
        int parsed = katoi(count_str);
        if (parsed <= 0) {
            out_puts("run: bad count\n");
            return;
        }
        n = parsed;
    }

    const inode_t *bin = fs_lookup(arg);
    if (bin == NULL) {
        out_puts("run: ");
        out_puts(arg);
        out_puts(": not found\n");
        return;
    }
    if (bin->type != FT_BINARY) {
        out_puts("run: ");
        out_puts(arg);
        out_puts(": not a binary\n");
        return;
    }

    for (int i = 0; i < n; i++) {
        proc_t *p = proc_exec_static(bin->data, bin->size,
                                     USER_TEXT_BASE, arg);
        if (p == NULL) {
            out_puts("run: no free proc slots\n");
            return;
        }
        out_puts("spawned pid ");
        out_putdec((uint64_t)p->pid);
        out_puts(" (");
        out_puts(arg);
        out_puts(")\n");
    }
}

static void cmd_kill(const char *arg) {
    if (*arg == '\0') {
        out_puts("usage: kill <pid>\n");
        return;
    }

    int pid = katoi(arg);
    if (pid < 0) {
        out_puts("kill: bad pid\n");
        return;
    }

    proc_t *target = proc_find_by_pid(pid);
    if (target == NULL) {
        out_puts("kill: no such pid\n");
        return;
    }
    if (target == current) {
        out_puts("kill: cannot kill shell\n");
        return;
    }

    target->state = ZOMBIE;

    out_puts("killed pid ");
    out_putdec((uint64_t)pid);
    out_putc('\n');
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void shell_exec_command(char *line) {
    while (*line == ' ') line++;

    if (*line == '\0')
        return;

    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg == ' ') {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    }

    if (str_eq(line, "help"))    { cmd_help();        return; }
    if (str_eq(line, "ls"))      { cmd_ls();          return; }
    if (str_eq(line, "cat"))     { cmd_cat(arg);      return; }
    if (str_eq(line, "ps"))      { cmd_ps();          return; }
    if (str_eq(line, "run"))     { cmd_run(arg);      return; }
    if (str_eq(line, "kill"))    { cmd_kill(arg);     return; }
    if (str_eq(line, "clear"))   { cmd_clear();       return; }
    if (str_eq(line, "stats"))   { cmd_stats();       return; }
    if (str_eq(line, "sched"))   { cmd_sched(arg);    return; }
    if (str_eq(line, "quantum")) { cmd_quantum(arg);  return; }

    out_puts("unknown command: ");
    out_puts(line);
    out_puts("\n");
}

/* ── Line editor ─────────────────────────────────────────────────── */

static uint64_t shell_readline(void) {
    input_len = 0;
    input_buf[0] = '\0';

    for (;;) {
        uint8_t c;
        if (!ring_pop(&rx_ring, &c)) {
            kernel_yield();
            continue;
        }

        /* Enter: submit the line. */
        if (c == '\r' || c == '\n')
            return input_len;

        /* Backspace / DEL: shrink the buffer. */
        if (c == 0x08 || c == 0x7f) {
            if (input_len > 0) {
                input_len--;
                input_buf[input_len] = '\0';
            }
            continue;
        }

        /* Printable: append. The TUI draws the updated buffer on its
         * next frame, so the user sees their character appear then. */
        if (c >= 0x20 && c <= 0x7e && input_len < LINE_BUF - 1) {
            input_buf[input_len++] = (char)c;
            input_buf[input_len] = '\0';
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────── */

void shell_thread(void) {
    ring_init(&rx_ring, rx_buf, RX_RING_SIZE);
    rx_ring_ready = 1;

    out_puts("shell: ready. type 'help' for commands.\n");

    for (;;) {
        out_puts("$ ");
        shell_readline();
        /* Echo the submitted command into the output ring so the
         * user sees "$ help" before the response. */
        out_puts(input_buf);
        out_putc('\n');
        shell_exec_command(input_buf);
    }
}
