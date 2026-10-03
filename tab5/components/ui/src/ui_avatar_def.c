/* ui_avatar_def — see ui_avatar_def.h. */
#include "ui_avatar_def.h"

#include "json_lite.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== the character that ships with the firmware
 *
 * Ours: a squat soft blob with two round-ended bars for eyes, written in the format's own terms (head turn,
 * eye width height x y angle, spacing) so that it and a card's file go through the same renderer. The numbers
 * are our own. A pose that a card's file lacks (hungry, eating, love) is taken from here and put on its body. */

#define XN(n) X_##n
enum {
    X_neutral, X_happy, X_sleepy, X_asleep, X_curious_left, X_curious_right, X_surprised, X_sad, X_hungry,
    X_eating, X_eating_open, X_listening, X_thinking, X_speaking, X_love, X_calm, X_calm_left, X_calm_right,
    X_dozing, X_COUNT
};

#define E(hx, hy, hz, lw, lh, lx, ly, la, rw, rh, rx, ry, ra, sp, me, mb, cb)                                       \
    { { hx, hy, hz, lw, lh, lx, ly, la, rw, rh, rx, ry, ra, sp }, cb, 0, me, mb }

static av_xname_t BX[X_COUNT] = {
    [X_neutral] = { "neutral", E(0, 0, 0, 22, 44, 0, -4, 0, 22, 44, 0, -4, 0, 40, 0, 0, 0) },
    [X_happy] = { "happy", E(5, 0, 0, 26, 28, 0, -6, 10, 26, 28, 0, -6, -10, 46, 1, 0, 0) },
    [X_sleepy] = { "sleepy", E(-9, 0, 0, 30, 13, 0, 2, 7, 30, 13, 0, 2, -7, 42, 0, 1, 0) },
    [X_asleep] = { "asleep", E(-12, 0, 0, 34, 8, 0, 3, 0, 34, 8, 0, 3, 0, 44, 0, 1, 0) },
    [X_curious_left] = { "curious-left", E(0, -16, 5, 24, 52, 0, -4, 0, 18, 38, 0, -3, 0, 46, 1, 0, 0) },
    [X_curious_right] = { "curious-right", E(0, 16, -5, 18, 38, 0, -3, 0, 24, 52, 0, -4, 0, 46, 1, 0, 0) },
    [X_surprised] = { "surprised", E(4, 0, 0, 31, 52, 0, -5, 0, 31, 52, 0, -5, 0, 52, 0, 0, 0) },
    [X_sad] = { "sad", E(-9, 0, 0, 20, 40, 0, 3, 20, 20, 40, 0, 3, -20, 40, 0, 1, 0) },
    [X_hungry] = { "hungry", E(-5, 0, 0, 19, 32, 0, 3, 7, 19, 32, 0, 3, -7, 38, 0, 0, 0) },
    [X_eating] = { "eating", E(-3, 0, 0, 30, 10, 0, 2, 0, 30, 10, 0, 2, 0, 44, 0, 0, 0) },
    [X_eating_open] = { "eating-open", E(-3, 0, 0, 24, 22, 0, 0, 0, 24, 22, 0, 0, 0, 44, 0, 0, 0) },
    [X_listening] = { "listening", E(3, 0, -4, 25, 54, 0, -6, 0, 25, 54, 0, -6, 0, 43, 1, 0, 0) },
    [X_thinking] = { "thinking", E(7, 17, 4, 18, 34, 0, -7, 12, 18, 34, 0, -7, 12, 38, 1, 0, 0) },
    [X_speaking] = { "speaking", E(1, 0, 0, 24, 46, 0, -4, 0, 24, 46, 0, -4, 0, 42, 1, 0, 0) },
    [X_love] = { "love", E(-3, 0, 5, 36, 38, 0, 0, 0, 36, 38, 0, 0, 0, 52, 0, 1, 0x1000000u | 0xFF9FCF) },
    [X_calm] = { "calm", E(0, 0, 0, 20, 40, 0, -3, 0, 20, 40, 0, -3, 0, 42, 0, 0, 0) },
    [X_calm_left] = { "calm-left", E(0, -11, 2, 20, 40, 0, -3, 0, 20, 40, 0, -3, 0, 42, 0, 0, 0) },
    [X_calm_right] = { "calm-right", E(0, 11, -2, 20, 40, 0, -3, 0, 20, 40, 0, -3, 0, 42, 0, 0, 0) },
    [X_dozing] = { "dozing", E(-7, 0, 0, 28, 16, 0, 1, 5, 28, 16, 0, 1, -5, 42, 0, 0, 0) },
};

