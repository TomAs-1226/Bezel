/* Tests for home_astro.c, the sun, moon and planets the clock faces draw: against published times (USNO / timeanddate)
 * for new, first-quarter and full moons, an equinox and a solstice, and a London midsummer sunrise and sunset. The
 * formulas are low-precision on purpose; the tolerances are what a picture needs (minutes, a degree), not an
 * almanac. Called from test_main.c (test_astro); it needs components/home/src/home_astro.c. */
#include "home_astro.h"

#include <math.h>
#include <stdio.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

/* the angular distance between two longitudes, degrees 0..180 */
static double dlon(double a, double b)
{
    double d = fmod(fabs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}

static void moon_phases(void)
{
    astro_moon_t m;
    astro_moon(1704974220.0, &m); /* new moon 2024-01-11 11:57 UTC */
    CHECK(m.lit < 0.01);
    CHECK(m.phase < 0.01 || m.phase > 0.99);
    astro_moon(1705549920.0, &m); /* first quarter 2024-01-18 03:52 UTC */
    NEAR(m.phase, 0.25, 0.01);
    NEAR(m.lit, 0.5, 0.03);
    astro_moon(1706205240.0, &m); /* full moon 2024-01-25 17:54 UTC */
    NEAR(m.phase, 0.5, 0.01);
    CHECK(m.lit > 0.99);
    /* the next full moon after the new one: within three hours of the published time */
    double full = astro_moon_next(1704974220.0 + 86400, 0.5);
    NEAR(full, 1706205240.0, 3 * 3600);
    CHECK(m.dist_km > 356000 && m.dist_km < 407000);
    /* a full moon rises as the sun sets: London, 2024-01-25 */
    double mr, ms, sr, ss, noon;
    astro_moon_day(1706205240.0, 51.5074, -0.1278, 0, &mr, &ms);
    CHECK(astro_sun_day(1706205240.0, 51.5074, -0.1278, 0, &sr, &ss, &noon));
    CHECK(mr > 0);
    NEAR(mr, ss, 5400);
}

static void sun_year(void)
{
    astro_sun_t s;
    astro_sun(1710903960.0, 51.5, 0, &s); /* the march equinox, 2024-03-20 03:06 UTC */
    CHECK(dlon(s.lon_ecl, 0) < 0.05);
    NEAR(s.dec, 0, 0.05);
    astro_sun(1718916660.0, 51.5, 0, &s); /* the june solstice, 2024-06-20 20:51 UTC */
    NEAR(s.dec, 23.44, 0.02);
    /* the earth is always opposite the sun */
    double lon, r;
    astro_planet(2, 1718916660.0, &lon, &r);
    CHECK(dlon(lon, s.lon_ecl + 180) < 0.5);
    NEAR(r, 1.016, 0.002); /* near aphelion */
}

static void sunrise_london(void)
{
    /* 2024-06-21 in London (BST, +1 h): sunrise 04:43, sunset 21:21 local */
    double rise, set, noon;
    bool ok = astro_sun_day(1718960400.0, 51.5074, -0.1278, 3600, &rise, &set, &noon);
    CHECK(ok);
    NEAR(rise, 1718941380.0, 150); /* 03:43 UTC */
    NEAR(set, 1719001260.0, 150);  /* 20:21 UTC */
    NEAR(noon, 90 - 51.5074 + 23.44, 0.5);
    /* and a polar summer: the sun doesn't set at 80 north */
    CHECK(!astro_sun_day(1718960400.0, 80, 0, 0, &rise, &set, &noon));
}

static void planets_sane(void)
{
    /* every planet's distance within its orbit's range */
    static const double LO[8] = { 0.30, 0.71, 0.98, 1.38, 4.9, 9.0, 18.2, 29.7 };
    static const double HI[8] = { 0.47, 0.73, 1.02, 1.67, 5.5, 10.1, 20.1, 30.4 };
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        double lon, r;
        astro_planet(i, 1718916660.0, &lon, &r);
        CHECK(r > LO[i] && r < HI[i]);
        CHECK(lon >= 0 && lon < 360);
    }
}

void test_astro(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    moon_phases();
    sun_year();
    sunrise_london();
    planets_sane();
}
