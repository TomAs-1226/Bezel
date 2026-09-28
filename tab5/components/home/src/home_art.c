/* home_art — see home_art.h. Every cover is painted at HOME_ART_L into an RGB888 working picture with home_px's
 * antialiased shapes, then halved for the small one, and both are finished into 565 with grain and a dither.
 *
 * The palette comes first and the style draws only with it: a ground, a deep and a light, two mid tones and one
 * accent, all at chosen OKLCH lightness and chroma around one base hue in one of five harmonies. The style is a
 * composition, not a texture: few large shapes, one light direction (up and to the left, as Bezel's glass is lit),
 * soft shadows falling away from it. */
#include "home_art.h"
#include "home_px.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define S HOME_ART_L
#define TAU 6.2831853f

static const char *const STYLE_NAME[HOME_ART_STYLES] = { "mesh", "orb", "sky", "bands", "bauhaus", "rings" };

const char *home_art_style_name(int style)
{
    return style >= 0 && style < HOME_ART_STYLES ? STYLE_NAME[style] : "real";
}

/* ---------------------------------------------------------------- the key */

static uint32_t fold(const char *s, uint32_t h)
{
    /* lowercase, and runs of spaces as one, trimmed: "The Band " and "the band" are the same album */
    bool gap = false, any = false;
    if (!h) h = 2166136261u;
    for (; s && *s; s++) {
        char ch = (char)tolower((unsigned char)*s);
        if (ch == ' ' || ch == '\t') {
            gap = any; /* a space counts only once, and only between words */
            continue;
        }
        if (gap) {
            h ^= (uint8_t)' ';
            h *= 16777619u;
            gap = false;
        }
        h ^= (uint8_t)ch;
        h *= 16777619u;
        any = true;
    }
    return h;
}

uint32_t home_art_key(const char *artist, const char *album, const char *title)
{
    bool ar = artist && artist[0], al = album && album[0], ti = title && title[0];
    uint32_t h;
    if (ar && al) h = fold(album, fold("\x1f", fold(artist, 0)));
    else if (ti) h = fold(title, fold("\x1e", ar ? fold(artist, 0) : 0));
    else if (ar) h = fold(artist, 0);
    else h = 0x5eed;
    h = hpx_hash(h);
    return h ? h : 1;
}

/* ---------------------------------------------------------------- the palette */

typedef struct {
    bool dark;              /* the cover's own mood: deep (most) or pale */
    float h0;
    uint32_t bg, deep, mid1, mid2, light, accent;
} pal_t;

/* A colour of the palette. Yellow is only yellow when it is light: darker, its hues read as olive and mud, so a
 * yellow asked for below a light tone turns to the nearer of amber and green. */
static uint32_t col(float L, float C, float h)
{
    float hh = fmodf(h, 360.0f);
    if (hh < 0) hh += 360.0f;
    if (hh > 72 && hh < 128 && L < 0.76f) hh = hh < 100 ? 62.0f : 142.0f;
    return hpx_oklch(L, C, hh);
}

