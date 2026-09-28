/* bz_frost — glass surfaces, baked once into images.
 *
 * The compositor in bz_comp.c renders glass per pixel per frame: a blur of what is behind, a bevel from
 * a signed-distance field, refraction, a directional rim. The tablet cannot afford any of that. It runs
 * BZ_LEAN, where bz_comp_compose is compiled out altogether, and it holds `calm`, which turns every
 * glass shape into an ordinary LVGL rounded rectangle with a solid fill. Measured across the dock's edge
 * the panel went 11 to 35 in a single step: no gradient, no bevel, not even an antialiased edge.
 *
 * So the surface becomes the texture. Each piece's bevel, grain and rim are drawn once, at that piece's
 * exact size and radius, into an ARGB8888 image that LVGL then blits as the object's background. Per
 * frame it is the same fill LVGL was already doing; the shape of the light is bought at startup instead
 * of sixty times a second.
 *
 * What it cannot do, and this is the honest trade against turning the compositor back on: the backdrop
 * is not in the picture. A baked surface cannot blur what happens to be behind it, so these read as
 * frosted panels lit from above rather than as windows onto the page. On a dock and an orb that sit over
 * a mostly dark ground, that is most of the look for a fraction of the cost.
 */
#pragma once
#include "lvgl.h"

#include <stdint.h>

/* The baked surface for a piece this size, colour and opacity, or NULL if one could not be made.
 *
 * Cached and shared: pieces of the same geometry and colour get the same image, which is what keeps the
 * dock's tiles from costing one texture each. `base` is 0xRRGGBB, `opa` is 0..255 and is baked into the
 * alpha, so a fading piece is not a new texture for every step -- callers quantize opacity before
 * asking (bz_glass already moves it in sixteenths). */
const lv_image_dsc_t *bz_frost(int w, int h, int radius, uint32_t base, int opa);

/* Each bz_frost() is a reference to the picture it returns: a piece that stops showing one (a new size, a new
 * colour, no surface at all) releases it, and only a picture nobody holds is ever evicted for a new one. */
void bz_frost_release(const lv_image_dsc_t *d);

/* Drop every baked surface: the palette changed, so the colours in them are wrong. */
void bz_frost_flush(void);

/* How much PSRAM the baked surfaces are holding, for the dev console's "mem". */
size_t bz_frost_bytes(void);

/* One sample of the shared surface-roughness field, -127..127, tiling every 64 px.
 *
 * Two octaves of smoothed value noise, baked once. White noise at one pixel reads as sensor grain and
 * dithering rather than as a surface, so it is smoothed to give the grain a size, with a second octave
 * to keep it from looking regular. Lives here because both glass renderers want the same one: the
 * compositor modulates its bevel light with it, and the baked surfaces below write it in. */
int8_t bz_grain_at(int x, int y);
