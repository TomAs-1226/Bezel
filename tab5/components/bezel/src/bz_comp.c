#include "bz_comp.h"
#include "bz_tokens.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef ESP_PLATFORM
#include <stdio.h>
#endif

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
#define TILE 32         /* ink occupancy tiles */
#define MAX_INK_RECTS 48
#define MAX_EDGES 96
#define PROF_N 256      /* bezel-profile tables over depth / band width */
#define MAX_CLUSTERS BZ_COMP_MAX_SHAPES
#define MAX_IDS 64
#define BAND 160        /* the backdrop's soft edge, px */

/* A cluster: the shapes of one glass group that are near enough to melt together. Shapes of a group
 * that are apart (the control center's modules) each get their own, so each keeps its own geometry
 * while they move independently. A cluster is known by its shapes' ids; an idle one keeps its caches
 * and is revived when the same shapes come back together (the dock, after the droplet leaves). */
typedef struct {
    bool active, seen;
    uint8_t pub;                     /* the public glass group */
    int nshapes;
    uint8_t ids[BZ_COMP_MAX_SHAPES]; /* the cluster's shapes by id, ascending: its key */
    int idx[BZ_COMP_MAX_SHAPES];     /* the same shapes' indices into c->shapes this frame */
    bz_area_t bbox;                  /* influence: shapes + shadow + AA; not clipped to the screen */
    /* geometry over bbox at full strength, at rest and unlit: strength, press, tint and the light's
     * direction are applied per frame. sid is the nearest shape's index in this cluster. */
    int lw, lh, lcap;
    uint8_t *cov, *shadow, *sid;
    uint8_t *ang, *rim, *band;       /* the edge's normal (256 steps), its rim and its inner band */
    int8_t *dx, *dy;
    int16_t *span0, *span1;          /* per row: the flat interior (full cover, no rim, no bend, no shadow) */
    int spcap;
    bool lut_valid;
    bz_glass_shape_t built[BZ_COMP_MAX_SHAPES];
    int nbuilt;
    /* blur cache, and the same image toned as frost (fast path) */
    bz_area_t barea;                 /* aligned to DS, covers bbox + refraction margin */
    int bw, bh, bcap, tcap;
    uint8_t *blur, *tone;            /* RGB888 at 1/DS */
    uint8_t *tmp[2];
    int tmpcap;
    bool blur_valid, tone_valid;
    bool blur_grow;                  /* translating: keep a blur that still covers, grow it when not */
    bool fast;                       /* every shape fully materialized and untinted: frost pre-toned */
} group_t;

struct bz_comp {
    int w, h;
    uint16_t *content, *out, *outs[2];
    uint32_t *ink;
    bz_gfx_ops_t ops;
    unsigned frame;

    bz_glass_shape_t shapes[BZ_COMP_MAX_SHAPES];
    int nshapes;
    group_t groups[MAX_CLUSTERS];
    bz_glass_shape_t prev_by_id[MAX_IDS];
    bool prev_have[MAX_IDS];

    bz_layer_t layers[BZ_COMP_MAX_LAYERS];
    bool layer_on[BZ_COMP_MAX_LAYERS];

    float lx, ly;
    bool dark, calm;
    /* backdrop: the page frosted and dimmed once (F), cross-faded by amount */
    float back_amt, back_dim, back_built_dim;
    uint8_t *back, *back_tmp[2];
    uint16_t *back_full;
    int back_w, back_h;
    bool back_valid, back_stale;
    int64_t back_built_us;
    float prev_back_amt;
    int back_lo, back_hi;   /* rows above back_lo are frosted; back_lo..back_hi fade; below, the page */

    /* ink occupancy: a tile holds something opaque on the glass layer */
    uint8_t *tiles;
    int tw, th;
    bz_area_t ink_rects[MAX_INK_RECTS];
    int nink;
    bool ink_rects_valid;

    bz_area_t dirty[BZ_COMP_MAX_DIRTY];
    int ndirty;
    bz_comp_stats_t stats;
    uint32_t part_lut_ring[2];
    uint32_t part_flat[2], part_edge[2], part_shadow[2];
    struct glass_job *job;           /* per compose_group call, kept off the stack and out of static RAM */
    struct cell *cells;              /* a damaged area's cells */
};

/* Tables, on the heap: the P4's static internal RAM is spoken for (the USB host's ISR code lives there). */
static float *PROF, *SLOPE;
#define GLOW_N 1024
static uint8_t *GLOW;             /* exp(-d / R) over d² in [0, (4R)²), ×255 */

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

static bz_area_t screen_of(const bz_comp_t *c) { return (bz_area_t){ 0, 0, (int16_t)(c->w - 1), (int16_t)(c->h - 1) }; }

