/* weather — the forecast behind home mode's corner: now (feels like, humidity, wind, sunrise and sunset), the
 * next 24 hours every two hours with a line of the temperature, and the week. From Open-Meteo, in the one
 * request home mode already makes every 20 minutes (home_weather.c); the refresh button asks again now.
 *
 * Every label is set only when the forecast's generation moves (at most every 20 minutes). */
#include "ui_home_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)
#define LW 420
#define RX (PAD + LW + BZ_GAP)
#define RW (W - PAD - RX)
#define HOURS_H 206
#define WEEK_Y (APP_Y + HOURS_H + BZ_GAP)
#define WEEK_H (H - PAD - WEEK_Y)
#define COLS 12                    /* every two hours */
#define COL_W (IN(RW) / COLS)
#define ROW_H 41

static const char *const WDAY[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

static struct {
    lv_obj_t *place, *icon, *temp, *desc, *range, *feels, *humid, *wind, *sun, *msg, *set_btn;
    lv_obj_t *h_hour[COLS], *h_temp[COLS], *h_pop[COLS], *spark;
    lv_obj_t *d_day[HOME_WX_DAYS], *d_desc[HOME_WX_DAYS], *d_pop[HOME_WX_DAYS], *d_temp[HOME_WX_DAYS];
    lv_obj_t *h_empty, *d_empty;   /* what the two forecast cards say while they have nothing */
    home_forecast_t fc;            /* PSRAM (the ui component's statics) */
    unsigned gen;
    const char *glyph;
    double asked_at;               /* "asking again..." shows until an answer, or this plus 20 s */
} WX;

static void set_place(lv_obj_t *o, void *u)
{
    (void)u;
    hm_open_settings(o);
}

static void again(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    home_weather_t w;
    home_weather_get(&w);
    if (!w.configured) {
        /* nothing to ask about: "asking again..." would have stayed up for good */
        ui_island_say(BZ_I_PIN_DROP, "set a place first: settings, home");
        return;
    }
    home_weather_refresh();
    ui_text(WX.place, "%s", "asking again...");
    WX.asked_at = hal_seconds();
}

static lv_obj_t *line(lv_obj_t *parent, bz_font_role_t f, bz_color_role_t c, int x, int y, int w)
{
    lv_obj_t *l = bz_label_line(parent, "", f, c, w);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void weather_build(lv_obj_t *b)
{
    lv_obj_t *hr = bz_row(b, 14);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(hr, 60);
    WX.place = bz_label_line(hr, "", BZ_F_LABEL, BZ_C_DIM, 420);
    lv_obj_set_style_text_align(WX.place, LV_TEXT_ALIGN_RIGHT, 0);
    ui_button(hr, BZ_I_REFRESH, "refresh", again, NULL);

    /* now */
    lv_obj_t *t = bz_tile(b, LW, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    bz_label(t, "now", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = bz_row(t, 14);
    lv_obj_set_pos(r, 0, 30);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    WX.icon = bz_icon(r, BZ_I_THERMOMETER, 40, BZ_C_INK);
    WX.temp = bz_label(r, "", BZ_F_DISPLAY, BZ_C_INK);
    WX.desc = line(t, BZ_F_NAME, BZ_C_INK, 0, 130, IN(LW));
    WX.range = line(t, BZ_F_BODY, BZ_C_DIM, 0, 170, IN(LW));
    WX.feels = line(t, BZ_F_BODY_S, BZ_C_INK, 0, 236, IN(LW));
    WX.humid = line(t, BZ_F_BODY_S, BZ_C_INK, 0, 272, IN(LW));
    WX.wind = line(t, BZ_F_BODY_S, BZ_C_INK, 0, 308, IN(LW));
    WX.sun = line(t, BZ_F_BODY_S, BZ_C_INK, 0, 344, IN(LW));
    WX.msg = bz_label(t, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(WX.msg, IN(LW));
    lv_label_set_long_mode(WX.msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(WX.msg, 0, 236);
    WX.set_btn = ui_button(t, BZ_I_PIN_DROP, "set the place", set_place, NULL);
    lv_obj_align(WX.set_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the next 24 hours */
    t = bz_tile(b, RW, HOURS_H);
    lv_obj_set_pos(t, RX, APP_Y);
    bz_label(t, "the next 24 hours", BZ_F_LABEL, BZ_C_DIM);
    for (int i = 0; i < COLS; i++) {
        int x = i * COL_W;
        WX.h_hour[i] = line(t, BZ_F_CAPTION, BZ_C_DIM, x, 30, COL_W - 4);
        WX.h_temp[i] = line(t, BZ_F_BODY_S, BZ_C_INK, x, 54, COL_W - 4);
        WX.h_pop[i] = line(t, BZ_F_CAPTION, BZ_C_ICE, x, 82, COL_W - 4);
    }
    WX.spark = bz_spark(t, IN(RW), 36, HOME_WX_HOURS);
    bz_spark_min_span(WX.spark, 4);
    bz_spark_color(WX.spark, BZ_C_ICE);
    lv_obj_set_pos(WX.spark, 0, 118);
    WX.h_empty = line(t, BZ_F_BODY_S, BZ_C_DIM, 0, 40, IN(RW));

    /* the week */
    t = bz_tile(b, RW, WEEK_H);
    lv_obj_set_pos(t, RX, WEEK_Y);
    bz_label(t, "the week", BZ_F_LABEL, BZ_C_DIM);
    for (int i = 0; i < HOME_WX_DAYS; i++) {
        int y = 30 + i * ROW_H;
        WX.d_day[i] = line(t, BZ_F_BODY, BZ_C_INK, 0, y, 130);
        WX.d_desc[i] = line(t, BZ_F_BODY_S, BZ_C_DIM, 140, y + 2, 300);
        WX.d_pop[i] = line(t, BZ_F_CAPTION, BZ_C_ICE, 450, y + 4, 90);
        WX.d_temp[i] = line(t, BZ_F_BODY, BZ_C_INK, IN(RW) - 190, y, 190);
        lv_obj_set_style_text_align(WX.d_temp[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    WX.d_empty = line(t, BZ_F_BODY_S, BZ_C_DIM, 0, 40, IN(RW));
}

static void vis(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void weather_refresh(void)
{
    home_want(HOME_WANT_WEATHER);
    home_weather_t w;
    home_weather_get(&w);
    /* an answer that never came: the head says the place again rather than "asking again..." for good */
    if (WX.asked_at && hal_seconds() - WX.asked_at > 20) WX.gen = 0;
    if (w.gen == WX.gen && WX.gen) return;
    WX.gen = w.gen ? w.gen : 1;
    WX.asked_at = 0;
    home_weather_forecast(&WX.fc);
    const home_forecast_t *f = &WX.fc;
    const char *deg = "\xc2\xb0";

    bool ok = w.configured && w.ok;
    vis(WX.msg, !ok);
    vis(WX.set_btn, !w.configured);
    lv_obj_t *now_rows[] = { WX.icon, WX.temp, WX.desc, WX.range, WX.feels, WX.humid, WX.wind, WX.sun };
    for (size_t i = 0; i < sizeof now_rows / sizeof now_rows[0]; i++) vis(now_rows[i], ok);
    if (!ok) {
        ui_text(WX.place, "%s", w.configured ? w.place : "no place set");
        ui_text(WX.msg, "%s", !w.configured ? "Set a place in settings, home: a city (Seattle) or latitude, longitude."
                              : w.err[0] ? w.err : "Looking up the weather...");
    } else {
        ui_text(WX.place, "%s", w.place);
        const char *g = hm_wx_icon(w.code, w.day);
        if (g != WX.glyph) {
            WX.glyph = g;
            bz_icon_set(WX.icon, g, 40, false);
        }
        ui_text(WX.temp, "%.0f%s", w.temp, deg);
        ui_text(WX.desc, "%s", home_weather_text(w.code));
        ui_text(WX.range, "high %.0f%s \xc2\xb7 low %.0f%s", w.hi, deg, w.lo, deg);
        if (!isnan(f->feels)) ui_text(WX.feels, "feels like %.0f%s", f->feels, deg);
        else ui_text(WX.feels, "%s", "");
        if (f->humidity >= 0) ui_text(WX.humid, "humidity %d %%", f->humidity);
        else ui_text(WX.humid, "%s", "");
        if (f->wind >= 0) ui_text(WX.wind, "wind %.0f %s", f->wind, w.fahrenheit ? "mph" : "km/h");
        else ui_text(WX.wind, "%s", "");
        if (f->sunrise[0]) ui_text(WX.sun, "sunrise %s \xc2\xb7 sunset %s", f->sunrise, f->sunset);
        else ui_text(WX.sun, "%s", "");
    }

    /* the hours: every second one, and the whole day as a line */
    for (int i = 0; i < COLS; i++) {
        int k = i * 2;
        bool have = ok && f->ok && k < f->nhours;
        if (have && i > 0) ui_text(WX.h_hour[i], "%d:00", f->hour[k].hour);
        else ui_text(WX.h_hour[i], "%s", have ? "now" : "");
        if (have) ui_text(WX.h_temp[i], "%.0f%s", f->hour[k].temp, deg);
        else ui_text(WX.h_temp[i], "%s", "");
        if (have && f->hour[k].pop >= 20) ui_text(WX.h_pop[i], "%d%%", f->hour[k].pop);
        else ui_text(WX.h_pop[i], "%s", "");
    }
    bz_spark_clear(WX.spark);
    for (int k = 0; ok && f->ok && k < f->nhours; k++) bz_spark_push(WX.spark, f->hour[k].temp);
    /* the two cards say why they're empty, rather than stand blank */
    const char *why = !w.configured ? "The forecast shows here once a place is set." : "Waiting for the forecast...";
    ui_text(WX.h_empty, "%s", ok && f->ok ? "" : why);
    ui_text(WX.d_empty, "%s", ok && f->ok ? "" : why);

    for (int i = 0; i < HOME_WX_DAYS; i++) {
        bool have = ok && f->ok && i < f->ndays;
        if (!have) {
            ui_text(WX.d_day[i], "%s", "");
            ui_text(WX.d_desc[i], "%s", "");
            ui_text(WX.d_pop[i], "%s", "");
            ui_text(WX.d_temp[i], "%s", "");
            continue;
        }
        int wd = f->day[i].wday;
        if (i == 0) ui_text(WX.d_day[i], "%s", "today");
        else ui_text(WX.d_day[i], "%s %d", wd >= 0 ? WDAY[wd] : "", f->day[i].mday);
        ui_text(WX.d_desc[i], "%s", home_weather_text(f->day[i].code));
        if (f->day[i].pop >= 20) ui_text(WX.d_pop[i], "rain %d%%", f->day[i].pop);
        else ui_text(WX.d_pop[i], "%s", "");
        ui_text(WX.d_temp[i], "%.0f%s / %.0f%s", f->day[i].hi, deg, f->day[i].lo, deg);
    }
}

static void weather_open(void)
{
    hm_cfg_apply();
    WX.gen = 0;
}

const ui_app_t APP_WEATHER = { .name = "weather", .icon = BZ_I_LIGHT_MODE, .build = weather_build, .open = weather_open,
                               .refresh = weather_refresh };
