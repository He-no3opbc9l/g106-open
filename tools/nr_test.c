/* Host test of the receive noise reduction (src/nr.c): `make test-nr` (SEED=n varies the noise).
 * Synthetic 16 kHz audio as it reaches the NR in audio.c (after the 20 Hz .. 2.7 kHz band-pass):
 * speech-like vowels (glottal pulses through formant resonators, syllables, pauses, fricatives),
 * a keyed 700 Hz CW tone (behind the CW biquads), plus white noise through the same band-pass.
 * Measurements:
 *  - SNR in / out by shadow filtering: the gains computed on the noisy input are applied by two
 *    more NR instances to the clean signal and to the noise alone (the NR is linear for given
 *    gains), so the output signal and output noise are known separately;
 *  - speech distortion (clean through those gains vs. the clean input, delayed), clean input alone;
 *  - musical noise: kurtosis of the STFT power of the residual noise vs. the input noise
 *    (Uemura et al.: kurtosis ratio ~1 = no isolated tones);
 *  - noise estimate bias, tracking of noise steps, silence, extreme levels, a long random run
 *    (finite output, bounded), perfect reconstruction with all gains 1. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nr.h"

#define FS   16000
#define BPF  191

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

static float h_bp[BPF];
static void design_bp(float lo, float hi)
{
    double fl = lo / FS, fh = hi / FS;
    for (int i = 0; i < BPF; i++) {
        double k = i - (BPF - 1) / 2.0;
        double s = k != 0 ? (sin(2 * M_PI * fh * k) - sin(2 * M_PI * fl * k)) / (M_PI * k) : 2 * (fh - fl);
        h_bp[i] = (float)(s * (0.42 - 0.5 * cos(2 * M_PI * i / (BPF - 1)) + 0.08 * cos(4 * M_PI * i / (BPF - 1))));
    }
}
static void filter(float *x, long n)
{
    float *t = malloc(n * sizeof *t);
    for (long i = 0; i < n; i++) {
        double a = 0;
        for (int j = 0; j < BPF && j <= i; j++)
            a += h_bp[j] * x[i - j];
        t[i] = (float)a;
    }
    memcpy(x, t, n * sizeof *t);
    free(t);
}

static double power(const float *x, long n)
{
    double a = 0;
    for (long i = 0; i < n; i++)
        a += (double)x[i] * x[i];
    return a / n;
}
static double db(double r) { return 10 * log10(r > 1e-30 ? r : 1e-30); }

/* ---- speech-like signal: vowels with F0 contour, formants, syllable envelopes, pauses ---- */
typedef struct { double a1, a2, y1, y2, g; } res_t;
static res_t res_design(double f, double bw)
{
    double r = exp(-M_PI * bw / FS), w = 2 * M_PI * f / FS;
    return (res_t){-2 * r * cos(w), r * r, 0, 0, 1 - r};
}
static double res_run(res_t *r, double x)
{
    double y = r->g * x - r->a1 * r->y1 - r->a2 * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* active[i] = 1 where speech is present (for noise-in-pause measurements) */
static void gen_speech(float *s, unsigned char *active, long n)
{
    static const double vow[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240},
                                     {530, 1840, 2480}, {570, 840, 2410}};
    long i = 0;
    memset(s, 0, n * sizeof *s);
    memset(active, 0, n);
    i += FS / 2;
    while (i < n) {
        int syl = 2 + (int)(urand() * 3);
        for (int k = 0; k < syl && i < n; k++) {
            const double *v = vow[(int)(urand() * 5)];
            res_t r[3] = {res_design(v[0], 90), res_design(v[1], 110), res_design(v[2], 150)};
            long len = (long)((0.12 + 0.13 * urand()) * FS);
            double f0 = 100 + 60 * urand(), f1 = f0 * (0.85 + 0.3 * urand()), ph = 0;
            double amp = 0.5 + 0.5 * urand();
            bool fric = urand() < 0.3;
            for (long j = 0; j < len && i < n; j++, i++) {
                double t = (double)j / len;
                double env = t < 0.12 ? 0.5 - 0.5 * cos(M_PI * t / 0.12)
                           : t > 0.85 ? 0.5 + 0.5 * cos(M_PI * (t - 0.85) / 0.15) : 1;
                double f = f0 + (f1 - f0) * t;
                ph += f / FS;
                double src = 0;
                if (ph >= 1) {
                    ph -= 1;
                    src = 1;
                }
                double y = res_run(&r[2], res_run(&r[1], res_run(&r[0], src * 40)));
                if (fric && t < 0.3)            /* fricative onset: noise burst */
                    y += 0.15 * gauss() * (1 - t / 0.3);
                s[i] = (float)(amp * env * y);
                active[i] = 1;
            }
        }
        i += (long)((0.2 + 0.4 * urand()) * FS);
    }
    filter(s, n);
    /* speech power over the active parts = 1 */
    double a = 0;
    long na = 0;
    for (long j = 0; j < n; j++)
        if (active[j]) {
            a += (double)s[j] * s[j];
            na++;
        }
    float g = (float)(1 / sqrt(a / na));
    for (long j = 0; j < n; j++)
        s[j] *= g;
}

