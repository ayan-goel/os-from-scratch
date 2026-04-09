/*
 * Linker script for QEMU virt machine.
 * QEMU loads the kernel ELF and jumps to 0x80000000.
 * RAM starts at 0x80000000 (128 MB available by default).
 *
 * Two load segments so the binary respects W^X:
 *   text segment : R+X  (.text, .rodata)
 *   data segment : R+W  (.data, .bss, kernel heap)
 *
 * Symbols exported to C:
 *   _etext       — page-aligned boundary between R+X and R+W regions
 *   _bss_start   — start of BSS (cleared by entry.S)
 *   _bss_end     — end of BSS (also cleared; covers the heap)
 *   _heap_start  — first byte of the kernel heap (page-aligned)
 *   _heap_end    — one past the last byte of the kernel heap
 *   _end         — first byte past everything; where pmem_init begins
 */

OUTPUT_ARCH(riscv)
ENTRY(_entry)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X  (PF_R | PF_X) */
    data PT_LOAD FLAGS(6);   /* R + W  (PF_R | PF_W) */
}

SECTIONS {
    /* Kernel loads at 0x80000000. */
    . = 0x80000000;

    .text : {
        *(.text.entry)   /* entry.S must be first */
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    /* Page-align the text/rodata boundary so the W^X split lands on a
     * 4 KB boundary. PTE permissions are page-granular, so _etext must
     * be a multiple of PAGE_SIZE for vm_init's two-range mapping to work. */
    . = ALIGN(4096);
    _etext = .;

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)

        /* Kernel heap lives at the end of .bss. This is NOBITS so it
         * doesn't bloat the ELF image, but entry.S's BSS clear will
         * zero it at boot. pmem_init begins at _end, past the heap,
         * so the heap and the physical page allocator cannot collide. */
        . = ALIGN(4096);
        _heap_start = .;
        . += 256 * 1024;   /* 256 KB kernel heap (keep in sync with HEAP_SIZE) */
        _heap_end = .;

        _bss_end = .;
    } :data

    _end = .;
}