static void palette(hpx_rng_t *r, pal_t *p)
{
    float h0 = hpx_rand(r) * 360.0f;
    int scheme = (int)(hpx_next(r) % 5);
    float ha, hb, hc; /* the two mid tones' hues and the accent's */
    switch (scheme) {
    case 0: ha = h0 + 26; hb = h0 + 54; hc = h0 - 38; break;    /* analogous */
    case 1: ha = h0 + 16; hb = h0 + 180; hc = h0 + 192; break;  /* complementary */
    case 2: ha = h0 + 150; hb = h0 + 210; hc = h0 + 150; break; /* split complementary */
    case 3: ha = h0 + 120; hb = h0 + 240; hc = h0 + 120; break; /* triad, a little muted */
    default: ha = h0 + 8; hb = h0 - 12; hc = h0 + 180; break;   /* one hue, and its opposite as a spark */
    }
    float cm = scheme == 3 ? 0.78f : 1.0f;
    p->h0 = h0;
    p->dark = hpx_rand(r) < 0.62f;
    if (p->dark) {
        p->bg = col(hpx_range(r, 0.17f, 0.23f), hpx_range(r, 0.03f, 0.07f), h0);
        p->deep = col(hpx_range(r, 0.29f, 0.37f), hpx_range(r, 0.06f, 0.10f) * cm, h0 + 12);
        p->mid1 = col(hpx_range(r, 0.50f, 0.62f), hpx_range(r, 0.11f, 0.17f) * cm, ha);
        p->mid2 = col(hpx_range(r, 0.60f, 0.72f), hpx_range(r, 0.08f, 0.14f) * cm, hb);
        p->light = col(hpx_range(r, 0.88f, 0.94f), hpx_range(r, 0.03f, 0.06f), ha);
        p->accent = col(hpx_range(r, 0.72f, 0.82f), hpx_range(r, 0.15f, 0.21f), hc);
    } else {
        p->bg = col(hpx_range(r, 0.89f, 0.95f), hpx_range(r, 0.02f, 0.05f), h0);
        p->deep = col(hpx_range(r, 0.28f, 0.38f), hpx_range(r, 0.05f, 0.10f) * cm, hb);
        p->mid1 = col(hpx_range(r, 0.62f, 0.74f), hpx_range(r, 0.10f, 0.16f) * cm, ha);
        p->mid2 = col(hpx_range(r, 0.76f, 0.85f), hpx_range(r, 0.06f, 0.11f) * cm, hb);
        p->light = col(hpx_range(r, 0.96f, 0.99f), hpx_range(r, 0.01f, 0.03f), h0);
        p->accent = col(hpx_range(r, 0.63f, 0.72f), hpx_range(r, 0.16f, 0.22f), hc);
    }
}

/* the palette's colours in an order the key chose, for styles that deal them out */
static void deal(hpx_rng_t *r, const pal_t *p, uint32_t *out, int n)
{
    uint32_t c[5] = { p->deep, p->mid1, p->mid2, p->accent, p->dark ? p->light : p->bg };
    for (int i = 4; i > 0; i--) {
        int j = (int)(hpx_next(r) % (uint32_t)(i + 1));
        uint32_t t = c[i];
        c[i] = c[j];
        c[j] = t;
    }
    for (int i = 0; i < n; i++) out[i] = c[i % 5];
}

/* ---------------------------------------------------------------- styles */

/* A soft gradient mesh: five large blobs of the palette pooled over the ground, their weights a gaussian of
 * distance, normalised (so colours meet as a blend, never as a painted edge), the field gently warped. Averaging
 * two far hues greys them (teal and pink meet as mud), so the blend is pushed back out from its own grey. */
static void paint_mesh(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    enum { N = 5 };
    float bx[N], by[N], k[N];
    int cr[N], cg[N], cb[N];
    uint32_t col[N] = { p->mid1, p->mid2, p->accent, p->deep, p->dark ? p->bg : p->light };
    for (int i = 0; i < N; i++) {
        bx[i] = hpx_range(r, -0.1f, 1.1f) * S;
        by[i] = hpx_range(r, -0.1f, 1.1f) * S;
        float rad = hpx_range(r, 0.34f, 0.62f) * S;
        k[i] = 64.0f / (rad * rad);
        cr[i] = hpx_r(col[i]);
        cg[i] = hpx_g(col[i]);
        cb[i] = hpx_b(col[i]);
    }
    /* weights: exp(-2.3 t) for t = d²/r², 64 steps a unit, out to t = 8; and the warp, a slow sine across each
     * axis. On the heap: the home component's statics and a worker's stack are internal RAM. */
    uint16_t *G = malloc(sizeof(uint16_t) * 513);
    float *wx = malloc(sizeof(float) * 2 * S), *wy = wx ? wx + S : NULL;
    if (!G || !wx) {
        free(G);
        free(wx);
        return;
    }
    for (int i = 0; i <= 512; i++) G[i] = (uint16_t)(1024.0f * expf(-2.8f * i / 64.0f) + 0.5f);
    int wb = 24; /* the ground's own weight: where no blob reaches, the ground shows */
    int gr = hpx_r(p->bg), gg = hpx_g(p->bg), gb = hpx_b(p->bg);
    float fa = hpx_range(r, 1.2f, 2.4f), fb = hpx_range(r, 1.2f, 2.4f), pa = hpx_rand(r) * TAU, pb = hpx_rand(r) * TAU;
    for (int i = 0; i < S; i++) {
        wx[i] = 0.07f * S * sinf(fa * TAU * i / S + pa);
        wy[i] = 0.07f * S * sinf(fb * TAU * i / S + pb);
    }
    for (int y = 0; y < S; y++) {
        uint32_t *row = &c->p32[(size_t)y * c->stride];
        for (int x = 0; x < S; x++) {
            float sx = (float)x + wy[y], sy = (float)y + wx[x];
            int ws = wb, ar = gr * wb, ag = gg * wb, ab = gb * wb;
            for (int i = 0; i < N; i++) {
                float dx = sx - bx[i], dy = sy - by[i];
                int t = (int)((dx * dx + dy * dy) * k[i]);
                if (t > 512) continue;
                int w = G[t];
                ws += w;
                ar += cr[i] * w;
                ag += cg[i] * w;
                ab += cb[i] * w;
            }
            int rr = ar / ws, gg2 = ag / ws, bb = ab / ws;
            int l = (rr * 54 + gg2 * 183 + bb * 19) >> 8;
            row[x] = hpx_rgb(l + ((rr - l) * 83 >> 6), l + ((gg2 - l) * 83 >> 6), l + ((bb - l) * 83 >> 6));
        }
    }
    free(G);
    free(wx);
}

