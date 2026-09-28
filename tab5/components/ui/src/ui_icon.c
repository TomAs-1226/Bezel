/* App icons: every app a squircle of its own colour with its Material Symbols glyph, the way a phone's home
 * screen tells its apps apart at a glance.
 *
 * The look. A continuous-corner squircle (a superellipse, n = 5), antialiased; a gradient lit from the one light
 * Bezel has, the top left (lighter there, deeper toward the bottom right) with a faint sheen near the light and a
 * hairline of it along the top edge; the glyph filled and white, standing a pixel off the surface on a soft
 * shadow. Colours are per app and harmonised: similar lightness and chroma across the set, so no icon shouts,
 * and each group of the library reads as a run of neighbouring hues rather than a wall of one grey.
 *
 * Three styles (settings > look > icons): colour (the default, above); tinted, Material You's themed icons, a
 * tonal container of the accent with the glyph in the accent; and mono, the surface with an ink glyph, as the
 * library looked before. Tinted and mono follow the tone and the accent.
 *
 * The cost. Each (app, size) is baked once into an RGB565A8 picture in PSRAM (3 bytes a pixel: a 72 px icon is
 * 15 KB) and every place that shows that app at that size shares it; LVGL only ever blits it. The squircle's
 * coverage and rim are computed once per size and shared by every app. A change of style, tone or accent bakes
 * the same buffers again in place (a few ms for the whole set) and the caller's one redraw shows it. Nothing
 * here runs per frame. */
#include "ui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */
#include "ui_home_mode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ the set */

/* The palette: saturated but not loud, each dark enough under a white glyph. A second colour, when given, is
 * the bottom-right end of the gradient (a two-hue icon: the assistant, photos, music, weather); otherwise both
 * ends come from the one hue, lighter and deeper. */
enum {
    RED = 0xEA4038, CORAL = 0xEE5F3C, ORANGE = 0xF07418, AMBER = 0xE08A06, GOLD = 0xD99A00, LIME = 0x5FAA2A,
    GREEN = 0x21A857, TEAL = 0x0E9C8A, CYAN = 0x1497C9, SKY = 0x3182E8, BLUE = 0x2F63E0, INDIGO = 0x4B4FDB,
    VIOLET = 0x7A4BE0, PURPLE = 0x9E43D2, MAGENTA = 0xCC3C92, PINK = 0xE8466F, SLATE = 0x4A6583,
    GRAPHITE = 0x535B68, BROWN = 0xA0612F,
};

typedef struct {
    const char *label;   /* the library's label, or the app's window name */
    const char *glyph;
    uint32_t c1, c2;     /* c2 0: from c1 */
} icon_def_t;

