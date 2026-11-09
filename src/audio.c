#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "stm32f4.h"
#include "system.h"
#include "audio.h"

#define I2S2_BASE     0x40003800
#define I2S2EXT_BASE  0x40003400
#define DMA1_BASE     0x40026000
#define DMA_S(n, off) REG32(DMA1_BASE + 0x10 + 0x18 * (n) + (off))
#define DMA_LISR      REG32(DMA1_BASE + 0x00)
#define DMA_LIFCR     REG32(DMA1_BASE + 0x08)
#define DMA_HIFCR     REG32(DMA1_BASE + 0x0C)
#define RX_STREAM     3
#define TX_STREAM     4

#define DWT_CTRL   REG32(0xE0001000)
#define DWT_CYCCNT REG32(0xE0001004)
#define DEMCR      REG32(0xE000EDFC)
#define HALF_WORDS    (AUDIO_BLOCK * 4)         /* 32 frames x 2 ch x 2 halfwords */

#define PI_F 3.14159265f

static volatile uint16_t rx_buf[2 * HALF_WORDS];
static volatile uint16_t tx_buf[2 * HALF_WORDS];

static volatile audio_mode_t mode = AUDIO_OFF;
static volatile float gain = 1.0f;
static float tone[48];                          /* one 1 kHz period at 48 kHz */
static unsigned tone_pos;
static volatile float peak_l, peak_r;
static volatile uint32_t block_count;
static float agc_env = 1e-4f, agc_g = 1.0f;
static volatile float sig_level;

/* Spectrum frame capture (front-end I/Q at 48 kHz) for the scope / S-meter. */
static float *volatile spec_buf;
static volatile unsigned spec_len, spec_pos;
static volatile bool spec_ready;

void audio_spectrum_capture(float *buf, unsigned n)
{
    spec_ready = false;
    spec_pos = 0;
    spec_len = n;
    spec_buf = buf;
}

bool audio_spectrum_ready(void)
{
    return spec_ready;
}

static inline void spec_push(float i, float q)
{
    if (spec_buf) {
        spec_buf[2 * spec_pos] = i;
        spec_buf[2 * spec_pos + 1] = q;
        if (++spec_pos >= spec_len) {
            spec_buf = 0;
            spec_ready = true;
        }
    }
}


/* ---- TX test output ---- */
static volatile float tx_power = 0.2f;
static volatile float tx_dc_i, tx_dc_q;         /* FS units */
static volatile bool tx_ramp_up;
static float tx_ramp;                           /* 0..1 */
static float tx_phase;

static volatile bool tx_data_src;
void audio_set_tx_source(bool data)
{
    tx_data_src = data;
}

void audio_set_tx(float power, int32_t dc_i, int32_t dc_q)
{
    tx_power = power;
    tx_dc_i = (float)dc_i * 256.0f / 2147483648.0f;
    tx_dc_q = (float)dc_q * 256.0f / 2147483648.0f;
}

void audio_tx_ramp(bool up)
{
    tx_ramp_up = up;
}

float audio_tx_level(void)
{
    return tx_ramp;
}

/* One TX output sample (I to the left channel, Q to the right, as stock). Carrier amplitude as
 * stock CW: I = Q = 0.09 * 0.5 at full ramp, times the power factor. */
static void tx_sample(audio_mode_t m, float *oi, float *oq)
{
    tx_ramp += tx_ramp_up ? 1.0f / 480 : -1.0f / 480;          /* 10 ms */
    tx_ramp = tx_ramp < 0 ? 0 : tx_ramp > 1 ? 1 : tx_ramp;
    float a = 0.045f * tx_ramp * tx_power, i = 0, q = 0;
    if (m == AUDIO_TX_CARRIER) {
        i = q = a;
    } else {
        float amp = a * 1.41421356f;
        tx_phase += 2 * PI_F * 1000.0f / AUDIO_FS;
        if (tx_phase > PI_F)
            tx_phase -= 2 * PI_F;
        i = amp * cosf(tx_phase);
        q = (m == AUDIO_TX_USB ? amp : -amp) * sinf(tx_phase);
    }
    spec_push(i, q);                    /* same orientation as the RX front end */
    *oi = i + tx_dc_i;
    *oq = q + tx_dc_q;
}

/* ===================== multirate RX chain =====================
 * 48 kHz: DC block + adaptive I/Q balance -> decimate by 3 (66-tap FIR, 8 kHz)
 * 16 kHz: demodulation (phasing SSB with a 191-tap Hilbert, CW band-pass, AM envelope),
 *         191-tap audio band-pass (variable high cut), AGC
 * 48 kHz: interpolate by 3 (same FIR, polyphase) -> codec
 * 16 kHz keeps up to ~6 kHz of audio bandwidth (stock runs its SSB path at 12 kHz). */
#define DEC      3
#define FS2      (AUDIO_FS / DEC)
#define NDEC     66
#define NPH      (NDEC / DEC)                   /* taps per interpolation phase */
#define HILB_N   191                            /* odd, type III; centre 95 (odd) */
#define HILB_D   (HILB_N / 2)
#define HILB_NZ  ((HILB_N + 1) / 2)             /* taps at odd distance from the centre */
#define BPF_N    191

static float h_dec[NDEC];
static float h_int[DEC][NPH];                   /* polyphase, already x DEC */
static float hilb_nz[HILB_NZ];
static int hilb_first;
#define HILB_PAIRS (HILB_NZ / 2)                /* antisymmetric: h[N-1-n] = -h[n] */
static float bpf_a[BPF_N], bpf_b[BPF_N];
static const float *volatile bpf = bpf_a;       /* active band-pass taps (swapped atomically) */

