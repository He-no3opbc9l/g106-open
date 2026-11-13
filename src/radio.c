/* Main receiver screen and controls, following stock behaviour (docs/stock_spec.md):
 *   screen: stock layout (0x08034374): top bar icons + mode, VFO label + big frequency with the
 *           tuning-step digit inverted, other VFO below, S-meter at y 42
 *   tune encoder: frequency += 10^digit (550 kHz .. 30 MHz), ignored when locked;
 *                 memory mode: next/previous stored channel;
 *                 while the soft-key panel is open it selects the page (1..5)
 *   tune push: open/close the soft-key panel       aux encoder: volume 0..46 (stock curve)
 *   MODE short: next enabled mode (LSB L-D USB U-D CW NFM AM)   MODE long: preamp on/off
 *   BAND-/BAND+ short: band slot (memory mode: channel) down/up   long: step digit left/right
 *   BAND- + BAND+: lock      volume push: PB0 toggle (stock)
 *   F1..F4: soft keys of the current page; the first press only opens the panel (stock);
 *           page 6 (own): FILT = RX/TX audio filter of the current mode (SSB, DATA, AM 2.0..6.0 kHz,
 *                         NFM 2.0..3.0 kHz; CW opens CWF),
 *                         AGC = receive AGC speed OFF / SOFT / MED / HARD (stock),
 *                         SQL = NFM squelch OFF / 1..30 (S-meter threshold, arrow on the meter),
 *                         SETT = SETTINGS menu (categories RX / TX / CW / DISPLAY / SYSTEM: all
 *                         own and advanced settings, see the SETTINGS menu section below)
 *   WFM (page 5 F1, stock 0x08033990): broadcast FM 87..108 MHz on the FM chip, see fm_key()
 *   dialogs: MW / MC (QUIT = F1, SAVE / CLR = F4, tune encoder = channel), VER, message box,
 *            setting editor (stock 0x080313a0: QUIT = F1, SAVE = F4 or tune push)
 * Deviations from stock: the preamp is global, not per VFO/band; NFM (RX + TX) is enabled. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyer.h"
#include <math.h>
#include "stm32f4.h"
#include "system.h"
#include "display.h"
#include "input.h"
#include "beep.h"
#include "audio.h"
#include "codec.h"
#include "rf.h"
#include "settings.h"
#include "radio.h"
#include "spectrum.h"
#include "fm.h"
#include "board.h"
#include "cat.h"
#include "i2c.h"
#include "versaclock.h"

#define F_MIN 550000u
#define F_MAX 30000000u

#define PANEL_MS 3000
#define POPUP_MS 2000
#define MSGBOX_MS 2000
#define BLINK_MS 500
#define BRAND "G106 OPEN"                /* caption when no call sign is set */

/* Boot / power-off caption (stock boot 0x0803458c): the call sign, else the brand line, centred
 * in (0,49)-(127,63). */
static void draw_caption(void)
{
    char cs[16];
    settings_read_callsign(cs);
    const char *t = cs[0] ? cs : BRAND;
    lcd_text_f(&font8, (128 - lcd_text_width(&font8, t)) / 2, 49 + (15 - font8.height) / 2, t, false);
}

typedef enum { UI_MAIN, UI_MW, UI_MC, UI_VER, UI_MSGBOX, UI_EDIT, UI_FM, UI_CSN, UI_MENU, UI_TXCAL } ui_state_t;

/* Stock setting editor descriptor (0x080313a0): range, label, value text, storage. */
typedef enum { ST_SETTINGS, ST_PARAM2, ST_WORKPARAMS, ST_OWN } store_t;
typedef struct {
    int min, max;
    const char *label;
    const char *fmt;                /* printf format, or NULL if names are used;
                                       "%k" = value in 100 Hz shown as kHz with one decimal,
                                       "%z" = value x10 shown as a zoom factor "2.5x",
                                       "%e" = EQ value 0..24 shown as -12..+12 dB,
                                       "%h" = value in 0.5 dB shown as "12.5dB",
                                       "%o" = number, 0 shown as "OFF",
                                       "%r" = signed offset from the middle of the range,
                                       "%p" = 0.01 ppm shown as "+1.23ppm" */
    const char *const *names;       /* value names indexed by value */
    void *var;
    uint8_t size;                   /* 1, 2 = int16 */
    store_t store;
} setting_t;

static const char *const cw_flt_names[] = {"50Hz ", "250Hz", "500Hz"};
static const char *const cw_ratio_names[] = {"2.0", "2.5", "3.0", "3.5", "4.0"};
static const char *const key_mode_names[] = {"MAN", "AUTO-L", "AUTO-R"};
static const char *const iambic_names[] = {"MODE-A", "MODE-B"};
static const char *const disp_names[] = {"SCOPE+S", "SCOPE", "BIG SCOPE:)"};
static const char *const txp_names[] = {"LOW ", "MID ", "HIGH"};

static const setting_t set_cw_flt = {0, 2, "CW FLT:", NULL, cw_flt_names,
                                     &stock_settings.cw_filter, 2, ST_SETTINGS};
static const setting_t set_cw_tone = {500, 1000, "CW TONE:", "%d Hz", NULL,
                                      &stock_settings.cw_tone, 2, ST_SETTINGS};
static const setting_t set_cw_ratio = {0, 4, "CW RATIO:", NULL, cw_ratio_names,
                                       &stock_settings.cw_ratio, 2, ST_SETTINGS};
static const setting_t set_qsk_time = {0, 1000, "QSK TIME:", "%d ms", NULL,
                                       &stock_settings.qsk_time, 2, ST_SETTINGS};
static const setting_t set_key_speed = {5, 50, "KEY SPEED:", "%d WPM", NULL,
                                        &stock_settings.key_speed, 1, ST_SETTINGS};
static const setting_t set_key_mode = {0, 2, "M/L/R:", NULL, key_mode_names,
                                       &stock_settings.key_mode, 1, ST_SETTINGS};
static const setting_t set_iambic = {0, 1, "IAMBIC:", NULL, iambic_names,
                                     &stock_settings.iambic, 1, ST_SETTINGS};
static const setting_t set_disp = {0, 2, "DISP:", NULL, disp_names,
                                   &stock_settings.disp, 1, ST_SETTINGS};
/* MG: stock 0..30 (MICPGA 0..15 dB), ours up to 60 = +30 dB; param2 keeps min(v, 30). */
static const setting_t set_mic_gain = {0, 60, "MIC GAIN:", "%h", NULL, &mic_gain, 1, ST_OWN};
/* Own (not in stock): SSB/AM receive high cut, 1.8..6.0 kHz in 0.1 kHz steps. */
static const setting_t set_filt[FILT_COUNT] = {
    {20, 60, "FILT SSB:", "%k", NULL, &filt_hi[FILT_SSB], 1, ST_OWN},
    {20, 60, "FILT DATA:", "%k", NULL, &filt_hi[FILT_DATA], 1, ST_OWN},
    {20, 60, "FILT AM:", "%k", NULL, &filt_hi[FILT_AM], 1, ST_OWN},
    {20, 30, "FILT NFM:", "%k", NULL, &filt_hi[FILT_NFM], 1, ST_OWN},
};
/* Own: scope zoom around the centre, 1.0x..3.0x in 0.1x steps. */
static const setting_t set_zoom = {10, 30, "SCOPE ZOOM:", "%z", NULL, &scope_zoom, 1, ST_OWN};
/* Own: mic equaliser bands, -12..+12 dB (stored +12). */
static const setting_t set_eq_low = {0, 24, "MIC LOW:", "%e", NULL, &mic_eq[0], 1, ST_OWN};
static const setting_t set_eq_mid = {0, 24, "MIC MID:", "%e", NULL, &mic_eq[1], 1, ST_OWN};
static const setting_t set_eq_high = {0, 24, "MIC HIGH:", "%e", NULL, &mic_eq[2], 1, ST_OWN};
static const char *const comp_names[] = {"OFF", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
static const setting_t set_comp = {0, 10, "COMP:", NULL, comp_names, &comp_level, 1, ST_OWN};
/* Own: receive AGC speed (HARD = stock). */
static const char *const agc_names[] = {"OFF ", "SOFT", "MED ", "HARD"};
static const setting_t set_agc = {AGC_OFF, AGC_HARD, "AGC:", NULL, agc_names, &agc_mode, 1, ST_OWN};
/* Own: NFM squelch (stock has no NFM): opens when the S-meter level reaches nfm_sql x 2 dB
 * (3 steps per S unit: 24 = S9, 30 = S9+12); shown as an arrow over the S-meter. */
static const setting_t set_sql = {0, 30, "NFM SQL:", "%o", NULL, &nfm_sql, 1, ST_OWN};
/* Own: AM TX modulation depth (of the RMS-levelled mic signal), 0..100 %. */
static const setting_t set_am_mod = {0, 100, "AM MOD:", "%d%%", NULL, &am_mod, 1, ST_OWN};
/* Own diagnostic (not stored): AM TX modulated by a 1 kHz sine at exactly AM MOD instead of the mic. */
static uint8_t am_tone;
static const char *const off_on_names[] = {"OFF", "ON "};
static const setting_t set_am_tone = {0, 1, "AM TONE:", NULL, off_on_names, &am_tone, 1, ST_OWN};
static const setting_t set_tx_power = {0, 2, "TX POWER:", NULL, txp_names,
                                       &wp.dsp_opt, 2, ST_WORKPARAMS};
/* Own: LCD contrast, -10..+10 around the stock register value 0x38 (2 per step). */
static const setting_t set_contrast = {0, CONTRAST_MAX, "CONTRAST:", "%r", NULL,
                                       &lcd_contrast_lvl, 1, ST_OWN};
/* Own: FREQ CAL, VersaClock reference crystal error (+ = the crystal runs fast, the radio
 * would be tuned too high). */
static const setting_t set_freq_cal = {-FREQ_CAL_MAX, FREQ_CAL_MAX, "FREQ CAL:", "%p", NULL,
                                       &freq_cal, 2, ST_OWN};
static const float tx_power[3] = {0.2f, 0.4f, 1.0f};      /* stock 0x10000080 */
/* Own: AM drive. The PA is linear up to about the MID CW level (CW carrier LOW 7.0 / MID 12.7 /
 * HIGH 15.2 Vrms): with the stock factors the AM carrier (1/2 of the peak) sat in saturation at
 * HIGH and the modulation was crushed (1 kHz test tone loud on LOW, almost gone on HIGH).
 * Here the AM envelope peak at HIGH = the MID CW level (carrier ~1 W, PEP ~4 W). */
static const float tx_power_am[3] = {0.2f, 0.3f, 0.4f};

static void apply_contrast(void)
{
    lcd_contrast(0x38 + 2 * (lcd_contrast_lvl - CONTRAST_STOCK));
}

static const char *const mode_names[MODE_COUNT] = {"LSB", "L-D", "USB", "U-D", "CW ", "CWR",
                                                   "NFM", "AM "};
/* Stock mode table 0x08038a54 disables CWR and NFM for the MODE key; NFM is ours. */
static const bool mode_enabled[MODE_COUNT] = {1, 1, 1, 1, 1, 0, 1, 1};

/* Soft keys (stock table 0x08038c44): labels per page; actions in softkey(). */
#define NUM_PAGES 6                     /* stock has 5; page 6 is ours */
static const char *const softkey_labels[NUM_PAGES][4] = {
    {"V/M", "A/B", "MW", "MC"},
    {"CWF", "CWT", "CWR", "QSK"},
    {"KS", "KM", "IMB", "CSN"},
    {"SPL", "DIS", "BP", "VER"},
    {"WFM", "BSM", "MG", "TXP"},
    {"FILT", "AGC", "SQL", "SETT"},
};
static const char *const mw_labels[4] = {"QUIT", " ", " ", "SAVE"};   /* stock 0x08038d04 */
static const char *const mc_labels[4] = {"QUIT", " ", " ", "CLR"};    /* stock 0x08038d40 */

static bool dirty = true;
static ui_state_t state = UI_MAIN;
static bool menu_back;                       /* CSN / VER were opened from the SETTINGS menu */
static bool ee_ok;                           /* stock work params valid at boot (VERSION screen) */
static float smeter_lvl;                    /* stock 0x200006b8 smoothing state */
static int smeter_peak, smeter_hold;
static uint16_t scope_state[SCOPE_COLS];     /* smoothed column heights (stock 0x20002408) */
static uint32_t panel_until, popup_until, msgbox_until;   /* millis() deadlines, 0 = closed */
static const char *popup_text, *msgbox_title, *msgbox_text;
#define disp_mode (stock_settings.disp)          /* 0..2: SCOPE+S / SCOPE / BIG SCOPE */
static const setting_t *edit;                /* setting being edited (UI_EDIT) */
static int edit_value, edit_orig;
static int dlg_ch;                           /* channel shown in MW / MC */
static vfo_t mem_work;                       /* working copy of the recalled memory */

static bool memory_mode(void)
{
    return wp.flags & WP_FLAG_MEMORY;
}

static vfo_t *cur(void)
{
    return memory_mode() ? &mem_work : &wp.vfo[wp.cur_vfo];
}

static float tx_drive(int idx)
{
    return cur()->mode == MODE_AM ? tx_power_am[idx] : tx_power[idx];
}

static int mode_class(int mode)
{
    return mode <= MODE_UD ? 0 : mode <= MODE_CWR ? 1 : 2;
}

static int16_t *digit(void)
{
    return &wp.digit[mode_class(cur()->mode)];
}

/* Stock volume curve (0x0802b3d4): 0..46 -> digipot 0..127, 0 mutes the HP driver. */
static void apply_volume(void)
{
    int v = wp.volume, pot;
    if (v <= 9)
        pot = v;
    else if (v < 20)
        pot = 2 * v - 10;
    else if (v < 30)
        pot = 3 * (v - 10);
    else if (v < 46)
        pot = 4 * v - 60;
    else
        pot = 127;
    if (state == UI_FM)
        fm_mute(v == 0);                /* WFM: HP drivers stay muted, the FM chip mutes */
    else
        codec_hp_gain(v ? 28 : -7);
    volume_set(pot);
}

static void apply_preamp(void)
{
    GPIOC->BSRR = preamp ? 1u << 9 : 1u << (9 + 16);
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 18)) | (1u << 18);
}

