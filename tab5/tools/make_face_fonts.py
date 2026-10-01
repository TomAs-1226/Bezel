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


def conv(name, card, path, size, ranges, out_c, out_bin):
    rng = make_fonts.available(path, ranges)
    common = ["--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size), "--font", path, "-r", rng]
    if out_bin:
        os.makedirs(out_bin, exist_ok=True)
        make_fonts._run([make_fonts._tool()] + common + ["--format", "bin", "-o", os.path.join(out_bin, card)])
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
