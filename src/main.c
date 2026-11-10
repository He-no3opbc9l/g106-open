#include <string.h>
#include "stm32f4.h"
#include "system.h"
#include "board.h"
#include "uart.h"
#include "display.h"
#include "input.h"
#include "beep.h"
#include "i2c.h"
#include "versaclock.h"
#include "codec.h"
#include "audio.h"
#include "radio.h"
#include "settings.h"
#include <math.h>

#define POWER_OFF_HOLD_MS 2000         /* stock 0x20000774 = 2000 ticks */

static const char banner[] = "\nG106 open firmware, build " __DATE__ " " __TIME__ "\n";

static void dump_gpio(const char *name, GPIO_TypeDef *p)
{
    uart_puts(name);
    uart_puts(" MODER=");
    uart_puthex(p->MODER, 8);
    uart_puts(" IDR=");
    uart_puthex(p->IDR, 4);
    uart_puts(" ODR=");
    uart_puthex(p->ODR, 4);
    uart_puts("\n");
}

static unsigned parse_uint(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void ticks_1ms(void)
{
    input_tick_1ms();
    beep_tick_1ms();
}

static const char *skip_word(const char *s)
{
    while (*s && *s != ' ')
        s++;
    while (*s == ' ')
        s++;
    return s;
}

static unsigned parse_num(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        unsigned v = 0;
        for (s += 2;; s++) {
            unsigned d = *s >= '0' && *s <= '9' ? *s - '0'
                       : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : 16;
            if (d > 15)
                return v;
            v = v * 16 + d;
        }
    }
    return parse_uint(s);
}

#define EEPROM_ADDR 0x50

/* Read-only hex dump of the 24Cxx (16-bit word address), 16 bytes per line. */
static void eeprom_dump(unsigned start, unsigned len)
{
    uint8_t buf[16];
    for (unsigned a = start; a < start + len; a += 16) {
        uint8_t wa[2] = {a >> 8, a & 0xFF};
        uart_puthex(a, 4);
        uart_puts(":");
        if (i2c_write_read(I2C_BUS1, EEPROM_ADDR, wa, 2, buf, 16) < 0) {
            uart_puts(" ERR\n");
            return;
        }
        for (int i = 0; i < 16; i++) {
            uart_putc(' ');
            uart_puthex(buf[i], 2);
        }
        uart_puts("\n");
    }
}

/* ---- RF diagnostics ---- */
#define FFT_N 1024
static float iq[2 * FFT_N];

static void fft(float *x, int n)        /* in-place radix-2, interleaved complex */
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = x[2 * i]; x[2 * i] = x[2 * j]; x[2 * j] = t;
            t = x[2 * i + 1]; x[2 * i + 1] = x[2 * j + 1]; x[2 * j + 1] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2 * 3.14159265f / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                float c = cosf(a * k), s = sinf(a * k);
                float *u = &x[2 * (i + k)], *v = &x[2 * (i + k + len / 2)];
                float vr = v[0] * c - v[1] * s, vi = v[0] * s + v[1] * c;
                v[0] = u[0] - vr; v[1] = u[1] - vi;
                u[0] += vr; u[1] += vi;
            }
    }
}

static void print_db(float p)
{
    int db = (int)(10.0f * log10f(p + 1e-30f));
    if (db < 0) {
        uart_putc('-');
        db = -db;
    }
    uart_putdec(db);
    uart_puts(" dB");
}

/* Complex spectrum of the raw I/Q: strongest bins (signed Hz) and their images. */
static void iq_fft(void)
{
    audio_capture_iq(iq, FFT_N);
    for (int i = 0; i < FFT_N; i++) {               /* Hann window */
        float w = 0.5f - 0.5f * cosf(2 * 3.14159265f * i / FFT_N);
        iq[2 * i] *= w;
        iq[2 * i + 1] *= w;
    }
    fft(iq, FFT_N);
    static float p[FFT_N];
    for (int k = 0; k < FFT_N; k++)
        p[k] = iq[2 * k] * iq[2 * k] + iq[2 * k + 1] * iq[2 * k + 1];
    p[0] = p[1] = p[FFT_N - 1] = 0;                 /* skip DC */
    for (int n = 0; n < 3; n++) {
        int best = 1;
        for (int k = 1; k < FFT_N; k++)
            if (p[k] > p[best])
                best = k;
        int hz = (best < FFT_N / 2 ? best : best - FFT_N) * AUDIO_FS / FFT_N;
        int img = (FFT_N - best) % FFT_N;
        float pi = p[img];
        uart_puts(hz < 0 ? "  -" : "  +");
        uart_putdec(hz < 0 ? -hz : hz);
        uart_puts(" Hz: ");
        print_db(p[best]);
        uart_puts(", image rejection ");
        print_db(p[best] / (pi + 1e-30f));
        uart_puts("\n");
        for (int d = -2; d <= 2; d++)
            p[(best + d + FFT_N) % FFT_N] = 0;
    }
}

