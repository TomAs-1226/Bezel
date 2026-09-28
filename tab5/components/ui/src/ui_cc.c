/* The control center: a sheet of glass modules pulled down from the top edge (Bezel's cc.js).
 *
 * The pull tracks the finger 1:1 (progress = dy / 380), rubber-bands past fully open, and on release
 * projects its velocity to decide open or closed. Modules cascade in by delay, each materializing by
 * strength, while a frosted, dimmed picture of the page comes down behind them like a blind, its soft
 * edge on the pull (the web specimen frosts the whole page by the pull's amount; on the P4 a
 * cross-fade of every pixel each frame costs more memory traffic than a frame has, while the blind
 * redraws only the rows its edge crosses — see docs/bezel-port.md). Its fills — the brightness and
 * volume levels, a toggle's "on" — are drawn crisp inside the glass, never frosted by it.
 *
 * What is on the sheet is a registry (ui_cc_control_t, ui_internal.h): nothing below knows what any
 * control does. The grid takes eight columns across the landscape frame and four across phone mode's, so
 * the same controls reflow into either rather than needing a second layout. */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NMOD 24     /* controls the sheet can hold */
#define NOTE_ROWS 6 /* notification rows the tile can hold; it shows as many as its height allows */

/* ---- the grid ---- */
#define CC_GAP 30  /* between modules: well past the 16 px at which glass melts, so they never do */
#define CC_TOP 70  /* the first row, under the status band */
#define CC_BOT 30  /* what the last row leaves at the foot, for the handle */
#define CC_ROWS 16

typedef struct {
    int r, c, rs, cs;      /* its cells */
    int x, y, w, h;        /* where it landed */
} slot_t;

static struct {
    lv_obj_t *strip, *scrim;
    const ui_cc_control_t *ctl[NMOD];
    lv_obj_t *mods[NMOD];
    bz_glass_t *glass[NMOD];   /* NULL for a solid tile */
    lv_obj_t *fill[NMOD], *tico[NMOD], *tlab[NMOD], *level[NMOD];
    bool last_on[NMOD];
    slot_t slot[NMOD];
    int nctl;
    int base_y[NMOD];
    float delay[NMOD];
    bz_motion_t p;
    float p0;
    bool dragging;
    lv_obj_t *link_robot, *link_wifi, *link_usb, *link_batt;
    int last_y[NMOD], last_opa[NMOD];
    bool asleep, shown, frozen;
    bz_motion_t boot;      /* the power-on blind: starts down, lifts off the page */
    double boot_at;
    double restart_at;     /* phone mode was switched: restart when the message has been read */
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
    /* the notifications, in whichever control asked to grow */
    lv_obj_t *notes, *note_row[NOTE_ROWS], *note_icon[NOTE_ROWS], *note_text[NOTE_ROWS], *note_age[NOTE_ROWS],
        *notes_empty, *notes_clear;
    int note_shown;
    unsigned notes_gen;
} C;

/* what ui_cc_add() collected before the sheet was built */
static const ui_cc_control_t *g_added[NMOD];
static int g_nadded;

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
    } else {
        lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < C.nctl; i++) {
        if (on) {
            if (C.last_y[i] != C.base_y[i]) lv_obj_set_y(C.mods[i], C.base_y[i]);
            if (C.last_opa[i] != 255) lv_obj_set_style_opa(C.mods[i], LV_OPA_COVER, 0);
            if (C.glass[i]) bz_glass_set_strength(C.glass[i], 1);
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
        for (int i = 0; i < C.nctl; i++)
            if (C.level[i] && C.ctl[i]->get) bz_level_set(C.level[i], C.ctl[i]->get(), false);
        C.notes_gen = ~0u; /* the ages ("now", "4 min") are read again */
        cc_refresh(NULL);
    }
}

static void cc_show_quiet(void *u) { cc_show((intptr_t)u != 0); }

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

void ui_cc_close(void)
{
    sheet_start(false);
    cc_to(0, 0);
}

static void (*g_after)(void); /* run once the sheet is away (ui_cc_after_close) */

