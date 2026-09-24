/* Shop tools: the ones a pit reaches for with a mechanism apart on the cart.
 *
 * gearing   a motor, how many, the reduction and the wheel: free speed, stall torque, pushing force and
 *           the torque a stator current limit leaves. Motor constants are Catalyst's MotorType presets
 *           (12 V nominal), ideal: no losses.
 * ruler     millimetres or inches along the glass's bottom edge, a tap-to-measure marker, and a
 *           calibration nudge saved on the tablet.
 * reference the numbers a repair needs without a laptop: wiring, CAN, battery, the team's addresses. */
#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)

static lv_obj_t *wrap_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *column_tile(lv_obj_t *b, int x, int w)
{
    lv_obj_t *t = bz_tile(b, w, APP_H);
    lv_obj_set_pos(t, x, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0);
    return t;
}

/* one chip of a group on, the rest off */
static void pick(lv_obj_t *const *chips, int n, int on)
{
    for (int i = 0; i < n; i++) ui_chip_set(chips[i], i == on);
}

/* ================================================================== gearing */

static const struct { const char *label; float torque, rpm, stall_a; } MOTOR[] = {
    { "X60", 7.09f, 6000, 366 },    { "X60 foc", 9.37f, 5800, 483 }, { "X44", 4.05f, 7530, 275 },
    { "X44 foc", 5.45f, 7200, 366 }, { "Falcon", 4.69f, 6380, 257 }, { "Falcon foc", 5.84f, 6080, 304 },
    { "Minion", 3.1f, 7200, 200 },   { "NEO", 2.6f, 5676, 105 },     { "Vortex", 3.6f, 6784, 211 },
    { "NEO 550", 0.97f, 11000, 100 },
};
#define NMOTOR (int)(sizeof MOTOR / sizeof MOTOR[0])
static const float RATIO_PRESET[] = { 3, 5, 6.75f, 8.14f, 12, 25, 50, 100 };
static const char *const RATIO_LABEL[] = { "3:1", "5:1", "6.75 (L2)", "8.14 (L1)", "12:1", "25:1", "50:1", "100:1" };
#define NRATIO 8
static const float WHEEL_IN[] = { 0, 2, 3, 4, 6 };
static const char *const WHEEL_LABEL[] = { "none", "2 in", "3 in", "4 in", "6 in" };
static const int LIMIT_A[] = { 40, 60, 80, 0 };
static const char *const LIMIT_LABEL[] = { "40", "60", "80", "off" };

static struct {
    lv_obj_t *motor[NMOTOR], *count[4], *preset[NRATIO], *wheel[5], *limit[4], *ratio;
    lv_obj_t *speed, *speed_sub, *torque, *torque_sub, *lim, *lim_sub, *current;
    int m, n, w, l;
    float ratio_v;
} GR = { .m = 0, .n = 1, .w = 3, .l = 0, .ratio_v = 6.75f };

