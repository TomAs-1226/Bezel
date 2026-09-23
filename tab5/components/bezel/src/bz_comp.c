#include "bz_comp.h"
#include "bz_tokens.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define BIG_ALLOC(n) malloc(n)
#endif

#define DS 4            /* blur downscale */
#define BLUR_R 3        /* box radius at 1/4 scale; two passes → σ ≈ 11.3 px at full scale */
#define REFRACT_MARGIN 24
#define BACK_DS 8       /* backdrop downscale: σ ≈ 22 px at full scale */

typedef struct {
    bool active;
    int nshapes;
    int idx[BZ_COMP_MAX_SHAPES];     /* shapes in this group, indices into c->shapes */
    bz_area_t bbox;                  /* influence: shapes + shadow + AA */
    /* geometry cache over bbox */
    int lw, lh, lcap;
    uint8_t *cov, *light, *shadow, *glow, *sid;
    int8_t *dx, *dy;
    bool lut_valid;
    /* blur cache */
    bz_area_t barea;                 /* aligned to DS, covers bbox + refraction margin */
    int bw, bh, bcap;
    uint8_t *blur;                   /* RGB888 at 1/DS */
    uint8_t *tmp;
    bool blur_valid;
} group_t;

struct bz_comp {
    int w, h;
    uint16_t *content, *out;
    uint32_t *ink;
    bz_gfx_ops_t ops;

    bz_glass_shape_t shapes[BZ_COMP_MAX_SHAPES], prev[BZ_COMP_MAX_SHAPES];
    int nshapes, nprev;
    group_t groups[BZ_COMP_MAX_GROUPS];

    float lx, ly;
    bool dark, calm;
    float back_amt, back_dim, prev_back_amt;
    uint8_t *back, *back_tmp;
    int back_w, back_h;
    bool back_valid;

    bz_area_t dirty[BZ_COMP_MAX_DIRTY];
    int ndirty;
    bz_comp_stats_t stats;
};

/* ------------------------------------------------------------------ areas */

static bool area_intersect(bz_area_t *r, const bz_area_t *a, const bz_area_t *b)
{
    r->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    r->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    r->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    r->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    return r->x1 <= r->x2 && r->y1 <= r->y2;
}

static bool area_overlap(const bz_area_t *a, const bz_area_t *b)
{
    bz_area_t r;
    return area_intersect(&r, a, b);
}

static void area_join(bz_area_t *a, const bz_area_t *b)
{
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
}

static long area_size(const bz_area_t *a) { return (long)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1); }

static void damage(bz_comp_t *c, bz_area_t a)
{
    bz_area_t screen = { 0, 0, (int16_t)(c->w - 1), (int16_t)(c->h - 1) };
    if (!area_intersect(&a, &a, &screen)) return;
    /* Merge with anything it overlaps, or anything where the union wastes little. */
    for (int i = 0; i < c->ndirty; i++) {
        bz_area_t u = c->dirty[i];
        area_join(&u, &a);
        if (area_overlap(&c->dirty[i], &a) || area_size(&u) <= area_size(&c->dirty[i]) + area_size(&a) + 4096) {
            c->dirty[i] = c->dirty[--c->ndirty];
            damage(c, u);
            return;
        }
    }
    if (c->ndirty == BZ_COMP_MAX_DIRTY) {
        for (int i = 1; i < c->ndirty; i++) area_join(&c->dirty[0], &c->dirty[i]);
        c->ndirty = 1;
        area_join(&c->dirty[0], &a);
        return;
    }
    c->dirty[c->ndirty++] = a;
}

/* ------------------------------------------------------------------ CPU primitives */

static inline void unpack565(uint16_t p, int *r, int *g, int *b)
{
    *r = ((p >> 11) & 31) << 3 | ((p >> 13) & 7);
    *g = ((p >> 5) & 63) << 2 | ((p >> 9) & 3);
    *b = (p & 31) << 3 | ((p >> 2) & 7);
}

/* 4×4 ordered dither for pixels the glass computes at 8-bit precision: the panel is RGB565, whose dark
 * steps are 8 levels apart, and a 10 % shadow on Bezel's ground would otherwise band or vanish. */
static const int8_t BAYER[16] = { -8, 0, -6, 2, 4, -4, 6, -2, -5, 3, -7, 1, 7, -1, 5, -3 };

