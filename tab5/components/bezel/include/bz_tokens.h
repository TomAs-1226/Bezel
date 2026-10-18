/* bz_tokens — Bezel's colour, shape and glass tokens as C constants.
 *
 * The palettes are device.css:4-15 (identical to Detent's src/bezel.js:29-45). Light mode is its own
 * table, never an inversion. Shape numbers are panel px on Bezel's 720-high design, which is the Tab5's
 * height in landscape: unit = 1, every number here is a Tab5 pixel. Glass parameters are the shader's
 * CSS-px values scaled ×1.41 to keep their proportion on a 720 px panel (see docs/bezel-port.md). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t ground, surface1, surface2, surface3, track, meter;
    uint32_t ink, dim, faint;
    uint32_t signal, on_signal;
    uint32_t ice, on_ice;
    uint32_t amber, on_amber, amber_container, amber_ink;
    uint32_t leaf, leaf_container;
    /* Catalyst status colours. Status colours mean status and nothing else (Catalyst Console's rule),
     * and always travel with a shape: ok ●, warn ▲, fault ■, stale ○. */
    uint32_t ok, warn, fault, stale;
    /* glass */
    uint32_t glass_even;   /* the grey glass evens toward */
    float glass_even_amt;  /* how far */
    uint32_t rim;          /* rim light colour */
    float rim_gain;
    float shadow_gain;
} bz_palette_t;

extern const bz_palette_t BZ_PAL_DARK, BZ_PAL_LIGHT;

/* The palette currently applied; set by bz_theme. */
extern const bz_palette_t *bz_pal;

/* Corner scale (M3) and the component radii Bezel derives from it (bezel.md:130-154). */
enum {
    BZ_R_XS = 4, BZ_R_S = 8, BZ_R_M = 12, BZ_R_L = 16, BZ_R_XL = 28, BZ_R_TILE = 32, BZ_R_CHIP = 30,
    BZ_R_DOCK = 46, BZ_R_DOCK_ITEM = 38, BZ_R_ISLAND = 30,
};

/* Spacing (device.css:62) */
enum {
    BZ_PAD_PAGE = 30,       /* page padding top and sides */
    BZ_PAD_PAGE_BOTTOM = 168,
    BZ_GAP = 14,            /* between tiles and chips */
    BZ_PAD_TILE = 24,
    BZ_TOUCH_MIN = 88,      /* 48 dp at the Tab5's 294 ppi */
};

/* Glass, in panel px (CSS values ×1.41) */
#define BZ_GLASS_FROST_SIGMA 11.3f
#define BZ_GLASS_THICKNESS 37.0f
#define BZ_GLASS_SATURATION 1.12f
#define BZ_GLASS_MERGE 22.6f
#define BZ_GLASS_GLOW_RADIUS 65.0f
#define BZ_GLASS_PRESS_SCALE 1.06f
/* The dock is too big to re-shape every frame of a press on the P4 (its geometry table is ~90k px): it
 * answers a press with the glow alone. Bezel scales it 1.02. */
#define BZ_GLASS_DOCK_PRESS_SCALE 1.0f

static inline uint8_t bz_r(uint32_t c) { return (uint8_t)(c >> 16); }
static inline uint8_t bz_g(uint32_t c) { return (uint8_t)(c >> 8); }
static inline uint8_t bz_b(uint32_t c) { return (uint8_t)c; }
static inline uint32_t bz_rgb(int r, int g, int b)
{
    return (uint32_t)((r < 0 ? 0 : r > 255 ? 255 : r) << 16 | (g < 0 ? 0 : g > 255 ? 255 : g) << 8 |
                      (b < 0 ? 0 : b > 255 ? 255 : b));
}
static inline uint32_t bz_mix(uint32_t a, uint32_t b, float t)
{
    return bz_rgb((int)(bz_r(a) + (bz_r(b) - bz_r(a)) * t + 0.5f), (int)(bz_g(a) + (bz_g(b) - bz_g(a)) * t + 0.5f),
                  (int)(bz_b(a) + (bz_b(b) - bz_b(a)) * t + 0.5f));
}