static float dbuf_i[2 * NDEC], dbuf_q[2 * NDEC];
static unsigned dpos, dphase;
static float qbuf[2 * HILB_N], ibuf[2 * HILB_N];
static unsigned hpos;
static float abuf[2 * BPF_N];
static unsigned apos;
static float ybuf[2 * NPH];
static unsigned ypos;
static float y12;                               /* last decimated-rate output sample */

static float blackman(int n, int len)
{
    return 0.42f - 0.5f * cosf(2 * PI_F * n / (len - 1)) + 0.08f * cosf(4 * PI_F * n / (len - 1));
}

/* Windowed-sinc low-pass, cut-off fc (fraction of fs), unity DC gain. */
static void design_lpf(float *h, int n, float fc)
{
    float sum = 0;
    for (int i = 0; i < n; i++) {
        float k = i - (n - 1) / 2.0f;
        float s = k != 0 ? sinf(2 * PI_F * fc * k) / (PI_F * k) : 2 * fc;
        h[i] = s * blackman(i, n);
        sum += h[i];
    }
    for (int i = 0; i < n; i++)
        h[i] /= sum;
}

static void dsp_init(void)
{
    design_lpf(h_dec, NDEC, 8000.0f / AUDIO_FS);
    /* y48[3n+p] = 3 * sum_k h[p + 3k] * y16[n - k]; window is oldest..newest. */
    for (int p = 0; p < DEC; p++)
        for (int i = 0; i < NPH; i++)
            h_int[p][i] = DEC * h_dec[p + DEC * (NPH - 1 - i)];

    hilb_first = (HILB_D & 1) ? 0 : 1;
    for (int n = hilb_first, j = 0; n < HILB_N; n += 2, j++) {
        int k = n - HILB_D;
        hilb_nz[j] = 2.0f / (PI_F * k) * blackman(n, HILB_N);
    }
}

/* Band-pass lo..hi Hz at 16 kHz, unity gain at the centre. Computed outside the ISR into the
 * inactive buffer, then swapped. */
void audio_set_rx_filter(float lo_hz, float hi_hz)
{
    float *h = bpf == bpf_a ? bpf_b : bpf_a;
    float fl = lo_hz / FS2, fh = hi_hz / FS2;
    for (int i = 0; i < BPF_N; i++) {
        float k = i - (BPF_N - 1) / 2.0f;
        float s = k != 0 ? (sinf(2 * PI_F * fh * k) - sinf(2 * PI_F * fl * k)) / (PI_F * k)
                         : 2 * (fh - fl);
        h[i] = s * blackman(i, BPF_N);
    }
    float fc = (fl + fh) / 2, re = 0, im = 0;
    for (int i = 0; i < BPF_N; i++) {
        re += h[i] * cosf(2 * PI_F * fc * i);
        im += h[i] * sinf(2 * PI_F * fc * i);
    }
    float g = sqrtf(re * re + im * im);
    for (int i = 0; i < BPF_N; i++)
        h[i] /= g;
    bpf = h;
}

/* CW band-pass: two cascaded RBJ band-pass biquads (0 dB peak), at 16 kHz. */
typedef struct { float b0, b2, a1, a2, x1, x2, y1, y2; } bq_t;
static bq_t cw_bq[2];

void audio_set_cw_filter(float tone_hz, float bw)
{
    float w0 = 2 * PI_F * tone_hz / FS2;
    float q = tone_hz / (bw * 1.55f);       /* two stages narrow the -3 dB width by ~0.64 */
    float alpha = sinf(w0) / (2 * q), a0 = 1 + alpha;
    for (int i = 0; i < 2; i++)
        cw_bq[i] = (bq_t){alpha / a0, -alpha / a0, -2 * cosf(w0) / a0, (1 - alpha) / a0, 0, 0, 0, 0};
}