static void apply_pb0(void)
{
    GPIOB->BSRR = wp.flags & 0x80 ? 1u : 1u << 16;
    GPIOB->MODER = (GPIOB->MODER & ~3u) | 1u;
}

/* LO offset per mode in RX, as stock radio_trx_task: CW -tone, CWR +tone, AM +100 Hz;
 * NFM (own) +100 Hz too, so an unmodulated carrier is not taken out by the DC block. */
static bool txing;

/* Split (stock flags bit2): RX on the current VFO, TX on the other one (frequency only; the mode
 * stays the current VFO's). Not in memory mode: stock refuses TX there (error beep). */
static uint32_t tx_freq(void)
{
    return (wp.flags & WP_FLAG_SPLIT) && !memory_mode() ? wp.vfo[!wp.cur_vfo].freq : cur()->freq;
}

static int32_t lo_offset(void)
{
    if (txing)
        return 0;                       /* TX: carrier exactly on the dial frequency */
    switch (cur()->mode) {
    case MODE_CW: return -stock_settings.cw_tone;
    case MODE_CWR: return stock_settings.cw_tone;
    case MODE_AM:
    case MODE_NFM: return 100;
    default: return 0;
    }
}

static void apply_freq(void);
static void fm_enter(void);
static void toggle_vm(void);

/* FILT group of a mode, -1 for CW / CWR (they use CWF; the band-pass is not in their chain). */
static int filt_group(int mode)
{
    switch (mode) {
    case MODE_LSB: case MODE_USB: return FILT_SSB;
    case MODE_LD: case MODE_UD: return FILT_DATA;
    case MODE_AM: return FILT_AM;
    case MODE_NFM: return FILT_NFM;
    default: return -1;
    }
}

/* CW band-pass and own FILT; safe while transmitting (no routing / mode change). */
static void apply_filters(void)
{
    static const float cw_bw[3] = {50, 250, 500};
    audio_set_cw_filter(stock_settings.cw_tone, cw_bw[stock_settings.cw_filter]);
    /* Own FILT per mode group (RX and TX share the taps); NFM from 300 Hz: no CTCSS hum. */
    int fg = filt_group(cur()->mode);
    audio_set_rx_filter(fg == FILT_NFM ? 300 : 20, (fg < 0 ? filt_hi[FILT_SSB] : filt_hi[fg]) * 100.0f);
}

static void apply_mode(void)
{
    static const int demod[MODE_COUNT] = {AUDIO_LSB, AUDIO_LSB, AUDIO_USB, AUDIO_USB,
                                          AUDIO_CW, AUDIO_CWR, AUDIO_NFM, AUDIO_AM};
    codec_route_rx_iq();
    audio_set_gain(1.0f);
    apply_filters();
    audio_set_mode(demod[cur()->mode]);
    apply_freq();                       /* LO offset depends on the mode */
}

/* Band of the stock per-band TX IQ table (0x08038bb8): <2, 2-4, 4-7, 7-10, 10-14, 14-18, 18-21,
 * 21-24, 24-28, 28-30 MHz. Stock's first entry starts at 5.5 MHz (a typo: nothing matches below
 * 2 MHz there); here band 0 covers everything below 2 MHz. */
static int txiq_band(uint32_t f)
{
    static const uint32_t lim[9] = {2000000, 4000000, 7000000, 10000000, 14000000, 18000000,
                                    21000000, 24000000, 28000000};
    int b = 0;
    while (b < 9 && f >= lim[b])
        b++;
    return b;
}

static void apply_txiq(void)
{
    const int16_t *c = stock_settings.tx_iq[txiq_band(tx_freq())];
    audio_set_tx_iq(c[0], c[1]);
}

static void apply_freq(void)
{
    apply_txiq();
    rf_set_freq_lo(txing ? tx_freq() : cur()->freq, lo_offset());
    if (!memory_mode())
        wp.band = band_slot_of(cur()->freq);
    dirty = true;
}

static void apply_all(void)
{
    apply_mode();
}

void radio_set_freq(uint32_t hz)
{
    cur()->freq = hz < F_MIN ? F_MIN : hz > F_MAX ? F_MAX : hz;
    apply_freq();
}

static bool fm_channels_changed;

/* Stock power-off (0x08029d6c) saves the FM channels, work params and band slots. */
void radio_save(void)
{
    if (state == UI_FM)
        fm_mute(true);
    if (fm_channels_changed && settings_save_fm_channels() == 0)
        fm_channels_changed = false;
    settings_save_workparams();
}

void radio_toggle_backlight(void)
{
    wp.flags ^= WP_FLAG_BACKLIGHT;      /* saved with the work params at power-off */
    lcd_backlight(wp.flags & WP_FLAG_BACKLIGHT);
    if (wp.flags & WP_FLAG_BEEP)
        beep(523, 100);                 /* stock sequence 0x08038a24 */
}

/* Stock power-off (0x08029d6c): save, mute, RF lines and relays off, falling three-tone beep
 * (0x080389dc: 392, 330, 262 Hz x 100 ms) under the power-off screen for 2 s, backlight off,
 * drop the latch. */
void radio_power_off(void)
{
    radio_tx_stop();
    radio_save();
    fm_mute(true);
    delay_ms(50);
    codec_hp_gain(-7);
    codec_hpr_gain(-7);
    board_rf_off_state();
    rf_off();
    lcd_clear();
    /* "73" in the big WFM digits, the call sign (or brand line) under it as on the boot screen. */
    lcd_text_f(&font_fm, (128 - lcd_text_width(&font_fm, "73")) / 2, 8, "73", false);
    draw_caption();
    lcd_flush();
    uint32_t t0 = millis();
    if (wp.flags & WP_FLAG_BEEP) {
        static const uint16_t tones[3] = {392, 330, 262};
        for (int k = 0; k < 3; k++) {
            beep(tones[k], 100);
            delay_ms(100);
        }
    }
    while (millis() - t0 < 2000)
        ;
    lcd_backlight(false);
    power_off();
}

uint32_t radio_freq(void)
{
    return cur()->freq;
}

int radio_mode(void)
{
    return cur()->mode;
}

void radio_set_mode(int m)
{
    cur()->mode = m;
    apply_mode();
}

/* Recall channel `ch` into the working copy (memory mode). */
static void memory_recall(int ch)
{
    wp.mem_ch = ch;
    mem_work = memories[ch];
    apply_all();
}

static int wrap_ch(int ch)
{
    return ch < 0 ? NUM_MEMORIES - 1 : ch >= NUM_MEMORIES ? 0 : ch;
}

/* Memory mode: step to the next valid channel in direction dir (stock 0x080263fa + 0x08032634). */
static void memory_step(int dir)
{
    int ch = memory_find(wrap_ch(wp.mem_ch + dir), dir);
    if (ch >= 0)
        memory_recall(ch);
}

/* ---- TX ----
 * Stock radio_trx_task order. RX->TX: PB1 (T/R) = 1, codec to TX routing, PA8 = 0, PC5 (PA)
 * = 1, LED red (PC10 = 0), PA15 = 1. TX->RX in reverse. Own addition: 10 ms for the T/R relay
 * before the PA is enabled and a 10 ms output ramp on both edges. */
static void pin(GPIO_TypeDef *port, int n, bool on)
{
    port->BSRR = on ? 1u << n : 1u << (n + 16);
}

bool radio_tx_active(void)
{
    return txing;
}

int radio_tx_start(int signal, int power_idx)
{
    if (txing)
        return 0;
    if (state == UI_FM)
        return -1;
    if ((wp.flags & WP_FLAG_SPLIT) && memory_mode()) {      /* stock 0x0802a136: no TX in split + memory */
        static uint32_t last_beep;
        if (!last_beep || millis() - last_beep > 1000) {
            last_beep = millis() | 1;
            beep(523, 100);
        }
        return -1;
    }
    if (power_idx < 0 || power_idx > 2)
        power_idx = wp.dsp_opt;
    audio_tx_ramp(false);
    audio_set_tx(tx_drive(power_idx), stock_settings.tx_dc_i, stock_settings.tx_dc_q);
    rf_set_freq_lo(tx_freq(), 0);       /* TX: no CW / AM LO offset */
    bool data = cur()->mode == MODE_LD || cur()->mode == MODE_UD;
    audio_set_tx_source(data);
    audio_set_mode(signal);

    pin(GPIOB, 1, true);                /* T/R */
    codec_route_tx(data, mic_gain);
    pin(GPIOA, 8, false);
    delay_ms(10);
    pin(GPIOC, 5, true);                /* PA on */
    pin(GPIOC, 10, false);              /* LED red */
    pin(GPIOA, 15, true);
    audio_tx_ramp(signal != AUDIO_TX_CARRIER);     /* CW: the keyer ramps the carrier */
    txing = true;
    dirty = true;
    return 0;
}

void radio_tx_stop(void)
{
    if (!txing)
        return;
    audio_tx_ramp(false);
    delay_ms(15);
    pin(GPIOC, 5, false);               /* PA off */
    pin(GPIOA, 8, true);
    delay_ms(5);
    pin(GPIOB, 1, false);               /* T/R back to RX */
    pin(GPIOC, 10, true);               /* LED green */
    pin(GPIOA, 15, false);
    txing = false;
    apply_mode();                       /* RX routing, demodulator, LO offset */
    dirty = true;
}

/* ---- CAT access (cat.c) ---- */

/* Frequency / mode / VFO changes are refused while transmitting (no relay switching under
 * power) and in WFM (the HF chain is off). */
bool radio_cat_can_tune(void)
{
    return !txing && state != UI_FM;
}

