/* The dials: a Braun-style clock and a chronograph with live sub-dials. Each plate (the dome, the rim, the tracks,
 * the sub-dials' wells) is one picture painted once per tone (home_scape.c); the hands are LVGL lines over it that
 * move once a minute and redraw only their own boxes, and the one moving part at rest, a second hand, is drawn by
 * an object over the plate that invalidates a few small boxes along itself each tick. */
#include "ui_home_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TAU 6.2831853f
#define DS 640                 /* the plate's picture */
#define DR 300                 /* the plate's radius */
#define DCX 640
#define DCY 360
#define SEG 6                  /* a sweeping hand's redraw, in pieces along it */

typedef struct {
    lv_obj_t *o;
    lv_point_precise_t p[2];
    int x, y, w, h, width;
} line_t;

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

/* the line from (x0, y0) to (x1, y1), in an object only as big as the line: moving it redraws its old box and its
 * new one, never the plate */
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

/* a hand from the centre (cx, cy) at `turns` of a revolution: its tail behind the centre, its tip ahead */
static void hand_at(line_t *l, float cx, float cy, float turns, float tail, float tip, float dx, float dy)
{
    float a = turns * TAU, s = sinf(a), c = -cosf(a);
    line_to(l, cx - s * tail + dx, cy - c * tail + dy, cx + s * tip + dx, cy + c * tip + dy);
}

/* ---- a sweeping hand: steps into each second on a spring, drawn by an object over its dial ---- */

typedef struct {
    lv_obj_t *o;
    float cx, cy, tail, tip, cap; /* in its object's coordinates (the face's, from 0, 0) */
    int width;
    lv_color_t color;
    float drawn;                  /* the turn drawn, -1 none */
    bz_motion_t m;
    long long target;
    int shown;
} sweep_t;

static void sweep_draw(lv_event_t *e)
{
    sweep_t *h = lv_event_get_user_data(e);
    if (h->drawn < 0) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(h->o, &co);
    float a = h->drawn * TAU, s = sinf(a), c = -cosf(a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = h->color;
    d.width = h->width;
    d.opa = LV_OPA_COVER;
    d.round_start = d.round_end = 1;
    float cx = co.x1 + h->cx, cy = co.y1 + h->cy;
    d.p1.x = cx - s * h->tail;
    d.p1.y = cy - c * h->tail;
    d.p2.x = cx + s * h->tip;
    d.p2.y = cy + c * h->tip;
    lv_draw_line(layer, &d);
    if (h->cap > 0) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = h->color;
        r.bg_opa = LV_OPA_COVER;
        r.radius = LV_RADIUS_CIRCLE;
        int k = (int)h->cap;
        lv_area_t cap = { (int32_t)cx - k, (int32_t)cy - k, (int32_t)cx + k - 1, (int32_t)cy + k - 1 };
        lv_draw_rect(layer, &r, &cap);
    }
}

