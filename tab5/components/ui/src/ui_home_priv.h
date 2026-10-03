/* Home mode's settings, shared by the surface (ui_home_mode.c), its pages (the desk there, now playing in
 * ui_home_np.c, the clock faces in ui_home_faces.c), its settings pane (ui_home_settings.c), its apps (music,
 * smart home, weather), the covers (ui_home_art.c) and the NFC tag. Private to the ui component. */
#pragma once
#include "home.h"
#include "home_art.h"
#include "ui_home_mode.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

typedef struct hm_art hm_art_t; /* a cover (ui_home_art.c) */

typedef struct {
    bool start_home;       /* "hm_start": the tablet starts in home mode */
    bool stand;            /* "hm_stand": home mode by itself on the desk stand */
    bool fahrenheit;       /* "hm_fahr" */
    bool pc;               /* "hm_pc": the PC's media through Catalyst Link */
    int saver_min;         /* "hm_saver": the card's photos after this many minutes untouched; 0 off */
    char tag[24];          /* "hm_tag": the NFC tag's UID (hex) that brings home mode; "" none */
    char net[33];          /* "hm_net": the home's Wi-Fi; set, home mode comes by itself only on it */
    char place[64];        /* "hm_place": "47.61,-122.33" or "Seattle" */
    char ha_url[128];      /* "ha_url" */
    char ha_token[320];    /* "ha_token": never logged, never shown */
    char picks[HOME_HA_PICKS * 64 + 8]; /* "ha_picks": entity ids, comma separated */
    /* the look (settings > look) */
    int cards;             /* "hm_cards": HM_CARD_* bits, what home mode shows */
    bool small_clock;      /* "hm_clock": "1" the clock in the display face instead of the big one */
    bool seconds;          /* "hm_secs": seconds beside the clock */
    bool tint;             /* "hm_bg": "1" the ground tinted with the accent */
    bool art_tint;         /* "hm_art": the surfaces take their colour from what's playing (default on) */
    int faces;             /* "hm_fset": the clock faces a flick up or down goes through (HM_FACE_* bits; kept by name) */
    int face;              /* "hm_fshow": the face the clock page shows (HM_FACE_*; kept by name) */
    int open_page;         /* "hm_open": the page home mode opens on (HM_PG_*) */
    bool idle_pet;         /* "hm_idlepet": untouched for a few minutes, home mode shows the pet */
    bool night;            /* "hm_night": the night face from 22:00 to 7:00 once nobody has touched it for a minute */
    bool dial_secs;        /* "hm_dsec": the dial's second hand (default on) */
    /* the timed photo (settings > home > photo): off by default, see home.h and home_capture.c */
    bool cam_on;           /* "hm_cam_on" */
    int cam_min;           /* "hm_cam_min": minutes between shots, default 5 */
    int cam_keep;          /* "hm_cam_keep": the ring size, default HOME_CAP_KEEP_DEFAULT */
} hm_cfg_t;

/* Where the weather starts, until the settings screen says otherwise. home_weather.c knows
 * this one by name (its KNOWN table), so it resolves with no geocoder round trip. */
#define HM_PLACE_DEFAULT "Mission Viejo"

enum {
    HM_CARD_MUSIC = 1, HM_CARD_COMPANION = 2, HM_CARD_HOME = 4, HM_CARD_WEATHER = 8, HM_CARD_MATCH = 16,
    HM_CARD_ALL = 31
};

/* the pages, left to right: the clock faces, the desk (home mode's own), what's playing */
enum { HM_PG_CLOCK, HM_PG_DESK, HM_PG_NP, HM_PG_PET, HM_NPAGES };

/* the clock faces, in the order a flick goes through them (ui_face_*.c); the night face is not in the cycle, it
 * comes by itself. Settings keep them by name, so the order can change. */
enum {
    HM_FACE_TYPE, HM_FACE_FLIP, HM_FACE_DIAL, HM_FACE_CHRONO, HM_FACE_SKY, HM_FACE_MOON, HM_FACE_ORRERY,
    HM_FACE_WORLD, HM_FACE_TREE, HM_FACE_MATCH, HM_FACE_WORDS, HM_FACE_PET, HM_NFACES, HM_FACE_NIGHT = HM_NFACES
};
#define HM_FACES_ALL ((1 << HM_NFACES) - 1)
extern const char *const HM_FACE_NAME[HM_NFACES + 1];
/* a face by name (the settings, the dev console; the round-one names too: horizon is the sky now), -1 none */
int hm_face_by_name(const char *name);

/* The look changed (settings > look): home mode lays itself out again, now if it's built, else when it is. */
void hm_look_changed(void);
/* Settings > look: home mode's cards, clock and ground, into a flex column `col` of width w. */
void ui_home_look_settings(lv_obj_t *col, int w);
void ui_home_look_settings_open(void);

/* the picks home mode itself shows (the smart home app shows them all) */
#define HM_HOME_TILES 4

hm_cfg_t *hm_cfg(void);    /* loaded from the kv store on first use (UI thread) */
void hm_cfg_save(void);    /* writes it back and hands it to the services (UI thread: it writes NVS) */
void hm_cfg_apply(void);   /* hands it to the services without writing */
void hm_settings_request(void); /* the next settings open lands on the home section */
void hm_open_settings(lv_obj_t *from); /* the settings app, on its home section */