static float bq_run(bq_t *f, float x)
{
    float y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

/* Dot products with 4 independent accumulators: a single accumulator chains every VFMA on the
 * previous one (~10 cycles/tap); four chains bring it to ~3 cycles/tap. */
static float dot(const float *c, const float *x, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int i = 0;
    for (; i + 3 < n; i += 4) {
        s0 += c[i] * x[i];
        s1 += c[i + 1] * x[i + 1];
        s2 += c[i + 2] * x[i + 2];
        s3 += c[i + 3] * x[i + 3];
    }
    for (; i < n; i++)
        s0 += c[i] * x[i];
    return (s0 + s1) + (s2 + s3);
}

/* Symmetric FIR (h[i] = h[n-1-i]): fold sample pairs first, then one FMA per pair. On the
 * Cortex-M4 a VFMA costs 3 cycles and an add 1, so this nearly halves the cost. */
static float dot_sym(const float *h, const float *x, int n)
{
    float s0 = 0, s1 = 0;
    const float *a = x, *b = x + n - 1;
    int pairs = n / 2, i = 0;
    for (; i + 1 < pairs; i += 2) {
        s0 += h[i] * (a[i] + b[-i]);
        s1 += h[i + 1] * (a[i + 1] + b[-i - 1]);
    }
    for (; i < pairs; i++)
        s0 += h[i] * (a[i] + b[-i]);
    if (n & 1)
        s0 += h[pairs] * x[pairs];
    return s0 + s1;
}

/* Same symmetric filter on two signals at once (decimation of I and Q). */
static void dot_sym2(const float *h, const float *x, const float *y, int n, float *ox, float *oy)
{
    float sx = 0, sy = 0;
    int pairs = n / 2;
    for (int i = 0; i < pairs; i++) {
        float c = h[i];
        sx += c * (x[i] + x[n - 1 - i]);
        sy += c * (y[i] + y[n - 1 - i]);
    }
    if (n & 1) {
        sx += h[pairs] * x[pairs];
        sy += h[pairs] * y[pairs];
    }
    *ox = sx;
    *oy = sy;
}

/* I/Q front end as stock (DMA ISR): DC block y = x - x1 + 0.995 y1, then blind adaptive I/Q
 * balance over blocks of 32 samples: c1 ~ E[sgn(I) Q], c2 ~ E[|I|], c3 ~ E[|Q|] (smoothed 0.997),
 * phase = -c1/c2, gain = sqrt(c3^2 - c1^2) / c2; Q' = Q + phase * I, I' = I * gain. */
#define IQB_N 32
static float dc_i, dc_q, prev_i, prev_q;
static float iqb_acc0, iqb_acc1, iqb_acc2, iqb_c1, iqb_c2, iqb_c3, iqb_phase, iqb_gain = 1.0f;
static unsigned iqb_cnt;

static void iq_front(float *pi, float *pq)
{
    dc_i = *pi - prev_i + 0.9975f * dc_i;          /* ~19 Hz (stock 0.995, ~38 Hz) */
    prev_i = *pi;
    dc_q = *pq - prev_q + 0.9975f * dc_q;
    prev_q = *pq;
    float i = dc_i, q = dc_q;

    float si = i < 0 ? -1.0f : i > 0 ? 1.0f : 0.0f;
    iqb_acc0 += si * q;
    iqb_acc1 += si * i;
    iqb_acc2 += (q < 0 ? -q : q);
    if (++iqb_cnt == IQB_N) {
        iqb_cnt = 0;
        iqb_c1 = -0.003f * iqb_acc0 / IQB_N + 0.997f * iqb_c1;
        iqb_c2 = 0.003f * iqb_acc1 / IQB_N + 0.997f * iqb_c2;
        iqb_c3 = 0.003f * iqb_acc2 / IQB_N + 0.997f * iqb_c3;
        /* Own guard against inf only (c2 -> 0 with a nearly silent I channel). The limits must
         * stay wide: some G106s need a large correction (raw image rejection 4..16 dB on one
         * radio vs 43..50 dB on another); clamping to +-0.3 / 0.7..1.4 left the opposite
         * sideband audible there. Stock has no limits at all. */
        float ph = iqb_c2 > 1e-9f ? iqb_c1 / iqb_c2 : 0;
        iqb_phase = ph < -4.0f ? -4.0f : ph > 4.0f ? 4.0f : ph;
        float g2 = iqb_c2 * iqb_c2;
        float r = g2 > 1e-18f ? (iqb_c3 * iqb_c3 - iqb_c1 * iqb_c1) / g2 : 0;
        float gn = r > 0 ? sqrtf(r) : 1.0f;
        iqb_gain = gn < 0.05f ? 0.05f : gn > 20.0f ? 20.0f : gn;
        iqb_acc0 = iqb_acc1 = iqb_acc2 = 0;
    }
    *pq = q + iqb_phase * i;
    *pi = i * iqb_gain;
}

/* AGC as stock (DMA ISR): works in dB, target -15 dB, fast attack 0.0578 per 48 kHz sample,
 * 960-sample (20 ms) hang, then slow recovery at a per-mode rate; gain clamped. Runs at 16 kHz
 * and updates the gain every 2 samples (= 6 samples at 48 kHz), constants scaled to match.
 * Look-ahead 10 ms and output scale 0.15 as stock.
 * Own: AGC speed (audio_set_agc): HARD = stock hang/decay, MED/SOFT = longer hang and slower
 * recovery, OFF = fixed gain AGC_OFF_DB (the output stage has ~31 dB headroom above the AGC
 * target before the DAC clips). */
#define AGC_TARGET_DB  (-15.0f)
#define AGC_STEP       6                        /* 48 kHz samples per gain update */
#define AGC_DELAY      (480 / DEC)
#define AGC_OUT_SCALE  0.15f
#define AGC_OFF_DB     40.0f
static float agc_db, agc_peak, agc_hold_peak;
static volatile float agc_decay = 0.00041658f;
static volatile float agc_max_db = 50.0f;
static volatile int agc_hang_len = 960;        /* in 48 kHz samples */
static volatile int agc_speed = AGC_HARD;
static int agc_hang;
static float agc_dline[AGC_DELAY];
static unsigned agc_dpos, agc_sub;

/* Fast log2 / exp2 (newlib's log10f/powf cost ~1000 cycles each here); ~1e-4 accuracy,
 * i.e. far below 0.01 dB in the AGC. */
static inline float fast_log2(float x)
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)(int)((v.u >> 23) & 0xFF) - 127.0f;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;      /* mantissa in [1, 2) */
    float m = v.f;
    return e + (-2.49684606f + (4.02854794f + (-2.08121416f + (0.628873618f - 0.0791581614f * m) * m) * m) * m);
}

static inline float fast_exp2(float x)
{
    if (x < -126.0f)
        return 0.0f;
    float fi = floorf(x), f = x - fi;
    float p = 1.0f + f * (0.6951579f + f * (0.2262207f + f * 0.0781009f));
    union { uint32_t u; float f; } v = {(uint32_t)((int)fi + 127) << 23};
    return v.f * p;
}

