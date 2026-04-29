#!/usr/bin/env python3
"""
parse_trace.py — turn a captured `make qemu` log into a CSV and a
Gantt-style graph of scheduler activity.

The kernel's `trace` shell command writes records straight to the UART
in this exact format, one per line:

    TRACE tick=0x00001234 pid=0x05 ev=RUN

Those lines are interleaved with TUI ANSI escape sequences in the raw
QEMU log, so the parser strips ANSI first, then matches anchored lines.

Outputs (by default, next to the input file):
    <stem>.csv                one row per trace record
    <stem>-gantt.png          horizontal-bar chart, one row per pid
    <stem>-summary.csv        (if --workload-summary) per-pid totals

Usage:
    python3 tools/parse_trace.py trace.log
    python3 tools/parse_trace.py trace.log --workload-summary
    python3 tools/parse_trace.py trace.log -o out/           # write to out/

Requires: matplotlib. Pandas is NOT required.
"""

import argparse
import csv
import os
import re
import sys

ANSI_RE  = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)\s*$"
)

EVENT_NAMES = {"SPAWN", "EXIT", "RUN", "YIELD", "PREEMPT", "SLEEP", "WAKE"}


def parse_log(path):
    """Return list of (tick, pid, event) tuples in file order."""
    records = []
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                m = TRACE_RE.match(line.strip())
                if not m:
                    continue
                tick = int(m.group(1), 16)
                pid  = int(m.group(2), 16)
                ev   = m.group(3)
                if ev not in EVENT_NAMES:
                    continue
                records.append((tick, pid, ev))
    return records


def write_csv(records, path):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["tick", "pid", "event"])
        for tick, pid, ev in records:
            w.writerow([tick, pid, ev])


def build_intervals(records):
    """
    Fold the record stream into (pid → list[(start_tick, end_tick)])
    intervals during which that pid was RUNNING. A RUN opens an
    interval; the next non-RUN event for that pid (YIELD, PREEMPT,
    SLEEP, EXIT) closes it. Repeated RUNs without a closer (shouldn't
    happen in a correct trace) also close the prior one.
    """
    open_run = {}     # pid -> start_tick of its current RUN interval
    intervals = {}    # pid -> list of (start, end)

    for tick, pid, ev in records:
        if ev == "RUN":
            if pid in open_run:
                intervals.setdefault(pid, []).append((open_run[pid], tick))
            open_run[pid] = tick
        elif ev in ("YIELD", "PREEMPT", "SLEEP", "EXIT"):
            if pid in open_run:
                intervals.setdefault(pid, []).append((open_run[pid], tick))
                del open_run[pid]
        # SPAWN and WAKE don't open or close intervals.

    # Close any intervals still open at the tail of the trace with
    # zero width — so they render as a hairline at the final tick.
    if records:
        last_tick = records[-1][0]
        for pid, start in open_run.items():
            intervals.setdefault(pid, []).append((start, last_tick))

    return intervals


def plot_gantt(records, intervals, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("parse_trace: matplotlib not installed, skipping Gantt plot",
              file=sys.stderr)
        return False

    pids = sorted(intervals.keys())
    if not pids:
        print("parse_trace: no RUN intervals to plot", file=sys.stderr)
        return False

    fig, ax = plt.subplots(figsize=(12, max(2.0, 0.6 * len(pids) + 1.0)))

    for i, pid in enumerate(pids):
        bars = [(s, e - s if e > s else 0.5) for s, e in intervals[pid]]
        ax.broken_barh(bars, (i - 0.4, 0.8),
                       facecolors="tab:blue", edgecolors="none")

    ax.set_yticks(range(len(pids)))
    ax.set_yticklabels([f"pid {p}" for p in pids])
    ax.set_xlabel("tick (10 ms each)")
    ax.set_title("Scheduler activity — Gantt of RUN intervals")
    ax.grid(axis="x", linestyle=":", alpha=0.5)

    # Event markers: YIELD / PREEMPT / SLEEP on the relevant row.
    marker_styles = {
        "YIELD":   ("v", "tab:green"),
        "PREEMPT": ("x", "tab:red"),
        "SLEEP":   ("s", "tab:orange"),
    }
    row_of = {pid: i for i, pid in enumerate(pids)}
    for tick, pid, ev in records:
        if ev in marker_styles and pid in row_of:
            marker, color = marker_styles[ev]
            ax.plot(tick, row_of[pid], marker=marker,
                    color=color, markersize=5, linestyle="none")

    # Legend.
    from matplotlib.lines import Line2D
    legend = [
        Line2D([0], [0], color="tab:blue", lw=6, label="RUNNING"),
        Line2D([0], [0], marker="v", color="tab:green",
               linestyle="none", label="YIELD"),
        Line2D([0], [0], marker="x", color="tab:red",
               linestyle="none", label="PREEMPT"),
        Line2D([0], [0], marker="s", color="tab:orange",
               linestyle="none", label="SLEEP"),
    ]
    ax.legend(handles=legend, loc="upper right", fontsize=8)

    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)
    return True


def workload_summary(records, intervals):
    """
    Per-pid totals. run_ticks is summed interval width (proxy for CPU
    consumed); counts are simple event frequencies.
    """
    summary = {}
    for pid, ivs in intervals.items():
        summary.setdefault(pid, {"run_ticks": 0, "yields": 0,
                                 "preempts": 0, "sleeps": 0, "spawns": 0,
                                 "exits": 0, "wakes": 0})
        summary[pid]["run_ticks"] = sum(e - s for s, e in ivs)

    for _tick, pid, ev in records:
        summary.setdefault(pid, {"run_ticks": 0, "yields": 0,
                                 "preempts": 0, "sleeps": 0, "spawns": 0,
                                 "exits": 0, "wakes": 0})
        key = {"YIELD": "yields", "PREEMPT": "preempts", "SLEEP": "sleeps",
               "SPAWN": "spawns", "EXIT": "exits", "WAKE": "wakes"}.get(ev)
        if key:
            summary[pid][key] += 1

    return summary


def write_summary_csv(summary, path):
    cols = ["pid", "run_ticks", "yields", "preempts", "sleeps",
            "spawns", "exits", "wakes"]
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for pid in sorted(summary):
            row = summary[pid]
            w.writerow([pid] + [row[k] for k in cols[1:]])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="QEMU/kernel log file containing TRACE lines")
    ap.add_argument("-o", "--out-dir", default=None,
                    help="output directory (default: alongside log)")
    ap.add_argument("--workload-summary", action="store_true",
                    help="also emit per-pid totals as <stem>-summary.csv")
    args = ap.parse_args()

    if not os.path.isfile(args.log):
        print(f"parse_trace: {args.log}: not found", file=sys.stderr)
        return 1

    out_dir = args.out_dir or os.path.dirname(os.path.abspath(args.log))
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(args.log))[0]
    csv_path     = os.path.join(out_dir, f"{stem}.csv")
    gantt_path   = os.path.join(out_dir, f"{stem}-gantt.png")
    summary_path = os.path.join(out_dir, f"{stem}-summary.csv")

    records = parse_log(args.log)
    if not records:
        print(f"parse_trace: no TRACE records found in {args.log}",
              file=sys.stderr)
        return 2

    write_csv(records, csv_path)
    print(f"parse_trace: {len(records)} records -> {csv_path}")

    intervals = build_intervals(records)
    ok = plot_gantt(records, intervals, gantt_path)
    if ok:
        print(f"parse_trace: gantt -> {gantt_path}")

    if args.workload_summary:
        summary = workload_summary(records, intervals)
        write_summary_csv(summary, summary_path)
        print(f"parse_trace: summary -> {summary_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
