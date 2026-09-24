/* Home mode: the surface, its triggers and its settings store.
 *
 * The layout, 1280 × 720, top to bottom:
 *   the time, big, with the date under it          the weather, right-aligned
 *   what's playing (art, title, progress, controls) the companion's face
 *   a row of up to six Home Assistant tiles
 *   catalyst mode · music · settings                a line of status
 *
 * Cheap to keep on screen: nothing animates at rest. The clock changes once a minute; the progress bar and
 * its time once a second while something plays; a tile only when its entity changes; the art once a song;
 * the companion blinks every few seconds (two small eyes, a few thousand pixels). Nothing is restyled unless
 * its state changed.
 *
 * Coming and going is a bottom sheet the platform slides (bz_ui_sheet_*, as an app opens), drawn a band at a
 * time; with no platform sheet (the simulator) it just switches. */
#include "ui_home_priv.h"

#include "ui_companion.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define M 48                        /* the surface's side margin */
#define ROW2_Y 256
#define ROW2_H 232
#define NP_W 760                    /* now playing */
#define CP_X (M + NP_W + BZ_GAP)
#define CP_W (W - M - CP_X)
#define ROW3_Y (ROW2_Y + ROW2_H + BZ_GAP)
#define ROW3_H 128
#define TILES HOME_HA_PICKS
#define TILE_W ((W - 2 * M - (TILES - 1) * BZ_GAP) / TILES)
#define FOOT_Y (H - 76)
#define ART HOME_ART
#define NP_IN (NP_W - 2 * BZ_PAD_TILE)
#define NP_TX (ART + 24)            /* the text column, inside the card */
#define NP_TW (NP_IN - NP_TX)
#define EYE_W 38
#define EYE_H 62

/* ================================================================== settings store */

static hm_cfg_t *CFG;
static bool g_settings_wanted;

static bool kv_bool(const char *k, bool def)
{
    char v[8];
    return hal_kv_get(k, v, sizeof v) ? v[0] == '1' : def;
}

hm_cfg_t *hm_cfg(void)
{
    if (CFG) return CFG;
    CFG = calloc(1, sizeof *CFG); /* ~900 bytes: PSRAM, not a static in internal RAM */
    if (!CFG) abort();            /* at start-up, with 32 MB of PSRAM: never */
    CFG->start_home = kv_bool("hm_start", false);
    CFG->stand = kv_bool("hm_stand", false);
    CFG->fahrenheit = kv_bool("hm_fahr", false);
    CFG->pc = kv_bool("hm_pc", true);
    hal_kv_get("hm_place", CFG->place, sizeof CFG->place);
    hal_kv_get("ha_url", CFG->ha_url, sizeof CFG->ha_url);
    hal_kv_get("ha_token", CFG->ha_token, sizeof CFG->ha_token);
    hal_kv_get("ha_picks", CFG->picks, sizeof CFG->picks);
    return CFG;
}

void hm_cfg_apply(void)
{
    hm_cfg_t *c = hm_cfg();
    home_pc_enable(c->pc);
    home_weather_config(c->place, c->fahrenheit);
    home_ha_config(c->ha_url, c->ha_token, c->picks);
}

void hm_cfg_save(void)
{
    hm_cfg_t *c = hm_cfg();
    hal_kv_set("hm_start", c->start_home ? "1" : "0");
    hal_kv_set("hm_stand", c->stand ? "1" : "0");
    hal_kv_set("hm_fahr", c->fahrenheit ? "1" : "0");
    hal_kv_set("hm_pc", c->pc ? "1" : "0");
    hal_kv_set("hm_place", c->place);
    hal_kv_set("ha_url", c->ha_url);
    hal_kv_set("ha_token", c->ha_token);
    hal_kv_set("ha_picks", c->picks);
    hm_cfg_apply();
}

void hm_settings_request(void) { g_settings_wanted = true; }

bool ui_home_settings_wanted(void)
{
    bool w = g_settings_wanted;
    g_settings_wanted = false;
    return w;
}

