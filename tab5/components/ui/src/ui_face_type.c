/* The type faces: the poster, the split-flap clock, the word clock and the night face. LVGL objects only, no
 * picture at all: a minute redraws the digits that changed, nothing else. */
#include "ui_home_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void bottom_rule_line(char *out, size_t n, const struct tm *tm)
{
    /* the ISO week and the day of the year: the grid's quiet footnote */
    char wk[8];
    strftime(wk, sizeof wk, "%V", tm);
    snprintf(out, n, "week %d \xc2\xb7 day %d", atoi(wk), tm->tm_yday + 1);
}

/* the next sunrise or sunset, whichever comes first: "sunset", "18:52" */
static void sun_next(char *what, size_t wn, char *when, size_t hn)
{
    double lat, lon;
    hm_where(&lat, &lon);
    double now = (double)time(NULL), rise, set, noon;
    what[0] = when[0] = 0;
    if (!astro_sun_day(now, lat, lon, hm_tz_s(), &rise, &set, &noon)) return;
    if (now < rise) {
        snprintf(what, wn, "sunrise");
        hm_hm_t((time_t)rise, when, hn);
    } else if (now < set) {
        snprintf(what, wn, "sunset");
        hm_hm_t((time_t)set, when, hn);
    } else if (astro_sun_day(now + 86400, lat, lon, hm_tz_s(), &rise, &set, &noon)) {
        snprintf(what, wn, "sunrise");
        hm_hm_t((time_t)rise, when, hn);
    }
}

/* ================================================================ the poster */

#define PO_X 44
#define PO_BASE 606
#define GRID_X0 56
#define GRID_W 396

static struct {
    lv_obj_t *hh, *colon, *mm, *top[3][2], *rule, *foot_l, *foot_r;
    char shown[8];
    const lv_font_t *font;     /* the numerals as last set out: they are set out again when it changes hands */
} PO;

