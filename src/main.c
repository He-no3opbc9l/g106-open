#include "stm32f4.h"
#include "system.h"
#include "board.h"
#include "display.h"
#include "input.h"
#include "beep.h"
#include "i2c.h"
#include "versaclock.h"
#include "codec.h"
#include "audio.h"
#include "radio.h"
#include "cat.h"
#include "keyer.h"
#include "gfx_data.h"
#include <stdio.h>

#define POWER_OFF_HOLD_MS 2000         /* stock 0x20000774 = 2000 ticks */

static void ticks_1ms(void)
{
    input_tick_1ms();
    beep_tick_1ms();
    keyer_tick_1ms();
}

volatile uint32_t audio_init_tries;     /* attempts of the boot audio bring-up (1 = clean) */
volatile int32_t audio_init_err;        /* last failure: 0, CODEC_ERR_* or AUDIO_ERR_BLOCKS */
volatile uint32_t audio_init_blocks;    /* blocks counted in the 20 ms check of the last attempt */

#define AUDIO_ERR_BLOCKS (-4)

/* Boot audio bring-up as G106 OPEN 1.0.5 (cures the "S9+40 noise after power-on"): every attempt
 * starts from scratch = codec in reset, I2C2 re-initialised, I2S slaves + DMA reset and armed while
 * the codec is still in reset (stock order), codec init + read-back + wait for its ADC / DAC power
 * flags, then the I2S stream must deliver 20..40 blocks of 32 frames in 20 ms (48 kHz = 30). Up to
 * 3 attempts; any failure redoes the whole chain, not only the codec registers. */
static int audio_bringup(void)
{
    float pk;
    for (audio_init_tries = 1; audio_init_tries <= 3; audio_init_tries++) {
        codec_hold_reset();
        audio_stop();
        delay_ms(2);
        i2c_init(I2C_BUS2);
        audio_start();
        int e = codec_init();
        if (e == 0) {
            uint32_t b0, b1;
            delay_ms(20);                       /* BCLK / WCLK settle */
            audio_stats(&pk, &pk, &b0);
            delay_ms(20);
            audio_stats(&pk, &pk, &b1);
            audio_init_blocks = b1 - b0;
            if (audio_init_blocks >= 20 && audio_init_blocks <= 40) {
                audio_init_err = 0;
                return 0;
            }
            e = AUDIO_ERR_BLOCKS;
        }
        audio_init_err = e;
    }
    codec_hold_reset();                         /* outputs silent */
    audio_stop();
    return -1;
}

/* All attempts failed: say so and wait for the power key (held 2 s = off, as in normal use). */
static void __attribute__((noreturn)) audio_fail_screen(void)
{
    char t[24];
    lcd_backlight(true);
    lcd_clear();
    lcd_text_f(&font8, 4, 4, "AUDIO INIT ERROR", false);
    snprintf(t, sizeof t, "E%ld B%lu", (long)audio_init_err, (unsigned long)audio_init_blocks);
    lcd_text_f(&font8, 4, 20, t, false);
    lcd_text_f(&font8, 4, 36, "POWER OFF / ON", false);
    lcd_flush();
    while (power_key_pressed())
        ;
    for (;;) {
        uint32_t t0 = millis();
        while (power_key_pressed())
            if (millis() - t0 >= POWER_OFF_HOLD_MS)
                power_off();
    }
}

int main(void)
{
    board_init();
    cat_init();
    lcd_init();
    input_init();
    beep_init();
    i2c_init(I2C_BUS1);
    i2c_init(I2C_BUS2);
    board_rf_rx_state();
    vc_init();
    if (audio_bringup() < 0)
        audio_fail_screen();
    tick_hook = ticks_1ms;
    radio_init();

    /* Power key as stock (TIM2 tick 0x08030414 + 0x08029d6c): the key is usually still held from
     * power-on, so it is ignored until released; then a press shorter than 2 s toggles the
     * backlight on release, holding it 2 s powers off while still held. Own: 20 ms debounce. */
    bool armed = false, down = false, last_raw = false;
    uint32_t pressed_at = 0, edge_at = 0;

    for (;;) {
        uint32_t now = millis();

        cat_poll();
        radio_poll();

        bool raw = power_key_pressed();
        if (raw != last_raw) {
            last_raw = raw;
            edge_at = now;
        }
        if (now - edge_at >= 20) {
            if (raw != down) {
                down = raw;
                if (down)
                    pressed_at = now;
                else if (armed)
                    radio_toggle_backlight();
            }
            if (!down)
                armed = true;
        }
        if (armed && down && now - pressed_at >= POWER_OFF_HOLD_MS)
            radio_power_off();
    }
}