static float agc(float y)
{
    float delayed = agc_dline[agc_dpos];
    agc_dline[agc_dpos] = y;
    agc_dpos = agc_dpos == AGC_DELAY - 1 ? 0 : agc_dpos + 1;

    float ay = fabsf(y);
    agc_peak = ay > agc_peak ? ay : agc_peak + (ay - agc_peak) * 0.5f;
    if (ay > agc_hold_peak)
        agc_hold_peak = ay;

    if (++agc_sub == AGC_STEP / DEC) {
        agc_sub = 0;
        float err = AGC_TARGET_DB - (agc_db + 6.0205999f * fast_log2(agc_hold_peak + 2.3e-10f));
        if (agc_speed == AGC_OFF) {
            agc_db = AGC_OFF_DB;
        } else if (err < 0) {
            agc_hang = agc_hang_len;
            agc_db += err * 0.2997f;            /* 1 - (1 - 0.0578)^6 */
        } else if (agc_hang > 0) {
            agc_hang -= AGC_STEP;
        } else {
            agc_db += err * agc_decay * AGC_STEP;
        }
        if (agc_db > agc_max_db)
            agc_db = agc_max_db;
        else if (agc_db < -90.0f)
            agc_db = -90.0f;
        agc_g = fast_exp2(agc_db * 0.16609640f);        /* 10^(dB/20) */
        agc_hold_peak = agc_peak;
    }
    agc_env += (agc_peak - agc_env) * 0.008f;      /* smoothed level for the S-meter */
    return delayed * agc_g * AGC_OUT_SCALE;
}

void audio_set_agc_max(float db)
{
    agc_max_db = db;
}

/* Hang / recovery time constant per AGC speed (SOFT, MED, HARD) and demodulator group
 * (SSB, CW, AM), ms. HARD = stock: hang 20 ms, decay per sample SSB 0.00041658 (50 ms),
 * CW 0.00208116 (10 ms), AM 4.1658e-05 (500 ms). */
static const uint16_t agc_times[3][3][2] = {
    {{600, 1000}, {300, 300}, {600, 2000}},     /* SOFT */
    {{250, 250}, {120, 80}, {250, 1000}},       /* MED */
    {{20, 50}, {20, 10}, {20, 500}},            /* HARD (stock) */
};

static void agc_params(audio_mode_t m, int speed)
{
    if (speed < AGC_SOFT || speed > AGC_HARD)
        speed = AGC_HARD;
    int g = m == AUDIO_CW || m == AUDIO_CWR ? 1 : m == AUDIO_AM ? 2 : 0;
    const uint16_t *t = agc_times[speed - AGC_SOFT][g];
    agc_hang_len = t[0] * 48;
    agc_decay = 1.0f / (t[1] * 48.0f);
}

void audio_set_agc(int speed)
{
    agc_params(mode, speed);
    agc_speed = speed;
}

static float bpf_run(float x)
{
    abuf[apos] = abuf[apos + BPF_N] = x;
    apos = apos == BPF_N - 1 ? 0 : apos + 1;
    return dot_sym(bpf, &abuf[apos], BPF_N);
}

static volatile float sbp_usb, sbp_lsb;          /* demodulated power per sideband (diag) */

void audio_sideband_power(float *usb, float *lsb)
{
    __disable_irq();
    *usb = sbp_usb;
    *lsb = sbp_lsb;
    sbp_usb = sbp_lsb = 0;
    __enable_irq();
}

/* Phasing SSB at 16 kHz: I delayed by the Hilbert group delay, Q through the Hilbert FIR. */
static float ssb_demod(float i, float q, int lsb)
{
    qbuf[hpos] = qbuf[hpos + HILB_N] = q;
    ibuf[hpos] = ibuf[hpos + HILB_N] = i;
    hpos = hpos == HILB_N - 1 ? 0 : hpos + 1;
    const float *qw = &qbuf[hpos];              /* oldest .. newest */
    /* non-zero taps at n = first, first+2, ...; fold n with HILB_N-1-n (antisymmetric) */
    float s0 = 0, s1 = 0;
    for (int j = 0; j + 1 < HILB_PAIRS; j += 2) {
        int n0 = hilb_first + 2 * j, n1 = n0 + 2;
        s0 += hilb_nz[j] * (qw[n0] - qw[HILB_N - 1 - n0]);
        s1 += hilb_nz[j + 1] * (qw[n1] - qw[HILB_N - 1 - n1]);
    }
    if (HILB_PAIRS & 1) {
        int j = HILB_PAIRS - 1, n0 = hilb_first + 2 * j;
        s0 += hilb_nz[j] * (qw[n0] - qw[HILB_N - 1 - n0]);
    }
    float hq = s0 + s1;
    float id = ibuf[hpos + HILB_D];
    float u = id - hq, l = id + hq;                /* both sidebands (diagnostics) */
    sbp_usb += u * u;
    sbp_lsb += l * l;
    return lsb ? l : u;
}

/* AM: envelope of the complex baseband (carrier at 100 Hz), DC tracked and removed,
 * x2 as stock (mode 7 branch: envelope high-pass * 2.0). */
static float am_dc;
static float am_demod(float i, float q)
{
    float env = sqrtf(i * i + q * q);
    am_dc += (env - am_dc) * 0.002f;
    return 2.0f * (env - am_dc);
}