/* ---- keyed CW tone, 20 WPM "PARIS ", 5 ms raised-cosine edges, behind two band-pass biquads ---- */
typedef struct { float b0, b2, a1, a2, x1, x2, y1, y2; } bq_t;
static float bq_run(bq_t *f, float x)
{
    float y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y;
    return y;
}
static void cw_filter(float *x, long n, float tone, float bw)
{
    float w0 = 2 * (float)M_PI * tone / FS, q = tone / (bw * 1.55f);
    float alpha = sinf(w0) / (2 * q), a0 = 1 + alpha;
    bq_t b[2];
    for (int k = 0; k < 2; k++)
        b[k] = (bq_t){alpha / a0, -alpha / a0, -2 * cosf(w0) / a0, (1 - alpha) / a0, 0, 0, 0, 0};
    for (long i = 0; i < n; i++)
        x[i] = bq_run(&b[1], bq_run(&b[0], x[i]));
}
static void gen_cw(float *s, unsigned char *active, long n)
{
    static const char *pat = ".--. .- .-. .. ... / ";    /* PARIS */
    long dot = FS * 60 / 1000, i = FS / 2;
    double env = 0, ph = 0;
    memset(s, 0, n * sizeof *s);
    memset(active, 0, n);
    const char *p = pat;
    while (i < n) {
        int on = 0, units = 1;
        if (*p == '.') { on = 1; units = 1; }
        else if (*p == '-') { on = 1; units = 3; }
        else if (*p == ' ') { units = 2; }      /* + element gap = 3 */
        else if (*p == '/') { units = 4; }
        for (long j = 0; j < units * dot && i < n; j++, i++) {
            env += on ? 1.0 / (0.005 * FS) : -1.0 / (0.005 * FS);
            env = env < 0 ? 0 : env > 1 ? 1 : env;
            s[i] = (float)((0.5 - 0.5 * cos(M_PI * env)) * sin(ph));
            ph += 2 * M_PI * 700 / FS;
            active[i] = on;
        }
        for (long j = 0; j < (on ? dot : 0) && i < n; j++, i++) {     /* element gap */
            env -= 1.0 / (0.005 * FS);
            env = env < 0 ? 0 : env;
            s[i] = (float)((0.5 - 0.5 * cos(M_PI * env)) * sin(ph));
            ph += 2 * M_PI * 700 / FS;
        }
        if (!*++p)
            p = pat;
    }
    cw_filter(s, n, 700, 500);
    double a = 0;
    long na = 0;
    for (long j = 0; j < n; j++)
        if (active[j]) {
            a += (double)s[j] * s[j];
            na++;
        }
    float g = (float)(1 / sqrt(a / na));
    for (long j = 0; j < n; j++)
        s[j] *= g;
}

static void gen_noise(float *x, long n, double sigma, int cw)
{
    for (long i = 0; i < n; i++)
        x[i] = (float)(sigma * gauss());
    if (cw)
        cw_filter(x, n, 700, 500);
    else
        filter(x, n);
}

/* ---- NR runs ---- */
static nr_t A, B, C;

