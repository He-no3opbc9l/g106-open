/* Clock setup, identical to stock V1.4b01:
 * HSI 16 MHz -> PLL M=8 N=168 P=2 Q=4 -> SYSCLK 168 MHz,
 * AHB /1 = 168 MHz, APB1 /4 = 42 MHz, APB2 /2 = 84 MHz, FLASH 5 WS + caches. */
#include "stm32f4.h"
#include "system.h"

static volatile uint32_t ticks;
void (*volatile tick_hook)(void);

void SystemInit(void)
{
    /* The bootloader may already run from PLL: fall back to HSI before touching it. */
    RCC->CR |= 1u << 0;                         /* HSION */
    while (!(RCC->CR & (1u << 1)))              /* HSIRDY */
        ;
    RCC->CFGR = 0;                              /* SW = HSI, no prescalers */
    while (RCC->CFGR & (3u << 2))               /* SWS != HSI */
        ;
    RCC->CR &= ~(1u << 24);                     /* PLLOFF */
    while (RCC->CR & (1u << 25))
        ;

    RCC->APB1ENR |= RCC_APB1ENR_PWR;
    PWR_CR |= 3u << 14;                         /* VOS scale 1 */

    RCC->PLLCFGR = (4u << 24)                   /* Q = 4 */
                 | (0u << 22)                   /* src = HSI */
                 | (0u << 16)                   /* P = 2 */
                 | (168u << 6)                  /* N = 168 */
                 | 8u;                          /* M = 8 */
    RCC->CR |= 1u << 24;
    while (!(RCC->CR & (1u << 25)))
        ;

    FLASH_ACR = 0x705;                          /* DCEN | ICEN | PRFTEN | 5 WS */
    RCC->CFGR = (4u << 13)                      /* PPRE2 = /2 */
              | (5u << 10)                      /* PPRE1 = /4 */
              | 2u;                             /* SW = PLL */
    while ((RCC->CFGR & (3u << 2)) != (2u << 2))
        ;

    SYSTICK->LOAD = SYSCLK_HZ / 1000 - 1;
    SYSTICK->VAL = 0;
    SYSTICK->CTRL = 7;                          /* CPU clock, IRQ, enable */
}

void SysTick_Handler(void)
{
    ticks++;
    if (tick_hook)
        tick_hook();
}

uint32_t millis(void)
{
    return ticks;
}

void delay_ms(uint32_t ms)
{
    uint32_t start = ticks;
    while (ticks - start < ms)
        ;
}

void system_reset(void)
{
    __dsb();
    SCB_AIRCR = (0x5FAu << 16) | (SCB_AIRCR & 0x700u) | (1u << 2);
    __dsb();
    for (;;)
        ;
}
