/* Home mode's clock faces: the page left of the desk. After Bezel's standby: designed faces, calm (nothing blinks,
 * nothing drifts faster than the minute), one light direction, tonal and quiet; a flick up or down slides the next
 * one in, settings choose which faces are in the cycle and which shows.
 *
 *   dial     a Braun-style dial: sixty ticks, ink hands with their shadows falling down and to the right, a signal
 *            second hand that steps into each second on the tick spring, a date window at three
 *   horizon  a round window onto the sky as it is now: its colour through the day, the sun on its arc (the
 *            moon after dark), three ridges; the time beside it with sunrise and sunset
 *   dots     the time in a dot matrix on a field of faint dots, each lit dot haloed in the accent
 *   words    a word clock: "it is twenty five to ten", the minutes past the five as four corner dots
 *   moon     tonight's moon, lit by the phase, its seas and the faint earthshine; the phase in words
 *   night    (not in the cycle) the time dim and warm on black, from 22:00 to 7:00 once nobody has touched it
 *
 * What it costs. The dial, the dots, the words and the night face are LVGL objects, no picture: a minute's change
 * redraws only the hands' boxes, the dots that changed or the words that did. The horizon and the moon are one
 * picture each (~350 KB and ~200 KB), painted on the covers' worker when the face comes on (and the horizon every
 * ten minutes, the moon when the day changes), freed when it goes. The second hand is the one thing that moves at
 * rest: a 3 px line in its own small box, a quarter of a second each second; it can be turned off.
 *
 * A step between faces is a sheet over the old one (up from the bottom for the next, down from the top for the
 * previous), drawn a band at a time as it comes; the new face's picture is painted first, so the sheet never
 * waits on the worker. */
#include "ui_home_priv.h"

#include "home_px.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

const char *const HM_FACE_NAME[HM_NFACES + 1] = { "dial", "horizon", "dots", "words", "moon", "night" };

#define TAU 6.2831853f
#define SYNODIC 29.530588853

/* the dial */
#define SEC_SEGS 5 /* the second hand's redraw, in pieces along it: a long diagonal's box is mostly empty dial */
#define SEC_TAIL 60
#define SEC_TIP 268
#define DCX 640
#define DCY 372
#define DR 292
/* the horizon's window */
#define HZ_S 420
#define HZ_X 132
#define HZ_Y 150
/* the moon */
#define MN_S 330
#define MN_R 146
#define MN_X 760
#define MN_Y 172
/* the dots */
#define DT_CELL 24
#define DT_C 27
#define DT_R 11
#define DT_X ((1280 - DT_C * DT_CELL) / 2)
#define DT_Y 196
/* the words */
#define WD_C 11
#define WD_R 10
#define WD_CW 62
#define WD_RH 58
#define WD_X ((1280 - WD_C * WD_CW) / 2)
#define WD_Y 66

typedef struct {
    lv_obj_t *o;
    lv_point_precise_t p[2];
    int x, y, w, h, width;
} line_t;

typedef struct {
    uint16_t *px;
    lv_image_dsc_t dsc;
    unsigned ticket;
    bool painted;             /* what's in px is a finished picture */
    /* what the worker paints from, set before it's asked */
    float hour, rise, set, phase, lit;
    bool dark;
    uint16_t ground;
    int code;
    int w, h;
} pic_t;

static struct {
    lv_obj_t *pg, *face[HM_NFACES + 1], *index, *index_dot[HM_NFACES];
    int cur;
    int last_min, last_sec, last_mday;
    int dark;                 /* the tone the faces were coloured for (-1 none yet) */
    unsigned wx_gen;          /* the weather as last written on a face */
    int index_shown;
    /* a step to another face */
    bool stepping, sheet;
    int step_to, step_dir;
    bz_motion_t k;
    /* dial */
    struct {
        lv_obj_t *disc, *datebox, *date, *top_l, *top_r;
        line_t hand[2], shadow[2];
        lv_color_t tick_major, tick_minor;
        lv_obj_t *cap, *sec;      /* the second hand: one object over the dial that draws it (sec_draw) */
        lv_color_t sec_color;
        bz_motion_t secm;
        long long target;     /* the second hand's target in seconds, running on past 60 */
        int shown_sec;
        float drawn;          /* the angle last drawn, turns */
    } D;
    /* horizon */
    struct {
        lv_obj_t *img, *time, *date, *sun, *wx;
        pic_t pic;
        int slot;             /* the ten minutes it was painted for */
    } HZ;
    /* dots */
    struct {
        lv_obj_t *grid, *dot[DT_R][DT_C], *caption;
        uint8_t st[DT_R][DT_C];
        lv_image_dsc_t tile_dsc;
        uint16_t tile[DT_CELL * DT_CELL];
    } DT;
    /* words */
    struct {
        lv_obj_t *cell[WD_R][WD_C], *corner[4];
        uint8_t lit[WD_R][WD_C];
        int corners;
    } WD;
    /* moon */
    struct {
        lv_obj_t *img, *time, *date, *phase, *lit;
        pic_t pic;
        int day;
    } MN;
    /* night */
    struct {
        lv_obj_t *time, *date;
    } NT;
} FC = { .cur = -1, .last_min = -1, .dark = -1, .index_shown = -1 };

static bool night_face(int f) { return f == HM_FACE_NIGHT; }

/* ---------------------------------------------------------------- small pieces */

static lv_obj_t *face_box(int f)
{
    lv_obj_t *b = bz_box(FC.pg);
    lv_obj_set_size(b, W, H);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    FC.face[f] = b;
    return b;
}