/* "3:07" (or "1:02:03"); "-:--" when unknown */
void hm_fmt_time(double s, char *out, size_t n);
/* A Home Assistant entity's glyph and its state in a few words ("on", "21.5 °C", "tap"). */
const char *hm_ha_icon(home_ha_kind_t k, const char *unit);
void hm_ha_state_text(const home_ha_entity_t *e, char *out, size_t n);

/* The NFC tag (a Unit RFID 2 on Port A; hal_nfc_*): the next tag the reader sees becomes home mode's. */
void hm_tag_pair_begin(void);
void hm_tag_pair_cancel(void);
/* 0 nothing going on, 1 waiting for a tag, 2 a tag was just paired (once), -1 no reader */
int hm_tag_pair_state(void);
/* the tablet is at home: on the home Wi-Fi, or anywhere when none is set */
bool hm_at_home(void);

/* ---- small shared pieces (ui_home_mode.c) ---- */
void hm_show(lv_obj_t *o, bool on);               /* the hidden flag, touched only on a change */
/* a local fill / text colour over the object's shared style, or (rgb = HM_ROLE) back to the style */
#define HM_ROLE 0xFFFFFFFFu
void hm_fill(lv_obj_t *o, uint32_t rgb);
void hm_ink(lv_obj_t *o, uint32_t rgb);
uint16_t hm_565(uint32_t rgb);                     /* the exact 565 value LVGL paints for rgb */
/* the song on show: the tablet's own player when it plays, else the PC's */
typedef struct {
    bool local;               /* the tablet's player (the card's songs) */
    bool have;                /* something is playing or paused */
    bool playing;
    bool can;                 /* the transport does something */
    char src[48];             /* "on the pc · spotify", "this tablet · microsd" */
    char title[96];
    char artist[80], album[64];
    char hint[112];           /* nothing playing: what to do about it */
    double pos, dur;          /* seconds, running on while playing; < 0 unknown */
    unsigned gen;             /* changes with anything but the position */
} hm_song_t;
void hm_song(hm_song_t *s);
const hm_art_t *hm_song_art(const hm_song_t *s); /* its cover: NULL while it's painted (and with nothing playing) */
void hm_cmd(const char *action);                 /* the transport, for whichever player is on show */
/* The surfaces' colours from what's playing, when the art tint is on and something is: NULL otherwise. The
 * generation changes whenever the answer does (a new song, the tone, the setting). */
const home_tint_t *hm_tint(unsigned *gen);
double hm_now(void);
/* a weather condition drawn (ui_home_wx.c): WMO code, day or night, on a flat ground of the tile's 565 value */
void hm_wx_glyph(uint16_t *px, int w, int h, uint16_t ground, int code, bool day, bool dark);

/* ---- covers (ui_home_art.c) ---- */
const hm_art_t *hm_art_pc(const home_pc_t *pc);
uint32_t hm_art_pc_key(const home_pc_t *pc); /* 0: nothing, or its own art is on its way */
const hm_art_t *hm_art_song(const char *artist, const char *album, const char *title);
const home_art_info_t *hm_art_info(const hm_art_t *a);
unsigned hm_art_serial(const hm_art_t *a);
uint32_t hm_art_key(const hm_art_t *a);
/* sets the small cover (HOME_ART_S²) on an image when it changed since *shown; true if it did */
bool hm_art_set(lv_obj_t *img, const hm_art_t *a, unsigned *shown);
/* the hero, painted on the worker into px (w × h): the cover big at (cx, cy), its shadow, the ground around */
bool hm_art_hero(const hm_art_t *a, uint16_t *px, int w, int h, int cx, int cy, int radius, uint16_t ground, bool dark);
unsigned hm_art_hero_done(void);   /* the serial of the last hero finished, 0 while one is being painted */
bool hm_art_busy(void);            /* the worker is running: nothing it paints into may be freed */
void hm_art_trim(void);            /* gives the covers' memory back */
/* any other picture (a clock face's): fn(arg) run on the same worker; the ticket says when it has */
unsigned hm_art_job(void (*fn)(void *), void *arg);
bool hm_art_job_done(unsigned ticket);
void hm_art_dev(const char *args);

/* ---- the now-playing page (ui_home_np.c) ---- */
lv_obj_t *hm_np_build(lv_obj_t *parent);
void hm_np_refresh(bool shown);    /* 10 Hz while home mode is up; `shown`: it's the page on screen */
void hm_np_release(void);          /* home mode left: its picture's memory back (when the worker allows) */
bool hm_np_released(void);

/* ---- the clock faces (ui_home_faces.c) ---- */
lv_obj_t *hm_faces_build(lv_obj_t *parent);
void hm_faces_refresh(bool shown); /* 10 Hz */
void hm_faces_frame(double now);   /* every frame while its page shows: the second hand */
void hm_faces_set(int face);       /* shows this face (drawn now: call while it isn't on screen, or accept a redraw) */
int hm_faces_face(void);
void hm_faces_look(void);          /* its options changed: written again on the next refresh */
void hm_faces_release(void);
bool hm_faces_released(void);
bool hm_faces_night(void);         /* night now (the night face is on and it's the hours for it) */
/* a vertical flick on the clock page: the next (+1) or previous (-1) face slides in */
void hm_faces_step(int dir);
bool hm_faces_moving(void);
/* the dev console: "hm step up|down" as a flick would, and a face's own ("hm tree ...") */
void hm_faces_dev(const char *args);
