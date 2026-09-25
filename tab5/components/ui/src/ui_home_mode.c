/* Home mode: the surface, its triggers and its settings store.
 *
 * The layout, 1280 × 720 at the tablet's type size (BZ_LEAN), top to bottom:
 *   a greeting, the time (large), the date                 the weather, right-aligned (a tap: the forecast)
 *   what's playing (art, title, progress, controls) · the companion's face · four Home Assistant tiles
 *   the launcher: music, smart home, weather, calendar, timer, alarms, photos, companion, settings, catalyst
 *
 * Every label has a width it was measured against at the tablet's type size (the lean faces: body 24, body_s
 * 21, name 30, mono 19/16), and a line that can run long (a song, a reason, a hint) either ellipsizes on one
 * line or has two lines of room.
 *
 * Cheap to keep on screen: nothing animates at rest. The clock changes once a minute; the progress bar and
 * its time once a second while something plays (and are hidden when nothing does); a tile only when its
 * entity changes; the art once a song; the companion blinks every few seconds (two small eyes). Nothing is
 * restyled unless its state changed.
 *
 * Coming and going is a bottom sheet the platform slides (bz_ui_sheet_*, as an app opens), drawn a band at a
 * time; with no platform sheet (the simulator) it just switches. Apps open over it and close back to it (the
 * shell keeps its status band, orb and dock away while home mode is active). */
#include "ui_home_priv.h"

#include "ui_companion.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define M 56                        /* the surface's side margin */
#define TOP_Y 50                    /* the greeting's top; the time under it, the date under that */
#define WX_W 460                    /* the weather's column, right-aligned */
#define CARD_Y 240
#define CARD_H 268
#define CARD_IN_H (CARD_H - 2 * BZ_PAD_TILE)
#define NP_W 560                    /* now playing */
#define CP_X (M + NP_W + BZ_GAP)    /* the companion */
#define CP_W 232
#define HA_X (CP_X + CP_W + BZ_GAP) /* the smart home tiles, two by two */
#define HA_W (W - M - HA_X)
#define HT_W ((HA_W - BZ_GAP) / 2)
#define HT_H ((CARD_H - BZ_GAP) / 2)
#define HT_PAD 16
#define ART HOME_ART                /* 160 */
#define NP_TX (ART + 24)            /* the text column, inside the card */
#define NP_TW (NP_W - 2 * BZ_PAD_TILE - NP_TX) /* 328 */
#define TIME_W 110
#define CTL_W 56                    /* five transport buttons and a spacer fill NP_TW exactly */
#define CTL_GAP 8
#define CTL_SPACER (NP_TW - 5 * CTL_W - 5 * CTL_GAP)
#define EYE_W 38
#define EYE_H 62
#define EYE_DX 36
#define EYE_Y 34
#define LN_Y (CARD_Y + CARD_H + 26) /* the launcher */
#define LN_H 108
#define LN_GAP 10
#define LN_W ((W - 2 * M - 9 * LN_GAP) / 10)
#define LN_X (M + (W - 2 * M - 10 * LN_W - 9 * LN_GAP) / 2)
#define TAG_DEBOUNCE_S 1.5          /* the tag gone this long before home mode hears it left */
#define TAG_PAIR_S 30.0             /* how long "pair a tag" waits for one */

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
    CFG = calloc(1, sizeof *CFG); /* ~2 KB: PSRAM, not a static in internal RAM */
    if (!CFG) abort();            /* at start-up, with 32 MB of PSRAM: never */
    CFG->start_home = kv_bool("hm_start", false);
    CFG->stand = kv_bool("hm_stand", false);
    CFG->fahrenheit = kv_bool("hm_fahr", false);
    CFG->pc = kv_bool("hm_pc", true);
    char v[8];
    CFG->saver_min = hal_kv_get("hm_saver", v, sizeof v) ? atoi(v) : 0;
    hal_kv_get("hm_tag", CFG->tag, sizeof CFG->tag);
    hal_kv_get("hm_net", CFG->net, sizeof CFG->net);
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
    char v[8];
    snprintf(v, sizeof v, "%d", c->saver_min);
    hal_kv_set("hm_saver", v);
    hal_kv_set("hm_tag", c->tag);
    hal_kv_set("hm_net", c->net);
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
    int kind;
    int lit;                       /* -1 not drawn yet, else whether it's drawn lit */
    bool used;
} ha_tile_t;

typedef struct {
    const ui_app_t *app;           /* NULL: one of the actions below */
    const char *icon, *label;
    int action;                    /* 1 companion, 2 settings, 3 catalyst mode */
} hm_app_t;

