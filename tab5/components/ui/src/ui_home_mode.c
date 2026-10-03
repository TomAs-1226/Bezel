/* Home mode: the surface, its pages, its triggers and its settings store.
 *
 * Three pages, side by side, paged with a swipe as Catalyst mode's are (a picture of the page under the finger,
 * the one it reveals drawn offscreen a band per frame, both moved by block copies; nothing redrawn while it moves):
 *   the clock faces   ui_home_faces.c: a dial, a horizon, dots, words, the moon (a flick up or down for the next)
 *   the desk          here: the time, the weather's sky, what's playing, the companion, Home Assistant, the launcher
 *   now playing       ui_home_np.c: the cover big on a surface tinted from it, the transport
 * Home mode opens on the page settings choose (the desk unless told otherwise). A dot row at the foot says where.
 *
 * The desk, 1280 × 720 at the tablet's type size (BZ_LEAN), top to bottom:
 *   a greeting, the time (large), the date, the next match          the weather: a tile of today's sky
 *   what's playing (its cover, title, progress, controls) · the companion's face · four Home Assistant tiles
 *   the launcher: music, smart home, weather, calendar, timer, alarms, photos, companion, settings, catalyst
 * With something playing (and "colour from the music" on, the default), the desk takes its colours from the
 * cover: the ground and every card tinted by its seed at fixed OKLCH tones (ui_home_art.c, home_art.c), so the
 * surface follows the song the way Bezel's Monet follows the album, and text keeps its contrast. A song without a
 * cover gets a generated one.
 *
 * Settings > look chooses which cards show, the clock, the ground and the pages' options; settings > home the
 * rest. hm_layout() places the desk for the choice, once per change.
 *
 * Cheap to keep on screen: nothing animates at rest but the dial's second hand (its own small box, a quarter of a
 * second each second; it can be turned off). The clock changes once a minute; the progress and its time once a
 * second while something plays; a tile only when its entity changes; the cover and the tint once a song; the
 * companion blinks every few seconds (two small eyes). Nothing is restyled unless its state changed. The pictures
 * (the covers, the weather's sky, a face's picture) are painted on a worker and freed when home mode is left.
 *
 * Coming and going is a bottom sheet the platform slides (bz_ui_sheet_*, as an app opens), drawn a band at a
 * time; with no platform sheet (the simulator) it just switches. Apps open over it and close back to it (the
 * shell keeps its status band, orb and dock away while home mode is active). */
#include "ui_home_priv.h"

#include "home_px.h"
#include "ui_companion.h"
#include "ui_home_face.h"
#include "ui_pet.h"
#include "ui_intel.h"
#include "voice.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define M 56                        /* the surface's side margin */
#define TOP_Y 40                    /* the greeting's top; the time under it, the date under that */
#define WX_W 392                    /* the weather's tile, top right under the orb */
#define WX_H 150
#define WX_X (W - M - WX_W)
#define WX_Y 82
#define WX_R 28
#define CARD_Y 250
#define CARD_H 268
#define CARD_IN_H (CARD_H - 2 * BZ_PAD_TILE)
#define NP_W 600                    /* now playing */
#define CP_W 214                    /* the companion */
#define HT_H ((CARD_H - BZ_GAP) / 2)
#define CLOCK_DY 18                 /* the time's top under the greeting's */
#define DATE_DY 114                 /* the date's top under the big clock's (the display face: less by its size) */
#define HT_PAD 16
#define ART HOME_ART_S              /* 180 */
#define NP_TX (ART + 26)            /* the text column, inside the card */
#define NP_TW (NP_W - 2 * BZ_PAD_TILE - NP_TX) /* 346 */
#define TIME_W 110
#define CTL_W 56                    /* five transport buttons and a spacer fill NP_TW exactly */
#define CTL_GAP 8
#define CTL_SPACER (NP_TW - 5 * CTL_W - 5 * CTL_GAP)
#define EYE_W 38
#define EYE_H 62
#define EYE_DX 36
#define EYE_Y 34
#define LN_Y (CARD_Y + CARD_H + 24) /* the launcher */
#define LN_H 104
#define LN_GAP 10
#define LN_W ((W - 2 * M - 9 * LN_GAP) / 10)
#define LN_X (M + (W - 2 * M - 10 * LN_W - 9 * LN_GAP) / 2)
#define DOTS_Y (H - 24)
#define TAG_DEBOUNCE_S 1.5          /* the tag gone this long before home mode hears it left */
#define TAG_PAIR_S 30.0             /* how long "pair a tag" waits for one */
#define NIGHT_IDLE_S 60.0           /* untouched this long at night: the night face */
#define SL_BANDS 8

/* ================================================================== settings store */

static hm_cfg_t *CFG;
static bool g_settings_wanted;

static bool kv_bool(const char *k, bool def)
{
    char v[8];
    return hal_kv_get(k, v, sizeof v) ? v[0] == '1' : def;
}

static int kv_int(const char *k, int def)
{
    char v[12];
    return hal_kv_get(k, v, sizeof v) && v[0] ? atoi(v) : def;
}

