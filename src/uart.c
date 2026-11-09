/* USART1 on PA9 (TX) / PA10 (RX), AF7. Polled TX, interrupt-driven RX ring. */
#include "stm32f4.h"
#include "system.h"
#include "uart.h"

#define RX_SIZE 128

static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;

void uart_init(uint32_t baud)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA;
    RCC->APB2ENR |= RCC_APB2ENR_USART1;
    __dsb();

    for (unsigned pin = 9; pin <= 10; pin++) {
        GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << 4 * (pin - 8))) | (7u << 4 * (pin - 8));
        GPIOA->MODER = (GPIOA->MODER & ~(3u << 2 * pin)) | (2u << 2 * pin);
    }
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 20)) | (1u << 20);  /* RX pull-up */

    USART1->CR1 = 0;
    USART1->CR2 = 0;
    USART1->CR3 = 0;                        /* bootloader leaves DMAR set */
    USART1->BRR = (APB2_HZ + baud / 2) / baud;
    USART1->CR1 = (1u << 13) | (1u << 5) | (1u << 3) | (1u << 2);  /* UE, RXNEIE, TE, RE */

    REG32(0xE000E100 + 4 * (37 / 32)) = 1u << (37 % 32);  /* NVIC enable USART1 */
}

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;
    if (sr & ((1u << 5) | (1u << 3))) {     /* RXNE or ORE: reading DR clears both */
        uint8_t c = USART1->DR;
        uint32_t next = (rx_head + 1) % RX_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

int uart_getc(void)
{
    if (rx_tail == rx_head)
        return -1;
    int c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_SIZE;
    return c;
}

void uart_putc(char c)
{
    while (!(USART1->SR & (1u << 7)))       /* TXE */
        ;
    USART1->DR = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_puthex(uint32_t v, int digits)
{
    while (digits--)
        uart_putc("0123456789ABCDEF"[(v >> (4 * digits)) & 0xF]);
}

void uart_putdec(uint32_t v)
{
    char buf[11];
    int i = 0;
    do {
        buf[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (i)
        uart_putc(buf[--i]);
}