/* Bezel's orb: a diagonal ground, thin rings round a lit sphere, its shadow falling down and to the right. */
static void paint_orb(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    uint32_t g0 = p->dark ? p->bg : p->light, g1 = p->dark ? p->deep : p->mid2;
    uint32_t stops[2] = { g0, g1 };
    int pos[2] = { 0, 256 };
    hpx_linear(c, 0, 0, 0.7071f, 0.7071f, S * 1.41f, stops, pos, 2, 255);
    float cx = hpx_range(r, 0.40f, 0.60f) * S, cy = hpx_range(r, 0.48f, 0.62f) * S, R = hpx_range(r, 0.20f, 0.28f) * S;
    uint32_t ring = p->dark ? p->light : p->deep;
    int rings = 5 + (int)(hpx_next(r) % 4);
    float step = hpx_range(r, 0.028f, 0.045f) * S;
    for (int i = 1; i <= rings; i++) hpx_ring(c, cx, cy, R + i * step, 0.0045f * S, 0, ring, p->dark ? 70 - i * 5 : 60 - i * 5);
    hpx_disc(c, cx + 0.035f * S, cy + 0.06f * S, R * 1.02f, 0.16f * S, 0x000000, p->dark ? 150 : 70);
    /* the sphere: lit from up-left, a gradient from the highlight to the body colour, darker on the far rim */
    uint32_t hi = p->light, body = p->accent, rim = p->dark ? p->deep : p->mid1;
    float lx = cx - R * 0.38f, ly = cy - R * 0.45f;
    hpx_t sub = *c;
    hpx_clip(&sub, (int)(cx - R - 2), (int)(cy - R - 2), (int)(cx + R + 2), (int)(cy + R + 2));
    for (int y = sub.y1; y <= sub.y2; y++)
        for (int x = sub.x1; x <= sub.x2; x++) {
            float fx = x + 0.5f - cx, fy = y + 0.5f - cy, d = sqrtf(fx * fx + fy * fy);
            float cv = R + 0.5f - d;
            if (cv <= 0) continue;
            float hx = x + 0.5f - lx, hy = y + 0.5f - ly;
            float t = sqrtf(hx * hx + hy * hy) / (R * 1.45f);
            t = t > 1 ? 1 : t;
            uint32_t col = hpx_mix(hi, body, (int)(sqrtf(t) * 256));
            float e = d / R; /* the far rim: a little of the rim colour past 80 % of the radius, away from the light */
            if (e > 0.78f && (fx * 0.42f + fy * 0.91f) > 0) col = hpx_mix(col, rim, (int)((e - 0.78f) / 0.22f * 90));
            hpx_put(&sub, x, y, col, cv >= 1 ? 255 : (int)(cv * 255));
        }
}

