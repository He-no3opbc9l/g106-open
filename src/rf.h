/* RF front end control (reversed from stock rf_set_filters_for_freq @0x0802b160):
 *   two bit-banged shift registers, data PA1, clock PA0, MSB first
 *     latch PA5: one-hot 0x08 <5.5 MHz, 0x04 <11.5, 0x02 <20, 0x01 >=20       (BPF?)
 *     latch PA2: one-hot 1<<n, n = band 1.7-4.2/5-7.6/9.6-15/16.5-22.5/23.5-32/<65/other (LPF?)
 *   DAC1 (PA4): stepped with frequency (varactor / preselector?)
 *   LO: VersaClock OUT1 = 4 x f (Tayloe QSD, zero IF)
 * RX only: nothing here keys the transmitter. */
#ifndef RF_H
#define RF_H

#include <stdint.h>

void rf_init(void);
int rf_set_freq(uint32_t hz);
/* Filters/DAC for the dial frequency `hz`, LO for hz + lo_offset (CW tone, AM +100 Hz). */
int rf_set_freq_lo(uint32_t hz, int32_t lo_offset);
/* WFM: PA2 bank output 6 (stock 0x0802b394(6, 0)); the next rf_set_freq* restores the HF path. */
void rf_fm_path(void);
/* Power-off: all PA5 bank relays released (stock 0x0802b034(0) + PA5 latch). */
void rf_off(void);

#endif
