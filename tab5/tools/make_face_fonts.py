#!/usr/bin/env python3
"""Bake home mode's clock-face type: a few digit faces far bigger than Bezel's type scale.

Bezel's own faces (tools/make_fonts.py, bz_fonts.h) stop at the 118 px clock. The clock faces set the time as
a poster, on split-flap cards and as a thin display line, read from across a room, in static instances of
Google Sans Flex cut from its axes (weight and width) for the job. Only the glyphs a clock face prints are
baked, at 4 bpp (the levels past sixteen buy nothing at this size), uncompressed (drawn once a minute, but
drawn fast).

Each face font is written in two forms from the one conversion:

  sdcard/CATOS/FONTS/<NAME>.BIN       LVGL binary fonts, what the firmware uses: ui_face_fonts.c loads them from
                                      the microSD card when a face that needs one shows (docs/catalyst-os.md,
                                      "Face fonts"). Copy them over the cable: tab5_dev.py COM9 "deploy".
  components/ui/fonts_c/hm_font_*.c   the same fonts as C arrays. NOT COMPILED: it is outside the ui component's
                                      source directory on purpose, because 760 KB of glyph bitmaps in the image
                                      are 760 KB of PSRAM taken for good (the program runs from PSRAM). Kept so a
                                      build that wants them baked in can, and as the readable reference.

Usage:  python tools/make_face_fonts.py [--fonts DIR] [--no-c] [--no-bin] [--out-c DIR] [--out-bin DIR]
        (DIR as make_fonts.py's; missing instances fetched)
Needs lv_font_conv (npm) and fontTools (pip)."""
import argparse
import os
import re
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_C = os.path.join(ROOT, "components", "ui", "fonts_c")
OUT_BIN = os.path.join(ROOT, "sdcard", "CATOS", "FONTS")
sys.path.insert(0, HERE)
import make_fonts  # noqa: E402  (its fetch, feature freezer and converter)

# file -> Google Fonts css2 spec (Google Sans Flex: wdth 25..151, wght 1..1000)
SOURCES = {
    "hm_w800c.ttf": "Google+Sans+Flex:wdth,wght@75,800",   # the poster: heavy and a little condensed
    "hm_w700.ttf": "Google+Sans+Flex:wdth,wght@100,700",   # the flip cards and the match
    "hm_w150.ttf": "Google+Sans+Flex:wdth,wght@100,150",   # the thin display line
}

TIME = "0x30-0x3A"                      # 0-9 :
MATCH = TIME + ",0x20,0x2D,0x45,0x46,0x4D,0x51,0x53,0x3F"  # space - E F M Q S ?

# (symbol, card file, source, size, ranges). The card file is upper-case 8.3 like the rest of CATOS (ui_storage.h),
# and ui_face_fonts.c has the same four names.
FACES = [
    ("hm_font_poster", "POSTER.BIN", "hm_w800c.ttf", 440, TIME),
    ("hm_font_flip", "FLIP.BIN", "hm_w700.ttf", 330, TIME),
    ("hm_font_thin", "THIN.BIN", "hm_w150.ttf", 200, TIME),
    ("hm_font_match", "MATCH.BIN", "hm_w700.ttf", 210, MATCH),
]

NOT_COMPILED = """/* NOT COMPILED. The firmware loads this face font from the microSD card (CATOS/FONTS/{card}, ui_face_fonts.c):
 * built in, its glyph bitmaps would sit in PSRAM for good, because the program runs from PSRAM. This file is
 * outside components/ui/src so the build's source glob never sees it; tools/make_face_fonts.py writes it next
 * to the .BIN from the same conversion. */
"""


def fetch(fonts_dir):
    os.makedirs(fonts_dir, exist_ok=True)
    for name, spec in SOURCES.items():
        path = os.path.join(fonts_dir, name)
        if os.path.exists(path):
            continue
        req = urllib.request.Request(f"https://fonts.googleapis.com/css2?family={spec}", headers={"User-Agent": "Wget/1.21"})
        css = urllib.request.urlopen(req).read().decode()
        url = re.search(r"https://[^)]*\.ttf", css).group(0)
        print("fetch", name)
        urllib.request.urlretrieve(url, path)


def _bin_sections(d):
    secs, off = {}, 0
    while off < len(d):
        ln, tag = struct.unpack_from("<I4s", d, off)
        secs[tag] = (off, ln)
        off += ln
    return secs


