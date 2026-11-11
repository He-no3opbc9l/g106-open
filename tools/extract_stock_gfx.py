#!/usr/bin/env python3
"""Extract the UI fonts and icons from the stock G106 app image (emWin structures) and emit
them as C, so the open firmware can draw the original screen without shipping stock data in
the source tree.

usage: python3 -I extract_stock_gfx.py STOCK_APP.bin OUT.c        generate C
       python3 -I extract_stock_gfx.py STOCK_APP.bin --preview     ASCII preview

Formats (emWin 5.x):
  GUI_FONT        +24 YSize YDist XMag YMag, +28 p (PROP or MONO), +32 Baseline LHeight CHeight
  GUI_FONT_PROP   {u16 First, u16 Last, GUI_CHARINFO *pa, GUI_FONT_PROP *pNext}
  GUI_CHARINFO    {u8 XSize, u8 XDist, u8 BytesPerLine, u8 pad, u8 *pData}
  GUI_FONT_MONO   {u8 *pData, u8 *pTransData, void *pTrans, u16 First, u16 Last,
                   u8 XSize, u8 XDist, u8 BytesPerLine}
  GUI_BITMAP      {u16 XSize, YSize, BytesPerLine, BitsPerPixel, u8 *pData, pPal, pMethods}
  MONO with First = 0xFFFF: chars mapped through pTrans (GUI_FONT_TRANSINFO, see read_font)
Pixel data is row-major, 1 bpp, MSB = leftmost pixel.
"""
import struct
import sys

BASE = 0x08020000
GUI_DISPCHAR_MONO = 0x08023031      # stock GUI_DispChar for mono fonts (0x080231bd = PROP)

FONTS = {                            # address -> name
    0x0804CEC4: "font8",            # main small font
    0x0804C468: "font_freq",        # big frequency digits
    0x0804C7AC: "font6",            # tiny (S-meter scale)
    0x0804DAF4: "font16",
    0x0804E1A0: "font_fm",          # huge digits (WFM frequency)
}
ICONS = {
    0x08039350: "icon_vfo",         # top bar (2,1)
    0x080397C8: "icon_x14",         # (14,1)
    0x08039148: "icon_x50",         # (50,1)
    0x08039120: "icon_x63",         # (63,1)
    0x080392F8: "icon_x86",         # (86,1)
    0x08039180: "icon_x109",        # (109,1)
    0x080397A0: "img_poweroff",     # 128x64 power-off screen
    0x080392C0: "img_boot",         # 48x48 boot logo @(39,0)
    0x08039320: "icon_e",           # inverted "E": empty memory channel (MW dialog)
    0x08039380: "icon_tx",          # top-left TX indicator (RX is icon_vfo)
    0x080397F0: "icon_cat",         # (75,1) CAT link active
}


class Img:
    def __init__(self, data):
        self.d = data

    def u8(self, a):
        return self.d[a - BASE]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a - BASE)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a - BASE)[0]

    def bytes(self, a, n):
        return self.d[a - BASE:a - BASE + n]


def read_font(img, addr):
    ysize, ydist = img.u8(addr + 24), img.u8(addr + 25)
    p = img.u32(addr + 28)
    baseline = img.u8(addr + 32)
    glyphs = {}
    if img.u32(addr) != GUI_DISPCHAR_MONO:
        while p:
            first, last, pa, nxt = img.u16(p), img.u16(p + 2), img.u32(p + 4), img.u32(p + 8)
            for c in range(first, last + 1):
                ci = pa + 8 * (c - first)
                xs, xd, bpl, pdat = img.u8(ci), img.u8(ci + 1), img.u8(ci + 2), img.u32(ci + 4)
                data = img.bytes(pdat, bpl * ysize) if pdat else bytes(bpl * ysize)
                glyphs[c] = (xs, xd, bpl, data)
            p = nxt
    else:
        # MONO: ASCII range First..Last is contiguous in pData (extended chars ignored)
        pdat, first, last = img.u32(p), img.u16(p + 12), img.u16(p + 14)
        xs, xd, bpl = img.u8(p + 16), img.u8(p + 17), img.u8(p + 18)
        if first == 0xFFFF:
            # no direct range: GUI_FONT_TRANSINFO {u16 First, u16 Last, pList} maps each char
            # to a glyph index via GUI_FONT_TRANSLIST {i16 c0, i16 c1} (c0 = -1: no glyph)
            pt = img.u32(p + 8)
            first, last, plist = img.u16(pt), img.u16(pt + 2), img.u32(pt + 4)
            for c in range(first, last + 1):
                idx = img.u16(plist + 4 * (c - first))
                if idx != 0xFFFF:
                    glyphs[c] = (xs, xd, bpl, img.bytes(pdat + idx * bpl * ysize, bpl * ysize))
            return {"ysize": ysize, "ydist": ydist, "baseline": baseline, "glyphs": glyphs}
        for c in range(first, last + 1):
            glyphs[c] = (xs, xd, bpl, img.bytes(pdat + (c - first) * bpl * ysize, bpl * ysize))
    return {"ysize": ysize, "ydist": ydist, "baseline": baseline, "glyphs": glyphs}


