#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
int uart_getc(void);                            /* -1 if nothing received */
void uart_write(const uint8_t *p, unsigned n);  /* queued, sent by the TX interrupt */

#endif
