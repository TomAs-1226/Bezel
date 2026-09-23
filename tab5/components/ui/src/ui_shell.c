/* The shell: pages side by side under a glass dock, the island, app windows, and the 10 Hz model loop.
 *
 * Structure follows Bezel's panel.js: slots paged horizontally on the `page` spring from a projected
 * flick; a dock whose droplet can be dragged between items and lands on the nearest (projection plus
 * snap); an island that carries status and morphs for a message; apps that grow out of the icon that
 * opened them and can be caught mid-flight or dragged down to close. */
#include "ui_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define NPAGES 5
#define DOCK_ITEM_W 104
#define DOCK_ITEM_H 76
#define DOCK_GAP 4
#define DOCK_PAD 8
#define DOCK_W (NPAGES * DOCK_ITEM_W + (NPAGES - 1) * DOCK_GAP + 2 * DOCK_PAD)
#define DOCK_H 92

cat_robot_t *R;
ui_settings_t S = { .team = 5805, .brightness = 0.8f, .volume = 0.5f, .dark = true };

static const char *const PAGE_NAMES[NPAGES] = { "pulse", "devices", "power", "motion", "tools" };
static const char *const PAGE_ICONS[NPAGES] = { BZ_I_MONITOR_HEART, BZ_I_HUB, BZ_I_BOLT, BZ_I_PRECISION_MANUFACTURING,
                                                BZ_I_APPS };

static struct {
    ui_config_t cfg;
    lv_obj_t *track, *pages[NPAGES];
    bz_motion_t offset;          /* track offset in px (0 … -(N-1)·W) */
    int page;
    float drag_from;

    lv_obj_t *dock, *dock_items[NPAGES], *dock_icons[NPAGES], *dock_labels[NPAGES], *platter, *droplet;
    bz_glass_t *dock_glass, *droplet_glass;
    bz_motion_t dock_tuck;       /* 0 shown, 1 tucked away */
    bz_motion_t drop_x, drop_lift;
    bool drop_dragging;

    lv_obj_t *island, *island_mark, *island_text, *island_icon, *island_tail;
    bz_glass_t *island_glass;
    double island_until;

    struct { ui_refresh_fn fn; void *user; } refresh[32];
    int nrefresh;
    double last_refresh;

    /* app windows */
    const ui_app_t *app;
    lv_obj_t *win, *win_inner, *pill, *pill_label, *pill_icon;
    bz_glass_t *pill_glass;
    lv_area_t from;
    bz_motion_t k;
    struct { const ui_app_t *app; lv_obj_t *inner; } built[16];
    int nbuilt;
    float drag_k0;
    bool win_dragging;
} U;

/* Motion caches (bz_ui.h): while the pages swipe, they are one picture — all five side by side — that
 * the compositor slides; while an app window grows out of its icon or shrinks back, the window is a
 * picture over a picture of the page. LVGL draws nothing while they move; it redraws once when they
 * land. The page pictures are refreshed a few times a second while nothing moves, so a swipe starts
 * without waiting for one. */
static struct {
    uint16_t *strip;         /* (NPAGES + 1)·W wide: the pages, half a page of ground either side */
    int stride;
    bool ok[NPAGES];
    double t[NPAGES];        /* when each was drawn */
    bool pages;              /* the pager is showing the strip */
    int track_page;          /* the page LVGL's track is placed at */
    uint16_t *win;           /* the app window, full screen */
    bool window;             /* the window is showing its picture */
    bool dark;
    double last_render;
} MC;

static double g_now;
double ui_now(void) { return g_now; }

/* ------------------------------------------------------------------ helpers */

void ui_text(lv_obj_t *label, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, buf)) lv_label_set_text(label, buf);
}

bz_status_t ui_sev_status(cat_sev_t s)
{
    return s == CAT_SEV_ERROR ? BZ_FAULT : s == CAT_SEV_WARN ? BZ_WARN : BZ_INFO;
}

bz_status_t ui_battery_status(double v)
{
    int band = cat_battery_band(v);
    return band == 2 ? BZ_OK : band == 1 ? BZ_WARN : BZ_FAULT;
}

void ui_on_refresh(ui_refresh_fn fn, void *user)
{
    if (U.nrefresh < 32) {
        U.refresh[U.nrefresh].fn = fn;
        U.refresh[U.nrefresh].user = user;
        U.nrefresh++;
    }
}

