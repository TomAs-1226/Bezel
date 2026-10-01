/* App icons: each app a squircle with its Material Symbols glyph.
 *
 * The colour (the default style, "glass"). The tile is Bezel's own surface with a breath of the app's family in it,
 * and the colour lives in the glyph: luminous, two-tone (SF Symbols' hierarchical pair: the outline at full tone,
 * the inside of the glyph -- the filled symbol less the outlined one -- at a third of it), a soft glow of itself on
 * the tile. In light tone the tile goes near-white and the glyph deep. Every app belongs to a family by its group
 * in the library, and the families are a harmony rather than a rainbow: a warm pair against a cool pair, as
 * Bezel's signal stands against its ice and its neutrals --
 *   ember (OKLCH h 46)  the robot        steel (h 246)  diagnosing it
 *   jade  (h 190)       this tablet      rose  (h 350)  everyday
 * Within a family each app turns a little (±13°) and lightens a little (±0.03) by a low-discrepancy walk, so
 * neighbours in the library differ and the family still reads as one. All of it is computed in OKLCH: every
 * gradient is a table of perceptual colours, and its hue turns as its lightness falls -- warm (toward yellow)
 * where the light from the top left falls, cool (toward blue-violet) in the shade -- the way light and shadow
 * shift a real colour. Chroma is weighted per family (a warm hue at low chroma is brown; teal leaves sRGB first).
 * tools: tab5-shots/icons/proto.py renders the same icons the same way (the owner chose this, direction B).
 *
 * Tinted is Material You's themed icons from the accent (a deep container of it and its pastel glyph in dark, the
 * reverse in light); mono is Bezel's surface and its ink. Both two-tone, both follow the tone and the accent.
 *
 * The cost. Each (app, size, what it sits on) is baked once into PSRAM and every place that shows it shares it;
 * LVGL only ever blits it. An icon on a known opaque surface (the library's drawer, a settings pane) is baked onto
 * that surface as plain RGB565: 2 bytes a pixel (8 KB at 64 px) and drawn as a copy, not a per-pixel blend.
 * Elsewhere (the ground, which can be a picture; glass; a tinted card) it is RGB565A8, 3 bytes a pixel, and one
 * picture serves all of them: the home page's quick row and the control center's favourites are the same six. A press shows the
 * surface3 platter under a cell: for that instant the icon swaps to a copy whose corners are the platter's
 * (ui_icon_press_with), fixed up from the resting one in microseconds. A change of style, tone or accent
 * bakes the same buffers again in place and the caller's one redraw shows it. Nothing here runs per frame. */
#include "ui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */
#include "ui_home_mode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ the set */

enum { ROBOT, DIAG, TABLET, EVERYDAY };

typedef struct {
    const char *label; /* the library's label, or the app's window name */
    const char *glyph;
    uint8_t group;
} icon_def_t;

