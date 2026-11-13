#include <stdint.h>
#include "stm32f4.h"
#include "audio.h"
#include "beep.h"
#include "keyer.h"

#define PIN_DOT  6                      /* PA6 */
#define PIN_DASH 7                      /* PA7 */

enum { MODE_MAN, MODE_AUTO_L, MODE_AUTO_R };
enum { ELEM_NONE, ELEM_DOT, ELEM_DASH };

static volatile bool enabled, ready, request;
static volatile bool qsk_on;
static volatile int speed_wpm = 20, mode = MODE_MAN, iambic = 1, hang_ms = 500, tone_hz = 800;
static volatile float dot_ms = 60.0f, ratio = 3.0f;

/* state (stock struct 0x20002008 offsets in the comments) */
static int on_cnt;                      /* +0x18 element on-time left, ms */
static int gap_cnt;                     /* +0x1C gap left, ms */
static int key;                         /* +0x2C key on */
static volatile int active;             /* +0x30 TX requested (key or hang) */
static int hang_cnt;                    /* +0x38 QSK hang left */
static int hang_extra;                  /* +0x3C 10 ms after it */
static int elem;                        /* +0x48 current / next element */
static int mem;                         /* +0x4C opposite paddle remembered */
static bool key_flag;                   /* sidetone / carrier state last sent out */

void keyer_init(void)
{
    keyer_set_speed(speed_wpm);
}

void keyer_set_speed(int wpm)
{
    wpm = wpm < 5 ? 5 : wpm > 100 ? 100 : wpm;
    speed_wpm = wpm;
    dot_ms = 1200.0f / wpm;
}

void keyer_set_mode(int m)
{
    mode = m;
}

void keyer_set_iambic(int b)
{
    iambic = b;
}

void keyer_set_ratio(float r)
{
    ratio = r < 2.0f ? 2.0f : r > 4.0f ? 4.0f : r;
}

void keyer_set_hang(int ms)
{
    hang_ms = ms;
}

void keyer_set_tone(int hz)
{
    tone_hz = hz;
}

void keyer_set_qsk(bool on)
{
    qsk_on = on;
}

static void reset_state(void)
{
    on_cnt = gap_cnt = key = hang_cnt = hang_extra = elem = mem = 0;
    active = 0;
    if (key_flag) {
        key_flag = false;
        beep(0, 0);
    }
}

void keyer_enable(bool on)
{
    if (on == enabled)
        return;
    enabled = false;                    /* the tick sees a consistent state */
    reset_state();
    request = false;
    ready = false;
    enabled = on;
}

void keyer_set_ready(bool r)
{
    ready = r;
}

bool keyer_request(void)
{
    bool r = request;
    request = false;
    return r;
}

bool keyer_active(void)
{
    return active;
}

bool keyer_down(void)
{
    return key;
}

/* Stock key-out callback 0x0802221c: sidetone on the edges, carrier envelope, TX request. */
static void key_out(int k, int act)
{
    if (k != key_flag) {
        key_flag = k;
        if (k)
            beep(tone_hz, 0xFFFFFFFFu);
        else
            beep(0, 0);
    }
    audio_tx_ramp(k);
    active = act;
}

static void start_dot(void)
{
    elem = ELEM_DOT;
    on_cnt = (int)dot_ms - 5;
    gap_cnt = (int)dot_ms + 5;
}

static void start_dash(void)
{
    elem = ELEM_DASH;
    on_cnt = (int)(dot_ms * ratio) - 5;
    gap_cnt = (int)dot_ms + 5;
}

void keyer_tick_1ms(void)
{
    if (!enabled)
        return;

    int p = (~GPIOA->IDR >> PIN_DOT & 1) | ((~GPIOA->IDR >> PIN_DASH & 1) << 1);
    if (!ready) {                       /* wait for the T/R switching, the element starts later */
        if (p)
            request = true;
        p = 0;
    }
    if (mode == MODE_AUTO_R)
        p = ((p << 1) & 2) | ((p >> 1) & 1);

    bool activate = false;              /* 0x080273e2: (re)start the hang time */
    bool idle = false;                  /* 0x080273f0: count the hang time down */

    if (mode == MODE_MAN) {
        on_cnt = gap_cnt = 0;
        key = p & 3 ? 1 : 0;
        if (key)
            activate = true;
        else
            idle = true;
    } else {
        if (on_cnt != 0) {
            if (iambic == 1 && p && !mem) {         /* mode B: remember the opposite paddle */
                if (elem == ELEM_DOT) {
                    if (p & 2) {
                        elem = ELEM_DASH;
                        mem = 1;
                    }
                } else if (elem == ELEM_DASH) {
                    if (p & 1) {
                        mem = 1;
                        elem = ELEM_DOT;
                    }
                }
            }
        } else if (gap_cnt == 0) {                  /* between elements: pick the next one */
            if (mem) {
                if (elem == ELEM_DOT)
                    start_dot();
                else if (elem == ELEM_DASH)
                    start_dash();
            } else if (p == 3) {
                if (elem == ELEM_DOT)
                    start_dash();
                else
                    start_dot();
            } else if (p == 1) {
                start_dot();
            } else if (p == 2) {
                start_dash();
            } else {
                elem = ELEM_NONE;
            }
            mem = 0;
        }
        if (on_cnt > 0) {
            on_cnt--;
            key = 1;
            activate = true;
        } else if (gap_cnt > 0) {
            gap_cnt--;
            key = 0;
            if (on_cnt == 0 && gap_cnt == 0)
                idle = true;
        } else if (key == 1) {
            activate = true;
        } else {
            idle = true;
        }
    }

    if (activate) {
        hang_cnt = hang_ms;
        active = 1;
        hang_extra = 10;
    } else if (idle && p == 0) {
        if (hang_cnt > 0) {
            hang_cnt--;
            if (hang_cnt <= 0 && hang_extra <= 0)
                active = 0;
        } else if (hang_extra > 0) {
            hang_extra--;
            if (hang_extra <= 0)
                active = 0;
        } else {
            active = 0;
        }
    }
    if (!qsk_on)                        /* 0x0802740c: QSK off, no TX request from the key */
        active = 0;
    key_out(key, active);
}
