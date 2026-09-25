/* blue alliance — the team's event from The Blue Alliance: the next match and when, where the team stands,
 * every match of ours there, and the event's rankings.
 *
 * Left, top to bottom: the event, the next match (its countdown the one label that changes on its own, once
 * a minute), and the team's standing. Right: our matches or the event's rankings, a chip apart. The data
 * comes from tba.h on the home worker, polled only while this app is open; the lists are built again only
 * when their content changes, and labels are set only when their text does. */
#include "ui_internal.h"
#include "tba.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define TB_BSS EXT_RAM_BSS_ATTR /* zeroed at start, like any .bss: no initialisers */
#else
#define TB_BSS
#endif

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define LW 420                      /* the left column */
#define RX (PAD + LW + BZ_GAP)
#define RW (W - PAD - RX)
#define EV_H 140
#define NX_H 226
#define ST_H (APP_H - EV_H - NX_H - 2 * BZ_GAP)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)
#define ROW_H 96                    /* a match */
#define TEAM_W 84

typedef struct {
    lv_obj_t *bar, *team[3];
} side_t;

static TB_BSS struct {
    tba_state_t *s;                 /* the UI's copy (PSRAM) */
    unsigned gen;
    uint32_t match_sig, rank_sig;
    bool show_ranks;
    lv_obj_t *chip_m, *chip_r;
    lv_obj_t *ev_name, *ev_where, *ev_as_of;
    lv_obj_t *nx_label, *nx_when, *nx_note;
    side_t nx_side[2];
    lv_obj_t *st_rank, *st_of, *st_alliance, *st_line;
    lv_obj_t *wrap_m, *wrap_r, *list_m, *list_r;
} TB;

/* ------------------------------------------------------------------ helpers */

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* "in 12 min", "in 1 h 05", "at 14:05", "sat 09:30", "now", "running late" */
static void when_text(time_t when, time_t now, char *out, size_t n)
{
    if (!when) {
        snprintf(out, n, "time not set");
        return;
    }
    long d = (long)(when - now);
    struct tm a, b;
    localtime_r(&when, &a);
    localtime_r(&now, &b);
    if (d < -600) snprintf(out, n, "running late");
    else if (d < 60) snprintf(out, n, "now");
    else if (d < 3600) snprintf(out, n, "in %ld min", d / 60);
    else if (d < 3 * 3600) snprintf(out, n, "in %ld h %02ld", d / 3600, d / 60 % 60);
    else if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) strftime(out, n, "at %H:%M", &a);
    else {
        static const char *const DAY[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
        snprintf(out, n, "%s %02d:%02d", DAY[a.tm_wday], a.tm_hour, a.tm_min);
    }
}

