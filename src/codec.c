#include "stm32f4.h"
#include "system.h"
#include "i2c.h"
#include "codec.h"

#define CODEC_ADDR  0x18
#define DIGIPOT_ADDR 0x2E
#define RESET_PIN   6       /* PC6 */

static int page = -1;

static int wr(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    for (int tries = 0; tries < 3; tries++) {
        if (i2c_write(I2C_BUS2, CODEC_ADDR, b, 2) == 0)
            return 0;
        delay_ms(10);
    }
    return -1;
}

static int sel(int p)
{
    if (p == page)
        return 0;
    page = p;
    return wr(0x00, p);
}

/* {page, reg, value}; page 0xFF = delay `value` ms. */
typedef struct {
    uint8_t page, reg, val;
} creg_t;

static int run(const creg_t *t, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (t[i].page == 0xFF) {
            delay_ms(t[i].val);
            continue;
        }
        if (sel(t[i].page) < 0 || wr(t[i].reg, t[i].val) < 0)
            return -1;
    }
    return 0;
}

static const creg_t init_seq[] = {
    {0, 0x01, 0x01},                    /* software reset */
    {0xFF, 0, 2},
    /* clocks for fs = 48 kHz from MCLK 26 MHz */
    {0, 0x04, 0x40},                    /* PLL_CLKIN = MCLK, high PLL range */
    {0, 0x06, 15},                      /* J */
    {0, 0x07, 1236 >> 8},               /* D MSB */
    {0, 0x08, 1236 & 0xFF},             /* D LSB */
    {0, 0x05, 0xC1},                    /* PLL on, P = 4, R = 1 */
    {0, 0x0D, 0x00},                    /* DOSR = 128 */
    {0, 0x0E, 0x80},
    {0, 0x14, 0x80},                    /* AOSR = 128 */
    {0, 0x0B, 0x82},                    /* NDAC = 2, on */
    {0, 0x12, 0x02},                    /* NADC = 2 (off: ADC uses DAC clocks) */
    {0, 0x0C, 0x88},                    /* MDAC = 8, on */
    {0, 0x13, 0x08},                    /* MADC = 8 (off) */
    {0, 0x04, 0x43},                    /* CODEC_CLKIN = PLL */
    {0, 0x1D, 0x00},                    /* BDIV_CLKIN = DAC_CLK */
    {0, 0x1E, 0x90},                    /* BCLK N = 16, on -> 3.072 MHz */
    {0, 0x1B, 0x3C},                    /* I2S, 32 bit, BCLK + WCLK outputs (master) */
    {0xFF, 0, 1},
    {0, 0x19, 0x06},                    /* CDIV_CLKIN (CLKOUT mux), as stock */
    {0, 0x1A, 0x84},                    /* CLKOUT M = 4, on */
    {0, 0x37, 0x06},                    /* MISO/MFP4 function, as stock */
    {0, 0x3C, 0x01},                    /* DAC PRB_P1 */
    {0, 0x3D, 0x01},                    /* ADC PRB_R1 */
    /* analog power */
    {1, 0x01, 0x08},                    /* disable weak AVDD-DVDD link */
    {1, 0x02, 0x01},                    /* analog blocks on, AVDD LDO */
    {1, 0x0A, 0x03},                    /* common mode */
    {1, 0x3D, 0x00},                    /* ADC power tune PTM_R4 */
    {1, 0x03, 0x00},                    /* DAC PTM_P3/4 */
    {1, 0x04, 0x00},
    {1, 0x47, 0x32},                    /* input quick charge */
    {1, 0x7B, 0x01},                    /* reference power-up 40 ms */
    {1, 0x3B, 0x1E},                    /* MICPGA L/R +15 dB */
    {1, 0x3C, 0x1E},
    {0, 0x51, 0xC0},                    /* ADC L+R on */
    {0, 0x52, 0x00},                    /* ADC unmuted */
    {0, 0x53, 0x00},                    /* ADC digital volume 0 dB */
    {0, 0x54, 0x00},
    /* outputs */
    {1, 0x14, 0x25},                    /* headphone driver start-up */
    {1, 0x0C, 0x08},                    /* HPL <- DAC L */
    {1, 0x0D, 0x08},                    /* HPR <- DAC R */
    {1, 0x0E, 0x08},                    /* LOL <- DAC L */
    {1, 0x0F, 0x08},                    /* LOR <- DAC R */
    {1, 0x09, 0x3C},                    /* HPL, HPR, LOL, LOR on */
    {1, 0x12, 0x10},                    /* LOL/LOR +16 dB (stock main: 0x0802c26c(3, 0x10)) */
    {1, 0x13, 0x10},
    {0, 0x41, 0x00},                    /* DAC digital volume 0 dB */
    {0, 0x42, 0x00},
    {0, 0x3F, 0xD6},                    /* DAC L+R on, L->L R->R, soft step */
    {0, 0x40, 0x00},                    /* DAC unmuted */
    {1, 0x10, 0x0A},                    /* HPL +10 dB */
    {1, 0x11, 0x00},                    /* HPR 0 dB */
};

