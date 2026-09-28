/* The ground under the pages (settings > look > ground): plain, or a light generated from the tone and the
 * accent -- a glow from the top, an aurora's bands of colour, film grain, contour lines -- baked once into a
 * small picture in PSRAM and tiled across the screen behind the pages.
 *
 * Small on purpose. PSRAM is tight (a full-screen RGB565 picture would be 1.8 MB), so every ground is built from
 * pictures that repeat: the light that only changes down the screen is a strip 32 px wide, tiled across; grain is
 * a 64 px wide strip whose noise repeats unseen; the contour lines are an A8 tile a quarter of the frame wide,
 * drawn in the accent over the glow. The largest ground holds about 150 KB, plain holds nothing.
 *
 * Two things the picture has to survive decide its shape:
 * - Pages slide as pictures of themselves, ground included, so page 1's left edge arrives beside page 0's right
 *   edge: every tile is a whole fraction of the frame's width, so the slide shows no seam.
 * - Lists scroll by moving pixels already drawn (bz_ui_scroll), so a ground that showed through a list would
 *   scroll with it. Every page that scrolls keeps its list on an opaque surface; the ground shows around it,
 *   never through it.
 * Cost: an opaque strip blit under whatever LVGL redraws (the strip covers, so the screen's fill under it is
 * skipped), and nothing at rest. A change of ground, tone or accent bakes it again (a few ms) and the one redraw
 * that follows shows it. */
#include "ui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const NAMES[UI_GROUND_COUNT] = { "plain", "glow", "aurora", "grain", "contour" };

const char *ui_ground_name(int g) { return g >= 0 && g < UI_GROUND_COUNT ? NAMES[g] : NAMES[0]; }

#define STRIP_W 32   /* a light that only changes down the screen */
#define GRAIN_W 64   /* grain: the noise repeats every 64 px, which nobody sees */

static struct {
    lv_obj_t *strip, *lines;           /* the light, and the contour lines over it */
    lv_image_dsc_t strip_dsc, lines_dsc;
    uint8_t *strip_px, *lines_px;
    size_t strip_bytes, lines_bytes;
    int shown;                         /* the ground on the screen now */
    bool hooked;
} G = { .shown = -1 };

/* 4x4 ordered dither into RGB565: a gentle ramp on a dark ground otherwise bands in five-bit red and blue */
static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint16_t px565(float r, float g, float b, int x, int y)
{
    int bay = BAYER[((y & 3) << 2) | (x & 3)];
    int ri = (int)r, gi = (int)g, bi = (int)b;
    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
    int r5 = (ri * 31 * 16 + bay * 255) / (255 * 16), g6 = (gi * 63 * 16 + bay * 255) / (255 * 16),
        b5 = (bi * 31 * 16 + bay * 255) / (255 * 16);
    return (uint16_t)((r5 > 31 ? 31 : r5) << 11 | (g6 > 63 ? 63 : g6) << 5 | (b5 > 31 ? 31 : b5));
}

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* The accent's hue strong enough to glow with: the dark tone's pastel pushed away from its own grey. */
static uint32_t glow_colour(void)
{
    uint32_t c = BZ_ACCENTS[bz_ui_accent()].dark;
    int r = bz_r(c), g = bz_g(c), b = bz_b(c);
    int m = (r + g + b) / 3;
    return bz_rgb(m + (r - m) * 2 - 30, m + (g - m) * 2 - 30, m + (b - m) * 2 - 30);
}

/* A second hue for the aurora: the accent's channels turned (violet brings mint, orange brings violet). */
static uint32_t turn(uint32_t c) { return bz_rgb(bz_g(c), bz_b(c), bz_r(c)); }