/* A sky: four stops of gradient, stars on a dark one, a sun with a halo, three ridges receding. */
static void paint_sky(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    float L0, C0, h0;
    hpx_to_oklch(p->mid1, &L0, &C0, &h0);
    float ha, La, Ca;
    hpx_to_oklch(p->accent, &La, &Ca, &ha);
    uint32_t stops[4];
    if (p->dark) {
        stops[0] = col(0.16f, 0.05f, p->h0);
        stops[1] = col(0.30f, 0.10f, h0);
        stops[2] = col(0.46f, 0.12f, ha);
        stops[3] = col(0.14f, 0.04f, p->h0);
    } else {
        stops[0] = col(0.72f, 0.09f, h0);
        stops[1] = col(0.84f, 0.07f, h0);
        stops[2] = col(0.92f, 0.06f, ha);
        stops[3] = col(0.86f, 0.04f, h0);
    }
    int pos[4] = { 0, 128, 200, 256 };
    hpx_linear(c, 0, 0, 0, 1, S, stops, pos, 4, 255);
    if (p->dark) {
        for (int i = 0; i < 520; i++) {
            int x = (int)(hpx_rand(r) * S), y = (int)(hpx_rand(r) * S * 0.62f);
            int a = (int)(hpx_rand(r) * 150);
            hpx_put(c, x, y, p->light, a);
            if (hpx_rand(r) < 0.08f && x + 1 < S && y + 1 < S) {
                hpx_put(c, x + 1, y, p->light, a);
                hpx_put(c, x, y + 1, p->light, a);
                hpx_put(c, x + 1, y + 1, p->light, a);
            }
        }
    }
    float sx = hpx_range(r, 0.25f, 0.75f) * S, sy = hpx_range(r, 0.30f, 0.60f) * S, sr = hpx_range(r, 0.07f, 0.17f) * S;
    uint32_t sun_top = p->dark ? col(0.93f, 0.06f, ha + 20) : col(0.98f, 0.04f, ha + 20);
    uint32_t sun_bot = p->accent;
    hpx_glow(c, sx, sy, sr * 2.8f, p->accent, p->dark ? 120 : 90);
    {
        hpx_t sub = *c;
        hpx_clip(&sub, (int)(sx - sr - 2), (int)(sy - sr - 2), (int)(sx + sr + 2), (int)(sy + sr + 2));
        for (int y = sub.y1; y <= sub.y2; y++) {
            uint32_t col = hpx_mix(sun_top, sun_bot, (int)((y - (sy - sr)) / (2 * sr) * 256));
            for (int x = sub.x1; x <= sub.x2; x++) {
                float fx = x + 0.5f - sx, fy = y + 0.5f - sy;
                float cv = sr + 0.5f - sqrtf(fx * fx + fy * fy);
                if (cv > 0) hpx_put(&sub, x, y, col, cv >= 1 ? 255 : (int)(cv * 255));
            }
        }
    }
    /* the ridges: sums of noise at three scales, each nearer one lower, darker and rougher */
    float *top = malloc(sizeof(float) * S);
    if (!top) return;
    uint32_t seed = hpx_next(r);
    for (int k = 0; k < 3; k++) {
        float base = (0.66f + 0.10f * k) * S, amp = (0.10f - 0.022f * k) * S;
        float f = 2.2f + 1.4f * k, off = hpx_rand(r) * 50.0f;
        for (int x = 0; x < S; x++) {
            float u = (float)x / S * f + off;
            float n = hpx_noise1(u, seed + k) * 0.62f + hpx_noise1(u * 2.7f, seed + 11 + k) * 0.26f +
                      hpx_noise1(u * 6.3f, seed + 23 + k) * 0.12f;
            top[x] = base + (n - 0.5f) * 2.0f * amp;
        }
        uint32_t rc = p->dark ? hpx_mix(stops[2], col(0.07f, 0.02f, p->h0), 150 + k * 50)
                               : hpx_mix(stops[2], p->deep, 70 + k * 70);
        hpx_ridge(c, top, 0, rc, 255);
    }
    free(top);
}

