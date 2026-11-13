/* CW keyer, the same state machine as stock (tick 0x080272b4, key out 0x0802221c), ticked at 1 kHz.
 *
 * Inputs: PA6 = dot paddle, PA7 = dash paddle (active low, pull-up). Modes (KM): MAN = either
 * contact is a straight key, AUTO-L = iambic, AUTO-R = iambic with swapped paddles. IMB: MODE-A =
 * paddles are read only between elements, MODE-B = the opposite paddle pressed during an element
 * is remembered. Dot = 1200 / WPM ms, dash = dot x ratio (2.0..4.0); the key is on for
 * element - 5 ms and off for dot + 5 ms (the 10 ms ramps make up the difference).
 *
 * The keyer only runs once the transmitter is ready (keyer_set_ready); a paddle pressed before
 * that is reported by keyer_request() and the first element starts after the T/R switching.
 * `active` stays set for the QSK time (+ 10 ms) after the last element: the TX hang time.
 * Key down starts the sidetone (beeper at the CW tone) and ramps the carrier (audio_tx_ramp). */
#ifndef KEYER_H
#define KEYER_H

#include <stdbool.h>

void keyer_init(void);
void keyer_set_speed(int wpm);          /* KS 5..50 (clamped 5..100) */
void keyer_set_mode(int mode);          /* KM 0 MAN, 1 AUTO-L, 2 AUTO-R */
void keyer_set_iambic(int b);           /* IMB 0 MODE-A, 1 MODE-B */
void keyer_set_ratio(float ratio);      /* CWR 2.0 .. 4.0 */
void keyer_set_hang(int ms);            /* QSK time 0..1000 ms */
void keyer_set_tone(int hz);            /* CWT sidetone 500..1000 Hz */
void keyer_set_qsk(bool on);            /* QSK flag: off = the keyer never requests TX (PTT needed) */

void keyer_enable(bool on);             /* only in CW / CWR; off resets the state and the sidetone */
void keyer_set_ready(bool ready);       /* transmitter switched to TX */
bool keyer_request(void);               /* paddle pressed while not ready (cleared when read) */
bool keyer_active(void);                /* TX hang: keying in progress or QSK time not over */
bool keyer_down(void);                  /* key currently on */

void keyer_tick_1ms(void);              /* call from SysTick */

#endif
