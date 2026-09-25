#include "bz_theme.h"
#include "bz_motion.h"
#include "bz_ui.h"
#include "lvgl_private.h" /* obj->styles: which shared style an object already wears */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_style_t st_font[BZ_F_COUNT], st_color[BZ_C_COUNT], st_fill[BZ_C_COUNT];
static lv_style_t st_plain, st_tile, st_tile_pressed, st_mark, st_track, st_capsule;
static bool inited;

static const lv_font_t *const FONTS[BZ_F_COUNT] = {
#if BZ_LEAN
    /* the tablet's scale: every role 1.25x Bezel's reference, read at arm's length on a 294 ppi panel */
    &bz_font_clock_118, &bz_font_display_92, &bz_font_display_56, &bz_font_title_44, &bz_font_name_30,
    &bz_font_body_24, &bz_font_body_21, &bz_font_mono_19, &bz_font_mono_16,
#else
    &bz_font_clock_118, &bz_font_display_76, &bz_font_display_46, &bz_font_title_36, &bz_font_name_24,
    &bz_font_body_20, &bz_font_body_17, &bz_font_mono_16, &bz_font_mono_13,
#endif
};

uint32_t bz_color(bz_color_role_t r)
{
    const bz_palette_t *P = bz_pal;
    switch (r) {
    case BZ_C_INK: return P->ink;
    case BZ_C_DIM: return P->dim;
    case BZ_C_FAINT: return P->faint;
    case BZ_C_SIGNAL: return P->signal;
    case BZ_C_ON_SIGNAL: return P->on_signal;
    case BZ_C_ICE: return P->ice;
    case BZ_C_ON_ICE: return P->on_ice;
    case BZ_C_OK: return P->ok;
    case BZ_C_WARN: return P->warn;
    case BZ_C_FAULT: return P->fault;
    case BZ_C_STALE: return P->stale;
    case BZ_C_GROUND: return P->ground;
    case BZ_C_SURFACE1: return P->surface1;
    case BZ_C_SURFACE2: return P->surface2;
    case BZ_C_SURFACE3: return P->surface3;
    case BZ_C_AMBER: return P->amber;
    case BZ_C_LEAF: return P->leaf;
    default: return P->ink;
    }
}

/* The panel is RGB565 and LVGL truncates to it, which pulls Bezel's cool greys green (0x16181B lands
 * on 16,24,24). Rounding each channel to its nearest 565 level keeps the greys the colour they are. */
static lv_color_t q565(uint32_t c)
{
    /* Nearest level of the expanded 565 palette, but judged on tint as well as distance: green has
     * twice the levels of red and blue, so plain rounding turns near-neutral greys pink or green
     * (0xFBFBFC rounds to 255,251,255). Search the neighbouring levels and penalise any change in how
     * far each channel sits from the colour's own grey. */
    int r = bz_r(c), g = bz_g(c), b = bz_b(c);
    float mean = (r + g + b) / 3.0f;
    int r0 = (r * 31 + 127) / 255, g0 = (g * 63 + 127) / 255, b0 = (b * 31 + 127) / 255;
    float best = 1e9f;
    int br = r0, bg = g0, bb = b0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dg = -2; dg <= 2; dg++)
            for (int db = -1; db <= 1; db++) {
                int R5 = r0 + dr, G6 = g0 + dg, B5 = b0 + db;
                if (R5 < 0 || R5 > 31 || G6 < 0 || G6 > 63 || B5 < 0 || B5 > 31) continue;
                int R8 = R5 << 3 | R5 >> 2, G8 = G6 << 2 | G6 >> 4, B8 = B5 << 3 | B5 >> 2;
                float m = (R8 + G8 + B8) / 3.0f;
                float e = (float)((R8 - r) * (R8 - r) + (G8 - g) * (G8 - g) + (B8 - b) * (B8 - b));
                float tr = (R8 - m) - (r - mean), tg = (G8 - m) - (g - mean), tb = (B8 - m) - (b - mean);
                e += 3 * (tr * tr + tg * tg + tb * tb);
                if (e < best) { best = e; br = R5; bg = G6; bb = B5; }
            }
    return lv_color_make((uint8_t)(br << 3 | br >> 2), (uint8_t)(bg << 2 | bg >> 4), (uint8_t)(bb << 3 | bb >> 2));
}

lv_color_t bz_lv(bz_color_role_t r) { return q565(bz_color(r)); }
lv_color_t bz_lv_rgb(uint32_t rgb) { return q565(rgb); }
const lv_font_t *bz_font(bz_font_role_t f) { return FONTS[f]; }

