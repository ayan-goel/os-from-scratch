#ifndef PMEM_H
#define PMEM_H

#include "defs.h"

/*
 * pmem — physical page allocator.
 *
 * Manages all RAM pages not occupied by the kernel binary.
 * Uses an intrusive free list: each free page's first 8 bytes are a pointer
 * to the next free page.
 *
 * Allocation is O(1). Free is O(1).
 */

#define PAGE_SIZE  4096UL
#define PHYS_BASE  0x80000000UL   /* RAM start (matches linker.ld) */
#define PHYS_END   0x88000000UL   /* RAM end (128 MB) */

/* Align x up to the nearest multiple of PAGE_SIZE. */
#define PAGE_ALIGN_UP(x)   (((uint64_t)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

void  pmem_init(void);
void *pmem_alloc(void);            /* returns a zeroed 4KB page, or NULL */
void  pmem_free(void *pa);

/* Returns the total number of free pages (for diagnostics). */
uint64_t pmem_free_count(void);

#endif /* PMEM_H */