static const icon_def_t DEFS[] = {
    /* the robot */
    { "preflight", BZ_I_CHECKLIST, GREEN, 0 },
    { "alerts", BZ_I_WARNING, RED, 0 },
    { "tune", BZ_I_TUNE, AMBER, 0 },
    { "auto", BZ_I_SPORTS_SCORE, MAGENTA, 0 },
    { "field", BZ_I_STADIUM, TEAL, 0 },
    { "robot", BZ_I_SMART_TOY, ORANGE, 0 },
    { "motors", BZ_I_MODE_FAN, CORAL, 0 },
    { "states", BZ_I_ACCOUNT_TREE, INDIGO, 0 },
    { "controls", BZ_I_SPORTS_ESPORTS, VIOLET, 0 },
    { "blue alliance", BZ_I_LEADERBOARD, BLUE, 0 },
    { "batteries", BZ_I_BATTERY_CHARGING_FULL, LIME, 0 },
    /* diagnosing it */
    { "systemcore", BZ_I_DEVELOPER_BOARD, SLATE, 0 },
    { "can tap", BZ_I_CABLE, CYAN, 0 },
    { "recorder", BZ_I_FIBER_MANUAL_RECORD, RED, 0 },
    { "logs", BZ_I_RECEIPT_LONG, BROWN, 0 },
    { "level", BZ_I_ADJUST, SKY, 0 },
    { "lens", BZ_I_PHOTO_CAMERA, GRAPHITE, 0 },
    { "gearing", BZ_I_SPEED, ORANGE, 0 },
    { "ruler", BZ_I_STRAIGHTEN, GOLD, 0 },
    { "reference", BZ_I_MENU_BOOK, INDIGO, 0 },
    /* this tablet */
    { "assist", BZ_I_AUTO_AWESOME, VIOLET, MAGENTA },
    { "link", BZ_I_COMPUTER, INDIGO, 0 },
    { "pair pc", BZ_I_LINK, CYAN, 0 },
    { "companion", BZ_I_VISIBILITY, PINK, 0 },
    { "home mode", BZ_I_HOME, TEAL, 0 },
    { "timer", BZ_I_TIMER, ORANGE, 0 },
    { "calculator", BZ_I_CALCULATE, SLATE, 0 },
    { "notes", BZ_I_EDIT_NOTE, GOLD, 0 },
    { "checklist", BZ_I_CHECKLIST_RTL, GREEN, 0 },
    { "flashlight", BZ_I_FLASHLIGHT_ON, AMBER, 0 },
    { "files", BZ_I_FOLDER, BLUE, 0 },
    { "system", BZ_I_MONITORING, LIME, 0 },
    { "settings", BZ_I_SETTINGS, GRAPHITE, 0 },
    /* everyday */
    { "clock", BZ_I_ALARM, VIOLET, 0 },
    { "calendar", BZ_I_CALENDAR_MONTH, RED, 0 },
    { "documents", BZ_I_ARTICLE, SKY, 0 },
    { "photos", BZ_I_PHOTO_LIBRARY, MAGENTA, ORANGE },
    { "storage", BZ_I_SD_CARD, SLATE, 0 },
    { "music", BZ_I_MUSIC_NOTE, PINK, RED },
    { "smart home", BZ_I_LIGHTBULB, AMBER, 0 },
    { "weather", BZ_I_PARTLY_CLOUDY_DAY, CYAN, BLUE },
};
#define NDEFS ((int)(sizeof DEFS / sizeof DEFS[0]))

static int def_by_label(const char *label)
{
    if (!label) return -1;
    for (int i = 0; i < NDEFS; i++)
        if (!strcmp(DEFS[i].label, label)) return i;
    return -1;
}

/* ------------------------------------------------------------------ the shape, once per size */

typedef struct {
    int size;
    uint8_t *cov;   /* coverage 0..255 */
    int8_t *edge;   /* the rim's light: + along edges facing the light, - along those facing away */
} shape_t;

#define MAX_SHAPES 6
static shape_t g_shapes[MAX_SHAPES];

/* inside the unit superellipse |x|^5 + |y|^5 <= 1: multiplications only */
static inline bool se_inside(float x, float y)
{
    float ax = fabsf(x), ay = fabsf(y);
    float x2 = ax * ax, y2 = ay * ay;
    return x2 * x2 * ax + y2 * y2 * ay <= 1.0f;
}