uint32_t radio_vfo_freq(int v)
{
    return wp.vfo[v & 1].freq;
}

int radio_vfo_mode(int v)
{
    return wp.vfo[v & 1].mode;
}

/* Set VFO v (A = 0, B = 1) directly, as stock CI-V 0x25 / 0x26; applied if it is the one in
 * use. freq 0 / mode < 0 = keep. */
void radio_set_vfo(int v, uint32_t freq, int mode)
{
    vfo_t *p = &wp.vfo[v & 1];
    if (freq)
        p->freq = freq < F_MIN ? F_MIN : freq > F_MAX ? F_MAX : freq;
    if (mode >= 0 && mode < MODE_COUNT)
        p->mode = mode;
    if (!memory_mode() && p == cur())
        apply_mode();
    dirty = true;
}

int radio_cur_vfo(void)
{
    return wp.cur_vfo;
}

/* v = 0 / 1, or -1 = toggle (A/B soft key). */
void radio_select_vfo(int v)
{
    wp.cur_vfo = v < 0 ? !wp.cur_vfo : v & 1;
    if (!memory_mode())
        apply_all();
    dirty = true;
}

void radio_leave_memory(void)
{
    if (memory_mode())
        toggle_vm();
    dirty = true;
}

/* Stock 0x0803396c (CI-V 07 A0): the other VFO = the current one. */
void radio_copy_vfo(void)
{
    wp.vfo[!wp.cur_vfo] = wp.vfo[wp.cur_vfo];
    dirty = true;
}

bool radio_preamp(void)
{
    return preamp;
}

void radio_set_preamp(bool on)
{
    preamp = on;
    apply_preamp();
    dirty = true;
}

bool radio_split(void)
{
    return wp.flags & WP_FLAG_SPLIT;
}

void radio_set_split(bool on)
{
    wp.flags = on ? wp.flags | WP_FLAG_SPLIT : wp.flags & ~WP_FLAG_SPLIT;
    dirty = true;
}

bool radio_lock(void)
{
    return wp.flags & WP_FLAG_LOCK;
}

void radio_set_lock(bool on)
{
    wp.flags = on ? wp.flags | WP_FLAG_LOCK : wp.flags & ~WP_FLAG_LOCK;
    dirty = true;
}

int radio_volume(void)
{
    return wp.volume;
}

void radio_set_volume(int v)
{
    wp.volume = v < 0 ? 0 : v > 46 ? 46 : v;
    apply_volume();
    dirty = true;
}

int radio_tx_power(void)
{
    return wp.dsp_opt;
}

void radio_set_tx_power(int idx)
{
    wp.dsp_opt = idx < 0 ? 0 : idx > 2 ? 2 : idx;       /* used at the next TX start */
}

void radio_set_pb0(bool on)
{
    wp.flags = on ? wp.flags | 0x80 : wp.flags & ~0x80;
    apply_pb0();
    dirty = true;
}

/* Receive filter width of the current mode in Hz (CWF for CW, FILT otherwise). */
int radio_filter_hz(void)
{
    static const int cw_bw[3] = {50, 250, 500};
    int fg = filt_group(cur()->mode);
    return fg < 0 ? cw_bw[stock_settings.cw_filter] : filt_hi[fg] * 100;
}

/* Stock boot screen (0x0803458c): backlight from flags bit8, rising beep (0x080389b0: 261.6,
 * 329.6, 392 Hz x 100 ms) if beep is on, logo @(39,0), call sign (default: brand name; ours: "G106 OPEN")
 * centred in (0,49)-(127,63), 3 s. Audio is still off (no demod mode set yet), as stock unmutes later. */
static void boot_screen(void)
{
    lcd_backlight(wp.flags & WP_FLAG_BACKLIGHT);
    lcd_clear();
    lcd_bitmap(&img_boot, 39, 0);
    draw_caption();
    lcd_flush();
    uint32_t t0 = millis();
    if (wp.flags & WP_FLAG_BEEP) {
        static const uint16_t tones[3] = {262, 330, 392};
        for (int k = 0; k < 3; k++) {
            beep(tones[k], 100);
            delay_ms(100);
        }
    }
    while (millis() - t0 < 3000)
        ;
}

static void apply_keyer(void);

void radio_init(void)
{
    rf_init();
    spectrum_init();
    ee_ok = settings_load() == 0;
    boot_screen();
    if (memory_mode()) {
        int ch = memory_find(wp.mem_ch, 0);
        if (ch < 0)
            ch = memory_find(0, 1);
        if (ch < 0)
            wp.flags &= ~WP_FLAG_MEMORY;
        else
            mem_work = memories[wp.mem_ch = ch];
    }
    audio_set_mic_eq(mic_eq[0] - 12, mic_eq[1] - 12, mic_eq[2] - 12);
    audio_set_comp(comp_level);
    audio_set_am_mod(am_mod);
    audio_set_agc(agc_mode);
    keyer_init();
    apply_keyer();
    apply_volume();
    apply_preamp();
    apply_pb0();
    apply_contrast();
    vc_set_ref_cal(freq_cal);
    apply_all();
    fm_init();                          /* stock boot: FM chip muted */
    if (wp.flags & WP_FLAG_FM)          /* stock ui_task: WFM survives power cycles */
        fm_enter();
}

/* Stock band change (0x08031d88): save the VFO into its slot, load the next slot. */
static void band_step(int dir)
{
    int slot = band_slot_of(cur()->freq), next;
    if (wp.flags & WP_FLAG_FULLBAND) {
        next = slot + dir;
        next = next < 0 ? 20 : next > 20 ? 0 : next;
    } else {
        next = slot + ((slot & 1) ? 2 * dir : dir);
        next = next < 0 ? 19 : next > 19 ? 1 : next;
    }
    band_slots[slot] = *cur();
    vfo_t v = band_slots[next];
    if (v.freq < F_MIN || v.freq > F_MAX || band_slot_of(v.freq) != next)
        v.freq = band_slot_default(next);
    *cur() = v;
    apply_all();
}

static void fmt_freq(char *buf, uint32_t f)
{
    snprintf(buf, 12, "%02lu.%03lu.%03lu", (unsigned long)(f / 1000000),
             (unsigned long)(f / 1000 % 1000), (unsigned long)(f % 1000));
}

/* Big frequency; digit index 7 = 10 MHz ... 0 = 1 Hz, `hl` digit drawn inverted (stock),
 * hl < 0: no highlight. */
static void draw_main_freq(uint32_t f, int hl)
{
    char s[12], one[2] = {0};
    fmt_freq(s, f);
    int x = 13, d = 7;
    for (const char *p = s; *p; p++) {
        one[0] = *p;
        bool inv = *p != '.' && d-- == hl;
        x = lcd_text_f(&font_freq, x, 10, one, inv);
    }
}

/* NFM squelch threshold on the S-meter scale (0..100 = -121..-21 dBm), 0 = off. */
static int sql_level(void)
{
    return cur()->mode == MODE_NFM && nfm_sql ? nfm_sql * 2 : 0;
}

/* NFM squelch gate from the FFT S-meter level of each new scope frame (~40 ms): opens at the
 * threshold, closes 3 dB below it after a 150 ms hang (no chopping on short fades). */
static void squelch_poll(bool fresh, uint32_t now)
{
    static bool open = true;
    static uint32_t below_since;
    int thr = sql_level();
    if (!thr || txing) {
        open = true;
        below_since = 0;
    } else if (fresh) {
        float lvl = spectrum_dbm() + 121.0f;
        if (lvl >= thr) {
            open = true;
            below_since = 0;
        } else if (lvl < thr - 3) {
            if (!below_since)
                below_since = now | 1;
            else if (now - below_since >= 150)
                open = false;
        }
    }
    audio_set_nfm_gate(open);
}

/* Stock S-meter 0x0802f848: dBm (FFT based, see spectrum.c) clamped to -121..-21, smoothed
 * (attack 0.5, decay 0.05) to a 0..100 level; S0 = -121 dBm, 6 dB per S unit, S9 = 48;
 * bar x 25.., peak held 50 frames; the scale text follows the held peak. */
static void draw_smeter(void)
{
    float dbm = spectrum_dbm();
    dbm = dbm < -121.0f ? -121.0f : dbm > -21.0f ? -21.0f : dbm;
    float x = dbm + 121.0f;
    smeter_lvl += (x - smeter_lvl) * (x > smeter_lvl ? 0.5f : 0.05f);
    int lvl = (int)smeter_lvl;

    int shown;
    if (lvl > smeter_peak) {
        smeter_peak = lvl;
        smeter_hold = 50;
        shown = lvl;
    } else if (smeter_hold == 0) {
        smeter_peak = 0;
        shown = lvl;
    } else {
        smeter_hold--;
        shown = smeter_peak;
    }

    if (lvl > 0)
        lcd_fill(25, 42, 24 + lvl, 46, true);
    if (smeter_peak > 0)
        for (int y = 42; y <= 46; y++)
            lcd_pixel(25 + smeter_peak, y, true);
    int sql = sql_level();
    if (sql) {                          /* own: NFM squelch arrow (pointing down) over the bar */
        for (int dx = -1; dx <= 1; dx++)
            lcd_pixel(25 + sql + dx, 40, true);
        lcd_pixel(25 + sql, 41, true);
    }

    char t[8];
    if (shown < 1)
        snprintf(t, sizeof t, "   S0");
    else if (shown > 48)
        snprintf(t, sizeof t, "S9+%d", shown < 59 ? 10 : shown < 69 ? 20 : shown < 79 ? 30 :
                                       shown < 89 ? 40 : shown < 99 ? 50 : 60);
    else
        snprintf(t, sizeof t, "   S%d", shown / 6 + 1);
    lcd_text_f(&font6, 4, 42, t, false);
}

/* Stock scope 0x0802f4f0: columns smoothed 0.2 new + 0.8 old, filled from the bottom.
 * Tuning marker with stock timing: shown for 50 scope draws after the frequency changes (stock
 * counter 0x20000338), at x 63 (CW/CWR: shifted by the tone). Deviation (user request): a plain
 * full-height line over the spectrum instead of stock's triangle + bar. */
static void draw_scope(int bottom, int height)
{
    static uint32_t marker_freq;
    static int marker_left;
    if (cur()->freq != marker_freq) {
        marker_freq = cur()->freq;
        marker_left = 50;
    }
    const uint16_t *raw = spectrum_columns();
    for (int c = 0; c < SCOPE_COLS; c++) {
        int v = (int)(raw[c] * 0.2f + scope_state[c] * 0.8f);
        scope_state[c] = v;
        if (v > height)
            v = height;
        for (int y = bottom - v; y <= bottom; y++)
            lcd_pixel(c, y, true);
    }
    if (marker_left > 0) {
        marker_left--;
        int off = (int)(stock_settings.cw_tone * 128.0f * (scope_zoom / 10.0f) / 48000.0f + 0.5f);
        int m = 63 + (cur()->mode == MODE_CW ? off : cur()->mode == MODE_CWR ? -off : 0);
        lcd_fill(m, bottom - height, m, bottom, true);
    }
}

/* Double rule + four 32 px label cells (stock 0x0802f67c). */
static void draw_labels(const char *const labels[4])
{
    lcd_hline(0, 127, 48, true);
    lcd_hline(0, 127, 49, true);
    for (int k = 0; k < 4; k++) {
        int w = lcd_text_width(&font8, labels[k]);
        lcd_text_f(&font8, 32 * k + (32 - w) / 2, 54, labels[k], false);
    }
}

/* Soft-key panel (stock 0x0802f76c): labels + "[page/5]". emWin draws text with background,
 * so the page indicator cuts a gap into the double rule. */
static void draw_panel(void)
{
    char t[8];
    draw_labels(softkey_labels[wp.page]);
    snprintf(t, sizeof t, "[%d/%d]", wp.page + 1, NUM_PAGES);
    lcd_fill(6, 45, 5 + lcd_text_width(&font8, t), 45 + font8.height - 1, false);
    lcd_text_f(&font8, 6, 45, t, false);
}

