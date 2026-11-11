/* USART1 on PA9 (TX) / PA10 (RX), AF7: the CAT port. Interrupt-driven RX and TX rings, so a
 * reply never blocks the main loop (a CI-V answer at 19200 baud takes several ms). */
#include "stm32f4.h"
#include "system.h"
#include "uart.h"

#define RX_SIZE 128
#define TX_SIZE 128

#define SR_RXNE (1u << 5)
#define SR_ORE  (1u << 3)
#define SR_TXE  (1u << 7)
#define CR1_TXEIE (1u << 7)

static volatile uint8_t rx_buf[RX_SIZE], tx_buf[TX_SIZE];
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;

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
    if (sr & (SR_RXNE | SR_ORE)) {          /* reading DR clears both */
        uint8_t c = USART1->DR;
        uint32_t next = (rx_head + 1) % RX_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
    if ((sr & SR_TXE) && (USART1->CR1 & CR1_TXEIE)) {
        if (tx_tail == tx_head) {
            USART1->CR1 &= ~CR1_TXEIE;
        } else {
            USART1->DR = tx_buf[tx_tail];
            tx_tail = (tx_tail + 1) % TX_SIZE;
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

void uart_write(const uint8_t *p, unsigned n)
{
    while (n--) {
        uint32_t next = (tx_head + 1) % TX_SIZE;
        while (next == tx_tail)             /* full: wait for the ISR */
            ;
        tx_buf[tx_head] = *p++;
        tx_head = next;
        /* The ISR only clears TXEIE, and only with the ring empty: if it does so inside this
         * read-modify-write, TXEIE comes back set and the ISR just clears it again. */
        USART1->CR1 |= CR1_TXEIE;
    }
}
