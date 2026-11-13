/* CAT on USART1, following the stock firmware: Icom CI-V, 19200 8N1, no echo
 * (stock: init 0x08026788, frame parser 0x08021f1c, command handler 0x0802eb00).
 *
 * Frame: FE FE <to> <from> <cmd> [data...] FD. Like stock, any <to> address is accepted and
 * the reply goes back with the addresses swapped: FE FE <from> <to> ... FD. Answers are either
 * FB (ok), FA (not good) or <cmd> <data>. Frequencies are 5 BCD bytes, least significant first.
 *
 *   00/03/05            read / set frequency (550 kHz .. 30 MHz, clamped)
 *   01/04/06            read / set mode: [mode filter]; 00 LSB 01 USB 02 AM 03 CW 05 FM 07 CW-R,
 *                       LSB/USB become L-D/U-D while the data mode is on (1A 06)
 *   02                  band edges 550 kHz - 30 MHz
 *   07                  (none) VFO mode, 00 VFO A, 01 VFO B, A0 A=B, B0 swap A/B
 *   0F                  split: read / set 00, 01 (work flags bit2, as the SPL soft key)
 *   11                  attenuator: none (reads 0, set ignored)
 *   14 01 / 14 0A       AF level 0..255 (volume 0..46) / RF power (TXP LOW/MID/HIGH);
 *                       other 14 xx levels are only stored, as stock
 *   15 02               S-meter 0..255 = -123..-23 dBm (stock scale)
 *   16 02 / 16 12 / 16 50   preamp / AGC (read only, stock values) / dial lock
 *   19 00               transceiver ID 0x70
 *   1A 03 / 1A 06       filter width (read) / data mode (L-D, U-D)
 *   1C 00               PTT: TX in SSB, AM, NFM like the PTT input; dropped if no frame
 *                       arrives for CAT_PTT_MS (stock does this too)
 *   1D 01 / 1D 19 / 1D 02 01 xx   Xiegu: ok / 19 01 06 / PB0 output (volume push)
 *   21 00/01/02         RIT offset / RIT / delta-TX: only stored, as stock
 *   25 / 26             frequency / mode of the selected (00) or unselected (01) VFO
 *
 * Deviations from stock (protocol fixes): malformed frames and unknown modes get FA (stock
 * answers FB or picks AM); 1A 03 is BCD with the real filter width (stock sends fixed binary
 * values); 25 / 26 echo the VFO selector (stock: the current VFO number) and 25 tunes at once;
 * 14 0A sets TXP; frequency, mode and VFO changes are refused (FA) while transmitting or in
 * WFM. */
#include <string.h>
#include "system.h"
#include "uart.h"
#include "radio.h"
#include "settings.h"
#include "spectrum.h"
#include "cat.h"

#define CAT_BAUD    19200
#define CAT_ID      0x70        /* stock answer to 19 00 */
#define CAT_LINK_MS 10000       /* CAT icon after the last frame */
#define CAT_PTT_MS  60000       /* CAT PTT watchdog */
#define MAX_DATA    32

#define F_MIN 550000u
#define F_MAX 30000000u

enum { R_NG, R_OK, R_DATA };

static enum { S_IDLE, S_PRE, S_TO, S_FROM, S_CMD, S_DATA } st;
static uint8_t to, from, cmd, data[MAX_DATA];
static unsigned len;
static bool overflow;
static uint8_t resp[16];
static unsigned rlen;

static bool ptt, seen;
static uint32_t last_frame;

/* Stored-only values, as stock (0x20002518 levels, 0x20000324 RIT, 0x20000312/3 flags). */
static uint16_t levels[0x1A];
static int16_t rit_hz;
static uint8_t rit_on, dtx_on;

void cat_init(void)
{
    uart_init(CAT_BAUD);
}

bool cat_ptt(void)
{
    return ptt;
}

bool cat_link(void)
{
    return seen && millis() - last_frame < CAT_LINK_MS;
}

/* ---- encoding ---- */

static uint8_t bcd(unsigned v)
{
    return (v / 10 % 10) << 4 | v % 10;
}

