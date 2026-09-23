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
#include "bz_comp.h"
#include "bz_motion.h"
#include "lvgl.h"

typedef struct {
    int w, h;
    uint16_t *content;      /* w*h RGB565, LVGL renders the scene here (direct mode) */
    uint32_t *ink;          /* w*h ARGB8888, LVGL renders the glass layer here */
    uint16_t *out;          /* w*h RGB565 composite; the HAL presents it */
    /* The HAL shows `out` for these areas (rotating into the panel on the Tab5). */
    void (*present)(const bz_area_t *areas, int n, void *user);
    /* The HAL's touch: first finger, screen coordinates. */
    bool (*read_touch)(int *x, int *y, void *user);
    void *user;
    const bz_gfx_ops_t *ops; /* accelerated copy/blend, or NULL for the CPU versions */
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
void bz_ui_wake(void);

/* Tone and calm. Applying a tone rebuilds styles (see bz_theme) and redraws everything. */
void bz_ui_set_mode(bool dark, bool calm);
bool bz_ui_dark(void);
bool bz_ui_calm(void);
/* The glass's one light, leaned by tilt: dx, dy in -1..1. Rides the `light` spring. */
void bz_ui_lean_light(float dx, float dy);

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
/* True while a drag owns the pointer (a click handler may check it). */
bool bz_drag_active(void);
bool bz_drag_recent(void);
