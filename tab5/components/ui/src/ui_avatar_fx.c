/* ui_avatar_fx — what is on and around the face: a mouth, a blush, and the small things that float.
 *
 * The Avatar Lab's format is a body and two eyes. That is enough to read a mood and not enough to be
 * somebody: there is nothing to smile with, nothing to chew with, and nothing to say "that was nice" beyond
 * the eyes closing. So the tablet adds, live and in the avatar's own coordinates, a mouth that follows the
 * state (and the voice, while it speaks), a blush, and particles -- hearts, sparkles, z's, a tear, a cookie
 * with bites out of it. All of it is a few primitives per frame; none of it is stored.
 *
 * Lengths are in the format's units (the avatar's box is 300 of them), scaled by `u`. */
#include "ui_avatar_fx.h"

#include <math.h>

#include "ui_internal.h"

enum { M_NONE, M_CAT, M_SMILE, M_BIG, M_O, M_SMALL_O, M_FLAT, M_FROWN, M_CHEW, M_TALK, M_YAWN };

static const uint8_t MOUTH[AV_S_COUNT] = {
    [AV_S_IDLE] = M_CAT,      [AV_S_QUIET] = M_CAT,      [AV_S_LISTEN] = M_SMILE,  [AV_S_THINK] = M_FLAT,
    [AV_S_SPEAK] = M_TALK,    [AV_S_HAPPY] = M_BIG,      [AV_S_CONFUSED] = M_FLAT, [AV_S_SURPRISED] = M_O,
    [AV_S_SLEEP] = M_SMALL_O, [AV_S_WAKE] = M_SMALL_O,   [AV_S_DROWSY] = M_FLAT,   [AV_S_SAD] = M_FROWN,
    [AV_S_HUNGRY] = M_FROWN,  [AV_S_EAT] = M_CHEW,       [AV_S_PETTED] = M_CAT,    [AV_S_LOVE] = M_SMILE,
    [AV_S_FED] = M_BIG,       [AV_S_CURIOUS] = M_SMALL_O, [AV_S_SICK] = M_FROWN,   [AV_S_SNEEZE] = M_O,
    [AV_S_GIGGLE] = M_BIG,    [AV_S_BLUSH] = M_CAT,      [AV_S_WINK] = M_SMILE,    [AV_S_POUT] = M_FROWN,
    [AV_S_NOD] = M_SMILE,     [AV_S_NO] = M_FLAT,        [AV_S_DANCE] = M_BIG,     [AV_S_YAWN] = M_YAWN,
};

#define PINK 0xff6f9c
#define BLUSH 0xff7fa6
#define GOLD 0xffe08a
#define PALE 0xd9d2ff
#define WATER 0x8fd3ff

static float clampf(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float ease(float x) { x = clampf(x); return x * x * (3 - 2 * x); }
/* in and out over a life of `d` s that began at `t0` */
static float life(float t, float t0, float d, float fin, float fout)
{
    float p = (t - t0) / d;
    if (p <= 0 || p >= 1) return 0;
    float a = p / fin, b = (1 - p) / fout;
    return a < b ? (a < 1 ? a : 1) : (b < 1 ? b : 1);
}

typedef struct { lv_layer_t *l; float u; } G;

static void disc(const G *g, float x, float y, float rx, float ry, uint32_t c, float o)
{
    if (o <= 0.02f || rx < 0.5f || ry < 0.5f) return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = bz_lv_rgb(c);
    r.bg_opa = (lv_opa_t)(clampf(o) * 255);
    r.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { (int)lroundf(x - rx), (int)lroundf(y - ry), (int)lroundf(x + rx) - 1, (int)lroundf(y + ry) - 1 };
    lv_draw_rect(g->l, &r, &a);
}

static void line(const G *g, float x1, float y1, float x2, float y2, float w, uint32_t c, float o)
{
    if (o <= 0.02f) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = bz_lv_rgb(c);
    d.opa = (lv_opa_t)(clampf(o) * 255);
    d.width = (int)lroundf(w) < 1 ? 1 : (int)lroundf(w);
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(g->l, &d);
}

static void tri(const G *g, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c, float o)
{
    if (o <= 0.02f) return;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = bz_lv_rgb(c);
    t.opa = (lv_opa_t)(clampf(o) * 255);
    t.p[0].x = x1; t.p[0].y = y1; t.p[1].x = x2; t.p[1].y = y2; t.p[2].x = x3; t.p[2].y = y3;
    lv_draw_triangle(g->l, &t);
}

/* LVGL's angles: 0 at three o'clock, clockwise, so 0..180 is the lower half: a smile */
static void arc(const G *g, float x, float y, float r, int a0, int a1, float w, uint32_t c)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = bz_lv_rgb(c);
    d.opa = LV_OPA_COVER;
    d.center.x = (int)lroundf(x);
    d.center.y = (int)lroundf(y);
    d.radius = (uint16_t)lroundf(r);
    d.start_angle = a0;
    d.end_angle = a1;
    d.width = (int)lroundf(w) < 1 ? 1 : (int)lroundf(w);
    d.rounded = 1;
    lv_draw_arc(g->l, &d);
}

