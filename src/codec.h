/* TLV320AIC3204 on I2C2 @0x18, reset line PC6 (high = run).
 * Register sequences are copied from stock (init 0x0802be20, clocks 0x0802c068,
 * RX routing 0x0802bfb0, TX/mic routing 0x0802c358).
 *
 * Clocks: MCLK 26 MHz -> PLL P=4 R=1 J=15 D=1236 = 98.30 MHz -> NDAC 2, MDAC 8, OSR 128
 * -> fs 48 kHz. Interface: I2S, 32-bit, codec is BCLK/WCLK master (BCLK = 64 fs).
 * Analog: RX I/Q -> MICPGA L/R -> ADC; mic on IN3L (+MICBIAS); DAC L/R -> HPL/HPR
 * (speaker/phones, via digipot @0x2E on I2C1) and LOL/LOR (TX I/Q to the modulator). */
#ifndef CODEC_H
#define CODEC_H

#include <stdbool.h>
#include <stdint.h>

int codec_init(void);
void codec_route_rx_iq(void);         /* RX: I/Q into the ADC, DAC to phones/speaker */
void codec_route_mic_monitor(void);
void codec_route_tx(bool data, uint8_t mic_gain);  /* TX: mic/data in, HP off, LOL/LOR = I/Q */   /* test: mic -> left ADC, DAC still to phones/speaker */
void codec_hp_gain(int db);           /* HPL driver gain -6..29 dB, < -6 = mute (as stock) */
void codec_hpr_gain(int db);          /* HPR driver, same coding (init 0 dB) */
void codec_micpga(uint8_t half_db);   /* MICPGA gain 0..95 (0.5 dB steps) */
int volume_set(uint8_t level);        /* speaker/phones digipot @0x2E, 0..127 */

#endif
