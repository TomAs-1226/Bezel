/* The world face, after Bezel's: four clocks in a row, here and three cities the team's world runs on (Houston,
 * where the championship is; London; Tokyo), each dial's face lighter while the sun is up there and darker after
 * it sets, the city's own time and a word for its day ("tomorrow", "yesterday") under it. Here's hands are the
 * signal colour. LVGL objects only: a minute moves eight hands and rewrites four times. */
#include "ui_home_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NC 4
#define WR 118                 /* a dial's radius */
#define WY 318                 /* the dials' centres */
#define TAU 6.2831853f

typedef struct {
    const char *name;
    double lat, lon;
    int std_h;                 /* standard offset from UTC, hours */
    int rule;                  /* 0 none, 1 the US's daylight time, 2 the EU's summer time */
} city_t;

static const city_t CITIES[NC] = {
    { "here", 0, 0, 0, -1 },
    { "houston", 29.76, -95.37, -6, 1 },
    { "london", 51.51, -0.13, 0, 2 },
    { "tokyo", 35.68, 139.69, 9, 0 },
};

typedef struct {
    lv_obj_t *face, *ticks, *hh, *mm, *cap, *name, *time, *day;
    lv_point_precise_t ph[2], pm[2];
    int shown_min, shown_day;
} dial_t;

static struct {
    dial_t d[NC];
    lv_obj_t *head;
    lv_color_t tick;
} WD2;

/* the nth weekday (0 sunday) of a month, counted from 1 (n = -1: the last), as a day of the month */
static int nth_wday(int year, int mon, int wday, int n)
{
    struct tm t = { .tm_year = year - 1900, .tm_mon = mon, .tm_mday = 1, .tm_hour = 12 };
    time_t x = mktime(&t);
    struct tm f;
    localtime_r(&x, &f);
    int first = 1 + (wday - f.tm_wday + 7) % 7;
    if (n > 0) return first + 7 * (n - 1);
    static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = DAYS[mon] + (mon == 1 && year % 4 == 0 ? 1 : 0);
    int last = first;
    while (last + 7 <= dim) last += 7;
    return last;
}

/* a city's offset from UTC at unix time t, seconds */
static long city_offset(const city_t *c, time_t t)
{
    if (c->rule < 0) return (long)hm_tz_s();
    struct tm u;
    gmtime_r(&t, &u);
    int y = u.tm_year + 1900;
    long off = c->std_h * 3600L;
    if (c->rule == 1) {
        /* 2:00 local on the second sunday of march to 2:00 local on the first sunday of november */
        struct tm a = { .tm_year = y - 1900, .tm_mon = 2, .tm_mday = nth_wday(y, 2, 0, 2), .tm_hour = 2 };
        struct tm b = { .tm_year = y - 1900, .tm_mon = 10, .tm_mday = nth_wday(y, 10, 0, 1), .tm_hour = 2 };
        long local = (long)t + off;
        struct tm l;
        time_t lt = (time_t)local;
        gmtime_r(&lt, &l);
        int md = l.tm_mon * 100 + l.tm_mday, h = l.tm_hour;
        int ma = a.tm_mon * 100 + a.tm_mday, mb = b.tm_mon * 100 + b.tm_mday;
        bool dst = (md > ma || (md == ma && h >= 2)) && (md < mb || (md == mb && h < 1));
        if (dst) off += 3600;
    } else if (c->rule == 2) {
        /* 1:00 UTC on the last sunday of march to 1:00 UTC on the last sunday of october */
        int ma = 2 * 100 + nth_wday(y, 2, 0, -1), mb = 9 * 100 + nth_wday(y, 9, 0, -1);
        int md = u.tm_mon * 100 + u.tm_mday, h = u.tm_hour;
        bool dst = (md > ma || (md == ma && h >= 1)) && (md < mb || (md == mb && h < 1));
        if (dst) off += 3600;
    }
    return off;
}

static void ticks_draw(lv_event_t *e)
{
    dial_t *d = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(d->ticks, &co);
    float cx = (co.x1 + co.x2 + 1) / 2.0f, cy = (co.y1 + co.y2 + 1) / 2.0f;
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = WD2.tick;
    l.round_start = l.round_end = 1;
    for (int i = 0; i < 12; i++) {
        float a = i / 12.0f * TAU, s = sinf(a), c = -cosf(a);
        bool q = i % 3 == 0;
        float r1 = WR - 12, r0 = r1 - (q ? 16 : 8);
        l.width = q ? 4 : 2;
        l.opa = q ? LV_OPA_COVER : (lv_opa_t)170;
        l.p1.x = cx + s * r0;
        l.p1.y = cy + c * r0;
        l.p2.x = cx + s * r1;
        l.p2.y = cy + c * r1;
        lv_draw_line(layer, &l);
    }
}

