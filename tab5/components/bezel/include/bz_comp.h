/* bz_comp — Bezel's two-pass glass renderer, cut down for the ESP32-P4.
 *
 * The web specimen draws everything under the glass into a scene texture (pass A), builds a blur
 * pyramid, then composites refracting, frosted, lit glass shapes over it (pass B), with the glass's own
 * labels drawn on top as DOM. This keeps that shape on a CPU plus the P4's pixel accelerator:
 *
 *   content  (RGB565)   pass A: LVGL display #1 renders tiles, text, icons — everything under glass
 *   ink      (ARGB8888) LVGL display #2 renders only what sits ON glass: dock labels, island text...
 *   out      (RGB565)   the composite, which the HAL rotates into the panel's scan-out buffer
 *
 * Per glass group (shapes that melt together, as panel.js's GROUPS), a low-resolution blurred copy of the
 * content under it is kept (1/4 scale, two box passes of radius 3 → σ ≈ 11.3 px, Bezel's frost at panel
 * scale) and rebuilt only when the content under it changes. A per-group geometry table caches coverage,
 * the refraction offset from the bezel profile and Snell's law at n = 1.5, the rim light and the shadow,
 * rebuilt only while the shape is moving. The per-frame work inside a glass shape is then one bilinear
 * lookup, a few integer colour ops and the ink blend; everywhere else it is a copy the accelerator does.
 *
 * Calm (reduced transparency) is the design's own cheap path: solid surface2, rim and shadow, no frost. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BZ_COMP_MAX_SHAPES 16
#define BZ_COMP_MAX_GROUPS 8
#define BZ_COMP_MAX_DIRTY 48

typedef struct { int16_t x1, y1, x2, y2; } bz_area_t; /* inclusive, like lv_area_t */

typedef struct {
    float x, y, w, h;      /* rest rect in screen px */
    float radius;
    float strength;        /* 0..1: materializes frost, evening, tint, rim, shadow and thickness together */
    float press;           /* 0..1 press glow and thickness */
    float press_x, press_y;
    float scale;           /* press scale about the centre (1.0 at rest) */
    uint8_t group;         /* 0..7; only shapes in one group melt together */
    int8_t tint;           /* -1 none; 0 signal, 1 amber, 2 leaf, 3 ice, 4 fault */
    float tint_amt;
} bz_glass_shape_t;

/* Accelerated primitives. The CPU versions are the default; the ESP32-P4 HAL points these at PPA. */
typedef struct {
    void (*copy565)(uint16_t *dst, int dst_stride, const uint16_t *src, int src_stride, int w, int h);
    /* dst565 = src565 under an ARGB8888 layer (straight alpha) */
    void (*blend)(uint16_t *dst, int dst_stride, const uint16_t *under, int under_stride,
                  const uint32_t *over, int over_stride, int w, int h);
} bz_gfx_ops_t;

typedef struct bz_comp bz_comp_t;

bz_comp_t *bz_comp_create(int w, int h, uint16_t *content, uint32_t *ink, uint16_t *out);
void bz_comp_set_ops(bz_comp_t *c, const bz_gfx_ops_t *ops);

/* The glass on screen this frame. Shapes that moved, reshaped or changed strength damage their old
 * and new footprint; unchanged ones cost nothing. */
void bz_comp_set_shapes(bz_comp_t *c, const bz_glass_shape_t *shapes, int n);
/* Light direction in screen space (y down), normalized by the compositor. Bezel rests at (-0.42, -0.91);
 * the Tab5 leans it with the IMU, which is what Bezel's "light: tilt" does with device orientation. */
void bz_comp_set_light(bz_comp_t *c, float lx, float ly);
void bz_comp_set_mode(bz_comp_t *c, bool dark, bool calm);
/* Page-wide frost behind the control center: amount 0..1 (σ grows to ~26 px), dim 0..1. */
void bz_comp_set_backdrop(bz_comp_t *c, float amount, float dim);

void bz_comp_damage_content(bz_comp_t *c, const bz_area_t *a);
void bz_comp_damage_ink(bz_comp_t *c, const bz_area_t *a);
void bz_comp_damage_all(bz_comp_t *c);

/* Composites every damaged area into `out`. Returns how many areas were written (copied to `areas`,
 * for the HAL to present), 0 if nothing changed. */
int bz_comp_compose(bz_comp_t *c, bz_area_t *areas, int max);

/* Frame statistics for the diagnostics overlay. */
typedef struct {
    uint32_t composed_px, glass_px, blur_rebuilds, lut_rebuilds;
    uint32_t compose_us;
} bz_comp_stats_t;
void bz_comp_stats(bz_comp_t *c, bz_comp_stats_t *out);
