/*
 * vm.c — Sv39 virtual memory.
 *
 * The kernel page table identity-maps physical RAM so that VA == PA
 * throughout the kernel range. This lets every kernel pointer keep working
 * when we drop to user mode (which is the first time the MMU actually
 * translates anything — see note below).
 *
 * W^X layout:
 *   [PHYS_BASE, _etext)   — R + X   (text, rodata)
 *   [_etext, PHYS_END)    — R + W   (data, bss, heap, free pages)
 *   UART / CLINT MMIO     — R + W
 *
 * Important caveat on when paging is actually active:
 *   In M-mode, instruction fetches and (by default) data accesses bypass
 *   SATP entirely. Writing SATP here configures the page table; it does
 *   NOT re-translate currently running kernel code. The mapping only starts
 *   mattering when we `mret` to U-mode in T6 — at that point, user VAs go
 *   through this table, and kernel VAs (during traps) continue to be
 *   identity-mapped so the handlers Just Work.
 *
 * In T6 each process gets its own page table, initialized by cloning the
 * kernel mappings and adding user-accessible (PTE_U) leaves in the lower
 * address range.
 */

#include "mm/vm.h"
#include "mm/pmem.h"
#include "dev/uart.h"
#include "defs.h"
#include "arch/riscv.h"

pagetable_t kernel_pagetable = NULL;

/* Linker-exported symbols marking kernel region boundaries. */
extern char _etext[];       /* end of text+rodata (page-aligned by linker.ld) */
extern char _heap_start[];  /* start of kernel heap (page-aligned) */
extern char _end[];         /* first byte past everything */

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * pte_walk — walk the 3-level page table for va.
 *
 * If alloc=1 and an intermediate table is missing, allocates a new page for
 * it. Returns a pointer to the leaf PTE (level 0), or NULL on failure.
 */
static pte_t *pte_walk(pagetable_t pt, uint64_t va, int alloc) {
    /*
     * Sv39: three 9-bit VPN fields extracted from bits [38:12].
     * Level 2 is the root (indexed by VA[38:30]).
     * Level 1 is the middle (indexed by VA[29:21]).
     * Level 0 is the leaf (indexed by VA[20:12]).
     */
    for (int level = 2; level > 0; level--) {
        /* Index into the current table: 9 bits from the appropriate VPN. */
        pte_t *pte = &pt[(va >> (12 + 9 * level)) & 0x1FF];

        if (*pte & PTE_V) {
            /* Follow the pointer to the next-level table. */
            pt = (pagetable_t)PTE2PA(*pte);
        } else {
            /* Allocate a new page table page if requested. */
            if (!alloc)
                return NULL;
            pt = (pagetable_t)pmem_alloc();
            if (pt == NULL)
                return NULL;
            /* Install as a pointer PTE: PPN set, R/W/X all zero = branch node. */
            *pte = PA2PTE(pt) | PTE_V;
        }
    }

    /* Return pointer to the level-0 leaf PTE. */
    return &pt[(va >> 12) & 0x1FF];
}

/* ── Public API ──────────────────────────────────────────────────────────── */

pagetable_t vm_create(void) {
    pagetable_t pt = (pagetable_t)pmem_alloc();
    if (pt == NULL)
        panic("vm_create: out of memory");
    return pt;
}

/*
 * vm_map — map [va, va+size) → [pa, pa+size) with the given permissions.
 *
 * size must be a multiple of PAGE_SIZE. va and pa must be page-aligned.
 * Returns 0 on success, -1 on allocation failure.
 */
int vm_map(pagetable_t pt, uint64_t va, uint64_t pa, uint64_t size, uint64_t perm) {
    if (size % PAGE_SIZE != 0)
        panic("vm_map: size not page-aligned");
    if (va % PAGE_SIZE != 0)
        panic("vm_map: va not page-aligned");

    uint64_t end = va + size;
    for (uint64_t cur = va; cur < end; cur += PAGE_SIZE, pa += PAGE_SIZE) {
        pte_t *pte = pte_walk(pt, cur, 1 /* alloc */);
        if (pte == NULL)
            return -1;
        if (*pte & PTE_V)
            panic("vm_map: remapping an already-mapped page");
        *pte = PA2PTE(pa) | perm | PTE_V;
    }
    return 0;
}

