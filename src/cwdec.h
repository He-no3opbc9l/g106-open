/* Own CW (Morse) decoder; stock has none. Pure logic without hardware access (host-testable,
 * see tools/cwdec_test.c): it is fed the tone power of the receiver once per CWDEC_FRAME_MS
 * (audio.c measures it in the DSP interrupt, radio.c passes it on from the main loop) and
 * produces text.
 *
 * Detector: power smoothed over ~0.2 dot, in dB; noise floor = average level in the spaces,
 * signal = peak tracker (fast attack, follows fading during marks). AUTO threshold at
 * noise + max(SNR - 6 dB, 0.7 x SNR) with hysteresis on the falling edge, gated off below
 * CWDEC_AUTO_SNR; a manual level puts the threshold that many dB over the noise floor (1 dB
 * hysteresis, no gate). Key changes shorter than ~0.15 dot are ignored, marks over 1.5 s are a
 * carrier.
 * Timing: the last 16 mark lengths are split into dots and dashes (2-means); a character's
 * elements are classified when it ends, with the latest split. Element / character / word gaps
 * are told apart from the dot mark and the element gap (the detector lengthens marks and
 * shortens gaps by the same amount); the word gap threshold also follows the measured
 * character and word gaps (Farnsworth spacing). */
#ifndef CWDEC_H
#define CWDEC_H

#include <stdbool.h>
#include <stdint.h>

#define CWDEC_FRAME_MS  2       /* one power value per 2 ms */
#define CWDEC_TEXT_N    64      /* decoded text kept (ring) */
#define CWDEC_LEVEL_MAX 20      /* manual threshold, dB over the noise floor */
#define CWDEC_AUTO_SNR  10.0f   /* AUTO: no decoding below this signal-to-noise ratio, dB */

/* Detector and timing back to the defaults (20 WPM); the text is kept. */
void cwdec_reset(void);
/* Text cleared. */
void cwdec_clear(void);
/* 0 = AUTO, 1..CWDEC_LEVEL_MAX = threshold in dB over the noise floor. */
void cwdec_set_level(int db);
/* One frame: mean tone power (any linear unit) over CWDEC_FRAME_MS. */
void cwdec_feed(float power);
/* Copies the newest at most n characters (n <= CWDEC_TEXT_N), NUL-terminated; returns the length. */
int cwdec_text(char *out, int n);
/* Incremented on every new character (redraw hint). */
uint32_t cwdec_seq(void);
/* Estimated speed in WPM, 0 = unknown (no signal for 10 s). */
int cwdec_wpm(void);
/* Key state seen by the detector (tuning aid). */
bool cwdec_key(void);

#endif