static void line_make(line_t *l, lv_obj_t *parent, int width)
{
    l->o = lv_line_create(parent);
    lv_obj_remove_style_all(l->o);
    lv_obj_remove_flag(l->o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(l->o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_line_width(l->o, width, 0);
    lv_obj_set_style_line_rounded(l->o, true, 0);
    l->width = width;
    l->x = l->y = -9999;
}

static void line_color(line_t *l, uint32_t rgb, int opa)
{
    lv_obj_set_style_line_color(l->o, bz_lv_rgb(rgb), 0);
    lv_obj_set_style_line_opa(l->o, (lv_opa_t)opa, 0);
}

/* The line from (x0, y0) to (x1, y1) in its parent's coordinates, in an object only as big as the line: moving it
 * redraws its old box and its new one, never the dial. */
static void line_to(line_t *l, float x0, float y0, float x1, float y1)
{
    int m = l->width / 2 + 2;
    int xa = (int)floorf(fminf(x0, x1)) - m, ya = (int)floorf(fminf(y0, y1)) - m;
    int xb = (int)ceilf(fmaxf(x0, x1)) + m, yb = (int)ceilf(fmaxf(y0, y1)) + m;
    if (xa != l->x || ya != l->y) {
        lv_obj_set_pos(l->o, xa, ya);
        l->x = xa;
        l->y = ya;
    }
    if (xb - xa != l->w || yb - ya != l->h) {
        lv_obj_set_size(l->o, xb - xa, yb - ya);
        l->w = xb - xa;
        l->h = yb - ya;
    }
    l->p[0].x = x0 - (float)xa;
    l->p[0].y = y0 - (float)ya;
    l->p[1].x = x1 - (float)xa;
    l->p[1].y = y1 - (float)ya;
    lv_line_set_points(l->o, l->p, 2);
}

static lv_obj_t *disc(lv_obj_t *parent, int d)
{
    lv_obj_t *o = bz_box(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void fill_rgb(lv_obj_t *o, uint32_t rgb) { lv_obj_set_style_bg_color(o, bz_lv_rgb(rgb), 0); }
static void text_rgb(lv_obj_t *o, uint32_t rgb) { lv_obj_set_style_text_color(o, bz_lv_rgb(rgb), 0); }

static struct tm now_tm(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm;
}

static bool clock_set(const struct tm *tm) { return tm->tm_year > 120; }

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

static void date_text(const struct tm *tm, char *out, size_t n, bool shortform)
{
    if (!clock_set(tm)) {
        snprintf(out, n, "%s", "the clock isn't set");
        return;
    }
    strftime(out, n, shortform ? "%a %e %b" : "%A, %B %e", tm);
    lower(out);
    char *dd = strstr(out, "  ");
    if (dd) memmove(dd, dd + 1, strlen(dd));
}

static void weather_text(char *out, size_t n)
{
    home_weather_t w;
    home_weather_get(&w);
    if (w.configured && w.ok) snprintf(out, n, "%.0f\xc2\xb0 %s", w.temp, home_weather_text(w.code));
    else out[0] = 0;
}

static float hours_of(const char *hm, float dflt)
{
    int h, m;
    if (hm && sscanf(hm, "%d:%d", &h, &m) == 2) return (float)h + (float)m / 60.0f;
    return dflt;
}

/* ---------------------------------------------------------------- the dial */

static void sec_draw(lv_event_t *e);

static void ticks_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t *clip = &layer->_clip_area;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    for (int i = 0; i < 60; i++) {
        bool major = i % 5 == 0;
        float a = (float)i / 60.0f * TAU, s = sinf(a), c = -cosf(a);
        float r1 = DR - 18, r0 = r1 - (major ? 34 : 13);
        float x0 = DCX + s * r0, y0 = DCY + c * r0, x1 = DCX + s * r1, y1 = DCY + c * r1;
        /* only the ticks that cross what's being drawn */
        if (fmaxf(x0, x1) + 4 < clip->x1 || fminf(x0, x1) - 4 > clip->x2 || fmaxf(y0, y1) + 4 < clip->y1 ||
            fminf(y0, y1) - 4 > clip->y2)
            continue;
        d.width = major ? 6 : 2;
        d.color = major ? FC.D.tick_major : FC.D.tick_minor;
        d.opa = major ? LV_OPA_COVER : (lv_opa_t)170;
        d.p1.x = x0;
        d.p1.y = y0;
        d.p2.x = x1;
        d.p2.y = y1;
        lv_draw_line(layer, &d);
    }
}

static void dial_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_DIAL);
    FC.D.disc = disc(b, 2 * DR);
    lv_obj_set_pos(FC.D.disc, DCX - DR, DCY - DR);
    lv_obj_set_style_border_width(FC.D.disc, 2, 0);
    /* the sixty ticks drawn by the disc itself: sixty objects cost a layout walk each to build, and a redraw
     * of any part of the dial only draws the ticks that cross it */
    lv_obj_add_event_cb(FC.D.disc, ticks_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    /* the date window at three */
    FC.D.datebox = bz_box(b);
    lv_obj_set_size(FC.D.datebox, 64, 44);
    lv_obj_set_style_radius(FC.D.datebox, 10, 0);
    lv_obj_set_style_bg_opa(FC.D.datebox, LV_OPA_COVER, 0);
    lv_obj_set_pos(FC.D.datebox, DCX + DR - 18 - 34 - 26 - 64, DCY - 22);
    FC.D.date = bz_label(FC.D.datebox, "", BZ_F_LABEL, BZ_C_INK);
    lv_obj_center(FC.D.date);
    /* shadows first, then the hands over them, the second hand over everything */
    for (int i = 0; i < 2; i++) line_make(&FC.D.shadow[i], b, i ? 12 : 16);
    line_make(&FC.D.hand[0], b, 14);
    line_make(&FC.D.hand[1], b, 9);
    FC.D.cap = disc(b, 26);
    lv_obj_set_pos(FC.D.cap, DCX - 13, DCY - 13);
    FC.D.sec = bz_box(b);
    lv_obj_set_size(FC.D.sec, 2 * DR, 2 * DR);
    lv_obj_set_pos(FC.D.sec, DCX - DR, DCY - DR);
    lv_obj_add_event_cb(FC.D.sec, sec_draw, LV_EVENT_DRAW_MAIN, NULL);
    FC.D.top_l = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_pos(FC.D.top_l, 58, 42);
    FC.D.top_r = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_style_text_align(FC.D.top_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(FC.D.top_r, W - PAD - ORB_CLEAR - 300, 42);
    bz_motion_init(&FC.D.secm, 0, 0.01f);
    FC.D.secm.keep = true;
    FC.D.shown_sec = -1;
    FC.D.drawn = -1;
}

static void dial_colors(bool dark)
{
    uint32_t face = bz_color(BZ_C_SURFACE1), ink = bz_color(BZ_C_INK), dim = bz_color(BZ_C_DIM);
    fill_rgb(FC.D.disc, face);
    lv_obj_set_style_border_color(FC.D.disc, bz_lv_rgb(hpx_mix(face, ink, dark ? 40 : 30)), 0);
    FC.D.tick_major = bz_lv_rgb(ink);
    FC.D.tick_minor = bz_lv_rgb(dim);
    lv_obj_invalidate(FC.D.disc);
    for (int i = 0; i < 2; i++) line_color(&FC.D.shadow[i], 0x000000, dark ? 90 : 38);
    line_color(&FC.D.hand[0], ink, 255);
    line_color(&FC.D.hand[1], ink, 255);
    FC.D.sec_color = bz_lv(BZ_C_SIGNAL);
    fill_rgb(FC.D.cap, ink);
    fill_rgb(FC.D.datebox, bz_color(BZ_C_SURFACE2));
}

/* the hands for a time: hours and minutes (with their fractions) */
static void dial_hands(float hours, float minutes)
{
    float ha = hours / 12.0f * TAU, ma = minutes / 60.0f * TAU;
    float hs = sinf(ha), hc = -cosf(ha), ms = sinf(ma), mc = -cosf(ma);
    /* the light is up and to the left: shadows fall down and a little right */
    const float sx = 4, sy = 8;
    line_to(&FC.D.shadow[0], DCX - hs * 26 + sx, DCY - hc * 26 + sy, DCX + hs * 170 + sx, DCY + hc * 170 + sy);
    line_to(&FC.D.shadow[1], DCX - ms * 30 + sx, DCY - mc * 30 + sy, DCX + ms * 252 + sx, DCY + mc * 252 + sy);
    line_to(&FC.D.hand[0], DCX - hs * 26, DCY - hc * 26, DCX + hs * 170, DCY + hc * 170);
    line_to(&FC.D.hand[1], DCX - ms * 30, DCY - mc * 30, DCX + ms * 252, DCY + mc * 252);
}

/* The small boxes along the hand, each covering where that piece was and where it goes (a frame of the tick
 * moves it a degree or two): one call a piece, since every invalidation has its own cost. */
static void sec_invalidate(float from, float to)
{
    if (to < 0) return;
    if (from < 0) from = to;
    float sa = sinf(from * TAU), ca = -cosf(from * TAU), sb = sinf(to * TAU), cb = -cosf(to * TAU);
    for (int i = 0; i < SEC_SEGS; i++) {
        float r0 = -SEC_TAIL + (float)(SEC_TIP + SEC_TAIL) * i / SEC_SEGS;
        float r1 = -SEC_TAIL + (float)(SEC_TIP + SEC_TAIL) * (i + 1) / SEC_SEGS;
        float xs[4] = { DCX + sa * r0, DCX + sa * r1, DCX + sb * r0, DCX + sb * r1 };
        float ys[4] = { DCY + ca * r0, DCY + ca * r1, DCY + cb * r0, DCY + cb * r1 };
        float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
        for (int k = 1; k < 4; k++) {
            x1 = fminf(x1, xs[k]);
            x2 = fmaxf(x2, xs[k]);
            y1 = fminf(y1, ys[k]);
            y2 = fmaxf(y2, ys[k]);
        }
        /* the face's coordinates are the screen's (it fills it from 0, 0); the cap is in the piece at the centre */
        lv_area_t ar = { (int32_t)floorf(x1) - 4, (int32_t)floorf(y1) - 4, (int32_t)ceilf(x2) + 4, (int32_t)ceilf(y2) + 4 };
        if (r0 < 0 && r1 > 0) {
            ar.x1 = LV_MIN(ar.x1, DCX - 10);
            ar.y1 = LV_MIN(ar.y1, DCY - 10);
            ar.x2 = LV_MAX(ar.x2, DCX + 10);
            ar.y2 = LV_MAX(ar.y2, DCY + 10);
        }
        lv_obj_invalidate_area(FC.D.sec, &ar);
    }
}

/* The second hand drawn by the object over the dial, clipped by LVGL to whatever is being redrawn: moving it
 * restyles and re-lays-out nothing (an object moved or resized every frame walks the layout; measured, it was
 * most of a frame), and redraws only the few small boxes along the hand. */
static void sec_draw(lv_event_t *e)
{
    if (FC.D.drawn < 0) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    float a = FC.D.drawn * TAU, s = sinf(a), c = -cosf(a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = FC.D.sec_color;
    d.width = 3;
    d.opa = LV_OPA_COVER;
    d.round_start = d.round_end = 1;
    d.p1.x = DCX - s * SEC_TAIL;
    d.p1.y = DCY - c * SEC_TAIL;
    d.p2.x = DCX + s * SEC_TIP;
    d.p2.y = DCY + c * SEC_TIP;
    lv_draw_line(layer, &d);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = FC.D.sec_color;
    r.bg_opa = LV_OPA_COVER;
    r.radius = LV_RADIUS_CIRCLE;
    lv_area_t cap = { DCX - 8, DCY - 8, DCX + 7, DCY + 7 };
    lv_draw_rect(layer, &r, &cap);
}

static void dial_second(float turns)
{
    if (fabsf(turns - FC.D.drawn) < 0.0004f) return;
    sec_invalidate(FC.D.drawn, turns);
    FC.D.drawn = turns;
}

static void dial_minute(const struct tm *tm)
{
    char d[40], w[48];
    date_text(tm, d, sizeof d, true);
    ui_text(FC.D.top_l, "%s", d);
    weather_text(w, sizeof w);
    ui_text(FC.D.top_r, "%s", w);
    ui_text(FC.D.date, "%d", clock_set(tm) ? tm->tm_mday : 0);
    dial_hands((float)(tm->tm_hour % 12) + tm->tm_min / 60.0f, (float)tm->tm_min + tm->tm_sec / 60.0f);
    bool secs = hm_cfg()->dial_secs;
    hm_show(FC.D.sec, secs);
}

/* every frame while the dial shows: the second hand steps into each second on the tick spring (a quartz
 * movement settling into its detent), and is still in between */
static void dial_frame(void)
{
    if (!hm_cfg()->dial_secs) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now = (long long)tv.tv_sec;
    if (FC.D.shown_sec < 0 || llabs(now - FC.D.target) > 3) {
        FC.D.target = now;
        bz_motion_set(&FC.D.secm, (float)(now % 60), 0);
        FC.D.shown_sec = (int)(now % 60);
    } else if (now != FC.D.target) {
        FC.D.target = now;
        float v = FC.D.secm.value;
        float to = (float)(now % 60);
        if (to < v - 30) v -= 60; /* past the top: carry on round, not back */
        bz_motion_set(&FC.D.secm, v, FC.D.secm.velocity);
        /* a quick tick that overshoots a hair and settles in ~0.2 s: a quartz movement's step */
        static const bz_spring_t QUARTZ = { 1600.0f, 52.0f, 1.0f };
        bz_motion_to(&FC.D.secm, to, QUARTZ);
    }
    bool moving = bz_motion_tick(&FC.D.secm);
    float t = FC.D.secm.value / 60.0f;
    dial_second(t - floorf(t));
    if (moving) bz_ui_keep_alive();
}

/* ---------------------------------------------------------------- the horizon */

static void pic_init(pic_t *p, int w, int h)
{
    p->w = w;
    p->h = h;
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p->dsc.header.w = (uint32_t)w;
    p->dsc.header.h = (uint32_t)h;
    p->dsc.header.stride = (uint32_t)w * 2;
    p->dsc.data_size = (uint32_t)(w * h * 2);
}

static bool pic_alloc(pic_t *p)
{
    if (p->px) return true;
    p->px = malloc(sizeof(uint16_t) * (size_t)p->w * (size_t)p->h);
    p->dsc.data = (const uint8_t *)p->px;
    p->painted = false;
    return p->px != NULL;
}

/* false while the worker may still be painting into it */
static bool pic_free(pic_t *p)
{
    if (!hm_art_job_done(p->ticket)) return false;
    lv_image_cache_drop(&p->dsc);
    free(p->px);
    p->px = NULL;
    p->dsc.data = NULL;
    p->painted = false;
    p->ticket = 0;
    return true;
}

static uint32_t rgbf(float r, float g, float b) { return hpx_rgb((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f)); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* A round window: outside the circle the page's ground, exactly, so the picture has no edge of its own; a thin
 * lighter rim where the glass would be. */
static void window_mask(uint16_t *px, int s, uint16_t ground, uint32_t rim, int rim_a)
{
    float c = s * 0.5f, r = s * 0.5f - 2;
    uint32_t g = hpx_from565(ground);
    hpx_t cv;
    hpx_init16(&cv, px, s, s, s);
    for (int y = 0; y < s; y++) {
        float fy = y + 0.5f - c;
        for (int x = 0; x < s; x++) {
            float fx = x + 0.5f - c, d = sqrtf(fx * fx + fy * fy) - r;
            if (d >= 0.5f) px[(size_t)y * s + x] = ground;
            else if (d > -0.5f) hpx_put(&cv, x, y, g, (int)((d + 0.5f) * 255));
            else if (d > -2.0f) hpx_put(&cv, x, y, rim, rim_a);
        }
    }
}

/* the sky at an hour, after Bezel's horizon scape: its colour through the day, the sun on its arc, stars after
 * dark, three ridges; tone 0 dark, 1 light */
static void horizon_paint(void *arg)
{
    pic_t *p = arg;
    const int S = HZ_S;
    uint16_t *px = p->px;
    if (!px) return;
    float tone = p->dark ? 0.0f : 1.0f;
    float span = p->set - p->rise;
    if (span < 4) span = 13;
    float day = (p->hour - p->rise) / span;
    float up = day >= 0 && day <= 1 ? 1.0f : 0.0f;
    float edge = fminf(fabsf(day), fabsf(1 - day));
    float dusk = expf(-edge * edge * 60.0f);
    float night = (1 - up) * smooth(0, 0.08f, edge);
    float top[3], low[3];
    for (int k = 0; k < 3; k++) {
        float td = mixf((float[]){ 0.24f, 0.45f, 0.74f }[k], (float[]){ 0.62f, 0.76f, 0.92f }[k], tone);
        float tk = mixf((float[]){ 0.15f, 0.16f, 0.34f }[k], (float[]){ 0.6f, 0.62f, 0.8f }[k], tone);
        float ld = mixf((float[]){ 0.62f, 0.77f, 0.9f }[k], (float[]){ 0.93f, 0.95f, 0.97f }[k], tone);
        float lk = mixf((float[]){ 0.95f, 0.5f, 0.32f }[k], (float[]){ 1.0f, 0.8f, 0.66f }[k], tone);
        float tn = mixf((float[]){ 0.015f, 0.02f, 0.05f }[k], (float[]){ 0.45f, 0.5f, 0.64f }[k], tone);
        float ln = mixf((float[]){ 0.05f, 0.07f, 0.14f }[k], (float[]){ 0.72f, 0.74f, 0.82f }[k], tone);
        top[k] = mixf(mixf(td, tk, dusk), tn, night);
        low[k] = mixf(mixf(ld, lk, dusk), ln, night);
    }
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    for (int y = 0; y < S; y++) {
        float t = smooth(0, 0.7f, (float)y / S);
        uint32_t col = rgbf(mixf(top[0], low[0], t), mixf(top[1], low[1], t), mixf(top[2], low[2], t));
        hpx_clip(&c, 0, y, S - 1, y);
        hpx_fill(&c, col);
    }
    hpx_unclip(&c);
    /* stars */
    if (night > 0.05f) {
        hpx_rng_t r = { 0x5ea5u };
        for (int i = 0; i < 260; i++) {
            int x = (int)(hpx_rand(&r) * S), y = (int)(hpx_rand(&r) * S * 0.6f);
            int a = (int)(hpx_rand(&r) * 200 * night * (1 - (float)y / S) * mixf(1.0f, 0.3f, tone));
            hpx_put(&c, x, y, 0xF2F4FF, a);
        }
    }
    /* the sun on its arc, or the moon at night */
    float dd = day < 0 ? 0 : day > 1 ? 1 : day;
    float sx = mixf(0.12f, 0.88f, dd) * S, sy = (0.66f - sinf(3.14159f * dd) * 0.44f) * S;
    if (up > 0) {
        uint32_t sun = hpx_mix(rgbf(1.0f, 0.95f, 0.82f), rgbf(1.0f, 0.58f, 0.32f), (int)(dusk * 256));
        hpx_glow(&c, sx, sy, S * 0.22f, sun, 110);
        hpx_disc(&c, sx, sy, S * 0.05f, 0, sun, 255);
    } else {
        float mt = p->hour < p->rise ? (p->hour + 24 - p->set) / (24 - span) : (p->hour - p->set) / (24 - span);
        mt = mt < 0 ? 0 : mt > 1 ? 1 : mt;
        float mx = mixf(0.2f, 0.8f, mt) * S, my = (0.5f - sinf(3.14159f * mt) * 0.26f) * S;
        uint32_t moon = tone > 0.5f ? 0xF6F4EC : 0xDCDDE2;
        hpx_glow(&c, mx, my, S * 0.12f, moon, tone > 0.5f ? 40 : 60);
        hpx_disc(&c, mx, my, S * 0.035f, 0, moon, 255);
    }
    /* the ridges: each nearer one lower, darker and rougher */
    float *ridge = malloc(sizeof(float) * S);
    if (ridge) {
        for (int k = 0; k < 3; k++) {
            float f = 2.4f + 1.3f * k;
            for (int x = 0; x < S; x++) {
                float u = (float)x / S * f + k * 5.1f;
                float n = hpx_noise1(u, 0x77 + k) * 0.65f + hpx_noise1(u * 2.7f, 0x99 + k) * 0.25f +
                          hpx_noise1(u * 6.1f, 0xbb + k) * 0.1f;
                ridge[x] = (0.58f + 0.085f * k + (n - 0.5f) * (0.11f - 0.025f * k)) * S;
            }
            float nr[3], fr[3];
            for (int q = 0; q < 3; q++) {
                nr[q] = mixf((float[]){ 0.03f, 0.04f, 0.07f }[q], (float[]){ 0.34f, 0.38f, 0.44f }[q], tone);
                fr[q] = mixf(mixf((float[]){ 0.2f, 0.25f, 0.36f }[q], (float[]){ 0.64f, 0.69f, 0.76f }[q], tone), low[q], 0.35f);
            }
            float t = k / 2.0f;
            hpx_ridge(&c, ridge, 0, rgbf(mixf(fr[0], nr[0], t), mixf(fr[1], nr[1], t), mixf(fr[2], nr[2], t)), 255);
        }
        free(ridge);
    }
    window_mask(px, S, p->ground, tone > 0.5f ? 0xFFFFFF : 0x9AA1A8, tone > 0.5f ? 150 : 60);
}

static void horizon_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_HORIZON);
    pic_init(&FC.HZ.pic, HZ_S, HZ_S);
    FC.HZ.img = lv_image_create(b);
    lv_obj_set_pos(FC.HZ.img, HZ_X, HZ_Y);
    lv_obj_add_flag(FC.HZ.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(FC.HZ.img, LV_OBJ_FLAG_EVENT_BUBBLE);
    int tx = HZ_X + HZ_S + 84;
    FC.HZ.time = bz_label(b, "", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(FC.HZ.time, tx - 6, 218);
    FC.HZ.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_DIM, W - tx - 60);
    lv_obj_set_pos(FC.HZ.date, tx, 358);
    FC.HZ.sun = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, W - tx - 60);
    lv_obj_set_pos(FC.HZ.sun, tx, 404);
    FC.HZ.wx = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, W - tx - 60);
    lv_obj_set_pos(FC.HZ.wx, tx, 434);
    FC.HZ.slot = -1;
}

static void horizon_minute(const struct tm *tm, bool force)
{
    char d[48], w[48];
    if (clock_set(tm)) ui_text(FC.HZ.time, "%d:%02d", tm->tm_hour, tm->tm_min);
    else ui_text(FC.HZ.time, "%s", "--:--");
    date_text(tm, d, sizeof d, false);
    ui_text(FC.HZ.date, "%s", d);
    home_forecast_t f;
    home_weather_forecast(&f);
    if (f.ok && f.sunrise[0] && f.sunset[0]) ui_text(FC.HZ.sun, "sunrise %s \xc2\xb7 sunset %s", f.sunrise, f.sunset);
    else ui_text(FC.HZ.sun, "%s", "");
    weather_text(w, sizeof w);
    ui_text(FC.HZ.wx, "%s", w);
    /* the picture again every ten minutes (the sun moves ~2.5 degrees), or when asked */
    int slot = tm->tm_hour * 6 + tm->tm_min / 10;
    if ((slot != FC.HZ.slot || force) && pic_alloc(&FC.HZ.pic)) {
        FC.HZ.slot = slot;
        pic_t *p = &FC.HZ.pic;
        p->hour = (float)tm->tm_hour + tm->tm_min / 60.0f;
        p->rise = hours_of(f.ok ? f.sunrise : NULL, 6.5f);
        p->set = hours_of(f.ok ? f.sunset : NULL, 19.5f);
        p->dark = bz_ui_dark();
        p->ground = hm_565(bz_color(BZ_C_GROUND));
        p->ticket = hm_art_job(horizon_paint, p);
        p->painted = false;
    }
}

/* ---------------------------------------------------------------- the moon */

static void moon_now(float *phase, float *lit)
{
    double days = (double)time(NULL) / 86400.0 + 2440587.5 - 2451550.1;
    double ph = fmod(days / SYNODIC, 1.0);
    if (ph < 0) ph += 1;
    *phase = (float)ph;
    *lit = (float)((1 - cos(ph * 6.283185307)) / 2);
}

/* tonight's moon, after Bezel's moon face: a sphere whose light turns with the phase, darker seas from value
 * noise, a few craters, faint earthshine, a quiet halo */
static void moon_paint(void *arg)
{
    pic_t *p = arg;
    const int S = MN_S;
    uint16_t *px = p->px;
    if (!px) return;
    float tone = p->dark ? 0.0f : 1.0f;
    for (int i = 0; i < S * S; i++) px[i] = p->ground;
    uint32_t g = hpx_from565(p->ground);
    hpx_t c;
    hpx_init16(&c, px, S, S, S);
    float a = p->phase * TAU, lx = sinf(a), lz = -cosf(a);
    float R = MN_R, cx = S * 0.5f, cy = S * 0.5f;
    /* the halo, by how much of it is lit */
    uint32_t halo = tone > 0.5f ? 0x000000 : 0x8A93A6;
    hpx_glow(&c, cx, cy, R * 1.45f, halo, (int)((tone > 0.5f ? 18 : 46) * p->lit));
    for (int y = (int)(cy - R - 1); y <= (int)(cy + R + 1); y++) {
        for (int x = (int)(cx - R - 1); x <= (int)(cx + R + 1); x++) {
            float dx = (x + 0.5f - cx) / R, dy = (y + 0.5f - cy) / R, r2 = dx * dx + dy * dy;
            float r = sqrtf(r2);
            float cov = (1.0f - r) * R + 0.5f;
            if (cov <= 0) continue;
            float nz = sqrtf(fmaxf(0, 1 - r2));
            float lit = smooth(-0.05f, 0.07f, dx * lx + nz * lz);
            float sx = dx * 3.1f + 2.3f, sy = dy * 3.1f + 2.3f;
            float n = hpx_noise2(sx, sy, 0x3001) * 0.7f + hpx_noise2(sx * 2.4f, sy * 2.4f, 0x3002) * 0.3f;
            float maria = smooth(0.42f, 0.82f, n);
            /* and a finer grain of highlands and small seas over it */
            float fine = hpx_noise2(sx * 7.3f, sy * 7.3f, 0x3003) - 0.5f;
            float alb = 0.87f - 0.26f * maria * 0.7f - 0.07f * fine;
            float dark = mixf(0.04f + alb * 0.03f, 0.64f, tone);
            float bright = mixf(alb, 1.0f - (0.87f - alb) * 0.6f, tone);
            float v = mixf(dark, bright, lit);
            /* a trace of warmth in the lit part, of blue in the earthshine */
            uint32_t col = rgbf(v * (1.0f + 0.02f * lit), v, v * (1.0f - 0.03f * lit + 0.04f * (1 - lit)));
            hpx_put(&c, x, y, col, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
    /* on a light page a pale moon needs its edge drawn, as Bezel's light moon has */
    if (tone > 0.5f) hpx_ring(&c, cx, cy, R - 0.5f, 1.4f, 0, 0x15181B, 56);
    (void)g;
}

static void moon_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_MOON);
    pic_init(&FC.MN.pic, MN_S, MN_S);
    FC.MN.img = lv_image_create(b);
    lv_obj_set_pos(FC.MN.img, MN_X, MN_Y);
    lv_obj_add_flag(FC.MN.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(FC.MN.img, LV_OBJ_FLAG_EVENT_BUBBLE);
    int tx = 120;
    FC.MN.time = bz_label(b, "", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(FC.MN.time, tx - 6, 186);
    FC.MN.date = bz_label_line(b, "", BZ_F_BODY, BZ_C_DIM, 520);
    lv_obj_set_pos(FC.MN.date, tx, 326);
    FC.MN.phase = bz_label_line(b, "", BZ_F_NAME, BZ_C_INK, 520);
    lv_obj_set_pos(FC.MN.phase, tx, 392);
    FC.MN.lit = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 520);
    lv_obj_set_pos(FC.MN.lit, tx, 434);
    FC.MN.day = -1;
}

static void moon_minute(const struct tm *tm, bool force)
{
    char d[48];
    if (clock_set(tm)) ui_text(FC.MN.time, "%d:%02d", tm->tm_hour, tm->tm_min);
    else ui_text(FC.MN.time, "%s", "--:--");
    date_text(tm, d, sizeof d, false);
    ui_text(FC.MN.date, "%s", d);
    float phase, lit;
    moon_now(&phase, &lit);
    static const struct { float lim; const char *name; } PH[] = {
        { 0.03f, "new moon" }, { 0.22f, "waxing crescent" }, { 0.28f, "first quarter" }, { 0.47f, "waxing gibbous" },
        { 0.53f, "full moon" }, { 0.72f, "waning gibbous" }, { 0.78f, "last quarter" }, { 0.97f, "waning crescent" },
        { 1.01f, "new moon" },
    };
    const char *name = "moon";
    for (size_t i = 0; i < sizeof PH / sizeof *PH; i++)
        if (phase < PH[i].lim) {
            name = PH[i].name;
            break;
        }
    ui_text(FC.MN.phase, "%s", name);
    float target = phase < 0.5f ? 0.5f : 1.0f;
    int days = (int)lroundf(fmodf(target - phase + 1.0f, 1.0f) * (float)SYNODIC);
    char when[24];
    if (days == 0) snprintf(when, sizeof when, "tonight");
    else snprintf(when, sizeof when, "in %d day%s", days, days == 1 ? "" : "s");
    ui_text(FC.MN.lit, "%d%% lit \xc2\xb7 %s %s", (int)lroundf(lit * 100), phase < 0.5f ? "full moon" : "new moon", when);
    /* the picture once a day, or when asked (a new tone) */
    int day = clock_set(tm) ? tm->tm_yday : 0;
    if ((day != FC.MN.day || force) && pic_alloc(&FC.MN.pic)) {
        FC.MN.day = day;
        pic_t *p = &FC.MN.pic;
        p->phase = phase;
        p->lit = lit;
        p->dark = bz_ui_dark();
        p->ground = hm_565(bz_color(BZ_C_GROUND));
        p->ticket = hm_art_job(moon_paint, p);
        p->painted = false;
    }
}

/* ---------------------------------------------------------------- the dots */

/* 5 × 7 digits, one 5-bit row each, top row first (Bezel's dot-matrix face) */
static const uint8_t FONT5[10][7] = {
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },    { 14, 17, 1, 2, 4, 8, 31 },
    { 31, 2, 4, 2, 1, 17, 14 },     { 2, 6, 10, 18, 31, 2, 2 },   { 31, 16, 30, 1, 1, 17, 14 },
    { 6, 8, 16, 30, 17, 17, 14 },   { 31, 1, 2, 4, 8, 8, 8 },     { 14, 17, 17, 14, 17, 17, 14 },
    { 14, 17, 17, 15, 1, 2, 12 },
};

static void dots_tile(bool dark)
{
    /* one faint dot in a cell, tiled over the page by LVGL */
    uint32_t g = bz_color(BZ_C_GROUND), dot = hpx_mix(g, bz_color(BZ_C_FAINT), dark ? 120 : 110);
    uint16_t gv = hm_565(g);
    for (int i = 0; i < DT_CELL * DT_CELL; i++) FC.DT.tile[i] = gv;
    hpx_t c;
    hpx_init16(&c, FC.DT.tile, DT_CELL, DT_CELL, DT_CELL);
    hpx_disc(&c, DT_CELL / 2.0f, DT_CELL / 2.0f, 2.1f, 0, dot, 255);
    lv_image_cache_drop(&FC.DT.tile_dsc);
    if (FC.DT.grid) lv_obj_invalidate(FC.DT.grid);
}

static void dots_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_DOTS);
    lv_image_dsc_t *d = &FC.DT.tile_dsc;
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = DT_CELL;
    d->header.h = DT_CELL;
    d->header.stride = DT_CELL * 2;
    d->data = (const uint8_t *)FC.DT.tile;
    d->data_size = sizeof FC.DT.tile;
    dots_tile(bz_ui_dark());
    /* the field of faint dots, its cells on the block's */
    FC.DT.grid = lv_image_create(b);
    lv_image_set_src(FC.DT.grid, d);
    lv_image_set_inner_align(FC.DT.grid, LV_IMAGE_ALIGN_TILE);
    int ox = DT_X % DT_CELL - DT_CELL, oy = DT_Y % DT_CELL - DT_CELL;
    lv_obj_set_pos(FC.DT.grid, ox, oy);
    lv_obj_set_size(FC.DT.grid, W - ox, H - oy);
    lv_obj_add_flag(FC.DT.grid, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int r = 0; r < DT_R; r++)
        for (int k = 0; k < DT_C; k++) {
            lv_obj_t *o = disc(b, 8);
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            FC.DT.dot[r][k] = o;
            FC.DT.st[r][k] = 0;
        }
    FC.DT.caption = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 800);
    lv_obj_set_style_text_align(FC.DT.caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(FC.DT.caption, (W - 800) / 2, DT_Y + DT_R * DT_CELL + 34);
}

static void dots_minute(const struct tm *tm, bool force)
{
    uint8_t lit[DT_R][DT_C];
    memset(lit, 0, sizeof lit);
    int dig[4] = { tm->tm_hour / 10, tm->tm_hour % 10, tm->tm_min / 10, tm->tm_min % 10 };
    static const int COL[4] = { 1, 7, 15, 21 };
    bool set = clock_set(tm);
    for (int i = 0; i < 4 && set; i++)
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (FONT5[dig[i]][r] >> (4 - k) & 1) lit[2 + r][COL[i] + k] = 1;
    if (set) lit[4][13] = lit[6][13] = 1;
    /* 1 lit, 2 its halo (a neighbour of a lit dot), 0 off: only the dots whose state changed are touched */
    /* the halo in the accent: its fill on a dark page, its deep on-colour, faint, on a light one (the light fill
     * is a pale container that vanishes on a pale page) */
    bool dark = bz_ui_dark();
    uint32_t ink = bz_color(BZ_C_INK), halo = bz_color(dark ? BZ_C_ICE : BZ_C_ON_ICE);
    for (int r = 0; r < DT_R; r++)
        for (int k = 0; k < DT_C; k++) {
            int st = lit[r][k];
            if (!st)
                for (int dr = -1; dr <= 1 && !st; dr++)
                    for (int dk = -1; dk <= 1 && !st; dk++) {
                        int rr = r + dr, kk = k + dk;
                        if (rr >= 0 && rr < DT_R && kk >= 0 && kk < DT_C && lit[rr][kk]) st = 2;
                    }
            if (st == FC.DT.st[r][k] && !force) continue;
            FC.DT.st[r][k] = (uint8_t)st;
            lv_obj_t *o = FC.DT.dot[r][k];
            if (!st) {
                hm_show(o, false);
                continue;
            }
            int d = st == 1 ? 20 : 9;
            lv_obj_set_size(o, d, d);
            lv_obj_set_pos(o, DT_X + k * DT_CELL + (DT_CELL - d) / 2, DT_Y + r * DT_CELL + (DT_CELL - d) / 2);
            fill_rgb(o, st == 1 ? ink : halo);
            lv_obj_set_style_bg_opa(o, st == 1 ? LV_OPA_COVER : (lv_opa_t)(dark ? 150 : 70), 0);
            hm_show(o, true);
        }
    char d[48], w[48];
    date_text(tm, d, sizeof d, false);
    weather_text(w, sizeof w);
    if (w[0]) ui_text(FC.DT.caption, "%s \xc2\xb7 %s", d, w);
    else ui_text(FC.DT.caption, "%s", d);
}

/* ---------------------------------------------------------------- the words */

static const char *const WD_ROWS[WD_R] = {
    "itlisasampm", "acquarterdc", "twentyfivex", "halfstenfto", "pasterunine",
    "onesixthree", "fourfivetwo", "eighteleven", "seventwelve", "tenseoclock",
};

typedef struct { const char *w; uint8_t r, c, n; } word_t;
enum { W_IT, W_IS, W_QUARTER, W_TWENTY, W_FIVE, W_HALF, W_TEN, W_TO, W_PAST, W_OCLOCK, W_H1 };
static const word_t WORDS[] = {
    { "it", 0, 0, 2 },       { "is", 0, 3, 2 },     { "quarter", 1, 2, 7 }, { "twenty", 2, 0, 6 },
    { "five", 2, 6, 4 },     { "half", 3, 0, 4 },   { "ten", 3, 5, 3 },     { "to", 3, 9, 2 },
    { "past", 4, 0, 4 },     { "oclock", 9, 5, 6 },
    /* the hours, one to twelve */
    { "one", 5, 0, 3 },      { "two", 6, 8, 3 },    { "three", 5, 6, 5 },   { "four", 6, 0, 4 },
    { "five", 6, 4, 4 },     { "six", 5, 3, 3 },    { "seven", 8, 0, 5 },   { "eight", 7, 0, 5 },
    { "nine", 4, 7, 4 },     { "ten", 9, 0, 3 },    { "eleven", 7, 5, 6 },  { "twelve", 8, 5, 6 },
};

static void words_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_WORDS);
    for (int r = 0; r < WD_R; r++)
        for (int k = 0; k < WD_C; k++) {
            char s[2] = { WD_ROWS[r][k], 0 };
            lv_obj_t *l = bz_label(b, s, BZ_F_VALUE, BZ_C_FAINT);
            lv_obj_set_width(l, WD_CW);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, WD_X + k * WD_CW, WD_Y + r * WD_RH);
            FC.WD.cell[r][k] = l;
            FC.WD.lit[r][k] = 2; /* neither: coloured on the first minute */
        }
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = disc(b, 10);
        int x = i == 0 || i == 3 ? WD_X - 26 : WD_X + WD_C * WD_CW + 16;
        int y = i < 2 ? WD_Y + 18 : WD_Y + WD_R * WD_RH - 30;
        lv_obj_set_pos(o, x, y);
        FC.WD.corner[i] = o;
    }
    FC.WD.corners = -1;
}

