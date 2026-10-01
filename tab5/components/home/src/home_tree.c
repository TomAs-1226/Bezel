/* home_tree — see home_tree.h. The mesh follows Bezel's tree.js line by line (the same seeded sequences, so the
 * same tree); the drawing is its own: triangles transformed once, then rasterized in bands of rows at twice the
 * picture's resolution each way against a small z-buffer, each band averaged down over what the picture held. */
#include "home_tree.h"

#include "home_px.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f

enum { K_WOOD, K_LEAF, K_FRUIT, K_BLOSSOM };
enum { T_WOOD, T_LEAF, T_DEEP, T_FRESH, T_BUD, T_BLOSSOM, T_GOLD, T_RED, T_RUST, T_TIP, T_FRUIT, T_FLOWER, T_N };

typedef struct {
    float p[3][3];
    float n[3];
    float pivot[3];
    float order;
    float shade;          /* the facet's own brightness, times its tone's colour */
    uint8_t tone, kind, index;
} tri_t;

struct ht_mesh {
    tri_t *t;
    int n, cap;
    int steps, fruit_max;
    float col[T_N][3];
};

/* ---------------------------------------------------------------- building */

typedef struct {
    uint32_t a;
} lcg_t;
static float rnd(lcg_t *r)
{
    r->a = r->a * 1664525u + 1013904223u;
    return (float)(r->a / 4294967296.0);
}

static void v_norm(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-9f) l = 1;
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
}

static void cross3(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void push(ht_mesh_t *m, const float a[3], const float b[3], const float d[3], int tone, float shade,
                 const float pivot[3], float order, int kind, int index)
{
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 1024;
        tri_t *nt = realloc(m->t, sizeof(tri_t) * (size_t)nc);
        if (!nt) return;
        m->t = nt;
        m->cap = nc;
    }
    tri_t *t = &m->t[m->n++];
    for (int i = 0; i < 3; i++) {
        t->p[0][i] = a[i];
        t->p[1][i] = b[i];
        t->p[2][i] = d[i];
        t->pivot[i] = pivot[i];
    }
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
    cross3(e1, e2, t->n);
    v_norm(t->n);
    /* outward, whatever the winding: away from the point the piece grows from */
    float out[3] = { (a[0] + b[0] + d[0]) / 3 - pivot[0], (a[1] + b[1] + d[1]) / 3 - pivot[1],
                     (a[2] + b[2] + d[2]) / 3 - pivot[2] };
    if (t->n[0] * out[0] + t->n[1] * out[1] + t->n[2] * out[2] < 0)
        for (int i = 0; i < 3; i++) t->n[i] = -t->n[i];
    t->order = order;
    t->shade = shade;
    t->tone = (uint8_t)tone;
    t->kind = (uint8_t)kind;
    t->index = (uint8_t)index;
}

