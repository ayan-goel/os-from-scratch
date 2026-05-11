#!/usr/bin/env python3
"""
cycles_table.py — Phase 6 per-decision scheduler overhead aggregation.

Reads every results/phase6/*.log, extracts the CYCLES dump captured by
the harness at the end of each run, and emits a single 5-row table:

  policy | total decisions | total cycles | mean cyc/dec | runs

The cycles dump from a single run already contains a row per policy,
but only the row for the policy that was active during that run has
non-zero counts (other policies were never invoked because the swap
was hot). So we sum the active-policy row across all runs for each
policy to get a stable per-policy overhead number.
"""

import os, re, sys
from collections import defaultdict

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASE6 = os.path.join(ROOT, "results", "phase6")

ANSI_RE   = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
CYCLES_RE = re.compile(
    r"^CYCLES\s+policy=(\w+)\s+decisions=(\d+)\s+cycles=(\d+)\s+cyc_per_dec=(\d+)"
)

POLICIES = ["rr", "mlfq", "v1", "v2", "bandit"]


def parse_cycles(path):
    """Return the most recent CYCLES dump in the log as a dict."""
    out = {}
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            stripped = ANSI_RE.sub("", raw).replace("\r", "")
            for line in stripped.split("\n"):
                m = CYCLES_RE.match(line.strip())
                if m:
                    out[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return out


def main():
    if not os.path.isdir(PHASE6):
        print(f"cycles_table: {PHASE6} not found", file=sys.stderr)
        return 1

    totals_dec   = defaultdict(int)
    totals_cyc   = defaultdict(int)
    runs_counted = defaultdict(int)

    for fname in sorted(os.listdir(PHASE6)):
        if not fname.endswith(".log"):
            continue
        # Filename shape: ${sched}_${workload}.log — the active sched
        # for this run is the part before the first underscore.
        path = os.path.join(PHASE6, fname)
        sched = fname.split("_", 1)[0]
        if sched not in POLICIES:
            continue
        dump = parse_cycles(path)
        row = dump.get(sched)
        if row is None or row[0] == 0:
            continue
        dec, cyc = row
        totals_dec[sched]   += dec
        totals_cyc[sched]   += cyc
        runs_counted[sched] += 1

    print("# Phase 6 — per-policy decision overhead (aggregated)\n")
    print("| policy | runs | total decisions | total cycles | mean cyc/dec |")
    print("|---|---|---|---|---|")
    for s in POLICIES:
        d = totals_dec[s]
        c = totals_cyc[s]
        n = runs_counted[s]
        mean = c // d if d > 0 else 0
        print(f"| {s} | {n} | {d:,} | {c:,} | {mean} |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