static const shape_t *shape_for(int s)
{
    for (int i = 0; i < MAX_SHAPES; i++)
        if (g_shapes[i].size == s) return &g_shapes[i];
    shape_t *sh = NULL;
    for (int i = 0; i < MAX_SHAPES; i++)
        if (!g_shapes[i].size) { sh = &g_shapes[i]; break; }
    if (!sh) return NULL;
    size_t n = (size_t)s * s;
    sh->cov = lv_malloc(n);
    sh->edge = lv_malloc(n);
    if (!sh->cov || !sh->edge) {
        lv_free(sh->cov);
        lv_free(sh->edge);
        sh->cov = NULL;
        sh->edge = NULL;
        return NULL;
    }
    sh->size = s;
    float a = s * 0.5f; /* half the side: the squircle touches the picture's edges */
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float px = (x + 0.5f - a) / a, py = (y + 0.5f - a) / a;
            float ax = fabsf(px), ay = fabsf(py);
            float x4 = ax * ax * ax * ax, y4 = ay * ay * ay * ay;
            float S = x4 * ax + y4 * ay;
            float N = S > 0 ? powf(S, 0.2f) : 0;
            /* distance to the boundary in px, to first order: (N - 1) over the norm's gradient */
            float n4 = N * N * N * N;
            float gx = n4 > 1e-9f ? x4 / n4 : 0, gy = n4 > 1e-9f ? y4 / n4 : 0;
            float gl = sqrtf(gx * gx + gy * gy);
            float d = gl > 1e-6f ? (N - 1) * a / gl : -a; /* negative inside */
            int cov;
            if (d < -1.2f) cov = 255;
            else if (d > 1.2f) cov = 0;
            else {
                /* near the edge: 4x4 samples */
                int in = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                        in += se_inside((x + (sx + 0.5f) / 4 - a) / a, (y + (sy + 0.5f) / 4 - a) / a);
                cov = in * 255 / 16;
            }
            sh->cov[y * s + x] = (uint8_t)cov;
            /* the rim: a hairline just inside the edge, lit where the edge faces up (the light), shaded where
             * it faces down; the sides get a little of each */
            float depth = -d;
            float rt = (depth - 0.3f) / 1.4f;
            rt = rt < 0 ? 0 : rt > 1 ? 1 : rt;
            float rim = 1 - rt * rt * (3 - 2 * rt);
            float ny = gl > 1e-6f ? (py < 0 ? -gy : gy) / gl : 0; /* +1 facing down, -1 facing up */
            float face = -ny;
            float lit = rim * (face > 0 ? 0.55f * face : 0.35f * face);
            int e = (int)lrintf(lit * 127);
            sh->edge[y * s + x] = (int8_t)(e < -127 ? -127 : e > 127 ? 127 : e);
        }
    return sh;
}

/* ------------------------------------------------------------------ the glyph */

typedef struct { const lv_font_t *font; int px; } face_t;
static const face_t FILLED[] = { { &bz_icons_fill_32, 32 }, { &bz_icons_fill_40, 40 } };
static const face_t OUTLINED[] = { { &bz_icons_outline_24, 24 }, { &bz_icons_outline_30, 30 }, { &bz_icons_outline_32, 32 },
                                   { &bz_icons_outline_40, 40 }, { &bz_icons_outline_48, 48 } };

static uint32_t utf8_cp(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0) return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
}

/* The glyph's coverage into `out` (s x s), its em `em` px, its box centred. The face is the smallest at least
 * that big (or the largest), scaled by bilinear sampling, with four taps a pixel when it shrinks. */