static const icon_def_t DEFS[] = {
    { "preflight", BZ_I_CHECKLIST, ROBOT },
    { "alerts", BZ_I_WARNING, ROBOT },
    { "tune", BZ_I_TUNE, ROBOT },
    { "auto", BZ_I_SPORTS_SCORE, ROBOT },
    { "field", BZ_I_STADIUM, ROBOT },
    { "robot", BZ_I_SMART_TOY, ROBOT },
    { "motors", BZ_I_MODE_FAN, ROBOT },
    { "states", BZ_I_ACCOUNT_TREE, ROBOT },
    { "controls", BZ_I_SPORTS_ESPORTS, ROBOT },
    { "blue alliance", BZ_I_LEADERBOARD, ROBOT },
    { "batteries", BZ_I_BATTERY_CHARGING_FULL, ROBOT },
    { "systemcore", BZ_I_DEVELOPER_BOARD, DIAG },
    { "can tap", BZ_I_CABLE, DIAG },
    { "recorder", BZ_I_FIBER_MANUAL_RECORD, DIAG },
    { "logs", BZ_I_RECEIPT_LONG, DIAG },
    { "level", BZ_I_ADJUST, DIAG },
    { "lens", BZ_I_PHOTO_CAMERA, DIAG },
    { "gearing", BZ_I_SPEED, DIAG },
    { "ruler", BZ_I_STRAIGHTEN, DIAG },
    { "reference", BZ_I_MENU_BOOK, DIAG },
    { "assist", BZ_I_AUTO_AWESOME, TABLET },
    { "insights", BZ_I_INSIGHTS, TABLET },
    { "link", BZ_I_COMPUTER, TABLET },
    { "pair pc", BZ_I_LINK, TABLET },
    { "companion", BZ_I_VISIBILITY, TABLET },
    { "home mode", BZ_I_HOME, TABLET },
    { "timer", BZ_I_TIMER, TABLET },
    { "calculator", BZ_I_CALCULATE, TABLET },
    { "notes", BZ_I_EDIT_NOTE, TABLET },
    { "checklist", BZ_I_CHECKLIST_RTL, TABLET },
    { "flashlight", BZ_I_FLASHLIGHT_ON, TABLET },
    { "files", BZ_I_FOLDER, TABLET },
    { "system", BZ_I_MONITORING, TABLET },
    { "settings", BZ_I_SETTINGS, TABLET },
    { "clock", BZ_I_ALARM, EVERYDAY },
    { "calendar", BZ_I_CALENDAR_MONTH, EVERYDAY },
    { "documents", BZ_I_ARTICLE, EVERYDAY },
    { "photos", BZ_I_PHOTO_LIBRARY, EVERYDAY },
    { "storage", BZ_I_SD_CARD, EVERYDAY },
    { "music", BZ_I_MUSIC_NOTE, EVERYDAY },
    { "smart home", BZ_I_LIGHTBULB, EVERYDAY },
    { "weather", BZ_I_PARTLY_CLOUDY_DAY, EVERYDAY },
};
#define NDEFS ((int)(sizeof DEFS / sizeof DEFS[0]))

/* the families: centre hue, how far an app may turn from it, and a chroma weight that gives each the same presence */
static const struct { float h, spread, chroma; } FAMILY[4] = {
    { 46.0f, 13.0f, 1.18f },  /* ember */
    { 246.0f, 14.0f, 1.00f }, /* steel */
    { 190.0f, 13.0f, 0.86f }, /* jade */
    { 350.0f, 13.0f, 1.06f }, /* rose */
};

static int def_by_label(const char *label)
{
    if (!label) return -1;
    for (int i = 0; i < NDEFS; i++)
        if (!strcmp(DEFS[i].label, label)) return i;
    return -1;
}

/* an app's place in its family: a golden-ratio walk over [-1, 1] by its order in the group */
static float walk(int i) { return fmodf(i * 0.6180340f + 0.31f, 1.0f) * 2 - 1; }

static void family_of(int def, float *h, float *dl, float *cw)
{
    int g = DEFS[def].group, i = 0;
    for (int k = 0; k < def; k++) i += DEFS[k].group == g;
    *h = fmodf(FAMILY[g].h + walk(i) * FAMILY[g].spread + 360.0f, 360.0f);
    *dl = walk(i + 3) * 0.03f;
    *cw = FAMILY[g].chroma;
}

/* ------------------------------------------------------------------ OKLCH */

typedef struct { float L, C, h; } lch_t;

static float lin_to_srgb(float x)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return (x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f) * 255.0f;
}

