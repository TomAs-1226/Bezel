/* The sky faces: the sky as it is now, tonight's moon and the planets today — each drawn from where things really
 * are (home_astro.c) for where the tablet is (the weather's place).
 *
 *   sky      the sky's colour from the sun's real altitude and the weather, the sun on today's real path across
 *            it (the part it has travelled solid, the rest dotted, below the horizon at night), stars after dark
 *   moon     the moon at its real phase, lit from the side the sun is on, with its next phase in words
 *   orrery   the eight planets on their orbits where they are today, the earth in the accent, each lit from the sun
 *
 * Their pictures: the sky is a strip 64 px wide tiled across the screen (92 KB, painted again every ten minutes),
 * the moon one 560 px picture (an hour), the orrery one 640 px picture (a day). */
#include "ui_home_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RADF (3.14159265f / 180.0f)

/* ink for text over a picture: light over a dark sky, the tone's own over a light one */
static void inks_for(uint32_t under, uint32_t *ink, uint32_t *dim)
{
    if (hpx_luma(under) < 120) {
        *ink = 0xF2F4F7;
        *dim = 0xB4BCC7;
    } else {
        *ink = 0x15181B;
        *dim = 0x4A525B;
    }
}

static const char *compass(double az)
{
    static const char *const P[8] = { "north", "northeast", "east", "southeast", "south", "southwest", "west",
                                       "northwest" };
    int i = (int)floor(fmod(az + 22.5 + 360.0, 360.0) / 45.0);
    return P[i & 7];
}

static void span_words(double s, char *out, size_t n)
{
    int m = (int)(s / 60 + 0.5);
    if (m < 60) snprintf(out, n, "%d min", m);
    else snprintf(out, n, "%d h %d min", m / 60, m % 60);
}

/* ================================================================ the sky */

#define SK_HOR 580             /* the horizon's row */
#define SK_X0 56
#define SK_X1 1224
#define SK_DEG 3.9f            /* pixels of height per degree of altitude */
#define SK_PTS 97              /* the day's path, every 15 minutes */
#define SK_STARS 44

static struct {
    hm_pic_t strip;
    hs_sky_t sky;
    lv_obj_t *done, *rest, *sun, *below, *stars[SK_STARS];
    lv_point_precise_t p_done[SK_PTS + 2], p_rest[SK_PTS + 2];
    lv_obj_t *time, *date, *line, *wx, *caption;
    int slot;                  /* the ten minutes the strip and the path were drawn for */
    float land_top, land_bot;
    uint32_t text_under;
} SK;

static void sky_paint(void *arg)
{
    hm_pic_t *p = arg;
    if (!p->px) return;
    uint32_t lt = p->tone.dark ? 0x0E1013 : 0xC9CDD2, lb = p->tone.dark ? 0x060708 : 0xB9BEC4;
    /* the land takes a little of the sky's light */
    lt = hpx_mix(lt, SK.sky.horizon, p->tone.dark ? 30 : 40);
    hs_land_strip(p->px, p->w, p->h, SK_HOR, &SK.sky, lt, lb);
}

