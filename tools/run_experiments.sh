#!/usr/bin/env bash
# run_experiments.sh — Phase 3 round-robin baseline run matrix.
#
# For every (quantum ∈ {1, 10, 100} ms) × (workload ∈ {cpu_bound,
# io_bound, mixed, bursty, forker}), boot the kernel, wait for
# init's boot-time children to finish, change the quantum, clear
# the trace ring, run the workload, wait a fixed runtime, dump the
# trace, and exit. parse_trace.py turns each log into CSV + Gantt.
#
# Outputs land in results/phase3/q${Q}_${W}.{raw,log,csv,gantt.png,
# summary.csv}. The .raw is the unfiltered QEMU stdout (useful for
# debugging if a run fails); the .log is ANSI-stripped.
#
# Requires: qemu-system-riscv64, perl (for the alarm wrapper used in
# place of `timeout` on macOS), python3 + matplotlib for the parser.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${ROOT}/kernel.elf"
PARSER="${ROOT}/tools/parse_trace.py"
OUT="${ROOT}/results/phase3"

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
    local q="$1" w="$2"
    local stem="q${q}_${w}"
    local raw="${OUT}/${stem}.raw"
    local log="${OUT}/${stem}.log"

    echo "── q=${q}ms  workload=${w}"

    # Compose the input script:
    #   wait BOOT_WAIT for init to settle,
    #   set quantum, clear trace, spawn workload,
    #   wait RUN_TIME, dump trace, idle TAIL_WAIT.
    {
        sleep "${BOOT_WAIT}"
        printf 'quantum %d\n' "${q}"
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

for q in "${QUANTA[@]}"; do
    for w in "${WORKLOADS[@]}"; do
        run_one "${q}" "${w}"
    done
done

echo
echo "Done. Results in ${OUT}/"
ls "${OUT}" | sort