def _bin_glyphs(d):
    """A binary font's glyphs as (advance, x, y, w, h, bitmap bytes), in glyph order, and its three field widths.
    Glyph 0 is the empty one (it has no data in the file)."""
    secs = _bin_sections(d)
    ho = secs[b"head"][0] + 8
    loc, _, _, bpp, xy, wh, adv, comp = struct.unpack_from("<8B", d, ho + 26)
    assert comp == 0, "compressed"
    lo = secs[b"loca"][0]
    go, gl = secs[b"glyf"]
    cnt = struct.unpack_from("<I", d, lo + 8)[0]
    offs = struct.unpack_from(("<%dH" if loc == 0 else "<%dI") % cnt, d, lo + 12)
    bits = lambda p: (d[p >> 3] >> (7 - (p & 7))) & 1
    out = []
    for i in range(cnt):
        end = go + (offs[i + 1] if i + 1 < cnt else gl)
        if go + offs[i] >= end:
            out.append((0, 0, 0, 0, 0, b""))
            continue
        pos = (go + offs[i]) * 8

        def rd(n, signed=False):
            nonlocal pos
            v = 0
            for _ in range(n):
                v = (v << 1) | bits(pos)
                pos += 1
            return v - (1 << n) if signed and v >> (n - 1) else v

        a, x, y, w, h = rd(adv), rd(xy, True), rd(xy, True), rd(wh), rd(wh)
        bm = bytearray(rd(8) for _ in range((w * h * bpp + 7) // 8))
        if bm and w * h * bpp % 8:
            bm[-1] &= 0xFF << (8 - w * h * bpp % 8) & 0xFF  # the bits past the last pixel are the next glyph's
        out.append((a, x, y, w, h, bytes(bm)))
    return out, (adv, xy, wh)


def align_bin(path):
    """Rewrites a binary font so LVGL's loader can read it fast.

    lv_font_conv packs each glyph's header (advance, offsets, size) in as few bits as will do, 39 to 43 of them, so
    a glyph's bitmap starts mid-byte, and LVGL's loader (lv_binfont_loader.c) then reads it a byte at a time through
    a bit iterator: 358 KB of the poster took 400-600 ms of the covers' worker. When the header is a whole number
    of bytes it reads each bitmap with one call. So: 16 bits for every field (10 bytes a glyph), the bitmaps after
    them unchanged, loca and the section lengths to match. Read back and compared with the original before it is
    written."""
    d = open(path, "rb").read()
    glyphs, _ = _bin_glyphs(d)
    secs = _bin_sections(d)
    glyf, offs = bytearray(), []
    for a, x, y, w, h, bm in glyphs:
        offs.append(8 + len(glyf))
        if bm or w or h or a:
            glyf += struct.pack(">HhhHH", a, x, y, w, h) + bm
    loc = 0 if max(offs) <= 0xFFFF else 1
    loca = struct.pack("<I", len(glyphs)) + struct.pack(("<%dH" if loc == 0 else "<%dI") % len(glyphs), *offs)
    loca += b"\0" * (-len(loca) % 4)
    head = bytearray(d[secs[b"head"][0]:sum(secs[b"head"])])
    head[8 + 26] = loc          # index_to_loc_format
    head[8 + 30:8 + 33] = bytes((16, 16, 16))  # xy_bits, wh_bits, advance_width_bits
    out = bytearray(head)
    for tag in secs:
        if tag == b"head":
            continue
        o, ln = secs[tag]
        if tag == b"loca":
            out += struct.pack("<I4s", 8 + len(loca), b"loca") + loca
        elif tag == b"glyf":
            out += struct.pack("<I4s", 8 + len(glyf), b"glyf") + glyf
        else:
            out += d[o:o + ln]
    back, widths = _bin_glyphs(bytes(out))
    assert widths == (16, 16, 16) and back == glyphs, "aligned font differs from the original in " + path
    open(path, "wb").write(out)


def conv(name, card, path, size, ranges, out_c, out_bin):
    rng = make_fonts.available(path, ranges)
    common = ["--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size), "--font", path, "-r", rng]
    if out_bin:
        os.makedirs(out_bin, exist_ok=True)
        make_fonts._run([make_fonts._tool()] + common + ["--format", "bin", "-o", os.path.join(out_bin, card)])
        align_bin(os.path.join(out_bin, card))
    if out_c:
        os.makedirs(out_c, exist_ok=True)
        out = os.path.join(out_c, name + ".c")
        make_fonts._run([make_fonts._tool()] + common + ["--format", "lvgl", "--lv-font-name", name, "--lv-include",
                                                         "lvgl.h", "-o", out])
        src = open(out, encoding="utf-8").read()
        src = re.sub(r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "[^"]*"\n#endif', '#include "lvgl.h"', src)
        open(out, "w", encoding="utf-8", newline="\n").write(NOT_COMPILED.format(card=card) + src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", default=os.environ.get("BZ_FONT_DIR", "/opt/fonts"))
    ap.add_argument("--out-c", default=OUT_C, help="where the C form goes (not compiled)")
    ap.add_argument("--out-bin", default=OUT_BIN, help="where the card files go")
    ap.add_argument("--no-c", action="store_true")
    ap.add_argument("--no-bin", action="store_true")
    a = ap.parse_args()
    fetch(a.fonts)
    for name, card, src, size, ranges in FACES:
        print("font", name, "->", card)
        conv(name, card, os.path.join(a.fonts, src), size, ranges, None if a.no_c else a.out_c,
             None if a.no_bin else a.out_bin)


if __name__ == "__main__":
    main()
