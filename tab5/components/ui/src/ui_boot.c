/* ui_boot — the Catalyst start-up screen (ui_boot.h).
 *
 * The drawing is the family banner's (FrcCatalyst/docs/assets/banner.svg, 1200 × 340), laid out full
 * screen at 1.16× with its tokens unchanged: the ground's diagonal #191a20 → #101116 → #08090b, the
 * crimson key light (#e94560 at 19 %) right of centre and a 7 % white rim light top-left, the bolt, the
 * wordmark as a light grey first word and a bold white second, the 300-wide crimson hairline, the bold
 * tagline, the grey line under it, the version pill and its letterspaced caption, and the version as a
 * 3 % ghost numeral. The banner's line is a trajectory arriving at a robot, Console's is telemetry
 * arriving at a panel; this one is telemetry arriving at the tablet, and it is the progress bar: the
 * line draws toward the Tab5 as start-up goes, and reaching it means ready.
 *
 * Entrances are springs (Detent's release/smooth, closed form) staggered the way the eye reads the
 * card: the bolt strikes, the wordmark rises letter by letter, the hairline draws out, the rest settles
 * in under it. Nothing loops but a pulse travelling up the line and the pill's dot breathing.
 *
 * The ground and the ghost numeral are drawn once into a copy of the screen; every frame restores what
 * the last one drew from it and draws the moving parts in a tracked box. Blends dither their fraction
 * (RGB565 would band a dark gradient into rings). */
#include "ui_boot.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_fonts.h"
#include "bz_tokens.h"
#include "lvgl.h"

#define TAU 6.28318530718f
#define MAXGLYPHS 64
#define INTRO_S 1.9   /* the outro never starts before the card is in */
#define ARRIVE_S 0.45 /* the line reaching the tablet, the tablet lighting */
#define OUTRO_S 0.45  /* the card fading to the UI's ground */

/* the banner's own colours */
#define C_CRIMSON 0xE94560u
#define C_LINE_HI 0xFF5D76u
#define C_LINE_END 0xFF6B81u
#define C_DOT 0xFF8FA0u
#define C_PILL_TXT 0xFF92A3u
#define C_BUMPER 0x8E2739u
#define C_WORD_LIGHT 0x8D8F97u
#define C_WHITE 0xFFFFFFu
#define C_TAGLINE 0xE6E7EAu
#define C_SPEC 0x83858Du
#define C_CAPTION 0x6B6D75u
#define C_FRAME 0x14151Au
#define C_FRAME_EDGE 0x3D404Au
#define C_SCREEN 0x0C0D11u

/* banner px → screen px: sizes and the text column at 1.16, the line's x a little tighter so the
 * tablet sits where the banner's robot sits, at five sixths of the width */
#define S 1.16f
#define TX(x) (12 + (x) * S)
#define TY(y) (155 + (y) * S)
#define LX(x) (12 + (x) * 1.07f)

/* ------------------------------------------------------------------ text, laid out on the LVGL thread */

typedef struct {
    const uint8_t *bm;   /* 4 bpp, rows packed without padding, high nibble first */
    int16_t x, y, w, h;  /* top-left relative to the line's start and baseline */
} glyph_t;

typedef struct {
    glyph_t g[MAXGLYPHS];
    int n, width;
} line_t;

/* `track`: extra advance per glyph in px (the banner's letter-spacing) */
static void layout(line_t *l, const lv_font_t *font, const char *text, int track)
{
    memset(l, 0, sizeof *l);
    int x = 0;
    const char *p = text;
    while (*p && l->n < MAXGLYPHS) {
        uint32_t cp = (uint8_t)*p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
        }
        uint32_t next = (uint8_t)*p;
        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(font, &d, cp, next)) continue;
        const lv_font_t *rf = d.resolved_font ? d.resolved_font : font;
        const lv_font_fmt_txt_dsc_t *fd = rf->dsc;
        if (fd && d.box_w && d.box_h && fd->bpp == 4) {
            const lv_font_fmt_txt_glyph_dsc_t *gd = &fd->glyph_dsc[d.gid.index];
            glyph_t *g = &l->g[l->n++];
            g->bm = &fd->glyph_bitmap[gd->bitmap_index];
            g->x = (int16_t)(x + d.ofs_x);
            g->y = (int16_t)(-(d.ofs_y + d.box_h));
            g->w = (int16_t)d.box_w;
            g->h = (int16_t)d.box_h;
        }
        x += d.adv_w + (*p ? track : 0);
    }
    l->width = x;
}

static int glyph_v(const glyph_t *g, int x, int y)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
    int idx = y * g->w + x;
    uint8_t byte = g->bm[idx >> 1];
    return (idx & 1) ? (byte & 15) : (byte >> 4);
}

/* ------------------------------------------------------------------ state */

struct ui_boot {
    uint16_t *buf, *bg;
    uint8_t *cov, *pos;   /* a stroke's coverage (max, not summed: joints don't double) and where along it */
    bz_area_t sbox;       /* what the stroke touched */
    int w, h;
    pthread_mutex_t lock;
    /* set by the starting task */
    float progress;
    line_t status;
    int status_gen;
    bool finish;
    /* the renderer's own */
    line_t word1, word2, tag, spec, ver, note, detail;
    bool has_note;
    float shown;          /* progress as drawn, chasing the target */
    double finish_t, last_t, status_t;
    int seen_gen;
    line_t status_draw, status_old;
    bz_area_t prev, cur;
};