static float srgb_to_lin(int c)
{
    float x = c / 255.0f;
    return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

static bool oklab_lin(float L, float a, float b, float out[3])
{
    float l = L + 0.3963377774f * a + 0.2158037573f * b;
    float m = L - 0.1055613458f * a - 0.0638541728f * b;
    float s = L - 0.0894841775f * a - 1.2914855480f * b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    const float e = 1e-4f;
    return out[0] >= -e && out[0] <= 1 + e && out[1] >= -e && out[1] <= 1 + e && out[2] >= -e && out[2] <= 1 + e;
}

/* OKLCH to sRGB 0..255, the chroma reduced until it fits (the hue and lightness kept) */
static void oklch_rgb(lch_t c, float rgb[3])
{
    float hr = c.h * (float)M_PI / 180.0f, ca = cosf(hr), sa = sinf(hr), lin[3];
    if (!oklab_lin(c.L, c.C * ca, c.C * sa, lin)) {
        float lo = 0, hi = c.C;
        for (int i = 0; i < 12; i++) {
            float mid = (lo + hi) * 0.5f;
            if (oklab_lin(c.L, mid * ca, mid * sa, lin)) lo = mid;
            else hi = mid;
        }
        oklab_lin(c.L, lo * ca, lo * sa, lin);
    }
    for (int k = 0; k < 3; k++) rgb[k] = lin_to_srgb(lin[k]);
}

static lch_t rgb_lch(uint32_t c)
{
    float r = srgb_to_lin(bz_r(c)), g = srgb_to_lin(bz_g(c)), b = srgb_to_lin(bz_b(c));
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    float L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    float h = atan2f(B, A) * 180.0f / (float)M_PI;
    return (lch_t){ L, sqrtf(A * A + B * B), h < 0 ? h + 360 : h };
}

/* a hue moved toward another along the short way round, by at most `deg` */
static float toward(float h, float target, float deg)
{
    float d = fmodf(target - h + 540.0f, 360.0f) - 180.0f;
    if (fabsf(d) <= deg) return target;
    return fmodf(h + (d > 0 ? deg : -deg) + 360.0f, 360.0f);
}
static float lit(float h, float deg) { return toward(h, 85, deg); }   /* the light leans warm */
static float shade(float h, float deg) { return toward(h, 275, deg); } /* the shade leans cool */

#define RAMP 64
/* a gradient from a to b in OKLCH, the hue the short way round */
static void ramp(float out[RAMP][3], lch_t a, lch_t b)
{
    float dh = fmodf(b.h - a.h + 540.0f, 360.0f) - 180.0f;
    for (int i = 0; i < RAMP; i++) {
        float t = i / (float)(RAMP - 1);
        oklch_rgb((lch_t){ a.L + (b.L - a.L) * t, a.C + (b.C - a.C) * t, a.h + dh * t }, out[i]);
    }
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
            float lit_ = rim * (face > 0 ? 0.55f * face : 0.35f * face);
            int e = (int)lrintf(lit_ * 127);
            sh->edge[y * s + x] = (int8_t)(e < -127 ? -127 : e > 127 ? 127 : e);
        }
    return sh;
}

/* ------------------------------------------------------------------ the glyph */

static uint32_t utf8_cp(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0) return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
}

/* The glyph's coverage into `out` (s x s) at an em of `em` px, the em square centred (where the symbol's designer
 * placed it, which the two faces share, so the outlined and the filled glyph land on each other). From the 32 or
 * the 40 px face, the smallest at least that big, both of which exist filled and outlined; bilinear, with four
 * taps a pixel when it shrinks. */
