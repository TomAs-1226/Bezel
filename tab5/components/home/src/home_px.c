/* home_px — see home_px.h. Coverage from signed distance, one row span at a time: the part of a row a shape can
 * touch is found first (so a thin diagonal hand scans its own few pixels a row, not its bounding box), the part it
 * surely covers is filled without a distance, and only the edge pixels pay for a square root. */
#include "home_px.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- pictures */

void hpx_init16(hpx_t *c, uint16_t *px, int w, int h, int stride)
{
    memset(c, 0, sizeof *c);
    c->p16 = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    hpx_unclip(c);
}

void hpx_init32(hpx_t *c, uint32_t *px, int w, int h, int stride)
{
    memset(c, 0, sizeof *c);
    c->p32 = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    hpx_unclip(c);
}

void hpx_unclip(hpx_t *c)
{
    c->x1 = c->y1 = 0;
    c->x2 = c->w - 1;
    c->y2 = c->h - 1;
}

void hpx_clip(hpx_t *c, int x1, int y1, int x2, int y2)
{
    c->x1 = x1 < 0 ? 0 : x1;
    c->y1 = y1 < 0 ? 0 : y1;
    c->x2 = x2 > c->w - 1 ? c->w - 1 : x2;
    c->y2 = y2 > c->h - 1 ? c->h - 1 : y2;
}

