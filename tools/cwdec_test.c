/* Host test of the CW decoder (src/cwdec.c): `make test-cwdec`.
 * Simulates the receive tap of audio.c at 16 kHz: a 700 Hz tone keyed from text (5 ms raised-
 * cosine edges, optional timing jitter, dash ratio, Farnsworth spacing) plus white noise, through
 * the decoder's two band-pass biquads (same design as audio_set_cw_filter), mean power per 2 ms
 * frame -> cwdec_feed(). Compares the decoded text with the sent one (character error rate). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cwdec.h"

#define FS     16000
#define TONE   700.0f
#define DET_BW 200.0f
#define FRAME  (FS * CWDEC_FRAME_MS / 1000)

static uint64_t rng = 88172645463325252ull;
static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (rng >> 11) * (1.0 / 9007199254740992.0);
}
static double gauss(void)
{
    double u = urand() + 1e-12, v = urand();
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

typedef struct { float b0, b2, a1, a2, x1, x2, y1, y2; } bq_t;
static bq_t bq[2];
static float bq_run(bq_t *f, float x)
{
    float y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y;
    return y;
}
static void bq_design(float tone, float bw)
{
    float w0 = 2 * (float)M_PI * tone / FS, q = tone / (bw * 1.55f);
    float alpha = sinf(w0) / (2 * q), a0 = 1 + alpha;
    for (int i = 0; i < 2; i++)
        bq[i] = (bq_t){alpha / a0, -alpha / a0, -2 * cosf(w0) / a0, (1 - alpha) / a0, 0, 0, 0, 0};
}

/* signal generator state */
static double ph, amp, sigma, qsb_db, qsb_ph;    /* QSB: sine fading depth in dB, 0.25 Hz */
static float acc;
static int nacc;
static void sample(double env)
{
    double a = amp;
    if (qsb_db > 0) {
        a *= pow(10, -qsb_db / 20 * (0.5 - 0.5 * cos(qsb_ph)));
        qsb_ph += 2 * M_PI * 0.25 / FS;
    }
    double x = a * env * sin(ph) + sigma * gauss();
    ph += 2 * M_PI * TONE / FS;
    if (ph > 2 * M_PI)
        ph -= 2 * M_PI;
    float y = bq_run(&bq[1], bq_run(&bq[0], (float)x));
    acc += y * y;
    if (++nacc == FRAME) {
        cwdec_feed(acc / FRAME);
        acc = 0;
        nacc = 0;
    }
}

static int keyed;           /* current key state for the edge ramps */
static double env;
static void segment(int on, double ms)
{
    long n = lround(ms * FS / 1000);
    double step = 1.0 / (0.005 * FS);       /* 5 ms ramp */
    for (long i = 0; i < n; i++) {
        env += on ? step : -step;
        env = env < 0 ? 0 : env > 1 ? 1 : env;
        double e = 0.5 - 0.5 * cos(M_PI * env);
        sample(e);
    }
    keyed = on;
}

static const char *pattern(char c)
{
    static const char *const az[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..",
        ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-",
        ".--", "-..-", "-.--", "--.."};
    static const char *const d[10] = {"-----", ".----", "..---", "...--", "....-", ".....",
        "-....", "--...", "---..", "----."};
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return d[c - '0'];
    switch (c) {
    case '?': return "..--..";
    case '/': return "-..-.";
    case '=': return "-...-";
    case '.': return ".-.-.-";
    case ',': return "--..--";
    }
    return NULL;
}

typedef struct {
    double wpm, fwpm;       /* character speed, Farnsworth (effective) speed, 0 = none */
    double ratio;           /* dash / dot */
    double jitter;          /* relative random timing error per element */
    double snr_db;          /* in 2.5 kHz, 99 = no noise */
} keying_t;

static double jit(const keying_t *k, double ms)
{
    return ms * (1 + k->jitter * (2 * urand() - 1));
}