static unsigned unbcd(uint8_t b)
{
    return (b >> 4) * 10 + (b & 15);
}

static void put_freq(uint8_t *p, uint32_t f)
{
    for (int i = 0; i < 5; i++, f /= 100)
        p[i] = bcd(f % 100);
}

static uint32_t get_freq(const uint8_t *p)
{
    uint32_t f = 0;
    for (int i = 4; i >= 0; i--)
        f = f * 100 + unbcd(p[i]);
    return f;
}

/* Level value 0..9999 as 2 BCD bytes, most significant first. */
static void put_level(uint8_t *p, unsigned v)
{
    p[0] = bcd(v / 100);
    p[1] = bcd(v);
}

static void reply(const uint8_t *d, unsigned n)
{
    memcpy(resp, d, n);
    rlen = n;
}

/* ---- modes ---- */

static bool is_data(int m)
{
    return m == MODE_LD || m == MODE_UD;
}

static uint8_t civ_mode(int m)
{
    static const uint8_t t[MODE_COUNT] = {0x00, 0x00, 0x01, 0x01, 0x03, 0x07, 0x05, 0x02};
    return t[m < MODE_COUNT ? m : 0];
}

/* CI-V mode -> own mode, -1 if unknown. */
static int mode_of(uint8_t c, bool data_mode)
{
    switch (c) {
    case 0x00: return data_mode ? MODE_LD : MODE_LSB;
    case 0x01: return data_mode ? MODE_UD : MODE_USB;
    case 0x02: return MODE_AM;
    case 0x03: return MODE_CW;
    case 0x05: return MODE_NFM;
    case 0x07: return MODE_CWR;
    default: return -1;
    }
}

/* Icom filter-width index (1A 03): SSB/CW 0..9 = 50..500 Hz, 10..40 = 600..3600 Hz;
 * AM/FM 0..49 = 200 Hz..10 kHz. */
static unsigned filter_index(int m, int hz)
{
    int i;
    if (m == MODE_AM || m == MODE_NFM)
        i = hz / 200 - 1;
    else if (hz <= 500)
        i = hz / 50 - 1;
    else
        i = 10 + (hz - 600) / 100;
    int max = m == MODE_AM || m == MODE_NFM ? 49 : 40;
    return i < 0 ? 0 : i > max ? max : i;
}

/* 0 = selected VFO, 1 = unselected (25 / 26). */
static int vfo_of(uint8_t sel)
{
    return sel ? !radio_cur_vfo() : radio_cur_vfo();
}

/* ---- commands (stock 0x0802eb00) ---- */

