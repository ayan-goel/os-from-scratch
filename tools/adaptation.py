#!/usr/bin/env python3
"""
adaptation.py — Phase 6 adaptation-speed metric for flipper.

For each policy, locate the I/O→CPU phase change in flipper's trace
and measure how many post-change bursts elapse before the policy
gives flipper a burst >= its "long" allotment. The intuition:

  - RR gives a constant 1-tick quantum, so it never adapts. Report N/A.
  - MLFQ demotes flipper from level 0 (allot=1) down to level 3
    (allot=8) over several preempts. The adaptation speed is the
    number of post-change bursts until the first burst with length >= 8.
  - V1 picks smallest-tau; flipper's tau rises with each cpu-bound
    burst until it stops being the cheapest candidate.
  - V2 reclassifies on each burst end; if it moves flipper from
    IO_BOUND to CPU_BOUND (5-tick quantum), bursts grow to ≥5.
  - V3 (bandit) updates weights per burst; convergence varies.

The phase change marker is the first PREEMPT event for flipper's pid
following any SLEEP event for the same pid (Phase A ends with the
last sleep; Phase B's first preempt is the first tick of cpu work).

The "long burst" threshold is 5 ticks — the smallest non-trivial
allotment in any policy (V2's CPU_BOUND allotment). A burst that
reaches 5 ticks means the policy has stopped preempting every tick.
"""

import os, re, sys
from collections import defaultdict

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASE6 = os.path.join(ROOT, "results", "phase6")

ANSI_RE  = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
TRACE_RE = re.compile(
    r"^TRACE\s+tick=0x([0-9a-fA-F]+)\s+pid=0x([0-9a-fA-F]+)\s+ev=(\w+)"
)

POLICIES        = ["rr", "mlfq", "v1", "v2", "bandit"]
LONG_BURST_TICKS = 5


def parse_trace(path):
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


def find_flipper_pid(records):
    """flipper's pid = first SPAWN after `trace clear`."""
    for _, p, ev in records:
        if ev == "SPAWN" and p != 0:
            return p
    return None


def analyze_one(path):
    records = parse_trace(path)
    pid = find_flipper_pid(records)
    if pid is None:
        return None

    # Extract events for this pid only, in chronological order.
    flipper = [(t, ev) for (t, p, ev) in records if p == pid]
    if not flipper:
        return None

    # Phase change: first PREEMPT after at least one SLEEP.
    saw_sleep = False
    change_idx = None
    for i, (_, ev) in enumerate(flipper):
        if ev == "SLEEP":
            saw_sleep = True
        elif ev == "PREEMPT" and saw_sleep:
            change_idx = i
            break

    if change_idx is None:
        return {"phase_change": False}

    # Walk RUN → (PREEMPT|SLEEP|EXIT|YIELD) pairs after change_idx
    # to extract burst lengths. burst_lengths[k] is the length of
    # the k-th burst that started AT OR AFTER the phase change.
    bursts = []
    open_run_tick = None
    for t, ev in flipper[change_idx:]:
        if ev == "RUN":
            open_run_tick = t
        elif ev in ("PREEMPT", "SLEEP", "EXIT", "YIELD"):
            if open_run_tick is not None:
                bursts.append(t - open_run_tick)
                open_run_tick = None

    # bursts_to_long: index of first burst with length >= LONG_BURST_TICKS,
    # or None if never reached.
    bursts_to_long = None
    for i, b in enumerate(bursts):
        if b >= LONG_BURST_TICKS:
            bursts_to_long = i + 1   # 1-indexed for human readability
            break

    return {
        "phase_change": True,
        "post_change_bursts": len(bursts),
        "burst_lengths": bursts,
        "bursts_to_long": bursts_to_long,
        "first_long_length": (bursts[bursts_to_long - 1]
                              if bursts_to_long is not None else None),
    }


def main():
    if not os.path.isdir(PHASE6):
        print(f"adaptation: {PHASE6} not found", file=sys.stderr)
        return 1

    print("# Phase 6 — adaptation speed (flipper workload)")
    print()
    print(f"For each policy, the number of post-phase-change bursts before "
          f"flipper receives a burst ≥ {LONG_BURST_TICKS} ticks.")
    print()
    print("| policy | post-change bursts | bursts to adapt | first long burst | "
          "notes |")
    print("|---|---|---|---|---|")

    for s in POLICIES:
        log = os.path.join(PHASE6, f"{s}_flipper.log")
        if not os.path.exists(log):
            print(f"| {s} | - | - | - | log missing |")
            continue

        res = analyze_one(log)
        if res is None:
            print(f"| {s} | - | - | - | no flipper pid in trace |")
            continue
        if not res["phase_change"]:
            print(f"| {s} | 0 | - | - | phase change not observed |")
            continue

        n   = res["post_change_bursts"]
        bta = res["bursts_to_long"]
        flb = res["first_long_length"]
        if s == "rr":
            note = "fixed 1-tick quantum — no adaptation"
        elif bta is None:
            note = f"did not adapt within {n} bursts"
        else:
            note = "adapted"

        bta_str = str(bta) if bta is not None else "—"
        flb_str = str(flb) if flb is not None else "—"
        print(f"| {s} | {n} | {bta_str} | {flb_str} | {note} |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
