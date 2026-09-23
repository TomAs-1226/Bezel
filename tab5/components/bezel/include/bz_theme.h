/* bz_theme — Bezel's tokens as shared LVGL styles, and the small widget kit built on them.
 *
 * Every colour and face is a shared style keyed by role, so switching tone (dark ↔ light) or calm
 * rewrites a handful of styles and LVGL restyles every object that uses them. Nothing stores a raw
 * colour. Widgets: tiles (Material surfaces with a state layer), labels by type role, icons, status
 * marks (shape + colour, never colour alone), the meter, the level slider and a sparkline. */
#pragma once
#include "bz_fonts.h"
#include "bz_tokens.h"
#include "lvgl.h"

typedef enum {
    BZ_F_CLOCK,    /* 118, Sans Flex 250, tabular digits */
    BZ_F_DISPLAY,  /* 76, Sans Flex 300 */
    BZ_F_VALUE,    /* 46, Sans Flex 300 — values in small tiles */
    BZ_F_TITLE,    /* 36, Sans Flex 600 */
    BZ_F_NAME,     /* 24, Sans Flex 600 */
    BZ_F_BODY,     /* 20, Sans Flex 400 */
    BZ_F_BODY_S,   /* 17 */
    BZ_F_LABEL,    /* 16, Sans Code 400 — lowercase mono labels */
    BZ_F_CAPTION,  /* 13, Sans Code 400 */
    BZ_F_COUNT
} bz_font_role_t;

typedef enum {
    BZ_C_INK, BZ_C_DIM, BZ_C_FAINT, BZ_C_SIGNAL, BZ_C_ON_SIGNAL, BZ_C_ICE, BZ_C_ON_ICE,
    BZ_C_OK, BZ_C_WARN, BZ_C_FAULT, BZ_C_STALE, BZ_C_GROUND, BZ_C_SURFACE1, BZ_C_SURFACE2,
    BZ_C_SURFACE3, BZ_C_AMBER, BZ_C_LEAF,
    BZ_C_COUNT
} bz_color_role_t;

typedef enum { BZ_OK, BZ_WARN, BZ_FAULT, BZ_STALE, BZ_INFO } bz_status_t;

void bz_theme_init(void);
uint32_t bz_color(bz_color_role_t role);
lv_color_t bz_lv(bz_color_role_t role);
const lv_font_t *bz_font(bz_font_role_t role);
bz_color_role_t bz_status_color(bz_status_t s);

/* Shared styles, for building custom objects. */
lv_style_t *bz_style_font(bz_font_role_t role);
lv_style_t *bz_style_color(bz_color_role_t role);   /* text colour */
lv_style_t *bz_style_fill(bz_color_role_t role);    /* background colour */
lv_style_t *bz_style_plain(void);                   /* no bg, no border, no padding, no scroll bar */

/* A container with no chrome: the building block for rows and columns. */
lv_obj_t *bz_box(lv_obj_t *parent);
lv_obj_t *bz_row(lv_obj_t *parent, int gap);
lv_obj_t *bz_col(lv_obj_t *parent, int gap);

/* Text in a type role and colour role. Mono labels are lowercase by design; pass them lowercase. */
lv_obj_t *bz_label(lv_obj_t *parent, const char *text, bz_font_role_t f, bz_color_role_t c);
/* One line, truncated with an ellipsis at `w` px. */
lv_obj_t *bz_label_line(lv_obj_t *parent, const char *text, bz_font_role_t f, bz_color_role_t c, int w);
void bz_set_color(lv_obj_t *obj, bz_color_role_t c);
void bz_set_font(lv_obj_t *obj, bz_font_role_t f);

/* A Material Symbols glyph (BZ_I_* from bz_fonts.h) at 24, 32 or 40, outlined; filled at 32. */
lv_obj_t *bz_icon(lv_obj_t *parent, const char *glyph, int size, bz_color_role_t c);
void bz_icon_set(lv_obj_t *icon, const char *glyph, int size, bool filled);

/* A Material surface: surface1 by default, radius 32, padding 24. Pass-through drags (EVENT_BUBBLE),
 * a state layer on press (12 %) and the press morph (radius × 0.85 on `hold`). */
lv_obj_t *bz_tile(lv_obj_t *parent, int w, int h);
void bz_tile_set_fill(lv_obj_t *tile, bz_color_role_t fill);

/* Status as shape + colour: ● ok, ◆ check, ■ fault, ○ stale. `size` px. */
lv_obj_t *bz_mark(lv_obj_t *parent, bz_status_t s, int size);
void bz_mark_set(lv_obj_t *mark, bz_status_t s);

/* Meter: a capsule track with a fill (0..1). Fill colour by role. */
lv_obj_t *bz_meter(lv_obj_t *parent, int w, int h);
void bz_meter_set(lv_obj_t *meter, float v, bz_color_role_t fill);

/* Level: Bezel's slider — a 1:1 drag with a thin handle that stretches with velocity, rubber-banding
 * past the ends, settling on `release`. Reports every change; `final` on release. */
typedef void (*bz_level_fn)(lv_obj_t *level, float value, bool final, void *user);
lv_obj_t *bz_level(lv_obj_t *parent, int w, int h, float min, float max, float step);
void bz_level_set(lv_obj_t *level, float v, bool animate);
float bz_level_get(lv_obj_t *level);
void bz_level_on_change(lv_obj_t *level, bz_level_fn fn, void *user);

/* Sparkline: the last `n` samples as a line, auto-ranged or with fixed bounds. */
lv_obj_t *bz_spark(lv_obj_t *parent, int w, int h, int n);
void bz_spark_push(lv_obj_t *spark, float v);
void bz_spark_range(lv_obj_t *spark, float lo, float hi); /* lo == hi → auto */
void bz_spark_color(lv_obj_t *spark, bz_color_role_t c);
void bz_spark_clear(lv_obj_t *spark);

/* Formats a value or "—" when absent (a missing value is never 0). */
const char *bz_fmt(char *buf, size_t n, bool have, const char *fmt, double v);
