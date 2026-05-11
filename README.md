# os-from-scratch

> *Most OS schedulers use fixed heuristics (round-robin quantum, MLFQ priority decay) that were tuned decades ago. Can a scheduler that models per-process behavior online make better decisions on modern workloads? I built a kernel from scratch to find out.*

A RISC-V 64-bit kernel written in C from bare metal. No libc, no malloc, no FP, no Linux underneath. It boots in QEMU, runs user processes from a flat in-memory filesystem, and renders a live `htop`-style dashboard over the UART. The point of the kernel isn't the kernel. It's the platform it gives me to compare scheduling algorithms (round-robin, MLFQ, and three online-learning policies) on the same hardware with the same workloads, hot-swappable from a shell prompt.

## Headline result

The strongest learning policy, an online behavioral classifier with windowed input counters, **beats MLFQ on every workload in the matrix** and matches its phase-change adaptation speed of 3 bursts on the `flipper` adversarial workload. A more ambitious contextual-bandit scheduler loses on cpu-heavy workloads, because UCB exploration costs eat the test budget. The full numbers, figures, and discussion are in [writeup/main.pdf](writeup/main.pdf).

## What's in it

- **Bare-metal RISC-V kernel.** Boots in M-mode on QEMU's `virt` machine, configures Sv39 paging, drops to user mode through a hand-written trap vector. ~3500 lines of C and a bit of assembly.
- **Real processes.** 64-slot process table, per-process page tables, `fork`/`exec`/`wait`/`yield`/`sleep` syscalls, kernel and user threads, timer-driven preemption.
- **Five hot-swappable schedulers:**
  - `rr` — cursor-sweep round-robin.
  - `mlfq` — 4-level demote ladder with 1/2/4/8-tick allotments and a periodic boost.
  - `v1` — exponentially-weighted burst predictor (approximate SRTF).
  - `v2` — online classifier into INTERACTIVE / IO_BOUND / CPU_BOUND / BATCH with per-class quantum. **Strongest policy in the matrix.**
  - `bandit` (V3) — contextual bandit with 8-feature linear value function and UCB exploration.
- **Interactive shell.** `ls`, `cat`, `ps`, `run`, `kill`, `stats`, `trace`, `cycles`, `clear`, plus the live knobs: `quantum <ms>` to retune the timer, `sched <name>` to swap the scheduler in place.
- **Live TUI.** ANSI-rendered dashboard at 20 fps showing the process table, scheduler stats, the burst-estimate bar (V1+), the CLASS column (V2+), the WEIGHTS row (V3), and a shell input line, all over a single UART.
- **A measurement harness.** Every scheduling event lands in a 16K-entry ring buffer. `rdcycle`-based instrumentation reports per-decision overhead. A Python parser turns trace dumps into CSVs and matplotlib bar charts. `tools/run_experiments.sh 6` runs the full 5-policy × 8-workload matrix headlessly into `results/phase6/`.

## What it looks like

```
 os-from-scratch  sched:v2  uptime:00:14
================================================================================
  PID  NAME            ST       CLASS  |  SCHEDULER
    2  shell           RDY      BAT    |    algorithm : v2
    3  tui             RUN      BAT    |    quantum   :   10 ms
    7  cpu_bound       RDY      CPU    |    ticks     :       1432
    8  io_bound        SLP      IO     |    decisions :    9871234
                                       |    ctx sw/s  :    1542893
--------------------------------------------------------------------------------
init: spawned cpu_bound pid=0x7
io_bound pid=0x8 round=0x4
init: reaped pid=0x6 status=0x0
--------------------------------------------------------------------------------
$ sched bandit
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
sched v2            # hot-swap to the windowed-classifier scheduler
quantum 1           # drop the timer quantum to 1 ms
trace 200           # dump the last 200 scheduling events
cycles              # per-policy mean cycles per pick_next call
ps                  # current process table
```

Exit QEMU with **Ctrl-A X**.

## Workloads

Eight synthetic user programs are compiled into the kernel image:

- `cpu_bound`: tight arithmetic loop, no voluntary yields.
- `io_bound`: repeated `sleep(10)` calls.
- `mixed`: alternating short compute and short sleep.
- `bursty`: long idle then sudden compute.
- `forker`: fork/exec churn.
- `concurrent`: `cpu_bound` and `io_bound` running together.
- `flipper` *(adversarial)*: 10 short sleeps then 5 rounds of compute. Forces a mid-run I/O to CPU phase change. Exposed V2's classifier-stickiness bug; the fix is in Phase 7.
- `wolf` *(adversarial)*: 100 single-tick yields then a long compute burst. The classical MLFQ priority-gaming exploit.

Each workload is sized to make at least 100 scheduling decisions at a 10 ms quantum. Below that, per-decision overhead variance drowns out the policy signal.

## Reproducing the results

```sh
make
bash tools/run_experiments.sh 6       # Phase 6 baseline matrix (~14 min)
bash tools/run_experiments.sh 7       # Phase 7 matrix with the V2 fix
python3 tools/compare_policies.py --phase 6
python3 tools/phase7_compare.py       # V2 before/after table
python3 tools/adaptation.py           # flipper adaptation speeds
python3 tools/phase6_graphs.py        # all 25 comparison PNGs
```

Outputs land in `results/phase6/` and `results/phase7/` (both gitignored).

## Repository layout

```
kernel/    bare-metal C + RISC-V assembly
user/      synthetic workload programs (8, listed above)
tools/     parse_trace.py, run_experiments.sh, compare_policies.py, ...
notes/     per-phase narratives (phase0-setup through phase7-improvements)
writeup/   LaTeX project (main.tex, references.bib, figures/)
linker.ld  kernel link script
Makefile
```

## Why this exists

I wanted a self-contained system small enough to reason about end to end, with enough surface area to compare scheduling policies meaningfully. Every constraint is on purpose: no allocation in the scheduler hot path, scheduler state in tens of bytes per process, every claim about a policy verifiable from a kernel boot you just did. The kernel is the experiment. The schedulers are what I'm actually after. The full discussion of what worked, what didn't, and what I'd do differently lives in [writeup/main.pdf](writeup/main.pdf).
