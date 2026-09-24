/* The home screen: the time, the robot and the tablet at a glance, and the things reached for most. */
#include "ui_internal.h"

#include <string.h>
#include <time.h>

#define BODY_HT (BODY_BOTTOM - BODY_Y)          /* 514 */
#define QUICK_H 112                             /* the quick actions' row */
#define TOP_H (BODY_HT - QUICK_H - BZ_GAP)      /* 386 */
#define CLOCK_W 560
#define WID_W ((W - 2 * PAD - CLOCK_W - 2 * BZ_GAP) / 2)
#define WID_H ((TOP_H - BZ_GAP) / 2)

static struct {
    lv_obj_t *time, *date, *greet;
    lv_obj_t *rb_mark, *rb_mode, *rb_batt, *rb_foot;
    lv_obj_t *al_count, *al_mark, *al_line;
    lv_obj_t *tb_batt, *tb_icon, *tb_foot;
    lv_obj_t *ln_robot, *ln_wifi, *ln_usb;
} HM;

static void open_app_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }
static void go_tap(lv_obj_t *o, void *u) { (void)o; ui_go((int)(intptr_t)u); }

static lv_obj_t *widget(lv_obj_t *page, int x, int y, int w, int h, const char *label)
{
    lv_obj_t *t = bz_tile(page, w, h);
    lv_obj_set_pos(t, x, y);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    bz_label(t, label, BZ_F_LABEL, BZ_C_DIM);
    return t;
}

static void home_refresh(void *u)
{
    (void)u;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) {
        ui_text(HM.time, "%d:%02d", tm.tm_hour, tm.tm_min);
        char d[40];
        strftime(d, sizeof d, "%A, %B %e", &tm);
        ui_text(HM.date, "%s", d);
        ui_text(HM.greet, "%s", tm.tm_hour < 5 ? "late night" : tm.tm_hour < 12 ? "good morning" : tm.tm_hour < 18 ? "good afternoon" : "good evening");
    } else {
        ui_text(HM.time, "--:--");
        ui_text(HM.date, "the clock isn't set");
        ui_text(HM.greet, "set it in settings · date and time");
    }

    const cat_robot_t *r = R;
    char b[24];
    bz_mark_set(HM.rb_mark, !r->connected ? BZ_STALE : r->estop ? BZ_FAULT : r->enabled ? BZ_WARN : BZ_OK);
    ui_text(HM.rb_mode, "%s", r->connected ? cat_mode_name(r) : "offline");
    ui_text(HM.rb_batt, "%s v", bz_fmt(b, sizeof b, r->have_battery, "%.2f", r->battery_v));
    if (r->connected) ui_text(HM.rb_foot, "%s · %.0f ms", r->have_identity ? r->name : r->address, r->rtt_ms);
    else ui_text(HM.rb_foot, "looking for team %d", S.team);

    if (!r->connected) {
        ui_text(HM.al_count, "\xe2\x80\x94");
        bz_mark_set(HM.al_mark, BZ_STALE);
        ui_text(HM.al_line, "no robot");
    } else if (r->n_errors || r->n_warnings) {
        ui_text(HM.al_count, "%d · %d", r->n_errors, r->n_warnings);
        bz_mark_set(HM.al_mark, r->n_errors ? BZ_FAULT : BZ_WARN);
        const char *first = "";
        for (int i = 0; i < r->nalerts; i++)
            if (r->alerts[i].sev != CAT_SEV_INFO) { first = r->alerts[i].text; break; }
        ui_text(HM.al_line, "%s", first);
    } else {
        ui_text(HM.al_count, "clear");
        bz_mark_set(HM.al_mark, BZ_OK);
        ui_text(HM.al_line, "nothing wrong");
    }

    hal_battery_t bt;
    if (hal_battery(&bt) && bt.ok) {
        ui_text(HM.tb_batt, "%d%%", bt.percent);
        ui_text(HM.tb_foot, "%.2f v%s", bt.volts, bt.charging ? " · charging" : bt.external ? " · external" : "");
    }
    hal_net_t n;
    hal_net(&n);
    hal_tether_t t;
    hal_tether(&t);
    ui_text(HM.ln_usb, "%s", t.up ? t.ip : t.present ? "attached" : "no tether");
    ui_text(HM.ln_wifi, "%s", n.up ? n.ssid : "wi-fi off");
    ui_text(HM.ln_robot, "%s", r->connected ? r->address : "no robot");
}

