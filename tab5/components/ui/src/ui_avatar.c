/* ui_avatar — see ui_avatar.h. */
#include "ui_avatar.h"

#include "bz_motion.h"
#include "bz_theme.h"
#include "bz_ui.h"

#include "src/misc/cache/instance/lv_image_cache.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BIG_FREE(p) heap_caps_free(p)
#else
#define BIG_ALLOC(n) malloc(n)
#define BIG_FREE(p) free(p)
#endif

/* ================================================================== what the app means, in one table
 *
 * For each meaning: the animations that say it, then the expressions that come nearest, in the character's
 * own names (Strobi's and Tabi's). A character that has none of them gets the built-in pose of that name put
 * on its own body and colours. */
static const struct {
    const char *name;
    const char *anim[3];
    const char *expr[4];
} MAP[AV_S_COUNT] = {
    [AV_S_IDLE] = { "idle", { "idle" }, { "neutral" } },
    [AV_S_QUIET] = { "quiet", { "idle-quiet", "idle" }, { "calm", "neutral" } },
    [AV_S_LISTEN] = { "listen", { "listening" }, { "attentive-left", "small-attentive" } },
    [AV_S_THINK] = { "think", { "thinking" }, { "upward-side-glance" } },
    [AV_S_SPEAK] = { "speak", { "speaking", "working" }, { "neutral" } },
    [AV_S_HAPPY] = { "happy", { "happy" }, { "joyful-wide" } },
    [AV_S_CONFUSED] = { "confused", { "confused" }, { "skeptical-right" } },
    [AV_S_SURPRISED] = { "surprised", { "surprised" }, { "surprised-wide-left", "surprised-left" } },
    [AV_S_SLEEP] = { "sleep", { "sleeping" }, { "eyes-closed", "drowsy-closed" } },
    [AV_S_WAKE] = { "wake", { "waking" }, { "small-attentive" } },
    [AV_S_DROWSY] = { "drowsy", { "drowsy" }, { "sleepy-squint" } },
    [AV_S_SAD] = { "sad", { "sad" }, { "shy-downward", "downward-gaze" } },
    [AV_S_HUNGRY] = { "hungry", { "hungry" }, { "hungry", "shy-downward" } },
    [AV_S_EAT] = { "eat", { "eating" }, { "eating" } },
    [AV_S_PETTED] = { "petted", { "petted" }, { "petted", "love", "joyful-wide" } },
    [AV_S_LOVE] = { "love", { "love" }, { "love", "joyful-wide" } },
    [AV_S_FED] = { "fed", { "fed" }, { "joyful-wide" } },
    [AV_S_CURIOUS] = { "curious", { "curious" }, { "curious-left" } },
};
const char *const AV_STATE_NAME[AV_S_COUNT] = { "idle", "quiet", "listen", "think", "speak", "happy", "confused",
                                                "surprised", "sleep", "wake", "drowsy", "sad", "hungry", "eat",
                                                "petted", "love", "fed", "curious" };

/* ================================================================== the character (shared) */

static av_def_t *g_loaded;
static uint32_t g_gen = 1;
static long g_mtime = -1, g_size = -1;

static const av_def_t *the_def(void) { return g_loaded ? g_loaded : av_builtin(); }

/* ================================================================== the baked body */

typedef struct {
    bool on;
    int refs, S;
    uint32_t gen;
    lv_image_dsc_t head, light, ear[4];
    uint8_t *hp, *lp, *ep[4];
    int nmask;
    uint8_t node_mask[AV_MAX_NODES]; /* node -> mask, or 255 (none) */
    av_surf_t mask_of[4];
    size_t bytes;
} bake_t;
static bake_t BK;

static void set_dsc(lv_image_dsc_t *d, uint8_t *px, int w, int h)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_A8;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w;
    d->data_size = (uint32_t)w * (uint32_t)h;
    d->data = px;
}

static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* A surface's silhouette as an alpha mask, one pixel of antialiasing, at k px a unit. With `light`, the
 * alpha is also scaled by a soft glow toward the upper left and only the upper-left part is kept. */