/* One-shot report for remote debugging: tune to a known signal, type `report`, send all. */
static void report(void)
{
    static const char *const mn[8] = {"LSB", "L-D", "USB", "U-D", "CW", "CWR", "NFM", "AM"};
    uart_puts(banner + 1);
    uart_puts("freq ");
    uart_putdec(radio_freq());
    uart_puts(" Hz, mode ");
    uart_puts(mn[radio_mode() & 7]);
    uart_puts(", filt ssb/data/am/nfm ");
    for (int k = 0; k < FILT_COUNT; k++) {
        uart_putdec(filt_hi[k] * 100);
        uart_puts(k < FILT_COUNT - 1 ? "/" : "");
    }
    uart_puts(" Hz, preamp ");
    uart_putdec(preamp);
    uart_puts("\n");
    float bg, bp;
    audio_iq_balance(&bg, &bp);
    uint32_t nres, first;
    audio_dsp_faults(&nres, &first);
    uart_puts("iq balance gain x1000 ");
    uart_putdec((uint32_t)(bg * 1000.0f + 0.5f));
    uart_puts(" phase x1000 ");
    uart_puts(bp < 0 ? "-" : "");
    uart_putdec((uint32_t)((bp < 0 ? -bp : bp) * 1000.0f + 0.5f));
    uart_puts(", nan resets ");
    uart_putdec(nres);
    uart_puts("/");
    uart_putdec(first);
    uart_puts("\n");
    float u, l;
    audio_sideband_power(&u, &l);
    delay_ms(1000);
    audio_sideband_power(&u, &l);
    uart_puts("demod power (SSB/CW modes only): USB side ");
    print_db(u / 16000.0f);
    uart_puts(", LSB side ");
    print_db(l / 16000.0f);
    uart_puts(", USB-LSB ");
    float r = (u + 1e-20f) / (l + 1e-20f);
    if (r < 1) {
        uart_puts("-");
        r = 1 / r;
    }
    print_db(r);
    uart_puts("\nraw I/Q peaks (Hz here = raw bin; RF offset from dial = minus this):\n");
    iq_fft();
    uart_puts("end of report\n");
}