#define ICO_T 1.61803399f
static const float ICO[12][3] = {
    { -1, ICO_T, 0 }, { 1, ICO_T, 0 }, { -1, -ICO_T, 0 }, { 1, -ICO_T, 0 }, { 0, -1, ICO_T }, { 0, 1, ICO_T },
    { 0, -1, -ICO_T }, { 0, 1, -ICO_T }, { ICO_T, 0, -1 }, { ICO_T, 0, 1 }, { -ICO_T, 0, -1 }, { -ICO_T, 0, 1 },
};
static const uint8_t ICO_F[20][3] = {
    { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
    { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
    { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
};
static const uint8_t OCTA_F[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
                                      { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };

/* the crown month by month (northern hemisphere): the share of leaf clusters in leaf, their size, and the tones
 * they take; winter is bare but for pale tips at the ends of the twigs */
typedef struct {
    bool tips;
    float keep, size;
    float w[T_N];
} look_t;
static const look_t MONTHS[12] = {
    { true, 0, 0, { 0 } },
    { true, 0, 0, { 0 } },
    { false, 0.6f, 0.72f, { [T_BUD] = 0.45f, [T_BLOSSOM] = 0.55f } },
    { false, 1, 0.92f, { [T_BLOSSOM] = 0.7f, [T_FRESH] = 0.3f } },
    { false, 1, 1.0f, { [T_FRESH] = 0.65f, [T_BLOSSOM] = 0.35f } },
    { false, 1, 1.08f, { [T_LEAF] = 0.65f, [T_DEEP] = 0.35f } },
    { false, 1, 1.1f, { [T_LEAF] = 0.55f, [T_DEEP] = 0.45f } },
    { false, 1, 1.08f, { [T_LEAF] = 0.5f, [T_DEEP] = 0.4f, [T_GOLD] = 0.1f } },
    { false, 1, 1.0f, { [T_LEAF] = 0.35f, [T_GOLD] = 0.4f, [T_RED] = 0.25f } },
    { false, 0.9f, 0.95f, { [T_GOLD] = 0.45f, [T_RED] = 0.45f, [T_LEAF] = 0.1f } },
    { false, 0.45f, 0.85f, { [T_RED] = 0.5f, [T_RUST] = 0.5f } },
    { true, 0, 0, { 0 } },
};

static int pick_tone(const look_t *l, float r)
{
    float sum = 0;
    int last = T_LEAF;
    for (int i = 0; i < T_N; i++) {
        if (l->w[i] <= 0) continue;
        sum += l->w[i];
        last = i;
        if (r < sum) return i;
    }
    return last;
}

typedef struct {
    float centre[3], r, dirs[12][3], shades[20];
    float order;
    bool terminal;
} cluster_t;

typedef struct {
    ht_mesh_t *m;
    lcg_t rand;
    int step;
    cluster_t *cl;
    int ncl, capcl;
} build_t;

static void basis(const float dir[3], float x[3], float y[3])
{
    float up[3] = { 0, 1, 0 };
    if (fabsf(dir[1]) >= 0.9f) {
        up[0] = 1;
        up[1] = 0;
    }
    cross3(dir, up, x);
    v_norm(x);
    cross3(dir, x, y);
}

static void prism(build_t *b, const float base[3], const float dir[3], float length, float r0, float r1, float order,
                  float top[3])
{
    float bx[3], by[3];
    basis(dir, bx, by);
    for (int i = 0; i < 3; i++) top[i] = base[i] + dir[i] * length;
    float shade = 0.9f + rnd(&b->rand) * 0.2f;
    for (int k = 0; k < 5; k++) {
        float a0 = k / 5.0f * 2 * PI_F, a1 = (k + 1) / 5.0f * 2 * PI_F;
        float A[3], B[3], D[3], E[3];
        for (int i = 0; i < 3; i++) {
            A[i] = base[i] + (bx[i] * cosf(a0) + by[i] * sinf(a0)) * r0;
            B[i] = base[i] + (bx[i] * cosf(a1) + by[i] * sinf(a1)) * r0;
            D[i] = top[i] + (bx[i] * cosf(a0) + by[i] * sinf(a0)) * r1;
            E[i] = top[i] + (bx[i] * cosf(a1) + by[i] * sinf(a1)) * r1;
        }
        push(b->m, A, B, E, T_WOOD, shade, base, order, K_WOOD, 0);
        push(b->m, A, E, D, T_WOOD, shade, base, order, K_WOOD, 0);
    }
}

static void cluster(build_t *b, const float centre[3], float r, float order, bool terminal)
{
    if (b->ncl == b->capcl) {
        int nc = b->capcl ? b->capcl * 2 : 64;
        cluster_t *n = realloc(b->cl, sizeof(cluster_t) * (size_t)nc);
        if (!n) return;
        b->cl = n;
        b->capcl = nc;
    }
    cluster_t *c = &b->cl[b->ncl++];
    memcpy(c->centre, centre, sizeof c->centre);
    c->r = r;
    c->order = order;
    c->terminal = terminal;
    for (int i = 0; i < 12; i++) {
        float v[3] = { ICO[i][0], ICO[i][1], ICO[i][2] };
        v_norm(v);
        float s = 0.85f + rnd(&b->rand) * 0.3f;
        for (int k = 0; k < 3; k++) c->dirs[i][k] = v[k] * s;
    }
    for (int f = 0; f < 20; f++) c->shades[f] = rnd(&b->rand);
}

static void grow(build_t *b, const float base[3], const float dir[3], float length, float radius, int depth)
{
    float order = (float)b->step++;
    float top[3];
    prism(b, base, dir, length, radius, radius * 0.68f, order, top);
    if (depth >= 4) {
        float r = (0.2f + rnd(&b->rand) * 0.1f) * 1.3f; /* fuller than tree.js: this crown is seen big */
        cluster(b, top, r, (float)b->step++, true);
        return;
    }
    int children = depth == 0 ? 3 : 2 + (rnd(&b->rand) < 0.4f ? 1 : 0);
    for (int k = 0; k < children; k++) {
        float spin = (float)k / children * 2 * PI_F + rnd(&b->rand) * 1.2f + depth;
        float lean = 0.42f + rnd(&b->rand) * 0.3f;
        float d[3] = { sinf(lean) * cosf(spin) * 0.9f + dir[0] * 0.5f, cosf(lean) + dir[1] * 0.3f,
                       sinf(lean) * sinf(spin) * 0.9f + dir[2] * 0.5f };
        v_norm(d);
        float len = length * (0.72f + rnd(&b->rand) * 0.1f);
        grow(b, top, d, len, radius * 0.66f, depth + 1);
    }
    if (depth >= 2 && rnd(&b->rand) < 0.85f) {
        float r = (0.16f + rnd(&b->rand) * 0.08f) * 1.3f;
        cluster(b, top, r, (float)b->step++, false);
    }
}

static int by_angle(const void *a, const void *b)
{
    const cluster_t *x = *(const cluster_t *const *)a, *y = *(const cluster_t *const *)b;
    float ax = atan2f(x->centre[2], x->centre[0]), ay = atan2f(y->centre[2], y->centre[0]);
    return ax < ay ? -1 : ax > ay;
}

/* n picks spread round a list sorted by angle, so that consecutive picks land on different sides */
static int spread(cluster_t **list, int len, int n, cluster_t **out)
{
    if (len <= 0) return 0;
    bool used[256] = { 0 };
    int k = 0;
    for (int i = 0; k < n && k < len && len <= 256; i++) {
        int j = (int)floorf(fmodf(i * 0.618034f, 1.0f) * len);
        while (used[j]) j = (j + 1) % len;
        used[j] = true;
        out[k++] = list[j];
    }
    return k;
}

static void octa(ht_mesh_t *m, const float c[3], float r, float stretch, int tone, float order, int kind, int index)
{
    float p[6][3] = { { r, 0, 0 }, { -r, 0, 0 }, { 0, r * stretch, 0 }, { 0, -r * stretch, 0 }, { 0, 0, r }, { 0, 0, -r } };
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) p[i][k] += c[k];
    for (int f = 0; f < 8; f++) push(m, p[OCTA_F[f][0]], p[OCTA_F[f][1]], p[OCTA_F[f][2]], tone, 1.0f, c, order, kind, index);
}

static void mix3(const float a[3], const float b[3], float t, float o[3])
{
    for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}

static void rgb3(uint32_t c, float o[3])
{
    o[0] = hpx_r(c) / 255.0f;
    o[1] = hpx_g(c) / 255.0f;
    o[2] = hpx_b(c) / 255.0f;
}

void ht_palette(bool dark, uint32_t leaf, uint32_t amber, ht_palette_t *p)
{
    p->wood = dark ? 0x5C4030 : 0x75543D;
    p->leaf = leaf;
    p->amber = amber;
    p->blossom = dark ? 0xF5CCDB : 0xEDADC4;
    p->flower = dark ? 0xFCE6EF : 0xF7CCDE;
    p->tip = dark ? 0xD6E0D4 : 0xB3C2B3;
}

ht_mesh_t *ht_build(int month, const ht_palette_t *pal, int fruit_room)
{
    ht_mesh_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    /* the palette, as tree.js makes it from the panel's tokens */
    float leaf[3], amber[3], w[3];
    rgb3(pal->leaf, leaf);
    rgb3(pal->amber, amber);
    rgb3(pal->wood, m->col[T_WOOD]);
    memcpy(m->col[T_LEAF], leaf, sizeof leaf);
    for (int i = 0; i < 3; i++) m->col[T_DEEP][i] = leaf[i] * 0.74f;
    mix3(leaf, (float[]){ 0.84f, 0.95f, 0.6f }, 0.3f, m->col[T_FRESH]);
    mix3(leaf, (float[]){ 0.9f, 0.94f, 0.8f }, 0.5f, m->col[T_BUD]);
    rgb3(pal->blossom, m->col[T_BLOSSOM]);
    for (int i = 0; i < 3; i++) m->col[T_GOLD][i] = amber[i] * 0.95f;
    mix3(amber, (float[]){ 0.76f, 0.2f, 0.12f }, 0.7f, m->col[T_RED]);
    mix3(amber, (float[]){ 0.52f, 0.25f, 0.13f }, 0.72f, m->col[T_RUST]);
    rgb3(pal->tip, m->col[T_TIP]);
    mix3(amber, (float[]){ 0.96f, 0.5f, 0.08f }, 0.4f, m->col[T_FRUIT]);
    rgb3(pal->flower, m->col[T_FLOWER]);
    (void)w;

    if (month < 0 || month > 11) month = 0;
    const look_t *look = &MONTHS[month];
    build_t b = { .m = m, .rand = { 1226 } };
    lcg_t roll = { 4711 };
    grow(&b, (float[]){ 0, 0, 0 }, (float[]){ 0, 1, 0 }, 0.62f, 0.11f, 0);
    m->steps = b.step;

    /* fruit hangs on twig ends spread round the crown; blossoms on others */
    enum { FRUIT_MAX = 24, BLOSSOMS = 12 };
    cluster_t *ends[256], *hosts_f[FRUIT_MAX], *hosts_b[BLOSSOMS], *rest[256];
    int ne = 0;
    for (int i = 0; i < b.ncl && ne < 256; i++)
        if (b.cl[i].terminal) ends[ne++] = &b.cl[i];
    qsort(ends, (size_t)ne, sizeof ends[0], by_angle);
    int want = fruit_room < FRUIT_MAX ? fruit_room : FRUIT_MAX;
    int nf = spread(ends, ne, want, hosts_f);
    int nr = 0;
    for (int i = 0; i < ne; i++) {
        bool f = false;
        for (int k = 0; k < nf; k++) f |= hosts_f[k] == ends[i];
        if (!f) rest[nr++] = ends[i];
    }
    int nb = look->tips || month < 2 || month > 5 ? 0 : spread(rest, nr, BLOSSOMS, hosts_b);
    m->fruit_max = nf;
    float size = look->tips ? 0.34f : look->size;

    for (int i = 0; i < b.ncl; i++) {
        cluster_t *k = &b.cl[i];
        bool host = false;
        for (int j = 0; j < nf; j++) host |= hosts_f[j] == k;
        for (int j = 0; j < nb; j++) host |= hosts_b[j] == k;
        int tone = -1;
        if (look->tips) tone = k->terminal ? T_TIP : -1;
        else if (rnd(&roll) < look->keep || host) tone = pick_tone(look, rnd(&roll));
        if (tone < 0) continue;
        float v[12][3];
        for (int p = 0; p < 12; p++)
            for (int q = 0; q < 3; q++) v[p][q] = k->centre[q] + k->dirs[p][q] * k->r * size;
        for (int f = 0; f < 20; f++)
            push(m, v[ICO_F[f][0]], v[ICO_F[f][1]], v[ICO_F[f][2]], tone, 0.82f + k->shades[f] * 0.32f, k->centre,
                 k->order, K_LEAF, 0);
    }
    /* fruit: a short stalk below the outer edge of its cluster, where the crown's shade sets it off */
    for (int i = 0; i < nf; i++) {
        cluster_t *k = hosts_f[i];
        float l = sqrtf(k->centre[0] * k->centre[0] + k->centre[2] * k->centre[2]);
        float o[3] = { l > 1e-3f ? k->centre[0] / l : 1, 0, l > 1e-3f ? k->centre[2] / l : 0 };
        float r = k->r * size;
        float top[3] = { k->centre[0] + o[0] * r * 0.55f, k->centre[1] - r * 0.95f, k->centre[2] + o[2] * r * 0.55f };
        float centre[3] = { top[0], top[1] - 0.14f, top[2] };
        float end[3] = { top[0], centre[1] + 0.065f, top[2] };
        for (int s = 0; s < 3; s++) {
            float a = s / 3.0f * 2 * PI_F, a2 = (s + 1) / 3.0f * 2 * PI_F;
            float ta[3] = { top[0] + cosf(a) * 0.008f, top[1], top[2] + sinf(a) * 0.008f };
            float tb[3] = { top[0] + cosf(a2) * 0.008f, top[1], top[2] + sinf(a2) * 0.008f };
            float ea[3] = { end[0] + cosf(a) * 0.008f, end[1], end[2] + sinf(a) * 0.008f };
            float eb[3] = { end[0] + cosf(a2) * 0.008f, end[1], end[2] + sinf(a2) * 0.008f };
            push(m, ta, ea, eb, T_WOOD, 1.0f, top, k->order, K_FRUIT, i);
            push(m, ta, eb, tb, T_WOOD, 1.0f, top, k->order, K_FRUIT, i);
        }
        octa(m, centre, 0.062f, 1.15f, T_FRUIT, k->order, K_FRUIT, i);
    }
    for (int i = 0; i < nb; i++) {
        cluster_t *k = hosts_b[i];
        float l = sqrtf(k->centre[0] * k->centre[0] + k->centre[2] * k->centre[2]);
        float d[3] = { l > 1e-3f ? k->centre[0] / l : 1, 0.9f, l > 1e-3f ? k->centre[2] / l : 0 };
        v_norm(d);
        float reach = k->r * size * 1.12f + 0.035f;
        float c[3] = { k->centre[0] + d[0] * reach, k->centre[1] + d[1] * reach, k->centre[2] + d[2] * reach };
        octa(m, c, 0.05f, 0.8f, i % 3 == 2 ? T_BLOSSOM : T_FLOWER, k->order, K_BLOSSOM, i);
    }
    free(b.cl);
    /* the triangles' array was grown by doubling: back to what it holds */
    tri_t *fit = m->n ? realloc(m->t, sizeof(tri_t) * (size_t)m->n) : NULL;
    if (fit) {
        m->t = fit;
        m->cap = m->n;
    }
    return m;
}

void ht_free(ht_mesh_t *m)
{
    if (!m) return;
    free(m->t);
    free(m);
}

int ht_steps(const ht_mesh_t *m) { return m ? m->steps : 0; }
int ht_fruit_max(const ht_mesh_t *m) { return m ? m->fruit_max : 0; }

/* ---------------------------------------------------------------- drawing */

typedef struct {
    float x[3], y[3], z[3];   /* picture pixels ×2 (samples), depth: distance from the eye */
    uint32_t rgb;
    float x0, y0, x1, y1;
} proj_t;

static float smoothstep(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

#define SS 2
#define EYE_Z 5.7f
#define EYE_Y 1.18f
#define PITCH (-0.12f)

/* one point of the tree into view space: turned about its trunk, bent by the wind, tipped by the pitch */
static void to_view(const ht_view_t *v, const float p[3], float o[3])
{
    float cy = cosf(v->yaw), sy = sinf(v->yaw);
    float x = p[0] * cy + p[2] * sy, z = -p[0] * sy + p[2] * cy, y = p[1];
    /* the wind: a bend that grows with the square of the height, always to the right (the field depends only on
     * where a point is, so joints never open) */
    float h = y > 0 ? y : 0;
    x += v->lean * 0.075f * h * h;
    y -= v->lean * 0.012f * h * h;
    float cp = cosf(PITCH), sp = sinf(PITCH);
    o[0] = x;
    o[1] = y * cp - z * sp;
    o[2] = y * sp + z * cp;
}

static bool project(const ht_mesh_t *m, const ht_view_t *v, const tri_t *t, proj_t *out)
{
    float s = smoothstep(t->order, t->order + 1.4f, v->growth);
    if (t->kind == K_FRUIT) s *= smoothstep((float)t->index, (float)t->index + 1.0f, (float)v->fruit);
    if (s <= 0.001f) return false;
    float F = v->scale * EYE_Z;
    for (int i = 0; i < 3; i++) {
        float p[3], q[3];
        for (int k = 0; k < 3; k++) p[k] = t->pivot[k] + (t->p[i][k] - t->pivot[k]) * s;
        to_view(v, p, q);
        float depth = EYE_Z - q[2];
        if (depth < 0.5f) return false;
        out->x[i] = (v->cx + q[0] / depth * F) * SS;
        out->y[i] = (v->base_y - EYE_Y * v->scale + (EYE_Y - q[1]) / depth * F) * SS;
        out->z[i] = depth;
    }
    /* the facet's light: its normal turned the same way (the bend is small enough to leave normals be) */
    float n[3];
    to_view(&(ht_view_t){ .yaw = v->yaw }, t->n, n); /* no wind: a pure turn, as a normal needs */
    float d = n[0] * v->light[0] + n[1] * v->light[1] + n[2] * v->light[2];
    float lit = v->ambient + (1 - v->ambient) * (d > 0 ? d : 0);
    if (t->kind >= K_FRUIT) lit = fmaxf(lit, t->kind == K_BLOSSOM ? 0.78f : 0.84f);
    const float *c = m->col[t->tone];
    float tr = hpx_r(v->tint) / 255.0f, tg = hpx_g(v->tint) / 255.0f, tb = hpx_b(v->tint) / 255.0f;
    float k = lit * t->shade;
    out->rgb = hpx_rgb((int)(c[0] * k * tr * 255), (int)(c[1] * k * tg * 255), (int)(c[2] * k * tb * 255));
    out->x0 = fminf(out->x[0], fminf(out->x[1], out->x[2]));
    out->x1 = fmaxf(out->x[0], fmaxf(out->x[1], out->x[2]));
    out->y0 = fminf(out->y[0], fminf(out->y[1], out->y[2]));
    out->y1 = fmaxf(out->y[0], fmaxf(out->y[1], out->y[2]));
    return true;
}

void ht_bounds(const ht_mesh_t *m, const ht_view_t *v, float o[4])
{
    o[0] = o[1] = 1e9f;
    o[2] = o[3] = -1e9f;
    for (int i = 0; m && i < m->n; i++) {
        proj_t p;
        if (!project(m, v, &m->t[i], &p)) continue;
        o[0] = fminf(o[0], p.x0 / SS);
        o[1] = fminf(o[1], p.y0 / SS);
        o[2] = fmaxf(o[2], p.x1 / SS);
        o[3] = fmaxf(o[3], p.y1 / SS);
    }
}

/* one triangle into a band of samples (rows by0..by1 of the picture ×2), with its z against the band's */
static void raster(const proj_t *p, uint32_t *col, float *zb, int bw, int by0, int by1)
{
    int xa = (int)floorf(p->x0), xb = (int)ceilf(p->x1), ya = (int)floorf(p->y0), yb = (int)ceilf(p->y1);
    if (xa < 0) xa = 0;
    if (xb > bw - 1) xb = bw - 1;
    if (ya < by0) ya = by0;
    if (yb > by1) yb = by1;
    if (xa > xb || ya > yb) return;
    float x0 = p->x[0], y0 = p->y[0], x1 = p->x[1], y1 = p->y[1], x2 = p->x[2], y2 = p->y[2];
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(area) < 1e-6f) return;
    float inv = 1.0f / area;
    for (int y = ya; y <= yb; y++) {
        float py = y + 0.5f;
        float *zr = zb + (size_t)(y - by0) * bw;
        uint32_t *cr = col + (size_t)(y - by0) * bw;
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * p->z[0] + w1 * p->z[1] + w2 * p->z[2];
            if (z >= zr[x]) continue;
            zr[x] = z;
            cr[x] = p->rgb;
        }
    }
}

void ht_paint(const ht_mesh_t *m, uint16_t *px, int w, int h, int stride, const ht_view_t *v)
{
    if (!m || !m->n) return;
    proj_t *pj = malloc(sizeof(proj_t) * (size_t)m->n);
    if (!pj) return;
    int np = 0;
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = 0; i < m->n; i++)
        if (project(m, v, &m->t[i], &pj[np])) {
            bx0 = fminf(bx0, pj[np].x0);
            by0 = fminf(by0, pj[np].y0);
            bx1 = fmaxf(bx1, pj[np].x1);
            by1 = fmaxf(by1, pj[np].y1);
            np++;
        }
    if (!np) {
        free(pj);
        return;
    }
    /* only the tree's own rectangle, in bands of 16 picture rows */
    int X0 = (int)floorf(bx0 / SS) - 1, X1 = (int)ceilf(bx1 / SS) + 1, Y0 = (int)floorf(by0 / SS) - 1,
        Y1 = (int)ceilf(by1 / SS) + 1;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > w - 1) X1 = w - 1;
    if (Y1 > h - 1) Y1 = h - 1;
    if (X0 > X1 || Y0 > Y1) {
        free(pj);
        return;
    }
    const int BAND = 8;
    int bw = (X1 - X0 + 1) * SS; /* samples across */
    uint32_t *col = malloc(sizeof(uint32_t) * (size_t)bw * BAND * SS);
    float *zb = malloc(sizeof(float) * (size_t)bw * BAND * SS);
    hpx_t c;
    hpx_init16(&c, px, w, h, stride);
    for (int y = Y0; col && zb && y <= Y1; y += BAND) {
        int rows = y + BAND - 1 <= Y1 ? BAND : Y1 - y + 1;
        int sy0 = y * SS, sy1 = (y + rows) * SS - 1;
        /* samples start as the picture's pixels; the tree is drawn over them */
        for (int r = 0; r < rows * SS; r++) {
            const uint16_t *src = px + (size_t)(y + r / SS) * stride + X0;
            uint32_t *d = col + (size_t)r * bw;
            float *z = zb + (size_t)r * bw;
            for (int x = 0; x < bw; x++) {
                d[x] = 0xFF000000u; /* not the tree: the picture's own pixel */
                z[x] = 1e9f;
            }
            (void)src;
        }
        for (int i = 0; i < np; i++) {
            if (pj[i].y1 < sy0 || pj[i].y0 > sy1 + 1) continue;
            proj_t p = pj[i];
            for (int k = 0; k < 3; k++) p.x[k] -= X0 * SS;
            p.x0 -= X0 * SS;
            p.x1 -= X0 * SS;
            raster(&p, col, zb, bw, sy0, sy1);
        }
        /* each pixel: the mean of its samples, the tree's colours over the picture's for the rest */
        for (int r = 0; r < rows; r++) {
            int yy = y + r;
            for (int x = 0; x < X1 - X0 + 1; x++) {
                int rr = 0, gg = 0, bb = 0, n = 0;
                for (int sy = 0; sy < SS; sy++)
                    for (int sx = 0; sx < SS; sx++) {
                        uint32_t s = col[(size_t)(r * SS + sy) * bw + x * SS + sx];
                        if (s & 0xFF000000u) continue;
                        rr += hpx_r(s);
                        gg += hpx_g(s);
                        bb += hpx_b(s);
                        n++;
                    }
                if (!n) continue;
                uint32_t mean = hpx_rgb(rr / n, gg / n, bb / n);
                hpx_put(&c, X0 + x, yy, mean, n * 255 / (SS * SS));
            }
        }
    }
    free(col);
    free(zb);
    free(pj);
}