static void heart(const G *g, float x, float y, float s, float o)
{
    if (o <= 0.02f) return;
    disc(g, x - 0.52f * s, y - 0.30f * s, 0.60f * s, 0.60f * s, PINK, o);
    disc(g, x + 0.52f * s, y - 0.30f * s, 0.60f * s, 0.60f * s, PINK, o);
    tri(g, x - 1.07f * s, y - 0.02f * s, x + 1.07f * s, y - 0.02f * s, x, y + 1.2f * s, PINK, o);
}

static void sparkle(const G *g, float x, float y, float s, float o)
{
    float w = s * 0.34f < 2 ? 2 : s * 0.34f;
    line(g, x - s, y, x + s, y, w, GOLD, o);
    line(g, x, y - s, x, y + s, w, GOLD, o);
}

static void zed(const G *g, float x, float y, float s, float o)
{
    float w = s * 0.38f < 2 ? 2 : s * 0.38f;
    line(g, x - s, y - s, x + s, y - s, w, PALE, o);
    line(g, x + s, y - s, x - s, y + s, w, PALE, o);
    line(g, x - s, y + s, x + s, y + s, w, PALE, o);
}

static void drop(const G *g, float x, float y, float s, float o)
{
    if (o <= 0.02f) return;
    tri(g, x - 0.8f * s, y, x + 0.8f * s, y, x, y - 1.7f * s, WATER, o);
    disc(g, x, y + 0.2f * s, s, s, WATER, o);
}

static void hearts(const G *g, float cx, float cy, float t, const float (*L)[5], int n)
{
    for (int i = 0; i < n; i++) {
        float p = (t - L[i][0]) / L[i][2];
        if (p <= 0 || p >= 1) continue;
        heart(g, cx + (L[i][1] + sinf(p * 7.5f) * L[i][4]) * g->u, cy - (58 + ease(p) * 84) * g->u,
              L[i][3] * (0.7f + 0.5f * clampf(p * 3)) * 8 * g->u, life(t, L[i][0], L[i][2], 0.2f, 0.3f));
    }
}

static void sparkles(const G *g, float cx, float cy, float t, const float (*L)[4], int n)
{
    for (int i = 0; i < n; i++) {
        float p = clampf((t - L[i][0]) / 0.7f);
        sparkle(g, cx + L[i][1] * g->u, cy + L[i][2] * g->u, L[i][3] * 11 * g->u * (0.4f + 0.9f * sinf(p * 3.14159f)),
                life(t, L[i][0], 0.7f, 0.3f, 0.4f));
    }
}

static void blush(const G *g, const av_fx_face_t *f, float o)
{
    if (o <= 0.02f) return;
    float lx = f->ex[0] < f->ex[1] ? f->ex[0] : f->ex[1], rx = f->ex[0] < f->ex[1] ? f->ex[1] : f->ex[0];
    float y = (f->ey[0] > f->ey[1] ? f->ey[0] : f->ey[1]) + 34 * g->u;
    disc(g, lx - 20 * g->u, y, 17 * g->u, 9 * g->u, BLUSH, o * 0.42f);
    disc(g, rx + 20 * g->u, y, 17 * g->u, 9 * g->u, BLUSH, o * 0.42f);
}