static uint8_t *bake_mask(const av_surf_t *s, float k, int *wp, int *hp, bool light, int *lw, int *lh)
{
    float hw = s->w * 0.5f * 1.0185f, hh = s->h * 0.5f * 1.0185f; /* the format draws a unit sphere 1.8 % big */
    int w = (int)ceilf(2 * hw * k) + 2, h = (int)ceilf(2 * hh * k) + 2;
    int ow = light ? (int)(w * 0.9f) : w, oh = light ? (int)(h * 0.75f) : h;
    uint8_t *px = BIG_ALLOC((size_t)ow * oh);
    if (!px) return NULL;
    for (int y = 0; y < oh; y++) {
        float uy = ((float)y + 0.5f - h * 0.5f) / k;
        for (int x = 0; x < ow; x++) {
            float ux = ((float)x + 0.5f - w * 0.5f) / k;
            float d = av_sdf(s, hw, hh, ux, uy);
            float cov = sat(0.5f - d * k);
            if (light && cov > 0) {
                float dx = ux + 0.22f * s->w, dy = uy + 0.34f * s->h;
                float t = 1.0f - sqrtf(dx * dx + dy * dy) / (0.50f * s->w);
                t = sat(t);
                /* the panel shows 565, so a light this faint would come out as one or two flat steps: an ordered
                 * dither in the mask (about one colour step either way) turns the contours into grain */
                static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
                float lit = t * t + ((BAYER[((y & 3) << 2) | (x & 3)] - 7.5f) / 16.0f) * 0.30f * (t > 0 ? 1.0f : 0.0f);
                cov *= sat(lit);
            }
            px[(size_t)y * ow + x] = (uint8_t)(cov * 255.0f + 0.5f);
        }
    }
    *wp = ow;
    *hp = oh;
    if (lw) *lw = w;
    if (lh) *lh = h;
    return px;
}

static void bake_free(void)
{
    if (!BK.on) return;
    lv_image_cache_drop(&BK.head);
    lv_image_cache_drop(&BK.light);
    BIG_FREE(BK.hp);
    BIG_FREE(BK.lp);
    for (int i = 0; i < BK.nmask; i++) {
        lv_image_cache_drop(&BK.ear[i]);
        BIG_FREE(BK.ep[i]);
    }
    memset(&BK, 0, sizeof BK);
}

static bool bake_take(int S)
{
    if (BK.on && BK.S == S && BK.gen == g_gen) {
        BK.refs++;
        return true;
    }
    if (BK.on && BK.refs > 0) return false; /* another avatar, another size, is showing */
    bake_free();
    const av_def_t *d = the_def();
    float k = (float)S / 300.0f;
    int w, h;
    BK.hp = bake_mask(&d->prim, k, &w, &h, false, NULL, NULL);
    if (!BK.hp) return false;
    set_dsc(&BK.head, BK.hp, w, h);
    BK.bytes = (size_t)w * h;
    int lw, lh, fw, fh;
    BK.lp = bake_mask(&d->prim, k, &lw, &lh, true, &fw, &fh);
    if (BK.lp) {
        set_dsc(&BK.light, BK.lp, lw, lh);
        BK.bytes += (size_t)lw * lh;
    }
    /* the nodes: one mask for each different surface, four at most */
    memset(BK.node_mask, 255, sizeof BK.node_mask);
    for (int i = 0; i < d->nnodes; i++) {
        const av_surf_t *s = &d->node[i].s;
        int m = -1;
        for (int j = 0; j < BK.nmask; j++)
            if (!memcmp(&BK.mask_of[j], s, sizeof *s)) m = j;
        if (m < 0 && BK.nmask < 4) {
            int nw, nh;
            uint8_t *px = bake_mask(s, k, &nw, &nh, false, NULL, NULL);
            if (px) {
                m = BK.nmask++;
                BK.ep[m] = px;
                BK.mask_of[m] = *s;
                set_dsc(&BK.ear[m], px, nw, nh);
                BK.bytes += (size_t)nw * nh;
            }
        }
        if (m >= 0) BK.node_mask[i] = (uint8_t)m;
    }
    BK.on = true;
    BK.S = S;
    BK.gen = g_gen;
    BK.refs = 1;
    return true;
}

static void bake_give(void)
{
    if (BK.refs > 0) BK.refs--;
    if (BK.refs == 0) bake_free();
}

size_t ui_avatar_bytes(void) { return BK.on ? BK.bytes : 0; }

/* ================================================================== the instance */

typedef struct {
    int16_t ex[2][6];  /* each eye: cx cy (half px), dx dy (1/64), len wid (half px) */
    int16_t ear[AV_MAX_NODES][3];
    uint32_t body, eyes;
    uint8_t blink;
} drawn_t;

struct ui_avatar {
    lv_obj_t *obj;
    int S;
    float k;
    bool active, baked;
    uint32_t gen;
    double last;
    uint32_t rng;
    /* which character animation is playing */
    const av_def_t *adef;
    const av_anim_t *an;
    const av_step_t *steps;
    av_anim_t syn;
    av_step_t synstep;
    int step, dir;
    double step_t0;
    av_state_t base;
    bool reacting, forced;
    float from[AV_NP], to[AV_NP], P[AV_NP];
    bz_motion_t s;
    /* ambient */
    double blink_at, blink_t0;
    float blink_dur;
    float ox, oy, tox, toy;
    double sacc_at;
    float calm, level, tint_k;
    uint32_t tint;
    float lookx, looky, lx, ly;
    double look_until;
    bool moving;
    bool has_pose;
    drawn_t dr;
    bool dr_ok;
};

static float rnd(ui_avatar_t *a)
{
    a->rng = a->rng * 1664525u + 1013904223u;
    return (float)(a->rng >> 8) / 16777216.0f;
}