static void sky_build(lv_obj_t *b)
{
    hm_pic_init(&SK.strip, b, 0, 0, 64, H);
    lv_obj_set_size(SK.strip.img, W, H);
    lv_image_set_inner_align(SK.strip.img, LV_IMAGE_ALIGN_TILE);
    for (int i = 0; i < SK_STARS; i++) {
        lv_obj_t *s = bz_box(b);
        /* scattered over the sky where the words aren't, the same every night */
        uint32_t h = hpx_hash(0x57A2u + (uint32_t)i * 7919u);
        int x, y;
        for (int k = 0;; k++) {
            x = 40 + (int)(h % 1200);
            y = 20 + (int)((h >> 12) % (SK_HOR - 90));
            if (!(x < 640 && y < 300) || k > 6) break;
            h = hpx_hash(h);
        }
        int d = (h >> 24) % 5 == 0 ? 3 : 2;
        lv_obj_set_size(s, d, d);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, bz_lv_rgb(0xF2F4FF), 0);
        lv_obj_set_pos(s, x, y);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
        SK.stars[i] = s;
    }
    SK.rest = lv_line_create(b);
    SK.done = lv_line_create(b);
    lv_obj_t *ln[2] = { SK.rest, SK.done };
    for (int i = 0; i < 2; i++) {
        lv_obj_remove_style_all(ln[i]);
        lv_obj_add_flag(ln[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_line_width(ln[i], i ? 3 : 2, 0);
        lv_obj_set_style_line_rounded(ln[i], true, 0);
    }
    lv_obj_set_style_line_dash_width(SK.rest, 3, 0);
    lv_obj_set_style_line_dash_gap(SK.rest, 9, 0);
    SK.below = bz_box(b);
    lv_obj_set_size(SK.below, 22, 22);
    lv_obj_set_style_radius(SK.below, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(SK.below, 2, 0);
    lv_obj_add_flag(SK.below, LV_OBJ_FLAG_HIDDEN);
    SK.sun = bz_box(b);
    lv_obj_set_size(SK.sun, 34, 34);
    lv_obj_set_style_radius(SK.sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(SK.sun, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(SK.sun, 70, 0);
    lv_obj_set_style_shadow_spread(SK.sun, 8, 0);
    SK.time = lv_label_create(b);
    ui_face_font_bind(SK.time, UI_FF_THIN);
    SK.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_INK, 560);
    SK.line = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 640);
    SK.wx = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 360);
    lv_obj_set_style_text_align(SK.wx, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(SK.wx, SK_X1 - 360 - 60, 44);
    SK.caption = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 900);
    lv_obj_set_pos(SK.caption, SK_X0, SK_HOR + 30);
    SK.slot = -1;
}

static float sky_y(double elev) { return SK_HOR - (float)(elev > 0 ? elev : 0) * SK_DEG; }
static float sky_x(double t, double day0) { return SK_X0 + (float)((t - day0) / 86400.0) * (SK_X1 - SK_X0); }

/* the sun's path between two times, as points: its ends exactly, every quarter hour between */
static int sky_arc(lv_point_precise_t *out, double t0, double t1, double day0, double lat, double lon)
{
    int n = 0;
    if (t1 - t0 < 60) return 0;
    for (double t = t0;; t = floor(t / 900.0 + 1) * 900.0) {
        if (t > t1) t = t1;
        astro_sun_t s;
        astro_sun(t, lat, lon, &s);
        out[n++] = (lv_point_precise_t){ sky_x(t, day0), sky_y(s.elev) };
        if (t >= t1 || n >= SK_PTS + 1) break;
    }
    return n;
}

static void sky_minute(const struct tm *tm, bool force)
{
    double lat, lon, now = (double)time(NULL), tz = hm_tz_s();
    hm_where(&lat, &lon);
    astro_sun_t sun;
    astro_sun(now, lat, lon, &sun);
    home_weather_t w;
    home_weather_get(&w);
    int slot = tm->tm_hour * 6 + tm->tm_min / 10;
    bool dark = bz_ui_dark();
    if (slot != SK.slot || force) {
        SK.slot = slot;
        hs_sky(sun.elev, w.ok ? w.code : -1, dark, &SK.sky);
        hm_tone(&SK.strip.tone);
        hm_pic_start(&SK.strip, sky_paint, &SK.strip);
        /* today's path: midnight to midnight across the screen, the altitude up; solid from sunrise to where the
         * sun is now, dotted on to sunset */
        double day0 = floor((now + tz) / 86400.0) * 86400.0 - tz;
        double rise, set, noon;
        int nd = 0, nr = 0;
        if (astro_sun_day(now, lat, lon, tz, &rise, &set, &noon)) {
            double mid = now < rise ? rise : now > set ? set : now;
            nd = sky_arc(SK.p_done, rise, mid, day0, lat, lon);
            nr = sky_arc(SK.p_rest, mid, set, day0, lat, lon);
        }
        lv_line_set_points(SK.done, SK.p_done, (uint32_t)(nd > 1 ? nd : 0));
        lv_line_set_points(SK.rest, SK.p_rest, (uint32_t)(nr > 1 ? nr : 0));
        hm_show(SK.done, nd > 1);
        hm_show(SK.rest, nr > 1);
        float sx = sky_x(now, day0), sy = sky_y(sun.elev);
        uint32_t path = hpx_luma(SK.sky.mid) < 120 ? 0xF2F4F7 : 0x15181B;
        lv_obj_set_style_line_color(SK.done, bz_lv_rgb(path), 0);
        lv_obj_set_style_line_opa(SK.done, 150, 0);
        lv_obj_set_style_line_color(SK.rest, bz_lv_rgb(path), 0);
        lv_obj_set_style_line_opa(SK.rest, 90, 0);
        /* the sun, or where it is below the horizon */
        bool up = sun.elev > -0.3;
        hm_show(SK.sun, up);
        hm_show(SK.below, !up);
        uint32_t warm = hpx_mix(0xFFF3D6, 0xFFB060, (int)(256 * (1 - fmin(1, fmax(0, sun.elev / 12.0)))));
        lv_obj_set_style_bg_color(SK.sun, bz_lv_rgb(warm), 0);
        lv_obj_set_style_shadow_color(SK.sun, bz_lv_rgb(hpx_mix(warm, 0xFF8A30, 120)), 0);
        lv_obj_set_style_shadow_opa(SK.sun, dark ? 110 : 150, 0);
        hm_pos(SK.sun, (int)lroundf(sx) - 17, (int)lroundf(sy) - 17);
        float by = SK_HOR + fminf(-(float)sun.elev * 3.0f, 70);
        lv_obj_set_style_border_color(SK.below, bz_lv_rgb(0xB4BCC7), 0);
        lv_obj_set_style_border_opa(SK.below, 110, 0);
        hm_pos(SK.below, (int)lroundf(sx) - 11, (int)lroundf(by) - 11);
        /* stars after dark */
        int st = (int)(SK.sky.stars * 230);
        for (int i = 0; i < SK_STARS; i++) {
            bool on = st > 12;
            hm_show(SK.stars[i], on);
            if (on) lv_obj_set_style_bg_opa(SK.stars[i], (lv_opa_t)(st * (40 + (i * 37) % 60) / 100), 0);
        }
        /* the words' ink, from the sky under them */
        uint32_t ink, dim;
        inks_for(hs_sky_at(&SK.sky, 0.25f), &ink, &dim);
        hm_color(SK.time, ink);
        hm_color(SK.date, ink);
        hm_color(SK.line, dim);
        hm_color(SK.wx, dim);
        uint32_t lt = dark ? 0x0E1013 : 0xC9CDD2;
        inks_for(lt, &ink, &dim);
        hm_color(SK.caption, dim);
    }
    char t[8], d[48];
    hm_hm(tm, t, sizeof t);
    ui_text(SK.time, "%s", t);
    hm_baseline(SK.time, SK_X0 - 6, 206);
    hm_date(tm, d, sizeof d, false);
    ui_text(SK.date, "%s", d);
    lv_obj_set_pos(SK.date, SK_X0, 226);
    lv_obj_set_pos(SK.line, SK_X0, 264);
    double rise, set, noon;
    char a[16], b[24];
    if (astro_sun_day(now, lat, lon, tz, &rise, &set, &noon)) {
        if (now < rise) {
            hm_hm_t((time_t)rise, a, sizeof a);
            span_words(rise - now, b, sizeof b);
            ui_text(SK.line, "sunrise %s \xc2\xb7 in %s", a, b);
        } else if (now < set) {
            hm_hm_t((time_t)set, a, sizeof a);
            span_words(set - now, b, sizeof b);
            ui_text(SK.line, "sunset %s \xc2\xb7 %s of daylight left", a, b);
        } else {
            double r2, s2, n2;
            if (astro_sun_day(now + 86400, lat, lon, tz, &r2, &s2, &n2)) {
                hm_hm_t((time_t)r2, a, sizeof a);
                span_words(r2 - now, b, sizeof b);
                ui_text(SK.line, "sunrise %s \xc2\xb7 in %s", a, b);
            }
        }
        span_words(set - rise, b, sizeof b);
        if (sun.elev > 0)
            ui_text(SK.caption, "the sun is %.0f\xc2\xb0 up in the %s \xc2\xb7 %s of daylight today", sun.elev,
                    compass(sun.azim), b);
        else if (sun.elev > -6)
            ui_text(SK.caption, "twilight \xc2\xb7 the sun is %.0f\xc2\xb0 below the %s horizon", -sun.elev,
                    compass(sun.azim));
        else
            ui_text(SK.caption, "night \xc2\xb7 the sun is %.0f\xc2\xb0 below the horizon \xc2\xb7 %s of daylight today",
                    -sun.elev, b);
    } else {
        ui_text(SK.line, "%s", sun.elev > 0 ? "the sun doesn't set today" : "the sun doesn't rise today");
        ui_text(SK.caption, "%s", "");
    }
    char wx[48];
    hm_weather_line(wx, sizeof wx);
    ui_text(SK.wx, "%s", wx);
}

static void sky_tick(void) { hm_pic_poll(&SK.strip); }
static bool sky_ready(void) { return !hm_pic_busy(&SK.strip); }
static void sky_leave(void)
{
    if (hm_pic_free(&SK.strip)) SK.slot = -1;
}
static bool sky_held(void) { return hm_pic_held(&SK.strip); }

const hm_face_t HM_F_SKY = { .name = "sky", .fonts = UI_FF_BIT(UI_FF_THIN), .build = sky_build, .minute = sky_minute,
                             .tick = sky_tick, .ready = sky_ready, .leave = sky_leave, .held = sky_held };

/* ================================================================ the moon */

#define MN_S 480
#define MN_R 226
#define MN_X 760
#define MN_Y 120

static struct {
    hm_pic_t pic;
    double phase;
    bool south;
    lv_obj_t *time, *date, *name, *lit, *rise, *next;
    int hour;                  /* the hour it was painted for */
} MN;

static void moon_paint(void *arg)
{
    (void)arg;
    if (!MN.pic.px) return;
    hs_moon(MN.pic.px, MN_S, MN_R, MN.phase, MN.south, &MN.pic.tone);
}

static void moon_build(lv_obj_t *b)
{
    hm_pic_init(&MN.pic, b, MN_X, MN_Y, MN_S, MN_S);
    MN.time = lv_label_create(b);
    ui_face_font_bind(MN.time, UI_FF_THIN);
    MN.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_DIM, 560);
    lv_obj_set_pos(MN.date, 56, 290);
    MN.name = bz_label_line(b, "", BZ_F_TITLE, BZ_C_INK, 580);
    lv_obj_set_pos(MN.name, 56, 380);
    MN.lit = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 580);
    lv_obj_set_pos(MN.lit, 56, 442);
    MN.next = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 580);
    lv_obj_set_pos(MN.next, 56, 472);
    MN.rise = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 580);
    lv_obj_set_pos(MN.rise, 56, 502);
    MN.hour = -1;
}