/* Bands: the palette as stripes of different widths, turned, and (half the time) a disc through which the stripes
 * run in the other order — a poster's sun. Coverage along the stripes' axis is exact, so their edges are clean. */
static void paint_bands(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    static const float ANG[] = { -0.42f, 0.42f, 0.0f, 1.5708f, -0.78f, 0.25f };
    float ang = ANG[hpx_next(r) % 6] + hpx_range(r, -0.08f, 0.08f);
    float ca = cosf(ang), sa = sinf(ang);
    /* t runs across the stripes: -S..2S covers the square turned any way */
    enum { T0 = -S, TN = 3 * S };
    uint32_t(*lut)[TN + 2] = malloc(sizeof(uint32_t) * 2 * (TN + 2));
    if (!lut) return;
    uint32_t col[10];
    deal(r, p, col, 10);
    uint32_t col2[10];
    for (int i = 0; i < 10; i++) col2[i] = col[(i + 2) % 10];
    for (int v = 0; v < 2; v++) {
        hpx_rng_t rr = { r->s }; /* the same widths both times: only the order of the colours differs */
        float t = 0;
        int i = 0;
        while (t <= TN + 1) {
            float wdt = hpx_range(&rr, 0.035f, 0.14f) * S;
            int a = (int)t, b = (int)(t + wdt);
            for (int k = a; k <= b && k <= TN + 1; k++) lut[v][k] = (v ? col2 : col)[i % 5];
            t += wdt;
            i++;
        }
    }
    bool sun = hpx_rand(r) < 0.5f;
    float sx = hpx_range(r, 0.35f, 0.65f) * S, sy = hpx_range(r, 0.35f, 0.65f) * S, sr = hpx_range(r, 0.22f, 0.32f) * S;
    for (int y = 0; y < S; y++) {
        uint32_t *row = &c->p32[(size_t)y * c->stride];
        for (int x = 0; x < S; x++) {
            float tf = (x + 0.5f - S * 0.5f) * ca + (y + 0.5f - S * 0.5f) * sa + S * 0.5f - T0;
            int ti = (int)tf;
            int fr = (int)((tf - ti) * 256);
            uint32_t a = hpx_mix(lut[0][ti], lut[0][ti + 1], fr);
            if (sun) {
                float fx = x + 0.5f - sx, fy = y + 0.5f - sy;
                float cv = sr + 0.5f - sqrtf(fx * fx + fy * fy);
                if (cv > 0) {
                    uint32_t b = hpx_mix(lut[1][ti], lut[1][ti + 1], fr);
                    a = cv >= 1 ? b : hpx_mix(a, b, (int)(cv * 256));
                }
            }
            row[x] = a;
        }
    }
    free(lut);
}

/* Bauhaus: a 2 × 2 or 3 × 3 grid of tiles, each a ground and one figure (a quarter circle from a corner, a half
 * circle on an edge, a circle, a triangle, bars), the colours dealt so a figure never vanishes into its ground. */