bz_color_role_t bz_status_color(bz_status_t s)
{
    switch (s) {
    case BZ_OK: return BZ_C_OK;
    case BZ_WARN: return BZ_C_WARN;
    case BZ_FAULT: return BZ_C_FAULT;
    case BZ_INFO: return BZ_C_DIM;
    default: return BZ_C_STALE;
    }
}

/* The state layer: a press mixes the surface 12 % toward its ink (white on dark fills, black on light). */
static lv_color_t press_filter(const lv_color_filter_dsc_t *f, lv_color_t c, lv_opa_t opa)
{
    (void)f;
    return lv_color_mix(q565(bz_pal->ink), c, opa);
}
static lv_color_filter_dsc_t press_dsc;

static void build(void)
{
    for (int f = 0; f < BZ_F_COUNT; f++) {
        lv_style_set_text_font(&st_font[f], FONTS[f]);
        lv_style_set_text_letter_space(&st_font[f], 0);
    }
    for (int c = 0; c < BZ_C_COUNT; c++) {
        lv_style_set_text_color(&st_color[c], bz_lv(c));
        lv_style_set_bg_color(&st_fill[c], bz_lv(c));
        lv_style_set_bg_opa(&st_fill[c], LV_OPA_COVER);
    }
    lv_style_set_bg_opa(&st_plain, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_plain, 0);
    lv_style_set_pad_all(&st_plain, 0);
    lv_style_set_radius(&st_plain, 0);
    lv_style_set_outline_width(&st_plain, 0);
    lv_style_set_shadow_width(&st_plain, 0);

    lv_style_set_bg_color(&st_tile, bz_lv(BZ_C_SURFACE1));
    lv_style_set_bg_opa(&st_tile, LV_OPA_COVER);
    lv_style_set_radius(&st_tile, BZ_R_TILE);
    lv_style_set_pad_all(&st_tile, BZ_PAD_TILE);
    lv_style_set_border_width(&st_tile, 0);
    lv_style_set_shadow_width(&st_tile, 0);
    lv_style_set_outline_width(&st_tile, 0);

    lv_color_filter_dsc_init(&press_dsc, press_filter);
    lv_style_set_color_filter_dsc(&st_tile_pressed, &press_dsc);
    lv_style_set_color_filter_opa(&st_tile_pressed, (lv_opa_t)(255 * 0.12f));

    lv_style_set_border_width(&st_mark, 0);
    lv_style_set_pad_all(&st_mark, 0);

    lv_style_set_bg_color(&st_track, q565(bz_pal->meter));
    lv_style_set_bg_opa(&st_track, LV_OPA_COVER);
    lv_style_set_border_width(&st_track, 0);
    lv_style_set_pad_all(&st_track, 0);
    lv_style_set_clip_corner(&st_track, true);

    lv_style_set_radius(&st_capsule, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&st_capsule, 0);
    lv_style_set_pad_all(&st_capsule, 0);
}

void bz_theme_init(void)
{
    if (!inited) {
        for (int f = 0; f < BZ_F_COUNT; f++) lv_style_init(&st_font[f]);
        for (int c = 0; c < BZ_C_COUNT; c++) {
            lv_style_init(&st_color[c]);
            lv_style_init(&st_fill[c]);
        }
        lv_style_init(&st_plain);
        lv_style_init(&st_tile);
        lv_style_init(&st_tile_pressed);
        lv_style_init(&st_mark);
        lv_style_init(&st_track);
        lv_style_init(&st_capsule);
        inited = true;
    }
    build();
    lv_obj_t *scr = bz_ui_content();
    lv_obj_set_style_bg_color(scr, bz_lv(BZ_C_GROUND), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, bz_lv(BZ_C_INK), 0);
    lv_obj_set_style_text_font(scr, FONTS[BZ_F_BODY], 0);
    lv_obj_set_style_text_color(bz_ui_glass(), bz_lv(BZ_C_INK), 0);
    lv_obj_set_style_text_font(bz_ui_glass(), FONTS[BZ_F_BODY], 0);
}

/* Called by bz_ui_set_mode: rewrite the shared styles and let LVGL restyle what uses them. */
void bz_theme_changed(void)
{
    if (!inited) return;
    bz_theme_init();
    lv_obj_report_style_change(NULL);
}

