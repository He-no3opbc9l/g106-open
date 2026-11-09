#include "stm32f4.h"
#include "system.h"
#include "beep.h"

#define TIM1_BASE   0x40010000
#define TIM1_CR1    REG32(TIM1_BASE + 0x00)
#define TIM1_EGR    REG32(TIM1_BASE + 0x14)
#define TIM1_CCMR2  REG32(TIM1_BASE + 0x1C)
#define TIM1_CCER   REG32(TIM1_BASE + 0x20)
#define TIM1_PSC    REG32(TIM1_BASE + 0x28)
#define TIM1_ARR    REG32(TIM1_BASE + 0x2C)
#define TIM1_CCR4   REG32(TIM1_BASE + 0x40)
#define TIM1_BDTR   REG32(TIM1_BASE + 0x44)

#define TIM1_CLK_HZ (APB2_HZ * 2)       /* APB2 prescaler != 1 -> timer clock x2 */
#define TIM1_PSC_V  99u

static volatile uint32_t remaining_ms;

void beep_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA;
    RCC->APB2ENR |= 1u << 0;            /* TIM1 */
    __dsb();

    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << 12)) | (1u << 12);  /* PA11 AF1 */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 22)) | (2u << 22);

    TIM1_CR1 = 0;
    TIM1_PSC = TIM1_PSC_V;
    TIM1_ARR = 1000;
    TIM1_CCR4 = 500;
    TIM1_CCMR2 = (6u << 12) | (1u << 11);   /* OC4: PWM mode 1, preload */
    TIM1_CCER = 1u << 12;                   /* CC4E */
    TIM1_BDTR = 0;                          /* MOE off = silent */
    TIM1_EGR = 1;
    TIM1_CR1 = (1u << 7) | 1u;              /* ARPE, CEN */
}

void beep(uint32_t freq_hz, uint32_t ms)
{
    if (!freq_hz || !ms) {
        TIM1_BDTR = 0;
        remaining_ms = 0;
        return;
    }
    uint32_t arr = TIM1_CLK_HZ / (TIM1_PSC_V + 1) / freq_hz;
    TIM1_ARR = arr - 1;
    TIM1_CCR4 = arr / 2;
    remaining_ms = ms;
    TIM1_BDTR = 1u << 15;                   /* MOE */
}

void beep_tick_1ms(void)
{
    if (remaining_ms && --remaining_ms == 0)
        TIM1_BDTR = 0;
}