uint32_t hpx_mix(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    int r = hpx_r(a) + ((hpx_r(b) - hpx_r(a)) * t >> 8);
    int g = hpx_g(a) + ((hpx_g(b) - hpx_g(a)) * t >> 8);
    int bl = hpx_b(a) + ((hpx_b(b) - hpx_b(a)) * t >> 8);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

/* 4×4 Bayer thresholds, 0..255 */
static const uint8_t BAYER[4][4] = {
    { 8, 136, 40, 168 }, { 200, 72, 232, 104 }, { 56, 184, 24, 152 }, { 248, 120, 216, 88 },
};

static inline uint16_t dither565(int r, int g, int b, int x, int y)
{
    int d = BAYER[y & 3][x & 3];
    int r5 = (r * 31 + d) / 255, g6 = (g * 63 + d) / 255, b5 = (b * 31 + d) / 255;
    return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

static inline void put(hpx_t *c, int x, int y, uint32_t rgb, int a)
{
    if (a <= 0) return;
    if (c->p32) {
        uint32_t *p = &c->p32[(size_t)y * c->stride + x];
        *p = a >= 255 ? rgb : hpx_mix(*p, rgb, a + (a >> 7));
        return;
    }
    uint16_t *p = &c->p16[(size_t)y * c->stride + x];
    int r = hpx_r(rgb), g = hpx_g(rgb), b = hpx_b(rgb);
    if (a < 255) {
        uint32_t o = hpx_from565(*p);
        int t = a + (a >> 7);
        r = hpx_r(o) + ((r - hpx_r(o)) * t >> 8);
        g = hpx_g(o) + ((g - hpx_g(o)) * t >> 8);
        b = hpx_b(o) + ((b - hpx_b(o)) * t >> 8);
    }
    *p = dither565(r, g, b, x, y);
}

void hpx_put(hpx_t *c, int x, int y, uint32_t rgb, int a) { put(c, x, y, rgb, a); }

static void span(hpx_t *c, int y, int xa, int xb, uint32_t rgb, int a)
{
    if (y < c->y1 || y > c->y2) return;
    if (xa < c->x1) xa = c->x1;
    if (xb > c->x2) xb = c->x2;
    if (xa > xb || a <= 0) return;
    if (a >= 255 && c->p32) {
        uint32_t *p = &c->p32[(size_t)y * c->stride + xa];
        for (int x = xa; x <= xb; x++) *p++ = rgb;
        return;
    }
    if (a >= 255) {
        /* a solid run in 565: the four dithered values of this row, repeated */
        uint16_t v[4];
        for (int k = 0; k < 4; k++) v[k] = dither565(hpx_r(rgb), hpx_g(rgb), hpx_b(rgb), k, y);
        uint16_t *p = &c->p16[(size_t)y * c->stride];
        for (int x = xa; x <= xb; x++) p[x] = v[x & 3];
        return;
    }
    for (int x = xa; x <= xb; x++) put(c, x, y, rgb, a);
}

void hpx_fill(hpx_t *c, uint32_t rgb)
{
    for (int y = c->y1; y <= c->y2; y++) span(c, y, c->x1, c->x2, rgb, 255);
}

void hpx_copy_from(hpx_t *c, const void *src, int src_stride)
{
    size_t bpp = c->p32 ? 4 : 2;
    int n = c->x2 - c->x1 + 1;
    if (n <= 0) return;
    for (int y = c->y1; y <= c->y2; y++) {
        const uint8_t *s = (const uint8_t *)src + ((size_t)y * src_stride + c->x1) * bpp;
        uint8_t *d = c->p32 ? (uint8_t *)&c->p32[(size_t)y * c->stride + c->x1] : (uint8_t *)&c->p16[(size_t)y * c->stride + c->x1];
        memcpy(d, s, (size_t)n * bpp);
    }
}

/* ---------------------------------------------------------------- coverage */

/* How far outside the geometric edge a shape still covers anything, and how far inside it is less than full */
static inline float reach(float soft) { return soft > 1.0f ? soft * 0.5f : 0.5f; }

/* coverage 0..255 of a pixel whose centre is d px outside the edge (negative: inside) */
static inline int cov(float d, float soft)
{
    if (soft <= 1.0f) {
        float v = 0.5f - d;
        return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f);
    }
    float t = 0.5f - d / soft;
    if (t <= 0) return 0;
    if (t >= 1) return 255;
    return (int)(t * t * (3.0f - 2.0f * t) * 255.0f);
}

static inline int ifloor(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline int iceil(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }

/* ---------------------------------------------------------------- shapes */

void hpx_disc(hpx_t *c, float cx, float cy, float r, float soft, uint32_t rgb, int a)
{
    float e = reach(soft), ro = r + e + 0.71f, ri = r - e - 0.71f;
    int ya = ifloor(cy - ro), yb = iceil(cy + ro);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    for (int y = ya; y <= yb; y++) {
        float fy = (float)y + 0.5f - cy, fy2 = fy * fy;
        if (fy2 >= ro * ro) continue;
        float ho = sqrtf(ro * ro - fy2);
        int xa = ifloor(cx - ho), xb = iceil(cx + ho);
        int ia = 1, ib = 0; /* the run of pixels surely inside */
        if (ri > 0 && fy2 < ri * ri) {
            float hi = sqrtf(ri * ri - fy2);
            ia = iceil(cx - hi - 0.5f);
            ib = ifloor(cx + hi - 0.5f);
        }
        if (xa < c->x1) xa = c->x1;
        if (xb > c->x2) xb = c->x2;
        for (int x = xa; x <= xb; x++) {
            if (x >= ia && x <= ib) {
                span(c, y, x, ib, rgb, a);
                x = ib;
                continue;
            }
            float fx = (float)x + 0.5f - cx;
            int k = cov(sqrtf(fx * fx + fy2) - r, soft);
            if (k) put(c, x, y, rgb, a * k / 255);
        }
    }
}

void hpx_ring(hpx_t *c, float cx, float cy, float r, float w, float soft, uint32_t rgb, int a)
{
    float hw = w * 0.5f, e = reach(soft);
    float ro = r + hw + e + 0.71f, rin = r - hw - e - 0.71f;
    int ya = ifloor(cy - ro), yb = iceil(cy + ro);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    for (int y = ya; y <= yb; y++) {
        float fy = (float)y + 0.5f - cy, fy2 = fy * fy;
        if (fy2 >= ro * ro) continue;
        float ho = sqrtf(ro * ro - fy2);
        int xa = ifloor(cx - ho), xb = iceil(cx + ho);
        int sa = 1, sb = 0; /* the hole: nothing to draw */
        if (rin > 0 && fy2 < rin * rin) {
            float hi = sqrtf(rin * rin - fy2);
            sa = iceil(cx - hi - 0.5f);
            sb = ifloor(cx + hi - 0.5f);
        }
        if (xa < c->x1) xa = c->x1;
        if (xb > c->x2) xb = c->x2;
        for (int x = xa; x <= xb; x++) {
            if (x >= sa && x <= sb) {
                x = sb;
                continue;
            }
            float fx = (float)x + 0.5f - cx;
            float d = fabsf(sqrtf(fx * fx + fy2) - r) - hw;
            int k = cov(d, soft);
            if (k) put(c, x, y, rgb, a * k / 255);
        }
    }
}

/* the x interval [lo, hi] where lo <= k·x + m <= hi... solved for x given the coefficient k */
static void lin_range(float k, float m, float lo, float hi, float *xa, float *xb)
{
    if (fabsf(k) < 1e-6f) {
        if (m >= lo && m <= hi) { *xa = -1e9f; *xb = 1e9f; }
        else { *xa = 1e9f; *xb = -1e9f; }
        return;
    }
    float a = (lo - m) / k, b = (hi - m) / k;
    *xa = a < b ? a : b;
    *xb = a < b ? b : a;
}

void hpx_capsule(hpx_t *c, float x0, float y0, float x1, float y1, float r, float soft, uint32_t rgb, int a)
{
    float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);
    if (L < 1e-3f) {
        hpx_disc(c, x0, y0, r, soft, rgb, a);
        return;
    }
    float ux = dx / L, uy = dy / L;
    float R = r + reach(soft) + 0.71f;
    float ri = r - reach(soft) - 0.71f;
    int ya = ifloor((y0 < y1 ? y0 : y1) - R), yb = iceil((y0 > y1 ? y0 : y1) + R);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    for (int y = ya; y <= yb; y++) {
        float py = (float)y + 0.5f;
        /* the row's reach: the band along the segment (|cross| <= R and 0 <= along <= L), and the two end discs */
        float xa = 1e9f, xb = -1e9f;
        float ba, bb, ta, tb;
        /* cross = (px - x0)·uy - (py - y0)·ux ; along = (px - x0)·ux + (py - y0)·uy */
        lin_range(uy, -x0 * uy - (py - y0) * ux, -R, R, &ba, &bb);
        lin_range(ux, -x0 * ux + (py - y0) * uy, 0, L, &ta, &tb);
        if (ba < ta) ba = ta;
        if (bb > tb) bb = tb;
        if (ba <= bb) { xa = ba; xb = bb; }
        for (int e = 0; e < 2; e++) {
            float ex = e ? x1 : x0, ey = e ? y1 : y0, fy = py - ey;
            if (fy * fy < R * R) {
                float h = sqrtf(R * R - fy * fy);
                if (ex - h < xa) xa = ex - h;
                if (ex + h > xb) xb = ex + h;
            }
        }
        if (xa > xb) continue;
        int ia = ifloor(xa), ib = iceil(xb);
        if (ia < c->x1) ia = c->x1;
        if (ib > c->x2) ib = c->x2;
        for (int x = ia; x <= ib; x++) {
            float px = (float)x + 0.5f, qx = px - x0, qy = py - y0;
            float t = qx * ux + qy * uy;
            t = t < 0 ? 0 : t > L ? L : t;
            float ddx = qx - ux * t, ddy = qy - uy * t;
            float d2 = ddx * ddx + ddy * ddy;
            if (ri > 0 && d2 < ri * ri) {
                put(c, x, y, rgb, a);
                continue;
            }
            int k = cov(sqrtf(d2) - r, soft);
            if (k) put(c, x, y, rgb, a * k / 255);
        }
    }
}

void hpx_rrect(hpx_t *c, float x, float y, float w, float h, float r, float soft, uint32_t rgb, int a)
{
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float e = reach(soft) + 0.71f;
    int ya = ifloor(y - e), yb = iceil(y + h + e);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
    for (int yy = ya; yy <= yb; yy++) {
        float py = fabsf((float)yy + 0.5f - cy) - hy;
        int xa = ifloor(x - e), xb = iceil(x + w + e);
        if (xa < c->x1) xa = c->x1;
        if (xb > c->x2) xb = c->x2;
        /* the rows between the corners: the middle is inside, only the two sides are edges */
        for (int xx = xa; xx <= xb; xx++) {
            float px = fabsf((float)xx + 0.5f - cx) - hx;
            float qx = px > 0 ? px : 0, qy = py > 0 ? py : 0;
            float out = sqrtf(qx * qx + qy * qy), in = px > py ? px : py;
            float d = (in < 0 ? in : 0) + out - r;
            if (d < -e) {
                /* inside by more than the edge: so is every pixel up to this one's mirror across the middle (the
                 * distance is convex along a row and symmetric about cx) */
                int end = ifloor(2.0f * cx - (float)xx - 1.0f);
                if (end > xb) end = xb;
                if (end > xx) {
                    span(c, yy, xx, end, rgb, a);
                    xx = end;
                    continue;
                }
            }
            int k = cov(d, soft);
            if (k) put(c, xx, yy, rgb, a * k / 255);
        }
    }
}

void hpx_tri(hpx_t *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, int a)
{
    /* clockwise or not: the edges' normals point out either way */
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(area) < 1e-3f) return;
    float s = area > 0 ? 1.0f : -1.0f;
    float ex[3], ey[3], ec[3];
    const float X[3] = { x0, x1, x2 }, Y[3] = { y0, y1, y2 };
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        float nx = (Y[j] - Y[i]) * s, ny = -(X[j] - X[i]) * s, l = sqrtf(nx * nx + ny * ny);
        ex[i] = nx / l;
        ey[i] = ny / l;
        ec[i] = -(ex[i] * X[i] + ey[i] * Y[i]);
    }
    float mnx = fminf(x0, fminf(x1, x2)), mxx = fmaxf(x0, fmaxf(x1, x2));
    float mny = fminf(y0, fminf(y1, y2)), mxy = fmaxf(y0, fmaxf(y1, y2));
    int xa = ifloor(mnx - 1), xb = iceil(mxx + 1), ya = ifloor(mny - 1), yb = iceil(mxy + 1);
    if (xa < c->x1) xa = c->x1;
    if (xb > c->x2) xb = c->x2;
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    for (int y = ya; y <= yb; y++) {
        float py = (float)y + 0.5f;
        for (int x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f, d = -1e9f;
            for (int i = 0; i < 3; i++) {
                float v = ex[i] * px + ey[i] * py + ec[i];
                if (v > d) d = v;
            }
            if (d > 0.71f) continue;
            int k = cov(d, 1);
            if (k) put(c, x, y, rgb, a * k / 255);
        }
    }
}

