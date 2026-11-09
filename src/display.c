/* LCD wiring (reversed from stock lcd init @0x080259c8 / DMA pump @0x08025abc):
 *   PB3  SPI1_SCK (AF5)      PB5  SPI1_MOSI (AF5)
 *   PB4  CS, active low      PD2  CD/A0: 0 = command, 1 = data
 *   PA12 RST, active low (kept high, as stock does; reset via command 0xE2)
 *   PC11 backlight (stock toggles it with a key and clears it on power-off)
 * SPI mode 0, MSB first, 84 MHz / 8 = 10.5 MHz. */
#include "stm32f4.h"
#include "system.h"
#include "display.h"
#include "font5x7.h"

#define CS_PIN   4      /* PB4 */
#define CD_PIN   2      /* PD2 */
#define RST_PIN  12     /* PA12 */
#define BL_PIN   11     /* PC11 */

static uint8_t fb[LCD_H / 8][LCD_W];

static void pin_out(GPIO_TypeDef *port, unsigned pin, bool level)
{
    port->BSRR = level ? (1u << pin) : (1u << (pin + 16));
    port->OTYPER &= ~(1u << pin);
    port->OSPEEDR |= 2u << 2 * pin;
    port->MODER = (port->MODER & ~(3u << 2 * pin)) | (1u << 2 * pin);
}

static void pin_af(GPIO_TypeDef *port, unsigned pin, unsigned af)
{
    port->AFR[pin / 8] = (port->AFR[pin / 8] & ~(0xFu << 4 * (pin % 8))) | (af << 4 * (pin % 8));
    port->OSPEEDR |= 2u << 2 * pin;
    port->MODER = (port->MODER & ~(3u << 2 * pin)) | (2u << 2 * pin);
}

static inline void cs(bool active)   { GPIOB->BSRR = active ? (1u << (CS_PIN + 16)) : (1u << CS_PIN); }
static inline void cd(bool data)     { GPIOD->BSRR = data ? (1u << CD_PIN) : (1u << (CD_PIN + 16)); }

static void spi_wait_idle(void)
{
    while (!(SPI1->SR & (1u << 1)))     /* TXE */
        ;
    while (SPI1->SR & (1u << 7))        /* BSY */
        ;
}

static void spi_send(const uint8_t *p, unsigned n)
{
    while (n--) {
        while (!(SPI1->SR & (1u << 1)))
            ;
        *(volatile uint8_t *)&SPI1->DR = *p++;
    }
    spi_wait_idle();
}

static void lcd_cmd(const uint8_t *p, unsigned n)
{
    cd(false);
    cs(true);
    spi_send(p, n);
    cs(false);
}

void lcd_backlight(bool on)
{
    GPIOC->BSRR = on ? (1u << BL_PIN) : (1u << (BL_PIN + 16));
}

void lcd_contrast(uint8_t value)
{
    uint8_t c[] = {0x81, value};
    lcd_cmd(c, sizeof c);
}

void lcd_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA | RCC_AHB1ENR_GPIOB | RCC_AHB1ENR_GPIOC | RCC_AHB1ENR_GPIOD;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1;
    __dsb();

    pin_out(GPIOB, CS_PIN, true);
    pin_out(GPIOD, CD_PIN, false);
    pin_out(GPIOA, RST_PIN, true);
    pin_out(GPIOC, BL_PIN, false);             /* switched on from the work params at boot */
    pin_af(GPIOB, 3, 5);
    pin_af(GPIOB, 5, 5);

    SPI1->CR1 = 0;
    SPI1->CR2 = 0;
    SPI1->CR1 = (1u << 15) | (1u << 14)        /* BIDIMODE, BIDIOE: TX only */
              | (1u << 9) | (1u << 8)          /* SSM, SSI */
              | (2u << 3)                      /* BR = fPCLK/8 */
              | (1u << 2);                     /* MSTR */
    SPI1->CR1 |= 1u << 6;                      /* SPE */

    delay_ms(10);
    /* Same sequence and values as stock. */
    static const uint8_t init[] = {
        0xE2,           /* system reset */
        0x2F,           /* power control: all on */
        0x81, 0x38,     /* contrast */
        0xEB,           /* LCD bias */
        0xC7,           /* mapping (mirror X/Y as on G106 glass) */
        0x40,           /* start line 0 */
        0xAF,           /* display on */
    };
    lcd_cmd(&init[0], 1);
    delay_ms(10);
    lcd_cmd(&init[1], 1);
    delay_ms(10);
    lcd_cmd(&init[2], sizeof init - 2);

    lcd_clear();
    lcd_flush();
}