static bz_spring_t spring_for(int kind, int ms)
{
    float T = (float)ms / 1000.0f;
    if (kind == AV_SNAPPY) T *= 0.8f;
    if (T < 0.05f) T = 0.05f;
    float z = kind == AV_SPRING ? 0.45f : kind == AV_SNAPPY ? 0.8f : 1.0f;
    float w = 6.2f / (z * T);
    return (bz_spring_t){ w * w, 2 * z * w, 1 };
}

static uint32_t body_base(ui_avatar_t *a)
{
    if (a->tint) return a->tint;
    if (!g_loaded) return bz_color(BZ_C_ICE) & 0xFFFFFF;
    return the_def()->body;
}

/* the pose of expression xi of the animation's own definition, wearing this character's colours */
static void pose_for(ui_avatar_t *a, int xi, float P[AV_NP])
{
    const av_expr_t *e = &a->adef->x[xi].e;
    av_pose_of(a->adef, xi, body_base(a), P);
    const av_def_t *me = the_def();
    if (a->adef != me) {
        /* a pose borrowed from the built-in set: the body and eyes are this character's, unless the pose
         * carries its own colour (love's blush) */
        if (!e->cbody) {
            uint32_t b = body_base(a);
            P[P_BR] = (float)(b >> 16 & 255);
            P[P_BG] = (float)(b >> 8 & 255);
            P[P_BB] = (float)(b & 255);
        }
        if (!e->ceyes) {
            P[P_ER] = (float)(me->eyes >> 16 & 255);
            P[P_EG] = (float)(me->eyes >> 8 & 255);
            P[P_EB] = (float)(me->eyes & 255);
        }
    }
}

static void begin_step(ui_avatar_t *a, int i, double now)
{
    const av_step_t *st = &a->steps[i];
    a->step = i;
    a->step_t0 = now;
    memcpy(a->from, a->has_pose ? a->P : a->to, sizeof a->from);
    pose_for(a, st->expr, a->to);
    if (!a->has_pose) {
        memcpy(a->from, a->to, sizeof a->from);
        a->has_pose = true;
        bz_motion_init(&a->s, 1, 0.002f);
    } else if (st->trans == 0) {
        bz_motion_init(&a->s, 1, 0.002f);
    } else {
        bz_motion_init(&a->s, 0, 0.002f);
        bz_motion_to(&a->s, 1, spring_for(st->kind, st->trans));
    }
    a->s.keep = true;
}

static void schedule_blink(ui_avatar_t *a, double now, bool first)
{
    a->blink_t0 = 0;
    if (!a->an || !a->an->blink) {
        a->blink_at = 0;
        return;
    }
    float lo = a->an->b_min / 1000.0f, hi = a->an->b_max / 1000.0f;
    a->blink_dur = a->an->b_dur / 1000.0f;
    a->blink_at = now + (first ? a->an->b_init / 1000.0 : lo + (hi - lo) * rnd(a));
}

static void play(ui_avatar_t *a, const av_def_t *d, const av_anim_t *an, const av_step_t *steps, bool once)
{
    double now = bz_motion_now();
    a->adef = d;
    a->an = an;
    a->steps = steps;
    a->dir = 1;
    a->forced = false;
    a->reacting = once;
    begin_step(a, 0, now);
    schedule_blink(a, now, true);
}

/* One of the app's meanings, resolved against the character, then ours */
static bool resolve(ui_avatar_t *a, av_state_t st, bool once)
{
    const av_def_t *me = the_def(), *own = av_builtin();
    for (int pass = 0; pass < 2; pass++) {
        const av_def_t *d = pass ? own : me;
        for (int i = 0; i < 3 && MAP[st].anim[i]; i++) {
            int ai = av_def_anim(d, MAP[st].anim[i]);
            if (ai >= 0) {
                play(a, d, &d->a[ai], d->st + d->a[ai].first, once);
                return true;
            }
        }
        for (int i = 0; i < 4 && MAP[st].expr[i]; i++) {
            int xi = av_def_expr(d, MAP[st].expr[i]);
            if (xi >= 0) {
                /* an expression held: one step that never ends, blinking now and then */
                memset(&a->syn, 0, sizeof a->syn);
                a->syn.mode = once ? AV_ONCE : AV_LOOP;
                a->syn.blink = st != AV_S_SLEEP;
                a->syn.b_init = 2500;
                a->syn.b_min = 3200;
                a->syn.b_max = 6500;
                a->syn.b_dur = 280;
                a->syn.n = 1;
                a->synstep = (av_step_t){ (uint16_t)xi, once ? 1800 : 8000, 450, AV_SMOOTH };
                play(a, d, &a->syn, &a->synstep, once);
                return true;
            }
        }
    }
    return false;
}

void ui_avatar_state(ui_avatar_t *a, av_state_t st)
{
    if (st < 0 || st >= AV_S_COUNT) return;
    if (a->base == st && a->an && !a->reacting && !a->forced) return;
    a->base = st;
    resolve(a, st, false);
}