/* Text centred in a box (emWin GUI_DispStringInRect with GUI_TA_HCENTER | GUI_TA_VCENTER),
 * '\n' separates lines. */
static void text_in_rect(const char *s, int x0, int y0, int x1, int y1)
{
    int lines = 1;
    for (const char *p = s; *p; p++)
        lines += *p == '\n';
    int y = y0 + (y1 - y0 + 1 - lines * font8.height) / 2;
    char line[32];
    while (*s) {
        int n = 0;
        while (*s && *s != '\n' && n < (int)sizeof line - 1)
            line[n++] = *s++;
        line[n] = 0;
        if (*s == '\n')
            s++;
        int w = lcd_text_width(&font8, line);
        lcd_text_f(&font8, x0 + (x1 - x0 + 1 - w) / 2, y, line, false);
        y += font8.height;
    }
}

/* Stock message box 0x0802f7bc: title box (10,5)-(117,18), body box (10,18)-(117,52). */
static void msgbox(const char *title, const char *text)
{
    msgbox_title = title;
    msgbox_text = text;
    msgbox_until = millis() + MSGBOX_MS;
    state = UI_MSGBOX;
}

static void draw_msgbox(void)
{
    lcd_fill(10, 5, 117, 52, false);
    lcd_rect(10, 5, 117, 18, true);
    lcd_rect(10, 18, 117, 52, true);
    text_in_rect(msgbox_title, 10, 5, 117, 18);
    text_in_rect(msgbox_text, 10, 18, 117, 52);
}

/* Stock VER screen 0x0803198c, with this firmware's identity. */
/* ---- CSN: call sign editor (stock 0x08033dec). The tune dial moves a cursor over a strip of
 * ten consecutive ASCII characters (0x20..0x7E, wraps) and scrolls it at the ends; dial push
 * appends the character under the cursor (max 15), F2 / F3 delete the last one, F4 saves (empty
 * clears), F1 quits. Shown at boot instead of the brand line. */
#define CSN_MAX 15
#define CSN_CHARS 95
static char csn_buf[CSN_MAX + 1];
static int csn_len, csn_base, csn_cur;

static void open_csn(void)
{
    settings_read_callsign(csn_buf);
    csn_len = strlen(csn_buf);
    csn_base = 'A' - ' ';               /* stock starts the strip at 'A' */
    csn_cur = 0;
    state = UI_CSN;
}

static char csn_char(int i)
{
    return ' ' + (csn_base + i) % CSN_CHARS;
}

static void draw_csn(bool blink)
{
    lcd_clear();
    text_in_rect("CALL SIGN EDITOR", 0, 0, 127, 8);
    lcd_text_f(&font8, 0, 55, "QUIT", false);
    lcd_text_f(&font8, 51, 55, "BACK", false);
    lcd_text_f(&font8, 104, 55, "SAVE", false);
    lcd_hline(0, 127, 9, true);
    lcd_hline(0, 127, 53, true);
    lcd_text_f(&font16, (128 - lcd_text_width(&font16, csn_buf)) / 2, 12, csn_buf, false);
    for (int i = 0; i < 10; i++) {
        char c[2] = {csn_char(i), 0};
        lcd_text_f(&font8, 4 + 12 * i, 39, c, false);
    }
    if (!blink) {                       /* cursor: two-pixel underline under the character */
        lcd_hline(3 + 12 * csn_cur, 9 + 12 * csn_cur, 47, true);
        lcd_hline(3 + 12 * csn_cur, 9 + 12 * csn_cur, 48, true);
    }
}

static void csn_key(int code)
{
    if (code == KEY_F1) {
        state = menu_back ? UI_MENU : UI_MAIN;
        menu_back = false;
    } else if (code == KEY_F2 || code == KEY_F3) {
        if (csn_len > 0)
            csn_buf[--csn_len] = 0;
    } else if (code == KEY_TUNE_PUSH) {
        if (csn_len < CSN_MAX) {
            csn_buf[csn_len++] = csn_char(csn_cur);
            csn_buf[csn_len] = 0;
        }
    } else if (code == KEY_F4) {
        state = menu_back ? UI_MENU : UI_MAIN;
        menu_back = false;
        if (settings_write_callsign(csn_buf) < 0)
            msgbox("ERROR", "CAN'T SAVE!");
    }
}

static void csn_dial(int d)
{
    csn_cur += d;
    if (csn_cur < 0) {
        csn_cur = 0;
        csn_base = (csn_base + CSN_CHARS - 1) % CSN_CHARS;
    } else if (csn_cur > 9) {
        csn_cur = 9;
        csn_base = (csn_base + 1) % CSN_CHARS;
    }
}

/* Stock apply functions 0x080333a8 ..: KS / KM / IMB / CWR (value name "2.0".. as a number) /
 * QSK time / CWT go to the keyer. */
static void apply_keyer(void)
{
    keyer_set_speed(stock_settings.key_speed);
    keyer_set_mode(stock_settings.key_mode);
    keyer_set_iambic(stock_settings.iambic);
    keyer_set_ratio(2.0f + 0.5f * stock_settings.cw_ratio);
    keyer_set_hang(stock_settings.qsk_time);
    keyer_set_tone(stock_settings.cw_tone);
    keyer_set_qsk(wp.flags & WP_FLAG_QSK);
}

static int setting_get(const setting_t *s)
{
    return s->size == 1 ? *(uint8_t *)s->var : *(int16_t *)s->var;
}

static void setting_set(const setting_t *s, int v)
{
    if (s->size == 1)
        *(uint8_t *)s->var = v;
    else
        *(int16_t *)s->var = v;
    /* DSP filters follow the filter settings; in TX (SETTINGS menu) only the taps change,
     * the RX routing comes back with radio_tx_stop(). */
    if (s == &set_cw_tone || s == &set_cw_flt || (s >= set_filt && s < set_filt + FILT_COUNT)) {
        if (txing)
            apply_filters();
        else
            apply_mode();
    }
    if (s == &set_mic_gain && txing)
        codec_micpga(mic_gain);         /* live while talking from the SETTINGS menu */
    if (s == &set_tx_power && txing)
        audio_set_tx(tx_drive(wp.dsp_opt), stock_settings.tx_dc_i, stock_settings.tx_dc_q);
    if (s == &set_contrast)
        apply_contrast();
    if (s == &set_freq_cal) {
        vc_set_ref_cal(freq_cal);
        apply_freq();                   /* LO again; TX-safe (same frequency, no routing) */
    }
    if (s == &set_eq_low || s == &set_eq_mid || s == &set_eq_high)
        audio_set_mic_eq(mic_eq[0] - 12, mic_eq[1] - 12, mic_eq[2] - 12);
    if (s == &set_comp)
        audio_set_comp(comp_level);
    if (s == &set_am_mod)
        audio_set_am_mod(am_mod);
    if (s == &set_am_tone)
        audio_set_am_tone(am_tone);
    if (s == &set_mic_gain)
        param2.mic_gain = mic_gain > 30 ? 30 : mic_gain;   /* stock block: stock range only */
    if (s == &set_agc)
        audio_set_agc(agc_mode);
    if (s == &set_cw_tone || s == &set_key_speed || s == &set_key_mode || s == &set_iambic ||
        s == &set_cw_ratio || s == &set_qsk_time)
        apply_keyer();
}

static void open_edit(const setting_t *s)
{
    edit = s;
    edit_value = edit_orig = setting_get(s);
    state = UI_EDIT;
}

/* Apply and store a changed value (work params: at power-off, as stock). */
static void commit_setting(const setting_t *s, int value, int orig)
{
    if (value == orig) {
        setting_set(s, orig);
        return;
    }
    setting_set(s, value);
    int err = s->store == ST_SETTINGS ? settings_save_settings()
            : s->store == ST_PARAM2 ? settings_save_param2()
            : s->store == ST_OWN ? settings_save_own() : 0;
    if (s == &set_mic_gain && err >= 0)
        err = settings_save_param2();
    if (err < 0)
        msgbox("ERROR", "CAN'T SAVE!");
}

static void close_edit(bool save)
{
    state = UI_MAIN;
    if (!save) {
        setting_set(edit, edit_orig);
        return;
    }
    commit_setting(edit, edit_value, edit_orig);
}

/* Value text of a setting. */
static void setting_text(const setting_t *s, int val, char *v, size_t n)
{
    if (s->names)
        snprintf(v, n, "%s", s->names[val - s->min]);
    else if (!strcmp(s->fmt, "%k"))
        snprintf(v, n, "%d.%dkHz", val / 10, val % 10);
    else if (!strcmp(s->fmt, "%z"))
        snprintf(v, n, "%d.%dx", val / 10, val % 10);
    else if (!strcmp(s->fmt, "%e"))
        snprintf(v, n, "%+ddB", val - 12);
    else if (!strcmp(s->fmt, "%o"))
        snprintf(v, n, val ? "%d" : "OFF", val);
    else if (!strcmp(s->fmt, "%r"))
        snprintf(v, n, "%+d", val - (s->min + s->max) / 2);
    else if (!strcmp(s->fmt, "%p"))
        snprintf(v, n, "%c%d.%02dppm", val < 0 ? '-' : '+', abs(val) / 100, abs(val) % 100);
    else if (!strcmp(s->fmt, "%h"))
        snprintf(v, n, "%d.%ddB", val / 2, val % 2 * 5);
    else
        snprintf(v, n, s->fmt, val);
}

static void draw_edit(bool blink)
{
    char v[20];
    lcd_fill(0, 31, 127, 38, false);
    if (blink)
        lcd_text_f(&font8, 3, 31, edit->label, false);
    setting_text(edit, edit_value, v, sizeof v);
    lcd_text_f(&font8, 3 + lcd_text_width(&font8, edit->label), 31, v, false);
    draw_labels(mw_labels);             /* QUIT · · SAVE (stock 0x08038c8c) */
}

static void popup(const char *text)
{
    popup_text = text;
    popup_until = millis() + POPUP_MS;
}

static void open_mw(void)
{
    dlg_ch = wp.mem_ch < NUM_MEMORIES ? wp.mem_ch : NUM_MEMORIES - 1;
    state = UI_MW;
}

static void open_mc(void)
{
    int ch = memory_find(wp.mem_ch, 0);
    if (ch < 0)
        ch = memory_find(0, 1);
    if (ch < 0) {
        msgbox("INFO", "NO VALID MOMOEY!");     /* sic, as stock */
        return;
    }
    dlg_ch = ch;
    state = UI_MC;
}

static void toggle_vm(void)
{
    if (memory_mode()) {
        wp.flags &= ~WP_FLAG_MEMORY;
        apply_all();
        return;
    }
    int ch = memory_find(wp.mem_ch, 1);
    if (ch < 0)
        return;                                 /* no stored channel: stay in VFO mode */
    wp.flags |= WP_FLAG_MEMORY;
    memory_recall(ch);
}

/* ---- VERSION screen (own; stock VER only shows its version text): firmware version, build,
 * live state for diagnostics. Tune encoder scrolls, any key goes back (as stock). ---- */
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_COMMIT
#define FW_COMMIT "?"
#endif
static int ver_top;
static char ver_i2c[24];                     /* I2C devices that did not answer, probed on open */

static void open_version(void)
{
    static const struct { int bus; uint8_t addr; const char *name; } dev[] = {
        {I2C_BUS1, 0x6A, "VC"}, {I2C_BUS1, 0x50, "EE"}, {I2C_BUS1, 0x60, "FM"},
        {I2C_BUS1, 0x2E, "POT"}, {I2C_BUS2, 0x18, "CODEC"},
    };
    ver_i2c[0] = 0;
    for (unsigned k = 0; k < sizeof dev / sizeof dev[0]; k++)
        if (i2c_probe(dev[k].bus, dev[k].addr) < 0)
            snprintf(ver_i2c + strlen(ver_i2c), sizeof ver_i2c - strlen(ver_i2c), "%s%s",
                     ver_i2c[0] ? " " : "NO ", dev[k].name);
    ver_top = 0;
    state = UI_VER;
    panel_until = 0;
}

