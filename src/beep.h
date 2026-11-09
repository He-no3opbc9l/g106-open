/* Beeper: TIM1 CH4 PWM on PA11 (AF1), as stock (PSC 99 -> 1.68 MHz count clock). */
#ifndef BEEP_H
#define BEEP_H

#include <stdint.h>

void beep_init(void);
void beep(uint32_t freq_hz, uint32_t ms);
void beep_tick_1ms(void);   /* call from SysTick */

#endif
