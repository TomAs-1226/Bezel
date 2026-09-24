/* Devices, power, motion and the tools launcher. */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BODY_H (H - BODY_Y - DOCK_CLEAR + 40) /* bodies may run under the dock's glass */

static void open_app_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }

/* ================================================================== devices */

#define DEV_COLS 3
#define DEV_COL_W ((W - 2 * PAD - (DEV_COLS - 1) * BZ_GAP) / DEV_COLS)

static struct {
    lv_obj_t *scroll, *summary;
    lv_obj_t *cols[DEV_COLS];
    uint32_t sig;
    struct { lv_obj_t *mark; int dev; } rows[CAT_MAX_DEVICES];
    int nrows;
    lv_obj_t *bus_meters[DEV_COLS], *bus_vals[DEV_COLS];
    int bus_index[DEV_COLS];
} D;

static uint32_t dev_signature(const cat_robot_t *r)
{
    uint32_t h = 2166136261u ^ (uint32_t)r->ndevices;
    for (int i = 0; i < r->ndevices; i++) {
        h = (h ^ (uint32_t)r->devices[i].id) * 16777619u;
        for (const char *p = r->devices[i].bus; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        for (const char *p = r->devices[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

static bz_status_t dev_status(const cat_device_t *d)
{
    return d->connected == 1 ? BZ_OK : d->connected == 0 ? BZ_FAULT : BZ_INFO;
}

static void devices_rebuild(const cat_robot_t *r)
{
    lv_obj_clean(D.scroll);
    D.nrows = 0;
    /* one tile per bus, in first-seen order; everything past the third bus shares the last tile */
    char buses[DEV_COLS][12] = { "" };
    int nb = 0;
    for (int i = 0; i < r->ndevices; i++) {
        int k;
        for (k = 0; k < nb; k++) if (!strcmp(buses[k], r->devices[i].bus)) break;
        if (k == nb && nb < DEV_COLS) snprintf(buses[nb++], sizeof buses[0], "%s", r->devices[i].bus);
    }
    lv_obj_t *grid = bz_row(D.scroll, BZ_GAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int b = 0; b < DEV_COLS; b++) {
        D.cols[b] = NULL;
        D.bus_meters[b] = NULL;
        if (b >= nb) continue;
        lv_obj_t *t = bz_tile(grid, DEV_COL_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(t, 10, 0);
        D.cols[b] = t;
        lv_obj_t *hd = bz_row(t, 10);
        lv_obj_set_width(hd, DEV_COL_W - 2 * BZ_PAD_TILE);
        bz_label(hd, buses[b], BZ_F_LABEL, BZ_C_DIM);
        lv_obj_t *sp = bz_box(hd);
        lv_obj_set_flex_grow(sp, 1);
        D.bus_meters[b] = bz_meter(hd, 90, 10);
        D.bus_vals[b] = bz_label(hd, "", BZ_F_CAPTION, BZ_C_DIM);
        int idx = -1;
        if (!strncmp(buses[b], "can_s", 5)) idx = atoi(buses[b] + 5);
        D.bus_index[b] = idx;
        for (int i = 0; i < r->ndevices; i++) {
            const cat_device_t *d = &r->devices[i];
            int k;
            for (k = 0; k < nb; k++) if (!strcmp(buses[k], d->bus)) break;
            if (k >= nb) k = nb - 1;
            if (k != b) continue;
            lv_obj_t *row = bz_row(t, 12);
            lv_obj_set_width(row, DEV_COL_W - 2 * BZ_PAD_TILE);
            lv_obj_t *m = bz_mark(row, dev_status(d), 10);
            char id[8];
            snprintf(id, sizeof id, "%2d", d->id);
            lv_obj_t *idl = bz_label(row, id, BZ_F_LABEL, BZ_C_INK);
            lv_obj_set_width(idl, 26);
            bz_label_line(row, d->name[0] ? d->name : d->type, BZ_F_BODY_S, BZ_C_INK, DEV_COL_W - 2 * BZ_PAD_TILE - 10 - 26 - 110 - 36);
            lv_obj_t *ty = bz_label_line(row, d->type, BZ_F_CAPTION, BZ_C_DIM, 110);
            lv_obj_set_style_text_align(ty, LV_TEXT_ALIGN_RIGHT, 0);
            if (D.nrows < CAT_MAX_DEVICES) {
                D.rows[D.nrows].mark = m;
                D.rows[D.nrows].dev = i;
                D.nrows++;
            }
        }
    }
    if (!nb) {
        lv_obj_t *t = bz_tile(D.scroll, W - 2 * PAD, 160);
        bz_label(t, R->connected ? "the robot declares no CAN devices" : "not connected", BZ_F_BODY, BZ_C_DIM);
        lv_obj_t *l = bz_label(t, "Catalyst publishes /Catalyst/CAN/Devices from 1.12; the can tap hears the bus directly.",
                               BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void devices_refresh(void *u)
{
    (void)u;
    const cat_robot_t *r = R;
    uint32_t sig = dev_signature(r) ^ (uint32_t)r->connected;
    if (sig != D.sig) {
        D.sig = sig;
        devices_rebuild(r);
    }
    for (int i = 0; i < D.nrows; i++) {
        if (D.rows[i].dev < r->ndevices) bz_mark_set(D.rows[i].mark, dev_status(&r->devices[D.rows[i].dev]));
    }
    for (int b = 0; b < DEV_COLS; b++) {
        if (!D.bus_meters[b]) continue;
        int idx = D.bus_index[b];
        double u = idx >= 0 && idx < r->ncan_util ? r->can_util[idx] : r->ncan_util == 1 ? r->can_util[0] : -1;
        if (u >= 0) {
            bz_meter_set(D.bus_meters[b], (float)u, u > 0.85 ? BZ_C_FAULT : u > 0.70 ? BZ_C_WARN : BZ_C_ICE);
            ui_text(D.bus_vals[b], "%.0f%%", u * 100);
        } else {
            ui_text(D.bus_vals[b], "\xe2\x80\x94");
        }
    }
    if (!r->connected) ui_text(D.summary, " ");
    else if (r->have_roster)
        ui_text(D.summary, "%d/%d motors · %d/%d cameras%s%s", r->motors_connected, r->motors_expected, r->cams_connected,
                r->cams_expected, r->have_controller ? " · " : "", r->have_controller ? r->controller_kind : "");
    else ui_text(D.summary, "%d declared · 1.x has no per-device state", r->ndevices);
}

void ui_page_devices(lv_obj_t *page)
{
    lv_obj_t *right = ui_head(page, "Devices", NULL);
    D.summary = bz_label_line(right, "", BZ_F_LABEL, BZ_C_DIM, ui_head_width("Devices"));
    lv_obj_t *wrap = bz_box(page);
    lv_obj_set_pos(wrap, PAD, BODY_Y);
    D.scroll = ui_scroller(wrap, W - 2 * PAD, BODY_H);
    D.sig = 1;
    ui_on_page_refresh(PG_DEVICES, devices_refresh, NULL);
}

/* ================================================================== power */

#define PW_RIGHT 392
#define PW_GRID_W (W - 2 * PAD - PW_RIGHT - BZ_GAP)
#define PW_CELL_W ((PW_GRID_W - 5 * 10) / 6)
#define PW_CELL_H ((BODY_BOTTOM - BODY_Y - 3 * 10) / 4) /* 121 */

static struct {
    lv_obj_t *cells[24], *names[24], *amps[24], *meters[24], *nums[24];
    lv_obj_t *batt, *band, *mark, *spark, *curr_spark, *foot, *total, *head;
    double last;
} PW;

/* PDH breakers: channels 0–19 take 40 A, 20–22 15 A (switchable 23 too). A PDP is 40/30 A. Without
 * the robot saying, the meter reads against 40 A so a hot channel still stands out. */
static float breaker(int ch) { return ch >= 20 ? 15.0f : 40.0f; }

static void power_refresh(void *u)
{
    (void)u;
    const cat_robot_t *r = R;
    char b[24];
    ui_text(PW.batt, "%s", bz_fmt(b, sizeof b, r->have_battery, "%.2f", r->battery_v));
    if (r->have_battery) {
        bz_mark_set(PW.mark, ui_battery_status(r->battery_v));
        int band = cat_battery_band(r->battery_v);
        ui_text(PW.band, "%s", band == 2 ? "charged" : band == 1 ? "swap before a match" : "low");
    } else {
        bz_mark_set(PW.mark, BZ_STALE);
        ui_text(PW.band, " ");
    }
    double total = r->have_pd_live ? r->pd_total : r->total_current;
    ui_text(PW.total, "%s", bz_fmt(b, sizeof b, total == total, "%.0f", total));
    if (ui_now() - PW.last >= 0.5) {
        PW.last = ui_now();
        if (r->have_battery) bz_spark_push(PW.spark, (float)r->battery_v);
        if (total == total) bz_spark_push(PW.curr_spark, (float)total);
    }
    if (!r->connected) {
        bz_spark_clear(PW.spark);
        bz_spark_clear(PW.curr_spark);
    }
    char pv[16], fl[16];
    ui_text(PW.foot, "brownout floor %s v · predicted %s v%s", bz_fmt(fl, sizeof fl, r->brownout_v == r->brownout_v, "%.2f", r->brownout_v),
            bz_fmt(pv, sizeof pv, r->predicted_v == r->predicted_v, "%.1f", r->predicted_v), r->brownout_risk ? " · at risk" : "");
    ui_text(PW.head, "%s%s%d of 24 in use", r->pd_module[0] ? r->pd_module : "", r->pd_module[0] ? " · " : "", r->nchannels);

    for (int ch = 0; ch < 24; ch++) {
        const cat_channel_t *c = NULL;
        for (int k = 0; k < r->nchannels; k++) if (r->channels[k].channel == ch) c = &r->channels[k];
        if (c && c->what[0]) {
            ui_text(PW.names[ch], "%s", c->what);
            bz_set_color(PW.names[ch], BZ_C_INK);
        } else {
            ui_text(PW.names[ch], "free");
            bz_set_color(PW.names[ch], BZ_C_FAINT);
        }
        double amps = r->pd_amps[ch];
        if (amps == amps) {
            ui_text(PW.amps[ch], "%.1f", amps);
            float f = (float)(amps / breaker(ch));
            bz_meter_set(PW.meters[ch], f, f > 0.9f ? BZ_C_FAULT : f > 0.7f ? BZ_C_WARN : BZ_C_ICE);
        } else {
            ui_text(PW.amps[ch], "%s", c ? "\xe2\x80\x94" : "");
            bz_meter_set(PW.meters[ch], 0, BZ_C_ICE);
        }
    }
}

void ui_page_power(lv_obj_t *page)
{
    lv_obj_t *right = ui_head(page, "Power", NULL);
    PW.head = bz_label_line(right, "", BZ_F_LABEL, BZ_C_DIM, ui_head_width("Power"));

    lv_obj_t *grid = bz_row(page, 10);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(grid, PW_GRID_W);
    lv_obj_set_pos(grid, PAD, BODY_Y);
    for (int ch = 0; ch < 24; ch++) {
        lv_obj_t *c = bz_tile(grid, PW_CELL_W, PW_CELL_H);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_pad_all(c, 14, 0);
        PW.cells[ch] = c;
        char n[4];
        snprintf(n, sizeof n, "%d", ch);
        PW.nums[ch] = bz_label(c, n, BZ_F_CAPTION, BZ_C_DIM);
        PW.amps[ch] = bz_label(c, "", BZ_F_NAME, BZ_C_INK);
        lv_obj_align(PW.amps[ch], LV_ALIGN_TOP_RIGHT, 0, -4);
        PW.names[ch] = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_INK, PW_CELL_W - 28);
        lv_obj_align(PW.names[ch], LV_ALIGN_BOTTOM_LEFT, 0, -18);
        PW.meters[ch] = bz_meter(c, PW_CELL_W - 28, 8);
        lv_obj_align(PW.meters[ch], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    lv_obj_t *t = bz_tile(page, PW_RIGHT, 4 * PW_CELL_H + 3 * 10);
    lv_obj_set_pos(t, W - PAD - PW_RIGHT, BODY_Y);
    bz_label(t, "battery", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *br = bz_row(t, 8);
    lv_obj_align(br, LV_ALIGN_TOP_RIGHT, 0, 0);
    PW.mark = bz_mark(br, BZ_STALE, 10);
    PW.band = bz_label(br, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *vr = bz_row(t, 10);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_pos(vr, 0, 28);
    PW.batt = bz_label(vr, "\xe2\x80\x94", BZ_F_DISPLAY, BZ_C_INK);
    lv_obj_t *vu = bz_label(vr, "v", BZ_F_NAME, BZ_C_DIM);
    lv_obj_set_style_pad_bottom(vu, 12, 0);
    PW.spark = bz_spark(t, PW_RIGHT - 2 * BZ_PAD_TILE, 70, 120);
    bz_spark_min_span(PW.spark, 1.0f);
    lv_obj_set_pos(PW.spark, 0, 140);
    lv_obj_t *cl = bz_label(t, "total current", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(cl, 0, 240);
    lv_obj_t *cr = bz_row(t, 8);
    lv_obj_set_flex_align(cr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(cr, LV_ALIGN_TOP_RIGHT, 0, 226);
    PW.total = bz_label(cr, "\xe2\x80\x94", BZ_F_NAME, BZ_C_INK);
    bz_label(cr, "a", BZ_F_LABEL, BZ_C_DIM);
    PW.curr_spark = bz_spark(t, PW_RIGHT - 2 * BZ_PAD_TILE, 70, 120);
    bz_spark_min_span(PW.curr_spark, 20.0f);
    lv_obj_set_pos(PW.curr_spark, 0, 276);
    bz_spark_color(PW.curr_spark, BZ_C_ICE);
    PW.foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(PW.foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    ui_on_page_refresh(PG_POWER, power_refresh, NULL);
}

/* ================================================================== motion */

#define MO_SWERVE 470

static struct {
    lv_obj_t *swerve, *pose, *heading, *speed;
    lv_obj_t *grid;
    lv_obj_t *mt[CAT_MAX_MECHS], *mname[CAT_MAX_MECHS], *mstate[CAT_MAX_MECHS], *mval[CAT_MAX_MECHS],
        *munit[CAT_MAX_MECHS], *msp[CAT_MAX_MECHS], *mmark[CAT_MAX_MECHS], *mfoot[CAT_MAX_MECHS], *mgoal[CAT_MAX_MECHS];
    int nm;
    uint32_t sig;
} MO;

/* Four modules at the chassis corners, each an arrow for what it's doing (ink) over what it was asked
 * to do (dim): angle is direction, length is speed. */
static void swerve_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const cat_robot_t *r = R;
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    float half = (lv_area_get_width(&a) < lv_area_get_height(&a) ? lv_area_get_width(&a) : lv_area_get_height(&a)) * 0.30f;
    float heading = r->have_pose ? (float)r->pose_rad : 0;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_TRANSP;
    rd.border_color = bz_lv(BZ_C_FAINT);
    rd.border_width = 2;
    rd.radius = 18;
    lv_area_t box = { (int)(cx - half - 26), (int)(cy - half - 26), (int)(cx + half + 26), (int)(cy + half + 26) };
    lv_draw_rect(layer, &rd, &box);

    /* the front, marked with a short bar: the robot's heading turns it */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = bz_lv(BZ_C_SIGNAL);
    ld.width = 4;
    ld.round_start = ld.round_end = 1;
    ld.p1.x = cx - 20; ld.p1.y = box.y1 - 12;
    ld.p2.x = cx + 20; ld.p2.y = box.y1 - 12;
    lv_draw_line(layer, &ld);
    (void)heading;

    static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } }; /* FL FR BL BR */
    for (int i = 0; i < 4; i++) {
        float mx = cx + corner[i][0] * half, my = cy + corner[i][1] * half;
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.center.x = (int32_t)mx;
        ad.center.y = (int32_t)my;
        ad.radius = 34;
        ad.width = 2;
        ad.start_angle = 0;
        ad.end_angle = 360;
        ad.color = bz_lv(BZ_C_FAINT);
        lv_draw_arc(layer, &ad);
        for (int pass = 0; pass < 2; pass++) {
            bool target = pass == 0;
            if (target && !r->have_targets) continue;
            if (!target && i >= r->nmodules) continue;
            float sp = (float)(target ? r->target_speed[i] : r->module_speed[i]);
            float an = (float)(target ? r->target_angle[i] : r->module_angle[i]);
            /* WPILib angles are counter-clockwise from the robot's front (up on screen) */
            float len = 14 + fminf(fabsf(sp), 5.0f) * 16;
            float dx = -sinf(an) * len, dy = -cosf(an) * len;
            if (sp < 0) { dx = -dx; dy = -dy; }
            ld.color = bz_lv(target ? BZ_C_DIM : BZ_C_INK);
            ld.width = target ? 3 : 5;
            ld.p1.x = mx; ld.p1.y = my;
            ld.p2.x = mx + dx; ld.p2.y = my + dy;
            lv_draw_line(layer, &ld);
        }
    }
}

static void motion_rebuild(const cat_robot_t *r)
{
    lv_obj_clean(MO.grid);
    MO.nm = r->nmechs;
    int w = (W - 2 * PAD - MO_SWERVE - BZ_GAP - BZ_GAP) / 2;
    for (int i = 0; i < r->nmechs && i < 6; i++) {
        lv_obj_t *t = bz_tile(MO.grid, w, 186);
        MO.mt[i] = t;
        MO.mname[i] = bz_label(t, r->mechs[i].name, BZ_F_LABEL, BZ_C_DIM);
        lv_obj_t *sr = bz_row(t, 8);
        lv_obj_align(sr, LV_ALIGN_TOP_RIGHT, 0, 0);
        MO.mmark[i] = bz_mark(sr, BZ_STALE, 10);
        MO.mstate[i] = bz_label(sr, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_t *vr = bz_row(t, 8);
        lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_pos(vr, 0, 28);
        MO.mval[i] = bz_label(vr, "\xe2\x80\x94", BZ_F_VALUE, BZ_C_INK);
        MO.munit[i] = bz_label(vr, r->mechs[i].unit, BZ_F_BODY, BZ_C_DIM);
        lv_obj_set_style_pad_bottom(MO.munit[i], 8, 0);
        MO.msp[i] = bz_label(t, "", BZ_F_LABEL, BZ_C_DIM);
        lv_obj_align(MO.msp[i], LV_ALIGN_TOP_RIGHT, 0, 30);
        MO.mgoal[i] = bz_meter(t, w - 2 * BZ_PAD_TILE, 8);
        lv_obj_align(MO.mgoal[i], LV_ALIGN_BOTTOM_LEFT, 0, -24);
        MO.mfoot[i] = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_align(MO.mfoot[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    if (!r->nmechs) {
        lv_obj_t *t = bz_tile(MO.grid, 2 * w + BZ_GAP, 186);
        bz_label(t, "no mechanisms published", BZ_F_BODY, BZ_C_DIM);
        lv_obj_t *l = bz_label(t, "Catalyst mechanisms publish AngleDegrees, PositionMeters or VelocityRPS under /Catalyst/<name>/.",
                               BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(l, 2 * w - 2 * BZ_PAD_TILE);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void motion_refresh(void *u)
{
    (void)u;
    const cat_robot_t *r = R;
    uint32_t sig = 2166136261u ^ (uint32_t)r->nmechs;
    for (int i = 0; i < r->nmechs; i++)
        for (const char *p = r->mechs[i].name; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
    if (sig != MO.sig) {
        MO.sig = sig;
        motion_rebuild(r);
    }
    for (int i = 0; i < r->nmechs && i < 6; i++) {
        const cat_mech_t *m = &r->mechs[i];
        const char *fmt = m->kind == CAT_MECH_LINEAR ? "%.3f" : m->kind == CAT_MECH_FLYWHEEL ? "%.1f" : "%.1f";
        char b[24], sp[24];
        ui_text(MO.mval[i], "%s", bz_fmt(b, sizeof b, m->pos == m->pos, fmt, m->pos));
        ui_text(MO.msp[i], "goal %s", bz_fmt(sp, sizeof sp, m->setpoint == m->setpoint, fmt, m->setpoint));
        ui_text(MO.mstate[i], "%s", m->state[0] ? m->state : " ");
        bz_mark_set(MO.mmark[i], m->at_goal < 0 ? BZ_STALE : m->at_goal ? BZ_OK : BZ_INFO);
        /* how far along to the goal: |pos| against |setpoint| */
        float f = 0;
        if (m->setpoint == m->setpoint && m->pos == m->pos && fabs(m->setpoint) > 1e-6) f = (float)(m->pos / m->setpoint);
        bz_meter_set(MO.mgoal[i], f, m->at_goal == 1 ? BZ_C_OK : BZ_C_ICE);
        char a[16], tc[16];
        ui_text(MO.mfoot[i], "%s a · %s °c", bz_fmt(a, sizeof a, m->current_a == m->current_a, "%.1f", m->current_a),
                bz_fmt(tc, sizeof tc, m->temp_c == m->temp_c, "%.0f", m->temp_c));
        bz_set_color(MO.mfoot[i], m->temp_c >= 70 ? BZ_C_WARN : BZ_C_DIM);
    }
    char b1[16], b2[16], b3[16];
    if (r->have_pose)
        ui_text(MO.pose, "x %s  y %s m", bz_fmt(b1, sizeof b1, true, "%.2f", r->pose_x), bz_fmt(b2, sizeof b2, true, "%.2f", r->pose_y));
    else ui_text(MO.pose, "no pose");
    ui_text(MO.heading, "%s°", bz_fmt(b3, sizeof b3, r->heading_deg == r->heading_deg, "%.1f", r->heading_deg));
    double spd = 0;
    for (int i = 0; i < r->nmodules; i++) spd += fabs(r->module_speed[i]);
    if (r->nmodules) ui_text(MO.speed, "%.2f m/s", spd / r->nmodules);
    else ui_text(MO.speed, " ");
    lv_obj_invalidate(MO.swerve);
}

void ui_page_motion(lv_obj_t *page)
{
    ui_head(page, "Motion", NULL);
    lv_obj_t *t = bz_tile(page, MO_SWERVE, BODY_BOTTOM - BODY_Y);
    lv_obj_set_pos(t, PAD, BODY_Y);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(t, open_app_tap, (void *)&APP_FIELD);
    bz_label(t, "swerve", BZ_F_LABEL, BZ_C_DIM);
    MO.heading = bz_label(t, "", BZ_F_NAME, BZ_C_INK);
    lv_obj_align(MO.heading, LV_ALIGN_TOP_RIGHT, 0, -4);
    MO.swerve = bz_box(t);
    lv_obj_set_size(MO.swerve, MO_SWERVE - 2 * BZ_PAD_TILE, BODY_BOTTOM - BODY_Y - 2 * BZ_PAD_TILE - 76);
    lv_obj_set_pos(MO.swerve, 0, 38);
    lv_obj_add_event_cb(MO.swerve, swerve_draw, LV_EVENT_DRAW_MAIN, NULL);
    MO.pose = bz_label(t, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(MO.pose, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    MO.speed = bz_label(t, "", BZ_F_LABEL, BZ_C_INK);
    lv_obj_align(MO.speed, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    MO.grid = bz_row(page, BZ_GAP);
    lv_obj_set_flex_flow(MO.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(MO.grid, W - 2 * PAD - MO_SWERVE - BZ_GAP);
    lv_obj_set_pos(MO.grid, PAD + MO_SWERVE + BZ_GAP, BODY_Y);
    MO.sig = 1;
    ui_on_page_refresh(PG_MOTION, motion_refresh, NULL);
}

/* ================================================================== apps */

/* The app library: every app, in four groups — the robot through Catalyst, diagnosing it from the
 * outside, the tablet's own tools, and Catalyst OS's everyday apps. Scrolls when it outgrows the screen. */
typedef struct { const ui_app_t *app; const char *icon; const char *label; const char *hint; } app_entry_t;
static const app_entry_t APPS_ROBOT[] = {
    { &APP_PREFLIGHT, BZ_I_CHECKLIST, "preflight", "go / no-go" },
    { &APP_ALERTS, BZ_I_WARNING, "alerts", "errors, health" },
    { &APP_TUNE, BZ_I_TUNE, "tune", "tunables" },
    { &APP_AUTO, BZ_I_FLAG, "auto", "chooser" },
    { &APP_FIELD, BZ_I_STADIUM, "field", "pose, vision" },
    { &APP_ROBOT, BZ_I_SMART_TOY, "robot", "spec, versions" },
    { &APP_MOTORS, BZ_I_HISTORY, "motors", "lifetime, wear" },
    { &APP_STATES, BZ_I_ACCOUNT_TREE, "states", "timelines" },
    { &APP_CONTROLS, BZ_I_SPORTS_ESPORTS, "controls", "bindings" },
};
static const app_entry_t APPS_DIAG[] = {
    { &APP_SYSTEMCORE, BZ_I_DEVELOPER_BOARD, "systemcore", "cores, buses" },
    { &APP_CANTAP, BZ_I_CABLE, "can tap", "bus sniffer" },
    { &APP_RECORDER, BZ_I_FIBER_MANUAL_RECORD, "recorder", "black box" },
    { &APP_LOGS, BZ_I_RECEIPT_LONG, "logs", "ds logs, sd" },
    { &APP_LEVEL, BZ_I_STRAIGHTEN, "level", "imu angle" },
    { &APP_LENS, BZ_I_PHOTO_CAMERA, "lens", "camera, clips" },
    { &APP_GEAR, BZ_I_SPEED, "gearing", "speed, torque" },
    { &APP_RULER, BZ_I_STRAIGHTEN, "ruler", "mm, inches" },
    { &APP_REF, BZ_I_HANDYMAN, "reference", "wiring, can" },
};
static const app_entry_t APPS_TABLET[] = {
    { &APP_ASSIST, BZ_I_AUTO_AWESOME, "assist", "ai technician" },
    { &APP_LINK, BZ_I_COMPUTER, "link", "pc, patches" },
    { &APP_COMPANION, BZ_I_VISIBILITY, "companion", "desk mode" },
    { &APP_TIMER, BZ_I_TIMER, "timer", "match, stopwatch" },
    { &APP_CALC, BZ_I_CALCULATE, "calculator", "ratios, units" },
    { &APP_NOTES, BZ_I_EDIT_NOTE, "notes", "pit notebook" },
    { &APP_CHECK, BZ_I_CHECKLIST_RTL, "checklist", "before a match" },
    { &APP_LIGHT, BZ_I_FLASHLIGHT_ON, "flashlight", "white, red" },
    { &APP_FILES, BZ_I_FOLDER, "files", "microsd" },
    { &APP_SYSMON, BZ_I_MONITORING, "system", "this tablet" },
    { &APP_SETTINGS, BZ_I_SETTINGS, "settings", "everything" },
};
static const app_entry_t APPS_EVERYDAY[] = {
    { &APP_CLOCK, BZ_I_SCHEDULE, "clock", "alarms, world" },
    { &APP_CALENDAR, BZ_I_GRID_VIEW, "calendar", "month, events" },
    { &APP_DOCS, BZ_I_DESCRIPTION, "documents", "txt, md" },
    { &APP_PHOTOS, BZ_I_CAMERA, "photos", "pictures" },
    { &APP_STORAGE, BZ_I_SD_CARD, "storage", "card usage" },
};

const ui_app_t *ui_app_find(const char *name)
{
    const app_entry_t *groups[4] = { APPS_ROBOT, APPS_DIAG, APPS_TABLET, APPS_EVERYDAY };
    int counts[4] = { (int)(sizeof APPS_ROBOT / sizeof APPS_ROBOT[0]), (int)(sizeof APPS_DIAG / sizeof APPS_DIAG[0]),
                      (int)(sizeof APPS_TABLET / sizeof APPS_TABLET[0]),
                      (int)(sizeof APPS_EVERYDAY / sizeof APPS_EVERYDAY[0]) };
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < counts[g]; i++)
            if (!strcmp(groups[g][i].label, name) || !strcmp(groups[g][i].app->name, name)) return groups[g][i].app;
    return NULL;
}

#define APPS_COLS 6
#define APP_TILE_H 140

static void apps_group(lv_obj_t *col, const char *title, const app_entry_t *e, int n)
{
    lv_obj_t *l = bz_label(col, title, BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_style_pad_top(l, 6, 0);
    int w = (W - 2 * PAD - (APPS_COLS - 1) * BZ_GAP) / APPS_COLS;
    lv_obj_t *grid = bz_row(col, BZ_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BZ_GAP, 0);
    lv_obj_set_width(grid, W - 2 * PAD);
    for (int i = 0; i < n; i++) {
        lv_obj_t *t = bz_tile(grid, w, APP_TILE_H);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(t, 18, 0);
        lv_obj_t *ic = bz_tile(t, 56, 56);
        lv_obj_add_style(ic, bz_style_fill(BZ_C_SURFACE3), 0);
        lv_obj_set_style_radius(ic, 18, 0);
        lv_obj_set_style_pad_all(ic, 0, 0);
        lv_obj_t *g = bz_icon(ic, e[i].icon, 32, BZ_C_INK);
        lv_obj_center(g);
        lv_obj_t *lb = bz_label_line(t, e[i].label, BZ_F_BODY, BZ_C_INK, w - 36);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, -22);
        lv_obj_t *h = bz_label_line(t, e[i].hint, BZ_F_CAPTION, BZ_C_DIM, w - 36);
        lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        bz_on_tap(t, open_app_tap, (void *)e[i].app);
    }
}

void ui_page_tools(lv_obj_t *page)
{
    lv_obj_t *right = ui_head(page, "Apps", NULL);
    char n[16];
    snprintf(n, sizeof n, "%d apps", (int)(sizeof APPS_ROBOT / sizeof APPS_ROBOT[0] + sizeof APPS_DIAG / sizeof APPS_DIAG[0] +
                                           sizeof APPS_TABLET / sizeof APPS_TABLET[0] +
                                           sizeof APPS_EVERYDAY / sizeof APPS_EVERYDAY[0]));
    bz_label_line(right, n, BZ_F_LABEL, BZ_C_DIM, ui_head_width("Apps"));
    lv_obj_t *wrap = bz_box(page);
    lv_obj_set_pos(wrap, PAD, BODY_Y);
    lv_obj_t *col = ui_scroller(wrap, W - 2 * PAD, BODY_H);
    apps_group(col, "robot", APPS_ROBOT, (int)(sizeof APPS_ROBOT / sizeof APPS_ROBOT[0]));
    apps_group(col, "diagnose", APPS_DIAG, (int)(sizeof APPS_DIAG / sizeof APPS_DIAG[0]));
    apps_group(col, "this tablet", APPS_TABLET, (int)(sizeof APPS_TABLET / sizeof APPS_TABLET[0]));
    apps_group(col, "everyday", APPS_EVERYDAY, (int)(sizeof APPS_EVERYDAY / sizeof APPS_EVERYDAY[0]));
    /* room to scroll the last row clear of the dock */
    lv_obj_t *sp = bz_box(col);
    lv_obj_set_height(sp, DOCK_CLEAR);
}