/* ------------------------------------------------------------------ colour */

typedef struct { int r, g, b; } rgb_t;

static rgb_t rgb(uint32_t c) { return (rgb_t){ (int)(c >> 16 & 255), (int)(c >> 8 & 255), (int)(c & 255) }; }

static uint16_t pack(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

static rgb_t mixc(rgb_t a, rgb_t b, float k)
{
    return (rgb_t){ (int)(a.r + (b.r - a.r) * k), (int)(a.g + (b.g - a.g) * k), (int)(a.b + (b.b - a.b) * k) };
}

/* RGB565 has 32 levels of red and blue: a dark gradient or a soft glow would come out in rings. Blends work
 * in the 565 codes themselves, in 1/256ths, and only the fraction is dithered (4×4 ordered), so a pixel a
 * blend doesn't change packs back to exactly itself. */
static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline int q(int v, int d, int max)
{
    v = (v + d) >> 8;
    return v < 0 ? 0 : v > max ? max : v;
}

/* a colour in 8-bit channels ×256 straight to a dithered pixel */
static inline uint16_t dither(int r256, int g256, int b256, int x, int y)
{
    int d = BAYER[y & 3][x & 3] * 16 + 8;
    return (uint16_t)(q(r256 * 31 / 255, d, 31) << 11 | q(g256 * 63 / 255, d, 63) << 5 | q(b256 * 31 / 255, d, 31));
}

static inline void blend(uint16_t *p, int x, int y, rgb_t c, float a, bool light)
{
    if (a <= 0.004f) return;
    int k = (int)(a * 256);
    if (!light && k >= 255) {
        *p = pack(c.r, c.g, c.b);
        return;
    }
    int r = (*p >> 11 & 31) << 8, g = (*p >> 5 & 63) << 8, b = (*p & 31) << 8;
    int cr = c.r * 31 * 256 / 255, cg = c.g * 63 * 256 / 255, cb = c.b * 31 * 256 / 255;
    if (light) {
        r += cr * k >> 8;
        g += cg * k >> 8;
        b += cb * k >> 8;
    } else {
        r += (cr - r) * k >> 8;
        g += (cg - g) * k >> 8;
        b += (cb - b) * k >> 8;
    }
    int d = BAYER[y & 3][x & 3] * 16 + 8;
    *p = (uint16_t)(q(r, d, 31) << 11 | q(g, d, 63) << 5 | q(b, d, 31));
}

/* ------------------------------------------------------------------ easing and springs */

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float x) { x = clamp01(x); return x * x * (3 - 2 * x); }

/* A spring from 0 to 1 released at rest, `t` s in: Detent's response and damping ratio. */
static float spring(float t, float response, float damping)
{
    if (t <= 0) return 0;
    float w0 = TAU / response, z = damping;
    if (z >= 1) return 1 - (1 + w0 * t) * expf(-w0 * t);
    float wd = w0 * sqrtf(1 - z * z);
    return 1 - expf(-z * w0 * t) * (cosf(wd * t) + z * w0 / wd * sinf(wd * t));
}
/* Detent roles (motion.js SPRINGS): release (response 0.5, ζ 0.85), smooth (ζ 1) */
#define RELEASE(t) spring((t), 0.5f, 0.85f)
#define SMOOTH(t) spring((t), 0.5f, 1.0f)

/* ------------------------------------------------------------------ the rasterizer */

static void touch(ui_boot_t *b, int x1, int y1, int x2, int y2)
{
    bz_area_t *c = &b->cur;
    if (c->x1 > c->x2) {
        *c = (bz_area_t){ (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2 };
        return;
    }
    if (x1 < c->x1) c->x1 = (int16_t)x1;
    if (y1 < c->y1) c->y1 = (int16_t)y1;
    if (x2 > c->x2) c->x2 = (int16_t)x2;
    if (y2 > c->y2) c->y2 = (int16_t)y2;
}

static bool clip(ui_boot_t *b, int *x1, int *y1, int *x2, int *y2, bool track)
{
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > b->w - 1) *x2 = b->w - 1;
    if (*y2 > b->h - 1) *y2 = b->h - 1;
    if (*x1 > *x2 || *y1 > *y2) return false;
    if (track) touch(b, *x1, *y1, *x2, *y2);
    return true;
}

static void disc(ui_boot_t *b, float cx, float cy, float r, rgb_t c, float a)
{
    if (a <= 0.004f || r <= 0.05f) return;
    int x1 = (int)floorf(cx - r - 1), x2 = (int)ceilf(cx + r + 1), y1 = (int)floorf(cy - r - 1), y2 = (int)ceilf(cy + r + 1);
    if (!clip(b, &x1, &y1, &x2, &y2, true)) return;
    for (int y = y1; y <= y2; y++) {
        uint16_t *row = b->buf + (size_t)y * b->w;
        float dy = y + 0.5f - cy;
        for (int x = x1; x <= x2; x++) {
            float dx = x + 0.5f - cx;
            float cov = clamp01(r + 0.5f - sqrtf(dx * dx + dy * dy));
            if (cov > 0) blend(&row[x], x, y, c, a * cov, false);
        }
    }
}

