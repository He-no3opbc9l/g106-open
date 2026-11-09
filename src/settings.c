#include <string.h>
#include "stm32f4.h"
#include "i2c.h"
#include "system.h"
#include "settings.h"
#include "audio.h"

#define EEPROM_ADDR 0x50
#define EE_WORKPARAMS 0x0C00
#define EE_BANDS      0x0400
#define EE_FM         0x0800

workparams_t wp;
stock_settings_t stock_settings;
param2_t param2;
vfo_t memories[NUM_MEMORIES];
vfo_t band_slots[NUM_BAND_SLOTS];
uint8_t fm_channels[NUM_FM_CHANNELS];
bool preamp;

/* Ham bands used for slot numbering (stock table 0x08038b3c). */
static const uint32_t ham[10][2] = {
    {1800000, 2000000}, {3500000, 4000000}, {5330500, 5405000}, {7000000, 7300000},
    {10100000, 10150000}, {14000000, 14350000}, {18068000, 18168000}, {21000000, 21450000},
    {24890000, 24990000}, {28000000, 29700000},
};

/* Default slot contents (stock 0x08038a94): even = general coverage, odd = ham band. */
static const vfo_t slot_defaults[NUM_BAND_SLOTS] = {
    {0, 7, 1500000}, {1, 0, 1800000}, {0, 7, 3150000}, {1, 0, 3500000}, {0, 7, 4500000},
    {1, 0, 5330500}, {0, 7, 6500000}, {1, 0, 7050000}, {0, 7, 9400000}, {1, 2, 10100000},
    {0, 7, 11500000}, {1, 2, 14270000}, {0, 7, 15500000}, {1, 2, 18068000}, {0, 7, 19500000},
    {1, 2, 21000000}, {0, 7, 22500000}, {1, 2, 24890000}, {0, 7, 27500000}, {1, 2, 28000000},
    {0, 7, 30000000},
};

int band_slot_of(uint32_t f)
{
    if (f < 1800000)
        return 0;
    if (f >= 30000000)
        return 20;
    for (int i = 0; i < 10; i++) {
        if (f <= ham[i][1])
            return f >= ham[i][0] ? 2 * i + 1 : 2 * i;
    }
    return 20;
}

uint32_t band_slot_default(int slot)
{
    return slot_defaults[slot].freq;
}

static uint32_t crc_words(const void *data, unsigned nwords);

static int ee_read(uint16_t addr, void *dst, unsigned len)
{
    uint8_t *p = dst;
    while (len) {
        unsigned n = len > 32 ? 32 : len;
        uint8_t a[2] = {addr >> 8, addr & 0xFF};
        if (i2c_write_read(I2C_BUS1, EEPROM_ADDR, a, 2, p, n) < 0)
            return -1;
        addr += n;
        p += n;
        len -= n;
    }
    return 0;
}

/* 24C64: 32-byte pages; after each page wait for the write cycle by polling for ACK.
 * Returns -1 if a page write was not acknowledged, -2 on write-cycle timeout. */
static int ee_write_once(uint16_t addr, const void *src, unsigned len)
{
    const uint8_t *p = src;
    while (len) {
        unsigned n = 32 - (addr & 31);
        if (n > len)
            n = len;
        uint8_t buf[2 + 32] = {addr >> 8, addr & 0xFF};
        memcpy(buf + 2, p, n);
        if (i2c_write(I2C_BUS1, EEPROM_ADDR, buf, 2 + n) < 0)
            return -1;
        int tries = 0;
        while (i2c_probe(I2C_BUS1, EEPROM_ADDR) < 0)
            if (++tries > 200)
                return -2;
        addr += n;
        p += n;
        len -= n;
    }
    return 0;
}

/* Retry a failed block write a couple of times (rare NACKs seen right after other I2C1 traffic). */
static int ee_write(uint16_t addr, const void *src, unsigned len)
{
    int rc = 0;
    for (int tries = 0; tries < 3; tries++) {
        rc = ee_write_once(addr, src, len);
        if (rc == 0)
            return 0;
        delay_ms(10);
        i2c_init(I2C_BUS1);
    }
    return rc;
}

int settings_write_memory(int ch, const vfo_t *v)
{
    if (ch < 0 || ch >= NUM_MEMORIES)
        return -1;
    if (ee_write(ch * sizeof(vfo_t), v, sizeof *v) < 0)
        return -1;
    memories[ch] = *v;
    return 0;
}

int settings_save_fm_channels(void)
{
    return ee_write(EE_FM, fm_channels, sizeof fm_channels);
}

int settings_save_settings(void)
{
    stock_settings.crc = crc_words(&stock_settings, 16);
    return ee_write(0x0540, &stock_settings, sizeof stock_settings);
}

int settings_save_param2(void)
{
    param2.crc = crc_words(&param2, 1);
    return ee_write(0x1000, &param2, sizeof param2);
}

