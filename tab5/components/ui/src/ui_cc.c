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
#include <stdlib.h>

#define NMOD 7
#define NOTE_ROWS 4 /* notifications listed under the modules */

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
    /* the system edge gesture: a press starting along the top edge and moving down pulls the sheet,
     * whatever it began on */
    bool edge;
    int edge_y0;
    float edge_vy, edge_last_y;
    double edge_last_t;
    /* the sheet: the platform slides a picture of it over the page, nothing drawn per frame
     * (bz_ui_sheet_*); `open` is where it rests, `sheet` a pull or settle under way, `sheet_mode` once
     * the platform has shown it can (the older per-module cascade is the fallback) */
    bool sheet, open, sheet_mode;
    bool want_open;        /* an orb tap came while another sheet slid: open when it's done */
    double want_at;
    lv_obj_t *panel;
    /* the notifications, under the modules */
    lv_obj_t *notes, *note_row[NOTE_ROWS], *note_icon[NOTE_ROWS], *note_text[NOTE_ROWS], *note_age[NOTE_ROWS],
        *notes_empty, *notes_clear;
    unsigned notes_gen;
} C;

#define CC_SH H       /* the sheet covers the screen: the modules, the notifications and a handle */
#define EDGE_H 64     /* the top band a pull may start in: the status bar and a finger's width */
#define EDGE_SLOP 10  /* px down, more down than sideways, before it's a pull */

static void cc_refresh(void *user);

static void cc_to(float target, float v)
{
    bz_motion_to_v(&C.p, target, BZ_RELEASE, v);
    bz_ui_keep_alive();
}

/* the pull's span: with the sheet its edge follows the finger (the full height); the cascade, 380 px */
static float span(void) { return C.sheet ? (float)CC_SH : 380.0f; }

/* The sheet as it rests: the modules in place, over solid ground (a sheet that slides can't be seen
 * through), or all of it out of the way. Only what changes is touched: each set redraws. */
