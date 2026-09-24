/* The shell: pages side by side under a glass dock, the island, app windows, and the 10 Hz model loop.
 *
 * Structure follows Bezel's panel.js: slots paged horizontally on the `page` spring from a projected
 * flick; a dock whose droplet can be dragged between items and lands on the nearest (projection plus
 * snap); an island that carries status and morphs for a message; apps that grow out of the icon that
 * opened them and can be caught mid-flight or dragged down to close. */
#include "ui_internal.h"
#include "as_snap.h"
#include "assist.h"
#include "link.h"

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NPAGES PG_COUNT
#define DOCK_ITEM_W 104
#define DOCK_ITEM_H 76
#define DOCK_GAP 4
#define DOCK_PAD 8
#define DOCK_W (NPAGES * DOCK_ITEM_W + (NPAGES - 1) * DOCK_GAP + 2 * DOCK_PAD)
#define DOCK_H 92
#define MAX_APPS 48         /* app windows built so far: every app in the library fits, with room */

cat_robot_t *R;
ui_settings_t S = { .team = 5805, .brightness = 0.8f, .volume = 0.5f, .dark = true };

static const char *const PAGE_NAMES[NPAGES] = { "home", "robot", "devices", "power", "motion", "apps" };
static const char *const PAGE_ICONS[NPAGES] = { BZ_I_HOME, BZ_I_MONITOR_HEART, BZ_I_HUB, BZ_I_BOLT,
                                                BZ_I_PRECISION_MANUFACTURING, BZ_I_APPS };

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
    lv_obj_t *status, *st_link, *st_batt_icon, *st_batt, *st_clock;
    bz_glass_t *island_glass;
    double island_until;

    struct { ui_refresh_fn fn; void *user; int page; } refresh[32]; /* page -1: always */
    int nrefresh;
    double last_refresh;

    /* app windows */
    const ui_app_t *app;
    lv_obj_t *win, *win_inner, *pill, *pill_label, *pill_icon;
    bz_glass_t *pill_glass;
    lv_area_t from;
    bz_motion_t k;
    struct { const ui_app_t *app; lv_obj_t *inner; } built[MAX_APPS];
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
    int band_page, band;     /* a page being refreshed a band per frame, and the next band */
    bool thaw_layers;        /* the pictures stay up while LVGL redraws under them (bz_ui_thawing) */
    bool thaw_strip;         /* ...and they are the pager's strip, still showing the page LVGL is redrawing */
} MC = { .band_page = -1 };

static double g_now;
double ui_now(void) { return g_now; }

#define SL_BANDS 8
static void slide_band(int page, int band);

/* Lean page slide (bz_ui_slide_*): the page under the finger is a snapshot; the one it reveals is drawn
 * offscreen a band per frame while the finger moves; on release a spring carries both into place and
 * only then does LVGL draw the page for real. Every frame of it is a PPA copy, no LVGL drawing. */
static struct {
    bool active, settling;
    int side, band;      /* the neighbour being drawn (+1 next, -1 previous, 0 none) and its next band */
    bool ready;
    int aim;
    bz_motion_t x;
} SL;

/* ------------------------------------------------------------------ helpers */

/* screen sleep: see the frame hook */
static struct {
    bool asleep, dimmed, request;
    double at;
} SLP;

void ui_sleep_now(void) { SLP.request = true; }
bool ui_asleep(void) { return SLP.asleep; }

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
        U.refresh[U.nrefresh].page = -1;
        U.nrefresh++;
    }
}

/* A page's own refreshes, now: before it's drawn for the first time in a while (hidden pages don't
 * refresh, so otherwise it would show stale or empty for a moment). */
static void page_refresh_now(int page)
{
    for (int i = 0; i < U.nrefresh; i++)
        if (U.refresh[i].page == page) U.refresh[i].fn(U.refresh[i].user);
}

void ui_on_page_refresh(int page, ui_refresh_fn fn, void *user)
{
    ui_on_refresh(fn, user);
    if (U.nrefresh > 0 && U.refresh[U.nrefresh - 1].fn == fn) U.refresh[U.nrefresh - 1].page = page;
}

/* A page's head, in the status band: its title, and a row beside it for a line of context (the returned
 * row; what goes in it must fit before the island — ui_head_width()). The band's right is the status. */
lv_obj_t *ui_head(lv_obj_t *page, const char *title, const char *label)
{
    (void)label;
    lv_obj_t *row = bz_row(page, 16);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_pos(row, PAD, HEAD_Y);
    bz_label(row, title, BZ_F_NAME, BZ_C_INK);
    lv_obj_t *right = bz_row(row, 12);
    lv_obj_set_style_pad_bottom(right, 4, 0);
    return right;
}

