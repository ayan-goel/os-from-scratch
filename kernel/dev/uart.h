#ifndef UART_H
#define UART_H

#include "defs.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex64(uint64_t val);   /* print 0x<16 hex digits> */
void uart_putdec(uint64_t val);     /* print decimal, no leading zeros */

/*
 * uart_write_raw — emit bytes verbatim, no LF→CRLF translation.
 * Used by the TUI renderer for ANSI escape sequences.
 */
void uart_write_raw(const char *buf, uint64_t len);

/*
 * Non-blocking read. Returns the next available byte from the RX FIFO
 * if one exists, or -1 if the FIFO is empty. The timer handler polls
 * this on every tick to drain characters into the RX ring buffer used
 * by the kernel shell.
 */
int uart_getc(void);

#endif