/* A soft glow: a Gaussian of `sigma`, added (separable: one row of weights, a multiply per pixel). */
static void glow(ui_boot_t *b, float cx, float cy, float sigma, rgb_t c, float a)
{
    if (a <= 0.004f || sigma <= 0.5f) return;
    float r = sigma * 3;
    int x1 = (int)(cx - r), x2 = (int)(cx + r), y1 = (int)(cy - r), y2 = (int)(cy + r);
    if (!clip(b, &x1, &y1, &x2, &y2, true)) return;
    float k = -1.0f / (2 * sigma * sigma), ex[400];
    int n = x2 - x1 + 1;
    if (n > 400) n = 400;
    for (int i = 0; i < n; i++) {
        float dx = x1 + i + 0.5f - cx;
        ex[i] = a * expf(k * dx * dx);
    }
    for (int y = y1; y <= y2; y++) {
        uint16_t *row = b->buf + (size_t)y * b->w + x1;
        float dy = y + 0.5f - cy, ey = expf(k * dy * dy);
        if (ey * a < 0.004f) continue;
        for (int i = 0; i < n; i++) {
            float v = ey * ex[i];
            if (v >= 0.004f) blend(&row[i], x1 + i, y, c, v, true);
        }
    }
}

/* A capsule from (x0, y0) to (x1, y1), width w, colour and alpha running from the first end to the second. */
static void capsule(ui_boot_t *b, float x0, float y0, float x1, float y1, float w, rgb_t c0, rgb_t c1, float a0,
                    float a1, bool light)
{
    if (a0 <= 0.004f && a1 <= 0.004f) return;
    float hw = w * 0.5f;
    int bx1 = (int)floorf(fminf(x0, x1) - hw - 1), bx2 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by1 = (int)floorf(fminf(y0, y1) - hw - 1), by2 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (!clip(b, &bx1, &by1, &bx2, &by2, true)) return;
    float vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
    for (int y = by1; y <= by2; y++) {
        uint16_t *row = b->buf + (size_t)y * b->w;
        for (int x = bx1; x <= bx2; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = ll > 0 ? clamp01((px * vx + py * vy) / ll) : 0;
            float ex = px - vx * t, ey = py - vy * t;
            float cov = clamp01(hw + 0.5f - sqrtf(ex * ex + ey * ey));
            if (cov > 0) blend(&row[x], x, y, mixc(c0, c1, t), (a0 + (a1 - a0) * t) * cov, light);
        }
    }
}

/* A rounded rectangle about (cx, cy), turned by `ang`. */
typedef struct {
    float cx, cy, w, h, r, ang;
} rrect_t;

static float rr_sdf(const rrect_t *r, float px, float py)
{
    float cs = cosf(-r->ang), sn = sinf(-r->ang);
    float lx = (px - r->cx) * cs - (py - r->cy) * sn, ly = (px - r->cx) * sn + (py - r->cy) * cs;
    float qx = fabsf(lx) - r->w * 0.5f + r->r, qy = fabsf(ly) - r->h * 0.5f + r->r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r->r;
}

/* filled (stroke 0) or a ring of `stroke` px on the edge; `feather` > 0 blurs it (a shadow) */
static void rrect(ui_boot_t *b, const rrect_t *r, rgb_t c, float a, float stroke, float feather)
{
    if (a <= 0.004f) return;
    float ext = sqrtf(r->w * r->w + r->h * r->h) * 0.5f + stroke + feather * 2 + 2;
    int x1 = (int)(r->cx - ext), x2 = (int)(r->cx + ext), y1 = (int)(r->cy - ext), y2 = (int)(r->cy + ext);
    if (!clip(b, &x1, &y1, &x2, &y2, true)) return;
    float soft = feather > 0 ? feather : 0.5f;
    for (int y = y1; y <= y2; y++) {
        uint16_t *row = b->buf + (size_t)y * b->w;
        for (int x = x1; x <= x2; x++) {
            float d = rr_sdf(r, x + 0.5f, y + 0.5f);
            float cov = stroke > 0 ? clamp01((stroke * 0.5f - fabsf(d)) / (2 * soft) + 0.5f) : clamp01(-d / (2 * soft) + 0.5f);
            if (cov > 0) blend(&row[x], x, y, c, a * cov, false);
        }
    }
}

/* A polygon, anti-aliased by 4×4 supersampling: the bolt. `reveal` 0..1 shows it from the top (the strike). */
static void polygon(ui_boot_t *b, const float *pts, int n, rgb_t c, float a, float reveal)
{
    if (a <= 0.004f || reveal <= 0) return;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, pts[2 * i]);
        maxx = fmaxf(maxx, pts[2 * i]);
        miny = fminf(miny, pts[2 * i + 1]);
        maxy = fmaxf(maxy, pts[2 * i + 1]);
    }
    float cut = miny + (maxy - miny) * reveal;
    int x1 = (int)floorf(minx), x2 = (int)ceilf(maxx), y1 = (int)floorf(miny), y2 = (int)ceilf(maxy);
    if (!clip(b, &x1, &y1, &x2, &y2, true)) return;
    for (int y = y1; y <= y2; y++) {
        uint16_t *row = b->buf + (size_t)y * b->w;
        for (int x = x1; x <= x2; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                float py = y + (sy + 0.5f) / 4;
                if (py > cut) continue;
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4;
                    bool in = false;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
                        if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
                    }
                    inside += in;
                }
            }
            if (inside) blend(&row[x], x, y, c, a * inside / 16.0f, false);
        }
    }
}

