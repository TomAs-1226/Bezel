/* The four robot pages — pulse, devices, power, motion — and the tools launcher. */
#include "ui_internal.h"

#include <math.h>
#include <string.h>

#define COL3 ((W - 2 * PAD - 2 * BZ_GAP) / 3)       /* 397 */
#define COL4 ((W - 2 * PAD - 3 * BZ_GAP) / 4)       /* 294 */
#define ROW_H ((BODY_BOTTOM - BODY_Y - BZ_GAP) / 2) /* 250 */

static lv_obj_t *tile_at(lv_obj_t *page, int x, int y, int w, int h)
{
    lv_obj_t *t = bz_tile(page, w, h);
    lv_obj_set_pos(t, x, y);
    return t;
}

/* A tile's label row: mono label left, optional status text right. */
static lv_obj_t *tile_label(lv_obj_t *tile, const char *text, lv_obj_t **right)
{
    lv_obj_t *l = bz_label(tile, text, BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(l, 0, 0);
    if (right) {
        *right = bz_label(tile, "", BZ_F_LABEL, BZ_C_DIM);
        lv_obj_align(*right, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return l;
}

static void open_app_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }
static void go_tap(lv_obj_t *o, void *u) { (void)o; ui_go((int)(intptr_t)u); }

static void make_tappable(lv_obj_t *tile)
{
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
}

/* ================================================================== pulse */

static struct {
    lv_obj_t *batt_v, *batt_band, *batt_mark, *batt_spark, *batt_foot;
    lv_obj_t *mode, *mode_mark, *mode_marks, *mode_ds, *mode_fms, *mode_ali, *mode_foot, *mode_time;
    lv_obj_t *al_count, *al_mark, *al_lines[3], *al_foot;
    lv_obj_t *loop_v, *loop_meter, *loop_foot;
    lv_obj_t *can_rows[3], *can_names[3], *can_meters[3], *can_vals[3], *can_foot;
    lv_obj_t *dev_v, *dev_mark, *dev_foot;
    lv_obj_t *vis_v, *vis_mark, *vis_foot;
    lv_obj_t *head_right, *team;
    double last_spark;
} P;

static void pulse_refresh(void *u)
{
    (void)u;
    const cat_robot_t *r = R;
    char b[32];

    ui_text(P.batt_v, "%s", bz_fmt(b, sizeof b, r->have_battery, "%.2f", r->battery_v));
    if (r->have_battery) {
        int band = cat_battery_band(r->battery_v);
        ui_text(P.batt_band, "%s", band == 2 ? "charged" : band == 1 ? "swap before a match" : "low");
        bz_mark_set(P.batt_mark, ui_battery_status(r->battery_v));
    } else {
        ui_text(P.batt_band, "%s", r->connected ? "not published" : "");
        bz_mark_set(P.batt_mark, BZ_STALE);
    }
    if (ui_now() - P.last_spark >= 0.5 && r->have_battery) {
        P.last_spark = ui_now();
        bz_spark_push(P.batt_spark, (float)r->battery_v);
        bz_spark_color(P.batt_spark, r->battery_v < 12.2 ? BZ_C_WARN : BZ_C_INK);
    }
    if (!r->connected) bz_spark_clear(P.batt_spark);
    char fl[16], cur[16];
    ui_text(P.batt_foot, "floor %s v · draw %s a", bz_fmt(fl, sizeof fl, r->brownout_v == r->brownout_v, "%.2f", r->brownout_v),
            bz_fmt(cur, sizeof cur, r->total_current == r->total_current, "%.0f", r->total_current));

    ui_text(P.mode, "%s", r->connected ? cat_mode_name(r) : "offline");
    bz_mark_set(P.mode_mark, !r->connected ? BZ_STALE : r->estop ? BZ_FAULT : r->enabled ? BZ_WARN : BZ_OK);
    bz_mark_set(P.mode_ds, r->ds ? BZ_OK : BZ_STALE);
    bz_mark_set(P.mode_fms, r->fms ? BZ_OK : BZ_STALE);
    if (r->have_alliance) ui_text(P.mode_ali, "%s %d", r->red ? "red" : "blue", r->station);
    else ui_text(P.mode_ali, " ");
    if (r->match_time == r->match_time) ui_text(P.mode_time, "%d:%02d", (int)r->match_time / 60, (int)r->match_time % 60);
    else ui_text(P.mode_time, " ");
    if (r->connected)
        ui_text(P.mode_foot, "catalyst %s · %s · %.0f ms", r->catalyst_version[0] ? r->catalyst_version : "?",
                r->line2 ? "systemcore" : "roborio", r->rtt_ms);
    else ui_text(P.mode_foot, "trying %s", r->address[0] ? r->address : "…");

    if (!r->connected) {
        ui_text(P.al_count, "\xe2\x80\x94");
        bz_mark_set(P.al_mark, BZ_STALE);
    } else if (r->n_errors || r->n_warnings) {
        ui_text(P.al_count, "%d · %d", r->n_errors, r->n_warnings);
        bz_mark_set(P.al_mark, r->n_errors ? BZ_FAULT : BZ_WARN);
    } else {
        ui_text(P.al_count, "clear");
        bz_mark_set(P.al_mark, BZ_OK);
    }
    int shown = 0;
    for (int i = 0; i < r->nalerts && shown < 3; i++) {
        if (r->alerts[i].sev == CAT_SEV_INFO) continue;
        ui_text(P.al_lines[shown], "%s%s%s", r->alerts[i].source, r->alerts[i].source[0] ? ": " : "", r->alerts[i].text);
        bz_set_color(P.al_lines[shown], BZ_C_INK);
        shown++;
    }
    for (; shown < 3; shown++) ui_text(P.al_lines[shown], " ");
    ui_text(P.al_foot, "%d error%s · %d warning%s", r->n_errors, r->n_errors == 1 ? "" : "s", r->n_warnings,
            r->n_warnings == 1 ? "" : "s");

    ui_text(P.loop_v, "%s", bz_fmt(b, sizeof b, r->have_loop, "%.1f", r->loop_avg_ms));
    if (r->have_loop) {
        double f = r->loop_avg_ms / 20.0;
        bz_meter_set(P.loop_meter, (float)f, f > 1 ? BZ_C_FAULT : f > 0.75 ? BZ_C_WARN : BZ_C_ICE);
        ui_text(P.loop_foot, "ms average · worst %.1f", r->loop_max_ms);
    } else {
        bz_meter_set(P.loop_meter, 0, BZ_C_ICE);
        ui_text(P.loop_foot, "ms average");
    }

    int row = 0;
    for (int i = 0; i < r->ncan_util && row < 3; i++) {
        if (r->ncan_util > 1 && r->can_util[i] <= 0) continue; /* unused Systemcore buses */
        double u = r->can_util[i];
        lv_obj_remove_flag(P.can_rows[row], LV_OBJ_FLAG_HIDDEN);
        if (r->ncan_util > 1) ui_text(P.can_names[row], "can_s%d", i);
        else ui_text(P.can_names[row], "bus");
        bz_meter_set(P.can_meters[row], (float)u, u > 0.85 ? BZ_C_FAULT : u > 0.70 ? BZ_C_WARN : BZ_C_ICE);
        ui_text(P.can_vals[row], "%.0f%%", u * 100);
        row++;
    }
    for (; row < 3; row++) lv_obj_add_flag(P.can_rows[row], LV_OBJ_FLAG_HIDDEN);
    if (r->ncan_util) ui_text(P.can_foot, "%d devices declared", r->ndevices);
    else ui_text(P.can_foot, "%s", r->connected ? "not published" : " ");

    if (r->have_roster) {
        ui_text(P.dev_v, "%d/%d", r->motors_connected, r->motors_expected);
        bz_mark_set(P.dev_mark, r->motors_connected < r->motors_expected ? BZ_FAULT : BZ_OK);
        char miss[80] = "";
        size_t o = 0;
        for (int i = 0; i < r->ndevices; i++)
            if (r->devices[i].connected == 0 && o < sizeof miss - 16)
                o += (size_t)snprintf(miss + o, sizeof miss - o, "%s%s %d", o ? ", " : "", r->devices[i].name, r->devices[i].id);
        ui_text(P.dev_foot, "%s", miss[0] ? miss : "motors answering");
    } else if (r->ndevices) {
        ui_text(P.dev_v, "%d", r->ndevices);
        bz_mark_set(P.dev_mark, BZ_INFO);
        ui_text(P.dev_foot, "declared on can");
    } else {
        ui_text(P.dev_v, "\xe2\x80\x94");
        bz_mark_set(P.dev_mark, BZ_STALE);
        ui_text(P.dev_foot, " ");
    }

    if (r->ncameras) {
        static const char *const lv[] = { "ok", "degraded", "blind" };
        int level = r->have_vision ? r->vision_level : (r->cameras[0].connected ? 0 : 2);
        ui_text(P.vis_v, "%s", lv[level < 0 ? 0 : level > 2 ? 2 : level]);
        bz_mark_set(P.vis_mark, level == 0 ? BZ_OK : level == 1 ? BZ_WARN : BZ_FAULT);
        const cat_camera_t *c = &r->cameras[0];
        if (c->fps == c->fps) ui_text(P.vis_foot, "%s · %.0f fps", c->name, c->fps);
        else ui_text(P.vis_foot, "%s", c->name);
    } else {
        ui_text(P.vis_v, "\xe2\x80\x94");
        bz_mark_set(P.vis_mark, BZ_STALE);
        ui_text(P.vis_foot, " ");
    }

    /* the robot by its own name when it publishes one */
    if (r->connected && r->have_identity && r->name[0]) ui_text(P.team, "%s \xc2\xb7 team %d", r->name, r->team);
    else if (r->connected && r->have_identity) ui_text(P.team, "team %d", r->team);
    else ui_text(P.team, "team %d", S.team);
}

/* A value in the display face with its unit set small beside it. */
static lv_obj_t *value_row(lv_obj_t *tile, bz_font_role_t f, const char *unit, lv_obj_t **value, int y_from_bottom)
{
    lv_obj_t *row = bz_row(tile, 10);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    *value = bz_label(row, "\xe2\x80\x94", f, BZ_C_INK);
    if (unit) {
        lv_obj_t *u = bz_label(row, unit, BZ_F_NAME, BZ_C_DIM);
        lv_obj_set_style_pad_bottom(u, f == BZ_F_DISPLAY ? 12 : 6, 0);
    }
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -y_from_bottom);
    return row;
}

void ui_page_overview(lv_obj_t *page)
{
    /* the home page carries the tablet's own name, like the device it is */
    lv_obj_t *head = ui_head(page, "Robot", NULL);
    P.team = bz_label_line(head, "", BZ_F_LABEL, BZ_C_DIM, ui_head_width("Robot"));

    int y1 = BODY_Y, y2 = BODY_Y + ROW_H + BZ_GAP;
    /* battery */
    lv_obj_t *t = tile_at(page, PAD, y1, COL3, ROW_H);
    make_tappable(t);
    bz_on_tap(t, go_tap, (void *)(intptr_t)PG_POWER);
    tile_label(t, "battery", NULL);
    lv_obj_t *br = bz_row(t, 8);
    lv_obj_align(br, LV_ALIGN_TOP_RIGHT, 0, 0);
    P.batt_mark = bz_mark(br, BZ_STALE, 10);
    P.batt_band = bz_label(br, "", BZ_F_LABEL, BZ_C_DIM);
    value_row(t, BZ_F_DISPLAY, "v", &P.batt_v, 26);
    P.batt_spark = bz_spark(t, 96, 56, 120); /* beside "12.60 v" (tabular: never wider), not under it */
    bz_spark_min_span(P.batt_spark, 1.0f);
    lv_obj_align(P.batt_spark, LV_ALIGN_RIGHT_MID, 0, 6);
    P.batt_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.batt_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* robot */
    t = tile_at(page, PAD + COL3 + BZ_GAP, y1, COL3, ROW_H);
    make_tappable(t);
    bz_on_tap(t, open_app_tap, (void *)&APP_ROBOT);
    lv_obj_t *mr;
    tile_label(t, "robot", &mr);
    P.mode_time = mr;
    lv_obj_t *mrow = bz_row(t, 14);
    lv_obj_set_pos(mrow, 0, 40);
    P.mode_mark = bz_mark(mrow, BZ_STALE, 14);
    P.mode = bz_label(mrow, "offline", BZ_F_VALUE, BZ_C_INK);
    lv_obj_t *marks = bz_row(t, 8);
    lv_obj_align(marks, LV_ALIGN_BOTTOM_LEFT, 0, -34);
    P.mode_ds = bz_mark(marks, BZ_STALE, 10);
    bz_label(marks, "ds", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *sp = bz_box(marks);
    lv_obj_set_width(sp, 10);
    P.mode_fms = bz_mark(marks, BZ_STALE, 10);
    bz_label(marks, "fms", BZ_F_LABEL, BZ_C_DIM);
    sp = bz_box(marks);
    lv_obj_set_width(sp, 10);
    P.mode_ali = bz_label(marks, "", BZ_F_LABEL, BZ_C_DIM);
    P.mode_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.mode_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* alerts */
    t = tile_at(page, PAD + 2 * (COL3 + BZ_GAP), y1, COL3, ROW_H);
    make_tappable(t);
    bz_on_tap(t, open_app_tap, (void *)&APP_ALERTS);
    tile_label(t, "alerts", NULL);
    lv_obj_t *arow = bz_row(t, 14);
    lv_obj_align(arow, LV_ALIGN_TOP_RIGHT, 0, -4);
    P.al_mark = bz_mark(arow, BZ_STALE, 12);
    P.al_count = bz_label(arow, "\xe2\x80\x94", BZ_F_NAME, BZ_C_INK);
    lv_obj_t *lines = bz_col(t, 6);
    lv_obj_set_pos(lines, 0, 44);
    for (int i = 0; i < 3; i++) {
        P.al_lines[i] = bz_label_line(lines, " ", BZ_F_BODY_S, BZ_C_INK, COL3 - 2 * BZ_PAD_TILE);
    }
    P.al_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.al_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* loop */
    int x = PAD;
    t = tile_at(page, x, y2, COL4, ROW_H);
    make_tappable(t);
    bz_on_tap(t, open_app_tap, (void *)&APP_ROBOT);
    tile_label(t, "loop", NULL);
    value_row(t, BZ_F_VALUE, NULL, &P.loop_v, 58);
    P.loop_meter = bz_meter(t, COL4 - 2 * BZ_PAD_TILE, 12);
    lv_obj_align(P.loop_meter, LV_ALIGN_BOTTOM_LEFT, 0, -30);
    P.loop_foot = bz_label(t, "ms average", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.loop_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* can */
    x += COL4 + BZ_GAP;
    t = tile_at(page, x, y2, COL4, ROW_H);
    make_tappable(t);
    bz_on_tap(t, open_app_tap, (void *)&APP_CANTAP);
    tile_label(t, "can", NULL);
    lv_obj_t *cc = bz_col(t, 10);
    lv_obj_set_pos(cc, 0, 44);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *rw = bz_row(cc, 10);
        P.can_rows[i] = rw;
        P.can_names[i] = bz_label(rw, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(P.can_names[i], 72);
        P.can_meters[i] = bz_meter(rw, COL4 - 2 * BZ_PAD_TILE - 72 - 60 - 20, 12);
        P.can_vals[i] = bz_label(rw, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_add_flag(rw, LV_OBJ_FLAG_HIDDEN);
    }
    P.can_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.can_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* devices */
    x += COL4 + BZ_GAP;
    t = tile_at(page, x, y2, COL4, ROW_H);
    make_tappable(t);
    bz_on_tap(t, go_tap, (void *)(intptr_t)PG_DEVICES);
    tile_label(t, "motors", NULL);
    P.dev_mark = bz_mark(t, BZ_STALE, 12);
    lv_obj_align(P.dev_mark, LV_ALIGN_TOP_RIGHT, 0, 2);
    value_row(t, BZ_F_VALUE, NULL, &P.dev_v, 30);
    P.dev_foot = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, COL4 - 2 * BZ_PAD_TILE);
    lv_obj_align(P.dev_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* vision */
    x += COL4 + BZ_GAP;
    t = tile_at(page, x, y2, COL4, ROW_H);
    make_tappable(t);
    bz_on_tap(t, open_app_tap, (void *)&APP_FIELD);
    tile_label(t, "vision", NULL);
    P.vis_mark = bz_mark(t, BZ_STALE, 12);
    lv_obj_align(P.vis_mark, LV_ALIGN_TOP_RIGHT, 0, 2);
    value_row(t, BZ_F_VALUE, NULL, &P.vis_v, 30);
    P.vis_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(P.vis_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    ui_on_page_refresh(PG_ROBOT, pulse_refresh, NULL);
}
