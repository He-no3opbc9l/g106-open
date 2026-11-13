/* Renesas/IDT VersaClock 5/6 on I2C1 @0x6A (reversed from stock FUN_08031150 / 0x080312a0).
 * 26 MHz xtal, VCO = 26 MHz * 111 = 2886 MHz. OUT1 drives the RX QSD at 4 x f. */
#ifndef VERSACLOCK_H
#define VERSACLOCK_H

#include <stdint.h>

int vc_init(void);
int vc_set_output(int ch, uint32_t freq_hz);
/* Own FREQ CAL: reference crystal error in 0.01 ppm; takes effect with the next vc_set_output. */
void vc_set_ref_cal(int cppm);
int vc_read(uint8_t reg, uint8_t *buf, unsigned n);   /* diagnostics */

#endif
