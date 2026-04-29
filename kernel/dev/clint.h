#ifndef CLINT_H
#define CLINT_H

#include "defs.h"

/*
 * CLINT — Core Local Interruptor.
 *
 * On the QEMU virt machine, the CLINT is at 0x2000000.
 * It contains a memory-mapped real-time counter (mtime) and a per-hart
 * compare register (mtimecmp). A machine timer interrupt fires whenever
 * mtime >= mtimecmp[hart].
 *
 * The CLINT clock runs at 10 MHz (100 ns per tick) regardless of CPU speed.
 * So `timer_interval` ticks = `timer_interval` / 10,000,000 seconds.
 *
 * `timer_interval` is a runtime variable so the shell's `quantum <ms>`
 * command can change the scheduling quantum live. Initial value is
 * 100000 (10 ms). Changes take effect on the next clint_set_timer call,
 * which happens once per timer interrupt — so latency is bounded by the
 * old quantum.
 */
#define CLINT_BASE          0x2000000ULL
#define CLINT_MTIMECMP(h)   (*(volatile uint64_t *)(CLINT_BASE + 0x4000 + (h) * 8))
#define CLINT_MTIME         (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))

extern volatile uint64_t timer_interval;

void clint_init(void);
void clint_set_timer(void);

#endif /* CLINT_H */
