#ifndef VM_H
#define VM_H

#include "defs.h"

/*
 * vm.h — Sv39 virtual memory.
 *
 * Sv39 uses a 3-level page table. Each table is one 4KB page containing
 * 512 × 8-byte PTEs. A virtual address is:
 *
 *   [63:39] sign-extended (must equal bit 38)
 *   [38:30] VPN[2]  — level-2 (root) page table index
 *   [29:21] VPN[1]  — level-1 page table index
 *   [20:12] VPN[0]  — level-0 (leaf) page table index
 *   [11: 0] offset  — byte offset within page
 *
 * PTE layout (64-bit):
 *   [63:54] reserved
 *   [53:28] PPN[2]  — physical page number bits 53:28
 *   [27:19] PPN[1]
 *   [18:10] PPN[0]
 *   [ 9: 8] RSW     — reserved for software
 *   [    7] D       — dirty
 *   [    6] A       — accessed
 *   [    5] G       — global
 *   [    4] U       — user-accessible
 *   [    3] X       — execute
 *   [    2] W       — write
 *   [    1] R       — read
 *   [    0] V       — valid
 *
 * A PTE with R=W=X=0 and V=1 is a pointer to the next-level page table
 * (the PPN field holds the physical page number of that table).
 */

typedef uint64_t  pte_t;
typedef pte_t    *pagetable_t;

/* PTE permission bits. */
#define PTE_V   (1ULL << 0)   /* valid */
#define PTE_R   (1ULL << 1)   /* readable */
#define PTE_W   (1ULL << 2)   /* writable */
#define PTE_X   (1ULL << 3)   /* executable */
#define PTE_U   (1ULL << 4)   /* user-accessible */
#define PTE_G   (1ULL << 5)   /* global */
#define PTE_A   (1ULL << 6)   /* accessed */
#define PTE_D   (1ULL << 7)   /* dirty */

/* Common permission combinations. */
#define PTE_KERN_RW  (PTE_V | PTE_R | PTE_W)
#define PTE_KERN_RX  (PTE_V | PTE_R | PTE_X)
#define PTE_KERN_RWX (PTE_V | PTE_R | PTE_W | PTE_X)
#define PTE_USER_RW  (PTE_V | PTE_R | PTE_W | PTE_U)
#define PTE_USER_RX  (PTE_V | PTE_R | PTE_X | PTE_U)
#define PTE_USER_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)

/* Extract the physical page number from a leaf PTE. */
#define PTE2PA(pte)  (((pte) >> 10) << 12)

/* Build a leaf PTE from a physical address and permission bits. */
#define PA2PTE(pa)   (((uint64_t)(pa) >> 12) << 10)

/* Sv39 SATP register: MODE=8 (Sv39) in bits [63:60], PPN in bits [43:0]. */
#define SATP_SV39    (8ULL << 60)
#define MAKE_SATP(pt) (SATP_SV39 | ((uint64_t)(pt) >> 12))

/* The global kernel page table — set up by vm_init() and never freed. */
extern pagetable_t kernel_pagetable;

void        vm_init(void);     /* build kernel_pagetable and enable paging */

pagetable_t vm_create(void);
int         vm_map(pagetable_t pt, uint64_t va, uint64_t pa,
                   uint64_t size, uint64_t perm);
void        vm_unmap(pagetable_t pt, uint64_t va, uint64_t size,
                     int free_phys_pages);
uint64_t    vm_pa_of(pagetable_t pt, uint64_t va);  /* 0 if not mapped */
void        vm_free(pagetable_t pt, int level);      /* free page table pages */

#endif /* VM_H */
