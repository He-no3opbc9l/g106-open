/* I2S2 full duplex with the codec (codec = clock master, MCU = slave), as stock:
 *   PB12 WS, PB13 CK, PB15 SD (SPI2: slave RX), PB14 ext_SD (I2S2ext: slave TX), AF5/AF6
 *   DMA1 Stream3 ch0 (SPI2_RX) and Stream4 ch2 (I2S2ext_TX), circular, 16-bit halves of
 *   32-bit frames, 2 x 32 stereo frames; processing in the Stream3 HT/TC interrupt. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_FS     48000
#define AUDIO_BLOCK  32

typedef enum {
    AUDIO_OFF,      /* silence */
    AUDIO_TONE,     /* 1 kHz sine, -12 dBFS, both channels */
    AUDIO_MIC,      /* left input -> both outputs */
    AUDIO_IQ,       /* raw passthrough L->L, R->R */
    AUDIO_USB,      /* phasing SSB demodulator + 3 kHz LPF + AGC */
    AUDIO_LSB,
    AUDIO_CW,       /* USB + band-pass around the CW tone (LO offset by -tone) */
    AUDIO_CWR,      /* LSB + band-pass (LO offset by +tone) */
    AUDIO_AM,       /* envelope |I + jQ|, DC removed, 3 kHz LPF (LO offset +100 Hz) */
    AUDIO_NFM,      /* FM discriminator, 2.5 kHz deviation, de-emphasis, squelch gate (LO offset +100 Hz) */
    AUDIO_TX_CARRIER,   /* TX test: constant I/Q = carrier at the LO (stock CW TX level) */
    AUDIO_TX_USB,       /* TX test: 1 kHz single tone, upper sideband */
    AUDIO_TX_LSB,       /* TX test: 1 kHz single tone, lower sideband */
    AUDIO_TX_SSB_U,     /* TX voice/data: upper sideband */
    AUDIO_TX_SSB_L,     /* TX voice/data: lower sideband */
    AUDIO_TX_AM,        /* TX AM: carrier + modulation */
    AUDIO_TX_NFM,       /* TX NFM: pre-emphasis, 2.5 kHz peak deviation, constant carrier */
} audio_mode_t;

int audio_start(void);          /* I2S + DMA armed; call before codec_init() (codec = clock master) */
void audio_set_mode(audio_mode_t m);
void audio_set_gain(float g);   /* digital gain for MIC/IQ modes */
/* Peak |input| per channel since last call, as fraction of full scale; block count. */
void audio_stats(float *peak_l, float *peak_r, uint32_t *blocks);
/* Demodulator: input level before AGC (fraction of FS, smoothed) and current AGC gain. */
float audio_signal_level(void);
float audio_agc_gain(void);
/* TX output: power factor (stock table 0.2 / 0.4 / 1.0), DC offsets (stock calibration,
 * int32 units / 256), ramp state. tx_level() is the current ramp 0..1. */
void audio_set_tx(float power, int32_t dc_i, int32_t dc_q);
void audio_tx_ramp(bool up);            /* 10 ms ramp, as stock CW keying */
/* Own: mic equaliser (TX, microphone only), each band -12..+12 dB:
 * low shelf 300 Hz, peak 1.2 kHz, high shelf 2.5 kHz. */
void audio_set_mic_eq(int low_db, int mid_db, int high_db);
/* Own: speech compressor 0 = off .. 10 (max boost 2 dB per step), microphone only. */
void audio_set_comp(int level);
/* TX audio source: false = microphone (left ADC), true = line/data input (right ADC). */
void audio_set_tx_source(bool data);
float audio_tx_level(void);
/* Diagnostics: DSP interrupt CPU load in % of the block period since last call. */
void audio_cpu_load(uint32_t *avg_pct, uint32_t *max_pct);
/* Spectrum: start capturing n front-end I/Q pairs (interleaved) into buf; ready() when full. */
void audio_spectrum_capture(float *buf, unsigned n);
bool audio_spectrum_ready(void);
/* Diagnostics: accumulated cycles per RX stage (front, decimate, demod, interpolate), reset. */
void audio_profile(uint32_t out[4]);
/* NaN/inf guard: number of DSP state resets, first bad variable (1-based index into the list
 * in dsp_check(): agc_db agc_peak agc_env iqb_gain iqb_phase dc_i dc_q y12 am_dc tx_dc alc_env
 * comp_env cw_y eq_y fm_de nfm_gain fm_phase), 0 = never. */
void audio_dsp_faults(uint32_t *resets, uint32_t *first);
/* Adaptive RX I/Q balance in use: I gain and Q += phase * I. */
void audio_iq_balance(float *gain, float *phase);
/* Power at the SSB/CW demodulator output for both sidebands since the last call (diag). */
void audio_sideband_power(float *usb, float *lsb);
/* Diagnostics: capture n raw I/Q pairs (interleaved I, Q; blocks until done). */
void audio_capture_iq(float *buf, unsigned n);
/* TX voice: peak level before the limiter since the last call (1.0 = limiter threshold =
 * full PEP in SSB, 90 % modulation in AM). */
float audio_tx_peak(void);
/* Upper AGC gain limit in dB (stock-like AGC; tuning aid). */
void audio_set_agc_max(float db);
/* Own: AGC speed. HARD = stock (20 ms hang, fast recovery), MED / SOFT = longer hang and
 * slower recovery, OFF = fixed gain. */
enum { AGC_OFF, AGC_SOFT, AGC_MED, AGC_HARD };
void audio_set_agc(int speed);
/* SSB/AM receive band-pass (16 kHz FIR, up to 6 kHz), lo..hi Hz; safe to call any time (no glitch). */
void audio_set_rx_filter(float lo_hz, float hi_hz);
/* Own: NFM squelch gate (the decision is made in radio.c from the S-meter level). */
void audio_set_nfm_gate(bool open);
/* CW band-pass: centre (tone) and bandwidth in Hz. */
void audio_set_cw_filter(float tone_hz, float bw_hz);

#endif
