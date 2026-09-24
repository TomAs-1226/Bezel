#include "bz_ui.h"
#ifdef ESP_PLATFORM
#include "esp_debug_helpers.h"
#include "esp_log.h"
#endif
#include "bz_theme.h"
#include "bz_tokens.h"
#include "lvgl_private.h" /* the pending invalidations and area helpers, for bz_ui_scroll */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#define MAX_GLASS BZ_COMP_MAX_SHAPES
#define MAX_HOOKS 24

struct bz_glass {
    lv_obj_t *obj;
    uint8_t group;
    float radius;
    float press_scale;
    int tint;
    bz_motion_t strength, press, scale, tint_amt;
    float px, py;
    bool use_rect;
    float rx, ry, rw, rh;
    bool alive, solo;
    int lite_opa;      /* calm: the solid background last given to obj (-1: none) */
    bool lite_dark;
};

static struct {
    bz_ui_config_t cfg;
    lv_display_t *disp_content, *disp_glass;
    lv_indev_t *in_content, *in_glass;
    bz_comp_t *comp;
    bz_glass_t glass[MAX_GLASS];
    struct { bz_frame_fn fn; void *user; } hooks[MAX_HOOKS];
    int nhooks;
    double now, last;
    double last_touch;
    bool dark, calm;
    bz_motion_t lx, ly;
    /* touch routing */
    bool pressed;
    int tx, ty;
    int owner; /* 0 none, 1 content, 2 glass */
    bool keep_alive;
    bool swallow_seen; /* the swallowed press has begun */
    bool swallow; /* the press under way, or the next one, is not the interface's (it wakes the screen) */
    /* motion caches */
    bool frozen, offscreen;
    int thaw;                /* the next band a thaw redraws, or -1 */
    lv_draw_buf_t *own_buf;
    /* performance */
    uint32_t lvgl_px, shift_px;
    bz_ui_perf_t perf;
#if BZ_LEAN
    bz_present_t lean[BZ_COMP_MAX_PRESENT]; /* this frame's dirty areas, straight from the content buffer */
    int nlean;
    uint16_t *snap, *ground; /* the slide's snapshot, and a screen of ground for the gap it opens */
    uint16_t *chrome_src;    /* the screen as it was, chrome included: where the fixed chrome is shown from */
    lv_draw_buf_t spare_db;  /* LVGL's other content buffer: at a slide's start the two swap, no copy */
    lv_draw_buf_t *bufs[2];
    uint16_t *bufpx[2];
    int cur_buf;
    uint16_t *nb;            /* the neighbouring page, drawn offscreen during the slide */
    int nb_side;
    bz_area_t chrome[8];     /* the top layer's visible pieces (dock, island…): they stay put while pages slide */
    int nchrome;
    uint32_t ground_color;
    bool sliding;
    int slide_dx, slide_shown;
#endif
} U;

void bz_theme_changed(void); /* bz_theme.c */

typedef struct {
    bz_drag_t d;
    lv_point_t start;
    bool tracking;
} drag_state_t;

static drag_state_t *g_claimed;      /* the handler that owns this press: the innermost that matches */
static bool g_press_claimed;         /* any drag claimed during the current press */
static double g_drag_end;
static bz_velocity_t g_vx, g_vy;     /* the pointer's velocity, shared by every handler */

/* ------------------------------------------------------------------ displays */

/* ---- where LVGL's time goes, per display: layout (refresh start to render start), render (drawing,
 * including the flush callbacks), flush (the callbacks alone) and the rest of lv_timer_handler ---- */
static double prof_wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static struct { double t_refr, t_render, layout, render, flush; } PROF[2];
static double prof_handler;

static void prof_event(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    double t = prof_wall();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START: PROF[i].t_refr = t; PROF[i].t_render = 0; break;
    case LV_EVENT_RENDER_START: PROF[i].t_render = t; PROF[i].layout += t - PROF[i].t_refr; break;
    case LV_EVENT_RENDER_READY: if (PROF[i].t_render > 0) PROF[i].render += t - PROF[i].t_render; break;
    case LV_EVENT_REFR_READY: if (PROF[i].t_render == 0) PROF[i].layout += t - PROF[i].t_refr; break;
    default: break;
    }
}

/* Who asks for (nearly) the whole screen to be redrawn: a backtrace at most every 3 s, decoded with
 * addr2line against the ELF. */