static int command(void)
{
    uint8_t r[12];
    uint8_t sub = len ? data[0] : 0;

    switch (cmd) {
    case 0x00: case 0x03: case 0x05:            /* frequency */
        if (len == 0) {
            put_freq(r, radio_freq());
            reply(r, 5);
            return R_DATA;
        }
        if (len != 5 || !radio_cat_can_tune())
            return R_NG;
        radio_set_freq(get_freq(data));
        return R_OK;

    case 0x01: case 0x04: case 0x06: {          /* mode */
        int m = radio_mode();
        if (len == 0) {
            r[0] = civ_mode(m);
            r[1] = m == MODE_CW || m == MODE_CWR ? stock_settings.cw_filter + 1 : 1;
            reply(r, 2);
            return R_DATA;
        }
        int nm = mode_of(sub, is_data(m));
        if (len > 2 || nm < 0 || !radio_cat_can_tune())
            return R_NG;
        radio_set_mode(nm);
        return R_OK;
    }

    case 0x02:                                  /* band edges */
        put_freq(r, F_MIN);
        r[5] = 0x2D;
        put_freq(r + 6, F_MAX);
        reply(r, 11);
        return R_DATA;

    case 0x07:                                  /* VFO */
        if (!radio_cat_can_tune())
            return R_NG;
        if (len == 0) {
            radio_leave_memory();
            return R_OK;
        }
        if (len != 1)
            return R_NG;
        switch (sub) {
        case 0x00: case 0x01: radio_select_vfo(sub); return R_OK;
        case 0xA0: radio_copy_vfo(); return R_OK;
        case 0xB0: radio_select_vfo(-1); return R_OK;
        default: return R_NG;
        }

    case 0x0F:                                  /* split: flags bit2 */
        if (len == 0) {
            r[0] = radio_split();
            reply(r, 1);
            return R_DATA;
        }
        if (len == 1 && sub <= 1) {
            radio_set_split(sub);
            return R_OK;
        }
        return R_NG;

    case 0x11:                                  /* attenuator: none */
        if (len == 0) {
            r[0] = 0;
            reply(r, 1);
            return R_DATA;
        }
        return R_OK;

    case 0x14: {                                /* levels */
        if (len == 1) {
            unsigned v;
            if (sub == 0x01)
                v = radio_volume() * 255 / 46;
            else if (sub == 0x0A)
                v = (const uint8_t[]){51, 102, 255}[radio_tx_power()];
            else
                v = sub < 0x1A ? levels[sub] : 0;
            r[0] = sub;
            put_level(r + 1, v);
            reply(r, 3);
            return R_DATA;
        }
        if (len != 2 && len != 3)
            return R_NG;
        unsigned v = len == 2 ? unbcd(data[1]) : unbcd(data[1]) * 100 + unbcd(data[2]);
        v = v > 255 ? 255 : v;
        if (sub == 0x01)
            radio_set_volume(v * 46 / 255);
        else if (sub == 0x0A)
            radio_set_tx_power(v < 77 ? 0 : v < 179 ? 1 : 2);
        else if (sub < 0x1A)
            levels[sub] = v;
        return R_OK;
    }

    case 0x15: {                                /* meters */
        if (len != 1)
            return R_NG;
        unsigned v = 0;
        if (sub == 0x02) {                      /* S-meter, stock: (dBm + 123) x 2.55 */
            int dbm = (int)spectrum_dbm();
            dbm = dbm < -123 ? -123 : dbm > -23 ? -23 : dbm;
            v = (unsigned)((dbm + 123) * 2.55f);
        }
        r[0] = sub;
        put_level(r + 1, v);
        reply(r, 3);
        return R_DATA;
    }

    case 0x16:                                  /* functions */
        if (len == 1) {
            int m = radio_mode();
            r[0] = sub;
            r[1] = sub == 0x02 ? radio_preamp()
                 : sub == 0x12 ? (m == MODE_LSB || m == MODE_USB ? 2 : m == MODE_CW || m == MODE_CWR ? 1 : 3)
                 : sub == 0x50 ? radio_lock() : 0;
            reply(r, 2);
            return R_DATA;
        }
        if (len != 2)
            return R_NG;
        if (sub == 0x02)
            radio_set_preamp(data[1]);
        else if (sub == 0x50)
            radio_set_lock(data[1]);
        return R_OK;

    case 0x19:                                  /* transceiver ID */
        if (len != 1)
            return R_NG;
        r[0] = sub;
        r[1] = sub == 0 ? CAT_ID : 0;
        reply(r, 2);
        return R_DATA;

    case 0x1A:
        if (len == 1 && sub == 0x03) {          /* filter width */
            r[0] = sub;
            r[1] = bcd(filter_index(radio_mode(), radio_filter_hz()));
            reply(r, 2);
            return R_DATA;
        }
        if (len == 1 && sub == 0x06) {          /* data mode */
            r[0] = sub;
            r[1] = is_data(radio_mode());
            r[2] = 0;
            reply(r, 3);
            return R_DATA;
        }
        if (len == 3 && sub == 0x06) {          /* stock 0x0802a068: LSB <-> L-D, USB <-> U-D */
            if (!radio_cat_can_tune())
                return R_NG;
            int m = radio_mode();
            if (data[1] && (m == MODE_LSB || m == MODE_USB))
                radio_set_mode(m + 1);
            else if (!data[1] && is_data(m))
                radio_set_mode(m - 1);
            return R_OK;
        }
        return R_NG;

    case 0x1C:                                  /* PTT */
        if (len == 1) {
            r[0] = sub;
            r[1] = sub == 0 ? radio_tx_active() : 0;
            reply(r, 2);
            return R_DATA;
        }
        if (len < 2)
            return R_NG;
        if (sub == 0)
            ptt = data[1] != 0;
        return R_OK;

    case 0x1D:                                  /* Xiegu */
        if (len == 1 && sub == 0x19) {
            reply((const uint8_t[]){0x19, 0x01, 0x06}, 3);
            return R_DATA;
        }
        if (len == 3 && sub == 0x02 && data[1] == 0x01)
            radio_set_pb0(data[2]);
        return R_OK;

    case 0x21:                                  /* RIT: stored only, as stock */
        if (len == 1 && sub == 0x00) {
            unsigned a = rit_hz < 0 ? -rit_hz : rit_hz;
            r[0] = sub;
            r[1] = bcd(a);
            r[2] = bcd(a / 100);
            r[3] = rit_hz < 0;
            reply(r, 4);
            return R_DATA;
        }
        if (len == 1 && (sub == 0x01 || sub == 0x02)) {
            r[0] = sub;
            r[1] = sub == 0x01 ? rit_on : dtx_on;
            reply(r, 2);
            return R_DATA;
        }
        if (len == 4 && sub == 0x00) {
            int v = unbcd(data[2]) * 100 + unbcd(data[1]);
            rit_hz = data[3] ? -v : v;
            return R_OK;
        }
        if (len == 2 && sub == 0x01) {
            rit_on = data[1];
            return R_OK;
        }
        if (len == 2 && sub == 0x02) {
            dtx_on = data[1];
            return R_OK;
        }
        return R_NG;

    case 0x25:                                  /* frequency of VFO sel */
        if (len == 1) {
            r[0] = sub;
            put_freq(r + 1, radio_vfo_freq(vfo_of(sub)));
            reply(r, 6);
            return R_DATA;
        }
        if (len != 6 || !radio_cat_can_tune())
            return R_NG;
        radio_set_vfo(vfo_of(sub), get_freq(data + 1), -1);
        return R_OK;

    case 0x26: {                                /* mode of VFO sel: [sel mode data filter] */
        int v = vfo_of(sub), m = radio_vfo_mode(v);
        if (len == 1) {
            r[0] = sub;
            r[1] = civ_mode(m);
            r[2] = is_data(m);
            r[3] = 1;
            reply(r, 4);
            return R_DATA;
        }
        if (len < 2 || len > 4 || !radio_cat_can_tune())
            return R_NG;
        int nm = mode_of(data[1], len >= 3 ? data[2] != 0 : is_data(m));
        if (nm < 0)
            return R_NG;
        radio_set_vfo(v, 0, nm);
        return R_OK;
    }

    default:
        return R_NG;
    }
}

