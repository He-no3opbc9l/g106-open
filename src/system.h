#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>

#define SYSCLK_HZ 168000000u
#define APB1_HZ   (SYSCLK_HZ / 4)
#define APB2_HZ   (SYSCLK_HZ / 2)

/* Called from SysTick every 1 ms once set (keep it short). */
extern void (*volatile tick_hook)(void);

uint32_t millis(void);
void delay_ms(uint32_t ms);
void system_reset(void) __attribute__((noreturn));

#endif