void hpx_sector(hpx_t *c, float cx, float cy, float r, float a0, float a1, uint32_t rgb, int a)
{
    /* the disc's pixels whose angle is in [a0, a1], with the two straight edges antialiased by their distance */
    float s0x = sinf(a0), s0y = -cosf(a0), s1x = sinf(a1), s1y = -cosf(a1);
    float span_a = a1 - a0;
    float ro = r + 1.21f;
    int ya = ifloor(cy - ro), yb = iceil(cy + ro), xa0 = ifloor(cx - ro), xb0 = iceil(cx + ro);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    if (xa0 < c->x1) xa0 = c->x1;
    if (xb0 > c->x2) xb0 = c->x2;
    for (int y = ya; y <= yb; y++) {
        float fy = (float)y + 0.5f - cy;
        for (int x = xa0; x <= xb0; x++) {
            float fx = (float)x + 0.5f - cx, d2 = fx * fx + fy * fy;
            if (d2 > ro * ro) continue;
            float dr = sqrtf(d2) - r;
            /* signed distances to the two edge rays (positive: outside the wedge) */
            float e0 = fx * s0y - fy * s0x, e1 = fy * s1x - fx * s1y;
            float de;
            if (span_a <= 3.14159265f) de = e0 > e1 ? e0 : e1;
            else de = e0 < e1 ? e0 : e1;
            float d = dr > de ? dr : de;
            int k = cov(d, 1);
            if (k) put(c, x, y, rgb, a * k / 255);
        }
    }
}

