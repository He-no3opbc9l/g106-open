#include <stdbool.h>
#include "stm32f4.h"
#include "system.h"
#include "i2c.h"

typedef struct {
    volatile uint32_t CR1, CR2, OAR1, OAR2, DR, SR1, SR2, CCR, TRISE, FLTR;
} I2C_TypeDef;

#define SR1_SB    (1u << 0)
#define SR1_ADDR  (1u << 1)
#define SR1_BTF   (1u << 2)
#define SR1_RXNE  (1u << 6)
#define SR1_TXE   (1u << 7)
#define SR1_BERR  (1u << 8)
#define SR1_ARLO  (1u << 9)
#define SR1_AF    (1u << 10)
#define CR1_PE    (1u << 0)
#define CR1_START (1u << 8)
#define CR1_STOP  (1u << 9)
#define CR1_ACK   (1u << 10)
#define CR1_SWRST (1u << 15)

#define TIMEOUT_MS 5

typedef struct {
    I2C_TypeDef *i2c;
    uint8_t scl, sda;       /* GPIOB pins */
    uint8_t rcc_bit;        /* APB1ENR */
} bus_t;

static const bus_t buses[3] = {
    [I2C_BUS1] = {(I2C_TypeDef *)0x40005400, 6, 7, 21},
    [I2C_BUS2] = {(I2C_TypeDef *)0x40005800, 10, 11, 22},
};

static void set_mode(unsigned pin, unsigned mode)
{
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 2 * pin)) | (mode << 2 * pin);
}

/* Clock out a stuck slave: up to 9 SCL pulses with SDA released, then a STOP. */
static void bus_recover(const bus_t *b)
{
    GPIOB->OTYPER |= (1u << b->scl) | (1u << b->sda);
    GPIOB->BSRR = (1u << b->scl) | (1u << b->sda);
    set_mode(b->scl, 1);
    set_mode(b->sda, 1);
    for (int i = 0; i < 9 && !(GPIOB->IDR & (1u << b->sda)); i++) {
        GPIOB->BSRR = 1u << (b->scl + 16);
        for (volatile int d = 0; d < 400; d++)
            ;
        GPIOB->BSRR = 1u << b->scl;
        for (volatile int d = 0; d < 400; d++)
            ;
    }
    GPIOB->BSRR = 1u << (b->sda + 16);
    for (volatile int d = 0; d < 400; d++)
        ;
    GPIOB->BSRR = 1u << b->sda;
}

void i2c_init(int bus)
{
    const bus_t *b = &buses[bus];
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOB;
    RCC->APB1ENR |= 1u << b->rcc_bit;
    __dsb();

    bus_recover(b);
    for (unsigned pin = b->scl, n = 0; n < 2; n++, pin = b->sda) {
        GPIOB->AFR[pin / 8] = (GPIOB->AFR[pin / 8] & ~(0xFu << 4 * (pin % 8))) | (4u << 4 * (pin % 8));
        GPIOB->OTYPER |= 1u << pin;
        set_mode(pin, 2);
    }

    I2C_TypeDef *i2c = b->i2c;
    i2c->CR1 = CR1_SWRST;
    i2c->CR1 = 0;
    i2c->CR2 = APB1_HZ / 1000000;                   /* 42 */
    i2c->CCR = APB1_HZ / (2 * 100000);              /* 210: standard mode 100 kHz */
    i2c->TRISE = APB1_HZ / 1000000 + 1;
    i2c->CR1 = CR1_PE;
}

static bool wait_flag(I2C_TypeDef *i2c, uint32_t flag)
{
    uint32_t t0 = millis();
    while (!(i2c->SR1 & flag)) {
        if (i2c->SR1 & (SR1_AF | SR1_BERR | SR1_ARLO))
            return false;
        if (millis() - t0 > TIMEOUT_MS)
            return false;
    }
    return true;
}

static int fail(int bus)
{
    I2C_TypeDef *i2c = buses[bus].i2c;
    i2c->CR1 |= CR1_STOP;
    i2c->SR1 = 0;
    if (i2c->SR2 & (1u << 1))           /* still BUSY: reset the peripheral */
        i2c_init(bus);
    return -1;
}

static bool start(I2C_TypeDef *i2c, uint8_t addr8)
{
    i2c->CR1 |= CR1_START;
    if (!wait_flag(i2c, SR1_SB))
        return false;
    i2c->DR = addr8;
    if (!wait_flag(i2c, SR1_ADDR))
        return false;
    (void)i2c->SR2;                     /* clear ADDR */
    return true;
}

int i2c_probe(int bus, uint8_t addr)
{
    I2C_TypeDef *i2c = buses[bus].i2c;
    if (!start(i2c, addr << 1))
        return fail(bus);
    i2c->CR1 |= CR1_STOP;
    return 0;
}

static bool send(I2C_TypeDef *i2c, const uint8_t *p, unsigned n)
{
    while (n--) {
        if (!wait_flag(i2c, SR1_TXE))
            return false;
        i2c->DR = *p++;
    }
    return wait_flag(i2c, SR1_BTF);
}

int i2c_write(int bus, uint8_t addr, const uint8_t *data, unsigned len)
{
    I2C_TypeDef *i2c = buses[bus].i2c;
    if (!start(i2c, addr << 1) || !send(i2c, data, len))
        return fail(bus);
    i2c->CR1 |= CR1_STOP;
    return 0;
}

int i2c_write_read(int bus, uint8_t addr, const uint8_t *w, unsigned wlen, uint8_t *r, unsigned rlen)
{
    I2C_TypeDef *i2c = buses[bus].i2c;
    if (wlen && (!start(i2c, addr << 1) || !send(i2c, w, wlen)))
        return fail(bus);
    if (!rlen) {
        i2c->CR1 |= CR1_STOP;
        return 0;
    }

    i2c->CR1 |= CR1_ACK;
    i2c->CR1 |= CR1_START;
    if (!wait_flag(i2c, SR1_SB))
        return fail(bus);
    i2c->DR = (addr << 1) | 1;
    if (!wait_flag(i2c, SR1_ADDR))
        return fail(bus);
    if (rlen == 1)
        i2c->CR1 &= ~CR1_ACK;
    (void)i2c->SR2;
    if (rlen == 1)
        i2c->CR1 |= CR1_STOP;

    while (rlen) {
        if (rlen == 1) {
            /* Last byte is now being received: NACK it and STOP afterwards. */
            i2c->CR1 &= ~CR1_ACK;
            i2c->CR1 |= CR1_STOP;
        }
        if (!wait_flag(i2c, SR1_RXNE))
            return fail(bus);
        *r++ = i2c->DR;
        rlen--;
    }
    return 0;
}