/* One 16 kHz I/Q sample -> one 16 kHz audio sample. */
static float demod12(audio_mode_t m, float i, float q)
{
    switch (m) {
    case AUDIO_USB:
    case AUDIO_LSB:
        return agc(bpf_run(ssb_demod(i, q, m == AUDIO_LSB)));
    case AUDIO_CW:
    case AUDIO_CWR:
        return agc(bq_run(&cw_bq[1], bq_run(&cw_bq[0], ssb_demod(i, q, m == AUDIO_CWR))));
    case AUDIO_AM:
        return agc(bpf_run(am_demod(i, q)));
    default:
        return 0;
    }
}

/* Per-stage cycle profile (diagnostics): front end, decimation, demod, interpolation. */
static volatile uint32_t prof[4];

/* RX chain for one 48 kHz I/Q sample; returns the 48 kHz audio sample. */
static float rx48(audio_mode_t m, float i, float q)
{
    uint32_t t = DWT_CYCCNT, t2;
    iq_front(&i, &q);
    spec_push(i, q);
    t2 = DWT_CYCCNT; prof[0] += t2 - t; t = t2;
    dbuf_i[dpos] = dbuf_i[dpos + NDEC] = i;
    dbuf_q[dpos] = dbuf_q[dpos + NDEC] = q;
    dpos = dpos == NDEC - 1 ? 0 : dpos + 1;

    if (++dphase == DEC) {
        dphase = 0;
        float i12, q12;
        dot_sym2(h_dec, &dbuf_i[dpos], &dbuf_q[dpos], NDEC, &i12, &q12);
        t2 = DWT_CYCCNT; prof[1] += t2 - t; t = t2;
        y12 = demod12(m, i12, q12);
        t2 = DWT_CYCCNT; prof[2] += t2 - t; t = t2;
        ybuf[ypos] = ybuf[ypos + NPH] = y12;
        ypos = ypos == NPH - 1 ? 0 : ypos + 1;
    }
    float y = dot(h_int[dphase], &ybuf[ypos], NPH);
    prof[3] += DWT_CYCCNT - t;
    return y;
}

void audio_profile(uint32_t out[4])
{
    for (int k = 0; k < 4; k++) {
        out[k] = prof[k];
        prof[k] = 0;
    }
}

/* ===================== I2S / DMA ===================== */

static void pin_af(unsigned pin, unsigned af)
{
    GPIOB->AFR[pin / 8] = (GPIOB->AFR[pin / 8] & ~(0xFu << 4 * (pin % 8))) | (af << 4 * (pin % 8));
    GPIOB->OSPEEDR |= 2u << 2 * pin;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 2 * pin)) | (2u << 2 * pin);
}

static void dma_setup(int s, uint32_t chsel, uint32_t dir, volatile void *periph, volatile uint16_t *mem)
{
    DMA_S(s, 0x00) = 0;
    while (DMA_S(s, 0x00) & 1)
        ;
    DMA_S(s, 0x04) = 2 * HALF_WORDS;            /* NDTR */
    DMA_S(s, 0x08) = (uint32_t)periph;          /* PAR */
    DMA_S(s, 0x0C) = (uint32_t)mem;             /* M0AR */
    DMA_S(s, 0x14) = 0;                         /* FIFO off (direct mode) */
    DMA_S(s, 0x00) = (chsel << 25) | (2u << 16) /* priority high */
                   | (1u << 13) | (1u << 11)    /* MSIZE, PSIZE = 16 bit */
                   | (1u << 10) | (1u << 8)     /* MINC, CIRC */
                   | (dir << 6);
}

int audio_start(void)
{
    for (int i = 0; i < 48; i++)
        tone[i] = 0.25f * sinf(2.0f * PI_F * i / 48.0f);
    dsp_init();
    audio_set_rx_filter(20, 2700);
    audio_set_cw_filter(800, 500);
    DEMCR |= 1u << 24;                          /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;                             /* CYCCNTENA */

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOB | (1u << 21);    /* DMA1 */
    RCC->APB1ENR |= 1u << 14;                           /* SPI2 */
    __dsb();

    pin_af(12, 5);
    pin_af(13, 5);
    pin_af(15, 5);
    pin_af(14, 6);

    volatile uint32_t *i2s = (volatile uint32_t *)I2S2_BASE;
    volatile uint32_t *ext = (volatile uint32_t *)I2S2EXT_BASE;
    i2s[7] = 0;                                 /* I2SCFGR: disable */
    ext[7] = 0;
    i2s[1] = 0;                                 /* CR2 */
    ext[1] = 0;

    dma_setup(RX_STREAM, 0, 0, &i2s[3], rx_buf);
    dma_setup(TX_STREAM, 2, 1, &ext[3], tx_buf);
    DMA_LIFCR = 0x0F7D0F7D;
    DMA_HIFCR = 0x0F7D0F7D;
    DMA_S(RX_STREAM, 0x00) |= (1u << 4) | (1u << 3);   /* TCIE, HTIE */

    /* I2SMOD, Philips, 32-bit data in 32-bit channel; SPI2 slave RX, ext slave TX. */
    i2s[7] = (1u << 11) | (1u << 8) | (2u << 1) | 1u;
    ext[7] = (1u << 11) | (0u << 8) | (2u << 1) | 1u;
    i2s[1] = 1u << 0;                           /* RXDMAEN */
    ext[1] = 1u << 1;                           /* TXDMAEN */

    DMA_S(TX_STREAM, 0x00) |= 1;
    DMA_S(RX_STREAM, 0x00) |= 1;

    /* As stock main (0x080305a4): armed before codec_init(), so the slave is waiting when
     * the codec (clock master) starts BCLK/WCLK and locks onto the first frame. Enabling it on
     * running clocks (with a WS-edge wait) made the L/R = I/Q order depend on code layout and
     * on the radio: mirrored sidebands on another G106. */
    ext[7] |= 1u << 10;                         /* I2SE */
    i2s[7] |= 1u << 10;

    /* DSP runs below UART/SysTick so they never overrun. */
    *(volatile uint8_t *)(0xE000E400 + 14) = 0x80;     /* NVIC_IPR: DMA1_Stream3 */
    REG32(0xE000E100) = 1u << 14;               /* NVIC: DMA1_Stream3 */
    return 0;
}

