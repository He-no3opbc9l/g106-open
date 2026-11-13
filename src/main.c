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

#define POWER_OFF_HOLD_MS 2000         /* stock 0x20000774 = 2000 ticks */

static void ticks_1ms(void)
{
    input_tick_1ms();
    beep_tick_1ms();
    keyer_tick_1ms();
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
    audio_start();                      /* before the codec starts its clocks, as stock */
    codec_init();
    float pk;
    uint32_t b0, b1;
    audio_stats(&pk, &pk, &b0);
    for (int t = 0; t < 50; t++) {      /* the codec needs a while before BCLK/WCLK run */
        delay_ms(10);
        audio_stats(&pk, &pk, &b1);
        if (b1 != b0)
            break;
    }
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
