/* 128x64 monochrome LCD, UC1609-family command set, SPI1 TX-only.
 * Framebuffer is page-organised like the controller: 8 pages x 128 columns,
 * bit 0 of each byte is the top row of the page, X is stored mirrored (as stock). */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "gfx_data.h"

#define LCD_W 128
#define LCD_H 64

void lcd_init(void);
void lcd_flush(void);
void lcd_clear(void);
void lcd_backlight(bool on);
void lcd_contrast(uint8_t value);
void lcd_pixel(int x, int y, bool on);
void lcd_hline(int x0, int x1, int y, bool on);
void lcd_rect(int x0, int y0, int x1, int y1, bool on);
/* 5x7 font, 6 px advance; scale 1..4. Returns x after the text. */
int lcd_text(int x, int y, const char *s, int scale, bool on);
void lcd_fill(int x0, int y0, int x1, int y1, bool on);
/* Original-UI fonts: (x, y) = top-left of the character cell, as emWin GUI_DispStringAt.
 * inverse: cell filled, glyph cleared. Returns x after the text. */
int lcd_text_f(const gfx_font_t *f, int x, int y, const char *s, bool inverse);
int lcd_text_width(const gfx_font_t *f, const char *s);
void lcd_bitmap(const gfx_bitmap_t *b, int x, int y);

#endif