int ui_head_width(const char *title)
{
    /* what's left for the context between the title and the island */
    lv_point_t sz;
    lv_text_get_size(&sz, title, bz_font(BZ_F_NAME), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int tw = sz.x;
    return W / 2 - ISLAND_HALF - PAD - tw - 16 - 16;
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
    bool away;      /* left away from the end by a finger: don't follow new content */
} scroller_t;

#define MAX_SCROLLERS 64     /* every list in every app: a scroller not in here would neither coast nor follow */
static scroller_t *g_scrollers[MAX_SCROLLERS];

/* The list's content is a fixed, very tall column, so a row growing or arriving redraws only itself and
 * what it pushes down: a column sized to its content would redraw the whole list every time it grew.
 * How far it scrolls is measured from its lowest row instead. */
#define SC_TALL 30000

static float scroll_min(scroller_t *s)
{
    lv_obj_update_layout(s->content);
    lv_area_t c;
    lv_obj_get_coords(s->content, &c);
    int32_t bottom = 0;
    uint32_t n = lv_obj_get_child_count(s->content);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(s->content, (int32_t)i);
        if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(ch, &a);
        if (a.y2 + 1 - c.y1 > bottom) bottom = a.y2 + 1 - c.y1;
    }
    float over = (float)(bottom + lv_obj_get_style_pad_bottom(s->content, 0)) - lv_obj_get_height(s->clip);
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
    bz_ui_scroll(s->clip, s->content, (int32_t)y);
}

static void sc_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx;
    scroller_t *s = u;
    s->dragging = false;
    float lo = scroll_min(s), y = s->y.value;
    s->away = y + bz_project(vy, BZ_RATE_NORMAL) > lo + 40;
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
    for (int i = 0; i < MAX_SCROLLERS; i++) {
        scroller_t *s = g_scrollers[i];
        if (!s || s->dragging) continue;
        if (bz_motion_tick(&s->y)) {
            bz_ui_scroll(s->clip, s->content, (int32_t)s->y.value);
            bz_ui_keep_alive();
        }
    }
}

bool ui_scroller_follow(lv_obj_t *content)
{
    for (int i = 0; i < MAX_SCROLLERS; i++) {
        scroller_t *s = g_scrollers[i];
        if (!s || s->content != content) continue;
        if (s->dragging || s->away) return false;
        float lo = scroll_min(s);
        if (fabsf(s->y.target - lo) > 0.5f) {
            bz_motion_to(&s->y, lo, BZ_SMOOTH);
            bz_ui_keep_alive();
        }
        return true;
    }
    return false;
}

lv_obj_t *ui_scroller(lv_obj_t *parent, int w, int h)
{
    scroller_t *s = lv_malloc_zeroed(sizeof *s);
    s->clip = bz_box(parent);
    lv_obj_set_size(s->clip, w, h);
    s->content = bz_col(s->clip, BZ_GAP);
    lv_obj_set_size(s->content, w, SC_TALL);
    bz_motion_init(&s->y, 0, 0.1f);
    s->y.keep = true; /* a flick keeps its momentum even in lean: moving drawn rows is cheap */
    bz_drag_t d = { .begin = sc_begin, .move = sc_move, .end = sc_end, .user = s, .axis = 2, .slop = 10 };
    bz_drag_attach(s->clip, &d);
    lv_obj_add_flag(s->clip, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < MAX_SCROLLERS; i++) if (!g_scrollers[i]) { g_scrollers[i] = s; break; }
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
#if BZ_LEAN
    /* only the page on screen exists for LVGL: the others are hidden, so their updates cost no layout */
    for (int i = 0; i < NPAGES; i++) {
        bool on = i == U.page;
        if (on == lv_obj_has_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN)) {
            if (on) page_refresh_now(i);
            if (on) lv_obj_remove_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
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

static void pages_begin(void);

static void pg_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
#if BZ_LEAN
    if (SL.settling) return; /* a catch mid-settle: let it land (a moment) */
    bz_ui_slide_begin();
    SL.active = true;
    SL.side = 0;
    SL.ready = false;
    return;
#endif
    pages_begin(); /* before the first move: the track mustn't move under LVGL, which would redraw it all */
    U.drag_from = U.offset.value;
    bz_motion_set(&U.offset, U.offset.value, 0);
}

static void pg_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vy; (void)u;
#if BZ_LEAN
    (void)vx;
    if (!SL.active || SL.settling) return;
    /* past the first or last page it gives a little, rubber-banded */
    bool edge = (dx > 0 && U.page == 0) || (dx < 0 && U.page == NPAGES - 1);
    int side = edge || dx == 0 ? 0 : dx < 0 ? 1 : -1;
    if (side != SL.side) {
        SL.side = side;
        SL.band = 0;
        SL.ready = false;
        bz_ui_slide_nb(0);
    }
    bz_ui_slide(edge ? (int)bz_rubber_band((float)dx, (float)W, 0.55f) : dx);
    return;
#endif
    float x = bz_rubber_clamp(U.drag_from + dx, -(float)(NPAGES - 1) * W, 0, W);
    bz_motion_set(&U.offset, x, vx);
    place_track();
}

static void pg_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vy; (void)u;
#if BZ_LEAN
    {
    if (!SL.active || SL.settling) return;
    /* a swipe of a sixth of the screen, or a flick, is one page */
    int aim = U.page;
    if ((dx < -W / 6 || vx < -600) && U.page < NPAGES - 1) aim = U.page + 1;
    else if ((dx > W / 6 || vx > 600) && U.page > 0) aim = U.page - 1;
    if (aim != U.page && (SL.side != aim - U.page || !SL.ready)) {
        /* released before its neighbour finished drawing: finish it now (a frame's hitch) */
        SL.side = aim - U.page;
        if (!SL.ready) {
            for (; SL.band < SL_BANDS; SL.band++) slide_band(U.page + SL.side, SL.band);
            SL.ready = true;
        }
        bz_ui_slide_nb(SL.side);
    }
    SL.aim = aim;
    SL.settling = true;
    int from = dx;
    bool edge = (dx > 0 && U.page == 0) || (dx < 0 && U.page == NPAGES - 1);
    if (edge) from = (int)bz_rubber_band((float)dx, (float)W, 0.55f);
    bz_motion_init(&SL.x, (float)from, 0.5f);
    SL.x.keep = true;
    bz_motion_to_v(&SL.x, (float)((U.page - aim) * W), BZ_PAGE, edge ? 0 : vx);
    bz_ui_keep_alive();
    return;
    }
