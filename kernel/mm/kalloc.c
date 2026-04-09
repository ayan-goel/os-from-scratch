/*
 * kalloc.c — kernel bump allocator.
 *
 * The heap lives in a fixed, page-aligned region reserved by linker.ld
 * (symbols _heap_start / _heap_end). That keeps the heap independent of
 * pmem's free-list ordering and makes pmem_init naturally skip the heap:
 * pmem's managed range starts at _end, which the linker places after the
 * heap.
 *
 * Allocation is a bump pointer — O(1) per call, no metadata, no per-object
 * free. kfree is a no-op. Phase 1's kernel allocates a bounded number of
 * long-lived objects (process table, page tables) and never individually
 * frees them, so a bump pointer is sufficient. Phase 2+ will revisit this
 * if we need dynamic freeing.
 */

#include "mm/kalloc.h"
#include "mm/pmem.h"
#include "dev/uart.h"
#include "defs.h"

/* Linker-exported heap boundaries. See linker.ld. */
extern char _heap_start[];
extern char _heap_end[];

static uint8_t *heap_start = NULL;
static uint8_t *heap_end   = NULL;
static uint8_t *heap_ptr   = NULL;

/*
 * kalloc_self_test — verify bump semantics right after init.
 *
 * Cheap at boot time (two 16-byte allocs) and catches pointer math errors
 * immediately instead of waiting for a downstream user to corrupt memory.
 */
static void kalloc_self_test(void) {
    uint8_t *p1 = (uint8_t *)kmalloc(16);
    uint8_t *p2 = (uint8_t *)kmalloc(16);

    KASSERT(p1 != NULL && p2 != NULL,     "kalloc: self-test alloc failed");
    KASSERT(p1 >= heap_start,             "kalloc: alloc below heap_start");
    KASSERT(p2 + 16 <= heap_end,          "kalloc: alloc past heap_end");
    KASSERT(p2 == p1 + 16,                "kalloc: non-adjacent bump allocs");
}

void kalloc_init(void) {
    heap_start = (uint8_t *)_heap_start;
    heap_end   = (uint8_t *)_heap_end;
    heap_ptr   = heap_start;

    /* Sanity: linker script should give us a page-aligned heap. */
    KASSERT(((uintptr_t)heap_start & (PAGE_SIZE - 1)) == 0,
            "kalloc: heap_start not page-aligned");
    KASSERT((uint64_t)(heap_end - heap_start) >= HEAP_SIZE,
            "kalloc: heap region smaller than HEAP_SIZE");

    uart_puts("kalloc: heap ready [");
    uart_puthex64((uint64_t)heap_start);
    uart_puts(", ");
    uart_puthex64((uint64_t)heap_end);
    uart_puts(")\n");

    kalloc_self_test();
}

void *kmalloc(uint64_t size) {
    /* Round up to 8-byte alignment. */
    size = (size + 7) & ~7ULL;

    if (heap_ptr + size > heap_end)
        return NULL;

    void *ptr = heap_ptr;
    heap_ptr += size;
    return ptr;
}

void kfree(void *ptr) {
    /* No-op. See header comment. */
    (void)ptr;
}