void ui_avatar_react(ui_avatar_t *a, av_state_t st)
{
    if (st < 0 || st >= AV_S_COUNT) return;
    resolve(a, st, true);
}

bool ui_avatar_reacting(ui_avatar_t *a) { return a->reacting; }

bool ui_avatar_show_expr(ui_avatar_t *a, const char *name)
{
    if (!name) {
        a->forced = false;
        resolve(a, a->base, false);
        return true;
    }
    const av_def_t *me = the_def();
    int xi = av_def_expr(me, name);
    const av_def_t *d = me;
    if (xi < 0) {
        d = av_builtin();
        xi = av_def_expr(d, name);
    }
    if (xi < 0) return false;
    memset(&a->syn, 0, sizeof a->syn);
    a->syn.n = 1;
    a->synstep = (av_step_t){ (uint16_t)xi, 60000, 250, AV_SMOOTH };
    play(a, d, &a->syn, &a->synstep, false);
    a->forced = true;
    a->blink_at = 0;
    return true;
}

void ui_avatar_look(ui_avatar_t *a, float x, float y, float hold)
{
    a->lookx = x < -1 ? -1 : x > 1 ? 1 : x;
    a->looky = y < -1 ? -1 : y > 1 ? 1 : y;
    a->look_until = bz_motion_now() + hold;
}

void ui_avatar_level(ui_avatar_t *a, float v) { a->level = v < 0 ? 0 : v > 1 ? 1 : v; }
void ui_avatar_calm(ui_avatar_t *a, float c) { a->calm = c < 0 ? 0 : c > 1 ? 1 : c; }
void ui_avatar_tint(ui_avatar_t *a, uint32_t rgb) { a->tint = rgb & 0xFFFFFF; }
lv_obj_t *ui_avatar_obj(ui_avatar_t *a) { return a->obj; }

/* ================================================================== drawing */

static void eye_ends(const drawn_t *d, int i, int *x1, int *y1, int *x2, int *y2, int *wid)
{
    const int16_t *e = d->ex[i];
    float dx = e[2] / 64.0f, dy = e[3] / 64.0f, len = e[4] / 2.0f, w = e[5] / 2.0f;
    float h = (len - w) * 0.5f;
    *x1 = (int)lroundf(e[0] / 2.0f - dx * h);
    *y1 = (int)lroundf(e[1] / 2.0f - dy * h);
    *x2 = (int)lroundf(e[0] / 2.0f + dx * h);
    *y2 = (int)lroundf(e[1] / 2.0f + dy * h);
    *wid = (int)lroundf(e[5] / 4.0f); /* the radius */
}

