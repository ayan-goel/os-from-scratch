# os-from-scratch

> *Most OS schedulers use fixed heuristics (round-robin quantum, MLFQ priority decay) that were tuned decades ago. Can a scheduler that models per-process behavior online make better decisions on modern workloads? I built a kernel from scratch to find out.*

A RISC-V 64-bit kernel written in C from bare metal, no libc, no malloc, no FP, no Linux underneath. It boots in QEMU, runs user processes from a flat in-memory filesystem, and renders a live `htop`-style dashboard over the UART. The point of the kernel isn't the kernel itself; it's the platform it gives me to compare scheduling algorithms, round-robin, MLFQ, and three online-learning policies, on the same hardware, with the same workloads, hot-swappable from a shell prompt.

## What's in it

- **Bare-metal RISC-V kernel.** Boots in M-mode on QEMU's `virt` machine, configures Sv39 paging, drops to user mode through a hand-written trap vector. ~3500 lines of C and a bit of assembly.
- **Real processes.** 64-slot process table, per-process page tables, fork/exec/wait/yield/sleep syscalls, kernel and user threads, timer-driven preemption.
- **Interactive shell.** `ls`, `cat`, `ps`, `run`, `kill`, `stats`, `trace`, `clear`, plus the live knobs: `quantum <ms>` to retune the timer, `sched <name>` to swap the scheduler in place.
- **Live TUI.** ANSI-rendered dashboard at 20 fps showing the process table, scheduler stats (decisions, ctx switches/sec, current policy), shell output panel, and an input line — all over a single UART.
- **Hot-swappable schedulers.** Each policy is a `sched_policy_t` function-pointer table. Currently round-robin and MLFQ; three learning schedulers are next.
- **A measurement harness.** Every scheduling event lands in a 16K-entry ring buffer; `trace` dumps it as ASCII; a Python parser turns the dump into CSV and matplotlib Gantt charts. A shell-script orchestrator runs the full (scheduler × quantum × workload) matrix headlessly.

## What it looks like

```
 os-from-scratch  sched:mlfq  uptime:00:14
================================================================================
  PID  NAME            ST       |  SCHEDULER
    2  shell           RDY      |    algorithm : mlfq
    3  tui             RUN      |    quantum   :   10 ms
    7  cpu_bound       RDY      |    ticks     :       1432
    8  io_bound        SLP      |    runnable  :          2
                                |    decisions :    9871234
                                |    ctx sw/s  :    1542893
--------------------------------------------------------------------------------
init: spawned cpu_bound pid=0x7
hello from pid 0x6, round 0x2
io_bound pid=0x8 round=0x4
init: reaped pid=0x6 status=0x0
--------------------------------------------------------------------------------
$ run cpu_bound 2
```

## Try it

```sh
brew install riscv64-elf-gcc riscv64-elf-binutils qemu
make qemu
```

Wait a few seconds for boot to settle, then at the `$` prompt:

```sh
ls                  # list embedded files
run cpu_bound 3     # spawn three CPU-bound workers
sched mlfq          # hot-swap the scheduler — header bar updates live
quantum 1           # drop the timer quantum to 1 ms
trace 200           # dump the last 200 scheduling events
ps                  # current process table
```

Exit QEMU with **Ctrl-A X**.

## Why this exists

I wanted a self-contained system small enough to reason about end to end, with enough surface area to compare scheduling policies meaningfully. Every constraint is intentional: no allocation in the scheduler hot path, scheduler state in tens of bytes per process, every claim about a policy verifiable from a kernel boot you just did. The kernel is the experiment — the schedulers are what I'm actually after.

## Repository layout

```
kernel/    bare-metal C + RISC-V assembly
user/      synthetic workload programs (cpu_bound, io_bound, mixed, bursty, forker, spin)
tools/     parse_trace.py, run_experiments.sh
linker.ld  kernel link script
Makefile
```