/* Shadow filtering: A on s + n, B / C apply A's gains to s / n. Outputs aligned (delay removed). */
static void run3(const float *s, const float *n, float *ys, float *yn, float *y, long len, int level)
{
    nr_set_level(&A, level);
    nr_reset(&A);
    nr_reset(&B);
    nr_reset(&C);
    B.ext_gain = C.ext_gain = A.gain;
    for (long i = 0; i < len + NR_DELAY; i++) {
        float xs = i < len ? s[i] : 0, xn = i < len ? n[i] : 0;
        float a = nr_process(&A, xs + xn), b = nr_process(&B, xs), c = nr_process(&C, xn);
        if (i >= NR_DELAY) {
            y[i - NR_DELAY] = a;
            ys[i - NR_DELAY] = b;
            yn[i - NR_DELAY] = c;
        }
    }
}

static double kurt(const float *x, long n, long skip)
{
    static float w[NR_N];
    for (int i = 0; i < NR_N; i++)
        w[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / NR_N));
    int k0 = 300 * NR_N / FS, k1 = 2500 * NR_N / FS;
    double sum = 0;
    for (int k = k0; k <= k1; k++) {
        double m1 = 0, m2 = 0;
        long cnt = 0;
        for (long f = skip; f + NR_N <= n; f += NR_HOP) {
            double re = 0, im = 0;
            for (int i = 0; i < NR_N; i++) {
                re += w[i] * x[f + i] * cos(2 * M_PI * k * i / NR_N);
                im -= w[i] * x[f + i] * sin(2 * M_PI * k * i / NR_N);
            }
            double p = re * re + im * im;
            m1 += p;
            m2 += p * p;
            cnt++;
        }
        m1 /= cnt;
        m2 /= cnt;
        sum += m2 / (m1 * m1);
    }
    return sum / (k1 - k0 + 1);
}

