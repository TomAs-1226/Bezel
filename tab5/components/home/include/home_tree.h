/* home_tree — Bezel's low-poly tree (tree.js, treepass.js), for a CPU: the same seeded mesh of five-sided prisms
 * for wood and faceted icosahedra for leaf clusters, each piece tagged with the growth step it appears at, drawn
 * flat-shaded by one light into an RGB565 picture with a z-buffer, supersampled 2 × 2 so its facets have clean
 * edges. Nothing in it moves by itself: it is painted again when what it shows changes (a match played, the
 * month, the light), on the covers' worker.
 *
 * The mesh is built once per month and palette (its crown follows the month, as Bezel's does: bare in winter
 * with pale tips, blossom in spring, gold and red in autumn); painting it costs ~100-200 ms on the tablet.
 * Pure C over home_px: it builds on the PC too. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ht_mesh ht_mesh_t;

/* the tree's colours, 0xRRGGBB: from Bezel's leaf and amber so the tone carries into it */
typedef struct {
    uint32_t wood, leaf, amber, blossom, flower, tip;
} ht_palette_t;
void ht_palette(bool dark, uint32_t leaf, uint32_t amber, ht_palette_t *out);

/* The grown tree for a month (0 january .. 11 december, already turned for the southern hemisphere), with room
 * for `fruit` fruit hung on the wood that grows first. NULL: no memory. */
ht_mesh_t *ht_build(int month, const ht_palette_t *pal, int fruit);
void ht_free(ht_mesh_t *m);
int ht_steps(const ht_mesh_t *m);     /* growth steps to the whole tree */
int ht_fruit_max(const ht_mesh_t *m); /* fruit it has room for */

typedef struct {
    float growth;        /* steps grown (0 .. ht_steps) */
    int fruit;           /* fruit showing */
    float yaw;           /* the turn about its trunk, radians */
    float lean;          /* the wind's bend, 0 still .. 1 a gale, to the right */
    float light[3];      /* toward the light, view space (x right, y up, z toward the viewer), unit */
    float ambient;       /* the unlit side's share of the light, 0..1 (0.46 by day) */
    uint32_t tint;       /* the light's colour (0xFFFFFF by day, warmer at dusk, cool by the moon) */
    float cx, base_y;    /* where the trunk meets the ground, in the picture */
    float scale;         /* pixels per unit (the tree is ~2.2 units tall grown) */
} ht_view_t;

/* The tree drawn over what px already holds (w × h, stride in pixels), antialiased against it. */
void ht_paint(const ht_mesh_t *m, uint16_t *px, int w, int h, int stride, const ht_view_t *v);
/* its extent in the picture for that view: x0, y0, x1, y1 */
void ht_bounds(const ht_mesh_t *m, const ht_view_t *v, float out[4]);

#ifdef __cplusplus
}
#endif
