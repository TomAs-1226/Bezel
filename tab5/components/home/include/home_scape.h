/* home_scape — the pictures home mode's clock faces are painted from: the moon, the sky, the planets, the dials'
 * plates, a card's shadow, the world by day and night. Each is painted once (a day, ten minutes, a tone) into an
 * RGB565 picture on the covers' worker and then only shown; see ui_home_faces.c for when.
 *
 * One light, above and to the left, as everywhere in Bezel: plates are brighter toward the top left, shadows fall
 * down and to the right. The moon and the planets are the exception, lit by the sun where it really is.
 *
 * Pure C over home_px (no LVGL): it builds on the PC, where these were tuned. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the tone a picture is painted for: the theme's colours (0xRRGGBB) and the ground's exact 565 value */
typedef struct {
    bool dark;
    uint32_t ground, surface1, surface2, ink, dim, faint, signal, accent;
    uint16_t ground565;
} hs_tone_t;

/* ---- the moon ----
 * The disc of radius R centred in an S × S picture on the ground: its seas and bright craters where they are on
 * the real moon (no libration), lit for the phase (0 new, 0.5 full; waxing lit on the right, as seen from the
 * north; `south` turns it over), its dark part in earthshine. */
void hs_moon(uint16_t *px, int S, float R, double phase, bool south, const hs_tone_t *tone);

/* ---- the sky ----
 * Its colour from the zenith down to the horizon for the sun's altitude now (degrees) and the weather (a WMO
 * code, -1 unknown): night, the twilights, the golden hour, the day; greyer under cloud. */
typedef struct {
    uint32_t zenith, mid, horizon;
    float glow;             /* how much of the sun's warmth sits on the horizon, 0..1 */
    float stars;            /* how many stars show, 0..1 */
} hs_sky_t;
void hs_sky(double elev, int wmo, bool dark, hs_sky_t *out);
/* the colour at height y01 (0 the top of the sky, 1 the horizon) */
uint32_t hs_sky_at(const hs_sky_t *s, float y01);
/* A strip `w` wide (a multiple of 4, so its dither tiles) of a sky `h` tall: shown tiled across the screen. */
void hs_sky_strip(uint16_t *px, int w, int h, const hs_sky_t *s);
/* The same with land under it: the sky down to row `horizon`, a hairline of light on the horizon, then the land's
 * colour darkening from `land_top` to `land_bottom` toward the viewer. */
void hs_land_strip(uint16_t *px, int w, int h, int horizon, const hs_sky_t *s, uint32_t land_top, uint32_t land_bottom);
/* Rows y0 .. y0 + rows - 1 of that scene (h tall in all) into a picture w wide: the ground behind something
 * painted over it that must meet the tiled strip without a seam (place it at a multiple of 4, so the dither
 * lines up). */
void hs_land_rows(uint16_t *px, int w, int rows, int y0, int h, int horizon, const hs_sky_t *s, uint32_t land_top,
                  uint32_t land_bottom);

/* ---- the planets ----
 * The solar system at time t, seen from above the ecliptic: the sun at the centre of an S × S picture, the eight
 * planets on their orbits (radii compressed so all eight fit), at their real heliocentric longitudes, each lit
 * from the sun; the earth in the accent. `names` gets each planet's position (x, y in the picture) if not NULL. */
void hs_orrery(uint16_t *px, int S, double t, const hs_tone_t *tone, float xy[][2]);
/* The orrery in pieces, for a face that draws the orbits itself: the sun and its glow (S × S), and a planet as a
 * sprite S × S with its own alpha (an RGB565 plane and an A8 plane), lit from the direction (lx, ly); `ring`:
 * Saturn's. */
void hs_sun(uint16_t *px, int S, const hs_tone_t *tone);
extern const uint32_t HS_PLANET_COL[8]; /* mercury .. neptune; the earth's (0) is the accent */
void hs_planet(uint16_t *rgb, uint8_t *alpha, int S, float r, float lx, float ly, uint32_t col, int ring,
               const hs_tone_t *tone);
/* where each planet is drawn in that picture (xy), its orbit's radius and its disc's radius */
void hs_orrery_layout(int S, double t, float xy[][2], float orbit[], float size[]);

/* ---- dials ----
 * A dial's plate: a disc of radius R centred in an S × S picture, faintly domed (lighter toward the top left), its
 * rim lit, its shadow on the ground. `chrono`: a chronograph's, with three sunken sub-dials at nine (small
 * seconds), three and six. Only what is soft: it can be painted at half size and shown doubled, the marks
 * (hs_dial_ticks) drawn sharp over it. */
void hs_dial(uint16_t *px, int S, float R, bool chrono, const hs_tone_t *tone);
uint32_t hs_dial_face(const hs_tone_t *tone); /* the plate's own colour */
/* The marks of a dial of radius R, relative to its centre: the minute track and hour bars (and the chronograph's
 * seconds track and its sub-dials' scales). Returns how many. */
typedef struct {
    float x0, y0, x1, y1, w;
    bool major;
} hs_tick_t;
int hs_dial_ticks(float R, bool chrono, hs_tick_t *out, int max);
/* where a chronograph's sub-dial sits, relative to the dial's centre, and its radius (i: 0 nine, 1 three, 2 six) */
void hs_chrono_sub(float R, int i, float *dx, float *dy, float *r);

/* ---- a soft shadow ----
 * The shadow of a rounded rectangle w × h (radius r) lying on the ground, offset by (dx, dy) and blurred by
 * `blur` px, in a picture (w + 2·pad) × (h + 2·pad) whose rectangle sits at (pad, pad). */
void hs_shadow(uint16_t *px, int w, int h, float r, int pad, float dx, float dy, float blur, float strength,
               const hs_tone_t *tone);

/* ---- the world ----
 * The world as dots on an equirectangular grid (latitude lat0..lat1, all longitudes), `pitch` px apart: the day
 * side lit, the night side dim, the twilight between them from the sun's real position at t; `home` marked. */
void hs_world(uint16_t *px, int w, int h, int pitch, double lat0, double lat1, double t, double home_lat,
              double home_lon, const hs_tone_t *tone);
/* where (lat, lon) lands in that picture */
void hs_world_xy(int w, int h, double lat0, double lat1, double lat, double lon, float *x, float *y);

#ifdef __cplusplus
}
#endif