static void sweep_make(sweep_t *h, lv_obj_t *parent, float cx, float cy, float tail, float tip, float cap, int width)
{
    h->o = bz_box(parent);
    lv_obj_set_size(h->o, W, H);
    lv_obj_set_pos(h->o, 0, 0);
    lv_obj_add_flag(h->o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(h->o, sweep_draw, LV_EVENT_DRAW_MAIN, h);
    h->cx = cx;
    h->cy = cy;
    h->tail = tail;
    h->tip = tip;
    h->cap = cap;
    h->width = width;
    h->drawn = -1;
    h->shown = -1;
    bz_motion_init(&h->m, 0, 0.01f);
    h->m.keep = true;
}

/* the small boxes along the hand, each covering where that piece was and where it goes */
static void sweep_invalidate(sweep_t *h, float from, float to)
{
    if (to < 0) return;
    if (from < 0) from = to;
    float sa = sinf(from * TAU), ca = -cosf(from * TAU), sb = sinf(to * TAU), cb = -cosf(to * TAU);
    int pad = h->width / 2 + 3;
    for (int i = 0; i < SEG; i++) {
        float r0 = -h->tail + (h->tip + h->tail) * i / SEG, r1 = -h->tail + (h->tip + h->tail) * (i + 1) / SEG;
        float xs[4] = { h->cx + sa * r0, h->cx + sa * r1, h->cx + sb * r0, h->cx + sb * r1 };
        float ys[4] = { h->cy + ca * r0, h->cy + ca * r1, h->cy + cb * r0, h->cy + cb * r1 };
        float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
        for (int k = 1; k < 4; k++) {
            x1 = fminf(x1, xs[k]);
            x2 = fmaxf(x2, xs[k]);
            y1 = fminf(y1, ys[k]);
            y2 = fmaxf(y2, ys[k]);
        }
        lv_area_t ar = { (int32_t)floorf(x1) - pad, (int32_t)floorf(y1) - pad, (int32_t)ceilf(x2) + pad,
                         (int32_t)ceilf(y2) + pad };
        if (r0 < 0 && r1 > 0) {
            int c = (int)h->cap + 3;
            ar.x1 = LV_MIN(ar.x1, (int32_t)h->cx - c);
            ar.y1 = LV_MIN(ar.y1, (int32_t)h->cy - c);
            ar.x2 = LV_MAX(ar.x2, (int32_t)h->cx + c);
            ar.y2 = LV_MAX(ar.y2, (int32_t)h->cy + c);
        }
        lv_obj_invalidate_area(h->o, &ar);
    }
}

static void sweep_set(sweep_t *h, float turns)
{
    if (fabsf(turns - h->drawn) < 0.0004f) return;
    sweep_invalidate(h, h->drawn, turns);
    h->drawn = turns;
}

/* every frame: into each second on the tick spring (a quartz movement settling into its detent), still between */
static void sweep_frame(sweep_t *h)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now = (long long)tv.tv_sec;
    if (h->shown < 0 || llabs(now - h->target) > 3) {
        h->target = now;
        bz_motion_set(&h->m, (float)(now % 60), 0);
        h->shown = (int)(now % 60);
    } else if (now != h->target) {
        h->target = now;
        float v = h->m.value, to = (float)(now % 60);
        if (to < v - 30) v -= 60; /* past the top: carry on round, not back */
        bz_motion_set(&h->m, v, h->m.velocity);
        static const bz_spring_t QUARTZ = { 1600.0f, 52.0f, 1.0f };
        bz_motion_to(&h->m, to, QUARTZ);
    }
    bool moving = bz_motion_tick(&h->m);
    float t = h->m.value / 60.0f;
    sweep_set(h, t - floorf(t));
    if (moving) bz_ui_keep_alive();
}

/* ---- the plates ----
 * What is soft on a plate (its dome, the rim's light, its shadow, the sub-dials' wells) is painted at half size
 * (205 KB rather than 800) and shown doubled; what must be sharp (the tracks, the hour bars, the sub-dials'
 * scales) is drawn over it by LVGL, the marks crossing the area being redrawn and no others. */

#define PS (DS / 2)
#define PR (DR / 2.0f)
#define NTICK 480

typedef struct {
    hm_pic_t pic;
    bool chrono;
    lv_obj_t *marks;
    hs_tick_t tick[NTICK];
    int ntick;
    lv_color_t major, minor;
} plate_t;

static void plate_paint(void *arg)
{
    plate_t *p = arg;
    if (!p->pic.px) return;
    hs_dial(p->pic.px, PS, PR, p->chrono, &p->pic.tone);
}

static void marks_draw(lv_event_t *e)
{
    plate_t *p = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t *clip = &layer->_clip_area;
    lv_area_t co;
    lv_obj_get_coords(p->marks, &co);
    float cx = co.x1 + DR, cy = co.y1 + DR;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    for (int i = 0; i < p->ntick; i++) {
        const hs_tick_t *t = &p->tick[i];
        float x0 = cx + t->x0, y0 = cy + t->y0, x1 = cx + t->x1, y1 = cy + t->y1;
        if (fmaxf(x0, x1) + 4 < clip->x1 || fminf(x0, x1) - 4 > clip->x2 || fmaxf(y0, y1) + 4 < clip->y1 ||
            fminf(y0, y1) - 4 > clip->y2)
            continue;
        d.width = (int32_t)lroundf(t->w);
        d.color = t->major ? p->major : p->minor;
        d.opa = t->major ? LV_OPA_COVER : (lv_opa_t)200;
        d.p1.x = x0;
        d.p1.y = y0;
        d.p2.x = x1;
        d.p2.y = y1;
        lv_draw_line(layer, &d);
    }
}

