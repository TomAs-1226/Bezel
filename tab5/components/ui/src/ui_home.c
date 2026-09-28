/* The home screen: the time, the robot and the tablet at a glance, and the apps reached for most.
 *
 * Hierarchy, not a grid of equals: the time is the hero (a tonal container of the accent, the one lit surface on
 * the page), the robot is the one wide card beside it, the three things that only need a glance (alerts, this
 * tablet, how it reaches things) are small cards under it, and the owner's favourite apps sit along the foot as
 * icons on the ground, the way a phone's dock row does.
 *
 * Landscape and phone mode are the same pieces in two arrangements, not one arrangement squeezed. Landscape: the
 * hero on the left, the robot and the three small cards beside it, the quick-launch row across the foot.
 * Portrait: the hero full width across the top, the robot under it, the three small cards across, the row in two
 * lines of three. `hm_layout()` is where that choice is made and the only place either frame's numbers appear. */
#include "ui_internal.h"

#include <string.h>
#include <time.h>

#define BODY_HT (BODY_BOTTOM - BODY_Y) /* 514 landscape, 1074 portrait */
#define QUICK_H 124                    /* a row of quick-launch icons */
#define QUICK_ICON 64                  /* the library's size: the same baked pictures */

typedef struct {
    int hero_w, hero_h, time_y;
    int rb_x, rb_y, rb_w, rb_h;        /* the robot card */
    int sm_x, sm_y, sm_w, sm_h;        /* the first of the two small cards */
    int quick_y, quick_cols, quick_w;
} hm_layout_t;

static hm_layout_t hm_layout(void)
{
    hm_layout_t l;
    int content = W - 2 * PAD;
    if (PORTRAIT) {
        /* 1074 to fill: a 380 hero, a 200 robot card, 190 small cards and two 124 rows, 14 apart */
        l.quick_cols = 3;
        l.hero_w = content;
        l.hero_h = BODY_HT - 200 - 190 - 2 * QUICK_H - 4 * BZ_GAP;
        l.time_y = 110;
        l.rb_x = PAD;
        l.rb_y = BODY_Y + l.hero_h + BZ_GAP;
        l.rb_w = content;
        l.rb_h = 200;
        l.sm_x = PAD;
        l.sm_y = l.rb_y + l.rb_h + BZ_GAP;
        l.sm_w = (content - BZ_GAP) / 2;
        l.sm_h = 190;
        l.quick_y = l.sm_y + l.sm_h + BZ_GAP;
    } else {
        int top = BODY_HT - QUICK_H - BZ_GAP;
        l.quick_cols = UI_QUICK_MAX;
        l.hero_w = 560;
        l.hero_h = top;
        l.time_y = 64;
        l.rb_x = PAD + l.hero_w + BZ_GAP;
        l.rb_y = BODY_Y;
        l.rb_w = W - PAD - l.rb_x;
        l.rb_h = (top - BZ_GAP) / 2;
        l.sm_x = l.rb_x;
        l.sm_y = BODY_Y + l.rb_h + BZ_GAP;
        l.sm_w = (l.rb_w - BZ_GAP) / 2;
        l.sm_h = top - l.rb_h - BZ_GAP;
        l.quick_y = BODY_Y + top + BZ_GAP;
    }
    l.quick_w = content / l.quick_cols;
    return l;
}

static struct {
    lv_obj_t *time, *date, *greet;
    lv_obj_t *rb_mark, *rb_mode, *rb_batt, *rb_unit, *rb_band, *rb_foot;
    lv_obj_t *al_count, *al_mark, *al_line;
    lv_obj_t *tb_batt, *tb_foot;
    lv_obj_t *ln_wifi, *ln_usb;
    lv_obj_t *team, *quick;
    bool match; /* the greeting shows the next match */
    int robot_state;
} HM = { .robot_state = -1 };

static void open_app_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }
static void go_tap(lv_obj_t *o, void *u) { (void)o; ui_go((int)(intptr_t)u); }

static lv_obj_t *card(lv_obj_t *page, int x, int y, int w, int h, const char *label)
{
    lv_obj_t *t = bz_tile(page, w, h);
    lv_obj_set_pos(t, x, y);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    if (label) bz_label(t, label, BZ_F_LABEL, BZ_C_DIM);
    return t;
}