static inline uint16_t pack565_dither(int r, int g, int b, int x, int y)
{
    int d = BAYER[(y & 3) * 4 + (x & 3)];
    r += d >> 0; g += d >> 1; b += d >> 0;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    /* ×31/255 and ×63/255 rounded, as multiply-shifts */
    r = (r * 249 + 1014) >> 11; g = (g * 253 + 505) >> 10; b = (b * 249 + 1014) >> 11;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static inline uint16_t pack565(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r * 249 + 1014) >> 11) << 11 | ((g * 253 + 505) >> 10) << 5 | ((b * 249 + 1014) >> 11));
}

static void cpu_copy565(uint16_t *dst, int ds, const uint16_t *src, int ss, int w, int h)
{
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * ds, src + (size_t)y * ss, (size_t)w * 2);
}

static void cpu_blend(uint16_t *dst, int ds, const uint16_t *under, int us, const uint32_t *over, int os, int w, int h)
{
    for (int y = 0; y < h; y++) {
        uint16_t *d = dst + (size_t)y * ds;
        const uint16_t *u = under + (size_t)y * us;
        const uint32_t *o = over + (size_t)y * os;
        for (int x = 0; x < w; x++) {
            uint32_t p = o[x];
            uint32_t a = p >> 24;
            if (a == 0) { d[x] = u[x]; continue; }
            if (a == 255) { d[x] = pack565((p >> 16) & 255, (p >> 8) & 255, p & 255); continue; }
            int r, g, b;
            unpack565(u[x], &r, &g, &b);
            r += (int)((((p >> 16) & 255) - r) * a) / 255;
            g += (int)((((p >> 8) & 255) - g) * a) / 255;
            b += (int)(((p & 255) - b) * a) / 255;
            d[x] = pack565(r, g, b);
        }
    }
}

/* ------------------------------------------------------------------ geometry */

static float sd_rrect(float px, float py, const bz_glass_shape_t *s)
{
    float sc = s->scale > 0 ? s->scale : 1;
    float hw = s->w * sc * 0.5f, hh = s->h * sc * 0.5f;
    float cx = s->x + s->w * 0.5f, cy = s->y + s->h * 0.5f;
    float r = s->radius * sc;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = fabsf(px - cx) - hw + r, qy = fabsf(py - cy) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

static float smin(float a, float b, float k)
{
    float h = k - fabsf(a - b);
    h = h > 0 ? h / k : 0;
    return (a < b ? a : b) - h * h * k * 0.25f;
}

/* The group's field: the smooth union of its shapes (they melt within the merge distance). Also
 * reports the nearest shape, which supplies strength, press and tint. */
static float group_sd(const bz_comp_t *c, const group_t *g, float x, float y, int *nearest)
{
    float d = 1e9f, best = 1e9f;
    for (int i = 0; i < g->nshapes; i++) {
        const bz_glass_shape_t *s = &c->shapes[g->idx[i]];
        float di = sd_rrect(x, y, s);
        if (di < best) {
            best = di;
            if (nearest) *nearest = g->idx[i];
        }
        d = i == 0 ? di : smin(d, di, BZ_GLASS_MERGE);
    }
    return d;
}

static bool grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    void *n = BIG_ALLOC((size_t)need * elem);
    if (!n) return false;
    free(*p);
    *p = n;
    *cap = need;
    return true;
}

static float big_of(const bz_glass_shape_t *s)
{
    float m = s->w < s->h ? s->w : s->h;
    float b = m / 127.0f;
    return b < 0.5f ? 0.5f : b > 1.4f ? 1.4f : b;
}

