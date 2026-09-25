/* bz_ui — Bezel's layers on LVGL: content, glass ink and the compositor between them.
 *
 * Two LVGL displays share one screen-sized coordinate space. `bz_ui_content()` is the scene under the
 * glass (tiles, text, icons: everything the glass may frost and bend); `bz_ui_glass()` holds only what
 * sits *on* glass — a glass control's own label and icon — and is transparent everywhere else. An
 * object on the glass layer becomes a glass shape with `bz_glass_attach()`; the compositor draws the
 * frosted, refracting, lit slab under it from the content layer, then the ink on top. This is the
 * specimen's `[data-glass]` rule: glass draws its own labels, and never sits over drawn-on-top text.
 *
 * One physical touch is routed to whichever layer it lands on first: a press that starts on a
 * clickable glass object belongs to the glass layer until release, anything else to the content. */
#pragma once

/* Lean: the tablet's renderer. One RGB565 display and no compositor — the glass parent is that display's
 * top layer, its shapes drawn as solid rounded surfaces; LVGL's dirty areas go straight to the panel;
 * springs are instant (bz_motion_set_instant) except list scrolling. The simulator keeps full glass. */
#ifndef BZ_LEAN
#ifdef ESP_PLATFORM
#define BZ_LEAN 1
#else
#define BZ_LEAN 0
#endif
#endif
#include "bz_comp.h"
#include "bz_motion.h"
#include "lvgl.h"

typedef struct {
    int w, h;
    uint16_t *content;      /* w*h RGB565, LVGL renders the scene here (direct mode) */
    uint32_t *ink;          /* w*h ARGB8888, LVGL renders the glass layer here */
    uint16_t *out;          /* w*h RGB565 composite */
    /* The HAL shows these areas, each from its own source (rotating into the panel on the Tab5). */
    void (*present)(const bz_present_t *areas, int n, void *user);
    /* The HAL's touch: first finger, screen coordinates. */
    bool (*read_touch)(int *x, int *y, void *user);
    void *user;
    const bz_gfx_ops_t *ops; /* accelerated copy/blend, or NULL for the CPU versions */
    bool async_present;      /* present returns before the panel has the frame: composite alternately
                                into a second buffer (allocated here) so the last one stays readable */
    const bz_slide_ops_t *slide; /* optional: page slides in the panel's own orientation */
} bz_ui_config_t;

void bz_ui_init(const bz_ui_config_t *cfg);
lv_obj_t *bz_ui_content(void);
lv_obj_t *bz_ui_glass(void);
bz_comp_t *bz_ui_comp(void);
/* Advances the motion clock, runs LVGL, updates glass shapes, composes and presents. Returns true while
 * anything is still animating (the caller may idle longer when it is false). */
bool bz_ui_frame(double now_s);
/* Seconds since the last touch: the chrome idles away, standby arrives. */
double bz_ui_idle_s(void);
/* The next press (or the one under way) still counts as a touch but never reaches the interface: the tap
 * that wakes a dark screen mustn't also press what was under it. */
void bz_ui_swallow_touch(void);
/* The finger now and where its press began (false: no press). For system gestures read every frame. */
bool bz_ui_press(int *x0, int *y0, int *x, int *y);
/* A system gesture takes the press under way: the interface sees it end without a click. */
void bz_ui_take_press(void);
void bz_ui_wake(void);
/* The next press is delivered after all: a screen woken by something other than a touch (an alarm) wants its
 * first tap to press what it shows. */
void bz_ui_swallow_cancel(void);

/* Tone and calm. Applying a tone rebuilds styles (see bz_theme) and redraws everything. */
void bz_ui_set_mode(bool dark, bool calm);
bool bz_ui_dark(void);
bool bz_ui_calm(void);
/* The glass's one light, leaned by tilt: dx, dy in -1..1. Rides the `light` spring. */
void bz_ui_lean_light(float dx, float dy);

/* ---- motion caches: move a picture instead of redrawing it ----
 *
 * While something big moves (pages swiping, an app window growing), the content layer can be frozen and
 * the compositor shown cached pictures of it instead (bz_comp_set_layer). */

