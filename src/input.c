#include <stdbool.h>
#include "stm32f4.h"
#include "input.h"

#define ROW_SETTLE_MS 2
#define EVQ_SIZE      16

typedef struct {
    GPIO_TypeDef *port;
    uint8_t pin;
} pin_t;

static const pin_t rows[3] = {{GPIOH, 0}, {GPIOC, 15}, {GPIOC, 14}};
static const pin_t cols[3] = {{GPIOC, 1}, {GPIOC, 0}, {GPIOH, 1}};
static const pin_t enc_pins[ENC_COUNT][2] = {
    {{GPIOC, 13}, {GPIOB, 9}},
    {{GPIOB, 8}, {GPIOC, 12}},
};
/* Not yet identified inputs (stock configures them as inputs): PTT/key/encoder push? */
static const pin_t misc[] = {
    {GPIOC, 2}, {GPIOC, 3}, {GPIOC, 4}, {GPIOA, 6}, {GPIOA, 7}, {GPIOB, 2},
};

static volatile int enc_count[ENC_COUNT];
static uint8_t enc_state[ENC_COUNT];
static int8_t enc_sub[ENC_COUNT];

static uint8_t row, row_timer;
static uint16_t scan_acc, scan_prev, keys_down;
static volatile uint16_t evq[EVQ_SIZE];
static volatile uint8_t evq_head, evq_tail;

static inline bool pin_read(const pin_t *p)
{
    return p->port->IDR & (1u << p->pin);
}

static void pin_input_pullup(const pin_t *p)
{
    p->port->MODER &= ~(3u << 2 * p->pin);
    p->port->PUPDR = (p->port->PUPDR & ~(3u << 2 * p->pin)) | (1u << 2 * p->pin);
}

static void row_select(unsigned r)
{
    for (unsigned i = 0; i < 3; i++)
        rows[i].port->BSRR = i == r ? 1u << (rows[i].pin + 16) : 1u << rows[i].pin;
}

void input_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA | RCC_AHB1ENR_GPIOB | RCC_AHB1ENR_GPIOC | RCC_AHB1ENR_GPIOH;
    __dsb();

    for (unsigned i = 0; i < 3; i++) {
        const pin_t *p = &rows[i];
        p->port->BSRR = 1u << p->pin;
        p->port->OTYPER &= ~(1u << p->pin);
        p->port->MODER = (p->port->MODER & ~(3u << 2 * p->pin)) | (1u << 2 * p->pin);
        pin_input_pullup(&cols[i]);
    }
    for (unsigned e = 0; e < ENC_COUNT; e++) {
        pin_input_pullup(&enc_pins[e][0]);
        pin_input_pullup(&enc_pins[e][1]);
        enc_state[e] = pin_read(&enc_pins[e][0]) << 1 | pin_read(&enc_pins[e][1]);
    }
    for (unsigned i = 0; i < sizeof misc / sizeof misc[0]; i++)
        pin_input_pullup(&misc[i]);

    row_select(0);
}

static void push_event(uint16_t ev)
{
    uint8_t next = (evq_head + 1) % EVQ_SIZE;
    if (next != evq_tail) {
        evq[evq_head] = ev;
        evq_head = next;
    }
}

static void encoders_tick(void)
{
    /* Gray-code transition table: +1 / -1 per valid step, 0 for none or invalid. */
    static const int8_t step[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

    for (unsigned e = 0; e < ENC_COUNT; e++) {
        uint8_t s = pin_read(&enc_pins[e][0]) << 1 | pin_read(&enc_pins[e][1]);
        enc_sub[e] += step[enc_state[e] << 2 | s];
        enc_state[e] = s;
        if (s == 3) {                    /* detent (both high): commit a full step */
            if (enc_sub[e] >= 3)
                enc_count[e]++;
            else if (enc_sub[e] <= -3)
                enc_count[e]--;
            enc_sub[e] = 0;
        }
    }
}

static void keys_tick(void)
{
    if (++row_timer < ROW_SETTLE_MS)
        return;
    row_timer = 0;

    for (unsigned c = 0; c < 3; c++)
        if (!pin_read(&cols[c]))
            scan_acc |= (1u << c) << (4 * row);

    row = (row + 1) % 3;
    row_select(row);
    if (row != 0)
        return;

    /* Full scan done: accept a bitmap only if two consecutive scans agree. */
    if (scan_acc == scan_prev)
        keys_down = scan_acc;
    scan_prev = scan_acc;
    scan_acc = 0;
}

/* Bitmap (4 bits per row) -> stock code (column bits | 0x100 << row). */
static uint16_t stock_code(uint16_t bitmap)
{
    uint16_t code = 0;
    for (unsigned r = 0; r < 3; r++) {
        uint16_t cols = (bitmap >> (4 * r)) & 7;
        if (cols)
            code |= cols | (0x100u << r);
    }
    return code;
}

static uint16_t session_code;
static uint32_t held_ms;
static bool long_sent;

static void key_session_tick(void)
{
    uint16_t code = stock_code(keys_down);
    if (code) {
        if (code != session_code) {
            session_code |= code;
            held_ms = 0;
        }
        if (!long_sent && ++held_ms >= LONG_PRESS_MS) {
            push_event(session_code | KEY_LONG);
            long_sent = true;
        }
    } else if (session_code) {
        if (!long_sent)
            push_event(session_code | KEY_SHORT);
        session_code = 0;
        long_sent = false;
        held_ms = 0;
    }
}

void input_tick_1ms(void)
{
    encoders_tick();
    keys_tick();
    key_session_tick();
}

int input_encoder_delta(int enc)
{
    __disable_irq();
    int d = enc_count[enc];
    enc_count[enc] = 0;
    __enable_irq();
    return d;
}

int input_key_event(void)
{
    if (evq_tail == evq_head)
        return -1;
    int ev = evq[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_SIZE;
    return ev;
}

uint16_t input_keys_down(void)
{
    return keys_down;
}

uint32_t input_raw_misc(void)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < sizeof misc / sizeof misc[0]; i++)
        v |= (uint32_t)!pin_read(&misc[i]) << i;    /* 1 = pulled low / active */
    return v;
}