void hpx_ridge(hpx_t *c, const float *top, int x0, uint32_t rgb, int a)
{
    for (int x = c->x1; x <= c->x2; x++) {
        float t = top[x - x0];
        int y = ifloor(t);
        if (y > c->y2) continue;
        if (y >= c->y1) {
            /* the pixel the line crosses, by how much of it is below the line */
            float k = (float)(y + 1) - t;
            put(c, x, y, rgb, (int)(a * k));
        }
        int ys = y + 1 < c->y1 ? c->y1 : y + 1;
        if (c->p32 && a >= 255) {
            for (int yy = ys; yy <= c->y2; yy++) c->p32[(size_t)yy * c->stride + x] = rgb;
        } else {
            for (int yy = ys; yy <= c->y2; yy++) put(c, x, yy, rgb, a);
        }
    }
}

/* ---------------------------------------------------------------- gradients */

void hpx_linear(hpx_t *c, float ox, float oy, float dx, float dy, float len, const uint32_t *stops, const int *pos,
                int n, int a)
{
    /* a 257-entry table along t, then per pixel one multiply-add for t */
    uint32_t lut[257];
    for (int i = 0; i <= 256; i++) {
        int k = 0;
        while (k < n - 1 && i > pos[k + 1]) k++;
        if (k >= n - 1) lut[i] = stops[n - 1];
        else if (i <= pos[k]) lut[i] = stops[k];
        else lut[i] = hpx_mix(stops[k], stops[k + 1], (i - pos[k]) * 256 / (pos[k + 1] - pos[k] ? pos[k + 1] - pos[k] : 1));
    }
    float kx = dx / len * 256.0f, ky = dy / len * 256.0f;
    for (int y = c->y1; y <= c->y2; y++) {
        float t0 = ((float)c->x1 + 0.5f - ox) * kx + ((float)y + 0.5f - oy) * ky;
        for (int x = c->x1; x <= c->x2; x++, t0 += kx) {
            int t = (int)t0;
            t = t < 0 ? 0 : t > 256 ? 256 : t;
            put(c, x, y, lut[t], a);
        }
    }
}