static void poster_build(lv_obj_t *b)
{
    PO.hh = lv_label_create(b);
    PO.colon = lv_label_create(b);
    PO.mm = lv_label_create(b);
    lv_obj_t *big[3] = { PO.hh, PO.colon, PO.mm };
    for (int i = 0; i < 3; i++) {
        ui_face_font_bind(big[i], UI_FF_POSTER);
        lv_obj_add_flag(big[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_label_set_text_static(PO.colon, ":");
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 2; r++) {
            PO.top[c][r] = bz_label_line(b, "", BZ_F_LABEL, r ? BZ_C_DIM : BZ_C_INK, GRID_W - 24);
            lv_obj_set_pos(PO.top[c][r], GRID_X0 + c * GRID_W, 44 + r * 28);
        }
    PO.rule = bz_box(b);
    lv_obj_set_style_bg_opa(PO.rule, LV_OPA_COVER, 0);
    lv_obj_set_size(PO.rule, W - 2 * GRID_X0, 1);
    lv_obj_set_pos(PO.rule, GRID_X0, 640);
    PO.foot_l = bz_label_line(b, "", BZ_F_LABEL, BZ_C_SIGNAL, 560);
    lv_obj_set_pos(PO.foot_l, GRID_X0, 654);
    PO.foot_r = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 400);
    lv_obj_set_style_text_align(PO.foot_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(PO.foot_r, W - GRID_X0 - 400, 654);
}

static void poster_minute(const struct tm *tm, bool force)
{
    if (force) {
        hm_color(PO.hh, bz_color(BZ_C_INK));
        hm_color(PO.mm, bz_color(BZ_C_INK));
        hm_color(PO.colon, bz_color(BZ_C_SIGNAL));
        lv_obj_set_style_bg_color(PO.rule, bz_lv_rgb(bz_mix(bz_color(BZ_C_GROUND), bz_color(BZ_C_FAINT), 0.45f)), 0);
        PO.shown[0] = 0;
    }
    char hm[8];
    hm_hm(tm, hm, sizeof hm);
    const lv_font_t *pf = ui_face_font(UI_FF_POSTER);
    if (strcmp(hm, PO.shown) || pf != PO.font) {
        PO.font = pf;
        snprintf(PO.shown, sizeof PO.shown, "%s", hm);
        char *c = strchr(hm, ':');
        if (c) *c = 0;
        ui_text(PO.hh, "%s", hm);
        ui_text(PO.mm, "%s", c ? c + 1 : "");
        /* set in one line, left on the grid: the hours, the colon, the minutes, each where the one before ends */
        int x = PO_X;
        hm_baseline(PO.hh, x, PO_BASE);
        x += hm_text_w(hm, pf);
        hm_baseline(PO.colon, x, PO_BASE);
        x += hm_text_w(":", pf);
        hm_baseline(PO.mm, x, PO_BASE);
    }
    char a[48], b[48];
    if (hm_clock_set(tm)) {
        strftime(a, sizeof a, "%A", tm);
        for (char *p = a; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        strftime(b, sizeof b, "%e %B", tm);
        for (char *p = b; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        ui_text(PO.top[0][0], "%s", a);
        ui_text(PO.top[0][1], "%s", b[0] == ' ' ? b + 1 : b);
    } else {
        ui_text(PO.top[0][0], "%s", "the clock isn't set");
        ui_text(PO.top[0][1], "%s", "");
    }
    home_weather_t w;
    home_weather_get(&w);
    if (w.configured && w.ok) {
        ui_text(PO.top[1][0], "%.0f\xc2\xb0  %s", w.temp, home_weather_text(w.code));
        ui_text(PO.top[1][1], "h %.0f\xc2\xb0  l %.0f\xc2\xb0", w.hi, w.lo);
    } else {
        ui_text(PO.top[1][0], "%s", "");
        ui_text(PO.top[1][1], "%s", "");
    }
    sun_next(a, sizeof a, b, sizeof b);
    ui_text(PO.top[2][0], "%s", a);
    ui_text(PO.top[2][1], "%s", b);
    char nx[96];
    if (ui_match_next_line(nx, sizeof nx)) ui_text(PO.foot_l, "%s", nx);
    else ui_text(PO.foot_l, "%s", "");
    if (hm_clock_set(tm)) {
        bottom_rule_line(a, sizeof a, tm);
        ui_text(PO.foot_r, "%s", a);
    }
}

const hm_face_t HM_F_TYPE = { .name = "type", .fonts = UI_FF_BIT(UI_FF_POSTER), .build = poster_build,
                              .minute = poster_minute };

/* ================================================================ the split flaps */

#define FL_CW 236
#define FL_CH 372
#define FL_GAP 14
#define FL_PAIR 56
#define FL_Y 150
#define FL_R 22
#define FL_SH 3                /* the steps of a card's shadow */
#define FL_DUR 0.46

typedef struct {
    lv_obj_t *card, *top, *bot, *ltop, *lbot, *flap, *lflap, *shadow[FL_SH];
    int x;
    char now[2], old[2];       /* the digit shown, and the one leaving while it flips */
    bool flipping;
    double t0;
    int phase;                 /* 1: the old top half falling to the hinge, 2: the new bottom half landing */
    int scale;                 /* the flap's scale as last set (256 = flat) */
} card_t;

static struct {
    card_t c[4];
    lv_obj_t *hinge[4], *date, *wx;
    uint32_t top_rgb, bot_rgb;
    const lv_font_t *font;     /* the digits as last fitted to their cards */
} FL;

/* where a digit's label sits on its half card, by the font's own height (the card's centre, a little low) */
static int digit_y(void) { return (FL_CH - ui_face_font(UI_FF_FLIP)->line_height) / 2 + 6; }

/* a digit's label inside a half: centred on the card, so each half shows its own part of it */
static lv_obj_t *half_label(lv_obj_t *half, bool lower)
{
    lv_obj_t *l = lv_label_create(half);
    ui_face_font_bind(l, UI_FF_FLIP);
    lv_obj_set_width(l, FL_CW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    int y = digit_y();
    lv_obj_set_pos(l, 0, lower ? y - FL_CH / 2 : y);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

static lv_obj_t *half_box(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *h = bz_box(parent);
    lv_obj_set_size(h, FL_CW, FL_CH / 2);
    lv_obj_set_pos(h, x, y);
    lv_obj_add_flag(h, LV_OBJ_FLAG_EVENT_BUBBLE);
    return h;
}

static void flip_build(lv_obj_t *b)
{
    int total = 4 * FL_CW + 2 * FL_GAP + FL_PAIR, x0 = (W - total) / 2;
    int xs[4] = { x0, x0 + FL_CW + FL_GAP, x0 + 2 * FL_CW + FL_GAP + FL_PAIR, x0 + 3 * FL_CW + 2 * FL_GAP + FL_PAIR };
    for (int i = 0; i < 4; i++) {
        card_t *c = &FL.c[i];
        c->x = xs[i];
        /* its shadow, down and to the right of the light: three soft steps, each wider and fainter */
        for (int k = 0; k < FL_SH; k++) {
            lv_obj_t *sh = bz_box(b);
            int g = 3 + 5 * k;
            lv_obj_set_size(sh, FL_CW + 2 * g, FL_CH + 2 * g);
            lv_obj_set_pos(sh, xs[i] - g + 3 + 2 * k, FL_Y - g + 7 + 4 * k);
            lv_obj_set_style_radius(sh, FL_R + g, 0);
            lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
            lv_obj_add_flag(sh, LV_OBJ_FLAG_EVENT_BUBBLE);
            c->shadow[k] = sh;
        }
        /* the card: its upper half a shade lighter than its lower (the light is above), one rounded box whose
         * gradient splits sharply at the hinge */
        c->card = bz_box(b);
        lv_obj_set_size(c->card, FL_CW, FL_CH);
        lv_obj_set_pos(c->card, xs[i], FL_Y);
        lv_obj_set_style_radius(c->card, FL_R, 0);
        lv_obj_set_style_bg_opa(c->card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(c->card, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(c->card, 128, 0);
        lv_obj_set_style_bg_grad_stop(c->card, 128, 0);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_EVENT_BUBBLE);
        c->top = half_box(b, xs[i], FL_Y);
        c->bot = half_box(b, xs[i], FL_Y + FL_CH / 2);
        c->ltop = half_label(c->top, false);
        c->lbot = half_label(c->bot, true);
        /* the flap that falls: a half card of its own, rounded like the card (its square corners at the hinge
         * would show; rounded ones there sit over the same colour and don't) */
        c->flap = half_box(b, xs[i], FL_Y);
        lv_obj_set_style_radius(c->flap, FL_R, 0);
        lv_obj_set_style_bg_opa(c->flap, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(c->flap, false, 0);
        c->lflap = half_label(c->flap, false);
        lv_obj_add_flag(c->flap, LV_OBJ_FLAG_HIDDEN);
        c->scale = 256;
        c->now[0] = c->old[0] = 0;
    }
    /* the hinge: a hairline of the ground across each pair, and the two pins that hold the flaps */
    for (int p = 0; p < 2; p++) {
        FL.hinge[p] = bz_box(b);
        lv_obj_set_size(FL.hinge[p], 2 * FL_CW + FL_GAP, 3);
        lv_obj_set_pos(FL.hinge[p], xs[p * 2], FL_Y + FL_CH / 2 - 1);
        lv_obj_set_style_bg_opa(FL.hinge[p], LV_OPA_COVER, 0);
        lv_obj_add_flag(FL.hinge[p], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    FL.date = bz_label_line(b, "", BZ_F_LABEL, BZ_C_INK, 600);
    lv_obj_set_pos(FL.date, x0, FL_Y + FL_CH + 42);
    FL.wx = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 400);
    lv_obj_set_style_text_align(FL.wx, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(FL.wx, x0 + total - 400, FL_Y + FL_CH + 42);
}

static void flip_colors(void)
{
    bool dark = bz_ui_dark();
    uint32_t g = bz_color(BZ_C_GROUND);
    FL.top_rgb = dark ? 0x222428 : 0xFFFFFF;
    FL.bot_rgb = dark ? 0x1B1D20 : 0xF3F4F6;
    uint32_t ink = bz_color(BZ_C_INK);
    for (int i = 0; i < 4; i++) {
        card_t *c = &FL.c[i];
        lv_obj_set_style_bg_color(c->card, bz_lv_rgb(FL.top_rgb), 0);
        lv_obj_set_style_bg_grad_color(c->card, bz_lv_rgb(FL.bot_rgb), 0);
        hm_color(c->ltop, ink);
        hm_color(c->lbot, ink);
        hm_color(c->lflap, ink);
    }
    for (int p = 0; p < 2; p++) lv_obj_set_style_bg_color(FL.hinge[p], bz_lv_rgb(g), 0);
    /* the shadow's steps: the ground darkened a little more toward the card */
    uint32_t sh = dark ? 0x000000 : 0x2A3240;
    static const int A_DARK[FL_SH] = { 120, 70, 35 }, A_LIGHT[FL_SH] = { 34, 20, 10 };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < FL_SH; k++) {
            lv_obj_set_style_bg_color(FL.c[i].shadow[k], bz_lv_rgb(sh), 0);
            lv_obj_set_style_bg_opa(FL.c[i].shadow[k], (lv_opa_t)(dark ? A_DARK[k] : A_LIGHT[k]), 0);
        }
}

static void flip_set_scale(card_t *c, float s, bool top_half)
{
    int v = (int)lroundf(s * 256);
    if (v < 1) v = 1;
    if (v == c->scale) return;
    c->scale = v;
    lv_obj_set_style_transform_scale_y(c->flap, v, 0);
    lv_obj_set_style_transform_pivot_y(c->flap, top_half ? FL_CH / 2 : 0, 0);
}

/* the flap for a phase: 1, the old digit's top half hinged at its bottom; 2, the new one's bottom half at its top */
static void flip_phase(card_t *c, int phase)
{
    c->phase = phase;
    bool top = phase == 1;
    lv_obj_set_pos(c->flap, c->x, top ? FL_Y : FL_Y + FL_CH / 2);
    lv_obj_set_style_bg_color(c->flap, bz_lv_rgb(top ? FL.top_rgb : FL.bot_rgb), 0);
    char s[2] = { top ? c->old[0] : c->now[0], 0 };
    lv_label_set_text(c->lflap, s);
    int y = digit_y();
    lv_obj_set_y(c->lflap, top ? y : y - FL_CH / 2);
    c->scale = -1;
    flip_set_scale(c, 1, top);
    hm_show(c->flap, true);
}

static void card_set(card_t *c, char d, bool animate)
{
    if (c->now[0] == d) return;
    char s[2] = { d, 0 };
    if (!animate || !c->now[0]) {
        c->now[0] = d;
        lv_label_set_text(c->ltop, s);
        lv_label_set_text(c->lbot, s);
        hm_show(c->flap, false);
        c->flipping = false;
        return;
    }
    /* the new digit's top half is behind the falling flap already; its bottom half lands when the flap does */
    c->old[0] = c->flipping && c->phase == 1 ? c->old[0] : c->now[0];
    c->now[0] = d;
    lv_label_set_text(c->ltop, s);
    char o[2] = { c->old[0], 0 };
    lv_label_set_text(c->lbot, o);
    c->flipping = true;
    c->t0 = hal_seconds();
    flip_phase(c, 1);
}

/* the digits' labels at the height the font now in them wants: again whenever it changes hands */
static void flip_fit(void)
{
    int y = digit_y();
    for (int i = 0; i < 4; i++) {
        card_t *c = &FL.c[i];
        lv_obj_set_y(c->ltop, y);
        lv_obj_set_y(c->lbot, y - FL_CH / 2);
        lv_obj_set_y(c->lflap, c->phase == 2 ? y - FL_CH / 2 : y);
    }
}

static void flip_minute(const struct tm *tm, bool force)
{
    if (force) flip_colors();
    if (FL.font != ui_face_font(UI_FF_FLIP)) {
        FL.font = ui_face_font(UI_FF_FLIP);
        flip_fit();
    }
    char hm[8] = "----";
    if (hm_clock_set(tm)) {
        int h = ui_look()->h24 ? tm->tm_hour : tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        snprintf(hm, sizeof hm, "%02d%02d", h, tm->tm_min);
        /* twelve-hour: no leading zero, a blank card instead */
        if (!ui_look()->h24 && hm[0] == '0') hm[0] = ' ';
    }
    for (int i = 0; i < 4; i++) card_set(&FL.c[i], hm[i], !force);
    char d[48], w[48];
    hm_date(tm, d, sizeof d, false);
    ui_text(FL.date, "%s", d);
    hm_weather_line(w, sizeof w);
    ui_text(FL.wx, "%s", w);
}

/* The fall: gravity (the square of time) to the hinge's far side, a small bounce off the stop, still. */
static void flip_frame(double now)
{
    (void)now;
    bool any = false;
    for (int i = 0; i < 4; i++) {
        card_t *c = &FL.c[i];
        if (!c->flipping) continue;
        any = true;
        double p = (hal_seconds() - c->t0) / FL_DUR;
        float ang; /* 0 flat on top .. 180 flat below, degrees */
        if (p < 1) ang = (float)(180.0 * p * p);
        else {
            /* the bounce: a damped rebound off the stop */
            double q = (p - 1) * FL_DUR;
            ang = 180.0f - (float)(9.0 * exp(-q * 14.0) * fabs(sin(q * 22.0)));
            if (q > 0.35) ang = 180;
        }
        if (ang < 90) {
            if (c->phase != 1) flip_phase(c, 1);
            flip_set_scale(c, cosf(ang * 3.14159265f / 180), true);
        } else {
            if (c->phase != 2) flip_phase(c, 2);
            flip_set_scale(c, -cosf(ang * 3.14159265f / 180), false);
        }
        if (p >= 1 && ang >= 180) {
            char s[2] = { c->now[0], 0 };
            lv_label_set_text(c->lbot, s);
            hm_show(c->flap, false);
            c->flipping = false;
        }
    }
    if (any) bz_ui_keep_alive();
}

const hm_face_t HM_F_FLIP = { .name = "flip", .fonts = UI_FF_BIT(UI_FF_FLIP), .build = flip_build, .minute = flip_minute,
                              .frame = flip_frame };

/* ================================================================ the word clock */

#define WD_C 11
#define WD_R 10
#define WD_CW 64
#define WD_RH 58
#define WD_X ((1280 - WD_C * WD_CW) / 2)
#define WD_Y 62

static const char *const WD_ROWS[WD_R] = {
    "itlisasampm", "acquarterdc", "twentyfivex", "halfstenfto", "pasterunine",
    "onesixthree", "fourfivetwo", "eighteleven", "seventwelve", "tenseoclock",
};

typedef struct { uint8_t r, c, n; } word_t;
enum { W_IT, W_IS, W_QUARTER, W_TWENTY, W_FIVE, W_HALF, W_TEN, W_TO, W_PAST, W_OCLOCK, W_H1 };
static const word_t WORDS[] = {
    { 0, 0, 2 }, { 0, 3, 2 }, { 1, 2, 7 }, { 2, 0, 6 }, { 2, 6, 4 }, { 3, 0, 4 }, { 3, 5, 3 }, { 3, 9, 2 },
    { 4, 0, 4 }, { 9, 5, 6 },
    /* the hours, one to twelve */
    { 5, 0, 3 }, { 6, 8, 3 }, { 5, 6, 5 }, { 6, 0, 4 }, { 6, 4, 4 }, { 5, 3, 3 }, { 8, 0, 5 }, { 7, 0, 5 },
    { 4, 7, 4 }, { 9, 0, 3 }, { 7, 5, 6 }, { 8, 5, 6 },
};

static struct {
    lv_obj_t *cell[WD_R][WD_C], *corner[4];
    uint8_t lit[WD_R][WD_C];
    int corners;
} WD;

static void words_build(lv_obj_t *b)
{
    for (int r = 0; r < WD_R; r++)
        for (int k = 0; k < WD_C; k++) {
            char s[2] = { WD_ROWS[r][k], 0 };
            lv_obj_t *l = bz_label(b, s, BZ_F_VALUE, BZ_C_FAINT);
            lv_obj_set_width(l, WD_CW);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, WD_X + k * WD_CW, WD_Y + r * WD_RH);
            WD.cell[r][k] = l;
            WD.lit[r][k] = 2;
        }
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = bz_box(b);
        lv_obj_set_size(o, 10, 10);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        int x = i == 0 || i == 3 ? WD_X - 34 : WD_X + WD_C * WD_CW + 24;
        int y = i < 2 ? WD_Y + 26 : WD_Y + WD_R * WD_RH - 26;
        lv_obj_set_pos(o, x, y);
        WD.corner[i] = o;
    }
    WD.corners = -1;
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
    if (hm_clock_set(tm))
        for (int i = 0; i < n; i++) {
            const word_t *w = &WORDS[lit[i]];
            for (int k = 0; k < w->n; k++) on[w->r][w->c + k] = 1;
        }
    bool dark = bz_ui_dark();
    uint32_t ink = bz_color(BZ_C_INK), off = hpx_mix(bz_color(BZ_C_GROUND), bz_color(BZ_C_FAINT), dark ? 110 : 120);
    for (int r = 0; r < WD_R; r++)
        for (int k = 0; k < WD_C; k++) {
            if (on[r][k] == WD.lit[r][k] && !force) continue;
            WD.lit[r][k] = on[r][k];
            hm_color(WD.cell[r][k], on[r][k] ? ink : off);
        }
    int past = hm_clock_set(tm) ? tm->tm_min % 5 : 0;
    if (past != WD.corners || force) {
        WD.corners = past;
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_bg_color(WD.corner[i], bz_lv_rgb(i < past ? bz_color(BZ_C_SIGNAL) : off), 0);
    }
}

const hm_face_t HM_F_WORDS = { .name = "words", .build = words_build, .minute = words_minute };

/* ================================================================ night */

static struct {
    lv_obj_t *time, *date;
} NT;

static void night_build(lv_obj_t *b)
{
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    NT.time = lv_label_create(b);
    ui_face_font_bind(NT.time, UI_FF_THIN);
    NT.date = bz_label(b, "", BZ_F_LABEL, BZ_C_DIM);
    /* warm and very dim, whatever the tone: this is what a dark bedroom sees */
    lv_obj_set_style_text_color(NT.time, bz_lv_rgb(0x5A4C46), 0);
    lv_obj_set_style_text_color(NT.date, bz_lv_rgb(0x3A312D), 0);
}

static void night_minute(const struct tm *tm, bool force)
{
    (void)force;
    char t[8], d[48];
    hm_hm(tm, t, sizeof t);
    ui_text(NT.time, "%s", t);
    hm_date(tm, d, sizeof d, false);
    ui_text(NT.date, "%s", d);
    lv_obj_align(NT.time, LV_ALIGN_CENTER, 0, -30);
    lv_obj_align(NT.date, LV_ALIGN_CENTER, 0, 80);
}

const hm_face_t HM_F_NIGHT = { .name = "night", .fonts = UI_FF_BIT(UI_FF_THIN), .build = night_build,
                               .minute = night_minute };
