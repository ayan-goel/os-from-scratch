#!/usr/bin/env python3
"""
phase6_graphs.py — generate Phase 6 cross-policy comparison plots.

Reads results/phase6/*.log, aggregates per-(workload, policy) metrics,
and writes PNG bar charts to results/phase6/graphs/. Per SPEC.md
§Phase 6 every graph carries a title, X label, Y label, and legend.

One PNG per (workload, metric) pair:

  ${workload}-events.png    — total trace records (proxy for overhead)
  ${workload}-preempts.png  — workload-pid involuntary preemptions
  ${workload}-cycdec.png    — mean cycles per decision under each policy

Plus one summary chart:

  cycdec-summary.png        — cyc/dec aggregated across the matrix

Usage:
    python3 tools/phase6_graphs.py

Requires matplotlib.
"""

import os, re, sys
from collections import defaultdict

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASE6 = os.path.join(ROOT, "results", "phase6")
OUTDIR = os.path.join(PHASE6, "graphs")

ANSI_RE   = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE  = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)
CYCLES_RE = re.compile(
    r"^CYCLES\s+policy=(\w+)\s+decisions=(\d+)\s+cycles=(\d+)\s+cyc_per_dec=(\d+)"
)

POLICIES  = ["rr", "mlfq", "v1", "v2", "bandit"]
WORKLOADS = ["cpu_bound", "io_bound", "mixed", "bursty",
             "forker", "concurrent", "flipper", "wolf"]

# Colorblind-friendly palette, one color per policy. Ordering matches
# POLICIES so the legend is consistent across all charts.
COLORS = ["#888888", "#1f77b4", "#2ca02c", "#d62728", "#9467bd"]


def parse_one(path):
    records = []
    cycles  = {}
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                ln = line.strip()
                m = TRACE_RE.match(ln)
                if m:
                    records.append((int(m.group(1), 16),
                                    int(m.group(2), 16),
                                    m.group(3)))
                    continue
                c = CYCLES_RE.match(ln)
                if c:
                    cycles[c.group(1)] = int(c.group(4))
    return records, cycles


def first_user_spawn(records):
    for _, p, ev in records:
        if ev == "SPAWN" and p != 0:
            return p
    return None


def metrics_for(sched, workload):
    log = os.path.join(PHASE6, f"{sched}_{workload}.log")
    if not os.path.exists(log):
        return None
    records, cycles = parse_one(log)
    if not records:
        return None
    cyc = cycles.get(sched, None)

    if workload == "concurrent":
        # Report cpu_bound (pid 7) preempts only — the metric of interest.
        preempts = sum(1 for _, p, ev in records
                       if p == 7 and ev == "PREEMPT")
    else:
        pid = first_user_spawn(records)
        preempts = sum(1 for _, p, ev in records
                       if pid is not None and p == pid and ev == "PREEMPT")

    return {
        "events":   len(records),
        "preempts": preempts,
        "cycdec":   cyc,
    }


def bar_chart(values, title, ylabel, outpath):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("phase6_graphs: matplotlib not installed", file=sys.stderr)
        return False

    fig, ax = plt.subplots(figsize=(7, 4.5))
    xs = list(range(len(POLICIES)))
    bars = ax.bar(xs, [v if v is not None else 0 for v in values],
                  color=COLORS, edgecolor="black", linewidth=0.5)

    # Annotate each bar with its value (or "n/a").
    for i, v in enumerate(values):
        label = "n/a" if v is None else str(v)
        ax.text(i, (v if v is not None else 0),
                label, ha="center", va="bottom", fontsize=9)

    ax.set_xticks(xs)
    ax.set_xticklabels(POLICIES)
    ax.set_xlabel("scheduler policy")
    ax.set_ylabel(ylabel)
    ax.set_title(title)

    # Legend — listing each policy with its bar color makes the chart
    # readable on its own without the X axis labels.
    from matplotlib.patches import Patch
    handles = [Patch(facecolor=COLORS[i], edgecolor="black",
                     label=POLICIES[i])
               for i in range(len(POLICIES))]
    ax.legend(handles=handles, loc="upper right", fontsize=8, title="policy")
    ax.grid(axis="y", linestyle=":", alpha=0.4)

    fig.tight_layout()
    fig.savefig(outpath, dpi=130)
    import matplotlib.pyplot as plt
    plt.close(fig)
    return True


def main():
    if not os.path.isdir(PHASE6):
        print(f"phase6_graphs: {PHASE6} not found. "
              f"Run `bash tools/run_experiments.sh 6` first.", file=sys.stderr)
        return 1
    os.makedirs(OUTDIR, exist_ok=True)

    summary_cycdec = defaultdict(list)   # policy -> list of cyc/dec across workloads

    for w in WORKLOADS:
        events   = []
        preempts = []
        cycdec   = []
        for s in POLICIES:
            m = metrics_for(s, w)
            if m is None:
                events.append(None); preempts.append(None); cycdec.append(None)
            else:
                events.append(m["events"])
                preempts.append(m["preempts"])
                cycdec.append(m["cycdec"])
                if m["cycdec"] is not None:
                    summary_cycdec[s].append(m["cycdec"])

        bar_chart(events,
                  f"Phase 6 — scheduler decisions on {w}",
                  "trace records (lower = lower scheduler overhead)",
                  os.path.join(OUTDIR, f"{w}-events.png"))
        bar_chart(preempts,
                  f"Phase 6 — preempts on {w} workload pid",
                  "involuntary preemptions",
                  os.path.join(OUTDIR, f"{w}-preempts.png"))
        bar_chart(cycdec,
                  f"Phase 6 — cycles per scheduling decision ({w})",
                  "mean cycles / pick_next call",
                  os.path.join(OUTDIR, f"{w}-cycdec.png"))
        print(f"phase6_graphs: wrote 3 charts for {w}")

    # Summary cyc/dec across all workloads (mean per policy).
    means = []
    for s in POLICIES:
        vals = summary_cycdec.get(s, [])
        means.append(sum(vals) // len(vals) if vals else None)
    bar_chart(means,
              "Phase 6 — mean cycles per decision (averaged across workloads)",
              "mean cycles / pick_next call",
              os.path.join(OUTDIR, "cycdec-summary.png"))
    print("phase6_graphs: wrote cycdec-summary.png")

    print(f"phase6_graphs: all output in {OUTDIR}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
