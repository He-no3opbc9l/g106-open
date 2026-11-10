#include "i2c.h"
#include "system.h"
#include "versaclock.h"

#define VC_ADDR    0x6A
#define VC_XTAL_HZ 26000000u
#define VC_FB_INT  111u                         /* VCO = 2886 MHz */
#define VC_VCO_HZ  (VC_XTAL_HZ * VC_FB_INT)

/* Register image written by stock at power-up (flash 0x08039894): reg address 0, then
 * registers 0x00..0x69. Kept verbatim. */
static const uint8_t vc_image[0x6B] = {
    0x00,
    0x01, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFD, 0xC0, 0x00, 0xB6, 0xB4, 0x92,
    0xE0, 0xCC, 0x01, 0x00, 0x00, 0x84, 0x8C, 0x06, 0xF0, 0x00, 0x00, 0x00, 0x9F, 0xFF, 0xE8, 0x80,
    0x00, 0x81, 0x01, 0x1F, 0x09, 0x24, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x90, 0x12,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x39,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x38, 0x05, 0xBB, 0x04, 0xBB, 0x04, 0x38, 0x04, 0xD7, 0x44,
};

static int wr(const uint8_t *p, unsigned n)
{
    for (int tries = 0; tries < 3; tries++) {
        if (i2c_write(I2C_BUS1, VC_ADDR, p, n) == 0)
            return 0;
        delay_ms(10);
    }
    return -1;
}

static int resync(void)
{
    static const uint8_t a[] = {0x76, 0x43}, b[] = {0x76, 0x63};
    delay_ms(10);
    if (wr(a, 2) < 0)
        return -1;
    delay_ms(10);
    return wr(b, 2);
}

int vc_init(void)
{
    if (wr(vc_image, sizeof vc_image) < 0 || resync() < 0)
        return -1;
    /* Feedback divider: integer 111, fraction 0 (regs 0x17..0x1B). */
    const uint8_t fb[] = {0x17, VC_FB_INT >> 4, (VC_FB_INT << 4) & 0xFF, 0, 0, 0};
    if (wr(fb, sizeof fb) < 0)
        return -1;
    if (vc_set_output(1, 57080000) < 0)        /* stock start-up value */
        return -1;
    return resync();
}

int vc_read(uint8_t reg, uint8_t *buf, unsigned n)
{
    return i2c_write_read(I2C_BUS1, VC_ADDR, &reg, 1, buf, n);
}

/* f_out = f_vco / (2 * (int + frac / 2^24)); packing identical to stock (0x080312a0). */
int vc_set_output(int ch, uint32_t freq_hz)
{
    if (ch < 1 || ch > 4 || !freq_hz)
        return -1;
    double div = (double)VC_VCO_HZ * 0.5 / freq_hz;
    uint32_t idiv = (uint32_t)div;
    uint32_t frac = (uint32_t)((div - idiv) * 16777216.0 + 0.5);

    const uint8_t i[] = {(uint8_t)((ch << 4) + 0x1D), idiv >> 4, (idiv << 4) & 0xFF};
    const uint8_t f[] = {(uint8_t)((ch << 4) + 0x12), frac >> 22, frac >> 14, frac >> 6, (frac << 2) & 0xFF};
    return wr(i, sizeof i) < 0 || wr(f, sizeof f) < 0 ? -1 : 0;
}