static void gr_show(void)
{
    pick(GR.motor, NMOTOR, GR.m);
    pick(GR.count, 4, GR.n - 1);
    pick(GR.wheel, 5, GR.w);
    pick(GR.limit, 4, GR.l);
    int rp = -1;
    for (int i = 0; i < NRATIO; i++)
        if (fabsf(RATIO_PRESET[i] - GR.ratio_v) < 0.005f) rp = i;
    pick(GR.preset, NRATIO, rp);
    ui_text(GR.ratio, "%.2f : 1", GR.ratio_v);

    float free = MOTOR[GR.m].rpm / GR.ratio_v;
    float stall = GR.n * MOTOR[GR.m].torque * GR.ratio_v;
    float kt = MOTOR[GR.m].torque / MOTOR[GR.m].stall_a;
    float r = WHEEL_IN[GR.w] * 0.0254f / 2;
    ui_text(GR.speed, "%.0f rpm", free);
    if (r > 0) {
        float v = free / 60 * 2 * 3.14159265f * r;
        ui_text(GR.speed_sub, "%.2f m/s \xc2\xb7 %.1f ft/s at the wheel", v, v * 3.2808399f);
    } else {
        ui_text(GR.speed_sub, "%.1f rev/s \xc2\xb7 %.0f \xc2\xb0/s at the output", free / 60, free * 6);
    }
    ui_text(GR.torque, "%.1f N\xc2\xb7m", stall);
    if (r > 0) ui_text(GR.torque_sub, "%.0f N (%.0f lbf) of push at stall", stall / r, stall / r * 0.22480894f);
    else ui_text(GR.torque_sub, "%.0f lbf\xc2\xb7in at the output, at stall", stall * 8.8507458f);
    if (LIMIT_A[GR.l]) {
        float t = fminf(GR.n * kt * LIMIT_A[GR.l] * GR.ratio_v, stall);
        ui_text(GR.lim, "%.1f N\xc2\xb7m", t);
        if (r > 0) ui_text(GR.lim_sub, "at %d A stator per motor \xc2\xb7 %.0f N of push", LIMIT_A[GR.l], t / r);
        else ui_text(GR.lim_sub, "at %d A stator per motor (%.0f %% of stall)", LIMIT_A[GR.l], t / stall * 100);
    } else {
        ui_text(GR.lim, "\xe2\x80\x94");
        ui_text(GR.lim_sub, "no current limit: the stall figures apply");
    }
    ui_text(GR.current, "%d \xc3\x97 %.0f A = %.0f A drawn at stall", GR.n, MOTOR[GR.m].stall_a, GR.n * MOTOR[GR.m].stall_a);
}

static void gr_motor(lv_obj_t *o, void *u) { (void)o; GR.m = (int)(intptr_t)u; gr_show(); }
static void gr_count(lv_obj_t *o, void *u) { (void)o; GR.n = (int)(intptr_t)u + 1; gr_show(); }
static void gr_wheel(lv_obj_t *o, void *u) { (void)o; GR.w = (int)(intptr_t)u; gr_show(); }
static void gr_limit(lv_obj_t *o, void *u) { (void)o; GR.l = (int)(intptr_t)u; gr_show(); }
static void gr_preset(lv_obj_t *o, void *u) { (void)o; GR.ratio_v = RATIO_PRESET[(intptr_t)u]; gr_show(); }

static void gr_step(lv_obj_t *o, void *u)
{
    (void)o;
    int dir = (int)(intptr_t)u;
    float step = GR.ratio_v < 10 ? 0.05f : GR.ratio_v < 50 ? 0.5f : 1;
    float v = roundf((GR.ratio_v + dir * step) / step) * step;
    GR.ratio_v = v < 0.1f ? 0.1f : v > 500 ? 500 : v;
    gr_show();
}

static lv_obj_t *gr_readout(lv_obj_t *t, const char *label, lv_obj_t **big, lv_obj_t **sub, int w)
{
    bz_label(t, label, BZ_F_LABEL, BZ_C_DIM);
    *big = bz_label_line(t, "", BZ_F_VALUE, BZ_C_INK, w);
    *sub = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(*sub, w);
    return *big;
}