#define ST(x, hold, trans, kind) { X_##x, hold, trans, kind }
#define S_ AV_SMOOTH
#define P_ AV_SPRING
#define N_ AV_SNAPPY

static const av_step_t BS[] = {
    /* 0 idle */ ST(neutral, 3200, 500, S_), ST(curious_left, 2600, 500, S_), ST(neutral, 3000, 500, S_),
    ST(curious_right, 2600, 500, S_),
    /* 4 idle-quiet */ ST(calm, 9000, 900, S_), ST(calm_left, 7000, 900, S_), ST(calm, 9000, 900, S_),
    ST(calm_right, 7000, 900, S_), ST(dozing, 5000, 1200, S_), ST(calm, 6000, 900, S_),
    /* 10 listening */ ST(listening, 2400, 300, N_),
    /* 11 thinking */ ST(thinking, 2400, 400, S_),
    /* 12 working */ ST(speaking, 2400, 250, S_),
    /* 13 happy */ ST(happy, 2400, 300, P_),
    /* 14 confused */ ST(curious_left, 1300, 350, S_), ST(curious_right, 1300, 350, S_),
    /* 16 surprised */ ST(surprised, 1800, 200, N_),
    /* 17 sleeping */ ST(asleep, 8000, 1500, S_),
    /* 18 waking */ ST(asleep, 300, 200, S_), ST(sleepy, 800, 600, S_), ST(neutral, 500, 500, S_),
    /* 21 drowsy */ ST(sleepy, 4000, 1000, S_),
    /* 22 sad */ ST(sad, 3000, 700, S_),
    /* 23 hungry */ ST(hungry, 4200, 700, S_), ST(neutral, 2400, 700, S_), ST(hungry, 3600, 700, S_),
    /* 26 eating */ ST(eating_open, 220, 140, N_), ST(eating, 260, 120, N_), ST(eating_open, 220, 120, N_),
    ST(eating, 260, 120, N_), ST(eating_open, 220, 120, N_), ST(happy, 1200, 350, P_),
    /* 32 petted */ ST(happy, 1400, 260, P_), ST(love, 1400, 400, P_), ST(neutral, 600, 600, S_),
    /* 35 love */ ST(love, 2400, 350, P_), ST(neutral, 600, 600, S_),
    /* 37 fed */ ST(happy, 1200, 300, P_), ST(neutral, 600, 600, S_),
    /* 39 curious */ ST(curious_left, 2600, 500, S_), ST(curious_right, 2600, 500, S_),
};