void ui_cc_after_close(void (*fn)(void))
{
    g_after = fn;
    ui_cc_close();
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
    ui_cc_close();
}

/* ---- the controls the sheet carries ---- */

void ui_cc_add(const ui_cc_control_t *c)
{
    if (g_nadded < NMOD && c) g_added[g_nadded++] = c;
}

/* link: where the tablet reaches the robot, the network, the PC and its own battery. */
static void link_build(lv_obj_t *m, int w, int h)
{
    (void)h;
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m, 8, 0);
    bz_label(m, "link", BZ_F_LABEL, BZ_C_DIM);
    /* each line inside the module, a long network name or address ending in "…" rather than past its edge */
    const int lw = w - 2 * 22 - 24 - 10;
    static const char *const ICONS[4] = { BZ_I_SMART_TOY, BZ_I_WIFI, BZ_I_USB, BZ_I_BATTERY_5_BAR };
    lv_obj_t **out[4] = { &C.link_robot, &C.link_wifi, &C.link_usb, &C.link_batt };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = bz_row(m, 10);
        bz_icon(row, ICONS[i], 24, BZ_C_INK);
        *out[i] = bz_label_line(row, "", BZ_F_BODY_S, BZ_C_INK, lw);
    }
}

static void link_refresh(lv_obj_t *m)
{
    (void)m;
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
}

static const ui_cc_control_t CTL_LINK = {
    .name = "link", .icon = BZ_I_LAN, .kind = UI_CC_PANEL, .cols = 3, .rows = 2,
    .build = link_build, .refresh = link_refresh,
};

/* brightness and volume: Bezel's levels, fills crisp inside the glass */
static float bright_get(void) { return S.brightness; }
static void bright_set(float v, bool final)
{
    S.brightness = v;
    hal_set_brightness(v);
    if (final) ui_settings_save();
}
static const ui_cc_control_t CTL_BRIGHT = {
    .name = "light", .icon = BZ_I_LIGHT_MODE, .kind = UI_CC_LEVEL, .cols = 4, .rows = 1, .h = 84,
    .get = bright_get, .set = bright_set,
};

static float vol_get(void) { return S.volume; }
static void vol_set(float v, bool final)
{
    S.volume = v;
    hal_set_volume(v);
    if (final) ui_settings_save();
}
static const ui_cc_control_t CTL_VOL = {
    .name = "volume", .icon = BZ_I_VOLUME_UP, .kind = UI_CC_LEVEL, .cols = 4, .rows = 1, .h = 84,
    .get = vol_get, .set = vol_set,
};

/* the light tone */
static bool tone_on(void) { return !S.dark; }
static void tone_tap(void) { ui_set_tone(!S.dark, S.calm); }
static const ui_cc_control_t CTL_TONE = {
    .name = "light", .icon = BZ_I_LIGHT_MODE, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .on = tone_on, .tap = tone_tap,
};

/* calm: solid glass and motion without overshoot. The tablet's renderer has no glass pass to skip and
 * starts calm, so the choice is only offered where it means something. */
static bool calm_avail(void) { return !BZ_LEAN; }
static bool calm_on(void) { return S.calm; }
static void calm_tap(void) { ui_set_tone(S.dark, !S.calm); }
static const ui_cc_control_t CTL_CALM = {
    .name = "calm", .icon = BZ_I_MOTION_PHOTOS_ON, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .avail = calm_avail, .on = calm_on, .tap = calm_tap,
};

/* Rotation lock: auto-rotate turns the picture over when the tablet is held the other way up. It is
 * independent of phone mode, which chooses the frame rather than which way up it is held. */
static bool rot_on(void) { return !S.auto_rotate; }
static void rot_tap(void)
{
    S.auto_rotate = !S.auto_rotate;
    ui_settings_save();
}
static const ui_cc_control_t CTL_ROTATE = {
    .name = "rotate", .icon = BZ_I_ROTATE_RIGHT, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .on = rot_on, .tap = rot_tap,
};

