/* The tree face: Bezel's low-poly tree (home_tree.c), grown by the team's season and dressed by the world outside.
 *
 * What it shows, and says it shows (the caption at the lower left):
 *   growth      a branch for every match the team played this season (the blue alliance's statuses of every event,
 *               tba_season); a sapling before the first, the whole tree after about fifty. Without the blue
 *               alliance it grows with the year instead, and says so.
 *   fruit       one for every match won.
 *   the crown   the month (bare in winter, blossom in spring, gold and red in autumn), a month earlier or later
 *               when it is unusually warm or cold; the southern hemisphere's months turned round.
 *   light       from where the sun really is: it lights the tree's facets from that side and throws its shadow the
 *               other way; after dark the tree is lit faintly by the moon's cool light.
 *   the sky     its colour from the sun's altitude and the weather, as the sky face's.
 *   wind        the tree leans with the live wind speed, and in autumn a few leaves blow off it.
 *   rain, snow  fall past it while it rains or snows.
 *
 * Cost: two pictures, the sky strip (92 KB, tiled) and the tree's own (600 × 620, 744 KB) painted on the covers'
 * worker in ~150 ms when what they show changes (a match, the month, the light every ten minutes); the mesh
 * (~150 KB) lives only while the face shows. The leaves, drops and flakes are a dozen or two small objects
 * moving at 30 fps, each redrawing only its own few pixels. */
#include "ui_home_face.h"

#include "home_tree.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TR_HOR 612             /* the horizon's row */
#define TP_X 640               /* the tree's picture, placed on the dither's grid */
#define TP_Y 108
#define TP_W 480
#define TP_H (720 - TP_Y)
#define TR_NP 26               /* particles at most */
#define TR_STRIPS 12           /* the tree's picture shown as this many strips of rows (a gust bends them) */
#define RADF (3.14159265f / 180.0f)

enum { P_NONE, P_LEAVES, P_RAIN, P_SNOW };

typedef struct {
    lv_obj_t *o;
    float x, y, vx, vy, phase;
    int shown_x, shown_y;
} particle_t;

static struct {
    hm_pic_t strip, pic;
    /* what the worker paints from, set on the UI thread before it's asked */
    hs_sky_t sky;
    uint32_t land_top, land_bot;
    ht_palette_t pal;
    int month, fruit_room;
    bool dark;
    ht_view_t view;
    float shadow_dx, shadow_len, shadow_a;
    float mound_light;
    /* the worker's own */
    ht_mesh_t *mesh;
    int mesh_month, mesh_fruit;
    uint32_t mesh_sig;
    int steps;                 /* the mesh's steps, as the worker last reported them */
    /* the words */
    lv_obj_t *time, *date, *title, *what, *data, *nature;
    uint32_t sig;              /* what the picture was painted for */
    int slot;
    /* weather in motion */
    particle_t p[TR_NP];
    int pmode, np;
    double plast;
    float wind01;
    int frame_skip;
    /* the gusts */
    lv_obj_t *strips[TR_STRIPS];
    lv_image_dsc_t sdsc[TR_STRIPS];
    int soff[TR_STRIPS];
    double gust_at;
} TR;

/* ---- the worker ---- */