#define AN(nm, mode, blink, i, mn, mx, dur, first, n) { nm, mode, blink, i, mn, mx, dur, first, n }
static av_anim_t BA[] = {
    AN("idle", AV_LOOP, true, 2600, 3400, 6200, 280, 0, 4),
    AN("idle-quiet", AV_LOOP, true, 3000, 5000, 11000, 320, 4, 6),
    AN("listening", AV_LOOP, true, 2500, 4000, 7000, 240, 10, 1),
    AN("thinking", AV_LOOP, true, 1800, 3000, 6000, 240, 11, 1),
    AN("working", AV_LOOP, true, 2500, 3500, 6500, 240, 12, 1),
    AN("happy", AV_LOOP, true, 1800, 2500, 5000, 240, 13, 1),
    AN("confused", AV_LOOP, true, 1800, 2800, 5000, 240, 14, 2),
    AN("surprised", AV_LOOP, false, 0, 4000, 8000, 240, 16, 1),
    AN("sleeping", AV_LOOP, false, 0, 4000, 8000, 300, 17, 1),
    AN("waking", AV_ONCE, true, 2600, 2000, 4000, 300, 18, 3),
    AN("drowsy", AV_LOOP, true, 1500, 2200, 4000, 420, 21, 1),
    AN("sad", AV_LOOP, true, 2000, 3500, 7000, 300, 22, 1),
    AN("hungry", AV_LOOP, true, 1500, 3000, 6000, 300, 23, 3),
    AN("eating", AV_ONCE, false, 0, 4000, 8000, 260, 26, 6),
    AN("petted", AV_ONCE, false, 0, 4000, 8000, 260, 32, 3),
    AN("love", AV_ONCE, true, 1500, 2500, 5000, 240, 35, 2),
    AN("fed", AV_ONCE, true, 1500, 2500, 5000, 240, 37, 2),
    AN("curious", AV_LOOP, true, 2000, 3000, 6000, 260, 39, 2),
};

static av_def_t BUILTIN = {
    .name = "built-in",
    .prim = { AV_SPHERE, 224, 200, 212, 1 },
    .nnodes = 0,
    .body = 0x8c7bf0, /* the accent replaces it */
    .eyes = 0x15121c,
    .nx = X_COUNT,
    .na = (int)(sizeof BA / sizeof BA[0]),
    .ns = (int)(sizeof BS / sizeof BS[0]),
    .x = BX,
    .a = BA,
    .st = (av_step_t *)BS,
};

static void prepare(av_def_t *d)
{
    d->sc = d->prim.w / 196.2f;
    d->rad = 0.5917f * cbrtf(d->prim.w * d->prim.h * d->prim.d);
    d->arc = 1.03f * d->rad;
}

const av_def_t *av_builtin(void)
{
    static bool done;
    if (!done) {
        done = true;
        prepare(&BUILTIN);
    }
    return &BUILTIN;
}

int av_def_expr(const av_def_t *d, const char *name)
{
    for (int i = 0; i < d->nx; i++)
        if (!strcmp(d->x[i].name, name)) return i;
    return -1;
}

int av_def_anim(const av_def_t *d, const char *name)
{
    for (int i = 0; i < d->na; i++)
        if (!strcmp(d->a[i].name, name)) return i;
    return -1;
}

void av_pose_of(const av_def_t *d, int i, uint32_t body, float P[AV_NP])
{
    const av_expr_t *e = &d->x[i].e;
    memcpy(P, e->v, sizeof e->v);
    uint32_t cb = e->cbody ? (e->cbody & 0xFFFFFF) : (body ? body : d->body);
    uint32_t ce = e->ceyes ? (e->ceyes & 0xFFFFFF) : d->eyes;
    P[P_BR] = (float)(cb >> 16 & 255);
    P[P_BG] = (float)(cb >> 8 & 255);
    P[P_BB] = (float)(cb & 255);
    P[P_ER] = (float)(ce >> 16 & 255);
    P[P_EG] = (float)(ce >> 8 & 255);
    P[P_EB] = (float)(ce & 255);
}

/* ================================================================== the file */

static bool hexcol(const jl_doc_t *D, int t, uint32_t *out)
{
    char s[16];
    if (t < 0 || !jl_str(D, t, s, sizeof s) || s[0] != '#' || strlen(s) != 7) return false;
    char *end;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (*end) return false;
    *out = (uint32_t)v;
    return true;
}

