#include <math.h>
#include "audio.h"
#include "settings.h"
#include "spectrum.h"

#define N     1024
#define PI_F  3.14159265f
#define CCM   __attribute__((section(".ccm")))

static float frame[2 * N] CCM;          /* interleaved I, Q from the DSP interrupt */
static float window[N] CCM;
static float tw[N] CCM;                 /* twiddles: cos, sin for k < N/2 */
static float spec[N] CCM;               /* magnitude, index = RF offset bin + 512 */
static float cum[N + 1] CCM;            /* prefix sums of spec, for fractional column edges */
static uint16_t cols[SCOPE_COLS];
static float ref_db = -100.0f;          /* stock 0x10000098 */
static float dbm = -140.0f;
static uint32_t nan_frames;

/* stock scope constants (CCM 0x10000090 / 94 / a0, 0x10000014 per display mode) */
#define REF_RISE   0.025f
#define REF_FALL   0.065f
#define REF_OFFSET 6.0f
static const float scale[3] = {0.6f, 0.88f, 1.36f};

void spectrum_init(void)
{
    for (int i = 0; i < N; i++)
        window[i] = 0.5f - 0.5f * cosf(2 * PI_F * i / (N - 1));
    for (int k = 0; k < N / 2; k++) {
        tw[2 * k] = cosf(2 * PI_F * k / N);
        tw[2 * k + 1] = -sinf(2 * PI_F * k / N);
    }
    audio_spectrum_capture(frame, N);
}

static void fft(float *x)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = x[2 * i]; x[2 * i] = x[2 * j]; x[2 * j] = t;
            t = x[2 * i + 1]; x[2 * i + 1] = x[2 * j + 1]; x[2 * j + 1] = t;
        }
    }
    for (int len = 2, step = N / 2; len <= N; len <<= 1, step >>= 1)
        for (int i = 0; i < N; i += len)
            for (int k = 0; k < len / 2; k++) {
                float c = tw[2 * k * step], s = tw[2 * k * step + 1];
                float *u = &x[2 * (i + k)], *v = &x[2 * (i + k + len / 2)];
                float vr = v[0] * c - v[1] * s, vi = v[0] * s + v[1] * c;
                v[0] = u[0] - vr; v[1] = u[1] - vi;
                u[0] += vr; u[1] += vi;
            }
}

/* Passband bins of each mode for the S-meter (stock 0x0802d404); L-D / U-D use the whole span. */
static void passband(int mode, int *lo, int *hi)
{
    static const int16_t pb[MODE_COUNT][2] = {
        {454, 506}, {0, 1023}, {518, 570}, {0, 1023}, {512, 529}, {495, 512}, {384, 639}, {448, 575},
    };
    *lo = pb[mode][0];
    *hi = pb[mode][1];
}

/* Sum of spec over the fractional bin range [a, b) via linear interpolation of the prefix sums. */
static float range_sum(float a, float b)
{
    if (a < 0) a = 0;
    if (b > N) b = N;
    int ia = (int)a, ib = (int)b;
    float ca = cum[ia] + (ia < N ? (a - ia) * spec[ia] : 0);
    float cb = cum[ib] + (ib < N ? (b - ib) * spec[ib] : 0);
    return cb - ca;
}

bool spectrum_poll(int mode, int disp_mode, bool preamp, float zoom, bool tx)
{
    if (!audio_spectrum_ready())
        return false;

    for (int i = 0; i < N; i++) {
        frame[2 * i] *= window[i];
        frame[2 * i + 1] *= window[i];
    }
    for (int i = 0; i < 2 * N; i++)
        if (!isfinite(frame[i])) {              /* never let one bad frame poison ref_db */
            nan_frames++;
            audio_spectrum_capture(frame, N);
            return false;
        }
    fft(frame);
    /* Our raw I/Q spectrum is inverted (USB appears at negative bins): mirror so that the index
     * grows with RF frequency, DC at 512, like stock. */
    for (int k = 0; k < N; k++) {
        int src = (N - (k - N / 2)) & (N - 1);
        float re = frame[2 * src], im = frame[2 * src + 1];
        spec[k] = sqrtf(re * re + im * im);
    }
    audio_spectrum_capture(frame, N);           /* re-arm while we draw */

    int lo, hi;
    passband(mode, &lo, &hi);
    float mx = 0;
    for (int k = lo; k <= hi; k++)
        if (spec[k] > mx)
            mx = spec[k];
    /* stock: -15 dB in RX, -60 dB in TX (the TX spectrum is the generated I/Q) */
    dbm = 20.0f * log10f(mx / 2048.0f + 1e-12f) + 16.0f - (tx ? 60.0f : 15.0f) - (preamp ? 12.0f : 0.0f);

    /* Own zoom around the centre: each column covers 8 / zoom bins; the sum is normalised back
     * to 8 bins so levels do not change with zoom (zoom 1 = stock). */
    cum[0] = 0;
    for (int k = 0; k < N; k++)
        cum[k + 1] = cum[k] + spec[k];
    float w = 8.0f / zoom, start = N / 2 - SCOPE_COLS / 2 * w;
    float sc = scale[disp_mode < 3 ? disp_mode : 0], mean = 0;
    for (int c = 0; c < SCOPE_COLS; c++) {
        float a = start + c * w;
        float sum = range_sum(a, a + w) * (8.0f / w);
        float db = 20.0f * log10f(sum * 0.000122f + 1e-7f);
        mean += db;
        float h = (db - (ref_db - REF_OFFSET)) * sc;
        cols[c] = h > 0 ? (uint16_t)h : 0;
    }
    mean /= SCOPE_COLS;
    if (mean < -140.0f)
        mean = -140.0f;
    else if (mean > -50.0f)
        mean = -50.0f;
    if (tx)
        ref_db = -70.0f;                        /* stock: fixed reference while transmitting */
    else
        ref_db += (mean - ref_db) * (mean > ref_db ? REF_RISE : REF_FALL);
    if (!isfinite(ref_db))
        ref_db = -100.0f;
    return true;
}

void spectrum_diag(float *ref, float *level, int *max_col, uint32_t *bad_frames)
{
    int m = 0;
    for (int c = 0; c < SCOPE_COLS; c++)
        if (cols[c] > m)
            m = cols[c];
    *ref = ref_db;
    *level = dbm;
    *max_col = m;
    *bad_frames = nan_frames;
}

const uint16_t *spectrum_columns(void)
{
    return cols;
}

float spectrum_dbm(void)
{
    return dbm;
}