void audio_set_mode(audio_mode_t m)
{
    agc_params(m, agc_speed);                   /* stock decay rates per mode, see agc_times */
    mode = m;
}

void audio_set_gain(float g)
{
    gain = g;
}

void audio_stats(float *pl, float *pr, uint32_t *blocks)
{
    __disable_irq();
    *pl = peak_l;
    *pr = peak_r;
    *blocks = block_count;
    peak_l = peak_r = 0;
    __enable_irq();
}

static inline float in_sample(const volatile uint16_t *p)
{
    int32_t v = (int32_t)((uint32_t)p[0] << 16 | p[1]);
    return v * (1.0f / 2147483648.0f);
}

static inline void out_sample(volatile uint16_t *p, float x)
{
    if (x > 0.999f)
        x = 0.999f;
    else if (x < -0.999f)
        x = -0.999f;
    uint32_t v = (uint32_t)(int32_t)(x * 2147483648.0f);
    p[0] = v >> 16;
    p[1] = v & 0xFFFF;
}

/* ===================== TX voice chain =====================
 * 48 kHz: mic (left ADC) or data input (right ADC) -> DC block -> decimate by 3
 * 16 kHz: band-pass 20 Hz .. SSBF (same taps as RX) -> peak limiter (ALC) ->
 *         SSB: I = delayed x, Q = +/- Hilbert(x);  AM: I = Q = carrier + modulation
 * 48 kHz: interpolate I and Q by 3 -> power factor + DC offsets.
 * Full scale: SSB peak envelope = the CW carrier vector (I = Q = 0.045 x power). */
static float tx_dc, tx_prev;
static float tdbuf[2 * NDEC];
static unsigned tdpos, tdphase;
static float tabuf[2 * BPF_N];
static unsigned tapos;
static float thbuf[2 * HILB_N];
static unsigned thpos;
static float tybuf_i[2 * NPH], tybuf_q[2 * NPH];
static unsigned typos;
static float alc_env, alc_gain = 1.0f;
static volatile float tx_peak;                  /* peak before the limiter (diag) */

/* Own: extra mic gain in AM. A receiver's AGC holds the AM carrier, so the audio level there is
 * the modulation depth: speech peaking at 20-30 % (fine for SSB, where the receiving AGC follows
 * the peaks) was very quiet. Stock modulates 0.95 x the mic sample with no limiter; here the
 * gain is raised and the peak limiter keeps the modulation at <= 90 %. */
#define AM_TX_GAIN 4.0f                         /* +12 dB */

float audio_tx_peak(void)
{
    float p = tx_peak;
    tx_peak = 0;
    return p;
}

/* Mic equaliser (own): low shelf 300 Hz, peak 1.2 kHz, high shelf 2.5 kHz at 16 kHz,
 * RBJ cookbook biquads (direct form I). Not applied to the data/line input. */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } eq_t;
static eq_t eq[3];
static volatile bool eq_flat = true;

static float eq_run(eq_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

/* type 0 = low shelf, 1 = peaking, 2 = high shelf; gain in dB. */
static eq_t eq_design(int type, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f), w0 = 2 * PI_F * f0 / FS2;
    float cw = cosf(w0), sw = sinf(w0), b0, b1, b2, a0, a1, a2;
    if (type == 1) {
        float alpha = sw / (2 * 0.9f);                      /* Q 0.9 */
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        float alpha = sw / 2 * sqrtf(2.0f), sa = 2 * sqrtf(A) * alpha;   /* shelf slope 1 */
        if (type == 0) {
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
        }
    }
    return (eq_t){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0, 0, 0};
}

/* Speech compressor (own): upward compression towards a target level with a noise gate.
 * Envelope: attack ~1 ms, release ~150 ms (16 kHz). Gain = target / env, limited to
 * 2 dB x level; below the gate the gain is held (no pumping up of background noise).
 * Gain smoothing: fast decrease, slow increase. Runs before the peak limiter. */
static volatile float comp_max_gain = 1.0f;     /* 1 = off */
static float comp_env, comp_g = 1.0f;

void audio_set_comp(int level)
{
    comp_max_gain = level > 0 ? powf(10.0f, 2.0f * level / 20.0f) : 1.0f;
}

static float compress(float y)
{
    float ay = fabsf(y);
    comp_env += (ay - comp_env) * (ay > comp_env ? 0.06f : 0.0004f);
    if (comp_env > 0.003f) {                    /* ~-50 dB gate */
        float g = 0.5f / comp_env;
        if (g > comp_max_gain)
            g = comp_max_gain;
        if (g < 1.0f)
            g = 1.0f;
        comp_g += (g - comp_g) * (g < comp_g ? 0.05f : 0.0008f);
    }
    return y * comp_g;
}