static double s_hook_ms[16];
static void big_inv(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    static double last;
    if (!a || lv_area_get_size(a) < 300000 || U.offscreen) return;
    double t = prof_wall();
    if (t - last < 3) return;
    last = t;
#ifdef ESP_PLATFORM
    ESP_LOGW("bz_ui", "full-screen invalidate %d,%d-%d,%d", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
#endif
}

void bz_ui_hooks_report(void)
{
#ifdef ESP_PLATFORM
    for (int i = 0; i < U.nhooks && i < 16; i++) {
        ESP_LOGI("bz_ui", "hook %d fn %p: %.0f ms/2s", i, (void *)U.hooks[i].fn, s_hook_ms[i]);
        s_hook_ms[i] = 0;
    }
#endif
}

static void prof_attach(lv_display_t *d, int i)
{
    lv_display_add_event_cb(d, big_inv, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(d, prof_event, LV_EVENT_REFR_START, (void *)(intptr_t)i);
    lv_display_add_event_cb(d, prof_event, LV_EVENT_RENDER_START, (void *)(intptr_t)i);
    lv_display_add_event_cb(d, prof_event, LV_EVENT_RENDER_READY, (void *)(intptr_t)i);
    lv_display_add_event_cb(d, prof_event, LV_EVENT_REFR_READY, (void *)(intptr_t)i);
}

void bz_ui_prof_take(float out[8])
{
    /* ms accumulated since the last call: content layout, render, flush; glass layout, render, flush;
     * the whole of lv_timer_handler */
    for (int i = 0; i < 2; i++) {
        out[i * 3 + 0] = (float)(PROF[i].layout * 1e3);
        out[i * 3 + 1] = (float)(PROF[i].render * 1e3);
        out[i * 3 + 2] = (float)(PROF[i].flush * 1e3);
        PROF[i].layout = PROF[i].render = PROF[i].flush = 0;
    }
    out[6] = (float)(prof_handler * 1e3);
    out[7] = 0;
    prof_handler = 0;
}

/* Pixels of the content buffer that changed: the compositor's damage, or in lean straight onto the list
 * of areas to present (merged into their bounding box once the list is full). */
static void damage_content(const bz_area_t *b)
{
#if BZ_LEAN
    if (U.nlean == BZ_COMP_MAX_PRESENT) {
        bz_area_t *m = &U.lean[0].a;
        for (int i = 1; i < U.nlean; i++) {
            bz_area_t *o = &U.lean[i].a;
            if (o->x1 < m->x1) m->x1 = o->x1;
            if (o->y1 < m->y1) m->y1 = o->y1;
            if (o->x2 > m->x2) m->x2 = o->x2;
            if (o->y2 > m->y2) m->y2 = o->y2;
        }
        U.nlean = 1;
        if (b->x1 < m->x1) m->x1 = b->x1;
        if (b->y1 < m->y1) m->y1 = b->y1;
        if (b->x2 > m->x2) m->x2 = b->x2;
        if (b->y2 > m->y2) m->y2 = b->y2;
    } else {
        U.lean[U.nlean++].a = *b;
    }
#else
    bz_comp_damage_content(U.comp, b);
#endif
}

static void flush_glass_body(lv_display_t *d, const lv_area_t *a, uint8_t *px);
static void split_cb(lv_event_t *e);
static void flush_content_body(lv_display_t *d, const lv_area_t *a, uint8_t *px);

static void flush_content(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    double t = prof_wall();
    flush_content_body(d, a, px);
    PROF[0].flush += prof_wall() - t;
}

static void flush_glass(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    double t = prof_wall();
    flush_glass_body(d, a, px);
    PROF[1].flush += prof_wall() - t;
}

static void flush_content_body(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    U.lvgl_px += (uint32_t)lv_area_get_size(a);
    if (!U.offscreen) {
#ifndef ESP_PLATFORM
        if (getenv("SIM_DEBUG_FLUSH"))
            fprintf(stderr, "flush content %d,%d-%d,%d\n", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
#endif
        bz_area_t b = { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 };
        damage_content(&b);
    }
    lv_display_flush_ready(d);
}

#ifndef ESP_PLATFORM
/* The simulator's SIM_DEBUG_FLUSH: who asks for the whole screen to be redrawn, and when. */
static void debug_inv(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (a && lv_area_get_size(a) > 200000) fprintf(stderr, "invalidate %d,%d-%d,%d at %.3f frozen %d off %d\n", (int)a->x1,
                                                  (int)a->y1, (int)a->x2, (int)a->y2, U.now, U.frozen, U.offscreen);
}
#endif

static void flush_glass_body(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    U.lvgl_px += (uint32_t)lv_area_get_size(a);
    bz_area_t b = { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 };
    bz_comp_damage_ink(U.comp, &b);
    lv_display_flush_ready(d);
}

static lv_obj_t *clickable_at(lv_obj_t *scr, int x, int y)
{
    lv_point_t p = { x, y };
    lv_obj_t *o = lv_indev_search_obj(scr, &p);
    while (o && o != scr) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return o;
        o = lv_obj_get_parent(o);
    }
    return NULL;
}

static void read_content(lv_indev_t *in, lv_indev_data_t *data)
{
    (void)in;
    data->point.x = U.tx;
    data->point.y = U.ty;
    data->state = U.pressed && U.owner == 1 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void read_glass(lv_indev_t *in, lv_indev_data_t *data)
{
    (void)in;
    data->point.x = U.tx;
    data->point.y = U.ty;
    data->state = U.pressed && U.owner == 2 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void poll_touch(void)
{
    int x = U.tx, y = U.ty;
    bool p = U.cfg.read_touch && U.cfg.read_touch(&x, &y, U.cfg.user);
    if (U.swallow) {
        /* counted as a touch (it ends idle) but never delivered; once it lifts, presses are normal again */
        if (p) {
            U.last_touch = U.now;
            U.swallow_seen = true;
        } else if (U.swallow_seen) {
            U.swallow = U.swallow_seen = false;
        }
        U.pressed = false;
        return;
    }
    if (p) {
        U.tx = x;
        U.ty = y;
        U.last_touch = U.now;
        if (!U.pressed) {
#if BZ_LEAN
            U.owner = 1; /* one display: LVGL finds the top layer (the chrome) before the page itself */
#else
            U.owner = clickable_at(lv_display_get_screen_active(U.disp_glass), x, y) ? 2 : 1;
#endif
            g_press_claimed = false;
            g_claimed = NULL;
            bz_velocity_reset(&g_vx);
            bz_velocity_reset(&g_vy);
        }
        bz_velocity_add(&g_vx, U.now, (float)x);
        bz_velocity_add(&g_vy, U.now, (float)y);
    }
    U.pressed = p;
}

lv_obj_t *bz_ui_content(void) { return lv_display_get_screen_active(U.disp_content); }
#if BZ_LEAN
lv_obj_t *bz_ui_glass(void) { return lv_display_get_layer_top(U.disp_content); }
#else
lv_obj_t *bz_ui_glass(void) { return lv_display_get_screen_active(U.disp_glass); }
#endif
bz_comp_t *bz_ui_comp(void) { return U.comp; }

static uint32_t tick_ms(void) { return (uint32_t)(U.now * 1000.0); }

void bz_ui_init(const bz_ui_config_t *cfg)
{
    memset(&U, 0, sizeof U);
    U.cfg = *cfg;
    U.dark = true;
    U.thaw = -1;
    lv_tick_set_cb(tick_ms);
    U.comp = bz_comp_create(cfg->w, cfg->h, cfg->content, cfg->ink, cfg->out);
    if (cfg->ops) bz_comp_set_ops(U.comp, cfg->ops);
    if (cfg->async_present) {
        uint16_t *out2 = aligned_alloc(64, (size_t)cfg->w * cfg->h * 2);
        if (out2) bz_comp_set_out2(U.comp, out2);
    }

    U.disp_content = lv_display_create(cfg->w, cfg->h);
    lv_display_set_color_format(U.disp_content, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(U.disp_content, cfg->content, NULL, (uint32_t)(cfg->w * cfg->h * 2),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(U.disp_content, flush_content);
    lv_display_add_event_cb(U.disp_content, split_cb, LV_EVENT_ALL, NULL);
    U.own_buf = lv_display_get_buf_active(U.disp_content);
#ifndef ESP_PLATFORM
    if (getenv("SIM_DEBUG_FLUSH")) lv_display_add_event_cb(U.disp_content, debug_inv, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

#if BZ_LEAN
    prof_attach(U.disp_content, 0);
    lv_obj_remove_flag(lv_display_get_screen_active(U.disp_content), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *top = lv_display_get_layer_top(U.disp_content);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    U.in_content = lv_indev_create();
    lv_indev_set_type(U.in_content, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(U.in_content, read_content);
    lv_indev_set_display(U.in_content, U.disp_content);
    lv_indev_set_mode(U.in_content, LV_INDEV_MODE_EVENT);
    U.calm = true;
    bz_motion_set_calm(true);
    bz_motion_set_instant(true);
    /* the slide's pictures up front: a first swipe mustn't wait on allocating 7 MB */
    {
        size_t px = (size_t)cfg->w * cfg->h * 2;
        uint16_t *spare = aligned_alloc(128, px);
        uint32_t stride_b = (uint32_t)cfg->w * 2;
        if (spare) lv_draw_buf_init(&U.spare_db, (uint32_t)cfg->w, (uint32_t)cfg->h, LV_COLOR_FORMAT_RGB565, stride_b,
                                    spare, (uint32_t)px);
        U.bufs[0] = U.own_buf;
        U.bufpx[0] = cfg->content;
        U.bufs[1] = spare ? &U.spare_db : NULL;
        U.bufpx[1] = spare;
        /* with the platform's slide (cfg->slide) the snapshot is the swapped buffer and the gap and chrome
         * come from the panel's side: only the neighbour's landscape band target is needed here */
        U.snap = cfg->slide ? NULL : aligned_alloc(128, px);
        U.ground = cfg->slide ? NULL : aligned_alloc(128, px);
        if (U.ground) {
            /* the gap a slide opens, in the dark ground (re-filled if the tone changes) */
            uint32_t g = bz_color(BZ_C_GROUND);
            uint16_t c = (uint16_t)(((g >> 19) & 31) << 11 | ((g >> 10) & 63) << 5 | ((g >> 3) & 31));
            for (size_t i = 0; i < px / 2; i++) U.ground[i] = c;
            U.ground_color = g;
        }
        U.chrome_src = cfg->slide ? NULL : aligned_alloc(128, px);
        U.nb = aligned_alloc(128, px);
    }
    bz_motion_init(&U.lx, -0.42f, 0.002f);
    bz_motion_init(&U.ly, -0.91f, 0.002f);
    lv_display_set_default(U.disp_content);
    return;
#endif
    U.disp_glass = lv_display_create(cfg->w, cfg->h);
    lv_display_set_color_format(U.disp_glass, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(U.disp_glass, cfg->ink, NULL, (uint32_t)(cfg->w * cfg->h * 4),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(U.disp_glass, flush_glass);
    prof_attach(U.disp_content, 0);
    prof_attach(U.disp_glass, 1);
    lv_display_add_event_cb(U.disp_glass, split_cb, LV_EVENT_ALL, NULL);
    memset(cfg->ink, 0, (size_t)cfg->w * cfg->h * 4);

    lv_obj_t *gs = lv_display_get_screen_active(U.disp_glass);
    lv_obj_set_style_bg_opa(gs, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(gs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(gs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cs = lv_display_get_screen_active(U.disp_content);
    lv_obj_remove_flag(cs, LV_OBJ_FLAG_SCROLLABLE);

    U.in_content = lv_indev_create();
    lv_indev_set_type(U.in_content, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(U.in_content, read_content);
    lv_indev_set_display(U.in_content, U.disp_content);
    lv_indev_set_mode(U.in_content, LV_INDEV_MODE_EVENT);
    U.in_glass = lv_indev_create();
    lv_indev_set_type(U.in_glass, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(U.in_glass, read_glass);
    lv_indev_set_display(U.in_glass, U.disp_glass);
    lv_indev_set_mode(U.in_glass, LV_INDEV_MODE_EVENT);

    bz_motion_init(&U.lx, -0.42f, 0.002f);
    bz_motion_init(&U.ly, -0.91f, 0.002f);
    lv_display_set_default(U.disp_content);
}

/* ------------------------------------------------------------------ glass registry */

static void glass_event(lv_event_t *e)
{
    bz_glass_t *g = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        g->px = (float)p.x;
        g->py = (float)p.y;
        bz_motion_to(&g->press, 1, BZ_HOLD);
        bz_motion_to(&g->scale, g->press_scale, BZ_HOLD);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        bz_motion_to(&g->press, 0, BZ_RELEASE);
        bz_motion_to(&g->scale, 1, BZ_RELEASE);
    } else if (code == LV_EVENT_DELETE) {
        g->alive = false;
    }
}

bz_glass_t *bz_glass_attach(lv_obj_t *obj, uint8_t group, float radius)
{
    for (int i = 0; i < MAX_GLASS; i++) {
        bz_glass_t *g = &U.glass[i];
        if (g->alive) continue;
        memset(g, 0, sizeof *g);
        g->alive = true;
        g->obj = obj;
        g->group = group;
        g->radius = radius;
        g->press_scale = BZ_GLASS_PRESS_SCALE;
        g->tint = -1;
        g->lite_opa = -1;
        bz_motion_init(&g->strength, 0, 0.002f);
        bz_motion_init(&g->press, 0, 0.002f);
        bz_motion_init(&g->scale, 1, 0.0005f);
        bz_motion_init(&g->tint_amt, 0, 0.002f);
        bz_motion_to(&g->strength, 1, BZ_LAYER);
        lv_obj_add_event_cb(obj, glass_event, LV_EVENT_ALL, g);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
        return g;
    }
    LV_LOG_WARN("bz_glass: more than %d glass shapes", MAX_GLASS);
    return NULL;
}

void bz_glass_show(bz_glass_t *g, bool show)
{
    if (!g) return;
    bz_motion_to(&g->strength, show ? 1 : 0, show ? BZ_LAYER : BZ_EFFECT);
}

void bz_glass_set_strength(bz_glass_t *g, float s)
{
    if (g) bz_motion_set(&g->strength, s, 0);
}

void bz_glass_set_tint(bz_glass_t *g, int tint, float amt)
{
    if (!g) return;
    if (tint >= 0) g->tint = tint;
    bz_motion_to(&g->tint_amt, amt, BZ_EFFECT);
}

void bz_glass_set_press_scale(bz_glass_t *g, float s) { if (g) g->press_scale = s; }
void bz_glass_set_radius(bz_glass_t *g, float r) { if (g) g->radius = r; }
void bz_glass_set_solo(bz_glass_t *g, bool solo) { if (g) g->solo = solo; }
float bz_glass_strength(const bz_glass_t *g) { return g ? g->strength.value : 0; }
lv_obj_t *bz_glass_obj(const bz_glass_t *g) { return g ? g->obj : NULL; }

void bz_glass_set_rect(bz_glass_t *g, bool use, float x, float y, float w, float h)
{
    if (!g) return;
    g->use_rect = use;
    g->rx = x; g->ry = y; g->rw = w; g->rh = h;
}

static bool visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
        if (lv_obj_get_style_opa(o, 0) < 8) return false;
    }
    return true;
}

static bool update_glass(void)
{
    bz_glass_shape_t shapes[MAX_GLASS];
    int n = 0;
    bool moving = false;
    for (int i = 0; i < MAX_GLASS; i++) {
        bz_glass_t *g = &U.glass[i];
        if (!g->alive) continue;
        moving |= bz_motion_tick(&g->strength);
        moving |= bz_motion_tick(&g->press);
        moving |= bz_motion_tick(&g->scale);
        moving |= bz_motion_tick(&g->tint_amt);
        /* Calm is also the fast path: no glass pass at all. The shape becomes an ordinary LVGL rounded
         * surface on the glass layer, which the compositor only blends like any other ink; strength
         * drives its opacity (in 16 steps, so a fade doesn't restyle every frame). */
        if (U.calm) {
            float st = g->strength.value < 0 ? 0 : g->strength.value > 1 ? 1 : g->strength.value;
            int opa = (int)(st * 16 + 0.5f) * 255 / 16;
            if (opa != g->lite_opa || g->lite_dark != U.dark) {
                g->lite_opa = opa;
                g->lite_dark = U.dark;
                lv_obj_set_style_bg_color(g->obj, bz_lv(BZ_C_SURFACE2), 0);
                lv_obj_set_style_bg_opa(g->obj, (lv_opa_t)opa, 0);
                lv_obj_set_style_radius(g->obj, (int32_t)g->radius, 0);
            }
            continue;
        }
        if (g->lite_opa >= 0) {
            g->lite_opa = -1;
            lv_obj_set_style_bg_opa(g->obj, LV_OPA_TRANSP, 0);
        }
        if (g->strength.value < 0.002f || !visible(g->obj)) continue;
        bz_glass_shape_t *s = &shapes[n++];
        if (g->use_rect) {
            s->x = g->rx; s->y = g->ry; s->w = g->rw; s->h = g->rh;
        } else {
            lv_area_t a;
            lv_obj_get_coords(g->obj, &a);
            s->x = (float)a.x1; s->y = (float)a.y1;
            s->w = (float)lv_area_get_width(&a); s->h = (float)lv_area_get_height(&a);
        }
        s->radius = g->radius;
        s->strength = g->strength.value > 1 ? 1 : g->strength.value;
        s->press = g->press.value < 0 ? 0 : g->press.value;
        s->press_x = g->px;
        s->press_y = g->py;
        s->scale = bz_motion_calm() ? 1 : g->scale.value;
        s->group = g->group;
        s->id = (uint8_t)i;
        s->solo = g->solo;
        s->tint = (int8_t)g->tint;
        s->tint_amt = g->tint_amt.value < 0 ? 0 : g->tint_amt.value;
    }
    bz_comp_set_shapes(U.comp, shapes, n);
    return moving;
}

/* ------------------------------------------------------------------ frame */

void bz_ui_on_frame(bz_frame_fn fn, void *user)
{
    if (U.nhooks < MAX_HOOKS) {
        U.hooks[U.nhooks].fn = fn;
        U.hooks[U.nhooks].user = user;
        U.nhooks++;
    }
}

void bz_ui_keep_alive(void) { U.keep_alive = true; }
double bz_ui_idle_s(void) { return U.now - U.last_touch; }
void bz_ui_swallow_touch(void)
{
    U.swallow = true;
    U.swallow_seen = false;
}
void bz_ui_wake(void) { U.last_touch = U.now; }

double bz_ui_clock(void);
static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static float ema(float old, float v) { return old == 0 ? v : old + (v - old) * 0.1f; }

/* The frame's work costed for the ESP32-P4 (360 MHz, two cores, PPA, PSRAM at 200 MHz), from what was
 * done rather than how long this machine took: the simulator runs on a desktop CPU, and its times say
 * little about the tablet's. Cycles per pixel are estimates for each kernel (the inner loops' operation
 * counts, and LVGL's software renderer at ~26 ns per pixel of a typical Bezel page across its two draw
 * units); PPA and PSRAM traffic at 400 MB/s effective. The panel is fed asynchronously — the PPA turns
 * frame N into the panel's buffer while the cores draw frame N+1 — so a frame costs the larger of the
 * two, plus a little. The tablet's overlay shows measured times. */
static float model_ms(const bz_comp_stats_t *s, uint32_t lvgl_px, uint32_t shift_px)
{
    const double MHZ = 360.0, PAR = 1.8, PPA_MBS = 400.0;
    double cpu = 0;
    /* glass by path: the flat interior (a row-interpolated sample), the edge (bilinear, rim, blend), a
     * shadow alone, and the rest of the box (one table read) */
    uint32_t visited = s->glass_flat_px + s->glass_edge_px + s->glass_shadow_px;
    cpu += s->glass_flat_px * 13.0 / PAR;
    cpu += s->glass_edge_px * (s->glass_fast_px ? 48.0 : 80.0) / PAR;
    cpu += s->glass_shadow_px * 18.0 / PAR;
    cpu += (s->glass_px > visited ? s->glass_px - visited : 0) * 3.0 / PAR;
    cpu += s->lut_px * 14.0 / PAR + s->lut_ring_px * 260.0 / PAR;
    cpu += s->blur_src_px * 6.0 / PAR;
    cpu += s->base_cpu_px * 8.0 / PAR;
    cpu += s->backdrop_build_px * 22.0 / PAR;
    double cpu_ms = cpu / (MHZ * 1000.0) + lvgl_px * 26e-6;
    cpu_ms += (double)shift_px * 4 / (PPA_MBS * 1000.0); /* a scrolled list's rows moved in PSRAM: read + write */
    double sync = 0, async = 0;
    sync += (double)s->composed_px * 4;              /* the base copied into out: the cores wait on it */
    sync += (double)s->ink_px * 8;
    async += (double)(s->composed_px + s->direct_px) * 4; /* the turn into the panel's buffer */
    cpu_ms += sync / (PPA_MBS * 1000.0);
    double ppa_ms = (sync + async) / (PPA_MBS * 1000.0);
    return (float)((cpu_ms > ppa_ms ? cpu_ms : ppa_ms) + 0.5);
}

/* A thaw redraws the content a band per frame (~115k px) rather than all ~920k px in one: the layers the
 * shell leaves up until bz_ui_thawing() says it's done hide the bands still waiting. A finger on the content, or
 * anything about to read the content buffer, finishes it at once. */
#define THAW_BANDS 8

static void thaw_band(int b)
{
    lv_area_t a = { 0, b * U.cfg.h / THAW_BANDS, U.cfg.w - 1, (b + 1) * U.cfg.h / THAW_BANDS - 1 };
    lv_inv_area(U.disp_content, &a);
}

static void thaw_step(void)
{
    if (U.thaw < 0) return;
    int n = U.pressed && U.owner == 1 ? THAW_BANDS - U.thaw : 1; /* a press on glass shows on glass */
    for (int i = 0; i < n && U.thaw < THAW_BANDS; i++) thaw_band(U.thaw++);
    if (U.thaw >= THAW_BANDS) U.thaw = -1; /* the last band draws this frame */
    U.keep_alive = true;
}

static void thaw_finish(void)
{
    if (U.thaw < 0) return;
    while (U.thaw < THAW_BANDS) thaw_band(U.thaw++);
    U.thaw = -1;
    lv_refr_now(U.disp_content);
}

bool bz_ui_thawing(void) { return U.thaw >= 0; }
void bz_ui_thaw_cancel(void) { U.thaw = -1; }

/* Where LVGL's time goes, summed since the last bz_ui_split(): hooks before it, refresh (layout plus
 * render) per display, and the render alone. lv_timer_handler minus refresh is LVGL's own timers and
 * animations. */
static struct { double hooks, lvgl, refr, render, t_refr, t_render; int frames; } SP;

static void split_cb(lv_event_t *e)
{
    double t = wall();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START: SP.t_refr = t; break;
    case LV_EVENT_RENDER_START: SP.t_render = t; break;
    case LV_EVENT_RENDER_READY: if (SP.t_render > 0) SP.render += t - SP.t_render; SP.t_render = 0; break;
    case LV_EVENT_REFR_READY: if (SP.t_refr > 0) SP.refr += t - SP.t_refr; SP.t_refr = 0; break;
    default: break;
    }
}

void bz_ui_split(float *hooks_ms, float *lvgl_ms, float *refr_ms, float *render_ms)
{
    int n = SP.frames > 0 ? SP.frames : 1;
    *hooks_ms = (float)(SP.hooks * 1000 / n);
    *lvgl_ms = (float)(SP.lvgl * 1000 / n);
    *refr_ms = (float)(SP.refr * 1000 / n);
    *render_ms = (float)(SP.render * 1000 / n);
    memset(&SP, 0, sizeof SP);
}

bool bz_ui_frame(double now_s)
{
    double dt = U.last > 0 ? now_s - U.last : 0.016;
    U.last = U.now = now_s;
    bz_motion_clock(now_s);
    U.keep_alive = false;
    U.lvgl_px = U.shift_px = 0;
#if BZ_LEAN
    U.nlean = 0; /* before the hooks and touch: a list scroll records its move here */
#endif
    double t0 = wall();

    poll_touch();
    lv_indev_read(U.in_content);
    if (U.in_glass) lv_indev_read(U.in_glass);

    for (int i = 0; i < U.nhooks; i++) {
        double th = prof_wall();
        U.hooks[i].fn(now_s, dt, U.hooks[i].user);
        if (i < 16) s_hook_ms[i] += (prof_wall() - th) * 1e3;
    }

#if BZ_LEAN
    bool moving = false;
#else
    bool moving = bz_motion_tick(&U.lx) | bz_motion_tick(&U.ly);
    bz_comp_set_light(U.comp, U.lx.value, U.ly.value);
#endif

    thaw_step();

    double t1 = wall();
    lv_timer_handler();
    double t2 = wall();
    prof_handler += t2 - t1;
    SP.hooks += t1 - t0;
    SP.lvgl += t2 - t1;
    SP.frames++;
    moving |= update_glass();

#if BZ_LEAN
    if (U.sliding && U.cfg.slide) {
        U.nlean = 0;
        if (U.slide_dx != U.slide_shown) {
            U.slide_shown = U.slide_dx;
            U.cfg.slide->frame(U.slide_dx, U.nb_side, U.chrome, U.nchrome);
        }
    } else if (U.sliding) {
        /* LVGL's drawing waits: what it drew goes on screen, whole, when the slide ends */
        U.nlean = 0;
        if (U.slide_dx != U.slide_shown) {
            int W = U.cfg.w, H = U.cfg.h, d = U.slide_dx;
            if (d > W) d = W;
            if (d < -W) d = -W;
            U.slide_shown = U.slide_dx;
            bz_present_t *o = U.lean;
            /* the gap shows the neighbour where it has been drawn, else ground */
            if (d >= 0) {
                if (d < W) o[U.nlean++] = (bz_present_t){ { (int16_t)d, 0, (int16_t)(W - 1), (int16_t)(H - 1) }, U.snap, W };
                const uint16_t *gap = U.nb_side < 0 ? U.nb + (W - d) : U.ground;
                if (d > 0) o[U.nlean++] = (bz_present_t){ { 0, 0, (int16_t)(d - 1), (int16_t)(H - 1) }, gap, W };
            } else {
                o[U.nlean++] = (bz_present_t){ { 0, 0, (int16_t)(W - 1 + d), (int16_t)(H - 1) }, U.snap - d, W };
                const uint16_t *gap = U.nb_side > 0 ? U.nb : U.ground + W + d;
                o[U.nlean++] = (bz_present_t){ { (int16_t)(W + d), 0, (int16_t)(W - 1), (int16_t)(H - 1) }, gap, W };
            }
            /* the chrome over it, unshifted, from the snapshot: presented last, it lands on top */
            for (int i = 0; i < U.nchrome && U.nlean < BZ_COMP_MAX_PRESENT; i++) {
                bz_area_t c = U.chrome[i];
                o[U.nlean++] = (bz_present_t){ c, U.chrome_src + (size_t)c.y1 * W + c.x1, W };
            }
        }
    } else {
        for (int i = 0; i < U.nlean; i++) {
            bz_area_t *a = &U.lean[i].a;
            U.lean[i].src = U.cfg.content + (size_t)a->y1 * U.cfg.w + a->x1;
            U.lean[i].stride = U.cfg.w;
        }
    }
    bz_present_t *areas = U.lean;
    int n = U.nlean;
#else
    bz_present_t areas[BZ_COMP_MAX_PRESENT];
    int n = bz_comp_compose(U.comp, areas, BZ_COMP_MAX_PRESENT);
#endif
    double t3 = wall();
    if (n && U.cfg.present) U.cfg.present(areas, n, U.cfg.user);
    double t4 = wall();

    if (n) {
        bz_ui_perf_t *p = &U.perf;
#if !BZ_LEAN
        bz_comp_stats(U.comp, &p->comp);
#endif
        p->lvgl_px = U.lvgl_px;
        static double last_present;
        if (last_present > 0 && now_s > last_present) p->fps = ema(p->fps, (float)(1.0 / (now_s - last_present)));
        last_present = now_s;
        p->frame_ms = ema(p->frame_ms, (float)((t4 - t0) * 1000));
        p->lvgl_ms = ema(p->lvgl_ms, (float)((t2 - t1) * 1000));
        p->compose_ms = ema(p->compose_ms, (float)((t3 - t2) * 1000));
        p->present_ms = ema(p->present_ms, (float)((t4 - t3) * 1000));
        p->model_ms = model_ms(&p->comp, U.lvgl_px, U.shift_px);
        p->shift_px = U.shift_px;
        p->frames++;
    }
    return moving || U.keep_alive || U.pressed;
}

void bz_ui_perf(bz_ui_perf_t *out) { *out = U.perf; }
double bz_ui_clock(void) { return wall(); }

#if BZ_LEAN
/* The top layer's pieces hidden while a slide picture of the page alone is drawn, then put back. */
static void chrome_hide(bool before, void *u)
{
    (void)u;
    static uint32_t shown;
    lv_obj_t *top = lv_display_get_layer_top(U.disp_content);
    uint32_t nc = lv_obj_get_child_count(top);
    if (before) shown = 0;
    for (uint32_t k = 0; k < nc && k < 32; k++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)k);
        if (before) {
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            shown |= 1u << k;
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        } else if (shown & (1u << k)) {
            lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
#endif

void bz_ui_slide_begin(void)
{
#if BZ_LEAN
    double tb = wall();
    size_t px = (size_t)U.cfg.w * U.cfg.h;
    if (!U.cfg.slide) {
        if (!U.snap) U.snap = aligned_alloc(128, px * 2);
        if (!U.ground) U.ground = aligned_alloc(128, px * 2);
        if (!U.chrome_src) U.chrome_src = aligned_alloc(128, px * 2);
        if (!U.snap || !U.ground || !U.chrome_src) return;
    }
    uint32_t g = bz_color(BZ_C_GROUND);
    if (U.ground && (g != U.ground_color || !U.sliding)) {
        uint16_t c = (uint16_t)(((g >> 19) & 31) << 11 | ((g >> 10) & 63) << 5 | ((g >> 3) & 31));
        if (g != U.ground_color) for (size_t i = 0; i < px; i++) U.ground[i] = c;
        U.ground_color = g;
    }
    /* what's on screen now: LVGL's buffer, complete once it has drawn what's pending */
    double tg = wall();
    lv_refr_now(U.disp_content);
    if (U.cfg.slide) {
        /* what LVGL just drew goes to the glass first: the platform's slide starts from the glass */
        for (int i = 0; i < U.nlean; i++) {
            bz_area_t *a = &U.lean[i].a;
            U.lean[i].src = U.cfg.content + (size_t)a->y1 * U.cfg.w + a->x1;
            U.lean[i].stride = U.cfg.w;
        }
        if (U.nlean && U.cfg.present) U.cfg.present(U.lean, U.nlean, U.cfg.user);
        U.nlean = 0;
    }
    double tr = wall();
    /* the picture LVGL just finished becomes the slide's snapshot; LVGL carries on in the other buffer
     * (it redraws everything when the slide ends, so what that buffer holds doesn't matter) */
    if (U.bufs[1]) {
        U.snap = U.bufpx[U.cur_buf];
        U.cur_buf ^= 1;
        U.cfg.content = U.bufpx[U.cur_buf];
        U.own_buf = U.bufs[U.cur_buf];
        lv_display_set_draw_buffers(U.disp_content, U.own_buf, NULL);
    } else if (U.cfg.ops && U.cfg.ops->copy565) {
        U.cfg.ops->copy565(U.snap, U.cfg.w, U.cfg.content, U.cfg.w, U.cfg.w, U.cfg.h);
    } else {
        memcpy(U.snap, U.cfg.content, px * 2);
    }
    double tc = wall();
    U.slide_dx = U.slide_shown = 0;
    U.nb_side = 0;
    U.nchrome = 0;
    lv_obj_t *top = lv_display_get_layer_top(U.disp_content);
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < nc && U.nchrome < 8; i++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        if (a.x1 < 0) a.x1 = 0;
        if (a.y1 < 0) a.y1 = 0;
        if (a.x2 > U.cfg.w - 1) a.x2 = U.cfg.w - 1;
        if (a.y2 > U.cfg.h - 1) a.y2 = U.cfg.h - 1;
        if (a.x2 < a.x1 || a.y2 < a.y1) continue;
        U.chrome[U.nchrome++] = (bz_area_t){ (int16_t)a.x1, (int16_t)a.y1, (int16_t)a.x2, (int16_t)a.y2 };
    }
    /* the snapshot is the page alone: under each piece of chrome, the page is drawn again without it (small
     * areas, a few ms), so the page slides clean under the chrome that stays put */
    /* the platform captures the glass (the page and its chrome) now that it knows where the chrome is */
    if (U.cfg.slide && !U.cfg.slide->begin(bz_color(BZ_C_GROUND), U.chrome, U.nchrome)) {
        /* no slide after all: LVGL is on the other buffer now, so everything is drawn again */
        lv_obj_invalidate(lv_display_get_screen_active(U.disp_content));
        lv_obj_invalidate(lv_display_get_layer_top(U.disp_content));
        return;
    }
    /* the chrome as it is, from the snapshot, before the page under it is drawn in there */
    for (int i = 0; i < U.nchrome && U.chrome_src; i++) {
        bz_area_t c = U.chrome[i];
        size_t n = (size_t)(c.x2 - c.x1 + 1) * 2;
        for (int y = c.y1; y <= c.y2; y++)
            memcpy(U.chrome_src + (size_t)y * U.cfg.w + c.x1, U.snap + (size_t)y * U.cfg.w + c.x1, n);
    }
    /* all of them in one refresh: one layout pass, not one per piece (six of those cost ~100 ms) */
    if (U.nchrome) {
        lv_display_t *d = U.disp_content;
        lv_obj_t *scr = lv_display_get_screen_active(d);
        lv_display_enable_invalidation(d, false);
        chrome_hide(true, NULL);
        lv_obj_update_layout(scr);
        static lv_draw_buf_t db;
        uint32_t stride_b = (uint32_t)U.cfg.w * 2;
        lv_draw_buf_init(&db, (uint32_t)U.cfg.w, (uint32_t)U.cfg.h, LV_COLOR_FORMAT_RGB565, stride_b, U.snap,
                         stride_b * (uint32_t)U.cfg.h);
        lv_display_set_draw_buffers(d, &db, NULL);
        U.offscreen = true;
        lv_display_enable_invalidation(d, true);
        for (int i = 0; i < U.nchrome; i++) {
            bz_area_t c = U.chrome[i];
            lv_area_t a = { c.x1, c.y1, c.x2, c.y2 };
            lv_obj_invalidate_area(scr, &a);
        }
        lv_refr_now(d);
        lv_display_enable_invalidation(d, false);
        U.offscreen = false;
        lv_display_set_draw_buffers(d, U.own_buf, NULL);
        chrome_hide(false, NULL);
        lv_obj_update_layout(scr);
        lv_display_enable_invalidation(d, true);
        /* the page without its chrome, under the chrome, into the platform's picture of it */
        if (U.cfg.slide)
            for (int i = 0; i < U.nchrome; i++) {
                bz_area_t c = U.chrome[i];
                bz_present_t p = { c, U.snap + (size_t)c.y1 * U.cfg.w + c.x1, U.cfg.w };
                U.cfg.slide->patch(&p, false);
            }
    }
    U.sliding = true;
    (void)tb; (void)tg; (void)tr; (void)tc;
#endif
}

void bz_ui_slide_nb_patch(const lv_area_t *a)
{
#if BZ_LEAN
    if (!U.cfg.slide || !U.nb) return;
    bz_present_t p = { { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 },
                       U.nb + (size_t)a->y1 * U.cfg.w + a->x1, U.cfg.w };
    U.cfg.slide->patch(&p, true);
#else
    (void)a;
#endif
}

uint16_t *bz_ui_slide_nb_buf(void)
{
#if BZ_LEAN
    if (!U.nb) U.nb = aligned_alloc(128, (size_t)U.cfg.w * U.cfg.h * 2);
    return U.nb;
#else
    return NULL;
#endif
}

void bz_ui_slide_nb(int side)
{
#if BZ_LEAN
    if (side == U.nb_side) return;
    U.nb_side = side;
    U.slide_shown = 0x7fffffff; /* show it (or take it away) next frame even if the finger is still */
#else
    (void)side;
#endif
}

void bz_ui_slide(int dx)
{
#if BZ_LEAN
    if (U.sliding) U.slide_dx = dx;
#else
    (void)dx;
#endif
}

void bz_ui_slide_end(void)
{
#if BZ_LEAN
    if (!U.sliding) return;
    U.sliding = false;
    if (U.cfg.slide) U.cfg.slide->end();
    lv_obj_invalidate(lv_display_get_screen_active(U.disp_content));
    lv_obj_invalidate(lv_display_get_layer_top(U.disp_content));
#endif
}

/* ------------------------------------------------------------------ motion caches */

void bz_ui_render_offscreen(uint16_t *buf, int stride, const lv_area_t *area, void (*prepare)(bool before, void *u),
                            void *u)
{
    lv_display_t *d = U.disp_content;
    lv_obj_t *scr = lv_display_get_screen_active(d);
    /* anything already pending belongs on screen, not in the picture */
    if (!U.frozen) lv_refr_now(d);
    /* LVGL's invalidation switch is a counter: every disable here is matched by an enable */
    lv_display_enable_invalidation(d, false);
    if (prepare) prepare(true, u);
    lv_obj_update_layout(scr);

    /* point the display at the picture: a draw buffer the screen's size whose rows are `stride` apart,
     * starting so that `area` lands on buf */
    static lv_draw_buf_t db;
    uint32_t stride_b = (uint32_t)stride * 2;
    uint8_t *origin = (uint8_t *)buf - (size_t)area->y1 * stride_b - (size_t)area->x1 * 2;
    lv_draw_buf_init(&db, (uint32_t)U.cfg.w, (uint32_t)U.cfg.h, LV_COLOR_FORMAT_RGB565, stride_b, origin,
                     stride_b * (uint32_t)U.cfg.h);
    lv_display_set_draw_buffers(d, &db, NULL);
    U.offscreen = true;
    lv_display_enable_invalidation(d, true);
    if (U.frozen) lv_display_enable_invalidation(d, true);
    lv_obj_invalidate_area(scr, area);
    lv_refr_now(d);
    if (U.frozen) lv_display_enable_invalidation(d, false);
    lv_display_enable_invalidation(d, false);
    U.offscreen = false;
    lv_display_set_draw_buffers(d, U.own_buf, NULL);

    if (prepare) prepare(false, u);
    lv_obj_update_layout(scr);
    lv_display_enable_invalidation(d, true);
}

void bz_ui_freeze(bool frozen)
{
    /* counted: the pager, an app window and the control center may each hold it */
    static int holds;
    holds += frozen ? 1 : -1;
    if (holds < 0) holds = 0;
    bool f = holds > 0;
    if (f == U.frozen) return;
    if (f) thaw_finish(); /* what freezes now must be whole: a picture may be taken of it */
    /* layout moved while frozen settles while still quiet: the thaw redraws everything anyway */
    else lv_obj_update_layout(lv_display_get_screen_active(U.disp_content));
    U.frozen = f;
    lv_display_enable_invalidation(U.disp_content, !f);
    if (!f) U.thaw = 0;
#ifndef ESP_PLATFORM
    if (getenv("SIM_DEBUG_FLUSH"))
        fprintf(stderr, "freeze %d holds %d enabled %d at %.3f\n", frozen, holds,
                lv_display_is_invalidation_enabled(U.disp_content), U.now);
#endif
}

bool bz_ui_frozen(void) { return U.frozen; }

void bz_ui_copy(uint16_t *dst, int dst_stride, const uint16_t *src, int src_stride, int w, int h)
{
    if (U.cfg.ops && U.cfg.ops->copy565) U.cfg.ops->copy565(dst, dst_stride, src, src_stride, w, h);
    else for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)w * 2);
}

uint16_t *bz_ui_content_buf(void)
{
    thaw_finish(); /* whoever reads it gets it whole */
    return U.cfg.content;
}

/* Whether `a` (the list's rect) shows its own pixels and nothing else: every ancestor holds it whole
 * (clear of its rounded corners), none fades or scales it, and nothing drawn later (a later sibling of
 * it or of an ancestor, or the top layer) reaches into it. */
static bool unobstructed(lv_obj_t *o, const lv_area_t *a)
{
    for (lv_obj_t *p = lv_obj_get_parent(o); p; o = p, p = lv_obj_get_parent(p)) {
        lv_area_t pc;
        lv_obj_get_coords(p, &pc);
        if (!lv_area_is_in(a, &pc, lv_obj_get_style_radius(p, 0))) return false;
        if (lv_obj_get_style_opa(p, 0) < LV_OPA_MAX || lv_obj_get_style_transform_scale_x(p, 0) != LV_SCALE_NONE ||
            lv_obj_get_style_transform_scale_y(p, 0) != LV_SCALE_NONE)
            return false;
        uint32_t n = lv_obj_get_child_count(p);
        for (uint32_t i = (uint32_t)lv_obj_get_index(o) + 1; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(p, (int32_t)i);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_area_t cc;
            lv_obj_get_coords(c, &cc);
            int32_t ext = lv_obj_get_ext_draw_size(c);
            lv_area_increase(&cc, ext, ext);
            if (lv_area_is_on(&cc, a)) return false;
        }
    }
    lv_obj_t *top = lv_display_get_layer_top(U.disp_content);
    for (uint32_t i = 0; top && i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
        lv_area_t cc;
        lv_obj_get_coords(c, &cc);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && lv_area_is_on(&cc, a)) return false;
    }
    return true;
}

bool bz_ui_scroll(lv_obj_t *clip, lv_obj_t *content, int32_t y)
{
    lv_display_t *d = U.disp_content;
    /* layout already pending elsewhere invalidates as it normally would; only this move is kept quiet */
    lv_obj_update_layout(content);
    lv_area_t a, c0, c1;
    lv_obj_get_coords(clip, &a);
    lv_obj_get_coords(content, &c0);
    lv_display_enable_invalidation(d, false);
    lv_obj_set_y(content, y);
    lv_obj_update_layout(content);
    lv_display_enable_invalidation(d, true);
    lv_obj_get_coords(content, &c1);
    int dy = (int)(c1.y1 - c0.y1), h = (int)lv_area_get_height(&a), w = (int)lv_area_get_width(&a);
    if (!dy) return true;
    if (U.frozen || U.offscreen || U.thaw >= 0 || abs(dy) >= h || lv_obj_get_style_radius(clip, 0) || !unobstructed(clip, &a)) {
        lv_obj_invalidate(clip); /* what a plain move would have: both positions, clipped to the list */
        return false;
    }
    /* damage still waiting to be drawn inside the list moves with it: those pixels are stale wherever
     * they land (the damage stays where it was too, and redraws whatever scrolls into it) */
    uint32_t n = d->inv_p;
    for (uint32_t i = 0; i < n && i < LV_INV_BUF_SIZE; i++) {
        lv_area_t m;
        if (d->inv_area_joined[i] || !lv_area_intersect(&m, &d->inv_areas[i], &a)) continue;
        m.y1 += dy;
        m.y2 += dy;
        if (lv_area_intersect(&m, &m, &a)) lv_inv_area(d, &m);
    }
    uint16_t *px = U.cfg.content + a.x1;
    size_t W = (size_t)U.cfg.w, row = (size_t)w * 2;
    if (dy < 0)
        for (int r = a.y1; r <= a.y2 + dy; r++) memcpy(px + r * W, px + (r - dy) * W, row);
    else
        for (int r = a.y2; r >= a.y1 + dy; r--) memcpy(px + r * W, px + (r - dy) * W, row);
    U.shift_px += (uint32_t)(w * (h - abs(dy)));
    bz_area_t b = { (int16_t)a.x1, (int16_t)a.y1, (int16_t)a.x2, (int16_t)a.y2 };
    damage_content(&b);
    /* LVGL draws only the strip that scrolled into view */
    lv_area_t band = a;
    if (dy < 0) band.y1 = a.y2 + dy + 1;
    else band.y2 = a.y1 + dy - 1;
    lv_inv_area(d, &band);
    return true;
}

void bz_ui_set_mode(bool dark, bool calm)
{
#if BZ_LEAN
    calm = true; /* no glass to draw: the shapes are always solid */
#endif
    bool changed = dark != U.dark || calm != U.calm;
    U.dark = dark;
    U.calm = calm;
    bz_pal = dark ? &BZ_PAL_DARK : &BZ_PAL_LIGHT;
    bz_motion_set_calm(calm);
    bz_comp_set_mode(U.comp, dark, calm);
    if (changed) {
        bz_theme_changed();
        lv_obj_invalidate(bz_ui_content());
        lv_obj_invalidate(bz_ui_glass());
    }
}

bool bz_ui_dark(void) { return U.dark; }
bool bz_ui_calm(void) { return U.calm; }

void bz_ui_lean_light(float dx, float dy)
{
    /* Bezel's light rests at (-0.42, -0.91) and leans by (dx·1.1, dy·0.9), renormalized (app.js:113-148) */
    float x = -0.42f + dx * 1.1f, y = -0.91f + dy * 0.9f;
    float l = sqrtf(x * x + y * y);
    if (l < 1e-3f) return;
    if (U.calm) { x = -0.42f; y = -0.91f; l = 1.0f; }
    bz_motion_to(&U.lx, x / l, BZ_LIGHT);
    bz_motion_to(&U.ly, y / l, BZ_LIGHT);
}

/* ------------------------------------------------------------------ gestures */


static void drag_event(lv_event_t *e)
{
    drag_state_t *s = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    double t = bz_motion_now();

    if (code == LV_EVENT_PRESSED) {
        s->tracking = true;
        s->start = p;
        return;
    }
    if (!s->tracking) return;
    int dx = p.x - s->start.x, dy = p.y - s->start.y;
    if (code == LV_EVENT_PRESSING) {
        if (!g_claimed) {
            int adx = abs(dx), ady = abs(dy);
            int slop = s->d.slop ? s->d.slop : 10;
            if (adx < slop && ady < slop) return;
            bool horiz = adx > ady;
            if (s->d.axis == 1 && !horiz) return;
            if (s->d.axis == 2 && horiz) return;
            g_claimed = s;
            g_press_claimed = true;
            if (s->d.begin) s->d.begin(lv_event_get_current_target(e), s->start, s->d.user);
        }
        if (g_claimed == s && s->d.move) {
            s->d.move(lv_event_get_current_target(e), dx, dy, bz_velocity_get(&g_vx, t), bz_velocity_get(&g_vy, t),
                      s->d.user);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s->tracking = false;
        if (g_claimed == s) {
            g_claimed = NULL;
            g_drag_end = t;
            if (s->d.end) s->d.end(lv_event_get_current_target(e), dx, dy, bz_velocity_get(&g_vx, t),
                                   bz_velocity_get(&g_vy, t), s->d.user);
        }
        return;
    }
    if (code == LV_EVENT_DELETE) {
        if (g_claimed == s) g_claimed = NULL;
        free(s);
    }
}

void bz_drag_attach(lv_obj_t *obj, const bz_drag_t *d)
{
    drag_state_t *s = lv_malloc_zeroed(sizeof *s); /* per widget: PSRAM, not internal RAM */
    if (!s) return;
    s->d = *d;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, drag_event, LV_EVENT_ALL, s);
}

typedef struct {
    bz_tap_fn cb;
    void *user;
    lv_point_t down; /* where the press began */
} tap_t;

/* How far a press may travel and still be a tap. LVGL keeps an object pressed when the finger slides off
 * it (PRESS_LOCK) and clicks it on release wherever that is: a drag that happened to start on a button
 * would press the button. */
#define TAP_SLOP 24

static void (*s_on_any_tap)(void);
void bz_ui_on_any_tap(void (*fn)(void)) { s_on_any_tap = fn; }

static void tap_event(lv_event_t *e)
{
    tap_t *t = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(t);
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (code == LV_EVENT_PRESSED) {
        if (in) lv_indev_get_point(in, &t->down);
        return;
    }
    if (g_press_claimed) return; /* a drag's release is not a tap */
    if (in) {
        lv_point_t up;
        lv_indev_get_point(in, &up);
        if (LV_ABS(up.x - t->down.x) > TAP_SLOP || LV_ABS(up.y - t->down.y) > TAP_SLOP) return;
    }
    if (s_on_any_tap) s_on_any_tap();
    t->cb(lv_event_get_current_target(e), t->user);
}

void bz_on_tap(lv_obj_t *obj, bz_tap_fn cb, void *user)
{
    tap_t *t = lv_malloc(sizeof *t); /* per widget: PSRAM, not internal RAM */
    if (!t) return;
    t->cb = cb;
    t->user = user;
    t->down = (lv_point_t){ 0, 0 };
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, tap_event, LV_EVENT_PRESSED, t);
    lv_obj_add_event_cb(obj, tap_event, LV_EVENT_CLICKED, t);
    lv_obj_add_event_cb(obj, tap_event, LV_EVENT_DELETE, t);
}

bool bz_drag_active(void) { return g_claimed != NULL; }
bool bz_drag_recent(void) { return g_press_claimed || (bz_motion_now() - g_drag_end) < 0.4; }