static uint8_t s_glow[257];
static bool s_glow_ready;

void hpx_glow(hpx_t *c, float cx, float cy, float r, uint32_t rgb, int a)
{
    if (!s_glow_ready) {
        /* exp(-4.5 t) over t = d²/r², pulled to zero at the edge so there is no seam */
        float e1 = expf(-4.5f);
        for (int i = 0; i <= 256; i++) s_glow[i] = (uint8_t)(255.0f * (expf(-4.5f * i / 256.0f) - e1) / (1 - e1) + 0.5f);
        s_glow_ready = true;
    }
    int ya = ifloor(cy - r), yb = iceil(cy + r), xa = ifloor(cx - r), xb = iceil(cx + r);
    if (ya < c->y1) ya = c->y1;
    if (yb > c->y2) yb = c->y2;
    if (xa < c->x1) xa = c->x1;
    if (xb > c->x2) xb = c->x2;
    float k = 256.0f / (r * r);
    for (int y = ya; y <= yb; y++) {
        float fy = (float)y + 0.5f - cy, fy2 = fy * fy * k;
        for (int x = xa; x <= xb; x++) {
            float fx = (float)x + 0.5f - cx;
            int t = (int)(fx * fx * k + fy2);
            if (t >= 256) continue;
            put(c, x, y, rgb, a * s_glow[t] / 255);
        }
    }
}

/* ---------------------------------------------------------------- finishing */

uint32_t hpx_hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint32_t hpx_hash_str(const char *s, uint32_t h)
{
    if (!h) h = 2166136261u;
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return hpx_hash(h);
}

