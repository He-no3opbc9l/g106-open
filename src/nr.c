/* Own receive noise reduction, see nr.h.
 *
 * Short-time spectral noise reduction (Wiener filter) at 16 kHz:
 *  - frames of 256 samples (16 ms, 62.5 Hz bins), hop 128, sqrt-Hann analysis and synthesis
 *    windows: with all gains 1 the output is the input delayed by NR_DELAY samples (exactly, up
 *    to rounding). Real FFT = 128-point complex radix-2 FFT + split.
 *  - Noise power per bin: MMSE estimator driven by a speech presence probability (Gerkmann &
 *    Hendriks 2012). No voice activity detector and no minimum search: falling noise is followed
 *    within ~100 ms, rising noise within ~1 s (+10 dB) .. 2 s (+20 dB); a steady tone held for
 *    several seconds slowly becomes "noise" (stagnation guard).
 *  - Gain: Wiener gain xi / (1 + xi) on the decision-directed a priori SNR xi (Ephraim & Malah),
 *    the standard remedy against "musical noise" (isolated tones that pop up in the residual
 *    noise of a plain spectral subtraction), limited to a floor: the residual noise stays as
 *    attenuated natural noise instead of gurgling. The NR level sets the floor (-6 .. -24 dB)
 *    and a slight noise over-estimation.
 * Cost on the Cortex-M4 (estimated from the instruction mix of this code run under qemu, see
 * tools/nr_test.c): ~95k cycles per frame = ~750 cycles per 16 kHz sample = ~7 % of the CPU.
 * The frame is worked off in 10 slots during the following hop (one more hop of delay, 24 ms in
 * total), at most ~11k cycles in one DSP interrupt (~10 % of its period) instead of one burst. */
#include <math.h>
#include <string.h>
#include "nr.h"

#if defined(__arm__)
#define NR_CCM __attribute__((section(".ccm")))     /* CPU only, zero wait states */
#else
#define NR_CCM
#endif

#define M        (NR_N / 2)         /* complex FFT size */
#define PI_F     3.14159265f
#define NR_INIT  8                  /* warm-up frames: noise = mean power */
#define XI_H1    31.622777f         /* SPP: a priori SNR of speech, 15 dB */
#define SPP_K    (1.0f + XI_H1)
#define SPP_E    (XI_H1 / (1.0f + XI_H1) * 1.44269504f)    /* x log2(e) for exp2 */
#define A_NOISE  0.9f               /* noise smoothing per 8 ms frame (~80 ms) */
#define A_SPP    0.95f              /* stagnation guard smoothing */
#define SPP_CAP  0.99f
#define A_DD     0.98f              /* decision-directed weight */
#define XI_MIN   0.003f             /* -25 dB */
#define N_FLOOR  1e-20f
#define NR_SLOTS 10                 /* work slots per frame, see slot() */
#define NR_SLOT_GAP 12              /* samples between slots (> 32 / 3 per interrupt) */
_Static_assert((NR_SLOTS - 1) * NR_SLOT_GAP < NR_HOP, "all slots inside one hop");

static float win[NR_N] NR_CCM;      /* sqrt of the periodic Hann window */
static float tw[M][2] NR_CCM;       /* W_N^k = cos - j sin: {cos, sin}, k < M */
static uint8_t rev[M] NR_CCM;       /* 7-bit bit reversal */

void nr_init(void)
{
    for (int i = 0; i < NR_N; i++)
        win[i] = sqrtf(0.5f - 0.5f * cosf(2 * PI_F * i / NR_N));
    for (int k = 0; k < M; k++) {
        tw[k][0] = cosf(2 * PI_F * k / NR_N);
        tw[k][1] = sinf(2 * PI_F * k / NR_N);
    }
    for (int i = 0; i < M; i++) {
        int r = 0;
        for (int b = 1, j = i; b < M; b <<= 1, j >>= 1)
            r = (r << 1) | (j & 1);
        rev[i] = (uint8_t)r;
    }
}

void nr_flush(nr_t *s)
{
    memset(s->in, 0, sizeof s->in);
    memset(s->ola, 0, sizeof s->ola);
    memset(s->out, 0, sizeof s->out);
    s->pos = 0;
    s->slot = NR_SLOTS;                     /* nothing in progress */
}

void nr_reset(nr_t *s)
{
    nr_flush(s);
    memset(s->noise, 0, sizeof s->noise);
    memset(s->s_prev, 0, sizeof s->s_prev);
    memset(s->spp, 0, sizeof s->spp);
    for (int k = 0; k < NR_BINS; k++)
        s->gain[k] = 1.0f;
    s->frames = 0;
    if (s->gmin <= 0 || !(s->gmin <= 1))
        nr_set_level(s, 5);
}

/* Level 1..10: gain floor -6 .. -24 dB (2 dB per step), noise over-estimation 0 .. +2.5 dB. */
void nr_set_level(nr_t *s, int level)
{
    level = level < 1 ? 1 : level > NR_LEVEL_MAX ? NR_LEVEL_MAX : level;
    float db = -4.0f - 2.0f * level;
    s->gmin = powf(10.0f, db / 20.0f);
    s->over = 1.0f + 0.08f * (level - 1);
}