static void av_draw(lv_event_t *e)
{
    ui_avatar_t *a = lv_event_get_user_data(e);
    if (!a->baked || !a->dr_ok) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(a->obj, &co);
    int cx = co.x1 + a->S / 2, cy = co.y1 + a->S / 2;
    const drawn_t *d = &a->dr;
    lv_color_t body = bz_lv_rgb(d->body);
    const av_def_t *def = the_def();

    lv_draw_image_dsc_t id;
    lv_draw_image_dsc_init(&id);
    id.recolor = body;
    id.recolor_opa = LV_OPA_COVER;
    id.opa = LV_OPA_COVER;
    /* nodes behind the head, the head, its light, the nodes in front of it */
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) {
            id.src = &BK.head;
            lv_area_t h = { cx - (int)BK.head.header.w / 2, cy - (int)BK.head.header.h / 2, 0, 0 };
            h.x2 = h.x1 + (int)BK.head.header.w - 1;
            h.y2 = h.y1 + (int)BK.head.header.h - 1;
            lv_draw_image(layer, &id, &h);
            if (BK.lp) {
                lv_draw_image_dsc_t li;
                lv_draw_image_dsc_init(&li);
                li.src = &BK.light;
                li.recolor = lv_color_white();
                li.recolor_opa = LV_OPA_COVER;
                li.opa = 80;
                lv_area_t l = { h.x1, h.y1, h.x1 + (int)BK.light.header.w - 1, h.y1 + (int)BK.light.header.h - 1 };
                lv_draw_image(layer, &li, &l);
            }
            continue;
        }
        for (int i = 0; i < def->nnodes; i++) {
            bool front = d->ear[i][2] >= 0;
            if ((pass == 2) != front || BK.node_mask[i] == 255) continue;
            const lv_image_dsc_t *m = &BK.ear[BK.node_mask[i]];
            id.src = m;
            lv_area_t r = { cx + d->ear[i][0] - (int)m->header.w / 2, cy + d->ear[i][1] - (int)m->header.h / 2, 0, 0 };
            r.x2 = r.x1 + (int)m->header.w - 1;
            r.y2 = r.y1 + (int)m->header.h - 1;
            lv_draw_image(layer, &id, &r);
        }
    }
    lv_color_t ec = bz_lv_rgb(d->eyes);
    for (int i = 0; i < 2; i++) {
        int x1, y1, x2, y2, w;
        eye_ends(d, i, &x1, &y1, &x2, &y2, &w);
        float len = d->ex[i][4] / 2.0f, wd = d->ex[i][5] / 2.0f;
        if (w < 1) w = 1;
        if (d->blink > 8) {
            /* a blink: the eye squashed flat, drawn as a rounded rectangle across what it covered */
            float q = 1.0f - 0.9f * d->blink / 255.0f;
            int bw = (int)lroundf(fabsf((x2 - x1)) + 2 * w), bh = (int)lroundf((fabsf((float)(y2 - y1)) + 2 * w) * q);
            if (bh < 4) bh = 4;
            lv_draw_rect_dsc_t r;
            lv_draw_rect_dsc_init(&r);
            r.bg_color = ec;
            r.bg_opa = LV_OPA_COVER;
            r.radius = bh / 2 < bw / 2 ? bh / 2 : bw / 2;
            int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
            lv_area_t b = { cx + mx - bw / 2, cy + my - bh / 2, cx + mx + bw / 2, cy + my + bh / 2 };
            lv_draw_rect(layer, &r, &b);
        } else if (len - wd < 1.5f || abs(x2 - x1) <= 2) {
            /* a round eye, or an upright bar: a rounded rectangle is cheaper and crisper than a line */
            lv_draw_rect_dsc_t r;
            lv_draw_rect_dsc_init(&r);
            r.bg_color = ec;
            r.bg_opa = LV_OPA_COVER;
            r.radius = w;
            int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
            int hh = (int)lroundf(len * 0.5f);
            lv_area_t b = { cx + mx - w, cy + my - hh, cx + mx + w - 1, cy + my + hh - 1 };
            if (len - wd < 1.5f) {
                b.y1 = cy + my - w;
                b.y2 = cy + my + w - 1;
            }
            lv_draw_rect(layer, &r, &b);
        } else {
            /* a slanted bar: a straight line for the body and a circle on each end (the line's own round caps
             * come out beaded at this width) */
            float ddx = (float)(x2 - x1), ddy = (float)(y2 - y1), dl = sqrtf(ddx * ddx + ddy * ddy);
            float nx = -ddy / dl * w, ny = ddx / dl * w;
            lv_draw_triangle_dsc_t t;
            lv_draw_triangle_dsc_init(&t);
            t.color = ec;
            t.opa = LV_OPA_COVER;
            /* the bar's corners A B (at the first end) and C D (at the second); two triangles on each diagonal, so
             * the hairline either pair leaves along its own diagonal is covered by the other pair's inside */
            lv_value_precise_t Ax = (lv_value_precise_t)(cx + x1 + nx), Ay = (lv_value_precise_t)(cy + y1 + ny);
            lv_value_precise_t Bx = (lv_value_precise_t)(cx + x1 - nx), By = (lv_value_precise_t)(cy + y1 - ny);
            lv_value_precise_t Cx = (lv_value_precise_t)(cx + x2 + nx), Cy = (lv_value_precise_t)(cy + y2 + ny);
            lv_value_precise_t Dx = (lv_value_precise_t)(cx + x2 - nx), Dy = (lv_value_precise_t)(cy + y2 - ny);
            lv_value_precise_t tri[4][6] = { { Ax, Ay, Bx, By, Cx, Cy }, { Bx, By, Dx, Dy, Cx, Cy },
                                             { Ax, Ay, Bx, By, Dx, Dy }, { Ax, Ay, Dx, Dy, Cx, Cy } };
            for (int q = 0; q < 4; q++) {
                for (int k = 0; k < 3; k++) {
                    t.p[k].x = tri[q][k * 2];
                    t.p[k].y = tri[q][k * 2 + 1];
                }
                lv_draw_triangle(layer, &t);
            }
            lv_draw_rect_dsc_t cap;
            lv_draw_rect_dsc_init(&cap);
            cap.bg_color = ec;
            cap.bg_opa = LV_OPA_COVER;
            cap.radius = LV_RADIUS_CIRCLE;
            lv_area_t c1 = { cx + x1 - w, cy + y1 - w, cx + x1 + w - 1, cy + y1 + w - 1 };
            lv_area_t c2 = { cx + x2 - w, cy + y2 - w, cx + x2 + w - 1, cy + y2 + w - 1 };
            lv_draw_rect(layer, &cap, &c1);
            lv_draw_rect(layer, &cap, &c2);
        }
    }
}

static lv_area_t eye_box(const drawn_t *d, int i, int cx, int cy)
{
    int x1, y1, x2, y2, w;
    eye_ends(d, i, &x1, &y1, &x2, &y2, &w);
    lv_area_t r = { cx + (x1 < x2 ? x1 : x2) - w - 3, cy + (y1 < y2 ? y1 : y2) - w - 3,
                    cx + (x1 > x2 ? x1 : x2) + w + 3, cy + (y1 > y2 ? y1 : y2) + w + 3 };
    return r;
}