#define VER_ROWS 5                      /* rows after the title bar (version is in the title) */
#define VER_VIS  5                      /* visible rows: no soft-key bar on this screen */
static void ver_row(int i, char *v, size_t n, const char **label)
{
    static const char *const labels[VER_ROWS] = {"DATE", "DSP LOAD", "DSP RESETS",
                                                 "EEPROM", "I2C"};
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const char date[] = __DATE__, time[] = __TIME__;   /* "Nov 13 2026", "12:56:03" */
    *label = labels[i];
    switch (i) {
    case 0: {
        int m = 0;
        while (m < 11 && strncmp(mon + 3 * m, date, 3))
            m++;
        snprintf(v, n, "%c%c.%02d.%c%c %.5s", date[4] == ' ' ? '0' : date[4], date[5], m + 1,
                 date[9], date[10], time);
        break;
    }
    case 1: {
        uint32_t avg, max;
        audio_cpu_load(&avg, &max);     /* since the last call: one draw period */
        snprintf(v, n, "%lu%% / %lu%%", (unsigned long)avg, (unsigned long)max);
        break;
    }
    case 2: {
        uint32_t resets, first;
        audio_dsp_faults(&resets, &first);
        snprintf(v, n, "%lu", (unsigned long)resets);
        break;
    }
    case 3: snprintf(v, n, ee_ok ? "OK" : "DEFAULTS"); break;
    default: snprintf(v, n, "%s", ver_i2c[0] ? ver_i2c : "OK"); break;
    }
}

static void draw_list_title(const char *text, int pos, int n);
static void draw_scrollbar(int top, int n, int rows);

static void draw_version(void)
{
    char v[24], t[28];
    const char *label;
    lcd_clear();
    snprintf(t, sizeof t, "G106 OPEN %s", FW_VERSION);
    draw_list_title(t, 0, 0);
    for (int r = 0; r < VER_VIS && ver_top + r < VER_ROWS; r++) {
        int y = 12 + 9 * r;
        ver_row(ver_top + r, v, sizeof v, &label);
        lcd_text_f(&font8, 4, y, label, false);
        lcd_text_f(&font8, 119 - lcd_text_width(&font8, v), y, v, false);
    }
    draw_scrollbar(ver_top, VER_ROWS, VER_VIS);
}

/* ---- TX IQ CAL (stock "SYSTEM CAL" -> "1.TX IQ CAL", 0x08032e00), opened from SETTINGS > CAL.
 * Stock edits the TX carrier-null DC offsets TXI / TXQ (settings +0x10 / +0x14, -1000000..1000000,
 * live in the DSP): TUNE push = I / Q, tune encoder = value x MAG, BAND- / BAND+ = MAG x1..x1000
 * (default x100), F1 BACK (asks "SAVE CHANGES?" NO / YES if anything changed), F2 TX on / off (TX without the microphone: only the carrier leakage
 * is left), F3 CLR (the selected value = 0), F4 SAVE (settings block). Own additions:
 *  - AMP / PHASE rows = the stock per-band TX IQ table (settings +0x18) for the band of the TX
 *    frequency; stock never applies it, here it corrects the TX I/Q balance (audio_set_tx_iq).
 *    TX on these rows sends a 1 kHz tone (USB, LSB in LSB / L-D / CWR): adjust for the least
 *    opposite sideband (1 kHz on the other side of the carrier) on another receiver.
 *  - Tune encoder = row cursor, TUNE push = edit the row (encoder = value), TUNE push again = done
 *    (stock: encoder always edits, TUNE push toggles I / Q); PTT transmits like F2 while held.
 *  - NO restores the old values (stock keeps the edited DC offsets live until power-off).
 *  - Keys work in TX (stock polls them in its own loop). Leaving the screen ends TX. ---- */
enum { CAL_DC_I, CAL_DC_Q, CAL_AMP, CAL_PHASE, CAL_ROWS };
#define CAL_DC_MAX 1000000
#define CAL_IQ_MAX 2000                 /* +-10 % / +-10 degrees */
static int cal_row, cal_mag = 2, cal_band;
static bool cal_tx, cal_edit, cal_confirm;           /* cal_edit: the tune encoder changes the value, not the row */
static int32_t cal_orig_dc[2];
static int16_t cal_orig_iq[2];
static const char *const cal_band_names[10] = {"160m", "80m", "60m", "40m", "30m", "20m", "17m",
                                               "15m", "12m", "10m"};

static void open_txcal(void)
{
    cal_band = txiq_band(tx_freq());
    cal_orig_dc[0] = stock_settings.tx_dc_i;
    cal_orig_dc[1] = stock_settings.tx_dc_q;
    cal_orig_iq[0] = stock_settings.tx_iq[cal_band][0];
    cal_orig_iq[1] = stock_settings.tx_iq[cal_band][1];
    cal_tx = cal_edit = cal_confirm = false;
    state = UI_TXCAL;
}

/* TX signal of the screen: no modulation for the DC rows, a test tone for AMP / PHASE. */
static int cal_signal(void)
{
    if (cal_row < CAL_AMP)
        return AUDIO_TX_NULL;
    int m = cur()->mode;
    return m == MODE_LSB || m == MODE_LD || m == MODE_CWR ? AUDIO_TX_LSB : AUDIO_TX_USB;
}

static void cal_apply(void)
{
    audio_set_tx(tx_power[wp.dsp_opt], stock_settings.tx_dc_i, stock_settings.tx_dc_q);
    apply_txiq();
}

static int32_t *cal_dc(int row)
{
    return row == CAL_DC_I ? &stock_settings.tx_dc_i : &stock_settings.tx_dc_q;
}

static void cal_dial(int d)
{
    static const int32_t mag[4] = {1, 10, 100, 1000};      /* stock step table 0x08038f2c */
    int64_t step = (int64_t)d * mag[cal_mag];
    if (cal_row < CAL_AMP) {
        int64_t v = *cal_dc(cal_row) + step;
        *cal_dc(cal_row) = v < -CAL_DC_MAX ? -CAL_DC_MAX : v > CAL_DC_MAX ? CAL_DC_MAX : (int32_t)v;
    } else {
        int16_t *p = &stock_settings.tx_iq[cal_band][cal_row - CAL_AMP];
        int64_t v = *p + step;
        *p = v < -CAL_IQ_MAX ? -CAL_IQ_MAX : v > CAL_IQ_MAX ? CAL_IQ_MAX : (int16_t)v;
    }
    cal_apply();
}

static void cal_close(bool save)
{
    cal_tx = false;
    radio_tx_stop();
    if (save) {
        if (settings_save_settings() < 0)
            msgbox("ERROR", "CAN'T SAVE!");
    } else {
        stock_settings.tx_dc_i = cal_orig_dc[0];
        stock_settings.tx_dc_q = cal_orig_dc[1];
        stock_settings.tx_iq[cal_band][0] = cal_orig_iq[0];
        stock_settings.tx_iq[cal_band][1] = cal_orig_iq[1];
        cal_apply();
    }
    if (state == UI_TXCAL)
        state = UI_MENU;
}

static bool cal_changed(void)
{
    return stock_settings.tx_dc_i != cal_orig_dc[0] || stock_settings.tx_dc_q != cal_orig_dc[1] ||
           stock_settings.tx_iq[cal_band][0] != cal_orig_iq[0] ||
           stock_settings.tx_iq[cal_band][1] != cal_orig_iq[1];
}

static void cal_key(int code, bool lng)
{
    if (lng)
        return;
    if (cal_confirm) {                  /* "SAVE CHANGES?": NO = discard, YES = save, both leave */
        if (code == KEY_F1)
            cal_close(false);
        else if (code == KEY_F4)
            cal_close(true);
        return;
    }
    switch (code) {
    case KEY_TUNE_PUSH:
        cal_edit = !cal_edit;
        break;
    case KEY_BAND_DOWN:
        if (cal_mag > 0)
            cal_mag--;
        break;
    case KEY_BAND_UP:
        if (cal_mag < 3)
            cal_mag++;
        break;
    case KEY_F1:                        /* BACK: ask only if something changed */
        cal_edit = false;
        if (cal_changed()) {
            cal_tx = false;
            cal_confirm = true;
        } else {
            cal_close(false);
        }
        break;
    case KEY_F2:
        cal_tx = !cal_tx;
        break;
    case KEY_F3:
        if (cal_row < CAL_AMP)
            *cal_dc(cal_row) = 0;
        else
            stock_settings.tx_iq[cal_band][cal_row - CAL_AMP] = 0;
        cal_apply();
        break;
    case KEY_F4:
        cal_close(true);
        break;
    default:
        break;
    }
}

/* TX in the cal screen (radio_poll): F2 toggle or PTT; the signal follows the selected row. */
static void cal_poll_tx(bool ptt)
{
    bool on = cal_tx || ptt;
    if (on && !txing) {
        if (radio_tx_start(cal_signal(), -1) < 0)
            cal_tx = false;
    } else if (!on && txing) {
        radio_tx_stop();
    } else if (txing) {
        audio_set_mode(cal_signal());
    }
}

/* Value with 3 decimals: v x 5 thousandths (0.005 % / 0.005 degree units). */
static void cal_fixed(int v, const char *unit, char *t, size_t n)
{
    int a = abs(v) * 5;
    snprintf(t, n, "%c%d.%03d%s", v < 0 ? '-' : v > 0 ? '+' : ' ', a / 1000, a % 1000, unit);
}

static void menu_chevron(int x, int cy, bool on);
static void menu_chevron_left(int x, int cy, bool on);

static void draw_txcal(void)
{
    static const char *const l_cal[4] = {"BACK", "TX", "CLR", "SAVE"};     /* stock: QUIT */
    static const char *const l_ask[4] = {"NO", " ", " ", "YES"};
    static const char *const label[CAL_ROWS] = {"CARRIER I", "CARRIER Q", "AMPLITUDE", "PHASE"};
    static const char *const mag_names[4] = {"x1", "x10", "x100", "x1000"};
    char t[20];
    lcd_clear();
    /* title bar: name + band left, MAG and the TX flag right */
    lcd_fill(0, 0, 127, 9, true);
    snprintf(t, sizeof t, "TX IQ %s", cal_band_names[cal_band]);
    lcd_text_f(&font8, 3, 1, t, true);
    int x = 125;
    if (txing) {
        int w = lcd_text_width(&font8, "TX");
        lcd_fill(123 - w, 1, 126, 8, false);
        lcd_text_f(&font8, 125 - w, 1, "TX", false);
        x = 120 - w;
    }
    lcd_text_f(&font8, x - lcd_text_width(&font8, mag_names[cal_mag]), 1, mag_names[cal_mag], true);

    if (cal_confirm) {
        lcd_text_f(&font8, (128 - lcd_text_width(&font8, "SAVE CHANGES?")) / 2, 24, "SAVE CHANGES?", false);
        draw_labels(l_ask);
        return;
    }
    for (int r = 0; r < CAL_ROWS; r++) {
        int y = 11 + 9 * r;
        bool on = r == cal_row;
        if (r < CAL_AMP)
            snprintf(t, sizeof t, "%ld", (long)*cal_dc(r));
        else
            cal_fixed(stock_settings.tx_iq[cal_band][r - CAL_AMP], r == CAL_AMP ? "%" : "deg", t,
                      sizeof t);
        bool ed = on && cal_edit;       /* editing: only the value box is inverted, with arrows */
        if (on && !ed)
            lcd_fill(0, y, 127, y + 8, true);
        lcd_text_f(&font8, 4, y + 1, label[r], on && !ed);
        int w = lcd_text_width(&font8, t);
        if (ed) {
            int bx = 120 - w;
            lcd_fill(bx - 6, y, 126, y + 8, true);
            lcd_text_f(&font8, bx, y + 1, t, true);
            menu_chevron_left(bx - 5, y + 4, false);
            menu_chevron(122, y + 4, false);
        } else {
            lcd_text_f(&font8, 124 - w, y + 1, t, on);
        }
    }
    draw_labels(l_cal);
}