hm_cfg_t *hm_cfg(void)
{
    if (CFG) return CFG;
    CFG = calloc(1, sizeof *CFG); /* ~2 KB: PSRAM, not a static in internal RAM */
    if (!CFG) abort();            /* at start-up, with 32 MB of PSRAM: never */
    CFG->start_home = kv_bool("hm_start", false);
    CFG->stand = kv_bool("hm_stand", false);
    /* Fahrenheit and Mission Viejo: this tablet's desk. Defaults, not fixed values -- the settings
     * screen still changes both, and a place typed there is geocoded as before. Without a default
     * place the weather card shows "not set" on a first boot, which is a card that does nothing.
     * Clearing the place in settings brings this one back: the price of a default over a stored value. */
    CFG->fahrenheit = kv_bool("hm_fahr", true);
    CFG->pc = kv_bool("hm_pc", true);
    char v[8];
    CFG->saver_min = hal_kv_get("hm_saver", v, sizeof v) ? atoi(v) : 0;
    hal_kv_get("hm_tag", CFG->tag, sizeof CFG->tag);
    hal_kv_get("hm_net", CFG->net, sizeof CFG->net);
    if (!hal_kv_get("hm_place", CFG->place, sizeof CFG->place) || !CFG->place[0])
        snprintf(CFG->place, sizeof CFG->place, "%s", HM_PLACE_DEFAULT);
    hal_kv_get("ha_url", CFG->ha_url, sizeof CFG->ha_url);
    hal_kv_get("ha_token", CFG->ha_token, sizeof CFG->ha_token);
    hal_kv_get("ha_picks", CFG->picks, sizeof CFG->picks);
    CFG->cards = hal_kv_get("hm_cards", v, sizeof v) ? atoi(v) & HM_CARD_ALL : HM_CARD_ALL;
    CFG->small_clock = kv_bool("hm_clock", false);
    CFG->seconds = kv_bool("hm_secs", false);
    CFG->tint = kv_bool("hm_bg", false);
    CFG->art_tint = kv_bool("hm_art", true);
    /* the faces by name ("hm_fset" "type,flip,dial", "hm_fshow" "type"), so their order can change; the first
     * round's numbered keys are read once for the face shown */
    char fs[200];
    CFG->faces = 0;
    if (hal_kv_get("hm_fset", fs, sizeof fs) && fs[0]) {
        char *save = NULL;
        for (char *tok = strtok_r(fs, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            int f = hm_face_by_name(tok);
            if (f >= 0 && f < HM_NFACES) CFG->faces |= 1 << f;
        }
    }
    if (!CFG->faces) CFG->faces = HM_FACES_ALL;
    CFG->face = -1;
    if (hal_kv_get("hm_fshow", fs, sizeof fs)) CFG->face = hm_face_by_name(fs);
    if (CFG->face < 0) {
        static const char *const OLD[5] = { "dial", "horizon", "dots", "words", "moon" };
        int o = kv_int("hm_face", -1);
        CFG->face = o >= 0 && o < 5 ? hm_face_by_name(OLD[o]) : HM_FACE_TYPE;
    }
    if (CFG->face < 0 || CFG->face >= HM_NFACES) CFG->face = HM_FACE_TYPE;
    if (!(CFG->faces >> CFG->face & 1)) CFG->faces |= 1 << CFG->face;
    CFG->open_page = kv_int("hm_open", HM_PG_DESK);
    if (CFG->open_page < 0 || CFG->open_page >= HM_NPAGES) CFG->open_page = HM_PG_DESK;
    CFG->night = kv_bool("hm_night", false);
    CFG->idle_pet = kv_bool("hm_idlepet", false);
    CFG->dial_secs = kv_bool("hm_dsec", true);
    CFG->cam_on = kv_bool("hm_cam_on", false); /* off until the owner turns it on */
    CFG->cam_min = hal_kv_get("hm_cam_min", v, sizeof v) ? atoi(v) : 5;
    CFG->cam_keep = hal_kv_get("hm_cam_keep", v, sizeof v) ? atoi(v) : HOME_CAP_KEEP_DEFAULT;
    return CFG;
}

void hm_cfg_apply(void)
{
    hm_cfg_t *c = hm_cfg();
    home_pc_enable(c->pc);
    home_weather_config(c->place, c->fahrenheit);
    home_ha_config(c->ha_url, c->ha_token, c->picks);
    home_capture_config(c->cam_on, c->cam_min, c->cam_keep);
}

static void kv_set_int(const char *k, int n)
{
    char v[12];
    snprintf(v, sizeof v, "%d", n);
    hal_kv_set(k, v);
}

void hm_cfg_save(void)
{
    hm_cfg_t *c = hm_cfg();
    hal_kv_set("hm_start", c->start_home ? "1" : "0");
    hal_kv_set("hm_stand", c->stand ? "1" : "0");
    hal_kv_set("hm_fahr", c->fahrenheit ? "1" : "0");
    hal_kv_set("hm_pc", c->pc ? "1" : "0");
    kv_set_int("hm_saver", c->saver_min);
    hal_kv_set("hm_tag", c->tag);
    hal_kv_set("hm_net", c->net);
    hal_kv_set("hm_place", c->place);
    hal_kv_set("ha_url", c->ha_url);
    hal_kv_set("ha_token", c->ha_token);
    hal_kv_set("ha_picks", c->picks);
    kv_set_int("hm_cards", c->cards);
    hal_kv_set("hm_clock", c->small_clock ? "1" : "0");
    hal_kv_set("hm_secs", c->seconds ? "1" : "0");
    hal_kv_set("hm_bg", c->tint ? "1" : "0");
    hal_kv_set("hm_art", c->art_tint ? "1" : "0");
    char fs[200];
    int k = 0;
    fs[0] = 0;
    for (int i = 0; i < HM_NFACES; i++)
        if (c->faces >> i & 1) k += snprintf(fs + k, sizeof fs - (size_t)k, "%s%s", k ? "," : "", HM_FACE_NAME[i]);
    hal_kv_set("hm_fset", fs);
    hal_kv_set("hm_fshow", c->face >= 0 && c->face < HM_NFACES ? HM_FACE_NAME[c->face] : "type");
    kv_set_int("hm_open", c->open_page);
    hal_kv_set("hm_night", c->night ? "1" : "0");
    hal_kv_set("hm_idlepet", c->idle_pet ? "1" : "0");
    hal_kv_set("hm_dsec", c->dial_secs ? "1" : "0");
    hal_kv_set("hm_cam_on", c->cam_on ? "1" : "0");
    kv_set_int("hm_cam_min", c->cam_min);
    kv_set_int("hm_cam_keep", c->cam_keep);
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

/* ================================================================== small shared pieces */

double hm_now(void) { return hal_seconds(); }

void hm_show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void hm_fill(lv_obj_t *o, uint32_t rgb)
{
    if (!o) return;
    if (rgb == HM_ROLE) lv_obj_remove_local_style_prop(o, LV_STYLE_BG_COLOR, 0);
    else lv_obj_set_style_bg_color(o, bz_lv_rgb(rgb), 0);
}

void hm_ink(lv_obj_t *o, uint32_t rgb)
{
    if (!o) return;
    if (rgb == HM_ROLE) lv_obj_remove_local_style_prop(o, LV_STYLE_TEXT_COLOR, 0);
    else lv_obj_set_style_text_color(o, bz_lv_rgb(rgb), 0);
}

uint16_t hm_565(uint32_t rgb) { return lv_color_to_u16(bz_lv_rgb(rgb)); }

/* ---- the song on show ---- */

static struct {
    unsigned gen;
    char sig[400];
    uint32_t key;             /* the cover's identity for the song on show (0: none yet) */
} SONG;

void hm_song(hm_song_t *s)
{
    memset(s, 0, sizeof *s);
    home_play_t pl;
    home_player_get(&pl);
    double now = hm_now();
    s->local = pl.state == HP_PLAYING || pl.state == HP_PAUSED || pl.state == HP_LOADING;
    s->pos = s->dur = -1;
    if (s->local) {
        s->have = true;
        s->can = true;
        s->playing = pl.state != HP_PAUSED;
        snprintf(s->src, sizeof s->src, "%s", "this tablet \xc2\xb7 microsd");
        snprintf(s->title, sizeof s->title, "%s", pl.title);
        snprintf(s->artist, sizeof s->artist, "%s", pl.state == HP_LOADING ? "starting" : pl.state == HP_PAUSED ? "paused" : "playing");
        s->pos = pl.position;
        s->dur = pl.duration;
    } else {
        home_pc_t pc;
        home_pc_get(&pc);
        if (!hm_cfg()->pc) {
            snprintf(s->src, sizeof s->src, "%s", "music");
            snprintf(s->hint, sizeof s->hint, "%s", "tap the cover for the songs on the card");
        } else if (pc.have) {
            s->have = true;
            s->can = true;
            s->playing = pc.playing;
            snprintf(s->src, sizeof s->src, "on the pc \xc2\xb7 %s", pc.app[0] ? pc.app : "media");
            snprintf(s->title, sizeof s->title, "%s", pc.title[0] ? pc.title : "untitled");
            snprintf(s->artist, sizeof s->artist, "%s", pc.artist);
            snprintf(s->album, sizeof s->album, "%s", pc.album);
            s->pos = pc.position;
            if (pc.playing && s->pos >= 0) s->pos += now - pc.at;
            s->dur = pc.duration;
            if (s->dur > 0 && s->pos > s->dur) s->pos = s->dur;
        } else {
            snprintf(s->src, sizeof s->src, "%s", pc.link ? "the pc" : "music");
            snprintf(s->hint, sizeof s->hint, "%s",
                     pc.link && !pc.available && pc.reason[0] ? pc.reason
                     : pc.link ? "play something on the pc, or tap the cover"
                               : "pair the pc in settings, home, for its music");
        }
    }
    /* the generation: anything but the position */
    char sig[sizeof SONG.sig];
    snprintf(sig, sizeof sig, "%d%d%d|%s|%s|%s|%s|%s|%.0f", s->local, s->have, s->playing, s->src, s->title, s->artist,
             s->album, s->hint, s->dur);
    if (strcmp(sig, SONG.sig)) {
        snprintf(SONG.sig, sizeof SONG.sig, "%s", sig);
        SONG.gen++;
    }
    s->gen = SONG.gen;
}

const hm_art_t *hm_song_art(const hm_song_t *s)
{
    if (!s->have) return NULL;
    if (s->local) return hm_art_song(NULL, NULL, s->title);
    home_pc_t pc;
    home_pc_get(&pc);
    return hm_art_pc(&pc);
}

void hm_cmd(const char *a)
{
    home_play_t pl;
    home_player_get(&pl);
    bool local = pl.state == HP_PLAYING || pl.state == HP_PAUSED || pl.state == HP_LOADING;
    if (local) {
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

/* ---- the tint: what's playing, when it's on ---- */

static struct {
    home_tint_t t;
    bool on;
    uint32_t sig;
    unsigned gen;
} TINT;

const home_tint_t *hm_tint(unsigned *gen)
{
    if (gen) *gen = TINT.gen;
    return TINT.on ? &TINT.t : NULL;
}

/* from the refresh: the tint follows the cover on show (a song without one yet keeps the last until it's painted) */
static void tint_update(const hm_art_t *a, bool have)
{
    bool on = hm_cfg()->art_tint && have;
    bool dark = bz_ui_dark();
    uint32_t seed = a ? hm_art_info(a)->seed : 0;
    if (on && !a) return; /* its cover is being painted: the colours follow it when it's there */
    uint32_t sig = on ? (seed * 2654435761u) ^ (dark ? 1u : 2u) : 0;
    if (on == TINT.on && sig == TINT.sig) return;
    TINT.on = on;
    TINT.sig = sig;
    if (on) home_art_tint(seed, dark, &TINT.t);
    TINT.gen++;
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
    lv_obj_t *root, *pg[HM_NPAGES];
    int page;                      /* the page on screen */
    lv_obj_t *desk, *greet, *clock_row, *clock, *secs, *date, *match;
    lv_obj_t *wx, *wx_img, *wx_temp, *wx_line, *wx_place;
    lv_obj_t *np, *np_art, *np_img, *np_ph, *np_src, *np_title, *np_artist, *np_meter, *np_time, *np_play;
    lv_obj_t *np_btn[5];
    lv_obj_t *eye[2], *face, *cp_state, *cp_name;
    lv_obj_t *ha_hint, *ha_hint_text;
    ha_tile_t ha[HM_HOME_TILES];
    unsigned art_shown, song_gen, wx_gen, ha_gen, tint_gen;
    int last_min, last_sec, secs_shown;
    uint32_t ground_sig;           /* the desk's ground as last painted */
    bool playing_shown, progress_shown, have_shown;
    const char *wx_glyph;
    /* the weather's sky */
    struct {
        uint16_t *px;
        lv_image_dsc_t dsc;
        unsigned ticket;
        bool painted;
        float hour, rise, set;
        int code;
        bool dark, day;
        uint16_t ground;
        uint32_t sig;
        int ink;                   /* the text on it: 1 light, 0 dark, -1 not decided */
    } sky;
    /* mode */
    bool built, active, sheet;
    int want;                      /* +1 enter, -1 leave, 0 nothing pending */
    ui_home_why_t why;             /* what brought it (an automatic trigger may take it away again) */
    double left_at;                /* hal_seconds() when it was last left by hand */
    int hid_page;                  /* the Catalyst page hidden under it (-1 none) */
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
    /* night */
    bool night_on;
    int night_page, night_face, night_force; /* where it was before; the dev console's override (-1 none) */
    /* the pet when idle: where it was before */
    bool idle_on;
    int idle_page, idle_face;
    /* the NFC tag */
    char cp_state_text[96];        /* the companion card's second line as last written */
    int tag_pairing;               /* 0 no, 1 waiting for a tag, 2 just paired */
    double tag_pair_until;
    bool tag_here;
    double tag_seen_at;
    int go_want;                   /* a page asked for without a finger (-1 none) */
} HM = { .hid_page = -1, .page = HM_PG_DESK, .go_want = -1, .night_force = -1 };

/* the page slide */
static struct {
    bool active, settling, ready, jump, plain;
    int side, band, nb_page, aim, prep_page;
    bz_motion_t x;
    uint32_t chrome;               /* the top layer's pieces hidden for a band, to put back */
} SL = { .prep_page = -1 };

bool ui_home_mode_active(void) { return HM.active; }
/* Home mode is what a person in front of the tablet is looking at: it is on, its sheet has settled, the
 * screen is awake, and nothing is open over it — except its own photo screensaver, which is still home
 * mode wearing a different coat. The microphones follow this, so the wake word is live exactly when the
 * surface that advertises it is showing. */
bool ui_home_mode_showing(void)
{
    return HM.active && !HM.sheet && !ui_asleep() && (!ui_app_any_open() || HM.saver_on);
}
bool ui_home_mode_keeps_awake(void) { return HM.active && HM.power; }
double ui_home_mode_presence_idle_s(double now) { return home_presence_idle_s(now); }

void hm_open_settings(lv_obj_t *from)
{
    hm_settings_request();
    ui_app_open(&APP_SETTINGS, from);
}

static void hm_go(int page);

static void launch_tap(lv_obj_t *o, void *u)
{
    const hm_app_t *a = u;
    if (a->app) ui_app_open(a->app, o);
    else if (a->action == 1) ui_companion_open(false, o);
    else if (a->action == 2) hm_open_settings(o);
    else ui_home_mode_exit();
}

static void open_tap(lv_obj_t *o, void *u) { ui_app_open((const ui_app_t *)u, o); }

/* the cover on the desk: what's playing, big, on its own page */
static void art_tap(lv_obj_t *o, void *u)
{
    (void)u;
    hm_song_t s;
    hm_song(&s);
    if (s.have) hm_go(HM_PG_NP);
    else ui_app_open(&APP_MUSIC, o);
}

static void face_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_companion_talk(o); /* the card says "tap to talk": it opens listening */
}

static void np_cmd(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cmd((const char *)u);
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

/* a card: surface1 with the tile's padding, placed on the desk */
static lv_obj_t *card(lv_obj_t *r, int x, int w)
{
    lv_obj_t *c = bz_tile(r, w, CARD_H);
    lv_obj_set_pos(c, x, CARD_Y);
    return c;
}

static void build_top(lv_obj_t *r)
{
    /* the time, left: the minutes, and the seconds beside them in the quiet colour when asked for (placed by
     * hm_layout) */
    HM.greet = bz_label_line(r, "", BZ_F_LABEL, BZ_C_DIM, 420);
    lv_obj_set_pos(HM.greet, M + 2, TOP_Y);
    HM.clock_row = bz_row(r, 10);
    lv_obj_set_flex_align(HM.clock_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_pos(HM.clock_row, M - 6, TOP_Y + CLOCK_DY);
    HM.clock = bz_label(HM.clock_row, "--:--", BZ_F_CLOCK, BZ_C_INK);
    HM.secs = bz_label(HM.clock_row, "00", BZ_F_TITLE, BZ_C_DIM);
    lv_obj_add_flag(HM.secs, LV_OBJ_FLAG_HIDDEN);
    HM.secs_shown = -1;
    HM.date = bz_label_line(r, "", BZ_F_BODY, BZ_C_DIM, 560);
    HM.match = bz_label_line(r, "", BZ_F_LABEL, BZ_C_SIGNAL, 560); /* the team's next match (ui_match.c) */

    /* the weather: a tile of the sky as it is (its colour through the day, the sun or the moon, clouds, rain),
     * painted on the worker, with the temperature and the day's words on it; a tap opens the forecast */
    lv_obj_t *wx = bz_box(r);
    HM.wx = wx;
    lv_obj_set_size(wx, WX_W, WX_H);
    lv_obj_set_pos(wx, WX_X, WX_Y);
    lv_obj_add_flag(wx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(wx, 8);
    bz_on_tap(wx, open_tap, (void *)&APP_WEATHER);
    HM.wx_img = lv_image_create(wx);
    lv_obj_set_pos(HM.wx_img, 0, 0);
    lv_obj_add_flag(HM.wx_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(HM.wx_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    HM.wx_temp = bz_label(wx, "", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(HM.wx_temp, 26, 20);
    HM.wx_line = bz_label_line(wx, "", BZ_F_BODY_S, BZ_C_INK, WX_W - 52);
    lv_obj_set_pos(HM.wx_line, 28, 86);
    HM.wx_place = bz_label_line(wx, "", BZ_F_CAPTION, BZ_C_DIM, WX_W - 52);
    lv_obj_set_pos(HM.wx_place, 28, 114);
    HM.sky.ink = -1;
    lv_image_dsc_t *d = &HM.sky.dsc;
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = WX_W;
    d->header.h = WX_H;
    d->header.stride = WX_W * 2;
    d->data_size = WX_W * WX_H * 2;
}

static void build_playing(lv_obj_t *r)
{
    lv_obj_t *np = card(r, M, NP_W);
    HM.np = np;
    lv_obj_t *artbox = bz_tile(np, ART, ART);
    HM.np_art = artbox;
    lv_obj_add_style(artbox, bz_style_fill(BZ_C_SURFACE2), 0);
    lv_obj_set_style_radius(artbox, 18, 0);
    lv_obj_set_style_pad_all(artbox, 0, 0);
    lv_obj_set_style_clip_corner(artbox, true, 0);
    lv_obj_set_pos(artbox, 0, (CARD_IN_H - ART) / 2);
    lv_obj_add_flag(artbox, LV_OBJ_FLAG_CLICKABLE); /* the cover: now playing, big */
    bz_on_tap(artbox, art_tap, NULL);
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
    HM.np_btn[0] = round_button(ctl, BZ_I_ARROW_BACK, "previous");
    HM.np_btn[1] = HM.np_play = round_button(ctl, BZ_I_PLAY_ARROW, "toggle");
    ui_chip_set(HM.np_play, true);
    HM.np_btn[2] = round_button(ctl, BZ_I_ARROW_FORWARD, "next");
    lv_obj_t *gap = bz_box(ctl);
    lv_obj_set_size(gap, CTL_SPACER, 1);
    HM.np_btn[3] = round_button(ctl, BZ_I_REMOVE, "volume_down");
    HM.np_btn[4] = round_button(ctl, BZ_I_ADD, "volume_up");
}

static void build_companion(lv_obj_t *r)
{
    lv_obj_t *cp = card(r, M + NP_W + BZ_GAP, CP_W);
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
    HM.cp_name = bz_label_line(cp, "companion", BZ_F_NAME, BZ_C_INK, CP_W - 2 * BZ_PAD_TILE);
    lv_obj_align(HM.cp_name, LV_ALIGN_BOTTOM_LEFT, 0, -26);
    lv_obj_t *cc = bz_label_line(cp, "tap to talk", BZ_F_CAPTION, BZ_C_DIM, CP_W - 2 * BZ_PAD_TILE);
    lv_obj_align(cc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    HM.cp_state = cc; /* voice_tick() rewrites it: it is how a person can tell the microphones are open */
}

static void build_home(lv_obj_t *r)
{
    /* up to four picked entities, two by two; the smart home app has them all */
    for (int i = 0; i < HM_HOME_TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        t->tile = bz_tile(r, 100, HT_H);
        lv_obj_set_style_pad_all(t->tile, HT_PAD, 0);
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t->tile, ha_tap, (void *)(intptr_t)i);
        t->icon = bz_icon(t->tile, BZ_I_HOME, 32, BZ_C_INK);
        t->state = bz_label_line(t->tile, "", BZ_F_LABEL, BZ_C_DIM, 60);
        lv_obj_set_style_text_align(t->state, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(t->state, LV_ALIGN_TOP_RIGHT, 0, 8);
        t->name = bz_label_line(t->tile, "", BZ_F_BODY_S, BZ_C_INK, 60);
        lv_obj_align(t->name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        t->kind = -1;
        t->lit = -1;
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_HIDDEN);
    }
    /* before anything is picked (or reachable): what to do, and a tap into the app */
    HM.ha_hint = card(r, 0, 100);
    lv_obj_add_flag(HM.ha_hint, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(HM.ha_hint, open_tap, (void *)&APP_SMARTHOME);
    bz_icon(HM.ha_hint, BZ_I_LIGHTBULB, 32, BZ_C_DIM);
    lv_obj_t *hn = bz_label_line(HM.ha_hint, "smart home", BZ_F_NAME, BZ_C_INK, 100);
    lv_obj_set_pos(hn, 0, 56);
    HM.ha_hint_text = bz_label(HM.ha_hint, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_label_set_long_mode(HM.ha_hint_text, LV_LABEL_LONG_DOT);
    lv_obj_set_size(HM.ha_hint_text, 100, 4 * lv_font_get_line_height(bz_font(BZ_F_BODY_S)));
    lv_obj_set_pos(HM.ha_hint_text, 0, 98);
}

static void press_cb(lv_event_t *e);

/* A launcher tile's picture: its glyph over its word. The one place the launcher draws an icon, so the shell's
 * own icon style can take it over. */
/* The app's own icon, the one the library and the home page show (ui_icon.c: 64 px is baked already and
 * shared); the way back to the catalyst pages, which is no app, keeps its glyph. */
static void launcher_icon(lv_obj_t *t, const hm_app_t *a)
{
    lv_obj_t *ic = a->app ? ui_app_icon(t, a->app, 64) : ui_icon_named(t, a->label, 64);
    if (ic) {
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 9);
    } else {
        ic = bz_icon(t, a->icon, 32, BZ_C_INK);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 22);
    }
    lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lb = bz_label_line(t, a->label, BZ_F_CAPTION, BZ_C_DIM, LN_W - 6);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -9);
}

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
        launcher_icon(t, a);
    }
}

/* The smart home tiles and their hint, in an area `w` wide from x */
static void layout_home(int x, int w)
{
    int tw = (w - BZ_GAP) / 2;
    for (int i = 0; i < HM_HOME_TILES; i++) {
        ha_tile_t *t = &HM.ha[i];
        lv_obj_set_pos(t->tile, x + (i % 2) * (tw + BZ_GAP), CARD_Y + (i / 2) * (HT_H + BZ_GAP));
        lv_obj_set_width(t->tile, tw);
        lv_obj_set_width(t->state, tw - 2 * HT_PAD - 50);
        lv_obj_set_width(t->name, tw - 2 * HT_PAD);
    }
    lv_obj_set_pos(HM.ha_hint, x, CARD_Y);
    lv_obj_set_width(HM.ha_hint, w);
    lv_obj_set_width(lv_obj_get_child(HM.ha_hint, 1), w - 2 * BZ_PAD_TILE);
    lv_obj_set_width(HM.ha_hint_text, w - 2 * BZ_PAD_TILE);
}

/* Everything placed for the look: which cards, which clock. Once per change (each set redraws). */
static void hm_layout(void)
{
    hm_cfg_t *c = hm_cfg();
    /* the time: the big face or the display face, the date and the next match under it */
    bz_set_font(HM.clock, c->small_clock ? BZ_F_DISPLAY : BZ_F_CLOCK);
    hm_show(HM.secs, c->seconds);
    HM.secs_shown = -1;
    int big = lv_font_get_line_height(bz_font(BZ_F_CLOCK)), now = lv_font_get_line_height(bz_font(c->small_clock ? BZ_F_DISPLAY : BZ_F_CLOCK));
    int date_y = TOP_Y + CLOCK_DY + DATE_DY - (big - now);
    lv_obj_set_pos(HM.date, M + 2, date_y);
    lv_obj_set_pos(HM.match, M + 2, date_y + 32);
    hm_show(HM.match, c->cards & HM_CARD_MATCH);
    hm_show(HM.wx, c->cards & HM_CARD_WEATHER);
    /* the middle row closes up: music and the companion keep their widths, the last card shown takes the rest */
    bool mu = c->cards & HM_CARD_MUSIC, cp = c->cards & HM_CARD_COMPANION, ha = c->cards & HM_CARD_HOME;
    int x = M;
    hm_show(HM.np, mu);
    if (mu) {
        lv_obj_set_pos(HM.np, x, CARD_Y);
        x += NP_W + BZ_GAP;
    }
    hm_show(HM.face, cp);
    if (cp) {
        lv_obj_set_pos(HM.face, x, CARD_Y);
        lv_obj_set_width(HM.face, ha ? CP_W : W - M - x);
        x += CP_W + BZ_GAP;
    }
    if (ha) layout_home(x, W - M - x);
    else {
        hm_show(HM.ha_hint, false);
        for (int i = 0; i < HM_HOME_TILES; i++) {
            hm_show(HM.ha[i].tile, false);
            HM.ha[i].used = false;
        }
    }
    /* the seconds sit on the minutes' baseline, whichever face */
    const lv_font_t *cf = bz_font(c->small_clock ? BZ_F_DISPLAY : BZ_F_CLOCK), *sf = bz_font(BZ_F_TITLE);
    lv_obj_set_style_translate_y(HM.secs, -(int)(cf->base_line - sf->base_line), 0);
    HM.ha_gen = 0; /* the tiles and the hint shown again, as the entities say */
}

/* The desk's colours: its ground (the song's tint, else the accent's when asked, else Bezel's) and its cards
 * (the tint's surface, else their roles). Only when something changed. */
static void desk_colors(void)
{
    unsigned tg;
    const home_tint_t *t = hm_tint(&tg);
    bool accent = hm_cfg()->tint;
    uint32_t sig = (t ? t->ground * 3u + 1u : accent ? bz_color(BZ_C_ICE) ^ 0x1000000u : 0) ^ (bz_ui_dark() ? 0x2000000u : 0);
    if (sig == HM.ground_sig) return;
    HM.ground_sig = sig;
    if (t) hm_fill(HM.desk, t->ground);
    else if (accent) hm_fill(HM.desk, bz_mix(bz_color(BZ_C_GROUND), bz_color(BZ_C_ICE), bz_ui_dark() ? 0.07f : 0.16f));
    else hm_fill(HM.desk, HM_ROLE);
    uint32_t R = HM_ROLE;
    hm_fill(HM.np, t ? t->surface : R);
    hm_fill(HM.face, t ? t->surface : R);
    hm_fill(HM.ha_hint, t ? t->surface : R);
    hm_fill(HM.np_art, t ? t->surface2 : R);
    for (int i = 0; i < HM_HOME_TILES; i++)
        if (HM.ha[i].lit != 1) hm_fill(HM.ha[i].tile, t ? t->surface : R);
    for (int i = 0; i < NLAUNCH && i < 12; i++) hm_fill(HM.tile[i], t ? (LAUNCH[i].action == 3 ? t->surface2 : t->surface) : R);
    /* the card's transport: the play button in the cover's accent, the rest a step up from the card */
    for (int i = 0; i < 5; i++) {
        bool play = HM.np_btn[i] == HM.np_play;
        hm_fill(HM.np_btn[i], t ? (play ? t->accent : t->surface2) : R);
        hm_ink(lv_obj_get_child(HM.np_btn[i], 0), t ? (play ? t->on_accent : t->ink) : R);
    }
    hm_fill(HM.np_meter, t ? t->track : R);
    hm_fill(lv_obj_get_child(HM.np_meter, 0), t ? t->accent : R);
    hm_ink(HM.np_src, t ? t->dim : R);
    hm_ink(HM.np_artist, t ? t->dim : R);
    hm_ink(HM.np_time, t ? t->dim : R);
}

void hm_look_changed(void)
{
    if (!HM.built) return;
    hm_layout();
    HM.ground_sig = 1; /* painted again */
    desk_colors();
    HM.last_min = -1; /* the date and the match line again, where they now are */
    HM.sky.sig = 0;
    /* the face chosen, and its options (the second hand), on the next refresh; never a picture while home
     * mode is off (it would be painted for nobody and held) */
    if (HM.active && !HM.night_on) hm_faces_set(hm_cfg()->face);
    hm_faces_look();
}

/* ---- the page dots ----
 * Where you are among the pages, on screen for as long as home mode is: on the top layer, so a page slide holds
 * them still over both pages (a slide keeps the top layer's pieces where they are, bz_ui_slide_begin) and a face's
 * push does too, instead of their sliding away with the page they were drawn on. Small and quiet (8 px, the
 * current one a 22 px pill in ink, the others faint) at the foot of the screen, below everything the pages put
 * there. When the page changes the pill moves over on a spring, which redraws only the dots' own 54 x 8 px. */
static struct {
    lv_obj_t *row, *dot[HM_NPAGES];
    bz_motion_t p;             /* where the pill is, in pages */
    int w[HM_NPAGES], x[HM_NPAGES];
    uint32_t col[HM_NPAGES];
    bool shown;
    int tone;                  /* the colours were set for: 0 dark, 1 light, 2 night (-1 none) */
} DOTS = { .tone = -1 };

static void dots_build(void)
{
    DOTS.row = bz_box(bz_ui_glass());
    lv_obj_set_size(DOTS.row, 8 * HM_NPAGES + 14 + 8 * (HM_NPAGES - 1), 8);
    lv_obj_set_pos(DOTS.row, (W - lv_obj_get_width(DOTS.row)) / 2, DOTS_Y);
    lv_obj_remove_flag(DOTS.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(DOTS.row, LV_OBJ_FLAG_HIDDEN);
    /* first among the top layer's children: the slides and pushes that hide the chrome for a band count only the
     * first 32 of them */
    lv_obj_move_to_index(DOTS.row, 0);
    for (int i = 0; i < HM_NPAGES; i++) {
        lv_obj_t *o = bz_box(DOTS.row);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        DOTS.dot[i] = o;
        DOTS.w[i] = DOTS.x[i] = -1;
    }
    bz_motion_init(&DOTS.p, (float)HM.page, 0.002f);
    DOTS.p.keep = true;
}

/* the dots for the pill at p: each dot's width by how near the pill is, the widths always summing the same */
static void dots_place(float p, bool recolor)
{
    bool night = HM.night_on;
    /* over a face that paints its own sky and land the faint dots would be lost: there they are a stronger grey */
    int face = hm_faces_face();
    bool scene = HM.page == HM_PG_CLOCK && (face == HM_FACE_SKY || face == HM_FACE_TREE);
    int tone = night ? 2 : (bz_ui_dark() ? 0 : 1) + (scene ? 4 : 0);
    if (tone != DOTS.tone) {
        DOTS.tone = tone;
        recolor = true;
    }
    uint32_t ink = night ? 0x5A4C46 : bz_color(BZ_C_INK), faint = night ? 0x2A2320 : bz_color(BZ_C_FAINT);
    if (scene && !night) faint = bz_mix(faint, ink, 0.4f);
    int x = 0;
    for (int i = 0; i < HM_NPAGES; i++) {
        float k = 1 - fabsf(p - (float)i);
        k = k < 0 ? 0 : k;
        int w = 8 + (int)lroundf(14 * k);
        if (w != DOTS.w[i] || x != DOTS.x[i]) {
            lv_obj_set_size(DOTS.dot[i], w, 8);
            lv_obj_set_pos(DOTS.dot[i], x, 0);
            DOTS.w[i] = w;
            DOTS.x[i] = x;
        }
        uint32_t c = hpx_mix(faint, ink, (int)(k * 256));
        if (c != DOTS.col[i] || recolor) {
            DOTS.col[i] = c;
            lv_obj_set_style_bg_color(DOTS.dot[i], bz_lv_rgb(c), 0);
        }
        x += w + 8;
    }
}

/* the page the dots point at: at once, or the pill moving over */
static void dots_go(int page, bool animate)
{
    if (!DOTS.row) return;
    if (animate) bz_motion_to(&DOTS.p, (float)page, BZ_RELEASE);
    else {
        bz_motion_set(&DOTS.p, (float)page, 0);
        bz_motion_to(&DOTS.p, (float)page, BZ_RELEASE);
        dots_place((float)page, true);
    }
}

/* every frame: shown while home mode is what's on screen (not under an app or the photos, not in catalyst mode),
 * and the pill's spring */
static void dots_frame(void)
{
    if (!DOTS.row) return;
    bool show = HM.active && !ui_app_any_open() && !HM.saver_on;
    if (show != DOTS.shown) {
        DOTS.shown = show;
        hm_show(DOTS.row, show);
        if (show) dots_place(DOTS.p.value, true);
    }
    if (!show || SL.active) return;
    bool moving = bz_motion_tick(&DOTS.p);
    dots_place(DOTS.p.value, false);
    if (moving) bz_ui_keep_alive();
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

    HM.pg[HM_PG_CLOCK] = hm_faces_build(r);
    lv_obj_t *d = bz_box(r);
    lv_obj_set_size(d, W, H);
    lv_obj_add_style(d, bz_style_fill(BZ_C_GROUND), 0);
    HM.desk = HM.pg[HM_PG_DESK] = d;
    HM.pg[HM_PG_NP] = hm_np_build(r);
    HM.pg[HM_PG_PET] = hm_pet_build(r);
    for (int i = 0; i < HM_NPAGES; i++) {
        lv_obj_set_pos(HM.pg[i], 0, 0);
        if (i != HM.page) lv_obj_add_flag(HM.pg[i], LV_OBJ_FLAG_HIDDEN);
    }

    build_top(d);
    build_playing(d);
    build_companion(d);
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
    build_home(d);
    build_launcher(d);

    /* where you are among the pages: on the top layer (dots_build) */
    dots_build();

    HM.built = true;
    hm_layout();
    HM.ground_sig = 1;
    desk_colors();
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

static void refresh_ha(void)
{
    if (!(hm_cfg()->cards & HM_CARD_HOME)) return; /* not on this look */
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
    hm_show(HM.ha_hint, hint);
    const home_tint_t *tint = hm_tint(NULL);
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
            /* lit: the accent's fill over any tint; not: the tint's surface when there is one */
            hm_fill(t->tile, lit || !tint ? HM_ROLE : tint->surface);
            bz_set_color(t->icon, lit ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->name, lit ? BZ_C_ON_ICE : BZ_C_INK);
            bz_set_color(t->state, lit ? BZ_C_ON_ICE : BZ_C_DIM);
        }
    }
}

/* ---- the weather's sky ---- */

static uint32_t rgbf(float r, float g, float b) { return hpx_rgb((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f)); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* A cloud: a few discs, a flat underside, soft edges; its shadow first, down and to the right of the light. */
static void cloud(hpx_t *c, float x, float y, float s, uint32_t col, uint32_t shade, int a)
{
    hpx_rrect(c, x - s * 1.1f + 3, y - s * 0.1f + 5, s * 2.3f, s * 0.62f, s * 0.31f, s * 0.35f, shade, a / 3);
    hpx_disc(c, x - s * 0.55f + 3, y + 5, s * 0.46f, s * 0.2f, shade, a / 3);
    hpx_disc(c, x + 0.2f * s + 3, y - s * 0.18f + 5, s * 0.62f, s * 0.2f, shade, a / 3);
    hpx_rrect(c, x - s * 1.1f, y - s * 0.1f, s * 2.3f, s * 0.62f, s * 0.31f, 0, col, a);
    hpx_disc(c, x - s * 0.55f, y, s * 0.46f, 0, col, a);
    hpx_disc(c, x + 0.2f * s, y - s * 0.18f, s * 0.62f, 0, col, a);
}

/* Today's sky in a tile: the colour of the hour (Bezel's horizon: day, dusk, night), and the weather in it, drawn
 * rather than an icon (the icon font has no clouds): the sun or the moon, clouds, rain, snow, fog, a bolt. The
 * corners are cut against the desk's ground, exactly, so the tile has no edge but its own. On the worker. */
static void sky_paint(void *arg)
{
    (void)arg;
    uint16_t *px = HM.sky.px;
    if (!px) return;
    const int w = WX_W, h = WX_H;
    float tone = HM.sky.dark ? 0.0f : 1.0f;
    float span = HM.sky.set - HM.sky.rise;
    if (span < 4) span = 13;
    float day = (HM.sky.hour - HM.sky.rise) / span;
    float up = day >= 0 && day <= 1 ? 1.0f : 0.0f;
    float edge = fminf(fabsf(day), fabsf(1 - day));
    float dusk = expf(-edge * edge * 60.0f);
    float night = (1 - up) * (edge > 0.08f ? 1.0f : edge / 0.08f);
    int code = HM.sky.code;
    bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82), snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    bool storm = code >= 95, fog = code == 45 || code == 48, overcast = code == 3 || rain || snow || storm;
    bool partly = code == 2;
    float grey = overcast ? 0.55f : fog ? 0.45f : partly ? 0.15f : 0.0f;
    float top[3], low[3];
    for (int k = 0; k < 3; k++) {
        /* dark tone keeps the sky deep, so the tile doesn't glare on a dark desk; light tone keeps it pale */
        float td = mixf((float[]){ 0.16f, 0.30f, 0.54f }[k], (float[]){ 0.52f, 0.70f, 0.90f }[k], tone);
        float tk = mixf((float[]){ 0.15f, 0.14f, 0.30f }[k], (float[]){ 0.60f, 0.58f, 0.78f }[k], tone);
        float ld = mixf((float[]){ 0.30f, 0.46f, 0.64f }[k], (float[]){ 0.80f, 0.88f, 0.96f }[k], tone);
        float lk = mixf((float[]){ 0.62f, 0.33f, 0.25f }[k], (float[]){ 0.98f, 0.76f, 0.62f }[k], tone);
        float tn = mixf((float[]){ 0.03f, 0.04f, 0.09f }[k], (float[]){ 0.26f, 0.30f, 0.44f }[k], tone);
        float ln = mixf((float[]){ 0.07f, 0.09f, 0.17f }[k], (float[]){ 0.38f, 0.42f, 0.56f }[k], tone);
        top[k] = mixf(mixf(td, tk, dusk), tn, night);
        low[k] = mixf(mixf(ld, lk, dusk), ln, night);
        float g = mixf(0.22f, 0.70f, tone) * (1 - night * 0.6f);
        top[k] = mixf(top[k], g, grey);
        low[k] = mixf(low[k], g + 0.06f, grey);
    }
    hpx_t c;
    hpx_init16(&c, px, w, h, w);
    for (int y = 0; y < h; y++) {
        float t = (float)y / h;
        hpx_clip(&c, 0, y, w - 1, y);
        hpx_fill(&c, rgbf(mixf(top[0], low[0], t), mixf(top[1], low[1], t), mixf(top[2], low[2], t)));
    }
    hpx_unclip(&c);
    float cx = w - 92.0f, cy = 62.0f;
    if (night > 0.5f && !overcast && !fog) {
        hpx_rng_t r = { 0x51a7u };
        for (int i = 0; i < 90; i++) {
            int x = (int)(hpx_rand(&r) * w), y = (int)(hpx_rand(&r) * h * 0.8f);
            hpx_put(&c, x, y, 0xF2F4FF, (int)(hpx_rand(&r) * 170));
        }
    }
    if (!overcast && !fog) {
        if (up > 0) {
            uint32_t sun = hpx_mix(rgbf(1.0f, 0.93f, 0.72f), rgbf(1.0f, 0.62f, 0.34f), (int)(dusk * 256));
            hpx_glow(&c, cx, cy, 76, sun, tone > 0.5f ? 120 : 90);
            hpx_disc(&c, cx, cy, 22, 0, sun, 255);
        } else {
            /* a crescent: the moon, and the sky cut out of it */
            uint32_t moon = 0xE9E7DD;
            hpx_glow(&c, cx, cy, 60, moon, 40);
            hpx_disc(&c, cx, cy, 19, 0, moon, 255);
            float t = 0.5f;
            uint32_t sk = rgbf(mixf(top[0], low[0], (cy - 8) / h * t + 0.2f), mixf(top[1], low[1], (cy - 8) / h * t + 0.2f),
                               mixf(top[2], low[2], (cy - 8) / h * t + 0.2f));
            hpx_disc(&c, cx + 9, cy - 7, 16.5f, 0, sk, 255);
        }
    }
    uint32_t cl = tone > 0.5f ? rgbf(0.98f, 0.99f, 1.0f) : night > 0.5f ? rgbf(0.36f, 0.39f, 0.46f) : rgbf(0.78f, 0.82f, 0.88f);
    uint32_t shade = storm ? 0x20242c : 0x000000;
    if (storm) cl = tone > 0.5f ? rgbf(0.58f, 0.60f, 0.66f) : rgbf(0.30f, 0.32f, 0.37f);
    if (partly) cloud(&c, cx + 18, cy + 24, 34, cl, shade, 245);
    if (overcast) {
        cloud(&c, cx - 70, cy + 6, 30, hpx_mix(cl, 0x808890, 40), shade, 235);
        cloud(&c, cx + 10, cy + 18, 40, cl, shade, 250);
    }
    if (fog)
        for (int i = 0; i < 4; i++) hpx_rrect(&c, 170.0f + i * 14, 34.0f + i * 22, 190, 9, 4.5f, 6, cl, 150);
    if (rain)
        for (int i = 0; i < 9; i++) {
            float x = cx - 64 + i * 15.0f + (i % 2) * 5, y = cy + 44 + (i % 3) * 9.0f;
            hpx_capsule(&c, x, y, x - 5, y + 14, 1.3f, 0, tone > 0.5f ? 0x5B7FB8 : 0x9CB6E0, 220);
        }
    if (snow)
        for (int i = 0; i < 10; i++) {
            float x = cx - 66 + i * 14.0f, y = cy + 46 + (i % 3) * 11.0f;
            hpx_disc(&c, x, y, 2.6f, 0, 0xFFFFFF, 230);
        }
    if (storm) {
        uint32_t bolt = 0xFFC940;
        hpx_tri(&c, cx + 4, cy + 30, cx - 12, cy + 62, cx + 2, cy + 58, bolt, 255);
        hpx_tri(&c, cx + 2, cy + 54, cx + 14, cy + 54, cx - 8, cy + 88, bolt, 255);
    }
    /* the corners, cut against the desk's ground */
    uint32_t g = hpx_from565(HM.sky.ground);
    const float R = WX_R;
    for (int y = 0; y < h; y++) {
        float fy = y + 0.5f < R ? R - (y + 0.5f) : y + 0.5f > h - R ? (y + 0.5f) - (h - R) : 0;
        if (fy <= 0) continue;
        for (int x = 0; x < w; x++) {
            float fx = x + 0.5f < R ? R - (x + 0.5f) : x + 0.5f > w - R ? (x + 0.5f) - (w - R) : 0;
            if (fx <= 0) continue;
            float d = sqrtf(fx * fx + fy * fy) - R;
            if (d >= 0.5f) px[(size_t)y * w + x] = HM.sky.ground;
            else if (d > -0.5f) hpx_put(&c, x, y, g, (int)((d + 0.5f) * 255));
        }
    }
    /* the text on it: light or dark, by the sky behind the words */
    uint32_t behind = rgbf(mixf(top[0], low[0], 0.45f), mixf(top[1], low[1], 0.45f), mixf(top[2], low[2], 0.45f));
    HM.sky.ink = hpx_luma(behind) < 150 ? 1 : 0;
}

static void refresh_weather(void)
{
    home_weather_t w;
    home_weather_get(&w);
    time_t tt = time(NULL);
    struct tm tm;
    localtime_r(&tt, &tm);
    bool dark = bz_ui_dark();
    home_forecast_t f;
    home_weather_forecast(&f);
    /* the sky again when the weather, the half hour, the tone or the ground under its corners changes */
    uint16_t ground = hm_565(bz_color(BZ_C_GROUND));
    unsigned tg;
    const home_tint_t *t = hm_tint(&tg);
    if (t) ground = hm_565(t->ground);
    else if (hm_cfg()->tint) ground = hm_565(bz_mix(bz_color(BZ_C_GROUND), bz_color(BZ_C_ICE), dark ? 0.07f : 0.16f));
    int code = w.configured && w.ok ? w.code : -1;
    uint32_t sig = (uint32_t)(code + 7) * 131u ^ (uint32_t)(tm.tm_hour * 2 + tm.tm_min / 30) * 7919u ^ (dark ? 1u : 2u) ^
                   (uint32_t)ground << 12;
    if (sig != HM.sky.sig && hm_art_job_done(HM.sky.ticket)) {
        if (!HM.sky.px) {
            HM.sky.px = malloc(sizeof(uint16_t) * WX_W * WX_H);
            HM.sky.dsc.data = (const uint8_t *)HM.sky.px;
        }
        if (HM.sky.px) {
            HM.sky.sig = sig;
            HM.sky.hour = (float)tm.tm_hour + tm.tm_min / 60.0f;
            HM.sky.rise = f.ok && f.sunrise[0] ? (float)atoi(f.sunrise) + (float)atoi(strchr(f.sunrise, ':') ? strchr(f.sunrise, ':') + 1 : "0") / 60.0f : 6.5f;
            HM.sky.set = f.ok && f.sunset[0] ? (float)atoi(f.sunset) + (float)atoi(strchr(f.sunset, ':') ? strchr(f.sunset, ':') + 1 : "0") / 60.0f : 19.5f;
            HM.sky.code = code < 0 ? 0 : code;
            HM.sky.dark = dark;
            HM.sky.ground = ground;
            HM.sky.painted = false;
            HM.sky.ticket = hm_art_job(sky_paint, NULL);
        }
    }
    if (HM.sky.px && !HM.sky.painted && hm_art_job_done(HM.sky.ticket)) {
        HM.sky.painted = true;
        lv_image_cache_drop(&HM.sky.dsc);
        lv_image_set_src(HM.wx_img, &HM.sky.dsc);
        lv_obj_invalidate(HM.wx_img);
        hm_show(HM.wx_img, true);
        uint32_t ink = HM.sky.ink ? 0xF4F6F8 : 0x15181B, dim = HM.sky.ink ? 0xD5DAE0 : 0x3A4048;
        hm_ink(HM.wx_temp, ink);
        hm_ink(HM.wx_line, ink);
        hm_ink(HM.wx_place, dim);
    }
    if (w.gen == HM.wx_gen && HM.wx_gen) return;
    HM.wx_gen = w.gen ? w.gen : 1;
    if (!w.configured || !w.ok) {
        ui_text(HM.wx_temp, "%s", "--\xc2\xb0");
        ui_text(HM.wx_line, "%s", !w.configured ? "set a place for the weather" : w.err[0] ? w.err : "looking up the weather");
        ui_text(HM.wx_place, "%s", w.configured ? w.place : "");
        return;
    }
    ui_text(HM.wx_temp, "%.0f\xc2\xb0", w.temp);
    ui_text(HM.wx_line, "%s \xc2\xb7 %.0f\xc2\xb0 / %.0f\xc2\xb0", home_weather_text(w.code), w.hi, w.lo);
    ui_text(HM.wx_place, "%s", w.place);
}

/* ---- what's playing, on the desk ---- */

static void show_progress(bool on)
{
    if (on == HM.progress_shown) return;
    HM.progress_shown = on;
    hm_show(HM.np_meter, on);
    hm_show(HM.np_time, on);
}

static void refresh_media(void)
{
    hm_song_t s;
    hm_song(&s);
    const hm_art_t *a = hm_song_art(&s);
    tint_update(a, s.have);
    if (s.gen != HM.song_gen || !HM.song_gen) {
        HM.song_gen = s.gen ? s.gen : 1;
        ui_text(HM.np_src, "%s", s.src);
        ui_text(HM.np_title, "%s", s.have ? s.title : "Nothing playing");
        if (!s.have) ui_text(HM.np_artist, "%s", s.hint);
        else if (s.artist[0] && s.album[0]) ui_text(HM.np_artist, "%s\n%s", s.artist, s.album);
        else ui_text(HM.np_artist, "%s", s.artist[0] ? s.artist : s.album);
        if (s.playing != HM.playing_shown) {
            HM.playing_shown = s.playing;
            bz_icon_set(lv_obj_get_child(HM.np_play, 0), s.playing ? BZ_I_PAUSE : BZ_I_PLAY_ARROW, 24, false);
        }
        show_progress(s.have && s.dur > 0);
    }
    /* the cover: the song's own or a generated one; the quiet placeholder with nothing playing */
    if (!s.have) {
        if (HM.art_shown) {
            HM.art_shown = 0;
            hm_show(HM.np_img, false);
            hm_show(HM.np_ph, true);
        }
    } else if (hm_art_set(HM.np_img, a, &HM.art_shown)) {
        hm_show(HM.np_img, true);
        hm_show(HM.np_ph, false);
    }
    int sec = (int)hm_now();
    if (sec != HM.last_sec && HM.progress_shown) {
        HM.last_sec = sec;
        char p[16], d[16];
        hm_fmt_time(s.pos, p, sizeof p);
        hm_fmt_time(s.dur, d, sizeof d);
        ui_text(HM.np_time, "%s / %s", p, d);
        bz_meter_set(HM.np_meter, s.dur > 0 ? (float)(s.pos / s.dur) : 0, BZ_C_ICE);
    }
}

static void refresh_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    /* the seconds, when shown: one small label a second, nothing else */
    if (hm_cfg()->seconds && tm.tm_sec != HM.secs_shown) {
        HM.secs_shown = tm.tm_sec;
        if (tm.tm_year > 120) ui_text(HM.secs, "%02d", tm.tm_sec);
        else ui_text(HM.secs, "%s", "");
    }
    if (tm.tm_min == HM.last_min) return;
    HM.last_min = tm.tm_min;
    char nx[96];
    if (!ui_match_next_line(nx, sizeof nx)) nx[0] = 0;
    ui_text(HM.match, "%s", nx);
    if (tm.tm_year > 120) {
        /* rolls up into place, unless this is the first time it's set */
        if (HM.clock && lv_label_get_text(HM.clock)[0] != '-' && HM.active && HM.page == HM_PG_DESK && HM.roll.keep) {
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
        char il[160];
        if (ui_intel_line(il, sizeof il)) ui_text(HM.greet, "%s", il); /* what matters now (ui_intel.h) */
        else ui_text(HM.greet, "%s", tm.tm_hour < 5 ? "late night" : tm.tm_hour < 12 ? "good morning"
                                     : tm.tm_hour < 18 ? "good afternoon" : "good evening");
    } else {
        ui_text(HM.clock, "--:--");
        ui_text(HM.date, "the clock isn't set");
        ui_text(HM.greet, "%s", "");
    }
}

/* one page's words brought up to date (the one on screen at 10 Hz; a neighbour just before it's drawn) */
static void page_refresh(int page, bool shown)
{
    if (page == HM_PG_DESK) {
        refresh_clock();
        refresh_weather();
        refresh_media();
        refresh_ha();
        desk_colors();
    } else if (page == HM_PG_NP) {
        hm_np_refresh(shown);
    } else if (page == HM_PG_PET) {
        hm_pet_refresh(shown);
    } else {
        hm_faces_refresh(shown);
    }
}

static void hm_refresh_now(void)
{
    home_want(HOME_WANT_PC | HOME_WANT_HA | HOME_WANT_WEATHER);
    page_refresh(HM.page, true);
    /* the song, its cover, its tint and the big cover on the page beside the desk stay current wherever you are */
    if (HM.page != HM_PG_DESK) refresh_media();
    if (HM.page != HM_PG_NP) hm_np_refresh(false);
    if (HM.page != HM_PG_PET) hm_pet_refresh(false);
    if (HM.page != HM_PG_CLOCK) hm_faces_refresh(false);
    if (HM.page != HM_PG_DESK) desk_colors();
}

/* ---- memory: what home mode painted, given back when it's left ---- */

static void release_pictures(void)
{
    hm_pet_near(false);
    hm_np_release();
    hm_faces_release();
    if (HM.sky.px && hm_art_job_done(HM.sky.ticket)) {
        lv_image_cache_drop(&HM.sky.dsc);
        hm_show(HM.wx_img, false);
        free(HM.sky.px);
        HM.sky.px = NULL;
        HM.sky.dsc.data = NULL;
        HM.sky.sig = 0;
        HM.sky.painted = false;
    }
    if (!ui_app_is_open(&APP_MUSIC)) hm_art_trim();
    HM.art_shown = 0;
    hm_show(HM.np_img, false);
    hm_show(HM.np_ph, true);
}

static bool released(void) { return hm_np_released() && hm_faces_released() && !HM.sky.px; }

static void hm_refresh(void *u)
{
    (void)u;
    if (!HM.active) {
        /* left while the worker was painting into something: freed once it has finished */
        if (HM.built && !released() && !HM.sheet) release_pictures();
        return;
    }
    if (HM.sheet) return;
    /* the network only for what's on screen: with an app over it, the app asks for its own */
    if (ui_app_any_open()) return;
    hm_refresh_now();
}

/* ================================================================== pages */

/* The top layer's pieces (the orb, a message's capsule) are not in a page's picture: hidden for a band, put back
 * after. The frame hooks put the orb back the moment they find it hidden, so every band hides them again. */
static void chrome_hide(bool keep)
{
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t k = 0; k < nc && k < 32; k++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)k);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (keep) SL.chrome |= 1u << k;
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
}

static void chrome_restore(void)
{
    lv_obj_t *top = bz_ui_glass();
    uint32_t nc = lv_obj_get_child_count(top);
    for (uint32_t k = 0; k < nc && k < 32; k++)
        if (SL.chrome & (1u << k)) lv_obj_remove_flag(lv_obj_get_child(top, (int32_t)k), LV_OBJ_FLAG_HIDDEN);
    SL.chrome = 0;
}

/* One band of page p into the slide's neighbour picture: the page shown alone, the chrome hidden. */
static void slide_prep(bool before, void *u)
{
    int p = (int)(intptr_t)u;
    if (before) {
        hm_show(HM.pg[HM.page], false);
        hm_show(HM.pg[p], true);
        SL.chrome = 0;
        chrome_hide(true);
    } else {
        chrome_restore();
        hm_show(HM.pg[p], false);
        hm_show(HM.pg[HM.page], true);
    }
}

static void bands_end(void)
{
    if (SL.prep_page < 0) return;
    SL.prep_page = -1;
    bz_ui_offscreen_end();
}

static void slide_band(int page, int band)
{
    uint16_t *nb = bz_ui_slide_nb_buf();
    if (!nb || page < 0 || page >= HM_NPAGES) return;
    if (band == 0) {
        bands_end();
        page_refresh(page, true); /* its words as of now, not as of when it was last shown */
    }
    if (SL.prep_page != page) {
        SL.prep_page = page;
        bz_ui_offscreen_begin(slide_prep, (void *)(intptr_t)page);
    }
    lv_area_t a = { 0, band * H / SL_BANDS, W - 1, (band + 1) * H / SL_BANDS - 1 };
    chrome_hide(false);
    bz_ui_offscreen_band(nb + (size_t)a.y1 * W, W, &a);
    bz_ui_slide_nb_patch(&a);
}

static void page_quiet(void *u)
{
    int p = (int)(intptr_t)u;
    hm_show(HM.pg[HM.page], false);
    hm_show(HM.pg[p], true);
    HM.page = p;
}

/* the page p on screen, now (a redraw of the whole screen: for when nothing is moving to hide it) */
static void page_set(int p)
{
    if (p == HM.page) return;
    hm_show(HM.pg[HM.page], false);
    hm_show(HM.pg[p], true);
    HM.page = p;
    dots_go(p, true);
    page_refresh(p, true);
}

static bool can_slide(void)
{
    return HM.active && !HM.sheet && !SL.active && !bz_ui_sheeting() && !hm_faces_moving() && !ui_app_any_open() &&
           !HM.night_on && bz_ui_can_sheet();
}

static void pg_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p; (void)u;
    if (!can_slide()) return;
    bands_end();
    bz_ui_slide_begin();
    SL.plain = !bz_ui_sliding(); /* no memory for the pictures: the page just changes at the end */
    SL.active = true;
    SL.settling = SL.jump = false;
    SL.side = 0;
    SL.ready = false;
}

static void pg_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vx; (void)vy; (void)u;
    if (!SL.active || SL.settling || SL.plain) return;
    bool edge = (dx > 0 && HM.page == 0) || (dx < 0 && HM.page == HM_NPAGES - 1);
    int side = edge || dx == 0 ? 0 : dx < 0 ? 1 : -1;
    if (side != SL.side) {
        SL.side = side;
        SL.nb_page = HM.page + side;
        SL.band = 0;
        SL.ready = false;
        bz_ui_slide_nb(0);
    }
    bz_ui_slide(edge ? (int)bz_rubber_band((float)dx, (float)W, 0.55f) : dx);
}

static void pg_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vy; (void)u;
    if (!SL.active || SL.settling) return;
    /* a swipe of a sixth of the screen, or a flick, is one page */
    int aim = HM.page;
    if ((dx < -W / 6 || vx < -600) && HM.page < HM_NPAGES - 1) aim = HM.page + 1;
    else if ((dx > W / 6 || vx > 600) && HM.page > 0) aim = HM.page - 1;
    if (SL.plain) {
        SL.active = false;
        if (aim != HM.page) page_set(aim);
        return;
    }
    if (aim != HM.page && (SL.side != aim - HM.page || !SL.ready)) {
        /* released before its neighbour finished drawing: finish it now (a frame's hitch) */
        if (SL.side != aim - HM.page) {
            SL.side = aim - HM.page;
            SL.nb_page = HM.page + SL.side;
            SL.band = 0;
            SL.ready = false;
        }
        if (!SL.ready) {
            for (; SL.band < SL_BANDS; SL.band++) slide_band(SL.nb_page, SL.band);
            SL.ready = true;
        }
        bands_end();
        bz_ui_slide_nb(SL.side);
    }
    SL.aim = aim;
    SL.settling = true;
    int from = dx;
    bool edge = (dx > 0 && HM.page == 0) || (dx < 0 && HM.page == HM_NPAGES - 1);
    if (edge) from = (int)bz_rubber_band((float)dx, (float)W, 0.55f);
    bz_motion_init(&SL.x, (float)from, 0.5f);
    SL.x.keep = true;
    bz_motion_to_v(&SL.x, (float)((HM.page - aim) * W), BZ_PAGE, edge ? 0 : vx);
    bz_ui_keep_alive();
}

/* A page asked for without a finger (the desk's cover, the dev console): the same road a swipe takes, the bands
 * drawn over eight frames and then the page spring. Noted here, started from the frame hook: a slide's picture
 * must not be taken inside LVGL's handler. */
static void hm_go(int page)
{
    if (page >= 0 && page < HM_NPAGES) HM.go_want = page;
}

static void go_step(void)
{
    if (HM.go_want < 0) return;
    int want = HM.go_want;
    HM.go_want = -1;
    if (want == HM.page || !HM.active) return;
    if (!can_slide() || bz_drag_active()) {
        page_set(want);
        return;
    }
    bands_end();
    bz_ui_slide_begin();
    if (!bz_ui_sliding()) {
        page_set(want);
        return;
    }
    SL.active = SL.jump = true;
    SL.settling = SL.ready = SL.plain = false;
    SL.band = 0;
    SL.side = want > HM.page ? 1 : -1;
    SL.nb_page = want;
    SL.aim = want;
    bz_motion_init(&SL.x, 0, 0.5f);
    SL.x.keep = true;
    bz_ui_keep_alive();
}

static void slide_frame(void)
{
    if (!SL.active || SL.plain) return;
    bz_ui_keep_alive();
    if (!SL.settling) {
        if (SL.side && !SL.ready) {
            slide_band(SL.nb_page, SL.band);
            if (++SL.band == SL_BANDS) {
                SL.ready = true;
                bands_end();
                bz_ui_slide_nb(SL.side);
                if (SL.jump) {
                    SL.settling = true;
                    bz_motion_to(&SL.x, (float)(-SL.side * W), BZ_PAGE);
                }
            }
        }
        return;
    }
    bool moving = bz_motion_tick(&SL.x);
    bz_ui_slide((int)lroundf(SL.x.value));
    if (moving) return;
    SL.active = SL.settling = SL.jump = false;
    bands_end();
    int aim = SL.aim;
    if (aim != HM.page) bz_ui_quiet(page_quiet, (void *)(intptr_t)aim);
    bz_ui_slide_end();
    dots_go(HM.page, true);
    page_refresh(HM.page, true);
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

/* the page home mode opens on */
static void open_page(void)
{
    int p = hm_cfg()->open_page;
    if (p < 0 || p >= HM_NPAGES) p = HM_PG_DESK;
    if (p != HM.page) {
        hm_show(HM.pg[HM.page], false);
        hm_show(HM.pg[p], true);
        HM.page = p;
    }
    dots_go(HM.page, false);
}

static void go(bool enter)
{
    HM.active = enter;
    if (enter) {
        HM.last_min = -1;
        HM.wx_gen = HM.ha_gen = HM.song_gen = 0;
        HM.ground_sig = 1;
        hm_cfg_apply();
        HM.blink_at = hal_seconds() + 3;
        HM.night_on = false;
        open_page();
        hm_faces_set(hm_cfg()->face);
    }
    /* the room-occupied poll only ever runs while home mode is actually on screen, the same as the eyes'
     * blink and the stand watcher's own timers: nothing here keeps a microphone duty-cycling in catalyst
     * mode or with the tablet asleep in a bag */
    home_presence_set_active(enter);
    bz_ui_wake();
    /* the platform slides the new picture in over the old one, drawing it a band at a time as it shows:
     * the state switches now, under a glass that keeps the old picture where the sheet hasn't reached */
    if (bz_ui_sheet_begin(enter, H, true, NULL, NULL)) {
        HM.sheet = true;
        bz_motion_set(&HM.k, enter ? 0 : 1, 0);
        bz_motion_to(&HM.k, enter ? 1 : 0, BZ_RELEASE);
    }
    set_state(enter);
    /* its words brought up to date before the sheet draws them (hm_refresh waits out the sheet: the surface
     * slid in with the clock, date and song of the last time it was up, then jumped) */
    if (enter) hm_refresh_now();
    else if (!HM.sheet) release_pictures();
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

/* ---- the timed photo: hand the camera and the lens app's own use of it to home_capture_tick, and put
 * something on screen for the moment the camera is actually live (see home.h's privacy note) ---- */
static void cap_tick(double now)
{
    static bool was_live;
    home_capture_tick(now, HM.active, !ui_app_any_open());
    home_capture_status_t st;
    home_capture_status(&st);
    if (st.live && !was_live) ui_island_say(BZ_I_PHOTO_CAMERA, "home photo");
    was_live = st.live;
}

/* ---- the photos screensaver: the card's pictures after a while untouched, on home mode only ---- */
static void saver_tick(void)
{
    int min = hm_cfg()->saver_min;
    double idle = bz_ui_idle_s();
    if (idle < 5) HM.saver_on = false; /* touched: the next idle spell may start it again */
    if (!min || HM.saver_on || HM.night_on || !HM.active || HM.sheet || HM.want || ui_app_any_open() || ui_asleep()) return;
    if (idle < min * 60.0 || !hal_sd_root()) return;
    HM.saver_on = true;
    ui_photos_slideshow(NULL);
}

/* ---- night: from 22:00 to 7:00, untouched for a minute, the dim night face; a touch gives back what was there.
 * A plain switch, not a slide: nobody is watching it happen. ---- */
static void night_tick(double now)
{
    static double next, since, left;
    if (!HM.active || HM.sheet || SL.active || hm_faces_moving()) return;
    double idle = bz_ui_idle_s();
    if (HM.night_on) {
        /* every frame, not once a second: a touch is under a second old for only a second, and a check once a
         * second missed half of them */
        bool night = HM.night_force >= 0 ? HM.night_force == 1 : now >= next ? hm_faces_night() : true;
        if (now >= next) next = now + 1.0;
        if (night && !(idle < 0.5 && now - since > 1.0)) return;
        HM.night_on = false;
        left = now;
        hm_faces_set(HM.night_face >= 0 && HM.night_face < HM_NFACES ? HM.night_face : hm_cfg()->face);
        page_set(HM.night_page);
        dots_go(HM.page, false);
        return;
    }
    if (now < next) return;
    next = now + 1.0;
    bool night = HM.night_force >= 0 ? HM.night_force == 1 : hm_faces_night();
    /* a minute untouched; the dev console's forced night comes back five seconds after a touch sent it away */
    bool quiet = idle >= NIGHT_IDLE_S || (HM.night_force == 1 && now - left > 5.0);
    if (!night || !quiet || ui_app_any_open() || HM.saver_on) return;
    HM.night_on = true;
    since = now;
    HM.night_page = HM.page;
    HM.night_face = hm_faces_face();
    if (HM.idle_on) {
        /* the pet was showing: night gives back what was there before it, not the pet */
        HM.night_page = HM.idle_page;
        HM.night_face = HM.idle_face;
        HM.idle_on = false;
    }
    hm_faces_set(HM_FACE_NIGHT);
    page_set(HM_PG_CLOCK);
    dots_go(HM.page, false);
}

/* ---- the pet when idle: untouched for three minutes (with the setting on), the pet face; a touch gives back what
 * was there. Like night, a plain switch. ---- */
#define PET_IDLE_S 180.0
static void pet_idle_tick(double now)
{
    static double next;
    if (!HM.active || HM.sheet || SL.active || hm_faces_moving() || HM.night_on) return;
    double idle = bz_ui_idle_s();
    if (HM.idle_on) {
        if (!hm_cfg()->idle_pet || idle < 0.5) {
            HM.idle_on = false;
            hm_faces_set(HM.idle_face >= 0 && HM.idle_face < HM_NFACES ? HM.idle_face : hm_cfg()->face);
            page_set(HM.idle_page);
            dots_go(HM.page, false);
        }
        return;
    }
    if (now < next) return;
    next = now + 1.0;
    if (!hm_cfg()->idle_pet || idle < PET_IDLE_S || ui_app_any_open() || HM.saver_on || ui_asleep()) return;
    if (HM.page == HM_PG_CLOCK && hm_faces_face() == HM_FACE_PET) return; /* already the pet */
    HM.idle_on = true;
    HM.idle_page = HM.page;
    HM.idle_face = hm_faces_face();
    hm_faces_set(HM_FACE_PET);
    page_set(HM_PG_CLOCK);
    dots_go(HM.page, false);
}

/* The eyes: blinks on a spring (now and then a double), a gaze that wanders and settles, and on power a
 * slow breath. Each frame sets only what moved, so a still face costs nothing. */
static void eyes_tick(double now)
{
    if (!HM.active || HM.sheet || HM.page != HM_PG_DESK || SL.active || ui_app_any_open() || ui_asleep() || !HM.eye[0]) return;
    if (!(hm_cfg()->cards & HM_CARD_COMPANION)) return; /* no face on this look: nothing to blink */
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

/* The companion card's second line says what the voice is doing. It is also the only sign that the
 * microphones are open: home mode listens for "Hi ESP" the whole time it is showing, and someone standing in
 * front of the tablet has a right to know that. */
static void voice_tick(double now)
{
    if (!ui_home_mode_showing() || !HM.cp_state) return;
    if (!(hm_cfg()->cards & HM_CARD_COMPANION)) return;
    static double next;
    if (now < next) return;
    next = now + 0.25;
    char line[96];
    vo_state_t st = voice_state();
    char note[160];
    bool nerr = false;
    double at = voice_note(note, sizeof note, &nerr);
    if (note[0] && now - at < (nerr ? 10.0 : 5.0)) snprintf(line, sizeof line, "%s", note);
    else if (st == VO_LISTEN) snprintf(line, sizeof line, "listening...");
    else if (st == VO_HEARING) snprintf(line, sizeof line, "hearing you...");
    else if (st == VO_THINKING) snprintf(line, sizeof line, "thinking...");
    else if (st == VO_SPEAKING) snprintf(line, sizeof line, "talking...");
    else {
        const char *w = voice_mics_on() ? voice_wake_word() : NULL;
        if (w) snprintf(line, sizeof line, "\"%s\" or tap", w);
        else snprintf(line, sizeof line, "tap to talk");
    }
    if (!strcmp(line, HM.cp_state_text)) return;
    snprintf(HM.cp_state_text, sizeof HM.cp_state_text, "%s", line);
    lv_label_set_text(HM.cp_state, line);
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

/* the dev console's "hm prof": where hm_frame's time goes, by part, since it last asked */
static double s_prof[10], s_prof_max[10];
static const char *const PROF_NAME[10] = { "stand+tag", "enter+slide", "faces", "night", "camera", "presence",
                                           "saver", "roll+press", "eyes", "voice" };
#define PROF(i, stmt)                                                                                              \
    do {                                                                                                           \
        double t0_ = hal_seconds();                                                                                \
        stmt;                                                                                                      \
        double d_ = hal_seconds() - t0_;                                                                           \
        s_prof[i] += d_;                                                                                           \
        if (d_ > s_prof_max[i]) s_prof_max[i] = d_;                                                                \
    } while (0)

static int s_minute_late_ms = -1; /* how long after the minute turned the page on show was last brought up to it */

static void hm_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    now = hal_seconds();
    PROF(0, stand_tick(now); tag_tick(now));
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
        if (!HM.active) release_pictures();
        return;
    }
    /* a pending change waits for any app window and sheet to finish */
    if (HM.want && !ui_app_any_open() && !bz_ui_sheeting() && !SL.active && !hm_faces_moving()) {
        int w = HM.want;
        HM.want = 0;
        if (w > 0 && !HM.active) PROF(1, go(true));
        else if (w < 0 && HM.active) PROF(1, go(false));
    }
    if (HM.active) {
        PROF(1, go_step(); slide_frame());
        if (HM.page == HM_PG_CLOCK && !SL.active && !ui_app_any_open() && !ui_asleep()) PROF(2, hm_faces_frame(now));
        PROF(3, night_tick(now); pet_idle_tick(now));
        {
            bool near = !ui_app_any_open() && !ui_asleep() && (HM.page == HM_PG_PET || HM.page == HM_PG_NP);
            hm_pet_near(near);
            hm_pet_frame(near && HM.page == HM_PG_PET && !SL.active);
        }
        /* The clock, from the wall clock, every frame: the second it turns over the page on show is brought up to
         * it (the 10 Hz refresh alone was up to a tenth late, and did nothing under an app or a sheet, so a face
         * came back showing the minute it was left in). Under an app it waits, and catches up the moment the app
         * starts to close, before the surface is drawn again. */
        {
            static time_t last_s;
            static long last_m = -1;
            static bool stale;
            time_t ts = time(NULL);
            if (ts != last_s) {
                last_s = ts;
                long m = (long)(ts / 60);
                bool covered = ui_app_any_open() || SL.active;
                if (m != last_m) {
                    last_m = m;
                    stale = true;
                } else if (!covered && hm_cfg()->seconds && HM.page == HM_PG_DESK) {
                    refresh_clock();
                }
            }
            if (stale && !ui_app_any_open() && !SL.active) {
                stale = false;
                hm_refresh_now();
                bz_ui_keep_alive();
                struct timeval tv;
                gettimeofday(&tv, NULL);
                s_minute_late_ms = (int)((tv.tv_sec % 60) * 1000 + tv.tv_usec / 1000);
            }
        }
    }
    PROF(4, cap_tick(now));
    PROF(5, home_presence_tick(now));
    PROF(6, saver_tick());
    PROF(8, eyes_tick(now));
    PROF(9, voice_tick(now));
    PROF(7, if (HM.page == HM_PG_DESK) roll_tick(); press_tick(); dots_frame());
}

/* Home mode, on the control center. The header has always said a toggle belonged there ("home" /
 * "catalyst"); the registry is what it took. */
static bool hm_cc_on(void) { return HM.active; }
static void hm_cc_enter(void) { ui_home_mode_enter(); }
static void hm_cc_tap(void)
{
    if (HM.active) ui_home_mode_exit();
    else ui_cc_after_close(hm_cc_enter);
}

void ui_home_mode_cc_add(void)
{
    static const ui_cc_control_t CTL = {
        .name = "home", .icon = BZ_I_HOME, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
        .on = hm_cc_on, .tap = hm_cc_tap,
    };
    ui_cc_add(&CTL);
}

/* ---- the dev console: "hm ..." ----
 *   hm                      where it is: the page, the face, night, the pictures' memory
 *   hm page 0|1|2           a page (the clock faces, the desk, now playing), as a swipe would bring it
 *   hm face <name>          that face, on the clock page (type flip dial chrono sky moon orrery tree match words
 *                           night)
 *   hm step up|down         the next or the previous face, as a flick up or down brings it
 *   hm faces                where the faces are: the one shown, a step under way, pictures still held
 *   hm tree                 what the tree shows: its growth, fruit, month, wind and weather
 *   hm night 1|0|auto       night forced on or off, or back to the clock's say
 *   hm song a|b|t, hm song off   a made-up song as the PC's (artist|album|title), for its cover and tint
 *   hm art [bench|style X]  the covers: what's cached, their timing, every generated one in one style
 *   hm prof                 where home mode's frame hook spent its time since the last ask, by part */
void ui_home_mode_dev(const char *a)
{
    while (*a == ' ') a++;
    if (!strncmp(a, "page", 4)) {
        if (!HM.active) ui_home_mode_enter();
        hm_go(atoi(a + 4));
        printf("hm: page %d\n", atoi(a + 4));
    } else if (!strncmp(a, "face", 4) && strncmp(a, "faces", 5)) {
        const char *s = a + 4;
        while (*s == ' ') s++;
        int f = *s >= '0' && *s <= '9' ? atoi(s) : hm_face_by_name(s);
        if (f < 0 || f > HM_FACE_NIGHT) {
            printf("hm: no face \"%s\"\n", s);
            return;
        }
        hm_faces_set(f);
        if (f < HM_NFACES) {
            hm_cfg()->face = f;
            hm_cfg()->faces |= 1 << f;
            hm_cfg_save();
        }
        if (HM.active && HM.page != HM_PG_CLOCK) hm_go(HM_PG_CLOCK);
        printf("hm: face %s\n", HM_FACE_NAME[f]);
    } else if (!strncmp(a, "step", 4) || !strncmp(a, "faces", 5)) {
        hm_faces_dev(a);
    } else if (!strncmp(a, "tree", 4)) {
        hm_tree_dev();
    } else if (!strncmp(a, "night", 5)) {
        const char *s = a + 5;
        while (*s == ' ') s++;
        HM.night_force = !strcmp(s, "1") ? 1 : !strcmp(s, "0") ? 0 : -1;
        printf("hm: night %s\n", HM.night_force < 0 ? "by the clock" : HM.night_force ? "on" : "off");
    } else if (!strncmp(a, "song", 4)) {
        /* "hm song artist|album|title", or "hm song off": a made-up song as the PC's, for its cover and tint */
        const char *s = a + 4;
        while (*s == ' ') s++;
        if (!*s || !strcmp(s, "off")) {
            home_pc_fake(NULL, NULL, NULL);
            printf("hm: no made-up song\n");
            return;
        }
        char buf[200], *f[3] = { NULL, NULL, NULL };
        snprintf(buf, sizeof buf, "%s", s);
        f[0] = buf;
        for (int i = 1; i < 3 && f[i - 1]; i++) {
            char *bar = strchr(f[i - 1], '|');
            if (bar) {
                *bar = 0;
                f[i] = bar + 1;
            }
        }
        if (f[2]) home_pc_fake(f[0], f[1], f[2]);
        else home_pc_fake(f[0], NULL, f[1] ? f[1] : f[0]);
        printf("hm: made-up song \"%s\"\n", f[2] ? f[2] : f[1] ? f[1] : f[0]);
    } else if (!strncmp(a, "prof", 4)) {
        for (int i = 0; i < 10; i++) {
            printf("hm prof: %-12s %7.1f ms, worst %5.1f ms\n", PROF_NAME[i], s_prof[i] * 1000, s_prof_max[i] * 1000);
            s_prof[i] = s_prof_max[i] = 0;
        }
    } else if (!strncmp(a, "art", 3)) {
        hm_art_dev(a + 3);
    } else {
        printf("hm: the last minute was shown %d ms after it turned\n", s_minute_late_ms);
        printf("hm: %s, page %d, face %s%s; sky %s, np %s, faces %s\n", HM.active ? "active" : "off", HM.page,
               HM_FACE_NAME[hm_faces_face() < 0 ? 0 : hm_faces_face()], HM.night_on ? " (night)" : "",
               HM.sky.px ? "held" : "free", hm_np_released() ? "free" : "held", hm_faces_released() ? "free" : "held");
        hm_art_dev("");
    }
}

void ui_home_mode_init(ui_home_mode_fn on_change)
{
    HM.on_change = on_change;
    if (HM.built) return; /* built early by a first enter: the shell's listener is all that was missing */
    hm_cfg();
    hm_cfg_apply();
    build();
    ui_pet_init();
    bz_motion_init(&HM.k, 0, 0.001f);
    HM.k.keep = true; /* animates though the lean renderer makes other motion instant */
    /* sideways anywhere on home mode: its pages (the clock page's own flick up and down is its faces) */
    bz_drag_t d = { .begin = pg_begin, .move = pg_move, .end = pg_end, .axis = 1, .slop = 10 };
    bz_drag_attach(HM.root, &d);
    lv_obj_remove_flag(HM.root, LV_OBJ_FLAG_EVENT_BUBBLE);
    bz_ui_on_frame(hm_frame, NULL);
    ui_on_refresh(hm_refresh, NULL);
    home_player_rescan();
    if (hm_cfg()->start_home) {
        /* at boot: no sheet, it's simply there */
        HM.why = UI_HOME_BY_BOOT;
        HM.active = true;
        HM.last_min = -1;
        open_page();
        hm_faces_set(hm_cfg()->face);
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
