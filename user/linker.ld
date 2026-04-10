/*
 * User program linker script.
 *
 * Places the program at USER_TEXT_BASE (0x40000000). This is in root entry 1
 * of Sv39 page tables — the kernel uses root entries 0 (UART/CLINT) and 2
 * (RAM at 0x80000000), so user mappings in entry 1 don't collide with any
 * kernel subtree in a process's cloned page table. That avoids the
 * clone-on-write dance we'd need if we put user text at the conventional
 * 0x10000 (which shares root entry 0 with UART/CLINT).
 *
 * The kernel embeds the objcopy -O binary output of the linked ELF, so the
 * first byte of the resulting .bin file must be the entry point. We route
 * _start through a dedicated .text.boot section listed first in .text to
 * guarantee this.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
    . = 0x40000000;

    .text : {
        *(.text.boot)        /* _start must be here, first */
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}
