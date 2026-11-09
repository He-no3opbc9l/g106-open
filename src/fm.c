#include "i2c.h"
#include "fm.h"

#define FM_ADDR  0x60
#define FM_XTAL  32768u
#define FM_IF    225000u

/* Stock register image 0x20000d18 (5 write bytes):
 *   [0] MUTE | SM | PLL[13:8]   [1] PLL[7:0]
 *   [2] 0x18 = HLSI (high-side LO) | MS (mono)
 *   [3] 0x10 = XTAL (32.768 kHz reference)   [4] 0x00 = PLLREF off, 50 us de-emphasis */
static uint8_t regs[5] = {0x80, 0x00, 0x18, 0x10, 0x00};

static int write_regs(void)
{
    return i2c_write(I2C_BUS1, FM_ADDR, regs, sizeof regs);
}

int fm_init(void)
{
    return write_regs();
}

int fm_mute(bool on)
{
    regs[0] = on ? regs[0] | 0x80 : regs[0] & 0x7F;
    return write_regs();
}

/* Stock tea5767_set_freq: PLL = round(4 * (f +/- IF) / 32768), + IF with HLSI. */
int fm_set_freq(uint32_t hz)
{
    uint32_t lo = regs[2] & 0x10 ? hz + FM_IF : hz - FM_IF;
    uint32_t pll = (4 * lo + FM_XTAL / 2) / FM_XTAL;
    regs[0] = (regs[0] & 0xC0) | ((pll >> 8) & 0x3F);
    regs[1] = pll & 0xFF;
    return write_regs();
}

/* Stock 0x080308d4: 5 read bytes, byte 4 bits 7..4 = level ADC. */
int fm_level(void)
{
    uint8_t r[5];
    if (i2c_write_read(I2C_BUS1, FM_ADDR, 0, 0, r, sizeof r) < 0)
        return -1;
    return r[3] >> 4;
}