lv_style_t *bz_style_font(bz_font_role_t r) { return &st_font[r]; }
lv_style_t *bz_style_color(bz_color_role_t r) { return &st_color[r]; }
lv_style_t *bz_style_fill(bz_color_role_t r) { return &st_fill[r]; }
lv_style_t *bz_style_plain(void) { return &st_plain; }

lv_obj_t *bz_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *bz_row(lv_obj_t *parent, int gap)
{
    lv_obj_t *o = bz_box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *bz_col(lv_obj_t *parent, int gap)
{
    lv_obj_t *o = bz_box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

/* Each object carries its colour and face as one of the shared styles; replacing them keeps it
 * restylable by tone. */
/* Whether `o` already wears the shared style `st` on its main part. Every remove/add of a style makes LVGL
 * recompute the object's styles and redraw it, so the setters below do nothing when nothing changes: the
 * 10 Hz refreshes call them with the same value almost every time. */
static bool wears(lv_obj_t *o, const lv_style_t *st)
{
    for (uint32_t i = 0; i < o->style_cnt; i++)
        if (o->styles[i].style == st && o->styles[i].selector == 0) return true;
    return false;
}

static void swap_style(lv_obj_t *o, lv_style_t *set, int count, lv_style_t *want)
{
    if (wears(o, want)) return;
    for (int i = 0; i < count; i++) lv_obj_remove_style(o, &set[i], 0);
    lv_obj_add_style(o, want, 0);
}

void bz_set_color(lv_obj_t *o, bz_color_role_t c) { swap_style(o, st_color, BZ_C_COUNT, &st_color[c]); }
void bz_set_font(lv_obj_t *o, bz_font_role_t f) { swap_style(o, st_font, BZ_F_COUNT, &st_font[f]); }

lv_obj_t *bz_label(lv_obj_t *parent, const char *text, bz_font_role_t f, bz_color_role_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(l, text ? text : "");
    lv_obj_add_style(l, &st_font[f], 0);
    lv_obj_add_style(l, &st_color[c], 0);
    return l;
}

lv_obj_t *bz_label_line(lv_obj_t *parent, const char *text, bz_font_role_t f, bz_color_role_t c, int w)
{
    lv_obj_t *l = bz_label(parent, text, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w, lv_font_get_line_height(FONTS[f]));
    return l;
}

static const lv_font_t *icon_font(int size, bool filled)
{
#if BZ_LEAN
    /* sizes are asked for in Bezel's reference px and scaled like the type */
    size = size * 5 / 4;
    if (filled) return size <= 36 ? &bz_icons_fill_32 : &bz_icons_fill_40;
    if (size <= 30) return &bz_icons_outline_30;
    if (size <= 40) return &bz_icons_outline_40;
    return &bz_icons_outline_48;
#else
    if (filled) return &bz_icons_fill_32;
    if (size <= 24) return &bz_icons_outline_24;
    if (size <= 32) return &bz_icons_outline_32;
    return &bz_icons_outline_40;
#endif
}

lv_obj_t *bz_icon(lv_obj_t *parent, const char *glyph, int size, bz_color_role_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, icon_font(size, false), 0);
    lv_obj_add_style(l, &st_color[c], 0);
    return l;
}

void bz_icon_set(lv_obj_t *icon, const char *glyph, int size, bool filled)
{
    lv_label_set_text(icon, glyph);
    lv_obj_set_style_text_font(icon, icon_font(size, filled), 0);
}

lv_obj_t *bz_tile(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &st_tile, 0);
    lv_obj_add_style(t, &st_tile_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(t, w, h);
    return t;
}

void bz_tile_set_fill(lv_obj_t *t, bz_color_role_t fill)
{
    if (wears(t, &st_fill[fill])) return;
    for (int i = 0; i < BZ_C_COUNT; i++) lv_obj_remove_style(t, &st_fill[i], 0);
    lv_obj_add_style(t, &st_fill[fill], 0);
}

lv_obj_t *bz_mark(lv_obj_t *parent, bz_status_t s, int size)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_add_style(m, &st_mark, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(m, size, size);
    bz_mark_set(m, s);
    return m;
}

void bz_mark_set(lv_obj_t *m, bz_status_t s)
{
    if (wears(m, &st_fill[bz_status_color(s)])) return; /* same colour: same status, same shape */
    int size = lv_obj_get_style_width(m, 0);
    for (int i = 0; i < BZ_C_COUNT; i++) lv_obj_remove_style(m, &st_fill[i], 0);
    lv_obj_add_style(m, &st_fill[bz_status_color(s)], 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_transform_rotation(m, 0, 0);
    switch (s) {
    case BZ_OK: case BZ_INFO:
        lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
        break;
    case BZ_WARN: /* a diamond: the square turned 45° */
        lv_obj_set_style_radius(m, 1, 0);
        lv_obj_set_style_transform_pivot_x(m, size / 2, 0);
        lv_obj_set_style_transform_pivot_y(m, size / 2, 0);
        lv_obj_set_style_transform_rotation(m, 450, 0);
        lv_obj_set_style_transform_scale(m, 200, 0);
        break;
    case BZ_FAULT:
        lv_obj_set_style_radius(m, 2, 0);
        break;
    case BZ_STALE:
        lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(m, bz_lv(BZ_C_STALE), 0);
        lv_obj_set_style_border_width(m, 2, 0);
        return;
    }
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
}

/* ------------------------------------------------------------------ meter */

lv_obj_t *bz_meter(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &st_track, 0);
    lv_obj_add_style(t, &st_capsule, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(t, w, h);
    lv_obj_t *f = lv_obj_create(t);
    lv_obj_remove_style_all(f);
    lv_obj_add_style(f, &st_capsule, 0);
    lv_obj_add_style(f, &st_fill[BZ_C_ICE], 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(f, 0, h);
    return t;
}

void bz_meter_set(lv_obj_t *t, float v, bz_color_role_t fill)
{
    lv_obj_t *f = lv_obj_get_child(t, 0);
    if (!f) return;
    int w = lv_obj_get_style_width(t, 0), h = lv_obj_get_style_height(t, 0);
    if (!(v >= 0)) v = 0;
    if (v > 1) v = 1;
    /* the fill reaches past its start so its leading edge is always a capsule end, never a sliver */
    int fw = v <= 0 ? 0 : h + (int)((w - h) * v);
    if (lv_obj_get_style_width(f, 0) != fw) lv_obj_set_width(f, fw);
    if (wears(f, &st_fill[fill])) return;
    for (int i = 0; i < BZ_C_COUNT; i++) lv_obj_remove_style(f, &st_fill[i], 0);
    lv_obj_add_style(f, &st_fill[fill], 0);
}

/* ------------------------------------------------------------------ level */

typedef struct {
    float min, max, step, value;
    bz_motion_t pos;        /* 0..1 of the track, may overshoot while rubber-banding */
    bz_motion_t held;       /* 0..1: the handle thins and grows while held */
    float start_pos;
    bz_level_fn fn;
    void *user;
    lv_obj_t *fill, *handle;
    bool dragging;
} level_t;

#define MAX_LEVELS 48
static lv_obj_t *g_levels[MAX_LEVELS];
static bool g_level_hooked;

/* The level's own declared size: valid before LVGL's first layout pass, unlike its coordinates. */
static int level_w(lv_obj_t *o) { return (int)lv_obj_get_style_width(o, 0); }
static int level_h(lv_obj_t *o) { return (int)lv_obj_get_style_height(o, 0); }

static void level_layout(lv_obj_t *o)
{
    level_t *L = lv_obj_get_user_data(o);
    int w = level_w(o), h = level_h(o);
    float p = L->pos.value;
    int x = (int)(p * w);
    /* segments reach 40 px past the handle and are clipped by the track (controls.js:97-175) */
    lv_obj_set_width(L->fill, x + 40 > 0 ? x + 40 : 0);
    lv_obj_set_x(L->fill, -40);
    float held = L->held.value;
    int hw = (int)(6 - 2 * held + 0.5f), hh = (int)((h - 40) + 16 * held);
    if (hh > h - 12) hh = h - 12;
    lv_obj_set_size(L->handle, hw, hh);
    int hx = x - 9 - hw;
    if (hx < 8) hx = 8;
    lv_obj_set_pos(L->handle, hx, (h - hh) / 2);
}

static void level_frame(double now, double dt, void *user)
{
    (void)now; (void)dt; (void)user;
    for (int i = 0; i < MAX_LEVELS; i++) {
        lv_obj_t *o = g_levels[i];
        if (!o) continue;
        level_t *L = lv_obj_get_user_data(o);
        bool a = bz_motion_tick(&L->pos);
        bool b = bz_motion_tick(&L->held);
        if (a || b || L->dragging) {
            level_layout(o);
            bz_ui_keep_alive();
        }
    }
}

static float level_quant(level_t *L, float v)
{
    if (v < L->min) v = L->min;
    if (v > L->max) v = L->max;
    if (L->step > 0) v = L->min + roundf((v - L->min) / L->step) * L->step;
    return v;
}

static float to_pos(level_t *L, float v) { return L->max > L->min ? (v - L->min) / (L->max - L->min) : 0; }

static void level_begin(lv_obj_t *o, lv_point_t start, void *user)
{
    (void)start; (void)user;
    level_t *L = lv_obj_get_user_data(o);
    L->dragging = true;
    L->start_pos = L->pos.value;
    bz_motion_set(&L->pos, L->pos.value, 0);
    bz_motion_to(&L->held, 1, BZ_HOLD);
}

static void level_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *user)
{
    (void)dy; (void)vy; (void)user;
    level_t *L = lv_obj_get_user_data(o);
    float w = (float)level_w(o);
    float p = L->start_pos + dx / w;
    /* rubber band past either end, compressed to about 12 % of the track */
    p = bz_rubber_clamp(p, 0, 1, 0.22f);
    bz_motion_set(&L->pos, p, vx / w);
    float v = level_quant(L, L->min + (p < 0 ? 0 : p > 1 ? 1 : p) * (L->max - L->min));
    if (v != L->value) {
        L->value = v;
        if (L->fn) L->fn(o, v, false, L->user);
    }
}

static void level_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *user)
{
    (void)dx; (void)dy; (void)vy; (void)user;
    level_t *L = lv_obj_get_user_data(o);
    L->dragging = false;
    /* Past an edge the rubber band springs back with no velocity; inside, the release hands off. */
    bool out = L->pos.value < 0 || L->pos.value > 1;
    bz_motion_to_v(&L->pos, to_pos(L, L->value), BZ_RELEASE, out ? 0 : vx / level_w(o));
    bz_motion_to(&L->held, 0, BZ_RELEASE);
    if (L->fn) L->fn(o, L->value, true, L->user);
}

static void level_press(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    level_t *L = lv_obj_get_user_data(o);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        for (int i = 0; i < MAX_LEVELS; i++) if (g_levels[i] == o) g_levels[i] = NULL;
        free(L);
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED && !bz_drag_recent()) {
        /* A tap on the track moves the handle there on `settle`. */
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        float f = (float)(p.x - a.x1) / lv_area_get_width(&a);
        L->value = level_quant(L, L->min + f * (L->max - L->min));
        bz_motion_to(&L->pos, to_pos(L, L->value), BZ_SETTLE);
        if (L->fn) L->fn(o, L->value, true, L->user);
        bz_ui_keep_alive();
    }
}