/* One laid-out line with its baseline at y; per-glyph lift and alpha from `fx` if given. `dst` is the
 * screen or the background; `scale` > 1 enlarges it (bilinear), for the ghost numeral. */
typedef struct { float dy, a; } glyph_fx_t;

static void text(ui_boot_t *b, uint16_t *dst, const line_t *l, float x0, float base, rgb_t c, float a,
                 const glyph_fx_t *fx, float scale)
{
    for (int i = 0; i < l->n; i++) {
        const glyph_t *g = &l->g[i];
        float ga = a * (fx ? fx[i].a : 1);
        if (ga <= 0.004f) continue;
        float gx = roundf(x0 + g->x * scale), gy = roundf(base + g->y * scale + (fx ? fx[i].dy : 0));
        int x1 = (int)gx, y1 = (int)gy;
        int x2 = (int)ceilf(gx + g->w * scale), y2 = (int)ceilf(gy + g->h * scale);
        if (!clip(b, &x1, &y1, &x2, &y2, dst == b->buf)) continue;
        for (int y = y1; y <= y2; y++) {
            uint16_t *row = dst + (size_t)y * b->w;
            float v_y = (y + 0.5f - gy) / scale - 0.5f;
            for (int x = x1; x <= x2; x++) {
                float v;
                if (scale == 1) {
                    v = glyph_v(g, x - (int)gx, y - (int)gy) / 15.0f;
                } else {
                    float u = (x + 0.5f - gx) / scale - 0.5f;
                    int ui = (int)floorf(u), vi = (int)floorf(v_y);
                    float fu = u - ui, fv = v_y - vi;
                    v = ((glyph_v(g, ui, vi) * (1 - fu) + glyph_v(g, ui + 1, vi) * fu) * (1 - fv) +
                         (glyph_v(g, ui, vi + 1) * (1 - fu) + glyph_v(g, ui + 1, vi + 1) * fu) * fv) / 15.0f;
                }
                if (v > 0.004f) blend(&row[x], x, y, c, ga * v, false);
            }
        }
    }
}

/* ------------------------------------------------------------------ the ground, drawn once */

static void ground(ui_boot_t *b)
{
    const rgb_t g0 = rgb(0x191A20), g1 = rgb(0x101116), g2 = rgb(0x08090B), key = rgb(C_CRIMSON);
    float kx = 0.76f * b->w, ky = 0.46f * b->h, kr = 0.62f * b->w;
    float rx = 0.16f * b->w, ry = 0.16f * b->h, rr = 0.6f * b->w;
    for (int y = 0; y < b->h; y++) {
        uint16_t *row = b->bg + (size_t)y * b->w;
        for (int x = 0; x < b->w; x++) {
            /* the diagonal: 0 top-left, 1 bottom-right */
            float t = (x / (float)b->w + y / (float)b->h) * 0.5f;
            rgb_t c = t < 0.55f ? mixc(g0, g1, t / 0.55f) : mixc(g1, g2, (t - 0.55f) / 0.45f);
            float r = c.r, g = c.g, bl = c.b;
            /* key light: 19 % at the centre, 5 % at 45 %, gone at the edge */
            float d = sqrtf((x - kx) * (x - kx) + (y - ky) * (y - ky)) / kr;
            float ka = d < 0.45f ? 0.19f + (0.05f - 0.19f) * d / 0.45f : d < 1 ? 0.05f * (1 - (d - 0.45f) / 0.55f) : 0;
            r += (key.r - r) * ka;
            g += (key.g - g) * ka;
            bl += (key.b - bl) * ka;
            /* rim light: 7 % white fading out */
            float e = sqrtf((x - rx) * (x - rx) + (y - ry) * (y - ry)) / rr;
            float ra = e < 1 ? 0.07f * (1 - e) : 0;
            r += (255 - r) * ra;
            g += (255 - g) * ra;
            bl += (255 - bl) * ra;
            row[x] = dither((int)(r * 256), (int)(g * 256), (int)(bl * 256), x, y);
        }
    }
    /* the version as a ghost, oversized, bottom right (the banner's "1.x", 250 px at 3.2 % white) */
    line_t ghost;
    char v[8];
    snprintf(v, sizeof v, "%.3s", CATALYST_TAB_VERSION); /* "1.0" */
    layout(&ghost, &bz_font_brand_bold_72, v, -4);
    float sc = 290.0f / 72;
    text(b, b->bg, &ghost, b->w - 40 - ghost.width * sc, TY(318), rgb(C_WHITE), 0.032f, NULL, sc);
}

/* ------------------------------------------------------------------ strokes

 * A polyline drawn as capsules would double its alpha wherever two meet (beads along a faint line). A
 * stroke instead keeps the largest coverage each pixel gets from any piece, with where along the line
 * that piece was, and is blended once at the end. */

