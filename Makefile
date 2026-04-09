# Makefile for os-from-scratch
# Targets: make, make qemu, make clean

# ── Toolchain ────────────────────────────────────────────────────────────────
CROSS   := riscv64-elf
CC      := $(CROSS)-gcc
OBJCOPY := $(CROSS)-objcopy
OBJDUMP := $(CROSS)-objdump

# ── Compiler flags ────────────────────────────────────────────────────────────
# -MMD -MP emits a .d file next to each .o capturing the header dependencies
# that were actually used during compilation (minus system headers, which
# -nostdinc excludes anyway). We -include those .d files at the bottom of
# this Makefile so editing a header triggers a rebuild of anything that
# transitively includes it.
CFLAGS  := -std=c11 \
            -Wall -Wextra -Werror \
            -O0 -g \
            -march=rv64gc -mabi=lp64d -mcmodel=medany \
            -ffreestanding -nostdlib -nostdinc \
            -fno-stack-protector \
            -fno-pie -no-pie \
            -MMD -MP \
            -Ikernel

LDFLAGS := -T linker.ld -nostdlib

# ── Sources ───────────────────────────────────────────────────────────────────
# Layout:
#   kernel/arch/  — boot + trap vector + context switch (RISC-V asm)
#   kernel/dev/   — device drivers (UART, CLINT)
#   kernel/mm/    — memory management (pmem, vm, kalloc)
#   kernel/       — main, proc, trap dispatch, shared defs
KERNEL_C_SRCS   := kernel/main.c \
                   kernel/trap.c \
                   kernel/proc.c \
                   kernel/dev/uart.c \
                   kernel/dev/clint.c \
                   kernel/mm/pmem.c \
                   kernel/mm/vm.c \
                   kernel/mm/kalloc.c

KERNEL_ASM_SRCS := kernel/arch/entry.S \
                   kernel/arch/trapvec.S \
                   kernel/arch/switch.S

OBJS := $(KERNEL_ASM_SRCS:.S=.o) $(KERNEL_C_SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

# ── Output ────────────────────────────────────────────────────────────────────
TARGET  := kernel.elf

# ── QEMU ─────────────────────────────────────────────────────────────────────
QEMU        := qemu-system-riscv64
QEMU_MACHINE:= virt
QEMU_MEM    := 128M
QEMU_FLAGS  := -machine $(QEMU_MACHINE) \
               -cpu rv64 \
               -m $(QEMU_MEM) \
               -nographic \
               -bios none \
               -kernel $(TARGET)

# ── Rules ─────────────────────────────────────────────────────────────────────
.PHONY: all qemu clean disasm

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built $(TARGET)"

# Compile .S files.
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# Compile .c files.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Run the kernel in QEMU. Exit with Ctrl-A X.
qemu: all
	@echo "Starting QEMU (exit with Ctrl-A X)..."
	$(QEMU) $(QEMU_FLAGS)

# Disassemble the kernel image (useful for debugging).
disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET) | less

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	@echo "Cleaned."

# Pull in auto-generated header dependencies. The leading `-` suppresses
# the "file not found" error on a clean build (before any .d exists).
-include $(DEPS)