lv_obj_t *bz_level(lv_obj_t *parent, int w, int h, float min, float max, float step)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &st_track, 0);
    lv_obj_set_style_radius(o, h >= 96 ? BZ_R_TILE : h / 3, 0);
    lv_obj_set_size(o, w, h);
    level_t *L = lv_malloc_zeroed(sizeof *L);
    L->min = min;
    L->max = max;
    L->step = step;
    L->value = min;
    bz_motion_init(&L->pos, 0, 0.0005f);
    bz_motion_init(&L->held, 0, 0.002f);
    lv_obj_set_user_data(o, L);

    L->fill = lv_obj_create(o);
    lv_obj_remove_style_all(L->fill);
    lv_obj_add_style(L->fill, &st_fill[BZ_C_ICE], 0);
    lv_obj_set_style_radius(L->fill, 8, 0);
    lv_obj_remove_flag(L->fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(L->fill, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_height(L->fill, h);

    L->handle = lv_obj_create(o);
    lv_obj_remove_style_all(L->handle);
    lv_obj_add_style(L->handle, &st_capsule, 0);
    lv_obj_add_style(L->handle, &st_fill[BZ_C_ON_ICE], 0);
    lv_obj_remove_flag(L->handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(L->handle, LV_OBJ_FLAG_EVENT_BUBBLE);

    bz_drag_t d = { .begin = level_begin, .move = level_move, .end = level_end, .axis = 1, .slop = 3 };
    bz_drag_attach(o, &d);
    lv_obj_add_event_cb(o, level_press, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(o, level_press, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < MAX_LEVELS; i++) if (!g_levels[i]) { g_levels[i] = o; break; }
    if (!g_level_hooked) {
        g_level_hooked = true;
        bz_ui_on_frame(level_frame, NULL);
    }
    level_layout(o);
    return o;
}

void bz_level_set(lv_obj_t *o, float v, bool animate)
{
    level_t *L = lv_obj_get_user_data(o);
    if (L->dragging) return; /* the finger wins over the robot's echo */
    L->value = level_quant(L, v);
    if (animate) bz_motion_to(&L->pos, to_pos(L, L->value), BZ_SETTLE);
    else bz_motion_set(&L->pos, to_pos(L, L->value), 0);
    level_layout(o);
}

float bz_level_get(lv_obj_t *o) { return ((level_t *)lv_obj_get_user_data(o))->value; }

void bz_level_on_change(lv_obj_t *o, bz_level_fn fn, void *user)
{
    level_t *L = lv_obj_get_user_data(o);
    L->fn = fn;
    L->user = user;
}

/* ------------------------------------------------------------------ spark */

typedef struct {
    float *v;
    int n, head, count;
    float lo, hi, min_span;
    bz_color_role_t color;
} spark_t;

static void spark_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    spark_t *S = lv_obj_get_user_data(o);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        free(S->v);
        free(S);
        return;
    }
    if (S->count < 2) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float lo = S->lo, hi = S->hi;
    if (lo == hi) {
        lo = 1e30f; hi = -1e30f;
        for (int i = 0; i < S->count; i++) {
            float v = S->v[(S->head - S->count + i + S->n) % S->n];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        float pad = (hi - lo) * 0.15f + 1e-3f;
        lo -= pad; hi += pad;
        if (hi - lo < S->min_span) {
            float mid = (hi + lo) / 2;
            lo = mid - S->min_span / 2;
            hi = mid + S->min_span / 2;
        }
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = bz_lv(S->color);
    d.width = 2;
    d.round_start = d.round_end = 1;
    float w = (float)lv_area_get_width(&a) - 2, h = (float)lv_area_get_height(&a) - 4;
    for (int i = 1; i < S->count; i++) {
        float v0 = S->v[(S->head - S->count + i - 1 + S->n) % S->n];
        float v1 = S->v[(S->head - S->count + i + S->n) % S->n];
        float x0 = a.x1 + 1 + w * (i - 1 + S->n - S->count) / (S->n - 1);
        float x1 = a.x1 + 1 + w * (i + S->n - S->count) / (S->n - 1);
        float y0 = a.y1 + 2 + h * (1 - (v0 - lo) / (hi - lo));
        float y1 = a.y1 + 2 + h * (1 - (v1 - lo) / (hi - lo));
        d.p1.x = x0; d.p1.y = y0; d.p2.x = x1; d.p2.y = y1;
        lv_draw_line(layer, &d);
    }
}

lv_obj_t *bz_spark(lv_obj_t *parent, int w, int h, int n)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(o, w, h);
    spark_t *S = lv_malloc_zeroed(sizeof *S);
    S->v = lv_malloc_zeroed((size_t)n * sizeof(float));
    S->n = n;
    S->color = BZ_C_INK;
    lv_obj_set_user_data(o, S);
    lv_obj_add_event_cb(o, spark_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, spark_draw, LV_EVENT_DELETE, NULL);
    return o;
}

void bz_spark_push(lv_obj_t *o, float v)
{
    spark_t *S = lv_obj_get_user_data(o);
    if (!(v == v)) return; /* NaN: nothing to plot */
    S->v[S->head] = v;
    S->head = (S->head + 1) % S->n;
    if (S->count < S->n) S->count++;
    lv_obj_invalidate(o);
}

void bz_spark_range(lv_obj_t *o, float lo, float hi)
{
    spark_t *S = lv_obj_get_user_data(o);
    S->lo = lo;
    S->hi = hi;
}

void bz_spark_min_span(lv_obj_t *o, float span)
{
    spark_t *S = lv_obj_get_user_data(o);
    S->min_span = span;
}

void bz_spark_color(lv_obj_t *o, bz_color_role_t c)
{
    spark_t *S = lv_obj_get_user_data(o);
    if (S->color != c) {
        S->color = c;
        lv_obj_invalidate(o);
    }
}

void bz_spark_clear(lv_obj_t *o)
{
    spark_t *S = lv_obj_get_user_data(o);
    if (!S->count) return; /* already empty: called every refresh while offline */
    S->count = S->head = 0;
    lv_obj_invalidate(o);
}

const char *bz_fmt(char *buf, size_t n, bool have, const char *fmt, double v)
{
    if (!have || v != v) snprintf(buf, n, "\xe2\x80\x94");
    else snprintf(buf, n, fmt, v);
    return buf;
}
