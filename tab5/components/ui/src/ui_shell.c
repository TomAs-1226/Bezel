/* The shell: pages side by side under a glass dock, the island, app windows, and the 10 Hz model loop.
 *
 * Structure follows Bezel's panel.js: slots paged horizontally on the `page` spring from a projected
 * flick; a dock whose droplet can be dragged between items and lands on the nearest (projection plus
 * snap); an island that carries status and morphs for a message; apps that grow out of the icon that
 * opened them and can be caught mid-flight or dragged down to close. */
#include "ui_internal.h"
#include "ui.h"
#include "as_snap.h"
#include "assist.h"
#include "ui_sys.h"
#include "link.h"
#include "voice.h"
#include "ui_home_mode.h"
#include "bz_frost.h"

#include <math.h>
#include <pthread.h>
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

    lv_obj_t *status, *st_link, *st_batt_icon, *st_batt, *st_clock;

    struct { ui_refresh_fn fn; void *user; int page; } refresh[32]; /* page -1: always */
    int nrefresh;
    double last_refresh;

    /* app windows */
    const ui_app_t *app;
    lv_obj_t *win, *win_inner, *pill, *pill_label, *pill_icon;
    bz_glass_t *pill_glass;
    lv_area_t from;
    bz_motion_t k;
    /* `bytes` is what the build cost in LVGL's heap, `used` when it was last opened: between them they
     * say which window is worth taking down when PSRAM runs short (app_evict). */
    struct { const ui_app_t *app; lv_obj_t *inner; size_t bytes; double used; } built[MAX_APPS];
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
static void slide_bands_end(void);

/* Lean page slide (bz_ui_slide_*): the page under the finger is a snapshot; the one it reveals is drawn
 * offscreen a band per frame while the finger moves; on release a spring carries both into place and
 * only then does LVGL draw the page for real. Every frame of it is a PPA copy, no LVGL drawing. */