static void glyph_mask(uint8_t *out, int s, const char *glyph, float em, bool filled)
{
    memset(out, 0, (size_t)s * s);
    const face_t *faces = filled ? FILLED : OUTLINED;
    int nf = filled ? 2 : 5;
    const face_t *f = &faces[nf - 1];
    for (int i = 0; i < nf; i++)
        if (faces[i].px >= em - 0.5f) { f = &faces[i]; break; }
    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof g);
    if (!lv_font_get_glyph_dsc(f->font, &g, utf8_cp(glyph), 0) || g.box_w <= 0 || g.box_h <= 0) return;
    lv_draw_buf_t *db = lv_draw_buf_create(g.box_w, g.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!db) return;
    /* it returns the draw buffer it filled (A8, db's stride), not the pixels */
    const uint8_t *bm = lv_font_get_glyph_bitmap(&g, db) ? db->data : NULL;
    if (!bm) {
        lv_draw_buf_destroy(db);
        return;
    }
    int stride = (int)db->header.stride;
    float k = em / f->px;
    float gw = g.box_w * k, gh = g.box_h * k;
    float x0 = (s - gw) * 0.5f, y0 = (s - gh) * 0.5f;
    int taps = k < 0.85f ? 2 : 1;
    for (int y = (int)floorf(y0) - 1; y <= (int)ceilf(y0 + gh) + 1; y++) {
        if (y < 0 || y >= s) continue;
        for (int x = (int)floorf(x0) - 1; x <= (int)ceilf(x0 + gw) + 1; x++) {
            if (x < 0 || x >= s) continue;
            int acc = 0;
            for (int ty = 0; ty < taps; ty++)
                for (int tx = 0; tx < taps; tx++) {
                    /* the output pixel's (sub)sample in the glyph's own pixels */
                    float u = (x + (tx + 0.5f) / taps - x0) / k - 0.5f, v = (y + (ty + 0.5f) / taps - y0) / k - 0.5f;
                    int iu = (int)floorf(u), iv = (int)floorf(v);
                    float fu = u - iu, fv = v - iv;
                    int c[4];
                    for (int q = 0; q < 4; q++) {
                        int qx = iu + (q & 1), qy = iv + (q >> 1);
                        c[q] = qx < 0 || qy < 0 || qx >= g.box_w || qy >= g.box_h ? 0 : bm[qy * stride + qx];
                    }
                    float top = c[0] + (c[1] - c[0]) * fu, bot = c[2] + (c[3] - c[2]) * fu;
                    acc += (int)(top + (bot - top) * fv + 0.5f);
                }
            acc /= taps * taps;
            out[y * s + x] = (uint8_t)(acc > 255 ? 255 : acc);
        }
    }
    lv_draw_buf_destroy(db);
}

/* ------------------------------------------------------------------ baking */

typedef struct {
    int def, size;
    int style;          /* the style it was baked in (previews keep theirs) */
    bool fixed;         /* a preview: its own style, whatever the setting */
    lv_image_dsc_t dsc;
    uint8_t *px;        /* RGB565 plane, then the A8 plane */
} entry_t;

#define MAX_ENTRIES 160
static entry_t g_entries[MAX_ENTRIES];
static int g_nentries;
static int g_style = UI_ICON_COLOUR;
static bool g_hooked;
static void icon_load(void);

static uint32_t lighten(uint32_t c, float t) { return bz_mix(c, 0xFFFFFF, t); }
static uint32_t darken(uint32_t c, float t) { return bz_mix(c, 0x000000, t); }

