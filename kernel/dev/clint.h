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
 * So TIMER_INTERVAL ticks = TIMER_INTERVAL / 10,000,000 seconds.
 *
 * TIMER_INTERVAL = 1,000,000 → 100 ms between interrupts.
 * This is large enough to see "tick" appear without flooding the screen.
 */
#define CLINT_BASE          0x2000000ULL
#define CLINT_MTIMECMP(h)   (*(volatile uint64_t *)(CLINT_BASE + 0x4000 + (h) * 8))
#define CLINT_MTIME         (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))

#define TIMER_INTERVAL      1000000ULL   /* 100 ms at 10 MHz */

void clint_init(void);
void clint_set_timer(void);

#endif /* CLINT_H */