static void area_union(lv_area_t *u, const lv_area_t *b)
{
    if (b->x1 < u->x1) u->x1 = b->x1;
    if (b->y1 < u->y1) u->y1 = b->y1;
    if (b->x2 > u->x2) u->x2 = b->x2;
    if (b->y2 > u->y2) u->y2 = b->y2;
}

/* Takes the new drawing; invalidates only what changed. */
static void set_drawn(ui_avatar_t *a, const drawn_t *n)
{
    lv_area_t co;
    lv_obj_get_coords(a->obj, &co);
    int cx = co.x1 + a->S / 2, cy = co.y1 + a->S / 2;
    const av_def_t *def = the_def();
    if (!a->dr_ok || n->body != a->dr.body) {
        a->dr = *n;
        a->dr_ok = true;
        lv_obj_invalidate(a->obj);
        return;
    }
    drawn_t *o = &a->dr;
    for (int i = 0; i < 2; i++) {
        if (memcmp(o->ex[i], n->ex[i], sizeof o->ex[i]) == 0 && o->eyes == n->eyes && o->blink == n->blink) continue;
        lv_area_t u = eye_box(o, i, cx, cy), b = eye_box(n, i, cx, cy);
        area_union(&u, &b);
        lv_obj_invalidate_area(a->obj, &u);
    }
    for (int i = 0; i < def->nnodes && BK.on; i++) {
        if (!memcmp(o->ear[i], n->ear[i], sizeof o->ear[i])) continue;
        if (BK.node_mask[i] == 255) continue;
        const lv_image_dsc_t *m = &BK.ear[BK.node_mask[i]];
        int w = (int)m->header.w, h = (int)m->header.h;
        lv_area_t u = { cx + o->ear[i][0] - w / 2 - 1, cy + o->ear[i][1] - h / 2 - 1, cx + o->ear[i][0] + w / 2 + 1,
                        cy + o->ear[i][1] + h / 2 + 1 };
        lv_area_t b = { cx + n->ear[i][0] - w / 2 - 1, cy + n->ear[i][1] - h / 2 - 1, cx + n->ear[i][0] + w / 2 + 1,
                        cy + n->ear[i][1] + h / 2 + 1 };
        area_union(&u, &b);
        lv_obj_invalidate_area(a->obj, &u);
        /* an ear that crosses in front of the head changes what the eyes sit on: their boxes again */
    }
    *o = *n;
}

/* ================================================================== time */

static float blink_curve(float t, float dur)
{
    if (t < 0 || t >= dur) return 0;
    float u = t / dur;
    return u < 0.35f ? u / 0.35f : 1.0f - (u - 0.35f) / 0.65f;
}

static void timeline(ui_avatar_t *a, double now)
{
    const av_step_t *st = &a->steps[a->step];
    double end = a->step_t0 + (st->trans + st->hold) / 1000.0;
    if (now < end) return;
    int n = a->an->n;
    int nx = a->step + a->dir;
    if (a->an->mode == AV_PINGPONG) {
        if (nx >= n) {
            a->dir = -1;
            nx = n >= 2 ? n - 2 : 0;
        } else if (nx < 0) {
            a->dir = 1;
            nx = n >= 2 ? 1 : 0;
        }
    } else if (nx >= n) {
        if (a->reacting) {
            /* a reaction is over: back to what it was doing */
            a->reacting = false;
            resolve(a, a->base, false);
            return;
        }
        if (a->an->mode == AV_ONCE) {
            a->step_t0 = now + 3600; /* settled on its last step */
            return;
        }
        nx = 0;
    }
    if (n == 1 && a->steps[nx].expr == st->expr && !a->reacting) {
        a->step_t0 = now - st->trans / 1000.0; /* the same pose again: just hold on */
        return;
    }
    begin_step(a, nx, now);
}