/* Phone mode: every screen laid out in the panel's own portrait frame. Each screen is built once, at
 * start-up, from W and H, so the choice lands on the next start — and the tablet restarts for it, having
 * said so. It never touches auto-rotate: that is which way up, this is which way round. */
static bool phone_on(void) { return hal_phone(); }

void ui_phone_set(bool on)
{
    if (on == hal_phone()) return;
    hal_phone_next(on);
    ui_island_say(BZ_I_DISPLAY_SETTINGS, on ? "phone mode · restarting" : "landscape · restarting");
    C.restart_at = ui_now() + 1.2; /* long enough for the island to be read */
}

static void phone_tap(void)
{
    ui_phone_set(!hal_phone());
    ui_cc_close();
}
static const ui_cc_control_t CTL_PHONE = {
    .name = "phone", .icon = BZ_I_DISPLAY_SETTINGS, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .on = phone_on, .tap = phone_tap,
};

/* the panel goes dark until the next touch; the robot link stays up */
static bool never_on(void) { return false; }
static void sleep_tap(void)
{
    ui_cc_close();
    ui_sleep_now();
}
static const ui_cc_control_t CTL_SLEEP = {
    .name = "sleep", .icon = BZ_I_DARK_MODE, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .on = never_on, .tap = sleep_tap,
};

static void power_tap(void) { ui_island_say(BZ_I_POWER, "hold the power button to turn off"); }
static const ui_cc_control_t CTL_POWER = {
    .name = "power", .icon = BZ_I_POWER, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
    .on = never_on, .tap = power_tap,
};

/* notifications: what the island said, newest first. The one control that grows: it takes whatever height
 * the rows above it left, and shows as many lines as that holds. */
static void notes_clear_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_notify_clear();
    cc_refresh(NULL);
}