static void stroke_piece(ui_boot_t *b, float x0, float y0, float x1, float y1, float w, float u0, float u1)
{
    float hw = w * 0.5f;
    int bx1 = (int)floorf(fminf(x0, x1) - hw - 1), bx2 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by1 = (int)floorf(fminf(y0, y1) - hw - 1), by2 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (!clip(b, &bx1, &by1, &bx2, &by2, false)) return;
    bz_area_t *s = &b->sbox;
    if (s->x1 > s->x2) *s = (bz_area_t){ (int16_t)bx1, (int16_t)by1, (int16_t)bx2, (int16_t)by2 };
    else {
        if (bx1 < s->x1) s->x1 = (int16_t)bx1;
        if (by1 < s->y1) s->y1 = (int16_t)by1;
        if (bx2 > s->x2) s->x2 = (int16_t)bx2;
        if (by2 > s->y2) s->y2 = (int16_t)by2;
    }
    float vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
    for (int y = by1; y <= by2; y++) {
        uint8_t *cr = b->cov + (size_t)y * b->w, *pr = b->pos + (size_t)y * b->w;
        for (int x = bx1; x <= bx2; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = ll > 0 ? clamp01((px * vx + py * vy) / ll) : 0;
            float ex = px - vx * t, ey = py - vy * t;
            int c = (int)(clamp01(hw + 0.5f - sqrtf(ex * ex + ey * ey)) * 255);
            if (c > cr[x]) {
                cr[x] = (uint8_t)c;
                pr[x] = (uint8_t)((u0 + (u1 - u0) * t) * 255);
            }
        }
    }
}

/* paint(u) gives the colour and alpha at u along the line; `gain` scales it; `pulse` (u, or < 0) brightens */
static void stroke_end(ui_boot_t *b, void (*paint)(float u, rgb_t *c, float *a), float gain, float pulse, float pgain,
                       bool light)
{
    bz_area_t s = b->sbox;
    if (s.x1 > s.x2) return;
    touch(b, s.x1, s.y1, s.x2, s.y2);
    for (int y = s.y1; y <= s.y2; y++) {
        uint8_t *cr = b->cov + (size_t)y * b->w, *pr = b->pos + (size_t)y * b->w;
        uint16_t *row = b->buf + (size_t)y * b->w;
        for (int x = s.x1; x <= s.x2; x++) {
            if (!cr[x]) continue;
            float u = pr[x] / 255.0f, a;
            rgb_t c;
            paint(u, &c, &a);
            float g = gain;
            if (pulse >= 0) g += pgain * expf(-(u - pulse) * (u - pulse) / 0.003f);
            blend(&row[x], x, y, c, a * g * cr[x] / 255.0f, light);
            cr[x] = 0;
        }
    }
    b->sbox = (bz_area_t){ 1, 1, 0, 0 };
}

/* ------------------------------------------------------------------ the line */

/* The banner's route: three cubic segments, in banner px */
static const float ROUTE[10][2] = {
    { -20, 320 }, { 180, 318 }, { 320, 306 }, { 430, 280 }, { 540, 254 },
    { 600, 214 }, { 720, 196 }, { 830, 180 }, { 906, 178 }, { 992, 160 },
};

static void route_at(float u, float *x, float *y)
{
    float s = clamp01(u) * 3;
    int seg = s >= 3 ? 2 : (int)s;
    float t = s - seg, it = 1 - t;
    const float *p0 = ROUTE[seg * 3], *p1 = ROUTE[seg * 3 + 1], *p2 = ROUTE[seg * 3 + 2], *p3 = ROUTE[seg * 3 + 3];
    float bx = it * it * it * p0[0] + 3 * it * it * t * p1[0] + 3 * it * t * t * p2[0] + t * t * t * p3[0];
    float by = it * it * it * p0[1] + 3 * it * it * t * p1[1] + 3 * it * t * t * p2[1] + t * t * t * p3[1];
    *x = LX(bx);
    *y = TY(by);
}

/* the banner's stroke gradient: crimson from nothing, 40 % at 28 %, brighter 92 % at 68 %, full at the end */
static void route_paint(float u, rgb_t *c, float *a)
{
    const rgb_t c0 = rgb(C_CRIMSON), c1 = rgb(C_LINE_HI), c2 = rgb(C_LINE_END);
    if (u < 0.28f) {
        *c = c0;
        *a = 0.40f * u / 0.28f;
    } else if (u < 0.68f) {
        float k = (u - 0.28f) / 0.40f;
        *c = mixc(c0, c1, k);
        *a = 0.40f + 0.52f * k;
    } else {
        float k = (u - 0.68f) / 0.32f;
        *c = mixc(c1, c2, k);
        *a = 0.92f + 0.08f * k;
    }
}

static void route(ui_boot_t *b, float head, float pulse)
{
    const int N = 90;
    float px = 0, py = 0;
    /* the banner's wide haze, the tight glow on the line (where the telemetry pulse shows), the stroke */
    static const float W[3] = { 14, 6, 3.2f }, G[3] = { 0.06f, 0.14f, 1 }, PG[3] = { 0, 0.35f, 0 };
    for (int pass = 0; pass < 3; pass++) {
        route_at(0, &px, &py);
        for (int i = 1; i <= N; i++) {
            float u0 = (i - 1) / (float)N, u1 = i / (float)N;
            if (u0 >= head) break;
            if (u1 > head) u1 = head;
            float x, y;
            route_at(u1, &x, &y);
            stroke_piece(b, px, py, x, y, W[pass], u0, u1);
            px = x;
            py = y;
        }
        stroke_end(b, route_paint, G[pass], pass == 1 ? pulse : -1, PG[pass], pass < 2);
    }
    if (head > 0.01f && head < 0.999f) {
        rgb_t c;
        float ha;
        route_paint(head, &c, &ha);
        glow(b, px, py, 7, c, 0.45f);
        disc(b, px, py, 2.4f, rgb(C_DOT), 1);
    }
    /* the sampled poses along it, fading back down, each appearing as the line reaches it */
    static const float DOTS[6][3] = { { 120, 317, 0.16f }, { 300, 301, 0.28f }, { 430, 280, 0.42f },
                                      { 578, 228, 0.58f }, { 720, 196, 0.74f }, { 862, 177, 0.88f } };
    static const float AT[6] = { 0.19f, 0.33f, 0.45f, 0.58f, 0.70f, 0.86f }; /* where each sits on the route */
    for (int i = 0; i < 6; i++) {
        float on = smooth((head - AT[i]) * 14);
        disc(b, LX(DOTS[i][0]), TY(DOTS[i][1]), (2.6f + 0.16f * i) * 1.1f, rgb(C_DOT), DOTS[i][2] * on);
    }
}