#endif
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
    lv_obj_align(U.dock, LV_ALIGN_BOTTOM_MID, 0, -DOCK_BOTTOM);
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
    /* power-on: the dock rises into place as the blind lifts (dock_frame sends it home) */
    bz_motion_init(&U.dock_tuck, bz_ui_calm() ? 0 : 1, 0.002f);
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
        float cy = H - DOCK_BOTTOM - DOCK_H / 2.0f - 2 * lift;
        bz_glass_set_rect(U.droplet_glass, true, U.drop_x.value - w / 2, cy - h / 2, w, h);
        lv_obj_set_style_bg_opa(U.platter, (lv_opa_t)(19 * (1 - lift)), 0);
    } else if (bz_glass_strength(U.droplet_glass) > 0.5f && !U.drop_dragging) {
        bz_glass_show(U.droplet_glass, false);
        lv_obj_set_style_bg_opa(U.platter, 19, 0);
    }

    /* the dock tucks away while an app is open, and after 4 s idle on a page (panel.js:34,166) */
    static double boot0;
    if (boot0 == 0) boot0 = g_now;
    bool tuck = U.app != NULL || g_now - boot0 < 0.5; /* at power-on it waits for the blind to start lifting */
    if (bz_motion_tick(&U.dock_tuck)) bz_ui_keep_alive();
    float target = tuck ? 1 : 0;
    if (U.dock_tuck.target != target) {
        bz_motion_to(&U.dock_tuck, target, tuck ? BZ_SMOOTH : BZ_RELEASE);
        bz_glass_show(U.dock_glass, !tuck);
    }
    lv_obj_set_y(U.dock, (int)(-DOCK_BOTTOM + 46 * U.dock_tuck.value));
    if (U.dock_tuck.value > 0.98f) lv_obj_add_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ island */

static void island_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_ROBOT, o);
}

/* The dev console's UI commands (tools/tab5_dev.py): taken on the console's task, run on the UI's loop. */
static char s_dev_cmd[48];
static volatile bool s_dev_pending;

static bool dev_handler(const char *line)
{
    if (strncmp(line, "open ", 5) && strncmp(line, "page ", 5) && strcmp(line, "close") && strcmp(line, "perf")) return false;
    if (s_dev_pending) return false;
    snprintf(s_dev_cmd, sizeof s_dev_cmd, "%s", line);
    s_dev_pending = true;
    return true;
}

static void dev_run(void)
{
    if (!s_dev_pending) return;
    if (!strncmp(s_dev_cmd, "open ", 5)) {
        const ui_app_t *a = ui_app_find(s_dev_cmd + 5);
        if (a) ui_app_open(a, NULL);
    } else if (!strncmp(s_dev_cmd, "page ", 5)) {
        ui_app_close();
        ui_go(atoi(s_dev_cmd + 5));
    } else if (!strcmp(s_dev_cmd, "close")) {
        ui_app_close();
    } else if (!strcmp(s_dev_cmd, "perf")) {
        /* where a frame's time has gone since the last "perf": the frame hooks, LVGL's handler, its
         * refresh (layout + render) and the render alone, per frame; and the presents */
        float hk, lv, rf, rd;
        bz_ui_split(&hk, &lv, &rf, &rd);
        bz_ui_perf_t pf;
        bz_ui_perf(&pf);
        printf("perf: hooks %.1f lvgl %.1f refresh %.1f render %.1f ms/frame; present %.1f ms, %.0f fps\n", hk, lv, rf, rd,
               pf.present_ms, pf.fps);
        bz_ui_hooks_report();
    }
    s_dev_pending = false;
}

/* a soft tick on every tap, if settings · sound says so */
static void tap_click(void)
{
    if (S.clicks) hal_tone(2400, 4, S.volume * 0.25f);
}

static void status_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_cc_open();
}

