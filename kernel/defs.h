#ifndef DEFS_H
#define DEFS_H

/*
 * defs.h — architecture-neutral types and globals.
 *
 * RISC-V-specific CSR access macros and bit constants live in arch/riscv.h.
 * Files that don't touch privilege state should only need this header.
 */

/* ── Integer types ──────────────────────────────────────────────────────────
 * We can't include <stdint.h> (no stdlib in kernel).
 *
 * On the lp64d ABI both `long` and `long long` are 64 bits, but they are
 * distinct types to the C compiler. Using `unsigned long` for both uint64_t
 * and uintptr_t keeps them ABI-compatible and avoids strict-aliasing warnings
 * when we cast between them (common when walking page tables).
 */
typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;
typedef unsigned long   uint64_t;
typedef signed   long   int64_t;
typedef unsigned long   uintptr_t;

#define NULL ((void *)0)

/* ── Global declarations ────────────────────────────────────────────────────
 * Functions needed across multiple translation units that don't have a
 * dedicated header.
 */
void panic(const char *msg) __attribute__((noreturn));

/*
 * KASSERT — runtime invariant check.
 *
 * Prefer KASSERT over `if (!x) panic(...)` at module boundaries. The macro
 * prefixes the panic message so post-mortem debugging is a grep away from
 * the assertion site.
 */
#define KASSERT(cond, msg) do { if (!(cond)) panic("KASSERT: " msg); } while (0)

#endif /* DEFS_H */
