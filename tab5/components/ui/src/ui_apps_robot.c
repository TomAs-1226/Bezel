/* Apps that read and (where Catalyst allows) write the robot: preflight, alerts, tune, auto, robot, field. */
#include "ui_internal.h"
#include "as_snap.h"
#include "cat_preflight.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)

static lv_obj_t *app_head_right(lv_obj_t *body)
{
    lv_obj_t *r = bz_row(body, 14);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(r, 60);
    return r;
}

static uint32_t hash_str(uint32_t h, const char *s)
{
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* ================================================================== preflight */

static struct {
    cat_preflight_t pf;
    lv_obj_t *verdict, *vmark, *counts, *meter, *list, *note;
    int shown;
} PF;

static void pf_run(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cat_preflight_start(&PF.pf, ui_now(), S.team);
    PF.shown = -1;
    lv_obj_clean(PF.list);
    /* "listening" in the display face is wider than the tile: a step down while it listens */
    bz_set_font(PF.verdict, BZ_F_VALUE);
    ui_text(PF.verdict, "listening");
    bz_mark_set(PF.vmark, BZ_STALE);
    ui_text(PF.counts, "three seconds of the robot, then the checklist");
}

static const char *verdict_word(cat_verdict_t v)
{
    return v == CAT_FAIL ? "fail" : v == CAT_WARN ? "warn" : v == CAT_PASS ? "pass" : "info";
}

static void pf_show(void)
{
    lv_obj_clean(PF.list);
    for (int i = 0; i < PF.pf.n; i++) {
        const cat_check_t *c = &PF.pf.c[i];
        lv_obj_t *row = bz_tile(PF.list, W - 2 * PAD - 420 - BZ_GAP, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 20, 0);
        lv_obj_set_style_pad_ver(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        bz_status_t st = c->v == CAT_FAIL ? BZ_FAULT : c->v == CAT_WARN ? BZ_WARN : c->v == CAT_PASS ? BZ_OK : BZ_INFO;
        bz_mark(row, st, 12);
        lv_obj_t *w = bz_label(row, verdict_word(c->v), BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(w, 44);
        /* "driver station" is 14 of the mono face's 11 px: 150 wrapped it onto two lines */
        lv_obj_t *what = bz_label(row, c->what, BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(what, 164);
        lv_obj_t *d = bz_label(row, c->detail, BZ_F_BODY_S, BZ_C_INK);
        lv_obj_set_flex_grow(d, 1);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    }
    bool go = PF.pf.fails == 0;
    bz_set_font(PF.verdict, BZ_F_DISPLAY);
    ui_text(PF.verdict, "%s", go ? "go" : "no-go");
    bz_mark_set(PF.vmark, go ? (PF.pf.warns ? BZ_WARN : BZ_OK) : BZ_FAULT);
    ui_text(PF.counts, "%d fail · %d warn · %d pass", PF.pf.fails, PF.pf.warns, PF.pf.passes);
    /* a chime: two rising notes for go, one low for no-go */
    if (go) {
        hal_tone(880, 90, S.volume);
        hal_tone(1320, 140, S.volume);
    } else {
        hal_tone(330, 260, S.volume);
    }
}

static void pf_frame(double now, double dt)
{
    (void)dt;
    if (PF.pf.running) {
        bz_meter_set(PF.meter, (float)cat_preflight_progress(&PF.pf, now), BZ_C_ICE);
        bz_ui_keep_alive();
    }
}

static void pf_refresh(void)
{
    if (PF.pf.running && cat_preflight_step(&PF.pf, R, ui_now())) {
        bz_meter_set(PF.meter, 1, PF.pf.fails ? BZ_C_FAULT : BZ_C_OK);
        pf_show();
    }
}

static void pf_open(void) { pf_run(NULL, NULL); }

static void pf_build(lv_obj_t *b)
{
    lv_obj_t *t = bz_tile(b, 420, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    bz_label(t, "preflight", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *vr = bz_row(t, 16);
    lv_obj_set_pos(vr, 0, 40);
    PF.vmark = bz_mark(vr, BZ_STALE, 18);
    PF.verdict = bz_label(vr, "", BZ_F_DISPLAY, BZ_C_INK);
    PF.counts = bz_label(t, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(PF.counts, 420 - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(PF.counts, 0, 146);
    PF.meter = bz_meter(t, 420 - 2 * BZ_PAD_TILE, 12);
    lv_obj_set_pos(PF.meter, 0, 196);
    PF.note = bz_label(t, "three seconds of the robot: link, program, battery, brownout, can, loop, motors, "
                          "cameras, gyro, pose and auto, with the robot's own preflight and system check. "
                          "any fail is a no-go.",
                       BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(PF.note, 420 - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(PF.note, 0, 236);
    lv_obj_t *btn = ui_button(t, BZ_I_REFRESH, "run again", pf_run, NULL);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD + 420 + BZ_GAP, APP_Y);
    PF.list = ui_scroller(wrap, W - 2 * PAD - 420 - BZ_GAP, APP_H);
    lv_obj_set_style_pad_row(PF.list, 10, 0);
}

const ui_app_t APP_PREFLIGHT = { .name = "preflight", .icon = BZ_I_CHECKLIST, .build = pf_build, .open = pf_open,
                                 .refresh = pf_refresh, .frame = pf_frame };

/* ================================================================== alerts */

static struct {
    lv_obj_t *list, *summary;
    uint32_t sig;
} AL;

static void al_refresh(void)
{
    const cat_robot_t *r = R;
    uint32_t sig = 2166136261u ^ (uint32_t)r->nalerts ^ (uint32_t)r->connected << 16;
    /* the rows show severity and source too: an alert that changes only those must rebuild them */
    for (int i = 0; i < r->nalerts; i++)
        sig = hash_str(hash_str(sig ^ (uint32_t)r->alerts[i].sev * 31u ^ (uint32_t)r->alerts[i].health, r->alerts[i].source),
                       r->alerts[i].text);
    /* offline, "0 errors" would claim a clean robot */
    if (r->connected)
        ui_text(AL.summary, "%d error%s · %d warning%s · %d note%s", r->n_errors, r->n_errors == 1 ? "" : "s", r->n_warnings,
                r->n_warnings == 1 ? "" : "s", r->n_infos, r->n_infos == 1 ? "" : "s");
    else ui_text(AL.summary, "no robot");
    if (sig == AL.sig) return;
    AL.sig = sig;
    lv_obj_clean(AL.list);
    if (!r->connected || !r->nalerts) {
        ui_empty(AL.list, W - 2 * PAD, r->connected ? "nothing to report" : "not connected",
                 "Catalyst's AlertManager, WPILib Alerts groups and firing HealthMonitor checks all land here.");
        return;
    }
    for (int sev = CAT_SEV_ERROR; sev >= CAT_SEV_INFO; sev--) {
        for (int i = 0; i < r->nalerts; i++) {
            const cat_alert_t *a = &r->alerts[i];
            if ((int)a->sev != sev) continue;
            lv_obj_t *row = bz_tile(AL.list, W - 2 * PAD, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 20, 0);
            lv_obj_set_style_pad_ver(row, 16, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 16, 0);
            bz_mark(row, ui_sev_status(a->sev), 12);
            bz_label_line(row, a->source[0] ? a->source : "robot", BZ_F_LABEL, BZ_C_DIM, 150); /* a long source stays in its column */
            lv_obj_t *txt = bz_label(row, a->text, BZ_F_BODY, BZ_C_INK);
            lv_obj_set_flex_grow(txt, 1);
            lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
            if (a->health) bz_label(row, "health check", BZ_F_CAPTION, BZ_C_DIM);
        }
    }
}

static void al_open(void) { AL.sig = 0; }

static void al_build(lv_obj_t *b)
{
    lv_obj_t *r = app_head_right(b);
    AL.summary = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    AL.list = ui_scroller(wrap, W - 2 * PAD, APP_H);
    lv_obj_set_style_pad_row(AL.list, 10, 0);
}

const ui_app_t APP_ALERTS = { .name = "alerts", .icon = BZ_I_WARNING, .build = al_build, .open = al_open,
                              .refresh = al_refresh };

/* ================================================================== tune */

#define TU_W ((W - 2 * PAD - BZ_GAP) / 2)

static struct {
    lv_obj_t *list, *note;
    uint32_t sig;
    lv_obj_t *vals[CAT_MAX_TUNABLES], *levels[CAT_MAX_TUNABLES], *chips[CAT_MAX_TUNABLES];
    int n;
    double last_sent_at[CAT_MAX_TUNABLES];
    bool snapped;
} TU;

/* Before this visit's first change, a snapshot of every tunable, so the assistant (or anyone) can put
 * them all back exactly: as_snap.h. */
static void tu_snapshot(const cat_tunable_t *t)
{
    if (TU.snapped) return;
    char why[96];
    snprintf(why, sizeof why, "before changing %s in tune", t->name);
    TU.snapped = snap_take(R, why) >= 0;
}

static void tu_fmt(char *buf, size_t n, const cat_tunable_t *t, double v)
{
    double step = t->step == t->step ? t->step : 0.01;
    int dec = step >= 1 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
    snprintf(buf, n, "%.*f", dec, v);
}

static void tu_level(lv_obj_t *lv, float v, bool final, void *u)
{
    (void)lv;
    int i = (int)(intptr_t)u;
    if (i >= R->ntunables) return;
    const cat_tunable_t *t = &R->tunables[i];
    char b[24];
    tu_fmt(b, sizeof b, t, v);
    ui_text(TU.vals[i], "%s", b);
    tu_snapshot(t);
    /* live while dragging (NT coalesces), and once more on release */
    cat_set_tunable(R, i, v);
    TU.last_sent_at[i] = ui_now();
    if (final) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s \xe2\x86\x92 %s%s%s", t->name, b, t->unit[0] ? " " : "", t->unit);
        ui_island_say(BZ_I_TUNE, msg);
        hal_tone(1800, 12, S.volume * 0.5f);
    }
}

static void tu_toggle(lv_obj_t *o, void *u)
{
    int i = (int)(intptr_t)u;
    if (i >= R->ntunables) return;
    const cat_tunable_t *t = &R->tunables[i];
    bool on = !(t->have && t->value != 0);
    tu_snapshot(t);
    cat_set_tunable(R, i, on);
    TU.last_sent_at[i] = ui_now();
    ui_chip_set(o, on);
    ui_text(lv_obj_get_child(o, 0), "%s", on ? "on" : "off");
    char msg[96];
    snprintf(msg, sizeof msg, "%s \xe2\x86\x92 %s", t->name, on ? "on" : "off");
    ui_island_say(BZ_I_TUNE, msg);
}

static void tu_step(lv_obj_t *o, void *u)
{
    (void)o;
    int code = (int)(intptr_t)u, i = code >> 1;
    if (i >= R->ntunables) return;
    const cat_tunable_t *t = &R->tunables[i];
    double step = t->step == t->step ? t->step : (fabs(t->value) > 1 ? 0.1 : 0.01);
    double v = t->value + ((code & 1) ? step : -step);
    tu_snapshot(t);
    cat_set_tunable(R, i, v);
    TU.last_sent_at[i] = ui_now();
    char b[24], msg[96];
    tu_fmt(b, sizeof b, t, v);
    ui_text(TU.vals[i], "%s", b);
    snprintf(msg, sizeof msg, "%s \xe2\x86\x92 %s", t->name, b);
    ui_island_say(BZ_I_TUNE, msg);
}

static void tu_rebuild(void)
{
    const cat_robot_t *r = R;
    lv_obj_clean(TU.list);
    TU.n = r->ntunables;
    if (!r->ntunables) {
        ui_empty(TU.list, W - 2 * PAD, r->connected ? "nothing to tune" : "not connected",
                 "A robot declares what may be tuned in /Catalyst/Tunables/.manifest (2.x and teams that publish one); "
                 "1.x TunableNumbers under /Catalyst/Tuning/ appear too. Nothing undeclared is ever written.");
        return;
    }
    lv_obj_t *grid = bz_row(TU.list, BZ_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(grid, W - 2 * PAD);
    for (int i = 0; i < r->ntunables; i++) {
        const cat_tunable_t *t = &r->tunables[i];
        lv_obj_t *tile = bz_tile(grid, TU_W, 150);
        /* the value is right-aligned in a fixed box so a wider number never re-flows the tile; the group and
         * name keep to the left of that box (and the unit), so a long name is cut rather than run under it */
        int unit_w = 0;
        if (t->unit[0]) {
            lv_obj_t *un = bz_label(tile, t->unit, BZ_F_LABEL, BZ_C_DIM);
            lv_obj_align(un, LV_ALIGN_TOP_RIGHT, 0, 26);
            lv_obj_update_layout(un);
            unit_w = lv_obj_get_width(un) + 8;
        }
        int name_w = TU_W - 2 * BZ_PAD_TILE - 220 - unit_w - 12;
        bz_label_line(tile, t->group[0] ? t->group : "tunable", BZ_F_LABEL, BZ_C_DIM, name_w);
        lv_obj_t *nm = bz_label_line(tile, t->name, BZ_F_NAME, BZ_C_INK, name_w);
        lv_obj_set_pos(nm, 0, 26);
        TU.vals[i] = bz_label_line(tile, "\xe2\x80\x94", BZ_F_VALUE, BZ_C_INK, 220);
        lv_obj_set_style_text_align(TU.vals[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(TU.vals[i], LV_ALIGN_TOP_RIGHT, -unit_w, -8);
        TU.levels[i] = TU.chips[i] = NULL;
        if (t->is_bool) {
            lv_obj_t *c = ui_chip(tile, t->have && t->value ? "on" : "off", tu_toggle, (void *)(intptr_t)i);
            ui_chip_set(c, t->have && t->value);
            lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            TU.chips[i] = c;
        } else if (t->min == t->min && t->max == t->max && t->max > t->min) {
            lv_obj_t *lv = bz_level(tile, TU_W - 2 * BZ_PAD_TILE, 52, (float)t->min, (float)t->max,
                                    (float)(t->step == t->step ? t->step : (t->max - t->min) / 100));
            lv_obj_align(lv, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            bz_level_set(lv, (float)t->value, false);
            bz_level_on_change(lv, tu_level, (void *)(intptr_t)i);
            TU.levels[i] = lv;
        } else {
            /* no range declared: steppers, never a slider that could jump the value */
            lv_obj_t *sr = bz_row(tile, 10);
            lv_obj_align(sr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            ui_button(sr, BZ_I_REMOVE, NULL, tu_step, (void *)(intptr_t)(i << 1));
            ui_button(sr, BZ_I_ADD, NULL, tu_step, (void *)(intptr_t)(i << 1 | 1));
            bz_label(sr, "no range declared", BZ_F_CAPTION, BZ_C_DIM);
        }
    }
}

static void tu_refresh(void)
{
    const cat_robot_t *r = R;
    uint32_t sig = 2166136261u ^ (uint32_t)r->ntunables ^ (uint32_t)r->connected << 20;
    for (int i = 0; i < r->ntunables; i++) sig = hash_str(sig, r->tunables[i].key);
    if (sig != TU.sig) {
        TU.sig = sig;
        tu_rebuild();
    }
    for (int i = 0; i < TU.n && i < r->ntunables; i++) {
        const cat_tunable_t *t = &r->tunables[i];
        /* the robot's echo, unless we just wrote it (it may clamp; the next refresh shows that) */
        if (ui_now() - TU.last_sent_at[i] < 0.6) continue;
        char b[24];
        if (t->is_bool) {
            ui_text(TU.vals[i], "%s", t->have ? (t->value ? "on" : "off") : "\xe2\x80\x94");
            if (TU.chips[i]) {
                /* the chip's word follows the robot too, not only its fill: it said "on" over an off value */
                ui_chip_set(TU.chips[i], t->have && t->value);
                ui_text(lv_obj_get_child(TU.chips[i], 0), "%s", t->have && t->value ? "on" : "off");
            }
        } else {
            if (t->have) tu_fmt(b, sizeof b, t, t->value);
            ui_text(TU.vals[i], "%s", t->have ? b : "\xe2\x80\x94");
            if (TU.levels[i] && t->have) bz_level_set(TU.levels[i], (float)t->value, true);
        }
    }
    ui_text(TU.note, "%s", r->have_manifest ? "declared by the robot's manifest" : r->ntunables ? "1.x TunableNumbers" : "");
}

static void tu_open(void)
{
    TU.sig = 0;
    TU.snapped = false;
}

static void tu_build(lv_obj_t *b)
{
    lv_obj_t *r = app_head_right(b);
    TU.note = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    TU.list = ui_scroller(wrap, W - 2 * PAD, APP_H);
}

const ui_app_t APP_TUNE = { .name = "tune", .icon = BZ_I_TUNE, .build = tu_build, .open = tu_open, .refresh = tu_refresh };

/* ================================================================== auto */

static struct {
    lv_obj_t *grid, *note, *chips[CAT_MAX_AUTOS], *marks[CAT_MAX_AUTOS];
    uint32_t sig;
    int n;
} AU;

static void au_pick(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i >= R->nautos) return;
    cat_select_auto(R, R->autos[i]);
    snprintf(R->auto_selected, sizeof R->auto_selected, "%s", R->autos[i]);
    char msg[80];
    snprintf(msg, sizeof msg, "auto \xe2\x86\x92 %s", R->autos[i]);
    ui_island_say(BZ_I_FLAG, msg);
    hal_tone(1500, 15, S.volume * 0.5f);
}

static void au_refresh(void)
{
    const cat_robot_t *r = R;
    uint32_t sig = 2166136261u ^ (uint32_t)r->nautos ^ (uint32_t)r->connected << 16; /* the empty state's words too */
    for (int i = 0; i < r->nautos; i++) sig = hash_str(sig, r->autos[i]);
    if (sig != AU.sig) {
        AU.sig = sig;
        lv_obj_clean(AU.grid);
        AU.n = r->nautos;
        int w = (W - 2 * PAD - 2 * BZ_GAP) / 3;
        for (int i = 0; i < r->nautos; i++) {
            lv_obj_t *t = bz_tile(AU.grid, w, 130);
            lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
            bz_on_tap(t, au_pick, (void *)(intptr_t)i);
            lv_obj_t *l = bz_label(t, r->autos[i], BZ_F_NAME, BZ_C_INK);
            lv_obj_set_width(l, w - 2 * BZ_PAD_TILE); /* a long routine's name wraps inside its tile */
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            AU.marks[i] = bz_label(t, "", BZ_F_LABEL, BZ_C_DIM);
            AU.chips[i] = t;
        }
        if (!r->nautos)
            ui_empty(AU.grid, W - 2 * PAD, r->connected ? "no auto selector" : "not connected",
                     "Catalyst's AutoSelector: /Auto Selector on 2.x, /SmartDashboard/Auto Selector on 1.x.");
    }
    for (int i = 0; i < AU.n && i < r->nautos; i++) {
        bool sel = !strcmp(r->autos[i], r->auto_selected), act = !strcmp(r->autos[i], r->auto_active);
        bz_tile_set_fill(AU.chips[i], sel ? BZ_C_ICE : BZ_C_SURFACE1);
        uint32_t n = lv_obj_get_child_count(AU.chips[i]);
        for (uint32_t k = 0; k < n; k++) bz_set_color(lv_obj_get_child(AU.chips[i], (int32_t)k), sel ? BZ_C_ON_ICE : (k ? BZ_C_DIM : BZ_C_INK));
        ui_text(AU.marks[i], "%s", act ? "active" : sel ? "selected" : "");
    }
    if (r->line2 && r->opmode[0])
        ui_text(AU.note, "%s · on 2.x the Driver Station's op mode (%s) wins when autos are published as op modes",
                r->auto_base, r->opmode);
    else ui_text(AU.note, "%s", r->have_autos ? r->auto_base : "");
}

static void au_open(void) { AU.sig = 0; }

#define AU_NOTE_H 44 /* two caption lines under the list */

static void au_build(lv_obj_t *b)
{
    AU.note = bz_label(b, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(AU.note, W - 2 * PAD); /* the 2.x op-mode note wraps rather than running off the screen */
    lv_obj_align(AU.note, LV_ALIGN_BOTTOM_LEFT, PAD, -PAD);
    /* the autos scroll: past nine of them (24 are allowed) the rest were off the bottom of the screen */
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    lv_obj_t *col = ui_scroller(wrap, W - 2 * PAD, APP_H - AU_NOTE_H);
    AU.grid = bz_row(col, BZ_GAP);
    lv_obj_set_flex_flow(AU.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(AU.grid, BZ_GAP, 0);
    lv_obj_set_width(AU.grid, W - 2 * PAD);
}

const ui_app_t APP_AUTO = { .name = "auto", .icon = BZ_I_FLAG, .build = au_build, .open = au_open, .refresh = au_refresh };

/* ================================================================== robot */

typedef struct { lv_obj_t *k, *v; } kv_t;

static struct {
    lv_obj_t *name, *sub;
    kv_t id[6], sw[6], link[6], sc[6], rt[6];
} RB;

static lv_obj_t *kv_tile(lv_obj_t *parent, int w, int h, const char *title, kv_t *rows, int n)
{
    lv_obj_t *t = bz_tile(parent, w, h);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    /* a title and six 23 px rows in 250 - 48: at 10 px apart the last row ran into the bottom padding */
    lv_obj_set_style_pad_row(t, 6, 0);
    bz_label(t, title, BZ_F_LABEL, BZ_C_DIM);
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = bz_row(t, 10);
        lv_obj_set_width(r, w - 2 * BZ_PAD_TILE);
        rows[i].k = bz_label(r, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(rows[i].k, 156);
        rows[i].v = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, w - 2 * BZ_PAD_TILE - 156 - 10); /* beside the key */
    }
    return t;
}

static void kv(kv_t *row, const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void kv(kv_t *row, const char *k, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_text(row->k, "%s", k);
    ui_text(row->v, "%s", buf[0] ? buf : "\xe2\x80\x94");
}

static void rb_refresh(void)
{
    const cat_robot_t *r = R;
    nt4_status_t st;
    nt4_status(cat_nt(), &st);
    ui_text(RB.name, "%s", r->have_identity ? r->name : r->connected ? "unnamed robot" : "no robot");
    ui_text(RB.sub, "%s", r->connected ? (r->line2 ? "catalyst 2.x · systemcore" : "catalyst 1.x · roborio") : "looking…");
    if (r->have_identity) kv(&RB.id[0], "team", "%d", r->team);
    else kv(&RB.id[0], "team", "%s", ""); /* not published yet: a dash, not 0 */
    kv(&RB.id[1], "controller", "%s", r->controller);
    kv(&RB.id[2], "mode", "%s", cat_mode_name(r));
    kv(&RB.id[3], "alliance", "%s", r->have_alliance ? (r->red ? "red" : "blue") : "");
    kv(&RB.id[4], "op mode", "%s", r->opmode);
    kv(&RB.id[5], "match", "%s", r->match_time == r->match_time ? "running" : "");
    kv(&RB.sw[0], "catalyst", "%s", r->catalyst_version);
    kv(&RB.sw[1], "git", "%s", r->git_sha);
    kv(&RB.sw[2], "robot code", "%s", r->code_version);
    kv(&RB.sw[3], "wpilib", "%s", r->wpilib_version);
    kv(&RB.sw[4], "tunables", "%d%s", r->ntunables, r->have_manifest ? " declared" : "");
    kv(&RB.sw[5], "mechanisms", "%d", r->nmechs);
    kv(&RB.link[0], "address", "%s", st.address);
    kv(&RB.link[1], "protocol", "%s", st.protocol);
    if (st.state == NT4_CONNECTED) kv(&RB.link[2], "round trip", "%.1f ms", st.rtt_us / 1000.0);
    else kv(&RB.link[2], "round trip", "%s", "");
    kv(&RB.link[3], "topics", "%u", (unsigned)st.topics);
    kv(&RB.link[4], "received", "%.1f kB", st.rx_bytes / 1024.0);
    kv(&RB.link[5], "last error", "%s", st.last_error);
    if (r->have_sc) {
        /* each published on its own: one missing is a dash, never "nan" */
        char f[4][24];
        kv(&RB.sc[0], "cpu", "%s", bz_fmt(f[0], sizeof f[0], true, "%.0f %%", r->sc_cpu));
        kv(&RB.sc[1], "temperature", "%s", bz_fmt(f[1], sizeof f[1], true, "%.0f °c", r->sc_temp));
        kv(&RB.sc[2], "memory", "%s", bz_fmt(f[2], sizeof f[2], true, "%.0f %%", r->sc_ram * 100));
        kv(&RB.sc[3], "storage", "%s", bz_fmt(f[3], sizeof f[3], true, "%.0f %%", r->sc_storage * 100));
        kv(&RB.sc[4], "emmc", "%s", r->sc_emmc >= 3 ? "replace" : r->sc_emmc == 2 ? "wearing" : "healthy");
    } else {
        /* the same keys with dashes: four unlabelled dashes under one key read as a broken table */
        kv(&RB.sc[0], "cpu", "%s", r->connected && !r->line2 ? "not published by 1.x" : "");
        kv(&RB.sc[1], "temperature", "%s", "");
        kv(&RB.sc[2], "memory", "%s", "");
        kv(&RB.sc[3], "storage", "%s", "");
        kv(&RB.sc[4], "emmc", "%s", "");
    }
    if (r->have_loop) kv(&RB.sc[5], "loop", "%.1f ms, worst %.1f", r->loop_avg_ms, r->loop_max_ms);
    else kv(&RB.sc[5], "loop", "%s", "");
    kv(&RB.rt[0], "system check", "%s", r->have_systemcheck ? (r->systemcheck_report[0] ? r->systemcheck_report : (r->systemcheck_ready ? "ready" : "not run")) : "");
    kv(&RB.rt[1], "preflight", "%s", r->have_preflight ? r->preflight_summary : "");
    if (r->have_wheel_radius) kv(&RB.rt[2], "wheel radius", "%s · %.3f in (%+.1f %%)", r->wheel_radius_status, r->wheel_radius_in, r->wheel_radius_change);
    else kv(&RB.rt[2], "wheel radius", "%s", "");
    if (r->have_start_check) kv(&RB.rt[3], "auto start", "%s · %.2f m, %.0f°", r->start_ready ? "on it" : "off", r->start_dist_m, r->start_heading_deg);
    else kv(&RB.rt[3], "auto start", "%s", "");
    kv(&RB.rt[4], "routines", "%s", "run from the DS: Utility op modes");
    kv(&RB.rt[5], "", "%s", " ");
}

static void rb_build(lv_obj_t *b)
{
    lv_obj_t *head = bz_col(b, 2);
    lv_obj_set_pos(head, PAD, APP_Y - 6);
    RB.sub = bz_label(head, "", BZ_F_LABEL, BZ_C_DIM);
    RB.name = bz_label(head, "", BZ_F_TITLE, BZ_C_INK);
    int w = (W - 2 * PAD - 2 * BZ_GAP) / 3;
    lv_obj_t *grid = bz_row(b, BZ_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_width(grid, W - 2 * PAD);
    lv_obj_set_pos(grid, PAD, APP_Y + 70);
    kv_tile(grid, w, 250, "identity", RB.id, 6);
    kv_tile(grid, w, 250, "software", RB.sw, 6);
    kv_tile(grid, w, 250, "link", RB.link, 6);
    kv_tile(grid, w + w / 2, 250, "controller", RB.sc, 6);
    kv_tile(grid, W - 2 * PAD - (w + w / 2) - BZ_GAP, 250, "routines' results", RB.rt, 6);
}

const ui_app_t APP_ROBOT = { .name = "robot", .icon = BZ_I_SMART_TOY, .build = rb_build, .refresh = rb_refresh };

/* ================================================================== field */

#define FIELD_L 16.54f
#define FIELD_W 8.07f
#define FIELD_SCALE 56.0f

static struct {
    lv_obj_t *map, *side, *pose, *tag, *cams[CAT_MAX_CAMERAS], *nocam, *blink;
    uint32_t map_sig; /* what the map shows: redrawn only when it changes */
} FD;

/* The pose and the path as drawn (a centimetre, a tenth of a degree): a robot at rest redraws nothing. */
static uint32_t field_sig(const cat_robot_t *r)
{
    uint32_t h = 2166136261u ^ (uint32_t)r->have_pose ^ (uint32_t)r->npath << 1;
    if (r->have_pose) {
        int32_t q[3] = { (int32_t)lround(r->pose_x * 100), (int32_t)lround(r->pose_y * 100), (int32_t)lround(r->pose_rad * 573) };
        for (int k = 0; k < 3; k++) h = (h ^ (uint32_t)q[k]) * 16777619u;
    }
    for (int i = 0; i < 3 * r->npath && i < (int)(sizeof r->path / sizeof r->path[0]); i++)
        h = (h ^ (uint32_t)(int32_t)lround(r->path[i] * 100)) * 16777619u;
    return h;
}

static void field_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const cat_robot_t *r = R;
    float s = FIELD_SCALE;
    /* field coordinates: origin at the blue driver station's right corner, x toward red, y left (up) */
#define FX(x) (a.x1 + (x) * s)
#define FY(y) (a.y2 - (y) * s)
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = bz_lv(BZ_C_SURFACE1);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 12;
    lv_draw_rect(layer, &rd, &a);
    /* alliance zones: a low-contrast tint of each alliance colour is not a status colour, so they're
     * drawn as outlines in dim ink with the words beside them */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = bz_lv(BZ_C_FAINT);
    ld.width = 2;
    ld.p1.x = FX(FIELD_L / 2); ld.p1.y = a.y1 + 8;
    ld.p2.x = FX(FIELD_L / 2); ld.p2.y = a.y2 - 8;
    lv_draw_line(layer, &ld);
    ld.p1.x = FX(3.95f); ld.p2.x = FX(3.95f);
    lv_draw_line(layer, &ld);
    ld.p1.x = FX(FIELD_L - 3.95f); ld.p2.x = FX(FIELD_L - 3.95f);
    lv_draw_line(layer, &ld);

    /* the active path */
    ld.color = bz_lv(BZ_C_DIM);
    ld.width = 3;
    ld.round_start = ld.round_end = 1;
    for (int i = 1; i < r->npath; i++) {
        ld.p1.x = FX(r->path[3 * (i - 1)]); ld.p1.y = FY(r->path[3 * (i - 1) + 1]);
        ld.p2.x = FX(r->path[3 * i]); ld.p2.y = FY(r->path[3 * i + 1]);
        lv_draw_line(layer, &ld);
    }
    if (!r->have_pose) return;
    /* the robot: a square the size of a 28 × 26 in frame, turned by its heading, front marked in signal */
    float cx = FX(r->pose_x), cy = FY(r->pose_y), th = (float)r->pose_rad;
    float hw = 0.36f * s, hh = 0.33f * s;
    float c = cosf(th), sn = sinf(th);
    float px[4], py[4];
    static const float k[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
    for (int i = 0; i < 4; i++) {
        float lx = k[i][0] * hw, ly = k[i][1] * hh;
        px[i] = cx + lx * c - ly * sn;
        py[i] = cy - (lx * sn + ly * c);
    }
    ld.color = bz_lv(BZ_C_INK);
    ld.width = 4;
    for (int i = 0; i < 4; i++) {
        ld.p1.x = px[i]; ld.p1.y = py[i];
        ld.p2.x = px[(i + 1) % 4]; ld.p2.y = py[(i + 1) % 4];
        if (i == 0) ld.color = bz_lv(BZ_C_SIGNAL);
        else ld.color = bz_lv(BZ_C_INK);
        lv_draw_line(layer, &ld);
    }
#undef FX
#undef FY
}

static void fd_refresh(void)
{
    const cat_robot_t *r = R;
    if (r->have_pose) ui_text(FD.pose, "(%.2f, %.2f) m · %.0f°", r->pose_x, r->pose_y, r->pose_rad * 57.2958);
    else ui_text(FD.pose, "no pose published");
    if (r->tag_seen >= 0) ui_text(FD.tag, "tag %d in view", r->tag_seen);
    else ui_text(FD.tag, "no tag in view");
    for (int i = 0; i < CAT_MAX_CAMERAS; i++) {
        if (i < r->ncameras) {
            const cat_camera_t *c = &r->cameras[i];
            lv_obj_remove_flag(FD.cams[i], LV_OBJ_FLAG_HIDDEN);
            bool ok = !strcmp(c->state, "OK") || !strcmp(c->state, "NO_TARGETS");
            bz_mark_set(lv_obj_get_child(FD.cams[i], 0), !c->connected ? BZ_FAULT : ok ? BZ_OK : BZ_WARN);
            char fps[16];
            ui_text(lv_obj_get_child(FD.cams[i], 1), "%s", c->name);
            ui_text(lv_obj_get_child(FD.cams[i], 2), "%s · %s fps", c->state, bz_fmt(fps, sizeof fps, c->fps == c->fps, "%.0f", c->fps));
        } else {
            lv_obj_add_flag(FD.cams[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* no camera: say so, and no blink button that would do nothing */
    if (r->ncameras) {
        lv_obj_add_flag(FD.nocam, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(FD.blink, LV_OBJ_FLAG_HIDDEN);
    } else {
        ui_text(FD.nocam, "%s", r->connected ? "none published" : "no robot");
        lv_obj_remove_flag(FD.nocam, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(FD.blink, LV_OBJ_FLAG_HIDDEN);
    }
    /* the map only when the pose or the path moved: it was a 926 × 452 redraw 10 times a second */
    uint32_t ms = field_sig(r);
    if (ms != FD.map_sig) {
        FD.map_sig = ms;
        lv_obj_invalidate(FD.map);
    }
}

static void fd_open(void) { FD.map_sig = 0; }

static void blink_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!R->ncameras) return;
    cat_blink_limelight(R->cameras[0].name, true);
    char msg[80];
    snprintf(msg, sizeof msg, "%s blinking", R->cameras[0].name);
    ui_island_say(BZ_I_LIGHTBULB, msg);
}

static void fd_close(void)
{
    if (R->ncameras) cat_blink_limelight(R->cameras[0].name, false);
}

static void fd_build(lv_obj_t *b)
{
    FD.map = bz_box(b);
    lv_obj_set_size(FD.map, (int)(FIELD_L * FIELD_SCALE), (int)(FIELD_W * FIELD_SCALE));
    lv_obj_set_pos(FD.map, PAD, APP_Y);
    lv_obj_add_event_cb(FD.map, field_draw, LV_EVENT_DRAW_MAIN, NULL);
    int sx = PAD + (int)(FIELD_L * FIELD_SCALE) + BZ_GAP;
    lv_obj_t *t = bz_tile(b, W - PAD - sx, (int)(FIELD_W * FIELD_SCALE));
    lv_obj_set_pos(t, sx, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 12, 0);
    int cw = W - PAD - (PAD + (int)(FIELD_L * FIELD_SCALE) + BZ_GAP) - 2 * BZ_PAD_TILE;
    bz_label(t, "pose", BZ_F_LABEL, BZ_C_DIM);
    FD.pose = bz_label(t, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(FD.pose, cw); /* "(12.34, 5.67) m · -179°" wraps inside the tile */
    FD.tag = bz_label(t, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(FD.tag, cw);
    bz_label(t, "cameras", BZ_F_LABEL, BZ_C_DIM);
    for (int i = 0; i < CAT_MAX_CAMERAS; i++) {
        lv_obj_t *row = bz_row(t, 8);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_width(row, cw);
        bz_mark(row, BZ_STALE, 10);
        bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, cw - 20);
        bz_label(row, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        FD.cams[i] = row;
    }
    FD.nocam = bz_label(t, "", BZ_F_BODY_S, BZ_C_DIM);
    FD.blink = ui_button(t, BZ_I_LIGHTBULB, "blink", blink_tap, NULL);
    lv_obj_t *cap = bz_label(b, "blue alliance wall on the left · the signal edge is the robot's front", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_pos(cap, PAD, APP_Y + (int)(FIELD_W * FIELD_SCALE) + 12);
}

const ui_app_t APP_FIELD = { .name = "field", .icon = BZ_I_STADIUM, .build = fd_build, .open = fd_open, .refresh = fd_refresh,
                             .close = fd_close };
