#!/usr/bin/env python3
"""
compare_policies.py — Phase 5 cross-policy comparison.

Reads results/phase5/${SCHED}_${WORKLOAD}.{log,summary.csv} for every
combination produced by `bash tools/run_experiments.sh 5` and prints a
side-by-side comparison table per workload.

For each (workload, scheduler) cell, reports:
  events     — total trace records (proxy for scheduler overhead)
  preempts   — workload's involuntary preemptions
  sleeps     — workload's voluntary sleeps
  demotes    — MLFQ-only; non-zero indicates the demote ladder fired
  run_ticks  — sum of RUN-interval widths for the workload's primary pid

For the "concurrent" workload, the report covers BOTH cpu_bound (pid 7)
and io_bound (pid 8) on separate rows.
"""

import csv, os, re, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASE5 = os.path.join(ROOT, "results", "phase5")

POLICIES  = ["rr", "mlfq", "v1", "v2", "bandit"]
WORKLOADS = ["cpu_bound", "io_bound", "mixed", "bursty", "forker", "concurrent"]

ANSI_RE  = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)


def parse_log(path):
    records = []
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                m = TRACE_RE.match(line.strip())
                if m:
                    records.append((int(m.group(1), 16),
                                    int(m.group(2), 16),
                                    m.group(3)))
    return records


def per_pid_metrics(records, pid):
    """For the given pid, count events and sum RUN-interval widths."""
    counts = defaultdict(int)
    run_ticks = 0
    open_run = None
    for t, p, ev in records:
        if p != pid:
            continue
        counts[ev] += 1
        if ev == "RUN":
            open_run = t
        elif ev in ("PREEMPT", "SLEEP", "EXIT", "YIELD") and open_run is not None:
            run_ticks += t - open_run
            open_run = None
    return counts, run_ticks


def compare_workload(workload):
    """Print a per-policy table for one workload."""
    print(f"### {workload}")
    print("| sched | events | preempts | sleeps | demotes | run_ticks |")
    print("|---|---|---|---|---|---|")
    for s in POLICIES:
        log = os.path.join(PHASE5, f"{s}_{workload}.log")
        if not os.path.exists(log):
            print(f"| {s} | - | - | - | - | - |")
            continue
        records = parse_log(log)
        if not records:
            print(f"| {s} | 0 | - | - | - | - |")
            continue

        if workload == "concurrent":
            # Show cpu_bound + io_bound rows.
            cpu_counts, cpu_runt = per_pid_metrics(records, 7)
            io_counts, io_runt   = per_pid_metrics(records, 8)
            print(f"| {s} (cpu) | {len(records)} | {cpu_counts['PREEMPT']} | "
                  f"{cpu_counts['SLEEP']} | {cpu_counts['DEMOTE']} | {cpu_runt} |")
            print(f"| {s} (io)  | -- | {io_counts['PREEMPT']} | "
                  f"{io_counts['SLEEP']} | {io_counts['DEMOTE']} | {io_runt} |")
        else:
            # Single-workload: identify the workload pid via the SPAWN
            # event. After `trace clear`, the only SPAWN should be the
            # workload itself (or the first child for forker).
            pid = next((p for _, p, ev in records
                        if ev == "SPAWN" and p != 0), None)
            if pid is None:
                print(f"| {s} | {len(records)} | - | - | - | - |")
                continue
            counts, runt = per_pid_metrics(records, pid)
            print(f"| {s} | {len(records)} | {counts['PREEMPT']} | "
                  f"{counts['SLEEP']} | {counts['DEMOTE']} | {runt} |")
    print()


def main():
    if not os.path.isdir(PHASE5):
        print(f"compare_policies: {PHASE5} not found. "
              f"Run `bash tools/run_experiments.sh 5` first.", file=sys.stderr)
        return 1
    for w in WORKLOADS:
        compare_workload(w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