static bool surface(const jl_doc_t *D, int t, av_surf_t *s)
{
    static const char *const T[] = { "sphere", "mickey", "cursor", "cube", "capsule", "cylinder", "cone", "diamond" };
    if (t < 0) return false;
    char ty[24];
    if (!jl_str(D, jl_get(D, t, "type"), ty, sizeof ty)) return false;
    s->type = AV_SPHERE;
    for (int i = 0; i < 8; i++)
        if (!strcmp(ty, T[i])) s->type = (uint8_t)i;
    s->w = (float)jl_num(D, jl_get(D, t, "width"), 200);
    s->h = (float)jl_num(D, jl_get(D, t, "height"), 200);
    s->d = (float)jl_num(D, jl_get(D, t, "depth"), 200);
    s->round = (float)jl_num(D, jl_get(D, t, "roundness"), 1);
    if (s->w < 1) s->w = 1;
    if (s->h < 1) s->h = 1;
    if (s->d < 1) s->d = 1;
    return true;
}

static void copy_name(char *out, size_t n, const jl_doc_t *D, int t)
{
    if (!jl_str(D, t, out, n)) out[0] = 0;
}

static bool expression(const jl_doc_t *D, int t, av_expr_t *e)
{
    if (t < 0) return false;
    memset(e, 0, sizeof *e);
    int head = jl_get(D, t, "head"), eyes = jl_get(D, t, "eyes");
    if (head < 0 || eyes < 0) return false;
    e->v[P_HX] = (float)jl_num(D, jl_get(D, head, "x"), 0);
    e->v[P_HY] = (float)jl_num(D, jl_get(D, head, "y"), 0);
    e->v[P_HZ] = (float)jl_num(D, jl_get(D, head, "z"), 0);
    for (int s = 0; s < 2; s++) {
        int o = jl_get(D, eyes, s ? "right" : "left");
        if (o < 0) return false;
        float *v = e->v + (s ? P_RW : P_LW);
        v[0] = (float)jl_num(D, jl_get(D, o, "width"), 20);
        v[1] = (float)jl_num(D, jl_get(D, o, "height"), 50);
        v[2] = (float)jl_num(D, jl_get(D, o, "x"), 0);
        v[3] = (float)jl_num(D, jl_get(D, o, "y"), 0);
        v[4] = (float)jl_num(D, jl_get(D, o, "angle"), 0);
    }
    e->v[P_SP] = (float)jl_num(D, jl_get(D, eyes, "spacing"), 35);
    int m = jl_get(D, t, "motion");
    if (m >= 0) {
        e->meye = jl_eq(D, jl_get(D, m, "eyes"), "microSaccades") ? 1 : jl_eq(D, jl_get(D, m, "eyes"), "shake") ? 2 : 0;
        e->mbody = jl_eq(D, jl_get(D, m, "body"), "slowDrift") ? 1 : jl_eq(D, jl_get(D, m, "body"), "shake") ? 2 : 0;
    }
    int c = jl_get(D, t, "colors");
    if (c >= 0) {
        uint32_t v;
        if (hexcol(D, jl_get(D, c, "body"), &v)) e->cbody = 0x1000000u | v;
        if (hexcol(D, jl_get(D, c, "eyes"), &v)) e->ceyes = 0x1000000u | v;
    }
    return true;
}

static uint16_t u16(double v, double lo, double hi)
{
    return (uint16_t)(v < lo ? lo : v > hi ? hi : v);
}