/*
 * vm_unmap — unmap [va, va+size).
 *
 * If free_phys_pages is non-zero, the physical pages backing the mapping are
 * returned to pmem. The page table pages themselves are NOT freed here (use
 * vm_free for that when tearing down an entire address space).
 */
void vm_unmap(pagetable_t pt, uint64_t va, uint64_t size, int free_phys_pages) {
    if (size % PAGE_SIZE != 0)
        panic("vm_unmap: size not page-aligned");

    uint64_t end = va + size;
    for (uint64_t cur = va; cur < end; cur += PAGE_SIZE) {
        pte_t *pte = pte_walk(pt, cur, 0 /* no alloc */);
        if (pte == NULL || !(*pte & PTE_V))
            panic("vm_unmap: unmapping page that isn't mapped");
        if (free_phys_pages)
            pmem_free((void *)PTE2PA(*pte));
        *pte = 0;
    }
}

/*
 * vm_pa_of — translate VA to PA. Returns 0 if not mapped.
 */
uint64_t vm_pa_of(pagetable_t pt, uint64_t va) {
    pte_t *pte = pte_walk(pt, va, 0);
    if (pte == NULL || !(*pte & PTE_V))
        return 0;
    return PTE2PA(*pte) | (va & (PAGE_SIZE - 1));
}

/*
 * vm_free — recursively free an address space's page table structure.
 *
 * Call with level=2 on the root. Walks down the tree, freeing branch pages.
 * Does NOT free the physical pages that leaf PTEs point to — the caller must
 * vm_unmap() those first (with free_phys_pages=1 if the pages were private).
 *
 * Panics if a leaf table still contains valid entries — if you hit this,
 * you forgot to unmap user pages before tearing down the address space.
 */
void vm_free(pagetable_t pt, int level) {
    if (level == 0) {
        /*
         * Leaf-table sanity check: all PTEs must have been cleared by
         * a prior vm_unmap. A V=1 leaf here means the caller is leaking
         * physical memory — crash loudly during development so it gets
         * caught in testing instead of a production memory leak.
         */
        for (int i = 0; i < 512; i++)
            KASSERT(!(pt[i] & PTE_V), "vm_free: live leaf PTE at teardown");
        pmem_free(pt);
        return;
    }
    for (int i = 0; i < 512; i++) {
        pte_t pte = pt[i];
        if ((pte & PTE_V) && !(pte & (PTE_R | PTE_W | PTE_X))) {
            /* Branch PTE: recurse into the next-level table. */
            vm_free((pagetable_t)PTE2PA(pte), level - 1);
        }
        pt[i] = 0;
    }
    pmem_free(pt);
}

/* ── Per-process pagetable cloning ───────────────────────────────────────── */

/*
 * vm_clone_kernel — allocate a new root and copy kernel_pagetable's 512
 * root entries verbatim.
 *
 * Why a memcpy-equivalent of the root is enough: the root has 512 entries,
 * each covering a 1 GB slice of VA space. Every slice currently in use
 * (RAM at 0x80000000, UART/CLINT at 0x02000000 and 0x10000000) is fully
 * described by an entry that points to a level-1 table. Copying the entry
 * makes the clone reuse the same level-1 subtree as the kernel, which is
 * fine as long as the clone doesn't try to edit that subtree.
 *
 * T6.3+ adds user mappings in the low VA range, which shares a root entry
 * with the UART/CLINT subtree. At that point vm_clone_kernel's callers
 * will need to clone-on-write the conflicting root entry before inserting
 * user PTEs. For T6.2, no caller yet modifies the clone's table, so simple
 * copy semantics are correct.
 */
pagetable_t vm_clone_kernel(void) {
    pagetable_t pt = vm_create();   /* pmem_alloc returns a zeroed page */
    for (int i = 0; i < 512; i++)
        pt[i] = kernel_pagetable[i];
    return pt;
}

/*
 * vm_free_subtree — recursively tear down a page table subtree that is
 * owned entirely by one process (not shared with the kernel).
 *
 * Frees both the physical data pages at leaves AND the intermediate
 * page table pages. Call on the level-1 table pointed to by a diverged
 * root entry.
 *
 * level=1: entries are either branches to level-0 tables or (shouldn't
 *          happen) 2 MB superpages.
 * level=0: entries are leaf 4 KB page mappings.
 */