void hpx_finish565(const hpx_t *s, uint16_t *dst, int dst_stride, int grain, uint32_t seed)
{
    for (int y = s->y1; y <= s->y2; y++) {
        const uint32_t *p = &s->p32[(size_t)y * s->stride];
        uint16_t *d = &dst[(size_t)y * dst_stride];
        uint32_t hy = hpx_hash(seed ^ (uint32_t)y * 0x9E3779B9u);
        for (int x = s->x1; x <= s->x2; x++) {
            uint32_t v = p[x];
            int r = hpx_r(v), g = hpx_g(v), b = hpx_b(v);
            if (grain) {
                uint32_t hh = hpx_hash(hy + (uint32_t)x * 0x85EBCA6Bu);
                int n = (int)(hh & 255) * grain / 256 - grain / 2;
                r += n;
                g += n;
                b += n;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
            }
            d[x] = dither565(r, g, b, x, y);
        }
    }
}

void hpx_half32(const uint32_t *src, int sw, int sh, int sstride, uint32_t *dst, int dstride)
{
    for (int y = 0; y < sh / 2; y++) {
        const uint32_t *a = &src[(size_t)(2 * y) * sstride], *b = a + sstride;
        uint32_t *d = &dst[(size_t)y * dstride];
        for (int x = 0; x < sw / 2; x++) {
            uint32_t p = a[2 * x], q = a[2 * x + 1], u = b[2 * x], v = b[2 * x + 1];
            int r = (hpx_r(p) + hpx_r(q) + hpx_r(u) + hpx_r(v) + 2) >> 2;
            int g = (hpx_g(p) + hpx_g(q) + hpx_g(u) + hpx_g(v) + 2) >> 2;
            int bl = (hpx_b(p) + hpx_b(q) + hpx_b(u) + hpx_b(v) + 2) >> 2;
            d[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
        }
    }
}

void hpx_half565(const uint32_t *src, int sw, int sh, int sstride, uint16_t *dst, int dstride, int grain, uint32_t seed)
{
    for (int y = 0; y < sh / 2; y++) {
        const uint32_t *a = &src[(size_t)(2 * y) * sstride], *b = a + sstride;
        uint16_t *d = &dst[(size_t)y * dstride];
        uint32_t hy = hpx_hash(seed ^ (uint32_t)y * 0x9E3779B9u);
        for (int x = 0; x < sw / 2; x++) {
            uint32_t p = a[2 * x], q = a[2 * x + 1], u = b[2 * x], v = b[2 * x + 1];
            int r = (hpx_r(p) + hpx_r(q) + hpx_r(u) + hpx_r(v) + 2) >> 2;
            int g = (hpx_g(p) + hpx_g(q) + hpx_g(u) + hpx_g(v) + 2) >> 2;
            int bl = (hpx_b(p) + hpx_b(q) + hpx_b(u) + hpx_b(v) + 2) >> 2;
            if (grain) {
                int n = (int)(hpx_hash(hy + (uint32_t)x * 0x85EBCA6Bu) & 255) * grain / 256 - grain / 2;
                r += n;
                g += n;
                bl += n;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
            }
            d[x] = dither565(r, g, bl, x, y);
        }
    }
}

void hpx_grid(const hpx_t *s, uint32_t *out, int n, int m)
{
    for (int j = 0; j < m; j++)
        for (int i = 0; i < n; i++) {
            int xa = s->w * i / n, xb = s->w * (i + 1) / n, ya = s->h * j / m, yb = s->h * (j + 1) / m;
            uint32_t r = 0, g = 0, b = 0, k = 0;
            /* every other pixel is plenty for an average */
            for (int y = ya; y < yb; y += 2)
                for (int x = xa; x < xb; x += 2) {
                    uint32_t v = s->p32 ? s->p32[(size_t)y * s->stride + x] : hpx_from565(s->p16[(size_t)y * s->stride + x]);
                    r += (uint32_t)hpx_r(v);
                    g += (uint32_t)hpx_g(v);
                    b += (uint32_t)hpx_b(v);
                    k++;
                }
            out[j * n + i] = k ? hpx_rgb((int)(r / k), (int)(g / k), (int)(b / k)) : 0;
        }
}

/* ---------------------------------------------------------------- noise */

static inline float lattice(int x, int y, uint32_t seed)
{
    return (float)(hpx_hash((uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ seed) >> 8) * (1.0f / 16777216.0f);
}

float hpx_noise1(float x, uint32_t seed)
{
    int i = ifloor(x);
    float f = x - (float)i, u = f * f * (3.0f - 2.0f * f);
    float a = lattice(i, 7, seed), b = lattice(i + 1, 7, seed);
    return a + (b - a) * u;
}

float hpx_noise2(float x, float y, uint32_t seed)
{
    int i = ifloor(x), j = ifloor(y);
    float fx = x - (float)i, fy = y - (float)j;
    float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    float a = lattice(i, j, seed), b = lattice(i + 1, j, seed), cc = lattice(i, j + 1, seed), d = lattice(i + 1, j + 1, seed);
    float top = a + (b - a) * ux, bot = cc + (d - cc) * ux;
    return top + (bot - top) * uy;
}

/* ---------------------------------------------------------------- colour */

static float enc(float v)
{
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static float dec(float v)
{
    if (v <= 0.04045f) return v / 12.92f;
    return powf((v + 0.055f) / 1.055f, 2.4f);
}

static bool oklab_rgb(float L, float a, float b, float out[3])
{
    float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
    float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
    float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    const float eps = 0.0005f;
    return out[0] >= -eps && out[0] <= 1 + eps && out[1] >= -eps && out[1] <= 1 + eps && out[2] >= -eps && out[2] <= 1 + eps;
}

uint32_t hpx_oklch(float L, float C, float h)
{
    L = L < 0 ? 0 : L > 1 ? 1 : L;
    float hr = h * 0.017453293f, ca = cosf(hr), sa = sinf(hr);
    float rgb[3];
    if (!oklab_rgb(L, C * ca, C * sa, rgb)) {
        /* out of gamut: the most chroma that fits, by halving */
        float lo = 0, hi = C;
        for (int i = 0; i < 14; i++) {
            float mid = (lo + hi) * 0.5f;
            if (oklab_rgb(L, mid * ca, mid * sa, rgb)) lo = mid;
            else hi = mid;
        }
        oklab_rgb(L, lo * ca, lo * sa, rgb);
    }
    int r = (int)(enc(rgb[0] < 0 ? 0 : rgb[0] > 1 ? 1 : rgb[0]) * 255.0f + 0.5f);
    int g = (int)(enc(rgb[1] < 0 ? 0 : rgb[1] > 1 ? 1 : rgb[1]) * 255.0f + 0.5f);
    int b = (int)(enc(rgb[2] < 0 ? 0 : rgb[2] > 1 ? 1 : rgb[2]) * 255.0f + 0.5f);
    return hpx_rgb(r, g, b);
}

void hpx_to_oklch(uint32_t rgb, float *L, float *C, float *h)
{
    float r = dec(hpx_r(rgb) / 255.0f), g = dec(hpx_g(rgb) / 255.0f), b = dec(hpx_b(rgb) / 255.0f);
    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;
    float l_ = cbrtf(l), m_ = cbrtf(m), s_ = cbrtf(s);
    float LL = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    float aa = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    float bb = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    *L = LL;
    *C = sqrtf(aa * aa + bb * bb);
    float hh = atan2f(bb, aa) * 57.29578f;
    *h = hh < 0 ? hh + 360.0f : hh;
}

static float rel_lum(uint32_t c)
{
    return 0.2126f * dec(hpx_r(c) / 255.0f) + 0.7152f * dec(hpx_g(c) / 255.0f) + 0.0722f * dec(hpx_b(c) / 255.0f);
}

int hpx_contrast(uint32_t a, uint32_t b)
{
    float la = rel_lum(a), lb = rel_lum(b);
    if (la < lb) {
        float t = la;
        la = lb;
        lb = t;
    }
    return (int)((la + 0.05f) / (lb + 0.05f) * 100.0f);
}