bool ui_avatar_frame(ui_avatar_t *a)
{
    if (!a->active || !a->baked) return false;
    double now = bz_motion_now();
    if (a->gen != g_gen) {
        /* a different character: the body is baked again, the state begins afresh */
        bool was = a->active;
        ui_avatar_active(a, false);
        a->dr_ok = false;
        a->has_pose = false;
        a->an = NULL;
        if (was) ui_avatar_active(a, true);
        if (!a->baked) return false;
        return true;
    }
    float interval = a->calm > 0.5f ? 0.1f : 0.05f;
    if (now - a->last < interval - 0.002) return a->moving;
    a->last = now;
    bool calm_all = bz_ui_calm();
    if (!a->an) ui_avatar_state(a, a->base);
    timeline(a, now);
    bool run = bz_motion_tick(&a->s);
    float s = a->s.value;
    for (int i = 0; i < AV_NP; i++) a->P[i] = a->from[i] + (a->to[i] - a->from[i]) * s;

    float Q[AV_NP];
    memcpy(Q, a->P, sizeof Q);
    const av_expr_t *ex = &a->adef->x[a->steps[a->step].expr].e;
    bool moving = run;
    float amb = calm_all ? 0 : 1.0f - a->calm;
    /* a saccade: the eyes jump a little and settle, now and then */
    if (ex->meye == 1 && amb > 0.3f) {
        if (now > a->sacc_at) {
            a->tox = (rnd(a) - 0.5f) * 3.0f;
            a->toy = (rnd(a) - 0.5f) * 2.2f;
            a->sacc_at = now + 0.7 + rnd(a) * 2.2;
        }
    } else {
        a->tox = a->toy = 0;
    }
    a->ox += (a->tox - a->ox) * 0.5f;
    a->oy += (a->toy - a->oy) * 0.5f;
    if (fabsf(a->tox - a->ox) > 0.05f || fabsf(a->toy - a->oy) > 0.05f) moving = true;
    if (ex->meye == 2 && amb > 0.5f) {
        Q[P_LX] += (rnd(a) - 0.5f) * 1.6f;
        Q[P_RX] += (rnd(a) - 0.5f) * 1.6f;
        moving = true;
    }
    Q[P_LX] += a->ox;
    Q[P_RX] += a->ox;
    Q[P_LY] += a->oy;
    Q[P_RY] += a->oy;
    /* the body: a slow drift of the head, or a tremble */
    if (ex->mbody == 1 && a->calm < 0.4f && !calm_all) {
        Q[P_HZ] += 1.4f * sinf((float)now * 0.9f);
        Q[P_HY] += 1.0f * sinf((float)now * 0.6f + 1.0f);
        moving = true;
    } else if (ex->mbody == 2 && a->calm < 0.4f && !calm_all) {
        Q[P_HZ] += (rnd(a) - 0.5f) * 1.8f;
        moving = true;
    }
    /* looking at a finger */
    if (now > a->look_until) a->lookx = a->looky = 0;
    a->lx += (a->lookx - a->lx) * 0.4f;
    a->ly += (a->looky - a->ly) * 0.4f;
    if (fabsf(a->lookx - a->lx) > 0.01f || fabsf(a->looky - a->ly) > 0.01f) moving = true;
    Q[P_HY] += a->lx * 16.0f;
    Q[P_HX] += -a->ly * 10.0f;
    Q[P_LX] += a->lx * 4.0f;
    Q[P_RX] += a->lx * 4.0f;
    Q[P_LY] += a->ly * 3.0f;
    Q[P_RY] += a->ly * 3.0f;
    /* the voice */
    if (a->level > 0.01f) {
        Q[P_LH] *= 1.0f + 0.3f * a->level;
        Q[P_RH] *= 1.0f + 0.3f * a->level;
        moving = true;
    }
    /* blinks */
    float bl = 0;
    if (a->blink_at > 0 && !calm_all) {
        if (a->blink_t0 == 0 && now >= a->blink_at) a->blink_t0 = now;
        if (a->blink_t0 > 0) {
            bl = blink_curve((float)(now - a->blink_t0), a->blink_dur);
            if (bl <= 0 && now - a->blink_t0 >= a->blink_dur) {
                schedule_blink(a, now, false);
                bl = 0;
            } else moving = true;
        }
    }
    if (a->steps[a->step].hold >= 60000 && a->forced) bl = 0;

    av_eye_t eye[2];
    av_ear_t ear[AV_MAX_NODES];
    av_geom(the_def(), Q, eye, ear);
    drawn_t n;
    memset(&n, 0, sizeof n);
    for (int i = 0; i < 2; i++) {
        n.ex[i][0] = (int16_t)lroundf(eye[i].cx * a->k * 2);
        n.ex[i][1] = (int16_t)lroundf(eye[i].cy * a->k * 2);
        n.ex[i][2] = (int16_t)lroundf(eye[i].dx * 64);
        n.ex[i][3] = (int16_t)lroundf(eye[i].dy * 64);
        n.ex[i][4] = (int16_t)lroundf(eye[i].len * a->k * 2);
        n.ex[i][5] = (int16_t)lroundf(eye[i].wid * a->k * 2);
    }
    for (int i = 0; i < the_def()->nnodes; i++) {
        n.ear[i][0] = (int16_t)lroundf(ear[i].x * a->k);
        n.ear[i][1] = (int16_t)lroundf(ear[i].y * a->k);
        n.ear[i][2] = ear[i].z >= 0 ? 1 : -1;
    }
    /* the colours, to the 565 the panel can show, so a slow fade is a few redraws and not hundreds */
    int br = (int)(Q[P_BR] + 0.5f) & 0xF8, bg = (int)(Q[P_BG] + 0.5f) & 0xFC, bb = (int)(Q[P_BB] + 0.5f) & 0xF8;
    int er = (int)(Q[P_ER] + 0.5f) & 0xF8, eg = (int)(Q[P_EG] + 0.5f) & 0xFC, eb = (int)(Q[P_EB] + 0.5f) & 0xF8;
    n.body = (uint32_t)(br << 16 | bg << 8 | bb);
    n.eyes = (uint32_t)(er << 16 | eg << 8 | eb);
    n.blink = (uint8_t)(bl * 255.0f + 0.5f);
    set_drawn(a, &n);
    a->moving = moving;
    return moving;
}