/* ------------------------------------------------------------------ the tablet */

static void tablet(ui_boot_t *b, float appear, float lit)
{
    if (appear <= 0.004f) return;
    float x, y;
    route_at(1, &x, &y);
    float cx = x + 62, cy = y + 4 + 14 * (1 - RELEASE(appear * 0.9f)), ang = -0.12f;
    float k = clamp01(appear * 1.6f);
    float cs = cosf(ang), sn = sinf(ang);
#define LOCX(lx, ly) (cx + (lx) * cs - (ly) * sn)
#define LOCY(lx, ly) (cy + (lx) * sn + (ly) * cs)
    /* contact shadow, so it sits on the ground instead of floating over it */
    rrect_t sh = { cx + 3, cy + 10, 170, 112, 18, ang };
    rrect(b, &sh, rgb(0), 0.5f * k, 0, 10);
    /* the Tab5 from the front: frame, its lit top edge, the screen */
    rrect_t body = { cx, cy, 172, 110, 16, ang };
    rrect(b, &body, rgb(C_FRAME), k, 0, 0);
    rrect(b, &body, rgb(C_FRAME_EDGE), k, 1.4f, 0);
    capsule(b, LOCX(-70, -54.3f), LOCY(-70, -54.3f), LOCX(70, -54.3f), LOCY(70, -54.3f), 1.4f, rgb(C_WHITE),
            rgb(C_WHITE), 0.10f * k, 0.10f * k, false);
    /* the side the line arrives at in the accent, the far side bumper-dark: the banner's front bumper */
    capsule(b, LOCX(-86, -38), LOCY(-86, -38), LOCX(-86, 38), LOCY(-86, 38), 4.5f, rgb(C_LINE_HI), rgb(C_LINE_HI),
            0.9f * k, 0.9f * k, false);
    capsule(b, LOCX(86, -38), LOCY(86, -38), LOCX(86, 38), LOCY(86, 38), 4.5f, rgb(C_BUMPER), rgb(C_BUMPER), 0.8f * k,
            0.8f * k, false);
    rrect_t scr = { cx, cy, 146, 86, 8, ang };
    rrect(b, &scr, rgb(C_SCREEN), k, 0, 0);
    /* on it, the reading it arrives to show: a live dot and a sparkline, brightening when the line lands */
    static const float SP[7][2] = { { -50, 20 }, { -34, 10 }, { -18, 16 }, { -2, 0 }, { 14, 8 }, { 30, -8 }, { 48, -4 } };
    float la = k * (0.3f + 0.7f * lit);
    for (int i = 0; i < 6; i++)
        capsule(b, LOCX(SP[i][0], SP[i][1]), LOCY(SP[i][0], SP[i][1]), LOCX(SP[i + 1][0], SP[i + 1][1]),
                LOCY(SP[i + 1][0], SP[i + 1][1]), 2, rgb(C_LINE_HI), rgb(C_LINE_HI), la, la, false);
    disc(b, LOCX(48, -4), LOCY(48, -4), 2.6f, rgb(C_DOT), la);
    disc(b, LOCX(-56, -28), LOCY(-56, -28), 3, rgb(C_LINE_HI), k * (0.5f + 0.5f * lit));
    if (lit > 0) glow(b, cx, cy, 44, rgb(C_CRIMSON), 0.18f * lit);
#undef LOCX
#undef LOCY
}

/* ------------------------------------------------------------------ public */