static void glyph_mask(uint8_t *out, int s, const char *glyph, float em, bool filled, int bb[4])
{
    memset(out, 0, (size_t)s * s);
    const lv_font_t *font = em > 32.5f ? (filled ? &bz_icons_fill_40 : &bz_icons_outline_40)
                                       : (filled ? &bz_icons_fill_32 : &bz_icons_outline_32);
    int px = em > 32.5f ? 40 : 32;
    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof g);
    if (!lv_font_get_glyph_dsc(font, &g, utf8_cp(glyph), 0) || g.box_w <= 0 || g.box_h <= 0) return;
    lv_draw_buf_t *db = lv_draw_buf_create(g.box_w, g.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!db) return;
    /* it returns the draw buffer it filled (A8, db's stride), not the pixels */
    const uint8_t *bm = lv_font_get_glyph_bitmap(&g, db) ? db->data : NULL;
    if (!bm) {
        lv_draw_buf_destroy(db);
        return;
    }
    int stride = (int)db->header.stride;
    float k = em / px;
    /* the em square spans the baseline up to one em: the box's top is box_h + ofs_y above the baseline */
    float ex = (s - em) * 0.5f, ey = (s - em) * 0.5f;
    float x0 = ex + g.ofs_x * k, y0 = ey + (px - g.box_h - g.ofs_y) * k;
    float gw = g.box_w * k, gh = g.box_h * k;
    int taps = k < 0.85f ? 2 : 1;
    /* the part of the picture it can reach, for whoever blurs it afterwards */
    int bx0 = (int)floorf(x0) - 1, by0 = (int)floorf(y0) - 1, bx1 = (int)ceilf(x0 + gw) + 1, by1 = (int)ceilf(y0 + gh) + 1;
    if (bx0 < bb[0]) bb[0] = bx0 < 0 ? 0 : bx0;
    if (by0 < bb[1]) bb[1] = by0 < 0 ? 0 : by0;
    if (bx1 > bb[2]) bb[2] = bx1 > s - 1 ? s - 1 : bx1;
    if (by1 > bb[3]) bb[3] = by1 > s - 1 ? s - 1 : by1;
    const float ik = 1.0f / k;
    for (int y = (int)floorf(y0) - 1; y <= (int)ceilf(y0 + gh) + 1; y++) {
        if (y < 0 || y >= s) continue;
        for (int x = (int)floorf(x0) - 1; x <= (int)ceilf(x0 + gw) + 1; x++) {
            if (x < 0 || x >= s) continue;
            int acc = 0;
            for (int ty = 0; ty < taps; ty++)
                for (int tx = 0; tx < taps; tx++) {
                    /* the output pixel's (sub)sample in the glyph's own pixels */
                    float u = (x + (tx + 0.5f) / taps - x0) * ik - 0.5f, v = (y + (ty + 0.5f) / taps - y0) * ik - 0.5f;
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

/* A box blur of radius 2 over the rectangle x0..x1, y0..y1 of an s-wide mask, rows then columns, in place (tmp: s*s);
 * outside it the mask is zero and stays so. /5 as a multiply and a shift (exact for sums up to 1275). */
static void box5(uint8_t *m, uint8_t *tmp, int s, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++) {
        const uint8_t *r = m + y * s;
        for (int x = x0; x <= x1; x++) {
            int a = 0;
            for (int d = -2; d <= 2; d++) {
                int xx = x + d;
                if (xx >= x0 && xx <= x1) a += r[xx];
            }
            tmp[y * s + x] = (uint8_t)((a * 13108) >> 16);
        }
    }
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++) {
            int a = 0;
            for (int d = -2; d <= 2; d++) {
                int yy = y + d;
                if (yy >= y0 && yy <= y1) a += tmp[yy * s + x];
            }
            m[y * s + x] = (uint8_t)((a * 13108) >> 16);
        }
}

/* ------------------------------------------------------------------ the styles */

typedef struct {
    lch_t t0, t1;       /* the tile: where the light falls, and in the shade */
    lch_t i0, i1;       /* the glyph: its top, its foot */
    float second;       /* the glyph's inside, as a part of its colour */
    float sheen, rim, glow;
} look_t;

static look_t look_for(int def, int style, bool dark)
{
    look_t k;
    if (style == UI_ICON_TINTED) {
        /* the accent's pair: its hue at the accent's own chroma, capped so no accent shouts */
        lch_t a = rgb_lch(BZ_ACCENTS[bz_ui_accent()].dark);
        float h = a.h, c = a.C;
        if (dark) {
            k.t0 = (lch_t){ 0.385f, fminf(c * 0.42f, 0.05f), lit(h, 8) };
            k.t1 = (lch_t){ 0.275f, fminf(c * 0.42f, 0.05f), shade(h, 12) };
            k.i0 = (lch_t){ 0.915f, fminf(c * 0.85f, 0.10f), lit(h, 8) };
            k.i1 = (lch_t){ 0.805f, fminf(c * 1.10f, 0.13f), shade(h, 8) };
            k.second = 0.34f, k.sheen = 0.06f, k.rim = 1.2f, k.glow = 0.12f;
        } else {
            k.t0 = (lch_t){ 0.965f, fminf(c * 0.40f, 0.04f), lit(h, 8) };
            k.t1 = (lch_t){ 0.895f, fminf(c * 0.55f, 0.06f), shade(h, 12) };
            k.i0 = (lch_t){ 0.43f, fminf(c * 1.2f, 0.12f), lit(h, 8) };
            k.i1 = (lch_t){ 0.34f, fminf(c * 1.2f, 0.12f), shade(h, 8) };
            k.second = 0.26f, k.sheen = 0.03f, k.rim = 0.9f, k.glow = 0;
        }
        return k;
    }
    if (style == UI_ICON_MONO) {
        /* Bezel's surface and its ink, in its own cool neutral */
        if (dark) {
            k.t0 = (lch_t){ 0.335f, 0.010f, 252 };
            k.t1 = (lch_t){ 0.255f, 0.009f, 258 };
            k.i0 = (lch_t){ 0.955f, 0.004f, 250 };
            k.i1 = (lch_t){ 0.895f, 0.006f, 250 };
            k.second = 0.30f, k.sheen = 0.06f, k.rim = 1.2f, k.glow = 0;
        } else {
            k.t0 = (lch_t){ 0.995f, 0.003f, 252 };
            k.t1 = (lch_t){ 0.925f, 0.008f, 258 };
            k.i0 = (lch_t){ 0.30f, 0.012f, 255 };
            k.i1 = (lch_t){ 0.22f, 0.012f, 255 };
            k.second = 0.22f, k.sheen = 0, k.rim = 0.9f, k.glow = 0;
        }
        return k;
    }
    float h, dl, cw;
    family_of(def, &h, &dl, &cw);
    if (dark) {
        k.t0 = (lch_t){ 0.385f, 0.026f, lit(h, 10) };
        k.t1 = (lch_t){ 0.262f, 0.021f, shade(h, 18) };
        k.i0 = (lch_t){ 0.925f + dl, 0.110f * cw, lit(h, 12) };
        k.i1 = (lch_t){ 0.775f + dl, 0.160f * cw, shade(h, 12) };
        k.second = 0.34f, k.sheen = 0.07f, k.rim = 1.25f, k.glow = 0.16f;
    } else {
        k.t0 = (lch_t){ 0.998f, 0.004f, lit(h, 10) };
        k.t1 = (lch_t){ 0.935f, 0.016f * cw, shade(h, 18) };
        k.i0 = (lch_t){ 0.66f + dl, 0.150f * cw, lit(h, 12) };
        k.i1 = (lch_t){ 0.51f + dl, 0.165f * cw, shade(h, 12) };
        k.second = 0.24f, k.sheen = 0, k.rim = 0.9f, k.glow = 0;
    }
    return k;
}

/* ------------------------------------------------------------------ baking */

#define BG_NONE (-1)

typedef struct {
    int def, size;
    int style;          /* the style it was baked in (previews keep theirs) */
    int bg;             /* the colour role it sits on (baked onto it, opaque), or BG_NONE: alpha */
    bool fixed;         /* a preview: its own style, whatever the setting */
    lv_image_dsc_t dsc;
    uint8_t *px;        /* RGB565 plane, then (alpha pictures) the A8 plane */
} entry_t;

#define MAX_ENTRIES 160
static entry_t g_entries[MAX_ENTRIES];
static int g_nentries;
static int g_style = UI_ICON_COLOUR;
static bool g_hooked;
static void icon_load(void);

/* the pixel the panel holds for a role: what LVGL writes for its (565-rounded) colour */
static uint16_t role565(int role)
{
    lv_color_t c = bz_lv((bz_color_role_t)role);
    return (uint16_t)((c.red & 0xF8) << 8 | (c.green & 0xFC) << 3 | c.blue >> 3);
}

static bool opaque_on(int bg) { return bg != BG_NONE; }

static void bake(entry_t *e)
{
    const int s = e->size;
    const size_t n = (size_t)s * s;
    const shape_t *sh = shape_for(s);
    const bool opaque = opaque_on(e->bg);
    uint16_t *rgb = (uint16_t *)e->px;
    uint8_t *alpha = e->px + n * 2;
    e->dsc.header.cf = opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
    e->dsc.data_size = (uint32_t)(n * (opaque ? 2 : 3));
    lv_image_cache_drop(&e->dsc);
    const uint16_t bgpx = opaque ? role565(e->bg) : 0;
    /* the masks and the two colour tables, in one PSRAM block for the bake */
    uint8_t *m = sh ? lv_malloc(n * 4 + sizeof(float) * RAMP * 3 * 2) : NULL;
    if (!m) {
        for (size_t i = 0; i < n; i++) rgb[i] = bgpx;
        if (!opaque) memset(alpha, 0, n);
        return;
    }
    const look_t k = look_for(e->def, e->style, bz_ui_dark());
    float(*tile)[3] = (float(*)[3])(m + n * 4), (*ink)[3] = tile + RAMP; /* n * 4 keeps floats aligned */
    ramp(tile, k.t0, k.t1);
    ramp(ink, k.i0, k.i1);

    /* the glyph: outlined (its lines) and filled; the inside is the difference; the glow is the whole of it, soft */
    uint8_t *fo = m, *ff = m + n, *gl = m + 2 * n, *tmp = m + 3 * n;
    const float em = s * 0.58f;
    int bb[4] = { s, s, -1, -1 };
    glyph_mask(fo, s, DEFS[e->def].glyph, em, false, bb);
    glyph_mask(ff, s, DEFS[e->def].glyph, em, true, bb);
    if (k.glow > 0 && bb[2] >= bb[0]) {
        /* three passes spread it 6 px: blur only the glyph's box grown by that */
        int x0 = bb[0] - 6 < 0 ? 0 : bb[0] - 6, y0 = bb[1] - 6 < 0 ? 0 : bb[1] - 6;
        int x1 = bb[2] + 6 > s - 1 ? s - 1 : bb[2] + 6, y1 = bb[3] + 6 > s - 1 ? s - 1 : bb[3] + 6;
        memset(gl, 0, n);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int i = y * s + x;
                gl[i] = fo[i] > ff[i] ? fo[i] : ff[i];
            }
        for (int p = 0; p < 3; p++) box5(gl, tmp, s, x0, y0, x1, y1); /* ~ a gaussian of sigma 2.5 */
    }

    const float bgr = opaque ? (float)((bgpx >> 11 & 31) * 255 / 31) : 0;
    const float bgg = opaque ? (float)((bgpx >> 5 & 63) * 255 / 63) : 0;
    const float bgb = opaque ? (float)((bgpx & 31) * 255 / 31) : 0;
    /* the ordered dither's thresholds, as fractions of a 565 step */
    static const float BAYER[16] = { 0 / 16.f, 8 / 16.f, 2 / 16.f, 10 / 16.f, 12 / 16.f, 4 / 16.f, 14 / 16.f, 6 / 16.f,
                                     3 / 16.f, 11 / 16.f, 1 / 16.f, 9 / 16.f, 15 / 16.f, 7 / 16.f, 13 / 16.f, 5 / 16.f };
    const float is = 1.0f / s, i127 = k.rim / 127.0f, i255 = 1.0f / 255.0f, isd = 1.0f / 0.85f;
    const float glow = k.glow * i255, second = k.second * i255, K5 = 31.0f / 255.0f, K6 = 63.0f / 255.0f;
    for (int y = 0; y < s; y++) {
        const float v = (y + 0.5f) * is;
        /* the glyph's colour runs down the glyph's own height, not the tile's */
        float gv = (v - 0.21f) / 0.58f;
        gv = gv < 0 ? 0 : gv > 1 ? 1 : gv;
        const float *ic = ink[(int)(gv * (RAMP - 1) + 0.5f)];
        for (int x = 0; x < s; x++) {
            const int i = y * s + x;
            const int cov = sh->cov[i];
            if (!cov) {
                rgb[i] = bgpx;
                if (!opaque) alpha[i] = 0;
                continue;
            }
            const float u = (x + 0.5f) * is;
            /* lit from the top left: mostly down the icon, a little across */
            float t = 0.30f * u + 0.70f * v;
            const float *tc = tile[(int)(t * (RAMP - 1) + 0.5f)];
            float r = tc[0], g = tc[1], b = tc[2];
            /* a soft sheen near the light */
            if (k.sheen > 0) {
                float dx = u - 0.18f, dy = v - 0.02f;
                float sd = 1 - sqrtf(dx * dx + dy * dy) * isd;
                if (sd > 0) {
                    float q = k.sheen * sd * sd;
                    r += (255 - r) * q;
                    g += (255 - g) * q;
                    b += (255 - b) * q;
                }
            }
            /* the rim: light off the top edge, a shade along the bottom (baked glass, as the dock's) */
            float el = sh->edge[i] * i127;
            if (el > 0) {
                float q = 0.38f * el;
                r += (255 - r) * q;
                g += (255 - g) * q;
                b += (255 - b) * q;
            } else if (el < 0) {
                float q = 1 + 0.30f * el;
                r *= q;
                g *= q;
                b *= q;
            }
            /* the glyph's light on the tile, its inside, its lines */
            float q = k.glow > 0 ? glow * gl[i] : 0;
            int inside = ff[i] - fo[i];
            if (inside > 0) q += (1 - q) * second * inside;
            if (fo[i]) q += (1 - q) * fo[i] * i255;
            if (q > 0) {
                r += (ic[0] - r) * q;
                g += (ic[1] - g) * q;
                b += (ic[2] - b) * q;
            }
            if (opaque && cov < 255) {
                /* onto the surface it sits on */
                float a = cov * i255;
                r = bgr + (r - bgr) * a;
                g = bgg + (g - bgg) * a;
                b = bgb + (b - bgb) * a;
            }
            float bay = BAYER[((y & 3) << 2) | (x & 3)];
            int r5 = (int)(r * K5 + bay), g6 = (int)(g * K6 + bay), b5 = (int)(b * K5 + bay);
            r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
            g6 = g6 < 0 ? 0 : g6 > 63 ? 63 : g6;
            b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
            rgb[i] = (uint16_t)(r5 << 11 | g6 << 5 | b5);
            if (!opaque) alpha[i] = (uint8_t)cov;
        }
    }
    lv_free(m);
}