static void tree_paint(void *arg)
{
    (void)arg;
    hm_pic_t *p = &TR.pic;
    if (!p->px) return;
    uint32_t msig = (uint32_t)TR.month * 131u + (uint32_t)TR.fruit_room * 7u + (TR.dark ? 1u : 0u) + TR.pal.leaf;
    if (!TR.mesh || msig != TR.mesh_sig) {
        ht_free(TR.mesh);
        TR.mesh = ht_build(TR.month, &TR.pal, TR.fruit_room);
        TR.mesh_sig = msig;
    }
    TR.steps = ht_steps(TR.mesh);
    /* the ground and sky behind it, exactly the strip's rows */
    hs_land_rows(p->px, TP_W, TP_H, TP_Y, 720, TR_HOR, &TR.sky, TR.land_top, TR.land_bot);
    hpx_t c;
    hpx_init16(&c, p->px, TP_W, TP_H, TP_W);
    float cx = TR.view.cx, gy = TR_HOR - TP_Y;
    /* a low rise of ground under it, faceted like the tree and lit from the same side */
    static const float MX[] = { -232, -150, -60, 40, 140, 232 }, MY[] = { 0, -13, -24, -25, -14, 0 };
    for (int i = 0; i < 5; i++) {
        float x0 = cx + MX[i], y0 = gy + MY[i], x1 = cx + MX[i + 1], y1 = gy + MY[i + 1];
        /* a facet facing the light (its slope toward the sun's side) is lighter */
        float slope = (y1 - y0) / (x1 - x0);
        float k = 0.5f - slope * 2.2f * TR.mound_light;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        uint32_t col = hpx_mix(hpx_mix(TR.land_top, 0x000000, 60), hpx_mix(TR.land_top, 0xFFFFFF, TR.dark ? 26 : 60),
                               (int)(k * 256));
        hpx_tri(&c, x0, y0, x1, y1, x1, gy + 3, col, 255);
        hpx_tri(&c, x0, y0, x1, gy + 3, x0, gy + 3, col, 255);
    }
    /* its shadow on the ground, away from the light: an ellipse, soft */
    float sx = cx + TR.shadow_dx * 0.5f, sy = gy + 2, rx = 70 + TR.shadow_len * 0.5f, ry = 14;
    for (int y = (int)(sy - ry - 3); y <= (int)(sy + ry + 3); y++)
        for (int x = (int)(sx - rx - 3); x <= (int)(sx + rx + 3); x++) {
            if (x < 0 || y < 0 || x >= TP_W || y >= TP_H) continue;
            float dx = (x + 0.5f - sx) / rx, dy = (y + 0.5f - sy) / ry, d = sqrtf(dx * dx + dy * dy);
            float a = (1 - d) * 2.2f;
            if (a <= 0) continue;
            hpx_put(&c, x, y, 0x000000, (int)(fminf(a, 1) * TR.shadow_a * 255));
        }
    ht_paint(TR.mesh, p->px, TP_W, TP_H, TP_W, &TR.view);
}

static void strip_paint(void *arg)
{
    hm_pic_t *p = arg;
    if (!p->px) return;
    hs_land_strip(p->px, p->w, p->h, TR_HOR, &TR.sky, TR.land_top, TR.land_bot);
}

/* ---- what it shows ---- */

/* the crown's month: the calendar's, turned for the south, nudged a month by an unusually warm or cold day */
static int crown_month(const struct tm *tm, double lat, const home_weather_t *w)
{
    int m = tm->tm_mon;
    if (lat < 0) m = (m + 6) % 12;
    if (w->ok) {
        float c = w->fahrenheit ? (w->temp - 32) / 1.8f : w->temp;
        if ((m == 8 || m == 9) && c < 8) m++;          /* an early autumn */
        else if ((m == 9 || m == 10) && c > 26) m--;   /* a late one */
        else if ((m == 2 || m == 3) && c > 24) m++;    /* an early spring */
        else if ((m == 3 || m == 4) && c < 6) m--;
    }
    return m;
}

static int particles_for(int wmo, int month, float wind01)
{
    if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return P_SNOW;
    if ((wmo >= 51 && wmo <= 67) || (wmo >= 80 && wmo <= 82) || wmo >= 95) return P_RAIN;
    if (month >= 8 && month <= 10 && wind01 > 0.12f) return P_LEAVES;
    return P_NONE;
}

