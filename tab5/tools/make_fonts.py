#!/usr/bin/env python3
"""Bake Bezel's type faces into LVGL bitmap fonts.

Bezel's faces are Google Sans Flex (display, titles, body), Google Sans Code (the lowercase mono labels)
and Material Symbols Rounded (icons: outline at wght 400, filled at wght 500 for a selected item).
LVGL can't drive variable axes or OpenType features, so this takes static instances from Google Fonts,
freezes tabular figures into the numeric faces (every changing number is tabular, so it never shifts
width as it updates) and converts each face at the exact pixel sizes Bezel's type scale uses.

Usage:  python tools/make_fonts.py [--fonts DIR]

DIR holds the downloaded .ttf instances (fetched from fonts.googleapis.com css2 with the axes below);
the script fetches any that are missing. Needs `lv_font_conv` (npm) and `pyftfeatfreeze`
(pip opentype-feature-freezer). Output: components/bezel/fonts/*.c and include/bz_fonts.h.
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "components", "bezel", "fonts")
HDR = os.path.join(ROOT, "components", "bezel", "include", "bz_fonts.h")

# file -> Google Fonts css2 family spec
SOURCES = {
    "flex400.ttf": "Google+Sans+Flex:wght@400",
    "flex600.ttf": "Google+Sans+Flex:wght@600",
    "flex_display300.ttf": "Google+Sans+Flex:wdth,wght@112,300",
    "flex_clock250.ttf": "Google+Sans+Flex:wdth,wght@118,250",
    "code400.ttf": "Google+Sans+Code:wght@400",
    "sym_outline.ttf": "Material+Symbols+Rounded:opsz,wght,FILL,GRAD@40,400,0,0",
    "sym_fill.ttf": "Material+Symbols+Rounded:opsz,wght,FILL,GRAD@40,500,1,0",
}

ASCII = "0x20-0x7E"
# typographic extras the UI prints: · — – ° ± µ × → ← ↑ ↓ … ● ◆ ■ ○ ▲ Ω
EXTRA = "0xB7,0x2014,0x2013,0xB0,0xB1,0xB5,0xD7,0x2192,0x2190,0x2191,0x2193,0x2026,0x25CF,0x25C6,0x25A0,0x25CB,0x25B2,0x3A9"
DIGITS = "0x20,0x2B-0x3A,0x25,0xB0,0x2014"  # space + - . / 0-9 : % ° —

# (symbol name, source file, size, ranges, frozen tnum?)
TEXT_FONTS = [
    ("bz_font_display_76", "flex_display300.ttf", 76, DIGITS + ",0x41-0x5A,0x61-0x7A", True),
    ("bz_font_display_46", "flex_display300.ttf", 46, ASCII + "," + EXTRA, True),
    ("bz_font_clock_118", "flex_clock250.ttf", 118, DIGITS, True),
    ("bz_font_title_36", "flex600.ttf", 36, ASCII + "," + EXTRA, True),
    ("bz_font_name_24", "flex600.ttf", 24, ASCII + "," + EXTRA, True),
    ("bz_font_body_20", "flex400.ttf", 20, ASCII + "," + EXTRA, True),
    ("bz_font_body_17", "flex400.ttf", 17, ASCII + "," + EXTRA, True),
    ("bz_font_mono_16", "code400.ttf", 16, ASCII + "," + EXTRA, False),
    ("bz_font_mono_13", "code400.ttf", 13, ASCII + "," + EXTRA, False),
]

ICONS = """
monitor_heart memory developer_board bolt electric_bolt precision_manufacturing warning error info
check_circle cancel apps checklist tune stadium graphic_eq straighten photo_camera cable lan
receipt_long settings battery_full battery_5_bar battery_3_bar battery_1_bar battery_alert
battery_charging_full wifi wifi_off usb sensors speed thermostat timer sync link link_off videocam
radar hub power play_arrow pause stop arrow_back arrow_forward close add remove refresh light_mode
dark_mode brightness_6 volume_up rotate_right explore mic sd_card router
bug_report construction handyman build smart_toy sports_esports thermometer device_thermostat
earthquake bar_chart show_chart timeline motion_photos_on camera auto_mode flag emergency_home
visibility lightbulb health_and_safety pin_drop my_location adjust square circle change_history
radio_button_unchecked more_horiz expand_more expand_less chevron_right keyboard_arrow_down
data_usage hourglass_top downloading save target filter_center_focus frame_inspect
""".split()

ICON_SIZES = [("outline", "sym_outline.ttf", [24, 32, 40]), ("fill", "sym_fill.ttf", [32])]


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


def tnum(src, fonts_dir):
    dst = os.path.join(fonts_dir, src.replace(".ttf", "_tnum.ttf"))
    if not os.path.exists(dst):
        subprocess.run(["pyftfeatfreeze", "-f", "tnum", os.path.join(fonts_dir, src), dst], check=True)
    return dst


def available(path, ranges):
    """Keeps only the codepoints `path` actually has: the faces differ in their symbol coverage."""
    from fontTools.ttLib import TTFont
    have = set(TTFont(path).getBestCmap())
    keep = []
    for part in ranges.split(","):
        lo, _, hi = part.partition("-")
        lo = int(lo, 16)
        hi = int(hi, 16) if hi else lo
        keep += [cp for cp in range(lo, hi + 1) if cp in have]
    return ",".join(hex(cp) for cp in keep)


def conv(name, path, size, ranges, extra_symbols=None):
    out = os.path.join(OUT, name + ".c")
    ranges = available(path, ranges)
    args = ["lv_font_conv", "--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size), "--format", "lvgl",
            "--lv-font-name", name, "--lv-include", "lvgl.h", "-o", out, "--font", path]
    if ranges:
        args += ["-r", ranges]
    if extra_symbols:
        args += ["-r", extra_symbols]
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    # lv_font_conv writes `#include "lvgl/lvgl.h"`-style guards; normalise to the component's include
    src = open(out, encoding="utf-8").read()
    src = re.sub(r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "[^"]*"\n#endif', '#include "lvgl.h"', src)
    open(out, "w", encoding="utf-8", newline="\n").write(src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", default=os.environ.get("BZ_FONT_DIR", "/opt/fonts"))
    a = ap.parse_args()
    fetch(a.fonts)
    os.makedirs(OUT, exist_ok=True)

    from fontTools.ttLib import TTFont
    cmap = TTFont(os.path.join(a.fonts, "sym_outline.ttf")).getBestCmap()
    by_name = {}
    for cp, g in cmap.items():
        by_name.setdefault(g, cp)
    fill_cmap = TTFont(os.path.join(a.fonts, "sym_fill.ttf")).getBestCmap()
    icons = []
    for n in ICONS:
        if n not in by_name:
            print("warning: no icon", n, file=sys.stderr)
            continue
        icons.append((n, by_name[n]))
    icon_ranges = ",".join(hex(cp) for _, cp in icons)

    for name, src, size, ranges, frozen in TEXT_FONTS:
        path = tnum(src, a.fonts) if frozen else os.path.join(a.fonts, src)
        print("font", name)
        conv(name, path, size, ranges)
    for style, src, sizes in ICON_SIZES:
        present = icon_ranges if style == "outline" else ",".join(hex(cp) for _, cp in icons if cp in fill_cmap)
        for s in sizes:
            name = f"bz_icons_{style}_{s}"
            print("icons", name)
            conv(name, os.path.join(a.fonts, src), s, present)

    with open(HDR, "w", encoding="utf-8", newline="\n") as h:
        h.write("/* Generated by tools/make_fonts.py — Bezel's faces baked for LVGL. Do not edit. */\n")
        h.write("#pragma once\n#include \"lvgl.h\"\n\n")
        for name, *_ in TEXT_FONTS:
            h.write(f"LV_FONT_DECLARE({name})\n")
        for style, _, sizes in ICON_SIZES:
            for s in sizes:
                h.write(f"LV_FONT_DECLARE(bz_icons_{style}_{s})\n")
        h.write("\n/* Material Symbols Rounded codepoints, as UTF-8 */\n")
        for n, cp in icons:
            utf8 = "".join(f"\\x{b:02x}" for b in chr(cp).encode("utf-8"))
            h.write(f"#define BZ_I_{n.upper()} \"{utf8}\"\n")
    print("wrote", HDR)


if __name__ == "__main__":
    main()