/* ================================================================== life cycle */

ui_avatar_t *ui_avatar_create(lv_obj_t *parent, int size)
{
    ui_avatar_t *a = lv_malloc_zeroed(sizeof *a);
    if (!a) return NULL;
    a->S = size;
    a->k = (float)size / 300.0f;
    a->rng = (uint32_t)(bz_motion_now() * 1000.0) | 1u;
    a->base = AV_S_IDLE;
    a->gen = g_gen;
    a->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(a->obj);
    lv_obj_set_size(a->obj, size, size);
    lv_obj_remove_flag(a->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->obj, av_draw, LV_EVENT_DRAW_MAIN, a);
    return a;
}

void ui_avatar_active(ui_avatar_t *a, bool on)
{
    if (!a || a->active == on) return;
    if (on) {
        a->gen = g_gen;
        a->baked = bake_take(a->S);
        a->active = true;
        a->dr_ok = false;
        a->has_pose = false;
        a->an = NULL;
        a->last = 0;
        if (a->baked) lv_obj_invalidate(a->obj);
    } else {
        a->active = false;
        if (a->baked) bake_give();
        a->baked = false;
    }
}

/* ================================================================== the character on the card */

static void say(const char *what, const char *why)
{
    printf("avatar: %s%s%s\n", what, why && why[0] ? ": " : "", why ? why : "");
}

bool ui_avatar_load(const char *path, char *why, size_t wn)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, wn, "cannot open the file");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 2 || n > 256 * 1024) {
        fclose(f);
        snprintf(why, wn, n < 2 ? "the file is empty" : "the file is over 256 KiB");
        return false;
    }
    char *js = BIG_ALLOC((size_t)n + 1);
    if (!js) {
        fclose(f);
        snprintf(why, wn, "out of memory");
        return false;
    }
    size_t got = fread(js, 1, (size_t)n, f);
    fclose(f);
    js[got] = 0;
    av_def_t *d = av_def_parse(js, got, why, wn);
    BIG_FREE(js); /* the text goes: only the compact struct stays */
    if (!d) return false;
    av_def_t *old = g_loaded;
    g_loaded = d;
    g_gen++;
    av_def_free(old);
    return true;
}

void ui_avatar_poll(const char *path)
{
    static double next;
    double now = bz_motion_now();
    if (now < next) return;
    next = now + 3.0;
    struct stat st;
    bool have = path && stat(path, &st) == 0;
    if (!have) {
        if (g_loaded || g_mtime != -1) {
            av_def_free(g_loaded);
            g_loaded = NULL;
            g_gen++;
            g_mtime = g_size = -1;
            say("no avatar file on the card, using the built-in one", NULL);
        }
        return;
    }
    if ((long)st.st_mtime == g_mtime && (long)st.st_size == g_size) return;
    g_mtime = (long)st.st_mtime;
    g_size = (long)st.st_size;
    char why[96] = "";
    if (ui_avatar_load(path, why, sizeof why)) {
        printf("avatar: loaded %s (%d expressions, %d animations, %d nodes)\n", g_loaded->name, g_loaded->nx,
               g_loaded->na, g_loaded->nnodes);
    } else {
        say("not using the card's file, keeping the current character", why);
    }
}

const char *ui_avatar_name(void) { return the_def()->name; }
bool ui_avatar_custom(void) { return g_loaded != NULL; }
const av_def_t *ui_avatar_def(void) { return the_def(); }

void ui_avatar_dump(void)
{
    const av_def_t *me = the_def();
    printf("avatar: %s, %d expressions, %d animations, %d nodes, baked %u bytes\n", me->name, me->nx, me->na,
           me->nnodes, (unsigned)ui_avatar_bytes());
    for (int s = 0; s < AV_S_COUNT; s++) {
        const char *how = "built-in";
        const char *which = "";
        for (int i = 0; i < 3 && MAP[s].anim[i] && !which[0]; i++)
            if (av_def_anim(me, MAP[s].anim[i]) >= 0) {
                how = "animation";
                which = MAP[s].anim[i];
            }
        for (int i = 0; i < 4 && MAP[s].expr[i] && !which[0]; i++)
            if (av_def_expr(me, MAP[s].expr[i]) >= 0) {
                how = "expression";
                which = MAP[s].expr[i];
            }
        printf("avatar:   %-9s %s %s\n", MAP[s].name, how, which);
    }
}