/* ------------------------------------------------------------------ the press: the icon on the platter */

/* One copy for whichever icon is under the finger (one finger, one press): the resting picture with the pixels
 * at its edge moved from the surface it rests on to the platter's -- out = icon*a + bg*(1-a), so the copy is
 * out + (platter - bg)*(1 - a), exact, no icon colour needed. A memcpy and a few hundred pixels. */
static struct {
    entry_t e;
    size_t cap;
    lv_obj_t *img;      /* showing it now */
    entry_t *from;      /* whose copy it is */
} g_press;

static void press_bake(entry_t *src)
{
    const int s = src->size;
    const size_t n = (size_t)s * s;
    if (g_press.cap < n * 2) {
        lv_free(g_press.e.px);
        g_press.e.px = lv_malloc(n * 2);
        g_press.cap = g_press.e.px ? n * 2 : 0;
        if (!g_press.e.px) return;
    }
    const shape_t *sh = shape_for(s);
    if (!sh) return;
    memcpy(g_press.e.px, src->px, n * 2);
    uint16_t *px = (uint16_t *)g_press.e.px;
    uint16_t a565 = role565(src->bg), b565 = role565(BZ_C_SURFACE3);
    int dr = (b565 >> 11 & 31) - (a565 >> 11 & 31), dg = (b565 >> 5 & 63) - (a565 >> 5 & 63), db = (b565 & 31) - (a565 & 31);
    for (size_t i = 0; i < n; i++) {
        int c = sh->cov[i];
        if (c == 255) continue;
        int w = 255 - c;
        int r = (px[i] >> 11 & 31) + (dr * w + (dr >= 0 ? 127 : -127)) / 255;
        int g = (px[i] >> 5 & 63) + (dg * w + (dg >= 0 ? 127 : -127)) / 255;
        int b = (px[i] & 31) + (db * w + (db >= 0 ? 127 : -127)) / 255;
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 63 ? 63 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
        px[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
    lv_image_dsc_t *d = &g_press.e.dsc;
    *d = src->dsc;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->data_size = (uint32_t)(n * 2);
    d->data = g_press.e.px;
    lv_image_cache_drop(d);
    g_press.from = src;
}

/* the cell's pressed state is what draws the platter: the icon follows it exactly, however it ends (a release, a
 * press lost, a drag that takes the finger) */
static void press_cb(lv_event_t *ev)
{
    lv_obj_t *cell = lv_event_get_current_target(ev);
    lv_obj_t *img = lv_event_get_user_data(ev);
    entry_t *e = lv_obj_get_user_data(img);
    bool down = lv_obj_has_state(cell, LV_STATE_PRESSED);
    if (down == (g_press.img == img)) return;
    if (down) {
        if (!e || !opaque_on(e->bg) || e->bg == BZ_C_SURFACE3) return;
        if (g_press.img && g_press.from) lv_image_set_src(g_press.img, &g_press.from->dsc);
        g_press.img = NULL;
        press_bake(e);
        if (g_press.from != e) return;
        lv_image_set_src(img, &g_press.e.dsc);
        g_press.img = img;
    } else {
        if (e) lv_image_set_src(img, &e->dsc);
        g_press.img = NULL;
    }
}

static void press_gone(lv_event_t *ev)
{
    if (g_press.img == lv_event_get_user_data(ev)) g_press.img = NULL;
}

void ui_icon_press_with(lv_obj_t *cell, lv_obj_t *icon)
{
    if (!cell || !icon) return;
    lv_obj_add_event_cb(cell, press_cb, LV_EVENT_STATE_CHANGED, icon);
    lv_obj_add_event_cb(icon, press_gone, LV_EVENT_DELETE, icon);
}

/* ------------------------------------------------------------------ the entries */

static void rebake_all(void)
{
    if (g_press.img && g_press.from) {
        lv_image_set_src(g_press.img, &g_press.from->dsc);
        g_press.img = NULL;
    }
    double t0 = bz_ui_clock();
    for (int i = 0; i < g_nentries; i++) {
        entry_t *e = &g_entries[i];
        if (!e->fixed) e->style = g_style;
        bake(e);
    }
    printf("icons: %d baked in %.0f ms (%u bytes)\n", g_nentries, (bz_ui_clock() - t0) * 1000,
           (unsigned)ui_icon_bytes());
}

static void theme_changed(void) { rebake_all(); }

static entry_t *entry_for(int def, int size, int style, bool fixed, int bg)
{
    if (def < 0 || size < 8 || size > 160) return NULL;
    for (int i = 0; i < g_nentries; i++) {
        entry_t *e = &g_entries[i];
        if (e->def == def && e->size == size && e->fixed == fixed && e->bg == bg && (!fixed || e->style == style))
            return e;
    }
    if (g_nentries >= MAX_ENTRIES) return NULL;
    if (!g_hooked) {
        g_hooked = true;
        icon_load();
        bz_theme_on_change(theme_changed);
    }
    entry_t *e = &g_entries[g_nentries];
    /* a surface under it: two bytes a pixel; anything else carries its alpha */
    size_t n = (size_t)size * size * (bg == BG_NONE ? 3 : 2);
    e->px = lv_malloc(n);
    if (!e->px) return NULL;
    g_nentries++;
    e->def = def;
    e->size = size;
    e->style = fixed ? style : g_style; /* g_style as loaded, which the first call's argument predates */
    e->fixed = fixed;
    e->bg = bg;
    e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.w = (uint32_t)size;
    e->dsc.header.h = (uint32_t)size;
    e->dsc.header.stride = (uint32_t)size * 2;
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
    lv_obj_set_user_data(img, e);
    if (e) lv_image_set_src(img, &e->dsc);
    lv_obj_set_size(img, size, size);
    return img;
}

const lv_image_dsc_t *ui_icon_image(const ui_app_t *app, int size)
{
    entry_t *e = entry_for(def_for_app(app), size, g_style, false, BG_NONE);
    return e ? &e->dsc : NULL;
}

lv_obj_t *ui_app_icon(lv_obj_t *parent, const ui_app_t *app, int size)
{
    return icon_obj(parent, entry_for(def_for_app(app), size, g_style, false, BG_NONE), size);
}

lv_obj_t *ui_app_icon_on(lv_obj_t *parent, const ui_app_t *app, int size, int bg)
{
    return icon_obj(parent, entry_for(def_for_app(app), size, g_style, false, bg < 0 ? BG_NONE : bg), size);
}

lv_obj_t *ui_icon_named(lv_obj_t *parent, const char *name, int size)
{
    int d = def_by_label(name);
    if (d < 0) {
        const ui_app_t *a = ui_app_find(name);
        d = def_for_app(a);
    }
    if (d < 0) return NULL;
    return icon_obj(parent, entry_for(d, size, g_style, false, BG_NONE), size);
}

lv_obj_t *ui_icon_preview(lv_obj_t *parent, const char *name, int size, int style)
{
    int d = def_by_label(name);
    if (d < 0) return NULL;
    return icon_obj(parent, entry_for(d, size, style, true, BG_NONE), size);
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
    size_t n = g_press.cap;
    for (int i = 0; i < g_nentries; i++) {
        const entry_t *e = &g_entries[i];
        n += (size_t)e->size * e->size * (e->bg == BG_NONE ? 3 : 2);
    }
    for (int i = 0; i < MAX_SHAPES; i++) n += (size_t)g_shapes[i].size * g_shapes[i].size * 2;
    return n;
}