/* How much of the colour, and which, `v` of the way down: the light every ground but plain starts from. */
static void light_at(int ground, float v, float *k1, float *k2)
{
    *k1 = *k2 = 0;
    switch (ground) {
    case UI_GROUND_GLOW:
    case UI_GROUND_CONTOUR: {
        float t = 1 - v / 0.62f;
        *k1 = t > 0 ? t * t * (ground == UI_GROUND_GLOW ? 0.36f : 0.20f) : 0;
        break;
    }
    case UI_GROUND_AURORA: {
        /* two bands, the turned hue high and the accent under it, over a faint wash */
        float d1 = (v - 0.10f) / 0.13f, d2 = (v - 0.30f) / 0.16f;
        float wash = 1 - v / 0.62f;
        /* everything fades to nothing by 0.62 down, inside the strip (2/3 of the height) */
        float end = wash > 0 ? (wash < 0.25f ? wash / 0.25f : 1) : 0;
        *k2 = expf(-d1 * d1) * 0.30f * end;
        *k1 = (expf(-d2 * d2) * 0.34f + (wash > 0 ? wash * wash * 0.10f : 0)) * end;
        break;
    }
    case UI_GROUND_GRAIN: {
        float t = 1 - v / 0.8f;
        *k1 = t > 0 ? t * t * 0.14f : 0;
        break;
    }
    default:
        break;
    }
}

static void bake_strip(int ground, int w, int h, uint16_t *out)
{
    const bool dark = bz_ui_dark();
    const uint32_t base = bz_pal->ground;
    const uint32_t c1 = glow_colour(), c2 = turn(c1);
    /* a light ground takes a little less before it reads as colour */
    const float amt = dark ? 1.0f : 0.75f;
    /* where the light has faded out, exactly the colour LVGL fills the rest of the screen with: a dithered
     * ground next to LVGL's own rounding of it shows as an edge where the strip ends */
    const uint16_t flat = lv_color_to_u16(bz_lv(BZ_C_GROUND));
    for (int y = 0; y < h; y++) {
        float v = (float)y / H;
        float k1, k2;
        light_at(ground, v, &k1, &k2);
        k1 *= amt;
        k2 *= amt;
        if (ground != UI_GROUND_GRAIN && k1 + k2 < 0.004f) {
            for (int x = 0; x < w; x++) out[(size_t)y * w + x] = flat;
            continue;
        }
        float r = bz_r(base), g = bz_g(base), b = bz_b(base);
        r += (bz_r(c1) - r) * k1 + (bz_r(c2) - r) * k2;
        g += (bz_g(c1) - g) * k1 + (bz_g(c2) - g) * k2;
        b += (bz_b(c1) - b) * k1 + (bz_b(c2) - b) * k2;
        for (int x = 0; x < w; x++) {
            float gr = 0;
            if (ground == UI_GROUND_GRAIN) gr = ((int)(hash2(x, y) & 0xFF) - 128) / 128.0f * (dark ? 5.0f : 7.0f);
            out[(size_t)y * w + x] = px565(r + gr, g + gr, b + gr, x, y);
        }
    }
}

/* Contour lines: a height field that repeats across the tile, a line every ninth of it, a pixel wide wherever
 * the field is steep or shallow (the distance to a line is the fraction over the field's gradient). */
static void bake_lines(int w, int h, uint8_t *out)
{
    const float TP = 6.2831853f;
    for (int y = 0; y < h; y++) {
        float v = (float)y / H;
        float fade = 1 - (float)y / h;
        for (int x = 0; x < w; x++) {
            float u = (float)x / w;
            float a1 = TP * u + 5.4f * v, a2 = TP * 2 * u - 5.1f * v + 0.8f, a3 = TP * 3 * u + 7.3f * v;
            float f = 0.55f * sinf(a1) + 0.35f * sinf(a2) + 0.22f * cosf(a3) + 1.6f * v;
            float fx = (0.55f * cosf(a1) * TP + 0.35f * cosf(a2) * TP * 2 - 0.22f * sinf(a3) * TP * 3) / w;
            float fy = (0.55f * cosf(a1) * 5.4f - 0.35f * cosf(a2) * 5.1f - 0.22f * sinf(a3) * 7.3f + 1.6f) / H;
            float s = f * 9.0f;
            float fr = s - floorf(s);
            float gl = sqrtf(fx * fx + fy * fy) * 9.0f + 1e-5f;
            float dist = fminf(fr, 1 - fr) / gl;
            float line = 1 - dist / 0.85f;
            line = line < 0 ? 0 : line > 1 ? 1 : line;
            int a = (int)lrintf(line * fade * fade * 120);
            out[(size_t)y * w + x] = (uint8_t)(a > 255 ? 255 : a);
        }
    }
}