/* Our own block (outside the stock layout, which leaves 0x1F00 unused): global preamp. */
#define EE_OWN      0x1F00
#define OWN_MAGIC   0x4E504F47u     /* "GOPN": v1, CRC over 2 words */
#define OWN_MAGIC2  0x32504F47u     /* "GOP2": v2, CRC over 3 words (+ mic EQ) */
#define OWN_MAGIC3  0x33504F47u     /* "GOP3": v3, CRC over 4 words (+ mic gain, AGC) */
typedef struct {
    uint32_t magic;
    uint8_t preamp;
    uint8_t ssb_filter_hi;      /* SSB/AM high cut in 100 Hz units, 18..60 */
    uint8_t page;               /* soft-key page incl. our pages (stock only knows 0..4) */
    uint8_t scope_zoom;         /* scope zoom x10, 10..30 */
    uint8_t mic_eq[3];          /* v2: mic EQ low / mid / high, dB + 12 (0..24) */
    uint8_t comp;               /* v2: speech compressor 0..10 */
    uint8_t mic_gain;           /* v3: MICPGA 0..60 (0.5 dB); param2 holds min(this, 30) */
    uint8_t agc;                /* v3: AGC_OFF..AGC_HARD */
    uint8_t pad[2];
    uint32_t crc;               /* v1: word 2, v2: word 3, v3: word 4 */
} own_block_t;

uint8_t ssb_filter_hi = 27;
uint8_t scope_zoom = 10;
uint8_t mic_eq[3] = {12, 12, 12};
uint8_t comp_level;
uint8_t mic_gain = 20;
uint8_t agc_mode = AGC_HARD;

int settings_save_own(void)
{
    own_block_t own = {OWN_MAGIC3, preamp, ssb_filter_hi, (uint8_t)wp.page, scope_zoom,
                       {mic_eq[0], mic_eq[1], mic_eq[2]}, comp_level, mic_gain, agc_mode,
                       {0, 0}, 0};
    own.crc = crc_words(&own, 4);
    return ee_write(EE_OWN, &own, sizeof own);
}

int settings_save_workparams(void)
{
    /* Stock indexes its 5-entry soft-key table with this; never store our page 6 there. */
    workparams_t w = wp;
    if (w.page > 4)
        w.page = 4;
    w.crc = crc_words(&w, 11);
    if (ee_write(EE_WORKPARAMS, &w, sizeof w) < 0)
        return -1;
    if (ee_write(EE_BANDS, band_slots, sizeof band_slots) < 0)
        return -1;
    return settings_save_own();
}

int memory_find(int ch, int dir)
{
    if (ch < 0)
        return -1;
    if (dir == 0)
        return ch < NUM_MEMORIES && MEM_VALID(memories[ch]) ? ch : -1;
    int step = dir > 0 ? 1 : -1;
    for (int i = 0; i < NUM_MEMORIES; i++) {
        if (ch < NUM_MEMORIES && MEM_VALID(memories[ch]))
            return ch;
        ch += step;
        ch = ch < 0 ? NUM_MEMORIES - 1 : ch >= NUM_MEMORIES ? 0 : ch;
    }
    return -1;
}

/* STM32 hardware CRC unit, as stock (CRC_ResetDR + CRC_CalcBlockCRC). */
static uint32_t crc_words(const void *data, unsigned nwords)
{
    RCC->AHB1ENR |= 1u << 12;
    __dsb();
    REG32(0x40023008) = 1;                      /* CRC_CR.RESET */
    const uint32_t *w = data;
    while (nwords--)
        REG32(0x40023000) = *w++;
    return REG32(0x40023000);
}

static void workparams_defaults(void)
{
    memset(&wp, 0, sizeof wp);
    wp.vfo[0] = (vfo_t){0, MODE_USB, 14270000};
    wp.vfo[1] = (vfo_t){0, MODE_LSB, 7050000};
    wp.flags = 0x100;
    wp.volume = 24;
    wp.dsp_opt = 2;
    wp.fm_freq = 95800000;
    wp.digit[0] = 3;
    wp.digit[1] = 2;
    wp.digit[2] = 4;
}

