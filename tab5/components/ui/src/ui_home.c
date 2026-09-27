/* The home screen: the time, the robot and the tablet at a glance, and the things reached for most.
 *
 * Landscape and phone mode are the same four widgets and the same six quick actions in two arrangements,
 * not one arrangement squeezed. Landscape: the clock on the left, the widgets two by two beside it, the
 * quick actions six across the foot. Portrait: the clock full width across the top, the widgets two by two
 * under it, the quick actions three across and two down. `hm_layout()` is where that choice is made and it
 * is the only place either frame's numbers appear. */
#include "ui_internal.h"

#include <string.h>
#include <time.h>

#define BODY_HT (BODY_BOTTOM - BODY_Y)          /* 514 landscape, 1074 portrait */
#define QUICK_H 112                             /* one row of quick actions */
#define NQUICK 6

typedef struct {
    int clock_w, clock_h, time_y;
    int wid_w, wid_h, x1, x2, y1, y2;
    int quick_cols, quick_w, quick_y;
} hm_layout_t;

static hm_layout_t hm_layout(void)
{
    hm_layout_t l;
    if (PORTRAIT) {
        /* 1074 to fill: a 400 clock, two 197 widget rows and two 112 action rows, 14 apart */
        int content = W - 2 * PAD;
        l.clock_w = content;
        l.wid_w = (content - BZ_GAP) / 2;
        l.quick_cols = 3;
        l.quick_w = (content - 2 * BZ_GAP) / l.quick_cols;
        l.wid_h = 197;
        l.clock_h = BODY_HT - 2 * l.wid_h - 2 * QUICK_H - 4 * BZ_GAP;
        l.time_y = 130;
        l.x1 = PAD;
        l.x2 = PAD + l.wid_w + BZ_GAP;
        l.y1 = BODY_Y + l.clock_h + BZ_GAP;
        l.y2 = l.y1 + l.wid_h + BZ_GAP;
        l.quick_y = l.y2 + l.wid_h + BZ_GAP;
    } else {
        int top_h = BODY_HT - QUICK_H - BZ_GAP;
        l.clock_w = 560;
        l.clock_h = top_h;
        l.wid_w = (W - 2 * PAD - l.clock_w - 2 * BZ_GAP) / 2;
        l.wid_h = (top_h - BZ_GAP) / 2;
        l.quick_cols = NQUICK;
        l.quick_w = (W - 2 * PAD - (NQUICK - 1) * BZ_GAP) / NQUICK;
        l.time_y = 70;
        l.x1 = PAD + l.clock_w + BZ_GAP;
        l.x2 = l.x1 + l.wid_w + BZ_GAP;
        l.y1 = BODY_Y;
        l.y2 = BODY_Y + l.wid_h + BZ_GAP;
        l.quick_y = BODY_Y + top_h + BZ_GAP;
    }
    return l;
}

static struct {
    lv_obj_t *time, *date, *greet;
    lv_obj_t *rb_mark, *rb_mode, *rb_batt, *rb_foot;
    lv_obj_t *al_count, *al_mark, *al_line;
    lv_obj_t *tb_batt, *tb_icon, *tb_foot;
    lv_obj_t *ln_robot, *ln_wifi, *ln_usb;
    lv_obj_t *team;
    bool match; /* the greeting shows the next match */
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
        /* not strftime's %e: it pads a one-digit day with a space ("September  5") */
        char d[40];
        strftime(d, sizeof d, "%A, %B", &tm);
        ui_text(HM.date, "%s %d", d, tm.tm_mday);
        /* an event day: the next match of ours where the greeting was ("next: Q34 · 14:52 · red with ..."),
         * in full ink (the accent is a fill, too pale to read as text in the light theme), and the card opens
         * The Blue Alliance */
        char nx[96];
        bool match = ui_match_next_line(nx, sizeof nx);
        if (match) ui_text(HM.greet, "%s", nx);
        else ui_text(HM.greet, "%s", tm.tm_hour < 5 ? "late night" : tm.tm_hour < 12 ? "good morning" : tm.tm_hour < 18 ? "good afternoon" : "good evening");
        if (match != HM.match) {
            HM.match = match;
            bz_set_color(HM.greet, match ? BZ_C_INK : BZ_C_DIM);
        }
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
    bz_set_color(HM.rb_batt, r->have_battery ? BZ_C_INK : BZ_C_FAINT); /* no reading: faint, not a value */
    if (r->connected) ui_text(HM.rb_foot, "%s · %.0f ms", r->have_identity ? r->name : r->address, r->rtt_ms);
    else ui_text(HM.rb_foot, "looking for team %d", S.team);