/* 2^x for x <= 0 (exp of the SPP), ~1e-4: no floorf call in the interrupt. */
static inline float exp2_neg(float x)
{
    if (x < -60.0f)
        return 0.0f;
    int i = (int)x;                         /* towards zero: i >= x */
    if ((float)i > x)
        i--;
    float f = x - i;
    float p = 1.0f + f * (0.6951579f + f * (0.2262207f + f * 0.0781009f));
    union { uint32_t u; float f; } v = {(uint32_t)(i + 127) << 23};
    return v.f * p;
}

static void bitrev(float *z)
{
    for (int i = 0; i < M; i++) {
        int j = rev[i];
        if (i < j) {
            float t = z[2 * i]; z[2 * i] = z[2 * j]; z[2 * j] = t;
            t = z[2 * i + 1]; z[2 * i + 1] = z[2 * j + 1]; z[2 * j + 1] = t;
        }
    }
}

/* Radix-2 stages of the forward complex FFT (M points, interleaved re/im, after bitrev()):
 * butterfly spans len = 2^first .. 2^last. Operands are loaded before anything is stored, else
 * the compiler reloads them after every store (u and v may alias as far as it knows). */
static void cfft_stages(float *z, int first, int last)
{
    for (int lg = first; lg <= last; lg++) {
        int len = 1 << lg, h = len / 2, st = NR_N / len;    /* W_M^(k M/len) = W_N^(k N/len) */
        for (int k = 0; k < h; k++) {
            float c = tw[k * st][0], sn = tw[k * st][1];
            for (float *u = &z[2 * k], *end = &z[2 * M]; u < end; u += 2 * len) {
                float *v = u + 2 * h;
                float ur = u[0], ui = u[1], xr = v[0], xi = v[1];
                float vr = xr * c + xi * sn, vi = xi * c - xr * sn;
                u[0] = ur + vr; u[1] = ui + vi;
                v[0] = ur - vr; v[1] = ui - vi;
            }
        }
    }
}

/* Real FFT split after the complex FFT of z[n] = x[2n] + j x[2n+1]. Packed result in place:
 * b[0] = X[0], b[1] = X[M], b[2k], b[2k+1] = X[k]. */
static void rfft_split(float *b)
{
    float z0r = b[0], z0i = b[1];
    b[0] = z0r + z0i;
    b[1] = z0r - z0i;
    for (int k = 1; k <= M / 2; k++) {
        float *a = &b[2 * k], *c = &b[2 * (M - k)];
        float ar = a[0], ai = a[1], cr = c[0], ci = c[1];
        float er = 0.5f * (ar + cr), ei = 0.5f * (ai - ci);         /* E = (Z[k] + Z*[M-k]) / 2 */
        float or_ = 0.5f * (ai + ci), oi = -0.5f * (ar - cr);       /* O = (Z[k] - Z*[M-k]) / 2j */
        float wc = tw[k][0], ws = tw[k][1];
        float tr = or_ * wc + oi * ws, ti = oi * wc - or_ * ws;       /* T = W^k O */
        a[0] = er + tr; a[1] = ei + ti;                              /* X[k] = E + T */
        c[0] = er - tr; c[1] = ti - ei;                              /* X[M-k] = (E - T)* */
    }
}

/* First half of the inverse real FFT: from the packed spectrum to the conjugate of
 * Z[k] = E[k] + j O[k]; the forward complex FFT of that is M x the conjugated z[n]
 * (x[2n] = re / M, x[2n+1] = -im / M, folded into the synthesis). */
static void irfft_split(float *b)
{
    float x0 = b[0], xm = b[1];
    b[0] = 0.5f * (x0 + xm);
    b[1] = -0.5f * (x0 - xm);
    for (int k = 1; k <= M / 2; k++) {
        float *a = &b[2 * k], *c = &b[2 * (M - k)];
        float ar = a[0], ai = a[1], cr = c[0], ci = c[1];
        float er = 0.5f * (ar + cr), ei = 0.5f * (ai - ci);         /* E = (X[k] + X*[M-k]) / 2 */
        float dr = 0.5f * (ar - cr), di = 0.5f * (ai + ci);         /* (X[k] - X*[M-k]) / 2 */
        float wc = tw[k][0], ws = tw[k][1];
        float or_ = dr * wc - di * ws, oi = di * wc + dr * ws;        /* O = that x W^-k */
        a[0] = er - oi; a[1] = -(ei + or_);                          /* Z[k]* */
        c[0] = er + oi; c[1] = ei - or_;                             /* Z[M-k]* = (E* + j O*)* */
    }
}