static void damage(bz_comp_t *c, bz_area_t a)
{
    bz_area_t screen = screen_of(c);
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

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ both cores */

typedef void (*rows_fn)(bz_comp_t *c, void *arg, int y0, int y1, int part);

typedef struct {
    bz_comp_t *c;
    rows_fn fn;
    void *arg;
    int y0, mid, y1;
} rows_job_t;

static void rows_job(void *p, int part)
{
    rows_job_t *j = p;
    if (part == 0) j->fn(j->c, j->arg, j->y0, j->mid, 0);
    else j->fn(j->c, j->arg, j->mid, j->y1, 1);
}

/* Runs fn over [y0, y1) split in two, one half per core when the platform provides a second one. */
static void run_rows(bz_comp_t *c, rows_fn fn, void *arg, int y0, int y1)
{
    if (y1 <= y0) return;
    if (!c->ops.parallel || y1 - y0 < 16) {
        fn(c, arg, y0, y1, 0);
        return;
    }
    rows_job_t j = { c, fn, arg, y0, (y0 + y1) / 2, y1 };
    c->ops.parallel(rows_job, &j);
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

/* 8-bit channel → its RGB565 field, clamped, over -256..767 (a rim can light past white): three table
 * reads instead of three clamps and three multiplies per pixel. */
#define QOFF 256
#define QN 1024
static uint16_t *Q_R, *Q_G, *Q_B;

static void quant_init(void)
{
    if (Q_R) return;
    Q_R = malloc(QN * 2 * 3);
    Q_G = Q_R + QN;
    Q_B = Q_G + QN;
    for (int i = 0; i < QN; i++) {
        int v = i - QOFF;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        /* ×31/255 and ×63/255 rounded, as multiply-shifts */
        Q_R[i] = (uint16_t)(((v * 249 + 1014) >> 11) << 11);
        Q_G[i] = (uint16_t)(((v * 253 + 505) >> 10) << 5);
        Q_B[i] = (uint16_t)((v * 249 + 1014) >> 11);
    }
}

static inline int qi(int v) { v += QOFF; return v < 0 ? 0 : v >= QN ? QN - 1 : v; }

static inline uint16_t pack565_dither(int r, int g, int b, int x, int y)
{
    int d = BAYER[(y & 3) * 4 + (x & 3)];
    return (uint16_t)(Q_R[qi(r + d)] | Q_G[qi(g + (d >> 1))] | Q_B[qi(b + d)]);
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

static void cpu_mix565(uint16_t *dst, int ds, const uint16_t *a, int as, const uint16_t *b, int bs, int w, int h,
                       uint8_t alpha)
{
    int k = alpha;
    for (int y = 0; y < h; y++) {
        uint16_t *d = dst + (size_t)y * ds;
        const uint16_t *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x++) {
            int ar, ag, ab, br, bg, bb;
            unpack565(pa[x], &ar, &ag, &ab);
            unpack565(pb[x], &br, &bg, &bb);
            d[x] = pack565(ar + ((br - ar) * k) / 255, ag + ((bg - ag) * k) / 255, ab + ((bb - ab) * k) / 255);
        }
    }
}

/* ------------------------------------------------------------------ the base: content under the layers */

/* A layer's coverage of pixel (x, y), 0..256: 256 inside, a ramp over the antialiased corners. */
static inline int layer_cov(const bz_layer_t *l, int x, int y)
{
    if (x < l->rect.x1 || x > l->rect.x2 || y < l->rect.y1 || y > l->rect.y2) return 0;
    float r = l->radius;
    if (r <= 0.5f) return 256;
    float px = x + 0.5f, py = y + 0.5f;
    float lx1 = l->rect.x1 + r, lx2 = l->rect.x2 + 1 - r, ly1 = l->rect.y1 + r, ly2 = l->rect.y2 + 1 - r;
    float cx = px < lx1 ? lx1 : px > lx2 ? lx2 : px;
    float cy = py < ly1 ? ly1 : py > ly2 ? ly2 : py;
    float dx = px - cx, dy = py - cy;
    if (dx == 0 && dy == 0) return 256;
    float d = sqrtf(dx * dx + dy * dy) - r;
    float cv = 0.5f - d;
    return cv <= 0 ? 0 : cv >= 1 ? 256 : (int)(cv * 256);
}

static inline const uint16_t *layer_px(const bz_layer_t *l, int x, int y)
{
    return l->px + (ptrdiff_t)(y - l->oy) * l->stride + (x - l->ox);
}

/* Base pixels x0..x1 of row y into dst (both on screen). */
static void base_span(const bz_comp_t *c, int y, int x0, int x1, uint16_t *dst)
{
    int n = x1 - x0 + 1;
    memcpy(dst, c->content + (size_t)y * c->w + x0, (size_t)n * 2);
    for (int s = 0; s < BZ_COMP_MAX_LAYERS; s++) {
        if (!c->layer_on[s]) continue;
        const bz_layer_t *l = &c->layers[s];
        if (y < l->rect.y1 || y > l->rect.y2) continue;
        int a = x0 > l->rect.x1 ? x0 : l->rect.x1, b = x1 < l->rect.x2 ? x1 : l->rect.x2;
        if (a > b) continue;
        int r = (int)ceilf(l->radius);
        bool corner_row = r > 0 && (y < l->rect.y1 + r || y > l->rect.y2 - r);
        int ia = a, ib = b; /* the part with no antialiasing */
        if (corner_row) {
            if (ia < l->rect.x1 + r) ia = l->rect.x1 + r;
            if (ib > l->rect.x2 - r) ib = l->rect.x2 - r;
        }
        if (ia <= ib) memcpy(dst + (ia - x0), layer_px(l, ia, y), (size_t)(ib - ia + 1) * 2);
        if (!corner_row) continue;
        for (int x = a; x <= b; x++) {
            if (x >= ia && x <= ib) continue;
            int k = layer_cov(l, x, y);
            if (!k) continue;
            uint16_t lp = *layer_px(l, x, y);
            if (k >= 256) { dst[x - x0] = lp; continue; }
            int r0, g0, b0, r1, g1, b1;
            unpack565(dst[x - x0], &r0, &g0, &b0);
            unpack565(lp, &r1, &g1, &b1);
            dst[x - x0] = pack565(r0 + (((r1 - r0) * k) >> 8), g0 + (((g1 - g0) * k) >> 8), b0 + (((b1 - b0) * k) >> 8));
        }
    }
}

/* Base pixels of row y over x0..x1 which may run off screen (edges clamp), into tmp. */
static const uint16_t *base_row_clamped(const bz_comp_t *c, int y, int x0, int x1, uint16_t *tmp)
{
    y = y < 0 ? 0 : y >= c->h ? c->h - 1 : y;
    int a = x0 < 0 ? 0 : x0, b = x1 >= c->w ? c->w - 1 : x1;
    base_span(c, y, a, b, tmp + (a - x0));
    for (int x = x0; x < a; x++) tmp[x - x0] = tmp[a - x0];
    for (int x = b + 1; x <= x1; x++) tmp[x - x0] = tmp[b - x0];
    return tmp;
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
            if (nearest) *nearest = i;
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

static float band_of(const bz_glass_shape_t *s)
{
    float sc = s->scale > 0 ? s->scale : 1;
    float bw = 0.9f * s->radius * sc;
    return bw < 11 ? 11 : bw > 40 ? 40 : bw;
}

/* The bezel profile (1 − (1 − x)^4)^(1/4) and its slope, tabulated: they were two powf per pixel. */
static void tables_init(void)
{
    if (PROF) return;
    PROF = malloc(sizeof(float) * (PROF_N + 1) * 2);
    SLOPE = PROF + PROF_N + 1;
    GLOW = malloc(GLOW_N);
    for (int i = 0; i <= PROF_N; i++) {
        float x = (float)i / PROF_N;
        float om = 1 - x, om4 = om * om * om * om;
        PROF[i] = powf(1 - om4, 0.25f);
        float s = x >= 1 ? 0 : om * om * om * powf(1 - om4 > 1e-6f ? 1 - om4 : 1e-6f, -0.75f);
        SLOPE[i] = s > 12 ? 12 : s;
    }
    for (int i = 0; i < GLOW_N; i++) {
        float d = sqrtf((float)i / GLOW_N) * 4; /* in radii */
        GLOW[i] = (uint8_t)(expf(-d) * 255 + 0.5f);
    }
}

static inline float table(const float *t, float x)
{
    if (x <= 0) return t[0];
    if (x >= 1) return t[PROF_N];
    float f = x * PROF_N;
    int i = (int)f;
    return t[i] + (t[i + 1] - t[i]) * (f - i);
}

typedef struct {
    group_t *g;
    float bw_max, reach;
} lut_job_t;

/* Geometry at full strength and at rest: coverage, shadow, rim light and refraction offset. Strength,
 * press and tint scale these per frame, so a shape fading in, or pressed, keeps its table. */
static void lut_rows(bz_comp_t *c, void *arg, int y0, int y1, int part)
{
    lut_job_t *J = arg;
    group_t *g = J->g;
    const float eta = 1.0f / 1.5f;
    const bz_palette_t *P = bz_pal;
    int w = g->lw;
    uint32_t ring = 0;
    (void)P;
    for (int yy = y0; yy < y1; yy++) {
        float py = g->bbox.y1 + yy + 0.5f;
        int best0 = 0, best1 = -1, run0 = -1;
        for (int xx = 0; xx < w; xx++) {
            float px = g->bbox.x1 + xx + 0.5f;
            int i = yy * w + xx;
            int near = 0;
            float d = group_sd(c, g, px, py, &near);
            const bz_glass_shape_t *s = &c->shapes[g->idx[near]];
            g->sid[i] = (uint8_t)near;
            g->dx[i] = g->dy[i] = 0;
            g->ang[i] = g->rim[i] = g->band[i] = g->shadow[i] = 0;
            /* deep inside: flat glass, nothing but coverage; far outside: nothing */
            bool flat = false;
            if (d < -(J->bw_max + 2)) {
                g->cov[i] = 255;
                flat = true;
            } else if (d > J->reach) {
                g->cov[i] = 0;
            } else {
                ring++;
                float cov = (0.75f - d) / 1.5f;
                cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
                g->cov[i] = (uint8_t)(cov * 255 + 0.5f);
                if (cov < 1) {
                    float big = big_of(s);
                    float ds = group_sd(c, g, px, py - 7 * big, NULL);
                    float t = (ds + 5.6f) / (34 * big + 5.6f);
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    float sm = t * t * (3 - 2 * t);
                    float dark = (1 - sm) * 0.14f * big * P->shadow_gain;
                    g->shadow[i] = (uint8_t)(dark * 255 > 255 ? 255 : dark * 255);
                }
                if (cov > 0) {
                    /* outward normal from the field's gradient */
                    float e = 0.75f;
                    float gx = group_sd(c, g, px + e, py, NULL) - group_sd(c, g, px - e, py, NULL);
                    float gy = group_sd(c, g, px, py + e, NULL) - group_sd(c, g, px, py - e, NULL);
                    float gl = sqrtf(gx * gx + gy * gy);
                    if (gl > 1e-6f) { gx /= gl; gy /= gl; } else { gx = gy = 0; }

                    float depth = d < 0 ? -d : 0;
                    float bw = band_of(s);
                    float x = depth / bw;
                    float prof = table(PROF, x), slope = table(SLOPE, x);

                    if (!c->calm) {
                        float nx = gx * slope, ny = gy * slope, nz = 1;
                        float nl = sqrtf(nx * nx + ny * ny + 1);
                        nx /= nl; ny /= nl; nz /= nl;
                        float cosi = nz;
                        float k = 1 - eta * eta * (1 - cosi * cosi);
                        float coef = eta * cosi - sqrtf(k > 0 ? k : 0);
                        float tx = coef * nx, ty = coef * ny, tz = -eta + coef * nz;
                        float thick = BZ_GLASS_THICKNESS * (0.3f + 0.7f * prof);
                        float den = -tz > 0.3f ? -tz : 0.3f;
                        /* held at 1/1.4 so a press's 1.4× thickness still fits the table's int8 and the margin */
                        float ox = tx / den * thick, oy = ty / den * thick;
                        const float m = REFRACT_MARGIN / 1.4f;
                        ox = ox < -m ? -m : ox > m ? m : ox;
                        oy = oy < -m ? -m : oy > m ? m : oy;
                        g->dx[i] = (int8_t)lrintf(ox);
                        g->dy[i] = (int8_t)lrintf(oy);
                    }

                    /* one directional rim, about a pixel, and a faint inner band; how bright each is
                     * depends on which way the edge faces the light, applied per frame */
                    float rt = (depth - 0.35f) / (1.5f - 0.35f);
                    rt = rt < 0 ? 0 : rt > 1 ? 1 : rt;
                    float rim = 1 - rt * rt * (3 - 2 * rt);
                    float bt = depth / (0.6f * bw);
                    bt = bt > 1 ? 1 : bt;
                    float band = 1 - bt;
                    float a = atan2f(gy, gx);
                    g->ang[i] = (uint8_t)((int)lrintf(a * (128.0f / 3.14159265f)) & 255);
                    g->rim[i] = (uint8_t)(rim * 255 + 0.5f);
                    g->band[i] = (uint8_t)(band * 255 + 0.5f);
                    flat = g->cov[i] == 255 && !g->rim[i] && !g->band[i] && !g->dx[i] && !g->dy[i];
                }
            }
            /* the longest flat run on this row */
            if (flat) {
                if (run0 < 0) run0 = xx;
                if (xx - run0 > best1 - best0) { best0 = run0; best1 = xx; }
            } else {
                run0 = -1;
            }
        }
        g->span0[yy] = (int16_t)best0;
        g->span1[yy] = (int16_t)best1;
    }
    c->part_lut_ring[part] += ring;
}

static void build_lut(bz_comp_t *c, group_t *g)
{
    int w = g->bbox.x2 - g->bbox.x1 + 1, h = g->bbox.y2 - g->bbox.y1 + 1;
    int n = w * h;
    if (n > g->lcap) {
        free(g->cov); free(g->shadow); free(g->sid); free(g->ang); free(g->rim); free(g->band); free(g->dx); free(g->dy);
        g->cov = BIG_ALLOC((size_t)n); g->shadow = BIG_ALLOC((size_t)n); g->sid = BIG_ALLOC((size_t)n);
        g->ang = BIG_ALLOC((size_t)n); g->rim = BIG_ALLOC((size_t)n); g->band = BIG_ALLOC((size_t)n);
        g->dx = BIG_ALLOC((size_t)n); g->dy = BIG_ALLOC((size_t)n);
        bool ok = g->cov && g->shadow && g->sid && g->ang && g->rim && g->band && g->dx && g->dy;
        g->lcap = ok ? n : 0;
        if (!ok) { g->lut_valid = false; return; }
    }
    if (h > g->spcap) {
        free(g->span0); free(g->span1);
        g->span0 = malloc((size_t)h * 2); g->span1 = malloc((size_t)h * 2);
        g->spcap = g->span0 && g->span1 ? h : 0;
        if (!g->spcap) { g->lut_valid = false; return; }
    }
    g->lw = w;
    g->lh = h;

    lut_job_t J = { g, 0, 0 };
    for (int k = 0; k < g->nshapes; k++) {
        const bz_glass_shape_t *s = &c->shapes[g->idx[k]];
        float bw = band_of(s), big = big_of(s);
        if (bw > J.bw_max) J.bw_max = bw;
        if (41 * big + 2 > J.reach) J.reach = 41 * big + 2;
    }
    c->part_lut_ring[0] = c->part_lut_ring[1] = 0;
    run_rows(c, lut_rows, &J, 0, h);
    c->stats.lut_px += (uint32_t)n;
    c->stats.lut_ring_px += c->part_lut_ring[0] + c->part_lut_ring[1];
    g->lut_valid = true;
    g->nbuilt = g->nshapes;
    for (int k = 0; k < g->nshapes; k++) g->built[k] = c->shapes[g->idx[k]];
    c->stats.lut_rebuilds++;
}

/* ------------------------------------------------------------------ blur */

typedef struct {
    int w, h, len, lines;
    uint8_t *img;
    uint8_t **tmp;
    bool horizontal;
} box_job_t;

/* Box blur, one axis, lines [l0, l1), in place through tmp; edges clamp. */
static void box_lines(bz_comp_t *c, void *arg, int l0, int l1, int part)
{
    (void)c;
    box_job_t *b = arg;
    int len = b->len;
    int step = b->horizontal ? 3 : b->w * 3, lstep = b->horizontal ? b->w * 3 : 3;
    int win = 2 * BLUR_R + 1;
    uint8_t *tmp = b->tmp[part];
    for (int l = l0; l < l1; l++) {
        uint8_t *base = b->img + l * lstep;
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

static void blur_passes(bz_comp_t *c, uint8_t *img, uint8_t **tmp, int w, int h)
{
    for (int pass = 0; pass < 2; pass++) {
        box_job_t hj = { w, h, w, h, img, tmp, true };
        run_rows(c, box_lines, &hj, 0, h);
        box_job_t vj = { w, h, h, w, img, tmp, false };
        run_rows(c, box_lines, &vj, 0, w);
    }
}

typedef struct {
    bz_area_t a;          /* source area (may run off screen) */
    int ds, bw;
    uint8_t *dst;
    uint16_t *rowbuf[2];  /* ds rows of the area's width, per part */
} down_job_t;

/* Averages ds×ds blocks of the base over the job's area into RGB888, block rows [b0, b1). */
static void down_rows(bz_comp_t *c, void *arg, int b0, int b1, int part)
{
    down_job_t *J = arg;
    int ds = J->ds, n = ds * ds, aw = J->a.x2 - J->a.x1 + 1;
    uint16_t *rows = J->rowbuf[part];
    for (int by = b0; by < b1; by++) {
        const uint16_t *rp[8];
        for (int yy = 0; yy < ds; yy++)
            rp[yy] = base_row_clamped(c, J->a.y1 + by * ds + yy, J->a.x1, J->a.x2, rows + (size_t)yy * aw);
        for (int bx = 0; bx < J->bw; bx++) {
            int r = 0, g = 0, b = 0;
            for (int yy = 0; yy < ds; yy++) {
                const uint16_t *p = rp[yy] + bx * ds;
                for (int xx = 0; xx < ds; xx++) {
                    uint16_t q = p[xx];
                    r += q >> 11; g += (q >> 5) & 63; b += q & 31;
                }
            }
            uint8_t *o = J->dst + (by * J->bw + bx) * 3;
            /* 5/6-bit sums to 8 bits: ×255/31 and ×255/63 folded into the average */
            o[0] = (uint8_t)((r * 255) / (31 * n));
            o[1] = (uint8_t)((g * 255) / (63 * n));
            o[2] = (uint8_t)((b * 255) / (31 * n));
        }
    }
}

static bool downsample(bz_comp_t *c, const bz_area_t *a, int ds, uint8_t *dst, int bw, int bh)
{
    int aw = a->x2 - a->x1 + 1;
    down_job_t J = { *a, ds, bw, dst, { NULL, NULL } };
    J.rowbuf[0] = malloc((size_t)aw * ds * 2);
    J.rowbuf[1] = malloc((size_t)aw * ds * 2);
    if (!J.rowbuf[0] || !J.rowbuf[1]) {
        free(J.rowbuf[0]);
        free(J.rowbuf[1]);
        return false;
    }
    run_rows(c, down_rows, &J, 0, bh);
    free(J.rowbuf[0]);
    free(J.rowbuf[1]);
    c->stats.blur_src_px += (uint32_t)(aw * bh * ds);
    return true;
}

static void build_blur(bz_comp_t *c, group_t *g)
{
    bz_area_t a = g->bbox;
    if (g->blur_grow) {
        /* a translating group: a blur is of the base, not of the glass, so one that still covers the
         * glass's new place stays good; when it doesn't, grow it generously along the way */
        a.x1 = (int16_t)(a.x1 - 160); a.y1 = (int16_t)(a.y1 - 160);
        a.x2 = (int16_t)(a.x2 + 160); a.y2 = (int16_t)(a.y2 + 160);
        bz_area_t lim = { (int16_t)(-REFRACT_MARGIN - 16), (int16_t)(-REFRACT_MARGIN - 16),
                          (int16_t)(c->w + REFRACT_MARGIN + 16), (int16_t)(c->h + REFRACT_MARGIN + 16) };
        area_intersect(&a, &a, &lim);
    }
    int ax1 = a.x1 - REFRACT_MARGIN - 16, ay1 = a.y1 - REFRACT_MARGIN - 16;
    a.x1 = (int16_t)(ax1 >= 0 ? (ax1 / DS) * DS : -((-ax1 + DS - 1) / DS) * DS);
    a.y1 = (int16_t)(ay1 >= 0 ? (ay1 / DS) * DS : -((-ay1 + DS - 1) / DS) * DS);
    a.x2 = (int16_t)(a.x2 + REFRACT_MARGIN + 16);
    a.y2 = (int16_t)(a.y2 + REFRACT_MARGIN + 16);
    int bw = (a.x2 - a.x1 + DS) / DS, bh = (a.y2 - a.y1 + DS) / DS;
    a.x2 = (int16_t)(a.x1 + bw * DS - 1);
    a.y2 = (int16_t)(a.y1 + bh * DS - 1);
    if (!grow((void **)&g->blur, &g->bcap, bw * bh * 3, 1)) return;
    int tneed = (bw > bh ? bw : bh) * 3;
    if (tneed > g->tmpcap) {
        free(g->tmp[0]);
        free(g->tmp[1]);
        g->tmp[0] = BIG_ALLOC((size_t)tneed);
        g->tmp[1] = BIG_ALLOC((size_t)tneed);
        g->tmpcap = g->tmp[0] && g->tmp[1] ? tneed : 0;
        if (!g->tmpcap) return;
    }
    g->barea = a;
    g->bw = bw;
    g->bh = bh;
    if (!downsample(c, &a, DS, g->blur, bw, bh)) return;
    blur_passes(c, g->blur, g->tmp, bw, bh);
    g->blur_valid = true;
    g->blur_grow = false;
    g->tone_valid = false;
    c->stats.blur_rebuilds++;
}

/* The frost's tone, applied once to the small blurred image rather than to every glass pixel:
 * evening toward the palette's glass grey, then Bezel's saturation. Exact while strength is 1. */
static void build_tone(bz_comp_t *c, group_t *g)
{
    (void)c;
    int n = g->bw * g->bh;
    if (!grow((void **)&g->tone, &g->tcap, n * 3, 1)) return;
    const bz_palette_t *P = bz_pal;
    int er = bz_r(P->glass_even), eg = bz_g(P->glass_even), eb = bz_b(P->glass_even);
    int ev = (int)(P->glass_even_amt * 256), sat = (int)(BZ_GLASS_SATURATION * 256);
    for (int i = 0; i < n; i++) {
        const uint8_t *s = g->blur + i * 3;
        int r = s[0] + (((er - s[0]) * ev) >> 8), gg = s[1] + (((eg - s[1]) * ev) >> 8), b = s[2] + (((eb - s[2]) * ev) >> 8);
        int l = (54 * r + 183 * gg + 19 * b) >> 8;
        r = l + (((r - l) * sat) >> 8); gg = l + (((gg - l) * sat) >> 8); b = l + (((b - l) * sat) >> 8);
        uint8_t *o = g->tone + i * 3;
        o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        o[1] = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
        o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
    g->tone_valid = true;
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

/* What the geometry table depends on. Strength, press and tint are applied per frame. */
static bool geom_eq(const bz_glass_shape_t *a, const bz_glass_shape_t *b, float dx, float dy)
{
    return fabsf(a->x + dx - b->x) < 1e-3f && fabsf(a->y + dy - b->y) < 1e-3f && a->w == b->w && a->h == b->h &&
           a->radius == b->radius && (a->scale > 0 ? a->scale : 1) == (b->scale > 0 ? b->scale : 1);
}

/* Every shape the group's geometry was built for, moved by the same whole-pixel offset and otherwise
 * the same shape: the table can simply move with them (the dock tucking away, a sheet sliding in). */
static bool translated(const bz_comp_t *c, const group_t *g, int *odx, int *ody)
{
    if (!g->lut_valid || g->nbuilt != g->nshapes || !g->nshapes) return false;
    const bz_glass_shape_t *a0 = &g->built[0], *b0 = &c->shapes[g->idx[0]];
    float fx = b0->x - a0->x, fy = b0->y - a0->y;
    int dx = (int)lrintf(fx), dy = (int)lrintf(fy);
    if (fabsf(fx - dx) > 1e-3f || fabsf(fy - dy) > 1e-3f) return false;
    for (int k = 0; k < g->nshapes; k++)
        if (!geom_eq(&g->built[k], &c->shapes[g->idx[k]], (float)dx, (float)dy)) return false;
    *odx = dx;
    *ody = dy;
    return true;
}

/* The rect a shape could melt with others within: its scaled rect grown by half the merge distance,
 * so two touch when the gap between the shapes is under it (smin changes nothing wider). */
static void melt_rect(const bz_glass_shape_t *s, float *x1, float *y1, float *x2, float *y2)
{
    float sc = s->scale > 0 ? s->scale : 1;
    float cx = s->x + s->w * 0.5f, cy = s->y + s->h * 0.5f, m = BZ_GLASS_MERGE * 0.5f + 1;
    *x1 = cx - s->w * sc * 0.5f - m; *x2 = cx + s->w * sc * 0.5f + m;
    *y1 = cy - s->h * sc * 0.5f - m; *y2 = cy + s->h * sc * 0.5f + m;
}

static int uf_find(int *p, int i) { while (p[i] != i) i = p[i] = p[p[i]]; return i; }

static void regroup(bz_comp_t *c)
{
    /* 1: clusters — shapes of one group whose melt rects touch, transitively */
    int n = c->nshapes, parent[BZ_COMP_MAX_SHAPES];
    bool live[BZ_COMP_MAX_SHAPES];
    for (int i = 0; i < n; i++) {
        parent[i] = i;
        live[i] = c->shapes[i].strength > 0.001f && c->shapes[i].id < MAX_IDS;
    }
    for (int i = 0; i < n; i++) {
        if (!live[i]) continue;
        float a1, b1, a2, b2;
        melt_rect(&c->shapes[i], &a1, &b1, &a2, &b2);
        for (int j = i + 1; j < n; j++) {
            if (!live[j] || c->shapes[j].group != c->shapes[i].group || c->shapes[i].solo || c->shapes[j].solo) continue;
            float p1, q1, p2, q2;
            melt_rect(&c->shapes[j], &p1, &q1, &p2, &q2);
            if (a1 < p2 && p1 < a2 && b1 < q2 && q1 < b2) parent[uf_find(parent, i)] = uf_find(parent, j);
        }
    }
    for (int k = 0; k < MAX_CLUSTERS; k++) c->groups[k].seen = false;

    /* 2: each cluster onto the internal group that already knows these shapes, or a free one */
    for (int root = 0; root < n; root++) {
        if (!live[root] || uf_find(parent, root) != root) continue;
        uint8_t ids[BZ_COMP_MAX_SHAPES];
        int idx[BZ_COMP_MAX_SHAPES], m = 0;
        for (int i = 0; i < n; i++) if (live[i] && uf_find(parent, i) == root) idx[m++] = i;
        for (int a = 1; a < m; a++) /* ascending by id: the key's canonical order */
            for (int b = a; b > 0 && c->shapes[idx[b]].id < c->shapes[idx[b - 1]].id; b--) {
                int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t;
            }
        for (int k = 0; k < m; k++) ids[k] = c->shapes[idx[k]].id;
        uint8_t pub = c->shapes[root].group;
        group_t *g = NULL, *fresh = NULL;
        for (int k = 0; k < MAX_CLUSTERS && !g; k++) {
            group_t *q = &c->groups[k];
            if (!q->seen && q->nshapes == m && q->pub == pub && !memcmp(q->ids, ids, (size_t)m)) g = q;
        }
        for (int k = 0; k < MAX_CLUSTERS && !g && !fresh; k++) if (!c->groups[k].active && !c->groups[k].seen && !c->groups[k].nshapes) fresh = &c->groups[k];
        for (int k = 0; k < MAX_CLUSTERS && !g && !fresh; k++) if (!c->groups[k].active && !c->groups[k].seen) fresh = &c->groups[k];
        if (!g) {
            g = fresh;
            if (!g) continue; /* more clusters than slots: can't happen with ≤ 16 shapes */
            g->active = false;
            g->lut_valid = false;
            g->blur_valid = false;
            g->nbuilt = 0;
        }
        bool was = g->active;
        if (!was) g->blur_valid = false; /* revived: the base may have changed while it slept */
        g->seen = true;
        g->pub = pub;
        g->nshapes = m;
        memcpy(g->ids, ids, (size_t)m);
        memcpy(g->idx, idx, sizeof(int) * (size_t)m);
        g->active = true;

        /* 3: what changed since last frame? */
        bool changed = !was;
        for (int k = 0; k < m && !changed; k++) {
            int id = ids[k];
            changed = !c->prev_have[id] || !shape_eq(&c->shapes[idx[k]], &c->prev_by_id[id]);
        }
        if (!changed) continue;
        bz_area_t old = g->bbox;

        int dx = 0, dy = 0;
        if (translated(c, g, &dx, &dy)) {
            if (was && !dx && !dy) {
                /* strength, press or tint only: the same table, drawn differently */
                damage(c, g->bbox);
                continue;
            }
            if (was) damage(c, old);
            g->bbox.x1 = (int16_t)(old.x1 + dx); g->bbox.x2 = (int16_t)(old.x2 + dx);
            g->bbox.y1 = (int16_t)(old.y1 + dy); g->bbox.y2 = (int16_t)(old.y2 + dy);
            for (int k = 0; k < m; k++) g->built[k] = c->shapes[idx[k]];
            bz_area_t need = { (int16_t)(g->bbox.x1 - REFRACT_MARGIN), (int16_t)(g->bbox.y1 - REFRACT_MARGIN),
                               (int16_t)(g->bbox.x2 + REFRACT_MARGIN), (int16_t)(g->bbox.y2 + REFRACT_MARGIN) };
            bz_area_t in;
            if (!g->blur_valid || !area_intersect(&in, &need, &g->barea) || memcmp(&in, &need, sizeof in) != 0) {
                g->blur_valid = false;
                g->blur_grow = true;
            }
            c->stats.lut_moves++;
            damage(c, g->bbox);
            continue;
        }

#ifndef ESP_PLATFORM
        if (getenv("SIM_DEBUG_LUT")) {
            fprintf(stderr, "rebuild cluster pub %d ids", pub);
            for (int k = 0; k < m; k++) {
                const bz_glass_shape_t *s = &c->shapes[idx[k]], *b = &g->built[k];
                fprintf(stderr, " %d[%.1f,%.1f %.1fx%.1f r%.1f sc%.4f | was %.1f,%.1f %.1fx%.1f sc%.4f]", ids[k], s->x, s->y,
                        s->w, s->h, s->radius, s->scale, b->x, b->y, b->w, b->h, b->scale);
            }
            fprintf(stderr, " was %d nbuilt %d\n", was, g->nbuilt);
        }
#endif
        if (was) damage(c, old);
        bz_area_t bb = shape_bbox(&c->shapes[idx[0]]);
        for (int k = 1; k < m; k++) {
            bz_area_t b2 = shape_bbox(&c->shapes[idx[k]]);
            area_join(&bb, &b2);
        }
        bool moved = !was || memcmp(&bb, &old, sizeof bb) != 0;
        g->bbox = bb;
        g->lut_valid = false;
        if (moved) g->blur_valid = false;
        damage(c, bb);
    }

    /* 4: clusters no longer on screen go idle, keeping their caches */
    for (int k = 0; k < MAX_CLUSTERS; k++) {
        group_t *g = &c->groups[k];
        if (g->active && !g->seen) {
            damage(c, g->bbox);
            g->active = false;
        }
    }
    memset(c->prev_have, 0, sizeof c->prev_have);
    for (int i = 0; i < n; i++) {
        int id = c->shapes[i].id;
        if (id >= MAX_IDS) continue;
        c->prev_by_id[id] = c->shapes[i];
        c->prev_have[id] = true;
    }
}

/* ------------------------------------------------------------------ public */

bz_comp_t *bz_comp_create(int w, int h, uint16_t *content, uint32_t *ink, uint16_t *out)
{
    bz_comp_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    tables_init();
    quant_init();
    c->w = w;
    c->h = h;
    c->content = content;
    c->ink = ink;
    c->out = out;
    c->ops.copy565 = cpu_copy565;
    c->ops.blend = cpu_blend;
    c->ops.mix565 = cpu_mix565;
    c->lx = -0.42f;
    c->ly = -0.91f;
    c->dark = true;
    c->back_w = (w + BACK_DS - 1) / BACK_DS;
    c->back_h = (h + BACK_DS - 1) / BACK_DS;
    c->tw = (w + TILE - 1) / TILE;
    c->th = (h + TILE - 1) / TILE;
    c->tiles = calloc((size_t)(c->tw * c->th), 1);

    bz_comp_damage_all(c);
    return c;
}

void bz_comp_set_out2(bz_comp_t *c, uint16_t *out2)
{
    c->outs[0] = c->out;
    c->outs[1] = out2;
}

void bz_comp_set_ops(bz_comp_t *c, const bz_gfx_ops_t *ops)
{
    if (ops->copy565) c->ops.copy565 = ops->copy565;
    if (ops->blend) c->ops.blend = ops->blend;
    if (ops->mix565) c->ops.mix565 = ops->mix565;
    if (ops->parallel) c->ops.parallel = ops->parallel;
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
    /* the rim is lit per frame from each pixel's edge normal, so a lean costs a recomposite, not a
     * rebuild; under about a degree of change it isn't worth that (the IMU's noise lives there) */
    if (fabsf(lx - c->lx) < 0.015f && fabsf(ly - c->ly) < 0.015f) return;
    c->lx = lx;
    c->ly = ly;
    for (int i = 0; i < MAX_CLUSTERS; i++)
        if (c->groups[i].active) damage(c, c->groups[i].bbox);
}

void bz_comp_set_mode(bz_comp_t *c, bool dark, bool calm)
{
    if (dark == c->dark && calm == c->calm) return;
    c->dark = dark;
    c->calm = calm;
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        c->groups[i].lut_valid = false;
        c->groups[i].tone_valid = false;
        c->groups[i].nbuilt = 0;
    }
    c->back_valid = false;
    bz_comp_damage_all(c);
}

/* The backdrop is a blind: frosted above an edge that runs from above the screen (amount 0) to
 * below it (amount 1), fading over BAND px. Moving it redraws only the rows the edge crossed. */
static void back_rows_of(const bz_comp_t *c, float amount, int *lo, int *hi)
{
    float edge = amount * (c->h + BAND) - BAND / 2.0f;
    *lo = (int)floorf(edge - BAND / 2.0f);
    *hi = (int)ceilf(edge + BAND / 2.0f) - 1;
}

void bz_comp_set_backdrop(bz_comp_t *c, float amount, float dim)
{
    if (fabsf(amount - c->back_amt) < 0.0005f && fabsf(dim - c->back_dim) < 0.002f) return;
    bool was = c->back_amt > 0.002f;
    int olo = c->back_lo, ohi = c->back_hi;
    c->back_amt = amount;
    c->back_dim = dim;
    back_rows_of(c, amount > 0.002f ? amount : 0, &c->back_lo, &c->back_hi);
    if (!was) {
        c->back_valid = false; /* a fresh pull: build it from the page as it is now */
        olo = ohi = -BAND;
    }
    if (amount <= 0.002f) { c->back_lo = c->back_hi = -BAND; }
    int y1 = olo < c->back_lo ? olo : c->back_lo, y2 = ohi > c->back_hi ? ohi : c->back_hi;
    if (y1 < 0) y1 = 0;
    if (y2 >= c->h) y2 = c->h - 1;
    if (y1 <= y2) damage(c, (bz_area_t){ 0, (int16_t)y1, (int16_t)(c->w - 1), (int16_t)y2 });
}

static void invalidate_blurs(bz_comp_t *c, const bz_area_t *a)
{
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        group_t *g = &c->groups[i];
        if (!g->active) continue;
        if (!g->blur_valid || area_overlap(a, &g->barea)) {
            /* The glass samples its neighbourhood: new base under or near it redraws all of it. */
            g->blur_valid = false;
            damage(c, g->bbox);
        }
    }
    if (c->back_amt > 0.002f) c->back_stale = true;
}

static bool layer_eq(const bz_layer_t *a, const bz_layer_t *b) { return memcmp(a, b, sizeof *a) == 0; }

void bz_comp_set_layer(bz_comp_t *c, int slot, const bz_layer_t *l)
{
    if (slot < 0 || slot >= BZ_COMP_MAX_LAYERS) return;
    bool on = l != NULL;
    if (on == c->layer_on[slot] && (!on || layer_eq(l, &c->layers[slot]))) return;
    if (c->layer_on[slot]) {
        damage(c, c->layers[slot].rect);
        invalidate_blurs(c, &c->layers[slot].rect);
    }
    c->layer_on[slot] = on;
    if (on) {
        c->layers[slot] = *l;
        damage(c, l->rect);
        invalidate_blurs(c, &l->rect);
    }
}

void bz_comp_damage_content(bz_comp_t *c, const bz_area_t *a)
{
    damage(c, *a);
    invalidate_blurs(c, a);
}

void bz_comp_damage_ink(bz_comp_t *c, const bz_area_t *a)
{
    bz_area_t screen = screen_of(c), r;
    if (!area_intersect(&r, a, &screen)) return;
    /* rescan the tiles it touched: does anything on the glass layer show there now? */
    for (int ty = r.y1 / TILE; ty <= r.y2 / TILE; ty++) {
        for (int tx = r.x1 / TILE; tx <= r.x2 / TILE; tx++) {
            int x0 = tx * TILE, y0 = ty * TILE;
            int x1 = x0 + TILE > c->w ? c->w : x0 + TILE, y1 = y0 + TILE > c->h ? c->h : y0 + TILE;
            uint8_t any = 0;
            for (int y = y0; y < y1 && !any; y++) {
                const uint32_t *p = c->ink + (size_t)y * c->w;
                for (int x = x0; x < x1; x++) if (p[x] >> 24) { any = 1; break; }
            }
            uint8_t *t = &c->tiles[ty * c->tw + tx];
            if (*t != any) {
                *t = any;
                c->ink_rects_valid = false;
            }
        }
    }
    damage(c, r);
}

void bz_comp_damage_all(bz_comp_t *c)
{
    c->ndirty = 1;
    c->dirty[0] = screen_of(c);
}

/* The occupied tiles as a few rectangles: runs along each tile row, merged down while they line up. */
static void build_ink_rects(bz_comp_t *c)
{
    c->nink = 0;
    for (int ty = 0; ty < c->th; ty++) {
        int tx = 0;
        while (tx < c->tw) {
            if (!c->tiles[ty * c->tw + tx]) { tx++; continue; }
            int s = tx;
            while (tx < c->tw && c->tiles[ty * c->tw + tx]) tx++;
            bz_area_t r = { (int16_t)(s * TILE), (int16_t)(ty * TILE), (int16_t)(tx * TILE - 1),
                            (int16_t)((ty + 1) * TILE - 1) };
            if (r.x2 >= c->w) r.x2 = (int16_t)(c->w - 1);
            if (r.y2 >= c->h) r.y2 = (int16_t)(c->h - 1);
            bool merged = false;
            for (int k = 0; k < c->nink && !merged; k++) {
                bz_area_t *q = &c->ink_rects[k];
                if (q->x1 == r.x1 && q->x2 == r.x2 && q->y2 + 1 == r.y1) { q->y2 = r.y2; merged = true; }
            }
            if (merged) continue;
            if (c->nink < MAX_INK_RECTS) c->ink_rects[c->nink++] = r;
            else area_join(&c->ink_rects[MAX_INK_RECTS - 1], &r);
        }
    }
    c->ink_rects_valid = true;
}

/* ------------------------------------------------------------------ backdrop */

typedef struct {
    int darken, desat;   /* 8.8 */
} back_job_t;

/* Upsamples the toned 1/8 image to full resolution (bilinear, dithered), rows [y0, y1). */
static void back_rows(bz_comp_t *c, void *arg, int y0, int y1, int part)
{
    (void)arg; (void)part;
    for (int y = y0; y < y1; y++) {
        uint16_t *d = c->back_full + (size_t)y * c->w;
        int fy = (y * 256 + 128) / BACK_DS;
        for (int x = 0; x < c->w; x++) {
            int r, g, b;
            sample_bilinear(c->back, c->back_w, c->back_h, (x * 256 + 128) / BACK_DS, fy, &r, &g, &b);
            d[x] = pack565_dither(r, g, b, x, y);
        }
    }
}

/* The page behind the control center, fully frosted and dimmed (cc.js:154; shaders.js:139-142):
 * blurred at 1/8, darkened and desaturated there, then brought back to full size once. The pull only
 * cross-fades it in; the page's live numbers change under it at most twice a second. */
static void build_backdrop(bz_comp_t *c)
{
    int n = c->back_w * c->back_h * 3;
    if (!c->back) c->back = BIG_ALLOC((size_t)n);
    int tn = (c->back_w > c->back_h ? c->back_w : c->back_h) * 3;
    if (!c->back_tmp[0]) c->back_tmp[0] = BIG_ALLOC((size_t)tn);
    if (!c->back_tmp[1]) c->back_tmp[1] = BIG_ALLOC((size_t)tn);
    if (!c->back_full) c->back_full = BIG_ALLOC((size_t)c->w * c->h * 2);
    if (!c->back || !c->back_tmp[0] || !c->back_tmp[1] || !c->back_full) return;
    bz_area_t a = { 0, 0, (int16_t)(c->back_w * BACK_DS - 1), (int16_t)(c->back_h * BACK_DS - 1) };
    if (!downsample(c, &a, BACK_DS, c->back, c->back_w, c->back_h)) return;
    blur_passes(c, c->back, c->back_tmp, c->back_w, c->back_h);
    float dim = c->back_dim;
    int darken = (int)((1 - dim * (c->dark ? 0.52f : 0.30f)) * 256), desat = (int)(dim * 0.35f * 256);
    for (int i = 0; i < c->back_w * c->back_h; i++) {
        uint8_t *p = c->back + i * 3;
        int l = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
        for (int ch = 0; ch < 3; ch++) {
            int v = p[ch] + (((l - p[ch]) * desat) >> 8);
            v = (v * darken) >> 8;
            p[ch] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    run_rows(c, back_rows, NULL, 0, c->h);
    c->stats.backdrop_build_px += (uint32_t)(c->w * c->h);
    c->back_valid = true;
    c->back_stale = false;
    c->back_built_dim = dim;
    c->back_built_us = now_us();
}

/* ------------------------------------------------------------------ glass */

typedef struct glass_job {
    group_t *g;
    bz_area_t a;
    /* per shape of the cluster */
    int st[BZ_COMP_MAX_SHAPES];      /* strength, 0..256 */
    int thick[BZ_COMP_MAX_SHAPES];   /* refraction scale: (1 + 0.4·press) (× strength off the fast path) */
    int glow[BZ_COMP_MAX_SHAPES];    /* press glow amplitude, 0..~56 */
    float px[BZ_COMP_MAX_SHAPES], py[BZ_COMP_MAX_SHAPES];
    bool tinted[BZ_COMP_MAX_SHAPES];
    float gk;                        /* d² → glow table index */
    bool press;
    /* the light, by edge direction: rim and band gains, 8.8 */
    int16_t lr[256], lb[256];
    int16_t *vrow[2];                /* a row of the toned blur, interpolated vertically, per part */
} glass_job_t;

/* The flat interior of a glass, untinted: the toned frost, bilinear, with the vertical interpolation
 * done once per row, cross-faded over the base by strength while it materializes. By far the most
 * common glass pixel. */
static void flat_span(const group_t *g, int16_t *v, int y, int x0, int x1, uint16_t *dst, int st)
{
    int fy = ((y - g->barea.y1) * 256 + 128) / DS - 128;
    int ty0 = fy >> 8, ay = fy & 255;
    int ty1 = ty0 + 1;
    ty0 = ty0 < 0 ? 0 : ty0 >= g->bh ? g->bh - 1 : ty0;
    ty1 = ty1 < 0 ? 0 : ty1 >= g->bh ? g->bh - 1 : ty1;
    /* texel centres sit at .5: pixel x samples between texels a and a+1 at 64ths that step by 16 */
    int fx0 = (x0 - g->barea.x1) * (256 / DS) + 128 / DS - 128;
    int t0 = fx0 >> 8, t1 = ((fx0 + (x1 - x0) * (256 / DS)) >> 8) + 1;
    if (t0 < 0) t0 = 0;
    if (t1 >= g->bw) t1 = g->bw - 1;
    const uint8_t *r0 = g->tone + (size_t)ty0 * g->bw * 3, *r1 = g->tone + (size_t)ty1 * g->bw * 3;
    /* the row between the two texel rows, ×16 for the horizontal steps' precision */
    for (int t = t0; t <= t1; t++)
        for (int ch = 0; ch < 3; ch++)
            v[t * 3 + ch] = (int16_t)((r0[t * 3 + ch] * 16) + (((r1[t * 3 + ch] - r0[t * 3 + ch]) * ay) >> 4));
    const int8_t *dith = &BAYER[(y & 3) * 4];
    int fx = fx0;
    int a = fx >> 8;
    const int16_t *pa = v + (a < t0 ? t0 : a) * 3, *pb = v + (a + 1 > t1 ? t1 : a + 1) * 3;
    for (int x = x0; x <= x1; x++, fx += 256 / DS) {
        if ((fx >> 8) != a) {
            a = fx >> 8;
            pa = v + (a < t0 ? t0 : a > t1 ? t1 : a) * 3;
            pb = v + (a + 1 > t1 ? t1 : a + 1) * 3;
        }
        int ax = fx & 255;
        int r = (pa[0] + (((pb[0] - pa[0]) * ax) >> 8)) >> 4;
        int gg = (pa[1] + (((pb[1] - pa[1]) * ax) >> 8)) >> 4;
        int bl = (pa[2] + (((pb[2] - pa[2]) * ax) >> 8)) >> 4;
        if (st < 256) {
            int ur, ug, ub;
            unpack565(dst[x], &ur, &ug, &ub);
            r = ur + (((r - ur) * st) >> 8); gg = ug + (((gg - ug) * st) >> 8); bl = ub + (((bl - ub) * st) >> 8);
        }
        int d = dith[x & 3];
        dst[x] = (uint16_t)(Q_R[qi(r + d)] | Q_G[qi(gg + (d >> 1))] | Q_B[qi(bl + d)]);
    }
}

static void glass_rows(bz_comp_t *c, void *arg, int y0, int y1, int part)
{
    glass_job_t *J = arg;
    group_t *g = J->g;
    const bz_area_t a = J->a;
    const bz_palette_t *P = bz_pal;
    int er = bz_r(P->glass_even), eg = bz_g(P->glass_even), eb = bz_b(P->glass_even);
    int rimr = bz_r(P->rim), rimg = bz_g(P->rim), rimb = bz_b(P->rim);
    int s2r = bz_r(P->surface2), s2g = bz_g(P->surface2), s2b = bz_b(P->surface2);
    bool fast = g->fast && !c->calm;
    for (int y = y0; y < y1; y++) {
        int ly = y - g->bbox.y1;
        uint16_t *dst = c->out + (size_t)y * c->w;
        size_t row = (size_t)ly * g->lw;
        /* the flat run of this row inside the area, done in one go */
        int f0 = g->span0[ly] + g->bbox.x1, f1 = g->span1[ly] + g->bbox.x1;
        if (f0 < a.x1) f0 = a.x1;
        if (f1 > a.x2) f1 = a.x2;
        bool flat = fast && !J->press && f0 <= f1;
        if (flat) {
            flat_span(g, J->vrow[part], y, f0, f1, dst, J->st[g->sid[row + (size_t)(f0 - g->bbox.x1)]]);
            c->part_flat[part] += (uint32_t)(f1 - f0 + 1);
        }
        for (int x = a.x1; x <= a.x2; x++) {
            if (flat && x == f0) { x = f1; continue; }
            size_t i = row + (size_t)(x - g->bbox.x1);
            int cov = g->cov[i], sh = g->shadow[i];
            if (!cov && !sh) continue; /* no glass or shadow here */
            int k = g->sid[i];
            int st = J->st[k];
            if (cov) c->part_edge[part]++;
            else c->part_shadow[part]++;
            /* the fast and calm paths compose the glass whole and cross-fade it in by strength; a tinted
             * glass (rare, and small) materializes the long way, each term by strength */
            bool whole = !J->tinted[k] || c->calm;
            int sw = whole ? 256 : st;
            if (!whole) sh = (sh * st) >> 8;
            int ur, ug, ub; /* the composite so far: the base (or its backdrop) and earlier glass */
            unpack565(dst[x], &ur, &ug, &ub);
            int br = ur, bgc = ug, bb = ub;
            if (sh && cov < 255) {
                int m = 255 - (sh * (255 - cov)) / 255;
                br = br * m / 255; bgc = bgc * m / 255; bb = bb * m / 255;
            }
            int rr = br, gg = bgc, bl = bb;
            if (cov) {
                int lt = (g->rim[i] * J->lr[g->ang[i]] + g->band[i] * J->lb[g->ang[i]]) >> 8;
                if (J->glow[k]) {
                    float ddx = x + 0.5f - J->px[k], ddy = y + 0.5f - J->py[k];
                    int gi = (int)((ddx * ddx + ddy * ddy) * J->gk);
                    if (gi < GLOW_N) lt += (J->glow[k] * GLOW[gi]) >> 8;
                }
                if (!whole) lt = (lt * st) >> 8;
                int fr, fg, fb;
                int sx = x + ((g->dx[i] * J->thick[k]) >> 8), sy = y + ((g->dy[i] * J->thick[k]) >> 8);
                int tx = ((sx - g->barea.x1) * 256 + 128) / DS, ty = ((sy - g->barea.y1) * 256 + 128) / DS;
                if (c->calm) {
                    fr = s2r; fg = s2g; fb = s2b;
                } else if (fast) {
                    sample_bilinear(g->tone, g->bw, g->bh, tx, ty, &fr, &fg, &fb);
                } else {
                    const bz_glass_shape_t *s = &c->shapes[g->idx[k]];
                    int qr, qg, qb;
                    sample_bilinear(g->blur, g->bw, g->bh, tx, ty, &qr, &qg, &qb);
                    /* frost scales with strength: sharp base → frosted as the glass materializes */
                    fr = ur + (((qr - ur) * sw) >> 8); fg = ug + (((qg - ug) * sw) >> 8); fb = ub + (((qb - ub) * sw) >> 8);
                    int ev = (int)(P->glass_even_amt * sw);
                    fr += ((er - fr) * ev) >> 8; fg += ((eg - fg) * ev) >> 8; fb += ((eb - fb) * ev) >> 8;
                    int l = (54 * fr + 183 * fg + 19 * fb) >> 8;
                    int sat = 256 + (int)((BZ_GLASS_SATURATION - 1) * sw);
                    fr = l + (((fr - l) * sat) >> 8); fg = l + (((fg - l) * sat) >> 8); fb = l + (((fb - l) * sat) >> 8);
                    if (s->tint >= 0 && s->tint_amt > 0) {
                        uint32_t tc = tint_color(s->tint);
                        int kk = 210 + ((90 * l) >> 8);              /* (0.82 + 0.35·l) in 8.8 */
                        int amt = (int)(0.82f * s->tint_amt * sw);    /* 8.8 */
                        fr += ((((bz_r(tc) * kk) >> 8) + 10 - fr) * amt) >> 8;
                        fg += ((((bz_g(tc) * kk) >> 8) + 10 - fg) * amt) >> 8;
                        fb += ((((bz_b(tc) * kk) >> 8) + 10 - fb) * amt) >> 8;
                    }
                }
                fr += (rimr * lt) >> 8; fg += (rimg * lt) >> 8; fb += (rimb * lt) >> 8;
                if (cov == 255) {
                    rr = fr; gg = fg; bl = fb;
                } else {
                    rr = br + (((fr - br) * cov) >> 8);
                    gg = bgc + (((fg - bgc) * cov) >> 8);
                    bl = bb + (((fb - bb) * cov) >> 8);
                }
            }
            if (whole && st < 256) {
                rr = ur + (((rr - ur) * st) >> 8); gg = ug + (((gg - ug) * st) >> 8); bl = ub + (((bl - ub) * st) >> 8);
            }
            dst[x] = pack565_dither(rr, gg, bl, x, y);
        }
    }
}

static void compose_group(bz_comp_t *c, group_t *g, const bz_area_t *r)
{
    bz_area_t a;
    if (!area_intersect(&a, r, &g->bbox)) return;
    if (!c->job) c->job = malloc(sizeof *c->job);
    glass_job_t J_, *Jp = c->job ? c->job : &J_;
#define J (*Jp)
    memset(&J, 0, sizeof J);
    J.g = g;
    J.a = a;
    J.gk = GLOW_N / (16.0f * BZ_GLASS_GLOW_RADIUS * BZ_GLASS_GLOW_RADIUS);
    for (int k = 0; k < g->nshapes; k++) {
        const bz_glass_shape_t *s = &c->shapes[g->idx[k]];
        float st = s->strength > 1 ? 1 : s->strength;
        float pr = s->press < 0 ? 0 : s->press;
        J.tinted[k] = s->tint >= 0 && s->tint_amt > 0.001f;
        J.st[k] = (int)(st * 256);
        J.thick[k] = (int)((J.tinted[k] ? st : 1) * (1 + 0.4f * pr) * 256);
        J.glow[k] = (int)(0.22f * pr * 256);
        J.px[k] = s->press_x;
        J.py[k] = s->press_y;
        J.press |= J.glow[k] > 0;
    }
    /* the light by edge direction: f = cos(normal − light); rim 0.16 + 0.62 f₊^1.5 + 0.26 f₋², band
     * 0.07 f₊ + 0.04 f₋, times the palette's rim gain (Bezel's shader, one directional light) */
    float ll = sqrtf(c->lx * c->lx + c->ly * c->ly);
    float la = atan2f(ll > 0 ? c->ly : -0.91f, ll > 0 ? c->lx : -0.42f);
    float gain = bz_pal->rim_gain * 256;
    for (int i = 0; i < 256; i++) {
        float f = cosf(i * (3.14159265f / 128.0f) - la);
        float fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
        J.lr[i] = (int16_t)((0.16f + 0.62f * fp * sqrtf(fp) + 0.26f * fn * fn) * gain);
        J.lb[i] = (int16_t)((0.07f * fp + 0.04f * fn) * gain);
    }
    int16_t vbuf[2][1536];
    bool heap = g->bw * 3 > 1536;
    J.vrow[0] = heap ? malloc((size_t)g->bw * 3 * 2) : vbuf[0];
    J.vrow[1] = heap ? malloc((size_t)g->bw * 3 * 2) : vbuf[1];
    for (int p = 0; p < 2; p++) c->part_flat[p] = c->part_edge[p] = c->part_shadow[p] = 0;
    if (J.vrow[0] && J.vrow[1]) run_rows(c, glass_rows, &J, a.y1, a.y2 + 1);
    c->stats.glass_flat_px += c->part_flat[0] + c->part_flat[1];
    c->stats.glass_edge_px += c->part_edge[0] + c->part_edge[1];
    c->stats.glass_shadow_px += c->part_shadow[0] + c->part_shadow[1];
    if (heap) { free(J.vrow[0]); free(J.vrow[1]); }
#undef J
    uint32_t px = (uint32_t)area_size(&a);
    c->stats.glass_px += px;
    if (g->fast && !c->calm) c->stats.glass_fast_px += px;
}

/* ------------------------------------------------------------------ cells */

enum { SRC_CONTENT = -1, SRC_MIXED = -2, SRC_BACK = -3 };

typedef struct cell {
    bz_area_t a;
    bool compose;
    int src;              /* SRC_CONTENT, a layer slot, or SRC_MIXED */
} cell_t;
#define MAX_CELLS 256

static int cmp_i16(const void *a, const void *b) { return *(const int16_t *)a - *(const int16_t *)b; }

static int uniq(int16_t *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_i16);
    int m = 0;
    for (int i = 0; i < n; i++) if (!m || v[m - 1] != v[i]) v[m++] = v[i];
    return m;
}

/* Where a whole-cell's pixels live: the topmost layer covering it, else the content. */
static int cell_source(const bz_comp_t *c, int x, int y)
{
    if (c->back_amt > 0.002f && c->back_valid && y < c->back_lo) return SRC_BACK;
    for (int s = BZ_COMP_MAX_LAYERS - 1; s >= 0; s--) {
        if (!c->layer_on[s]) continue;
        const bz_area_t *r = &c->layers[s].rect;
        if (x >= r->x1 && x <= r->x2 && y >= r->y1 && y <= r->y2) return s;
    }
    return SRC_CONTENT;
}

/* Splits dirty rect D into cells that either go to the panel straight from their source, or need
 * compositing (glass, backdrop, ink, a layer's antialiased corner). Returns the count, or -1 when the
 * split would be too fine to be worth it (the caller composites D whole). */
static int split(bz_comp_t *c, const bz_area_t *D, cell_t *cells, int max, bool all_compose)
{
    bz_area_t special[MAX_CLUSTERS + MAX_INK_RECTS + 4 * BZ_COMP_MAX_LAYERS];
    bool mixed[MAX_CLUSTERS + MAX_INK_RECTS + 4 * BZ_COMP_MAX_LAYERS];
    int ns = 0;
    int16_t xs[MAX_EDGES], ys[MAX_EDGES];
    int nx = 0, ny = 0;
#define EDGE(r)                                                                                       \
    do {                                                                                              \
        if (nx + 2 <= MAX_EDGES && ny + 2 <= MAX_EDGES) {                                             \
            xs[nx++] = (r).x1 > D->x1 ? (r).x1 : D->x1; xs[nx++] = (int16_t)(((r).x2 < D->x2 ? (r).x2 : D->x2) + 1); \
            ys[ny++] = (r).y1 > D->y1 ? (r).y1 : D->y1; ys[ny++] = (int16_t)(((r).y2 < D->y2 ? (r).y2 : D->y2) + 1); \
        } else {                                                                                      \
            return -1;                                                                                \
        }                                                                                             \
    } while (0)
    bz_area_t t;
    xs[nx++] = D->x1; xs[nx++] = (int16_t)(D->x2 + 1);
    ys[ny++] = D->y1; ys[ny++] = (int16_t)(D->y2 + 1);
    for (int gi = 0; gi < MAX_CLUSTERS; gi++) {
        group_t *g = &c->groups[gi];
        if (!g->active || !area_intersect(&t, D, &g->bbox)) continue;
        mixed[ns] = false;
        special[ns++] = t;
        EDGE(t);
    }
    for (int k = 0; k < c->nink; k++) {
        if (!area_intersect(&t, D, &c->ink_rects[k])) continue;
        mixed[ns] = false;
        special[ns++] = t;
        EDGE(t);
    }
    if (c->back_amt > 0.002f && c->back_valid) {
        /* the blind: frosted rows come straight from its picture; its soft edge is mixed */
        bz_area_t top = { D->x1, 0, D->x2, (int16_t)(c->back_lo - 1) };
        if (area_intersect(&t, D, &top)) EDGE(t);
        bz_area_t band = { D->x1, (int16_t)c->back_lo, D->x2, (int16_t)c->back_hi };
        if (area_intersect(&t, D, &band)) {
            mixed[ns] = false;
            special[ns++] = t;
            EDGE(t);
        }
    }
    for (int s = 0; s < BZ_COMP_MAX_LAYERS; s++) {
        if (!c->layer_on[s]) continue;
        const bz_layer_t *l = &c->layers[s];
        if (!area_intersect(&t, D, &l->rect)) continue;
        EDGE(t);
        int r = (int)ceilf(l->radius);
        if (r <= 0) continue;
        bz_area_t corners[4] = {
            { l->rect.x1, l->rect.y1, (int16_t)(l->rect.x1 + r - 1), (int16_t)(l->rect.y1 + r - 1) },
            { (int16_t)(l->rect.x2 - r + 1), l->rect.y1, l->rect.x2, (int16_t)(l->rect.y1 + r - 1) },
            { l->rect.x1, (int16_t)(l->rect.y2 - r + 1), (int16_t)(l->rect.x1 + r - 1), l->rect.y2 },
            { (int16_t)(l->rect.x2 - r + 1), (int16_t)(l->rect.y2 - r + 1), l->rect.x2, l->rect.y2 },
        };
        for (int k = 0; k < 4; k++) {
            if (!area_intersect(&t, D, &corners[k])) continue;
            mixed[ns] = true; /* per-pixel coverage: assembled by the CPU */
            special[ns++] = t;
            EDGE(t);
        }
    }
#undef EDGE
    nx = uniq(xs, nx);
    ny = uniq(ys, ny);

    int n = 0;
    for (int by = 0; by + 1 < ny; by++) {
        int y1 = ys[by], y2 = ys[by + 1] - 1;
        int this_start = n;
        cell_t run = { { 0, 0, 0, 0 }, false, 0 };
        bool have = false;
        for (int bx = 0; bx + 1 < nx; bx++) {
            int x1 = xs[bx], x2 = xs[bx + 1] - 1;
            bool comp = all_compose, mix = false;
            for (int k = 0; k < ns; k++) {
                if (x1 >= special[k].x1 && x1 <= special[k].x2 && y1 >= special[k].y1 && y1 <= special[k].y2) {
                    comp = true;
                    mix |= mixed[k];
                }
            }
            int src = mix ? SRC_MIXED : cell_source(c, x1, y1);
            if (have && run.compose == comp && run.src == src) {
                run.a.x2 = (int16_t)x2;
                continue;
            }
            if (have) {
                if (n >= max) return -1;
                cells[n++] = run;
            }
            run = (cell_t){ { (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2 }, comp, src };
            have = true;
        }
        if (have) {
            if (n >= max) return -1;
            cells[n++] = run;
        }
        /* merge each new cell into an identical one ending right above it */
        for (int i = this_start; i < n; i++) {
            for (int j = 0; j < this_start; j++) {
                cell_t *u = &cells[j];
                if (u->a.x1 == cells[i].a.x1 && u->a.x2 == cells[i].a.x2 && u->a.y2 + 1 == cells[i].a.y1 &&
                    u->compose == cells[i].compose && u->src == cells[i].src) {
                    u->a.y2 = cells[i].a.y2;
                    cells[i].a.x1 = 1; cells[i].a.x2 = 0; /* consumed */
                    break;
                }
            }
        }
        /* drop consumed cells, keeping this band's survivors together */
        int m = this_start;
        for (int i = this_start; i < n; i++) if (cells[i].a.x1 <= cells[i].a.x2) cells[m++] = cells[i];
        n = m;
    }
    return n;
}

typedef struct {
    bz_area_t a;
} fill_job_t;

static void fill_rows(bz_comp_t *c, void *arg, int y0, int y1, int part)
{
    (void)part;
    fill_job_t *J = arg;
    for (int y = y0; y < y1; y++) base_span(c, y, J->a.x1, J->a.x2, c->out + (size_t)y * c->w + J->a.x1);
}

/* Composites one cell into `out`: base, backdrop, glass, ink. */
typedef struct {
    bz_area_t a;
} blind_job_t;

/* The blind over the base in `out`, rows [y0, y1): its picture above the edge, mixed across it. */
static void blind_rows(bz_comp_t *c, void *arg, int y0, int y1, int part)
{
    (void)part;
    blind_job_t *J = arg;
    int n = J->a.x2 - J->a.x1 + 1;
    for (int y = y0; y < y1; y++) {
        size_t o = (size_t)y * c->w + J->a.x1;
        if (y < c->back_lo) {
            memcpy(c->out + o, c->back_full + o, (size_t)n * 2);
            continue;
        }
        if (y > c->back_hi) continue;
        float t = (float)(c->back_hi + 1 - y) / (float)(c->back_hi + 1 - c->back_lo);
        int k = (int)(t * t * (3 - 2 * t) * 256);
        if (k <= 0) continue;
        uint16_t *d = c->out + o;
        const uint16_t *f = c->back_full + o;
        for (int x = 0; x < n; x++) {
            int ar, ag, ab, br, bg, bb;
            unpack565(d[x], &ar, &ag, &ab);
            unpack565(f[x], &br, &bg, &bb);
            d[x] = pack565_dither(ar + (((br - ar) * k) >> 8), ag + (((bg - ag) * k) >> 8), ab + (((bb - ab) * k) >> 8),
                                  J->a.x1 + x, y);
        }
    }
}

static void compose_cell(bz_comp_t *c, const cell_t *cell, bool back)
{
    const bz_area_t *r = &cell->a;
    int w = r->x2 - r->x1 + 1, h = r->y2 - r->y1 + 1;
    size_t off = (size_t)r->y1 * c->w + r->x1;
    /* 1: the base under everything */
    if (cell->src == SRC_BACK) {
        c->ops.copy565(c->out + off, c->w, c->back_full + off, c->w, w, h);
    } else if (cell->src == SRC_CONTENT) {
        c->ops.copy565(c->out + off, c->w, c->content + off, c->w, w, h);
    } else if (cell->src >= 0) {
        const bz_layer_t *l = &c->layers[cell->src];
        c->ops.copy565(c->out + off, c->w, layer_px(l, r->x1, r->y1), l->stride, w, h);
    } else {
        fill_job_t J = { *r };
        run_rows(c, fill_rows, &J, r->y1, r->y2 + 1);
        c->stats.base_cpu_px += (uint32_t)(w * h);
    }
    /* 2: frosted and dimmed behind the control center, as far down as the blind has come */
    if (back && c->back_valid && cell->src != SRC_BACK && r->y1 <= c->back_hi) {
        blind_job_t J = { *r };
        int y2 = r->y2 < c->back_hi ? r->y2 : c->back_hi;
        run_rows(c, blind_rows, &J, r->y1, y2 + 1);
        c->stats.mix_px += (uint32_t)(w * (y2 - r->y1 + 1));
    }
    /* 3: each glass group over the running composite, in group order; glass samples only the base */
    for (int pub = 0; pub < BZ_COMP_MAX_GROUPS; pub++) {
        for (int gi = 0; gi < MAX_CLUSTERS; gi++) {
            group_t *g = &c->groups[gi];
            if (g->active && g->pub == pub && g->lut_valid && (c->calm || g->blur_valid) &&
                (!g->fast || g->tone_valid || c->calm))
                compose_group(c, g, r);
        }
    }
    /* 4: what sits on the glass — its labels and icons — last, crisp */
    for (int k = 0; k < c->nink; k++) {
        bz_area_t t;
        if (!area_intersect(&t, r, &c->ink_rects[k])) continue;
        int tw = t.x2 - t.x1 + 1, th = t.y2 - t.y1 + 1;
        size_t o = (size_t)t.y1 * c->w + t.x1;
        c->ops.blend(c->out + o, c->w, c->out + o, c->w, c->ink + o, c->w, tw, th);
        c->stats.ink_px += (uint32_t)(tw * th);
    }
    c->stats.composed_px += (uint32_t)(w * h);
}

static void emit(bz_comp_t *c, bz_present_t *out, int *n, int max, const cell_t *cell)
{
    bz_present_t p = { cell->a, NULL, c->w };
    if (cell->compose || cell->src == SRC_MIXED) {
        p.src = c->out + (size_t)cell->a.y1 * c->w + cell->a.x1;
    } else if (cell->src == SRC_BACK) {
        p.src = c->back_full + (size_t)cell->a.y1 * c->w + cell->a.x1;
        c->stats.direct_px += (uint32_t)area_size(&cell->a);
    } else if (cell->src == SRC_CONTENT) {
        p.src = c->content + (size_t)cell->a.y1 * c->w + cell->a.x1;
        c->stats.direct_px += (uint32_t)area_size(&cell->a);
    } else {
        const bz_layer_t *l = &c->layers[cell->src];
        p.src = layer_px(l, cell->a.x1, cell->a.y1);
        p.stride = l->stride;
        c->stats.direct_px += (uint32_t)area_size(&cell->a);
    }
    if (*n < max) out[(*n)++] = p;
}

int bz_comp_compose(bz_comp_t *c, bz_present_t *out, int max)
{
    if (!c->ndirty) return 0;
    if (!c->cells && !(c->cells = malloc(sizeof(cell_t) * MAX_CELLS))) return 0;
    int64_t t0 = now_us();
    memset(&c->stats, 0, sizeof c->stats);
    if (c->outs[1]) c->out = c->outs[++c->frame & 1];
    bool back = c->back_amt > 0.002f;
    if (back) {
        /* rebuilt from the page as it is now: at the start of a pull, and while the page changes under
         * a control center that has come to rest (never mid-pull, which would stall the finger) */
        bool resting = fabsf(c->back_amt - c->prev_back_amt) < 0.002f;
        if (!c->back_valid || (c->back_stale && resting && now_us() - c->back_built_us > 500000) ||
            fabsf(c->back_dim - c->back_built_dim) > 0.01f) {
            build_backdrop(c);
            if (c->back_hi >= 0) damage(c, (bz_area_t){ 0, 0, (int16_t)(c->w - 1), (int16_t)(c->back_hi < c->h ? c->back_hi : c->h - 1) });
        }
    }
    c->prev_back_amt = c->back_amt;
    if (!c->ink_rects_valid) build_ink_rects(c);

    for (int gi = 0; gi < MAX_CLUSTERS; gi++) {
        group_t *g = &c->groups[gi];
        if (!g->active) continue;
        bool touched = false;
        for (int i = 0; i < c->ndirty && !touched; i++) touched = area_overlap(&c->dirty[i], &g->bbox);
        if (!touched) continue;
        g->fast = true;
        for (int k = 0; k < g->nshapes; k++) {
            const bz_glass_shape_t *s = &c->shapes[g->idx[k]];
            if (s->tint >= 0 && s->tint_amt > 0.001f) g->fast = false;
        }
        if (!g->lut_valid) build_lut(c, g);
        if (!c->calm && !g->blur_valid) build_blur(c, g);
        if (!c->calm && g->fast && g->blur_valid && !g->tone_valid) build_tone(c, g);
    }

    int n = 0;
    cell_t *cells = c->cells;
    for (int i = 0; i < c->ndirty; i++) {
        bz_area_t *D = &c->dirty[i];
        int k = split(c, D, cells, MAX_CELLS, false);
        /* too many pieces for the panel's queue: take this area whole */
        if (k < 0 || n + k > max - (c->ndirty - i - 1)) {
            bool layered = false;
            for (int s = 0; s < BZ_COMP_MAX_LAYERS; s++) layered |= c->layer_on[s] && area_overlap(D, &c->layers[s].rect);
            cell_t whole = { *D, true, layered ? SRC_MIXED : SRC_CONTENT };
            compose_cell(c, &whole, back);
            emit(c, out, &n, max, &whole);
            c->stats.cells++;
            continue;
        }
        for (int j = 0; j < k; j++) {
            if (cells[j].compose) compose_cell(c, &cells[j], back);
            emit(c, out, &n, max, &cells[j]);
        }
        c->stats.cells += (uint32_t)k;
    }
    c->ndirty = 0;
    c->stats.compose_us = (uint32_t)(now_us() - t0);
    return n;
}

void bz_comp_stats(bz_comp_t *c, bz_comp_stats_t *out) { *out = c->stats; }

#ifndef ESP_PLATFORM
#include <stdio.h>
void bz_comp_debug_base(bz_comp_t *c, uint16_t *dst)
{
    for (int y = 0; y < c->h; y++) base_span(c, y, 0, c->w - 1, dst + (size_t)y * c->w);
}

/* The simulator's `dump glass`: each cluster's blurred and toned images as PPM, for eyes. */
void bz_comp_debug_dump(bz_comp_t *c, const char *dir)
{
    for (int k = 0; k < MAX_CLUSTERS; k++) {
        group_t *g = &c->groups[k];
        if (!g->active || !g->blur_valid) continue;
        for (int which = 0; which < 2; which++) {
            const uint8_t *img = which ? g->tone : g->blur;
            if (!img || (which && !g->tone_valid)) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/glass%d-%s-g%d.ppm", dir, k, which ? "tone" : "blur", g->pub);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            fprintf(f, "P6 %d %d 255\n", g->bw, g->bh);
            fwrite(img, 3, (size_t)(g->bw * g->bh), f);
            fclose(f);
            printf("%s  barea %d,%d-%d,%d  bbox %d,%d-%d,%d\n", path, g->barea.x1, g->barea.y1, g->barea.x2, g->barea.y2,
                   g->bbox.x1, g->bbox.y1, g->bbox.x2, g->bbox.y2);
        }
    }
}
#endif
