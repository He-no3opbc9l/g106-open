/* Minimal STM32F405/407 register definitions (only what we use). */
#ifndef STM32F4_H
#define STM32F4_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2];
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t SR, DR, BRR, CR1, CR2, CR3, GTPR;
} USART_TypeDef;

typedef struct {
    volatile uint32_t CR1, CR2, SR, DR, CRCPR, RXCRCR, TXCRCR, I2SCFGR, I2SPR;
} SPI_TypeDef;

typedef struct {
    volatile uint32_t CR, PLLCFGR, CFGR, CIR, AHB1RSTR, AHB2RSTR, AHB3RSTR, _r0,
                      APB1RSTR, APB2RSTR, _r1[2], AHB1ENR, AHB2ENR, AHB3ENR, _r2,
                      APB1ENR, APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t CTRL, LOAD, VAL, CALIB;
} SysTick_TypeDef;

#define GPIOA   ((GPIO_TypeDef *)0x40020000)
#define GPIOB   ((GPIO_TypeDef *)0x40020400)
#define GPIOC   ((GPIO_TypeDef *)0x40020800)
#define GPIOD   ((GPIO_TypeDef *)0x40020C00)
#define GPIOH   ((GPIO_TypeDef *)0x40021C00)
#define SPI1    ((SPI_TypeDef *)0x40013000)
#define USART1  ((USART_TypeDef *)0x40011000)
#define RCC     ((RCC_TypeDef *)0x40023800)
#define SYSTICK ((SysTick_TypeDef *)0xE000E010)

#define FLASH_ACR   REG32(0x40023C00)
#define PWR_CR      REG32(0x40007000)
#define SCB_VTOR    REG32(0xE000ED08)
#define SCB_CPACR   REG32(0xE000ED88)
#define SCB_AIRCR   REG32(0xE000ED0C)
#define NVIC_ICER(n) REG32(0xE000E180 + 4 * (n))
#define NVIC_ICPR(n) REG32(0xE000E280 + 4 * (n))

#define RCC_AHB1ENR_GPIOA  (1u << 0)
#define RCC_AHB1ENR_GPIOB  (1u << 1)
#define RCC_AHB1ENR_GPIOC  (1u << 2)
#define RCC_AHB1ENR_GPIOD  (1u << 3)
#define RCC_AHB1ENR_GPIOH  (1u << 7)
#define RCC_APB1ENR_PWR    (1u << 28)
#define RCC_APB2ENR_USART1 (1u << 4)
#define RCC_APB2ENR_SPI1   (1u << 12)

static inline void __disable_irq(void) { __asm volatile("cpsid i" ::: "memory"); }
static inline void __enable_irq(void)  { __asm volatile("cpsie i" ::: "memory"); }
static inline void __dsb(void)         { __asm volatile("dsb" ::: "memory"); }
static inline void __isb(void)         { __asm volatile("isb" ::: "memory"); }

#endif