/* Renders the content layer into `buf` (RGB565, `stride` px per row; buf is the pixel at area's x1, y1)
 * as it looks between prepare(true, u) and prepare(false, u), without touching the screen. Objects
 * moved or hidden in prepare don't cause redraws. */
void bz_ui_render_offscreen(uint16_t *buf, int stride, const lv_area_t *area, void (*prepare)(bool before, void *u),
                            void *u);
/* Frozen: LVGL stops drawing the content layer; the compositor shows whatever layers say. Thawing
 * redraws the whole content layer, a band per frame over the next few frames: keep the layers that hid
 * it up until bz_ui_thawing() is false. Counted: each freeze(true) needs its freeze(false). */
void bz_ui_freeze(bool frozen);
bool bz_ui_frozen(void);
bool bz_ui_thawing(void);
/* Stops a thaw where it is, leaving the content partly stale: only for a caller about to freeze again
 * whose layers cover the whole screen until the next thaw (which redraws everything). */
void bz_ui_thaw_cancel(void);
/* The platform's fast copy (PPA on the Tab5). */
void bz_ui_copy(uint16_t *dst, int dst_stride, const uint16_t *src, int src_stride, int w, int h);
uint16_t *bz_ui_content_buf(void);
/* Scrolls `content` inside `clip` to y (lv_obj_set_y) by moving the pixels already drawn and having LVGL
 * draw only the strip that comes into view: a list that scrolls a few pixels a frame costs those rows,
 * not the whole list. Falls back to an ordinary move (LVGL redraws both positions) whenever moving the
 * pixels couldn't be right: frozen, partly off screen or clipped, something drawn over the list, a
 * rounded clip, or a jump taller than the list. Returns true if the pixels were moved. */
bool bz_ui_scroll(lv_obj_t *clip, lv_obj_t *content, int32_t y);

/* ---- performance ---- */
typedef struct {
    float fps;                 /* frames presented per second, smoothed */
    float frame_ms, lvgl_ms, compose_ms, present_ms; /* this machine's, smoothed */
    float model_ms;            /* the same frame's work costed for the ESP32-P4 (see bz_ui.c) */
    uint32_t lvgl_px;          /* content + glass pixels LVGL drew last frame */
    uint32_t shift_px;         /* pixels a scrolling list moved instead of redrawing */
    uint32_t frames;           /* frames presented since boot */
    bz_comp_stats_t comp;      /* last frame */
} bz_ui_perf_t;
void bz_ui_perf(bz_ui_perf_t *out);
/* LVGL's time since the last call, ms: content layout/render/flush, glass layout/render/flush, handler */
void bz_ui_prof_take(float out[8]);
/* Lean page slide: a snapshot of the screen taken at begin is shown shifted by dx (the gap filled with
 * ground) until end, straight to the panel with no LVGL drawing; end redraws the whole screen. */
void bz_ui_slide_begin(void);
void bz_ui_slide(int dx);
void bz_ui_slide_end(void);
/* fn run with LVGL's invalidation off: for a change the glass already shows (a page a slide came to rest
 * on), so it isn't drawn again */
void bz_ui_quiet(void (*fn)(void *u), void *u);
/* The page beside it during a slide: a screen-sized buffer to draw it into (bz_ui_render_offscreen), and
 * which side it shows on once drawn — +1 to the right (the next page), -1 left, 0 none: ground. */
uint16_t *bz_ui_slide_nb_buf(void);
void bz_ui_slide_nb(int side);
/* A band of the neighbour has been drawn into the buffer: the platform's slide takes it from there. */
void bz_ui_slide_nb_patch(const lv_area_t *a);
/* A full-screen sheet over the page, pulled down from the top (the control center), composed by the
 * platform in its panel's orientation with no drawing per frame. Opening: the page is what's on the glass
 * and the sheet is drawn at rest offscreen, a band at a time as it's revealed, with `prep(true)` making
 * it appear for the drawing and `prep(false)` putting things back. Closing: the other way round, the
 * sheet on the glass and the page drawn. h: how much of the sheet shows, 0 .. screen height. false: no
 * such platform support (animate some other way). End puts LVGL back in charge without a redraw, so show
 * the resting height (0 or `height`) first. The sheet is `height` rows from the top; below, the page. With
 * `bottom` it is the whole screen coming up from the bottom edge (an app opening). */