void audio_set_mic_eq(int low_db, int mid_db, int high_db)
{
    eq_t n[3] = {eq_design(0, 300, low_db), eq_design(1, 1200, mid_db), eq_design(2, 2500, high_db)};
    __disable_irq();
    for (int k = 0; k < 3; k++) {           /* keep the filter state, swap coefficients */
        n[k].x1 = eq[k].x1; n[k].x2 = eq[k].x2; n[k].y1 = eq[k].y1; n[k].y2 = eq[k].y2;
        eq[k] = n[k];
    }
    eq_flat = !low_db && !mid_db && !high_db;
    __enable_irq();
}

static void tx16(audio_mode_t m, float x, float *oi, float *oq)
{
    /* band-pass, then a fast-attack / slow-release peak limiter to |x| <= 1 */
    tabuf[tapos] = tabuf[tapos + BPF_N] = x;
    tapos = tapos == BPF_N - 1 ? 0 : tapos + 1;
    float y = dot_sym(bpf, &tabuf[tapos], BPF_N) * 4.0f;   /* +12 dB make-up */
    if (!tx_data_src && !eq_flat)
        y = eq_run(&eq[2], eq_run(&eq[1], eq_run(&eq[0], y)));
    if (!tx_data_src && comp_max_gain > 1.0f)
        y = compress(y);
    if (m == AUDIO_TX_AM)
        y *= AM_TX_GAIN;
    float ay = fabsf(y);
    if (ay > tx_peak)
        tx_peak = ay;
    alc_env = ay > alc_env ? ay : alc_env * 0.9995f;
    alc_gain = alc_env > 1.0f ? 1.0f / alc_env : 1.0f;
    y *= alc_gain;

    if (m == AUDIO_TX_AM) {
        float v = 0.5f + 0.45f * y;                          /* carrier 1/2, 90 % modulation */
        *oi = *oq = v * 0.70710678f;
        return;
    }
    thbuf[thpos] = thbuf[thpos + HILB_N] = y;
    thpos = thpos == HILB_N - 1 ? 0 : thpos + 1;
    const float *w = &thbuf[thpos];
    float s0 = 0, s1 = 0;
    for (int j = 0; j + 1 < HILB_PAIRS; j += 2) {
        int n0 = hilb_first + 2 * j, n1 = n0 + 2;
        s0 += hilb_nz[j] * (w[n0] - w[HILB_N - 1 - n0]);
        s1 += hilb_nz[j + 1] * (w[n1] - w[HILB_N - 1 - n1]);
    }
    if (HILB_PAIRS & 1) {
        int j = HILB_PAIRS - 1, n0 = hilb_first + 2 * j;
        s0 += hilb_nz[j] * (w[n0] - w[HILB_N - 1 - n0]);
    }
    float h = s0 + s1;
    *oi = w[HILB_D];
    *oq = m == AUDIO_TX_SSB_U ? h : -h;
}

static void tx48(audio_mode_t m, float l, float r, float *oi, float *oq)
{
    float x = tx_data_src ? r : l;
    tx_dc = x - tx_prev + 0.9975f * tx_dc;          /* ~19 Hz */
    tx_prev = x;
    tdbuf[tdpos] = tdbuf[tdpos + NDEC] = tx_dc;
    tdpos = tdpos == NDEC - 1 ? 0 : tdpos + 1;
    if (++tdphase == DEC) {
        tdphase = 0;
        float x16 = dot_sym(h_dec, &tdbuf[tdpos], NDEC), i16, q16;
        tx16(m, x16, &i16, &q16);
        tybuf_i[typos] = tybuf_i[typos + NPH] = i16;
        tybuf_q[typos] = tybuf_q[typos + NPH] = q16;
        typos = typos == NPH - 1 ? 0 : typos + 1;
    }
    float i = dot(h_int[tdphase], &tybuf_i[typos], NPH);
    float q = dot(h_int[tdphase], &tybuf_q[typos], NPH);
    spec_push(i * 0.0636396f, q * 0.0636396f);      /* TX spectrum (stock shows it too) */

    tx_ramp += tx_ramp_up ? 1.0f / 480 : -1.0f / 480;
    tx_ramp = tx_ramp < 0 ? 0 : tx_ramp > 1 ? 1 : tx_ramp;
    float a = 0.0636396f * tx_ramp * tx_power;              /* = |(0.045, 0.045)| */
    *oi = i * a + tx_dc_i;
    *oq = q * a + tx_dc_q;
}

/* Diagnostic capture of raw I/Q (L = I, R = Q). */
static float *volatile cap_buf;
static volatile unsigned cap_len, cap_pos;

void audio_capture_iq(float *buf, unsigned n)
{
    cap_pos = 0;
    cap_len = n;
    cap_buf = buf;
    while (cap_buf)
        ;
}

/* NaN/inf guard: one non-finite sample leaves the AGC (and the I/Q balance) stuck at NaN for
 * good, since every comparison with NaN is false: silence and an empty scope until reboot.
 * Checked once per block; on a hit all DSP state is cleared. The first bad variable is kept
 * for `astat`. */
static volatile uint32_t dsp_resets, dsp_fault;