static void notes_build(lv_obj_t *m, int w, int h)
{
    C.notes = m;
    lv_obj_set_style_radius(m, 36, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nh = bz_label(m, "notifications", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(nh, 0, 0);
    C.notes_clear = ui_button(m, BZ_I_CLOSE, "clear", notes_clear_tap, NULL);
    lv_obj_align(C.notes_clear, LV_ALIGN_TOP_RIGHT, 0, -12);
    /* nothing yet: a bell in a disc and a line, centred in the tile, rather than a sentence in its corner */
    int iw = w - 2 * BZ_PAD_TILE;
    C.notes_empty = bz_col(m, 10);
    lv_obj_set_size(C.notes_empty, iw, h - 2 * BZ_PAD_TILE);
    lv_obj_set_flex_align(C.notes_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *disc = bz_box(C.notes_empty);
    lv_obj_set_size(disc, 72, 72);
    lv_obj_add_style(disc, bz_style_fill(BZ_C_SURFACE2), 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(bz_icon(disc, BZ_I_NOTIFICATIONS, 32, BZ_C_DIM));
    bz_label(C.notes_empty, "no notifications", BZ_F_BODY, BZ_C_INK);
    bz_label(C.notes_empty, "what the island tells you stays here", BZ_F_LABEL, BZ_C_DIM);
    C.note_shown = (h - 2 * BZ_PAD_TILE - 48) / 40;
    if (C.note_shown > NOTE_ROWS) C.note_shown = NOTE_ROWS;
    if (C.note_shown < 1) C.note_shown = 1;
    for (int i = 0; i < C.note_shown; i++) {
        lv_obj_t *r = bz_row(m, 14);
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
}

static void notes_refresh(lv_obj_t *m)
{
    (void)m;
    if (C.notes_gen == ui_notify_gen()) return;
    C.notes_gen = ui_notify_gen();
    int n = ui_notify_count();
    for (int i = 0; i < C.note_shown; i++) {
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

/* the owner's favourite apps (settings > look > quick launch), from anywhere: their icons, a tap opens one once
 * the sheet is away. Rebuilt when the favourites change. */
#define CC_APPS 4
static struct {
    lv_obj_t *row;
    unsigned gen;
    const ui_app_t *pending;
} CA = { .gen = ~0u };

static void apps_open_pending(void)
{
    if (CA.pending) ui_app_launch(CA.pending, NULL);
    CA.pending = NULL;
}

static void apps_tap(lv_obj_t *o, void *u)
{
    (void)o;
    CA.pending = (const ui_app_t *)u;
    ui_cc_after_close(apps_open_pending);
}

static void apps_fill(void)
{
    lv_obj_clean(CA.row);
    const ui_look_t *l = ui_look();
    int shown = 0;
    for (int i = 0; i < l->nquick && shown < CC_APPS; i++) {
        const ui_app_t *app = NULL;
        for (int k = 0; k < ui_app_count(); k++)
            if (!strcmp(ui_app_entry(k)->label, l->quick[i])) app = ui_app_entry(k)->app;
        if (!app) continue;
        lv_obj_t *c = bz_box(CA.row);
        lv_obj_set_size(c, 76, 76);
        lv_obj_set_style_radius(c, 22, 0);
        lv_obj_add_style(c, bz_style_fill(BZ_C_SURFACE3), LV_STATE_PRESSED);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = ui_app_icon(c, app, 64);
        lv_obj_center(ic);
        bz_on_tap(c, apps_tap, (void *)app);
        shown++;
    }
    CA.gen = l->quick_gen;
}

static void apps_build(lv_obj_t *m, int w, int h)
{
    (void)w; (void)h;
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_radius(m, 36, 0);
    CA.row = bz_row(m, 6);
    lv_obj_set_size(CA.row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_align(CA.row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    apps_fill();
}

static void apps_refresh(lv_obj_t *m)
{
    (void)m;
    if (CA.gen != ui_look()->quick_gen) apps_fill();
}

static const ui_cc_control_t CTL_APPS = {
    .name = "apps", .icon = BZ_I_APPS, .kind = UI_CC_TILE, .cols = 3, .rows = 1,
    .build = apps_build, .refresh = apps_refresh,
};

static const ui_cc_control_t CTL_NOTES = {
    .name = "notifications", .icon = BZ_I_NOTIFICATIONS, .kind = UI_CC_TILE, .cols = 99, .rows = 2,
    .grow = true, .build = notes_build, .refresh = notes_refresh,
};

/* ---- laying the grid out ---- */

static void cc_place(void)
{
    int cols = PORTRAIT ? 4 : 8;
    /* the margin the notifications tile has always had on the landscape sheet; the page's own in portrait,
     * where 720 px has none to spare */
    int margin = PORTRAIT ? PAD : 130;
    int cw = (W - 2 * margin - (cols - 1) * CC_GAP) / cols;
    unsigned occ[CC_ROWS] = { 0 };
    int nrows = 0, grow = -1;
    for (int i = 0; i < C.nctl; i++) {
        const ui_cc_control_t *t = C.ctl[i];
        int cs = t->cols < 1 ? 1 : t->cols > cols ? cols : t->cols;
        int rs = t->rows < 1 ? 1 : t->rows;
        int r = 0, c = 0;
        for (r = 0; r + rs <= CC_ROWS; r++) {
            for (c = 0; c + cs <= cols; c++) {
                unsigned mask = (unsigned)(((1u << cs) - 1u) << c);
                bool free_ = true;
                for (int k = 0; k < rs && free_; k++) free_ = (occ[r + k] & mask) == 0;
                if (free_) goto placed;
            }
        }
        r = CC_ROWS - rs; /* the sheet is full: it lands on the last row it can, and the caller sees it */
        c = 0;
    placed:
        for (int k = 0; k < rs; k++) occ[r + k] |= (unsigned)(((1u << cs) - 1u) << c);
        C.slot[i] = (slot_t){ r, c, rs, cs, 0, 0, 0, 0 };
        if (r + rs > nrows) nrows = r + rs;
        if (t->grow) grow = i;
    }
    /* a row is as tall as the tallest control that takes exactly one of it; a row only passed through by
     * taller ones is a cell square */
    int rh[CC_ROWS];
    for (int r = 0; r < nrows; r++) rh[r] = 0;
    for (int i = 0; i < C.nctl; i++) {
        if (C.slot[i].rs != 1) continue;
        int h = C.ctl[i]->h ? C.ctl[i]->h : cw;
        if (h > rh[C.slot[i].r]) rh[C.slot[i].r] = h;
    }
    for (int r = 0; r < nrows; r++)
        if (!rh[r]) rh[r] = cw;
    /* It has to fit the frame: the rows that simply took the cell's size give the difference back, rather
     * than the last row running off the bottom. A row sized by a control that asked for a height keeps it —
     * a level shortened by five pixels is a level with its fill clipped. */
    int avail = H - CC_TOP - CC_BOT, total = (nrows - 1) * CC_GAP;
    for (int r = 0; r < nrows; r++) total += rh[r];
    if (total > avail) {
        int asked = 0, square = 0;
        for (int r = 0; r < nrows; r++) {
            if (rh[r] == cw) square += rh[r];
            else asked += rh[r];
        }
        int room = avail - (nrows - 1) * CC_GAP - asked;
        if (room < 40 * nrows) room = 40 * nrows; /* past this the sheet is simply too full: it overflows */
        for (int r = 0; r < nrows; r++)
            if (rh[r] == cw) rh[r] = rh[r] * room / (square > 0 ? square : 1);
    }
    int y[CC_ROWS];
    int at = CC_TOP;
    for (int r = 0; r < nrows; r++) {
        y[r] = at;
        at += rh[r] + CC_GAP;
    }
    for (int i = 0; i < C.nctl; i++) {
        slot_t *s = &C.slot[i];
        s->x = margin + s->c * (cw + CC_GAP);
        s->w = s->cs * cw + (s->cs - 1) * CC_GAP;
        int spanh = (s->rs - 1) * CC_GAP;
        for (int k = 0; k < s->rs; k++) spanh += rh[s->r + k];
        if (i == grow) {
            s->y = y[s->r];
            s->h = H - CC_BOT - s->y;
        } else if (C.ctl[i]->h && s->rs == 1 && C.ctl[i]->h < spanh) {
            s->h = C.ctl[i]->h;
            s->y = y[s->r] + (spanh - s->h) / 2;
        } else {
            s->h = spanh;
            s->y = y[s->r];
        }
    }
}

/* ---- building them ---- */

static void toggle_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= C.nctl || !C.ctl[i]->tap) return;
    hal_tone(1600, 10, S.volume * 0.4f);
    C.ctl[i]->tap();
    cc_refresh(NULL);
}

static void level_cb(lv_obj_t *lv, float v, bool final, void *u)
{
    (void)lv;
    int i = (int)(intptr_t)u;
    if (i >= 0 && i < C.nctl && C.ctl[i]->set) C.ctl[i]->set(v, final);
}

static void build_control(int i)
{
    const ui_cc_control_t *t = C.ctl[i];
    const slot_t *s = &C.slot[i];
    bool glass = t->kind != UI_CC_TILE;
    int side = s->w < s->h ? s->w : s->h;
    lv_obj_t *m;
    if (glass) {
        m = lv_obj_create(bz_ui_glass());
        lv_obj_remove_style_all(m);
        lv_obj_set_style_pad_all(m, 22, 0);
        /* staggered, they pass near each other on the way in: they must not melt */
        C.glass[i] = bz_glass_attach(m, 6, t->kind == UI_CC_TOGGLE ? side / 2.0f : 44);
        bz_glass_set_solo(C.glass[i], true);
        bz_glass_set_strength(C.glass[i], 0);
    } else {
        m = bz_tile(bz_ui_glass(), s->w, s->h);
    }
    lv_obj_set_size(m, s->w, s->h);
    lv_obj_set_pos(m, s->x, s->y);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    C.mods[i] = m;
    C.base_y[i] = s->y;

    switch (t->kind) {
    case UI_CC_TOGGLE: {
        lv_obj_set_style_pad_all(m, 0, 0);
        lv_obj_set_style_radius(m, side / 2, 0);
        C.fill[i] = lv_obj_create(m);
        lv_obj_remove_style_all(C.fill[i]);
        lv_obj_add_style(C.fill[i], bz_style_fill(BZ_C_ICE), 0);
        lv_obj_set_style_radius(C.fill[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(C.fill[i], side - 16, side - 16);
        lv_obj_set_style_bg_opa(C.fill[i], LV_OPA_TRANSP, 0);
        lv_obj_center(C.fill[i]);
        lv_obj_remove_flag(C.fill[i], LV_OBJ_FLAG_CLICKABLE);
        /* the icon over the control's own one word: eight of these fit the sheet now, and an icon alone
         * stopped saying which was which */
        lv_obj_t *col = bz_col(m, 2);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_center(col);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
        C.tico[i] = bz_icon(col, t->icon, side >= 128 ? 40 : 32, BZ_C_INK);
        C.tlab[i] = bz_label(col, t->name, BZ_F_CAPTION, BZ_C_INK);
        bz_on_tap(m, toggle_tap, (void *)(intptr_t)i);
        break;
    }
    case UI_CC_LEVEL: {
        lv_obj_set_style_pad_all(m, 14, 0);
        lv_obj_t *row = bz_row(m, 14);
        bz_icon(row, t->icon, 24, BZ_C_INK);
        C.level[i] = bz_level(row, s->w - 28 - 38, 56, 0.05f, 1, 0.01f);
        bz_level_on_change(C.level[i], level_cb, (void *)(intptr_t)i);
        if (t->get) bz_level_set(C.level[i], t->get(), false);
        break;
    }
    case UI_CC_PANEL:
    case UI_CC_TILE:
        if (t->build) t->build(m, s->w, s->h);
        break;
    }
}

/* ---- the pull ---- */

static void cc_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    /* phone mode was switched: the island has had its moment, the next start builds the other frame */
    if (C.restart_at && now >= C.restart_at) {
        C.restart_at = 0;
        hal_restart_planned("phone mode changed");
    }
    /* what a control asked for once the sheet was out of the way */
    if (g_after && !C.open && !C.sheet && !C.shown && !C.dragging) {
        void (*fn)(void) = g_after;
        g_after = NULL;
        fn();
    }
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
        /* The glass already shows this, drawn band by band into the sheet's picture (cc_prep did the same
         * cc_show before each band). Showing it for real invalidated the whole screen, and LVGL redrew all
         * 1280x720 (measured 84 ms) and the PPA turned all of it, right as the sheet came to rest: the one
         * hitch left in opening or closing the control center. It is moved in quietly instead, the way a
         * page slide lands (place_track), and the refresh after it redraws any number that has moved on
         * since the bands were drawn. */
        bz_ui_quiet(cc_show_quiet, (void *)(intptr_t)C.open);
        if (C.open) {
            C.notes_gen = ~0u;
            cc_refresh(NULL);
        }
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
    /* the cascade's rest state, as the sheet branch keeps it: ui_cc_is_open() and with it ui_overlay_up()
     * read this, and so the assistant's orb steps aside and the push-up-to-close gesture arms */
    C.open = C.p.target > 0.5f;
    /* power-on: the page starts under the frosted blind and it lifts, a beat after the first frame */
    if (C.boot_at == 0) C.boot_at = now + 0.35;
    if (now >= C.boot_at && C.boot.target > 0) bz_motion_to(&C.boot, 0, BZ_SMOOTH);
    if (bz_motion_tick(&C.boot)) bz_ui_keep_alive();
    float blind = C.boot.value > p ? C.boot.value : p;
    /* the lean renderer has no compose pass to frost through, so the blind is the scrim's solid dim below */
    if (!BZ_LEAN) bz_comp_set_backdrop(bz_ui_comp(), blind > 1 ? 1 : blind < 0 ? 0 : blind, 0.9f);
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
            /* lean: no frosted blind — the page goes under a solid dim of the ground, set once, and the
             * sheet's own ground stands on it, which is what the platform sheet gives in landscape */
            if (BZ_LEAN) {
                lv_obj_set_style_bg_color(C.scrim, bz_lv(BZ_C_GROUND), 0);
                lv_obj_set_style_bg_opa(C.scrim, LV_OPA_80, 0);
                lv_obj_remove_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < C.nctl; i++)
                if (C.level[i] && C.ctl[i]->get) bz_level_set(C.level[i], C.ctl[i]->get(), false);
            C.notes_gen = ~0u;
            cc_refresh(NULL);
        } else {
            lv_obj_add_flag(C.scrim, LV_OBJ_FLAG_HIDDEN);
            if (BZ_LEAN) lv_obj_add_flag(C.panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < C.nctl; i++) {
        float k = (p - C.delay[i]) / (1 - C.delay[i]);
        k = k < 0 ? 0 : k;
        float kc = k > 1 ? 1 : k;
        int y = C.base_y[i] - (int)(52 * (1 - kc)) + (int)((k > 1 ? k - 1 : 0) * 60);
        if (bz_ui_calm()) y = C.base_y[i];
        if (C.glass[i]) bz_glass_set_strength(C.glass[i], kc);
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

/* Live state inside the controls, at the model's 10 Hz. */
static void cc_refresh(void *user)
{
    (void)user;
    if (!C.shown) return;
    for (int i = 0; i < C.nctl; i++) {
        const ui_cc_control_t *t = C.ctl[i];
        if (t->refresh) t->refresh(C.mods[i]);
        if (t->kind != UI_CC_TOGGLE || !t->on) continue;
        bool on = t->on();
        if (on == C.last_on[i]) continue;
        C.last_on[i] = on;
        lv_obj_set_style_bg_opa(C.fill[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        bz_set_color(C.tico[i], on ? BZ_C_ON_ICE : BZ_C_INK);
        bz_set_color(C.tlab[i], on ? BZ_C_ON_ICE : BZ_C_DIM);
    }
}

bool ui_cc_is_open(void) { return C.open || C.sheet; }

void ui_cc_open(void)
{
    if (C.open || C.sheet) return;
    ui_kb_dismiss(); /* likewise: nothing sits over the control centre except the keyboard, so it goes */
    sheet_start(true);
    if (C.sheet_mode && !C.sheet) {
        C.want_open = true; /* another sheet is sliding: open once it's done (cc_frame) */
        C.want_at = hal_seconds();
        return;
    }
    cc_to(1, 0);
}

static void add(const ui_cc_control_t *t)
{
    if (C.nctl >= NMOD) return;
    if (t->avail && !t->avail()) return;
    C.ctl[C.nctl++] = t;
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

    /* the sheet's contents: the system's own, then whatever else asked to be on it, then the three that
     * belong at the foot */
    ui_light_cc_add();
    ui_home_mode_cc_add();
    add(&CTL_LINK);
    add(&CTL_BRIGHT);
    add(&CTL_VOL);
    add(&CTL_TONE);
    add(&CTL_CALM);
    add(&CTL_ROTATE);
    add(&CTL_PHONE);
    for (int i = 0; i < g_nadded; i++) add(g_added[i]);
    add(&CTL_SLEEP);
    add(&CTL_POWER);
    add(&CTL_APPS); /* the favourites, in the cells the toggles leave on their row */
    add(&CTL_NOTES);
    cc_place();
    for (int i = 0; i < C.nctl; i++) {
        C.last_y[i] = C.last_opa[i] = -1;
        C.last_on[i] = true; /* the first refresh sets every toggle, whichever way it reads */
        C.delay[i] = C.nctl > 1 ? 0.28f * (float)i / (float)(C.nctl - 1) : 0;
        build_control(i);
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
    /* the sheet starts put away: the cascade hid the modules on its first frame, the sheet never does */
    if (C.sheet_mode) cc_show(false);
    bz_ui_on_frame(cc_frame, NULL);
    ui_on_refresh(cc_refresh, NULL);
}
