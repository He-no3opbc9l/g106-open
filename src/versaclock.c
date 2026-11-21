#include "i2c.h"
#include "system.h"
#include "versaclock.h"

#define VC_ADDR    0x6A
#define VC_XTAL_HZ 26000000u
#define VC_FB_INT  111u                         /* VCO = 2886 MHz */
#define VC_VCO_HZ  (VC_XTAL_HZ * VC_FB_INT)

/* Own FREQ CAL: crystal error in 0.01 ppm (positive = the crystal runs fast). The VCO is an
 * integer multiple of the crystal, so the output dividers are computed from the real VCO. */
static double vco_hz = VC_VCO_HZ;

void vc_set_ref_cal(int cppm)
{
    vco_hz = (double)VC_VCO_HZ * (1.0 + cppm * 1e-8);
}

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

volatile uint32_t vc_errs, vc_last_hz;      /* diagnostics: failed writes, last OUT1 request */
volatile uint32_t vc_init_tries;            /* init attempts of the last vc_init (1 = clean) */
volatile uint32_t vc_verify_fail;           /* register read-back mismatches (init + set_output) */
volatile uint32_t vc_bad_reg;               /* last mismatch: reg << 16 | wrote << 8 | read */

/* OUT1 register shadow: what vc_set_output last asked for (0x22..0x2E), checked by the init read-back. */
static uint8_t out1[13];

/* Read back regs [r, r + n) and compare with exp; 0 = equal. */
static int verify(uint8_t r, const uint8_t *exp, unsigned n)
{
    uint8_t b[32];
    for (unsigned o = 0; o < n; o += sizeof b) {
        unsigned k = n - o < sizeof b ? n - o : sizeof b;
        if (vc_read((uint8_t)(r + o), b, k) < 0)
            return -1;
        for (unsigned i = 0; i < k; i++)
            if (b[i] != exp[o + i]) {
                vc_verify_fail++;
                vc_bad_reg = (uint32_t)(r + o + i) << 16 | (uint32_t)exp[o + i] << 8 | b[i];
                return -1;
            }
    }
    return 0;
}

static int init_once(void)
{
    if (wr(vc_image, sizeof vc_image) < 0 || resync() < 0)
        return -1;
    /* Feedback divider: integer 111, fraction 0 (regs 0x17..0x1B). */
    const uint8_t fb[] = {0x17, VC_FB_INT >> 4, (VC_FB_INT << 4) & 0xFF, 0, 0, 0};
    if (wr(fb, sizeof fb) < 0)
        return -1;
    if (vc_set_output(1, 57080000) < 0)        /* stock start-up value */
        return -1;
    if (resync() < 0)
        return -1;
    /* Stock writes and hopes; we read the whole map back (all of 0x00..0x69 reads as written, checked
     * on the G90 body with the same chip and image), with the feedback and OUT1 dividers we set on top. */
    uint8_t exp[0x6A];
    for (unsigned i = 0; i < sizeof exp; i++)
        exp[i] = vc_image[i + 1];
    for (unsigned i = 1; i < sizeof fb; i++)
        exp[0x17 + i - 1] = fb[i];
    for (unsigned i = 0; i < sizeof out1; i++)
        exp[0x22 + i] = out1[i];
    return verify(0x00, exp, sizeof exp);
}

/* Up to 3 full attempts; a failed attempt starts over from the register image. */
int vc_init(void)
{
    for (vc_init_tries = 1; vc_init_tries <= 3; vc_init_tries++) {
        if (init_once() == 0)
            return 0;
        delay_ms(20);
    }
    return -1;
}

int vc_read(uint8_t reg, uint8_t *buf, unsigned n)
{
    return i2c_write_read(I2C_BUS1, VC_ADDR, &reg, 1, buf, n);
}

/* f_out = f_vco / (2 * (int + frac / 2^24)); field packing identical to stock (0x080312a0; G90 stock 0x0800ac94).
 * Stock writes the integer part (0x2D, 0x2E) and the fraction (0x22..0x25) in two transactions and never reads
 * them back; between the two the divider is new int + old frac (~5 % off when the integer changes, e.g. at
 * 18.0375 MHz RF: 20 -> 19). Ours: one burst 0x22..0x2E (spread-spectrum regs 0x26..0x2C rewritten with their
 * image values), read back, rewritten up to 3 times on a mismatch. */
int vc_set_output(int ch, uint32_t freq_hz)
{
    if (ch < 1 || ch > 4 || !freq_hz)
        return -1;
    double div = vco_hz * 0.5 / freq_hz;
    uint32_t idiv = (uint32_t)div;
    uint32_t frac = (uint32_t)((div - idiv) * 16777216.0 + 0.5);

    uint8_t base = (uint8_t)((ch << 4) + 0x12);
    uint8_t b[14];
    b[0] = base;
    for (unsigned i = 0; i < 13; i++)
        b[1 + i] = vc_image[1 + base + i];
    b[1] = (uint8_t)(frac >> 22);
    b[2] = (uint8_t)(frac >> 14);
    b[3] = (uint8_t)(frac >> 6);
    b[4] = (uint8_t)(frac << 2);
    b[12] = (uint8_t)(idiv >> 4);
    b[13] = (uint8_t)(idiv << 4);
    vc_last_hz = freq_hz;
    if (ch == 1) {
        for (unsigned i = 0; i < 13; i++)
            out1[i] = b[1 + i];
    }
    for (int tries = 0; tries < 3; tries++)
        if (wr(b, sizeof b) == 0 && verify(base, b + 1, 13) == 0)
            return 0;
    vc_errs++;
    return -1;
}