static void paint_bauhaus(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    int n = hpx_rand(r) < 0.55f ? 2 : 3;
    float t = (float)S / n;
    uint32_t set[6] = { p->bg, p->deep, p->mid1, p->mid2, p->accent, p->dark ? p->light : 0x1b1b1f };
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float x = i * t, y = j * t;
            int gi = (int)(hpx_next(r) % 6), fi = (gi + 1) % 6;
            for (int tries = 0; tries < 24; tries++) {
                fi = (int)(hpx_next(r) % 6);
                if (fi != gi && abs(hpx_luma(set[fi]) - hpx_luma(set[gi])) >= 36) break;
            }
            if (fi == gi) fi = (gi + 1) % 6;
            uint32_t g = set[gi], f = set[fi];
            hpx_t sub = *c;
            hpx_clip(&sub, (int)x, (int)y, (int)(x + t) - 1, (int)(y + t) - 1);
            hpx_fill(&sub, g);
            int kind = (int)(hpx_next(r) % 6), corner = (int)(hpx_next(r) % 4);
            float qx = x + (corner & 1 ? t : 0), qy = y + (corner & 2 ? t : 0);
            switch (kind) {
            case 0: /* a quarter circle out of a corner */
                hpx_disc(&sub, qx, qy, t, 0, f, 255);
                break;
            case 1: { /* a half circle on an edge */
                float ex = x + t * 0.5f, ey = y + t * 0.5f;
                if (corner == 0) ey = y;
                else if (corner == 1) ex = x + t;
                else if (corner == 2) ey = y + t;
                else ex = x;
                hpx_disc(&sub, ex, ey, t * 0.5f, 0, f, 255);
                break;
            }
            case 2: /* a circle, and a smaller one in it */
                hpx_disc(&sub, x + t * 0.5f, y + t * 0.5f, t * 0.36f, 0, f, 255);
                if (hpx_rand(r) < 0.5f) hpx_disc(&sub, x + t * 0.5f, y + t * 0.5f, t * 0.16f, 0, g, 255);
                break;
            case 3: /* a triangle: two corners and the middle of the far side */
                if (corner & 1) hpx_tri(&sub, x, y, x, y + t, x + t, y + t * 0.5f, f, 255);
                else hpx_tri(&sub, x, y, x + t, y, x + t * 0.5f, y + t, f, 255);
                break;
            case 4: /* bars */
                for (int b = 0; b < 3; b++) {
                    float w = t / 7;
                    if (corner & 1) hpx_rrect(&sub, x + w * (1 + 2 * b), y + w, w, t - 2 * w, w * 0.5f, 0, f, 255);
                    else hpx_rrect(&sub, x + w, y + w * (1 + 2 * b), t - 2 * w, w, w * 0.5f, 0, f, 255);
                }
                break;
            default: /* split on the diagonal */
                if (corner & 1) hpx_tri(&sub, x, y, x + t, y, x, y + t, f, 255);
                else hpx_tri(&sub, x + t, y, x + t, y + t, x, y + t, f, 255);
                break;
            }
        }
}

/* Concentric rings from a point off the middle, each a little displaced from the last, toward the light: a
 * ripple, or a record, or a planet's orbits. */
static void paint_rings(hpx_t *c, hpx_rng_t *r, const pal_t *p)
{
    uint32_t col[10];
    deal(r, p, col, 10);
    hpx_fill(c, p->dark ? p->bg : p->light);
    float cx = hpx_range(r, 0.3f, 0.7f) * S, cy = hpx_range(r, 0.3f, 0.7f) * S;
    float drift = hpx_range(r, 0.0f, 0.035f) * S;
    int n = 6 + (int)(hpx_next(r) % 5);
    float R = S * 1.15f;
    float widths[12], sum = 0;
    for (int i = 0; i < n; i++) sum += widths[i] = hpx_range(r, 0.5f, 1.6f);
    float rr = R;
    for (int i = 0; i < n; i++) {
        float k = (float)i / n;
        float ox = cx - drift * 0.42f * k * 6, oy = cy - drift * 0.91f * k * 6;
        if (i) hpx_disc(c, ox + 0.012f * S, oy + 0.02f * S, rr, 0.05f * S, 0x000000, p->dark ? 60 : 30);
        hpx_disc(c, ox, oy, rr, 0, col[i % 5], 255);
        rr -= R * widths[i] / sum * 0.92f;
        if (rr < 4) break;
    }
}

/* ---------------------------------------------------------------- painting */

