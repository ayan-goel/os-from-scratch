#ifndef KALLOC_H
#define KALLOC_H

#include "defs.h"

/*
 * kalloc — simple kernel bump allocator.
 *
 * Carves a fixed 256 KB heap out of physical memory at init time.
 * kmalloc returns 8-byte aligned pointers. kfree is a no-op for now —
 * kernel allocations in Phase 1 are for long-lived structures (page tables,
 * process descriptors) that are never individually freed during a run.
 */

#define HEAP_SIZE  (256 * 1024UL)   /* 256 KB */

void  kalloc_init(void);
void *kmalloc(uint64_t size);   /* returns NULL if heap is exhausted */
void  kfree(void *ptr);         /* no-op in Phase 1 */

#endif /* KALLOC_H */
