/* home_astro — where the sun, the moon and the planets are, for home mode's clock faces.
 *
 * Low-precision formulas, good to a fraction of a degree for the sun and a degree or so for the moon and the
 * planets over this century: plenty for a picture, never a navigation instrument. Times are unix seconds (UTC);
 * angles are degrees unless a name says radians; latitude north and longitude east are positive.
 *
 * Sources: the sun after the NOAA/Meeus low-precision solar position; the moon's longitude from its mean elements
 * and the four largest periodic terms (Meeus ch. 47); the planets from JPL's "Keplerian elements for approximate
 * positions of the major planets" (1800-2050), with the equation of the centre to second order.
 *
 * Pure C with no dependencies: it builds on the PC too (the faces' pictures are tuned there). */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double elev, azim;     /* the sun seen from (lat, lon): altitude above the horizon; azimuth from north, east + */
    double dec;            /* its declination */
    double sub_lat, sub_lon; /* the point on the earth it stands over */
    double lon_ecl;        /* its ecliptic longitude */
} astro_sun_t;
void astro_sun(double t, double lat, double lon, astro_sun_t *out);

/* Sunrise and sunset around t (the local day that t falls in, by `tz_s` seconds east of UTC), as unix times, and
 * the sun's highest altitude that day. false: it doesn't rise or doesn't set (polar day or night). */
bool astro_sun_day(double t, double lat, double lon, double tz_s, double *rise, double *set, double *noon_elev);

typedef struct {
    double phase;          /* 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter */
    double lit;            /* the fraction of the disc lit, 0..1 */
    double elong;          /* its elongation from the sun, degrees 0..360 east */
    double lon_ecl, lat_ecl;
    double dist_km;
} astro_moon_t;
void astro_moon(double t, astro_moon_t *out);
/* The next time after t the moon reaches `phase` (0 new, 0.5 full), to about an hour. */
double astro_moon_next(double t, double phase);
/* The moon's altitude and azimuth from (lat, lon) at t (geocentric: up to a degree low near the horizon). */
void astro_moon_altaz(double t, double lat, double lon, double *alt, double *az);
/* Moonrise and moonset in the local day t falls in (zone tz_s): either can be missing on a given day (the moon
 * rises ~50 minutes later each day), then 0. */
void astro_moon_day(double t, double lat, double lon, double tz_s, double *rise, double *set);

/* Heliocentric ecliptic longitude (degrees) and distance (au) of planet i: 0 mercury, 1 venus, 2 earth, 3 mars,
 * 4 jupiter, 5 saturn, 6 uranus, 7 neptune. */
#define ASTRO_PLANETS 8
void astro_planet(int i, double t, double *lon, double *r_au);
extern const char *const ASTRO_PLANET_NAME[ASTRO_PLANETS];

/* day of the year 0..365 in UTC; and the julian day */
double astro_jd(double t);

#ifdef __cplusplus
}
#endif
