/* Polled I2C master, 100 kHz (as stock).
 *   I2C1: PB6 SCL, PB7 SDA — VersaClock @0x6A, TEA5767 @0x60, 24Cxx EEPROM @0x50, ? @0x2E
 *   I2C2: PB10 SCL, PB11 SDA — TLV320AIC3204 codec @0x18
 * Addresses are 7-bit. Functions return 0 on success, <0 on error/NACK/timeout. */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

enum { I2C_BUS1 = 1, I2C_BUS2 = 2 };

void i2c_init(int bus);
int i2c_probe(int bus, uint8_t addr);
int i2c_write(int bus, uint8_t addr, const uint8_t *data, unsigned len);
/* Write `wlen` bytes (e.g. register address), repeated start, read `rlen` bytes. */
int i2c_write_read(int bus, uint8_t addr, const uint8_t *w, unsigned wlen, uint8_t *r, unsigned rlen);

/* Diagnostics: CR1 CR2 SR1 SR2 now; failures and (re-)inits per bus, CR1 SR1 SR2 at the last
 * failure (every failure resets the peripheral). */
void i2c_regs(int bus, uint32_t r[4]);
extern uint32_t i2c_fails[3], i2c_inits[3], i2c_last[3][3];

#endif