static void bake(entry_t *e)
{
    const icon_def_t *d = &DEFS[e->def];
    const int s = e->size;
    const shape_t *sh = shape_for(s);
    uint16_t *rgb = (uint16_t *)e->px;
    uint8_t *alpha = e->px + (size_t)s * s * 2;
    if (!sh) {
        memset(e->px, 0, (size_t)s * s * 3);
        return;
    }
    bool dark = bz_ui_dark();
    const bz_accent_t *acc = &BZ_ACCENTS[bz_ui_accent()];
    uint32_t top, bot, ink;
    float sheen, shadow, rim;
    bool filled = true;
    switch (e->style) {
    case UI_ICON_TINTED:
        /* Material You's themed icons: the accent's tonal container, the glyph in the accent */
        if (dark) {
            top = bz_mix(bz_pal->surface3, acc->dark, 0.30f);
            bot = bz_mix(bz_pal->surface2, acc->dark, 0.18f);
            ink = acc->dark;
        } else {
            top = lighten(acc->light, 0.25f);
            bot = acc->light;
            ink = acc->on_light;
        }
        sheen = 0.04f;
        shadow = 0;
        rim = 0.6f;
        break;
    case UI_ICON_MONO:
        /* the surface and an outlined ink glyph: the library as it was, in the new shape */
        /* light: a grey tile, never white -- white on the white drawer had no edge at all */
        top = dark ? lighten(bz_pal->surface3, 0.04f) : lighten(bz_pal->surface2, 0.3f);
        bot = dark ? bz_pal->surface2 : bz_pal->surface3;
        ink = bz_pal->ink;
        filled = false;
        sheen = 0.03f;
        shadow = 0;
        rim = dark ? 0.6f : 0.8f;
        break;
    default:
        if (d->c2) {
            top = lighten(d->c1, 0.10f);
            bot = darken(d->c2, 0.06f);
        } else {
            top = lighten(d->c1, 0.20f);
            bot = darken(d->c1, 0.16f);
        }
        ink = 0xFFFFFF;
        sheen = 0.12f;
        shadow = 0.30f;
        rim = 1.0f;
        break;
    }

    /* the glyph, and for the colour style its shadow: the same mask a pixel and a half lower, softened */
    uint8_t *gm = lv_malloc((size_t)s * s * 2);
    if (!gm) {
        memset(e->px, 0, (size_t)s * s * 3);
        return;
    }
    uint8_t *sm = gm + (size_t)s * s;
    glyph_mask(gm, s, d->glyph, s * 0.58f, filled);
    memset(sm, 0, (size_t)s * s);
    if (shadow > 0) {
        int off = s >= 56 ? 2 : 1;
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int yy = y - off + dy, xx = x + dx;
                        if (yy < 0 || yy >= s || xx < 0 || xx >= s) continue;
                        sum += gm[yy * s + xx];
                        n++;
                    }
                sm[y * s + x] = (uint8_t)(n ? sum / 9 : 0);
            }
    }

    const int tr = bz_r(top), tg = bz_g(top), tb = bz_b(top);
    const int br = bz_r(bot), bg = bz_g(bot), bb = bz_b(bot);
    const int ir = bz_r(ink), ig = bz_g(ink), ib = bz_b(ink);
    static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            int i = y * s + x;
            int cov = sh->cov[i];
            if (!cov) {
                rgb[i] = 0;
                alpha[i] = 0;
                continue;
            }
            float u = (x + 0.5f) / s, v = (y + 0.5f) / s;
            /* lit from the top left: mostly down the icon, a little across */
            float t = 0.28f * u + 0.72f * v;
            float r = tr + (br - tr) * t, g = tg + (bg - tg) * t, b = tb + (bb - tb) * t;
            /* a soft sheen near the light */
            float dx = u - 0.18f, dy = v - 0.02f;
            float sd = 1 - sqrtf(dx * dx + dy * dy) / 0.85f;
            if (sd > 0) {
                float k = sheen * sd * sd;
                r += (255 - r) * k;
                g += (255 - g) * k;
                b += (255 - b) * k;
            }
            /* the rim: light off the top edge, a shade along the bottom */
            float el = sh->edge[i] / 127.0f * rim;
            if (el > 0) {
                float k = 0.38f * el;
                r += (255 - r) * k;
                g += (255 - g) * k;
                b += (255 - b) * k;
            } else if (el < 0) {
                float k = 1 + 0.30f * el;
                r *= k;
                g *= k;
                b *= k;
            }
            /* the glyph's shadow, then the glyph */
            if (sm[i]) {
                float k = 1 - shadow * sm[i] / 255.0f;
                r *= k;
                g *= k;
                b *= k;
            }
            if (gm[i]) {
                float k = gm[i] / 255.0f;
                r += (ir - r) * k;
                g += (ig - g) * k;
                b += (ib - b) * k;
            }
            int bay = BAYER[((y & 3) << 2) | (x & 3)];
            int r5 = ((int)r * 31 * 16 + bay * 255) / (255 * 16);
            int g6 = ((int)g * 63 * 16 + bay * 255) / (255 * 16);
            int b5 = ((int)b * 31 * 16 + bay * 255) / (255 * 16);
            r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
            g6 = g6 < 0 ? 0 : g6 > 63 ? 63 : g6;
            b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
            rgb[i] = (uint16_t)(r5 << 11 | g6 << 5 | b5);
            alpha[i] = (uint8_t)cov;
        }
    }
    lv_free(gm);
    lv_image_cache_drop(&e->dsc);
}

static void rebake_all(void)
{
    for (int i = 0; i < g_nentries; i++) {
        entry_t *e = &g_entries[i];
        if (!e->fixed) e->style = g_style;
        /* colour icons don't follow the tone or the accent: only what does is baked again */
        bake(e);
    }
}