lv_obj_t *ui_head(lv_obj_t *page, const char *title, const char *label)
{
    lv_obj_t *col = bz_col(page, 2);
    lv_obj_set_pos(col, PAD, HEAD_Y - 4);
    bz_label(col, label, BZ_F_LABEL, BZ_C_DIM);
    bz_label(col, title, BZ_F_TITLE, BZ_C_INK);
    lv_obj_t *right = bz_row(page, 14);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -PAD, HEAD_Y + 14);
    return right;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *icon, const char *text, bz_tap_fn fn, void *user)
{
    lv_obj_t *b = bz_tile(parent, LV_SIZE_CONTENT, 56);
    bz_tile_set_fill(b, BZ_C_SURFACE2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_set_ext_click_area(b, 12);
    if (icon) bz_icon(b, icon, 24, BZ_C_INK);
    if (text) bz_label(b, text, BZ_F_LABEL, BZ_C_INK);
    if (fn) bz_on_tap(b, fn, user);
    return b;
}

lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, bz_tap_fn fn, void *user)
{
    lv_obj_t *b = ui_button(parent, NULL, text, fn, user);
    lv_obj_set_style_radius(b, BZ_R_M, 0);
    return b;
}

void ui_chip_set(lv_obj_t *chip, bool on)
{
    bz_tile_set_fill(chip, on ? BZ_C_ICE : BZ_C_SURFACE2);
    uint32_t n = lv_obj_get_child_count(chip);
    for (uint32_t i = 0; i < n; i++) bz_set_color(lv_obj_get_child(chip, (int32_t)i), on ? BZ_C_ON_ICE : BZ_C_INK);
    /* Bezel's mode buttons: corners open from 12 to full when selected */
    lv_obj_set_style_radius(chip, on ? LV_RADIUS_CIRCLE : BZ_R_M, 0);
}

/* ------------------------------------------------------------------ scroller */

typedef struct {
    lv_obj_t *clip, *content;
    bz_motion_t y;
    float start;
    bool dragging;
} scroller_t;

static scroller_t *g_scrollers[16];

static float scroll_min(scroller_t *s)
{
    lv_obj_update_layout(s->content);
    float over = (float)lv_obj_get_height(s->content) - lv_obj_get_height(s->clip);
    return over > 0 ? -over : 0;
}

static void sc_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p;
    scroller_t *s = u;
    s->dragging = true;
    s->start = s->y.value;
    bz_motion_set(&s->y, s->y.value, 0);
}

static void sc_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx;
    scroller_t *s = u;
    float y = bz_rubber_clamp(s->start + dy, scroll_min(s), 0, (float)lv_obj_get_height(s->clip));
    bz_motion_set(&s->y, y, vy);
    lv_obj_set_y(s->content, (int)y);
}

static void sc_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx;
    scroller_t *s = u;
    s->dragging = false;
    float lo = scroll_min(s), y = s->y.value;
    if (y > 0 || y < lo) {
        /* released past an edge: spring straight back to the bound, no velocity */
        bz_motion_to_v(&s->y, y > 0 ? 0 : lo, BZ_EDGE, 0);
        return;
    }
    /* coast: aim at where the fling projects (deceleration 0.998), landing on the edge spring if it
     * would leave the bounds */
    float aim = y + bz_project(vy, BZ_RATE_NORMAL);
    if (aim > 0 || aim < lo) bz_motion_to_v(&s->y, aim > 0 ? 0 : lo, BZ_EDGE, vy > 5000 ? 5000 : vy < -5000 ? -5000 : vy);
    else bz_motion_to_v(&s->y, aim, BZ_SMOOTH, vy);
}

static void scroll_frame(double now, double dt, void *user)
{
    (void)now; (void)dt; (void)user;
    for (int i = 0; i < 16; i++) {
        scroller_t *s = g_scrollers[i];
        if (!s || s->dragging) continue;
        if (bz_motion_tick(&s->y)) {
            lv_obj_set_y(s->content, (int)s->y.value);
            bz_ui_keep_alive();
        }
    }
}

lv_obj_t *ui_scroller(lv_obj_t *parent, int w, int h)
{
    scroller_t *s = calloc(1, sizeof *s);
    s->clip = bz_box(parent);
    lv_obj_set_size(s->clip, w, h);
    s->content = bz_col(s->clip, BZ_GAP);
    lv_obj_set_width(s->content, w);
    bz_motion_init(&s->y, 0, 0.1f);
    bz_drag_t d = { .begin = sc_begin, .move = sc_move, .end = sc_end, .user = s, .axis = 2, .slop = 10 };
    bz_drag_attach(s->clip, &d);
    lv_obj_add_flag(s->clip, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < 16; i++) if (!g_scrollers[i]) { g_scrollers[i] = s; break; }
    return s->content;
}

/* ------------------------------------------------------------------ pager */

static uint16_t *strip_page(int i) { return MC.strip + W / 2 + (size_t)i * W; }

static void strip_layer(float offset)
{
    float lo = -(float)(NPAGES - 1) * W - W / 2.0f, hi = W / 2.0f;
    int off = (int)lroundf(offset < lo ? lo : offset > hi ? hi : offset);
    bz_layer_t l = { { 0, 0, W - 1, H - 1 }, 0, MC.strip, off - W / 2, 0, MC.stride };
    bz_comp_set_layer(bz_ui_comp(), 0, &l);
}