static void tree_build(lv_obj_t *b)
{
    hm_pic_init(&TR.strip, b, 0, 0, 64, H);
    lv_obj_set_size(TR.strip.img, W, H);
    lv_image_set_inner_align(TR.strip.img, LV_IMAGE_ALIGN_TILE);
    hm_pic_init(&TR.pic, b, TP_X, TP_Y, TP_W, TP_H);
    for (int k = 0; k < TR_STRIPS; k++) {
        TR.strips[k] = lv_image_create(b);
        lv_obj_add_flag(TR.strips[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(TR.strips[k], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    for (int i = 0; i < TR_NP; i++) {
        particle_t *p = &TR.p[i];
        p->o = bz_box(b);
        lv_obj_set_style_bg_opa(p->o, LV_OPA_COVER, 0);
        lv_obj_add_flag(p->o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->o, LV_OBJ_FLAG_EVENT_BUBBLE);
        p->shown_x = p->shown_y = -9999;
    }
    TR.time = lv_label_create(b);
    lv_obj_set_style_text_font(TR.time, &hm_font_thin, 0);
    TR.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_INK, 520);
    lv_obj_set_pos(TR.date, 56, 226);
    TR.title = bz_label_line(b, "the season's tree", BZ_F_NAME, BZ_C_INK, 520);
    lv_obj_set_pos(TR.title, 56, 420);
    TR.what = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 540);
    lv_obj_set_pos(TR.what, 56, 458);
    TR.data = bz_label_line(b, "", BZ_F_LABEL, BZ_C_INK, 540);
    lv_obj_set_pos(TR.data, 56, 490);
    TR.nature = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 540);
    lv_obj_set_pos(TR.nature, 56, 520);
    TR.slot = -1;
}

static const char *season_name(int m)
{
    return m <= 1 || m == 11 ? "winter" : m <= 4 ? "spring" : m <= 7 ? "summer" : "autumn";
}

static void tree_minute(const struct tm *tm, bool force)
{
    double now = (double)time(NULL), lat, lon;
    hm_where(&lat, &lon);
    astro_sun_t sun;
    astro_sun(now, lat, lon, &sun);
    home_weather_t w;
    home_weather_get(&w);
    home_forecast_t fc;
    home_weather_forecast(&fc);
    bool dark = bz_ui_dark();
    int month = crown_month(tm, lat, &w);
    /* the season, from the blue alliance: growth by matches played, fruit by matches won */
    tba_season_t se;
    bool season = tba_season(&se) && se.year == tm->tm_year + 1900;
    int steps = TR.steps > 0 ? TR.steps : 126;
    float growth;
    int fruit = 0;
    if (season) {
        growth = 16 + (float)se.played * (steps - 16) / 50.0f;
        fruit = se.wins;
    } else {
        growth = 16 + (float)tm->tm_yday * (steps - 16) / 365.0f;
    }
    if (growth > steps) growth = (float)steps;
    /* the wind: 0 calm .. 1 a gale (50 km/h) */
    float kmh = fc.ok && fc.wind >= 0 ? (w.fahrenheit ? fc.wind * 1.609f : fc.wind) : 0;
    TR.wind01 = kmh / 50.0f > 1 ? 1 : kmh / 50.0f;
    /* the light: the sun's direction in the view (the tree seen from the south, looking north: east is to the
     * right), or the moon's cool light after dark */
    float L[3], amb, el = (float)sun.elev;
    uint32_t tint;
    if (el > -2) {
        float e = (el < 6 ? 6 : el) * RADF, az = (float)sun.azim * RADF;
        L[0] = sinf(az) * cosf(e);
        L[1] = sinf(e);
        L[2] = -cosf(az) * cosf(e);
        amb = 0.46f;
        float warm = el < 18 ? 1 - (el < 0 ? 0 : el) / 18 : 0;
        tint = hpx_mix(0xFFFFFF, 0xFFC08A, (int)(warm * 200));
    } else {
        L[0] = -0.45f;
        L[1] = 0.8f;
        L[2] = 0.4f;
        amb = 0.5f;
        tint = dark ? 0x69748F : 0x9AA6C2;
    }
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    uint32_t sig = (uint32_t)(growth * 4) * 2654435761u ^ (uint32_t)fruit * 40503u ^ (uint32_t)month * 97u ^
                   (uint32_t)(TR.wind01 * 20) * 7919u ^ (dark ? 0x80000000u : 0) ^ (uint32_t)(w.ok ? w.code : -1) * 131u;
    int slot = tm->tm_hour * 6 + tm->tm_min / 10;
    if (force || sig != TR.sig || slot != TR.slot) {
        TR.sig = sig;
        TR.slot = slot;
        hs_sky(sun.elev, w.ok ? w.code : -1, dark, &TR.sky);
        uint32_t lt = dark ? 0x10130F : 0xB8C2AE, lb = dark ? 0x07080A : 0xA3AD99;
        TR.land_top = hpx_mix(lt, TR.sky.horizon, dark ? 26 : 36);
        TR.land_bot = lb;
        TR.dark = dark;
        TR.month = month;
        TR.fruit_room = 12;
        ht_palette(dark, bz_color(BZ_C_LEAF), bz_color(BZ_C_AMBER), &TR.pal);
        TR.view = (ht_view_t){ .growth = growth, .fruit = fruit, .yaw = 0.6f, .lean = TR.wind01, .ambient = amb,
                               .tint = tint, .cx = TP_W / 2.0f, .base_y = (float)(TR_HOR - TP_Y + 6), .scale = 226 };
        for (int k = 0; k < 3; k++) TR.view.light[k] = L[k] / ll;
        /* the shadow, away from the sun: long when it is low, gone at night */
        float sdx = -L[0], len = el > 2 ? 60.0f / tanf((el < 8 ? 8 : el) * RADF) : 0;
        TR.shadow_dx = sdx * (len > 180 ? 180 : len);
        TR.shadow_len = len > 180 ? 180 : len;
        TR.shadow_a = el > 2 ? (dark ? 0.55f : 0.28f) : 0.2f;
        TR.mound_light = L[0];
        hm_tone(&TR.strip.tone);
        TR.pic.tone = TR.strip.tone;
        hm_pic_start(&TR.strip, strip_paint, &TR.strip);
        hm_pic_start(&TR.pic, tree_paint, NULL);
        /* the words' ink, from the sky under them */
        uint32_t under = hs_sky_at(&TR.sky, 0.3f), ink, dim;
        if (hpx_luma(under) < 120) {
            ink = 0xF2F4F7;
            dim = 0xB4BCC7;
        } else {
            ink = 0x15181B;
            dim = 0x4A525B;
        }
        hm_color(TR.time, ink);
        hm_color(TR.date, ink);
        hm_color(TR.title, ink);
        hm_color(TR.data, ink);
        hm_color(TR.what, dim);
        hm_color(TR.nature, dim);
        /* the weather in motion */
        int mode = particles_for(w.ok ? w.code : -1, month, TR.wind01);
        if (mode != TR.pmode || force) {
            TR.pmode = mode;
            TR.np = mode == P_RAIN ? 26 : mode == P_SNOW ? 20 : mode == P_LEAVES ? 3 + (int)(TR.wind01 * 12) : 0;
            if (TR.np > TR_NP) TR.np = TR_NP;
            hpx_rng_t r = { (uint32_t)mode * 77u + 1 };
            for (int i = 0; i < TR_NP; i++) {
                particle_t *p = &TR.p[i];
                bool on = i < TR.np;
                hm_show(p->o, false);
                p->shown_x = p->shown_y = -9999;
                if (!on) continue;
                p->x = hpx_range(&r, 0, W);
                p->y = hpx_range(&r, -200, TR_HOR);
                p->phase = hpx_range(&r, 0, 6.28f);
                if (mode == P_RAIN) {
                    lv_obj_set_size(p->o, 2, 18);
                    lv_obj_set_style_radius(p->o, 1, 0);
                    lv_obj_set_style_bg_color(p->o, bz_lv_rgb(dark ? 0x9FB2C8 : 0x5E7690), 0);
                    lv_obj_set_style_bg_opa(p->o, 120, 0);
                    p->vy = hpx_range(&r, 700, 900);
                } else if (mode == P_SNOW) {
                    int d = 4 + (int)hpx_range(&r, 0, 3);
                    lv_obj_set_size(p->o, d, d);
                    lv_obj_set_style_radius(p->o, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_color(p->o, bz_lv_rgb(0xF4F6FA), 0);
                    lv_obj_set_style_bg_opa(p->o, 220, 0);
                    p->vy = hpx_range(&r, 40, 80);
                } else {
                    /* a leaf off the crown: gold, red or rust, falling slowly and blown along */
                    static const uint32_t LC[3] = { 0xE0B040, 0xC85A30, 0x9A5A30 };
                    lv_obj_set_size(p->o, 9, 6);
                    lv_obj_set_style_radius(p->o, 3, 0);
                    lv_obj_set_style_bg_color(p->o, bz_lv_rgb(LC[i % 3]), 0);
                    lv_obj_set_style_bg_opa(p->o, LV_OPA_COVER, 0);
                    p->x = TP_X + hpx_range(&r, 120, 360);
                    p->y = TP_Y + hpx_range(&r, 60, 260);
                    p->vy = hpx_range(&r, 30, 55);
                }
            }
            TR.plast = 0;
        }
    }
    char t[8], d[48];
    hm_hm(tm, t, sizeof t);
    ui_text(TR.time, "%s", t);
    hm_baseline(TR.time, 50, 206);
    hm_date(tm, d, sizeof d, false);
    ui_text(TR.date, "%s", d);
    if (season) {
        ui_text(TR.what, "%s", "a branch for every match played, a fruit for every win");
        if (se.best_rank)
            ui_text(TR.data, "%d \xc2\xb7 %d matches \xc2\xb7 %d won \xc2\xb7 best rank %d of %d", se.year, se.played,
                    se.wins, se.best_rank, se.best_of);
        else ui_text(TR.data, "%d \xc2\xb7 %d matches \xc2\xb7 %d won", se.year, se.played, se.wins);
    } else {
        ui_text(TR.what, "%s", "it grows with the team's matches once the blue alliance answers");
        ui_text(TR.data, "until then, with the year: day %d of %d", tm->tm_yday + 1,
                (tm->tm_year % 4) == 0 ? 366 : 365);
    }
    char n[96];
    int k = snprintf(n, sizeof n, "%s", season_name(month));
    if (w.ok) k += snprintf(n + k, sizeof n - (size_t)k, " \xc2\xb7 %.0f\xc2\xb0 %s", w.temp, home_weather_text(w.code));
    if (fc.ok && fc.wind >= 0 && k < (int)sizeof n)
        snprintf(n + k, sizeof n - (size_t)k, " \xc2\xb7 wind %.0f %s", fc.wind, w.fahrenheit ? "mph" : "km/h");
    ui_text(TR.nature, "%s", n);
}

/* The tree's picture is shown as horizontal strips of itself (one buffer, a dozen images of its rows), so that a
 * gust can bend it: each strip slides sideways by a few pixels, more the higher it is (the square of its height,
 * as the lean), swinging and dying away over a couple of seconds. The strips carry their own sky, which matches
 * the tiled sky behind them, so the gap a strip opens shows the same sky. Gusts come more often and swing harder
 * the stronger the live wind; in a calm there are none, and between gusts nothing moves. */
static void strips_show(void)
{
    for (int k = 0; k < TR_STRIPS; k++) {
        int r0 = k * TP_H / TR_STRIPS, r1 = (k + 1) * TP_H / TR_STRIPS;
        lv_image_dsc_t *d = &TR.sdsc[k];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = TP_W;
        d->header.h = (uint32_t)(r1 - r0);
        d->header.stride = TP_W * 2;
        d->data_size = (uint32_t)((r1 - r0) * TP_W * 2);
        d->data = (const uint8_t *)(TR.pic.px + (size_t)r0 * TP_W);
        lv_image_cache_drop(d);
        lv_image_set_src(TR.strips[k], d);
        lv_obj_set_pos(TR.strips[k], TP_X + TR.soff[k], TP_Y + r0);
        lv_obj_invalidate(TR.strips[k]);
        hm_show(TR.strips[k], true);
    }
}

static void strips_hide(void)
{
    for (int k = 0; k < TR_STRIPS; k++) {
        hm_show(TR.strips[k], false);
        lv_image_cache_drop(&TR.sdsc[k]);
    }
}

static void tree_tick(void)
{
    hm_pic_poll(&TR.strip);
    if (hm_pic_poll(&TR.pic)) {
        hm_show(TR.pic.img, false); /* shown in strips instead */
        strips_show();
    }
}

/* a gust: when the next comes, and the one under way */
static void gust_frame(double t)
{
    if (TR.wind01 < 0.08f) return;
    if (TR.gust_at <= 0) TR.gust_at = t + 3 + (double)(hpx_hash((uint32_t)t) % 1000) / 1000.0 * 6;
    if (t < TR.gust_at) return;
    const double LEN = 2.6;
    double g = (t - TR.gust_at) / LEN;
    bool done = g >= 1;
    float amp = done ? 0 : (float)(sin(g * 3.14159) * (2.0 + TR.wind01 * 7.0)) * (float)exp(-g * 1.2);
    float sw = sinf((float)(t - TR.gust_at) * 2 * 3.14159f * 0.7f);
    float gy = TR_HOR - TP_Y;
    for (int k = 0; k < TR_STRIPS; k++) {
        float yc = (k + 0.5f) * TP_H / TR_STRIPS, h = (gy - yc) / gy;
        h = h < 0 ? 0 : h;
        int off = (int)lroundf(amp * sw * h * h);
        if (off != TR.soff[k]) {
            TR.soff[k] = off;
            lv_obj_set_x(TR.strips[k], TP_X + off);
        }
    }
    bz_ui_keep_alive();
    if (done) {
        /* the next, sooner in a stronger wind */
        double gap = 22.0 - 16.0 * TR.wind01;
        TR.gust_at = t + gap * (0.7 + (double)(hpx_hash((uint32_t)(t * 7)) % 600) / 1000.0);
    }
}

/* the weather in motion, at 30 frames a second: each piece moves a few pixels and redraws only itself */
static void tree_frame(double now)
{
    (void)now;
    if (!TR.pic.painted) return;
    double t = hal_seconds();
    gust_frame(t);
    if (!TR.np) return;
    bz_ui_keep_alive();
    if (++TR.frame_skip & 1) return;
    float dt = TR.plast > 0 ? (float)(t - TR.plast) : 0.033f;
    if (dt > 0.1f) dt = 0.1f;
    TR.plast = t;
    float drift = 20 + TR.wind01 * 160;
    for (int i = 0; i < TR.np; i++) {
        particle_t *p = &TR.p[i];
        p->phase += dt;
        if (TR.pmode == P_RAIN) {
            p->y += p->vy * dt;
            p->x += drift * 0.3f * dt;
            if (p->y > TR_HOR + 40 + (i % 5) * 14) {
                p->y = -20 - (float)(i * 37 % 120);
                p->x = (float)((i * 1553 + (int)(t * 97)) % W);
            }
        } else if (TR.pmode == P_SNOW) {
            p->y += p->vy * dt;
            p->x += (drift * 0.3f + sinf(p->phase * 1.3f + i) * 18) * dt;
            if (p->y > TR_HOR + 60 + (i % 4) * 16) {
                p->y = -10;
                p->x = (float)((i * 811 + (int)(t * 53)) % W);
            }
        } else {
            p->y += p->vy * dt;
            p->x += (drift + sinf(p->phase * 2.1f + i) * 40) * dt;
            if (p->y > TR_HOR + 30 || p->x > W + 20) {
                /* back into the crown for the next one */
                p->x = TP_X + 130 + (float)((i * 97 + (int)(t * 31)) % 220);
                p->y = TP_Y + 80 + (float)((i * 53 + (int)(t * 17)) % 180);
            }
        }
        if (p->x < -20) p->x += W + 40;
        if (p->x > W + 20 && TR.pmode != P_LEAVES) p->x -= W + 40;
        int x = (int)lroundf(p->x), y = (int)lroundf(p->y);
        bool vis = y > -20 && y < H;
        hm_show(p->o, vis);
        if (vis && (x != p->shown_x || y != p->shown_y)) {
            lv_obj_set_pos(p->o, x, y);
            p->shown_x = x;
            p->shown_y = y;
        }
    }
}

static bool tree_ready(void) { return !hm_pic_busy(&TR.strip) && !hm_pic_busy(&TR.pic); }

static void tree_leave(void)
{
    bool a = hm_pic_free(&TR.strip), b = !hm_pic_busy(&TR.pic);
    if (b) {
        strips_hide();
        hm_pic_free(&TR.pic);
    }
    if (a && b && !hm_art_busy()) {
        ht_free(TR.mesh);
        TR.mesh = NULL;
        TR.mesh_sig = 0;
    }
    if (a && b) TR.slot = -1;
    for (int i = 0; i < TR_NP; i++) hm_show(TR.p[i].o, false);
}

static bool tree_held(void) { return hm_pic_held(&TR.strip) || hm_pic_held(&TR.pic) || TR.mesh; }

/* the dev console: "hm tree" says what it's showing */
void hm_tree_dev(void)
{
    printf("hm tree: growth %.1f of %d, fruit %d, month %d, wind %.2f, particles %d (mode %d), mesh %s\n",
           TR.view.growth, TR.steps, TR.view.fruit, TR.month, TR.wind01, TR.np, TR.pmode, TR.mesh ? "held" : "free");
}

const hm_face_t HM_F_TREE = { .name = "tree", .build = tree_build, .minute = tree_minute, .tick = tree_tick,
                              .frame = tree_frame, .ready = tree_ready, .leave = tree_leave, .held = tree_held };