void ui_page_home(lv_obj_t *page)
{
    lv_obj_t *head = ui_head(page, "Catalyst Tab", NULL);
    char team[16];
    snprintf(team, sizeof team, "team %d", S.team);
    bz_label_line(head, team, BZ_F_LABEL, BZ_C_DIM, ui_head_width("Catalyst Tab"));

    /* the time, big */
    lv_obj_t *c = bz_tile(page, CLOCK_W, TOP_H);
    lv_obj_set_pos(c, PAD, BODY_Y);
    HM.greet = bz_label(c, "", BZ_F_LABEL, BZ_C_DIM);
    HM.time = bz_label(c, "--:--", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(HM.time, -4, 70);
    HM.date = bz_label_line(c, "", BZ_F_NAME, BZ_C_DIM, CLOCK_W - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.date, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    int x1 = PAD + CLOCK_W + BZ_GAP, x2 = x1 + WID_W + BZ_GAP, y1 = BODY_Y, y2 = BODY_Y + WID_H + BZ_GAP;
    /* the robot */
    lv_obj_t *t = widget(page, x1, y1, WID_W, WID_H, "robot");
    bz_on_tap(t, go_tap, (void *)(intptr_t)PG_ROBOT);
    lv_obj_t *mr = bz_row(t, 12);
    lv_obj_set_pos(mr, 0, 40);
    HM.rb_mark = bz_mark(mr, BZ_STALE, 14);
    HM.rb_mode = bz_label_line(mr, "offline", BZ_F_NAME, BZ_C_INK, WID_W - 2 * BZ_PAD_TILE - 30);
    HM.rb_batt = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_pos(HM.rb_batt, 0, 88);
    HM.rb_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, WID_W - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.rb_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* what's wrong */
    t = widget(page, x2, y1, WID_W, WID_H, "alerts");
    bz_on_tap(t, open_app_tap, (void *)&APP_ALERTS);
    lv_obj_t *ar = bz_row(t, 12);
    lv_obj_set_pos(ar, 0, 40);
    HM.al_mark = bz_mark(ar, BZ_STALE, 14);
    HM.al_count = bz_label(ar, "\xe2\x80\x94", BZ_F_NAME, BZ_C_INK);
    HM.al_line = bz_label_line(t, "", BZ_F_BODY_S, BZ_C_DIM, WID_W - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.al_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the tablet itself */
    t = widget(page, x1, y2, WID_W, WID_H, "this tablet");
    bz_on_tap(t, open_app_tap, (void *)&APP_SYSMON);
    HM.tb_batt = bz_label(t, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(HM.tb_batt, 0, 34);
    HM.tb_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, WID_W - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.tb_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* how it reaches things */
    t = widget(page, x2, y2, WID_W, WID_H, "links");
    bz_on_tap(t, open_app_tap, (void *)&APP_SETTINGS);
    lv_obj_t *col = bz_col(t, 8);
    lv_obj_set_pos(col, 0, 36);
    static const char *const LI[3] = { BZ_I_SMART_TOY, BZ_I_WIFI, BZ_I_USB };
    lv_obj_t **lv[3] = { &HM.ln_robot, &HM.ln_wifi, &HM.ln_usb };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = bz_row(col, 10);
        bz_icon(r, LI[i], 24, BZ_C_DIM);
        *lv[i] = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, WID_W - 2 * BZ_PAD_TILE - 44);
    }

    /* the things reached for most */
    static const struct { const ui_app_t *app; const char *icon; const char *label; } QUICK[] = {
        { &APP_PREFLIGHT, BZ_I_CHECKLIST, "preflight" }, { &APP_LOGS, BZ_I_RECEIPT_LONG, "logs" },
        { &APP_TIMER, BZ_I_TIMER, "timer" },             { &APP_CHECK, BZ_I_CHECKLIST_RTL, "checklist" },
        { &APP_NOTES, BZ_I_EDIT_NOTE, "notes" },         { &APP_ASSIST, BZ_I_AUTO_AWESOME, "assist" },
    };
    int n = (int)(sizeof QUICK / sizeof QUICK[0]);
    int qw = (W - 2 * PAD - (n - 1) * BZ_GAP) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *q = bz_tile(page, qw, QUICK_H);
        lv_obj_set_pos(q, PAD + i * (qw + BZ_GAP), BODY_Y + TOP_H + BZ_GAP);
        lv_obj_add_flag(q, LV_OBJ_FLAG_CLICKABLE);
        /* icon over label: six across leaves too little width for the two side by side */
        lv_obj_set_style_pad_all(q, 10, 0);
        lv_obj_set_flex_flow(q, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(q, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(q, 6, 0);
        lv_obj_t *ic = bz_tile(q, 52, 52);
        lv_obj_add_style(ic, bz_style_fill(BZ_C_SURFACE3), 0);
        lv_obj_set_style_radius(ic, 18, 0);
        lv_obj_set_style_pad_all(ic, 0, 0);
        lv_obj_t *g = bz_icon(ic, QUICK[i].icon, 32, BZ_C_INK);
        lv_obj_center(g);
        lv_obj_t *ql = bz_label_line(q, QUICK[i].label, BZ_F_BODY_S, BZ_C_INK, qw - 20);
        lv_obj_set_style_text_align(ql, LV_TEXT_ALIGN_CENTER, 0);
        bz_on_tap(q, open_app_tap, (void *)QUICK[i].app);
    }
    ui_on_page_refresh(PG_HOME, home_refresh, NULL);
}