static void vm_free_subtree(pagetable_t pt, int level) {
    for (int i = 0; i < 512; i++) {
        pte_t pte = pt[i];
        if (!(pte & PTE_V))
            continue;

        if (level > 0 && !(pte & (PTE_R | PTE_W | PTE_X))) {
            /* Branch PTE — recurse into the next-level table. */
            vm_free_subtree((pagetable_t)PTE2PA(pte), level - 1);
        } else {
            /* Leaf PTE (4 KB page or superpage). Free the physical page. */
            pmem_free((void *)PTE2PA(pte));
        }
        pt[i] = 0;
    }
    pmem_free(pt);
}

/*
 * vm_free_clone — release a per-process cloned pagetable.
 *
 * Walks the 512 root entries. Entries that still match kernel_pagetable
 * are shared kernel subtrees — skip them (do NOT free). Entries that
 * diverge are process-private user subtrees — tear them down completely
 * via vm_free_subtree, which frees both physical data pages and
 * intermediate page table pages.
 *
 * Finally frees the root page itself.
 */
void vm_free_clone(pagetable_t pt) {
    for (int i = 0; i < 512; i++) {
        if (pt[i] == kernel_pagetable[i])
            continue;   /* shared kernel subtree — hands off */
        if (pt[i] & PTE_V) {
            /* Diverged entry — process owns this subtree. */
            vm_free_subtree((pagetable_t)PTE2PA(pt[i]), 1);
        }
    }
    pmem_free(pt);
}

/*
 * vm_teardown_user — remove all user mappings from a cloned pagetable.
 *
 * Used by exec to tear down the old address space before loading a new
 * binary. After this call, pt is equivalent to a freshly cloned kernel
 * pagetable — ready for new vm_map calls.
 */
void vm_teardown_user(pagetable_t pt) {
    for (int i = 0; i < 512; i++) {
        if (pt[i] == kernel_pagetable[i])
            continue;
        if (pt[i] & PTE_V)
            vm_free_subtree((pagetable_t)PTE2PA(pt[i]), 1);
        pt[i] = kernel_pagetable[i];
    }
}

/*
 * vm_copy_user_pages — deep-copy all user-mapped pages from src to dst.
 *
 * Walks root entries of src that differ from kernel_pagetable (user-owned
 * subtrees). For each valid leaf PTE, allocates a new physical page,
 * copies 4096 bytes from the source PA, and maps the same VA in dst with
 * the same permission bits.
 *
 * The walk reconstructs full VAs from the three VPN indices. We iterate
 * levels 2→1→0 manually rather than using pte_walk for each VA because
 * we don't know which VAs are mapped without a full scan.
 *
 * Returns 0 on success, -1 on OOM (dst may be partially populated — the
 * caller must tear it down via vm_free_clone on failure).
 */
int vm_copy_user_pages(pagetable_t dst, pagetable_t src) {
    for (int i2 = 0; i2 < 512; i2++) {
        if (src[i2] == kernel_pagetable[i2])
            continue;   /* shared kernel subtree — skip */
        if (!(src[i2] & PTE_V))
            continue;

        pagetable_t l1 = (pagetable_t)PTE2PA(src[i2]);
        for (int i1 = 0; i1 < 512; i1++) {
            if (!(l1[i1] & PTE_V))
                continue;
            if (l1[i1] & (PTE_R | PTE_W | PTE_X)) {
                /* 2 MB superpage — shouldn't exist, skip defensively. */
                continue;
            }

            pagetable_t l0 = (pagetable_t)PTE2PA(l1[i1]);
            for (int i0 = 0; i0 < 512; i0++) {
                pte_t pte = l0[i0];
                if (!(pte & PTE_V))
                    continue;

                /* Reconstruct the full VA from the three VPN indices. */
                uint64_t va = ((uint64_t)i2 << 30) |
                              ((uint64_t)i1 << 21) |
                              ((uint64_t)i0 << 12);

                uint64_t src_pa = PTE2PA(pte);
                uint64_t perm = pte & (PTE_R | PTE_W | PTE_X | PTE_U);

                void *new_page = pmem_alloc();
                if (new_page == NULL)
                    return -1;

                /* Copy the page contents. */
                uint8_t *s = (uint8_t *)src_pa;
                uint8_t *d = (uint8_t *)new_page;
                for (uint64_t b = 0; b < PAGE_SIZE; b++)
                    d[b] = s[b];

                if (vm_map(dst, va, (uint64_t)new_page, PAGE_SIZE, perm) < 0)
                    return -1;
            }
        }
    }
    return 0;
}

