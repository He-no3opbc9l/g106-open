/* Receiver UI following stock behaviour; see radio.c for the controls. */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>

void radio_init(void);
void radio_poll(void);              /* call from the main loop */
void radio_set_freq(uint32_t hz);
void radio_set_mode(int mode);      /* audio_mode_t */
uint32_t radio_freq(void);
int radio_mode(void);
void radio_save(void);
/* Stock power key (0x08029d6c): short press toggles the backlight, long press powers off. */
void radio_toggle_backlight(void);
void radio_power_off(void) __attribute__((noreturn));
/* TX test: signal = AUDIO_TX_CARRIER / AUDIO_TX_USB / AUDIO_TX_LSB, power 0..2 (-1 = TX POWER
 * setting). Stops after 30 s, on any key or radio_tx_stop(). */
int radio_tx_start(int signal, int power_idx);
void radio_tx_stop(void);
bool radio_tx_active(void);              /* persist work params + band slots (call before power-off) */

#endif