/* ---- SETTINGS menu (own): categories -> items. The own and advanced settings live here.
 *   tune encoder: cursor (wraps); while editing: value (fast spin = x10 on wide ranges)
 *   TUNE push / F4: open a category, edit a value in place, toggle an ON/OFF item, open CSN / VER
 *   F1: back (category list -> main screen); while editing: QUIT (old value back)
 *   F1 long: leave the menu at once
 * Edited values are applied live; SAVE (F4 / TUNE push) stores them like the stock setting
 * editor. The cursor position survives closing the menu. PTT / CAT PTT transmit from the menu
 * (not in CW), so MIC GAIN / EQ / COMP / TX POWER / FILTER can be tuned while talking; keys are
 * ignored in TX as everywhere, the tune encoder still edits. ---- */
typedef enum { MI_SET, MI_PREAMP, MI_QSK, MI_BEEP, MI_BANDS, MI_CSN, MI_VER, MI_TXCAL } mi_kind_t;
typedef struct {
    const char *label;
    mi_kind_t kind;
    const setting_t *s;
} mitem_t;
typedef struct {
    const char *name;
    const mitem_t *items;
    int n;
} mcat_t;

static const mitem_t mi_rx[] = {
    {"SSB FILTER", MI_SET, &set_filt[FILT_SSB]}, {"DATA FILTER", MI_SET, &set_filt[FILT_DATA]},
    {"AM FILTER", MI_SET, &set_filt[FILT_AM]}, {"NFM FILTER", MI_SET, &set_filt[FILT_NFM]},
    {"AGC", MI_SET, &set_agc}, {"PREAMP", MI_PREAMP, NULL}, {"NFM SQUELCH", MI_SET, &set_sql},
};
static const mitem_t mi_tx[] = {
    {"TX POWER", MI_SET, &set_tx_power}, {"MIC GAIN", MI_SET, &set_mic_gain},
    {"COMPRESSOR", MI_SET, &set_comp}, {"EQ 300Hz", MI_SET, &set_eq_low},
    {"EQ 1200Hz", MI_SET, &set_eq_mid}, {"EQ 2500Hz", MI_SET, &set_eq_high},
    {"AM MOD", MI_SET, &set_am_mod}, {"AM TEST TONE", MI_SET, &set_am_tone},
};
static const mitem_t mi_cw[] = {
    {"KEY SPEED", MI_SET, &set_key_speed}, {"KEY MODE", MI_SET, &set_key_mode},
    {"IAMBIC", MI_SET, &set_iambic}, {"DOT:DASH", MI_SET, &set_cw_ratio},
    {"CW TONE", MI_SET, &set_cw_tone}, {"CW FILTER", MI_SET, &set_cw_flt},
    {"QSK", MI_QSK, NULL}, {"QSK DELAY", MI_SET, &set_qsk_time},
};
static const mitem_t mi_disp[] = {
    {"SCOPE", MI_SET, &set_disp}, {"SCOPE ZOOM", MI_SET, &set_zoom}, {"CONTRAST", MI_SET, &set_contrast},
};
static const mitem_t mi_sys[] = {
    {"KEY BEEP", MI_BEEP, NULL}, {"BANDS", MI_BANDS, NULL}, {"CALL SIGN", MI_CSN, NULL},
    {"VERSION", MI_VER, NULL},
};
static const mitem_t mi_cal[] = {
    {"TX IQ CAL", MI_TXCAL, NULL}, {"FREQ CAL", MI_SET, &set_freq_cal},
};
#define MCAT(n, a) {n, a, (int)(sizeof a / sizeof a[0])}
static const mcat_t menu_cats[] = {
    MCAT("RX", mi_rx), MCAT("TX", mi_tx), MCAT("CW", mi_cw), MCAT("DISPLAY", mi_disp),
    MCAT("SYSTEM", mi_sys), MCAT("CAL", mi_cal),
};
#define MENU_CAT_CAL 5
#define MENU_CATS ((int)(sizeof menu_cats / sizeof menu_cats[0]))
#define MENU_ROWS 4

static int menu_level;                  /* 0 categories, 1 items */
static int menu_cat;                    /* category cursor / open category */
static int menu_sel, menu_top;          /* item cursor and first visible item row */
static bool menu_editing;

static int menu_count(void)
{
    return menu_level ? menu_cats[menu_cat].n : MENU_CATS;
}

static int *menu_cursor_var(void)
{
    return menu_level ? &menu_sel : &menu_cat;
}

static int menu_first_row(void)
{
    return menu_level ? menu_top : (menu_cat >= MENU_ROWS ? menu_cat - MENU_ROWS + 1 : 0);
}

static void menu_cursor(int d)
{
    int n = menu_count(), *c = menu_cursor_var();
    *c = ((*c + d) % n + n) % n;
    if (menu_level) {
        if (menu_sel < menu_top)
            menu_top = menu_sel;
        else if (menu_sel >= menu_top + MENU_ROWS)
            menu_top = menu_sel - MENU_ROWS + 1;
    }
}

static void menu_open_cat(int c)
{
    menu_cat = c;
    menu_level = 1;
    menu_sel = menu_top = 0;
}

static void open_menu(void)
{
    state = UI_MENU;                    /* back where the user left it */
    menu_editing = false;
    panel_until = 0;
}

static const mitem_t *menu_item(void)
{
    return &menu_cats[menu_cat].items[menu_sel];
}

/* The filter that is in use in the current mode (marked in the list). */
static const setting_t *active_filter(void)
{
    int fg = filt_group(cur()->mode);
    return fg < 0 ? &set_cw_flt : &set_filt[fg];
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
    char *p = s;
    while (*p == ' ')
        p++;
    memmove(s, p, strlen(p) + 1);
}

static void menu_value(const mitem_t *it, char *v, size_t n)
{
    if (it->s) {
        setting_text(it->s, menu_editing && it->s == edit ? edit_value : setting_get(it->s), v, n);
        trim(v);
        return;
    }
    switch (it->kind) {
    case MI_PREAMP: snprintf(v, n, preamp ? "ON" : "OFF"); break;
    case MI_QSK: snprintf(v, n, wp.flags & WP_FLAG_QSK ? "ON" : "OFF"); break;
    case MI_BEEP: snprintf(v, n, wp.flags & WP_FLAG_BEEP ? "ON" : "OFF"); break;
    case MI_BANDS: snprintf(v, n, wp.flags & WP_FLAG_FULLBAND ? "FULL" : "HAM"); break;
    default: v[0] = 0; break;
    }
}

/* Small right-pointing triangle, 3 px wide, centred on cy. */
static void menu_chevron(int x, int cy, bool on)
{
    for (int k = 0; k < 3; k++)
        for (int y = cy - (2 - k); y <= cy + (2 - k); y++)
            lcd_pixel(x + k, y, on);
}

static void menu_chevron_left(int x, int cy, bool on)
{
    for (int k = 0; k < 3; k++)
        for (int y = cy - k; y <= cy + k; y++)
            lcd_pixel(x + k, y, on);
}

/* List screens (SETTINGS, VERSION): inverted title bar, text left; on the right "TX" while
 * transmitting, else "pos/n" (pos 0 = none). */
static void draw_list_title(const char *text, int pos, int n)
{
    char t[12];
    lcd_fill(0, 0, 127, 9, true);
    lcd_text_f(&font8, 3, 1, text, true);
    if (txing) {
        int w = lcd_text_width(&font8, "TX");
        lcd_fill(123 - w, 1, 126, 8, false);
        lcd_text_f(&font8, 125 - w, 1, "TX", false);
    } else if (pos) {
        snprintf(t, sizeof t, "%d/%d", pos, n);
        lcd_text_f(&font8, 125 - lcd_text_width(&font8, t), 1, t, true);
    }
}

/* Scroll bar at the right edge of the MENU_ROWS list rows (y 11..46), if needed. */
static void draw_scrollbar(int top, int n, int rows)
{
    if (n <= rows)
        return;
    int h = rows * 9 - 1;
    for (int y = 11; y <= 11 + h; y += 2)
        lcd_pixel(126, y, true);
    int th = h * rows / n < 4 ? 4 : h * rows / n;
    int ty = 11 + (h + 1 - th) * top / (n - rows);
    lcd_fill(125, ty, 127, ty + th - 1, true);
}

static void draw_menu(void)
{
    static const char *const l_cat[4] = {"EXIT", " ", " ", "OPEN"};
    static const char *const l_item[4] = {"BACK", " ", " ", "EDIT"};
    static const char *const l_tog[4] = {"BACK", " ", " ", "SET"};
    static const char *const l_open[4] = {"BACK", " ", " ", "OPEN"};
    char t[28], v[20];
    int n = menu_count(), top = menu_first_row(), sel = *menu_cursor_var();

    lcd_clear();
    /* title bar: path left, TX flag / cursor position right */
    snprintf(t, sizeof t, menu_level ? "SETTINGS>%s" : "SETTINGS", menu_cats[menu_cat].name);
    draw_list_title(t, 0, 0);

    for (int r = 0; r < MENU_ROWS && top + r < n; r++) {
        int i = top + r, y = 11 + 9 * r;
        bool on = i == sel;
        bool editing = on && menu_editing;
        if (on && !editing)
            lcd_fill(0, y, 121, y + 8, true);
        if (!menu_level) {
            lcd_text_f(&font8, 4, y + 1, menu_cats[i].name, on);
            menu_chevron(115, y + 4, !on);
            continue;
        }
        const mitem_t *it = &menu_cats[menu_cat].items[i];
        if (it->s && it->s == active_filter())      /* the filter in use: dot before the label */
            lcd_fill(0, y + 3, 1, y + 5, !on || editing);
        lcd_text_f(&font8, 4, y + 1, it->label, on && !editing);
        if (it->kind == MI_CSN || it->kind == MI_VER || it->kind == MI_TXCAL) {
            menu_chevron(115, y + 4, !on);
            continue;
        }
        menu_value(it, v, sizeof v);
        int w = lcd_text_width(&font8, v);
        if (editing) {
            /* only the value is inverted, as a spin box with arrows where it can still move */
            int bx = 115 - w;
            lcd_fill(bx - 6, y, 121, y + 8, true);
            lcd_text_f(&font8, bx, y + 1, v, true);
            if (edit_value > edit->min)
                menu_chevron_left(bx - 5, y + 4, false);
            if (edit_value < edit->max)
                menu_chevron(117, y + 4, false);
        } else {
            lcd_text_f(&font8, 119 - w, y + 1, v, on);
        }
    }

    draw_scrollbar(top, n, MENU_ROWS);

    if (menu_editing) {
        draw_labels(mw_labels);                     /* QUIT · · SAVE */
    } else if (!menu_level) {
        draw_labels(l_cat);
    } else {
        mi_kind_t k = menu_item()->kind;
        draw_labels(k == MI_SET ? l_item : k == MI_CSN || k == MI_VER || k == MI_TXCAL ? l_open : l_tog);
    }
}

static void menu_end_edit(bool save)
{
    menu_editing = false;
    if (save)
        commit_setting(edit, edit_value, edit_orig);
    else
        setting_set(edit, edit_orig);
}