bool bz_ui_sheet_begin(bool opening, int height, bool bottom, void (*prep)(bool before, void *u), void *u);
void bz_ui_sheet(int h);
int bz_ui_sheet_shown(void); /* the height on the panel now (-1: none yet) */
bool bz_ui_sheeting(void);
void bz_ui_sheet_end(void);
double bz_ui_clock(void);    /* monotonic seconds, for timing */
void bz_ui_hooks_report(void);
void bz_ui_trace_inv(int frames); /* log every invalidated area for this many frames */ /* logs each frame hook's time since the last call */
/* Per-frame averages since the last call: frame hooks, lv_timer_handler, display refreshes, renders. */
void bz_ui_split(float *hooks_ms, float *lvgl_ms, float *refr_ms, float *render_ms);
/* the slowest frame's hooks and LVGL since the last bz_ui_split (which resets both) */
void bz_ui_split_max(float *hooks_ms, float *lvgl_ms);

/* Frame hooks, run every frame before LVGL renders. */
typedef void (*bz_frame_fn)(double now_s, double dt, void *user);
void bz_ui_on_frame(bz_frame_fn fn, void *user);
/* Keeps frames coming (a motion is running somewhere a hook owns). */
void bz_ui_keep_alive(void);

/* ---- glass ---- */

typedef struct bz_glass bz_glass_t;

/* Makes `obj` (on the glass layer) a glass shape of `group` with corner `radius`. Glass appears by
 * strength on the `layer` spring, presses answer on `hold` with a glow at the touch and a small scale,
 * and let go on `release`. */
bz_glass_t *bz_glass_attach(lv_obj_t *obj, uint8_t group, float radius);
void bz_glass_show(bz_glass_t *g, bool show);            /* strength 0↔1, never opacity */
void bz_glass_set_strength(bz_glass_t *g, float s);      /* driven directly, e.g. by a pull */
void bz_glass_set_tint(bz_glass_t *g, int tint, float amt); /* amt rides `effect` */
void bz_glass_set_press_scale(bz_glass_t *g, float s);    /* 1.06 default, the dock 1.02 */
void bz_glass_set_radius(bz_glass_t *g, float r);
/* A glass that never melts with its group's others, however near (cards meant to stay apart). */
void bz_glass_set_solo(bz_glass_t *g, bool solo);
/* Rect override for shapes whose glass isn't the object's own box (the dock droplet). */
void bz_glass_set_rect(bz_glass_t *g, bool use, float x, float y, float w, float h);
float bz_glass_strength(const bz_glass_t *g);
lv_obj_t *bz_glass_obj(const bz_glass_t *g);

/* ---- gestures ---- */

/* A drag handler, Detent-style: begin on the first move past the slop, move with the 1:1 offset and
 * live velocity, end with the release velocity (px/s) for projection and hand-off. */
typedef struct {
    void (*begin)(lv_obj_t *obj, lv_point_t start, void *user);
    void (*move)(lv_obj_t *obj, int dx, int dy, float vx, float vy, void *user);
    void (*end)(lv_obj_t *obj, int dx, int dy, float vx, float vy, void *user);
    void *user;
    uint8_t axis;     /* 0 free, 1 horizontal, 2 vertical: locks after the slop */
    uint8_t slop;     /* 10 px pans, 3 px sliders */
} bz_drag_t;

/* Installs `d` on `obj` (copied). Children that should let the drag through carry
 * LV_OBJ_FLAG_EVENT_BUBBLE; the innermost handler whose axis matches claims the press. */
void bz_drag_attach(lv_obj_t *obj, const bz_drag_t *d);
/* A click that isn't the release of a drag. */
typedef void (*bz_tap_fn)(lv_obj_t *obj, void *user);
void bz_on_tap(lv_obj_t *obj, bz_tap_fn cb, void *user);
void bz_ui_on_any_tap(void (*fn)(void)); /* called before every tap's own callback: a click sound */
/* True while a drag owns the pointer (a click handler may check it). */
bool bz_drag_active(void);
bool bz_drag_recent(void);
