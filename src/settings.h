/* Persistent state in the stock EEPROM format (see docs/stock_spec.md), so stock and this
 * firmware share settings, memories and calibration. Read-only for now.
 * Deviation from stock: the preamp is one global setting (stock keeps it per VFO/band),
 * stored in our own block so the stock layout stays untouched. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define NUM_BAND_SLOTS 21
#define NUM_MEMORIES   50
#define NUM_FM_CHANNELS 211     /* 87.0 .. 108.0 MHz, 100 kHz */

enum { MODE_LSB, MODE_LD, MODE_USB, MODE_UD, MODE_CW, MODE_CWR, MODE_NFM, MODE_AM, MODE_COUNT };

typedef struct {
    uint16_t flags;         /* stock: 0xAA00 = valid marker, bit0 = preamp */
    uint16_t mode;
    uint32_t freq;
} vfo_t;

/* Stock "work params" at EEPROM 0x0C00 (RAM 0x2000205c in stock). */
typedef struct {
    vfo_t vfo[2];
    uint16_t flags;         /* bit0 memory mode, bit1 lock, bit4 beep, bit5 FM, bit7 PB0,
                               bit8 PC11, bit9 full band */
    int16_t page;           /* soft-key page 0..4 */
    uint8_t band;           /* band slot 0..20 */
    uint8_t mem_ch;         /* 0..49 */
    uint8_t cur_vfo;        /* 0 = A, 1 = B */
    uint8_t _pad0;
    int16_t volume;         /* 0..46 */
    int16_t dsp_opt;        /* 0..2 */
    uint32_t fm_freq;
    uint32_t _rsv;
    int16_t digit[3];       /* tuning-step digit for SSB / CW / AM+NFM modes */
    uint16_t _pad1;
    uint32_t crc;
} workparams_t;

_Static_assert(sizeof(workparams_t) == 0x30, "stock work params layout");
_Static_assert(__builtin_offsetof(workparams_t, volume) == 0x18, "stock work params layout");
_Static_assert(__builtin_offsetof(workparams_t, digit) == 0x24, "stock work params layout");
_Static_assert(__builtin_offsetof(workparams_t, crc) == 0x2C, "stock work params layout");

#define WP_FLAG_MEMORY    (1u << 0)
#define WP_FLAG_LOCK      (1u << 1)
#define WP_FLAG_BEEP      (1u << 4)
#define WP_FLAG_FM        (1u << 5)
#define WP_FLAG_BACKLIGHT (1u << 8)     /* PC11 */
#define WP_FLAG_FULLBAND  (1u << 9)

/* Stock "settings" block at EEPROM 0x0540 (RAM 0x20001fc4), CRC over 16 words at +0x40. */
typedef struct {
    int16_t cw_tone;        /* +0x00 500..1000 Hz */
    int16_t qsk_time;       /* +0x02 0..1000 ms */
    int16_t cw_ratio;       /* +0x04 0..4 = dash/dot 2.0..4.0 */
    int16_t cw_filter;      /* +0x06 0..2 = 50/250/500 Hz */
    uint8_t unk08;          /* +0x08 (default 1) */
    uint8_t disp;           /* +0x09 0..2 = SCOPE+S / SCOPE / BIG SCOPE */
    uint8_t key_speed;      /* +0x0A 5..50 WPM */
    uint8_t key_mode;       /* +0x0B 0..2 = MAN / AUTO-L / AUTO-R */
    uint8_t iambic;         /* +0x0C 0..1 = MODE-A / MODE-B */
    uint8_t _pad0d[3];
    int32_t tx_dc_i;        /* +0x10 TX carrier-null DC offset I (x256 int32 units) */
    int32_t tx_dc_q;        /* +0x14 Q */
    int16_t tx_iq[10][2];   /* +0x18 per-band TX IQ cal {amplitude, phase} (TX IQ CAL) */
    uint32_t crc;
} stock_settings_t;

_Static_assert(sizeof(stock_settings_t) == 0x44, "stock settings layout");
_Static_assert(__builtin_offsetof(stock_settings_t, tx_dc_i) == 0x10, "stock settings layout");

/* Stock "param2" at EEPROM 0x1000 (RAM 0x2000037c), CRC over 1 word at +4. */
typedef struct {
    uint8_t mic_gain;       /* 0..30 = MICPGA 0..15 dB (ours: min(mic_gain, 30)) */
    uint8_t flags;
    uint8_t _pad[2];
    uint32_t crc;
} param2_t;

extern param2_t param2;

extern stock_settings_t stock_settings;
extern vfo_t memories[NUM_MEMORIES];       /* EEPROM 0x0000, valid if (flags & 0xFF00) == 0xAA00 */

#define MEM_VALID(v) (((v).flags & 0xFF00) == 0xAA00)
extern workparams_t wp;
/* Stock FM channel table at EEPROM 0x0800: one byte per 100 kHz from 87.0 MHz, 0xAA = stored
 * (marked by SEEK). */
extern uint8_t fm_channels[NUM_FM_CHANNELS];
extern vfo_t band_slots[NUM_BAND_SLOTS];
extern bool preamp;
/* Own setting FILT: RX/TX audio high cut per mode group, 100 Hz units (SSB / DATA / AM 20..60,
 * NFM 20..30: wider audio no longer fits the 16 kHz channel at 2.5 kHz deviation). */
enum { FILT_SSB, FILT_DATA, FILT_AM, FILT_NFM, FILT_COUNT };
extern uint8_t filt_hi[FILT_COUNT];
extern uint8_t scope_zoom;              /* own setting: scope zoom x10, 10..30 */
extern uint8_t mic_eq[3];               /* own setting: mic EQ low/mid/high, dB + 12 (0..24) */
extern uint8_t comp_level;              /* own setting: speech compressor 0 (off)..10 */
extern uint8_t mic_gain;                /* own setting: MICPGA 0..60 (0.5 dB), stock MG <= 30 */
extern uint8_t agc_mode;                /* own setting: AGC_OFF..AGC_HARD (audio.h) */
extern uint8_t nfm_sql;                 /* own setting: NFM squelch 0 (open)..30, 2 dB steps on the S-meter */

/* Returns 0 if the stock work params were valid (CRC ok), <0 if defaults were used. */
int settings_load(void);
/* Call sign record at EEPROM 0x0500 {0xAA, len, char[16]} (stock CSN); "" if not set. */
void settings_read_callsign(char out[16]);
/* Write the call sign (stock CSN, at most 15 characters; "" clears it). <0 on EEPROM error. */
int settings_write_callsign(const char *s);
int band_slot_of(uint32_t freq);    /* stock 0x08031b98: 0..20 */
/* Write one memory channel (stock format, 8 bytes at ch * 8). <0 on EEPROM error. */
int settings_write_memory(int ch, const vfo_t *v);
/* Save stock blocks (CRC recomputed). <0 on EEPROM error. */
int settings_save_settings(void);
int settings_save_param2(void);
int settings_save_workparams(void);     /* + band slots, as stock does at power-off */
int settings_save_fm_channels(void);
int settings_save_own(void);            /* our block: preamp, SSB filter, page, zoom */
/* Next valid memory from `ch` in direction dir (0 = only `ch` itself), stock 0x08032634.
 * Returns the channel or -1. */
int memory_find(int ch, int dir);
uint32_t band_slot_default(int slot);

#endif