av_def_t *av_def_parse(const char *js, size_t len, char *why, size_t wn)
{
#define FAIL(...) do { snprintf(why, wn, __VA_ARGS__); goto fail; } while (0)
    jl_tok_t *tk = NULL;
    av_def_t *d = NULL;
    int n = -1;
    for (size_t div = 5; n < 0 && div >= 2; div--) {
        free(tk);
        size_t cap = len / div + 256;
        tk = malloc(cap * sizeof *tk);
        if (!tk) FAIL("out of memory for %u KB of JSON", (unsigned)(len >> 10));
        n = jl_parse(js, len, tk, (int)cap);
    }
    if (n < 1 || tk[0].type != JL_OBJ) FAIL("not a JSON object");
    jl_doc_t D = { js, tk, n };
    char s[64];
    if (!jl_str(&D, jl_get(&D, 0, "schema"), s, sizeof s) || strcmp(s, "bible-strong/avatar-definition"))
        FAIL("not a bible-strong/avatar-definition");
    if (jl_num(&D, jl_get(&D, 0, "schemaVersion"), 0) != 1) FAIL("schemaVersion is not 1");
    d = calloc(1, sizeof *d);
    if (!d) FAIL("out of memory");
    d->heap = true;
    copy_name(d->name, sizeof d->name, &D, jl_get(&D, 0, "name"));
    int body = jl_get(&D, 0, "body");
    if (body < 0 || !surface(&D, jl_get(&D, body, "primary"), &d->prim)) FAIL("no body.primary");
    int nodes = jl_get(&D, body, "nodes");
    for (int i = 0; nodes >= 0 && i < AV_MAX_NODES; i++) {
        int t = jl_at(&D, nodes, i);
        if (t < 0) break;
        av_node_t *nd = &d->node[d->nnodes];
        int pos = jl_get(&D, t, "position");
        if (!surface(&D, jl_get(&D, t, "surface"), &nd->s) || pos < 0) continue;
        for (int k = 0; k < 3; k++) nd->pos[k] = (float)jl_num(&D, jl_at(&D, pos, k), 0);
        d->nnodes++;
    }
    int col = jl_get(&D, 0, "colors");
    if (col < 0 || !hexcol(&D, jl_get(&D, col, "body"), &d->body) || !hexcol(&D, jl_get(&D, col, "eyes"), &d->eyes))
        FAIL("no colors.body / colors.eyes");

    /* expressions, in the file's order (expressionOrder lists them; the object's own order is the same) */
    int ex = jl_get(&D, 0, "expressions");
    if (ex < 0 || tk[ex].type != JL_OBJ || tk[ex].size < 1) FAIL("no expressions");
    int nx = tk[ex].size > 128 ? 128 : tk[ex].size;
    d->x = calloc((size_t)nx, sizeof *d->x);
    if (!d->x) FAIL("out of memory");
    {
        int t = ex + 1; /* key, value, key, value... */
        for (int i = 0; i < tk[ex].size && d->nx < nx; i++) {
            int key = t, val = t + 1;
            copy_name(d->x[d->nx].name, sizeof d->x[d->nx].name, &D, key);
            if (expression(&D, val, &d->x[d->nx].e)) d->nx++;
            t = tk[val].skip;
        }
    }
    if (av_def_expr(d, "neutral") < 0) FAIL("no \"neutral\" expression");

    int an = jl_get(&D, 0, "animations");
    int total = 0;
    if (an >= 0 && tk[an].type == JL_OBJ) {
        int t = an + 1, na = tk[an].size > 64 ? 64 : tk[an].size;
        /* first pass: count the steps */
        for (int i = 0; i < tk[an].size; i++) {
            int val = t + 1;
            int st = jl_get(&D, val, "steps");
            if (st >= 0) total += tk[st].size > 128 ? 128 : tk[st].size;
            t = tk[val].skip;
        }
        d->a = calloc((size_t)(na ? na : 1), sizeof *d->a);
        d->st = calloc((size_t)(total ? total : 1), sizeof *d->st);
        if (!d->a || !d->st) FAIL("out of memory");
        t = an + 1;
        for (int i = 0; i < tk[an].size && d->na < na; i++) {
            int key = t, val = t + 1;
            t = tk[val].skip;
            av_anim_t *a = &d->a[d->na];
            copy_name(a->name, sizeof a->name, &D, key);
            a->mode = jl_eq(&D, jl_get(&D, val, "playbackMode"), "once") ? AV_ONCE
                      : jl_eq(&D, jl_get(&D, val, "playbackMode"), "pingPong") ? AV_PINGPONG : AV_LOOP;
            int bl = jl_get(&D, val, "blink");
            if (bl >= 0) {
                a->blink = jl_bool(&D, jl_get(&D, bl, "enabled"), false);
                a->b_init = u16(jl_num(&D, jl_get(&D, bl, "initialDelayMs"), 2000), 0, 60000);
                a->b_min = u16(jl_num(&D, jl_get(&D, bl, "minIntervalMs"), 3000), 250, 60000);
                a->b_max = u16(jl_num(&D, jl_get(&D, bl, "maxIntervalMs"), 6000), 250, 60000);
                a->b_dur = u16(jl_num(&D, jl_get(&D, bl, "durationMs"), 280), 50, 2000);
                if (a->b_max < a->b_min) a->b_max = a->b_min;
            }
            int st = jl_get(&D, val, "steps");
            a->first = (uint16_t)d->ns;
            for (int k = 0; st >= 0 && k < 128; k++) {
                int s1 = jl_at(&D, st, k);
                if (s1 < 0) break;
                char xn[64];
                copy_name(xn, sizeof xn, &D, jl_get(&D, s1, "expression"));
                xn[31] = 0; /* names are kept to 31 characters */
                int xi = av_def_expr(d, xn);
                if (xi < 0) continue;
                av_step_t *p = &d->st[d->ns++];
                p->expr = (uint16_t)xi;
                p->hold = u16(jl_num(&D, jl_get(&D, s1, "holdMs"), 1000), 100, 60000);
                p->trans = u16(jl_num(&D, jl_get(&D, s1, "transitionMs"), 400), 0, 5000);
                p->kind = jl_eq(&D, jl_get(&D, s1, "transition"), "spring") ? AV_SPRING
                          : jl_eq(&D, jl_get(&D, s1, "transition"), "snappy") ? AV_SNAPPY : AV_SMOOTH;
            }
            a->n = (uint16_t)(d->ns - a->first);
            if (a->n) d->na++;
        }
    }
    prepare(d);
    free(tk);
    return d;
fail:
    free(tk);
    av_def_free(d);
    return NULL;
#undef FAIL
}