/* ── W^X self-test ───────────────────────────────────────────────────────── */

/*
 * vm_check_wx — walk the page table for known-text and known-data addresses
 * and panic if the permissions are wrong.
 *
 * Runs at the end of vm_init, after the mapping is built but before the
 * sfence. A failure here means the W^X split in linker.ld drifted away from
 * the split in vm_init below — both must land on the same page boundary.
 *
 * Note: we check permissions by walking the table directly (via pte_walk)
 * rather than trying to execute/write through the mapping, because in M-mode
 * the MMU is bypassed and the hardware wouldn't enforce our bits anyway.
 * The point of this test is to verify the table's *shape* — the hardware
 * will enforce it for real once we drop to U-mode in T6.
 */
static void vm_check_wx(void) {
    pte_t *pte;

    /* Kernel text @ PHYS_BASE: must be R+X, must not be W. */
    pte = pte_walk(kernel_pagetable, PHYS_BASE, 0);
    KASSERT(pte != NULL && (*pte & PTE_V), "vm_check: text not mapped");
    KASSERT(*pte & PTE_X,                  "vm_check: text is not executable");
    KASSERT(!(*pte & PTE_W),               "vm_check: text is writable (W^X)");

    /* Kernel heap @ _heap_start: must be R+W, must not be X. */
    pte = pte_walk(kernel_pagetable, (uint64_t)_heap_start, 0);
    KASSERT(pte != NULL && (*pte & PTE_V), "vm_check: heap not mapped");
    KASSERT(*pte & PTE_W,                  "vm_check: heap is not writable");
    KASSERT(!(*pte & PTE_X),               "vm_check: heap is executable (W^X)");
}

/*
 * vm_init — build the kernel page table and configure Sv39.
 *
 * Memory layout (see linker.ld for _etext):
 *   [PHYS_BASE, _etext)          → R+X   — .text, .rodata
 *   [_etext,    PHYS_END)        → R+W   — .data, .bss, heap, free pages
 *   UART MMIO (one page)         → R+W
 *   CLINT MMIO (64 KB)           → R+W
 *
 * Writing SATP here does NOT translate kernel instruction fetches — in
 * M-mode the MMU is bypassed regardless of SATP. The table is configured
 * so that when T6 drops to U-mode, user accesses go through it. Kernel
 * traps continue to run with identity mappings so handler code Just Works.
 */
void vm_init(void) {
    kernel_pagetable = vm_create();

    uint64_t etext = (uint64_t)_etext;

    /* [PHYS_BASE, _etext) — kernel code + rodata: R+X. */
    if (vm_map(kernel_pagetable, PHYS_BASE, PHYS_BASE,
               etext - PHYS_BASE, PTE_KERN_RX) < 0)
        panic("vm_init: failed to map kernel text");

    /* [_etext, PHYS_END) — data, bss, heap, and all free pages: R+W. */
    if (vm_map(kernel_pagetable, etext, etext,
               PHYS_END - etext, PTE_KERN_RW) < 0)
        panic("vm_init: failed to map RAM data region");

    /* UART MMIO (one page). */
    if (vm_map(kernel_pagetable, 0x10000000UL, 0x10000000UL,
               PAGE_SIZE, PTE_KERN_RW) < 0)
        panic("vm_init: failed to map UART");

    /* CLINT MMIO (64 KB covers msip, mtimecmp, and mtime). */
    if (vm_map(kernel_pagetable, 0x2000000UL, 0x2000000UL,
               0x10000UL, PTE_KERN_RW) < 0)
        panic("vm_init: failed to map CLINT");

    /*
     * Verify the W^X split before arming SATP. If this panics, vm_init's
     * boundaries disagree with linker.ld's _etext — fix one or the other.
     */
    vm_check_wx();

    /*
     * Arm SATP. Has no immediate effect (M-mode bypasses translation), but
     * positions the kernel page table so that the first mret to U-mode
     * in T6 will translate user accesses through it.
     */
    CSR_WRITE(satp, MAKE_SATP(kernel_pagetable));
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");

    uart_puts("vm: kernel page table built (activates on first U-mode entry)\n");
}