static void mouth(const G *g, const av_fx_face_t *f, int kind, float t)
{
    float u = g->u, x = (f->ex[0] + f->ex[1]) * 0.5f, y = (f->ey[0] + f->ey[1]) * 0.5f + 46 * u;
    uint32_t c = f->eyes;
    if (kind == M_CHEW) kind = ((int)(t * 5.0f) & 1) ? M_SMALL_O : M_FLAT;
    float open = 0;
    if (kind == M_TALK) {
        /* the voice's own loudness when there is one; a murmur of its own when the level is not coming through */
        open = f->level > 0.03f ? f->level : 0.35f + 0.3f * sinf(t * 11.0f) * sinf(t * 3.1f);
        open = clampf(open);
    }
    switch (kind) {
    case M_CAT: /* the resting face: a small "w" */
        arc(g, x - 6.5f * u, y, 6.5f * u, 0, 180, 3.2f * u, c);
        arc(g, x + 6.5f * u, y, 6.5f * u, 0, 180, 3.2f * u, c);
        break;
    case M_SMILE: arc(g, x, y - 5 * u, 12 * u, 25, 155, 3.4f * u, c); break;
    case M_BIG: /* open and laughing: the lower half of a disc */
        arc(g, x, y - 5 * u, 15 * u, 0, 180, 15 * u, c);
        disc(g, x, y + 5.5f * u, 6.5f * u, 3.6f * u, PINK, 0.9f);
        break;
    case M_O: disc(g, x, y + 2 * u, 8 * u, 10 * u, c, 1); break;
    case M_SMALL_O: disc(g, x, y + 1 * u, 4.6f * u, 5.2f * u, c, 1); break;
    case M_YAWN: disc(g, x, y + 4 * u, 11 * u, 14 * u * (0.5f + 0.5f * sinf(clampf(t / 1.7f) * 3.14159f)) + 3 * u, c, 1); break;
    case M_TALK: disc(g, x, y + 1 * u, (7 + 2 * open) * u, (2.5f + 9 * open) * u, c, 1); break;
    case M_FLAT: line(g, x - 7 * u, y, x + 7 * u, y, 3.4f * u, c, 1); break;
    case M_FROWN: arc(g, x, y + 11 * u, 11 * u, 205, 335, 3.4f * u, c); break;
    default: break;
    }
}

int av_fx_rate(av_state_t s, double t, float level)
{
    switch (s) {
    case AV_S_SPEAK: return 30;
    case AV_S_EAT: return t < 2.8 ? 30 : 0;
    case AV_S_LOVE: case AV_S_PETTED: return t < 3.2 ? 30 : 0;
    case AV_S_BLUSH: return t < 2.8 ? 30 : 0;
    case AV_S_GIGGLE: case AV_S_HAPPY: case AV_S_FED: case AV_S_DANCE: return t < 2.2 ? 30 : 0;
    case AV_S_WINK: return t < 1.0 ? 30 : 0;
    case AV_S_SNEEZE: return t < 2.2 ? 30 : 0;
    case AV_S_SURPRISED: return t < 1.7 ? 30 : 0;
    case AV_S_YAWN: return t < 1.8 ? 20 : 0;
    case AV_S_THINK: return 12;
    case AV_S_SLEEP: case AV_S_SICK: case AV_S_SAD: return 10; /* slow things, drawn slowly: it may sleep all night */
    default: return level > 0.03f ? 30 : 0;
    }
}

unsigned av_fx_regions(av_state_t s, double t)
{
    switch (s) {
    case AV_S_SPEAK: case AV_S_YAWN: return AV_R_MOUTH;
    case AV_S_EAT: return AV_R_MOUTH | AV_R_BOTTOM | (t > 1.4 ? AV_R_CHEEKS | AV_R_SIDES : 0);
    case AV_S_LOVE: case AV_S_PETTED: case AV_S_BLUSH: return AV_R_TOP | AV_R_CHEEKS;
    case AV_S_GIGGLE: return AV_R_SIDES | AV_R_CHEEKS;
    case AV_S_HAPPY: case AV_S_FED: case AV_S_DANCE: return AV_R_SIDES;
    case AV_S_WINK: case AV_S_SLEEP: case AV_S_DROWSY: case AV_S_THINK: case AV_S_SURPRISED: return AV_R_RTOP;
    case AV_S_SICK: return AV_R_LTOP;
    case AV_S_SAD: return AV_R_TEAR;
    case AV_S_SNEEZE: return AV_R_BOTTOM | AV_R_RTOP;
    default: return AV_R_MOUTH;
    }
}