def read_bitmap(img, addr):
    xs, ys, bpl, bpp = img.u16(addr), img.u16(addr + 2), img.u16(addr + 4), img.u16(addr + 6)
    assert bpp == 1, (hex(addr), bpp)
    return {"w": xs, "h": ys, "bpl": bpl, "data": img.bytes(img.u32(addr + 8), bpl * ys)}


def rows(w, h, bpl, data):
    for y in range(h):
        yield "".join("#" if data[y * bpl + x // 8] & (0x80 >> (x % 8)) else "." for x in range(w))


def preview(img):
    for addr, name in FONTS.items():
        f = read_font(img, addr)
        cs = sorted(f["glyphs"])
        print(f"== {name} @{addr:#x}: {len(cs)} glyphs {cs[0]:#x}..{cs[-1]:#x}, "
              f"ysize {f['ysize']} baseline {f['baseline']}")
        sample = [c for c in map(ord, "0123456789.:ABS+-LUD") if c in f["glyphs"]]
        lines = [""] * f["ysize"]
        for c in sample:
            xs, xd, bpl, data = f["glyphs"][c]
            for y, r in enumerate(rows(xs, f["ysize"], bpl, data)):
                lines[y] += r + "." * (xd - xs) + " "
        print("\n".join(lines))
    for addr, name in ICONS.items():
        b = read_bitmap(img, addr)
        if b["w"] > 32:
            continue
        print(f"== {name} {b['w']}x{b['h']}")
        print("\n".join(rows(b["w"], b["h"], b["bpl"], b["data"])))


def c_bytes(data):
    return ", ".join(f"0x{b:02X}" for b in data)


def generate(img, out):
    o = ["/* GENERATED by tools/extract_stock_gfx.py from the stock G106 app image. Do not edit,",
         " * do not commit: contains graphics from the original firmware. */",
         '#include "gfx_data.h"', ""]
    for addr, name in FONTS.items():
        f = read_font(img, addr)
        cs = sorted(f["glyphs"])
        first, last = cs[0], cs[-1]
        blob, entries = bytearray(), []
        for c in range(first, last + 1):
            if c in f["glyphs"]:
                xs, xd, bpl, data = f["glyphs"][c]
                entries.append(f"{{{xs}, {xd}, {bpl}, {len(blob)}}}")
                blob += data
            else:
                entries.append("{0, 0, 0, 0}")
        assert len(blob) < 65536
        o.append(f"static const uint8_t {name}_bits[] = {{{c_bytes(blob)}}};")
        o.append(f"static const gfx_glyph_t {name}_glyphs[] = {{{', '.join(entries)}}};")
        o.append(f"const gfx_font_t {name} = {{{f['ysize']}, {f['baseline']}, {first}, {last}, "
                 f"{name}_glyphs, {name}_bits}};")
        o.append("")
    for addr, name in ICONS.items():
        b = read_bitmap(img, addr)
        o.append(f"static const uint8_t {name}_bits[] = {{{c_bytes(b['data'])}}};")
        o.append(f"const gfx_bitmap_t {name} = {{{b['w']}, {b['h']}, {b['bpl']}, {name}_bits}};")
        o.append("")
    open(out, "w").write("\n".join(o))


def main():
    img = Img(open(sys.argv[1], "rb").read())
    if sys.argv[2] == "--preview":
        preview(img)
    else:
        generate(img, sys.argv[2])


if __name__ == "__main__":
    main()