static void gear_build(lv_obj_t *b)
{
    int w1 = 330, w2 = 430, w3 = W - 2 * PAD - w1 - w2 - 2 * BZ_GAP;
    lv_obj_t *t = column_tile(b, PAD, w1);
    bz_label(t, "motor", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = wrap_row(t, IN(w1));
    for (int i = 0; i < NMOTOR; i++) GR.motor[i] = ui_chip(r, MOTOR[i].label, gr_motor, (void *)(intptr_t)i);
    bz_label(t, "motors on the shaft", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(t, IN(w1));
    static const char *const N[4] = { "1", "2", "3", "4" };
    for (int i = 0; i < 4; i++) GR.count[i] = ui_chip(r, N[i], gr_count, (void *)(intptr_t)i);

    t = column_tile(b, PAD + w1 + BZ_GAP, w2);
    bz_label(t, "reduction", BZ_F_LABEL, BZ_C_DIM);
    r = bz_row(t, 10);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_button(r, NULL, "\xe2\x80\x93", gr_step, (void *)(intptr_t)-1); /* en dash: the faces have no minus sign */
    GR.ratio = bz_label_line(r, "", BZ_F_NAME, BZ_C_INK, 170);
    lv_obj_set_style_text_align(GR.ratio, LV_TEXT_ALIGN_CENTER, 0);
    ui_button(r, NULL, "+", gr_step, (void *)(intptr_t)1);
    r = wrap_row(t, IN(w2));
    for (int i = 0; i < NRATIO; i++) GR.preset[i] = ui_chip(r, RATIO_LABEL[i], gr_preset, (void *)(intptr_t)i);
    bz_label(t, "wheel", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(t, IN(w2));
    for (int i = 0; i < 5; i++) GR.wheel[i] = ui_chip(r, WHEEL_LABEL[i], gr_wheel, (void *)(intptr_t)i);
    bz_label(t, "stator current limit, amps", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(t, IN(w2));
    for (int i = 0; i < 4; i++) GR.limit[i] = ui_chip(r, LIMIT_LABEL[i], gr_limit, (void *)(intptr_t)i);

    t = column_tile(b, PAD + w1 + w2 + 2 * BZ_GAP, w3);
    lv_obj_set_style_pad_row(t, 6, 0);
    gr_readout(t, "free speed", &GR.speed, &GR.speed_sub, IN(w3));
    gr_readout(t, "stall torque", &GR.torque, &GR.torque_sub, IN(w3));
    gr_readout(t, "torque at the limit", &GR.lim, &GR.lim_sub, IN(w3));
    GR.current = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(GR.current, IN(w3));
    lv_obj_t *note = bz_label(t, "Ideal: no losses. Real mechanisms reach roughly 80\xe2\x80\x93" "90 % of these.", BZ_F_CAPTION,
                              BZ_C_FAINT);
    lv_obj_set_width(note, IN(w3));
    gr_show();
}

const ui_app_t APP_GEAR = { .name = "gearing", .icon = BZ_I_SPEED, .build = gear_build };

/* ================================================================== ruler */

/* The panel's active area is 110.4 mm across its 1280 pixels (the 5" module's datasheet); a nudge
 * against a real rule corrects the rest, and is kept. */
#define RULER_PX_MM (1280 / 110.4f)
#define RULER_H 150

static struct {
    lv_obj_t *box, *read, *scale_note, *unit_chips[2];
    float px_mm;
    bool inches;
    int mark; /* px from the left edge, -1 none */
} RU = { .mark = -1 };

static void ru_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int base = a.y2; /* the glass's bottom edge */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.width = 2;
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = bz_font(BZ_F_LABEL);
    td.color = bz_lv(BZ_C_DIM);
    /* ticks: mm (1, 5, 10) or inches (1/16, 1/8, 1/4, 1/2, 1) */
    float step = RU.inches ? RU.px_mm * 25.4f / 16 : RU.px_mm;
    for (int i = 0;; i++) {
        float x = a.x1 + i * step;
        if (x > a.x2) break;
        int len;
        bool num = false;
        if (RU.inches) {
            len = i % 16 == 0 ? 70 : i % 8 == 0 ? 48 : i % 4 == 0 ? 34 : i % 2 == 0 ? 24 : 16;
            num = i % 16 == 0;
        } else {
            len = i % 10 == 0 ? 70 : i % 5 == 0 ? 44 : 24;
            num = i % 10 == 0;
        }
        ld.color = bz_lv(len >= 44 ? BZ_C_INK : BZ_C_DIM);
        ld.p1.x = x;
        ld.p1.y = base;
        ld.p2.x = x;
        ld.p2.y = base - len;
        lv_draw_line(layer, &ld);
        if (num && i > 0) {
            char s[8];
            snprintf(s, sizeof s, "%d", RU.inches ? i / 16 : i / 10);
            td.text = s;
            td.text_local = 1;
            lv_area_t ta = { (int32_t)x - 30, base - 104, (int32_t)x + 30, base - 76 };
            td.align = LV_TEXT_ALIGN_CENTER;
            lv_draw_label(layer, &td, &ta);
        }
    }
    if (RU.mark >= 0) {
        ld.color = bz_lv(BZ_C_ICE);
        ld.width = 3;
        ld.p1.x = a.x1 + RU.mark;
        ld.p1.y = a.y1;
        ld.p2.x = a.x1 + RU.mark;
        ld.p2.y = base;
        lv_draw_line(layer, &ld);
    }
}

static void ru_show(void)
{
    pick(RU.unit_chips, 2, RU.inches);
    if (RU.mark >= 0) {
        float mm = RU.mark / RU.px_mm;
        ui_text(RU.read, "%.1f mm \xc2\xb7 %.3f in", mm, mm / 25.4f);
    } else {
        ui_text(RU.read, "tap the scale to measure from the left edge");
    }
    ui_text(RU.scale_note, "%.3f px per mm (%+.1f %% from the datasheet)", RU.px_mm, (RU.px_mm / RULER_PX_MM - 1) * 100);
    lv_obj_invalidate(RU.box);
}

static void ru_press(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    RU.mark = p.x - a.x1 < 0 ? 0 : p.x - a.x1;
    ru_show();
}

static void ru_unit(lv_obj_t *o, void *u) { (void)o; RU.inches = (intptr_t)u != 0; ru_show(); }

static void ru_nudge(lv_obj_t *o, void *u)
{
    (void)o;
    int d = (int)(intptr_t)u;
    RU.px_mm = d ? RU.px_mm * (1 + d * 0.002f) : RULER_PX_MM;
    char s[16];
    snprintf(s, sizeof s, "%.4f", RU.px_mm);
    hal_kv_set("ruler_pxmm", s);
    ru_show();
}

static void ruler_build(lv_obj_t *b)
{
    char s[16];
    RU.px_mm = hal_kv_get("ruler_pxmm", s, sizeof s) ? (float)atof(s) : RULER_PX_MM;
    if (RU.px_mm < RULER_PX_MM * 0.9f || RU.px_mm > RULER_PX_MM * 1.1f) RU.px_mm = RULER_PX_MM;

    lv_obj_t *t = bz_tile(b, W - 2 * PAD, H - APP_Y - RULER_H - 2 * BZ_GAP);
    lv_obj_set_pos(t, PAD, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0);
    RU.read = bz_label_line(t, "", BZ_F_NAME, BZ_C_INK, IN(W - 2 * PAD));
    lv_obj_t *r = bz_row(t, 10);
    RU.unit_chips[0] = ui_chip(r, "millimetres", ru_unit, (void *)0);
    RU.unit_chips[1] = ui_chip(r, "inches", ru_unit, (void *)1);
    bz_label(t, "calibrate: lay a real rule along the bottom edge and nudge until they agree", BZ_F_LABEL, BZ_C_DIM);
    r = bz_row(t, 10);
    ui_button(r, NULL, "shorter", ru_nudge, (void *)(intptr_t)1);
    ui_button(r, NULL, "longer", ru_nudge, (void *)(intptr_t)-1);
    ui_button(r, BZ_I_REFRESH, "datasheet", ru_nudge, (void *)0);
    RU.scale_note = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);

    /* the scale: the whole width, down to the glass's edge */
    RU.box = bz_box(b);
    lv_obj_set_size(RU.box, W, RULER_H);
    lv_obj_set_pos(RU.box, 0, H - RULER_H);
    lv_obj_add_flag(RU.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(RU.box, ru_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(RU.box, ru_press, LV_EVENT_PRESSING, NULL);
    ru_show();
}

const ui_app_t APP_RULER = { .name = "ruler", .icon = BZ_I_STRAIGHTEN, .build = ruler_build };

/* ================================================================== reference */

enum { RF_WIRING, RF_CAN, RF_BATTERY, RF_NETWORK, RF_MOTORS, RF_COUNT };
static const char *const RF_NAME[RF_COUNT] = { "wiring", "can bus", "battery", "network", "motors" };

static const char *const RF_TEXT[RF_COUNT] = {
    /* wiring */
    "Wire for its breaker (the usual FRC table; check this season's manual):\n"
    "\xe2\x80\x94 40 A breaker: 12 AWG or thicker\n"
    "\xe2\x80\x94 30 A: 14 AWG \xc2\xb7 20 A: 18 AWG \xc2\xb7 5 A and under: 22 AWG\n"
    "\xe2\x80\x94 battery to main breaker to power hub: 6 AWG, 120 A main breaker\n"
    "Red, white, brown or yellow for +12 V; black or blue for ground.\n"
    "Tug-test every crimp and Wago. A connection that is warm after a match is failing.",
    /* can */
    "Two 120 \xce\xa9 terminators, one at each end of the chain.\n"
    "Power off, meter CAN-H to CAN-L: about 60 \xce\xa9 is healthy; about 120 \xce\xa9 means one terminator is "
    "missing; near 40 \xce\xa9 means one too many; open means a break.\n"
    "Yellow is CAN-H, green is CAN-L.\n"
    "Devices dropping out together: look where the chain passes them (connector, pinched wire), not at the "
    "devices. Keep bus load well under 80 %.",
    /* battery */
    "Resting (off the charger for an hour, robot off):\n"
    "\xe2\x80\x94 12.8-13.0 V: full \xc2\xb7 about 12.4 V: charge it before a match\n"
    "Under load the voltage sags; the controller browns out near 6.8 V and cuts the motors first.\n"
    "Internal resistance (a battery analyser) is the real test: under about 0.015 \xce\xa9 is match-ready; "
    "a battery that climbs past about 0.02 \xce\xa9 is for practice.\n"
    "Check the lugs are tight and the heat-shrink is whole: a loose lug looks like a bad battery.",
    /* network: filled in from the team number */
    "",
    /* motors */
    "Before blaming code: spin it by hand (binding), check the breaker, check it is on the CAN chain and "
    "its firmware is current.\n"
    "A brushless motor turning the wrong way is inverted in code, never by swapping wires.\n"
    "Supply limit: protects the breaker and battery. Stator limit: caps torque and heat.\n"
    "Hot after a match (too hot to hold): stalled or over-geared \xe2\x80\x94 check the gearing app.",
};

static struct {
    lv_obj_t *nav[RF_COUNT], *title, *text;
    int cur;
} RF;

static void rf_show(void)
{
    pick(RF.nav, RF_COUNT, RF.cur);
    ui_text(RF.title, "%s", RF_NAME[RF.cur]);
    if (RF.cur == RF_NETWORK) {
        int t = S.team;
        static char net[512]; /* past ui_text's 256 */
        snprintf(net, sizeof net,
                "Team %d on the robot's own network (10.TE.AM.x):\n"
                "\xe2\x80\x94 radio 10.%d.%d.1 \xc2\xb7 robot controller 10.%d.%d.2 \xc2\xb7 driver station 10.%d.%d.5\n"
                "\xe2\x80\x94 Systemcore: robot.local \xc2\xb7 roboRIO: roborio-%d-frc.local\n"
                "\xe2\x80\x94 USB: roboRIO 172.22.11.2 \xc2\xb7 Systemcore 172.26.0.1\n"
                "Wi-Fi to the robot isn't allowed at events: tether over USB-A, or an adapter into the radio.",
                t, t / 100, t % 100, t / 100, t % 100, t / 100, t % 100, t);
        lv_label_set_text_static(RF.text, net);
    } else {
        lv_label_set_text_static(RF.text, RF_TEXT[RF.cur]); /* longer than ui_text's buffer */
    }
}

static void rf_pick(lv_obj_t *o, void *u) { (void)o; RF.cur = (int)(intptr_t)u; rf_show(); }

static void ref_build(lv_obj_t *b)
{
    int lw = 260, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *l = column_tile(b, PAD, lw);
    for (int i = 0; i < RF_COUNT; i++) {
        RF.nav[i] = ui_chip(l, RF_NAME[i], rf_pick, (void *)(intptr_t)i);
        lv_obj_set_width(RF.nav[i], IN(lw));
    }
    lv_obj_t *r = column_tile(b, PAD + lw + BZ_GAP, rw);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    RF.title = bz_label(r, "", BZ_F_TITLE, BZ_C_INK);
    RF.text = bz_label(r, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_width(RF.text, IN(rw));
    lv_label_set_long_mode(RF.text, LV_LABEL_LONG_WRAP);
    rf_show();
}

/* the team number can change in settings: the network card follows */
static void ref_open(void) { rf_show(); }

const ui_app_t APP_REF = { .name = "reference", .icon = BZ_I_HANDYMAN, .build = ref_build, .open = ref_open };
