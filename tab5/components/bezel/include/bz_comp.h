/* bz_comp — Bezel's two-pass glass renderer, cut down for the ESP32-P4.
 *
 * The web specimen draws everything under the glass into a scene texture (pass A), builds a blur
 * pyramid, then composites refracting, frosted, lit glass shapes over it (pass B), with the glass's own
 * labels drawn on top as DOM. This keeps that shape on a CPU plus the P4's pixel accelerator:
 *
 *   content  (RGB565)   pass A: LVGL display #1 renders tiles, text, icons — everything under glass
 *   layers   (RGB565)   cached pictures shown in place of the content while something big moves: the
 *                       pages side by side while they swipe, an app window while it grows out of its
 *                       icon, a list while it scrolls. Moving one costs a pointer, not a render.
 *   ink      (ARGB8888) LVGL display #2 renders only what sits ON glass: dock labels, island text...
 *   out      (RGB565)   the composite, where glass, backdrop or ink needs one
 *
 * The page is `content` overlaid by the layers ("base" below). Per glass group (shapes that melt
 * together, as panel.js's GROUPS), a low-resolution blurred copy of the base under it is kept (1/4 scale,
 * two box passes of radius 3 → σ ≈ 11.3 px, Bezel's frost at panel scale) and rebuilt only when the
 * base under it changes; while every shape in the group is fully materialized and untinted, the frost's
 * tone (evening, saturation) is applied to that small image instead of to every pixel. A per-group
 * geometry table caches coverage, the refraction offset from the bezel profile and Snell's law at
 * n = 1.5, the rim light and the shadow, rebuilt only while the shape changes and merely moved while it
 * only translates. Per frame inside a glass shape: one bilinear lookup, the rim, a blend.
 *
 * Nothing is copied that doesn't need to be. Each damaged area is split into cells: a cell with no
 * glass, backdrop, ink or antialiased layer corner in it is handed to the panel straight from wherever
 * its pixels live (the content buffer, or a layer's cache); only the rest is composited into `out`.
 * Work inside a frame is split across both cores (ops.parallel) by rows.
 *
 * Calm (reduced transparency) is the design's own cheap path: solid surface2, rim and shadow, no frost. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BZ_COMP_MAX_SHAPES 16
#define BZ_COMP_MAX_GROUPS 8
#define BZ_COMP_MAX_DIRTY 48
#define BZ_COMP_MAX_LAYERS 4
#define BZ_COMP_MAX_PRESENT 96

typedef struct { int16_t x1, y1, x2, y2; } bz_area_t; /* inclusive, like lv_area_t */

/* What the panel gets: an area, and where its pixels are. */
typedef struct {
    bz_area_t a;            /* screen rect, inclusive */
    const uint16_t *src;    /* RGB565 pixel (a.x1, a.y1) of this area's source */
    int stride;             /* the source's row pitch, in pixels */
} bz_present_t;

/* A page slide done by the platform in its panel's own orientation (the Tab5: rows of the portrait
 * buffer, no rotation per frame). begin captures what's on the glass; patch turns a landscape area into
 * the page's picture (neighbour false: the page without its chrome) or the neighbour's; frame shows the
 * page moved by dx with the neighbour (side -1 left, +1 right, 0 none) in the gap and the chrome fixed. */
typedef struct bz_slide_ops {
    bool (*begin)(uint32_t ground_rgb, const bz_area_t *chrome, int nchrome);
    void (*patch)(const bz_present_t *area, bool neighbour);
    void (*frame)(int dx, int side, const bz_area_t *chrome, int nchrome);
    void (*end)(void);
    /* A sheet `sh` rows tall over the page. From the top (bottom false): landscape rows [0, h) show the
     * sheet by its last h rows. From the bottom (a full-screen sheet, sh = the screen): rows [H - h, H)
     * show it by its first h rows. `swapped` (closing a sheet that was on the glass): the sheet is the
     * snapshot begin took, and the page's rows it uncovers are the neighbour. */
    void (*sheet)(int h, int sh, bool swapped, bool bottom);
    /* Both of the panel's buffers made what is on the glass now, so drawing can carry on in areas. */
    void (*settle)(void);
    /* A list scrolled: the landscape rect's pixels move by dy rows in the next present, copied in the
     * panel's orientation from the picture on the glass; only what scrolled into view is drawn. */
    void (*scroll)(const bz_area_t *a, int dy);
} bz_slide_ops_t;

typedef struct {
    float x, y, w, h;      /* rest rect in screen px */
    float radius;
    float strength;        /* 0..1: materializes frost, evening, tint, rim, shadow and thickness together */
    float press;           /* 0..1 press glow and thickness */
    float press_x, press_y;
    float scale;           /* press scale about the centre (1.0 at rest) */
    uint8_t group;         /* 0..7; only shapes in one group melt together */
    uint8_t id;            /* stable per glass object (< 64): its cached geometry follows it */
    bool solo;             /* never melts, even with its group near (the control center's modules) */
    int8_t tint;           /* -1 none; 0 signal, 1 amber, 2 leaf, 3 ice, 4 fault */
    float tint_amt;
} bz_glass_shape_t;

