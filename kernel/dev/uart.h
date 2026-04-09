#ifndef UART_H
#define UART_H

#include "defs.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex64(uint64_t val);   /* print 0x<16 hex digits> */

#endif