static void words_minute(const struct tm *tm, bool force)
{
    uint8_t on[WD_R][WD_C];
    memset(on, 0, sizeof on);
    int lit[8], n = 0;
    lit[n++] = W_IT;
    lit[n++] = W_IS;
    int m = tm->tm_min / 5 * 5, h = tm->tm_hour;
    switch (m) {
    case 0: lit[n++] = W_OCLOCK; break;
    case 5: lit[n++] = W_FIVE; lit[n++] = W_PAST; break;
    case 10: lit[n++] = W_TEN; lit[n++] = W_PAST; break;
    case 15: lit[n++] = W_QUARTER; lit[n++] = W_PAST; break;
    case 20: lit[n++] = W_TWENTY; lit[n++] = W_PAST; break;
    case 25: lit[n++] = W_TWENTY; lit[n++] = W_FIVE; lit[n++] = W_PAST; break;
    case 30: lit[n++] = W_HALF; lit[n++] = W_PAST; break;
    case 35: lit[n++] = W_TWENTY; lit[n++] = W_FIVE; lit[n++] = W_TO; h++; break;
    case 40: lit[n++] = W_TWENTY; lit[n++] = W_TO; h++; break;
    case 45: lit[n++] = W_QUARTER; lit[n++] = W_TO; h++; break;
    case 50: lit[n++] = W_TEN; lit[n++] = W_TO; h++; break;
    default: lit[n++] = W_FIVE; lit[n++] = W_TO; h++; break;
    }
    int h12 = h % 12 ? h % 12 : 12;
    lit[n++] = W_H1 + h12 - 1;
    if (clock_set(tm))
        for (int i = 0; i < n; i++) {
            const word_t *w = &WORDS[lit[i]];
            for (int k = 0; k < w->n; k++) on[w->r][w->c + k] = 1;
        }
    uint32_t ink = bz_color(BZ_C_INK), off = hpx_mix(bz_color(BZ_C_GROUND), bz_color(BZ_C_FAINT), bz_ui_dark() ? 130 : 150);
    for (int r = 0; r < WD_R; r++)
        for (int k = 0; k < WD_C; k++) {
            if (on[r][k] == FC.WD.lit[r][k] && !force) continue;
            FC.WD.lit[r][k] = on[r][k];
            text_rgb(FC.WD.cell[r][k], on[r][k] ? ink : off);
        }
    int past = clock_set(tm) ? tm->tm_min % 5 : 0;
    if (past != FC.WD.corners || force) {
        FC.WD.corners = past;
        for (int i = 0; i < 4; i++) fill_rgb(FC.WD.corner[i], i < past ? ink : off);
    }
}