static void build_lut(bz_comp_t *c, group_t *g)
{
    int w = g->bbox.x2 - g->bbox.x1 + 1, h = g->bbox.y2 - g->bbox.y1 + 1;
    int n = w * h;
    int cap = g->lcap;
    bool ok = true;
    if (n > g->lcap) {
        int dummy;
        free(g->cov); free(g->light); free(g->shadow); free(g->glow); free(g->sid); free(g->dx); free(g->dy);
        g->cov = g->light = g->shadow = g->glow = g->sid = NULL;
        g->dx = g->dy = NULL;
        g->cov = BIG_ALLOC((size_t)n); g->light = BIG_ALLOC((size_t)n); g->shadow = BIG_ALLOC((size_t)n);
        g->glow = BIG_ALLOC((size_t)n); g->sid = BIG_ALLOC((size_t)n);
        g->dx = BIG_ALLOC((size_t)n); g->dy = BIG_ALLOC((size_t)n);
        ok = g->cov && g->light && g->shadow && g->glow && g->sid && g->dx && g->dy;
        cap = ok ? n : 0;
        (void)dummy;
    }
    g->lcap = cap;
    if (!ok) { g->lut_valid = false; return; }
    g->lw = w;
    g->lh = h;

    float llen = sqrtf(c->lx * c->lx + c->ly * c->ly);
    float Lx = llen > 0 ? c->lx / llen : -0.42f, Ly = llen > 0 ? c->ly / llen : -0.91f;
    const float eta = 1.0f / 1.5f;
    const bz_palette_t *P = bz_pal;

    for (int yy = 0; yy < h; yy++) {
        float py = g->bbox.y1 + yy + 0.5f;
        for (int xx = 0; xx < w; xx++) {
            float px = g->bbox.x1 + xx + 0.5f;
            int i = yy * w + xx;
            int near = g->idx[0];
            float d = group_sd(c, g, px, py, &near);
            const bz_glass_shape_t *s = &c->shapes[near];
            float st = s->strength;
            g->sid[i] = (uint8_t)near;
            float cov = (0.75f - d) / 1.5f;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            g->cov[i] = (uint8_t)(cov * 255 + 0.5f);
            g->dx[i] = g->dy[i] = 0;
            g->light[i] = g->glow[i] = g->shadow[i] = 0;

            if (cov < 1) {
                float big = big_of(s);
                float ds = group_sd(c, g, px, py - 7 * big, NULL);
                float t = (ds + 5.6f) / (34 * big + 5.6f);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float sm = t * t * (3 - 2 * t);
                float dark = (1 - sm) * st * 0.14f * big * P->shadow_gain;
                g->shadow[i] = (uint8_t)(dark * 255 > 255 ? 255 : dark * 255);
            }
            if (cov <= 0) continue;

            /* outward normal from the field's gradient */
            float e = 0.75f;
            float gx = group_sd(c, g, px + e, py, NULL) - group_sd(c, g, px - e, py, NULL);
            float gy = group_sd(c, g, px, py + e, NULL) - group_sd(c, g, px, py - e, NULL);
            float gl = sqrtf(gx * gx + gy * gy);
            if (gl > 1e-6f) { gx /= gl; gy /= gl; } else { gx = gy = 0; }

            float depth = d < 0 ? -d : 0;
            float sc = s->scale > 0 ? s->scale : 1;
            float bw = 0.9f * s->radius * sc;
            bw = bw < 11 ? 11 : bw > 40 ? 40 : bw;
            float x = depth / bw;
            x = x > 1 ? 1 : x;
            float om = 1 - x, om4 = om * om * om * om;
            float prof = powf(1 - om4, 0.25f);
            float slope = x >= 1 ? 0 : om * om * om * powf(1 - om4 > 1e-6f ? 1 - om4 : 1e-6f, -0.75f);
            slope = slope > 12 ? 12 : slope;

            if (!c->calm) {
                float nx = gx * slope, ny = gy * slope, nz = 1;
                float nl = sqrtf(nx * nx + ny * ny + 1);
                nx /= nl; ny /= nl; nz /= nl;
                float cosi = nz;
                float k = 1 - eta * eta * (1 - cosi * cosi);
                float coef = eta * cosi - sqrtf(k > 0 ? k : 0);
                float tx = coef * nx, ty = coef * ny, tz = -eta + coef * nz;
                float thick = BZ_GLASS_THICKNESS * (0.3f + 0.7f * prof) * (1 + 0.4f * s->press) * st;
                float den = -tz > 0.3f ? -tz : 0.3f;
                float ox = tx / den * thick, oy = ty / den * thick;
                ox = ox < -REFRACT_MARGIN ? -REFRACT_MARGIN : ox > REFRACT_MARGIN ? REFRACT_MARGIN : ox;
                oy = oy < -REFRACT_MARGIN ? -REFRACT_MARGIN : oy > REFRACT_MARGIN ? REFRACT_MARGIN : oy;
                g->dx[i] = (int8_t)lrintf(ox);
                g->dy[i] = (int8_t)lrintf(oy);
            }

            /* one directional rim, about a pixel, brightest where it faces the light */
            float f = gx * Lx + gy * Ly;
            float fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
            float rt = (depth - 0.35f) / (1.5f - 0.35f);
            rt = rt < 0 ? 0 : rt > 1 ? 1 : rt;
            float rim = 1 - rt * rt * (3 - 2 * rt);
            float bt = depth / (0.6f * bw);
            bt = bt > 1 ? 1 : bt;
            float band = 1 - bt;
            float lit = rim * (0.16f + 0.62f * powf(fp, 1.5f) + 0.26f * fn * fn) + band * (0.07f * fp + 0.04f * fn);
            lit *= st * P->rim_gain;
            g->light[i] = (uint8_t)(lit * 255 > 255 ? 255 : lit * 255);

            if (s->press > 0) {
                float ddx = px - s->press_x, ddy = py - s->press_y;
                float gw = 0.22f * s->press * expf(-sqrtf(ddx * ddx + ddy * ddy) / BZ_GLASS_GLOW_RADIUS);
                g->glow[i] = (uint8_t)(gw * 255 > 255 ? 255 : gw * 255);
            }
        }
    }
    g->lut_valid = true;
    c->stats.lut_rebuilds++;
}

