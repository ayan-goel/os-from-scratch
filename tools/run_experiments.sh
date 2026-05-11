#!/usr/bin/env bash
# run_experiments.sh — Phase 3/4/5 scheduler run matrix.
#
# Phase 3 (round-robin only):   3 quanta × 5 workloads          = 15 runs.
# Phase 4 (RR + MLFQ):           2 sched × 3 quanta × 5 workloads = 30 runs.
# Phase 5 (RR + MLFQ + V1/V2/V3): 5 sched × 6 workloads at q=10  = 30 runs.
#                                  workloads include "concurrent" =
#                                  cpu_bound + io_bound running together.
# Phase 6 (eval only — same 5 scheds): 5 sched × 8 workloads at q=10 = 40 runs.
#                                  workloads add "flipper" (I/O→CPU phase
#                                  change) and "wolf" (yield-spam then long
#                                  burst — MLFQ gaming attack). Per SPEC.md
#                                  §Phase 6 no scheduler bodies change; the
#                                  matrix exists only for evaluation. Each
#                                  log captures the final `cycles` dump for
#                                  per-decision overhead extraction.
#
# For every (sched, quantum, workload), boot the kernel, wait for
# init's boot-time children to finish, set the quantum, switch to
# the requested scheduler, clear the trace ring, run the workload,
# wait a fixed runtime, dump the trace, and exit. parse_trace.py
# turns each log into CSV + Gantt + per-pid summary.
#
# Output layout:
#   results/phase3/q${Q}_${W}.{raw,log,csv,gantt.png,summary.csv}
#   results/phase4/${SCHED}_q${Q}_${W}.{...}
#   results/phase5/${SCHED}_${W}.{...}    (single quantum)
#
# Phase 3/4 outputs are kept untouched so prior notes still resolve.
#
# Requires: qemu-system-riscv64, perl (for the alarm wrapper used in
# place of `timeout` on macOS), python3 + matplotlib for the parser.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${ROOT}/kernel.elf"
PARSER="${ROOT}/tools/parse_trace.py"

PHASE="${PHASE:-${1:-3}}"
case "${PHASE}" in
    3)  OUT="${ROOT}/results/phase3"; SCHEDS=(rr)                    ; QUANTA=(1 10 100); WORKLOADS=(cpu_bound io_bound mixed bursty forker) ;;
    4)  OUT="${ROOT}/results/phase4"; SCHEDS=(rr mlfq)               ; QUANTA=(1 10 100); WORKLOADS=(cpu_bound io_bound mixed bursty forker) ;;
    5)  OUT="${ROOT}/results/phase5"; SCHEDS=(rr mlfq v1 v2 bandit)  ; QUANTA=(10)      ; WORKLOADS=(cpu_bound io_bound mixed bursty forker concurrent) ;;
    6)  OUT="${ROOT}/results/phase6"; SCHEDS=(rr mlfq v1 v2 bandit)  ; QUANTA=(10)      ; WORKLOADS=(cpu_bound io_bound mixed bursty forker concurrent flipper wolf) ;;
    7)  OUT="${ROOT}/results/phase7"; SCHEDS=(rr mlfq v1 v2 bandit)  ; QUANTA=(10)      ; WORKLOADS=(cpu_bound io_bound mixed bursty forker concurrent flipper wolf) ;;
    *)  echo "run_experiments: unknown phase '${PHASE}' (try 3, 4, 5, 6, or 7)" >&2; exit 1 ;;
esac

mkdir -p "${OUT}"

if [[ ! -f "${KERNEL}" ]]; then
    echo "run_experiments: ${KERNEL} not built. Run \`make\` first." >&2
    exit 1
fi

# Per-run timing (seconds wall-clock).
BOOT_WAIT=8        # let init's auto-spawn (cpu_bound + io_bound + hello) finish
RUN_TIME=6         # workload runtime budget after `run` is issued
TAIL_WAIT=2        # let final trace events flush before exit
# Phase 6 flipper/wolf need extra time: Phase A (10 sleeps × 1 tick = ~100 ms)
# plus Phase B (5 rounds of tight compute @ ~1.5 s each = ~7.5 s). 12 s budget
# leaves enough headroom for the phase change to be visible in the trace.
RUN_TIME_LONG=12
TOTAL=$(( BOOT_WAIT + RUN_TIME + TAIL_WAIT + 4 ))   # +4 buffer (short workloads)
TOTAL_LONG=$(( BOOT_WAIT + RUN_TIME_LONG + TAIL_WAIT + 4 ))