static lv_obj_t *layer(lv_obj_t *parent)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_image_set_inner_align(o, LV_IMAGE_ALIGN_TILE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static bool buf(uint8_t **px, size_t *have, size_t want)
{
    if (*have == want && *px) return true;
    lv_free(*px);
    *px = want ? lv_malloc(want) : NULL;
    *have = *px ? want : 0;
    return *px != NULL || !want;
}

static void dsc_init(lv_image_dsc_t *d, lv_color_format_t cf, int w, int h, int bpp, const uint8_t *px)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)(w * bpp);
    d->data_size = (uint32_t)(w * h * bpp);
    d->data = px;
    lv_image_cache_drop(d);
}

/* Show what is baked on one pair of layers (the pages' own, or a mirror on the lock screen). */
static void show_on(lv_obj_t *strip, lv_obj_t *lines)
{
    if (G.shown <= UI_GROUND_PLAIN || !G.strip_px) {
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lines, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(strip, NULL);
        lv_image_set_src(lines, NULL);
        return;
    }
    lv_image_set_src(strip, &G.strip_dsc);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_size(strip, W, (int32_t)G.strip_dsc.header.h);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
    if (G.shown == UI_GROUND_CONTOUR && G.lines_px) {
        lv_image_set_src(lines, &G.lines_dsc);
        lv_obj_set_style_image_recolor(lines, bz_lv_rgb(glow_colour()), 0);
        lv_obj_set_style_image_recolor_opa(lines, LV_OPA_COVER, 0);
        lv_obj_set_pos(lines, 0, 0);
        lv_obj_set_size(lines, W, (int32_t)G.lines_dsc.header.h);
        lv_obj_remove_flag(lines, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lines, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(lines, NULL);
    }
}

#define MAX_MIRRORS 3
static struct { lv_obj_t *strip, *lines; } g_mirror[MAX_MIRRORS];
static int g_nmirror;

static void show_all(void)
{
    show_on(G.strip, G.lines);
    for (int i = 0; i < g_nmirror; i++) show_on(g_mirror[i].strip, g_mirror[i].lines);
}

static void apply(void)
{
    int ground = ui_look()->ground;
    if (!G.strip) {
        G.strip = layer(bz_ui_content());
        G.lines = layer(bz_ui_content());
        /* behind the pages, the lines over the light */
        lv_obj_move_background(G.lines);
        lv_obj_move_background(G.strip);
    }
    if (ground == UI_GROUND_PLAIN) {
        G.shown = UI_GROUND_PLAIN;
        show_all(); /* nothing points at the pictures before they go */
        buf(&G.strip_px, &G.strip_bytes, 0);
        buf(&G.lines_px, &G.lines_bytes, 0);
        return;
    }
    /* the light: as far down as it reaches (the whole screen for grain), a strip tiled across */
    int sw = ground == UI_GROUND_GRAIN ? GRAIN_W : STRIP_W;
    int sh = ground == UI_GROUND_GRAIN ? H : ground == UI_GROUND_AURORA ? H * 2 / 3 : H * 5 / 8;
    G.shown = UI_GROUND_PLAIN;
    show_all(); /* the buffers may move: nothing shows them meanwhile */
    if (!buf(&G.strip_px, &G.strip_bytes, (size_t)sw * sh * 2)) {
        ui_look_edit()->ground = UI_GROUND_PLAIN;
        return;
    }
    bake_strip(ground, sw, sh, (uint16_t *)G.strip_px);
    dsc_init(&G.strip_dsc, LV_COLOR_FORMAT_RGB565, sw, sh, 2, G.strip_px);
    if (ground == UI_GROUND_CONTOUR) {
        /* a quarter of the frame wide, so the slide lands on a whole number of tiles */
        int lw = W / 4, lh = H * 9 / 16;
        if (buf(&G.lines_px, &G.lines_bytes, (size_t)lw * lh)) {
            bake_lines(lw, lh, G.lines_px);
            dsc_init(&G.lines_dsc, LV_COLOR_FORMAT_A8, lw, lh, 1, G.lines_px);
        }
    } else {
        buf(&G.lines_px, &G.lines_bytes, 0);
    }
    G.shown = ground;
    show_all();
}

lv_obj_t *ui_ground_layer(lv_obj_t *parent)
{
    if (g_nmirror >= MAX_MIRRORS) return NULL;
    lv_obj_t *strip = layer(parent), *lines = layer(parent);
    lv_obj_move_background(lines);
    lv_obj_move_background(strip);
    g_mirror[g_nmirror].strip = strip;
    g_mirror[g_nmirror].lines = lines;
    g_nmirror++;
    if (G.strip) show_on(strip, lines);
    return strip;
}

static void theme_changed(void)
{
    if (G.shown > UI_GROUND_PLAIN) apply();
}

void ui_ground_init(void)
{
    if (!G.hooked) {
        G.hooked = true;
        bz_theme_on_change(theme_changed);
    }
    apply();
}

void ui_ground_set(int ground)
{
    if (ground < 0 || ground >= UI_GROUND_COUNT) return;
    ui_look_t *l = ui_look_edit();
    if (l->ground == ground && G.shown == ground) return;
    l->ground = ground;
    ui_look_save();
    apply();
    lv_obj_invalidate(bz_ui_content());
}

bool ui_ground_active(void) { return G.shown > UI_GROUND_PLAIN; }

size_t ui_ground_bytes(void) { return G.strip_bytes + G.lines_bytes; }

/* A chooser's picture of a ground, `w` x `h` RGB565: the screen scaled down, made the same way. */
void ui_ground_thumb(int ground, uint16_t *out, int w, int h)
{
    const uint32_t base = bz_pal->ground;
    const uint32_t c1 = glow_colour(), c2 = turn(c1);
    const bool dark = bz_ui_dark();
    const float amt = dark ? 1.0f : 0.75f;
    const float sx = (float)W / w, sy = (float)H / h;
    const float TP = 6.2831853f;
    for (int y = 0; y < h; y++) {
        float v = (y + 0.5f) / h;
        float k1, k2;
        light_at(ground, v, &k1, &k2);
        k1 *= amt;
        k2 *= amt;
        for (int x = 0; x < w; x++) {
            float r = bz_r(base), g = bz_g(base), b = bz_b(base);
            r += (bz_r(c1) - r) * k1 + (bz_r(c2) - r) * k2;
            g += (bz_g(c1) - g) * k1 + (bz_g(c2) - g) * k2;
            b += (bz_b(c1) - b) * k1 + (bz_b(c2) - b) * k2;
            if (ground == UI_GROUND_GRAIN) {
                float gr = ((int)(hash2(x, y) & 0xFF) - 128) / 128.0f * (dark ? 6.0f : 8.0f);
                r += gr;
                g += gr;
                b += gr;
            } else if (ground == UI_GROUND_CONTOUR && v < 9.0f / 16) {
                /* the same field, a line per step, drawn thin at this scale */
                float u = (x + 0.5f) * sx / (W / 4.0f);
                u -= floorf(u);
                float a1 = TP * u + 5.4f * v, a2 = TP * 2 * u - 5.1f * v + 0.8f, a3 = TP * 3 * u + 7.3f * v;
                float f = 0.55f * sinf(a1) + 0.35f * sinf(a2) + 0.22f * cosf(a3) + 1.6f * v;
                float s = f * 9.0f, fr = s - floorf(s);
                float line = 1 - fminf(fr, 1 - fr) / (0.06f * sy / 8);
                float fade = 1 - v / (9.0f / 16);
                if (line > 0) {
                    float k = line * fade * fade * 0.47f;
                    r += (bz_r(c1) - r) * k;
                    g += (bz_g(c1) - g) * k;
                    b += (bz_b(c1) - b) * k;
                }
            }
            out[y * w + x] = px565(r, g, b, x, y);
        }
    }
}