static void plate_make(plate_t *p, lv_obj_t *b, bool chrono)
{
    p->chrono = chrono;
    hm_pic_init(&p->pic, b, DCX - PS / 2, DCY - PS / 2, PS, PS);
    lv_image_set_pivot(p->pic.img, PS / 2, PS / 2);
    lv_image_set_scale(p->pic.img, 512);
    p->marks = bz_box(b);
    lv_obj_set_size(p->marks, 2 * DR, 2 * DR);
    lv_obj_set_pos(p->marks, DCX - DR, DCY - DR);
    lv_obj_add_flag(p->marks, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(p->marks, marks_draw, LV_EVENT_DRAW_MAIN, p);
    p->ntick = hs_dial_ticks(DR, chrono, p->tick, NTICK);
}

static void plate_colors(plate_t *p)
{
    hm_tone(&p->pic.tone);
    uint32_t face = hs_dial_face(&p->pic.tone);
    p->major = bz_lv(BZ_C_INK);
    p->minor = bz_lv_rgb(bz_mix(face, bz_color(BZ_C_DIM), bz_ui_dark() ? 0.75f : 0.85f));
    lv_obj_invalidate(p->marks);
    hm_pic_start(&p->pic, plate_paint, p);
}

/* ================================================================ the dial */

static struct {
    plate_t plate;
    lv_obj_t *num[12], *datebox, *date, *top_l, *top_r;
    line_t hand[2], shadow[2];
    lv_obj_t *cap;
    sweep_t sec;
} DL;

static void dial_build(lv_obj_t *b)
{
    plate_make(&DL.plate, b, false);
    /* the hours in Braun's manner: every numeral, small and upright, inside the track */
    for (int i = 0; i < 12; i++) {
        char s[4];
        snprintf(s, sizeof s, "%d", i ? i : 12);
        DL.num[i] = bz_label(b, s, BZ_F_NAME, BZ_C_INK);
        float a = i / 12.0f * TAU, r = DR - 84;
        int w = hm_text_w(s, bz_font(BZ_F_NAME)), hh = lv_font_get_line_height(bz_font(BZ_F_NAME));
        lv_obj_set_pos(DL.num[i], DCX + (int)lroundf(sinf(a) * r) - w / 2, DCY - (int)lroundf(cosf(a) * r) - hh / 2);
    }
    hm_show(DL.num[3], false); /* the date window has three's place */
    DL.datebox = bz_box(b);
    lv_obj_set_size(DL.datebox, 56, 40);
    lv_obj_set_style_radius(DL.datebox, 8, 0);
    lv_obj_set_style_bg_opa(DL.datebox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(DL.datebox, 1, 0);
    lv_obj_set_pos(DL.datebox, DCX + DR - 84 - 28, DCY - 20);
    DL.date = bz_label(DL.datebox, "", BZ_F_LABEL, BZ_C_INK);
    lv_obj_center(DL.date);
    /* shadows first, then the hands over them, the second hand over everything */
    line_make(&DL.shadow[0], b, 16);
    line_make(&DL.shadow[1], b, 11);
    line_make(&DL.hand[0], b, 14);
    line_make(&DL.hand[1], b, 9);
    DL.cap = bz_box(b);
    lv_obj_set_size(DL.cap, 26, 26);
    lv_obj_set_style_radius(DL.cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(DL.cap, LV_OPA_COVER, 0);
    lv_obj_set_pos(DL.cap, DCX - 13, DCY - 13);
    sweep_make(&DL.sec, b, DCX, DCY, 62, DR - 26, 8, 3);
    DL.top_l = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_pos(DL.top_l, 56, 44);
    DL.top_r = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_style_text_align(DL.top_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(DL.top_r, W - 56 - 300, 668);
}

static void hands_colors(line_t *hand, line_t *shadow, lv_obj_t *cap, bool dark)
{
    uint32_t ink = bz_color(BZ_C_INK);
    for (int i = 0; i < 2; i++) line_color(&shadow[i], 0x000000, dark ? 110 : 46);
    line_color(&hand[0], ink, 255);
    line_color(&hand[1], ink, 255);
    lv_obj_set_style_bg_color(cap, bz_lv_rgb(ink), 0);
}

/* the hours and minutes for a time, shadows falling down and to the right of the light at the top left */
static void hands_set(line_t *hand, line_t *shadow, float cx, float cy, const struct tm *tm, float R)
{
    float h = ((tm->tm_hour % 12) + tm->tm_min / 60.0f) / 12.0f, m = (tm->tm_min + tm->tm_sec / 60.0f) / 60.0f;
    const float sx = 4, sy = 9;
    hand_at(&shadow[0], cx, cy, h, 26, R * 0.56f, sx, sy);
    hand_at(&shadow[1], cx, cy, m, 30, R * 0.86f, sx, sy);
    hand_at(&hand[0], cx, cy, h, 26, R * 0.56f, 0, 0);
    hand_at(&hand[1], cx, cy, m, 30, R * 0.86f, 0, 0);
}

static void dial_minute(const struct tm *tm, bool force)
{
    bool dark = bz_ui_dark();
    if (force) {
        plate_colors(&DL.plate);
        hands_colors(DL.hand, DL.shadow, DL.cap, dark);
        DL.sec.color = bz_lv(BZ_C_SIGNAL);
        uint32_t ink = bz_color(BZ_C_INK);
        for (int i = 0; i < 12; i++) hm_color(DL.num[i], ink);
        uint32_t face = dark ? bz_mix(bz_color(BZ_C_SURFACE1), bz_color(BZ_C_GROUND), 0.25f) : 0xFBFBFA;
        lv_obj_set_style_bg_color(DL.datebox, bz_lv_rgb(dark ? bz_mix(face, 0x000000, 0.4f) : 0xFFFFFF), 0);
        lv_obj_set_style_border_color(DL.datebox, bz_lv_rgb(bz_mix(face, ink, 0.25f)), 0);
        DL.sec.shown = -1;
        lv_obj_invalidate(DL.sec.o);
    }
    char d[40], w[48];
    hm_date(tm, d, sizeof d, true);
    ui_text(DL.top_l, "%s", d);
    hm_weather_line(w, sizeof w);
    ui_text(DL.top_r, "%s", w);
    ui_text(DL.date, "%d", hm_clock_set(tm) ? tm->tm_mday : 0);
    hands_set(DL.hand, DL.shadow, DCX, DCY, tm, DR);
    hm_show(DL.sec.o, hm_cfg()->dial_secs);
}

static void dial_tick(void) { hm_pic_poll(&DL.plate.pic); }

static void dial_frame(double now)
{
    (void)now;
    if (hm_cfg()->dial_secs && DL.plate.pic.painted) sweep_frame(&DL.sec);
}

static bool dial_ready(void) { return !hm_pic_busy(&DL.plate.pic); }
static void dial_leave(void) { hm_pic_free(&DL.plate.pic); }
static bool dial_held(void) { return hm_pic_held(&DL.plate.pic); }

const hm_face_t HM_F_DIAL = { .name = "dial", .build = dial_build, .minute = dial_minute, .tick = dial_tick,
                              .frame = dial_frame, .ready = dial_ready, .leave = dial_leave, .held = dial_held };

/* ================================================================ the chronograph */

static struct {
    plate_t plate;
    line_t hand[2], shadow[2], batt, match;
    lv_obj_t *cap, *sub_l[3], *sub_v[3], *top_l, *top_r;
    sweep_t sec;
    float sx[3], sy[3], sr[3];
    int batt_shown;
    char match_shown[40];
} CH;

static void chrono_build(lv_obj_t *b)
{
    plate_make(&CH.plate, b, true);
    static const char *const NAME[3] = { "sec", "battery", "next" };
    for (int i = 0; i < 3; i++) {
        float dx, dy, r;
        hs_chrono_sub(DR, i, &dx, &dy, &r);
        /* the picture is DS square about the dial's centre: its pixel centre is DS / 2 */
        CH.sx[i] = DCX + dx;
        CH.sy[i] = DCY + dy;
        CH.sr[i] = r;
        CH.sub_l[i] = bz_label_line(b, NAME[i], BZ_F_CAPTION, BZ_C_DIM, (int)(r * 1.6f));
        lv_obj_set_style_text_align(CH.sub_l[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(CH.sub_l[i], (int)(CH.sx[i] - r * 0.8f), (int)(CH.sy[i] - r * 0.52f));
        CH.sub_v[i] = bz_label_line(b, "", BZ_F_LABEL, BZ_C_INK, (int)(r * 1.6f));
        lv_obj_set_style_text_align(CH.sub_v[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(CH.sub_v[i], (int)(CH.sx[i] - r * 0.8f), (int)(CH.sy[i] + r * 0.22f));
    }
    hm_show(CH.sub_v[0], false);
    line_make(&CH.batt, b, 4);
    line_make(&CH.match, b, 4);
    line_make(&CH.shadow[0], b, 16);
    line_make(&CH.shadow[1], b, 11);
    line_make(&CH.hand[0], b, 14);
    line_make(&CH.hand[1], b, 9);
    CH.cap = bz_box(b);
    lv_obj_set_size(CH.cap, 24, 24);
    lv_obj_set_style_radius(CH.cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(CH.cap, LV_OPA_COVER, 0);
    lv_obj_set_pos(CH.cap, DCX - 12, DCY - 12);
    /* the small seconds at nine */
    sweep_make(&CH.sec, b, CH.sx[0], CH.sy[0], 12, CH.sr[0] - 10, 4, 2);
    CH.top_l = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_pos(CH.top_l, 56, 44);
    CH.top_r = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 300);
    lv_obj_set_style_text_align(CH.top_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(CH.top_r, W - 56 - 300, 668);
    CH.batt_shown = -2;
}

/* the next match, for the six o'clock sub-dial: minutes to it (0..60+), its label; false: none */
static bool next_match(int *mins, char *label, size_t n)
{
    tba_match_t m;
    time_t when;
    if (!tba_next_match(&m, &when) || !when) return false;
    double d = difftime(when, time(NULL)) / 60.0;
    if (d < -10) return false;
    *mins = d < 0 ? 0 : (int)lround(d);
    snprintf(label, n, "%s", m.label);
    return true;
}

static void chrono_minute(const struct tm *tm, bool force)
{
    bool dark = bz_ui_dark();
    if (force) {
        plate_colors(&CH.plate);
        hands_colors(CH.hand, CH.shadow, CH.cap, dark);
        line_color(&CH.batt, bz_color(BZ_C_INK), 255);
        line_color(&CH.match, bz_color(BZ_C_SIGNAL), 255);
        CH.sec.color = bz_lv(BZ_C_INK);
        CH.sec.shown = -1;
        CH.batt_shown = -2;
        CH.match_shown[0] = 1;
        lv_obj_invalidate(CH.sec.o);
    }
    char d[40], w[48];
    hm_date(tm, d, sizeof d, true);
    ui_text(CH.top_l, "%s", d);
    hm_weather_line(w, sizeof w);
    ui_text(CH.top_r, "%s", w);
    hands_set(CH.hand, CH.shadow, DCX, DCY, tm, DR);
    /* three o'clock: the tablet's battery, 0 to 100 once round */
    hal_battery_t bat;
    int pct = hal_battery(&bat) && bat.ok ? bat.percent : -1;
    if (pct != CH.batt_shown) {
        CH.batt_shown = pct;
        float t = pct < 0 ? 0 : pct / 100.0f * 0.999f;
        hand_at(&CH.batt, CH.sx[1], CH.sy[1], t, 8, CH.sr[1] - 12, 0, 0);
        if (pct < 0) ui_text(CH.sub_v[1], "%s", "\xe2\x80\x94");
        else ui_text(CH.sub_v[1], "%d%%%s", pct, bat.charging ? " +" : "");
    }
    /* six o'clock: the minutes to the team's next match, up to an hour; parked at the top without one */
    int mins = 0;
    char lab[16], sig[40];
    bool have = next_match(&mins, lab, sizeof lab);
    snprintf(sig, sizeof sig, "%d %s %d", have, have ? lab : "", mins);
    if (strcmp(sig, CH.match_shown)) {
        snprintf(CH.match_shown, sizeof CH.match_shown, "%s", sig);
        float t = have ? (mins > 60 ? 60 : mins) / 60.0f * 0.999f : 0;
        hand_at(&CH.match, CH.sx[2], CH.sy[2], t, 8, CH.sr[2] - 12, 0, 0);
        hm_show(CH.match.o, have);
        if (!have) ui_text(CH.sub_v[2], "%s", "no match");
        else if (mins > 60) ui_text(CH.sub_v[2], "%s %dh", lab, (mins + 30) / 60);
        else ui_text(CH.sub_v[2], "%s %dm", lab, mins);
    }
}

static void chrono_tick(void) { hm_pic_poll(&CH.plate.pic); }

static void chrono_frame(double now)
{
    (void)now;
    if (CH.plate.pic.painted) sweep_frame(&CH.sec);
}

static bool chrono_ready(void) { return !hm_pic_busy(&CH.plate.pic); }
static void chrono_leave(void) { hm_pic_free(&CH.plate.pic); }
static bool chrono_held(void) { return hm_pic_held(&CH.plate.pic); }

const hm_face_t HM_F_CHRONO = { .name = "chrono", .build = chrono_build, .minute = chrono_minute,
                                .tick = chrono_tick, .frame = chrono_frame, .ready = chrono_ready,
                                .leave = chrono_leave, .held = chrono_held };
