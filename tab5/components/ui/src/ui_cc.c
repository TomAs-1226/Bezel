/* The control center: a sheet of glass modules pulled down from the top edge (Bezel's cc.js).
 *
 * The pull tracks the finger 1:1 (progress = dy / 380), rubber-bands past fully open, and on release
 * projects its velocity to decide open or closed. Modules cascade in by delay, each materializing by
 * strength, while a frosted, dimmed picture of the page comes down behind them like a blind, its soft
 * edge on the pull (the web specimen frosts the whole page by the pull's amount; on the P4 a
 * cross-fade of every pixel each frame costs more memory traffic than a frame has, while the blind
 * redraws only the rows its edge crosses — see docs/bezel-port.md). Its fills — the brightness and
 * volume levels, a toggle's "on" — are drawn crisp inside the glass, never frosted by it. */
#include "ui_internal.h"

#include <math.h>

#define NMOD 7

static struct {
    lv_obj_t *strip, *scrim;
    lv_obj_t *mods[NMOD];
    bz_glass_t *glass[NMOD];
    int base_y[NMOD];
    float delay[NMOD];
    bz_motion_t p;
    float p0;
    bool dragging;
    lv_obj_t *link_robot, *link_wifi, *link_usb, *link_batt, *bright, *vol;
    lv_obj_t *tog_fill[4], *tog_icon[4];
    int last_y[NMOD], last_opa[NMOD], last_tog;
    bool asleep, shown, frozen;
    bz_motion_t boot;      /* the power-on blind: starts down, lifts off the page */
    double boot_at;
} C;

static void cc_refresh(void *user);

static void cc_to(float target, float v)
{
    bz_motion_to_v(&C.p, target, BZ_RELEASE, v);
    bz_ui_keep_alive();
}

static void st_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
    C.dragging = true;
    C.p0 = C.p.value;
    bz_motion_set(&C.p, C.p.value, 0);
}

static void st_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
    float p = C.p0 + dy / 380.0f;
    p = bz_rubber_clamp(p, 0, 1, 0.12f * 2);
    bz_motion_set(&C.p, p, vy / 380.0f);
}

static void st_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx; (void)u;
    C.dragging = false;
    float v = vy / 380.0f;
    float aim = C.p.value + bz_project(v * 1000, BZ_RATE_FAST) / 1000;
    cc_to(aim > 0.5f ? 1 : 0, C.p.value > 1 ? 0 : v);
}

static void scrim_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cc_to(0, 0);
}

static lv_obj_t *module(int i, int x, int y, int w, int h, float radius, float delay)
{
    lv_obj_t *m = lv_obj_create(bz_ui_glass());
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, w, h);
    lv_obj_set_pos(m, x, y);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(m, 22, 0);
    C.mods[i] = m;
    C.glass[i] = bz_glass_attach(m, 6, radius);
    bz_glass_set_solo(C.glass[i], true); /* staggered, they pass near each other: they must not melt */
    bz_glass_set_strength(C.glass[i], 0);
    C.base_y[i] = y;
    C.delay[i] = delay;
    return m;
}

static void toggle_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    switch (i) {
    case 0: S.dark = !S.dark; bz_ui_set_mode(S.dark, S.calm); break;
    case 1:
#if BZ_LEAN
        /* rotation lock: the picture stays the way up it is now */
        S.auto_rotate = !S.auto_rotate;
#else
        S.calm = !S.calm;
        bz_ui_set_mode(S.dark, S.calm);
#endif
        break;
    case 2:
        /* the panel goes dark until the next touch; the robot link stays up */
        C.asleep = true;
        hal_set_brightness(0);
        cc_to(0, 0);
        break;
    case 3: ui_island_say(BZ_I_POWER, "hold the power button to turn off"); break;
    }
    ui_settings_save();
    hal_tone(1600, 10, S.volume * 0.4f);
}

static void level_cb(lv_obj_t *lv, float v, bool final, void *u)
{
    (void)lv;
    if ((intptr_t)u == 0) {
        S.brightness = v;
        hal_set_brightness(v);
    } else {
        S.volume = v;
        hal_set_volume(v);
    }
    if (final) ui_settings_save();
}

