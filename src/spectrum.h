/* Spectrum scope and FFT-based S-meter, as stock (spectrum task 0x0802d404, scope draw
 * 0x0802f4f0, S-meter 0x0802f848):
 *   1024-point complex FFT of the front-end I/Q (after DC block + I/Q balance) at 48 kHz,
 *   Hann window, magnitude, bins re-ordered so the index grows with RF frequency (DC at 512),
 *   128 columns of 8 bins in dB, auto reference level, per-display-mode scale.
 *   S-meter: strongest bin inside the current mode's passband,
 *   dBm = 20 log10(max / 2048) + 16 - 15 (- 12 with the preamp on). */
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#define SCOPE_COLS 128

void spectrum_init(void);
/* Call from the main loop: processes a captured frame if one is ready. True if updated. */
bool spectrum_poll(int mode, int disp_mode, bool preamp, float zoom, bool tx);  /* zoom 1..3 (own) */
const uint16_t *spectrum_columns(void);   /* raw column heights in pixels (before smoothing) */
float spectrum_dbm(void);                 /* stock S-meter input, dBm */

/* Diagnostics: reference level dB, S-meter dBm, highest column, frames dropped as non-finite. */
void spectrum_diag(float *ref, float *level, int *max_col, uint32_t *bad_frames);

#endif
