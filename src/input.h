/* Front panel input (reversed from stock key scan @0x08030414, encoders @0x0802fec0).
 *
 * Key matrix 3x3, rows driven low one at a time, columns read active-low:
 *   rows:    PH0 = row 0, PC15 = row 1, PC14 = row 2
 *   columns: PC1 = bit 0, PC0 = bit 1, PH1 = bit 2
 *   key code = column bit | (0x100 << row)   (same codes as stock: 0x101 .. 0x404)
 *
 * Encoders (quadrature, inputs with pull-up):
 *   ENC_TUNE: A = PC13, B = PB9
 *   ENC_AUX:  A = PB8,  B = PC12   (stock uses it for a 0..46 level, likely volume)
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

enum { ENC_TUNE, ENC_AUX, ENC_COUNT };

/* Physical front-panel keys (identified on hardware). Power is separate (PC8). */
#define KEY_MODE      0x401
#define KEY_BAND_DOWN 0x402
#define KEY_BAND_UP   0x404
#define KEY_F1        0x101     /* buttons 1..4 under the LCD, left to right */
#define KEY_F2        0x102
#define KEY_F3        0x104
#define KEY_F4        0x201
#define KEY_TUNE_PUSH 0x202     /* tuning encoder push */
#define KEY_VOL_PUSH  0x204     /* volume encoder push */

/* Key events carry the stock combined code: OR of (column bit | 0x100 << row) of every key
 * pressed during the press session, e.g. BAND- + BAND+ together = 0x406.
 * SHORT is sent when all keys are released (unless LONG fired), LONG after 1 s held. */
#define KEY_SHORT         0x0000
#define KEY_LONG          0x4000
#define KEY_CODE(ev)      ((ev) & 0x0FFF)
#define LONG_PRESS_MS     1000

void input_init(void);
void input_tick_1ms(void);              /* call from SysTick */
int input_encoder_delta(int enc);       /* detents since last call */
int input_key_event(void);              /* -1 if none, else code | KEY_SHORT/KEY_LONG */
uint16_t input_keys_down(void);         /* current debounced key bitmap */
uint32_t input_raw_misc(void);          /* unknown inputs, see input.c */

#endif