static void place_track(void)
{
    if (MC.pages) strip_layer(U.offset.value);
    else lv_obj_set_x(U.track, (int)lroundf(U.offset.value));
}

void ui_go(int page)
{
    if (page < 0) page = 0;
    if (page >= NPAGES) page = NPAGES - 1;
    U.page = page;
    bz_motion_to(&U.offset, -(float)page * W, BZ_PAGE);
    bz_ui_keep_alive();
}

int ui_page(void) { return U.page; }
lv_obj_t *ui_page_body(int page) { return U.pages[page]; }

static void pg_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
    U.drag_from = U.offset.value;
    bz_motion_set(&U.offset, U.offset.value, 0);
}

static void pg_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vy; (void)u;
    float x = bz_rubber_clamp(U.drag_from + dx, -(float)(NPAGES - 1) * W, 0, W);
    bz_motion_set(&U.offset, x, vx);
    place_track();
}

static void pg_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vy; (void)u;
    /* pageFor: one page at a time from a projected flick (motion.js:141-144) */
    int from = U.page;
    int aim = (int)lroundf(-(U.offset.value + bz_project(vx, BZ_RATE_NORMAL)) / W);
    if (aim < from - 1) aim = from - 1;
    if (aim > from + 1) aim = from + 1;
    if (aim < 0) aim = 0;
    if (aim > NPAGES - 1) aim = NPAGES - 1;
    U.page = aim;
    bool out = U.offset.value > 0 || U.offset.value < -(float)(NPAGES - 1) * W;
    bz_motion_to_v(&U.offset, -(float)aim * W, BZ_PAGE, out ? 0 : vx);
}

/* ------------------------------------------------------------------ dock */

static float item_cx(int i)
{
    return (W - DOCK_W) / 2.0f + DOCK_PAD + i * (DOCK_ITEM_W + DOCK_GAP) + DOCK_ITEM_W / 2.0f;
}

static void dock_style_items(float pos)
{
    int cur = (int)lroundf(pos);
    for (int i = 0; i < NPAGES; i++) {
        bool on = i == cur;
        bz_set_color(U.dock_icons[i], on ? BZ_C_INK : BZ_C_DIM);
        bz_set_color(U.dock_labels[i], on ? BZ_C_INK : BZ_C_DIM);
        bz_icon_set(U.dock_icons[i], PAGE_ICONS[i], 32, on);
    }
}

static void dock_tap(lv_obj_t *o, void *u)
{
    (void)o;
    bz_ui_wake();
    ui_go((int)(intptr_t)u);
}

static void dk_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)u;
    U.drop_dragging = true;
    float x = (float)p.x;
    bz_motion_set(&U.drop_x, x, 0);
    bz_motion_to(&U.drop_lift, 1, BZ_HOLD);
    bz_glass_show(U.droplet_glass, true);
}

static void dk_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vy; (void)u;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    float x = bz_rubber_clamp((float)p.x, item_cx(0), item_cx(NPAGES - 1), 120);
    bz_motion_set(&U.drop_x, x, vx);
    (void)dx;
}

static void dk_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vy; (void)u;
    U.drop_dragging = false;
    float targets[NPAGES];
    for (int i = 0; i < NPAGES; i++) targets[i] = item_cx(i);
    float x = bz_snap_target(U.drop_x.value, vx, targets, NPAGES);
    int page = 0;
    for (int i = 0; i < NPAGES; i++) if (targets[i] == x) page = i;
    bz_motion_to_v(&U.drop_x, x, BZ_RELEASE, vx);
    bz_motion_to(&U.drop_lift, 0, BZ_WOBBLE);
    ui_go(page);
}

