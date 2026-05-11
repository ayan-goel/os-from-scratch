#!/usr/bin/env python3
"""
phase7_beforeafter_graph.py — generate the Phase 7 before/after chart
used in the writeup. Single PNG: side-by-side bars of V2 events on
flipper before and after the windowed-classifier fix, plus the
adaptation-speed annotation on top.

Reads results/phase6/v2_flipper.log and results/phase7/v2_flipper.log;
writes writeup/figures/phase7-flipper-beforeafter.png.
"""

import os, re, sys

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P6_LOG = os.path.join(ROOT, "results", "phase6", "v2_flipper.log")
P7_LOG = os.path.join(ROOT, "results", "phase7", "v2_flipper.log")
OUT    = os.path.join(ROOT, "writeup", "figures",
                      "phase7-flipper-beforeafter.png")

ANSI_RE  = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)


def parse(path):
    recs = []
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                m = TRACE_RE.match(line.strip())
                if m:
                    recs.append((int(m.group(1), 16),
                                 int(m.group(2), 16),
                                 m.group(3)))
    return recs


def metrics(path):
    recs = parse(path)
    pid = next((p for _, p, ev in recs if ev == "SPAWN" and p != 0), None)
    if pid is None:
        return 0, 0, None
    events = len(recs)
    preempts = sum(1 for _, p, ev in recs if p == pid and ev == "PREEMPT")
    flip = [(t, ev) for t, p, ev in recs if p == pid]
    saw_sleep = False
    change = None
    for i, (_, ev) in enumerate(flip):
        if ev == "SLEEP":
            saw_sleep = True
        elif ev == "PREEMPT" and saw_sleep:
            change = i
            break
    bta = None
    if change is not None:
        bursts = []
        open_t = None
        for t, ev in flip[change:]:
            if ev == "RUN":
                open_t = t
            elif ev in ("PREEMPT", "SLEEP", "EXIT", "YIELD"):
                if open_t is not None:
                    bursts.append(t - open_t)
                    open_t = None
        for i, b in enumerate(bursts):
            if b >= 5:
                bta = i + 1
                break
    return events, preempts, bta


def main():
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed", file=sys.stderr)
        return 1

    p6e, p6p, p6b = metrics(P6_LOG)
    p7e, p7p, p7b = metrics(P7_LOG)

    fig, ax = plt.subplots(figsize=(7.5, 4.8))
    x = [0, 1]
    bars = ax.bar(x, [p6e, p7e],
                  color=["#d62728", "#2ca02c"],
                  edgecolor="black", linewidth=0.5,
                  width=0.5)

    # Value labels on top of each bar.
    for i, v in enumerate([p6e, p7e]):
        ax.text(i, v + 1, str(v), ha="center", fontsize=11, fontweight="bold")

    # Adaptation note inside each bar, near the bottom.
    p6_note = ("did not adapt\n(13 post-change bursts)"
               if p6b is None else f"adapted in {p6b} bursts")
    p7_note = ("did not adapt"
               if p7b is None else f"adapted in {p7b} bursts")
    ax.text(0, max(p6e, p7e) * 0.04, p6_note,
            ha="center", fontsize=10, color="white", fontweight="bold")
    ax.text(1, max(p6e, p7e) * 0.04, p7_note,
            ha="center", fontsize=10, color="white", fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(["Phase 6 V2\n(lifetime counters)",
                        "Phase 7 V2\n(windowed classifier)"])
    ax.set_ylabel("trace records on flipper workload")
    ax.set_title("V2 on flipper: lifetime counters vs windowed classifier")
    ax.set_ylim(0, max(p6e, p7e) * 1.25)
    ax.grid(axis="y", linestyle=":", alpha=0.4)

    from matplotlib.patches import Patch
    legend = [
        Patch(facecolor="#d62728", edgecolor="black", label="before fix (Phase 6)"),
        Patch(facecolor="#2ca02c", edgecolor="black", label="after fix  (Phase 7)"),
    ]
    ax.legend(handles=legend, loc="upper right", fontsize=9)

    fig.tight_layout()
    fig.savefig(OUT, dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
