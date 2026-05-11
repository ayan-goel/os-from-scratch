#!/usr/bin/env python3
"""
phase7_compare.py — Phase 7 V2 before/after comparison.

Loads V2's row from results/phase6/ (Phase 6, pre-fix) and
results/phase7/ (post-fix) for each of the 8 workloads. Emits a
markdown table with per-workload deltas and a regression-check
summary at the bottom.

SPEC.md §Phase 7 acceptance: V2 events on cpu_bound / io_bound /
mixed / bursty / forker / concurrent / wolf must not regress by
more than 10%. Adaptation speed on flipper is the headline win.

Run after `bash tools/run_experiments.sh 7` completes.
"""

import os, re, sys

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASE6 = os.path.join(ROOT, "results", "phase6")
PHASE7 = os.path.join(ROOT, "results", "phase7")

ANSI_RE   = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE  = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)
CYCLES_RE = re.compile(
    r"^CYCLES\s+policy=(\w+)\s+decisions=(\d+)\s+cycles=(\d+)\s+cyc_per_dec=(\d+)"
)

WORKLOADS = ["cpu_bound", "io_bound", "mixed", "bursty",
             "forker", "concurrent", "flipper", "wolf"]

# SPEC acceptance: V2 events must not regress by more than 10% on
# anything except flipper (which is the targeted improvement). Per
# Phase 6 §Honest losses, wolf is included in the "must not regress"
# list — V2's already-poor wolf result must not get worse.
REGRESSION_THRESHOLD = 1.10


def parse_log(path):
    """Return (events, preempts_workload, cyc_per_dec, burst_lengths_flipper).

    burst_lengths_flipper is only computed for flipper logs; it's the
    sequence of post-phase-change burst lengths (used to compute
    bursts-to-adapt).
    """
    records = []
    cyc_per_dec = None
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
                if c and c.group(1) == "v2":
                    cyc_per_dec = int(c.group(4))
    return records, cyc_per_dec


def workload_pid(records):
    for _, p, ev in records:
        if ev == "SPAWN" and p != 0:
            return p
    return None


def preempt_count(records, pid):
    if pid is None:
        return 0
    return sum(1 for _, p, ev in records if p == pid and ev == "PREEMPT")


def adaptation_metrics(records):
    """Bursts-to-adapt for flipper. Returns (bursts_to_long, first_long_length).

    Bursts-to-adapt = 1-indexed position of the first post-phase-change
    burst whose length is >= 5 ticks. None if never reached.
    """
    pid = workload_pid(records)
    if pid is None:
        return None, None
    flip = [(t, ev) for (t, p, ev) in records if p == pid]
    # Locate first PREEMPT after at least one SLEEP — the phase change.
    saw_sleep = False
    change_idx = None
    for i, (_, ev) in enumerate(flip):
        if ev == "SLEEP":
            saw_sleep = True
        elif ev == "PREEMPT" and saw_sleep:
            change_idx = i
            break
    if change_idx is None:
        return None, None
    bursts = []
    open_t = None
    for t, ev in flip[change_idx:]:
        if ev == "RUN":
            open_t = t
        elif ev in ("PREEMPT", "SLEEP", "EXIT", "YIELD"):
            if open_t is not None:
                bursts.append(t - open_t)
                open_t = None
    for i, b in enumerate(bursts):
        if b >= 5:
            return i + 1, b
    return None, None


def metrics_for(phase_dir, workload):
    """Return dict of metrics for V2 on this workload in this phase, or None."""
    log = os.path.join(phase_dir, f"v2_{workload}.log")
    if not os.path.exists(log):
        return None
    records, cyc = parse_log(log)
    pid = workload_pid(records)
    if workload == "concurrent":
        pid_cpu = 7  # cpu_bound spawns first under concurrent
    else:
        pid_cpu = pid

    out = {
        "events":   len(records),
        "preempts": preempt_count(records, pid_cpu),
        "cycdec":   cyc,
    }
    if workload == "flipper":
        bta, flb = adaptation_metrics(records)
        out["bursts_to_long"]   = bta
        out["first_long_burst"] = flb
    return out


