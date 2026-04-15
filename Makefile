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
                   kernel/syscall.c \
                   kernel/ring.c \
                   kernel/io.c \
                   kernel/shell.c \
                   kernel/tui.c \
                   kernel/fs.c \
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

# Default target first, so `make` with no args builds the kernel
# (not the first user-program intermediate that appears below).
.PHONY: all qemu clean disasm
all: $(TARGET)

# ── User programs ────────────────────────────────────────────────────────────
# Each program is:  user/<name>.c + user/ulib.c → user/<name>.elf
#                   → user/<name>.bin (objcopy -O binary)
#                   → user/<name>_bin.o (objcopy -I binary, embedded in kernel)
# Embedded symbols: _binary_user_<name>_bin_{start,end,size}
#
# CFLAGS are inherited from the kernel (freestanding, no libc) but the
# -Ikernel flag is harmless since user sources only #include "ulib.h" which
# lives in user/ (found via the default "." include path used by gcc).
USER_PROGS  := init hello cpu_bound io_bound
USER_ULIB_O := user/ulib.o

# Per-program objects (each has its own _start).
USER_PROG_OBJS := $(patsubst %,user/%.o,$(USER_PROGS))
USER_ALL_OBJS  := $(USER_PROG_OBJS) $(USER_ULIB_O)
USER_DEPS      := $(USER_ALL_OBJS:.o=.d)

USER_ELFS   := $(patsubst %,user/%.elf,$(USER_PROGS))
USER_BINS   := $(patsubst %,user/%.bin,$(USER_PROGS))
USER_EMBEDS := $(patsubst %,user/%_bin.o,$(USER_PROGS))

# Each user ELF is the program's .o + ulib.o, linked at USER_TEXT_BASE.
user/%.elf: user/%.o $(USER_ULIB_O) user/linker.ld
	$(CC) -T user/linker.ld -nostdlib -o $@ $< $(USER_ULIB_O)

# Strip to raw binary.
user/%.bin: user/%.elf
	$(OBJCOPY) -O binary $< $@

# Re-wrap as an ELF .o with .rodata section.
user/%_bin.o: user/%.bin
	$(OBJCOPY) -I binary -O elf64-littleriscv -B riscv \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

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

$(TARGET): $(OBJS) $(USER_EMBEDS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(USER_EMBEDS)
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
	rm -f $(USER_ALL_OBJS) $(USER_DEPS) $(USER_ELFS) $(USER_BINS) $(USER_EMBEDS)
	@echo "Cleaned."

# Pull in auto-generated header dependencies. The leading `-` suppresses
# the "file not found" error on a clean build (before any .d exists).
-include $(DEPS)
-include $(USER_DEPS)