void av_fx_draw(lv_layer_t *layer, const av_fx_face_t *f, av_state_t s, double td)
{
    G g = { layer, f->u };
    float u = f->u, cx = f->cx, cy = f->cy, t = (float)td;
    float bl = 0;
    switch (s) {
    case AV_S_LOVE: bl = life(t, 0.05f, 3.0f, 0.12f, 0.3f); break;
    case AV_S_PETTED: bl = life(t, 0, 3.1f, 0.1f, 0.3f); break;
    case AV_S_BLUSH: bl = life(t, 0, 2.7f, 0.15f, 0.3f) * 1.2f; break;
    case AV_S_GIGGLE: bl = life(t, 0, 2.0f, 0.2f, 0.3f) * 0.7f; break;
    case AV_S_EAT: bl = life(t, 1.5f, 1.3f, 0.2f, 0.3f) * 0.8f; break;
    case AV_S_IDLE: case AV_S_LISTEN: case AV_S_SPEAK: bl = 0.45f; break; /* always a little: it is that kind of face */
    default: break;
    }
    blush(&g, f, bl);
    mouth(&g, f, s < AV_S_COUNT ? MOUTH[s] : M_NONE, t);

    switch (s) {
    case AV_S_LOVE: {
        static const float H[][5] = { { 0.15f, -118, 1.5f, 1.5f, 8 }, { 0.45f, 122, 1.5f, 1.2f, -7 }, { 0.8f, -96, 1.5f, 1.0f, 6 },
                                      { 1.15f, 108, 1.4f, 1.4f, -8 }, { 1.6f, -124, 1.3f, 1.1f, 7 } };
        hearts(&g, cx, cy, t, H, 5);
        break;
    }
    case AV_S_PETTED: {
        static const float H[][5] = { { 0.3f, 120, 1.5f, 1.3f, -7 }, { 0.9f, -120, 1.5f, 1.1f, 7 }, { 1.5f, 104, 1.4f, 1.5f, -6 },
                                      { 2.0f, -108, 1.1f, 1.0f, 6 } };
        hearts(&g, cx, cy, t, H, 4);
        break;
    }
    case AV_S_BLUSH: {
        static const float H[][5] = { { 0.9f, 118, 1.5f, 1.0f, -6 } };
        hearts(&g, cx, cy, t, H, 1);
        break;
    }
    case AV_S_GIGGLE: case AV_S_HAPPY: case AV_S_FED: case AV_S_DANCE: {
        static const float S[][4] = { { 0.08f, -130, -84, 1.3f }, { 0.38f, 128, -96, 1.1f }, { 0.7f, 118, -30, 0.8f },
                                      { 1.05f, -120, -34, 0.9f }, { 1.4f, 132, -70, 1.2f } };
        sparkles(&g, cx, cy, t, S, 5);
        break;
    }
    case AV_S_WINK: sparkle(&g, cx + 104 * u, cy - 76 * u, 14 * u * sinf(clampf(t / 0.8f) * 3.14159f), life(t, 0.1f, 0.8f, 0.3f, 0.4f)); break;
    case AV_S_SLEEP:
        for (int i = 0; i < 3; i++) {
            float p = fmodf(t + i * 1.2f, 3.6f) / 3.6f, o = p / 0.2f < (1 - p) / 0.35f ? p / 0.2f : (1 - p) / 0.35f;
            zed(&g, cx + (88 + p * 44 + sinf(p * 9) * 5) * u, cy - (78 + p * 60) * u, (5 + p * 7) * u, o > 1 ? 1 : o);
        }
        break;
    case AV_S_DROWSY: case AV_S_YAWN: zed(&g, cx + 104 * u, cy - (96 + clampf(t / 2) * 26) * u, 8 * u, life(t, 0.3f, 1.6f, 0.2f, 0.3f)); break;
    case AV_S_SICK: {
        float p = fmodf(t, 3.2f) / 3.2f, o = p / 0.15f < (1 - p) / 0.3f ? p / 0.15f : (1 - p) / 0.3f;
        drop(&g, cx - 80 * u, cy - (70 - ease(p) * 34) * u, 8 * u, o > 1 ? 1 : o);
        break;
    }
    case AV_S_SAD: {
        float p = fmodf(t, 2.6f) / 2.6f, o = p / 0.2f < (1 - p) / 0.3f ? p / 0.2f : (1 - p) / 0.3f;
        float lx = f->ex[0] < f->ex[1] ? f->ex[0] : f->ex[1];
        drop(&g, lx, (f->ey[0] > f->ey[1] ? f->ey[0] : f->ey[1]) + (30 + ease(p) * 40) * u, 6 * u, o > 1 ? 1 : o);
        break;
    }
    case AV_S_SNEEZE:
        for (int i = 0; i < 7; i++) {
            float a = 0.5f + i * 0.3f, p = clampf((t - 0.9f) / 0.52f);
            disc(&g, cx + cosf(a) * (40 + p * 90) * (i % 2 ? 0.9f : -0.9f) * u, cy + (60 + sinf(a) * 20 + p * (50 + i * 8)) * u,
                 (5 - p * 3) * u, (5 - p * 3) * u, 0xd6ecff, life(t, 0.9f, 0.52f, 0.1f, 0.5f));
        }
        break;
    case AV_S_THINK:
        for (int i = 0; i < 3; i++) {
            float o = 0.35f + 0.65f * (sinf(fmodf(t, 1.5f) / 1.5f * 6.283f - i * 0.9f) > 0 ? sinf(fmodf(t, 1.5f) / 1.5f * 6.283f - i * 0.9f) : 0);
            disc(&g, cx + (96 + i * 17) * u, cy - (104 + i * 9) * u, (4 + i * 1.6f) * u, (4 + i * 1.6f) * u, PALE, o);
        }
        break;
    case AV_S_SURPRISED: {
        float o = life(t, 0.06f, 1.5f, 0.08f, 0.4f), x = cx + 112 * u, y = cy - 112 * u;
        line(&g, x, y - 12 * u, x + 3 * u, y + 6 * u, 8 * u, 0xffd166, o);
        disc(&g, x + 5 * u, y + 20 * u, 5 * u, 5 * u, 0xffd166, o);
        break;
    }
    case AV_S_EAT: {
        /* a cookie comes up to the mouth and goes in three bites, with crumbs */
        float mx = (f->ex[0] + f->ex[1]) * 0.5f, my = (f->ey[0] + f->ey[1]) * 0.5f + 46 * u;
        float y = my + (64 - ease(t / 0.26f) * 30) * u, r = 22 * u, o = life(t, 0, 1.6f, 0.1f, 0.06f);
        int bites = (int)(t / 0.44f);
        if (bites > 3) bites = 3;
        if (o > 0.02f) {
            disc(&g, mx, y, r, r, 0xd59a55, o);
            disc(&g, mx - 8 * u, y - 5 * u, 3.6f * u, 3.6f * u, 0x6b3f1f, o);
            disc(&g, mx + 7 * u, y + 3 * u, 4.0f * u, 4.0f * u, 0x6b3f1f, o);
            disc(&g, mx - 2 * u, y + 10 * u, 3.2f * u, 3.2f * u, 0x6b3f1f, o);
            static const float B[3][2] = { { -14, 2 }, { 12, 5 }, { -1, 13 } };
            for (int i = 0; i < bites; i++) disc(&g, mx + B[i][0] * u, y - r + B[i][1] * u, 11 * u, 11 * u, f->body, o); /* a bite is the body showing through */
        }
        for (int i = 0; i < 6; i++) {
            float t0 = 0.3f + i * 0.19f, p = clampf((t - t0) / 0.6f);
            disc(&g, mx + (i % 2 ? 1 : -1) * (14 + p * (20 + i * 5)) * u, my + (30 + p * p * 60) * u, (3.2f - p * 1.4f) * u, (3.2f - p * 1.4f) * u,
                 0xd59a55, life(t, t0, 0.6f, 0.1f, 0.4f));
        }
        static const float S[][4] = { { 1.75f, -126, -80, 1.1f }, { 2.05f, 124, -70, 1.0f } };
        sparkles(&g, cx, cy, t, S, 2);
        break;
    }
    default: break;
    }
}