static void home_refresh(void *u)
{
    (void)u;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) {
        char hm[16];
        ui_fmt_hm(hm, sizeof hm, &tm);
        ui_text(HM.time, "%s", hm);
        /* not strftime's %e: it pads a one-digit day with a space ("September  5") */
        char d[40];
        strftime(d, sizeof d, "%A, %B", &tm);
        ui_text(HM.date, "%s %d", d, tm.tm_mday);
        /* an event day: the next match of ours where the greeting was ("next: Q34 · 14:52 · red with ..."),
         * in full ink, and the card opens The Blue Alliance */
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
    /* offline, the word is quieter than a live mode: it is the absence of one */
    int st = r->connected ? 1 : 0;
    if (st != HM.robot_state) {
        HM.robot_state = st;
        bz_set_color(HM.rb_mode, st ? BZ_C_INK : BZ_C_DIM);
    }
    ui_text(HM.rb_batt, "%s", bz_fmt(b, sizeof b, r->have_battery, "%.2f", r->battery_v));
    bz_set_color(HM.rb_batt, r->have_battery ? BZ_C_INK : BZ_C_FAINT); /* no reading: faint, not a value */
    if (r->have_battery) {
        int band = cat_battery_band(r->battery_v);
        ui_text(HM.rb_band, "%s", band == 2 ? "charged" : band == 1 ? "swap before a match" : "low");
        bz_set_color(HM.rb_band, band == 2 ? BZ_C_DIM : BZ_C_WARN);
    } else {
        ui_text(HM.rb_band, "%s", r->connected ? "not published" : "battery");
        bz_set_color(HM.rb_band, BZ_C_DIM);
    }
    if (r->connected) ui_text(HM.rb_foot, "%s · %.0f ms", r->have_identity && r->name[0] ? r->name : r->address, r->rtt_ms);
    else ui_text(HM.rb_foot, "looking for team %d · tap for the robot's pages", S.team);

    if (!r->connected) {
        /* words, not a lone dash beside the mark: a bare "—" in the name face reads as a glitch */
        ui_text(HM.al_count, "quiet");
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
        ui_text(HM.tb_foot, "%.2f v\n%s", bt.volts, bt.charging ? "charging" : bt.external ? "plugged in" : "on battery");
    } else {
        ui_text(HM.tb_batt, "\xe2\x80\x94");
        ui_text(HM.tb_foot, "not reporting");
    }
    /* the team can change in settings: the head follows */
    ui_text(HM.team, "team %d", S.team);
    hal_net_t n;
    hal_net(&n);
    hal_tether_t t;
    hal_tether(&t);
    /* the tether in a word: the address is on the robot card once it is used */
    ui_text(HM.ln_usb, "%s", t.up ? "up" : t.present ? "wait" : "none");
    bz_set_color(HM.ln_usb, t.up ? BZ_C_INK : BZ_C_DIM);
    /* Not "wi-fi off" whenever it isn't up: say which network, and whether it can be found ("no <ssid>" matches
     * the lines either side of it, "no robot" and "no tether", which mean the same thing: not here). */
    /* the card is narrow: the network's name alone, dim until it is joined */
    if (n.up) ui_text(HM.ln_wifi, "%s", n.ssid);
    else if (!n.want[0]) ui_text(HM.ln_wifi, "no wi-fi");
    else ui_text(HM.ln_wifi, "%s", n.want);
    bz_set_color(HM.ln_wifi, n.up ? BZ_C_INK : BZ_C_DIM);

}

/* The quick-launch row: the owner's favourites (settings > look), centred along the foot. */
static void quick_build(void)
{
    hm_layout_t l = hm_layout();
    lv_obj_clean(HM.quick);
    const ui_look_t *lk = ui_look();
    for (int i = 0; i < lk->nquick; i++) {
        const ui_app_t *app = ui_app_find(lk->quick[i]);
        if (!app) {
            /* home mode is a tile, not an app: the library's entry has it */
            for (int k = 0; k < ui_app_count(); k++)
                if (!strcmp(ui_app_entry(k)->label, lk->quick[i])) app = ui_app_entry(k)->app;
        }
        if (!app) continue;
        lv_obj_t *c = ui_app_cell(HM.quick, app, lk->quick[i], NULL, l.quick_w, QUICK_ICON);
        lv_obj_set_style_pad_top(c, 8, 0);
        lv_obj_set_style_pad_bottom(c, 6, 0);
    }
}

void ui_home_quick_changed(void)
{
    if (!HM.quick) return;
    quick_build();
}

