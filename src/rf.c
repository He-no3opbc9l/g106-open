#include "stm32f4.h"
#include "versaclock.h"
#include "rf.h"

#define SR_CLK      0   /* PA0 */
#define SR_DATA     1   /* PA1 */
#define SR_LATCH_B  2   /* PA2: LPF bank */
#define SR_LATCH_A  5   /* PA5: BPF bank */

#define DAC_CR      REG32(0x40007400)
#define DAC_DHR12R1 REG32(0x40007408)

static int bank_a = -1, bank_b = -1;
static uint16_t dac_val = 0xFFFF;

static void out_pin(unsigned pin)
{
    GPIOA->BSRR = 1u << (pin + 16);
    GPIOA->OTYPER &= ~(1u << pin);
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 2 * pin)) | (1u << 2 * pin);
}

/* ~1 us: 74HC595 setup/pulse-width margins at 3.3 V (back-to-back GPIO writes are only ~12 ns
 * apart at 168 MHz; stock's GPIO_WriteBit calls were naturally slow). */
static void sr_delay(void)
{
    for (volatile int i = 0; i < 40; i++)
        ;
}

static void pulse(unsigned pin)
{
    GPIOA->BSRR = 1u << pin;
    sr_delay();
    GPIOA->BSRR = 1u << (pin + 16);
    sr_delay();
}

static void shift_out(uint8_t v, unsigned latch)
{
    for (int bit = 7; bit >= 0; bit--) {
        GPIOA->BSRR = (v >> bit) & 1 ? 1u << SR_DATA : 1u << (SR_DATA + 16);
        sr_delay();
        pulse(SR_CLK);
    }
    pulse(latch);
}

void rf_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA;
    RCC->APB1ENR |= 1u << 29;               /* DAC */
    __dsb();
    out_pin(SR_CLK);
    out_pin(SR_DATA);
    out_pin(SR_LATCH_A);
    out_pin(SR_LATCH_B);
    GPIOA->MODER |= 3u << 2 * 4;            /* PA4 analog (DAC_OUT1) */
    DAC_CR |= 1u;                           /* EN1 */
}

static int bpf_bank(uint32_t f)
{
    return f < 5500001 ? 3 : f < 11500001 ? 2 : f < 20000001 ? 1 : 0;
}

static int lpf_bank(uint32_t f)
{
    if (f - 1700000u < 2500001u)  return 0;
    if (f - 5000000u < 2600001u)  return 1;
    if (f - 9600000u < 5400001u)  return 2;
    if (f - 16500000u < 6000001u) return 3;
    if (f - 23500000u < 8500001u) return 4;
    return f < 65000000 ? 5 : 6;
}

static uint16_t dac_for(uint32_t f)
{
    static const struct { uint32_t below; uint16_t v; } t[] = {
        {2000001, 0x11D}, {5000001, 0x23A}, {7000001, 0x358}, {10000001, 0x475},
        {14000001, 0x593}, {18000001, 0x6B0}, {21000000, 0x7CD}, {24000001, 0x8EB},
        {28000001, 0xA08}, {50000001, 0xB26},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (f < t[i].below)
            return t[i].v;
    return 0xC43;
}

int rf_set_freq(uint32_t hz)
{
    return rf_set_freq_lo(hz, 0);
}

int rf_set_freq_lo(uint32_t hz, int32_t lo_offset)
{
    int a = bpf_bank(hz), b = lpf_bank(hz);
    if (a != bank_a) {
        shift_out(1u << a, SR_LATCH_A);
        bank_a = a;
    }
    if (b != bank_b) {
        shift_out(1u << b, SR_LATCH_B);
        bank_b = b;
    }
    uint16_t d = dac_for(hz);
    if (d != dac_val) {
        DAC_DHR12R1 = d;
        dac_val = d;
    }
    return vc_set_output(1, 4 * (uint32_t)((int32_t)hz + lo_offset));
}

void rf_fm_path(void)
{
    shift_out(1u << 6, SR_LATCH_B);
    bank_b = 6;
}

void rf_off(void)
{
    shift_out(0, SR_LATCH_A);
    bank_a = -1;
}