void home_art_paint(uint32_t key, int style, uint32_t *work, uint16_t *small, home_art_info_t *info)
{
    hpx_rng_t r = { key };
    for (int i = 0; i < 3; i++) hpx_next(&r);
    pal_t p;
    palette(&r, &p);
    if (style < 0 || style >= HOME_ART_STYLES) {
        /* the mesh a little more often: it is the one that suits anything */
        static const uint8_t PICK[8] = { HOME_ART_MESH, HOME_ART_MESH, HOME_ART_ORB, HOME_ART_SKY, HOME_ART_BANDS,
                                         HOME_ART_BAUHAUS, HOME_ART_RINGS, HOME_ART_SKY };
        style = PICK[hpx_hash(key ^ 0xA5A5u) % 8];
    }
    hpx_t c;
    hpx_init32(&c, work, S, S, S);
    hpx_fill(&c, p.bg);
    switch (style) {
    case HOME_ART_MESH: paint_mesh(&c, &r, &p); break;
    case HOME_ART_ORB: paint_orb(&c, &r, &p); break;
    case HOME_ART_SKY: paint_sky(&c, &r, &p); break;
    case HOME_ART_BANDS: paint_bands(&c, &r, &p); break;
    case HOME_ART_BAUHAUS: paint_bauhaus(&c, &r, &p); break;
    default: paint_rings(&c, &r, &p); break;
    }
    if (info) {
        info->style = style;
        info->dark = p.dark;
        info->seed = p.mid1;
        info->colors[0] = p.bg;
        info->colors[1] = p.deep;
        info->colors[2] = p.mid1;
        info->colors[3] = p.mid2;
        info->colors[4] = p.accent;
    }
    if (small) hpx_half565(work, S, S, S, small, HOME_ART_S, p.dark ? 9 : 7, key);
}

/* The colour Monet would pick from a picture: the most common of its vivid colours. Buckets of 4 bits a channel,
 * each scored by its count and its chroma; the winner is its bucket's mean. Greys only if nothing else. */
static uint32_t seed_of(const uint32_t *px, int w, int h, int stride, int *luma)
{
    enum { NB = 4096 };
    uint32_t *cnt = calloc(NB, sizeof(uint32_t) * 4);
    if (!cnt) return 0x808080;
    uint32_t lsum = 0, ln = 0;
    for (int y = 0; y < h; y += 3)
        for (int x = 0; x < w; x += 3) {
            uint32_t v = px[(size_t)y * stride + x];
            int b = (hpx_r(v) >> 4) << 8 | (hpx_g(v) >> 4) << 4 | hpx_b(v) >> 4;
            cnt[b * 4]++;
            cnt[b * 4 + 1] += (uint32_t)hpx_r(v);
            cnt[b * 4 + 2] += (uint32_t)hpx_g(v);
            cnt[b * 4 + 3] += (uint32_t)hpx_b(v);
            lsum += (uint32_t)hpx_luma(v);
            ln++;
        }
    float best = -1;
    uint32_t pick = 0x808080;
    for (int b = 0; b < NB; b++) {
        uint32_t n = cnt[b * 4];
        if (!n) continue;
        uint32_t mean = hpx_rgb((int)(cnt[b * 4 + 1] / n), (int)(cnt[b * 4 + 2] / n), (int)(cnt[b * 4 + 3] / n));
        int mx = hpx_r(mean), mn = mx;
        if (hpx_g(mean) > mx) mx = hpx_g(mean);
        if (hpx_b(mean) > mx) mx = hpx_b(mean);
        if (hpx_g(mean) < mn) mn = hpx_g(mean);
        if (hpx_b(mean) < mn) mn = hpx_b(mean);
        int lum = hpx_luma(mean);
        /* vivid and neither near black nor near white */
        float chroma = (float)(mx - mn) / 255.0f;
        float score = (float)n * (0.08f + chroma * chroma * 4.0f) * (lum < 24 || lum > 236 ? 0.2f : 1.0f);
        if (score > best) {
            best = score;
            pick = mean;
        }
    }
    free(cnt);
    if (luma) *luma = ln ? (int)(lsum / ln) : 128;
    return pick;
}

void home_art_adopt(const uint16_t *big, uint32_t *work, uint16_t *small, home_art_info_t *info)
{
    for (int i = 0; i < S * S; i++) work[i] = hpx_from565(big[i]);
    if (info) {
        int luma = 128;
        info->style = -1;
        info->seed = seed_of(work, S, S, S, &luma);
        info->dark = luma < 110;
        for (int i = 0; i < 5; i++) info->colors[i] = work[(size_t)(S / 6 + i * S / 6) * S + S / 2];
    }
    if (small) hpx_half565(work, S, S, S, small, HOME_ART_S, 0, 1);
}

