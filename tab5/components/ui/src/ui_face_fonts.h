/* Home mode's clock-face type, loaded from the microSD card instead of built in (ui_face_fonts.c).
 *
 * The four faces (poster, flip, thin, match) are 760 KB of glyph bitmaps. Built in, that is 760 KB of PSRAM
 * for good: the program runs from PSRAM, and so does its read-only data. On the card they cost nothing until a
 * face that sets its time in one is on screen, and then only that face's font. Made by tools/make_face_fonts.py,
 * copied to <sd>/CATOS/FONTS/ by tab5_dev.py's "deploy".
 *
 * A label never holds a font that can go away: it is bound (ui_face_font_bind) and the module hands it the card's
 * font when that has loaded, and the stand-in, a bezel face that is always built in, whenever it hasn't, or
 * isn't on the card, or has been given back. So a missing file shows a smaller clock, never an empty one. */
#pragma once
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_FF_POSTER,   /* 440 px, the poster's numerals */
    UI_FF_FLIP,     /* 330 px, the split-flap cards */
    UI_FF_THIN,     /* 200 px, the display line of the sky, moon, orrery, tree, match and night faces */
    UI_FF_MATCH,    /* 210 px, the match face's label */
    UI_FF_COUNT
} ui_ff_t;
#define UI_FF_BIT(f) (1u << (f))

/* The font as it is now: the card's once it has loaded, the stand-in until then. Never NULL. It changes only in
 * ui_face_fonts_tick (UI thread), so a face that measures with it measures again when that reports a change. */
const lv_font_t *ui_face_font(ui_ff_t which);
/* `label` shows this font: given it now, and given whatever it becomes. The label must outlive the module's
 * use of it (the faces' objects are built once and stay). At most 16 per font. */
void ui_face_font_bind(lv_obj_t *label, ui_ff_t which);

/* The fonts the face on screen (and the one arriving) need, as UI_FF_BIT()s: any that are not held start loading
 * on the covers' worker (never on the UI thread), any held that are not in `mask` are given back. */
void ui_face_fonts_want(uint32_t mask);
/* Every font in `mask` has settled: loaded, or not coming (no file, no memory). A face waits for this before it
 * shows, so it never draws in the stand-in and then jumps. */
bool ui_face_fonts_ready(uint32_t mask);
/* The UI thread's turn, ~10 Hz and while a face waits: takes what the worker finished. true when a font changed
 * hands, and the faces that use it have to lay their text out again. */
bool ui_face_fonts_tick(void);
/* All given back (home mode left). */
void ui_face_fonts_release(void);
/* Nothing held and nothing loading. */
bool ui_face_fonts_released(void);
/* "hm fonts": one line each, for the dev console. */
void ui_face_fonts_dev(void);
