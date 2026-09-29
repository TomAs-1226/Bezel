/* home_astro — see home_astro.h. */
#include "home_astro.h"

#include <math.h>

#define RAD (3.14159265358979323846 / 180.0)
#define DEG (180.0 / 3.14159265358979323846)

const char *const ASTRO_PLANET_NAME[ASTRO_PLANETS] = { "mercury", "venus", "earth", "mars",
                                                       "jupiter", "saturn", "uranus", "neptune" };

double astro_jd(double t) { return t / 86400.0 + 2440587.5; }

static double norm360(double a)
{
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

static double norm180(double a)
{
    a = norm360(a);
    return a > 180.0 ? a - 360.0 : a;
}

/* ---------------------------------------------------------------- the sun */

void astro_sun(double t, double lat, double lon, astro_sun_t *o)
{
    double d = astro_jd(t) - 2451545.0;
    double g = norm360(357.529 + 0.98560028 * d) * RAD;
    double q = norm360(280.459 + 0.98564736 * d);
    double L = norm360(q + 1.915 * sin(g) + 0.020 * sin(2 * g));
    double e = (23.439 - 0.00000036 * d) * RAD;
    double Lr = L * RAD;
    double ra = atan2(cos(e) * sin(Lr), cos(Lr)) * DEG;
    double dec = asin(sin(e) * sin(Lr));
    double gmst = norm360((18.697374558 + 24.06570982441908 * d) * 15.0);
    double H = norm180(gmst + lon - ra) * RAD;
    double la = lat * RAD;
    double sa = sin(la) * sin(dec) + cos(la) * cos(dec) * cos(H);
    o->elev = asin(sa < -1 ? -1 : sa > 1 ? 1 : sa) * DEG;
    o->azim = norm360(atan2(-sin(H), tan(dec) * cos(la) - sin(la) * cos(H)) * DEG);
    o->dec = dec * DEG;
    o->sub_lat = dec * DEG;
    o->sub_lon = norm180(ra - gmst);
    o->lon_ecl = L;
}

static double elev_at(double t, double lat, double lon)
{
    astro_sun_t s;
    astro_sun(t, lat, lon, &s);
    return s.elev;
}

/* the moment between a and b where the altitude crosses h (they straddle it) */
static double cross(double a, double b, double h, double lat, double lon)
{
    double ea = elev_at(a, lat, lon) - h;
    for (int i = 0; i < 24; i++) {
        double m = (a + b) / 2, em = elev_at(m, lat, lon) - h;
        if ((em < 0) == (ea < 0)) {
            a = m;
            ea = em;
        } else {
            b = m;
        }
    }
    return (a + b) / 2;
}

bool astro_sun_day(double t, double lat, double lon, double tz_s, double *rise, double *set, double *noon_elev)
{
    const double H0 = -0.833; /* refraction and the sun's radius: the upper limb on the horizon */
    double day0 = floor((t + tz_s) / 86400.0) * 86400.0 - tz_s;
    const double STEP = 600.0;
    double prev_t = day0, prev = elev_at(day0, lat, lon) - H0, best = prev + H0;
    bool have_r = false, have_s = false;
    *rise = *set = 0;
    for (double s = day0 + STEP; s <= day0 + 86400.0 + 1; s += STEP) {
        double e = elev_at(s, lat, lon);
        if (e > best) best = e;
        double v = e - H0;
        if (prev < 0 && v >= 0 && !have_r) {
            *rise = cross(prev_t, s, H0, lat, lon);
            have_r = true;
        } else if (prev >= 0 && v < 0 && !have_s) {
            *set = cross(prev_t, s, H0, lat, lon);
            have_s = true;
        }
        prev = v;
        prev_t = s;
    }
    if (noon_elev) *noon_elev = best;
    return have_r && have_s;
}

/* ---------------------------------------------------------------- the moon */

void astro_moon(double t, astro_moon_t *o)
{
    double d = astro_jd(t) - 2451545.0;
    double Lp = norm360(218.3164477 + 13.17639648 * d);
    double D = norm360(297.8501921 + 12.19074912 * d) * RAD;
    double M = norm360(357.5291092 + 0.98560028 * d) * RAD;
    double Mp = norm360(134.9633964 + 13.06499295 * d) * RAD;
    double F = norm360(93.2720950 + 13.22935024 * d) * RAD;
    double lam = Lp + 6.289 * sin(Mp) + 1.274 * sin(2 * D - Mp) + 0.658 * sin(2 * D) + 0.214 * sin(2 * Mp) -
                 0.186 * sin(M) - 0.114 * sin(2 * F);
    double bet = 5.128 * sin(F) + 0.281 * sin(Mp + F) + 0.278 * sin(Mp - F) + 0.173 * sin(2 * D - F);
    o->lon_ecl = norm360(lam);
    o->lat_ecl = bet;
    o->dist_km = 385001.0 - 20905.0 * cos(Mp) - 3699.0 * cos(2 * D - Mp) - 2956.0 * cos(2 * D);
    astro_sun_t s;
    astro_sun(t, 0, 0, &s);
    /* the elongation along the ecliptic and, for how much is lit, across it too */
    double el = norm360(o->lon_ecl - s.lon_ecl);
    double cpsi = cos(bet * RAD) * cos(el * RAD);
    o->elong = el;
    o->phase = el / 360.0;
    o->lit = (1.0 - cpsi) / 2.0;
}

double astro_moon_next(double t, double phase)
{
    astro_moon_t m;
    astro_moon(t, &m);
    double target = phase * 360.0;
    double prev = norm360(m.elong - target);
    double a = t;
    for (int i = 0; i < 240; i++) { /* six-hour steps, ~60 days at most */
        double b = a + 6 * 3600.0;
        astro_moon(b, &m);
        double v = norm360(m.elong - target);
        if (v < prev && prev - v > 180.0) {
            /* wrapped through the target between a and b */
            for (int k = 0; k < 20; k++) {
                double c = (a + b) / 2;
                astro_moon(c, &m);
                double w = norm360(m.elong - target);
                if (w > 180.0) a = c;
                else b = c;
            }
            return (a + b) / 2;
        }
        prev = v;
        a = b;
    }
    return t;
}

void astro_moon_altaz(double t, double lat, double lon, double *alt, double *az)
{
    astro_moon_t m;
    astro_moon(t, &m);
    double d = astro_jd(t) - 2451545.0;
    double e = (23.439 - 0.00000036 * d) * RAD, L = m.lon_ecl * RAD, B = m.lat_ecl * RAD;
    /* ecliptic to equatorial */
    double ra = atan2(sin(L) * cos(e) - tan(B) * sin(e), cos(L)) * DEG;
    double dec = asin(sin(B) * cos(e) + cos(B) * sin(e) * sin(L));
    double gmst = norm360((18.697374558 + 24.06570982441908 * d) * 15.0);
    double H = norm180(gmst + lon - ra) * RAD, la = lat * RAD;
    double sa = sin(la) * sin(dec) + cos(la) * cos(dec) * cos(H);
    *alt = asin(sa < -1 ? -1 : sa > 1 ? 1 : sa) * DEG;
    *az = norm360(atan2(-sin(H), tan(dec) * cos(la) - sin(la) * cos(H)) * DEG);
}

void astro_moon_day(double t, double lat, double lon, double tz_s, double *rise, double *set)
{
    /* the upper limb on the horizon, allowing for the moon's parallax: +0.125 degrees, geocentrically */
    const double H0 = 0.125;
    double day0 = floor((t + tz_s) / 86400.0) * 86400.0 - tz_s;
    *rise = *set = 0;
    double a, z;
    astro_moon_altaz(day0, lat, lon, &a, &z);
    double prev = a - H0, prev_t = day0;
    for (double s = day0 + 600; s <= day0 + 86400.0 + 1; s += 600) {
        astro_moon_altaz(s, lat, lon, &a, &z);
        double v = a - H0;
        if ((prev < 0) != (v < 0)) {
            /* bisect the crossing */
            double lo = prev_t, hi = s, plo = prev;
            for (int k = 0; k < 20; k++) {
                double mid = (lo + hi) / 2, am, zm;
                astro_moon_altaz(mid, lat, lon, &am, &zm);
                if (((am - H0) < 0) == (plo < 0)) lo = mid;
                else hi = mid;
            }
            if (prev < 0 && !*rise) *rise = (lo + hi) / 2;
            else if (prev >= 0 && !*set) *set = (lo + hi) / 2;
        }
        prev = v;
        prev_t = s;
    }
}

/* ---------------------------------------------------------------- the planets */

static const struct {
    double a, e, L, Ldot, peri;
} PL[ASTRO_PLANETS] = {
    { 0.38709927, 0.20563593, 252.25032350, 149472.67411175, 77.45779628 },
    { 0.72333566, 0.00677672, 181.97909950, 58517.81538729, 131.60246718 },
    { 1.00000261, 0.01671123, 100.46457166, 35999.37244981, 102.93768193 },
    { 1.52371034, 0.09339410, -4.55343205, 19140.30268499, -23.94362959 },
    { 5.20288700, 0.04838624, 34.39644051, 3034.74612775, 14.72847983 },
    { 9.53667594, 0.05386179, 49.95424423, 1222.49362201, 92.59887831 },
    { 19.18916464, 0.04725744, 313.23810451, 428.48202785, 170.95427630 },
    { 30.06992276, 0.00859048, -55.12002969, 218.45945325, 44.96476227 },
};

void astro_planet(int i, double t, double *lon, double *r_au)
{
    if (i < 0 || i >= ASTRO_PLANETS) {
        *lon = 0;
        *r_au = 1;
        return;
    }
    double T = (astro_jd(t) - 2451545.0) / 36525.0;
    double e = PL[i].e;
    double M = norm180(PL[i].L + PL[i].Ldot * T - PL[i].peri) * RAD;
    double E = M + e * sin(M);
    for (int k = 0; k < 6; k++) E -= (E - e * sin(E) - M) / (1 - e * cos(E));
    double nu = 2 * atan2(sqrt(1 + e) * sin(E / 2), sqrt(1 - e) * cos(E / 2));
    *r_au = PL[i].a * (1 - e * cos(E));
    *lon = norm360(nu * DEG + PL[i].peri);
}