/* Box blur, one pass, one axis, in place through tmp; edges clamp. */
static void box_pass(uint8_t *img, uint8_t *tmp, int w, int h, bool horizontal)
{
    int len = horizontal ? w : h, lines = horizontal ? h : w;
    int step = horizontal ? 3 : w * 3, lstep = horizontal ? w * 3 : 3;
    int win = 2 * BLUR_R + 1;
    for (int l = 0; l < lines; l++) {
        uint8_t *base = img + l * lstep;
        for (int ch = 0; ch < 3; ch++) {
            int sum = 0;
            for (int k = -BLUR_R; k <= BLUR_R; k++) {
                int j = k < 0 ? 0 : k >= len ? len - 1 : k;
                sum += base[j * step + ch];
            }
            for (int i = 0; i < len; i++) {
                tmp[i * 3 + ch] = (uint8_t)(sum / win);
                int out = i - BLUR_R, in = i + BLUR_R + 1;
                out = out < 0 ? 0 : out;
                in = in >= len ? len - 1 : in;
                sum += base[in * step + ch] - base[out * step + ch];
            }
        }
        for (int i = 0; i < len; i++) {
            base[i * step] = tmp[i * 3];
            base[i * step + 1] = tmp[i * 3 + 1];
            base[i * step + 2] = tmp[i * 3 + 2];
        }
    }
}

/* Averages `ds`×`ds` blocks of the content in `a` (aligned to ds) into an RGB888 image. */
static void downsample(const bz_comp_t *c, const bz_area_t *a, int ds, uint8_t *dst, int bw, int bh)
{
    int n = ds * ds;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int r = 0, g = 0, b = 0;
            for (int yy = 0; yy < ds; yy++) {
                int y = a->y1 + by * ds + yy;
                y = y < 0 ? 0 : y >= c->h ? c->h - 1 : y;
                const uint16_t *row = c->content + (size_t)y * c->w;
                for (int xx = 0; xx < ds; xx++) {
                    int x = a->x1 + bx * ds + xx;
                    x = x < 0 ? 0 : x >= c->w ? c->w - 1 : x;
                    int rr, gg, bb;
                    unpack565(row[x], &rr, &gg, &bb);
                    r += rr; g += gg; b += bb;
                }
            }
            uint8_t *p = dst + (by * bw + bx) * 3;
            p[0] = (uint8_t)(r / n);
            p[1] = (uint8_t)(g / n);
            p[2] = (uint8_t)(b / n);
        }
    }
}