static const char *phase_name(double ph)
{
    static const struct { double lim; const char *name; } P[] = {
        { 0.02, "new moon" }, { 0.23, "waxing crescent" }, { 0.27, "first quarter" }, { 0.48, "waxing gibbous" },
        { 0.52, "full moon" }, { 0.73, "waning gibbous" }, { 0.77, "last quarter" }, { 0.98, "waning crescent" },
        { 1.01, "new moon" },
    };
    for (size_t i = 0; i < sizeof P / sizeof *P; i++)
        if (ph < P[i].lim) return P[i].name;
    return "moon";
}

static void when_words(double t, char *out, size_t n)
{
    double now = (double)time(NULL);
    time_t tt = (time_t)t;
    struct tm tm, today;
    localtime_r(&tt, &tm);
    time_t nn = (time_t)now;
    localtime_r(&nn, &today);
    int days = (int)floor((t - now) / 86400.0 + 0.5);
    if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) snprintf(out, n, "today");
    else if (days <= 1 && (tm.tm_yday == (today.tm_yday + 1) % 366)) snprintf(out, n, "tomorrow");
    else if (days < 7) {
        strftime(out, n, "on %A", &tm);
        for (char *p = out; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    } else {
        strftime(out, n, "on %e %B", &tm);
        for (char *p = out; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        char *dd = strstr(out, "  ");
        if (dd) memmove(dd, dd + 1, strlen(dd));
    }
}

static void moon_minute(const struct tm *tm, bool force)
{
    double now = (double)time(NULL);
    astro_moon_t m;
    astro_moon(now, &m);
    double lat, lon;
    hm_where(&lat, &lon);
    if (force) {
        uint32_t ink = bz_color(BZ_C_INK);
        hm_color(MN.time, ink);
    }
    if (tm->tm_hour != MN.hour || force) {
        MN.hour = tm->tm_hour;
        MN.phase = m.phase;
        MN.south = lat < 0;
        hm_tone(&MN.pic.tone);
        hm_pic_start(&MN.pic, moon_paint, NULL);
    }
    char t[8], d[48];
    hm_hm(tm, t, sizeof t);
    ui_text(MN.time, "%s", t);
    hm_baseline(MN.time, 50, 262);
    hm_date(tm, d, sizeof d, false);
    ui_text(MN.date, "%s", d);
    ui_text(MN.name, "%s", phase_name(m.phase));
    ui_text(MN.lit, "%d%% lit \xc2\xb7 %.0f km away", (int)lround(m.lit * 100), m.dist_km);
    /* the next of the four phases */
    static const char *const Q[4] = { "new moon", "first quarter", "full moon", "last quarter" };
    double best = 1e18;
    int bq = 0;
    for (int q = 0; q < 4; q++) {
        double at = astro_moon_next(now + 3600, q * 0.25);
        if (at > now && at < best) {
            best = at;
            bq = q;
        }
    }
    char w[40];
    when_words(best, w, sizeof w);
    ui_text(MN.next, "%s %s", Q[bq], w);
    /* today's moonrise and moonset here, whichever the day has */
    double mr, ms, alt, az;
    astro_moon_day(now, lat, lon, hm_tz_s(), &mr, &ms);
    astro_moon_altaz(now, lat, lon, &alt, &az);
    char a[12] = "", b[12] = "";
    if (mr > 0) hm_hm_t((time_t)mr, a, sizeof a);
    if (ms > 0) hm_hm_t((time_t)ms, b, sizeof b);
    const char *up = alt > 0 ? "up now" : "below the horizon";
    if (mr > 0 && ms > 0)
        ui_text(MN.rise, "%s \xc2\xb7 %s %s \xc2\xb7 %s %s", up, mr < ms ? "rises" : "sets", mr < ms ? a : b,
                mr < ms ? "sets" : "rises", mr < ms ? b : a);
    else if (mr > 0) ui_text(MN.rise, "%s \xc2\xb7 rises %s", up, a);
    else if (ms > 0) ui_text(MN.rise, "%s \xc2\xb7 sets %s", up, b);
    else ui_text(MN.rise, "%s", up);
}

static void moon_tick(void) { hm_pic_poll(&MN.pic); }
static bool moon_ready(void) { return !hm_pic_busy(&MN.pic); }
static void moon_leave(void)
{
    if (hm_pic_free(&MN.pic)) MN.hour = -1;
}
static bool moon_held(void) { return hm_pic_held(&MN.pic); }

const hm_face_t HM_F_MOON = { .name = "moon", .fonts = UI_FF_BIT(UI_FF_THIN), .build = moon_build,
                              .minute = moon_minute, .tick = moon_tick, .ready = moon_ready, .leave = moon_leave,
                              .held = moon_held };

/* ================================================================ the planets */

/* The orrery is drawn in pieces rather than as one picture: the orbits are LVGL rings (a line each, redrawn only
 * where something changes over them), the sun and its glow one small picture (a 220 px square, 97 KB), and each
 * planet a sprite with its own alpha (a few KB) lit from the sun's side for where it stands today. */

#define OR_S 640               /* the square the orbits fit in */
#define OR_X 596
#define OR_Y 40
#define OR_SUN 220
#define OR_SPR 48              /* a planet's sprite */

static struct {
    hm_pic_t sun;
    uint8_t *spr;              /* eight sprites, each an RGB565 plane then an A8 plane */
    lv_image_dsc_t spr_dsc[ASTRO_PLANETS];
    unsigned spr_ticket;
    bool spr_ready;
    float lx[ASTRO_PLANETS], ly[ASTRO_PLANETS], size[ASTRO_PLANETS];
    uint32_t col[ASTRO_PLANETS];
    hs_tone_t tone;
    lv_obj_t *orbit[ASTRO_PLANETS], *clear[ASTRO_PLANETS], *planet[ASTRO_PLANETS], *moon, *name[ASTRO_PLANETS];
    lv_obj_t *time, *date, *line;
    int day;
} OR;

static void sun_paint(void *arg)
{
    hm_pic_t *p = arg;
    if (!p->px) return;
    hs_sun(p->px, OR_SUN, &p->tone);
}

static void sprites_paint(void *arg)
{
    (void)arg;
    if (!OR.spr) return;
    size_t one = (size_t)OR_SPR * OR_SPR * 3;
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        uint8_t *base = OR.spr + one * (size_t)i;
        hs_planet((uint16_t *)base, base + OR_SPR * OR_SPR * 2, OR_SPR, OR.size[i], OR.lx[i], OR.ly[i], OR.col[i],
                  i == 5, &OR.tone);
    }
}

