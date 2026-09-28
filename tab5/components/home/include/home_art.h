/* home_art — album covers for songs that come without one, and the colour a cover gives the surface around it.
 *
 * The PC's media through Catalyst Link often has no art (a video in a browser), and the card's songs never do.
 * Such a song gets a generated cover: deterministic from a hash of its artist and album (else its title), so the
 * same album always wears the same cover. The hash picks one of several styles (a soft gradient mesh, an orb in
 * rings, a sky with a sun and ridges, Bauhaus geometry, diagonal bands, concentric rings) and a palette built in
 * OKLCH from one of a few harmonies (analogous, complementary, split, triad, one hue with an accent), so every
 * cover is a designed thing rather than noise. Film grain and a dither keep its gradients from banding in 565.
 *
 * A cover, real or generated, also gives a tint: its seed colour (vivid and common, as Monet picks one), and a
 * few surfaces for dark and light tone derived from it at fixed OKLCH lightness, so text keeps its contrast
 * whatever the cover. Pure C, no LVGL: tens of ms on the P4, so it runs off the UI thread. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_ART_L 360  /* the big cover: home mode's now playing */
#define HOME_ART_S 180  /* cards: the big one halved, box-filtered */

enum { HOME_ART_MESH, HOME_ART_ORB, HOME_ART_SKY, HOME_ART_BANDS, HOME_ART_BAUHAUS, HOME_ART_RINGS, HOME_ART_STYLES };

typedef struct {
    int style;                     /* HOME_ART_*, or -1 for real art */
    bool dark;                     /* the cover is mostly deep (else pale) */
    uint32_t seed;                 /* the colour the surface is tinted from */
    uint32_t colors[5];            /* the palette (generated), or colours found in it (real) */
} home_art_info_t;

/* Who a cover belongs to: artist and album when both are known, else the title, else the artist. Case and
 * repeated spaces don't matter. Never 0. */
uint32_t home_art_key(const char *artist, const char *album, const char *title);
const char *home_art_style_name(int style);

/* Paints the cover for `key` into work (HOME_ART_L² 0x00RRGGBB, kept for home_art_hero) and, when small isn't
 * NULL, its half-size copy (HOME_ART_S² RGB565, grain and dither). style < 0: the key chooses. */
void home_art_paint(uint32_t key, int style, uint32_t *work, uint16_t *small, home_art_info_t *info);
/* Real art (HOME_ART_L² RGB565): into work, its small copy, and its seed */
void home_art_adopt(const uint16_t *big, uint32_t *work, uint16_t *small, home_art_info_t *info);
/* The cover in work set on a flat ground (an exact 565 value, so the picture meets the page around it without a
 * seam) at (cx, cy) with rounded corners and a soft shadow falling down-right of Bezel's light: w × h RGB565. */
void home_art_hero(const uint32_t *work, uint16_t *px, int w, int h, int cx, int cy, int radius, uint16_t ground,
                   bool dark, uint32_t seed);

/* A surface's colours from a cover's seed, for a tone. Text on ground and surface keeps 4.5:1 or better. */
typedef struct {
    uint32_t ground, surface, surface2;   /* the page, a card on it, a control on the card */
    uint32_t ink, dim;                    /* text */
    uint32_t accent, on_accent;           /* the play button, the progress fill */
    uint32_t track;                       /* the progress bar's empty part */
} home_tint_t;
void home_art_tint(uint32_t seed, bool dark, home_tint_t *out);

#ifdef __cplusplus
}
#endif
