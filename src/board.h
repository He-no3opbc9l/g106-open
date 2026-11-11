/* G106 board pins (reversed from stock bootloader + live GPIO dump of V1.4b01).
 *
 *   PC7  out  power latch, 1 = stay on. Clearing it (or any MCU reset) cuts power.
 *   PC8  in   power key, pull-up, 0 = pressed
 *   PC10 out  RX/TX LED: 1 = green (RX), 0 = red (TX); stock also drives PA15 = !PC10
 *   PA9/PA10  USART1 TX/RX (CAT, bootloader IAP)
 *   PA13/PA14 SWD — never reconfigure
 *
 * Bootloader also drives: PA12=1, PC6=1, PA15=0, PB1=0, PC9=0, PC15=0 (roles unknown,
 * left untouched). Full stock pin list: see README.md.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

void board_init(void);
void board_rf_rx_state(void);
void board_rf_off_state(void);
bool power_key_pressed(void);
void power_off(void) __attribute__((noreturn));
void led_set(bool on);
void led_toggle(void);

#endif