static int fails;
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        rng += strtoull(argv[1], 0, 10) * 0x9E3779B97F4A7C15ull;
    nr_init();
    design_bp(20, 2700);
    const long L = 30L * FS;
    float *s = malloc(L * sizeof *s), *n = malloc(L * sizeof *n), *y = malloc(L * sizeof *y);
    float *ys = malloc(L * sizeof *ys), *yn = malloc(L * sizeof *yn), *t = malloc(L * sizeof *t);
    unsigned char *act = malloc(L);

    /* 1. perfect reconstruction with all gains 1 */
    {
        static float ones[NR_BINS];
        for (int k = 0; k < NR_BINS; k++)
            ones[k] = 1;
        nr_reset(&B);
        B.ext_gain = ones;
        gen_noise(n, FS * 2, 0.3, 0);
        double e = 0, p = 0;
        for (long i = 0; i < FS * 2; i++) {
            float o = nr_process(&B, n[i]);
            if (i >= NR_DELAY) {
                double d = o - n[i - NR_DELAY];
                e += d * d;
                p += (double)n[i - NR_DELAY] * n[i - NR_DELAY];
            }
        }
        printf("reconstruction (all gains 1): error %.1f dB\n", db(e / p));
        check(db(e / p) < -100, "perfect reconstruction");
        B.ext_gain = 0;
    }

    /* 2. noise estimate bias: white noise sigma 0.01 -> passband bin power 128 sigma^2 */
    {
        const long n2 = 10L * FS;
        for (long i = 0; i < n2; i++)
            n[i] = (float)(0.01 * gauss());
        filter(n, n2);
        nr_set_level(&A, 5);
        nr_reset(&A);
        double acc = 0;
        long cnt = 0;
        for (long i = 0; i < n2; i++) {
            nr_process(&A, n[i]);
            if (i > 2 * FS && A.pos == 0) {
                for (int k = 10; k <= 38; k++)
                    acc += A.noise[k];
                cnt += 29;
            }
        }
        double bias = db(acc / cnt / (128 * 1e-4));
        printf("noise estimate bias (white noise, 600..2400 Hz): %+.2f dB\n", bias);
        check(fabs(bias) < 2.0, "noise estimate bias within 2 dB");
    }

    /* 3. speech in noise, per level and SNR */
    gen_speech(s, act, L);
    double ps = power(s, L);
    printf("\nSSB speech (30 s, %.0f %% active), white noise 20 Hz..2.7 kHz\n", 100 * ps);
    printf("level | SNR in | SNR out | gain   | noise in pauses | speech att. | speech SDR\n");
    static const int levels[] = {1, 3, 5, 7, 10};
    static const double snrs[] = {0, 6, 12, 20};
    for (unsigned li = 0; li < sizeof levels / sizeof levels[0]; li++)
        for (unsigned si = 0; si < sizeof snrs / sizeof snrs[0]; si++) {
            double sig = sqrt(ps / pow(10, snrs[si] / 10) / (2 * 2680.0 / FS));
            gen_noise(n, L, sig, 0);
            double snr_in = db(ps / power(n, L));
            run3(s, n, ys, yn, y, L, levels[li]);
            double snr_out = db(power(ys, L) / power(yn, L));
            double pin = 0, pout = 0, e = 0;
            for (long i = 2 * FS; i < L; i++) {
                if (!act[i]) {
                    pin += (double)n[i] * n[i];
                    pout += (double)yn[i] * yn[i];
                }
                double d = ys[i] - s[i];
                e += d * d;
            }
            double att = db(power(ys + 2 * FS, L - 2 * FS) / power(s + 2 * FS, L - 2 * FS));
            double sdr = db(power(s + 2 * FS, L - 2 * FS) * (L - 2 * FS) / e);
            for (long i = 0; i < L; i++)
                check(isfinite(y[i]), "finite output");
            printf("%5d | %5.1f  | %6.1f  | %+5.1f  | %+6.1f dB       | %+6.1f dB   | %5.1f dB\n",
                   levels[li], snr_in, snr_out, snr_out - snr_in, db(pout / pin), att, sdr);
            if (levels[li] == 5 && snrs[si] == 6) {
                check(snr_out - snr_in > 6, "level 5, 6 dB SNR: > 6 dB improvement");
                check(att > -2.0, "level 5, 6 dB SNR: speech attenuation < 2 dB");
            }
        }

    /* 4. clean speech (no noise at all, then 40 dB SNR) */
    printf("\nclean speech: ");
    for (unsigned li = 0; li < sizeof levels / sizeof levels[0]; li++) {
        memset(n, 0, L * sizeof *n);
        run3(s, n, ys, yn, y, L, levels[li]);
        double e = 0;
        for (long i = 2 * FS; i < L; i++) {
            double d = y[i] - s[i];
            e += d * d;
        }
        double sdr = db(power(s + 2 * FS, L - 2 * FS) * (L - 2 * FS) / e);
        double att = db(power(y + 2 * FS, L - 2 * FS) / power(s + 2 * FS, L - 2 * FS));
        printf("L%d SDR %.1f dB (level %+.2f dB)  ", levels[li], sdr, att);
        if (levels[li] == 5)
            check(att > -1.0, "clean speech level 5: attenuation < 1 dB");
    }
    printf("\n");

    /* 5. musical noise: kurtosis of the residual noise (noise only) */
    printf("\nnoise only (kurtosis of STFT power 300..2500 Hz; Gaussian noise = 2.0):\n");
    gen_noise(n, L, 0.01, 0);
    memset(s, 0, L * sizeof *s);
    double kin = kurt(n, L, 3 * FS);
    for (unsigned li = 0; li < sizeof levels / sizeof levels[0]; li++) {
        run3(s, n, ys, yn, y, L, levels[li]);
        double kout = kurt(y, L, 3 * FS);
        double red = db(power(y + 3 * FS, L - 3 * FS) / power(n + 3 * FS, L - 3 * FS));
        printf("  level %2d: noise %+5.1f dB, kurtosis in %.2f out %.2f, ratio %.2f\n",
               levels[li], red, kin, kout, kout / kin);
        check(kout / kin < 1.5, "musical noise: kurtosis ratio < 1.5");
    }

    /* 6. CW 20 WPM behind the 500 Hz CW filter */
    gen_cw(s, act, L);
    ps = power(s, L);
    printf("\nCW 700 Hz 20 WPM, CW filter 500 Hz (SNR in the 500 Hz filter):\n");
    for (unsigned si = 0; si < 3; si++) {
        static const double cs[3] = {-6, 0, 10};
        gen_noise(n, L, 1, 1);
        float g = (float)sqrt(ps / pow(10, cs[si] / 10) / power(n, L));
        for (long i = 0; i < L; i++)
            n[i] *= g;
        run3(s, n, ys, yn, y, L, 5);
        double snr_in = db(ps / power(n, L)), snr_out = db(power(ys, L) / power(yn, L));
        double mk = 0, mk0 = 0, sp = 0, sp0 = 0;
        for (long i = 2 * FS; i < L; i++)
            if (act[i]) {
                mk += (double)ys[i] * ys[i];
                mk0 += (double)s[i] * s[i];
            } else {
                sp += (double)yn[i] * yn[i];
                sp0 += (double)n[i] * n[i];
            }
        printf("  SNR in %+5.1f dB -> out %+5.1f dB (%+5.1f), marks %+5.1f dB, noise in spaces %+5.1f dB\n",
               snr_in, snr_out, snr_out - snr_in, db(mk / mk0), db(sp / sp0));
    }

    /* 7. noise steps +-20 dB: time until the noise estimate is within 3 dB of the new level */
    printf("\nnoise steps (level 5): ");
    for (int dir = 0; dir < 2; dir++) {
        const long n7 = 12L * FS, st = 6L * FS;
        for (long i = 0; i < n7; i++)
            n[i] = (float)(gauss() * (((i < st) ^ dir) ? 0.001 : 0.01));
        filter(n, n7);
        nr_set_level(&A, 5);
        nr_reset(&A);
        double target = 128 * (dir ? 1e-6 : 1e-4);
        long settled = -1;
        for (long i = 0; i < n7; i++) {
            nr_process(&A, n[i]);
            if (i > st && A.pos == 0) {
                double acc = 0;
                for (int k = 10; k <= 38; k++)
                    acc += A.noise[k];
                double e = db(acc / 29 / target);
                if (fabs(e) < 3) {
                    if (settled < 0)
                        settled = i;
                } else {
                    settled = -1;
                }
            }
        }
        double ms = settled < 0 ? -1 : (settled - st) * 1000.0 / FS;
        printf("%s20 dB: %.0f ms  ", dir ? "-" : "+", ms);
        check(settled > 0 && ms < (dir ? 500 : 3000), "noise step tracking");
    }
    printf("\n");

    /* 8. silence, extreme levels, long random run */
    {
        nr_reset(&A);
        int ok = 1;
        for (long i = 0; i < 5L * FS; i++)
            ok &= nr_process(&A, 0) == 0;
        check(ok, "silence in -> exact silence out");
        double maxin = 0, maxout = 0;
        int fin = 1;
        nr_reset(&A);
        float lvl = 1e-3f;
        int speech_on = 0;
        long spos = 0;
        gen_speech(s, act, L);
        for (long i = 0; i < 600L * FS; i++) {      /* 10 minutes */
            if (i % (FS / 2) == 0) {
                lvl = (float)pow(10, -8 + 8 * urand());       /* 1e-8 .. 1 */
                speech_on = urand() < 0.5;
            }
            float x = (float)(lvl * gauss() * 0.1);
            if (speech_on)
                x += lvl * s[spos];
            spos = (spos + 1) % L;
            if (urand() < 2e-5)                       /* static crash */
                x += (float)(lvl * 50 * (urand() - 0.5));
            if (i % (37 * FS) < FS / 4)               /* dead silence now and then */
                x = 0;
            float o = nr_process(&A, x);
            fin &= isfinite(o);
            if (fabs(x) > maxin) maxin = fabs(x);
            if (fabs(o) > maxout) maxout = fabs(o);
        }
        printf("10 min random levels 1e-8..1, crashes, silences: %s, max |in| %.2f, max |out| %.2f, faults %u\n",
               fin ? "finite" : "NOT FINITE", maxin, maxout, (unsigned)A.faults);
        check(fin && A.faults == 0, "long run finite");
        check(maxout < 2 * maxin, "long run bounded");
    }

    free(s); free(n); free(y); free(ys); free(yn); free(t); free(act);
    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
    return fails != 0;
}