static void cc_show(bool on)
{
    if (on) {
        /* the scrim only catches a tap or a pull outside; the sheet's own ground is the panel */
        lv_obj_set_style_bg_opa(C.scrim, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(C.notes, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(C.notes, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < NMOD; i++) {
        if (on) {
            if (C.last_y[i] != C.base_y[i]) lv_obj_set_y(C.mods[i], C.base_y[i]);
            if (C.last_opa[i] != 255) lv_obj_set_style_opa(C.mods[i], LV_OPA_COVER, 0);
            bz_glass_set_strength(C.glass[i], 1);
            lv_obj_remove_flag(C.mods[i], LV_OBJ_FLAG_HIDDEN);
            C.last_y[i] = C.base_y[i];
            C.last_opa[i] = 255;
        } else {
            lv_obj_add_flag(C.mods[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    C.shown = on;
    if (on) {
        /* settings, an alarm or home mode may have moved them since: the levels as they are now */
        bz_level_set(C.bright, S.brightness, false);
        bz_level_set(C.vol, S.volume, false);
        C.notes_gen = ~0u; /* the ages ("now", "4 min") are read again */
        cc_refresh(NULL);
    }
}

/* drawing the sheet's other picture, a band at a time: opening, the sheet itself; closing, the page */
static void cc_prep(bool before, void *u)
{
    bool opening = (intptr_t)u != 0;
    cc_show(before ? opening : C.open);
}

static void sheet_start(bool opening)
{
    if (C.sheet) return;
    if (bz_ui_sheet_begin(opening, CC_SH, false, cc_prep, (void *)(intptr_t)opening)) C.sheet = C.sheet_mode = true;
}

static void cc_close(void)
{
    sheet_start(false);
    cc_to(0, 0);
}

static void st_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
    sheet_start(!C.open);
    C.dragging = true;
    C.p0 = C.p.value;
    bz_motion_set(&C.p, C.p.value, 0);
}

static void st_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
    float p = C.p0 + dy / span();
    p = bz_rubber_clamp(p, 0, 1, 0.12f * 2);
    bz_motion_set(&C.p, p, vy / span());
}

static void st_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx; (void)u;
    C.dragging = false;
    float v = vy / span();
    float aim = C.p.value + bz_project(v * 1000, BZ_RATE_FAST) / 1000;
    cc_to(aim > 0.5f ? 1 : 0, C.p.value > 1 ? 0 : v);
}

static void scrim_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cc_close();
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
        cc_close();
        ui_sleep_now();
        break;
    case 3: ui_island_say(BZ_I_POWER, "hold the power button to turn off"); break;
    }
    ui_settings_save();
    hal_tone(1600, 10, S.volume * 0.4f);
}

static void notes_clear_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_notify_clear();
    cc_refresh(NULL);
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
    int x0, y0, x, y;
    bool down = bz_ui_press(&x0, &y0, &x, &y);
    /* opening: a pull down from the top edge; closing: a push up anywhere on the open sheet, whatever it
     * began on (a module, a toggle) */
    bool pull = !C.open && C.p.target <= 0 && y0 <= EDGE_H && y - y0 > EDGE_SLOP && y - y0 > abs(x - x0);
    bool push = C.open && C.p.target >= 1 && y0 <= CC_SH && y0 - y > EDGE_SLOP && y0 - y > abs(x - x0);
    if (!C.edge && down && !C.dragging && !bz_drag_active() && (pull || push) && !ui_alarm_up()) {
        C.edge = true;
        C.edge_y0 = y0;
        C.edge_vy = 0;
        C.edge_last_y = (float)y;
        C.edge_last_t = now;
        bz_ui_take_press();
        st_begin(NULL, (lv_point_t){ x, y }, NULL);
    }
    if (C.edge) {
        if (down) {
            double et = now - C.edge_last_t;
            if (et > 0.001) {
                float v = (float)((y - C.edge_last_y) / et);
                C.edge_vy += (v - C.edge_vy) * 0.5f; /* a light smoothing: touch reports are uneven */
                C.edge_last_y = (float)y;
                C.edge_last_t = now;
            }
            st_move(NULL, 0, y - C.edge_y0, 0, C.edge_vy, NULL);
        } else {
            C.edge = false;
            st_end(NULL, 0, 0, 0, C.edge_vy, NULL);
        }
    }
    bool moving = bz_motion_tick(&C.p);
    if (moving || C.dragging) bz_ui_keep_alive();
    if (C.sheet) {
        float k = C.p.value < 0 ? 0 : C.p.value > 1 ? 1 : C.p.value;
        if (moving || C.dragging) {
            bz_ui_sheet((int)(k * CC_SH + 0.5f));
            return;
        }
        /* at rest: the resting height on the glass first, then the real thing takes over from the
         * picture, drawn once and not sent (the glass already shows it) */
        int rest = C.p.target > 0.5f ? CC_SH : 0;
        if (bz_ui_sheet_shown() != rest) {
            bz_ui_sheet(rest);
            bz_ui_keep_alive();
            return;
        }
        C.open = rest > 0;
        cc_show(C.open);
        bz_ui_sheet_end();
        C.sheet = false;
        return;
    }
    if (C.sheet_mode) {
        /* resting: nothing moves, cc_show set it all. A pull or an orb tap whose sheet was refused (a page or an
         * app was sliding) left the pull's target at open with nothing shown, and the edge pull (which needs the
         * target at closed) dead from then on: the rest state is put back, and a tap is tried again once the
         * other sheet is done. */
        float rest = C.open ? 1.0f : 0.0f;
        if (!C.edge && (C.p.target != rest || C.p.value != rest)) bz_motion_set(&C.p, rest, 0);
        if (C.want_open && (C.open || hal_seconds() - C.want_at > 2.0)) C.want_open = false; /* opened, or stale */
        if (C.want_open && !bz_ui_sheeting()) {
            C.want_open = false;
            ui_cc_open();
        }
        return;
    }
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
            bz_level_set(C.bright, S.brightness, false);
            bz_level_set(C.vol, S.volume, false);
            C.notes_gen = ~0u;
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
    if (C.notes_gen != ui_notify_gen()) {
        C.notes_gen = ui_notify_gen();
        int n = ui_notify_count();
        for (int i = 0; i < NOTE_ROWS; i++) {
            const ui_note_t *nt = ui_notify_get(i);
            if (!nt) {
                lv_obj_add_flag(C.note_row[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            char age[16];
            ui_notify_age(nt, age, sizeof age);
            lv_label_set_text(C.note_icon[i], nt->icon);
            ui_text(C.note_text[i], "%s", nt->text);
            ui_text(C.note_age[i], "%s", age);
            lv_obj_remove_flag(C.note_row[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (n) lv_obj_add_flag(C.notes_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(C.notes_empty, LV_OBJ_FLAG_HIDDEN);
        if (n) lv_obj_remove_flag(C.notes_clear, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(C.notes_clear, LV_OBJ_FLAG_HIDDEN);
    }
    int bits = on[0] | on[1] << 1 | 16;
    (void)0;
    if (bits == C.last_tog) return;
    C.last_tog = bits;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_opa(C.tog_fill[i], on[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        bz_set_color(C.tog_icon[i], on[i] ? BZ_C_ON_ICE : BZ_C_INK);
    }
}

bool ui_cc_is_open(void) { return C.open || C.sheet; }

void ui_cc_open(void)
{
    if (C.open || C.sheet) return;
    sheet_start(true);
    if (C.sheet_mode && !C.sheet) {
        C.want_open = true; /* another sheet is sliding: open once it's done (cc_frame) */
        C.want_at = hal_seconds();
        return;
    }
    cc_to(1, 0);
}

void ui_cc_init(void)
{
    lv_obj_t *g = bz_ui_glass();
    /* known from the platform, not learned from the first sheet that worked: a first open refused (an orb tap
     * while an app was still sliding in) fell back to the old cascade for good, the app showing through it and
     * its buttons drawn over the modules */
    C.sheet_mode = bz_ui_can_sheet();
    C.scrim = lv_obj_create(g);
    lv_obj_remove_style_all(C.scrim);
    lv_obj_set_size(C.scrim, W, H);
    lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
    bz_on_tap(C.scrim, scrim_tap, NULL);
    bz_drag_t d = { .begin = st_begin, .move = st_move, .end = st_end, .axis = 2, .slop = 8 };
    bz_drag_attach(C.scrim, &d);
    /* the sheet's ground, with a handle at its foot to pull it back up by */
    C.panel = lv_obj_create(g);
    lv_obj_remove_style_all(C.panel);
    lv_obj_set_size(C.panel, W, CC_SH + 40);
    lv_obj_set_pos(C.panel, 0, -40); /* its top corners above the screen: only the foot is rounded */
    lv_obj_add_style(C.panel, bz_style_fill(BZ_C_GROUND), 0); /* the modules and tiles stand on it */
    lv_obj_set_style_radius(C.panel, 28, 0);
    lv_obj_remove_flag(C.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(C.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *handle = lv_obj_create(C.panel);
    lv_obj_remove_style_all(handle);
    lv_obj_add_style(handle, bz_style_fill(BZ_C_FAINT), 0);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(handle, 96, 8);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* link */
    /* 26 px between modules, as Bezel's: apart by more than the merge distance, they never melt */
    lv_obj_t *m = module(0, 130, 70, 414, BZ_LEAN ? 250 : 206, 44, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m, 8, 0);
    bz_label(m, "link", BZ_F_LABEL, BZ_C_DIM);
    /* each line inside the module, a long network name or address ending in "…" rather than past its edge */
    const int lw = 414 - 2 * 22 - 24 - 10;
    lv_obj_t *row = bz_row(m, 10);
    bz_icon(row, BZ_I_SMART_TOY, 24, BZ_C_INK);
    C.link_robot = bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, lw);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_WIFI, 24, BZ_C_INK);
    C.link_wifi = bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, lw);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_USB, 24, BZ_C_INK);
    C.link_usb = bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, lw);
    row = bz_row(m, 10);
    bz_icon(row, BZ_I_BATTERY_5_BAR, 24, BZ_C_INK);
    C.link_batt = bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, lw);

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

    /* notifications: what the island said, newest first */
    C.notes = bz_tile(g, 1020, 244);
    lv_obj_set_pos(C.notes, 130, 426);
    lv_obj_set_style_radius(C.notes, 36, 0);
    lv_obj_remove_flag(C.notes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(C.notes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *nh = bz_label(C.notes, "notifications", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(nh, 0, 0);
    C.notes_clear = ui_button(C.notes, BZ_I_CLOSE, "clear", notes_clear_tap, NULL);
    lv_obj_align(C.notes_clear, LV_ALIGN_TOP_RIGHT, 0, -12);
    C.notes_empty = bz_label(C.notes, "Nothing yet. What the island tells you stays here.", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_pos(C.notes_empty, 0, 44);
    int iw = 1020 - 2 * BZ_PAD_TILE;
    for (int i = 0; i < NOTE_ROWS; i++) {
        lv_obj_t *r = bz_row(C.notes, 14);
        lv_obj_set_width(r, iw);
        lv_obj_set_pos(r, 0, 48 + i * 40);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        C.note_icon[i] = bz_icon(r, BZ_I_INFO, 24, BZ_C_DIM);
        /* the words take what the icon and the age leave: fixed widths added up past the row and pushed the
         * age ("4 min") out of the card */
        C.note_text[i] = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, 100);
        lv_obj_set_flex_grow(C.note_text[i], 1);
        C.note_age[i] = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, 90);
        lv_obj_set_style_text_align(C.note_age[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        C.note_row[i] = r;
    }
    C.notes_gen = ~0u;

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