static void menu_activate(void)
{
    if (!menu_level) {
        menu_open_cat(menu_cat);
        return;
    }
    const mitem_t *it = menu_item();
    if (it->s) {
        edit = it->s;
        edit_value = edit_orig = setting_get(it->s);
        menu_editing = true;
        return;
    }
    switch (it->kind) {
    case MI_PREAMP:
        preamp = !preamp;
        apply_preamp();
        break;
    case MI_QSK:
        wp.flags ^= WP_FLAG_QSK;
        apply_keyer();
        break;
    case MI_BEEP:
        wp.flags ^= WP_FLAG_BEEP;
        break;
    case MI_BANDS:
        wp.flags ^= WP_FLAG_FULLBAND;
        break;
    case MI_CSN:
        menu_back = true;
        open_csn();
        break;
    case MI_VER:
        menu_back = true;
        open_version();
        break;
    case MI_TXCAL:
        open_txcal();
        break;
    default:
        break;
    }
}

static void menu_key(int code, bool lng)
{
    if (lng) {
        if (code == KEY_F1) {           /* F1 long: straight back to the main screen */
            if (menu_editing)
                menu_end_edit(false);
            state = UI_MAIN;
        }
        return;
    }
    if (menu_editing) {
        if (code == KEY_F1)
            menu_end_edit(false);
        else if (code == KEY_F4 || code == KEY_TUNE_PUSH)
            menu_end_edit(true);
        return;
    }
    if (code == KEY_F1) {
        if (menu_level)
            menu_level = 0;             /* the category cursor stays on the closed category */
        else
            state = UI_MAIN;
    } else if (code == KEY_F4 || code == KEY_TUNE_PUSH) {
        menu_activate();
    }
}

static void softkey(int k, bool lng)
{
    if (lng) {
        if (wp.page == 1 && k == 3)     /* QSK long: QSK TIME */
            open_edit(&set_qsk_time);
        panel_until = 0;
        return;
    }
    switch (wp.page * 4 + k) {
    case 0:                             /* V/M */
        toggle_vm();
        break;
    case 1:                             /* A/B */
        wp.cur_vfo = !wp.cur_vfo;
        if (!memory_mode())
            apply_all();
        break;
    case 2:                             /* MW */
        open_mw();
        break;
    case 3:                             /* MC */
        open_mc();
        break;
    case 4: open_edit(&set_cw_flt); break;        /* CWF */
    case 5: open_edit(&set_cw_tone); break;       /* CWT */
    case 6: open_edit(&set_cw_ratio); break;      /* CWR = CW RATIO */
    case 7:                             /* QSK: key-controlled TX on / off, icon on the main screen */
        wp.flags ^= WP_FLAG_QSK;
        apply_keyer();
        break;
    case 8: open_edit(&set_key_speed); break;     /* KS */
    case 9: open_edit(&set_key_mode); break;      /* KM */
    case 10: open_edit(&set_iambic); break;       /* IMB */
    case 11: open_csn(); break;                   /* CSN */
    case 12:                            /* SPL: split on / off */
        wp.flags ^= WP_FLAG_SPLIT;
        dirty = true;
        break;
    case 13: open_edit(&set_disp); break;         /* DIS */
    case 18: open_edit(&set_mic_gain); break;     /* MG */
    case 19: open_edit(&set_tx_power); break;     /* TXP */
    case 20: {                                    /* FILT (own page 6): current mode's filter */
        int fg = filt_group(cur()->mode);
        open_edit(fg < 0 ? &set_cw_flt : &set_filt[fg]);
        break;
    }
    case 21: open_edit(&set_agc); break;          /* AGC (own page 6) */
    case 22: open_edit(&set_sql); break;          /* SQL (own page 6) */
    case 23: open_menu(); break;                  /* SETT: SETTINGS menu (own) */
    case 14:                            /* BP */
        wp.flags ^= WP_FLAG_BEEP;
        popup(wp.flags & WP_FLAG_BEEP ? "BEEP ON" : "BEEP OFF");
        break;
    case 15:                            /* VER (own screen) */
        open_version();
        break;
    case 16:                            /* WFM */
        fm_enter();
        break;
    case 17:                            /* BSM */
        wp.flags ^= WP_FLAG_FULLBAND;
        popup(wp.flags & WP_FLAG_FULLBAND ? "FULL BAND" : "HAM BAND");
        break;
    default:
        popup("NOT YET");
        break;
    }
    panel_until = state == UI_MAIN ? millis() + PANEL_MS : 0;
}

/* Right part of the second line in memory mode / MW / MC: "MEMO|MW|MC" @85 + channel @115. */
static void draw_channel_tag(const char *tag, int ch, bool show_tag)
{
    char n[4];
    lcd_fill(85, 31, 127, 38, false);
    if (show_tag)
        lcd_text_f(&font8, 85, 31, tag, false);
    snprintf(n, sizeof n, "%02d", ch);
    lcd_text_f(&font8, 115, 31, n, false);
}

/* ---- WFM (stock 0x08033990) ----
 * Screen: "FM RADIO", frequency in 100 kHz in the huge font, labels QUIT < >.
 * Tune encoder: +-100 kHz (87.0..108.0 MHz). F2 / F3: SEEK down / up (100 kHz steps, a channel
 * with level > 9 stops it and is stored as 0xAA, passed ones are cleared). BAND- / BAND+: previous /
 * next stored channel. F1: back to HF. Volume encoder and volume push (PB0) work as usual.
 * The codec HP drivers are muted: the FM chip's audio goes straight to the amplifier.
 * Deviation: BAND-/BAND+ search from the current frequency (stock from the last stored channel). */
static int fm_seek_dir;                 /* 0 = idle, +-1 = seeking */
static bool fm_seek_level;              /* tuned, level read due at fm_seek_at */
static uint32_t fm_seek_at;

static int fm_index(uint32_t f)
{
    return (f - FM_MIN) / FM_STEP;
}

static void fm_tune(uint32_t f)
{
    wp.fm_freq = f;
    fm_set_freq(f);
    dirty = true;
}

static void fm_mark(uint32_t f, bool stored)
{
    uint8_t v = stored ? 0xAA : 0;
    if (fm_channels[fm_index(f)] != v) {
        fm_channels[fm_index(f)] = v;
        fm_channels_changed = true;
    }
}

static void fm_enter(void)
{
    codec_hp_gain(-7);
    codec_hpr_gain(-7);
    rf_fm_path();
    if (wp.fm_freq < FM_MIN || wp.fm_freq > FM_MAX)
        wp.fm_freq = FM_MIN;
    state = UI_FM;
    wp.flags |= WP_FLAG_FM;
    panel_until = 0;
    fm_seek_dir = 0;
    fm_mute(wp.volume == 0);
    delay_ms(100);
    fm_tune(wp.fm_freq);
}

static void fm_exit(void)
{
    fm_mute(true);
    if (fm_channels_changed && settings_save_fm_channels() == 0)
        fm_channels_changed = false;
    delay_ms(50);
    state = UI_MAIN;
    wp.flags &= ~WP_FLAG_FM;
    codec_hpr_gain(0);                  /* init value */
    apply_volume();
    apply_all();                        /* HF filters, LO, demodulator */
}

static void fm_seek_start(int dir)
{
    fm_seek_dir = dir;
    fm_seek_level = false;
    fm_seek_at = millis();
}

/* One seek step per call when due: tune, 50 ms later read the level, next step 50 ms after that. */
static void fm_seek_poll(uint32_t now)
{
    if (!fm_seek_dir || (int32_t)(now - fm_seek_at) < 0)
        return;
    if (fm_seek_level) {
        bool found = fm_level() > 9;
        fm_mark(wp.fm_freq, found);
        fm_seek_level = false;
        if (found)
            fm_seek_dir = 0;
        fm_seek_at = now + 50;
        dirty = true;
        return;
    }
    uint32_t f = wp.fm_freq + fm_seek_dir * (int32_t)FM_STEP;
    if (f < FM_MIN || f > FM_MAX) {     /* band edge: stop */
        fm_seek_dir = 0;
        dirty = true;
        return;
    }
    fm_tune(f);
    fm_seek_level = true;
    fm_seek_at = now + 50;
}

static void fm_channel_step(int dir)
{
    int i = fm_index(wp.fm_freq);
    for (int n = 0; n < NUM_FM_CHANNELS; n++) {
        i += dir;
        i = i < 0 ? NUM_FM_CHANNELS - 1 : i >= NUM_FM_CHANNELS ? 0 : i;
        if (fm_channels[i] == 0xAA) {
            fm_tune(FM_MIN + i * FM_STEP);
            return;
        }
    }
}

static void fm_key(int code)
{
    if (code == KEY_F1) {
        fm_exit();
    } else if (code == KEY_VOL_PUSH) {
        wp.flags ^= 0x80;
        apply_pb0();
    } else if (fm_seek_dir) {
        return;                         /* stock: only QUIT and PB0 while seeking */
    } else if (code == KEY_F2 || code == KEY_F3) {
        fm_seek_start(code == KEY_F3 ? 1 : -1);
    } else if (code == KEY_BAND_DOWN || code == KEY_BAND_UP) {
        fm_channel_step(code == KEY_BAND_UP ? 1 : -1);
    }
}

/* Stock 0x08033990 / 0x0802fca4. */
static void draw_fm(void)
{
    char s[8];
    lcd_clear();
    lcd_text_f(&font8, 0, 0, "FM RADIO", false);
    if (fm_seek_dir)
        lcd_text_f(&font8, 54, 0, "SEEK", false);
    snprintf(s, sizeof s, "%3lu", (unsigned long)(wp.fm_freq / 1000000));
    lcd_text_f(&font_fm, 0, 15, s, false);
    snprintf(s, sizeof s, "%lu", (unsigned long)(wp.fm_freq / 100000 % 10));
    lcd_text_f(&font_fm, 88, 15, s, false);
    lcd_text_f(&font16, 116, 35, "M", false);
    lcd_pixel(79, 42, true);            /* decimal point: emWin GUI_FillCircle(79, 44, 2) */
    lcd_hline(78, 80, 43, true);
    lcd_hline(77, 81, 44, true);
    lcd_hline(78, 80, 45, true);
    lcd_pixel(79, 46, true);
    lcd_text_f(&font8, 0, 55, "QUIT   <     >", false);
}

static void draw(void)
{
    char s[12];
    uint32_t now = millis();
    bool blink = (now / BLINK_MS) & 1;

    if (state == UI_FM) {
        draw_fm();
        lcd_flush();
        return;
    }
    if (state == UI_VER) {
        draw_version();
        lcd_flush();
        return;
    }

    if (state == UI_MENU) {
        draw_menu();
        lcd_flush();
        return;
    }
    if (state == UI_TXCAL) {
        draw_txcal();
        lcd_flush();
        return;
    }
    if (state == UI_CSN) {
        draw_csn(blink);
        lcd_flush();
        return;
    }

    /* Split TX (stock 0x08033400): the TX VFO takes over the main line, the RX one moves down. */
    bool stx = txing && (wp.flags & WP_FLAG_SPLIT) && !memory_mode();
    int vi = stx ? !wp.cur_vfo : wp.cur_vfo;
    const vfo_t *v = state == UI_MC ? &memories[dlg_ch] : stx ? &wp.vfo[vi] : cur();
    lcd_clear();
    lcd_bitmap(txing ? &icon_tx : &icon_vfo, 2, 1);
    lcd_text_f(&font8, 26, 1, mode_names[v->mode < MODE_COUNT ? v->mode : 0], false);
    if (wp.flags & 0x80)                /* PB0 (volume push), stock 0x0802fd3c */
        lcd_bitmap(&icon_x14, 14, 1);
    if (state == UI_MC ? (v->flags & 1) : preamp)
        lcd_bitmap(&icon_x50, 50, 1);
    if (wp.flags & WP_FLAG_LOCK)
        lcd_bitmap(&icon_x63, 63, 1);
    if (wp.flags & WP_FLAG_QSK)
        lcd_bitmap(&icon_x109, 109, 1);
    if (wp.flags & WP_FLAG_SPLIT)
        lcd_bitmap(&icon_x86, 86, 1);
    if (cat_link())                     /* stock 0x08033400: CAT frames seen recently */
        lcd_bitmap(&icon_cat, 75, 1);

    lcd_text_f(&font8, 3, 19, vi ? "B" : "A", false);
    draw_main_freq(v->freq, state == UI_MAIN || state == UI_MSGBOX ? *digit() : -1);

    if (popup_until && now < popup_until) {
        lcd_text_f(&font8, 3, 31, popup_text, false);
    } else if (disp_mode != 2) {
        lcd_text_f(&font8, 3, 31, vi ? "A " : "B ", false);
        fmt_freq(s, wp.vfo[!vi].freq);
        lcd_text_f(&font8, 16, 31, s, false);
    }

    switch (state) {
    case UI_MW:
        draw_channel_tag("MW", dlg_ch, blink);
        if (!MEM_VALID(memories[dlg_ch]))
            lcd_bitmap(&icon_e, 99, 33);
        draw_labels(mw_labels);
        break;
    case UI_MC:
        draw_channel_tag("MC", dlg_ch, blink);
        draw_labels(mc_labels);
        break;
    case UI_EDIT:
        draw_edit(blink);
        if (edit == &set_sql)
            draw_smeter();              /* own: the squelch arrow moves while editing */
        break;
    default:
        if (memory_mode())
            draw_channel_tag("MEMO", wp.mem_ch, true);
        if (panel_until && now < panel_until) {
            draw_panel();
        } else {
            static const uint8_t scope_h[3] = {15, 22, 34};
            if (disp_mode == 0)
                draw_smeter();
            draw_scope(63, scope_h[disp_mode]);
        }
        if (state == UI_MSGBOX)
            draw_msgbox();
        break;
    }
    lcd_flush();
}