static void execute(void)
{
    rlen = 0;
    int res = overflow ? R_NG : command();
    uint8_t out[6 + sizeof resp] = {0xFE, 0xFE, from, to};
    unsigned n = 4;
    if (res == R_DATA) {
        out[n++] = cmd;
        memcpy(out + n, resp, rlen);
        n += rlen;
    } else {
        out[n++] = res == R_OK ? 0xFB : 0xFA;
    }
    out[n++] = 0xFD;
    uart_write(out, n);
}

void cat_poll(void)
{
    int c;
    while ((c = uart_getc()) >= 0) {
        if (c == 0xFE) {                        /* preamble, may repeat */
            st = st == S_PRE || st == S_TO ? S_TO : S_PRE;
            continue;
        }
        if (c == 0xFD) {
            if (st == S_DATA) {
                last_frame = millis();
                seen = true;
                execute();
            }
            st = S_IDLE;
            continue;
        }
        switch (st) {
        case S_TO: to = c; st = S_FROM; break;
        case S_FROM: from = c; st = S_CMD; break;
        case S_CMD: cmd = c; len = 0; overflow = false; st = S_DATA; break;
        case S_DATA:
            if (len < MAX_DATA)
                data[len++] = c;
            else
                overflow = true;
            break;
        default: st = S_IDLE; break;            /* FC (collision) or noise */
        }
    }
    if (ptt && millis() - last_frame >= CAT_PTT_MS)
        ptt = false;                            /* the program went away while in TX */
}
