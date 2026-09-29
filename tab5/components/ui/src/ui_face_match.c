/* The match face: the team's next match as a clock face, from what the blue alliance app already polls (tba.h:
 * no request of its own). The match's name big, how long until it, and the alliance card: our colour, us and our
 * partners, the opponents; under it the event, our rank and record, and the last result. Without a next match it
 * says why (the event is over, not started, nothing scheduled, no key) and shows the season instead.
 * LVGL objects only: a minute rewrites the countdown, nothing else. */
#include "ui_home_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MX 56
#define CARD_X 776
#define CARD_Y 118
#define CARD_W 448
#define CARD_H 452

static struct {
    lv_obj_t *time, *big_time, *head, *label, *when, *at, *event, *record;
    lv_obj_t *card, *bar, *side, *us[3], *vs, *them[3];
    lv_obj_t *foot1, *foot2, *hint;
    int ours_shown;            /* the alliance the card is coloured for (-1 none) */
} MT;

static lv_obj_t *num_label(lv_obj_t *p, bz_font_role_t f, bz_color_role_t c, int x, int y)
{
    lv_obj_t *l = bz_label_line(p, "", f, c, CARD_W - 80);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void match_build(lv_obj_t *b)
{
    MT.time = bz_label(b, "", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(MT.time, MX - 6, 26);
    /* without a match to come the time is the face, as large as the other faces' */
    MT.big_time = lv_label_create(b);
    lv_obj_set_style_text_font(MT.big_time, &hm_font_thin, 0);
    MT.event = bz_label_line(b, "", BZ_F_NAME, BZ_C_INK, 1100);
    lv_obj_set_pos(MT.event, MX, 336);
    MT.record = bz_label_line(b, "", BZ_F_TITLE, BZ_C_INK, 1100);
    lv_obj_set_pos(MT.record, MX, 376);
    MT.head = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 660);
    lv_obj_set_pos(MT.head, MX, 196);
    MT.label = lv_label_create(b);
    lv_obj_set_style_text_font(MT.label, &hm_font_match, 0);
    MT.when = bz_label_line(b, "", BZ_F_TITLE, BZ_C_INK, 660);
    MT.at = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 660);
    MT.hint = bz_label(b, "", BZ_F_BODY, BZ_C_DIM);
    lv_label_set_long_mode(MT.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(MT.hint, 640);
    lv_obj_set_pos(MT.hint, MX, 470);
    /* the alliance card: a tile in our alliance's colour, the bar down its edge in the colour itself */
    MT.card = bz_tile(b, CARD_W, CARD_H);
    lv_obj_set_pos(MT.card, CARD_X, CARD_Y);
    lv_obj_remove_flag(MT.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(MT.card, 0, 0);
    MT.bar = bz_box(MT.card);
    lv_obj_set_size(MT.bar, 8, CARD_H - 72);
    lv_obj_set_pos(MT.bar, 30, 36);
    lv_obj_set_style_radius(MT.bar, 4, 0);
    lv_obj_set_style_bg_opa(MT.bar, LV_OPA_COVER, 0);
    MT.side = num_label(MT.card, BZ_F_LABEL, BZ_C_DIM, 64, 34);
    for (int i = 0; i < 3; i++) MT.us[i] = num_label(MT.card, BZ_F_VALUE, BZ_C_INK, 64, 70 + i * 64);
    MT.vs = num_label(MT.card, BZ_F_LABEL, BZ_C_DIM, 64, 290);
    for (int i = 0; i < 3; i++) MT.them[i] = num_label(MT.card, BZ_F_NAME, BZ_C_DIM, 64, 324 + i * 36);
    MT.foot1 = bz_label_line(b, "", BZ_F_LABEL, BZ_C_INK, 1100);
    lv_obj_set_pos(MT.foot1, MX, 612);
    MT.foot2 = bz_label_line(b, "", BZ_F_LABEL, BZ_C_DIM, 1100);
    lv_obj_set_pos(MT.foot2, MX, 640);
    MT.ours_shown = -2;
}

static void countdown(double s, char *out, size_t n)
{
    int m = (int)ceil(s / 60.0);
    if (m <= 0) snprintf(out, n, "now");
    else if (m < 60) snprintf(out, n, "in %d min", m);
    else if (m < 24 * 60) snprintf(out, n, "in %d h %02d min", m / 60, m % 60);
    else snprintf(out, n, "in %d days", (m + 12 * 60) / (24 * 60));
}

static void card_colors(int ours)
{
    if (ours == MT.ours_shown) return;
    MT.ours_shown = ours;
    bool dark = bz_ui_dark();
    uint32_t col = ours == 1 ? (dark ? 0xFF6166 : 0xD7263D) : ours == 2 ? (dark ? 0x6E9BFF : 0x2F5BD6) : bz_color(BZ_C_FAINT);
    uint32_t fill = ours ? bz_mix(bz_color(BZ_C_SURFACE1), col, dark ? 0.12f : 0.08f) : bz_color(BZ_C_SURFACE1);
    lv_obj_set_style_bg_color(MT.card, bz_lv_rgb(fill), 0);
    lv_obj_set_style_bg_color(MT.bar, bz_lv_rgb(col), 0);
}

static void team_text(lv_obj_t *l, int team, bool us)
{
    if (team <= 0) ui_text(l, "%s", "");
    else ui_text(l, "%d%s", team, us ? "  \xc2\xb7 us" : "");
}

static void match_minute(const struct tm *tm, bool force)
{
    if (force) {
        hm_color(MT.label, bz_color(BZ_C_INK));
        hm_color(MT.big_time, bz_color(BZ_C_INK));
        MT.ours_shown = -2;
    }
    char t[8];
    hm_hm(tm, t, sizeof t);
    ui_text(MT.time, "%s", t);
    ui_text(MT.big_time, "%s", t);
    hm_baseline(MT.big_time, MX - 6, 222);
    const tba_state_t *s = hm_tba();
    tba_match_t m;
    time_t when = 0;
    bool next = tba_next_match(&m, &when);
    int team = s ? s->team : 0;
    if (next) {
        hm_show(MT.hint, false);
        hm_show(MT.label, true);
        hm_show(MT.card, true);
        ui_text(MT.head, "next match \xc2\xb7 %s", s && s->event_name[0] ? s->event_name : "");
        ui_text(MT.label, "%s", m.label);
        hm_baseline(MT.label, MX - 8, 420);
        char c[32], at[16];
        if (when) {
            countdown(difftime(when, time(NULL)), c, sizeof c);
            hm_hm_t(when, at, sizeof at);
            ui_text(MT.when, "%s", c);
            ui_text(MT.at, "%s %s", m.predicted ? "predicted for" : "scheduled for", at);
        } else {
            ui_text(MT.when, "%s", "time to be announced");
            ui_text(MT.at, "%s", "");
        }
        lv_obj_set_pos(MT.when, MX, 450);
        lv_obj_set_pos(MT.at, MX, 506);
        card_colors(m.ours);
        const int *us = m.ours == 1 ? m.red : m.blue, *them = m.ours == 1 ? m.blue : m.red;
        ui_text(MT.side, "%s alliance", m.ours == 1 ? "red" : "blue");
        /* us first, then the partners */
        int k = 0;
        for (int i = 0; i < 3; i++)
            if (us[i] == team) team_text(MT.us[k++], us[i], true);
        for (int i = 0; i < 3; i++)
            if (us[i] != team) team_text(MT.us[k++], us[i], false);
        for (; k < 3; k++) team_text(MT.us[k], 0, false);
        ui_text(MT.vs, "%s", "against");
        for (int i = 0; i < 3; i++) team_text(MT.them[i], them[i], false);
        hm_color(MT.us[0], bz_color(BZ_C_INK));
    } else {
        /* no next match: why, and the season */
        hm_show(MT.card, false);
        hm_show(MT.label, false);
        const char *why;
        if (!s || s->phase == TBA_NO_KEY || s->phase == TBA_IDLE)
            why = "The team's next match shows here: when, the alliance, partners and rank. Add a Blue Alliance key "
                  "(TBA_API_KEY in CATOS/KEYS.ENV) and the team in the blue alliance app.";
        else if (s->phase == TBA_NO_TEAM) why = "Set the team number in the blue alliance app to see its next match.";
        else if (s->phase == TBA_REFUSED) why = "The Blue Alliance refused the key: check TBA_API_KEY.";
        else if (s->phase == TBA_LOADING) why = "Asking the Blue Alliance...";
        else if (s->phase == TBA_NO_EVENT) why = "No events for the team this season yet.";
        else if (s->have_event && s->nmatches && s->end[0]) why = "Nothing more scheduled at the team's event.";
        else why = "No match of ours is scheduled yet.";
        /* the event as it stood: its name, our rank and record, big enough to read from the door */
        lv_obj_set_pos(MT.head, MX, 300);
        bool have = s && s->have_event;
        ui_text(MT.head, "%s", have ? (s->live ? "no match of ours to come today" : "the team's latest event") : "next match");
        ui_text(MT.event, "%s", have ? s->event_name : "");
        if (have && s->have_status && s->rank)
            ui_text(MT.record, "rank %d of %d \xc2\xb7 %d-%d-%d", s->rank, s->num_teams, s->wins, s->losses, s->ties);
        else ui_text(MT.record, "%s", "");
        bool quiet = have && (s->phase == TBA_READY);
        if (quiet) hm_show(MT.hint, false);
        else {
            lv_label_set_text_static(MT.hint, why);
            hm_show(MT.hint, true);
        }
    }
    /* the footnotes: rank and record here, then the last result, or the season */
    if (next && s && s->have_status && s->rank)
        ui_text(MT.foot1, "rank %d of %d \xc2\xb7 %d-%d-%d%s%s", s->rank, s->num_teams, s->wins, s->losses, s->ties,
                s->alliance[0] ? " \xc2\xb7 " : "", s->alliance);
    else if (!next && s && (s->playoff[0] || s->alliance[0]))
        ui_text(MT.foot1, "%s%s%s", s->alliance, s->alliance[0] && s->playoff[0] ? " \xc2\xb7 " : "", s->playoff);
    else ui_text(MT.foot1, "%s", "");
    const tba_match_t *last = NULL;
    for (int i = 0; s && i < s->nmatches; i++)
        if (s->matches[i].played && s->matches[i].ours) last = &s->matches[i];
    tba_season_t se;
    bool season = tba_season(&se);
    if (last) {
        int ours = last->ours == 1 ? last->red_score : last->blue_score, theirs = last->ours == 1 ? last->blue_score
                                                                                                  : last->red_score;
        ui_text(MT.foot2, "last: %s %s %d-%d%s", last->label, last->result > 0 ? "won" : last->result < 0 ? "lost" : "tied",
                ours, theirs, season && se.played ? "" : "");
    } else if (season && se.played) {
        ui_text(MT.foot2, "the %d season: %d matches, %d won%s", se.year, se.played, se.wins,
                se.won ? ", an event won" : "");
    } else {
        ui_text(MT.foot2, "%s", "");
    }
}

const hm_face_t HM_F_MATCH = { .name = "match", .build = match_build, .minute = match_minute };