/* ---------------------------------------------------------------- night */

static void night_build(void)
{
    lv_obj_t *b = face_box(HM_FACE_NIGHT);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    FC.NT.time = bz_label(b, "", BZ_F_CLOCK, BZ_C_DIM);
    lv_obj_align(FC.NT.time, LV_ALIGN_CENTER, 0, -30);
    FC.NT.date = bz_label(b, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(FC.NT.date, LV_ALIGN_CENTER, 0, 60);
    /* warm and very dim, whatever the tone: this is what a dark bedroom sees */
    text_rgb(FC.NT.time, 0x5A4C46);
    text_rgb(FC.NT.date, 0x3A312D);
}

static void night_minute(const struct tm *tm)
{
    char d[48];
    if (clock_set(tm)) ui_text(FC.NT.time, "%d:%02d", tm->tm_hour, tm->tm_min);
    else ui_text(FC.NT.time, "%s", "--:--");
    date_text(tm, d, sizeof d, false);
    ui_text(FC.NT.date, "%s", d);
    lv_obj_align(FC.NT.time, LV_ALIGN_CENTER, 0, -30);
    lv_obj_align(FC.NT.date, LV_ALIGN_CENTER, 0, 60);
}

/* ---------------------------------------------------------------- the faces together */

static void face_build(int f)
{
    if (FC.face[f]) return;
    switch (f) {
    case HM_FACE_DIAL: dial_build(); break;
    case HM_FACE_HORIZON: horizon_build(); break;
    case HM_FACE_DOTS: dots_build(); break;
    case HM_FACE_WORDS: words_build(); break;
    case HM_FACE_MOON: moon_build(); break;
    default: night_build(); break;
    }
}

/* its words and pictures brought up to the minute (and its colours to the tone, when `force`) */
static void face_minute(int f, bool force)
{
    if (f < 0 || !FC.face[f]) return;
    struct tm tm = now_tm();
    switch (f) {
    case HM_FACE_DIAL:
        if (force) dial_colors(bz_ui_dark());
        dial_minute(&tm);
        break;
    case HM_FACE_HORIZON: horizon_minute(&tm, force); break;
    case HM_FACE_DOTS:
        if (force) dots_tile(bz_ui_dark());
        dots_minute(&tm, force);
        break;
    case HM_FACE_WORDS: words_minute(&tm, force); break;
    case HM_FACE_MOON: moon_minute(&tm, force); break;
    default: night_minute(&tm); break;
    }
}

static pic_t *face_pic(int f, lv_obj_t **img)
{
    if (f == HM_FACE_HORIZON && FC.face[f]) {
        *img = FC.HZ.img;
        return &FC.HZ.pic;
    }
    if (f == HM_FACE_MOON && FC.face[f]) {
        *img = FC.MN.img;
        return &FC.MN.pic;
    }
    return NULL;
}

/* a face's picture painted: shown */
static void pic_poll(int f)
{
    lv_obj_t *img;
    pic_t *p = face_pic(f, &img);
    if (!p || !p->px || p->painted || !hm_art_job_done(p->ticket)) return;
    p->painted = true;
    lv_image_cache_drop(&p->dsc);
    lv_image_set_src(img, &p->dsc);
    lv_obj_invalidate(img);
    hm_show(img, true);
}

static bool pic_ready(int f)
{
    lv_obj_t *img;
    pic_t *p = face_pic(f, &img);
    return !p || !p->px || hm_art_job_done(p->ticket);
}

/* a face that went: its picture's memory back */
static void face_leave(int f)
{
    lv_obj_t *img;
    pic_t *p = face_pic(f, &img);
    if (p && p->px && pic_free(p)) {
        hm_show(img, false);
        if (f == HM_FACE_HORIZON) FC.HZ.slot = -1;
        if (f == HM_FACE_MOON) FC.MN.day = -1;
    }
    if (f >= 0 && FC.face[f]) hm_show(FC.face[f], false);
}

static void index_show(void)
{
    int faces = hm_cfg()->faces, n = 0;
    for (int i = 0; i < HM_NFACES; i++) n += faces >> i & 1;
    int sig = night_face(FC.cur) || n < 2 ? -2 : FC.cur * 64 + faces;
    if (sig == FC.index_shown) return;
    FC.index_shown = sig;
    hm_show(FC.index, sig != -2);
    if (sig == -2) return;
    int y = 0;
    for (int i = 0; i < HM_NFACES; i++) {
        bool in = faces >> i & 1;
        hm_show(FC.index_dot[i], in);
        if (!in) continue;
        bool on = i == FC.cur;
        lv_obj_set_size(FC.index_dot[i], 8, on ? 22 : 8);
        lv_obj_set_pos(FC.index_dot[i], 0, y);
        lv_obj_set_style_bg_color(FC.index_dot[i], bz_lv(on ? BZ_C_INK : BZ_C_FAINT), 0);
        y += (on ? 22 : 8) + 8;
    }
    lv_obj_set_size(FC.index, 8, y);
    lv_obj_set_pos(FC.index, W - 26, (H - y) / 2);
}

static void flick(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
    if (dy < -50 || vy < -500) hm_faces_step(1);
    else if (dy > 50 || vy > 500) hm_faces_step(-1);
}

lv_obj_t *hm_faces_build(lv_obj_t *parent)
{
    FC.pg = bz_box(parent);
    lv_obj_set_size(FC.pg, W, H);
    lv_obj_add_style(FC.pg, bz_style_fill(BZ_C_GROUND), 0);
    FC.index = bz_box(FC.pg);
    for (int i = 0; i < HM_NFACES; i++) {
        lv_obj_t *d = bz_box(FC.index);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        FC.index_dot[i] = d;
    }
    bz_motion_init(&FC.k, 0, 0.001f);
    FC.k.keep = true;
    /* a flick up or down: the next face, or the one before (sideways goes on to home mode's own pages) */
    bz_drag_t d = { .end = flick, .axis = 2, .slop = 12 };
    bz_drag_attach(FC.pg, &d);
    return FC.pg;
}

void hm_faces_set(int f)
{
    if (f < 0 || f > HM_FACE_NIGHT) f = HM_FACE_DIAL;
    if (f == FC.cur && FC.face[f]) return;
    int old = FC.cur;
    face_build(f);
    FC.cur = f;
    FC.dark = bz_ui_dark();
    face_minute(f, true);
    hm_show(FC.face[f], true);
    if (old >= 0 && old != f) face_leave(old);
    lv_obj_move_foreground(FC.index);
    FC.index_shown = -1;
    index_show();
    FC.D.shown_sec = -1;
}

int hm_faces_face(void) { return FC.cur; }

void hm_faces_look(void) { FC.last_min = -1; }

bool hm_faces_night(void)
{
    if (!hm_cfg()->night) return false;
    struct tm tm = now_tm();
    return clock_set(&tm) && (tm.tm_hour >= 22 || tm.tm_hour < 7);
}

void hm_faces_step(int dir)
{
    if (FC.stepping || night_face(FC.cur)) return;
    int faces = hm_cfg()->faces & HM_FACES_ALL;
    int f = FC.cur;
    for (int i = 0; i < HM_NFACES; i++) {
        f = (f + (dir > 0 ? 1 : HM_NFACES - 1)) % HM_NFACES;
        if (faces >> f & 1) break;
    }
    if (f == FC.cur || !(faces >> f & 1)) return;
    FC.stepping = true;
    FC.step_to = f;
    FC.step_dir = dir;
    face_build(f);
    face_minute(f, true); /* its picture starts painting now; the sheet waits for it */
    hm_cfg()->face = f;
    hm_cfg_save();
}

bool hm_faces_moving(void) { return FC.stepping; }

void hm_faces_refresh(bool shown)
{
    if (FC.cur < 0) return;
    bool dark = bz_ui_dark();
    bool tone = (int)dark != FC.dark;
    FC.dark = dark;
    pic_poll(FC.cur);
    if (FC.stepping) pic_poll(FC.step_to);
    (void)shown; /* hidden, its labels change without drawing: kept current so a slide shows it right */
    struct tm tm = now_tm();
    home_weather_t w;
    home_weather_get(&w);
    if (tm.tm_min != FC.last_min || tone || w.gen != FC.wx_gen) {
        FC.last_min = tm.tm_min;
        FC.wx_gen = w.gen;
        face_minute(FC.cur, tone);
    }
    index_show();
}

/* a finished step: the sheet taken away, the old face's picture freed */
static void step_frame(void)
{
    if (!FC.stepping) return;
    bz_ui_keep_alive();
    if (!FC.sheet) {
        /* waiting for the new face's picture, then the state switched under a sheet that shows the old one */
        if (!pic_ready(FC.step_to)) return;
        pic_poll(FC.step_to);
        int old = FC.cur;
        bool sheet = bz_ui_sheet_begin(true, H, FC.step_dir > 0, NULL, NULL);
        FC.cur = FC.step_to;
        hm_show(FC.face[FC.cur], true);
        hm_show(FC.face[old], false);
        FC.index_shown = -1;
        index_show();
        FC.D.shown_sec = -1;
        face_leave(old);
        if (!sheet) {
            FC.stepping = false;
            return;
        }
        FC.sheet = true;
        bz_motion_init(&FC.k, 0, 0.001f);
        FC.k.keep = true;
        bz_motion_to(&FC.k, 1, BZ_RELEASE);
        return;
    }
    bool moving = bz_motion_tick(&FC.k);
    float k = FC.k.value < 0 ? 0 : FC.k.value > 1 ? 1 : FC.k.value;
    if (moving) {
        bz_ui_sheet((int)(k * H + 0.5f));
        return;
    }
    if (bz_ui_sheet_shown() != H) {
        bz_ui_sheet(H);
        return;
    }
    bz_ui_sheet_end();
    FC.sheet = false;
    FC.stepping = false;
}

void hm_faces_frame(double now)
{
    (void)now;
    step_frame();
    if (FC.cur == HM_FACE_DIAL && !FC.stepping) dial_frame();
}

void hm_faces_release(void)
{
    for (int f = 0; f <= HM_FACE_NIGHT; f++) {
        lv_obj_t *img;
        pic_t *p = face_pic(f, &img);
        if (p && p->px && pic_free(p)) hm_show(img, false);
    }
    if (FC.face[HM_FACE_HORIZON]) FC.HZ.slot = -1;
    if (FC.face[HM_FACE_MOON]) FC.MN.day = -1;
    FC.last_min = -1;
}

bool hm_faces_released(void)
{
    for (int f = 0; f <= HM_FACE_NIGHT; f++) {
        lv_obj_t *img;
        pic_t *p = face_pic(f, &img);
        if (p && p->px) return false;
    }
    return true;
}
