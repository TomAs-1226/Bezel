/* home_scape — see home_scape.h. */
#include "home_scape.h"

#include "home_astro.h"
#include "home_px.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f
#define RADF (PI_F / 180.0f)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth(float a, float b, float x)
{
    float t = clampf((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
static uint32_t rgbf(float r, float g, float b)
{
    return hpx_rgb((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f));
}
static void unpack(uint32_t c, float *r, float *g, float *b)
{
    *r = hpx_r(c) / 255.0f;
    *g = hpx_g(c) / 255.0f;
    *b = hpx_b(c) / 255.0f;
}
static uint32_t mixc(uint32_t a, uint32_t b, float t) { return hpx_mix(a, b, (int)(clampf(t, 0, 1) * 256)); }

static void fill565(uint16_t *px, size_t n, uint16_t v)
{
    for (size_t i = 0; i < n; i++) px[i] = v;
}

/* ================================================================ the moon */

/* The near side's seas as a field of soft patches: selenographic latitude and longitude (east +), a reach in
 * degrees of arc on the moon (1 degree is 30 km) and a weight. Patches that overlap merge, so the big seas come
 * out joined and irregular as they are (Procellarum into Imbrium, Serenitatis into Tranquillitatis) rather than
 * as separate discs; their shores are the field crossing a threshold, made ragged by noise. Centres and sizes from
 * the IAU gazetteer. `tone` shades each sea a little: Serenitatis is lighter, Tranquillitatis darker and bluer. */
typedef struct {
    float lat, lon, r, w, tone;
} sea_t;

static const sea_t SEAS[] = {
    /* Oceanus Procellarum: the great western ocean, lat -10..+45, lon -80..-25, lighter than Imbrium */
    { 30, -55, 14, 0.9f, -0.15f }, { 17, -60, 15, 0.9f, -0.15f }, { 5, -57, 12, 0.85f, -0.1f },
    { -4, -48, 9, 0.75f, -0.1f }, { 40, -45, 9, 0.8f, -0.1f }, { 24, -43, 10, 0.9f, -0.1f }, { 10, -70, 8, 0.75f, 0 },
    /* Imbrium, with Sinus Iridum on its north-west shore */
    { 33, -17, 17, 1.2f, 0.05f }, { 36, -28, 11, 1.0f, 0 }, { 45, -31, 5, 0.8f, 0 }, { 26, -9, 8, 0.85f, 0 },
    /* Frigoris, a thin sea along the north */
    { 57, -35, 4.5f, 0.7f, -0.1f }, { 56, -20, 4.5f, 0.7f, -0.1f }, { 56, -5, 4.5f, 0.7f, -0.1f },
    { 56, 10, 4.5f, 0.65f, -0.1f }, { 57, 25, 4, 0.6f, -0.1f }, { 58, 38, 4, 0.5f, -0.1f },
    /* Serenitatis (lighter), Vaporum, Tranquillitatis (darker), Crisium */
    { 28, 17.5f, 11.5f, 1.1f, -0.2f }, { 13, 4, 5, 0.8f, 0 }, { 8, 25, 9.5f, 1.05f, 0.25f }, { 4, 35, 9.5f, 1.05f, 0.25f },
    { 15, 31, 6.5f, 0.9f, 0.2f }, { 17, 59, 9, 1.25f, 0.1f },
    /* Fecunditatis, Nectaris, Sinus Medii */
    { -4, 50, 9, 0.95f, 0.05f }, { -13, 53, 7, 0.9f, 0.05f }, { -15, 35, 5.5f, 0.95f, 0 }, { 2, 2, 3.5f, 0.55f, -0.2f },
    /* Insularum, Cognitum, Nubium (pale), Humorum (small, round, dark) */
    { 8, -30, 7, 0.8f, -0.1f }, { -10, -23, 5.5f, 0.65f, -0.15f }, { -21, -16, 9, 0.75f, -0.3f },
    { -24.5f, -38.5f, 6, 1.15f, 0.05f },
    /* at the limbs: Smythii, Marginis, Australe; Grimaldi and Plato, small dark floors */
    { -1.5f, 87, 6, 0.8f, 0 }, { 13, 86, 5, 0.7f, 0 }, { -40, 86, 8, 0.6f, 0 },
    { -5.2f, -68.6f, 3.2f, 1.3f, 0.25f }, { 51.6f, -9.4f, 1.9f, 1.4f, 0.25f },
};

/* bright young craters: Tycho and Copernicus (with rays), Kepler, Aristarchus (the brightest spot on the moon),
 * Proclus, Menelaus */
typedef struct {
    float lat, lon, r, a;
    int rays;
    float reach;
} crater_t;
static const crater_t BRIGHT[] = {
    { -43.3f, -11.4f, 1.6f, 1.0f, 30, 0.95f }, { 9.6f, -20.1f, 1.6f, 0.85f, 18, 0.32f },
    { 8.1f, -38.0f, 0.8f, 0.75f, 10, 0.2f }, { 23.7f, -47.4f, 0.8f, 1.0f, 0, 0 },
    { 16.1f, 46.8f, 0.6f, 0.6f, 6, 0.15f }, { 16.3f, 16.0f, 0.5f, 0.5f, 0, 0 },
};

static void sel_vec(float lat, float lon, float v[3])
{
    v[0] = cosf(lat * RADF) * sinf(lon * RADF);
    v[1] = sinf(lat * RADF);
    v[2] = cosf(lat * RADF) * cosf(lon * RADF);
}

void hs_moon(uint16_t *px, int S, float R, double phase, bool south, const hs_tone_t *tone)
{
    const bool dark = tone->dark;
    fill565(px, (size_t)S * S, tone->ground565);
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    float cx = S * 0.5f, cy = S * 0.5f;
    float el = (float)(phase * 360.0) * RADF;
    /* the sun's direction: behind the moon at new (z -1), from the right at first quarter, from us at full */
    float Lx = sinf(el), Lz = -cosf(el);
    float lit_frac = (1.0f - cosf(el)) * 0.5f;
    float earthshine = (1.0f + cosf(el)) * 0.5f; /* the earth is full when the moon is new */
    if (dark) hpx_glow(&c, cx, cy, R * 1.45f, 0x8E97AA, (int)(30 * lit_frac * lit_frac + 3));
    enum { NS = sizeof SEAS / sizeof SEAS[0], NB = sizeof BRIGHT / sizeof BRIGHT[0] };
    float sv[NS][3], sk[NS], bv[NB][3], bk[NB];
    for (int i = 0; i < NS; i++) {
        sel_vec(SEAS[i].lat, SEAS[i].lon, sv[i]);
        float r = SEAS[i].r * 1.6f * RADF; /* the patch's reach, past its nominal radius: the field is soft */
        sk[i] = 2.0f / (r * r);            /* theta² = 2(1 - cos theta), so (theta / r)² = (1 - d)·sk */
    }
    for (int i = 0; i < NB; i++) {
        sel_vec(BRIGHT[i].lat, BRIGHT[i].lon, bv[i]);
        float r = BRIGHT[i].r * 2.4f * RADF;
        bk[i] = 2.0f / (r * r);
    }
    /* the rays, in the picture's plane from each crater: a direction, a length and a strength */
    struct { float ox, oy, ux, uy, len, a, w; } ray[64];
    int nray = 0;
    hpx_rng_t rng = { 0x7c0u };
    for (int k = 0; k < NB; k++) {
        float v[3];
        sel_vec(BRIGHT[k].lat, BRIGHT[k].lon, v);
        for (int i = 0; i < BRIGHT[k].rays && nray < 64; i++) {
            float a = (i + hpx_rand(&rng) * 0.8f) / BRIGHT[k].rays * 2 * PI_F;
            ray[nray].ox = v[0];
            ray[nray].oy = v[1];
            ray[nray].ux = cosf(a);
            ray[nray].uy = sinf(a);
            ray[nray].len = BRIGHT[k].reach * (0.35f + hpx_rand(&rng) * 0.65f);
            ray[nray].a = BRIGHT[k].a * (0.4f + hpx_rand(&rng) * 0.6f);
            ray[nray].w = 0.008f + hpx_rand(&rng) * 0.012f;
            nray++;
        }
    }
    /* colours: warm grey highlands, cooler seas; on a light page a pale moon with soft grey seas */
    float hr, hg, hb, mr, mg, mb;
    if (dark) {
        hr = 1.0f; hg = 0.985f; hb = 0.95f;
        mr = 0.97f; mg = 0.985f; mb = 1.0f;
    } else {
        hr = 1.02f; hg = 1.015f; hb = 1.0f;
        mr = 1.0f; mg = 1.01f; mb = 1.03f;
    }
    float ur, ug, ub;
    unpack(dark ? 0x000000 : hpx_mix(tone->ground, tone->ink, 34), &ur, &ug, &ub);
    int y0 = (int)floorf(cy - R - 1), y1 = (int)ceilf(cy + R + 1);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= S) continue;
        float fy = (y + 0.5f - cy) / R;
        for (int x = (int)floorf(cx - R - 1); x <= (int)ceilf(cx + R + 1); x++) {
            if (x < 0 || x >= S) continue;
            float fx = (x + 0.5f - cx) / R, r2 = fx * fx + fy * fy;
            float cov = (1.0f - sqrtf(r2)) * R + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            float nx = fx, ny = -fy;
            if (south) {
                nx = -nx;
                ny = -ny;
            }
            float nz = sqrtf(fmaxf(0, 1 - r2));
            /* the seas: the patches' field, a noisy shore */
            float field = 0, ftone = 0;
            for (int i = 0; i < NS; i++) {
                float q = (1.0f - (nx * sv[i][0] + ny * sv[i][1] + nz * sv[i][2])) * sk[i];
                if (q >= 1) continue;
                float b = (1 - q) * (1 - q) * SEAS[i].w;
                field += b;
                ftone += b * SEAS[i].tone;
            }
            float n1 = hpx_noise2(nx * 7.0f + 3.1f, ny * 7.0f + 7.7f, 0x3001);
            float n2 = hpx_noise2(nx * 19.0f + 1.3f, ny * 19.0f + 2.9f, 0x3002);
            float n3 = hpx_noise2(nx * 61.0f, ny * 61.0f, 0x3003);
            float n4 = hpx_noise2(nx * 150.0f, ny * 150.0f, 0x3005);
            float shore = field + (n1 - 0.5f) * 0.3f + (n2 - 0.5f) * 0.14f;
            float sea = smooth(0.3f, 0.72f, shore);
            float st = field > 0.01f ? ftone / field : 0;
            /* the highlands: bright, pocked with small craters (a noise's peaks and pits); the seas: smoother, mottled
             * with lighter ridges */
            float pit = smooth(0.74f, 0.9f, n4) * 0.08f - smooth(0.26f, 0.1f, n4) * 0.07f;
            float hi = 0.87f + (n1 - 0.5f) * 0.05f + (n3 - 0.5f) * 0.04f + pit * 0.6f;
            float lo = 0.53f - st * 0.07f + (n2 - 0.5f) * 0.07f + (n3 - 0.5f) * 0.03f + pit * 0.3f;
            float alb = mixf(hi, lo, sea);
            float bright = 0;
            for (int i = 0; i < NB; i++) {
                float q = (1.0f - (nx * bv[i][0] + ny * bv[i][1] + nz * bv[i][2])) * bk[i];
                if (q >= 1) continue;
                bright = fmaxf(bright, (1 - q) * (1 - q) * BRIGHT[i].a);
            }
            for (int i = 0; i < nray; i++) {
                float dx = nx - ray[i].ox, dy = ny - ray[i].oy;
                float along = dx * ray[i].ux + dy * ray[i].uy;
                if (along <= 0.01f || along > ray[i].len) continue;
                float perp = fabsf(dx * ray[i].uy - dy * ray[i].ux);
                float wdt = ray[i].w * (1 + along * 5);
                if (perp > wdt) continue;
                /* soft across, patchy along: a ray is a spray of ejecta, not a line */
                float across = 1 - perp / wdt;
                float patch = hpx_noise1(along * 26.0f + i * 7.3f, 0x3006);
                float k = across * across * (1 - along / ray[i].len) * (0.35f + 0.65f * patch);
                bright = fmaxf(bright, k * ray[i].a * 1.25f);
            }
            /* past the highlands' white: a young crater is the brightest thing on the moon */
            alb = alb + (1.0f - alb) * clampf(bright, 0, 1) * 0.85f;
            /* light: Lommel-Seeliger (no limb darkening: the full moon is evenly bright), a terminator made
             * ragged by the terrain, and relief showing near it */
            float mu0 = nx * Lx + nz * Lz;
            float mu = nz < 0.02f ? 0.02f : nz;
            float rough = (n2 - 0.5f) * 0.05f + (n3 - 0.5f) * 0.035f;
            float day = smooth(-0.02f, 0.05f, mu0 + rough);
            float ls = mu0 > 0 ? 2.0f * mu0 / (mu0 + mu) : 0;
            float I = day * clampf(0.28f + 0.72f * ls + (n3 - 0.5f) * 0.25f * (1 - clampf(ls, 0, 1)), 0, 1.15f);
            float seaw = clampf(sea, 0, 1);
            float cr = mixf(hr, mr, seaw) * alb, cg = mixf(hg, mg, seaw) * alb, cb = mixf(hb, mb + st * 0.02f, seaw) * alb;
            float R0, G0, B0;
            if (dark) {
                float es = 0.04f + 0.07f * earthshine;
                R0 = mixf(cr * es * 0.85f, cr, I);
                G0 = mixf(cg * es * 0.95f, cg, I);
                B0 = mixf(cb * es * 1.2f, cb, I);
            } else {
                R0 = mixf(ur, cr, I);
                G0 = mixf(ug, cg, I);
                B0 = mixf(ub, cb, I);
            }
            uint32_t col = rgbf(clampf(R0, 0, 1), clampf(G0, 0, 1), clampf(B0, 0, 1));
            hpx_put(&c, x, y, col, (int)(cov * 255));
        }
    }
    /* on a light page the moon's edge, faintly, so the unlit part reads as a disc */
    if (!dark) hpx_ring(&c, cx, cy, R - 0.5f, 1.2f, 0, tone->ink, 26);
}

/* ================================================================ the sky */

void hs_sky(double elev, int wmo, bool dark, hs_sky_t *o)
{
    /* keyframes by the sun's altitude, zenith / middle / horizon: deep night, astronomical and nautical twilight,
     * civil twilight, the golden hour, the day */
    typedef struct { float e; uint32_t z, m, h; } key_t;
    static const key_t D[] = {
        { -18, 0x05070D, 0x080B14, 0x0C101B },
        { -12, 0x070A16, 0x0E1428, 0x1A1E36 },
        { -6, 0x0B1230, 0x232A55, 0x533A5E },
        { -2, 0x121E48, 0x363F68, 0xA85E4C },
        { 2, 0x1A3264, 0x4A5F8C, 0xD08A5E },
        { 8, 0x1A355C, 0x3C5F8A, 0x9A8C84 },
        { 20, 0x15325A, 0x2F5584, 0x5F7FA6 },
        { 60, 0x122F57, 0x2A5082, 0x5677A0 },
    };
    static const key_t L[] = {
        { -18, 0x2A3350, 0x3A4466, 0x4A5376 },
        { -12, 0x33406A, 0x4A5582, 0x6A6890 },
        { -6, 0x4A5A94, 0x7A80B4, 0xC4A0B8 },
        { -2, 0x6A82C0, 0xA8B4DC, 0xF4C0A4 },
        { 2, 0x80A4DC, 0xB8CCEC, 0xFAD6B4 },
        { 8, 0x8CB8EA, 0xC0DAF4, 0xF6E6D4 },
        { 20, 0x94C4F0, 0xC4E0F8, 0xEAF4FC },
        { 60, 0x8EC0F0, 0xC0DEF8, 0xE8F4FC },
    };
    int n = (int)(sizeof D / sizeof D[0]);
    const key_t *K = dark ? D : L;
    float e = (float)elev;
    int i = 0;
    while (i < n - 2 && e > K[i + 1].e) i++;
    float t = clampf((e - K[i].e) / (K[i + 1].e - K[i].e), 0, 1);
    uint32_t z = mixc(K[i].z, K[i + 1].z, t), m = mixc(K[i].m, K[i + 1].m, t), h = mixc(K[i].h, K[i + 1].h, t);
    /* cloud: overcast greys it toward its own luminance, rain and snow darken it */
    float cloud = 0, gloom = 0;
    if (wmo == 2) cloud = 0.35f;
    else if (wmo == 3) cloud = 0.8f;
    else if (wmo == 45 || wmo == 48) cloud = 0.9f;
    else if (wmo >= 51 && wmo <= 67) { cloud = 0.85f; gloom = 0.25f; }
    else if (wmo >= 71 && wmo <= 77) { cloud = 0.9f; gloom = 0.05f; }
    else if (wmo >= 80 && wmo <= 86) { cloud = 0.75f; gloom = 0.2f; }
    else if (wmo >= 95) { cloud = 0.95f; gloom = 0.45f; }
    uint32_t *cs[3] = { &z, &m, &h };
    for (int k = 0; k < 3; k++) {
        int l = hpx_luma(*cs[k]);
        uint32_t grey = hpx_rgb(l, l + 2, l + 6);
        *cs[k] = mixc(*cs[k], grey, cloud);
        *cs[k] = mixc(*cs[k], 0x000000, gloom * (dark ? 1.0f : 0.5f));
    }
    o->zenith = z;
    o->mid = m;
    o->horizon = h;
    o->glow = (1 - cloud) * smooth(-8, -1, e) * (1 - smooth(4, 14, e));
    o->stars = (1 - cloud * 0.9f) * (1 - smooth(-14, -5, e)) * (dark ? 1.0f : 0.35f);
}

uint32_t hs_sky_at(const hs_sky_t *s, float y)
{
    y = clampf(y, 0, 1);
    /* the middle tone sits low: most of a sky is its upper colour, the horizon's band is thin */
    if (y < 0.62f) return mixc(s->zenith, s->mid, smooth(0, 0.62f, y) * 0.9f + y * 0.1f / 0.62f);
    return mixc(s->mid, s->horizon, smooth(0.62f, 1.0f, y));
}

void hs_sky_strip(uint16_t *px, int w, int h, const hs_sky_t *s)
{
    hpx_t c;
    hpx_init16(&c, px, w, h, w);
    for (int y = 0; y < h; y++) {
        hpx_clip(&c, 0, y, w - 1, y);
        hpx_fill(&c, hs_sky_at(s, (y + 0.5f) / h));
    }
}

void hs_land_rows(uint16_t *px, int w, int rows, int y0, int h, int horizon, const hs_sky_t *s, uint32_t land_top,
                  uint32_t land_bottom)
{
    hpx_t c;
    hpx_init16(&c, px, w, rows, w);
    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        uint32_t col;
        if (y < horizon) col = hs_sky_at(s, (y + 0.5f) / horizon);
        else if (y == horizon) col = mixc(hs_sky_at(s, 1), land_top, 0.35f);
        else col = mixc(land_top, land_bottom, smooth(0, 1, (float)(y - horizon) / (h - horizon)));
        hpx_clip(&c, 0, r, w - 1, r);
        hpx_fill(&c, col);
    }
}