void lcd_flush(void)
{
    for (unsigned page = 0; page < LCD_H / 8; page++) {
        uint8_t addr[] = {0xB0 | page, 0x10, 0x00};  /* page, column 0 */
        lcd_cmd(addr, sizeof addr);
        cd(true);
        cs(true);
        spi_send(fb[page], LCD_W);
        cs(false);
    }
}

void lcd_clear(void)
{
    for (unsigned p = 0; p < LCD_H / 8; p++)
        for (unsigned x = 0; x < LCD_W; x++)
            fb[p][x] = 0;
}

void lcd_pixel(int x, int y, bool on)
{
    if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H)
        return;
    x = LCD_W - 1 - x;      /* mapping 0xC7 puts column 0 on the right edge; stock mirrors too */
    if (on)
        fb[y >> 3][x] |= 1u << (y & 7);
    else
        fb[y >> 3][x] &= ~(1u << (y & 7));
}

void lcd_hline(int x0, int x1, int y, bool on)
{
    for (int x = x0; x <= x1; x++)
        lcd_pixel(x, y, on);
}

void lcd_rect(int x0, int y0, int x1, int y1, bool on)
{
    lcd_hline(x0, x1, y0, on);
    lcd_hline(x0, x1, y1, on);
    for (int y = y0; y <= y1; y++) {
        lcd_pixel(x0, y, on);
        lcd_pixel(x1, y, on);
    }
}

int lcd_text(int x, int y, const char *s, int scale, bool on)
{
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < FONT5X7_FIRST || c > FONT5X7_LAST)
            c = '?';
        const uint8_t *g = font5x7[c - FONT5X7_FIRST];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1u << row))
                    for (int dx = 0; dx < scale; dx++)
                        for (int dy = 0; dy < scale; dy++)
                            lcd_pixel(x + col * scale + dx, y + row * scale + dy, on);
        x += 6 * scale;
    }
    return x;
}

void lcd_fill(int x0, int y0, int x1, int y1, bool on)
{
    for (int y = y0; y <= y1; y++)
        lcd_hline(x0, x1, y, on);
}

static const gfx_glyph_t *glyph(const gfx_font_t *f, char ch)
{
    unsigned c = (unsigned char)ch;
    if (c < f->first || c > f->last)
        return 0;
    const gfx_glyph_t *g = &f->glyphs[c - f->first];
    return g->adv ? g : 0;
}

int lcd_text_width(const gfx_font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const gfx_glyph_t *g = glyph(f, *s);
        w += g ? g->adv : 0;
    }
    return w;
}

int lcd_text_f(const gfx_font_t *f, int x, int y, const char *s, bool inverse)
{
    for (; *s; s++) {
        const gfx_glyph_t *g = glyph(f, *s);
        if (!g)
            continue;
        if (inverse)
            lcd_fill(x, y, x + g->adv - 1, y + f->height - 1, true);
        const uint8_t *bits = f->bits + g->off;
        for (int row = 0; row < f->height; row++)
            for (int col = 0; col < g->w; col++)
                if (bits[row * g->bpl + col / 8] & (0x80 >> (col % 8)))
                    lcd_pixel(x + col, y + row, !inverse);
        x += g->adv;
    }
    return x;
}

void lcd_bitmap(const gfx_bitmap_t *b, int x, int y)
{
    for (int row = 0; row < b->h; row++)
        for (int col = 0; col < b->w; col++)
            if (b->bits[row * b->bpl + col / 8] & (0x80 >> (col % 8)))
                lcd_pixel(x + col, y + row, true);
}