static void orrery_build(lv_obj_t *b)
{
    hm_pic_init(&OR.sun, b, OR_X + (OR_S - OR_SUN) / 2, OR_Y + (OR_S - OR_SUN) / 2, OR_SUN, OR_SUN);
    float orbit[ASTRO_PLANETS], size[ASTRO_PLANETS];
    hs_orrery_layout(OR_S, 0, NULL, orbit, size);
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        lv_obj_t *o = bz_box(b);
        int r = (int)lroundf(orbit[i]);
        lv_obj_set_size(o, 2 * r + 1, 2 * r + 1);
        lv_obj_set_pos(o, OR_X + OR_S / 2 - r, OR_Y + OR_S / 2 - r);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(o, i == 2 ? 2 : 1, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
        OR.orbit[i] = o;
        OR.size[i] = size[i];
    }
    for (int i = 0; i < ASTRO_PLANETS; i++) {
        /* a clearing in the orbit's line round each planet, then the planet */
        lv_obj_t *c = bz_box(b);
        int d = (int)(size[i] * 2 + 8);
        lv_obj_set_size(c, d, d);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        OR.clear[i] = c;
        lv_obj_t *p = lv_image_create(b);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
        OR.planet[i] = p;
        lv_image_dsc_t *ds = &OR.spr_dsc[i];
        ds->header.magic = LV_IMAGE_HEADER_MAGIC;
        ds->header.cf = LV_COLOR_FORMAT_RGB565A8;
        ds->header.w = OR_SPR;
        ds->header.h = OR_SPR;
        ds->header.stride = OR_SPR * 2;
        ds->data_size = OR_SPR * OR_SPR * 3;
        OR.name[i] = bz_label(b, ASTRO_PLANET_NAME[i], BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_add_flag(OR.name[i], LV_OBJ_FLAG_HIDDEN);
    }
    OR.moon = bz_box(b);
    lv_obj_set_size(OR.moon, 6, 6);
    lv_obj_set_style_radius(OR.moon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(OR.moon, LV_OPA_COVER, 0);
    lv_obj_add_flag(OR.moon, LV_OBJ_FLAG_HIDDEN);
    OR.time = lv_label_create(b);
    ui_face_font_bind(OR.time, UI_FF_THIN);
    OR.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_DIM, 520);
    lv_obj_set_pos(OR.date, 56, 290);
    OR.line = bz_label(b, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_width(OR.line, 500);
    lv_obj_set_pos(OR.line, 56, 380);
    OR.day = -1;
}

static void orrery_minute(const struct tm *tm, bool force)
{
    double now = (double)time(NULL);
    bool dark = bz_ui_dark();
    if (force) {
        hm_color(OR.time, bz_color(BZ_C_INK));
        uint32_t g = bz_color(BZ_C_GROUND), ink = bz_color(BZ_C_INK), acc = bz_color(dark ? BZ_C_ICE : BZ_C_ON_ICE);
        uint32_t line = hpx_mix(g, ink, dark ? 34 : 46);
        for (int i = 0; i < ASTRO_PLANETS; i++) {
            lv_obj_set_style_border_color(OR.orbit[i], bz_lv_rgb(i == 2 ? hpx_mix(line, acc, 140) : line), 0);
            lv_obj_set_style_bg_color(OR.clear[i], bz_lv_rgb(g), 0);
        }
        lv_obj_set_style_bg_color(OR.moon, bz_lv_rgb(dark ? 0xD8D8D4 : 0x9A9EA4), 0);
    }
    if (tm->tm_yday != OR.day || force) {
        OR.day = tm->tm_yday;
        hm_tone(&OR.sun.tone);
        hm_pic_start(&OR.sun, sun_paint, &OR.sun);
        OR.tone = OR.sun.tone;
        float xy[ASTRO_PLANETS][2], orbit[ASTRO_PLANETS], size[ASTRO_PLANETS];
        hs_orrery_layout(OR_S, now, xy, orbit, size);
        for (int i = 0; i < ASTRO_PLANETS; i++) {
            float x = OR_X + xy[i][0], y = OR_Y + xy[i][1];
            /* lit from the sun */
            OR.lx[i] = OR_S / 2.0f - xy[i][0];
            OR.ly[i] = OR_S / 2.0f - xy[i][1];
            OR.col[i] = i == 2 ? OR.tone.accent : HS_PLANET_COL[i];
            int d = (int)(size[i] * 2 + 8);
            hm_pos(OR.clear[i], (int)lroundf(x) - d / 2, (int)lroundf(y) - d / 2);
            hm_show(OR.clear[i], true);
            hm_pos(OR.planet[i], (int)lroundf(x) - OR_SPR / 2, (int)lroundf(y) - OR_SPR / 2);
            /* the names beside them, outward from the sun */
            float dx = xy[i][0] - OR_S / 2.0f, dy = xy[i][1] - OR_S / 2.0f, l = sqrtf(dx * dx + dy * dy);
            if (l < 1) l = 1;
            int w = hm_text_w(ASTRO_PLANET_NAME[i], bz_font(BZ_F_CAPTION));
            int hh = lv_font_get_line_height(bz_font(BZ_F_CAPTION));
            float off = size[i] * (i == 5 ? 1.9f : 1) + 10;
            float lx = x + dx / l * off, ly = y + dy / l * off;
            int nx = (int)lx - (dx < -l * 0.3f ? w : dx > l * 0.3f ? 0 : w / 2);
            int ny = (int)ly - (dy < -l * 0.3f ? hh : dy > l * 0.3f ? 0 : hh / 2);
            hm_pos(OR.name[i], nx, ny);
            hm_show(OR.name[i], true);
            hm_color(OR.name[i], i == 2 ? bz_color(BZ_C_INK) : bz_color(BZ_C_DIM));
            if (i == 2) {
                hm_pos(OR.moon, (int)lroundf(x + 16) - 3, (int)lroundf(y - 12) - 3);
                hm_show(OR.moon, true);
            }
        }
        if (!OR.spr) OR.spr = malloc((size_t)OR_SPR * OR_SPR * 3 * ASTRO_PLANETS);
        if (OR.spr) {
            OR.spr_ready = false;
            OR.spr_ticket = hm_art_job(sprites_paint, NULL);
        }
    }
    char t[8], d[48];
    hm_hm(tm, t, sizeof t);
    ui_text(OR.time, "%s", t);
    hm_baseline(OR.time, 50, 262);
    hm_date(tm, d, sizeof d, false);
    ui_text(OR.date, "%s", d);
    /* the earth's place: how far round since the march equinox, and how far mars is */
    double elon, er, mlon, mr;
    astro_planet(2, now, &elon, &er);
    astro_planet(3, now, &mlon, &mr);
    double dm = sqrt(er * er + mr * mr - 2 * er * mr * cos((elon - mlon) * RADF));
    ui_text(OR.line, "the planets where they are today.\nthe earth is %.0f\xc2\xb0 round the sun from the march "
            "equinox; mars is %.2f au from us.", fmod(elon + 180.0, 360.0), dm);
}

static void orrery_tick(void)
{
    hm_pic_poll(&OR.sun);
    if (OR.spr && !OR.spr_ready && hm_art_job_done(OR.spr_ticket)) {
        OR.spr_ready = true;
        size_t one = (size_t)OR_SPR * OR_SPR * 3;
        for (int i = 0; i < ASTRO_PLANETS; i++) {
            OR.spr_dsc[i].data = OR.spr + one * (size_t)i;
            lv_image_cache_drop(&OR.spr_dsc[i]);
            lv_image_set_src(OR.planet[i], &OR.spr_dsc[i]);
            lv_obj_invalidate(OR.planet[i]);
            hm_show(OR.planet[i], true);
        }
    }
}

static bool orrery_ready(void) { return !hm_pic_busy(&OR.sun) && (!OR.spr || hm_art_job_done(OR.spr_ticket)); }

static void orrery_leave(void)
{
    bool a = hm_pic_free(&OR.sun);
    if (OR.spr && hm_art_job_done(OR.spr_ticket)) {
        for (int i = 0; i < ASTRO_PLANETS; i++) {
            hm_show(OR.planet[i], false);
            lv_image_cache_drop(&OR.spr_dsc[i]);
        }
        free(OR.spr);
        OR.spr = NULL;
        OR.spr_ready = false;
    }
    if (a && !OR.spr) OR.day = -1;
}

static bool orrery_held(void) { return hm_pic_held(&OR.sun) || OR.spr; }

const hm_face_t HM_F_ORRERY = { .name = "orrery", .fonts = UI_FF_BIT(UI_FF_THIN), .build = orrery_build,
                                .minute = orrery_minute, .tick = orrery_tick, .ready = orrery_ready,
                                .leave = orrery_leave, .held = orrery_held };