void hs_land_strip(uint16_t *px, int w, int h, int horizon, const hs_sky_t *s, uint32_t land_top, uint32_t land_bottom)
{
    hs_land_rows(px, w, h, 0, h, horizon, s, land_top, land_bottom);
}

/* ================================================================ the planets */

const uint32_t HS_PLANET_COL[ASTRO_PLANETS] = { 0xB4ADA3, 0xEAD8AA, 0, 0xD4764A, 0xD9B58E, 0xE3CC90, 0xA2D8DE, 0x6B8FDC };

void hs_orrery_layout(int S, double t, float xy[][2], float orbit[], float size[])
{
    /* the orbits' radii, compressed so eight fit: spaced by rank, a little wider apart outward; sizes not to
     * scale, but in order */
    static const float SZ[ASTRO_PLANETS] = { 5.5f, 8.5f, 9, 7, 17, 14.5f, 11, 11 };
    float cx = S * 0.5f, cy = S * 0.5f, Rmax = S * 0.5f - 22;
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        float r = Rmax * (0.17f + 0.83f * powf((i + 1) / 8.0f, 1.1f));
        double lon, au;
        astro_planet(i, t, &lon, &au);
        /* ecliptic longitude counter-clockwise from the right, as seen from the north */
        float a = (float)lon * RADF;
        if (xy) {
            xy[i][0] = cx + cosf(a) * r;
            xy[i][1] = cy - sinf(a) * r;
        }
        if (orbit) orbit[i] = r;
        if (size) size[i] = SZ[i];
    }
}

