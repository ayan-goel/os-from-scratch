/*
 * uart.c — 16550-compatible UART driver for the QEMU virt machine.
 *
 * The QEMU virt machine maps a 16550 UART at physical address 0x10000000.
 * Registers are 1 byte wide, spaced 1 byte apart.
 *
 * Relevant registers (offsets from base):
 *   0x00  RBR/THR  Receive Buffer (read) / Transmit Holding (write)
 *   0x05  LSR      Line Status Register
 *                    bit 0 (DR)  : 1 = RX FIFO has a byte ready to read
 *                    bit 5 (THRE): 1 = TX holding register is empty (safe to write)
 *
 * We do not enable interrupts here — polling only. The TX path spins on
 * THRE, and the RX path is drained by the timer handler on every tick.
 */

#include "dev/uart.h"

#define UART_BASE       0x10000000UL

/* Register offsets. */
#define UART_RBR        0   /* Receive Buffer Register (read) */
#define UART_THR        0   /* Transmit Holding Register (write) */
#define UART_IER        1   /* Interrupt Enable Register */
#define UART_FCR        2   /* FIFO Control Register */
#define UART_LCR        3   /* Line Control Register */
#define UART_LSR        5   /* Line Status Register */

#define UART_LSR_DR     (1 << 0)   /* RX data ready */
#define UART_LSR_THRE   (1 << 5)   /* Transmit holding register empty */

/* MMIO accessor — volatile to prevent the compiler from caching reads/writes. */
static volatile unsigned char *const uart = (volatile unsigned char *)UART_BASE;

void uart_init(void) {
    /*
     * Minimal init: disable interrupts, set 8N1 (8 data bits, no parity,
     * 1 stop bit). QEMU's 16550 comes out of reset in a usable state, but
     * being explicit is good practice.
     */
    uart[UART_IER] = 0x00;      /* disable all interrupts */
    uart[UART_LCR] = 0x03;      /* 8-bit word length, no parity, 1 stop bit */
    uart[UART_FCR] = 0x07;      /* enable and clear FIFOs */
}

void uart_putc(char c) {
    /* Spin until the transmit holding register is empty, then write. */
    while (!(uart[UART_LSR] & UART_LSR_THRE))
        ;
    uart[UART_THR] = (unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s) {
        /* Translate '\n' to '\r\n' for terminal compatibility. */
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_puthex64(uint64_t val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}

int uart_getc(void) {
    if (!(uart[UART_LSR] & UART_LSR_DR))
        return -1;
    return (int)uart[UART_RBR];
}

void uart_write_raw(const char *buf, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        while (!(uart[UART_LSR] & UART_LSR_THRE))
            ;
        uart[UART_THR] = (unsigned char)buf[i];
    }
}

void uart_putdec(uint64_t val) {
    /* Worst-case 20 digits for a 64-bit unsigned. */
    char buf[21];
    int i = 0;
    if (val == 0) {
        uart_putc('0');
        return;
    }
    while (val > 0 && i < 20) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0)
        uart_putc(buf[--i]);
}