static struct {
    bool active, settling;
    int side, band;      /* the neighbour being drawn (+1 next, -1 previous, 0 none) and its next band */
    bool ready;
    int aim;
    int nb_page;         /* the page the bands are drawing: the neighbour, or the one a jump aims at */
    bool jump;           /* no finger: the spring starts itself once the bands are done */
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
 * row; what goes in it must fit before the status cluster — ui_head_width()). The band's right is the status,
 * then the island's orb. */
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
    /* what's left for the context between the title and the status cluster (the orb is past it) */
    lv_point_t sz;
    lv_text_get_size(&sz, title, bz_font(BZ_F_NAME), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int tw = sz.x;
    return W + HEAD_RIGHT_X - STATUS_W - 24 - PAD - tw - 16;
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

/* Bezel's FAB: the one primary action of a view, a round of the signal colour with its glyph (icons only: small
 * text on signal fails contrast in the light tone). 72 round -- Bezel's 66 on this denser panel -- the glyph filled.
 * The glyph is the button's only child; ui_fab_set changes it (only when it differs: a set restyles). */
lv_obj_t *ui_fab(lv_obj_t *parent, const char *icon, bz_tap_fn fn, void *user)
{
    lv_obj_t *b = bz_tile(parent, 72, 72);
    bz_tile_set_fill(b, BZ_C_SIGNAL);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_t *ic = bz_icon(b, icon, 28, BZ_C_ON_SIGNAL);
    bz_icon_set(ic, icon, 28, true);
    lv_obj_center(ic);
    if (fn) bz_on_tap(b, fn, user);
    return b;
}

void ui_fab_set(lv_obj_t *fab, const char *icon)
{
    lv_obj_t *ic = lv_obj_get_child(fab, 0);
    if (!ic || !strcmp(lv_label_get_text(ic), icon)) return;
    bz_icon_set(ic, icon, 28, true);
}

/* A section's name in a pane or a list: the lowercase mono label, dim, with a section's worth of room above it and a
 * row's below (the column's own gap), so it reads as the head of what follows, not the foot of what came before. */
lv_obj_t *ui_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = bz_label(parent, text, BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_style_pad_top(l, 12, 0);
    return l;
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
    /* Bezel's mode buttons: corners open from 12 to full when selected. Only on a change: setting a local style
     * restyles and redraws the chip even when the value is the same, and refreshes call this 10 times a second. */
    int32_t rad = on ? LV_RADIUS_CIRCLE : BZ_R_M;
    if (lv_obj_get_style_radius(chip, 0) != rad) lv_obj_set_style_radius(chip, rad, 0);
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
    /* A drag that starts cancels the press it began on (Bezel's press rule, and LVGL's own scrolls do the same):
     * the row under the finger drops its platter instead of riding the list up the screen lit. One restyle, once. */
    lv_indev_t *in = lv_indev_active();
    lv_obj_t *act = in ? lv_indev_get_active_obj() : NULL;
    if (act && lv_obj_has_state(act, LV_STATE_PRESSED)) lv_obj_remove_state(act, LV_STATE_PRESSED);
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
        /* No lv_obj_is_valid() here, though it reads like the careful thing to do. It is not a
         * pointer test: it walks the whole object tree through obj_valid_child() looking for the
         * object. Two of those per scroller slot per frame is dozens of full tree walks a frame,
         * and a soak caught the panic INSIDE obj_valid_child -- a walk hunting for corruption
         * faults on the same bad child as any other reader. scroller_gone() on LV_EVENT_DELETE
         * removes the scroller the moment its clip is destroyed, on this thread, with no window
         * in between; that is the guarantee, and it needs no second one. */
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

/* Back to the top at once, no motion: a list whose page is off screen (the library, left scrolled), or a
 * settings section opened again. The caller makes sure nothing is drawing a picture of it (no slide or sheet
 * under way). */
void ui_scroller_reset(lv_obj_t *content)
{
    for (int i = 0; i < MAX_SCROLLERS; i++) {
        scroller_t *s = g_scrollers[i];
        if (!s || s->content != content || s->dragging) continue;
        s->away = false;
        bz_motion_set(&s->y, 0, 0);
        if (lv_obj_get_y(content) != 0) lv_obj_set_y(content, 0);
        return;
    }
}

/* The clip is being deleted, so its scroller must stop existing too.
 *
 * Without this the scroller stays in g_scrollers with pointers to freed objects, and the first
 * frame in which its spring is still moving lays out freed memory -- which is the crash behind
 * "scrolling the apps menu crashes it". On the UI thread, at the moment of deletion, so there is no
 * window between the objects going and the scroller going. */
static void scroller_gone(lv_event_t *e)
{
    scroller_t *s = lv_event_get_user_data(e);
    for (int i = 0; i < MAX_SCROLLERS; i++)
        if (g_scrollers[i] == s) g_scrollers[i] = NULL;
    lv_free(s);
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
    lv_obj_add_event_cb(s->clip, scroller_gone, LV_EVENT_DELETE, s);
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

static bool s_track_quiet;

static void track_move(void *u)
{
    (void)u;
    if (MC.pages) strip_layer(U.offset.value);
    else lv_obj_set_x(U.track, (int)lroundf(U.offset.value));
#if BZ_LEAN
    for (int i = 0; i < NPAGES; i++) {
        bool on = i == U.page;
        if (on == lv_obj_has_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN)) {
            if (on) lv_obj_remove_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_update_layout(U.track);
#endif
}

static void place_track(void)
{
#if BZ_LEAN
    /* after a slide: the glass shows the page already; moved in with no redraw, then its numbers
     * brought up to date, which redraws only what changed since the slide's picture of it */
    if (s_track_quiet) {
        s_track_quiet = false;
        bz_ui_quiet(track_move, NULL);
        page_refresh_now(U.page);
        return;
    }
#endif
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

#if BZ_LEAN
/* A page asked for with no finger on the screen: the dock, the droplet.
 *
 * It used to be ui_go straight away, which in the lean renderer means hiding one page and showing
 * another: LVGL redraws all 1280x720 (62 ms) and the PPA turns all of it into the panel (45 ms), and
 * measured on the tablet a dock tap froze the interface for 116 ms — a tap that feels broken, and the
 * only thing on any screen that took over 100 ms. The slide already knows how to change page without
 * either: the page it's going to is drawn a band of 90 rows per frame into the picture beside the one
 * on the glass, and then both are moved by block copies with nothing turned. So a tap takes the same
 * road a swipe does, minus the finger: the bands go down over eight live frames, and when the picture
 * is ready the page spring carries it in.
 *
 * Asked for from an LVGL event (a dock item's click), so it is only noted here: starting a slide takes
 * a picture of the glass, which must not happen inside lv_timer_handler. shell_frame picks it up. */
static int s_go_want = -1;

static void go_page(int page)
{
    if (page < 0) page = 0;
    if (page >= NPAGES) page = NPAGES - 1;
    s_go_want = page;
}

static void go_step(void)
{
    if (s_go_want < 0) return;
    int want = s_go_want;
    s_go_want = -1;
    if (want == U.page) return;
    /* a finger on the pages, a sheet or home mode owns the glass; so does a slide already running */
    if (SL.active || bz_drag_active() || bz_ui_sheeting() || ui_home_mode_active() || !bz_ui_can_sheet()) {
        ui_go(want);
        return;
    }
    slide_bands_end();
    bz_ui_slide_begin();
    if (!bz_ui_sliding()) { /* no memory for the pictures, or it refused: the old way */
        ui_go(want);
        return;
    }
    SL.active = true;
    SL.settling = false;
    SL.ready = false;
    SL.jump = true;
    SL.band = 0;
    SL.side = want > U.page ? 1 : -1; /* several pages along still comes in from its own side */
    SL.nb_page = want;
    SL.aim = want;
    bz_motion_init(&SL.x, 0, 0.5f);
    SL.x.keep = true; /* animates though the lean renderer makes other motion instant */
    bz_ui_keep_alive();
}
#else
static void go_page(int page) { ui_go(page); }
static void go_step(void) {}
#endif

int ui_page(void) { return U.page; }
lv_obj_t *ui_page_body(int page) { return U.pages[page]; }

static void pages_begin(void);

/* The Wi-Fi watchdog's restart (hal_tab5_net.c): where the interface is, noted from the watchdog's task (two
 * plain reads, no LVGL), and taken back after the restart: the same page, the same app, and a word why */
static void note_where(void) { hal_resume_note(U.page, U.app ? U.app->name : ""); }

/* from the frame hook, once the interface is up: opening an app while ui_init was still building it (its sheet
 * wants the glass) hung the start on the boot card */
static void resume_where(void)
{
    int page;
    char app[24];
    if (!hal_resume_take(&page, app, sizeof app)) return;
    if (page >= 0 && page < NPAGES) ui_go(page);
    const ui_app_t *a = app[0] ? ui_app_find(app) : NULL;
    if (a) ui_app_open(a, NULL);
    ui_island_say(BZ_I_WIFI, "wi-fi reset: back online where you were");
}

/* Home mode (ui_home_mode.c) covers the pages: the status band and the orb go while it's up; the dock tucks
 * itself (dock_frame) */
static void home_changed(bool active)
{
    if (active) {
        lv_obj_add_flag(U.status, LV_OBJ_FLAG_HIDDEN);
        ui_orb_show(false);
    } else if (!U.app) {
        lv_obj_remove_flag(U.status, LV_OBJ_FLAG_HIDDEN);
        ui_orb_show(true);
    }
}

static void pg_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
#if BZ_LEAN
    /* a catch mid-settle lets it land (a moment); a sheet still moving (an app closing) owns the glass:
     * a slide begun under it took its chrome from a half-drawn picture and lost it */
    if (SL.active || ui_home_mode_active() || bz_ui_sheeting()) return;
    slide_bands_end(); /* nothing prepared from a slide that didn't finish */
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
        SL.nb_page = U.page + side;
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
        /* released before its neighbour finished drawing: finish it now (a frame's hitch). When the release
         * aims at the other side from the one being drawn (a drag that turned back, then a flick), that
         * neighbour starts over from its first band: carrying on from the other page's count left the other
         * page in the top bands, and the slide came to rest on the two pages spliced. */
        if (SL.side != aim - U.page) {
            SL.side = aim - U.page;
            SL.nb_page = U.page + SL.side;
            SL.band = 0;
            SL.ready = false;
        }
        if (!SL.ready) {
            for (; SL.band < SL_BANDS; SL.band++) slide_band(SL.nb_page, SL.band);
            SL.ready = true;
        }
        slide_bands_end();
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

#define PLATTER_OPA (bz_ui_dark() ? 56 : 255)
/* the tone changed: the platter's strength with it */
static void dock_theme(void)
{
    if (U.platter) lv_obj_set_style_bg_opa(U.platter, PLATTER_OPA, 0);
}

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
    go_page((int)(intptr_t)u);
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
    go_page(page);
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
    /* its baked surface hides the page under it: lists scrolling under it are cut around its middle rather
     * than carrying it along and drawing it again every frame (the rounded ends are still carried) */
    bz_ui_opaque_core(U.dock, BZ_R_DOCK, 0);

    /* the platter under the current item: the accent, as M3's navigation indicator is -- a breath of it on the dark
     * tone, its full container on the light one (Bezel's droplet, landed) */
    U.platter = lv_obj_create(U.dock);
    lv_obj_remove_style_all(U.platter);
    lv_obj_set_size(U.platter, DOCK_ITEM_W, DOCK_ITEM_H);
    lv_obj_add_style(U.platter, bz_style_fill(BZ_C_ICE), 0);
    lv_obj_set_style_bg_opa(U.platter, PLATTER_OPA, 0);
    bz_theme_on_change(dock_theme);
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
        if (!ui_look()->dock_labels) lv_obj_add_flag(U.dock_labels[i], LV_OBJ_FLAG_HIDDEN);
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
    /* Every LVGL set restyles and redraws (9.5), even to the same value: each is made only on a change.
     * This ran every frame and redrew the dock under every scrolling list. */
    float pos = -U.offset.value / W;
    static int styled = -1, platter_x = -99999, dock_y = -99999;
    if ((int)lroundf(pos) != styled) {
        styled = (int)lroundf(pos);
        dock_style_items(pos);
    }
    /* the platter follows the pager continuously */
    int px = (int)lroundf(DOCK_PAD + pos * (DOCK_ITEM_W + DOCK_GAP));
    if (px != platter_x) {
        platter_x = px;
        lv_obj_set_pos(U.platter, px, DOCK_PAD);
    }

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
        lv_obj_set_style_bg_opa(U.platter, (lv_opa_t)(PLATTER_OPA * (1 - lift)), 0);
    } else if (bz_glass_strength(U.droplet_glass) > 0.5f && !U.drop_dragging) {
        bz_glass_show(U.droplet_glass, false);
        if (lv_obj_get_style_bg_opa(U.platter, 0) != PLATTER_OPA) lv_obj_set_style_bg_opa(U.platter, PLATTER_OPA, 0);
    }

    /* the dock tucks away while an app is open, and after 4 s idle on a page (panel.js:34,166) */
    static double boot0;
    if (boot0 == 0) boot0 = g_now;
    bool tuck = U.app != NULL || ui_home_mode_active() || g_now - boot0 < 0.5; /* at power-on it waits for the blind to start lifting */
    if (bz_motion_tick(&U.dock_tuck)) bz_ui_keep_alive();
    float target = tuck ? 1 : 0;
    if (U.dock_tuck.target != target) {
        bz_motion_to(&U.dock_tuck, target, tuck ? BZ_SMOOTH : BZ_RELEASE);
        bz_glass_show(U.dock_glass, !tuck);
    }
    int dy = (int)(-DOCK_BOTTOM + 46 * U.dock_tuck.value);
    if (dy != dock_y) {
        dock_y = dy;
        lv_obj_set_y(U.dock, dy);
    }
    bool gone = U.dock_tuck.value > 0.98f;
    if (gone != lv_obj_has_flag(U.dock, LV_OBJ_FLAG_HIDDEN)) {
        if (gone) lv_obj_add_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(U.dock, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ island */

/* The island rests as an orb in the top-right corner, never over the content: the robot's state is its
 * colour and mark (connected ●, a warning ◆, a fault ■, looking ○), a small amber pip says there are
 * notifications not seen yet, and a tap opens the control center, which lists them. A message grows out of
 * it: the orb pulses amber, a capsule stretches leftwards from the corner on `release`, its words fading in
 * once it is 55 % grown, holds ISL_HOLD_S and shrinks back into the orb on `smooth` (Bezel's top island;
 * Detent's Dynamic Island morph: the orb stays where it is while the capsule grows from it).
 *
 * Lean: every changed pixel is turned for the portrait panel, so only the capsule's own strip (≤ ISL_MAX_W ×
 * ORB_D) redraws while it moves, only the orb's disc while it pulses, and nothing at rest. Nothing moves
 * while the platform slides a picture (a page, a sheet): it waits and carries on after. */
#define ISL_MAX_W 660
#define ISL_HOLD_S 3.0
#define ISL_PULSE_S 0.5      /* one amber pulse */
#define ISL_PULSES 3
#define ISL_PAD 22           /* the capsule's left padding */
#define ISL_ICON (BZ_LEAN ? 30 : 24)
#define ISL_MARK 14

static struct {
    lv_obj_t *obj, *disc, *mark, *pip, *icon, *text;
    lv_obj_t *act_icon, *act_time; /* a live activity sitting in the orb: its icon, or a countdown's time left */
    bz_motion_t w;           /* the capsule's width: ORB_D at rest */
    int msg_w;               /* its width with the message showing */
    int shown_w, text_opa;
    double until;            /* the message shrinks back in at this time (0: none showing) */
    double flash_at;         /* the amber pulses began (0: none) */
    bz_status_t st;
    uint32_t disc_rgb;       /* the disc's fill as set */
    unsigned seen_gen;       /* notifications as of the last look at the control center */
    bool pip_on;
    /* the link, told as it changes (not kept as a notification) */
    bool conn, told, ever;
    double conn_at, told_at;
} IS = { .text_opa = -1, .st = BZ_INFO };

static bool island_act_words(void); /* an activity sitting in the orb shows its words: false if there is none */

static void island_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    bz_ui_wake();
    /* a live activity folded into the orb: the first tap says what it is; with its words out, the control
     * center as always */
    if (IS.until == 0 && IS.w.target <= ORB_D && island_act_words()) return;
    ui_cc_open(); /* the notifications, and the link's detail */
}

/* The dev console's UI commands (tools/tab5_dev.py): taken on the console's task, run on the UI's loop. */
static char s_dev_cmd[192]; /* room for a typed question ("ask ...", "assist ask ...") */
static volatile bool s_dev_pending;

/* the dev console's "perf": what a band of the neighbour costs, split — refreshing the page's numbers,
 * drawing the band, turning it into the panel's orientation — and the worst band frame of the three
 * together. A band frame competes with the slide's own DMA copies for the same PSRAM. */
static double s_band[4];
static int s_bandn;

/* "psram": who holds the 32 MB, not merely how much is left.
 *
 * The free figure on its own has never been enough to act on. Almost all of the PSRAM is spoken for
 * before the first screen is built -- the program itself runs from it (XIP), the panel scans three frame
 * buffers out of it, and the renderer and the slide each keep whole screens of it -- and the part the UI
 * can actually spend is what is left after that. This prints the fixed blocks first, so the figure at
 * the bottom is read against them, and then the three pools that grow while the tablet is used: LVGL's
 * objects, the baked glass, and the app windows that are built once and never taken down.
 *
 * `internal` is the one to watch. When PSRAM runs out, bz_lv_mem hands LVGL internal RAM instead of
 * failing, and internal RAM is the scarce kind that the DMA reserve, the Wi-Fi link and every task stack
 * come out of. A non-zero count there means the UI is quietly eating the memory the system needs. */
static bool s_mem_force; /* dev "psram shed": one pass of the pressure path whatever is free */

static void mem_line(const char *s) { fputs(s, stdout); }

static void mem_report(void)
{
    size_t m[4];
    hal_mem_psram(m);
    printf("MEM PSRAM: %u KB free, largest block %u KB\n", (unsigned)(m[1] / 1024), (unsigned)(m[2] / 1024));
    /* Not a memory figure at all, but the one that explains a cyan flash, and this is where anyone
     * looking at a stall will read it: the panel missing its deadline is a bandwidth fault, and the cure
     * is less PSRAM traffic, not more free bytes. */
    printf("MEM panel underruns (cyan frames): %u since boot\n", (unsigned)hal_underruns());
    printf("MEM fixed, taken at boot and never given back: %u KB\n", (unsigned)(m[0] / 1024));
    hal_mem_psram_lines(mem_line);
    bz_ui_mem_lines(mem_line);

    size_t held = 0, peak = 0;
    int blocks = 0, internal = 0;
    bz_lv_mem_stat(&held, &peak, &blocks, &internal);
    printf("MEM growing with use:\n");
    printf("MEM   LVGL objects            %4u KB  (peak %u KB, %d blocks%s)\n", (unsigned)(held / 1024),
           (unsigned)(peak / 1024), blocks, internal ? ", SOME IN INTERNAL RAM" : "");
    if (internal)
        printf("MEM   !! %d LVGL blocks fell back to internal RAM: PSRAM ran out while the UI was building\n",
               internal);
    printf("MEM   baked glass             %4u KB\n", (unsigned)(bz_frost_bytes() / 1024));
    printf("MEM   app icons               %4u KB\n", (unsigned)(ui_icon_bytes() / 1024));
    printf("MEM   ground                  %4u KB\n", (unsigned)(ui_ground_bytes() / 1024));
    size_t win = 0;
    for (int i = 0; i < U.nbuilt; i++) win += U.built[i].bytes;
    /* What nothing above names: the heap's handed-out bytes less the fixed blocks, the renderer's buffers and the
     * pools just listed. It is where a leak outside LVGL shows (a cache that grows, a buffer never freed, a
     * stuck TLS session or HTTP body), because every named pool is accounted for and this is the rest. */
    size_t ablocks = 0;
    size_t alloc = hal_psram_allocated(&ablocks);
    size_t named = m[0] + bz_ui_mem_bytes() + held + bz_frost_bytes() + ui_icon_bytes() + ui_ground_bytes();
    printf("MEM   everything else         %4d KB  (the heap holds %u blocks: the system's and the apps' mallocs)\n",
           (int)(((long)alloc - (long)named) / 1024), (unsigned)ablocks);
    printf("MEM   app windows built       %4d of %d, %u KB of the LVGL figure above\n", U.nbuilt, MAX_APPS,
           (unsigned)(win / 1024));
    for (int i = 0; i < U.nbuilt; i++)
        printf("MEM     %-14s %5u KB  last opened %.0f s ago%s\n",
               U.built[i].app && U.built[i].app->name ? U.built[i].app->name : "?",
               (unsigned)(U.built[i].bytes / 1024), ui_now() - U.built[i].used,
               U.built[i].app && U.built[i].app->shed ? "  (may be shed)" : "");
}

static bool dev_handler(const char *line)
{
    if (strncmp(line, "open ", 5) && strncmp(line, "page ", 5) && strcmp(line, "close") && strcmp(line, "perf") && strncmp(line, "inv", 3) && strcmp(line, "redraw") && strcmp(line, "home") && strncmp(line, "ask ", 4) && strncmp(line, "phone", 5) &&
        strncmp(line, "alarm test", 10) && strncmp(line, "match fake", 10) && strncmp(line, "bms", 3) && strncmp(line, "say ", 4) && strncmp(line, "act", 3) &&
        strncmp(line, "accent ", 7) && strncmp(line, "settings ", 9) && strncmp(line, "voice", 5) && strncmp(line, "look ", 5) &&
        strncmp(line, "docwrite", 8) && strncmp(line, "assist", 6) && strncmp(line, "memo", 4) && strncmp(line, "carry", 5) &&
        strncmp(line, "cut", 3) && strncmp(line, "hm", 2) && strncmp(line, "pca", 3) && strncmp(line, "psram", 5))
        return false;
    if (s_dev_pending) return false;
    snprintf(s_dev_cmd, sizeof s_dev_cmd, "%s", line);
    s_dev_pending = true;
    return true;
}

/* the dev console's "perf": seconds spent in each part of shell_frame since it last asked */
static double s_prof[12];
static const char *const s_prof_name[12] = { "slide", "caches", "dock", "orient", "windows", "perf", "model",
                                             "assist", "island", "pages", "app", "rest" };
static double s_batt_s; /* of the island's refresh, the part spent inside hal_battery */

/* A document that exercises everything the renderer draws, written onto the card through the very
 * function the assistant's document tool calls. It is the only way to see a chart and a drawing on the
 * tablet without a model in the loop, and the syntax it shows is the syntax docs/render-blocks.md
 * specifies — if one drifts from the other, this is what catches it. */
static const char DOC_SAMPLE[] =
    "# What this tablet draws\n"
    "\n"
    "This page is written by the dev console and rendered by the same code as an answer from the\n"
    "assistant. If any of it looks wrong here, it looks wrong there.\n"
    "\n"
    "## Text\n"
    "\n"
    "A paragraph with **bold words** in it, a `monospaced run`, and ordinary prose around them so the\n"
    "faces can be compared on the same line.\n"
    "\n"
    "- a bullet, which the body face draws with an em dash\n"
    "- a second one, long enough to wrap onto another line and show that the text hangs under itself\n"
    "\n"
    "1. numbered\n"
    "2. also numbered\n"
    "\n"
    "> a quoted line, dimmed, with a rule down its left\n"
    "\n"
    "---\n"
    "\n"
    "## A chart\n"
    "\n"
    "```chart\n"
    "line battery through a match\n"
    "x 0 15 30 45 60 75 90 105 120\n"
    "y 12.6 12.4 12.1 11.7 12.0 11.9 11.6 11.4 11.8\n"
    "unit V\n"
    "```\n"
    "\n"
    "```chart\n"
    "bar current by mechanism\n"
    "y 34 18 52 7\n"
    "labels drive, arm, shooter, intake\n"
    "unit A\n"
    "```\n"
    "\n"
    "## A chart that is wrong on purpose\n"
    "\n"
    "```chart\n"
    "line two series of different lengths\n"
    "y 1 2 3 4\n"
    "y 1 2\n"
    "```\n"
    "\n"
    "## A drawing\n"
    "\n"
    "```svg\n"
    "<svg viewBox=\"0 0 200 90\">\n"
    "  <rect x=\"6\" y=\"6\" width=\"188\" height=\"78\" rx=\"8\" fill=\"none\" stroke=\"#7f8ea3\" stroke-width=\"2\"/>\n"
    "  <circle cx=\"40\" cy=\"45\" r=\"22\" fill=\"#8f7ae6\"/>\n"
    "  <path d=\"M74 66 L96 24 L118 66 Z\" fill=\"none\" stroke=\"#5fcf8f\" stroke-width=\"3\"/>\n"
    "  <line x1=\"130\" y1=\"66\" x2=\"178\" y2=\"24\" stroke=\"#e6b45f\" stroke-width=\"3\"/>\n"
    "  <text x=\"100\" y=\"82\" font-size=\"9\" text-anchor=\"middle\">shapes</text>\n"
    "</svg>\n"
    "```\n"
    "\n"
    "## Code\n"
    "\n"
    "```\n"
    "10.58.5.2:8765\n"
    "devtools gradle -- deploy\n"
    "```\n";

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
    } else if (!strncmp(s_dev_cmd, "alarm test", 10)) {
        /* a made-up match's alarm, in 5 s or "alarm test N" s (time to switch apps or let it sleep): the queue
         * reminder, or "alarm test N match" the match one, "alarm test N chime" a schedule change's chime */
        const char *a = s_dev_cmd + 10;
        double d = *a == ' ' ? atof(a + 1) : 5;
        int kind = strstr(a, "match") ? 1 : strstr(a, "chime") ? 2 : 0;
        ui_match_test_kind(d > 0 ? d : 5, kind);
    } else if (!strncmp(s_dev_cmd, "match fake", 10)) {
        /* a made-up match of ours "match fake N" minutes out (default 30), tracked as a real one: its reminders
         * ring, home shows it, the assistant knows it; "match fake 0" takes it away */
        const char *a = s_dev_cmd + 10;
        ui_match_fake(*a == ' ' ? atof(a + 1) : 30);
    } else if (!strncmp(s_dev_cmd, "say ", 4)) {
        /* a message out of the island, as any notification comes */
        ui_island_say(BZ_I_INFO, s_dev_cmd + 4);
    } else if (!strncmp(s_dev_cmd, "act", 3)) {
        /* "act N words": a live activity counting down N s (0: none) under those words; "act end [words]"
         * ends the newest, leaving the words as a message */
        static int ids[8], n;
        const char *a = s_dev_cmd + 3;
        if (!strncmp(a, " end", 4)) {
            if (n) ui_activity_end(ids[--n], a[4] == ' ' ? a + 5 : NULL);
        } else if (*a == ' ') {
            char *rest;
            double secs = strtod(a + 1, &rest);
            while (*rest == ' ') rest++;
            int id = ui_activity_begin(secs > 0 ? BZ_I_TIMER : BZ_I_EDIT_NOTE, *rest ? rest : "working", secs);
            if (n < 8) ids[n++] = id;
        }
    } else if (!strncmp(s_dev_cmd, "accent ", 7)) {
        /* "accent violet" or "accent 3" */
        const char *a = s_dev_cmd + 7;
        int k = *a >= '0' && *a <= '9' ? atoi(a) : -1;
        for (int i = 0; k < 0 && i < BZ_NACCENTS; i++)
            if (!strcmp(a, BZ_ACCENTS[i].name)) k = i;
        if (k >= 0) ui_set_accent(k);
    } else if (!strncmp(s_dev_cmd, "settings ", 9)) {
        ui_settings_show(s_dev_cmd + 9);
    } else if (!strncmp(s_dev_cmd, "look ", 5)) {
        ui_look_dev(s_dev_cmd + 5); /* "look ground aurora", "look icons tinted", "look tone light"... */
    } else if (!strncmp(s_dev_cmd, "bms", 3)) {
        /* the battery fleet: "bms" ranks it; scan, pick <label>, demo, reset, gpt, json */
        ui_batt_dev(s_dev_cmd + 3);
    } else if (!strncmp(s_dev_cmd, "psram", 5)) {
        /* "psram shed" asks for one pass of the pressure path whatever the free figure is: it is the
         * only way to see a window taken down and built again without first filling the heap. */
        if (!strcmp(s_dev_cmd + 5, " shed")) s_mem_force = true;
        else mem_report();
    } else if (!strncmp(s_dev_cmd, "inv", 3)) {
        /* "inv" or "inv N": log every area LVGL invalidates for the next N frames (20 by default). A
         * gesture is worth 200: the point is to catch what asks for the whole screen. */
        int n = s_dev_cmd[3] == ' ' ? atoi(s_dev_cmd + 4) : 20;
        bz_ui_trace_inv(n > 0 ? n : 20);
    } else if (!strncmp(s_dev_cmd, "ask ", 4)) {
        /* a typed question to the companion, as its keyboard would send it */
        voice_ask(s_dev_cmd + 4);
    } else if (!strncmp(s_dev_cmd, "carry", 5)) {
        /* "carry 0": a card or a hint over a scrolling list makes the whole list redraw again, as it did
         * before. "carry 1": it is carried with the list. Both costs, on one build, on one screen. */
        bool on = s_dev_cmd[5] != ' ' || s_dev_cmd[6] != '0';
        bz_ui_scroll_carry(on);
        printf("carry: %s\n", on ? "on" : "off");
    } else if (!strncmp(s_dev_cmd, "pca", 3)) {
        ui_pcaudio_dev(s_dev_cmd); /* on, off, stat (ui_pcaudio.c) */
    } else if (!strncmp(s_dev_cmd, "cut", 3)) {
        /* "cut 0": a list scrolling under the dock carries it and draws it again every frame, as it did.
         * "cut 1": the list is shifted in the pieces around the dock's opaque middle. */
        bool on = s_dev_cmd[3] != ' ' || s_dev_cmd[4] != '0';
        bz_ui_scroll_cut(on);
        printf("cut: %s\n", on ? "on" : "off");
    } else if (!strncmp(s_dev_cmd, "assist fake", 11)) {
        /* "assist fake N": N turns of the sample document into the transcript, no model and no network.
         * Scrolling a long answer is the app's dearest frame, and measuring it twice the same way needs
         * the same answer twice — so it starts a new conversation, exactly as the "new" button does. */
        int n = s_dev_cmd[11] == ' ' ? atoi(s_dev_cmd + 12) : 2;
        assist_reset();
        for (int i = 0; i < (n > 0 ? n : 2); i++) assist_fake("what does this tablet draw?", DOC_SAMPLE);
        ui_app_open(&APP_ASSIST, NULL);
    } else if (!strncmp(s_dev_cmd, "assist", 6) || !strncmp(s_dev_cmd, "memo", 4)) {
        ui_assist_dev(s_dev_cmd); /* ask, new, dump; memory (ui_app_assist.c) */
    } else if (!strncmp(s_dev_cmd, "docwrite", 8)) {
        /* the assistant's own document tool, driven from the cable: writes the sample onto the card */
        const char *nm = s_dev_cmd[8] == ' ' ? s_dev_cmd + 9 : "render sample";
        char why[128] = "";
        bool ok = ui_docs_write(nm, DOC_SAMPLE, why, sizeof why);
        printf("docwrite: %s%s%s\n", ok ? "written" : "refused", ok ? "" : ": ", ok ? "" : why);
    } else if (!strncmp(s_dev_cmd, "voice", 5)) {
        /* The companion's voice, from the cable. The spoken path is the one that is hard to reproduce at a
         * desk, and it is where it was failing: "voice" reports the state, the note and the last reply,
         * "voice talk" records as a tap on the face would, "voice stop" cancels, "voice new" forgets. */
        const char *a = s_dev_cmd[5] == ' ' ? s_dev_cmd + 6 : "";
        if (!strncmp(a, "talk", 4)) {
            voice_listen();
            printf("voice: listening (as a tap)\n");
        } else if (!strncmp(a, "stop", 4)) {
            voice_cancel();
            printf("voice: cancelled\n");
        } else if (!strncmp(a, "new", 3)) {
            voice_reset();
            printf("voice: a new conversation\n");
        } else {
            static const char *const STATE[] = { "off", "idle", "listening", "hearing", "thinking", "speaking" };
            vo_state_t st = voice_state();
            char why[96] = "";
            bool ready = voice_ready(why, sizeof why);
            char note[160];
            bool nerr = false;
            double at = voice_note(note, sizeof note, &nerr);
            voice_config_t c;
            voice_config(&c);
            const char *w = voice_wake_word();
            printf("voice: %s, microphones %s, wake word %s (setting %s), ready %s%s%s\n",
                   STATE[st < 6 ? st : 0], voice_mics_on() ? "on" : "off", w ? w : "not loaded",
                   c.wake ? "on" : "off", ready ? "yes" : "no", ready ? "" : ": ", ready ? "" : why);
            printf("voice: level %.2f, out %s, follow %s, stt %s, chat %s, tts %s\n", voice_mic_level(),
                   c.out == VO_OUT_SPEAK ? "speak" : c.out == VO_OUT_SHOW ? "show" : "both", c.follow ? "on" : "off",
                   c.stt_model[0] ? c.stt_model : VO_STT_DEFAULT, c.chat_model[0] ? c.chat_model : "the assistant's",
                   c.tts_model[0] ? c.tts_model : VO_TTS_DEFAULT);
            if (note[0])
                printf("voice: note %.0f s ago%s: %s\n", hal_seconds() - at, nerr ? " (a failure)" : "", note);
            vo_reply_t *r = malloc(sizeof *r);
            if (r) {
                if (voice_reply(r) && r->seq)
                    printf("voice: reply #%u%s: you said \"%s\" -> \"%.120s\"\n", (unsigned)r->seq,
                           r->failed ? " (failed)" : "", r->heard, r->say);
                free(r);
            }
        }
    } else if (!strcmp(s_dev_cmd, "home")) {
        if (ui_home_mode_active()) ui_home_mode_exit();
        else ui_home_mode_enter();
    } else if (!strncmp(s_dev_cmd, "hm", 2)) {
        ui_home_mode_dev(s_dev_cmd + 2); /* home mode's own: its pages, faces, night, covers */
    } else if (!strncmp(s_dev_cmd, "phone", 5)) {
        /* "phone", "phone 1", "phone 0": which frame the interface is laid out in, from the cable.
         *
         * It exists because driving the control centre by synthetic swipes to reach this toggle has
         * now twice produced a test that quietly did nothing -- the tap missed, the mode never
         * changed, and two hundred seconds of swipes went to coordinates off the bottom of a
         * landscape screen while reporting "no crash". A switch that cannot be set reliably cannot
         * be tested reliably. */
        bool want = s_dev_cmd[5] == ' ' ? s_dev_cmd[6] != '0' : !hal_phone();
        printf("phone: %s -> %s\n", hal_phone() ? "on" : "off", want ? "on" : "off");
        ui_phone_set(want);
    } else if (!strcmp(s_dev_cmd, "redraw")) {
        /* the whole screen drawn again: a panel shot before and after shows what a present left stale */
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    } else if (!strcmp(s_dev_cmd, "perf")) {
        /* where a frame's time has gone since the last "perf": the frame hooks, LVGL's handler, its
         * refresh (layout + render) and the render alone, per frame; and the presents */
        float hk, lv, rf, rd, hkm, lvm;
        bz_ui_split_max(&hkm, &lvm);
        bz_ui_split(&hk, &lv, &rf, &rd);
        printf("  slowest: hooks %.1f lvgl %.1f ms\n", hkm, lvm);
        bz_ui_perf_t pf;
        bz_ui_perf(&pf);
        printf("perf: hooks %.1f lvgl %.1f refresh %.1f render %.1f ms/frame; present %.1f ms, %.0f fps\n", hk, lv, rf, rd,
               pf.present_ms, pf.fps);
        printf("  frame %.1f ms: compose %.1f\n", pf.frame_ms, pf.compose_ms);
        bz_ui_hooks_report();
        for (int i = 0; i < 12; i++) {
            printf("  shell %s %.0f ms\n", s_prof_name[i], s_prof[i] * 1000);
            s_prof[i] = 0;
        }
        printf("  shell batt %.0f ms (of island)\n", s_batt_s * 1000);
        s_batt_s = 0;
        int gp[4];
        hal_frame_gaps(gp);
        double lp[4];
        hal_loop_prof(lp);
        printf("  loops: %.0f busy, work mean %.1f max %.1f ms; %.0f idle\n", lp[0], lp[1] * 1000, lp[2] * 1000, lp[3]);
        int lh[5];
        hal_loop_hist(lh);
        printf("  loops: work <5 ms %d, <10 %d, <16.7 %d, <25 %d, more %d\n", lh[0], lh[1], lh[2], lh[3], lh[4]);
        printf("  frames: %d at 60, %d at 30, %d at 20, %d slower\n", gp[0], gp[1], gp[2], gp[3]);
        double pp[6];
        hal_present_prof(pp);
        if (pp[5] > 0)
            printf("  present x%.0f: pick %.1f catch-up %.1f scroll %.1f rotate %.1f hand-over %.1f ms each\n", pp[5],
                   pp[0] * 1000 / pp[5], pp[1] * 1000 / pp[5], pp[2] * 1000 / pp[5], pp[3] * 1000 / pp[5],
                   pp[4] * 1000 / pp[5]);
        double sp[4];
        hal_slide_prof(sp);
        if (sp[0] > 0)
            printf("  composed x%.0f: %.1f ms each (chrome %.1f), worst %.1f ms\n", sp[0], sp[1] * 1000 / sp[0],
                   sp[3] * 1000 / sp[0], sp[2] * 1000);
        double off[3];
        bz_ui_offscreen_prof(off);
        if (s_bandn)
            printf("  bands x%d: refresh %.0f render %.0f patch %.0f ms (of the render: ready %.0f draw %.0f back"
                   " %.0f), worst band frame %.1f ms\n",
                   s_bandn, s_band[0] * 1000, s_band[1] * 1000, s_band[2] * 1000, off[0] * 1000, off[1] * 1000,
                   off[2] * 1000, s_band[3] * 1000);
        s_band[0] = s_band[1] = s_band[2] = s_band[3] = 0;
        s_bandn = 0;
        uint32_t ipx = 0;
        int inv = bz_ui_big_inv(&ipx);
        printf("  whole-screen redraws: %d (%u px)\n", inv, (unsigned)ipx);
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
    /* the status cluster, right of the band and left of the orb: how the tablet reaches the robot, its
     * battery, the time. Made first: the capsule grows over it. */
    U.status = bz_row(g, 14);
    lv_obj_set_flex_align(U.status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    U.st_link = bz_icon(U.status, BZ_I_LINK_OFF, 24, BZ_C_DIM);
    U.st_batt_icon = bz_icon(U.status, BZ_I_BATTERY_FULL, 24, BZ_C_INK);
    U.st_batt = bz_label(U.status, "", BZ_F_LABEL, BZ_C_INK);
    if (!ui_look()->batt_pct) lv_obj_add_flag(U.st_batt, LV_OBJ_FLAG_HIDDEN);
    U.st_clock = bz_label(U.status, "--:--", BZ_F_BODY, BZ_C_INK);
    lv_obj_update_layout(U.status);
    /* centred on the orb */
    lv_obj_align(U.status, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, ORB_Y + (ORB_D - lv_obj_get_height(U.status)) / 2);
    /* like a phone's status bar: a tap opens the control center (it pulls down from the top edge too) */
    lv_obj_add_flag(U.status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(U.status, 16);
    bz_on_tap(U.status, status_tap, NULL);

    /* the capsule: surface2, as the calm glass is; at rest exactly the orb */
    IS.obj = lv_obj_create(g);
    lv_obj_remove_style_all(IS.obj);
    lv_obj_add_style(IS.obj, bz_style_fill(BZ_C_SURFACE2), 0);
    lv_obj_set_style_radius(IS.obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(IS.obj, ORB_D, ORB_D);
    lv_obj_set_pos(IS.obj, W - PAD - ORB_D, ORB_Y);
    lv_obj_remove_flag(IS.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(IS.obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(IS.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(IS.obj, 20); /* 88 px to the finger: 48 dp */
    bz_on_tap(IS.obj, island_tap, NULL);
    /* the message: icon and words right-aligned against the orb, so the capsule uncovers them as it grows */
    IS.icon = bz_icon(IS.obj, BZ_I_INFO, 24, BZ_C_INK);
    IS.text = bz_label_line(IS.obj, "", BZ_F_LABEL, BZ_C_INK, 10);
    lv_obj_add_flag(IS.icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(IS.text, LV_OBJ_FLAG_HIDDEN);
    /* the orb itself: a disc in the state's colour at the capsule's right end, the mark on it */
    IS.disc = lv_obj_create(IS.obj);
    lv_obj_remove_style_all(IS.disc);
    lv_obj_set_size(IS.disc, ORB_D, ORB_D);
    lv_obj_set_style_radius(IS.disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(IS.disc, LV_OPA_COVER, 0);
    lv_obj_align(IS.disc, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(IS.disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(IS.disc, LV_OBJ_FLAG_EVENT_BUBBLE);
    IS.mark = bz_mark(IS.disc, BZ_STALE, ISL_MARK);
    lv_obj_center(IS.mark);
    /* in place of the mark while an activity is live */
    IS.act_icon = bz_icon(IS.disc, BZ_I_TIMER, 24, BZ_C_INK);
    lv_obj_center(IS.act_icon);
    lv_obj_add_flag(IS.act_icon, LV_OBJ_FLAG_HIDDEN);
    IS.act_time = bz_label(IS.disc, "", BZ_F_CAPTION, BZ_C_INK);
    lv_obj_center(IS.act_time);
    lv_obj_add_flag(IS.act_time, LV_OBJ_FLAG_HIDDEN);
    /* notifications not seen yet: a pip on the orb's shoulder */
    IS.pip = lv_obj_create(IS.obj);
    lv_obj_remove_style_all(IS.pip);
    lv_obj_add_style(IS.pip, bz_style_fill(BZ_C_AMBER), 0);
    lv_obj_set_style_radius(IS.pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(IS.pip, 12, 12);
    lv_obj_align(IS.pip, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_remove_flag(IS.pip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(IS.pip, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(IS.pip, LV_OBJ_FLAG_HIDDEN);
    bz_motion_init(&IS.w, ORB_D, 0.5f);
    IS.w.keep = true; /* it grows though the lean renderer makes other motion instant: a strip, cheap */
    IS.shown_w = IS.msg_w = ORB_D;
    IS.seen_gen = ui_notify_gen();
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
    double t_batt_ = bz_ui_clock();
    bool batt_ok_ = hal_battery(&b);
    s_batt_s += bz_ui_clock() - t_batt_;
    if (batt_ok_ && b.ok) {
        const char *bi = b.charging ? BZ_I_BATTERY_CHARGING_FULL : b.percent > 70 ? BZ_I_BATTERY_FULL
                         : b.percent > 40 ? BZ_I_BATTERY_5_BAR : b.percent > 15 ? BZ_I_BATTERY_3_BAR : BZ_I_BATTERY_ALERT;
        if (strcmp(lv_label_get_text(U.st_batt_icon), bi)) lv_label_set_text(U.st_batt_icon, bi);
        bz_set_color(U.st_batt_icon, b.percent <= 15 && !b.charging ? BZ_C_WARN : BZ_C_INK);
        ui_text(U.st_batt, "%d%%", b.percent);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) {
        char hm[16];
        ui_fmt_hm(hm, sizeof hm, &tm);
        ui_text(U.st_clock, "%s", hm);
    } else {
        ui_text(U.st_clock, "--:--");
    }
}

/* settings > look changed the clock's hours, the battery's percent or the dock's words: shown now */
void ui_status_look_changed(void)
{
    const ui_look_t *l = ui_look();
    if (l->batt_pct) lv_obj_remove_flag(U.st_batt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.st_batt, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < NPAGES; i++) {
        if (l->dock_labels) lv_obj_remove_flag(U.dock_labels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(U.dock_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    status_refresh();
}

/* The capsule's words: set and measured, the capsule sent to their width (a change of width only; the words
 * are re-measured only when they change). */
static void island_words(const char *icon, const char *text)
{
    const char *ic = icon ? icon : BZ_I_INFO;
    if (strcmp(lv_label_get_text(IS.icon), ic)) lv_label_set_text(IS.icon, ic);
    if (strcmp(lv_label_get_text(IS.text), text) || IS.msg_w == ORB_D) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, bz_font(BZ_F_LABEL), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int room = ISL_MAX_W - ISL_PAD - ISL_ICON - 10 - (ORB_D - 4);
        int tw = sz.x < room ? sz.x + 2 : room;
        ui_text(IS.text, "%s", text);
        if (lv_obj_get_width(IS.text) != tw) lv_obj_set_width(IS.text, tw);
        lv_obj_align(IS.text, LV_ALIGN_RIGHT_MID, -(ORB_D - 4), 0);
        lv_obj_align(IS.icon, LV_ALIGN_RIGHT_MID, -(ORB_D - 4 + tw + 10), 0);
        IS.msg_w = ISL_PAD + ISL_ICON + 10 + tw + ORB_D - 4;
    }
    if (fabsf(IS.w.target - (float)IS.msg_w) > 0.5f) bz_motion_to(&IS.w, (float)IS.msg_w, BZ_RELEASE);
}

/* A message out of the orb, held a few seconds. A message arriving while one shows reshapes the capsule from
 * where it is and starts the hold again. */
static void island_show(const char *icon, const char *text, bool flash)
{
    if (!IS.obj) return;
    island_words(icon, text);
    IS.until = g_now + ISL_HOLD_S;
    if (flash) IS.flash_at = g_now;
    bz_ui_keep_alive();
}

/* ---- live activities: what is going on right now, kept on the island while it lasts ----
 *
 * A timer counting down, the assistant writing a document. Unlike a message, an activity holds the capsule
 * open until it ends; a message still shows over it for its few seconds, and the capsule goes back to the
 * activity after. Any thread may begin, update and end one (the assistant's workers do): they only write
 * this table under its lock, and the island's frame on the UI thread draws it. An activity's end can leave a
 * message behind, which is said the ordinary way (kept as a notification). */
#define ACT_MAX 4
#define ACT_STALE_S 90.0 /* a countdown nobody ended, this long after it reached zero: dropped */
static pthread_mutex_t s_act_lock = PTHREAD_MUTEX_INITIALIZER;
#define ACT_WORDS_S 4.0  /* an activity's words show this long when it begins (and when the orb is tapped) */
#define ACT_URGENT_S 10  /* a countdown's words show again for its last seconds */
static struct {
    int id;              /* 0: a free slot */
    char icon[8];
    char text[72];
    double ends;         /* hal_seconds() a countdown reaches zero (0: not a countdown) */
    double at;           /* when it began or last changed: the newest is the one shown */
    double words_until;  /* its words show until then; after, it sits in the orb */
} s_act[ACT_MAX];
static struct {
    bool pending;
    char icon[8];
    char text[96];
} s_act_said[ACT_MAX];
static int s_act_next = 1;
static unsigned s_act_gen;

static int act_slot(int id)
{
    for (int i = 0; i < ACT_MAX; i++) if (id > 0 && s_act[i].id == id) return i;
    return -1;
}

int ui_activity_begin(const char *icon, const char *text, double countdown_s)
{
    pthread_mutex_lock(&s_act_lock);
    int slot = -1;
    double oldest = 1e18;
    for (int i = 0; i < ACT_MAX && slot < 0; i++) if (!s_act[i].id) slot = i;
    if (slot < 0) /* all taken: the oldest gives way */
        for (int i = 0; i < ACT_MAX; i++) if (s_act[i].at < oldest) { oldest = s_act[i].at; slot = i; }
    int id = s_act_next++;
    if (s_act_next <= 0) s_act_next = 1;
    double now = hal_seconds();
    s_act[slot].id = id;
    snprintf(s_act[slot].icon, sizeof s_act[slot].icon, "%s", icon ? icon : BZ_I_AUTO_AWESOME);
    snprintf(s_act[slot].text, sizeof s_act[slot].text, "%s", text ? text : "");
    s_act[slot].ends = countdown_s > 0 ? now + countdown_s : 0;
    s_act[slot].at = now;
    s_act[slot].words_until = now + ACT_WORDS_S;
    s_act_gen++;
    pthread_mutex_unlock(&s_act_lock);
    return id;
}

void ui_activity_update(int id, const char *text)
{
    pthread_mutex_lock(&s_act_lock);
    int i = act_slot(id);
    if (i >= 0 && text && strcmp(s_act[i].text, text)) {
        snprintf(s_act[i].text, sizeof s_act[i].text, "%s", text);
        s_act_gen++;
    }
    pthread_mutex_unlock(&s_act_lock);
}

void ui_activity_end(int id, const char *said)
{
    pthread_mutex_lock(&s_act_lock);
    int i = act_slot(id);
    if (i >= 0) {
        if (said && said[0]) {
            s_act_said[i].pending = true;
            snprintf(s_act_said[i].icon, sizeof s_act_said[i].icon, "%s", s_act[i].icon);
            snprintf(s_act_said[i].text, sizeof s_act_said[i].text, "%s", said);
        }
        s_act[i].id = 0;
        s_act_gen++;
    }
    pthread_mutex_unlock(&s_act_lock);
}

bool ui_activity_live(int id)
{
    pthread_mutex_lock(&s_act_lock);
    bool live = act_slot(id) >= 0;
    pthread_mutex_unlock(&s_act_lock);
    return live;
}

/* UI thread, every frame: messages left by ended activities, and what the capsule shows while any is live.
 * Words change at most once a second (a countdown), so this costs a label and nothing else. true: an
 * activity holds the capsule open. */
static bool s_act_force; /* a message's hold ended: what the capsule shows is decided again */
static bool island_activities(double now)
{
    static unsigned gen = (unsigned)-1;
    static int last_sec = -1;
    static bool held;
    int sec = (int)now;
    if (gen == s_act_gen && sec == last_sec && !s_act_force) return held;
    s_act_force = false;
    held = false;
    char said_icon[ACT_MAX][8], said_text[ACT_MAX][96];
    int nsaid = 0;
    char icon[8] = "", text[112] = "", small[8] = "";
    bool words = false;
    int live = 0;
    pthread_mutex_lock(&s_act_lock);
    gen = s_act_gen;
    last_sec = sec;
    for (int i = 0; i < ACT_MAX; i++)
        if (s_act_said[i].pending) {
            s_act_said[i].pending = false;
            memcpy(said_icon[nsaid], s_act_said[i].icon, sizeof said_icon[0]);
            memcpy(said_text[nsaid], s_act_said[i].text, sizeof said_text[0]);
            nsaid++;
        }
    int show = -1;
    for (int i = 0; i < ACT_MAX; i++) {
        if (!s_act[i].id) continue;
        if (s_act[i].ends > 0 && now - s_act[i].ends > ACT_STALE_S) { /* ended by nobody */
            s_act[i].id = 0;
            continue;
        }
        live++;
        if (show < 0 || s_act[i].at > s_act[show].at) show = i;
    }
    if (show >= 0) {
        snprintf(icon, sizeof icon, "%s", s_act[show].icon);
        words = now < s_act[show].words_until;
        if (s_act[show].ends > 0) {
            int left = (int)ceil(s_act[show].ends - now);
            if (left < 0) left = 0;
            if (left >= 3600)
                snprintf(text, sizeof text, "%s \xc2\xb7 %d:%02d:%02d", s_act[show].text, left / 3600, left / 60 % 60, left % 60);
            else snprintf(text, sizeof text, "%s \xc2\xb7 %d:%02d", s_act[show].text, left / 60, left % 60);
            /* in the orb: "48s", "4:59", then whole units rounded up ("12m", "2h") where m:ss won't fit */
            if (left >= 3600) snprintf(small, sizeof small, "%dh", (left + 3599) / 3600);
            else if (left >= 600) snprintf(small, sizeof small, "%dm", (left + 59) / 60);
            else if (left >= 60) snprintf(small, sizeof small, "%d:%02d", left / 60, left % 60);
            else snprintf(small, sizeof small, "%ds", left);
            words |= left <= ACT_URGENT_S;
        } else {
            snprintf(text, sizeof text, "%s", s_act[show].text);
        }
        if (live > 1) {
            size_t l = strlen(text);
            snprintf(text + l, sizeof text - l, " \xc2\xb7 +%d", live - 1);
        }
    }
    pthread_mutex_unlock(&s_act_lock);
    for (int i = 0; i < nsaid; i++) ui_island_say(said_icon[i], said_text[i]);
    /* the orb: the robot's mark, or the activity folded into it (its time left, else its icon) */
    bool in_orb = live > 0;
    if (in_orb == !lv_obj_has_flag(IS.mark, LV_OBJ_FLAG_HIDDEN)) {
        if (in_orb) lv_obj_add_flag(IS.mark, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(IS.mark, LV_OBJ_FLAG_HIDDEN);
    }
    /* while the words are out they carry the time: the orb shows the icon then, not the time a second time */
    bool want_time = in_orb && small[0] && !(words && IS.until == 0), want_icon = in_orb && !want_time;
    if (want_time != !lv_obj_has_flag(IS.act_time, LV_OBJ_FLAG_HIDDEN)) {
        if (want_time) lv_obj_remove_flag(IS.act_time, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(IS.act_time, LV_OBJ_FLAG_HIDDEN);
    }
    if (want_icon != !lv_obj_has_flag(IS.act_icon, LV_OBJ_FLAG_HIDDEN)) {
        if (want_icon) lv_obj_remove_flag(IS.act_icon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(IS.act_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (want_time) {
        ui_text(IS.act_time, "%s", small);
        lv_obj_center(IS.act_time);
    }
    if (want_icon && strcmp(lv_label_get_text(IS.act_icon), icon)) lv_label_set_text(IS.act_icon, icon);
    if (!live) {
        if (IS.until == 0 && IS.w.target > ORB_D) bz_motion_to(&IS.w, ORB_D, BZ_SMOOTH);
        return false;
    }
    /* A message holding the capsule goes first. Otherwise the words show when the activity begins, when the
     * orb is tapped and in a countdown's last seconds; the rest of the time it sits folded into the orb, so it
     * never covers an app's buttons or the clock for long. */
    if (IS.until == 0) {
        if (words) island_words(icon, text);
        else if (IS.w.target > ORB_D) bz_motion_to(&IS.w, ORB_D, BZ_SMOOTH);
    }
    held = words;
    return words;
}

static bool island_act_words(void)
{
    double now = hal_seconds();
    bool any = false;
    pthread_mutex_lock(&s_act_lock);
    int show = -1;
    for (int i = 0; i < ACT_MAX; i++)
        if (s_act[i].id && (show < 0 || s_act[i].at > s_act[show].at)) show = i;
    if (show >= 0) {
        s_act[show].words_until = now + ACT_WORDS_S;
        s_act_gen++;
        any = true;
    }
    pthread_mutex_unlock(&s_act_lock);
    return any;
}

void ui_island_say(const char *icon, const char *text)
{
    ui_notify_add(icon, text); /* kept for the control center and the lock screen */
    island_show(icon, text, true);
}

/* The orb's colour: the state's tint over surface2, and toward amber while it pulses. */
static uint32_t island_disc_rgb(double now)
{
    uint32_t base = bz_color(BZ_C_SURFACE2);
    float tint = IS.st == BZ_OK ? 0.28f : IS.st == BZ_WARN ? 0.32f : IS.st == BZ_FAULT ? 0.4f : 0;
    uint32_t c = tint > 0 ? bz_mix(base, bz_color(bz_status_color(IS.st)), tint) : bz_color(BZ_C_SURFACE3);
    if (IS.flash_at) {
        double t = now - IS.flash_at;
        if (t >= ISL_PULSE_S * ISL_PULSES) {
            IS.flash_at = 0;
        } else {
            /* raised-cosine pulses, in 12 steps (each step is a restyle of the 48 px disc, nothing else) */
            float k = 0.5f - 0.5f * cosf((float)(2 * M_PI * t / ISL_PULSE_S));
            k = roundf(k * 12) / 12;
            c = bz_mix(c, bz_color(BZ_C_AMBER), 0.85f * k);
        }
    }
    return c;
}

/* Every frame: the capsule's spring, the words' fade, the orb's pulse. Only what changed is set. */
static void island_frame(double now)
{
    if (!IS.obj) return;
    if (SL.active || bz_ui_sheeting()) return; /* a picture is sliding: nothing drawn under it */
    if (IS.until && now >= IS.until) {
        IS.until = 0;
        s_act_force = true;
        bz_motion_to(&IS.w, ORB_D, BZ_SMOOTH); /* or back to a live activity, just below */
    }
    island_activities(now);
    bool moving = bz_motion_tick(&IS.w);
    float wv = IS.w.value;
    int w = (int)lroundf(wv < ORB_D ? ORB_D : wv > ISL_MAX_W + 24 ? ISL_MAX_W + 24 : wv);
    if (w != IS.shown_w) {
        IS.shown_w = w;
        lv_obj_set_width(IS.obj, w);
        lv_obj_set_x(IS.obj, W - PAD - w);
    }
    /* the words: in once the capsule is 55 % grown, out over its first 45 % back */
    float k = IS.msg_w > ORB_D ? (float)(w - ORB_D) / (float)(IS.msg_w - ORB_D) : 0;
    float f = (k - 0.55f) / 0.4f;
    int opa = f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 16) * 16;
    if (opa != IS.text_opa) {
        bool was = IS.text_opa > 0;
        IS.text_opa = opa;
        if ((opa > 0) != was) {
            if (opa > 0) {
                lv_obj_remove_flag(IS.icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(IS.text, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(IS.icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(IS.text, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (opa > 0) {
            lv_obj_set_style_text_opa(IS.icon, (lv_opa_t)opa, 0);
            lv_obj_set_style_text_opa(IS.text, (lv_opa_t)opa, 0);
        }
    }
    uint32_t c = island_disc_rgb(now);
    if (c != IS.disc_rgb) {
        IS.disc_rgb = c;
        lv_obj_set_style_bg_color(IS.disc, bz_lv_rgb(c), 0);
    }
    if (moving || IS.flash_at) bz_ui_keep_alive(); /* the hold needs no frames: the idle loop sees it end */
}

static void island_refresh(void)
{
    const cat_robot_t *r = R;
    bz_status_t st = !r->connected ? BZ_STALE : r->n_errors || r->estop ? BZ_FAULT : r->n_warnings ? BZ_WARN : BZ_OK;
    if (st != IS.st) {
        IS.st = st;
        bz_mark_set(IS.mark, st);
    }
    /* the pip: something new since the control center was last looked at */
    if (ui_cc_is_open()) IS.seen_gen = ui_notify_gen();
    bool pip = ui_notify_count() > 0 && IS.seen_gen != ui_notify_gen();
    if (pip != IS.pip_on) {
        IS.pip_on = pip;
        if (pip) lv_obj_remove_flag(IS.pip, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(IS.pip, LV_OBJ_FLAG_HIDDEN);
    }
    /* the link as it changes, once it has held a moment (a flapping link isn't news every second), in the
     * capsule only: the orb's colour is the lasting word */
    if (r->connected != IS.conn) {
        IS.conn = r->connected;
        IS.conn_at = g_now;
        IS.told = false;
    }
    if (!IS.told && g_now - IS.conn_at > (IS.conn ? 1.0 : 4.0) && (IS.told_at == 0 || g_now - IS.told_at > 15)) {
        IS.told = true;
        if (IS.conn) {
            char name[40], batt[16], msg[96];
            snprintf(name, sizeof name, "%s", r->have_identity ? r->name : "robot");
            for (char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
            snprintf(msg, sizeof msg, "%s \xc2\xb7 %s \xc2\xb7 %s v", name, cat_mode_name(r),
                     bz_fmt(batt, sizeof batt, r->have_battery, "%.2f", r->battery_v));
            island_show(BZ_I_SMART_TOY, msg, false);
            IS.ever = true;
            IS.told_at = g_now;
        } else if (IS.ever) {
            char msg[48];
            snprintf(msg, sizeof msg, "lost the robot \xc2\xb7 looking for %d", S.team);
            island_show(BZ_I_LINK_OFF, msg, false);
            IS.told_at = g_now;
        }
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
    for (int i = 0; i < U.nbuilt; i++)
        if (U.built[i].app == app) {
            U.built[i].used = ui_now();
            return U.built[i].inner;
        }
    /* What this one window costs, measured rather than guessed: LVGL's own counter either side of the
     * build. It is what "psram" prints per app and what eviction ranks by -- a window is worth taking
     * down for its bytes, and before this there was no way to know which ones had them. */
    size_t before = 0;
    bz_lv_mem_stat(&before, NULL, NULL, NULL);
    lv_obj_t *inner = bz_box(U.win);
    lv_obj_set_size(inner, W, H);
    lv_obj_add_flag(inner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    app->build(inner);
    size_t after = 0;
    bz_lv_mem_stat(&after, NULL, NULL, NULL);
    if (U.nbuilt < MAX_APPS) {
        U.built[U.nbuilt].app = app;
        U.built[U.nbuilt].inner = inner;
        U.built[U.nbuilt].bytes = after > before ? after - before : 0;
        U.built[U.nbuilt].used = ui_now();
        U.nbuilt++;
    }
    return inner;
}

/* ---- giving PSRAM back ----
 *
 * An app window is built once and then kept, which is what makes the second open instant. The cost is
 * that it is never given back: open all 39 apps and their widgets hold some 3.7 MB, out of the ~6.5 MB
 * the UI has to spend once the renderer and the panel have taken theirs. At the bottom of that the glass
 * cache cannot hold the dock's surfaces any more, so they are baked again and again -- tens of
 * milliseconds each, at no particular moment, which is what a random stall actually was.
 *
 * So below a floor the shell starts handing memory back, cheapest first: unreferenced baked glass, then
 * the windows of apps that said they can be rebuilt (`shed`), oldest-opened first. It only ever runs
 * while nothing is moving, because deleting a few thousand widgets is itself a frame or two.
 *
 * The floor is 1.5 MB, not "whatever is left": a single baked panel is a few hundred KB and a window
 * being built asks for a thousand small blocks, so the UI needs room to work in, not just room to exist. */
#define MEM_FLOOR (1536u * 1024u)
#define MEM_TARGET (2560u * 1024u) /* shed until this much is free, so it is not back at the floor next frame */

static void app_evict(const ui_app_t *app)
{
    for (int i = 0; i < U.nbuilt; i++) {
        if (U.built[i].app != app) continue;
        lv_obj_delete(U.built[i].inner);
        U.built[i] = U.built[--U.nbuilt];
        return;
    }
}

static double s_mem_quiet_until; /* nothing could be given back: don't scan again until then */

static void mem_pressure(void)
{
    /* The request is not consumed until the pass actually runs. It used to be cleared on entry, and the
     * gates below then threw it away on the first frame -- the dev command's own touch of the UI loop
     * counts as activity, so the very next frame is never a quiet one. */
    bool force = s_mem_force;
    if (!force && ui_now() < s_mem_quiet_until) return;
    /* Not every frame. hal_mem_psram asks the heap for its free size AND its largest free block, and the
     * second of those walks every free block on a 23 MB heap with the heap's lock held -- which is a
     * stall, on the UI thread, sixty times a second, to answer a question that cannot change meaningfully
     * in 16 ms. The first version of this did exactly that and made the tablet slower, which is the
     * opposite of the point. Twice a second is far more often than memory can run out. */
    static double looked;
    if (!force && ui_now() - looked < 0.5) return;
    looked = ui_now();
    size_t m[4];
    hal_mem_psram(m);
    /* The floor is what the tablet should normally never reach, which makes the shedding path the one
     * least likely to be exercised by accident -- so it can be asked for, and is, by the test that
     * proves a shed window builds again correctly. */
    if (!force && m[1] >= MEM_FLOOR) return;
    /* not mid-gesture, mid-slide or mid-animation: this deletes widgets, which costs a frame. And not
     * while anyone is typing: the keyboard lifts the body of whichever app owns the field, and deleting
     * a window out from under it would take the field away mid-word. */
    if (U.app || MC.pages || MC.window || bz_ui_thawing() || U.win_dragging || bz_ui_idle_s() < 1) {
        if (force) printf("mem: asked to shed, but the screen is busy (app %d pages %d win %d thaw %d "
                          "drag %d idle %.1f s)\n", U.app != NULL, MC.pages, MC.window, bz_ui_thawing(),
                          U.win_dragging, bz_ui_idle_s());
        return;
    }
    if (ui_kb_up() || ui_overlay_up() || ui_alarm_up() || ui_home_mode_active()) {
        if (force) printf("mem: asked to shed, but something is up (kb %d overlay %d alarm %d home %d)\n",
                          ui_kb_up(), ui_overlay_up(), ui_alarm_up(), ui_home_mode_active());
        return;
    }
    s_mem_force = false; /* past the gates: the pass is happening, so the request is spent */

    size_t glass = bz_frost_trim(MEM_TARGET - m[1]); /* only surfaces nothing is showing go */
    hal_mem_psram(m);
    int shed = 0;
    size_t freed = 0;
    /* Forced, it sheds exactly one window: enough to prove the path and to see that app build itself
     * again, without emptying the list of everything that may go. */
    while (force ? shed < 1 : m[1] < MEM_TARGET) {
        int pick = -1;
        for (int i = 0; i < U.nbuilt; i++) {
            if (!U.built[i].app || !U.built[i].app->shed) continue;
            if (U.built[i].app == U.app) continue;
            if (pick < 0 || U.built[i].used < U.built[pick].used) pick = i;
        }
        if (pick < 0) break;
        freed += U.built[pick].bytes;
        printf("mem: shedding %s (%u KB, last opened %.0f s ago)\n", U.built[pick].app->name,
               (unsigned)(U.built[pick].bytes / 1024), ui_now() - U.built[pick].used);
        app_evict(U.built[pick].app);
        shed++;
        hal_mem_psram(m);
    }
    if (!glass && !shed) {
        /* Nothing left that may be given back: below the floor with every surface in use and every
         * window either open or one nobody has cleared for shedding. Saying so once is useful; saying
         * it every quiet frame -- which is what the first version did, thirty times a second -- is not,
         * and the printing itself was costing more than the scan. So it waits a minute before looking
         * again, and the UI simply runs short until something is closed. */
        static double said;
        if (ui_now() - said > 60) {
            said = ui_now();
            printf("mem: %u KB free and nothing to give back (%u KB of glass all in use, %d windows, "
                   "none shed-able)\n", (unsigned)(m[1] / 1024), (unsigned)(bz_frost_bytes() / 1024), U.nbuilt);
        }
        s_mem_quiet_until = ui_now() + 60;
        return;
    }
    printf("mem: %u KB free after giving back %u KB of glass and %d app window%s (%u KB)\n",
           (unsigned)(m[1] / 1024), (unsigned)(glass / 1024), shed, shed == 1 ? "" : "s",
           (unsigned)(freed / 1024));
}

/* The apps that are slow to build, built ahead while the tablet sits idle, one per idle spell, so that their
 * first open is not a freeze: settings measured 396 ms to build, systemcore 187 (the rest are under 50). Built
 * into the window while it is hidden, so nothing is drawn; ui_app_open finds them already there. */
static void warm_apps(double now)
{
    static const ui_app_t *const WARM[] = { &APP_SETTINGS, &APP_SYSTEMCORE };
    static size_t next;
    static double idle_since;
    static bool glass_done;
    if (next >= sizeof WARM / sizeof WARM[0] && glass_done) return;
    bool quiet = !U.app && U.k.value <= 0.01f && lv_obj_has_flag(U.win, LV_OBJ_FLAG_HIDDEN) && !MC.pages && !MC.window &&
                 !bz_ui_thawing() && !ui_home_mode_active() && !ui_overlay_up() && !ui_alarm_up() && bz_ui_idle_s() > 3;
    if (!quiet || now < 20) {
        idle_since = now;
        return;
    }
    /* building ahead is a luxury: with PSRAM this short it would be shed again before anyone opened it,
     * and the build itself needs the room it is about to take */
    if (hal_psram_free() < MEM_TARGET) return;
    if (now - idle_since < 1) return; /* quiet for a moment first: not the frame a finger lands */
    if (next >= sizeof WARM / sizeof WARM[0]) {
        /* then the glass surfaces not baked yet (the control center's panels), one per quiet moment */
        glass_done = !bz_ui_glass_prewarm_one();
        idle_since = now - 0.6; /* the next in ~0.4 s, if it stays quiet */
        return;
    }
    double t0 = hal_seconds();
    app_inner(WARM[next]);
    printf("app %s: built ahead in %.0f ms\n", WARM[next]->name, (hal_seconds() - t0) * 1e3);
    next++;
    idle_since = now;
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

/* The chrome — the dock, the assistant's orb, the island — out of an offscreen render, and back after.
 *
 * Every offscreen render of a page has to do this, because on the tablet the glass IS the content
 * display's top layer (bz_ui_glass), and LVGL draws the top layer into whatever it renders. A page
 * picture taken without hiding it has a dock and an orb baked into it, and then the slide puts that
 * picture up as a layer and moves it: the baked pair slide away with the page while the real pair stay
 * where they belong, which is two docks and two AI buttons on screen for the length of the swipe.
 *
 * Piece by piece, not the top layer itself: the top layer has no parent, and LVGL's flag setters
 * dereference the parent to mark its layout dirty. The bitmask puts back exactly what was visible, so a
 * piece that was already hidden — the orb inside an app — stays hidden. */
static uint32_t chrome_hide(void)
{
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top), shown = 0;
    for (uint32_t k = 0; k < nc && k < 32; k++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)k);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        shown |= 1u << k;
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    return shown;
}

/* Hide it again, keeping the record of what was visible that chrome_hide already took.
 *
 * A picture is rendered a band a frame, and the run stays open across those frames -- but the frame
 * hooks keep running, and they own the chrome's visibility: orb_frame puts the orb back the moment it
 * finds it hidden, and the dock is managed the same way. So only the first band came out clean and
 * every later one had the chrome drawn into it, which is why it was the dock and the orb -- they live
 * at the bottom of the page, in the last band of all. Re-applied before each band. */
static void chrome_rehide(void)
{
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t k = 0; k < nc && k < 32; k++)
        lv_obj_add_flag(lv_obj_get_child(top, (int32_t)k), LV_OBJ_FLAG_HIDDEN);
}

static void chrome_restore(uint32_t shown)
{
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t k = 0; k < nc && k < 32; k++)
        if (shown & (1u << k)) lv_obj_remove_flag(lv_obj_get_child(top, (int32_t)k), LV_OBJ_FLAG_HIDDEN);
}

static void prep_page(bool before, void *u)
{
    static int x;
    static bool win_hidden;
    static uint32_t shown;
    if (before) {
        x = lv_obj_get_x(U.track);
        win_hidden = lv_obj_has_flag(U.win, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(U.track, -(int)(intptr_t)u * W);
        lv_obj_add_flag(U.win, LV_OBJ_FLAG_HIDDEN);
        shown = chrome_hide();
    } else {
        chrome_restore(shown);
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
    if (before) {
        x = lv_obj_get_x(U.track);
        lv_obj_remove_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.pages[U.page], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(U.track, -i * W);
        shown = chrome_hide();
    } else {
        chrome_restore(shown);
        lv_obj_set_x(U.track, x);
        lv_obj_add_flag(U.pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.pages[U.page], LV_OBJ_FLAG_HIDDEN);
    }
}

/* the page the open band run is prepared for, -1 if none is open */
static int s_band_page = -1;

/* the run closed: the tree goes back to the page on screen (see bz_ui_offscreen_begin) */
static void slide_bands_end(void)
{
    if (s_band_page < 0) return;
    s_band_page = -1;
    bz_ui_offscreen_end();
}

static void slide_band(int page, int band)
{
    uint16_t *nb = bz_ui_slide_nb_buf();
    if (!nb || page < 0 || page >= NPAGES) return;
    double t0 = bz_ui_clock();
    if (band == 0) {
        slide_bands_end(); /* a run for the other side, abandoned when the finger turned back */
        page_refresh_now(page); /* its numbers as of now, not as of when it was last shown */
    }
    if (s_band_page != page) {
        s_band_page = page;
        bz_ui_offscreen_begin(slide_prep, (void *)(intptr_t)page);
    }
    double t1 = bz_ui_clock();
    lv_area_t a = { 0, band * H / SL_BANDS, W - 1, (band + 1) * H / SL_BANDS - 1 };
    chrome_rehide(); /* the frame hooks put it back between bands: see chrome_rehide */
    bz_ui_offscreen_band(nb + (size_t)a.y1 * W, W, &a);
    double t2 = bz_ui_clock();
    bz_ui_slide_nb_patch(&a); /* into the panel's orientation, a band at a time (the Tab5's slide) */
    double t3 = bz_ui_clock();
    s_band[0] += t1 - t0;
    s_band[1] += t2 - t1;
    s_band[2] += t3 - t2;
    if (t3 - t0 > s_band[3]) s_band[3] = t3 - t0;
    s_bandn++;
}

/* Each frame of a slide: the next band of the neighbour, or the settle spring. */
static void slide_frame(void)
{
    if (!SL.active) return;
    bz_ui_keep_alive();
    if (!SL.settling) {
        if (SL.side && !SL.ready) {
            slide_band(SL.nb_page, SL.band);
            if (++SL.band == SL_BANDS) {
                SL.ready = true;
                slide_bands_end();
                bz_ui_slide_nb(SL.side);
                /* nobody's finger is holding this one: the page spring takes it from here */
                if (SL.jump) {
                    SL.settling = true;
                    bz_motion_to(&SL.x, (float)(-SL.side * W), BZ_PAGE);
                }
            }
        }
        return;
    }
    bool moving = bz_motion_tick(&SL.x);
    bz_ui_slide((int)lroundf(SL.x.value));
    if (moving) return;
    SL.active = SL.settling = SL.jump = false;
    slide_bands_end();
    if (SL.aim != U.page) {
        ui_go(SL.aim);
        s_track_quiet = true; /* the glass already shows the page: place_track moves it in quietly */
    }
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
    chrome_rehide(); /* between bands the frame hooks put it back: see chrome_rehide */
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
    /* The copy came from the content buffer, and the chrome is drawn into that buffer -- on the tablet
     * the glass is this display's top layer. Left in the picture it slides away with the page while the
     * real dock and orb stay where they belong, which is two docks and two AI buttons for the length of
     * a swipe. So the rectangles they cover are drawn again with the chrome hidden: roughly 70k pixels,
     * against the 920k that re-rendering the whole page instead of copying it would cost.
     *
     * Gathered before the render begins, because the prepare hook is what hides them. */
    lv_area_t rect[8];
    int nr = 0;
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t k = 0; k < nc && nr < 8; k++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)k);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        /* Room for whatever a piece draws outside itself -- the glass pieces carry a shadow. The exact
         * figure is lv_obj_get_ext_draw_size, which lives in a private LVGL header this file does not
         * pull in; a fixed margin is plenty here, since these rectangles only have to be large enough
         * and they are clipped to the screen below. */
        lv_area_increase(&a, 24, 24);
        if (a.x1 < 0) a.x1 = 0;
        if (a.y1 < 0) a.y1 = 0;
        if (a.x2 > W - 1) a.x2 = W - 1;
        if (a.y2 > H - 1) a.y2 = H - 1;
        if (a.x1 <= a.x2 && a.y1 <= a.y2) rect[nr++] = a;
    }
    if (nr) {
        bz_ui_offscreen_begin(prep_page, (void *)(intptr_t)i);
        for (int r = 0; r < nr; r++)
            bz_ui_offscreen_band(strip_page(i) + (size_t)rect[r].y1 * MC.stride + rect[r].x1, MC.stride, &rect[r]);
        bz_ui_offscreen_end();
    }
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
    /* A tap that opens an app freezes the interface for as long as these take, so a slow one says which part
     * was slow (the first open builds the app; after that, open and refresh are the whole cost). */
    double t0 = hal_seconds();
    U.win_inner = app_inner(app);
    double t1 = hal_seconds();
    lv_obj_remove_flag(U.win_inner, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < U.nbuilt; i++) if (U.built[i].inner != U.win_inner) lv_obj_add_flag(U.built[i].inner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.win);
    double t2 = t1, t3 = t1;
    if (!reopening || U.k.target == 0) {
        if (app->open) app->open();
        t2 = hal_seconds();
        if (app->refresh) app->refresh();
        t3 = hal_seconds();
    }
    if (t3 - t0 > 0.025)
        printf("app %s: build %.0f open %.0f refresh %.0f ms\n", app->name, (t1 - t0) * 1e3, (t2 - t1) * 1e3,
               (t3 - t2) * 1e3);
    if (U.k.value < 0.02f) window_begin(true);
    /* grows out of the icon on the release spring; a closing window caught here reopens from where it is */
    bz_motion_to(&U.k, 1, BZ_RELEASE);
    ui_text(U.pill_label, "%s", app->name);
    lv_label_set_text(U.pill_icon, BZ_I_ARROW_BACK);
    bz_glass_show(U.pill_glass, true);
    lv_obj_remove_flag(U.pill, LV_OBJ_FLAG_HIDDEN);
    /* an app is full screen: its head takes the band's right (short of the island's orb, which stays) and
     * its body the assistant orb's corner */
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
        if (!ui_home_mode_active()) {
            lv_obj_remove_flag(U.status, LV_OBJ_FLAG_HIDDEN);
            ui_orb_show(true);
        }
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
    hal_kv_set("accent", BZ_ACCENTS[S.accent >= 0 && S.accent < BZ_NACCENTS ? S.accent : 0].name);
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
#ifdef ESP_PLATFORM
    /* The simulator's address is the tablet itself, where no robot ever answers. It was found stored on the
     * owner's tablet (a random-tap test pressing the "simulator" chip, most likely), which left it looking
     * for the robot at 127.0.0.1 and nowhere else: back to finding it by team. */
    if (!strcmp(S.address, "127.0.0.1") || !strcmp(S.address, "localhost")) {
        S.address[0] = 0;
        hal_kv_set("address", "");
    }
#endif
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
    S.accent = 0;
    if (hal_kv_get("accent", buf, sizeof buf))
        for (int i = 0; i < BZ_NACCENTS; i++)
            if (!strcmp(buf, BZ_ACCENTS[i].name)) S.accent = i;
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

void ui_set_accent(int accent)
{
    if (accent < 0 || accent >= BZ_NACCENTS || accent == S.accent) return;
    S.accent = accent;
    bz_ui_set_accent(accent); /* shared styles rewritten: every screen, built or not, takes it */
    ui_settings_save();
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

/* the dev console's "perf": each section's time, summed until it asks */
#define PROF_MARK(i) do { double t_ = bz_ui_clock(); s_prof[i] += t_ - p_; p_ = t_; } while (0)

static void shell_frame(double now, double dt, void *user)
{
    (void)user;
    g_now = now;
    /* The first frames of the interface repaint everything, content and composite together.
     *
     * The boot card owns the screen until it hands over, and what it painted stays in the buffers
     * until something paints over it. main() damages the compositor at the handover, but that only
     * says "compose again" -- it does not make LVGL re-render, and it happens before the interface
     * has drawn anything, so the compositor faithfully composes the card's content one more time and
     * the re-render that follows has nothing marked dirty left to show. Wherever the interface does
     * not happen to paint, the card's pixels survive: in landscape the status bar appears at the
     * bottom with a strip of displaced cards down the side, in phone mode a second clock behind the
     * real one. It is every start, not only a change of frame shape -- the shape change only made it
     * impossible to miss.
     *
     * Both halves are needed and in this order: invalidate LVGL's content, then tell the compositor
     * the result is new. Repeated for a few frames because the first one after the interface is
     * released can still land while the card's last present is in flight. Three full repaints at
     * start-up cost nothing anyone can see, and the alternative is a screen that looks broken until
     * something happens to touch every pixel.
     *
     * Here rather than in main() because this runs on the task that owns LVGL. */
    static int settle = 3;
    if (settle > 0) {
        settle--;
        lv_obj_invalidate(lv_screen_active());
        bz_comp_damage_all(bz_ui_comp());
    }
    double p_ = bz_ui_clock();
    dev_run();
#if BZ_LEAN
    go_step(); /* a page asked for by the dock: started here, never inside an LVGL event */
    slide_frame();
    /* instant motion: the offset is already where it's going; move the track there once */
    if (MC.track_page != U.page) {
        MC.track_page = U.page;
        place_track();
    }
#endif
    PROF_MARK(0);
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
    mem_pressure(); /* before warm_apps: do not build another window into a heap that is already short */
    warm_apps(now);
    PROF_MARK(1);
    dock_frame();
    island_frame(now);
    PROF_MARK(2);
    orient_frame(now);
    PROF_MARK(3);
    windows_frame(now, dt);
    PROF_MARK(4);
    perf_frame(now);
    PROF_MARK(5);

    /* Nothing is refreshed while something is moving across the screen.
     *
     * This block is the interface's bookkeeping -- the robot model, the island, every page's numbers --
     * and it runs ten times a second whatever else is happening, including through the whole of an app
     * opening or a page sliding. It is the single most expensive thing in a frame during an animation,
     * and none of it is worth a dropped frame: the most it can be out of date by is the length of the
     * animation, under a second, over numbers that are themselves seconds old. Deferred, not skipped --
     * last_refresh is left alone, so the moment the motion stops the next frame does the work. */
    bool moving = bz_ui_sheeting() || SL.active || (U.app && fabsf(U.k.value - U.k.target) > 0.01f);
    if (!moving && now - U.last_refresh >= 0.1) {
        U.last_refresh = now;
        cat_model_update(R);
        PROF_MARK(6);
        assist_feed(R);
        PROF_MARK(7);
        island_refresh();
        PROF_MARK(8);
        /* A page's refresh only while it's on screen and nothing covers it.
         *
         * Covered means the window is still in front of it, not that the window is on its way in.
         * U.k.target drops to 0 the instant a close is asked for, while the window itself takes the
         * whole of its spring to shrink away -- so every page refresher ran, at 10 Hz, against a page
         * nobody could see yet, for the length of every close. Measured through four opens and closes
         * that stage cost 967 ms of every 2 s, about 48 ms a refresh, against 10 ms of every 2 s while
         * scrolling. It is most of why an app open runs at 22 fps. */
        bool covered = (U.app && (U.k.target > 0 || U.k.value > 0.02f)) || ui_home_mode_active();
        for (int i = 0; i < U.nrefresh; i++)
            if (U.refresh[i].page < 0 || (U.refresh[i].page == U.page && !covered)) U.refresh[i].fn(U.refresh[i].user);
        PROF_MARK(9);
        if (U.app && U.app->refresh && U.k.target > 0) U.app->refresh();
        PROF_MARK(10);
    }
    /* the interface fades in with the backlight after the boot card faded out with it */
    static double shown_at;
    static bool faded_in, resumed;
    if (!shown_at) shown_at = now;
    if (!resumed && now - shown_at > 1.0) {
        resumed = true;
        resume_where(); /* back where the Wi-Fi watchdog's restart found it, if it did */
    }
    if (!faded_in) {
        float k = (float)((now - shown_at) / 0.35);
        if (k >= 1) { k = 1; faded_in = true; }
        hal_set_brightness(S.brightness * k * k * (3 - 2 * k));
        bz_ui_keep_alive();
    }
    /* untouched, the panel dims (settings: display), then goes off; a tap wakes it, and that tap presses
     * nothing. Asleep, the link to the robot and everything behind the glass keep running. */
    double idle_s = bz_ui_idle_s();
    /* home mode: the room's own ambient level counts as a touch too, so the dim/sleep timer below brightens
     * on noise and dims again on the same timer as everywhere else once it's been quiet a while, rather than
     * a second timer of its own (ui_home_mode_presence_idle_s is a large number until there's a signal, so
     * this can only ever pull idle_s down, never make it dim or sleep sooner than a touch would) */
    if (ui_home_mode_active()) {
        double p = ui_home_mode_presence_idle_s(now);
        if (p < idle_s) idle_s = p;
    }
    float full = S.brightness;
    /* the companion on a stand, on power, stays awake (it only dims) */
    if (!SLP.asleep && (SLP.request || (!ui_companion_keeps_awake() && !ui_home_mode_keeps_awake() && S.sleep_s > 0 && idle_s > S.sleep_s))) {
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
        hal_set_brightness(full);
        hal_tone(1200, 8, S.volume * 0.3f);
    }
    bool idle = !SLP.asleep && S.dim_s > 0 && idle_s > S.dim_s;
    if (!SLP.asleep && idle != SLP.dimmed) {
        SLP.dimmed = idle;
        hal_set_brightness(idle ? S.brightness * 0.25f : full);
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
    PROF_MARK(11);
}

void ui_init(const ui_config_t *cfg)
{
    U.cfg = *cfg;
    R = calloc(1, sizeof *R);
    assist_import_card(); /* keys and settings dropped on the card: into kv before anything reads kv */
    settings_load();
    if (cfg->team > 0 && !hal_kv_get("team", (char[8]){ 0 }, 8)) S.team = cfg->team;
    cat_model_init(cfg->nt);
    ui_sc_boot();
    ui_os_boot(); /* alarms ring whether or not the clock app has been opened */
    ui_apply_addresses();
    bz_ui_set_accent(S.accent); /* before the tone: the first styles are built with it */
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
    ui_look_boot(); /* the owner's ground under the pages, the tone's schedule (settings > look) */

    build_windows();
    build_dock();
    build_island();
    ui_cc_init();
    ui_lock_init();
    ui_match_boot(); /* after the lock screen: the alarm screen goes over it */
    ui_batt_boot();  /* the battery fleet off the card, its log reading and the robot's live numbers */
    ui_orb_init();
    ui_home_mode_init(home_changed);
    hal_restart_hook(note_where);
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