void hs_orrery(uint16_t *px, int S, double t, const hs_tone_t *tone, float xy_out[][2])
{
    const bool dark = tone->dark;
    fill565(px, (size_t)S * S, tone->ground565);
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    float cx = S * 0.5f, cy = S * 0.5f;
    float xy[ASTRO_PLANETS][2], rad[ASTRO_PLANETS], sz[ASTRO_PLANETS];
    hs_orrery_layout(S, t, xy, rad, sz);
    if (xy_out) memcpy(xy_out, xy, sizeof xy);
    uint32_t orbit = hpx_mix(tone->ground, tone->ink, dark ? 34 : 46);
    for (int i = 0; i < ASTRO_PLANETS; i++)
        hpx_ring(&c, cx, cy, rad[i], i == 2 ? 1.8f : 1.2f, 0, i == 2 ? mixc(orbit, tone->accent, 0.55f) : orbit, 255);
    /* the sun: a warm glow, a bright disc lit a little brighter at its top left */
    uint32_t sun = dark ? 0xFFCE70 : 0xF4A63A;
    hpx_glow(&c, cx, cy, rad[1] * 0.95f, dark ? 0xFF9030 : 0xF7B25A, dark ? 64 : 80);
    hpx_disc(&c, cx, cy, 17, 0, sun, 255);
    hpx_disc(&c, cx - 4, cy - 5, 8, 7, 0xFFF6DC, 170);
    const uint32_t *COL = HS_PLANET_COL;
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        float x = xy[i][0], y = xy[i][1], s = sz[i];
        uint32_t col = i == 2 ? tone->accent : COL[i];
        /* a clearing in the orbit's line, then the planet lit from the sun: the half toward it bright, the far half
         * in shade */
        hpx_disc(&c, x, y, s + 4, 0, tone->ground, 255);
        if (i == 5) hpx_ring(&c, x, y, s * 1.8f, 2.4f, 0, mixc(col, tone->ground, 0.3f), 255);
        uint32_t shade = mixc(col, dark ? 0x000000 : tone->ground, dark ? 0.66f : 0.5f);
        hpx_disc(&c, x, y, s, 0, shade, 255);
        float ux = cx - x, uy = cy - y, ul = sqrtf(ux * ux + uy * uy);
        if (ul > 0) {
            ux /= ul;
            uy /= ul;
        }
        for (int yy = (int)(y - s - 2); yy <= (int)(y + s + 2); yy++)
            for (int xx = (int)(x - s - 2); xx <= (int)(x + s + 2); xx++) {
                if (xx < 0 || yy < 0 || xx >= S || yy >= S) continue;
                float fx = xx + 0.5f - x, fy = yy + 0.5f - y;
                float d = sqrtf(fx * fx + fy * fy);
                float cov = clampf(s - d + 0.5f, 0, 1);
                if (cov <= 0) continue;
                /* the terminator of a sphere lit from the side: the lit half's edge is an ellipse, here softened */
                float k = (fx * ux + fy * uy) / s;
                float lit = smooth(-0.3f, 0.3f, k);
                if (lit <= 0) continue;
                hpx_put(&c, xx, yy, col, (int)(cov * lit * 255));
            }
        if (i == 2) {
            /* the moon beside the earth */
            float mx = x + 15, my = y - 11;
            hpx_disc(&c, mx, my, 3, 0, dark ? 0xD8D8D4 : 0x9A9EA4, 255);
        }
    }
}