run_one() {
    local s="$1" q="$2" w="$3"
    local stem
    case "${PHASE}" in
        3)  stem="q${q}_${w}"            ;;
        4)  stem="${s}_q${q}_${w}"       ;;
        5)  stem="${s}_${w}"             ;;   # single quantum
        6)  stem="${s}_${w}"             ;;   # single quantum, evaluation-only
        7)  stem="${s}_${w}"             ;;   # single quantum, after V2 fix
    esac

    # flipper / wolf have two phases (I/O-bound → CPU-bound) and need
    # extra wall time to complete. Other workloads use the short budget.
    local run_time alarm_total
    if [[ "${w}" == "flipper" || "${w}" == "wolf" ]]; then
        run_time="${RUN_TIME_LONG}"
        alarm_total="${TOTAL_LONG}"
    else
        run_time="${RUN_TIME}"
        alarm_total="${TOTAL}"
    fi
    local raw="${OUT}/${stem}.raw"
    local log="${OUT}/${stem}.log"

    echo "── sched=${s}  q=${q}ms  workload=${w}"

    # Build the run-spawn block. "concurrent" = cpu_bound + io_bound.
    local spawn_block
    if [[ "${w}" == "concurrent" ]]; then
        spawn_block=$'printf \'run cpu_bound\\n\'\nsleep 1\nprintf \'run io_bound\\n\''
    else
        spawn_block="printf 'run ${w}\\n'"
    fi

    # Compose the input script:
    #   wait BOOT_WAIT for init to settle,
    #   set quantum (RR uses it; MLFQ/V1/V2/V3 ignore per design),
    #   switch to the requested scheduler,
    #   clear trace, spawn workload, wait, dump, idle.
    # QEMU exit protocol: after the trace dump, send Ctrl-A X
    # (0x01 0x78), QEMU's documented "stop the emulator" sequence.
    # The perl alarm is a fallback in case Ctrl-A X doesn't fire (e.g.
    # the kernel's input handling is wedged) — but the typical path is
    # a clean exit, not a timeout.
    {
        sleep "${BOOT_WAIT}"
        printf 'quantum %d\n' "${q}"
        sleep 1
        printf 'sched %s\n' "${s}"
        sleep 1
        printf 'trace clear\n'
        sleep 1
        eval "${spawn_block}"
        sleep "${run_time}"
        printf 'trace 1024\n'
        sleep 1
        # Phase 6: capture the per-decision overhead snapshot into the log.
        printf 'cycles\n'
        sleep "${TAIL_WAIT}"
        printf '\x01x'         # Ctrl-A X — QEMU exits cleanly
    } | perl -e "
        \$|=1;
        \$SIG{ALRM}=sub{ kill 'KILL', \$pid; exit };
        \$pid = open(Q, '-|', 'qemu-system-riscv64 -machine virt -cpu rv64 ' .
                              '-m 128M -nographic -bios none -kernel ${KERNEL} 2>&1');
        alarm ${alarm_total};
        while (<Q>) { print }
        alarm 0;
    " > "${raw}" || true

    perl -pe 's/\e\[[0-9;?]*[a-zA-Z]//g; s/\r//g' "${raw}" > "${log}"

    local n_trace
    n_trace=$(grep -c '^TRACE' "${log}" || true)
    echo "   ${n_trace} TRACE records captured"

    # Parser writes <stem>.csv, <stem>-gantt.png, <stem>-summary.csv.
    python3 "${PARSER}" --workload-summary "${log}" >/dev/null
}

for s in "${SCHEDS[@]}"; do
    for q in "${QUANTA[@]}"; do
        for w in "${WORKLOADS[@]}"; do
            run_one "${s}" "${q}" "${w}"
        done
    done
done

echo
echo "Done. Results in ${OUT}/"
echo "Run count: $(ls "${OUT}" | grep -c summary.csv)"