static void theme_changed(void) { rebake_all(); }

static entry_t *entry_for(int def, int size, int style, bool fixed)
{
    if (def < 0 || size < 8 || size > 160) return NULL;
    for (int i = 0; i < g_nentries; i++) {
        entry_t *e = &g_entries[i];
        if (e->def == def && e->size == size && e->fixed == fixed && (!fixed || e->style == style)) return e;
    }
    if (g_nentries >= MAX_ENTRIES) return NULL;
    if (!g_hooked) {
        g_hooked = true;
        icon_load();
        bz_theme_on_change(theme_changed);
    }
    entry_t *e = &g_entries[g_nentries];
    size_t n = (size_t)size * size * 3;
    e->px = lv_malloc(n);
    if (!e->px) return NULL;
    g_nentries++;
    e->def = def;
    e->size = size;
    e->style = fixed ? style : g_style; /* g_style as loaded, which the first call's argument predates */
    e->fixed = fixed;
    e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    e->dsc.header.w = (uint32_t)size;
    e->dsc.header.h = (uint32_t)size;
    e->dsc.header.stride = (uint32_t)size * 2;
    e->dsc.data_size = (uint32_t)n;
    e->dsc.data = e->px;
    bake(e);
    return e;
}

/* ------------------------------------------------------------------ the interface */

static int def_for_app(const ui_app_t *app)
{
    if (!app) return -1;
    int d = def_by_label(ui_app_label(app));
    return d >= 0 ? d : def_by_label(app->name);
}

static lv_obj_t *icon_obj(lv_obj_t *parent, entry_t *e, int size)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (e) lv_image_set_src(img, &e->dsc);
    lv_obj_set_size(img, size, size);
    return img;
}

const lv_image_dsc_t *ui_icon_image(const ui_app_t *app, int size)
{
    entry_t *e = entry_for(def_for_app(app), size, g_style, false);
    return e ? &e->dsc : NULL;
}

lv_obj_t *ui_app_icon(lv_obj_t *parent, const ui_app_t *app, int size)
{
    return icon_obj(parent, entry_for(def_for_app(app), size, g_style, false), size);
}

lv_obj_t *ui_icon_named(lv_obj_t *parent, const char *name, int size)
{
    int d = def_by_label(name);
    if (d < 0) {
        const ui_app_t *a = ui_app_find(name);
        d = def_for_app(a);
    }
    if (d < 0) return NULL;
    return icon_obj(parent, entry_for(d, size, g_style, false), size);
}

lv_obj_t *ui_icon_preview(lv_obj_t *parent, const char *name, int size, int style)
{
    int d = def_by_label(name);
    if (d < 0) return NULL;
    return icon_obj(parent, entry_for(d, size, style, true), size);
}

int ui_icon_style(void) { return g_style; }

static const char *const STYLE_KV[UI_ICON_NSTYLES] = { "colour", "tinted", "mono" };

static void icon_load(void)
{
    char buf[16];
    if (hal_kv_get("icons", buf, sizeof buf))
        for (int i = 0; i < UI_ICON_NSTYLES; i++)
            if (!strcmp(buf, STYLE_KV[i])) g_style = i;
}

void ui_icon_set_style(int style)
{
    if (style < 0 || style >= UI_ICON_NSTYLES || style == g_style) return;
    g_style = style;
    hal_kv_set("icons", STYLE_KV[style]);
    rebake_all();
    /* one redraw, as an accent change does: every icon on every screen shows its new picture */
    lv_obj_invalidate(bz_ui_content());
    lv_obj_invalidate(bz_ui_glass());
}

size_t ui_icon_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < g_nentries; i++) n += (size_t)g_entries[i].size * g_entries[i].size * 3;
    for (int i = 0; i < MAX_SHAPES; i++) n += (size_t)g_shapes[i].size * g_shapes[i].size * 2;
    return n;
}