/* ================================================================ dials */

void hs_chrono_sub(float R, int i, float *dx, float *dy, float *r)
{
    float off = R * 0.47f;
    *r = R * 0.235f;
    *dx = i == 0 ? -off : i == 1 ? off : 0;
    *dy = i == 2 ? off : 0;
}

/* a scale of n ticks round (cx, cy) at radius r (outer end), `len` long, every `major`-th longer and heavier */
static int ticks(hs_tick_t *out, int k, int max, float cx, float cy, float r, int n, int major, float len, float w)
{
    for (int i = 0; i < n && k < max; i++) {
        bool M = major > 0 && i % major == 0;
        float a = 2 * PI_F * i / n;
        float s = sinf(a), c = -cosf(a);
        float r1 = r, r0 = r - (M ? len * 1.9f : len);
        out[k++] = (hs_tick_t){ cx + s * r0, cy + c * r0, cx + s * r1, cy + c * r1, M ? w * 1.7f : w, M };
    }
    return k;
}

int hs_dial_ticks(float R, bool chrono, hs_tick_t *out, int max)
{
    float fr = R - 7;
    int k = 0;
    if (!chrono) return ticks(out, k, max, 0, 0, fr - 16, 60, 5, 12, 2.4f);
    k = ticks(out, k, max, 0, 0, fr - 10, 240, 0, 5, 1.1f);
    k = ticks(out, k, max, 0, 0, fr - 10, 60, 5, 10, 2.2f);
    for (int i = 0; i < 3; i++) {
        float dx, dy, r;
        hs_chrono_sub(R, i, &dx, &dy, &r);
        k = ticks(out, k, max, dx, dy, r - 5, i == 1 ? 50 : 60, i == 1 ? 10 : 15, 5, 1.2f);
    }
    return k;
}