static void send(const char *s, const keying_t *k)
{
    double u = 1200.0 / k->wpm;
    double cg = 3 * u, wg = 7 * u;
    if (k->fwpm > 0 && k->fwpm < k->wpm) {
        /* ARRL Farnsworth: total delay distributed as 3 / 7 of a word gap unit */
        double ta = (60 * k->wpm - 37.2 * k->fwpm) / (k->wpm * k->fwpm) * 1000;
        cg = 3 * ta / 19;
        wg = 7 * ta / 19;
    }
    for (const char *p = s; *p; p++) {
        if (*p == ' ') {
            segment(0, jit(k, wg - cg));
            continue;
        }
        const char *m = pattern(*p);
        for (const char *e = m; *e; e++) {
            segment(1, jit(k, *e == '.' ? u : k->ratio * u));
            segment(0, jit(k, e[1] ? u : cg));
        }
    }
}

static void setup(const keying_t *k)
{
    amp = 0.1;
    double ps = amp * amp / 2;
    sigma = k->snr_db >= 99 ? 0 : sqrt(ps / pow(10, k->snr_db / 10) / (2500.0 / (FS / 2)));
    bq_design(TONE, DET_BW);
    cwdec_reset();
    cwdec_clear();
}

/* Levenshtein distance on strings with collapsed spaces, trimmed. */
static void norm(const char *in, char *out)
{
    int n = 0;
    for (; *in; in++) {
        if (*in == ' ' && (n == 0 || out[n - 1] == ' '))
            continue;
        out[n++] = *in;
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}
static int lev(const char *a, const char *b)
{
    int la = strlen(a), lb = strlen(b);
    int *d = malloc((lb + 1) * sizeof *d);
    for (int j = 0; j <= lb; j++) d[j] = j;
    for (int i = 1; i <= la; i++) {
        int prev = d[0];
        d[0] = i;
        for (int j = 1; j <= lb; j++) {
            int t = d[j];
            int c = prev + (a[i - 1] != b[j - 1]);
            if (d[j] + 1 < c) c = d[j] + 1;
            if (d[j - 1] + 1 < c) c = d[j - 1] + 1;
            d[j] = c;
            prev = t;
        }
    }
    int r = d[lb];
    free(d);
    return r;
}

static int failures;

/* Sends text (in pieces small enough for the 64-char ring), returns the CER in %. */
static double run(const char *name, const char *msg, const keying_t *k, double max_cer)
{
    setup(k);
    char got[4096] = "", buf[CWDEC_TEXT_N + 1];
    uint32_t seen = cwdec_seq();
    segment(0, 500);
    /* feed word by word, collect new text after each word */
    const char *p = msg;
    while (*p) {
        const char *e = strchr(p, ' ');
        int n = e ? (int)(e - p + 1) : (int)strlen(p);
        char w[64];
        memcpy(w, p, n);
        w[n] = 0;
        send(w, k);
        p += n;
        uint32_t s = cwdec_seq();
        int fresh = (int)(s - seen);
        seen = s;
        if (fresh > CWDEC_TEXT_N) fresh = CWDEC_TEXT_N;
        cwdec_text(buf, fresh);
        strcat(got, buf);
    }
    segment(0, 3000);
    uint32_t s = cwdec_seq();
    int fresh = (int)(s - seen);
    if (fresh > CWDEC_TEXT_N) fresh = CWDEC_TEXT_N;
    cwdec_text(buf, fresh);
    strcat(got, buf);

    char a[4096], b[4096];
    norm(msg, a);
    norm(got, b);
    double cer = 100.0 * lev(a, b) / strlen(a);
    bool ok = cer <= max_cer;
    if (!ok)
        failures++;
    printf("%-4s %-34s %5.1f%% (max %4.1f) wpm=%2d\n", ok ? "ok" : "FAIL", name, cer, max_cer, cwdec_wpm());
    if (!ok || getenv("VERBOSE"))
        printf("     sent: %s\n     got:  %s\n", a, b);
    return cer;
}

int main(void)
{
    const char *msg = "CQ CQ DE R1ABC R1ABC K UA3XYZ DE R1ABC GM UR RST 599 599 NAME OLEG QTH MOSCOW "
                      "HW? BK TNX FER QSO 73 ES GL = PSE QSL VIA BURO 5NN TU";
    char name[64];
    if (getenv("SEED"))         /* other noise / jitter realisations: SEED=n make test-cwdec */
        rng ^= strtoull(getenv("SEED"), NULL, 0) * 0x9E3779B97F4A7C15ull;

    cwdec_set_level(0);
    static const int speeds[] = {5, 8, 12, 16, 20, 25, 30, 35, 40, 45, 50};
    for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        keying_t k = {speeds[i], 0, 3.0, 0, 99};
        snprintf(name, sizeof name, "clean %d WPM", speeds[i]);
        run(name, msg, &k, 2.0);    /* the first element may come before the speed is learnt */
    }
    for (int r = 0; r < 3; r++) {
        static const double ratio[] = {2.5, 3.5, 4.0};
        keying_t k = {20, 0, ratio[r], 0, 99};
        snprintf(name, sizeof name, "ratio %.1f 20 WPM", ratio[r]);
        run(name, msg, &k, 2.0);
    }
    {
        keying_t k = {18, 0, 3.0, 0.15, 99};
        run("hand keying +-15% 18 WPM", msg, &k, 3.0);
        k.jitter = 0.25;
        run("hand keying +-25% 18 WPM", msg, &k, 10.0);
    }
    {
        keying_t k = {20, 10, 3.0, 0, 99};
        run("Farnsworth 20/10", msg, &k, 5.0);
        k.fwpm = 6;
        run("Farnsworth 20/6", msg, &k, 10.0);
    }
    static const double snrs[] = {20, 10, 6, 3, 0};
    static const double snr_max[] = {2, 3, 8, 30, 100};
    for (int i = 0; i < 5; i++) {
        keying_t k = {20, 0, 3.0, 0, snrs[i]};
        snprintf(name, sizeof name, "AUTO SNR %+.0f dB/2.5k 20 WPM", snrs[i]);
        run(name, msg, &k, snr_max[i]);
    }
    /* manual level: too low a level decodes the noise (report only), a fitting one works */
    static const int lvls[] = {4, 6, 8, 12};
    static const double lvl_snr[] = {3, 3, 3, 10};
    static const double lvl_max[] = {1000, 1000, 5, 3};
    for (int i = 0; i < 4; i++) {
        cwdec_set_level(lvls[i]);
        keying_t k = {20, 0, 3.0, 0, lvl_snr[i]};
        snprintf(name, sizeof name, "LEVEL %d dB SNR %+.0f dB 20 WPM", lvls[i], lvl_snr[i]);
        run(name, msg, &k, lvl_max[i]);
    }
    cwdec_set_level(0);
    {
        keying_t k = {30, 0, 3.0, 0, 6};
        run("AUTO SNR +6 dB/2.5k 30 WPM", msg, &k, 5.0);
        k.wpm = 20;
        k.snr_db = 20;
        qsb_db = 15;
        run("QSB 15 dB 0.25 Hz SNR +20 dB 20 WPM", msg, &k, 3.0);
        qsb_db = 0;
    }
    {
        /* speed change: same session, 30 WPM then 14 WPM */
        keying_t k = {30, 0, 3.0, 0, 30};
        setup(&k);
        segment(0, 300);
        send("CQ TEST R1ABC ", &k);
        k.wpm = 14;
        send("R1ABC DE UA3XYZ 5NN ", &k);
        segment(0, 3000);
        char buf[CWDEC_TEXT_N + 1], a[128], b[128];
        cwdec_text(buf, CWDEC_TEXT_N);
        norm("CQ TEST R1ABC R1ABC DE UA3XYZ 5NN", a);
        norm(buf, b);
        double cer = 100.0 * lev(a, b) / strlen(a);
        bool ok = cer <= 8;
        failures += !ok;
        printf("%-4s %-34s %5.1f%% (max  8.0) wpm=%2d\n", ok ? "ok" : "FAIL", "speed change 30 -> 14 WPM", cer, cwdec_wpm());
        if (!ok || getenv("VERBOSE"))
            printf("     sent: %s\n     got:  %s\n", a, b);
    }
    {
        /* noise only (AUTO): nothing may be decoded */
        keying_t k = {20, 0, 3.0, 0, 0};
        setup(&k);
        amp = 0;
        segment(0, 30000);
        char buf[CWDEC_TEXT_N + 1];
        int n = cwdec_text(buf, CWDEC_TEXT_N);
        bool ok = n == 0;
        failures += !ok;
        printf("%-4s %-34s %d chars \"%s\"\n", ok ? "ok" : "FAIL", "noise only 30 s (AUTO)", n, buf);
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