static void cc_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    if (C.asleep && bz_ui_idle_s() < 0.2) {
        C.asleep = false;
        hal_set_brightness(S.brightness);
    }
    if (bz_motion_tick(&C.p) || C.dragging) bz_ui_keep_alive();
    float p = C.p.value < 0 ? 0 : C.p.value;
    bool shown = p > 0.002f || C.dragging;
    /* power-on: the page starts under the frosted blind and it lifts, a beat after the first frame */
    if (C.boot_at == 0) C.boot_at = now + 0.35;
    if (now >= C.boot_at && C.boot.target > 0) bz_motion_to(&C.boot, 0, BZ_SMOOTH);
    if (bz_motion_tick(&C.boot)) bz_ui_keep_alive();
    float blind = C.boot.value > p ? C.boot.value : p;
    bz_comp_set_backdrop(bz_ui_comp(), blind > 1 ? 1 : blind < 0 ? 0 : blind, 0.9f);
    /* the page behind is frosted and dimmed out of sight: it stops redrawing (live numbers under the
     * blind would only make every module re-frost) until the blind starts back up */
    /* (lean: the modules are drawn on the page's own display, so freezing it would freeze them too) */
    bool hold = !BZ_LEAN && ((shown && C.p.target > 0) || C.dragging);
    if (hold != C.frozen) {
        C.frozen = hold;
        bz_ui_freeze(hold);
    }
    if (shown != C.shown) {
        C.shown = shown;
        if (shown) {
            /* lean: no frosted blind — the page goes under a solid dim of the ground, set once */
            if (BZ_LEAN) {
                lv_obj_set_style_bg_color(C.scrim, bz_lv(BZ_C_GROUND), 0);
                lv_obj_set_style_bg_opa(C.scrim, LV_OPA_80, 0);
            }
            lv_obj_remove_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
            cc_refresh(NULL);
        } else {
            lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < NMOD; i++) {
        float k = (p - C.delay[i]) / (1 - C.delay[i]);
        k = k < 0 ? 0 : k;
        float kc = k > 1 ? 1 : k;
        int y = C.base_y[i] - (int)(52 * (1 - kc)) + (int)((k > 1 ? k - 1 : 0) * 60);
        if (bz_ui_calm()) y = C.base_y[i];
        bz_glass_set_strength(C.glass[i], kc);
        /* only what changed: every style set redraws the module's ink */
        int opa = (int)(255 * (kc * 1.4f > 1 ? 1 : kc * 1.4f));
        if (y != C.last_y[i]) lv_obj_set_y(C.mods[i], y);
        if (opa != C.last_opa[i]) {
            lv_obj_set_style_opa(C.mods[i], (lv_opa_t)opa, 0);
            if (kc <= 0.002f) lv_obj_add_flag(C.mods[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(C.mods[i], LV_OBJ_FLAG_HIDDEN);
        }
        C.last_y[i] = y;
        C.last_opa[i] = opa;
    }
}

/* Live state inside the modules, at the model's 10 Hz. */
static void cc_refresh(void *user)
{
    (void)user;
    if (!C.shown) return;
    const cat_robot_t *r = R;
    if (r->connected) ui_text(C.link_robot, "%s · %.0f ms", r->address, r->rtt_ms);
    else ui_text(C.link_robot, "looking for team %d", S.team);
    hal_net_t n;
    hal_net(&n);
    if (n.up) ui_text(C.link_wifi, "%s · %d dBm", n.ssid, n.rssi);
    else ui_text(C.link_wifi, "wi-fi not connected");
    hal_tether_t t;
    hal_tether(&t);
    if (t.up) ui_text(C.link_usb, "%s · %s%s", t.kind, t.ip, t.dhcp ? "" : " (fallback)");
    else if (t.present) ui_text(C.link_usb, "%s attached · no address yet", t.kind[0] ? t.kind : "adapter");
    else ui_text(C.link_usb, "no usb tether");
    hal_battery_t b;
    if (hal_battery(&b)) ui_text(C.link_batt, "%d %% · %.2f v%s", b.percent, b.volts, b.charging ? " · charging" : "");
#if BZ_LEAN
    bool on[4] = { !S.dark, !S.auto_rotate, false, false };
#else
    bool on[4] = { !S.dark, S.calm, false, false };
#endif
    int bits = on[0] | on[1] << 1 | 16;
    (void)0;
    if (bits == C.last_tog) return;
    C.last_tog = bits;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_opa(C.tog_fill[i], on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        bz_set_color(C.tog_icon[i], on[i] ? BZ_C_ON_ICE : BZ_C_INK);
    }
}

void ui_cc_open(void) { cc_to(1, 0); }

void ui_cc_init(void)
{
    lv_obj_t *g = bz_ui_glass();
    C.scrim = lv_obj_create(g);
    lv_obj_remove_style_all(C.scrim);
    lv_obj_set_size(C.scrim, W, H);
    lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
    bz_on_tap(C.scrim, scrim_tap, NULL);
    bz_drag_t d = { .begin = st_begin, .move = st_move, .end = st_end, .axis = 2, .slop = 8 };
    bz_drag_attach(C.scrim, &d);

    /* link */
    /* 26 px between modules, as Bezel's: apart by more than the merge distance, they never melt */
    lv_obj_t *m = module(0, 130, 70, 414, BZ_LEAN ? 250 : 206, 44, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m, 8, 0);
    bz_label(m, "link", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *row = bz_row(m, 10);
    bz_icon(row, BZ_I_SMART_TOY, 24, BZ_C_INK);
    C.link_robot = bz_label(row, "", BZ_F_BODY_S, BZ_C_INK);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_WIFI, 24, BZ_C_INK);
    C.link_wifi = bz_label(row, "", BZ_F_BODY_S, BZ_C_INK);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_USB, 24, BZ_C_INK);
    C.link_usb = bz_label(row, "", BZ_F_BODY_S, BZ_C_INK);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_BATTERY_5_BAR, 24, BZ_C_INK);
    C.link_batt = bz_label(row, "", BZ_F_BODY_S, BZ_C_INK);

    /* brightness and volume: Bezel's levels, fills crisp inside the glass */
    m = module(1, 570, 70, 580, 84, 42, 0.06f);
    lv_obj_set_style_pad_all(m, 14, 0);
    row = bz_row(m, 14);
    bz_icon(row, BZ_I_LIGHT_MODE, 24, BZ_C_INK);
    C.bright = bz_level(row, 580 - 28 - 38, 56, 0.05f, 1, 0.01f);
    bz_level_on_change(C.bright, level_cb, (void *)(intptr_t)0);
    bz_level_set(C.bright, S.brightness, false);
    m = module(2, 570, 180, 580, 84, 42, 0.12f);
    lv_obj_set_style_pad_all(m, 14, 0);
    row = bz_row(m, 14);
    bz_icon(row, BZ_I_VOLUME_UP, 24, BZ_C_INK);
    C.vol = bz_level(row, 580 - 28 - 38, 56, 0, 1, 0.01f);
    bz_level_on_change(C.vol, level_cb, (void *)(intptr_t)1);
    bz_level_set(C.vol, S.volume, false);

    /* toggles: light tone, calm, sleep the panel, power */
    static const char *const icons[4] = { BZ_I_LIGHT_MODE, BZ_LEAN ? BZ_I_ROTATE_RIGHT : BZ_I_MOTION_PHOTOS_ON,
                                          BZ_I_DARK_MODE, BZ_I_POWER };
    for (int i = 0; i < 4; i++) {
        m = module(3 + i, 570 + i * 150, 290, 120, 120, 60, 0.16f + 0.04f * i);
        lv_obj_set_style_pad_all(m, 0, 0);
        C.tog_fill[i] = lv_obj_create(m);
        lv_obj_remove_style_all(C.tog_fill[i]);
        lv_obj_add_style(C.tog_fill[i], bz_style_fill(BZ_C_ICE), 0);
        lv_obj_set_style_radius(C.tog_fill[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(C.tog_fill[i], 104, 104);
        lv_obj_center(C.tog_fill[i]);
        lv_obj_remove_flag(C.tog_fill[i], LV_OBJ_FLAG_CLICKABLE);
        C.tog_icon[i] = bz_icon(m, icons[i], 40, BZ_C_INK);
        lv_obj_center(C.tog_icon[i]);
        bz_on_tap(m, toggle_tap, (void *)(intptr_t)i);
    }

    /* the pull strip along the top edge (a 30 px band, as the specimen) */
    C.strip = lv_obj_create(g);
    lv_obj_remove_style_all(C.strip);
    lv_obj_set_size(C.strip, W, 16);
    lv_obj_set_pos(C.strip, 0, 0);
    lv_obj_set_ext_click_area(C.strip, 6);
    bz_drag_attach(C.strip, &d);

    bz_motion_init(&C.p, 0, 0.001f);
    bz_motion_init(&C.boot, bz_ui_calm() ? 0 : 1, 0.001f);
    for (int i = 0; i < NMOD; i++) C.last_y[i] = C.last_opa[i] = -1;
    bz_ui_on_frame(cc_frame, NULL);
    ui_on_refresh(cc_refresh, NULL);
}