def fmt_delta(p6_val, p7_val):
    if p6_val is None or p7_val is None:
        return "—"
    delta = p7_val - p6_val
    sign = "+" if delta > 0 else ("" if delta == 0 else "")
    return f"{sign}{delta}"


def fmt_pct(p6_val, p7_val):
    if p6_val is None or p7_val is None or p6_val == 0:
        return "—"
    pct = 100.0 * (p7_val - p6_val) / p6_val
    return f"{pct:+.1f}%"


def main():
    if not os.path.isdir(PHASE6):
        print(f"phase7_compare: {PHASE6} not found", file=sys.stderr)
        return 1
    if not os.path.isdir(PHASE7):
        print(f"phase7_compare: {PHASE7} not found. "
              f"Run `bash tools/run_experiments.sh 7` first.", file=sys.stderr)
        return 1

    print("# Phase 7 — V2 before/after\n")
    print(f"V2 numbers from `results/phase6/` (pre-fix) and `results/phase7/` "
          f"(windowed-classifier fix). Regression threshold: {REGRESSION_THRESHOLD:.0%}.\n")
    print("| workload | P6 events | P7 events | Δ% | P6 preempts | P7 preempts | "
          "P6 cyc/dec | P7 cyc/dec |")
    print("|---|---|---|---|---|---|---|---|")

    rows = []
    regressions = []
    for w in WORKLOADS:
        p6 = metrics_for(PHASE6, w) or {}
        p7 = metrics_for(PHASE7, w) or {}
        e6 = p6.get("events")
        e7 = p7.get("events")
        p6p = p6.get("preempts")
        p7p = p7.get("preempts")
        c6  = p6.get("cycdec")
        c7  = p7.get("cycdec")
        rows.append((w, p6, p7))

        # Regression check — applies to all workloads except flipper
        # (flipper is the targeted improvement; lower events expected).
        if w != "flipper" and e6 is not None and e7 is not None and e6 > 0:
            if e7 > e6 * REGRESSION_THRESHOLD:
                regressions.append((w, e6, e7))

        print(f"| {w} | {e6 if e6 is not None else '—'} | "
              f"{e7 if e7 is not None else '—'} | "
              f"{fmt_pct(e6, e7)} | "
              f"{p6p if p6p is not None else '—'} | "
              f"{p7p if p7p is not None else '—'} | "
              f"{c6 if c6 is not None else '—'} | "
              f"{c7 if c7 is not None else '—'} |")

    # Adaptation row.
    print()
    print("## Adaptation speed (flipper)")
    print()
    print("| metric | Phase 6 | Phase 7 |")
    print("|---|---|---|")
    p6f = metrics_for(PHASE6, "flipper") or {}
    p7f = metrics_for(PHASE7, "flipper") or {}
    bta6 = p6f.get("bursts_to_long")
    bta7 = p7f.get("bursts_to_long")
    flb6 = p6f.get("first_long_burst")
    flb7 = p7f.get("first_long_burst")
    print(f"| bursts to adapt | {bta6 if bta6 is not None else 'did not adapt'} "
          f"| {bta7 if bta7 is not None else 'did not adapt'} |")
    print(f"| first long burst (ticks) | {flb6 if flb6 is not None else '—'} "
          f"| {flb7 if flb7 is not None else '—'} |")

    # Regression summary.
    print()
    print("## Regression check")
    print()
    if not regressions:
        print(f"No workload regressed by more than "
              f"{(REGRESSION_THRESHOLD-1)*100:.0f}% — acceptance met.")
    else:
        print(f"Regressions found (>{(REGRESSION_THRESHOLD-1)*100:.0f}% increase "
              f"in events):")
        for w, e6, e7 in regressions:
            print(f"- **{w}**: {e6} → {e7} (+{(e7-e6)/e6*100:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
