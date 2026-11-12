/* Fonts and icons of the original UI (the boot logo is our own,
 * src/gfx_own.c). The data itself is generated at build time from the
 * stock app image (tools/extract_stock_gfx.py -> build/stock_gfx.c), not kept in the tree.
 * Bitmaps: row-major, 1 bpp, MSB = leftmost pixel. */
#ifndef GFX_DATA_H
#define GFX_DATA_H

#include <stdint.h>

typedef struct {
    uint8_t w, adv, bpl;    /* glyph width, advance, bytes per row */
    uint16_t off;           /* into font bits */
} gfx_glyph_t;

typedef struct {
    uint8_t height, baseline;
    uint8_t first, last;
    const gfx_glyph_t *glyphs;
    const uint8_t *bits;
} gfx_font_t;

typedef struct {
    uint16_t w, h, bpl;
    const uint8_t *bits;
} gfx_bitmap_t;

extern const gfx_font_t font8;      /* main small font, 6x8 */
extern const gfx_font_t font_freq;  /* big frequency digits, 18 px */
extern const gfx_font_t font6;      /* tiny, S-meter scale */
extern const gfx_font_t font16;
extern const gfx_font_t font_fm;    /* huge digits 24x32 (WFM frequency) */

extern const gfx_bitmap_t icon_vfo;     /* top bar, x = 2 */
extern const gfx_bitmap_t icon_x14;     /* speaker */
extern const gfx_bitmap_t icon_x50;     /* "P" (preamp) */
extern const gfx_bitmap_t icon_x63;     /* lock */
extern const gfx_bitmap_t icon_x86;     /* "SPL" */
extern const gfx_bitmap_t icon_cat;     /* CAT link active, x = 75 */
extern const gfx_bitmap_t icon_x109;    /* "QSK" */
extern const gfx_bitmap_t img_boot;     /* 48x48 boot logo, own artwork (src/gfx_own.c) */
extern const gfx_bitmap_t icon_e;       /* inverted "E": empty memory channel */
extern const gfx_bitmap_t icon_tx;      /* top-left TX indicator (icon_vfo is RX) */

#endif