int settings_load(void)
{
    int ok = ee_read(EE_WORKPARAMS, &wp, sizeof wp) == 0 &&
             crc_words(&wp, 11) == wp.crc;
    if (!ok)
        workparams_defaults();
    if (wp.cur_vfo > 1)
        wp.cur_vfo = 0;
    for (int i = 0; i < 2; i++) {
        if (wp.vfo[i].mode >= MODE_COUNT)
            wp.vfo[i].mode = MODE_USB;
        if (wp.vfo[i].freq < 550000 || wp.vfo[i].freq > 30000000)
            wp.vfo[i].freq = 7050000;
    }
    for (int i = 0; i < 3; i++)
        if (wp.digit[i] < 0 || wp.digit[i] > 7)
            wp.digit[i] = 3;
    if (wp.page < 0 || wp.page > 4)
        wp.page = 0;
    if (wp.volume < 0 || wp.volume > 46)
        wp.volume = 24;

    if (ee_read(EE_FM, fm_channels, sizeof fm_channels) < 0)
        memset(fm_channels, 0, sizeof fm_channels);         /* as stock WFM entry */
    if (wp.fm_freq < 87000000 || wp.fm_freq > 108000000)
        wp.fm_freq = 87000000;

    if (ee_read(0x0000, memories, sizeof memories) < 0)
        memset(memories, 0, sizeof memories);
    for (int i = 0; i < NUM_MEMORIES; i++)          /* unusable records count as empty (RAM only) */
        if (MEM_VALID(memories[i]) && (memories[i].freq < 550000 || memories[i].freq > 30000000 ||
                                       memories[i].mode >= MODE_COUNT))
            memories[i].flags &= 0x00FF;
    if (wp.mem_ch >= NUM_MEMORIES)
        wp.mem_ch = NUM_MEMORIES - 1;

    /* Band slots: validate like stock (frequency must fall into its own slot). */
    if (ee_read(EE_BANDS, band_slots, sizeof band_slots) < 0)
        memcpy(band_slots, slot_defaults, sizeof band_slots);
    for (int i = 0; i < NUM_BAND_SLOTS; i++)
        if (band_slots[i].freq < 550000 || band_slots[i].freq > 30000000 ||
            band_slot_of(band_slots[i].freq) != i || band_slots[i].mode >= MODE_COUNT)
            band_slots[i] = slot_defaults[i];

    if (ee_read(0x0540, &stock_settings, sizeof stock_settings) < 0 ||
        crc_words(&stock_settings, 16) != stock_settings.crc) {
        /* stock defaults (boot 0x0803458c) */
        memset(&stock_settings, 0, sizeof stock_settings);
        stock_settings.cw_tone = 800;
        stock_settings.qsk_time = 100;
        stock_settings.cw_ratio = 2;
        stock_settings.unk08 = 1;
        stock_settings.key_speed = 20;
        stock_settings.iambic = 1;
    }
    #define CLAMP(v, lo, hi) ((v) = (v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    CLAMP(stock_settings.cw_tone, 500, 1000);
    CLAMP(stock_settings.qsk_time, 0, 1000);
    CLAMP(stock_settings.cw_ratio, 0, 4);
    CLAMP(stock_settings.cw_filter, 0, 2);
    CLAMP(stock_settings.disp, 0, 2);
    CLAMP(stock_settings.key_speed, 5, 50);
    CLAMP(stock_settings.key_mode, 0, 2);
    CLAMP(stock_settings.iambic, 0, 1);

    if (ee_read(0x1000, &param2, sizeof param2) < 0 || crc_words(&param2, 1) != param2.crc) {
        memset(&param2, 0, sizeof param2);
        param2.mic_gain = 20;
    }
    CLAMP(param2.mic_gain, 0, 30);
    CLAMP(wp.dsp_opt, 0, 2);
    mic_gain = param2.mic_gain;

    /* Global preamp: our own block, else start from the stock per-VFO flag. */
    own_block_t own;
    const uint32_t *ow = (const uint32_t *)&own;
    bool own_ok = false;
    if (ee_read(EE_OWN, &own, sizeof own) == 0) {
        bool v3 = own.magic == OWN_MAGIC3 && crc_words(&own, 4) == ow[4];
        bool v2 = own.magic == OWN_MAGIC2 && crc_words(&own, 3) == ow[3];
        bool v1 = own.magic == OWN_MAGIC && crc_words(&own, 2) == ow[2];    /* no EQ yet */
        own_ok = v1 || v2 || v3;
        if (v2 || v3) {
            for (int k = 0; k < 3; k++)
                mic_eq[k] = own.mic_eq[k] <= 24 ? own.mic_eq[k] : 12;
            comp_level = own.comp <= 10 ? own.comp : 0;
        }
        if (v3) {
            /* Gain above stock's 30 only if stock left MIC GAIN at 30 (else stock changed it). */
            if (own.mic_gain > 30 && own.mic_gain <= 60 && param2.mic_gain == 30)
                mic_gain = own.mic_gain;
            agc_mode = own.agc <= AGC_HARD ? own.agc : AGC_HARD;
        }
    }
    if (own_ok) {
        preamp = own.preamp & 1;
        ssb_filter_hi = own.ssb_filter_hi >= 18 && own.ssb_filter_hi <= 60 ? own.ssb_filter_hi : 27;
        scope_zoom = own.scope_zoom >= 10 && own.scope_zoom <= 30 ? own.scope_zoom : 10;
        if (own.page >= 5 && own.page <= 6 && wp.page == 4)
            wp.page = own.page;                 /* our pages 6/7 survive power cycles */
    } else {
        preamp = wp.vfo[wp.cur_vfo].flags & 1;
    }
    return ok ? 0 : -1;
}

/* Stock boot (0x0803458c): length = min(len, strlen), at most 15 characters. */
void settings_read_callsign(char out[16])
{
    uint8_t rec[0x12];
    out[0] = 0;
    if (ee_read(0x0500, rec, sizeof rec) < 0 || rec[0] != 0xAA)
        return;
    unsigned n = 0;
    while (n < 15 && n < rec[1] && rec[2 + n])
        n++;
    memcpy(out, &rec[2], n);
    out[n] = 0;
}
