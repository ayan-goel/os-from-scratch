/*
 * pmem.c — physical page allocator.
 *
 * Free pages form a singly-linked intrusive list. The first 8 bytes of each
 * free page hold a pointer to the next free page (or NULL at the tail).
 * pmem_head points to the first free page.
 *
 * The managed range is [_end, PHYS_END), where _end is the first byte past
 * the kernel's BSS (exported by the linker script). pmem_init rounds _end up
 * to the nearest 4KB boundary before building the list.
 */

#include "mm/pmem.h"
#include "dev/uart.h"

/* Linker-exported symbol: first byte after kernel image (in BSS). */
extern char _end[];

/* Head of the free-page list. */
static void *pmem_head = NULL;
static uint64_t free_pages = 0;

static void memzero(void *ptr, uint64_t n) {
    uint64_t *p = (uint64_t *)ptr;
    uint64_t words = n / 8;
    for (uint64_t i = 0; i < words; i++)
        p[i] = 0;
}

void pmem_init(void) {
    /*
     * Start just past the kernel image, rounded up to a page boundary.
     * Walk to PHYS_END in PAGE_SIZE steps, pushing each page onto the list.
     */
    uint64_t start = PAGE_ALIGN_UP((uint64_t)_end);

    for (uint64_t pa = start; pa + PAGE_SIZE <= PHYS_END; pa += PAGE_SIZE) {
        /* Use pmem_free to build the list — it handles the pointer writes. */
        pmem_free((void *)pa);
    }

    uart_puts("pmem: initialized. free pages: ");
    uart_puthex64(free_pages);
    uart_puts("\n");
}

void pmem_free(void *pa) {
    /*
     * Push this page onto the head of the free list.
     * Zero the page first so callers get clean memory from pmem_alloc.
     */
    memzero(pa, PAGE_SIZE);
    *(void **)pa = pmem_head;
    pmem_head = pa;
    free_pages++;
}

void *pmem_alloc(void) {
    if (pmem_head == NULL)
        return NULL;

    void *page = pmem_head;
    pmem_head = *(void **)page;
    free_pages--;
    return page;
}

uint64_t pmem_free_count(void) {
    return free_pages;
}