static lv_obj_t *hand(lv_obj_t *b, int w)
{
    lv_obj_t *l = lv_line_create(b);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_size(l, 2 * WR, 2 * WR);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

static void world_build(lv_obj_t *b)
{
    int gap = (W - 2 * 56 - NC * 2 * WR) / (NC - 1);
    WD2.head = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 600);
    lv_obj_set_pos(WD2.head, 56, 44);
    for (int i = 0; i < NC; i++) {
        dial_t *d = &WD2.d[i];
        int cx = 56 + WR + i * (2 * WR + gap);
        d->face = bz_box(b);
        lv_obj_set_size(d->face, 2 * WR, 2 * WR);
        lv_obj_set_pos(d->face, cx - WR, WY - WR);
        lv_obj_set_style_radius(d->face, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d->face, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d->face, 2, 0);
        lv_obj_add_flag(d->face, LV_OBJ_FLAG_EVENT_BUBBLE);
        d->ticks = bz_box(b);
        lv_obj_set_size(d->ticks, 2 * WR, 2 * WR);
        lv_obj_set_pos(d->ticks, cx - WR, WY - WR);
        lv_obj_add_flag(d->ticks, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(d->ticks, ticks_draw, LV_EVENT_DRAW_MAIN, d);
        d->hh = hand(b, 8);
        d->mm = hand(b, 5);
        lv_obj_set_pos(d->hh, cx - WR, WY - WR);
        lv_obj_set_pos(d->mm, cx - WR, WY - WR);
        d->cap = bz_box(b);
        lv_obj_set_size(d->cap, 14, 14);
        lv_obj_set_pos(d->cap, cx - 7, WY - 7);
        lv_obj_set_style_radius(d->cap, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d->cap, LV_OPA_COVER, 0);
        d->name = bz_label_line(b, CITIES[i].name, BZ_F_NAME, BZ_C_INK, 2 * WR);
        lv_obj_set_style_text_align(d->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(d->name, cx - WR, WY + WR + 30);
        d->time = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 2 * WR);
        lv_obj_set_style_text_align(d->time, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(d->time, cx - WR, WY + WR + 68);
        d->day = bz_label_line(b, "", BZ_F_CAPTION, BZ_C_DIM, 2 * WR);
        lv_obj_set_style_text_align(d->day, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(d->day, cx - WR, WY + WR + 96);
        d->shown_min = -1;
    }
}

static void world_minute(const struct tm *tm, bool force)
{
    bool dark = bz_ui_dark();
    time_t now = time(NULL);
    double lat, lon;
    hm_where(&lat, &lon);
    uint32_t ink = bz_color(BZ_C_INK), g = bz_color(BZ_C_GROUND);
    if (force) {
        WD2.tick = bz_lv(BZ_C_INK);
        for (int i = 0; i < NC; i++) WD2.d[i].shown_min = -1;
    }
    char d0[48];
    hm_date(tm, d0, sizeof d0, false);
    ui_text(WD2.head, "%s \xc2\xb7 four cities", d0);
    struct tm here;
    localtime_r(&now, &here);
    for (int i = 0; i < NC; i++) {
        dial_t *d = &WD2.d[i];
        const city_t *c = &CITIES[i];
        long off = city_offset(c, now);
        time_t lt = now + off;
        struct tm ct;
        gmtime_r(&lt, &ct);
        double clat = i ? c->lat : lat, clon = i ? c->lon : lon;
        astro_sun_t sun;
        astro_sun((double)now, clat, clon, &sun);
        /* day: a light face in the dark tone, bright in the light; night: sunk into the ground */
        float dayk = sun.elev > 3 ? 1 : sun.elev < -6 ? 0 : (float)((sun.elev + 6) / 9);
        uint32_t day = dark ? 0x2A2E34 : 0xFFFFFF, night = dark ? 0x0F1114 : bz_mix(g, ink, 0.12f);
        uint32_t face = bz_mix(night, day, dayk);
        int key = ct.tm_hour * 60 + ct.tm_min + (int)(dayk * 8) * 10000;
        if (key == d->shown_min) continue;
        d->shown_min = key;
        lv_obj_set_style_bg_color(d->face, bz_lv_rgb(face), 0);
        lv_obj_set_style_border_color(d->face, bz_lv_rgb(bz_mix(face, ink, 0.18f)), 0);
        uint32_t hc = i == 0 ? bz_color(BZ_C_SIGNAL) : ink;
        lv_obj_set_style_line_color(d->hh, bz_lv_rgb(hc), 0);
        lv_obj_set_style_line_color(d->mm, bz_lv_rgb(hc), 0);
        lv_obj_set_style_bg_color(d->cap, bz_lv_rgb(hc), 0);
        float h = ((ct.tm_hour % 12) + ct.tm_min / 60.0f) / 12.0f * TAU, m = ct.tm_min / 60.0f * TAU;
        d->ph[0] = (lv_point_precise_t){ WR - sinf(h) * 14, WR + cosf(h) * 14 };
        d->ph[1] = (lv_point_precise_t){ WR + sinf(h) * (WR * 0.52f), WR - cosf(h) * (WR * 0.52f) };
        d->pm[0] = (lv_point_precise_t){ WR - sinf(m) * 16, WR + cosf(m) * 16 };
        d->pm[1] = (lv_point_precise_t){ WR + sinf(m) * (WR * 0.8f), WR - cosf(m) * (WR * 0.8f) };
        lv_line_set_points(d->hh, d->ph, 2);
        lv_line_set_points(d->mm, d->pm, 2);
        char t[8];
        hm_hm(&ct, t, sizeof t);
        ui_text(d->time, "%s", t);
        int dd = (ct.tm_year - here.tm_year) * 400 + ct.tm_yday - here.tm_yday;
        const char *when = dd > 0 ? "tomorrow" : dd < 0 ? "yesterday" : "today";
        if (i == 0) ui_text(d->day, "%s", sun.elev > 0 ? "day" : "night");
        else ui_text(d->day, "%s \xc2\xb7 %s", when, sun.elev > 0 ? "day" : "night");
        lv_obj_invalidate(d->ticks);
    }
}

const hm_face_t HM_F_WORLD = { .name = "world", .build = world_build, .minute = world_minute };