uint32_t hs_dial_face(const hs_tone_t *tone) { return tone->dark ? mixc(tone->surface1, tone->ground, 0.25f) : 0xFBFBFA; }

void hs_dial(uint16_t *px, int S, float R, bool chrono, const hs_tone_t *tone)
{
    const bool dark = tone->dark;
    fill565(px, (size_t)S * S, tone->ground565);
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    float cx = S * 0.5f, cy = S * 0.5f, k = R / 300.0f; /* sizes are for a 300 px dial, scaled */
    /* the case's shadow on the ground, down and to the right */
    hpx_disc(&c, cx + 6 * k, cy + 12 * k, R + 2 * k, 26 * k, 0x000000, dark ? 150 : 60);
    /* the rim: a ring lit at the top left, in shade at the bottom right */
    uint32_t face = hs_dial_face(tone);
    uint32_t rim_hi = dark ? mixc(face, tone->ink, 0.22f) : 0xFFFFFF;
    uint32_t rim_lo = dark ? mixc(face, 0x000000, 0.55f) : mixc(face, tone->ink, 0.16f);
    hpx_disc(&c, cx, cy, R, 0, rim_lo, 255);
    hpx_disc(&c, cx - 1.5f * k, cy - 1.5f * k, R - 1.5f * k, 3 * k, rim_hi, dark ? 150 : 255);
    /* the face: domed, faintly brighter toward the light, darker near the rim */
    float fr = R - 7 * k;
    uint32_t face_hi = mixc(face, dark ? tone->ink : 0xFFFFFF, dark ? 0.07f : 0.6f);
    uint32_t face_lo = mixc(face, 0x000000, dark ? 0.35f : 0.05f);
    for (int y = (int)(cy - fr - 1); y <= (int)(cy + fr + 1); y++) {
        if (y < 0 || y >= S) continue;
        float fy = (y + 0.5f - cy) / fr;
        for (int x = (int)(cx - fr - 1); x <= (int)(cx + fr + 1); x++) {
            if (x < 0 || x >= S) continue;
            float fx = (x + 0.5f - cx) / fr, d = sqrtf(fx * fx + fy * fy);
            float cov = clampf((1 - d) * fr + 0.5f, 0, 1);
            if (cov <= 0) continue;
            float kk = clampf(0.5f - 0.35f * (fx + fy) * 0.7071f, 0, 1);
            uint32_t col = kk > 0.5f ? mixc(face, face_hi, (kk - 0.5f) * 2) : mixc(face_lo, face, kk * 2);
            col = mixc(col, face_lo, smooth(0.86f, 1.0f, d) * 0.6f);
            hpx_put(&c, x, y, col, (int)(cov * 255));
        }
    }
    if (!chrono) return;
    /* the chronograph's three sub-dials, sunk into the face: a darker well, lit on its lower right edge (the light
     * falls in), fine concentric grooves */
    for (int i = 0; i < 3; i++) {
        float dx, dy, r;
        hs_chrono_sub(R, i, &dx, &dy, &r);
        float sx = cx + dx, sy = cy + dy;
        uint32_t well = mixc(face, dark ? 0x000000 : tone->ink, dark ? 0.32f : 0.05f);
        hpx_disc(&c, sx, sy, r + 2 * k, 1.5f * k, mixc(face, dark ? 0x000000 : tone->ink, dark ? 0.5f : 0.14f), 255);
        hpx_disc(&c, sx + 0.8f * k, sy + 0.8f * k, r + 0.5f * k, 2 * k, mixc(well, dark ? tone->ink : 0xFFFFFF, 0.14f), 255);
        hpx_disc(&c, sx, sy, r, k, well, 255);
        for (float rr = r * 0.25f; rr < r * 0.8f; rr += 3.2f * k)
            hpx_ring(&c, sx, sy, rr, k, 0, mixc(well, tone->ink, dark ? 0.035f : 0.03f), 255);
    }
}