static const hm_app_t LAUNCH[] = {
    { &APP_MUSIC, BZ_I_GRAPHIC_EQ, "music", 0 },
    { &APP_SMARTHOME, BZ_I_LIGHTBULB, "house", 0 },
    { &APP_WEATHER, BZ_I_LIGHT_MODE, "weather", 0 },
    { &APP_CALENDAR, BZ_I_GRID_VIEW, "calendar", 0 },
    { &APP_TIMER, BZ_I_TIMER, "timer", 0 },
    { &APP_CLOCK, BZ_I_SCHEDULE, "alarms", 0 },
    { &APP_PHOTOS, BZ_I_CAMERA, "photos", 0 },
    { NULL, BZ_I_VISIBILITY, "companion", 1 },
    { NULL, BZ_I_SETTINGS, "settings", 2 },
    { NULL, BZ_I_SMART_TOY, "catalyst", 3 },
};
#define NLAUNCH ((int)(sizeof LAUNCH / sizeof LAUNCH[0]))

static struct {
    lv_obj_t *root, *greet, *clock, *date, *match;
    lv_obj_t *wx, *wx_icon, *wx_temp, *wx_line, *wx_place;
    lv_obj_t *np, *np_img, *np_ph, *np_src, *np_title, *np_artist, *np_meter, *np_time, *np_play;
    lv_obj_t *eye[2], *face;
    lv_obj_t *ha_hint, *ha_hint_text;
    ha_tile_t ha[HM_HOME_TILES];
    lv_image_dsc_t art_dsc;
    uint16_t *art;                 /* ART² RGB565 */
    unsigned art_gen, pc_gen, wx_gen, ha_gen, pl_gen;
    int last_min, last_sec;
    bool art_shown;
    bool local;                    /* the card shows the tablet's own player (else the PC's) */
    bool playing_shown, progress_shown;
    const char *wx_glyph;
    /* mode */
    bool built, active, sheet;
    int want;                      /* +1 enter, -1 leave, 0 nothing pending */
    ui_home_why_t why;             /* what brought it (an automatic trigger may take it away again) */
    double left_at;                /* hal_seconds() when it was last left by hand */
    int hid_page;                  /* the page hidden under it (-1 none) */
    bz_motion_t k;
    double blink_at;
    bool blinking;
    /* the living parts, each a small area redrawn only while it moves */
    bz_motion_t lid, gx, gy;       /* the eyes: open 0..1, where they look (px) */
    double gaze_at;
    int eye_h[2], eye_x[2], eye_y[2];
    bz_motion_t roll;              /* the time rolling up to a new minute, 1 → 0 */
    int roll_px, roll_opa;
    bz_motion_t press[12];         /* the launcher's tiles sinking under a finger */
    int press_scale[12];
    lv_obj_t *tile[12];
    ui_home_mode_fn on_change;
    bool power;
    /* the photos screensaver */
    bool saver_on;
    /* the NFC tag */
    int tag_pairing;               /* 0 no, 1 waiting for a tag, 2 just paired */
    double tag_pair_until;
    bool tag_here;
    double tag_seen_at;
} HM = { .hid_page = -1 };

bool ui_home_mode_active(void) { return HM.active; }
bool ui_home_mode_keeps_awake(void) { return HM.active && HM.power; }

void hm_open_settings(lv_obj_t *from)
{
    hm_settings_request();
    ui_app_open(&APP_SETTINGS, from);
}

static void launch_tap(lv_obj_t *o, void *u)
{
    const hm_app_t *a = u;
    if (a->app) ui_app_open(a->app, o);
    else if (a->action == 1) ui_companion_open(false, o);
    else if (a->action == 2) hm_open_settings(o);
    else ui_home_mode_exit();
}

static void open_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }

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
    if (!HM.ha[i].used) return;
    home_ha_tap(HM.ha[i].id);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, const char *action)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, np_cmd, (void *)action);
    lv_obj_set_size(b, CTL_W, 56);
    lv_obj_set_style_pad_hor(b, 0, 0);
    return b;
}

/* a card: surface1 with the tile's padding, placed on the surface */
static lv_obj_t *card(lv_obj_t *r, int x, int w)
{
    lv_obj_t *c = bz_tile(r, w, CARD_H);
    lv_obj_set_pos(c, x, CARD_Y);
    return c;
}