/* stock RX routing (0x0802bfb0): QSD I/Q into the ADC */
static const creg_t rx_seq[] = {
    {1, 0x34, 0x10}, {1, 0x36, 0x50}, {1, 0x37, 0x40}, {1, 0x39, 0x50},
    {1, 0x0C, 0x08}, {1, 0x0D, 0x08}, {1, 0x0E, 0x08}, {1, 0x0F, 0x08},
    {1, 0x3B, 0x1E}, {1, 0x3C, 0x1E},
    {0, 0x53, 0x00}, {0, 0x54, 0x00},
    {1, 0x33, 0x20},                    /* MICBIAS off */
};

/* Mic monitor test: stock TX input selection (0x0802c358, mask 1: IN3L) + MICBIAS,
 * but the DAC stays routed to HPL/HPR so the result is audible. */
static const creg_t mic_seq[] = {
    {1, 0x34, 0x04}, {1, 0x36, 0x40},  /* left MICPGA: IN3L+, CM1L- */
    {1, 0x37, 0x00}, {1, 0x39, 0x00},  /* right MICPGA: nothing */
    {1, 0x0C, 0x08}, {1, 0x0D, 0x08}, {1, 0x0E, 0x08}, {1, 0x0F, 0x08},
    {1, 0x3B, 0x1E}, {1, 0x3C, 0x00},
    {0, 0x53, 0x00}, {0, 0x54, 0x00},
    {1, 0x33, 0x60},                    /* MICBIAS on */
};

/* Stock TX routing (0x0802c358 mask 1 + MICBIAS 0x0802c2b0): mic IN3L -> left MICPGA,
 * HPL/HPR disconnected (speaker silent), LOL/LOR keep the DAC = TX I/Q to the modulator. */
static const creg_t tx_seq[] = {
    {1, 0x34, 0x04}, {1, 0x36, 0x40},
    {1, 0x37, 0x00}, {1, 0x39, 0x00},
    {1, 0x0C, 0x00}, {1, 0x0D, 0x00}, {1, 0x0E, 0x08}, {1, 0x0F, 0x08},
    {1, 0x3B, 0x1E}, {1, 0x3C, 0x00},
    {0, 0x53, 0x00}, {0, 0x54, 0x74},
    {1, 0x33, 0x60},
};

/* data = true: L-D / U-D take the line/data input on IN3R into the right MICPGA, no MICBIAS
 * (stock 0x0802c358 mask 2 + 0x0802c2b0(0)). mic_gain = MIC GAIN -> MICPGA L in 0.5 dB steps
 * (stock 0..30 = 0..15 dB, ours up to 60 = 30 dB; the PGA goes to 95 = 47.5 dB). */
void codec_route_tx(bool data, uint8_t mic_gain)
{
    run(tx_seq, sizeof tx_seq / sizeof tx_seq[0]);
    sel(1);
    wr(0x3B, mic_gain > 95 ? 95 : mic_gain);
    if (data) {
        wr(0x34, 0x00);
        wr(0x36, 0x00);
        wr(0x37, 0x04);                 /* right MICPGA: IN3R+ */
        wr(0x39, 0x40);                 /* CM1R- */
        wr(0x33, 0x20);                 /* MICBIAS off */
    }
}

int codec_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOC;
    __dsb();
    GPIOC->BSRR = 1u << RESET_PIN;
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 2 * RESET_PIN)) | (1u << 2 * RESET_PIN);
    delay_ms(1);
    page = -1;
    if (run(init_seq, sizeof init_seq / sizeof init_seq[0]) < 0)
        return -1;
    codec_route_rx_iq();
    return 0;
}

void codec_route_rx_iq(void)
{
    run(rx_seq, sizeof rx_seq / sizeof rx_seq[0]);
}

void codec_route_mic_monitor(void)
{
    run(mic_seq, sizeof mic_seq / sizeof mic_seq[0]);
}

/* Stock (0x0802c228 with mask 1) changes only HPL; HPR stays at its init gain (0 dB). */
void codec_hp_gain(int db)
{
    uint8_t v = db < -6 ? 0x40 : (uint8_t)((db > 29 ? 29 : db) & 0x3F);
    sel(1);
    wr(0x10, v);
}

void codec_hpr_gain(int db)
{
    uint8_t v = db < -6 ? 0x40 : (uint8_t)((db > 29 ? 29 : db) & 0x3F);
    sel(1);
    wr(0x11, v);
}

void codec_micpga(uint8_t half_db)
{
    sel(1);
    wr(0x3B, half_db > 95 ? 95 : half_db);
}

int volume_set(uint8_t level)
{
    const uint8_t b[2] = {0x00, level > 127 ? 127 : level};
    return i2c_write(I2C_BUS1, DIGIPOT_ADDR, b, 2);
}