void ui_page_home(lv_obj_t *page)
{
    hm_layout_t l = hm_layout();
    lv_obj_t *head = ui_head(page, "Catalyst Tab", NULL);
    char team[16];
    snprintf(team, sizeof team, "team %d", S.team);
    HM.team = bz_label_line(head, team, BZ_F_LABEL, BZ_C_DIM, ui_head_width("Catalyst Tab"));

    /* the time: the hero, in the accent's container */
    lv_obj_t *c = bz_tile(page, l.hero_w, l.hero_h);
    bz_tile_set_fill(c, BZ_C_CONTAINER);
    lv_obj_set_pos(c, PAD, BODY_Y);
    lv_obj_set_style_pad_all(c, 30, 0);
    HM.greet = bz_label_line(c, "", BZ_F_LABEL, BZ_C_DIM, l.hero_w - 60);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(c, open_app_tap, (void *)&APP_TBA);
    HM.time = bz_label(c, "--:--", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(HM.time, -6, l.time_y);
    HM.date = bz_label_line(c, "", BZ_F_NAME, BZ_C_INK, l.hero_w - 60);
    lv_obj_align(HM.date, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the robot: the one wide card. Its state on the left, its battery on the right, how it's reached under both */
    lv_obj_t *t = card(page, l.rb_x, l.rb_y, l.rb_w, l.rb_h, "robot");
    bz_on_tap(t, go_tap, (void *)(intptr_t)PG_ROBOT);
    lv_obj_t *mr = bz_row(t, 14);
    lv_obj_set_pos(mr, 0, 38);
    HM.rb_mark = bz_mark(mr, BZ_STALE, 16);
    HM.rb_mode = bz_label_line(mr, "offline", BZ_F_VALUE, BZ_C_DIM, l.rb_w / 2 - 40);
    HM.rb_band = bz_label(t, "battery", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(HM.rb_band, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *vr = bz_row(t, 8);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    HM.rb_batt = bz_label(vr, "\xe2\x80\x94", BZ_F_VALUE, BZ_C_FAINT);
    HM.rb_unit = bz_label(vr, "v", BZ_F_NAME, BZ_C_DIM);
    lv_obj_set_style_pad_bottom(HM.rb_unit, 8, 0);
    lv_obj_align(vr, LV_ALIGN_TOP_RIGHT, 0, 34);
    HM.rb_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, l.rb_w - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.rb_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    int sw = l.sm_w, sh = l.sm_h, inner = sw - 2 * BZ_PAD_TILE;
    /* what's wrong */
    t = card(page, l.sm_x, l.sm_y, sw, sh, "alerts");
    bz_on_tap(t, open_app_tap, (void *)&APP_ALERTS);
    lv_obj_t *ar = bz_row(t, 12);
    lv_obj_set_pos(ar, 0, 38);
    HM.al_mark = bz_mark(ar, BZ_STALE, 14);
    HM.al_count = bz_label_line(ar, "", BZ_F_NAME, BZ_C_INK, inner - 26);
    HM.al_line = bz_label(t, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_size(HM.al_line, inner, 2 * lv_font_get_line_height(bz_font(BZ_F_BODY_S)));
    lv_label_set_long_mode(HM.al_line, LV_LABEL_LONG_DOT);
    lv_obj_align(HM.al_line, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the tablet itself, and how it reaches things: its charge, then its network and tether along the foot */
    t = card(page, l.sm_x + sw + BZ_GAP, l.sm_y, sw, sh, "this tablet");
    bz_on_tap(t, open_app_tap, (void *)&APP_SYSMON);
    HM.tb_batt = bz_label(t, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(HM.tb_batt, 0, 30);
    HM.tb_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_style_text_align(HM.tb_foot, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(HM.tb_foot, LV_ALIGN_TOP_RIGHT, 0, 40);
    lv_obj_t *lr = bz_row(t, 8);
    lv_obj_align(lr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    bz_icon(lr, BZ_I_WIFI, 24, BZ_C_DIM);
    /* icon, name, icon, word: the name takes what the others leave (30 px icons, 8 px apart) */
    HM.ln_wifi = bz_label_line(lr, "", BZ_F_BODY_S, BZ_C_INK, inner - 30 - 30 - 52 - 3 * 8);
    bz_icon(lr, BZ_I_USB, 24, BZ_C_DIM);
    HM.ln_usb = bz_label_line(lr, "", BZ_F_BODY_S, BZ_C_INK, 52);

    /* the apps reached for most, on the ground */
    HM.quick = bz_row(page, 0);
    lv_obj_set_width(HM.quick, W - 2 * PAD);
    lv_obj_set_flex_flow(HM.quick, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(HM.quick, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_pos(HM.quick, PAD, l.quick_y);
    quick_build();
    ui_on_page_refresh(PG_HOME, home_refresh, NULL);
}