void hm_fmt_time(double s, char *out, size_t n)
{
    if (s < 0) {
        snprintf(out, n, "-:--");
        return;
    }
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/* ================================================================== the surface */

typedef struct {
    lv_obj_t *tile, *icon, *name, *state;
    char id[64];
    char shown_state[40];
    int kind;
    bool on, used;
} ha_tile_t;

static struct {
    lv_obj_t *root, *greet, *clock, *date;
    lv_obj_t *wx_icon, *wx_temp, *wx_line, *wx_place;
    lv_obj_t *np, *np_img, *np_ph, *np_src, *np_title, *np_artist, *np_meter, *np_time, *np_play, *np_prev, *np_next;
    lv_obj_t *np_vdn, *np_vup;
    lv_obj_t *eye[2], *face;
    lv_obj_t *ha_hint, *ha_hint_text;
    ha_tile_t *ha;                 /* TILES, on the heap */
    lv_obj_t *status;
    lv_image_dsc_t art_dsc;
    uint16_t *art;                 /* ART² RGB565 */
    unsigned art_gen, pc_gen, wx_gen, ha_gen, pl_gen;
    int last_min, last_sec;
    bool art_shown;
    bool local;                    /* the card shows the tablet's own player (else the PC's) */
    bool playing_shown;
    /* mode */
    bool built, active, sheet;
    int want;                      /* +1 enter, -1 leave, 0 nothing pending */
    ui_home_why_t why;             /* what brought it (an automatic trigger may take it away again) */
    double left_at;                /* hal_seconds() when it was last left by hand */
    int hid_page;                  /* the page hidden under it (-1 none) */
    bz_motion_t k;
    double blink_at;
    bool blinking;
    ui_home_mode_fn on_change;
    bool power;
} HM = { .hid_page = -1 };

bool ui_home_mode_active(void) { return HM.active; }
bool ui_home_mode_keeps_awake(void) { return HM.active && HM.power; }

static void open_app(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }

static void to_catalyst(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_home_mode_exit();
}

static void to_settings(lv_obj_t *o, void *u)
{
    (void)u;
    hm_settings_request();
    ui_app_open(&APP_SETTINGS, o);
}

static void face_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_companion_open(false, o);
}

/* the now-playing card's buttons act on whichever player it shows */
static void np_cmd(lv_obj_t *o, void *u)
{
    (void)o;
    const char *a = u;
    if (HM.local) {
        if (!strcmp(a, "toggle")) home_player_toggle();
        else if (!strcmp(a, "next")) home_player_skip(1);
        else if (!strcmp(a, "previous")) home_player_skip(-1);
        else if (!strcmp(a, "volume_up") || !strcmp(a, "volume_down")) {
            S.volume += !strcmp(a, "volume_up") ? 0.1f : -0.1f;
            S.volume = S.volume < 0 ? 0 : S.volume > 1 ? 1 : S.volume;
            hal_set_volume(S.volume);
            ui_settings_save();
            char m[32];
            snprintf(m, sizeof m, "volume %d %%", (int)lroundf(S.volume * 100));
            ui_island_say(BZ_I_VOLUME_UP, m);
        }
        return;
    }
    home_pc_cmd(a);
}

static void ha_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (!HM.ha || !HM.ha[i].used) return;
    home_ha_tap(HM.ha[i].id);
}

static void ha_hint_tap(lv_obj_t *o, void *u)
{
    (void)u;
    to_settings(o, NULL);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, const char *action)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, np_cmd, (void *)action);
    lv_obj_set_size(b, 64, 56);
    lv_obj_set_style_pad_hor(b, 0, 0);
    return b;
}