static void build_island(void)
{
    lv_obj_t *g = bz_ui_glass();
    U.island = bz_row(g, 12);
    lv_obj_set_height(U.island, 56);
    lv_obj_set_style_pad_hor(U.island, 22, 0);
    lv_obj_set_style_max_width(U.island, 2 * ISLAND_HALF, 0);
    lv_obj_align(U.island, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_remove_flag(U.island, LV_OBJ_FLAG_EVENT_BUBBLE);
    U.island_glass = bz_glass_attach(U.island, 3, BZ_R_ISLAND);
    U.island_mark = bz_mark(U.island, BZ_STALE, 12);
    U.island_icon = bz_icon(U.island, BZ_I_INFO, 24, BZ_C_INK);
    lv_obj_add_flag(U.island_icon, LV_OBJ_FLAG_HIDDEN);
    U.island_text = bz_label(U.island, "", BZ_F_LABEL, BZ_C_INK);
    U.island_tail = bz_label(U.island, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_add_flag(U.island_tail, LV_OBJ_FLAG_HIDDEN);
    bz_on_tap(U.island, island_tap, NULL);

    /* the status cluster, right of the band: how the tablet reaches the robot, its battery, the time */
    U.status = bz_row(g, 14);
    lv_obj_set_flex_align(U.status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(U.status, LV_ALIGN_TOP_RIGHT, -PAD, 22);
    U.st_link = bz_icon(U.status, BZ_I_LINK_OFF, 24, BZ_C_DIM);
    U.st_batt_icon = bz_icon(U.status, BZ_I_BATTERY_FULL, 24, BZ_C_INK);
    U.st_batt = bz_label(U.status, "", BZ_F_LABEL, BZ_C_INK);
    U.st_clock = bz_label(U.status, "", BZ_F_BODY, BZ_C_INK);
    /* like a phone's status bar: a tap opens the control center (it pulls down from the top edge too) */
    lv_obj_add_flag(U.status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(U.status, 16);
    bz_on_tap(U.status, status_tap, NULL);
}

static void status_refresh(void)
{
    hal_net_t n;
    hal_net(&n);
    hal_tether_t t;
    hal_tether(&t);
    const char *li = t.up ? BZ_I_USB : n.up ? BZ_I_WIFI : BZ_I_LINK_OFF;
    if (strcmp(lv_label_get_text(U.st_link), li)) lv_label_set_text(U.st_link, li);
    bz_set_color(U.st_link, t.up || n.up ? BZ_C_INK : BZ_C_DIM);
    hal_battery_t b;
    if (hal_battery(&b) && b.ok) {
        const char *bi = b.charging ? BZ_I_BATTERY_CHARGING_FULL : b.percent > 70 ? BZ_I_BATTERY_FULL
                         : b.percent > 40 ? BZ_I_BATTERY_5_BAR : b.percent > 15 ? BZ_I_BATTERY_3_BAR : BZ_I_BATTERY_ALERT;
        if (strcmp(lv_label_get_text(U.st_batt_icon), bi)) lv_label_set_text(U.st_batt_icon, bi);
        bz_set_color(U.st_batt_icon, b.percent <= 15 && !b.charging ? BZ_C_WARN : BZ_C_INK);
        ui_text(U.st_batt, "%d%%", b.percent);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) ui_text(U.st_clock, "%d:%02d", tm.tm_hour, tm.tm_min);
    else ui_text(U.st_clock, "--:--");
}

void ui_island_say(const char *icon, const char *text)
{
    ui_notify_add(icon, text); /* kept for the control center and the lock screen */
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
    hal_tether_t t;
    hal_tether(&t);
    status_refresh();
    /* the cable came or went: the robot's likeliest address changed with it */
    static bool was_up;
    if (t.up != was_up) {
        was_up = t.up;
        ui_apply_addresses();
        if (t.up) ui_island_say(BZ_I_USB, "usb tether up");
    }
}

/* ------------------------------------------------------------------ apps */

static lv_obj_t *app_inner(const ui_app_t *app)
{
    for (int i = 0; i < U.nbuilt; i++) if (U.built[i].app == app) return U.built[i].inner;
    lv_obj_t *inner = bz_box(U.win);
    lv_obj_set_size(inner, W, H);
    lv_obj_add_flag(inner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    app->build(inner);
    if (U.nbuilt < MAX_APPS) {
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

/* One band of page i into the slide's neighbour buffer: the page shown alone, chrome and window hidden. */
static void slide_prep(bool before, void *u)
{
    int i = (int)(intptr_t)u;
    static int x;
    static uint32_t shown; /* which of the chrome's pieces were visible, to put back exactly those */
    /* the chrome is hidden piece by piece: the top layer itself has no parent, and LVGL's flag setters
     * dereference the parent to mark its layout dirty */
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    if (before) {
        x = lv_obj_get_x(U.track);
        lv_obj_remove_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.pages[U.page], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(U.track, -i * W);
        shown = 0;
        for (uint32_t k = 0; k < nc && k < 32; k++) {
            lv_obj_t *c = lv_obj_get_child(top, (int32_t)k);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            shown |= 1u << k;
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_set_x(U.track, x);
        lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.pages[U.page], LV_OBJ_FLAG_HIDDEN);
        for (uint32_t k = 0; k < nc && k < 32; k++)
            if (shown & (1u << k)) lv_obj_remove_flag(lv_obj_get_child(top, (int32_t)k), LV_OBJ_FLAG_HIDDEN);
    }
}

static void slide_band(int page, int band)
{
    uint16_t *nb = bz_ui_slide_nb_buf();
    if (!nb || page < 0 || page >= NPAGES) return;
    if (band == 0) page_refresh_now(page); /* its numbers as of now, not as of when it was last shown */
    lv_area_t a = { 0, band * H / SL_BANDS, W - 1, (band + 1) * H / SL_BANDS - 1 };
    bz_ui_render_offscreen(nb + (size_t)a.y1 * W, W, &a, slide_prep, (void *)(intptr_t)page);
    bz_ui_slide_nb_patch(&a); /* into the panel's orientation, a band at a time (the Tab5's slide) */
}

/* Each frame of a slide: the next band of the neighbour, or the settle spring. */
static void slide_frame(void)
{
    if (!SL.active) return;
    bz_ui_keep_alive();
    if (!SL.settling) {
        if (SL.side && !SL.ready) {
            slide_band(U.page + SL.side, SL.band);
            if (++SL.band == SL_BANDS) {
                SL.ready = true;
                bz_ui_slide_nb(SL.side);
            }
        }
        return;
    }
    bool moving = bz_motion_tick(&SL.x);
    bz_ui_slide((int)lroundf(SL.x.value));
    if (moving) return;
    SL.active = SL.settling = false;
    if (SL.aim != U.page) ui_go(SL.aim);
    bz_ui_slide_end();
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
#if BZ_LEAN
    return false; /* no page or window pictures: pages and windows change in one redraw */
#endif
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

/* A refresh in the background is drawn a band per frame: a whole page at once (~920k px) would cost
 * a frame on the tablet, and the screen may be animating (a reply streaming in) while nobody touches it. */
#define CACHE_BANDS 8

static void render_band(int i, int b)
{
    lv_area_t a = { 0, b * H / CACHE_BANDS, W - 1, (b + 1) * H / CACHE_BANDS - 1 };
    bz_ui_render_offscreen(strip_page(i) + (size_t)a.y1 * MC.stride, MC.stride, &a, prep_page, (void *)(intptr_t)i);
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

/* The pictures a thaw left up come down once LVGL has redrawn everything under them. */
static void thaw_layers_drop(void)
{
    if (!MC.thaw_layers) return;
    MC.thaw_layers = false;
    bz_comp_set_layer(bz_ui_comp(), 0, NULL);
    bz_comp_set_layer(bz_ui_comp(), 1, NULL);
}

static void pages_begin(void)
{
    if (MC.pages || MC.window || U.app || !caches_ready()) return;
    if (MC.thaw_strip && MC.thaw_layers && bz_ui_thawing()) {
        /* swiping again right after landing: the strip is still up with this page in it, so the thaw
         * can stop where it is rather than finish in one frame */
        bz_ui_thaw_cancel();
        MC.thaw_layers = false;
    } else {
        grab_page(MC.track_page); /* finishes a thaw still going */
        thaw_layers_drop();
    }
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
    bz_ui_freeze(false);                /* one redraw, of the page as it is now, under the strip */
    MC.thaw_layers = MC.thaw_strip = true;
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
    thaw_layers_drop();
    bz_ui_freeze(true);
    MC.window = true;
    bz_layer_t page = { { 0, 0, W - 1, H - 1 }, 0, strip_page(MC.track_page), 0, 0, MC.stride };
    bz_comp_set_layer(bz_ui_comp(), 0, &page);
}

static void window_end(void)
{
    if (!MC.window) return;
    MC.window = false;
    bz_ui_freeze(false);
    MC.thaw_layers = true;
    MC.thaw_strip = false;
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
        MC.band_page = -1;
    }
    if (MC.pages || MC.window || bz_ui_thawing() || bz_ui_idle_s() < 0.4) return;
    if (U.k.value != U.k.target || fabsf(U.offset.value - U.offset.target) > 0.5f) return;
    if (MC.band_page >= 0) {
        /* a picture finished band by band may mix two moments a few frames apart: fine for a picture
         * that is only shown while it slides, and redrawn for real where it lands */
        render_band(MC.band_page, MC.band);
        if (++MC.band == CACHE_BANDS) {
            MC.ok[MC.band_page] = true;
            MC.t[MC.band_page] = MC.last_render = g_now;
            MC.band_page = -1;
        }
        return;
    }
    if (g_now - MC.last_render < 0.3) return;
    int best = -1;
    double worst = 0;
    for (int i = 0; i < NPAGES; i++) {
        bool here = i == MC.track_page;
        if (here && !U.app) continue; /* on screen: grabbed, not drawn */
        double limit = here ? 3 : abs(i - MC.track_page) == 1 ? 2 : 10;
        double age = MC.ok[i] ? g_now - MC.t[i] : 1e9;
        if (age > limit && age - limit > worst) { worst = age - limit; best = i; }
    }
    if (best >= 0) {
        MC.band_page = best;
        MC.band = 0;
    }
}

/* Lean: an app comes up from the bottom edge as a sheet, and goes back down it. The platform slides a
 * picture of it over the page with no drawing per frame (bz_ui_sheet_*); the window itself switches at
 * once underneath, and takes over from the picture when the sheet comes to rest. */
static struct {
    bool on;
    bz_motion_t k;
} ASH;

static bool app_sheet_start(bool opening)
{
#if BZ_LEAN
    if (ASH.on || bz_ui_sheeting()) return false;
    if (!bz_ui_sheet_begin(opening, H, true, NULL, NULL)) return false;
    if (!ASH.k.keep) {
        bz_motion_init(&ASH.k, 0, 0.001f);
        ASH.k.keep = true; /* animates though the lean renderer makes other motion instant */
    }
    bz_motion_set(&ASH.k, opening ? 0 : 1, 0);
    bz_motion_to(&ASH.k, opening ? 1 : 0, BZ_RELEASE);
    ASH.on = true;
    bz_ui_keep_alive();
    return true;
#else
    (void)opening;
    return false;
#endif
}

static void app_sheet_frame(void)
{
    if (!ASH.on) return;
    bool moving = bz_motion_tick(&ASH.k);
    float k = ASH.k.value < 0 ? 0 : ASH.k.value > 1 ? 1 : ASH.k.value;
    bz_ui_keep_alive();
    if (moving) {
        bz_ui_sheet((int)(k * H + 0.5f));
        return;
    }
    /* at rest: the resting picture on the glass, then the window (or the page) takes over, not redrawn */
    int rest = ASH.k.target > 0.5f ? H : 0;
    if (bz_ui_sheet_shown() != rest) {
        bz_ui_sheet(rest);
        return;
    }
    bz_ui_sheet_end();
    ASH.on = false;
}

void ui_app_open(const ui_app_t *app, lv_obj_t *from)
{
    bz_ui_wake();
    /* from the page (nothing open): the sheet starts while the glass still shows the page */
    if (!U.app || U.k.target == 0) app_sheet_start(true);
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
    /* an app is full screen: its head takes the band's right and its body the orb's corner; the island
     * stays, with the robot's state */
    lv_obj_add_flag(U.status, LV_OBJ_FLAG_HIDDEN);
    ui_orb_show(false);
    win_layout();
}

void ui_app_close(void)
{
    if (!U.app) return;
    if (U.k.value > 0.98f) app_sheet_start(false);
    if (U.k.value > 0.98f) window_begin(false);
    bz_motion_to(&U.k, 0, BZ_RELEASE);
    bz_glass_show(U.pill_glass, false);
}

bool ui_app_is_open(const ui_app_t *app) { return U.app == app && U.k.target > 0; }
bool ui_app_any_open(void) { return U.app && U.k.target > 0; }

static void wd_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
#if BZ_LEAN
    return;
#endif
    if (U.k.value > 0.98f) window_begin(false);
    U.win_dragging = true;
    U.drag_k0 = U.k.value;
    bz_motion_set(&U.k, U.k.value, 0);
}

static void wd_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
#if BZ_LEAN
    (void)dy; (void)vy;
    return;
#endif
    /* dragged down the window shrinks toward a card (0.3 at most, apps.js) under the finger */
    float k = U.drag_k0 - dy / 900.0f;
    k = bz_rubber_clamp(k, 0.3f, 1, 0.3f);
    bz_motion_set(&U.k, k, -vy / 900.0f);
    win_layout();
}

static void wd_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx; (void)u;
#if BZ_LEAN
    if (dy > 160 || vy > 900) ui_app_close(); /* swipe down closes */
    return;
#endif
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
    app_sheet_frame();
    if (!U.app) return;
    bool moving = bz_motion_tick(&U.k);
    if (moving) bz_ui_keep_alive();
    win_layout();
    if (MC.window && !moving && !U.win_dragging) window_end();
    if (U.app->frame && U.k.target > 0) U.app->frame(now, dt);
    if (U.k.target == 0 && U.k.value <= 0.01f) {
        window_end(); /* the spring may still be settling, but the window is gone */
        if (U.app->close) U.app->close();
        U.app = NULL;
        lv_obj_add_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.status, LV_OBJ_FLAG_HIDDEN);
        ui_orb_show(true);
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
    hal_kv_set("perf", S.perf ? "1" : "0");
    hal_kv_set("autorot", S.auto_rotate ? "1" : "0");
    char n[12];
    snprintf(n, sizeof n, "%d", S.dim_s);
    hal_kv_set("dim", n);
    snprintf(n, sizeof n, "%d", S.sleep_s);
    hal_kv_set("sleep", n);
    hal_kv_set("lock", S.lock ? "1" : "0");
    hal_kv_set("clicks", S.clicks ? "1" : "0");
    snprintf(n, sizeof n, "%d", S.tz);
    hal_kv_set("tzi", n);
    hal_kv_set("flip", S.flip ? "1" : "0");
}

static void settings_load(void)
{
    char buf[64];
#ifdef ESP_PLATFORM
    /* on the tablet, solid surfaces unless glass is asked for: calm skips the glass pass entirely */
    S.calm = true;
#endif
    if (hal_kv_get("team", buf, sizeof buf)) S.team = atoi(buf);
    if (hal_kv_get("address", buf, sizeof buf)) snprintf(S.address, sizeof S.address, "%s", buf);
    if (hal_kv_get("bright", buf, sizeof buf)) S.brightness = (float)atof(buf);
    if (hal_kv_get("volume", buf, sizeof buf)) S.volume = (float)atof(buf);
    if (hal_kv_get("dark", buf, sizeof buf)) S.dark = buf[0] == '1';
    if (hal_kv_get("calm", buf, sizeof buf)) S.calm = buf[0] == '1';
    if (hal_kv_get("perf", buf, sizeof buf)) S.perf = buf[0] == '1';
    S.auto_rotate = true;
    if (hal_kv_get("autorot", buf, sizeof buf)) S.auto_rotate = buf[0] == '1';
    S.dim_s = 90;
    if (hal_kv_get("dim", buf, sizeof buf)) S.dim_s = atoi(buf);
    S.sleep_s = 300;
    if (hal_kv_get("sleep", buf, sizeof buf)) S.sleep_s = atoi(buf);
    S.lock = true;
    if (hal_kv_get("lock", buf, sizeof buf)) S.lock = buf[0] == '1';
    S.clicks = true;
    if (hal_kv_get("clicks", buf, sizeof buf)) S.clicks = buf[0] == '1';
    if (hal_kv_get("tzi", buf, sizeof buf)) S.tz = atoi(buf);
    /* which way up is main's to decide, before the boot card's first frame (and the card follows the
     * tablet while it plays): taking it from the panel here never turns the picture mid-animation */
    S.flip = hal_flip();
}

void ui_set_flip(bool flip)
{
    if (flip == hal_flip()) return;
    S.flip = flip;
    hal_set_flip(flip);
    lv_obj_invalidate(bz_ui_content());
    lv_obj_invalidate(bz_ui_glass());
}

/* Auto-rotate: gravity read ten times a second in the picture's own frame; held upside down (gravity
 * toward the top of the screen) for a third of a second, the picture turns. */
static void orient_frame(double now)
{
    static double last, since;
    if (!S.auto_rotate || now - last < 0.1) return;
    last = now;
    hal_imu_t imu;
    if (!hal_imu(&imu) || !imu.ok) return;
    if (imu.ay < -0.6f) {
        if (since == 0) since = now;
        if (now - since >= 0.35) {
            since = 0;
            ui_set_flip(!hal_flip());
            ui_settings_save();
        }
    } else {
        since = 0;
    }
}

void ui_set_tone(bool dark, bool calm)
{
    S.dark = dark;
    S.calm = calm;
    bz_ui_set_mode(dark, calm);
    ui_settings_save();
}

void ui_apply_addresses(void)
{
    char addrs[10][64];
    const char *ptrs[10];
    const char *override = S.address[0] ? S.address : U.cfg.sim_address;
    int n = cat_addresses(S.team, override, addrs, 10);
    if (U.cfg.sim_address && !S.address[0]) n = 1; /* the simulator talks to its fake robot only */
    for (int i = 0; i < n; i++) ptrs[i] = addrs[i];
    /* tethered, the cable's addresses go first: over it, `.local` names don't resolve (lwIP asks the
     * default interface, which is Wi-Fi), and it's the link the technician plugged in on purpose */
    hal_tether_t t;
    hal_tether(&t);
    if (t.up && !S.address[0]) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (!strncmp(ptrs[i], "172.26.", 7) || !strncmp(ptrs[i], "172.27.", 7) ||
                (t.gw[0] && !strcmp(ptrs[i], t.gw))) {
                const char *p = ptrs[i];
                memmove(&ptrs[k + 1], &ptrs[k], sizeof(ptrs[0]) * (size_t)(i - k));
                ptrs[k++] = p;
            }
    }
    nt4_set_addresses(U.cfg.nt, ptrs, n);
    /* a USB-Ethernet dongle into the robot's radio or switch gets 10.TE.AM.60 if nothing answers DHCP */
    if (S.team > 0 && S.team < 10000) {
        char ip[16];
        snprintf(ip, sizeof ip, "10.%d.%d.60", S.team / 100, S.team % 100);
        hal_tether_fallback(ip, "255.255.255.0");
    }
}

/* ------------------------------------------------------------------ the frame-time overlay */

/* Frames per second and where a frame's time goes, measured on this machine (the tablet's own clock
 * on the Tab5), four times a second so the overlay doesn't cost frames itself. The simulator adds the
 * frame costed for the ESP32-P4 (bz_ui.c). */
static lv_obj_t *g_perf;

static void perf_frame(double now)
{
    static double last;
    if (!S.perf) {
        if (g_perf) lv_obj_add_flag(g_perf, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!g_perf) {
        g_perf = bz_label(bz_ui_glass(), "", BZ_F_CAPTION, BZ_C_INK);
        lv_obj_set_style_bg_opa(g_perf, LV_OPA_70, 0);
        lv_obj_add_style(g_perf, bz_style_fill(BZ_C_SURFACE1), 0);
        lv_obj_set_style_pad_hor(g_perf, 10, 0);
        lv_obj_set_style_pad_ver(g_perf, 6, 0);
        lv_obj_set_style_radius(g_perf, 10, 0);
        lv_obj_set_pos(g_perf, PAD, H - DOCK_CLEAR - 44);
    }
    lv_obj_remove_flag(g_perf, LV_OBJ_FLAG_HIDDEN);
    if (now - last < 0.25) return;
    last = now;
    bz_ui_perf_t p;
    bz_ui_perf(&p);
#ifdef ESP_PLATFORM
    ui_text(g_perf, "%4.1f fps · %4.1f ms · lvgl %.1f · comp %.1f · present %.1f", p.fps, p.frame_ms, p.lvgl_ms,
            p.compose_ms, p.present_ms);
#else
    ui_text(g_perf, "%4.1f fps · p4 %4.1f ms · lvgl %u px · glass %u px", p.fps, p.model_ms, (unsigned)p.lvgl_px,
            (unsigned)p.comp.glass_px);
#endif
}

/* ------------------------------------------------------------------ frame */

static void shell_frame(double now, double dt, void *user)
{
    (void)user;
    g_now = now;
    dev_run();
#if BZ_LEAN
    slide_frame();
    /* instant motion: the offset is already where it's going; move the track there once */
    if (MC.track_page != U.page) {
        MC.track_page = U.page;
        place_track();
    }
#endif
    bool paging = bz_motion_tick(&U.offset);
    if (!BZ_LEAN && (paging || fabsf(U.offset.value + MC.track_page * W) > 0.5f)) {
        pages_begin();
        place_track();
        bz_ui_keep_alive();
        int p = (int)lroundf(-U.offset.value / W);
        if (p != U.page && U.offset.target == -(float)p * W) U.page = p;
    }
    if (MC.pages && !paging && !bz_drag_active() && fabsf(U.offset.value + U.page * W) < 0.5f) pages_end();
    if (!bz_ui_thawing()) thaw_layers_drop();
    caches_idle();
    dock_frame();
    orient_frame(now);
    windows_frame(now, dt);
    perf_frame(now);

    if (now - U.last_refresh >= 0.1) {
        U.last_refresh = now;
        cat_model_update(R);
        assist_feed(R);
        island_refresh();
        /* a page's refresh only while it's on screen and nothing covers it */
        bool covered = U.app && U.k.target > 0;
        for (int i = 0; i < U.nrefresh; i++)
            if (U.refresh[i].page < 0 || (U.refresh[i].page == U.page && !covered)) U.refresh[i].fn(U.refresh[i].user);
        if (U.app && U.app->refresh && U.k.target > 0) U.app->refresh();
    }
    /* the interface fades in with the backlight after the boot card faded out with it */
    static double shown_at;
    static bool faded_in;
    if (!shown_at) shown_at = now;
    if (!faded_in) {
        float k = (float)((now - shown_at) / 0.35);
        if (k >= 1) { k = 1; faded_in = true; }
        hal_set_brightness(S.brightness * k * k * (3 - 2 * k));
        bz_ui_keep_alive();
    }
    /* untouched, the panel dims (settings: display), then goes off; a tap wakes it, and that tap presses
     * nothing. Asleep, the link to the robot and everything behind the glass keep running. */
    double idle_s = bz_ui_idle_s();
    if (!SLP.asleep && (SLP.request || (S.sleep_s > 0 && idle_s > S.sleep_s))) {
        SLP.asleep = true;
        SLP.request = false;
        SLP.at = now;
        hal_set_brightness(0);
        bz_ui_swallow_touch();
        ui_lock_show(); /* drawn while dark: it's what the screen wakes to */
    } else if (SLP.asleep && now - idle_s > SLP.at + 0.05) {
        /* a touch since it went off: back on, and the dim timer starts over */
        SLP.asleep = false;
        SLP.dimmed = false;
        hal_set_brightness(S.brightness);
        hal_tone(1200, 8, S.volume * 0.3f);
    }
    bool idle = !SLP.asleep && S.dim_s > 0 && idle_s > S.dim_s;
    if (!SLP.asleep && idle != SLP.dimmed) {
        SLP.dimmed = idle;
        hal_set_brightness(idle ? S.brightness * 0.25f : S.brightness);
    }
    /* the glass's one light leans with how the tablet is held: read at 10 Hz, low-passed and moved only in
     * steps of 0.05, because every move relights and redraws every glass shape, and a hand never holds
     * still — at 60 Hz raw that kept the whole glass layer redrawing */
    static double imu_t;
    static float lx, ly, qx = 99, qy = 99;
    hal_imu_t imu;
    if (!BZ_LEAN && now - imu_t >= 0.1 && hal_imu(&imu) && imu.ok) {
        imu_t = now;
        lx += (imu.ax * 1.2f - lx) * 0.3f;
        ly += (imu.ay * 1.2f - ly) * 0.3f;
        float nx = roundf(lx * 20) / 20, ny = roundf(ly * 20) / 20;
        if (nx != qx || ny != qy) {
            qx = nx;
            qy = ny;
            bz_ui_lean_light(qx, qy);
        }
    }
}

void ui_init(const ui_config_t *cfg)
{
    U.cfg = *cfg;
    R = calloc(1, sizeof *R);
    settings_load();
    if (cfg->team > 0 && !hal_kv_get("team", (char[8]){ 0 }, 8)) S.team = cfg->team;
    cat_model_init(cfg->nt);
    ui_sc_boot();
    ui_os_boot(); /* alarms ring whether or not the clock app has been opened */
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

    ui_page_home(U.pages[PG_HOME]);
    ui_page_overview(U.pages[PG_ROBOT]);
    ui_page_devices(U.pages[PG_DEVICES]);
    ui_page_power(U.pages[PG_POWER]);
    ui_page_motion(U.pages[PG_MOTION]);
    ui_page_tools(U.pages[PG_APPS]);

    build_windows();
    build_dock();
    build_island();
    ui_cc_init();
    ui_lock_init();
    ui_orb_init();
    snap_init();
    link_init();
    assist_init();
    bz_ui_on_frame(scroll_frame, NULL);
    bz_ui_on_frame(shell_frame, NULL);
    bz_ui_on_any_tap(tap_click);
    hal_dev_set_handler(dev_handler);
    place_track(); /* lean: hides every page but the first */
}

/* For the simulator's `dump strip`: the page pictures as they are. */
const uint16_t *ui_debug_strip(int *stride, int *w, int *h)
{
    *stride = MC.stride;
    *w = MC.stride;
    *h = H;
    return MC.strip;
}
