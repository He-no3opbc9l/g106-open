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
/* TX with signal AUDIO_TX_* at power 0..2 (-1 = TX POWER setting); used by the PTT logic. */
int radio_tx_start(int signal, int power_idx);
void radio_tx_stop(void);
bool radio_tx_active(void);

/* CAT (cat.c). VFO index: 0 = A, 1 = B. */
bool radio_cat_can_tune(void);          /* false in TX / WFM: no frequency, mode or VFO changes */
uint32_t radio_vfo_freq(int v);
int radio_vfo_mode(int v);
void radio_set_vfo(int v, uint32_t freq, int mode);    /* freq 0 / mode < 0 = keep */
int radio_cur_vfo(void);
void radio_select_vfo(int v);           /* -1 = toggle */
void radio_leave_memory(void);
void radio_copy_vfo(void);              /* other VFO = current */
bool radio_preamp(void);
void radio_set_preamp(bool on);
bool radio_lock(void);
void radio_set_lock(bool on);
int radio_volume(void);                 /* 0..46 */
void radio_set_volume(int v);
int radio_tx_power(void);               /* TXP 0..2 */
void radio_set_tx_power(int idx);
void radio_set_pb0(bool on);
int radio_filter_hz(void);              /* receive filter width of the current mode */

#endif