/* ---- the orrery's pieces: the sun and its glow, and each planet as a small sprite with its own alpha ---- */

void hs_sun(uint16_t *px, int S, const hs_tone_t *tone)
{
    const bool dark = tone->dark;
    fill565(px, (size_t)S * S, tone->ground565);
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    float cx = S * 0.5f, cy = S * 0.5f;
    hpx_glow(&c, cx, cy, S * 0.48f, dark ? 0xFF9030 : 0xF7B25A, dark ? 64 : 80);
    hpx_disc(&c, cx, cy, 17, 0, dark ? 0xFFCE70 : 0xF4A63A, 255);
    hpx_disc(&c, cx - 4, cy - 5, 8, 7, 0xFFF6DC, 170);
}

void hs_planet(uint16_t *rgb, uint8_t *alpha, int S, float r, float lx, float ly, uint32_t col, int ring,
               const hs_tone_t *tone)
{
    const bool dark = tone->dark;
    float cx = S * 0.5f, cy = S * 0.5f;
    uint32_t shade = mixc(col, dark ? 0x000000 : tone->ground, dark ? 0.66f : 0.5f);
    uint32_t ringc = mixc(col, tone->ground, 0.3f);
    float ll = sqrtf(lx * lx + ly * ly);
    if (ll > 0) {
        lx /= ll;
        ly /= ll;
    }
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float fx = x + 0.5f - cx, fy = y + 0.5f - cy, d = sqrtf(fx * fx + fy * fy);
            float cov = clampf(r - d + 0.5f, 0, 1);
            uint32_t c = shade;
            float a = cov;
            if (cov > 0) {
                /* the lit half toward the sun, softened across the terminator */
                float kk = (fx * lx + fy * ly) / r;
                c = mixc(shade, col, smooth(-0.3f, 0.3f, kk));
            }
            if (ring) {
                /* Saturn's ring, seen from above the ecliptic: a thin circle round it */
                float rd = fabsf(d - r * 1.8f);
                float rc = clampf(1.2f - rd, 0, 1) * 0.9f;
                if (rc > a) {
                    c = ringc;
                    a = rc;
                }
            }
            rgb[y * S + x] = hpx_to565(c);
            alpha[y * S + x] = (uint8_t)(a * 255);
        }
}

/* ================================================================ a soft shadow */

void hs_shadow(uint16_t *px, int w, int h, float r, int pad, float dx, float dy, float blur, float strength,
               const hs_tone_t *tone)
{
    int W = w + 2 * pad, H = h + 2 * pad;
    fill565(px, (size_t)W * H, tone->ground565);
    hpx_t c;
    hpx_init16(&c, px, W, H, W);
    uint32_t sh = tone->dark ? 0x000000 : hpx_mix(tone->ground, 0x1A2030, 200);
    hpx_rrect(&c, pad + dx, pad + dy, (float)w, (float)h, r, blur, sh, (int)(strength * 255));
}

/* ================================================================ the world */

void hs_world_xy(int w, int h, double lat0, double lat1, double lat, double lon, float *x, float *y)
{
    *x = (float)((lon + 180.0) / 360.0 * w);
    *y = (float)((lat1 - lat) / (lat1 - lat0) * h);
}