static void dsp_state_reset(void)
{
    memset(dbuf_i, 0, sizeof dbuf_i);
    memset(dbuf_q, 0, sizeof dbuf_q);
    memset(qbuf, 0, sizeof qbuf);
    memset(ibuf, 0, sizeof ibuf);
    memset(abuf, 0, sizeof abuf);
    memset(ybuf, 0, sizeof ybuf);
    memset(agc_dline, 0, sizeof agc_dline);
    memset(tdbuf, 0, sizeof tdbuf);
    memset(tabuf, 0, sizeof tabuf);
    memset(thbuf, 0, sizeof thbuf);
    memset(tybuf_i, 0, sizeof tybuf_i);
    memset(tybuf_q, 0, sizeof tybuf_q);
    for (int k = 0; k < 2; k++)
        cw_bq[k].x1 = cw_bq[k].x2 = cw_bq[k].y1 = cw_bq[k].y2 = 0;
    for (int k = 0; k < 3; k++)
        eq[k].x1 = eq[k].x2 = eq[k].y1 = eq[k].y2 = 0;
    y12 = am_dc = 0;
    dc_i = dc_q = prev_i = prev_q = 0;
    iqb_acc0 = iqb_acc1 = iqb_acc2 = iqb_c1 = iqb_c2 = iqb_c3 = iqb_phase = 0;
    iqb_gain = 1.0f;
    agc_db = agc_peak = agc_hold_peak = 0;
    agc_env = 1e-4f;
    agc_g = 1.0f;
    tx_dc = tx_prev = alc_env = comp_env = 0;
    alc_gain = comp_g = 1.0f;
}

static void dsp_check(void)
{
    const float v[] = {agc_db, agc_peak, agc_env, iqb_gain, iqb_phase, dc_i, dc_q, y12,
                       am_dc, tx_dc, alc_env, comp_env, cw_bq[1].y1, eq[2].y1};
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++)
        if (!isfinite(v[k])) {
            if (!dsp_fault)
                dsp_fault = k + 1;
            dsp_resets++;
            dsp_state_reset();
            return;
        }
}

void audio_iq_balance(float *gain_, float *phase)
{
    *gain_ = iqb_gain;
    *phase = iqb_phase;
}

void audio_dsp_faults(uint32_t *resets, uint32_t *first)
{
    *resets = dsp_resets;
    *first = dsp_fault;
}

static void process(const volatile uint16_t *in, volatile uint16_t *out)
{
    float pl = peak_l, pr = peak_r, g = gain;
    audio_mode_t m = mode;

    for (int i = 0; i < AUDIO_BLOCK; i++, in += 4, out += 4) {
        float l = in_sample(in), r = in_sample(in + 2);
        if (cap_buf) {
            cap_buf[2 * cap_pos] = l;
            cap_buf[2 * cap_pos + 1] = r;
            if (++cap_pos >= cap_len)
                cap_buf = 0;
        }
        float al = fabsf(l), ar = fabsf(r);
        if (al > pl)
            pl = al;
        if (ar > pr)
            pr = ar;

        float ol = 0, or_ = 0;
        switch (m) {
        case AUDIO_TONE:
            ol = or_ = tone[tone_pos];
            tone_pos = tone_pos == 47 ? 0 : tone_pos + 1;
            break;
        case AUDIO_MIC:
            ol = or_ = l * g;
            break;
        case AUDIO_IQ:
            ol = l * g;
            or_ = r * g;
            break;
        case AUDIO_USB:
        case AUDIO_LSB:
        case AUDIO_CW:
        case AUDIO_CWR:
        case AUDIO_AM:
            ol = or_ = rx48(m, l, r) * g;
            break;
        case AUDIO_TX_CARRIER:
        case AUDIO_TX_USB:
        case AUDIO_TX_LSB:
            tx_sample(m, &ol, &or_);
            break;
        case AUDIO_TX_SSB_U:
        case AUDIO_TX_SSB_L:
        case AUDIO_TX_AM:
            tx48(m, l, r, &ol, &or_);
            break;
        default:
            break;
        }
        out_sample(out, ol);
        out_sample(out + 2, or_);
    }
    peak_l = pl;
    peak_r = pr;
    dsp_check();
    sig_level = agc_env;
    block_count++;
}

/* CPU load of the DSP interrupt, measured with the DWT cycle counter. */
static volatile uint32_t isr_cycles_max, isr_cycles_sum, isr_count;

void audio_cpu_load(uint32_t *avg_pct, uint32_t *max_pct)
{
    const uint32_t per_block = SYSCLK_HZ / (AUDIO_FS / AUDIO_BLOCK);
    __disable_irq();
    uint32_t n = isr_count ? isr_count : 1;
    *avg_pct = (uint32_t)((uint64_t)isr_cycles_sum * 100 / n / per_block);
    *max_pct = isr_cycles_max * 100 / per_block;
    isr_cycles_sum = isr_cycles_max = isr_count = 0;
    __enable_irq();
}

void DMA1_Stream3_IRQHandler(void)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t isr = DMA_LISR;
    if (isr & (1u << 26)) {                     /* HTIF3: first half received */
        DMA_LIFCR = 1u << 26;
        process(&rx_buf[0], &tx_buf[0]);
    }
    if (isr & (1u << 27)) {                     /* TCIF3: second half received */
        DMA_LIFCR = 1u << 27;
        process(&rx_buf[HALF_WORDS], &tx_buf[HALF_WORDS]);
    }
    DMA_LIFCR = isr & ((1u << 22) | (1u << 24) | (1u << 25));  /* FE/DME/TE */
    uint32_t dt = DWT_CYCCNT - t0;
    isr_cycles_sum += dt;
    isr_count++;
    if (dt > isr_cycles_max)
        isr_cycles_max = dt;
}

float audio_signal_level(void)
{
    return sig_level;
}

float audio_agc_gain(void)
{
    return agc_g;
}