static void build_top(lv_obj_t *r)
{
    /* the time, left */
    HM.greet = bz_label_line(r, "", BZ_F_LABEL, BZ_C_DIM, 420);
    lv_obj_set_pos(HM.greet, M + 2, TOP_Y);
    HM.clock = bz_label(r, "--:--", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(HM.clock, M - 6, TOP_Y + 20);
    HM.date = bz_label_line(r, "", BZ_F_BODY, BZ_C_DIM, 560);
    lv_obj_set_pos(HM.date, M + 2, TOP_Y + 134);
    HM.match = bz_label_line(r, "", BZ_F_LABEL, BZ_C_SIGNAL, 560); /* the team's next match (ui_match.c) */
    lv_obj_set_pos(HM.match, M + 2, TOP_Y + 166);

    /* the weather, right-aligned: a tap opens the forecast */
    lv_obj_t *wx = bz_col(r, 2);
    HM.wx = wx;
    lv_obj_set_width(wx, WX_W);
    lv_obj_set_pos(wx, W - M - WX_W, TOP_Y);
    lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_add_flag(wx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(wx, 12);
    bz_on_tap(wx, open_tap, (void *)&APP_WEATHER);
    lv_obj_t *wr = bz_row(wx, 12);
    lv_obj_set_flex_align(wr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    HM.wx_icon = bz_icon(wr, BZ_I_THERMOMETER, 40, BZ_C_DIM);
    lv_obj_add_flag(HM.wx_icon, LV_OBJ_FLAG_HIDDEN);
    HM.wx_temp = bz_label(wr, "", BZ_F_DISPLAY, BZ_C_INK);
    HM.wx_line = bz_label_line(wx, "", BZ_F_BODY, BZ_C_DIM, WX_W);
    lv_obj_set_style_text_align(HM.wx_line, LV_TEXT_ALIGN_RIGHT, 0);
    HM.wx_place = bz_label_line(wx, "", BZ_F_CAPTION, BZ_C_DIM, WX_W);
    lv_obj_set_style_text_align(HM.wx_place, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_playing(lv_obj_t *r)
{
    lv_obj_t *np = card(r, M, NP_W);
    HM.np = np;
    lv_obj_t *artbox = bz_tile(np, ART, ART);
    lv_obj_add_style(artbox, bz_style_fill(BZ_C_SURFACE3), 0);
    lv_obj_set_style_radius(artbox, 20, 0);
    lv_obj_set_style_pad_all(artbox, 0, 0);
    lv_obj_set_style_clip_corner(artbox, true, 0);
    lv_obj_set_pos(artbox, 0, (CARD_IN_H - ART) / 2);
    lv_obj_add_flag(artbox, LV_OBJ_FLAG_CLICKABLE); /* the art opens the music app */
    bz_on_tap(artbox, open_tap, (void *)&APP_MUSIC);
    HM.np_ph = bz_icon(artbox, BZ_I_GRAPHIC_EQ, 40, BZ_C_DIM);
    lv_obj_center(HM.np_ph);
    HM.np_img = lv_image_create(artbox);
    lv_obj_set_pos(HM.np_img, 0, 0);
    lv_obj_add_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(HM.np_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    HM.np_src = bz_label_line(np, "", BZ_F_LABEL, BZ_C_DIM, NP_TW);
    lv_obj_set_pos(HM.np_src, NP_TX, 0);
    HM.np_title = bz_label_line(np, "", BZ_F_NAME, BZ_C_INK, NP_TW);
    lv_obj_set_pos(HM.np_title, NP_TX, 28);
    /* two lines: an artist and album, or a hint when nothing plays */
    HM.np_artist = bz_label(np, "", BZ_F_BODY, BZ_C_DIM);
    lv_label_set_long_mode(HM.np_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(HM.np_artist, NP_TW, 2 * lv_font_get_line_height(bz_font(BZ_F_BODY)));
    lv_obj_set_pos(HM.np_artist, NP_TX, 66);
    HM.np_meter = bz_meter(np, NP_TW - TIME_W - 12, 6);
    lv_obj_set_pos(HM.np_meter, NP_TX, 134);
    HM.np_time = bz_label_line(np, "", BZ_F_CAPTION, BZ_C_DIM, TIME_W);
    lv_obj_set_style_text_align(HM.np_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(HM.np_time, NP_TX + NP_TW - TIME_W, 126);
    lv_obj_add_flag(HM.np_meter, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(HM.np_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ctl = bz_row(np, CTL_GAP);
    lv_obj_set_pos(ctl, NP_TX, CARD_IN_H - 56);
    round_button(ctl, BZ_I_ARROW_BACK, "previous");
    HM.np_play = round_button(ctl, BZ_I_PLAY_ARROW, "toggle");
    ui_chip_set(HM.np_play, true);
    round_button(ctl, BZ_I_ARROW_FORWARD, "next");
    lv_obj_t *gap = bz_box(ctl);
    lv_obj_set_size(gap, CTL_SPACER, 1);
    round_button(ctl, BZ_I_REMOVE, "volume_down");
    round_button(ctl, BZ_I_ADD, "volume_up");
}

static void build_companion(lv_obj_t *r)
{
    lv_obj_t *cp = card(r, CP_X, CP_W);
    HM.face = cp;
    lv_obj_add_flag(cp, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(cp, face_tap, NULL);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *e = lv_obj_create(cp);
        lv_obj_remove_style_all(e);
        lv_obj_add_style(e, bz_style_fill(BZ_C_INK), 0);
        lv_obj_set_style_radius(e, 18, 0);
        lv_obj_set_size(e, EYE_W, EYE_H);
        lv_obj_remove_flag(e, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(e, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(e, LV_ALIGN_TOP_MID, (i ? 1 : -1) * EYE_DX, EYE_Y);
        HM.eye[i] = e;
    }
    lv_obj_t *cl = bz_label_line(cp, "companion", BZ_F_NAME, BZ_C_INK, CP_W - 2 * BZ_PAD_TILE);
    lv_obj_align(cl, LV_ALIGN_BOTTOM_LEFT, 0, -26);
    lv_obj_t *cc = bz_label_line(cp, "tap to talk", BZ_F_CAPTION, BZ_C_DIM, CP_W - 2 * BZ_PAD_TILE);
    lv_obj_align(cc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void build_home(lv_obj_t *r)
{
    /* up to four picked entities, two by two; the smart home app has them all */
    for (int i = 0; i < HM_HOME_TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        t->tile = bz_tile(r, HT_W, HT_H);
        lv_obj_set_pos(t->tile, HA_X + (i % 2) * (HT_W + BZ_GAP), CARD_Y + (i / 2) * (HT_H + BZ_GAP));
        lv_obj_set_style_pad_all(t->tile, HT_PAD, 0);
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t->tile, ha_tap, (void *)(intptr_t)i);
        t->icon = bz_icon(t->tile, BZ_I_HOME, 32, BZ_C_INK);
        t->state = bz_label_line(t->tile, "", BZ_F_LABEL, BZ_C_DIM, HT_W - 2 * HT_PAD - 50);
        lv_obj_set_style_text_align(t->state, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(t->state, LV_ALIGN_TOP_RIGHT, 0, 8);
        t->name = bz_label_line(t->tile, "", BZ_F_BODY_S, BZ_C_INK, HT_W - 2 * HT_PAD);
        lv_obj_align(t->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        t->kind = -1;
        t->lit = -1;
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_HIDDEN);
    }
    /* before anything is picked (or reachable): what to do, and a tap into the app */
    HM.ha_hint = card(r, HA_X, HA_W);
    lv_obj_add_flag(HM.ha_hint, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(HM.ha_hint, open_tap, (void *)&APP_SMARTHOME);
    bz_icon(HM.ha_hint, BZ_I_LIGHTBULB, 32, BZ_C_DIM);
    lv_obj_t *hn = bz_label_line(HM.ha_hint, "smart home", BZ_F_NAME, BZ_C_INK, HA_W - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(hn, 0, 56);
    HM.ha_hint_text = bz_label(HM.ha_hint, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_label_set_long_mode(HM.ha_hint_text, LV_LABEL_LONG_DOT);
    lv_obj_set_size(HM.ha_hint_text, HA_W - 2 * BZ_PAD_TILE, 4 * lv_font_get_line_height(bz_font(BZ_F_BODY_S)));
    lv_obj_set_pos(HM.ha_hint_text, 0, 98);
}

static void press_cb(lv_event_t *e);

static void build_launcher(lv_obj_t *r)
{
    for (int i = 0; i < NLAUNCH; i++) {
        const hm_app_t *a = &LAUNCH[i];
        lv_obj_t *t = bz_tile(r, LN_W, LN_H);
        lv_obj_set_pos(t, LN_X + i * (LN_W + LN_GAP), LN_Y);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_radius(t, 28, 0);
        if (a->action == 3) bz_tile_set_fill(t, BZ_C_SURFACE2); /* the way out, set apart by its tone */
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t, launch_tap, (void *)a);
        if (i < 12) {
            HM.tile[i] = t;
            HM.press_scale[i] = 256;
            bz_motion_init(&HM.press[i], 0, 0.004f);
            HM.press[i].keep = true;
            lv_obj_set_style_transform_pivot_x(t, LN_W / 2, 0);
            lv_obj_set_style_transform_pivot_y(t, LN_H / 2, 0);
            lv_obj_add_event_cb(t, press_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(t, press_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(t, press_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
        }
        lv_obj_t *ic = bz_icon(t, a->icon, 32, BZ_C_INK);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 18);
        lv_obj_t *lb = bz_label_line(t, a->label, BZ_F_CAPTION, BZ_C_DIM, LN_W - 6);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
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

    build_top(r);
    build_playing(r);
    build_companion(r);
    /* the living parts' springs: kept (they move even though the lean renderer's motion is instant) */
    bz_motion_init(&HM.lid, 1, 0.01f);
    bz_motion_init(&HM.gx, 0, 0.3f);
    bz_motion_init(&HM.gy, 0, 0.3f);
    bz_motion_init(&HM.roll, 0, 0.01f);
    HM.lid.keep = HM.gx.keep = HM.gy.keep = HM.roll.keep = true;
    HM.roll_opa = 255;
    for (int i = 0; i < 2; i++) {
        HM.eye_h[i] = EYE_H;
        HM.eye_x[i] = (i ? 1 : -1) * EYE_DX;
        HM.eye_y[i] = EYE_Y;
    }
    build_home(r);
    build_launcher(r);

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

const char *hm_ha_icon(home_ha_kind_t k, const char *unit)
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

void hm_ha_state_text(const home_ha_entity_t *e, char *out, size_t n)
{
    if (e->pending) snprintf(out, n, "...");
    else if (!strcmp(e->state, "unavailable")) snprintf(out, n, "away");
    else if (e->kind == HA_SCENE || e->kind == HA_SCRIPT || e->kind == HA_BUTTON) snprintf(out, n, "tap");
    else if (e->kind == HA_LIGHT && e->on && e->brightness > 0) snprintf(out, n, "%d %%", e->brightness);
    else if (e->kind == HA_CLIMATE && !isnan(e->current)) snprintf(out, n, "%.1f\xc2\xb0", e->current);
    else if (e->unit[0]) snprintf(out, n, "%s %s", e->state, e->unit);
    else if (!e->state[0]) snprintf(out, n, "\xe2\x80\x94");
    else snprintf(out, n, "%s", e->state);
}

const char *hm_wx_icon(int code, bool day)
{
    if (code >= 0 && code <= 1) return day ? BZ_I_LIGHT_MODE : BZ_I_DARK_MODE;
    if (code >= 95) return BZ_I_ELECTRIC_BOLT;
    return BZ_I_THERMOMETER;
}

static void show(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_ha(void)
{
    home_ha_status_t st;
    home_ha_status(&st);
    if (st.gen == HM.ha_gen && HM.ha_gen) return;
    HM.ha_gen = st.gen;
    static home_ha_entity_t tiles[HM_HOME_TILES]; /* the ui component's statics are in PSRAM, not on the stack */
    int n = st.configured ? home_ha_tiles(tiles, HM_HOME_TILES) : 0;
    bool never = st.configured && !st.ok && st.last_ok == 0 && st.err[0]; /* not reached yet: say why */
    bool hint = !st.configured || n == 0 || never;
    if (hint) {
        ui_text(HM.ha_hint_text, "%s", !st.configured ? "Add Home Assistant in settings: its address and a token."
                                       : never ? st.err
                                       : "Pick lights, switches and sensors to show here.");
    }
    show(HM.ha_hint, hint);
    for (int i = 0; i < HM_HOME_TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        if (hint || i >= n) {
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
            bz_icon_set(t->icon, hm_ha_icon(e->kind, e->unit), 32, false);
        }
        ui_text(t->name, "%s", e->name);
        char s[40];
        hm_ha_state_text(e, s, sizeof s);
        ui_text(t->state, "%s", s);
        int lit = e->on && e->actionable;
        if (lit != t->lit) {
            t->lit = lit;
            bz_tile_set_fill(t->tile, lit ? BZ_C_ICE : BZ_C_SURFACE1);
            bz_set_color(t->icon, lit ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->name, lit ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->state, lit ? BZ_C_ON_ICE : BZ_C_DIM);
        }
    }
}

static void refresh_weather(void)
{
    home_weather_t w;
    home_weather_get(&w);
    if (w.gen == HM.wx_gen && HM.wx_gen) return;
    HM.wx_gen = w.gen ? w.gen : 1;
    if (!w.configured || !w.ok) {
        ui_text(HM.wx_temp, "%s", "");
        ui_text(HM.wx_line, "%s", !w.configured ? "set a place for the weather" : w.err[0] ? w.err : "looking up the weather");
        ui_text(HM.wx_place, "%s", w.configured ? w.place : "");
        show(HM.wx_icon, false);
        return;
    }
    const char *g = hm_wx_icon(w.code, w.day);
    if (g != HM.wx_glyph) {
        HM.wx_glyph = g;
        bz_icon_set(HM.wx_icon, g, 40, false);
    }
    show(HM.wx_icon, true);
    ui_text(HM.wx_temp, "%.0f\xc2\xb0", w.temp);
    ui_text(HM.wx_line, "%s \xc2\xb7 %.0f\xc2\xb0 / %.0f\xc2\xb0", home_weather_text(w.code), w.hi, w.lo);
    ui_text(HM.wx_place, "%s", w.place);
}

static void set_art(bool show_it)
{
    if (show_it == HM.art_shown && !show_it) return;
    if (show_it) {
        lv_image_cache_drop(&HM.art_dsc);
        lv_image_set_src(HM.np_img, &HM.art_dsc);
        lv_obj_invalidate(HM.np_img);
        lv_obj_remove_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(HM.np_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(HM.np_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(HM.np_ph, LV_OBJ_FLAG_HIDDEN);
    }
    HM.art_shown = show_it;
}

static void show_playing(bool playing)
{
    if (playing == HM.playing_shown) return;
    HM.playing_shown = playing;
    bz_icon_set(lv_obj_get_child(HM.np_play, 0), playing ? BZ_I_PAUSE : BZ_I_PLAY_ARROW, 24, false);
}

/* the progress bar and its time only while there's a song: without one the hint has the room */
static void show_progress(bool on)
{
    if (on == HM.progress_shown) return;
    HM.progress_shown = on;
    show(HM.np_meter, on);
    show(HM.np_time, on);
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
            HM.pl_gen = pl.gen ? pl.gen : 1;
            ui_text(HM.np_src, "%s", "this tablet \xc2\xb7 microsd");
            ui_text(HM.np_title, "%s", pl.title);
            ui_text(HM.np_artist, "%s", pl.state == HP_LOADING ? "starting" : pl.state == HP_PAUSED ? "paused" : "playing");
            show_playing(pl.state != HP_PAUSED);
            show_progress(true);
        }
        if (tick) {
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
            ui_text(HM.np_artist, "%s", "tap the art for the songs on the card");
        } else if (pc.have) {
            ui_text(HM.np_src, "on the pc \xc2\xb7 %s", pc.app[0] ? pc.app : "media");
            ui_text(HM.np_title, "%s", pc.title[0] ? pc.title : "untitled");
            if (pc.artist[0] && pc.album[0]) ui_text(HM.np_artist, "%s\n%s", pc.artist, pc.album);
            else ui_text(HM.np_artist, "%s", pc.artist[0] ? pc.artist : pc.album);
        } else {
            ui_text(HM.np_src, "%s", pc.link ? "the pc" : "music");
            ui_text(HM.np_title, "%s", "Nothing playing");
            ui_text(HM.np_artist, "%s", pc.link && !pc.available && pc.reason[0] ? pc.reason
                                        : pc.link ? "play something on the pc, or tap the art"
                                        : "pair the pc in settings, home, for its music");
        }
        show_playing(pc.have && pc.playing);
        show_progress(pc.have && pc.duration > 0);
        if (pc.art_gen != HM.art_gen) {
            HM.art_gen = pc.art_gen;
            set_art(HM.art && pc.art && home_pc_art(HM.art));
        }
        if (!pc.have) set_art(false);
    }
    if (tick && HM.progress_shown) {
        double pos = pc.position;
        if (pc.have && pc.playing && pos >= 0) pos += now - pc.at;
        if (pc.duration > 0 && pos > pc.duration) pos = pc.duration;
        char a[16], b[16];
        hm_fmt_time(pos, a, sizeof a);
        hm_fmt_time(pc.duration, b, sizeof b);
        ui_text(HM.np_time, "%s / %s", a, b);
        bz_meter_set(HM.np_meter, pc.duration > 0 ? (float)(pos / pc.duration) : 0, BZ_C_ICE);
    }
}

static void refresh_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_min == HM.last_min) return;
    HM.last_min = tm.tm_min;
    char nx[96];
    if (!ui_match_next_line(nx, sizeof nx)) nx[0] = 0;
    ui_text(HM.match, "%s", nx);
    if (tm.tm_year > 120) {
        /* rolls up into place, unless this is the first time it's set */
        if (HM.clock && lv_label_get_text(HM.clock)[0] != '-' && HM.active && HM.roll.keep) {
            bz_motion_init(&HM.roll, 1, 0.01f);
            HM.roll.keep = true;
            bz_motion_to(&HM.roll, 0, BZ_SMOOTH);
        }
        ui_text(HM.clock, "%d:%02d", tm.tm_hour, tm.tm_min);
        char d[40];
        strftime(d, sizeof d, "%A, %B %e", &tm);
        for (char *c = d; *c; c++) *c = (char)tolower((unsigned char)*c); /* lowercase, like the rest */
        char *dd = strstr(d, "  "); /* %e pads a single day with a space */
        if (dd) memmove(dd, dd + 1, strlen(dd));
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
    /* the network only for what's on screen: with an app over it, the app asks for its own */
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
        /* the tag is a deliberate act: whatever app was open gives way (the stand waits for it instead) */
        if (why == UI_HOME_BY_TAG && ui_app_any_open()) ui_app_close();
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
bool hm_at_home(void)
{
    const char *net = hm_cfg()->net;
    if (!net[0]) return true;
    hal_net_t n;
    hal_net(&n);
    return n.up && !strcmp(n.ssid, net);
}

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
    /* at the shop (another network) the stand never brings it: only at home */
    if (!hm_cfg()->stand || ui_app_any_open() || !HM.power || !still || !hm_at_home()) {
        still_since = 0;
        return;
    }
    if (still_since == 0) still_since = now;
    if (now - still_since > 60 && bz_ui_idle_s() > 45 && (HM.left_at == 0 || now - HM.left_at > 300)) {
        still_since = 0;
        ui_home_mode_trigger(UI_HOME_BY_STAND, true);
    }
}

/* ---- the NFC tag ----
 * The reader's task (hal_tab5_nfc.c) keeps the card in the field; this reads it ten times a second on the UI
 * thread, so home mode is only ever touched from here. The tag arriving asks for home mode at once; it has to
 * be gone TAG_DEBOUNCE_S before home mode hears it left (a tag at the edge of the field flickers). */
static void tag_tick(double now)
{
    static double next;
    if (now < next) return;
    next = now + 0.1;
    if (!hal_nfc_present()) {
        if (HM.tag_pairing == 1) HM.tag_pairing = 0;
        return;
    }
    char uid[24];
    bool seen = hal_nfc_card(uid, sizeof uid);
    hm_cfg_t *c = hm_cfg();
    if (HM.tag_pairing == 1) {
        if (seen) {
            snprintf(c->tag, sizeof c->tag, "%s", uid); /* a UID, never a secret: fine in the log and on screen */
            hm_cfg_save();
            HM.tag_pairing = 2;
            HM.tag_here = true; /* it's in the reader's field now: arriving doesn't count until it's left */
            HM.tag_seen_at = now;
            ui_island_say(BZ_I_HOME, "tag paired: it brings home mode");
        } else if (now > HM.tag_pair_until) {
            HM.tag_pairing = 0;
        }
        return;
    }
    if (!c->tag[0]) return;
    bool mine = seen && !strcmp(uid, c->tag);
    if (mine) {
        HM.tag_seen_at = now;
        if (!HM.tag_here) {
            HM.tag_here = true;
            if (hm_at_home()) ui_home_mode_trigger(UI_HOME_BY_TAG, true);
        }
    } else if (HM.tag_here && now - HM.tag_seen_at > TAG_DEBOUNCE_S) {
        HM.tag_here = false;
        ui_home_mode_trigger(UI_HOME_BY_TAG, false);
    }
}

void hm_tag_pair_begin(void)
{
    HM.tag_pairing = 1;
    HM.tag_pair_until = hal_seconds() + TAG_PAIR_S;
}

void hm_tag_pair_cancel(void)
{
    if (HM.tag_pairing == 1) HM.tag_pairing = 0;
}

int hm_tag_pair_state(void)
{
    if (!hal_nfc_present()) return -1;
    int s = HM.tag_pairing;
    if (s == 2) HM.tag_pairing = 0;
    return s;
}

/* ---- the photos screensaver: the card's pictures after a while untouched, on home mode only ---- */
static void saver_tick(void)
{
    int min = hm_cfg()->saver_min;
    double idle = bz_ui_idle_s();
    if (idle < 5) HM.saver_on = false; /* touched: the next idle spell may start it again */
    if (!min || HM.saver_on || !HM.active || HM.sheet || HM.want || ui_app_any_open() || ui_asleep()) return;
    if (idle < min * 60.0 || !hal_sd_root()) return;
    HM.saver_on = true;
    ui_photos_slideshow(NULL);
}

/* The eyes: blinks on a spring (now and then a double), a gaze that wanders and settles, and on power a
 * slow breath. Each frame sets only what moved, so a still face costs nothing. */
static void eyes_tick(double now)
{
    if (!HM.active || HM.sheet || ui_app_any_open() || ui_asleep() || !HM.eye[0]) return;
    if (now >= HM.blink_at && HM.lid.target > 0.5f) {
        bz_motion_to(&HM.lid, 0, BZ_TICK);
        HM.blinking = true;
    }
    if (HM.blinking && HM.lid.value < 0.08f) {
        HM.blinking = false;
        bz_motion_to(&HM.lid, 1, BZ_SMOOTH);
        bool twice = rand() % 6 == 0;
        HM.blink_at = now + (twice ? 0.28 : 3.5 + (rand() % 5500) / 1000.0);
    }
    if (now >= HM.gaze_at) {
        /* mostly ahead, sometimes a look to a side or up at whoever is there */
        int r = rand() % 10;
        float x = r < 5 ? 0 : (float)((rand() % 21) - 10), y = r < 5 ? 0 : (float)((rand() % 11) - 6);
        bz_motion_to(&HM.gx, x, BZ_SMOOTH);
        bz_motion_to(&HM.gy, y, BZ_SMOOTH);
        HM.gaze_at = now + 2.5 + (rand() % 4000) / 1000.0;
    }
    bool moving = bz_motion_tick(&HM.lid);
    moving |= bz_motion_tick(&HM.gx);
    moving |= bz_motion_tick(&HM.gy);
    float breath = HM.power ? 1.0f + 0.035f * sinf((float)(now * 2 * M_PI / 4.2)) : 1.0f;
    /* the breath needs no frame of its own: the loop's idle pace (~30 Hz) is plenty for a 4 s swell */
    float lid = HM.lid.value < 0 ? 0 : HM.lid.value > 1 ? 1 : HM.lid.value;
    for (int i = 0; i < 2; i++) {
        int h = (int)lroundf(EYE_H * breath * (0.1f + 0.9f * lid));
        if (h < 6) h = 6;
        int x = (i ? 1 : -1) * EYE_DX + (int)lroundf(HM.gx.value);
        int y = EYE_Y + (EYE_H - h) / 2 + (int)lroundf(HM.gy.value);
        if (h != HM.eye_h[i]) {
            HM.eye_h[i] = h;
            lv_obj_set_height(HM.eye[i], h);
        }
        if (x != HM.eye_x[i] || y != HM.eye_y[i]) {
            HM.eye_x[i] = x;
            HM.eye_y[i] = y;
            lv_obj_align(HM.eye[i], LV_ALIGN_TOP_MID, x, y);
        }
    }
    if (moving) bz_ui_keep_alive();
}

/* A new minute rolls up into place */
static void roll_tick(void)
{
    if (!bz_motion_tick(&HM.roll) && HM.roll_px == 0 && HM.roll_opa == 255) return;
    float k = HM.roll.value < 0 ? 0 : HM.roll.value;
    int px = (int)lroundf(22 * k), opa = (int)lroundf(255 * (1 - k));
    if (px != HM.roll_px) {
        HM.roll_px = px;
        lv_obj_set_style_translate_y(HM.clock, px, 0);
    }
    if (opa != HM.roll_opa) {
        HM.roll_opa = opa;
        lv_obj_set_style_opa(HM.clock, (lv_opa_t)opa, 0);
    }
    bz_ui_keep_alive();
}

/* The launcher's tiles sink a little under a finger and spring back */
static void press_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) bz_motion_to(&HM.press[i], 1, BZ_HOLD);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) bz_motion_to(&HM.press[i], 0, BZ_RELEASE);
    bz_ui_keep_alive();
}

static void press_tick(void)
{
    for (int i = 0; i < NLAUNCH && i < 12; i++) {
        if (!HM.tile[i]) continue;
        bool moving = bz_motion_tick(&HM.press[i]);
        int sc = 256 - (int)lroundf(18 * HM.press[i].value);
        if (sc != HM.press_scale[i]) {
            HM.press_scale[i] = sc;
            lv_obj_set_style_transform_scale(HM.tile[i], sc, 0);
        }
        if (moving) bz_ui_keep_alive();
    }
}

static void hm_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    now = hal_seconds();
    stand_tick(now);
    tag_tick(now);
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
    saver_tick();
    eyes_tick(now);
    roll_tick();
    press_tick();
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