static void run_command(const char *cmd)
{
    if (!strcmp(cmd, "help")) {
        uart_puts("help ver gpio uptime off | lcd <text> | contrast <0-255> | bl on|off | beep <hz> | i2cscan 1|2 | i2cstat | vc | eedump <start> <len>\n"
                  "audio off|tone|mic|iq | vol <0-127> | hp <dB> | gain <x10> | astat\n"
                  "f <hz> | mode <0-7: LSB L-D USB U-D CW CWR NFM AM> | agc <max dB> | sig | iqfft\n"
                  "tx carrier|usb|lsb [0-2] | tx off | report\n");
    } else if (!strcmp(cmd, "ver")) {
        uart_puts(banner + 1);
    } else if (!strcmp(cmd, "gpio")) {
        dump_gpio("A", GPIOA);
        dump_gpio("B", GPIOB);
        dump_gpio("C", GPIOC);
        dump_gpio("H", GPIOH);
    } else if (!strcmp(cmd, "uptime")) {
        uart_putdec(millis());
        uart_puts(" ms\n");
    } else if (!strcmp(cmd, "off")) {
        uart_puts("power off\n");
        radio_power_off();
    } else if (!strncmp(cmd, "lcd ", 4)) {
        lcd_clear();
        lcd_text(0, 0, cmd + 4, 1, true);
        lcd_flush();
    } else if (!strncmp(cmd, "contrast ", 9)) {
        lcd_contrast(parse_uint(cmd + 9));
    } else if (!strcmp(cmd, "bl on") || !strcmp(cmd, "bl off")) {
        lcd_backlight(cmd[4] == 'n');
    } else if (!strncmp(cmd, "beep ", 5)) {
        beep(parse_uint(cmd + 5), 200);
    } else if (!strncmp(cmd, "i2cscan ", 8)) {
        int bus = parse_uint(cmd + 8) == 2 ? I2C_BUS2 : I2C_BUS1;
        for (unsigned a = 0x08; a < 0x78; a++)
            if (i2c_probe(bus, a) == 0) {
                uart_puts(" 0x");
                uart_puthex(a, 2);
            }
        uart_puts("\n");
    } else if (!strcmp(cmd, "vc")) {            /* VersaClock registers 0x00..0x7F */
        uint8_t r[16];
        for (unsigned a = 0; a < 0x80; a += 16) {
            uart_puthex(a, 2);
            uart_puts(":");
            if (vc_read(a, r, 16) < 0) {
                uart_puts(" read FAIL\n");
                continue;
            }
            for (int k = 0; k < 16; k++) {
                uart_puts(" ");
                uart_puthex(r[k], 2);
            }
            uart_puts("\n");
        }
    } else if (!strcmp(cmd, "i2cstat")) {       /* I2C1 registers, failures, re-inits */
        uint32_t r[4];
        i2c_regs(I2C_BUS1, r);
        static const char *const n[4] = {"CR1 ", " CR2 ", " SR1 ", " SR2 "};
        for (int k = 0; k < 4; k++) {
            uart_puts(n[k]);
            uart_puthex(r[k], 4);
        }
        uart_puts("\nfails ");
        uart_putdec(i2c_fails[I2C_BUS1]);
        uart_puts(" inits ");
        uart_putdec(i2c_inits[I2C_BUS1]);
        uart_puts("\nlast fail CR1 ");
        uart_puthex(i2c_last[I2C_BUS1][0], 4);
        uart_puts(" SR1 ");
        uart_puthex(i2c_last[I2C_BUS1][1], 4);
        uart_puts(" SR2 ");
        uart_puthex(i2c_last[I2C_BUS1][2], 4);
        uart_puts("\n");
    } else if (!strncmp(cmd, "eedump ", 7)) {
        const char *p = cmd + 7;
        unsigned start = parse_num(p);
        p = skip_word(p);
        eeprom_dump(start & ~15u, *p ? parse_num(p) : 256);
    } else if (!strncmp(cmd, "audio ", 6)) {
        const char *a = cmd + 6;
        if (!strcmp(a, "tone"))
            audio_set_mode(AUDIO_TONE);
        else if (!strcmp(a, "mic")) {
            codec_route_mic_monitor();
            audio_set_mode(AUDIO_MIC);
        } else if (!strcmp(a, "iq")) {
            codec_route_rx_iq();
            audio_set_mode(AUDIO_IQ);
        } else
            audio_set_mode(AUDIO_OFF);
    } else if (!strncmp(cmd, "vol ", 4)) {
        uart_puts(volume_set(parse_uint(cmd + 4)) < 0 ? "digipot error\n" : "ok\n");
    } else if (!strncmp(cmd, "hp ", 3)) {
        int neg = cmd[3] == '-';
        int db = parse_uint(cmd + 3 + neg);
        codec_hp_gain(neg ? -db : db);
    } else if (!strncmp(cmd, "gain ", 5)) {
        audio_set_gain(parse_uint(cmd + 5) / 10.0f);
    } else if (!strcmp(cmd, "astat")) {
        float pl, pr;
        uint32_t n;
        audio_stats(&pl, &pr, &n);
        uart_puts("blocks ");
        uart_putdec(n);
        uart_puts(" peak L ");
        uart_putdec((uint32_t)(pl * 100000));
        uart_puts(" R ");
        uart_putdec((uint32_t)(pr * 100000));
        uart_puts(" (x1e-5 FS)\n");
        uint32_t avg, mx;
        audio_cpu_load(&avg, &mx);
        uart_puts("dsp cpu avg ");
        uart_putdec(avg);
        uart_puts("% max ");
        uart_putdec(mx);
        uart_puts("%\n");
        uint32_t cyc[4];
        audio_profile(cyc);
        uart_puts("cycles front ");
        uart_putdec(cyc[0]);
        uart_puts(" dec ");
        uart_putdec(cyc[1]);
        uart_puts(" demod ");
        uart_putdec(cyc[2]);
        uart_puts(" interp ");
        uart_putdec(cyc[3]);
        uart_puts("\n");
        uint32_t nres, first;
        audio_dsp_faults(&nres, &first);
        uart_puts("dsp nan resets ");
        uart_putdec(nres);
        uart_puts(" first var ");
        uart_putdec(first);
        float bg, bp;
        audio_iq_balance(&bg, &bp);
        uart_puts("\niq balance gain x1000 ");
        uart_putdec((uint32_t)(bg * 1000.0f + 0.5f));
        uart_puts(" phase x1000 ");
        if (bp < 0) {
            uart_putc('-');
            bp = -bp;
        }
        uart_putdec((uint32_t)(bp * 1000.0f + 0.5f));
        uart_puts("\ntx peak x1000 (1000 = limiter) ");
        uart_putdec((uint32_t)(audio_tx_peak() * 1000.0f + 0.5f));
        uart_puts("\n");
    } else if (!strncmp(cmd, "f ", 2)) {
        radio_set_freq(parse_uint(cmd + 2));
        uart_putdec(radio_freq());
        uart_puts(" Hz\n");
    } else if (!strncmp(cmd, "mode ", 5)) {
        radio_set_mode(parse_uint(cmd + 5) % 8);
    } else if (!strcmp(cmd, "sig")) {
        float s = 0;
        for (int i = 0; i < 20; i++) {
            s += audio_signal_level();
            delay_ms(25);
        }
        print_db((s / 20) * (s / 20));
        uart_puts("FS (passband, before AGC)\n");
    } else if (!strncmp(cmd, "tx ", 3)) {
        const char *a = cmd + 3;
        const char *pw = skip_word(a);
        int pidx = *pw ? (int)parse_uint(pw) : -1;
        if (!strncmp(a, "off", 3)) {
            radio_tx_stop();
            uart_puts("tx off\n");
        } else {
            int sig = !strncmp(a, "carrier", 7) ? AUDIO_TX_CARRIER : !strncmp(a, "usb", 3) ? AUDIO_TX_USB
                    : !strncmp(a, "lsb", 3) ? AUDIO_TX_LSB : -1;
            if (sig < 0) {
                uart_puts("tx carrier|usb|lsb [0-2] | tx off\n");
            } else {
                radio_tx_start(sig, pidx);
                uart_puts("TX ON (30 s timeout, any key or 'tx off' stops)\n");
            }
        }
    } else if (!strcmp(cmd, "saveown")) {
        int rc = settings_save_own();
        uart_puts(rc == 0 ? "ok\n" : rc == -1 ? "write failed\n" : "write cycle timeout\n");
    } else if (!strcmp(cmd, "report")) {
        report();
    } else if (!strcmp(cmd, "iqfft")) {
        iq_fft();
    } else if (!strncmp(cmd, "agc ", 4)) {
        audio_set_agc_max(parse_uint(cmd + 4));
    } else if (cmd[0]) {
        uart_puts("unknown command, try help\n");
    }
}