void av_def_free(av_def_t *d)
{
    if (!d || !d->heap) return;
    free(d->x);
    free(d->a);
    free(d->st);
    free(d);
}

/* ================================================================== geometry */

static void turn(float *x, float *y, float *z, float rx, float ry, float rz)
{
    float c = cosf(ry), s = sinf(ry), t;
    t = *x * c + *z * s;
    *z = -*x * s + *z * c;
    *x = t;
    c = cosf(rx);
    s = sinf(rx);
    t = *y * c - *z * s;
    *z = *y * s + *z * c;
    *y = t;
    c = cosf(rz);
    s = sinf(rz);
    t = *x * c - *y * s;
    *y = *x * s + *y * c;
    *x = t;
}

void av_geom(const av_def_t *d, const float P[AV_NP], av_eye_t eye[2], av_ear_t ear[AV_MAX_NODES])
{
    const float D2R = 0.017453293f;
    float hx = P[P_HX] * D2R, hy = P[P_HY] * D2R, hz = P[P_HZ] * D2R;
    for (int i = 0; i < 2; i++) {
        const float *v = P + (i ? P_RW : P_LW);
        float w = v[0] * d->sc, h = v[1] * d->sc;
        float fx = ((i ? 1.0f : -1.0f) * P[P_SP] * 0.5f + v[2]) * d->sc, fy = v[3] * d->sc;
        float lon = fx / d->arc, lat = fy / d->arc, a = v[4] * D2R;
        if (lon > 1.3f) lon = 1.3f;
        if (lon < -1.3f) lon = -1.3f;
        if (lat > 1.3f) lat = 1.3f;
        if (lat < -1.3f) lat = -1.3f;
        float cl = cosf(lat), sl = sinf(lat), co = cosf(lon), so = sinf(lon), R = d->rad;
        float px = R * cl * so, py = R * sl, pz = R * cl * co;
        /* the surface's own axes there: east along the longitude, north (down the screen) along the latitude */
        float ex = co, ey = 0, ez = -so;
        float nx = -sl * so, ny = cl, nz = -sl * co;
        float ca = cosf(a), sa = sinf(a);
        float hax = -ca * nx + sa * ex, hay = -ca * ny + sa * ey, haz = -ca * nz + sa * ez; /* the bar's length axis */
        float wax = ca * ex + sa * nx, way = ca * ey + sa * ny, waz = ca * ez + sa * nz;
        turn(&px, &py, &pz, hx, hy, hz);
        turn(&hax, &hay, &haz, hx, hy, hz);
        turn(&wax, &way, &waz, hx, hy, hz);
        float hl = sqrtf(hax * hax + hay * hay), wl = sqrtf(wax * wax + way * way);
        float H = h * hl, W = w * wl;
        av_eye_t *o = &eye[i];
        o->cx = px;
        o->cy = py;
        if (H >= W) {
            o->len = H;
            o->wid = W;
            o->dx = hl > 1e-4f ? hax / hl : 0;
            o->dy = hl > 1e-4f ? hay / hl : 1;
        } else {
            o->len = W;
            o->wid = H;
            o->dx = wl > 1e-4f ? wax / wl : 1;
            o->dy = wl > 1e-4f ? way / wl : 0;
        }
        if (o->wid < 1) o->wid = 1;
        if (o->len < o->wid) o->len = o->wid;
    }
    for (int i = 0; i < d->nnodes; i++) {
        float x = d->node[i].pos[0], y = d->node[i].pos[1], z = d->node[i].pos[2];
        turn(&x, &y, &z, hx, hy, hz);
        ear[i].x = x;
        ear[i].y = y;
        ear[i].z = z;
    }
}