static void build(void)
{
    lv_obj_t *r = bz_box(bz_ui_content());
    HM.root = r;
    lv_obj_set_size(r, W, H);
    lv_obj_set_pos(r, 0, 0);
    lv_obj_add_style(r, bz_style_fill(BZ_C_GROUND), 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); /* nothing under it takes a tap */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);

    /* the time */
    HM.greet = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(HM.greet, M + 4, 30);
    HM.clock = bz_label(r, "--:--", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(HM.clock, M - 4, 50);
    HM.date = bz_label_line(r, "", BZ_F_NAME, BZ_C_DIM, 640);
    lv_obj_set_pos(HM.date, M + 4, 204);

    /* the weather, right-aligned */
    lv_obj_t *wx = bz_col(r, 4);
    lv_obj_set_width(wx, 480);
    lv_obj_set_pos(wx, W - M - 480, 60);
    lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *wr = bz_row(wx, 14);
    lv_obj_set_flex_align(wr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    HM.wx_icon = bz_icon(wr, BZ_I_THERMOMETER, 40, BZ_C_DIM);
    HM.wx_temp = bz_label(wr, "", BZ_F_DISPLAY, BZ_C_INK);
    HM.wx_line = bz_label_line(wx, "", BZ_F_BODY, BZ_C_DIM, 480);
    lv_obj_set_style_text_align(HM.wx_line, LV_TEXT_ALIGN_RIGHT, 0);
    HM.wx_place = bz_label_line(wx, "", BZ_F_CAPTION, BZ_C_DIM, 480);
    lv_obj_set_style_text_align(HM.wx_place, LV_TEXT_ALIGN_RIGHT, 0);

    /* now playing */
    lv_obj_t *np = bz_tile(r, NP_W, ROW2_H);
    HM.np = np;
    lv_obj_set_pos(np, M, ROW2_Y);
    lv_obj_t *artbox = bz_tile(np, ART, ART);
    lv_obj_add_style(artbox, bz_style_fill(BZ_C_SURFACE3), 0);
    lv_obj_set_style_radius(artbox, 20, 0);
    lv_obj_set_style_pad_all(artbox, 0, 0);
    lv_obj_set_style_clip_corner(artbox, true, 0);
    lv_obj_set_pos(artbox, 0, (ROW2_H - 2 * BZ_PAD_TILE - ART) / 2);
    lv_obj_remove_flag(artbox, LV_OBJ_FLAG_CLICKABLE);
    HM.np_ph = bz_icon(artbox, BZ_I_GRAPHIC_EQ, 40, BZ_C_DIM);
    lv_obj_center(HM.np_ph);
    HM.np_img = lv_image_create(artbox);
    lv_obj_set_pos(HM.np_img, 0, 0);
    lv_obj_add_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
    HM.np_src = bz_label_line(np, "", BZ_F_LABEL, BZ_C_DIM, NP_TW);
    lv_obj_set_pos(HM.np_src, NP_TX, 0);
    HM.np_title = bz_label_line(np, "", BZ_F_NAME, BZ_C_INK, NP_TW);
    lv_obj_set_pos(HM.np_title, NP_TX, 24);
    HM.np_artist = bz_label_line(np, "", BZ_F_BODY, BZ_C_DIM, NP_TW);
    lv_obj_set_pos(HM.np_artist, NP_TX, 60);
    HM.np_meter = bz_meter(np, NP_TW - 110, 6);
    lv_obj_set_pos(HM.np_meter, NP_TX, 104);
    HM.np_time = bz_label_line(np, "", BZ_F_CAPTION, BZ_C_DIM, 100);
    lv_obj_set_style_text_align(HM.np_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(HM.np_time, NP_TX + NP_TW - 100, 96);
    lv_obj_t *ctl = bz_row(np, 10);
    lv_obj_set_pos(ctl, NP_TX, ROW2_H - 2 * BZ_PAD_TILE - 56);
    HM.np_prev = round_button(ctl, BZ_I_ARROW_BACK, "previous");
    HM.np_play = round_button(ctl, BZ_I_PLAY_ARROW, "toggle");
    ui_chip_set(HM.np_play, true);
    HM.np_next = round_button(ctl, BZ_I_ARROW_FORWARD, "next");
    lv_obj_t *gap = bz_box(ctl);
    lv_obj_set_size(gap, 18, 1);
    HM.np_vdn = round_button(ctl, BZ_I_REMOVE, "volume_down");
    HM.np_vup = round_button(ctl, BZ_I_ADD, "volume_up");

    /* the companion: a face that opens it */
    lv_obj_t *cp = bz_tile(r, CP_W, ROW2_H);
    HM.face = cp;
    lv_obj_set_pos(cp, CP_X, ROW2_Y);
    lv_obj_add_flag(cp, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(cp, face_tap, NULL);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *e = lv_obj_create(cp);
        lv_obj_remove_style_all(e);
        lv_obj_add_style(e, bz_style_fill(BZ_C_INK), 0);
        lv_obj_set_style_radius(e, 18, 0);
        lv_obj_set_size(e, EYE_W, EYE_H);
        lv_obj_remove_flag(e, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(e, LV_ALIGN_TOP_MID, (i ? 1 : -1) * 48, 22);
        HM.eye[i] = e;
    }
    lv_obj_t *cl = bz_label(cp, "companion", BZ_F_NAME, BZ_C_INK);
    lv_obj_align(cl, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    lv_obj_t *cc = bz_label(cp, "tap to talk \xc2\xb7 claude code on the pc", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(cc, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* the smart home */
    HM.ha = calloc(TILES, sizeof *HM.ha);
    for (int i = 0; HM.ha && i < TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        t->tile = bz_tile(r, TILE_W, ROW3_H);
        lv_obj_set_pos(t->tile, M + i * (TILE_W + BZ_GAP), ROW3_Y);
        lv_obj_set_style_pad_all(t->tile, 18, 0);
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t->tile, ha_tap, (void *)(intptr_t)i);
        t->icon = bz_icon(t->tile, BZ_I_HOME, 32, BZ_C_INK);
        t->state = bz_label_line(t->tile, "", BZ_F_LABEL, BZ_C_DIM, TILE_W - 36 - 44);
        lv_obj_set_style_text_align(t->state, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(t->state, LV_ALIGN_TOP_RIGHT, 0, 6);
        t->name = bz_label_line(t->tile, "", BZ_F_BODY_S, BZ_C_INK, TILE_W - 36);
        lv_obj_align(t->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        t->kind = -1;
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_HIDDEN);
    }
    HM.ha_hint = bz_tile(r, W - 2 * M, ROW3_H);
    lv_obj_set_pos(HM.ha_hint, M, ROW3_Y);
    lv_obj_add_flag(HM.ha_hint, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(HM.ha_hint, ha_hint_tap, NULL);
    lv_obj_set_flex_flow(HM.ha_hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(HM.ha_hint, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(HM.ha_hint, 18, 0);
    bz_icon(HM.ha_hint, BZ_I_LIGHTBULB, 32, BZ_C_DIM);
    lv_obj_t *hc = bz_col(HM.ha_hint, 4);
    bz_label(hc, "smart home", BZ_F_NAME, BZ_C_INK);
    HM.ha_hint_text = bz_label_line(hc, "", BZ_F_BODY_S, BZ_C_DIM, W - 2 * M - 120);

    /* the way back, music, settings, and a line of status */
    lv_obj_t *foot = bz_row(r, 10);
    lv_obj_set_pos(foot, M, FOOT_Y);
    ui_button(foot, BZ_I_SMART_TOY, "catalyst mode", to_catalyst, NULL);
    ui_button(foot, BZ_I_GRAPHIC_EQ, "music", open_app, (void *)&APP_MUSIC);
    ui_button(foot, BZ_I_SETTINGS, "settings", to_settings, NULL);
    HM.status = bz_label_line(r, "", BZ_F_LABEL, BZ_C_DIM, 520);
    lv_obj_set_style_text_align(HM.status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(HM.status, W - M - 520, FOOT_Y + 18);

    HM.art = malloc((size_t)ART * ART * 2);
    HM.art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    HM.art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    HM.art_dsc.header.w = ART;
    HM.art_dsc.header.h = ART;
    HM.art_dsc.header.stride = ART * 2;
    HM.art_dsc.data = (const uint8_t *)HM.art;
    HM.art_dsc.data_size = ART * ART * 2;
    HM.built = true;
}

/* ---- what changes ---- */

static const char *ha_icon(home_ha_kind_t k, const char *unit)
{
    switch (k) {
    case HA_LIGHT: return BZ_I_LIGHTBULB;
    case HA_SWITCH: return BZ_I_POWER;
    case HA_FAN: return BZ_I_AUTO_MODE;
    case HA_SCENE: return BZ_I_PALETTE;
    case HA_SCRIPT: return BZ_I_AUTO_AWESOME;
    case HA_BUTTON: return BZ_I_RADIO_BUTTON_UNCHECKED;
    case HA_SENSOR: return unit && (strstr(unit, "\xc2\xb0") || !strcmp(unit, "K")) ? BZ_I_THERMOMETER : BZ_I_SENSORS;
    case HA_BINARY: return BZ_I_SENSORS;
    case HA_MEDIA: return BZ_I_GRAPHIC_EQ;
    case HA_COVER: return BZ_I_EXPAND_LESS;
    case HA_CLIMATE: return BZ_I_THERMOSTAT;
    case HA_LOCK: return BZ_I_LOCK;
    default: return BZ_I_HOME;
    }
}

static void ha_state_text(const home_ha_entity_t *e, char *out, size_t n)
{
    if (e->pending) snprintf(out, n, "...");
    else if (e->kind == HA_SCENE || e->kind == HA_SCRIPT || e->kind == HA_BUTTON)
        snprintf(out, n, "%s", !strcmp(e->state, "unavailable") ? "away" : "tap");
    else if (e->unit[0]) snprintf(out, n, "%s %s", e->state, e->unit);
    else if (!e->state[0]) snprintf(out, n, "\xe2\x80\x94");
    else snprintf(out, n, "%s", e->state);
}

static void refresh_ha(void)
{
    home_ha_status_t st;
    home_ha_status(&st);
    if (st.gen == HM.ha_gen && HM.ha_gen) return;
    HM.ha_gen = st.gen;
    home_ha_entity_t tiles[TILES];
    int n = st.configured ? home_ha_tiles(tiles, TILES) : 0;
    if (!st.configured || n == 0) {
        ui_text(HM.ha_hint_text, "%s", !st.configured ? "Add Home Assistant in settings, home: its address and a long-lived token."
                                                       : "Pick lights, switches and sensors to show here: settings, home.");
        lv_obj_remove_flag(HM.ha_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(HM.ha_hint, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; HM.ha && i < TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        if (i >= n) {
            if (t->used) lv_obj_add_flag(t->tile, LV_OBJ_FLAG_HIDDEN);
            t->used = false;
            continue;
        }
        const home_ha_entity_t *e = &tiles[i];
        if (!t->used) lv_obj_remove_flag(t->tile, LV_OBJ_FLAG_HIDDEN);
        t->used = true;
        snprintf(t->id, sizeof t->id, "%s", e->id);
        if (t->kind != (int)e->kind) {
            t->kind = (int)e->kind;
            bz_icon_set(t->icon, ha_icon(e->kind, e->unit), 32, false);
        }
        ui_text(t->name, "%s", e->name);
        char s[40];
        ha_state_text(e, s, sizeof s);
        ui_text(t->state, "%s", s);
        bool on = e->on && e->actionable;
        if (on != t->on || !t->shown_state[0]) {
            t->on = on;
            snprintf(t->shown_state, sizeof t->shown_state, "set");
            bz_tile_set_fill(t->tile, on ? BZ_C_ICE : BZ_C_SURFACE1);
            bz_set_color(t->icon, on ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->name, on ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->state, on ? BZ_C_ON_ICE : BZ_C_DIM);
        }
    }
    if (st.configured && !st.ok && st.err[0]) ui_text(HM.status, "home assistant: %s", st.err);
    else ui_text(HM.status, "%s", "");
}

static void refresh_weather(void)
{
    home_weather_t w;
    home_weather_get(&w);
    if (w.gen == HM.wx_gen && HM.wx_gen) return;
    HM.wx_gen = w.gen;
    if (!w.configured) {
        ui_text(HM.wx_temp, "%s", "");
        ui_text(HM.wx_line, "%s", "the weather: set a place in settings, home");
        ui_text(HM.wx_place, "%s", "");
        lv_obj_add_flag(HM.wx_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!w.ok) {
        ui_text(HM.wx_temp, "%s", "");
        ui_text(HM.wx_line, "%s", w.err[0] ? w.err : "looking up the weather");
        ui_text(HM.wx_place, "%s", w.place);
        lv_obj_add_flag(HM.wx_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(HM.wx_icon, LV_OBJ_FLAG_HIDDEN);
    bz_icon_set(HM.wx_icon, w.code <= 1 ? (w.day ? BZ_I_LIGHT_MODE : BZ_I_DARK_MODE)
                            : w.code >= 95 ? BZ_I_ELECTRIC_BOLT : BZ_I_THERMOMETER, 40, false);
    ui_text(HM.wx_temp, "%.0f\xc2\xb0", w.temp);
    ui_text(HM.wx_line, "%s \xc2\xb7 %.0f\xc2\xb0 / %.0f\xc2\xb0", home_weather_text(w.code), w.hi, w.lo);
    ui_text(HM.wx_place, "%s", w.place);
}

static void set_art(bool show)
{
    if (show == HM.art_shown && !show) return;
    if (show) {
        lv_image_cache_drop(&HM.art_dsc);
        lv_image_set_src(HM.np_img, &HM.art_dsc);
        lv_obj_invalidate(HM.np_img);
        lv_obj_remove_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(HM.np_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(HM.np_ph, LV_OBJ_FLAG_HIDDEN);
    }
    HM.art_shown = show;
}

static void show_playing(bool playing)
{
    if (playing == HM.playing_shown) return;
    HM.playing_shown = playing;
    bz_icon_set(lv_obj_get_child(HM.np_play, 0), playing ? BZ_I_PAUSE : BZ_I_PLAY_ARROW, 24, false);
}

static void refresh_media(void)
{
    home_play_t pl;
    home_player_get(&pl);
    bool local = pl.state == HP_PLAYING || pl.state == HP_PAUSED || pl.state == HP_LOADING;
    double now = hal_seconds();
    int sec = (int)now;
    bool tick = sec != HM.last_sec;
    HM.last_sec = sec;
    if (local != HM.local) {
        HM.local = local;
        HM.pc_gen = HM.pl_gen = 0; /* redraw the card from the other source */
        set_art(false);
    }
    if (local) {
        if (pl.gen != HM.pl_gen || !HM.pl_gen) {
            HM.pl_gen = pl.gen;
            ui_text(HM.np_src, "%s", "this tablet \xc2\xb7 microsd");
            ui_text(HM.np_title, "%s", pl.title);
            ui_text(HM.np_artist, "%s", pl.state == HP_LOADING ? "starting" : pl.state == HP_PAUSED ? "paused" : "playing");
            show_playing(pl.state != HP_PAUSED);
        }
        if (tick || pl.gen != HM.pl_gen) {
            char a[16], b[16];
            hm_fmt_time(pl.position, a, sizeof a);
            hm_fmt_time(pl.duration, b, sizeof b);
            ui_text(HM.np_time, "%s / %s", a, b);
            bz_meter_set(HM.np_meter, pl.duration > 0 ? (float)(pl.position / pl.duration) : 0, BZ_C_ICE);
        }
        return;
    }
    home_pc_t pc;
    home_pc_get(&pc);
    if (pc.gen != HM.pc_gen || !HM.pc_gen) {
        HM.pc_gen = pc.gen ? pc.gen : 1;
        if (!hm_cfg()->pc) {
            ui_text(HM.np_src, "%s", "music");
            ui_text(HM.np_title, "%s", "Nothing playing");
            ui_text(HM.np_artist, "%s", "tap music for the songs on the card");
        } else if (pc.have) {
            ui_text(HM.np_src, "on the pc \xc2\xb7 %s", pc.app[0] ? pc.app : "media");
            ui_text(HM.np_title, "%s", pc.title[0] ? pc.title : "untitled");
            ui_text(HM.np_artist, "%s", pc.artist[0] ? pc.artist : pc.album);
        } else {
            ui_text(HM.np_src, "%s", pc.link ? "the pc" : "music");
            ui_text(HM.np_title, "%s", "Nothing playing");
            ui_text(HM.np_artist, "%s", pc.link && !pc.available && pc.reason[0] ? pc.reason
                                        : pc.link ? "play something on the pc, or tap music for the card"
                                        : "pair the pc (settings, pc link) or tap music for the card");
        }
        show_playing(pc.have && pc.playing);
        if (pc.art_gen != HM.art_gen) {
            HM.art_gen = pc.art_gen;
            set_art(HM.art && pc.art && home_pc_art(HM.art));
        }
        if (!pc.have) set_art(false);
    }
    if (tick) {
        double pos = pc.position;
        if (pc.have && pc.playing && pos >= 0) pos += now - pc.at;
        if (pc.duration > 0 && pos > pc.duration) pos = pc.duration;
        char a[16], b[16];
        hm_fmt_time(pc.have ? pos : -1, a, sizeof a);
        hm_fmt_time(pc.have ? pc.duration : -1, b, sizeof b);
        if (pc.have && pc.duration > 0) ui_text(HM.np_time, "%s / %s", a, b);
        else ui_text(HM.np_time, "%s", pc.have && pos >= 0 ? a : "");
        bz_meter_set(HM.np_meter, pc.have && pc.duration > 0 ? (float)(pos / pc.duration) : 0, BZ_C_ICE);
    }
}

static void refresh_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_min == HM.last_min) return;
    HM.last_min = tm.tm_min;
    if (tm.tm_year > 120) {
        ui_text(HM.clock, "%d:%02d", tm.tm_hour, tm.tm_min);
        char d[40];
        strftime(d, sizeof d, "%A, %B %e", &tm);
        ui_text(HM.date, "%s", d);
        ui_text(HM.greet, "%s", tm.tm_hour < 5 ? "late night" : tm.tm_hour < 12 ? "good morning"
                                : tm.tm_hour < 18 ? "good afternoon" : "good evening");
    } else {
        ui_text(HM.clock, "--:--");
        ui_text(HM.date, "the clock isn't set");
        ui_text(HM.greet, "%s", "");
    }
}

static void hm_refresh(void *u)
{
    (void)u;
    if (!HM.active || HM.sheet) return;
    /* the network only for what's on screen: with an app over it, only the music card's source */
    if (ui_app_any_open()) return;
    home_want(HOME_WANT_PC | HOME_WANT_HA | HOME_WANT_WEATHER);
    refresh_clock();
    refresh_weather();
    refresh_media();
    refresh_ha();
}

/* ---- coming and going ---- */

static void hide_page(bool hide)
{
    if (hide) {
        if (HM.hid_page >= 0) return;
        HM.hid_page = ui_page();
        lv_obj_add_flag(ui_page_body(HM.hid_page), LV_OBJ_FLAG_HIDDEN);
    } else if (HM.hid_page >= 0) {
        lv_obj_remove_flag(ui_page_body(HM.hid_page), LV_OBJ_FLAG_HIDDEN);
        HM.hid_page = -1;
    }
}

/* The resting state for LVGL: home mode showing (root up, the page under it hidden, the chrome away) or not. */
static void set_state(bool home)
{
    if (home) {
        lv_obj_remove_flag(HM.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(HM.root);
        hide_page(true);
    } else {
        hide_page(false);
        lv_obj_add_flag(HM.root, LV_OBJ_FLAG_HIDDEN);
    }
    if (HM.on_change) HM.on_change(home);
}

static void go(bool enter)
{
    HM.active = enter;
    if (enter) {
        HM.last_min = -1;
        HM.wx_gen = HM.ha_gen = HM.pc_gen = HM.pl_gen = 0;
        HM.art_gen = ~0u;
        hm_cfg_apply();
        HM.blink_at = hal_seconds() + 3;
    }
    bz_ui_wake();
    /* the platform slides the new picture in over the old one, drawing it a band at a time as it shows:
     * the state switches now, under a glass that keeps the old picture where the sheet hasn't reached */
    if (bz_ui_sheet_begin(enter, H, true, NULL, NULL)) {
        HM.sheet = true;
        bz_motion_set(&HM.k, enter ? 0 : 1, 0);
        bz_motion_to(&HM.k, enter ? 1 : 0, BZ_RELEASE);
    }
    set_state(enter);
    if (enter) hm_refresh(NULL);
    bz_ui_keep_alive();
}

void ui_home_mode_enter(void)
{
    if (!HM.built) ui_home_mode_init(HM.on_change); /* a shell that hasn't wired it yet: built on first use */
    HM.why = UI_HOME_BY_HAND;
    HM.want = HM.active ? 0 : 1;
    if (HM.want && ui_app_any_open()) ui_app_close(); /* entered from settings: the window goes first */
}

void ui_home_mode_exit(void)
{
    if (!HM.built) return;
    HM.want = HM.active ? -1 : 0;
    if (HM.want) HM.left_at = hal_seconds();
}

void ui_home_mode_trigger(ui_home_why_t why, bool present)
{
    if (!HM.built) return;
    if (present) {
        if (HM.active || HM.want > 0) return;
        HM.why = why;
        HM.want = 1;
    } else if (HM.active && HM.why == why && why != UI_HOME_BY_HAND) {
        HM.want = -1;
    }
}

void ui_home_mode_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_home_mode_enter();
}

/* The stand: upright (landscape), still and on external power for a minute, and nobody touching it; not
 * left by hand in the last five minutes. Brought by the stand, it leaves when lifted or unplugged. The same
 * test as the companion's desk mode (ui_app_companion.c), which home mode turns off when it takes over. */
static void stand_tick(double now)
{
    static double next, still_since, gone_since;
    if (now < next) return;
    next = now + 1.0;
    hal_battery_t bat;
    HM.power = hal_battery(&bat) && bat.ok && (bat.charging || bat.external);
    hal_imu_t m;
    bool imu = hal_imu(&m) && m.ok;
    bool upright = imu && fabsf(m.ay) > 0.72f && fabsf(m.ax) < 0.35f;
    bool still = upright && sqrtf(m.gx * m.gx + m.gy * m.gy + m.gz * m.gz) < 4.0f;
    if (HM.active) {
        still_since = 0;
        if (HM.why != UI_HOME_BY_STAND) return;
        bool gone = !HM.power || (imu && !upright);
        if (!gone) {
            gone_since = 0;
        } else {
            if (gone_since == 0) gone_since = now;
            if (now - gone_since > 4) {
                gone_since = 0;
                ui_home_mode_trigger(UI_HOME_BY_STAND, false);
            }
        }
        return;
    }
    gone_since = 0;
    if (!hm_cfg()->stand || ui_app_any_open() || !HM.power || !still) {
        still_since = 0;
        return;
    }
    if (still_since == 0) still_since = now;
    if (now - still_since > 60 && bz_ui_idle_s() > 45 && (HM.left_at == 0 || now - HM.left_at > 300)) {
        still_since = 0;
        ui_home_mode_trigger(UI_HOME_BY_STAND, true);
    }
}

static void blink_tick(double now)
{
    if (!HM.active || HM.sheet || ui_app_any_open() || ui_asleep()) return;
    if (!HM.blinking && now >= HM.blink_at) {
        HM.blinking = true;
        for (int i = 0; i < 2; i++) {
            lv_obj_set_height(HM.eye[i], 8);
            lv_obj_align(HM.eye[i], LV_ALIGN_TOP_MID, (i ? 1 : -1) * 48, 22 + (EYE_H - 8) / 2);
        }
        HM.blink_at = now + 0.13;
    } else if (HM.blinking && now >= HM.blink_at) {
        HM.blinking = false;
        for (int i = 0; i < 2; i++) {
            lv_obj_set_height(HM.eye[i], EYE_H);
            lv_obj_align(HM.eye[i], LV_ALIGN_TOP_MID, (i ? 1 : -1) * 48, 22);
        }
        HM.blink_at = now + 4 + (rand() % 5000) / 1000.0; /* 4-9 s */
    }
}

static void hm_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    now = hal_seconds();
    stand_tick(now);
    if (HM.sheet) {
        bool moving = bz_motion_tick(&HM.k);
        float k = HM.k.value < 0 ? 0 : HM.k.value > 1 ? 1 : HM.k.value;
        bz_ui_keep_alive();
        if (moving) {
            bz_ui_sheet((int)(k * H + 0.5f));
            return;
        }
        int rest = HM.k.target > 0.5f ? H : 0;
        if (bz_ui_sheet_shown() != rest) {
            bz_ui_sheet(rest);
            return;
        }
        bz_ui_sheet_end();
        HM.sheet = false;
        return;
    }
    /* a pending change waits for any app window and sheet to finish */
    if (HM.want && !ui_app_any_open() && !bz_ui_sheeting()) {
        int w = HM.want;
        HM.want = 0;
        if (w > 0 && !HM.active) go(true);
        else if (w < 0 && HM.active) go(false);
    }
    blink_tick(now);
}

void ui_home_mode_init(ui_home_mode_fn on_change)
{
    HM.on_change = on_change;
    if (HM.built) return; /* built early by a first enter: the shell's listener is all that was missing */
    hm_cfg();
    hm_cfg_apply();
    build();
    bz_motion_init(&HM.k, 0, 0.001f);
    HM.k.keep = true; /* animates though the lean renderer makes other motion instant */
    bz_ui_on_frame(hm_frame, NULL);
    ui_on_refresh(hm_refresh, NULL);
    home_player_rescan();
    if (hm_cfg()->start_home) {
        /* at boot: no sheet, it's simply there */
        HM.why = UI_HOME_BY_BOOT;
        HM.active = true;
        HM.last_min = -1;
        HM.art_gen = ~0u;
        set_state(true);
    }
    /* home mode on the stand replaces the companion's own desk mode: one of them decides */
    if (hm_cfg()->stand) {
        ui_companion_cfg_t *c = ui_companion_cfg();
        if (c->desk_auto) {
            c->desk_auto = false;
            ui_companion_cfg_save();
        }
    }
}