static void build_blur(bz_comp_t *c, group_t *g)
{
    bz_area_t a = g->bbox;
    a.x1 = (int16_t)(((a.x1 - REFRACT_MARGIN - 16) / DS) * DS);
    a.y1 = (int16_t)(((a.y1 - REFRACT_MARGIN - 16) / DS) * DS);
    a.x2 = (int16_t)(a.x2 + REFRACT_MARGIN + 16);
    a.y2 = (int16_t)(a.y2 + REFRACT_MARGIN + 16);
    int bw = (a.x2 - a.x1 + DS) / DS, bh = (a.y2 - a.y1 + DS) / DS;
    a.x2 = (int16_t)(a.x1 + bw * DS - 1);
    a.y2 = (int16_t)(a.y1 + bh * DS - 1);
    if (!grow((void **)&g->blur, &g->bcap, bw * bh * 3, 1)) return;
    int tcap = 0;
    free(g->tmp);
    g->tmp = NULL;
    if (!grow((void **)&g->tmp, &tcap, (bw > bh ? bw : bh) * 3, 1)) return;
    g->barea = a;
    g->bw = bw;
    g->bh = bh;
    downsample(c, &a, DS, g->blur, bw, bh);
    for (int pass = 0; pass < 2; pass++) {
        box_pass(g->blur, g->tmp, bw, bh, true);
        box_pass(g->blur, g->tmp, bw, bh, false);
    }
    g->blur_valid = true;
    c->stats.blur_rebuilds++;
}

static inline void sample_bilinear(const uint8_t *img, int w, int h, int fx, int fy, int *r, int *g, int *b)
{
    /* fx, fy in 8.8 fixed point, texel centres at .5 */
    fx -= 128;
    fy -= 128;
    int x0 = fx >> 8, y0 = fy >> 8;
    int ax = fx & 255, ay = fy & 255;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0;
    x1 = x1 < 0 ? 0 : x1 >= w ? w - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0;
    y1 = y1 < 0 ? 0 : y1 >= h ? h - 1 : y1;
    const uint8_t *p00 = img + (y0 * w + x0) * 3, *p10 = img + (y0 * w + x1) * 3;
    const uint8_t *p01 = img + (y1 * w + x0) * 3, *p11 = img + (y1 * w + x1) * 3;
#define BL(ch) ((((p00[ch] * (256 - ax) + p10[ch] * ax) * (256 - ay)) + ((p01[ch] * (256 - ax) + p11[ch] * ax) * ay)) >> 16)
    *r = BL(0);
    *g = BL(1);
    *b = BL(2);
#undef BL
}

/* ------------------------------------------------------------------ groups */

static uint32_t tint_color(int t)
{
    const bz_palette_t *P = bz_pal;
    switch (t) {
    case 0: return P->signal;
    case 1: return P->amber;
    case 2: return P->leaf;
    case 3: return P->ice;
    case 4: return P->fault;
    default: return 0;
    }
}

static bz_area_t shape_bbox(const bz_glass_shape_t *s)
{
    float sc = s->scale > 0 ? s->scale : 1;
    float cx = s->x + s->w * 0.5f, cy = s->y + s->h * 0.5f;
    float hw = s->w * sc * 0.5f, hh = s->h * sc * 0.5f;
    float big = big_of(s);
    float m = 34 * big + 2, mb = 34 * big + 7 * big + 2; /* the shadow falls 7·big below */
    bz_area_t a = { (int16_t)floorf(cx - hw - m), (int16_t)floorf(cy - hh - m + 7 * big),
                    (int16_t)ceilf(cx + hw + m), (int16_t)ceilf(cy + hh + mb) };
    return a;
}

