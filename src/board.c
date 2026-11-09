#include "stm32f4.h"
#include "board.h"
#include "system.h"

#define PWR_LATCH_PIN 7     /* PC7 */
#define PWR_KEY_PIN   8     /* PC8 */
#define LED_PIN       10    /* PC10 */

static void gpio_output(GPIO_TypeDef *port, unsigned pin, bool level)
{
    /* Set the level before switching the mode so the pin never glitches. */
    port->BSRR = level ? (1u << pin) : (1u << (pin + 16));
    port->OTYPER &= ~(1u << pin);
    port->MODER = (port->MODER & ~(3u << 2 * pin)) | (1u << 2 * pin);
}

void board_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA | RCC_AHB1ENR_GPIOB | RCC_AHB1ENR_GPIOC | RCC_AHB1ENR_GPIOH;
    __dsb();

    gpio_output(GPIOC, PWR_LATCH_PIN, true);    /* already high from bootloader; keep it */

    GPIOC->MODER &= ~(3u << 2 * PWR_KEY_PIN);
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(3u << 2 * PWR_KEY_PIN)) | (1u << 2 * PWR_KEY_PIN);

    gpio_output(GPIOC, LED_PIN, true);          /* RX/TX LED: 1 = green (RX), 0 = red (TX) */
}

/* RF control lines in the stock RX state (radio_trx_task 0x08029f90). PC5 is driven high
 * by stock only in TX (PA enable?), so keeping it low here keeps the transmitter off. */
void board_rf_rx_state(void)
{
    gpio_output(GPIOC, 5, false);   /* TX enable (?) */
    gpio_output(GPIOB, 1, false);   /* T/R */
    gpio_output(GPIOA, 15, false);  /* TX indicator / control */
    gpio_output(GPIOA, 8, true);    /* RX enable (?) */
}

/* RF/control lines as stock power-off (0x08029d6c): PA15, PC10, PA8 high; PB1, PC5, PC9 low. */
void board_rf_off_state(void)
{
    gpio_output(GPIOA, 15, true);
    gpio_output(GPIOC, LED_PIN, true);
    gpio_output(GPIOA, 8, true);
    gpio_output(GPIOB, 1, false);
    gpio_output(GPIOC, 5, false);
    gpio_output(GPIOC, 9, false);   /* preamp */
}

bool power_key_pressed(void)
{
    return !(GPIOC->IDR & (1u << PWR_KEY_PIN));
}

void led_set(bool on)
{
    GPIOC->BSRR = on ? (1u << LED_PIN) : (1u << (LED_PIN + 16));
}

void led_toggle(void)
{
    GPIOC->ODR ^= 1u << LED_PIN;
}

/* Same sequence as stock (0x08029e96): drop latch, wait for key release, reset.
 * While the key is held the radio stays powered through the key itself. */
void power_off(void)
{
    __disable_irq();
    GPIOC->BSRR = 1u << (PWR_LATCH_PIN + 16);
    while (power_key_pressed())
        ;
    system_reset();
}
