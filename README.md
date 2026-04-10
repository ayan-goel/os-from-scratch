# os-from-scratch

> *Most OS schedulers use fixed heuristics (round-robin quantum, MLFQ priority decay) that were tuned decades ago. Can a scheduler that models per-process behavior online make better decisions on modern workloads? I built a kernel from scratch to find out.*

A RISC-V kernel in C, built from bare metal, ending in an online-learning process scheduler evaluated against round-robin and MLFQ baselines.

## Quickstart

```sh
# Install toolchain (macOS)
brew install riscv64-elf-gcc riscv64-elf-binutils qemu

# Build and run
make clean && make qemu
# Expected: "hello from the kernel"
# Exit QEMU: Ctrl-A X
```

## Project structure

```
kernel/   kernel source (C + RISC-V assembly)
user/     synthetic workload programs
tools/    trace parsing and graph generation
notes/    phase-by-phase design notes
SPEC.md   full project specification
```

## Phases

| Phase | Goal | Status |
|---|---|---|
| 0 | Toolchain + bare-metal hello | Done |
| 1 | Baseline kernel (trap, pmem, vm, processes, round-robin) | Planned |
| 2 | Interactive shell, flat ramfs, ANSI TUI | Planned |
| 3 | Instrumentation + baseline measurements | Planned |
| 4 | Classical MLFQ | Planned |
| 5 | Learning scheduler (V1 burst predict → V2 classifier → V3 bandit) | Planned |
| 6 | Evaluation across all schedulers | Planned |
| 7 | Writeup | Planned |

See [SPEC.md](SPEC.md) for full design decisions, acceptance criteria, and constraints.
