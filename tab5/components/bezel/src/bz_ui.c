#include "bz_ui.h"
#include "bz_tokens.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    bool alive;
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
    bz_area_t b = { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 };
    bz_comp_damage_content(U.comp, &b);
    lv_display_flush_ready(d);
}

static void flush_glass(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
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

    U.disp_content = lv_display_create(cfg->w, cfg->h);
    lv_display_set_color_format(U.disp_content, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(U.disp_content, cfg->content, NULL, (uint32_t)(cfg->w * cfg->h * 2),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(U.disp_content, flush_content);

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

bool bz_ui_frame(double now_s)
{
    double dt = U.last > 0 ? now_s - U.last : 0.016;
    U.last = U.now = now_s;
    bz_motion_clock(now_s);
    U.keep_alive = false;

    poll_touch();
    lv_indev_read(U.in_content);
    lv_indev_read(U.in_glass);

    for (int i = 0; i < U.nhooks; i++) U.hooks[i].fn(now_s, dt, U.hooks[i].user);

    bool moving = bz_motion_tick(&U.lx) | bz_motion_tick(&U.ly);
    bz_comp_set_light(U.comp, U.lx.value, U.ly.value);

    lv_timer_handler();
    moving |= update_glass();

    bz_area_t areas[BZ_COMP_MAX_DIRTY];
    int n = bz_comp_compose(U.comp, areas, BZ_COMP_MAX_DIRTY);
    if (n && U.cfg.present) U.cfg.present(areas, n, U.cfg.user);
    return moving || U.keep_alive || U.pressed;
}

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