/* ---------------------------------------------------------------- the hero */

void home_art_hero(const uint32_t *work, uint16_t *px, int w, int h, int cx, int cy, int radius, uint16_t ground,
                   bool dark, uint32_t seed)
{
    for (int i = 0; i < w * h; i++) px[i] = ground;
    hpx_t c;
    hpx_init16(&c, px, w, h, w);
    /* the shadow: a little of the seed's hue in it, so it reads as the cover's own and not as grey dirt */
    float L, C, hh;
    hpx_to_oklch(seed, &L, &C, &hh);
    uint32_t sh = hpx_oklch(dark ? 0.06f : 0.35f, C * 0.4f, hh);
    hpx_rrect(&c, cx + S * 0.02f, cy + S * 0.05f, S, S, (float)radius + 6, S * 0.12f, sh, dark ? 190 : 80);
    /* the cover, its corners rounded by coverage against what is already there */
    float rr = (float)radius;
    for (int y = 0; y < S; y++) {
        const uint32_t *s = &work[(size_t)y * S];
        int py = cy + y;
        if (py < 0 || py >= h) continue;
        float fy = (y + 0.5f < rr) ? rr - (y + 0.5f) : (y + 0.5f > S - rr) ? (y + 0.5f) - (S - rr) : 0;
        uint32_t hy = hpx_hash(seed ^ (uint32_t)y * 0x9E3779B9u);
        for (int x = 0; x < S; x++) {
            int pxx = cx + x;
            if (pxx < 0 || pxx >= w) continue;
            float fx = (x + 0.5f < rr) ? rr - (x + 0.5f) : (x + 0.5f > S - rr) ? (x + 0.5f) - (S - rr) : 0;
            int a = 255;
            if (fx > 0 && fy > 0) {
                float d = sqrtf(fx * fx + fy * fy) - rr;
                if (d >= 0.5f) continue;
                if (d > -0.5f) a = (int)((0.5f - d) * 255);
            }
            /* the same grain the small one has, so both sizes look like one print */
            uint32_t v = s[x];
            int n = (int)(hpx_hash(hy + (uint32_t)x * 0x85EBCA6Bu) & 255) * 10 / 256 - 5;
            v = hpx_rgb(hpx_r(v) + n, hpx_g(v) + n, hpx_b(v) + n);
            hpx_put(&c, pxx, py, v, a);
        }
    }
}

/* ---------------------------------------------------------------- the tint */

static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void home_art_tint(uint32_t seed, bool dark, home_tint_t *o)
{
    float L, C, h;
    hpx_to_oklch(seed, &L, &C, &h);
    (void)L;
    if (dark) {
        o->ground = hpx_oklch(0.19f, minf(C * 0.40f, 0.05f), h);
        o->surface = hpx_oklch(0.245f, minf(C * 0.45f, 0.055f), h);
        o->surface2 = hpx_oklch(0.31f, minf(C * 0.50f, 0.06f), h);
        o->track = hpx_oklch(0.34f, minf(C * 0.45f, 0.055f), h);
        o->ink = 0xEEF0F2;
        o->dim = hpx_oklch(0.78f, minf(C * 0.25f, 0.035f), h);
        o->accent = hpx_oklch(0.82f, clampf(C, 0.05f, 0.13f), h);
        o->on_accent = hpx_oklch(0.22f, minf(C * 0.5f, 0.06f), h);
    } else {
        o->ground = hpx_oklch(0.93f, minf(C * 0.28f, 0.032f), h);
        o->surface = hpx_oklch(0.985f, minf(C * 0.12f, 0.012f), h);
        o->surface2 = hpx_oklch(0.885f, minf(C * 0.30f, 0.035f), h);
        o->track = hpx_oklch(0.85f, minf(C * 0.30f, 0.04f), h);
        o->ink = 0x15181B;
        o->dim = hpx_oklch(0.45f, minf(C * 0.30f, 0.04f), h);
        o->accent = hpx_oklch(0.84f, clampf(C, 0.05f, 0.12f), h);
        o->on_accent = hpx_oklch(0.24f, minf(C * 0.5f, 0.07f), h);
    }
}
