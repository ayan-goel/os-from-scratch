#!/usr/bin/env python3
"""
compare_policies.py — cross-policy comparison for Phase 5 + Phase 6.

Reads results/phase${PHASE}/${SCHED}_${WORKLOAD}.{log} for every
combination produced by `bash tools/run_experiments.sh ${PHASE}` and
prints a side-by-side comparison table per workload.

Usage:
  python3 tools/compare_policies.py            # phase 5 (default, back-compat)
  python3 tools/compare_policies.py --phase 6  # phase 6 incl. flipper, wolf,
                                               # and per-policy cyc/dec column

For each (workload, scheduler) cell, reports:
  events     — total trace records (proxy for scheduler overhead)
  preempts   — workload's involuntary preemptions
  sleeps     — workload's voluntary sleeps
  demotes    — MLFQ-only; non-zero indicates the demote ladder fired
  run_ticks  — sum of RUN-interval widths for the workload's primary pid
  cyc/dec    — (phase 6 only) mean cycles per scheduling decision under
               this policy, captured from the tail of the log via the
               `cycles` shell command.

For the "concurrent" workload, the report covers BOTH cpu_bound (pid 7)
and io_bound (pid 8) on separate rows.
"""

import argparse, os, re, sys
from collections import defaultdict

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POLICIES = ["rr", "mlfq", "v1", "v2", "bandit"]

ANSI_RE  = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)
CYCLES_RE = re.compile(
    r"^CYCLES\s+policy=(\w+)\s+decisions=(\d+)\s+cycles=(\d+)\s+cyc_per_dec=(\d+)"
)


def parse_log(path):
    """Return (trace_records, cycles_by_policy).

    cycles_by_policy is { policy_name: cyc_per_dec_int } populated from
    the most recent CYCLES dump in the log (typically just before the
    final QEMU exit). Empty if the log predates Phase 6.
    """
    records = []
    cycles = {}
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                stripped_line = line.strip()
                m = TRACE_RE.match(stripped_line)
                if m:
                    records.append((int(m.group(1), 16),
                                    int(m.group(2), 16),
                                    m.group(3)))
                    continue
                c = CYCLES_RE.match(stripped_line)
                if c:
                    cycles[c.group(1)] = int(c.group(4))
    return records, cycles


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


def compare_workload(workload, phase_dir, show_cycles):
    print(f"### {workload}")
    if show_cycles:
        print("| sched | events | preempts | sleeps | demotes | run_ticks | cyc/dec |")
        print("|---|---|---|---|---|---|---|")
    else:
        print("| sched | events | preempts | sleeps | demotes | run_ticks |")
        print("|---|---|---|---|---|---|")

    for s in POLICIES:
        log = os.path.join(phase_dir, f"{s}_{workload}.log")
        if not os.path.exists(log):
            cells = "- | " * (6 if show_cycles else 5)
            print(f"| {s} | {cells}")
            continue

        records, cycles_by_policy = parse_log(log)
        cyc = cycles_by_policy.get(s, None)
        cyc_str = str(cyc) if cyc is not None else "-"

        if not records:
            extra = f" | {cyc_str}" if show_cycles else ""
            print(f"| {s} | 0 | - | - | - | -{extra} |")
            continue

        if workload == "concurrent":
            cpu_counts, cpu_runt = per_pid_metrics(records, 7)
            io_counts,  io_runt  = per_pid_metrics(records, 8)
            cpu_extra = f" | {cyc_str}" if show_cycles else ""
            io_extra  = " | --"        if show_cycles else ""
            print(f"| {s} (cpu) | {len(records)} | {cpu_counts['PREEMPT']} | "
                  f"{cpu_counts['SLEEP']} | {cpu_counts['DEMOTE']} | {cpu_runt}"
                  f"{cpu_extra} |")
            print(f"| {s} (io)  | -- | {io_counts['PREEMPT']} | "
                  f"{io_counts['SLEEP']} | {io_counts['DEMOTE']} | {io_runt}"
                  f"{io_extra} |")
        else:
            pid = next((p for _, p, ev in records
                        if ev == "SPAWN" and p != 0), None)
            if pid is None:
                extra = f" | {cyc_str}" if show_cycles else ""
                print(f"| {s} | {len(records)} | - | - | - | -{extra} |")
                continue
            counts, runt = per_pid_metrics(records, pid)
            extra = f" | {cyc_str}" if show_cycles else ""
            print(f"| {s} | {len(records)} | {counts['PREEMPT']} | "
                  f"{counts['SLEEP']} | {counts['DEMOTE']} | {runt}{extra} |")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", type=int, default=5,
                    help="phase to compare (5 or 6; default 5)")
    args = ap.parse_args()

    phase_dir = os.path.join(ROOT, "results", f"phase{args.phase}")
    if not os.path.isdir(phase_dir):
        print(f"compare_policies: {phase_dir} not found. "
              f"Run `bash tools/run_experiments.sh {args.phase}` first.",
              file=sys.stderr)
        return 1

    workloads = ["cpu_bound", "io_bound", "mixed", "bursty", "forker", "concurrent"]
    if args.phase >= 6:
        workloads += ["flipper", "wolf"]

    show_cycles = args.phase >= 6
    for w in workloads:
        compare_workload(w, phase_dir, show_cycles)
    return 0


if __name__ == "__main__":
    sys.exit(main())