/* Gains for bins k0..k1-1 (noise estimate, a priori SNR, Wiener gain), applied in place. */
static void gains(nr_t *s, int k0, int k1)
{
    float *b = s->buf;
    const float *eg = s->ext_gain;
    float gmin = s->gmin, inv_over = 1.0f / s->over;
    bool warm = s->frames < NR_INIT;
    float wk = 1.0f / (s->frames + 1);
    for (int k = k0; k < k1; k++) {
        float *x = k == 0 ? &b[0] : k == M ? &b[1] : &b[2 * k];
        bool edge = k == 0 || k == M;
        float p = edge ? x[0] * x[0] : x[0] * x[0] + x[1] * x[1];
        float g;
        if (eg) {
            g = eg[k];
        } else {
            float n = s->noise[k];
            if (warm) {
                n += (p - n) * wk;
                n = n < N_FLOOR ? N_FLOOR : n;
                s->noise[k] = n;
            }
            /* the noise estimate of the previous frame drives the speech presence and this
             * frame's gain (8 ms behind); then it is updated */
            float rn = 1.0f / n, post = p * rn;
            if (!warm) {
                float ph1 = 1.0f / (1.0f + SPP_K * exp2_neg(-post * SPP_E));
                float q = s->spp[k] = A_SPP * s->spp[k] + (1 - A_SPP) * ph1;
                if (q > SPP_CAP && ph1 > SPP_CAP)
                    ph1 = SPP_CAP;
                float nn = n + (1 - A_NOISE) * (1 - ph1) * (p - n);
                s->noise[k] = nn < N_FLOOR ? N_FLOOR : nn;
            }
            float gam = post * inv_over;
            float xi = A_DD * s->s_prev[k] * rn * inv_over + (1 - A_DD) * (gam > 1.0f ? gam - 1.0f : 0.0f);
            if (xi < XI_MIN)
                xi = XI_MIN;
            float gw = xi / (1.0f + xi);
            s->s_prev[k] = gw * gw * p;
            g = gw < gmin ? gmin : gw;
            s->gain[k] = g;
        }
        x[0] *= g;
        if (!edge)
            x[1] *= g;
    }
}

/* Noise estimate / gains sanity (never seen): keeps a bad state from sticking. */
static bool state_ok(nr_t *s)
{
    float check = 0;
    for (int k = 0; k < NR_BINS; k++)
        check += s->noise[k] + s->s_prev[k];
    return isfinite(check);
}

/* One frame of work, spread over the hop after it was taken: slot n runs at sample n x
 * NR_SLOT_GAP of the hop, so a DSP interrupt (~11 samples at 16 kHz) never runs two of them.
 * Costs on the Cortex-M4 are in the comments (estimated, see tools/nr_test.c). */
static void slot(nr_t *s, int n)
{
    float *b = s->buf;
    switch (n) {
    case 0:                                 /* frame -> buf (windowed), bit reversal: ~8k */
        for (int i = 0; i < NR_N; i++)
            b[i] = s->in[i] * win[i];
        for (int i = 0; i < NR_N - NR_HOP; i++)
            s->in[i] = s->in[i + NR_HOP];
        bitrev(b);
        break;
    case 1: cfft_stages(b, 1, 4); break;    /* ~9.6k */
    case 2: cfft_stages(b, 5, 7); break;    /* ~7.2k */
    case 3: rfft_split(b); gains(s, 0, 20); break;              /* ~9.7k */
    case 4: gains(s, 20, 63); break;        /* ~220 per bin: ~9.5k */
    case 5: gains(s, 63, 106); break;
    case 6:                                 /* ~11k */
        gains(s, 106, NR_BINS);
        if (!s->ext_gain && !state_ok(s)) {
            s->faults++;
            nr_reset(s);                    /* drops this frame */
            return;
        }
        if (s->frames < 0xFFFF)
            s->frames++;
        irfft_split(b);
        break;
    case 7: bitrev(b); cfft_stages(b, 1, 3); break;             /* ~9k */
    case 8: cfft_stages(b, 4, 7); break;    /* ~9.6k */
    case 9: {                               /* overlap-add (1/M and the conjugate folded in): ~4k */
        const float sc = 1.0f / M;
        float *o = s->ola;
        for (int i = 0; i < NR_N; i += 2) {
            o[i] += b[i] * win[i] * sc;
            o[i + 1] -= b[i + 1] * win[i + 1] * sc;
        }
        break;
    }
    }
    s->slot = n + 1;
}

float nr_process(nr_t *s, float x)
{
    unsigned pos = s->pos;
    float y = s->out[pos];
    s->in[NR_N - NR_HOP + pos] = x;
    if (++pos == NR_HOP) {
        pos = 0;
        while (s->slot < NR_SLOTS)          /* only if a slot was skipped (never in normal use) */
            slot(s, s->slot);
        /* finished hop of the previous frame out, accumulator shifted */
        float *o = s->ola;
        for (int i = 0; i < NR_HOP; i++) {
            s->out[i] = o[i];
            o[i] = o[i + NR_HOP];
            o[i + NR_HOP] = 0;
        }
        slot(s, 0);
    } else if (pos % NR_SLOT_GAP == 0 && s->slot == pos / NR_SLOT_GAP) {
        slot(s, s->slot);
    }
    s->pos = pos;
    return y;
}