static void build_dock(void)
{
    lv_obj_t *g = bz_ui_glass();
    U.dock = lv_obj_create(g);
    lv_obj_remove_style_all(U.dock);
    lv_obj_set_size(U.dock, DOCK_W, DOCK_H);
    lv_obj_align(U.dock, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_remove_flag(U.dock, LV_OBJ_FLAG_SCROLLABLE);
    U.dock_glass = bz_glass_attach(U.dock, 0, BZ_R_DOCK);
    bz_glass_set_press_scale(U.dock_glass, BZ_GLASS_DOCK_PRESS_SCALE);

    /* the platter under the current item: 7.5 % ink at rest (Bezel's droplet, landed) */
    U.platter = lv_obj_create(U.dock);
    lv_obj_remove_style_all(U.platter);
    lv_obj_set_size(U.platter, DOCK_ITEM_W, DOCK_ITEM_H);
    lv_obj_add_style(U.platter, bz_style_fill(BZ_C_INK), 0);
    lv_obj_set_style_bg_opa(U.platter, 19, 0);
    lv_obj_set_style_radius(U.platter, BZ_R_DOCK_ITEM, 0);
    lv_obj_remove_flag(U.platter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(U.platter, LV_OBJ_FLAG_IGNORE_LAYOUT);

    for (int i = 0; i < NPAGES; i++) {
        lv_obj_t *it = bz_col(U.dock, 2);
        lv_obj_set_size(it, DOCK_ITEM_W, DOCK_ITEM_H);
        lv_obj_set_pos(it, DOCK_PAD + i * (DOCK_ITEM_W + DOCK_GAP), DOCK_PAD);
        lv_obj_set_flex_align(it, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_ext_click_area(it, 8);
        U.dock_icons[i] = bz_icon(it, PAGE_ICONS[i], 32, BZ_C_DIM);
        U.dock_labels[i] = bz_label(it, PAGE_NAMES[i], BZ_F_CAPTION, BZ_C_DIM);
        U.dock_items[i] = it;
        bz_on_tap(it, dock_tap, (void *)(intptr_t)i);
    }
    bz_drag_t d = { .begin = dk_begin, .move = dk_move, .end = dk_end, .axis = 1, .slop = 10 };
    bz_drag_attach(U.dock, &d);

    /* the droplet: a second glass shape in the dock's group, so it melts into the dock when lifted */
    U.droplet = lv_obj_create(g);
    lv_obj_remove_style_all(U.droplet);
    lv_obj_remove_flag(U.droplet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(U.droplet, 1, 1);
    U.droplet_glass = bz_glass_attach(U.droplet, 0, BZ_R_DOCK_ITEM);
    bz_glass_show(U.droplet_glass, false);
    bz_motion_init(&U.drop_x, item_cx(0), 0.1f);
    bz_motion_init(&U.drop_lift, 0, 0.002f);
    bz_motion_init(&U.dock_tuck, 0, 0.002f);
}

static void dock_frame(void)
{
    float pos = -U.offset.value / W;
    dock_style_items(pos);
    /* the platter follows the pager continuously */
    float px = DOCK_PAD + pos * (DOCK_ITEM_W + DOCK_GAP);
    lv_obj_set_pos(U.platter, (int)lroundf(px), DOCK_PAD);

    bool a = bz_motion_tick(&U.drop_x), b = bz_motion_tick(&U.drop_lift);
    if (a || b || U.drop_dragging) bz_ui_keep_alive();
    float lift = U.drop_lift.value < 0 ? 0 : U.drop_lift.value;
    if (lift > 0.01f || U.drop_dragging) {
        /* droplet (panel.js:491-502): stretches with speed, lifts 2 px, grows while held */
        float st = fminf(0.04f * fabsf(U.drop_x.velocity), 30);
        if (bz_ui_calm()) st = 0;
        float w = DOCK_ITEM_W * (1 + 0.12f * lift) + st;
        float h = DOCK_ITEM_H * (1 + 0.42f * lift) / (1 + st / (1.6f * DOCK_ITEM_W));
        float cy = H - 30 - DOCK_H / 2.0f - 2 * lift;
        bz_glass_set_rect(U.droplet_glass, true, U.drop_x.value - w / 2, cy - h / 2, w, h);
        lv_obj_set_style_bg_opa(U.platter, (lv_opa_t)(19 * (1 - lift)), 0);
    } else if (bz_glass_strength(U.droplet_glass) > 0.5f && !U.drop_dragging) {
        bz_glass_show(U.droplet_glass, false);
        lv_obj_set_style_bg_opa(U.platter, 19, 0);
    }

    /* the dock tucks away while an app is open, and after 4 s idle on a page (panel.js:34,166) */
    bool tuck = U.app != NULL;
    if (bz_motion_tick(&U.dock_tuck)) bz_ui_keep_alive();
    float target = tuck ? 1 : 0;
    if (U.dock_tuck.target != target) {
        bz_motion_to(&U.dock_tuck, target, tuck ? BZ_SMOOTH : BZ_RELEASE);
        bz_glass_show(U.dock_glass, !tuck);
    }
    lv_obj_set_y(U.dock, (int)(-30 + 46 * U.dock_tuck.value));
    if (U.dock_tuck.value > 0.98f) lv_obj_add_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ island */

static void island_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_ROBOT, o);
}

static void build_island(void)
{
    lv_obj_t *g = bz_ui_glass();
    U.island = bz_row(g, 12);
    lv_obj_set_height(U.island, 60);
    lv_obj_set_style_pad_hor(U.island, 24, 0);
    lv_obj_align(U.island, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_remove_flag(U.island, LV_OBJ_FLAG_EVENT_BUBBLE);
    U.island_glass = bz_glass_attach(U.island, 3, BZ_R_ISLAND);
    U.island_mark = bz_mark(U.island, BZ_STALE, 12);
    U.island_icon = bz_icon(U.island, BZ_I_INFO, 24, BZ_C_INK);
    lv_obj_add_flag(U.island_icon, LV_OBJ_FLAG_HIDDEN);
    U.island_text = bz_label(U.island, "", BZ_F_LABEL, BZ_C_INK);
    U.island_tail = bz_label(U.island, "", BZ_F_LABEL, BZ_C_DIM);
    bz_on_tap(U.island, island_tap, NULL);
}

void ui_island_say(const char *icon, const char *text)
{
    U.island_until = g_now + 2.4;
    lv_obj_add_flag(U.island_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U.island_icon, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(U.island_icon, icon ? icon : BZ_I_INFO);
    ui_text(U.island_text, "%s", text);
    lv_obj_add_flag(U.island_tail, LV_OBJ_FLAG_HIDDEN);
    bz_glass_set_tint(U.island_glass, 0, 0.35f);
}

static void island_refresh(void)
{
    if (U.island_until && g_now < U.island_until) return;
    if (U.island_until) {
        U.island_until = 0;
        lv_obj_add_flag(U.island_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.island_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.island_tail, LV_OBJ_FLAG_HIDDEN);
        bz_glass_set_tint(U.island_glass, -1, 0);
    }
    const cat_robot_t *r = R;
    bz_status_t st = !r->connected ? BZ_STALE : r->n_errors || r->estop ? BZ_FAULT : r->n_warnings ? BZ_WARN : BZ_OK;
    bz_mark_set(U.island_mark, st);
    char batt[16];
    if (!r->connected) {
        ui_text(U.island_text, "looking for %d", S.team);
    } else {
        char name[40];
        snprintf(name, sizeof name, "%s", r->have_identity ? r->name : "robot");
        for (char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        ui_text(U.island_text, "%s · %s · %s v", name, cat_mode_name(r), bz_fmt(batt, sizeof batt, r->have_battery, "%.2f", r->battery_v));
    }
    hal_battery_t b;
    hal_net_t n;
    hal_net(&n);
    if (hal_battery(&b) && b.ok) ui_text(U.island_tail, "%s  %d%%", n.up ? "wi-fi" : "no link", b.percent);
}

/* ------------------------------------------------------------------ apps */

static lv_obj_t *app_inner(const ui_app_t *app)
{
    for (int i = 0; i < U.nbuilt; i++) if (U.built[i].app == app) return U.built[i].inner;
    lv_obj_t *inner = bz_box(U.win);
    lv_obj_set_size(inner, W, H);
    lv_obj_add_flag(inner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    app->build(inner);
    if (U.nbuilt < 16) {
        U.built[U.nbuilt].app = app;
        U.built[U.nbuilt].inner = inner;
        U.nbuilt++;
    }
    return inner;
}

static void pill_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_app_close();
}

static void win_layout(void)
{
    float k = U.k.value;
    float kc = k < 0 ? 0 : k;
    lv_area_t f = U.from;
    float x1 = f.x1 * (1 - kc), y1 = f.y1 * (1 - kc);
    float x2 = f.x2 + (W - 1 - f.x2) * kc, y2 = f.y2 + (H - 1 - f.y2) * kc;
    if (k > 1) { /* overshoot past full: grow a little beyond the screen, symmetric */
        float o = (k - 1) * 40;
        x1 -= o; y1 -= o; x2 += o; y2 += o;
    }
    lv_obj_set_pos(U.win, (int)x1, (int)y1);
    lv_obj_set_size(U.win, (int)(x2 - x1 + 1), (int)(y2 - y1 + 1));
    /* a style set redraws the whole window even when the value is the same */
    int radius = (int)(32 * (1 - (kc > 1 ? 1 : kc)));
    if (radius != lv_obj_get_style_radius(U.win, 0)) lv_obj_set_style_radius(U.win, radius, 0);
    if (MC.window) {
        bz_layer_t l = { { (int16_t)(x1 < 0 ? 0 : x1), (int16_t)(y1 < 0 ? 0 : y1), (int16_t)(x2 > W - 1 ? W - 1 : x2),
                           (int16_t)(y2 > H - 1 ? H - 1 : y2) },
                         /* in half pixels: a spring settling by hundredths shouldn't redraw the screen */
                         roundf(64 * (1 - (kc > 1 ? 1 : kc))) / 2, MC.win, 0, 0, W };
        if (k <= 0.01f && U.k.target == 0) bz_comp_set_layer(bz_ui_comp(), 1, NULL);
        else bz_comp_set_layer(bz_ui_comp(), 1, &l);
    }
    if (U.win_inner) lv_obj_set_pos(U.win_inner, -(int)x1, -(int)y1);
    if (k <= 0.01f && U.k.target == 0) {
        lv_obj_add_flag(U.win, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(U.win, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- motion caches ---- */

static void prep_page(bool before, void *u)
{
    static int x;
    static bool win_hidden;
    if (before) {
        x = lv_obj_get_x(U.track);
        win_hidden = lv_obj_has_flag(U.win, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(U.track, -(int)(intptr_t)u * W);
        lv_obj_add_flag(U.win, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_x(U.track, x);
        if (!win_hidden) lv_obj_remove_flag(U.win, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The window as it is when fully open, whatever it looks like now. */
static void prep_window(bool before, void *u)
{
    (void)u;
    if (before) {
        lv_obj_remove_flag(U.win, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(U.win, 0, 0);
        lv_obj_set_size(U.win, W, H);
        lv_obj_set_style_radius(U.win, 0, 0);
        if (U.win_inner) lv_obj_set_pos(U.win_inner, 0, 0);
    } else {
        win_layout();
    }
}

static bool caches_ready(void)
{
    if (MC.strip) return true;
    MC.stride = (NPAGES + 1) * W;
    MC.strip = aligned_alloc(64, (size_t)MC.stride * H * 2);
    MC.win = aligned_alloc(64, (size_t)W * H * 2);
    if (!MC.strip || !MC.win) {
        free(MC.strip);
        free(MC.win);
        MC.strip = MC.win = NULL;
        return false;
    }
    MC.dark = !bz_ui_dark(); /* forces the ground in the margins to be painted */
    return true;
}

static void render_page(int i)
{
    lv_area_t a = { 0, 0, W - 1, H - 1 };
    bz_ui_render_offscreen(strip_page(i), MC.stride, &a, prep_page, (void *)(intptr_t)i);
    MC.ok[i] = true;
    MC.t[i] = g_now;
    MC.last_render = g_now;
}

/* The page on screen, straight from the content buffer: it is exactly what LVGL has drawn. */
static void grab_page(int i)
{
    bz_ui_copy(strip_page(i), MC.stride, bz_ui_content_buf(), W, W, H);
    MC.ok[i] = true;
    MC.t[i] = g_now;
}

static void pages_begin(void)
{
    if (MC.pages || MC.window || U.app || !caches_ready()) return;
    grab_page(MC.track_page);
    for (int i = 0; i < NPAGES; i++) if (!MC.ok[i]) render_page(i);
    bz_ui_freeze(true);
    MC.pages = true;
    strip_layer(U.offset.value);
}

static void pages_end(void)
{
    if (!MC.pages) return;
    MC.pages = false;
    MC.track_page = U.page;
    lv_obj_set_x(U.track, -U.page * W); /* frozen: no redraw for the move */
    bz_comp_set_layer(bz_ui_comp(), 0, NULL);
    bz_ui_freeze(false);                /* one redraw, of the page as it is now */
}

static void window_begin(bool opening)
{
    if (MC.window || MC.pages || !caches_ready()) return;
    if (opening) {
        /* the page as it is under the window, and the window as it will be when full */
        grab_page(MC.track_page);
        lv_area_t a = { 0, 0, W - 1, H - 1 };
        bz_ui_render_offscreen(MC.win, W, &a, prep_window, NULL);
    } else {
        /* the window as it is on screen now; the page under it as last drawn */
        bz_ui_copy(MC.win, W, bz_ui_content_buf(), W, W, H);
        if (!MC.ok[MC.track_page]) render_page(MC.track_page);
    }
    bz_ui_freeze(true);
    MC.window = true;
    bz_layer_t page = { { 0, 0, W - 1, H - 1 }, 0, strip_page(MC.track_page), 0, 0, MC.stride };
    bz_comp_set_layer(bz_ui_comp(), 0, &page);
}

static void window_end(void)
{
    if (!MC.window) return;
    MC.window = false;
    bz_comp_set_layer(bz_ui_comp(), 0, NULL);
    bz_comp_set_layer(bz_ui_comp(), 1, NULL);
    bz_ui_freeze(false);
}

/* While nothing moves, keep the pictures fresh: the pages beside this one within 2 s, the others
 * within 10 s, and the one under an open app within 3 s. One drawing at a time, a few a second. */
static void caches_idle(void)
{
    if (!MC.strip && !caches_ready()) return;
    if (MC.dark != bz_ui_dark()) {
        MC.dark = bz_ui_dark();
        uint32_t c = bz_color(BZ_C_GROUND);
        uint16_t g = (uint16_t)(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
        for (int y = 0; y < H; y++) {
            uint16_t *row = MC.strip + (size_t)y * MC.stride;
            for (int x = 0; x < W / 2; x++) row[x] = row[MC.stride - 1 - x] = g;
        }
        for (int i = 0; i < NPAGES; i++) MC.ok[i] = false;
    }
    if (MC.pages || MC.window || bz_ui_idle_s() < 0.4 || g_now - MC.last_render < 0.3) return;
    if (U.k.value != U.k.target || fabsf(U.offset.value - U.offset.target) > 0.5f) return;
    int best = -1;
    double worst = 0;
    for (int i = 0; i < NPAGES; i++) {
        bool here = i == MC.track_page;
        if (here && !U.app) continue; /* on screen: grabbed, not drawn */
        double limit = here ? 3 : abs(i - MC.track_page) == 1 ? 2 : 10;
        double age = MC.ok[i] ? g_now - MC.t[i] : 1e9;
        if (age > limit && age - limit > worst) { worst = age - limit; best = i; }
    }
    if (best >= 0) render_page(best);
}

void ui_app_open(const ui_app_t *app, lv_obj_t *from)
{
    bz_ui_wake();
    if (U.app && U.app != app) {
        if (U.app->close) U.app->close();
        lv_obj_add_flag(U.win_inner, LV_OBJ_FLAG_HIDDEN);
    }
    bool reopening = U.app == app;
    U.app = app;
    if (from) lv_obj_get_coords(from, &U.from);
    else U.from = (lv_area_t){ W / 2 - 60, H / 2 - 60, W / 2 + 60, H / 2 + 60 };
    U.win_inner = app_inner(app);
    lv_obj_remove_flag(U.win_inner, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < U.nbuilt; i++) if (U.built[i].inner != U.win_inner) lv_obj_add_flag(U.built[i].inner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.win);
    if (!reopening || U.k.target == 0) {
        if (app->open) app->open();
        if (app->refresh) app->refresh();
    }
    if (U.k.value < 0.02f) window_begin(true);
    /* grows out of the icon on the release spring; a closing window caught here reopens from where it is */
    bz_motion_to(&U.k, 1, BZ_RELEASE);
    ui_text(U.pill_label, "%s", app->name);
    lv_label_set_text(U.pill_icon, BZ_I_ARROW_BACK);
    bz_glass_show(U.pill_glass, true);
    lv_obj_remove_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
    win_layout();
}

void ui_app_close(void)
{
    if (!U.app) return;
    if (U.k.value > 0.98f) window_begin(false);
    bz_motion_to(&U.k, 0, BZ_RELEASE);
    bz_glass_show(U.pill_glass, false);
}

bool ui_app_is_open(const ui_app_t *app) { return U.app == app && U.k.target > 0; }

static void wd_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
    if (U.k.value > 0.98f) window_begin(false);
    U.win_dragging = true;
    U.drag_k0 = U.k.value;
    bz_motion_set(&U.k, U.k.value, 0);
}

static void wd_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
    /* dragged down the window shrinks toward a card (0.3 at most, apps.js) under the finger */
    float k = U.drag_k0 - dy / 900.0f;
    k = bz_rubber_clamp(k, 0.3f, 1, 0.3f);
    bz_motion_set(&U.k, k, -vy / 900.0f);
    win_layout();
}

static void wd_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx; (void)u;
    U.win_dragging = false;
    float kv = -vy / 900.0f;
    float proj = U.k.value + bz_project(kv * 1000, BZ_RATE_FAST) / 1000;
    if (proj < 0.78f) ui_app_close();
    else bz_motion_to_v(&U.k, 1, BZ_RELEASE, kv);
}

static void build_windows(void)
{
    U.win = bz_box(bz_ui_content());
    lv_obj_add_style(U.win, bz_style_fill(BZ_C_GROUND), 0);
    lv_obj_set_style_clip_corner(U.win, true, 0);
    lv_obj_add_flag(U.win, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.win, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.win, LV_OBJ_FLAG_EVENT_BUBBLE);
    bz_drag_t d = { .begin = wd_begin, .move = wd_move, .end = wd_end, .axis = 2, .slop = 10 };
    bz_drag_attach(U.win, &d);
    bz_motion_init(&U.k, 0, 0.001f);

    U.pill = bz_row(bz_ui_glass(), 8);
    lv_obj_set_height(U.pill, 60);
    lv_obj_set_style_pad_left(U.pill, 18, 0);
    lv_obj_set_style_pad_right(U.pill, 24, 0);
    lv_obj_set_pos(U.pill, PAD, 18);
    lv_obj_remove_flag(U.pill, LV_OBJ_FLAG_EVENT_BUBBLE);
    U.pill_glass = bz_glass_attach(U.pill, 4, 30);
    U.pill_icon = bz_icon(U.pill, BZ_I_ARROW_BACK, 24, BZ_C_INK);
    U.pill_label = bz_label(U.pill, "", BZ_F_LABEL, BZ_C_INK);
    bz_on_tap(U.pill, pill_tap, NULL);
    bz_glass_show(U.pill_glass, false);
    lv_obj_add_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
}

static void windows_frame(double now, double dt)
{
    if (!U.app) return;
    bool moving = bz_motion_tick(&U.k);
    if (moving) bz_ui_keep_alive();
    win_layout();
    if (MC.window && !moving && !U.win_dragging) window_end();
    if (U.app->frame && U.k.target > 0) U.app->frame(now, dt);
    if (U.k.target == 0 && U.k.value <= 0.01f) {
        if (U.app->close) U.app->close();
        U.app = NULL;
        lv_obj_add_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ settings */

void ui_settings_save(void)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", S.team);
    hal_kv_set("team", buf);
    hal_kv_set("address", S.address);
    snprintf(buf, sizeof buf, "%.2f", S.brightness);
    hal_kv_set("bright", buf);
    snprintf(buf, sizeof buf, "%.2f", S.volume);
    hal_kv_set("volume", buf);
    hal_kv_set("dark", S.dark ? "1" : "0");
    hal_kv_set("calm", S.calm ? "1" : "0");
}

static void settings_load(void)
{
    char buf[64];
    if (hal_kv_get("team", buf, sizeof buf)) S.team = atoi(buf);
    if (hal_kv_get("address", buf, sizeof buf)) snprintf(S.address, sizeof S.address, "%s", buf);
    if (hal_kv_get("bright", buf, sizeof buf)) S.brightness = (float)atof(buf);
    if (hal_kv_get("volume", buf, sizeof buf)) S.volume = (float)atof(buf);
    if (hal_kv_get("dark", buf, sizeof buf)) S.dark = buf[0] == '1';
    if (hal_kv_get("calm", buf, sizeof buf)) S.calm = buf[0] == '1';
}

void ui_apply_addresses(void)
{
    char addrs[8][64];
    const char *ptrs[8];
    const char *override = S.address[0] ? S.address : U.cfg.sim_address;
    int n = cat_addresses(S.team, override, addrs, 8);
    if (U.cfg.sim_address && !S.address[0]) n = 1; /* the simulator talks to its fake robot only */
    for (int i = 0; i < n; i++) ptrs[i] = addrs[i];
    nt4_set_addresses(U.cfg.nt, ptrs, n);
}

/* ------------------------------------------------------------------ frame */

static void shell_frame(double now, double dt, void *user)
{
    (void)user;
    g_now = now;
    bool paging = bz_motion_tick(&U.offset);
    if (paging || fabsf(U.offset.value + MC.track_page * W) > 0.5f) {
        pages_begin();
        place_track();
        bz_ui_keep_alive();
        int p = (int)lroundf(-U.offset.value / W);
        if (p != U.page && U.offset.target == -(float)p * W) U.page = p;
    }
    if (MC.pages && !paging && !bz_drag_active() && fabsf(U.offset.value + U.page * W) < 0.5f) pages_end();
    caches_idle();
    dock_frame();
    windows_frame(now, dt);

    if (now - U.last_refresh >= 0.1) {
        U.last_refresh = now;
        cat_model_update(R);
        island_refresh();
        for (int i = 0; i < U.nrefresh; i++) U.refresh[i].fn(U.refresh[i].user);
        if (U.app && U.app->refresh && U.k.target > 0) U.app->refresh();
    }
    /* after 90 s untouched the panel dims to save the tablet's battery; a touch brings it back */
    static bool dimmed;
    bool idle = bz_ui_idle_s() > 90;
    if (idle != dimmed) {
        dimmed = idle;
        hal_set_brightness(idle ? S.brightness * 0.25f : S.brightness);
    }
    /* the glass's one light leans with how the tablet is held */
    hal_imu_t imu;
    if (hal_imu(&imu) && imu.ok) bz_ui_lean_light(imu.ax * 1.2f, imu.ay * 1.2f);
}

void ui_init(const ui_config_t *cfg)
{
    U.cfg = *cfg;
    R = calloc(1, sizeof *R);
    settings_load();
    if (cfg->team > 0 && !hal_kv_get("team", (char[8]){ 0 }, 8)) S.team = cfg->team;
    cat_model_init(cfg->nt);
    ui_apply_addresses();
    bz_ui_set_mode(S.dark, S.calm);
    hal_set_brightness(S.brightness);
    hal_set_volume(S.volume);

    lv_obj_t *scr = bz_ui_content();
    U.track = bz_box(scr);
    lv_obj_set_size(U.track, W * NPAGES, H);
    lv_obj_set_pos(U.track, 0, 0);
    for (int i = 0; i < NPAGES; i++) {
        U.pages[i] = bz_box(U.track);
        lv_obj_set_size(U.pages[i], W, H);
        lv_obj_set_pos(U.pages[i], i * W, 0);
    }
    bz_motion_init(&U.offset, 0, 0.1f);
    bz_drag_t d = { .begin = pg_begin, .move = pg_move, .end = pg_end, .axis = 1, .slop = 10 };
    bz_drag_attach(U.track, &d);

    ui_page_overview(U.pages[0]);
    ui_page_devices(U.pages[1]);
    ui_page_power(U.pages[2]);
    ui_page_motion(U.pages[3]);
    ui_page_tools(U.pages[4]);

    build_windows();
    build_dock();
    build_island();
    ui_cc_init();
    bz_ui_on_frame(scroll_frame, NULL);
    bz_ui_on_frame(shell_frame, NULL);
}

/* For the simulator's `dump strip`: the page pictures as they are. */
const uint16_t *ui_debug_strip(int *stride, int *w, int *h)
{
    *stride = MC.stride;
    *w = MC.stride;
    *h = H;
    return MC.strip;
}