/* ================================================================== shapes */

static float sd_ellipse(float hw, float hh, float x, float y)
{
    float px = x / hw, py = y / hh;
    float k0 = sqrtf(px * px + py * py);
    if (k0 < 1e-6f) return -(hw < hh ? hw : hh);
    float qx = x / (hw * hw), qy = y / (hh * hh);
    float k1 = sqrtf(qx * qx + qy * qy);
    return k0 * (k0 - 1.0f) / (k1 > 1e-9f ? k1 : 1e-9f);
}

static float sd_box(float hw, float hh, float r, float x, float y)
{
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = fabsf(x) - (hw - r), qy = fabsf(y) - (hh - r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - r;
}

float av_sdf(const av_surf_t *s, float hw, float hh, float x, float y)
{
    float m = hw < hh ? hw : hh;
    switch (s->type) {
    case AV_CUBE: return sd_box(hw, hh, 0.45f * s->round * m, x, y);
    case AV_CAPSULE: return sd_box(hw, hh, m, x, y);
    case AV_CYLINDER: return sd_box(hw, hh, 0.3f * (s->round > 0.2f ? s->round : 0.2f) * m, x, y);
    case AV_DIAMOND: {
        float d = (fabsf(x) / hw + fabsf(y) / hh - 1.0f) / sqrtf(1.0f / (hw * hw) + 1.0f / (hh * hh));
        return d - 0.0f;
    }
    case AV_CONE: {
        /* a rounded triangle, apex up: the sides lean in from the top */
        float u = (y + hh) / (2 * hh); /* 0 at the apex, 1 at the base */
        float half = hw * u;
        float dx = fabsf(x) - half, db = y - hh;
        float side = dx * 0.9f;
        float d = side > db ? side : db;
        if (u < 0) d = -u * hh + (fabsf(x) > 0 ? fabsf(x) : 0);
        return d;
    }
    case AV_MICKEY: {
        /* a head and two round ears at its upper corners */
        float h = sd_ellipse(hw * 0.78f, hh * 0.78f, x, y + hh * 0.12f);
        float er = hw * 0.34f;
        float e1 = sqrtf((x - hw * 0.62f) * (x - hw * 0.62f) + (y + hh * 0.62f) * (y + hh * 0.62f)) - er;
        float e2 = sqrtf((x + hw * 0.62f) * (x + hw * 0.62f) + (y + hh * 0.62f) * (y + hh * 0.62f)) - er;
        float d = h < e1 ? h : e1;
        return d < e2 ? d : e2;
    }
    default: return sd_ellipse(hw, hh, x, y);
    }
}
