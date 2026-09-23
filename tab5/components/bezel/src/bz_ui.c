#include "bz_ui.h"
#include "bz_tokens.h"

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
    /* motion caches */
    bool frozen, offscreen;
    lv_draw_buf_t *own_buf;
    /* performance */
    uint32_t lvgl_px;
    bz_ui_perf_t perf;
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

static void flush_content(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    U.lvgl_px += (uint32_t)lv_area_get_size(a);
    if (!U.offscreen) {
#ifndef ESP_PLATFORM
        if (getenv("SIM_DEBUG_FLUSH") && lv_area_get_size(a) > 200000)
            fprintf(stderr, "flush content %d,%d-%d,%d\n", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
#endif
        bz_area_t b = { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 };
        bz_comp_damage_content(U.comp, &b);
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

static void flush_glass(lv_display_t *d, const lv_area_t *a, uint8_t *px)
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
    if (p) {
        U.tx = x;
        U.ty = y;
        U.last_touch = U.now;
        if (!U.pressed) {
            U.owner = clickable_at(lv_display_get_screen_active(U.disp_glass), x, y) ? 2 : 1;
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
lv_obj_t *bz_ui_glass(void) { return lv_display_get_screen_active(U.disp_glass); }
bz_comp_t *bz_ui_comp(void) { return U.comp; }

static uint32_t tick_ms(void) { return (uint32_t)(U.now * 1000.0); }

void bz_ui_init(const bz_ui_config_t *cfg)
{
    memset(&U, 0, sizeof U);
    U.cfg = *cfg;
    U.dark = true;
    lv_tick_set_cb(tick_ms);
    U.comp = bz_comp_create(cfg->w, cfg->h, cfg->content, cfg->ink, cfg->out);
    if (cfg->ops) bz_comp_set_ops(U.comp, cfg->ops);
    uint16_t *out2 = aligned_alloc(64, (size_t)cfg->w * cfg->h * 2);
    if (out2) bz_comp_set_out2(U.comp, out2);

    U.disp_content = lv_display_create(cfg->w, cfg->h);
    lv_display_set_color_format(U.disp_content, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(U.disp_content, cfg->content, NULL, (uint32_t)(cfg->w * cfg->h * 2),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(U.disp_content, flush_content);
    U.own_buf = lv_display_get_buf_active(U.disp_content);
#ifndef ESP_PLATFORM
    if (getenv("SIM_DEBUG_FLUSH")) lv_display_add_event_cb(U.disp_content, debug_inv, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

    U.disp_glass = lv_display_create(cfg->w, cfg->h);
    lv_display_set_color_format(U.disp_glass, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(U.disp_glass, cfg->ink, NULL, (uint32_t)(cfg->w * cfg->h * 4),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(U.disp_glass, flush_glass);
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
void bz_ui_wake(void) { U.last_touch = U.now; }

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
static float model_ms(const bz_comp_stats_t *s, uint32_t lvgl_px)
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
    double sync = 0, async = 0;
    sync += (double)s->composed_px * 4;              /* the base copied into out: the cores wait on it */
    sync += (double)s->ink_px * 8;
    async += (double)(s->composed_px + s->direct_px) * 4; /* the turn into the panel's buffer */
    cpu_ms += sync / (PPA_MBS * 1000.0);
    double ppa_ms = (sync + async) / (PPA_MBS * 1000.0);
    return (float)((cpu_ms > ppa_ms ? cpu_ms : ppa_ms) + 0.5);
}

bool bz_ui_frame(double now_s)
{
    double dt = U.last > 0 ? now_s - U.last : 0.016;
    U.last = U.now = now_s;
    bz_motion_clock(now_s);
    U.keep_alive = false;
    U.lvgl_px = 0;
    double t0 = wall();

    poll_touch();
    lv_indev_read(U.in_content);
    lv_indev_read(U.in_glass);

    for (int i = 0; i < U.nhooks; i++) U.hooks[i].fn(now_s, dt, U.hooks[i].user);

    bool moving = bz_motion_tick(&U.lx) | bz_motion_tick(&U.ly);
    bz_comp_set_light(U.comp, U.lx.value, U.ly.value);

    double t1 = wall();
    lv_timer_handler();
    double t2 = wall();
    moving |= update_glass();

    bz_present_t areas[BZ_COMP_MAX_PRESENT];
    int n = bz_comp_compose(U.comp, areas, BZ_COMP_MAX_PRESENT);
    double t3 = wall();
    if (n && U.cfg.present) U.cfg.present(areas, n, U.cfg.user);
    double t4 = wall();

    if (n) {
        bz_ui_perf_t *p = &U.perf;
        bz_comp_stats(U.comp, &p->comp);
        p->lvgl_px = U.lvgl_px;
        static double last_present;
        if (last_present > 0 && now_s > last_present) p->fps = ema(p->fps, (float)(1.0 / (now_s - last_present)));
        last_present = now_s;
        p->frame_ms = ema(p->frame_ms, (float)((t4 - t0) * 1000));
        p->lvgl_ms = ema(p->lvgl_ms, (float)((t2 - t1) * 1000));
        p->compose_ms = ema(p->compose_ms, (float)((t3 - t2) * 1000));
        p->present_ms = ema(p->present_ms, (float)((t4 - t3) * 1000));
        p->model_ms = model_ms(&p->comp, U.lvgl_px);
        p->frames++;
    }
    return moving || U.keep_alive || U.pressed;
}

void bz_ui_perf(bz_ui_perf_t *out) { *out = U.perf; }

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
    U.frozen = f;
    lv_display_enable_invalidation(U.disp_content, !f);
    if (!f) lv_obj_invalidate(lv_display_get_screen_active(U.disp_content));
}

bool bz_ui_frozen(void) { return U.frozen; }

void bz_ui_copy(uint16_t *dst, int dst_stride, const uint16_t *src, int src_stride, int w, int h)
{
    if (U.cfg.ops && U.cfg.ops->copy565) U.cfg.ops->copy565(dst, dst_stride, src, src_stride, w, h);
    else for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)w * 2);
}

uint16_t *bz_ui_content_buf(void) { return U.cfg.content; }

void bz_ui_set_mode(bool dark, bool calm)
{
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
    drag_state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->d = *d;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, drag_event, LV_EVENT_ALL, s);
}

typedef struct {
    bz_tap_fn cb;
    void *user;
} tap_t;

static void tap_event(lv_event_t *e)
{
    tap_t *t = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        free(t);
        return;
    }
    if (g_press_claimed) return; /* a drag's release is not a tap */
    t->cb(lv_event_get_current_target(e), t->user);
}

void bz_on_tap(lv_obj_t *obj, bz_tap_fn cb, void *user)
{
    tap_t *t = malloc(sizeof *t);
    if (!t) return;
    t->cb = cb;
    t->user = user;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, tap_event, LV_EVENT_CLICKED, t);
    lv_obj_add_event_cb(obj, tap_event, LV_EVENT_DELETE, t);
}

bool bz_drag_active(void) { return g_claimed != NULL; }
bool bz_drag_recent(void) { return g_press_claimed || (bz_motion_now() - g_drag_end) < 0.4; }
