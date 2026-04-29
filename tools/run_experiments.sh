#!/usr/bin/env bash
# run_experiments.sh — Phase 3/4 scheduler run matrix.
#
# Phase 3 (round-robin only): 3 quanta × 5 workloads = 15 runs.
# Phase 4 (RR + MLFQ):        2 sched × 3 quanta × 5 workloads = 30 runs.
#
# For every (sched, quantum, workload), boot the kernel, wait for
# init's boot-time children to finish, set the quantum, switch to
# the requested scheduler, clear the trace ring, run the workload,
# wait a fixed runtime, dump the trace, and exit. parse_trace.py
# turns each log into CSV + Gantt + per-pid summary.
#
# Output layout:
#   results/phase3/q${Q}_${W}.{raw,log,csv,gantt.png,summary.csv}
#       — produced when invoked as `bash tools/run_experiments.sh`
#         (no PHASE override) — preserves the Phase 3 baseline files
#         exactly as they were before MLFQ existed.
#   results/phase4/${SCHED}_q${Q}_${W}.{...}
#       — produced when invoked as `PHASE=4 bash tools/run_experiments.sh`
#         (or by the same script with the `phase4` arg).
#
# The two layouts are kept side-by-side because the Phase 3 notes
# reference results/phase3/ stems and shouldn't break.
#
# Requires: qemu-system-riscv64, perl (for the alarm wrapper used in
# place of `timeout` on macOS), python3 + matplotlib for the parser.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${ROOT}/kernel.elf"
PARSER="${ROOT}/tools/parse_trace.py"

PHASE="${PHASE:-${1:-3}}"
case "${PHASE}" in
    3)  OUT="${ROOT}/results/phase3"; SCHEDS=(rr) ;;
    4)  OUT="${ROOT}/results/phase4"; SCHEDS=(rr mlfq) ;;
    *)  echo "run_experiments: unknown phase '${PHASE}' (try 3 or 4)" >&2; exit 1 ;;
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
TOTAL=$(( BOOT_WAIT + RUN_TIME + TAIL_WAIT + 4 ))   # +4 buffer

QUANTA=(1 10 100)
WORKLOADS=(cpu_bound io_bound mixed bursty forker)

run_one() {
    local s="$1" q="$2" w="$3"
    local stem
    if [[ "${PHASE}" == "3" ]]; then
        stem="q${q}_${w}"
    else
        stem="${s}_q${q}_${w}"
    fi
    local raw="${OUT}/${stem}.raw"
    local log="${OUT}/${stem}.log"

    echo "── sched=${s}  q=${q}ms  workload=${w}"

    # Compose the input script:
    #   wait BOOT_WAIT for init to settle,
    #   set quantum (RR uses it; MLFQ ignores per design),
    #   switch to the requested scheduler,
    #   clear trace, spawn workload, wait, dump, idle.
    {
        sleep "${BOOT_WAIT}"
        printf 'quantum %d\n' "${q}"
        sleep 1
        printf 'sched %s\n' "${s}"
        sleep 1
        printf 'trace clear\n'
        sleep 1
        printf 'run %s\n' "${w}"
        sleep "${RUN_TIME}"
        printf 'trace 1024\n'
        sleep "${TAIL_WAIT}"
    } | perl -e "
        \$|=1;
        \$SIG{ALRM}=sub{ kill 'TERM', \$pid; exit };
        \$pid = open(Q, '-|', 'qemu-system-riscv64 -machine virt -cpu rv64 ' .
                              '-m 128M -nographic -bios none -kernel ${KERNEL} 2>&1');
        alarm ${TOTAL};
        while (<Q>) { print }
    " > "${raw}"

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