/* MW / MC dialogs: F1 quits, F4 saves / clears, tune encoder selects the channel. */
static void dialog_key(int code)
{
    if (code == KEY_F1) {
        state = UI_MAIN;
    } else if (code == KEY_F4 && state == UI_MW) {
        vfo_t rec = *cur();
        rec.flags = (rec.flags & 0x00FF) | 0xAA00;
        state = UI_MAIN;
        if (settings_write_memory(dlg_ch, &rec) < 0)
            msgbox("ERROR", "CAN'T WRITE MEMORY!");
    } else if (code == KEY_F4 && state == UI_MC) {
        vfo_t rec = memories[dlg_ch];
        rec.flags &= 0x00FF;                    /* drop the 0xAA marker */
        state = UI_MAIN;
        if (settings_write_memory(dlg_ch, &rec) < 0) {
            msgbox("ERROR", "CAN'T WRITE MEMORY!");
        } else if (memory_mode()) {
            int ch = memory_find(dlg_ch, 1);
            if (ch < 0) {
                wp.flags &= ~WP_FLAG_MEMORY;
                apply_all();
            } else {
                memory_recall(ch);
            }
        }
    }
}

static void handle_key(int ev)
{
    int code = KEY_CODE(ev);
    bool lng = ev & KEY_LONG;

    if (state == UI_FM) {
        fm_key(code);
    } else if (state == UI_VER || state == UI_MSGBOX) {
        state = state == UI_VER && menu_back ? UI_MENU : UI_MAIN;
        menu_back = false;
    } else if (state == UI_MENU) {
        menu_key(code, lng);
    } else if (state == UI_TXCAL) {
        cal_key(code, lng);
    } else if (state == UI_EDIT) {
        if (!lng && code == KEY_F1)
            close_edit(false);
        else if (!lng && (code == KEY_F4 || code == KEY_TUNE_PUSH))
            close_edit(true);
    } else if (state == UI_CSN) {
        if (!lng)
            csn_key(code);
    } else if (state == UI_MW || state == UI_MC) {
        if (!lng)
            dialog_key(code);
    } else if (!lng && code == (KEY_BAND_DOWN | KEY_BAND_UP)) {
        wp.flags ^= WP_FLAG_LOCK;
    } else if (code == KEY_MODE) {
        if (lng) {
            preamp = !preamp;
            apply_preamp();
        } else {
            int m = cur()->mode;
            do
                m = (m + 1) % MODE_COUNT;
            while (!mode_enabled[m]);
            radio_set_mode(m);
        }
    } else if (code == KEY_BAND_DOWN || code == KEY_BAND_UP) {
        int dir = code == KEY_BAND_UP ? 1 : -1;
        if (lng) {
            int d = *digit() - dir;             /* BAND- long: digit +1 (left) */
            *digit() = d < 0 ? 7 : d > 7 ? 0 : d;
        } else if (memory_mode()) {
            memory_step(dir);
        } else {
            band_step(dir);
        }
    } else if (code == KEY_VOL_PUSH && !lng) {
        wp.flags ^= 0x80;
        apply_pb0();
    } else if (code == KEY_TUNE_PUSH && !lng) {
        panel_until = panel_until && millis() < panel_until ? 0 : millis() + PANEL_MS;
    } else if (code == KEY_F1 || code == KEY_F2 || code == KEY_F3 || code == KEY_F4) {
        bool open = panel_until && millis() < panel_until;
        panel_until = millis() + PANEL_MS;
        if (open)
            softkey(code == KEY_F1 ? 0 : code == KEY_F2 ? 1 : code == KEY_F3 ? 2 : 3, lng);
    } else {
        return;
    }
    dirty = true;
}

/* Setting editor / SETTINGS menu value change. Wide ranges (CW TONE, QSK TIME: 500..1000
 * values) step x10 while the dial is spun fast (detents < 50 ms apart); own, stock steps by 1. */
static void edit_spin(int d, uint32_t now)
{
    static uint32_t last;
    if (edit != &set_freq_cal && edit != &set_am_mod && edit->max - edit->min >= 100 && (now - last < 50 || d > 1 || d < -1))
        d *= 10;                        /* FREQ CAL: always 0.01 ppm */
    last = now;
    int v = edit_value + d;
    edit_value = v < edit->min ? edit->min : v > edit->max ? edit->max : v;
    setting_set(edit, edit_value);
}

static void tune(int d, uint32_t now)
{
    if (state == UI_FM) {
        if (fm_seek_dir)
            return;                     /* stock ignores the dial while seeking */
        int64_t f = (int64_t)wp.fm_freq + (int64_t)d * FM_STEP;
        fm_tune(f < FM_MIN ? FM_MIN : f > FM_MAX ? FM_MAX : (uint32_t)f);
    } else if (state == UI_EDIT || (state == UI_MENU && menu_editing)) {
        edit_spin(d, now);              /* stock applies live, restores on QUIT */
    } else if (state == UI_MENU) {
        menu_cursor(d > 0 ? 1 : -1);
    } else if (state == UI_TXCAL) {
        if (cal_confirm)
            return;
        if (cal_edit)
            cal_dial(d);
        else
            cal_row = ((cal_row + (d > 0 ? 1 : -1)) % CAL_ROWS + CAL_ROWS) % CAL_ROWS;
    } else if (state == UI_VER) {
        int t = ver_top + (d > 0 ? 1 : -1);
        ver_top = t < 0 ? 0 : t > VER_ROWS - VER_VIS ? VER_ROWS - VER_VIS : t;
    } else if (state == UI_CSN) {
        csn_dial(d);
    } else if (state == UI_MW) {
        dlg_ch = wrap_ch(dlg_ch + d);
    } else if (state == UI_MC) {
        int ch = memory_find(wrap_ch(dlg_ch + d), d);
        if (ch >= 0)
            dlg_ch = ch;
    } else if (panel_until && now < panel_until) {
        int p = wp.page + (d > 0 ? 1 : -1);
        wp.page = p < 0 ? NUM_PAGES - 1 : p >= NUM_PAGES ? 0 : p;
        panel_until = now + PANEL_MS;
    } else if (wp.flags & WP_FLAG_LOCK) {
        return;
    } else if (memory_mode()) {
        memory_step(d > 0 ? 1 : -1);
    } else {
        uint32_t st = 1;
        for (int i = 0; i < *digit(); i++)
            st *= 10;
        int64_t f = (int64_t)cur()->freq + (int64_t)d * st;
        radio_set_freq(f < (int64_t)F_MIN ? F_MIN : (uint32_t)f);
    }
    dirty = true;
}

void radio_poll(void)
{
    static uint32_t last_draw;
    uint32_t now = millis();

    if (state == UI_MSGBOX && now >= msgbox_until)
        state = UI_MAIN;

    int d = input_encoder_delta(ENC_TUNE);
    if (d)
        tune(d, now);

    d = input_encoder_delta(ENC_AUX);
    if (d) {
        int v = wp.volume + d;
        wp.volume = v < 0 ? 0 : v > 46 ? 46 : v;
        apply_volume();
        dirty = true;
    }

    /* PTT (PC2, active low, 20 ms debounce) or CAT PTT (CI-V 1C 00, stock 0x20000318):
     * voice/data TX in SSB, AM and NFM, level-triggered as stock radio_trx_task. No band
     * limits (user choice; stock allows ham bands only). CW keying: TODO. */
    static uint32_t ptt_since;
    static bool ptt_state;
    bool ptt_raw = input_raw_misc() & 1;
    if (ptt_raw != ptt_state) {
        if (!ptt_since)
            ptt_since = now;
        if (now - ptt_since >= 20) {
            ptt_state = ptt_raw;
            ptt_since = 0;
        }
    } else {
        ptt_since = 0;
    }
    bool want_tx = ptt_state || cat_ptt();
    int m = cur()->mode;
    bool cw = m == MODE_CW || m == MODE_CWR;
    keyer_enable(cw && state == UI_MAIN);
    if (state == UI_TXCAL) {
        cal_poll_tx(want_tx);
    } else if (cw) {
        /* CW (stock radio_trx_task + keyer): the paddles request TX, the keyer ramps the carrier
         * and stays "active" for the QSK time after the last element. PTT / CAT PTT only
         * switch the T/R relays (no carrier without the key), as stock. */
        bool req = keyer_request() && (wp.flags & WP_FLAG_QSK);
        if ((want_tx || req) && !txing && state == UI_MAIN) {
            if (radio_tx_start(AUDIO_TX_CARRIER, -1) == 0)
                keyer_set_ready(true);
        } else if (txing && !want_tx && !keyer_active()) {
            keyer_set_ready(false);
            radio_tx_stop();
        }
    } else if (want_tx && !txing && (state == UI_MAIN || state == UI_MENU)) {
        int sig = m == MODE_AM ? AUDIO_TX_AM : m == MODE_NFM ? AUDIO_TX_NFM
                : (m == MODE_USB || m == MODE_UD) ? AUDIO_TX_SSB_U : AUDIO_TX_SSB_L;
        radio_tx_start(sig, -1);
    } else if (!want_tx && txing) {
        radio_tx_stop();
    }

    static bool cat_shown;
    if (cat_link() != cat_shown) {
        cat_shown = !cat_shown;
        dirty = true;
    }

    int ev;
    while ((ev = input_key_event()) >= 0) {
        if (txing && state != UI_TXCAL) /* keys are ignored in TX (not in TX IQ CAL, as stock) */
            continue;
        if ((wp.flags & WP_FLAG_BEEP) && !txing)
            beep(2000, 20);
        handle_key(ev);
    }

    if (state == UI_FM) {
        fm_seek_poll(now);
        if (dirty || now - last_draw >= 200) {
            dirty = false;
            last_draw = now;
            draw();
        }
        return;
    }

    bool fresh = spectrum_poll(cur()->mode, disp_mode, preamp, scope_zoom / 10.0f, txing);
    squelch_poll(fresh, now);
    if (dirty || (fresh && now - last_draw >= 40) || now - last_draw >= 200) {
        dirty = false;
        last_draw = now;
        draw();
    }
}

/* CI-V 1D 01 (Xiegu): stock opens its SYSTEM CAL menu (0x080330b0) from the main loop; here the
 * SETTINGS > CAL category. Only from the main screen in RX. */
void radio_cat_syscal(void)
{
    if (txing || state != UI_MAIN)
        return;
    menu_open_cat(MENU_CAT_CAL);
    open_menu();
    dirty = true;
}