ui_boot_t *ui_boot_create(uint16_t *buf, int w, int h, const hal_boot_t *prev, bool safe_mode)
{
    ui_boot_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->bg = malloc((size_t)w * h * 2);
    b->cov = calloc((size_t)w * h, 1);
    b->pos = malloc((size_t)w * h);
    if (!b->bg || !b->cov || !b->pos) {
        free(b->bg);
        free(b->cov);
        free(b->pos);
        free(b);
        return NULL;
    }
    b->sbox = (bz_area_t){ 1, 1, 0, 0 };
    b->buf = buf;
    b->w = w;
    b->h = h;
    pthread_mutex_init(&b->lock, NULL);
    b->finish_t = -1;
    b->cur = (bz_area_t){ 1, 1, 0, 0 };
    b->prev = (bz_area_t){ 0, 0, (int16_t)(w - 1), (int16_t)(h - 1) }; /* the first frame is the whole card */
    layout(&b->word1, &bz_font_brand_light_72, "Catalyst", -2);
    layout(&b->word2, &bz_font_brand_bold_72, "Tab", -3);
    layout(&b->tag, &bz_font_name_24, "Diagnose it where it sits.", 0);
    layout(&b->spec, &bz_font_body_17, "Systemcore \xc2\xb7 NetworkTables \xc2\xb7 Catalyst Link", 0);
    layout(&b->ver, &bz_font_mono_16, "v" CATALYST_TAB_VERSION, 0);
    if (prev && prev->failed) {
        char note[96];
        snprintf(note, sizeof note, "last start: %s at %s%s", prev->reason, prev->stage, safe_mode ? " \xc2\xb7 safe mode" : "");
        for (char *c = note; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c -= 32;
        layout(&b->note, &bz_font_mono_13, note, 2);
        if (prev->detail[0]) layout(&b->detail, &bz_font_mono_13, prev->detail, 0);
        b->has_note = true;
    }
    ground(b);
    return b;
}

void ui_boot_set(ui_boot_t *b, float progress, const char *status)
{
    if (!b) return;
    line_t l;
    if (status) {
        /* the banner's caption: uppercase, letterspaced */
        char up[64];
        snprintf(up, sizeof up, "%s", status);
        for (char *c = up; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c -= 32;
        layout(&l, &bz_font_mono_13, up, 2);
    }
    pthread_mutex_lock(&b->lock);
    if (progress > b->progress) b->progress = progress > 1 ? 1 : progress;
    if (status) {
        b->status = l;
        b->status_gen++;
    }
    pthread_mutex_unlock(&b->lock);
}

void ui_boot_finish(ui_boot_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->lock);
    b->finish = true;
    b->progress = 1;
    pthread_mutex_unlock(&b->lock);
}

void ui_boot_destroy(ui_boot_t *b)
{
    if (!b) return;
    pthread_mutex_destroy(&b->lock);
    free(b->bg);
    free(b->cov);
    free(b->pos);
    free(b);
}

static void restore(ui_boot_t *b, const bz_area_t *a)
{
    if (a->x1 > a->x2) return;
    size_t n = (size_t)(a->x2 - a->x1 + 1) * 2;
    for (int y = a->y1; y <= a->y2; y++) memcpy(b->buf + (size_t)y * b->w + a->x1, b->bg + (size_t)y * b->w + a->x1, n);
}

bool ui_boot_frame(ui_boot_t *b, double t, bz_area_t *damage)
{
    pthread_mutex_lock(&b->lock);
    float target = b->progress;
    bool finishing = b->finish;
    if (b->status_gen != b->seen_gen) {
        b->seen_gen = b->status_gen;
        b->status_old = b->status_draw;
        b->status_draw = b->status;
        b->status_t = t;
    }
    pthread_mutex_unlock(&b->lock);

    double dt = b->last_t > 0 ? t - b->last_t : 1 / 60.0;
    if (dt > 0.1) dt = 0.1;
    b->last_t = t;
    if (finishing && b->finish_t < 0) b->finish_t = t > INTRO_S ? t : INTRO_S;
    float T = (float)t;
    float arrive = b->finish_t >= 0 ? clamp01((float)((t - b->finish_t) / ARRIVE_S)) : 0;
    float out = b->finish_t >= 0 ? clamp01((float)((t - b->finish_t - ARRIVE_S) / OUTRO_S)) : 0;

    restore(b, &b->prev);
    b->cur = (bz_area_t){ 1, 1, 0, 0 };

    /* ---- the mark: the bolt strikes, drawn top to bottom, with a flash that settles */
    static const float BOLT[6][2] = { { 13.5f, 2 }, { 5, 13 }, { 10, 13 }, { 8.5f, 22 }, { 19, 10 }, { 13.5f, 10 } };
    float bpts[12], bx = TX(76), by = TY(84), bs = 2.5f * S;
    for (int i = 0; i < 6; i++) {
        bpts[2 * i] = bx + BOLT[i][0] * bs;
        bpts[2 * i + 1] = by + BOLT[i][1] * bs;
    }
    float strike = smooth((T - 0.12f) / 0.16f);
    float flash = strike * expf(-fmaxf(T - 0.28f, 0) / 0.25f);
    glow(b, bx + 12 * bs, by + 12 * bs, 22, rgb(C_CRIMSON), 0.35f * flash);
    polygon(b, bpts, 6, rgb(C_CRIMSON), 1, strike);

    /* ---- the wordmark: "Catalyst" light grey, "Tab" bold white, rising letter by letter */
    float wx = TX(138), wb = TY(124);
    glyph_fx_t fx[MAXGLYPHS];
    for (int i = 0; i < b->word1.n; i++) {
        float st = T - (0.28f + 0.035f * i);
        fx[i].dy = 16 * (1 - RELEASE(st));
        fx[i].a = clamp01(st * 4);
    }
    text(b, b->buf, &b->word1, wx, wb, rgb(C_WORD_LIGHT), 1, fx, 1);
    for (int i = 0; i < b->word2.n; i++) {
        float st = T - (0.62f + 0.05f * i);
        fx[i].dy = 16 * (1 - RELEASE(st));
        fx[i].a = clamp01(st * 4);
    }
    text(b, b->buf, &b->word2, wx + b->word1.width + 20, wb, rgb(C_WHITE), 1, fx, 1);

    /* ---- the hairline draws out under it */
    float hair = SMOOTH((T - 0.8f) * 1.1f);
    if (hair > 0.004f) {
        float hx = TX(140), hy = TY(146) + 1, hw = 300 * S * hair;
        capsule(b, hx, hy, hx + hw, hy, 1.8f, rgb(C_CRIMSON), rgb(C_CRIMSON), 0.85f, 0.85f * (1 - hair), false);
    }

    /* ---- tagline, the line under it, the pill and its caption */
    float tg = SMOOTH((T - 0.95f) * 1.3f);
    text(b, b->buf, &b->tag, TX(139), TY(184) + 10 * (1 - tg), rgb(C_TAGLINE), tg, NULL, 1);
    float sp = SMOOTH((T - 1.08f) * 1.3f);
    text(b, b->buf, &b->spec, TX(140), TY(212) + 10 * (1 - sp), rgb(C_SPEC), sp, NULL, 1);
    float pl = SMOOTH((T - 1.2f) * 1.3f);
    if (pl > 0.004f) {
        float px = TX(140), py = TY(240) + 8 * (1 - pl), pw = 33 * S + b->ver.width + 16, ph = 30 * S;
        rrect_t pill = { px + pw / 2, py + ph / 2, pw, ph, ph / 2, 0 };
        rrect(b, &pill, rgb(C_CRIMSON), 0.14f * pl, 0, 0);
        rrect(b, &pill, rgb(C_CRIMSON), 0.6f * pl, 1.2f, 0);
        /* the dot breathes while the tablet is still starting */
        float breathe = arrive > 0 ? 1 : 0.6f + 0.4f * cosf(TAU * T / 1.3f);
        disc(b, px + 19 * S, py + ph / 2, 4 * S, rgb(C_LINE_HI), pl * breathe);
        text(b, b->buf, &b->ver, px + 33 * S, py + ph / 2 + 6, rgb(C_PILL_TXT), pl, NULL, 1);
        /* the caption says what it's doing, crossfading as it changes */
        float cx = px + pw + 18, cy = py + ph / 2 + 5;
        float na = clamp01((float)(t - b->status_t) * 6);
        text(b, b->buf, &b->status_draw, cx, cy, rgb(C_CAPTION), pl * na, NULL, 1);
        if (na < 0.99f) text(b, b->buf, &b->status_old, cx, cy, rgb(C_CAPTION), pl * (1 - na), NULL, 1);
    }
    if (b->has_note) {
        float na = SMOOTH((T - 1.3f) * 1.3f);
        text(b, b->buf, &b->note, TX(140), TY(304), rgb(BZ_PAL_DARK.warn), na, NULL, 1);
        text(b, b->buf, &b->detail, TX(140), TY(304) + 20, rgb(C_SPEC), na, NULL, 1);
    }

    /* ---- the line is the progress: chased critically damped, never ahead of the card coming in; when
     * everything's ready it runs the rest of the way and arrives */
    b->shown += (target - b->shown) * (1 - expf(-(float)dt * 6));
    float head = fminf(b->shown, smooth((T - 0.25f) * 0.8f)) * 0.9f;
    if (arrive > 0) head += (1 - head) * smooth(arrive * 1.7f);
    float pulse = T > 1.5f && arrive <= 0 ? fmodf((T - 1.5f) * 0.6f, 1.25f) * head : -1;
    route(b, head, pulse);

    /* ---- the tablet it arrives at, and beyond it, where it's going: dotted to a ring */
    float lit = smooth((arrive - 0.4f) * 2.5f);
    tablet(b, (T - 0.45f) * 1.4f, lit);
    if (lit > 0.01f) {
        float x0, y0;
        route_at(1, &x0, &y0);
        float x1 = x0 + 62 + 100, y1 = y0 - 12, x2 = b->w - 36.0f, y2 = y0 - 26;
        for (int i = 0; i < 5; i++) {
            float u = i / 5.0f;
            disc(b, x1 + (x2 - x1) * u, y1 + (y2 - y1) * u, 1.4f, rgb(C_LINE_HI), 0.5f * lit);
        }
        rrect_t ring = { x2, y2, 12.8f, 12.8f, 6.4f, 0 };
        rrect(b, &ring, rgb(C_DOT), 0.75f * lit, 2, 0);
    }

    /* ---- the outro: the whole card fades to the UI's ground */
    bz_area_t d;
    if (out > 0) {
        rgb_t gr = rgb(BZ_PAL_DARK.ground);
        float k = smooth(out);
        for (int y = 0; y < b->h; y++) {
            uint16_t *row = b->buf + (size_t)y * b->w;
            for (int x = 0; x < b->w; x++) blend(&row[x], x, y, gr, k, false);
        }
        d = (bz_area_t){ 0, 0, (int16_t)(b->w - 1), (int16_t)(b->h - 1) };
        b->prev = d;
        *damage = d;
        return out < 1;
    }

    /* damage: last frame's box (now restored) and this one's */
    d = b->cur;
    if (b->prev.x1 <= b->prev.x2) {
        if (d.x1 > d.x2) d = b->prev;
        else {
            if (b->prev.x1 < d.x1) d.x1 = b->prev.x1;
            if (b->prev.y1 < d.y1) d.y1 = b->prev.y1;
            if (b->prev.x2 > d.x2) d.x2 = b->prev.x2;
            if (b->prev.y2 > d.y2) d.y2 = b->prev.y2;
        }
    }
    if (d.x1 > d.x2) d = (bz_area_t){ 0, 0, 0, 0 };
    *damage = d;
    b->prev = b->cur;
    return true;
}