    if (!r->connected) {
        /* words, not a lone dash beside the mark: a bare "—" in the name face reads as a glitch */
        ui_text(HM.al_count, "no robot");
        bz_mark_set(HM.al_mark, BZ_STALE);
        ui_text(HM.al_line, "waiting for the robot");
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
    } else {
        ui_text(HM.tb_batt, "\xe2\x80\x94");
        ui_text(HM.tb_foot, "battery not reporting");
    }
    /* the team can change in settings: the head follows */
    ui_text(HM.team, "team %d", S.team);
    hal_net_t n;
    hal_net(&n);
    hal_tether_t t;
    hal_tether(&t);
    ui_text(HM.ln_usb, "%s", t.up ? t.ip : t.present ? "attached" : "no tether");
    /* Not "wi-fi off" whenever it isn't up: that is what it said for a night while it hunted for a
     * network that was not in the building, and it reads as the radio dropping rather than as the one
     * saved network being somewhere else. Say which network, and whether it can be found. */
    if (n.up) ui_text(HM.ln_wifi, "%s", n.ssid);
    else if (!n.want[0]) ui_text(HM.ln_wifi, "no wi-fi set up");
    /* "no <ssid>", not "<ssid> not in range", which the tile is too narrow for -- and it matches the two
     * lines either side of it, "no robot" and "no tether", which mean exactly the same thing: not here. */
    else if (n.absent) ui_text(HM.ln_wifi, "no %s", n.want);
    else ui_text(HM.ln_wifi, "joining %s", n.want);
    ui_text(HM.ln_robot, "%s", r->connected ? r->address : "no robot");
}

void ui_page_home(lv_obj_t *page)
{
    hm_layout_t l = hm_layout();
    lv_obj_t *head = ui_head(page, "Catalyst Tab", NULL);
    char team[16];
    snprintf(team, sizeof team, "team %d", S.team);
    HM.team = bz_label_line(head, team, BZ_F_LABEL, BZ_C_DIM, ui_head_width("Catalyst Tab"));

    /* the time, big */
    lv_obj_t *c = bz_tile(page, l.clock_w, l.clock_h);
    lv_obj_set_pos(c, PAD, BODY_Y);
    HM.greet = bz_label_line(c, "", BZ_F_LABEL, BZ_C_DIM, l.clock_w - 2 * BZ_PAD_TILE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(c, open_app_tap, (void *)&APP_TBA);
    HM.time = bz_label(c, "--:--", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(HM.time, -4, l.time_y);
    HM.date = bz_label_line(c, "", BZ_F_NAME, BZ_C_DIM, l.clock_w - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.date, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    int x1 = l.x1, x2 = l.x2, y1 = l.y1, y2 = l.y2;
    /* the robot */
    lv_obj_t *t = widget(page, x1, y1, l.wid_w, l.wid_h, "robot");
    bz_on_tap(t, go_tap, (void *)(intptr_t)PG_ROBOT);
    lv_obj_t *mr = bz_row(t, 12);
    lv_obj_set_pos(mr, 0, 40);
    HM.rb_mark = bz_mark(mr, BZ_STALE, 14);
    HM.rb_mode = bz_label_line(mr, "offline", BZ_F_NAME, BZ_C_INK, l.wid_w - 2 * BZ_PAD_TILE - 30);
    HM.rb_batt = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_pos(HM.rb_batt, 0, 88);
    HM.rb_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, l.wid_w - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.rb_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* what's wrong */
    t = widget(page, x2, y1, l.wid_w, l.wid_h, "alerts");
    bz_on_tap(t, open_app_tap, (void *)&APP_ALERTS);
    lv_obj_t *ar = bz_row(t, 12);
    lv_obj_set_pos(ar, 0, 40);
    HM.al_mark = bz_mark(ar, BZ_STALE, 14);
    HM.al_count = bz_label(ar, "\xe2\x80\x94", BZ_F_NAME, BZ_C_INK);
    HM.al_line = bz_label_line(t, "", BZ_F_BODY_S, BZ_C_DIM, l.wid_w - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.al_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the tablet itself */
    t = widget(page, x1, y2, l.wid_w, l.wid_h, "this tablet");
    bz_on_tap(t, open_app_tap, (void *)&APP_SYSMON);
    HM.tb_batt = bz_label(t, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(HM.tb_batt, 0, 34);
    HM.tb_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, l.wid_w - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.tb_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* how it reaches things */
    t = widget(page, x2, y2, l.wid_w, l.wid_h, "links");
    bz_on_tap(t, open_app_tap, (void *)&APP_SETTINGS);
    lv_obj_t *col = bz_col(t, 8);
    lv_obj_set_pos(col, 0, 36);
    static const char *const LI[3] = { BZ_I_SMART_TOY, BZ_I_WIFI, BZ_I_USB };
    lv_obj_t **lv[3] = { &HM.ln_robot, &HM.ln_wifi, &HM.ln_usb };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = bz_row(col, 10);
        bz_icon(r, LI[i], 24, BZ_C_DIM);
        *lv[i] = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, l.wid_w - 2 * BZ_PAD_TILE - 44);
    }

    /* the things reached for most */
    static const struct { const ui_app_t *app; const char *icon; const char *label; } QUICK[] = {
        { &APP_PREFLIGHT, BZ_I_CHECKLIST, "preflight" }, { &APP_LOGS, BZ_I_RECEIPT_LONG, "logs" },
        { &APP_TIMER, BZ_I_TIMER, "timer" },             { &APP_CHECK, BZ_I_CHECKLIST_RTL, "checklist" },
        { &APP_NOTES, BZ_I_EDIT_NOTE, "notes" },         { &APP_ASSIST, BZ_I_AUTO_AWESOME, "assist" },
    };
    int n = (int)(sizeof QUICK / sizeof QUICK[0]);
    int qw = l.quick_w;
    for (int i = 0; i < n; i++) {
        lv_obj_t *q = bz_tile(page, qw, QUICK_H);
        lv_obj_set_pos(q, PAD + (i % l.quick_cols) * (qw + BZ_GAP),
                       l.quick_y + (i / l.quick_cols) * (QUICK_H + BZ_GAP));
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