static bool shape_eq(const bz_glass_shape_t *a, const bz_glass_shape_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void regroup(bz_comp_t *c)
{
    bz_area_t screen = { 0, 0, (int16_t)(c->w - 1), (int16_t)(c->h - 1) };
    for (int gi = 0; gi < BZ_COMP_MAX_GROUPS; gi++) {
        group_t *g = &c->groups[gi];
        bool was = g->active;
        bz_area_t old = g->bbox;
        int oldn = g->nshapes;
        int oldidx[BZ_COMP_MAX_SHAPES];
        memcpy(oldidx, g->idx, sizeof oldidx);

        g->nshapes = 0;
        for (int i = 0; i < c->nshapes; i++)
            if (c->shapes[i].group == gi && c->shapes[i].strength > 0.001f) g->idx[g->nshapes++] = i;
        g->active = g->nshapes > 0;

        /* Did anything about this group change? Compare with last frame's shapes of the group. */
        bool changed = was != g->active || oldn != g->nshapes;
        for (int k = 0; k < g->nshapes && !changed; k++) {
            int i = g->idx[k], j = oldidx[k];
            changed = j >= c->nprev || !shape_eq(&c->shapes[i], &c->prev[j]);
        }
        if (!changed) continue;

        if (was) damage(c, old);
        if (!g->active) continue;
        bz_area_t bb = shape_bbox(&c->shapes[g->idx[0]]);
        for (int k = 1; k < g->nshapes; k++) {
            bz_area_t b2 = shape_bbox(&c->shapes[g->idx[k]]);
            area_join(&bb, &b2);
        }
        area_intersect(&bb, &bb, &screen);
        bool moved = !was || memcmp(&bb, &old, sizeof bb) != 0;
        g->bbox = bb;
        g->lut_valid = false;
        if (moved) g->blur_valid = false;
        damage(c, bb);
    }
    memcpy(c->prev, c->shapes, sizeof c->shapes);
    c->nprev = c->nshapes;
}

/* ------------------------------------------------------------------ public */

bz_comp_t *bz_comp_create(int w, int h, uint16_t *content, uint32_t *ink, uint16_t *out)
{
    bz_comp_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->w = w;
    c->h = h;
    c->content = content;
    c->ink = ink;
    c->out = out;
    c->ops.copy565 = cpu_copy565;
    c->ops.blend = cpu_blend;
    c->lx = -0.42f;
    c->ly = -0.91f;
    c->dark = true;
    c->back_w = (w + BACK_DS - 1) / BACK_DS;
    c->back_h = (h + BACK_DS - 1) / BACK_DS;
    bz_comp_damage_all(c);
    return c;
}

void bz_comp_set_ops(bz_comp_t *c, const bz_gfx_ops_t *ops)
{
    if (ops->copy565) c->ops.copy565 = ops->copy565;
    if (ops->blend) c->ops.blend = ops->blend;
}

void bz_comp_set_shapes(bz_comp_t *c, const bz_glass_shape_t *shapes, int n)
{
    if (n > BZ_COMP_MAX_SHAPES) n = BZ_COMP_MAX_SHAPES;
    memcpy(c->shapes, shapes, sizeof(bz_glass_shape_t) * (size_t)n);
    c->nshapes = n;
    regroup(c);
}

void bz_comp_set_light(bz_comp_t *c, float lx, float ly)
{
    if (fabsf(lx - c->lx) < 0.002f && fabsf(ly - c->ly) < 0.002f) return;
    c->lx = lx;
    c->ly = ly;
    for (int i = 0; i < BZ_COMP_MAX_GROUPS; i++) {
        if (!c->groups[i].active) continue;
        c->groups[i].lut_valid = false;
        damage(c, c->groups[i].bbox);
    }
}

void bz_comp_set_mode(bz_comp_t *c, bool dark, bool calm)
{
    if (dark == c->dark && calm == c->calm) return;
    c->dark = dark;
    c->calm = calm;
    for (int i = 0; i < BZ_COMP_MAX_GROUPS; i++) c->groups[i].lut_valid = false;
    bz_comp_damage_all(c);
}

void bz_comp_set_backdrop(bz_comp_t *c, float amount, float dim)
{
    if (fabsf(amount - c->back_amt) < 0.002f && fabsf(dim - c->back_dim) < 0.002f) return;
    c->back_amt = amount;
    c->back_dim = dim;
    bz_comp_damage_all(c);
}

void bz_comp_damage_content(bz_comp_t *c, const bz_area_t *a)
{
    damage(c, *a);
    for (int i = 0; i < BZ_COMP_MAX_GROUPS; i++) {
        group_t *g = &c->groups[i];
        if (!g->active) continue;
        if (!g->blur_valid || area_overlap(a, &g->barea)) {
            /* The glass samples its neighbourhood: new content under or near it redraws all of it. */
            g->blur_valid = false;
            damage(c, g->bbox);
        }
    }
    if (c->back_amt > 0.002f) {
        c->back_valid = false;
        bz_comp_damage_all(c);
    }
}

void bz_comp_damage_ink(bz_comp_t *c, const bz_area_t *a) { damage(c, *a); }

void bz_comp_damage_all(bz_comp_t *c)
{
    c->ndirty = 1;
    c->dirty[0] = (bz_area_t){ 0, 0, (int16_t)(c->w - 1), (int16_t)(c->h - 1) };
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void build_backdrop(bz_comp_t *c)
{
    int n = c->back_w * c->back_h * 3;
    if (!c->back) c->back = BIG_ALLOC((size_t)n);
    if (!c->back_tmp) c->back_tmp = BIG_ALLOC((size_t)(c->back_w > c->back_h ? c->back_w : c->back_h) * 3);
    if (!c->back || !c->back_tmp) return;
    bz_area_t a = { 0, 0, (int16_t)(c->back_w * BACK_DS - 1), (int16_t)(c->back_h * BACK_DS - 1) };
    downsample(c, &a, BACK_DS, c->back, c->back_w, c->back_h);
    for (int pass = 0; pass < 2; pass++) {
        box_pass(c->back, c->back_tmp, c->back_w, c->back_h, true);
        box_pass(c->back, c->back_tmp, c->back_w, c->back_h, false);
    }
    c->back_valid = true;
}

/* The page behind the control center: frosted by `amount`, dimmed and desaturated by `dim`
 * (cc.js:154; shaders.js:139-142). Without the ink layer. */
static inline void backdrop_px(const bz_comp_t *c, int x, int y, int *r, int *g, int *b)
{
    int cr, cg, cb, br, bg, bb;
    unpack565(c->content[(size_t)y * c->w + x], &cr, &cg, &cb);
    sample_bilinear(c->back, c->back_w, c->back_h, (x * 256 + 128) / BACK_DS, (y * 256 + 128) / BACK_DS, &br, &bg, &bb);
    int A = (int)(c->back_amt * 256);
    float r0 = cr + (((br - cr) * A) >> 8), g0 = cg + (((bg - cg) * A) >> 8), b0 = cb + (((bb - cb) * A) >> 8);
    float darken = 1 - c->back_dim * (c->dark ? 0.52f : 0.30f);
    float desat = c->back_dim * 0.35f;
    float l = 0.2126f * r0 + 0.7152f * g0 + 0.0722f * b0;
    *r = (int)((r0 + (l - r0) * desat) * darken);
    *g = (int)((g0 + (l - g0) * desat) * darken);
    *b = (int)((b0 + (l - b0) * desat) * darken);
}

static void compose_backdrop(bz_comp_t *c, const bz_area_t *r)
{
    for (int y = r->y1; y <= r->y2; y++) {
        uint16_t *dst = c->out + (size_t)y * c->w;
        for (int x = r->x1; x <= r->x2; x++) {
            int rr, gg, bl;
            backdrop_px(c, x, y, &rr, &gg, &bl);
            dst[x] = pack565_dither(rr, gg, bl, x, y);
        }
    }
}

static void compose_group(bz_comp_t *c, group_t *g, const bz_area_t *r)
{
    bz_area_t a;
    if (!area_intersect(&a, r, &g->bbox)) return;
    const bz_palette_t *P = bz_pal;
    int er = bz_r(P->glass_even), eg = bz_g(P->glass_even), eb = bz_b(P->glass_even);
    int rimr = bz_r(P->rim), rimg = bz_g(P->rim), rimb = bz_b(P->rim);
    int s2r = bz_r(P->surface2), s2g = bz_g(P->surface2), s2b = bz_b(P->surface2);
    for (int y = a.y1; y <= a.y2; y++) {
        int ly = y - g->bbox.y1;
        const uint16_t *src = c->content + (size_t)y * c->w;
        uint16_t *dst = c->out + (size_t)y * c->w;
        for (int x = a.x1; x <= a.x2; x++) {
            int i = ly * g->lw + (x - g->bbox.x1);
            int cov = g->cov[i], sh = g->shadow[i];
            if (!cov && !sh) continue; /* no glass or shadow here */
            /* the composite so far: the page (or its backdrop) and any glass group drawn before this one */
            int br, bgc, bb;
            unpack565(dst[x], &br, &bgc, &bb);
            if (sh && cov < 255) {
                int k = 255 - (sh * (255 - cov)) / 255;
                br = br * k / 255; bgc = bgc * k / 255; bb = bb * k / 255;
            }
            int rr = br, gg = bgc, bl = bb;
            if (cov) {
                const bz_glass_shape_t *s = &c->shapes[g->sid[i]];
                float st = s->strength;
                float fr, fg, fb;
                if (c->calm) {
                    fr = s2r; fg = s2g; fb = s2b;
                } else {
                    int sx = x + g->dx[i], sy = y + g->dy[i];
                    int qr, qg, qb;
                    sample_bilinear(g->blur, g->bw, g->bh, ((sx - g->barea.x1) * 256 + 128) / DS,
                                    ((sy - g->barea.y1) * 256 + 128) / DS, &qr, &qg, &qb);
                    /* frost scales with strength: sharp content → frosted as the glass materializes */
                    int cr, cg, cb;
                    unpack565(src[x], &cr, &cg, &cb);
                    fr = cr + (qr - cr) * st; fg = cg + (qg - cg) * st; fb = cb + (qb - cb) * st;
                    float ev = P->glass_even_amt * st;
                    fr += (er - fr) * ev; fg += (eg - fg) * ev; fb += (eb - fb) * ev;
                    float l = 0.2126f * fr + 0.7152f * fg + 0.0722f * fb;
                    float sat = 1 + (BZ_GLASS_SATURATION - 1) * st;
                    fr = l + (fr - l) * sat; fg = l + (fg - l) * sat; fb = l + (fb - l) * sat;
                    if (s->tint >= 0 && s->tint_amt > 0) {
                        uint32_t tc = tint_color(s->tint);
                        float k = 0.82f + 0.35f * (l / 255.0f);
                        float amt = 0.82f * s->tint_amt * st;
                        fr += (bz_r(tc) * k + 10 - fr) * amt;
                        fg += (bz_g(tc) * k + 10 - fg) * amt;
                        fb += (bz_b(tc) * k + 10 - fb) * amt;
                    }
                }
                int lt = g->light[i] + g->glow[i];
                fr += rimr * lt / 255.0f; fg += rimg * lt / 255.0f; fb += rimb * lt / 255.0f;
                rr = br + (int)((fr - br) * cov / 255);
                gg = bgc + (int)((fg - bgc) * cov / 255);
                bl = bb + (int)((fb - bb) * cov / 255);
            }
            dst[x] = pack565_dither(rr, gg, bl, x, y);
        }
    }
    c->stats.glass_px += (uint32_t)area_size(&a);
}

int bz_comp_compose(bz_comp_t *c, bz_area_t *areas, int max)
{
    if (!c->ndirty) return 0;
    int64_t t0 = now_us();
    c->stats.composed_px = c->stats.glass_px = 0;
    bool back = c->back_amt > 0.002f;
    if (back && !c->back_valid) build_backdrop(c);

    for (int gi = 0; gi < BZ_COMP_MAX_GROUPS; gi++) {
        group_t *g = &c->groups[gi];
        if (!g->active) continue;
        bool touched = false;
        for (int i = 0; i < c->ndirty && !touched; i++) touched = area_overlap(&c->dirty[i], &g->bbox);
        if (!touched) continue;
        if (!g->lut_valid) build_lut(c, g);
        if (!g->blur_valid && !c->calm) build_blur(c, g);
    }

    int n = 0;
    for (int i = 0; i < c->ndirty; i++) {
        bz_area_t *r = &c->dirty[i];
        int w = r->x2 - r->x1 + 1, h = r->y2 - r->y1 + 1;
        size_t off = (size_t)r->y1 * c->w + r->x1;
        /* 1: the page under everything (frosted behind the control center when it is open) */
        if (back) compose_backdrop(c, r);
        else c->ops.copy565(c->out + off, c->w, c->content + off, c->w, w, h);
        /* 2: each glass group over the running composite; glass samples only the page, never glass */
        for (int gi = 0; gi < BZ_COMP_MAX_GROUPS; gi++) {
            group_t *g = &c->groups[gi];
            if (g->active && g->lut_valid && (c->calm || g->blur_valid)) compose_group(c, g, r);
        }
        /* 3: what sits on the glass — its labels and icons — last, crisp */
        c->ops.blend(c->out + off, c->w, c->out + off, c->w, c->ink + off, c->w, w, h);
        c->stats.composed_px += (uint32_t)(w * h);
        if (n < max) areas[n++] = *r;
    }
    if (c->ndirty > max && max > 0) {
        /* more areas than the caller can present: hand back their union */
        for (int i = max; i < c->ndirty; i++) area_join(&areas[max - 1], &c->dirty[i]);
    }
    c->ndirty = 0;
    c->stats.compose_us = (uint32_t)(now_us() - t0);
    return n;
}

void bz_comp_stats(bz_comp_t *c, bz_comp_stats_t *out) { *out = c->stats; }
