/* Broadcast FM receiver chip on I2C1 @0x60, driven in TEA5767 mode like stock
 * (tea5767_* @0x080308dc..0x08030964; the chip also ACKs 0x10/0x11, probably an RDA5807M).
 * Its audio reaches the speaker amplifier directly (stock mutes the codec HP drivers in WFM);
 * the level is set by the digipot @0x2E. */
#ifndef FM_H
#define FM_H

#include <stdbool.h>
#include <stdint.h>

#define FM_MIN  87000000u
#define FM_MAX  108000000u
#define FM_STEP 100000u

int fm_init(void);                  /* stock boot: register image with mute on */
int fm_mute(bool on);
int fm_set_freq(uint32_t hz);
int fm_level(void);                 /* ADC level 0..15 (stock: > 9 = station), <0 on error */

#endif