static void clock_text(time_t t, char *out, size_t n)
{
    struct tm a;
    localtime_r(&t, &a);
    static const char *const DAY[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    snprintf(out, n, "%s %02d:%02d", DAY[a.tm_wday], a.tm_hour, a.tm_min);
}

/* An alliance's line: a colour bar (amber red, ice blue: not status colours) and three team numbers. */
static void side_build(lv_obj_t *parent, side_t *sd, int x, int y, bool blue)
{
    sd->bar = bz_box(parent);
    lv_obj_add_style(sd->bar, bz_style_fill(blue ? BZ_C_ICE : BZ_C_AMBER), 0);
    lv_obj_set_style_bg_opa(sd->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sd->bar, 3, 0);
    lv_obj_set_size(sd->bar, 6, 24);
    lv_obj_set_pos(sd->bar, x, y + 2);
    for (int i = 0; i < 3; i++) {
        sd->team[i] = bz_label_line(parent, "", BZ_F_BODY, BZ_C_INK, TEAM_W - 8);
        lv_obj_set_pos(sd->team[i], x + 18 + i * TEAM_W, y);
    }
}

static void side_set(side_t *sd, const int *teams, int us)
{
    for (int i = 0; i < 3; i++) {
        if (teams[i]) ui_text(sd->team[i], "%d", teams[i]);
        else ui_text(sd->team[i], " ");
        bz_set_color(sd->team[i], teams[i] && teams[i] == us ? BZ_C_SIGNAL : BZ_C_INK);
    }
}

/* ------------------------------------------------------------------ the lists */

static void hint(lv_obj_t *list, const char *text, const char *more)
{
    lv_obj_t *t = bz_tile(list, RW, 150);
    bz_tile_set_fill(t, BZ_C_SURFACE2);
    lv_obj_t *l = bz_label(t, text, BZ_F_BODY, BZ_C_INK);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, IN(RW));
    if (more) {
        lv_obj_t *m = bz_label(t, more, BZ_F_CAPTION, BZ_C_DIM);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, IN(RW));
        lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

/* the empty state's words for a phase, or NULL when there is data to show */
static const char *empty_text(const tba_state_t *s, const char **more)
{
    *more = NULL;
    switch (s->phase) {
    case TBA_NO_KEY:
        *more = "a read key from thebluealliance.com/account (read api keys); docs/keys.md has the format";
        return "add TBA_API_KEY to CATOS/KEYS.ENV on the card, then restart";
    case TBA_NO_TEAM: return "set the team number in settings";
    case TBA_NO_CLOCK: return "waiting for the clock: the season and the day come from it";
    case TBA_REFUSED:
        *more = "check TBA_API_KEY in CATOS/KEYS.ENV: a read key, not a write key";
        return "the blue alliance refused the key";
    case TBA_NO_EVENT: return "no events for this team this season";
    case TBA_IDLE:
    case TBA_LOADING: return s->offline ? s->err : "asking the blue alliance...";
    case TBA_READY: return NULL;
    }
    return NULL;
}

static void matches_build(const tba_state_t *s)
{
    lv_obj_clean(TB.list_m);
    const char *more, *e = empty_text(s, &more);
    if (e) {
        hint(TB.list_m, e, more);
        return;
    }
    if (!s->nmatches) {
        hint(TB.list_m, "no match schedule yet", "the schedule usually appears the evening before qualifications");
        return;
    }
    int next = -1;
    for (int i = 0; i < s->nmatches && next < 0; i++)
        if (!s->matches[i].played && s->matches[i].ours) next = i;
    for (int i = 0; i < s->nmatches; i++) {
        const tba_match_t *m = &s->matches[i];
        lv_obj_t *t = bz_tile(TB.list_m, RW, ROW_H);
        bz_tile_set_fill(t, i == next ? BZ_C_SURFACE3 : BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(t, 12, 0);
        lv_obj_set_style_pad_hor(t, 20, 0);
        bz_label_line(t, m->label, BZ_F_NAME, i == next ? BZ_C_SIGNAL : BZ_C_INK, 110);
        char b[32];
        bz_color_role_t sub_c = BZ_C_DIM;
        if (m->played) {
            snprintf(b, sizeof b, "%s", !m->ours ? "played" : m->result > 0 ? "won" : m->result < 0 ? "lost" : "tie");
            if (m->result > 0) sub_c = BZ_C_INK;
        } else {
            time_t w = m->predicted ? m->predicted : m->time; /* the countdown is the left column's */
            if (w) clock_text(w, b, sizeof b);
            else snprintf(b, sizeof b, "not played");
        }
        lv_obj_t *sub = bz_label_line(t, b, BZ_F_CAPTION, sub_c, 116);
        lv_obj_set_pos(sub, 0, 44);
        side_t sd[2];
        side_build(t, &sd[0], 132, 0, false);
        side_build(t, &sd[1], 132, 36, true);
        side_set(&sd[0], m->red, s->team);
        side_set(&sd[1], m->blue, s->team);
        /* the scores, the winner's in ink */
        for (int k = 0; k < 2; k++) {
            int sc = k ? m->blue_score : m->red_score, other = k ? m->red_score : m->blue_score;
            lv_obj_t *l = bz_label(t, "", m->played ? BZ_F_NAME : BZ_F_BODY, m->played && sc > other ? BZ_C_INK : BZ_C_DIM);
            if (m->played) ui_text(l, "%d", sc);
            else ui_text(l, "-");
            lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, k ? 34 : -2);
        }
    }
    lv_obj_t *sp = bz_box(TB.list_m); /* the last row clear of the bottom */
    lv_obj_set_height(sp, 40);
}

static void ranks_build(const tba_state_t *s)
{
    lv_obj_clean(TB.list_r);
    const char *more, *e = empty_text(s, &more);
    if (e) {
        hint(TB.list_r, e, more);
        return;
    }
    if (!s->nranks) {
        hint(TB.list_r, "no rankings yet", "they appear once qualification matches are played");
        return;
    }
    char b[96];
    snprintf(b, sizeof b, "rank  team     w-l-t      played  %.20s", s->sort_name[0] ? s->sort_name : "");
    lv_obj_t *hd = bz_label_line(TB.list_r, b, BZ_F_LABEL, BZ_C_DIM, RW);
    lv_obj_set_style_pad_left(hd, 20, 0);
    lv_obj_set_style_pad_bottom(hd, 6, 0);
    int prec = s->sort_precision < 0 ? 0 : s->sort_precision > 3 ? 3 : s->sort_precision;
    for (int i = 0; i < s->nranks; i++) {
        const tba_rank_t *r = &s->ranks[i];
        bool us = r->team == s->team;
        lv_obj_t *row = bz_box(TB.list_r);
        lv_obj_set_size(row, RW, 40);
        if (us) {
            lv_obj_add_style(row, bz_style_fill(BZ_C_SURFACE3), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, BZ_R_M, 0);
        }
        char rec[16], so[16];
        snprintf(rec, sizeof rec, "%d-%d-%d", r->wins, r->losses, r->ties);
        if (r->sort1 == r->sort1) snprintf(so, sizeof so, "%.*f", prec, r->sort1);
        else snprintf(so, sizeof so, "-");
        snprintf(b, sizeof b, "%4d  %-7d  %-9s  %6d  %s%s", r->rank, r->team, rec, r->played, so, r->dq ? "  dq" : "");
        lv_obj_t *l = bz_label_line(row, b, BZ_F_LABEL, us ? BZ_C_SIGNAL : BZ_C_INK, RW - 40);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
    }
    lv_obj_t *sp = bz_box(TB.list_r);
    lv_obj_set_height(sp, 40);
}

static void show_list(bool ranks)
{
    TB.show_ranks = ranks;
    ui_chip_set(TB.chip_m, !ranks);
    ui_chip_set(TB.chip_r, ranks);
    if (ranks) {
        lv_obj_add_flag(TB.wrap_m, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(TB.wrap_r, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(TB.wrap_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(TB.wrap_m, LV_OBJ_FLAG_HIDDEN);
    }
}

static void list_tap(lv_obj_t *o, void *u)
{
    (void)o;
    if (TB.show_ranks != (u != NULL)) show_list(u != NULL);
}

static void refresh_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    tba_refresh();
}

/* ------------------------------------------------------------------ the left column */

static void left_update(const tba_state_t *s, time_t now)
{
    char b[48];
    if (s->have_event) {
        ui_text(TB.ev_name, "%s", s->event_name[0] ? s->event_name : s->event_key);
        ui_text(TB.ev_where, "%s%s%.5s - %.5s%s", s->event_where, s->event_where[0] ? " \xc2\xb7 " : "", s->start + 5,
                s->end + 5, s->live ? " \xc2\xb7 today" : "");
    } else {
        ui_text(TB.ev_name, "%s", s->team > 0 ? "no event" : "no team");
        ui_text(TB.ev_where, "team %d", s->team);
    }
    if (s->as_of) {
        clock_text(s->as_of, b, sizeof b);
        ui_text(TB.ev_as_of, "%s%s", s->offline ? "offline \xc2\xb7 as of " : "updated ", b);
    } else ui_text(TB.ev_as_of, "%s", s->offline ? s->err : " ");
    bz_set_color(TB.ev_as_of, s->offline ? BZ_C_WARN : BZ_C_DIM);

    const tba_match_t *nx = NULL;
    for (int i = 0; i < s->nmatches && !nx; i++)
        if (!s->matches[i].played && s->matches[i].ours) nx = &s->matches[i];
    if (nx) {
        time_t w = nx->predicted ? nx->predicted : nx->time;
        ui_text(TB.nx_label, "%s", nx->label);
        when_text(w, now, b, sizeof b);
        ui_text(TB.nx_when, "%s", b);
        char at[24] = "";
        if (w) clock_text(w, at, sizeof at);
        ui_text(TB.nx_note, "%s alliance%s%s%s", nx->ours == 1 ? "red" : "blue", w ? " \xc2\xb7 " : "",
                w ? (nx->predicted ? "predicted " : "scheduled ") : "", at);
        side_set(&TB.nx_side[0], nx->red, s->team);
        side_set(&TB.nx_side[1], nx->blue, s->team);
    } else {
        static const int NONE[3] = { 0, 0, 0 };
        ui_text(TB.nx_label, "%s", "\xe2\x80\x94");
        ui_text(TB.nx_when, " ");
        ui_text(TB.nx_note, "%s", s->nmatches ? "no more matches here" : "no schedule yet");
        side_set(&TB.nx_side[0], NONE, 0);
        side_set(&TB.nx_side[1], NONE, 0);
    }

    if (s->have_status && s->rank > 0) {
        ui_text(TB.st_rank, "%d", s->rank);
        ui_text(TB.st_of, "of %d \xc2\xb7 %d-%d-%d", s->num_teams, s->wins, s->losses, s->ties);
    } else {
        ui_text(TB.st_rank, "%s", "\xe2\x80\x94");
        ui_text(TB.st_of, "%s", s->have_status ? "not ranked yet" : " ");
    }
    ui_text(TB.st_alliance, "%s%s%s", s->alliance, s->alliance[0] && s->playoff[0] ? " \xc2\xb7 " : "", s->playoff);
    ui_text(TB.st_line, "%s", s->status_line[0] ? s->status_line : " ");
}

/* ------------------------------------------------------------------ the app */

static lv_obj_t *column_tile(lv_obj_t *b, int y, int h, const char *cap)
{
    lv_obj_t *t = bz_tile(b, LW, h);
    lv_obj_set_pos(t, PAD, y);
    bz_label(t, cap, BZ_F_LABEL, BZ_C_DIM);
    return t;
}

static void tba_build(lv_obj_t *b)
{
    TB.s = calloc(1, sizeof *TB.s); /* ~9 KB: PSRAM */
    lv_obj_t *hr = bz_row(b, 14);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, -PAD, 18);
    lv_obj_set_height(hr, 60);
    TB.chip_m = ui_chip(hr, "matches", list_tap, NULL);
    TB.chip_r = ui_chip(hr, "rankings", list_tap, (void *)1);
    ui_button(hr, BZ_I_REFRESH, "refresh", refresh_tap, NULL);

    lv_obj_t *t = column_tile(b, APP_Y, EV_H, "event");
    TB.ev_name = bz_label_line(t, "", BZ_F_NAME, BZ_C_INK, IN(LW));
    lv_obj_set_pos(TB.ev_name, 0, 24);
    TB.ev_where = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, IN(LW));
    lv_obj_set_pos(TB.ev_where, 0, 56);
    TB.ev_as_of = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, IN(LW));
    lv_obj_align(TB.ev_as_of, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    t = column_tile(b, APP_Y + EV_H + BZ_GAP, NX_H, "next match");
    TB.nx_label = bz_label(t, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(TB.nx_label, 0, 20);
    TB.nx_when = bz_label_line(t, "", BZ_F_NAME, BZ_C_SIGNAL, IN(LW) - 150);
    lv_obj_align(TB.nx_when, LV_ALIGN_TOP_RIGHT, 0, 40);
    lv_obj_set_style_text_align(TB.nx_when, LV_TEXT_ALIGN_RIGHT, 0);
    side_build(t, &TB.nx_side[0], 0, 86, false);
    side_build(t, &TB.nx_side[1], 0, 116, true);
    TB.nx_note = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, IN(LW));
    lv_obj_align(TB.nx_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    t = column_tile(b, APP_Y + EV_H + NX_H + 2 * BZ_GAP, ST_H, "standing");
    TB.st_rank = bz_label(t, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(TB.st_rank, 0, 20);
    TB.st_of = bz_label_line(t, "", BZ_F_BODY, BZ_C_DIM, IN(LW) - 110);
    lv_obj_set_pos(TB.st_of, 110, 44);
    TB.st_alliance = bz_label_line(t, "", BZ_F_BODY_S, BZ_C_INK, IN(LW));
    lv_obj_set_pos(TB.st_alliance, 0, 84);
    TB.st_line = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_label_set_long_mode(TB.st_line, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(TB.st_line, IN(LW));
    lv_obj_set_pos(TB.st_line, 0, 110);

    TB.wrap_m = bz_box(b);
    lv_obj_set_pos(TB.wrap_m, RX, APP_Y);
    lv_obj_set_size(TB.wrap_m, RW, APP_H);
    TB.list_m = ui_scroller(TB.wrap_m, RW, APP_H);
    TB.wrap_r = bz_box(b);
    lv_obj_set_pos(TB.wrap_r, RX, APP_Y);
    lv_obj_set_size(TB.wrap_r, RW, APP_H);
    TB.list_r = ui_scroller(TB.wrap_r, RW, APP_H);
    lv_obj_set_style_pad_row(TB.list_r, 4, 0); /* rankings are rows of a table, not tiles */
    show_list(false);
    TB.gen = (unsigned)-1;
    TB.match_sig = TB.rank_sig = 1;
}

static void tba_open(void)
{
    /* the key is kv's (NVS: this thread); handed over, then wiped from here */
    char *key = calloc(1, 1024);
    if (key) {
        if (!hal_kv_get("tba_key", key, 1024)) key[0] = 0;
        size_t l = strlen(key);
        while (l && (key[l - 1] == ' ' || key[l - 1] == '\r' || key[l - 1] == '\n')) key[--l] = 0;
        tba_config(key, S.team);
        memset(key, 0, 1024);
        free(key);
    }
    tba_want();
    tba_refresh();
}

static void tba_ui_refresh(void)
{
    if (!TB.s) return;
    tba_want();
    unsigned g = tba_gen();
    time_t now = time(NULL);
    if (g != TB.gen) {
        TB.gen = g;
        tba_get(TB.s);
        const tba_state_t *s = TB.s;
        /* the lists only when what they show changed (not for a new "updated" time) */
        uint32_t ms = fnv(s->matches, sizeof s->matches[0] * (size_t)s->nmatches, 2166136261u ^ (uint32_t)s->phase);
        ms = fnv(&s->team, sizeof s->team, fnv(&s->offline, sizeof s->offline, ms));
        uint32_t rs = fnv(s->ranks, sizeof s->ranks[0] * (size_t)s->nranks, 2166136261u ^ (uint32_t)s->phase);
        rs = fnv(s->sort_name, sizeof s->sort_name, fnv(&s->team, sizeof s->team, rs));
        if (ms != TB.match_sig) {
            TB.match_sig = ms;
            matches_build(s);
        }
        if (rs != TB.rank_sig) {
            TB.rank_sig = rs;
            ranks_build(s);
        }
    }
    left_update(TB.s, now); /* every label guarded: the countdown changes the text once a minute */
}

const ui_app_t APP_TBA = { .name = "tba", .icon = BZ_I_BAR_CHART, .build = tba_build, .open = tba_open,
                           .refresh = tba_ui_refresh };