static void console_poll(void)
{
    static char line[64];
    static unsigned len;
    int c;

    while ((c = uart_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            line[len] = 0;
            run_command(line);
            len = 0;
            uart_puts("> ");
        } else if ((c == 8 || c == 127) && len) {
            len--;
            uart_puts("\b \b");
        } else if (c >= ' ' && len < sizeof(line) - 1) {
            line[len++] = c;
            uart_putc(c);
        }
    }
}

int main(void)
{
    board_init();
    uart_init(115200);
    uart_puts(banner);
    lcd_init();
    input_init();
    beep_init();
    i2c_init(I2C_BUS1);
    i2c_init(I2C_BUS2);
    board_rf_rx_state();
    uart_puts(vc_init() < 0 ? "versaclock: FAIL\n" : "versaclock: ok\n");
    audio_start();                      /* before the codec starts its clocks, as stock */
    uart_puts(codec_init() < 0 ? "codec: FAIL\n" : "codec: ok\n");
    float pk;
    uint32_t b0, b1;
    audio_stats(&pk, &pk, &b0);
    for (int t = 0; t < 50; t++) {      /* the codec needs a while before BCLK/WCLK run */
        delay_ms(10);
        audio_stats(&pk, &pk, &b1);
        if (b1 != b0)
            break;
    }
    uart_puts(b1 != b0 ? "i2s: running\n" : "i2s: no clock from codec\n");
    tick_hook = ticks_1ms;
    radio_init();
    uart_puts("> ");

    /* Power key as stock (TIM2 tick 0x08030414 + 0x08029d6c): the key is usually still held from
     * power-on, so it is ignored until released; then a press shorter than 2 s toggles the
     * backlight on release, holding it 2 s powers off while still held. Own: 20 ms debounce. */
    bool armed = false, down = false, last_raw = false;
    uint32_t pressed_at = 0, edge_at = 0;

    for (;;) {
        uint32_t now = millis();


        radio_poll();

        bool raw = power_key_pressed();
        if (raw != last_raw) {
            last_raw = raw;
            edge_at = now;
        }
        if (now - edge_at >= 20) {
            if (raw != down) {
                down = raw;
                if (down)
                    pressed_at = now;
                else if (armed)
                    radio_toggle_backlight();
            }
            if (!down)
                armed = true;
        }
        if (armed && down && now - pressed_at >= POWER_OFF_HOLD_MS) {
            uart_puts("\npower key: off\n");
            radio_power_off();
        }

        console_poll();
    }
}