/* A cached picture over the content: screen pixel (x, y) inside `rect` is
 * px[(y - oy) * stride + (x - ox)]. Corners round by `radius`, antialiased. */
typedef struct {
    bz_area_t rect;
    float radius;
    const uint16_t *px;
    int ox, oy, stride;
} bz_layer_t;

/* Accelerated primitives. The CPU versions are the default; the ESP32-P4 HAL points these at PPA. */
typedef struct {
    void (*copy565)(uint16_t *dst, int dst_stride, const uint16_t *src, int src_stride, int w, int h);
    /* dst565 = src565 under an ARGB8888 layer (straight alpha) */
    void (*blend)(uint16_t *dst, int dst_stride, const uint16_t *under, int under_stride,
                  const uint32_t *over, int over_stride, int w, int h);
    /* dst565 = a565 · (1 − alpha/255) + b565 · alpha/255 */
    void (*mix565)(uint16_t *dst, int dst_stride, const uint16_t *a, int a_stride, const uint16_t *b, int b_stride,
                   int w, int h, uint8_t alpha);
    /* Runs job(arg, 0) and job(arg, 1) at once, one on each core, and returns when both have. NULL:
     * the compositor runs both parts itself. */
    void (*parallel)(void (*job)(void *arg, int part), void *arg);
} bz_gfx_ops_t;

typedef struct bz_comp bz_comp_t;

bz_comp_t *bz_comp_create(int w, int h, uint16_t *content, uint32_t *ink, uint16_t *out);
/* A second composite buffer: frames alternate between the two, so the panel can still be reading the
 * last frame's composite while this one is drawn (the Tab5 presents asynchronously). */
void bz_comp_set_out2(bz_comp_t *c, uint16_t *out2);
void bz_comp_set_ops(bz_comp_t *c, const bz_gfx_ops_t *ops);

/* The glass on screen this frame. Shapes that moved, reshaped or changed strength damage their old
 * and new footprint; unchanged ones cost nothing. */
void bz_comp_set_shapes(bz_comp_t *c, const bz_glass_shape_t *shapes, int n);
/* Light direction in screen space (y down), normalized by the compositor. Bezel rests at (-0.42, -0.91);
 * the Tab5 leans it with the IMU, which is what Bezel's "light: tilt" does with device orientation. */
void bz_comp_set_light(bz_comp_t *c, float lx, float ly);
void bz_comp_set_mode(bz_comp_t *c, bool dark, bool calm);
/* Page-wide frost behind the control center: amount 0..1, dim 0..1. The frosted, dimmed page is built
 * once (and again at most twice a second while the page underneath changes) and cross-faded in by
 * `amount`, which is what the spring moves every frame; `dim` sets how dark it is at full amount. */
void bz_comp_set_backdrop(bz_comp_t *c, float amount, float dim);

/* A layer in `slot` (0 lowest), or off with NULL. Changing it damages what it covered and covers. */
void bz_comp_set_layer(bz_comp_t *c, int slot, const bz_layer_t *l);

/* The base changed inside `a`: the content (LVGL flushed there) or a layer's pixels. */
void bz_comp_damage_content(bz_comp_t *c, const bz_area_t *a);
void bz_comp_damage_ink(bz_comp_t *c, const bz_area_t *a);
void bz_comp_damage_all(bz_comp_t *c);

/* Composites every damaged area. Fills `out` with what the panel needs this frame (at most `max`
 * entries; beyond that they're merged) and returns how many; 0 if nothing changed. */
int bz_comp_compose(bz_comp_t *c, bz_present_t *out, int max);

/* Frame statistics for the performance overlay and the simulator's cost model: what was done, by
 * which path, this frame. Times are this machine's. */
typedef struct {
    uint32_t composed_px;    /* written into `out` */
    uint32_t direct_px;      /* presented straight from content or a layer */
    uint32_t glass_px, glass_fast_px;
    uint32_t glass_flat_px, glass_edge_px, glass_shadow_px; /* by path: flat interior, edge, shadow only */
    uint32_t lut_px, lut_ring_px;  /* geometry rebuilt: all pixels visited, and those needing the full math */
    uint32_t blur_src_px;    /* base pixels averaged into blur images */
    uint32_t base_cpu_px;    /* base pixels assembled by the CPU (layer corners, mixed cells) */
    uint32_t mix_px, ink_px, backdrop_build_px;
    uint32_t blur_rebuilds, lut_rebuilds, lut_moves;
    uint32_t cells;
    uint32_t compose_us;
} bz_comp_stats_t;
void bz_comp_stats(bz_comp_t *c, bz_comp_stats_t *out);
