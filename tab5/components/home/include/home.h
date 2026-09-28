/* home — the services behind home mode: the PC's media through Catalyst Link, Home Assistant, the weather,
 * and the music player for the microSD card.
 *
 * Threading. Everything that touches the network runs on one worker ("home", hal_thread, core 0), which
 * lives only while something wants it: the UI calls home_want() from its 10 Hz refresh for what is on
 * screen, and the worker ends itself ~15 s after the last want, giving its internal-RAM stack back. Results
 * land in state behind a mutex; the UI copies them out with the *_get() calls and redraws only when a
 * generation counter moves. Commands (play/pause, a light's toggle) are queued and sent at once.
 *
 * Settings are the UI's (hal_kv, UI thread only: the worker may run on a PSRAM stack, which must not touch
 * NVS). The UI reads them and hands them over with the *_config() calls. Secrets (the Home Assistant token)
 * are never logged or shown. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the worker ---- */
enum {
    HOME_WANT_PC = 1,        /* the PC's now-playing, every second */
    HOME_WANT_HA = 2,        /* the picked Home Assistant entities, every 5 s */
    HOME_WANT_WEATHER = 4,   /* the weather, every 20 min */
    HOME_WANT_TBA = 8,       /* The Blue Alliance (tba.h), every 20-60 s */
};
/* From the UI's refresh: keeps the worker (and those polls) going for a few seconds more. */
void home_want(unsigned mask);

/* Text from the network for fonts that carry Latin-1 and a little punctuation: curly quotes to straight,
 * an ellipsis to "...", dashes to "-", anything else outside Latin-1 (emoji, CJK) dropped. In place. */
void home_fold_text(char *s);

/* ---- the PC's media, through Catalyst Link (GET /media/now, /media/art, POST /media/control) ---- */
#define HOME_ART 420          /* album art: HOME_ART × HOME_ART RGB565 (home_art.h's HOME_ART_L: the big view's) */
typedef struct {
    bool link;                /* the Link is configured and answering */
    bool available;           /* it serves the media remote (a Link without it, or no Windows media: false) */
    char reason[96];          /* why not, when !available */
    bool have;                /* something is (or was just) playing */
    char title[96], artist[80], album[64], app[32];
    char state[12];           /* "playing", "paused", "stopped", "changing", "closed", "opened" */
    bool playing;
    double position, duration; /* seconds; < 0 unknown. position as of `at` */
    double at;                /* hal_seconds() when read: position runs on from there while playing */
    bool can_next, can_prev;
    float volume;             /* 0..1, < 0 unknown (the PC has no pycaw) */
    bool muted;
    unsigned art_gen;         /* changes when new art arrives (or goes) */
    bool art;                 /* there is art for this song */
    bool art_ready;           /* ...and it has arrived: home_pc_art() copies this song's, not the last one's */
    unsigned gen;             /* changes with anything above */
} home_pc_t;
void home_pc_enable(bool on);                  /* settings: the Link media remote on/off */
void home_pc_get(home_pc_t *out);
/* Copies the current art (HOME_ART² RGB565) into `px`; false when there is none. `gen` (may be NULL) gets the
 * art_gen of what was copied: a worker copying it while a new song's arrives knows which one it has. */
bool home_pc_art(uint16_t *px, unsigned *gen);
/* "play", "pause", "toggle", "next", "previous", "volume_up", "volume_down", "mute"; queued. */
void home_pc_cmd(const char *action);
void home_pc_volume(float level01);            /* needs pycaw on the PC */

/* ---- Home Assistant (REST: /api/states, /api/template, /api/services) ---- */
#define HOME_HA_PICKS 24          /* entities picked for the smart home app; the first few show on home mode */
#define HOME_HA_LIST 200
typedef enum {
    HA_LIGHT, HA_SWITCH, HA_FAN, HA_SCENE, HA_SCRIPT, HA_BUTTON, HA_SENSOR, HA_BINARY, HA_MEDIA, HA_COVER,
    HA_CLIMATE, HA_LOCK, HA_OTHER
} home_ha_kind_t;
typedef struct {
    char id[64];              /* "light.desk" */
    char name[48];            /* friendly_name, or the id */
    char state[24];           /* "on", "off", "21.5", "playing", "unavailable" */
    char unit[12];            /* sensors: "°C", "%" */
    char area[32];            /* its room in Home Assistant ("" when it has none, or the token can't say) */
    home_ha_kind_t kind;
    bool on;                  /* shown lit: on, open, playing, home */
    bool actionable;          /* a tap does something (toggle, turn on, play/pause) */
    bool pending;             /* a tap sent, not yet confirmed */
    bool dimmable;            /* a light with a brightness */
    int brightness;           /* 0..100 while on (0 off); -1 unknown */
    float target, current;    /* climate: the set point and the room's temperature; NAN unknown */
    float step;               /* climate: the set point's step (0.5 by default) */
} home_ha_entity_t;
typedef struct {
    bool configured;          /* a URL and a token */
    bool ok;                  /* the last poll answered */
    int status;               /* its HTTP status (-1 unreachable) */
    char err[80];             /* what went wrong, in words */
    double last_ok;
    unsigned gen;             /* changes with the tiles */
} home_ha_status_t;
/* url like "http://192.168.1.30:8123"; token a long-lived access token; picks a comma list of entity ids. */
void home_ha_config(const char *url, const char *token, const char *picks);
void home_ha_status(home_ha_status_t *out);
int home_ha_tiles(home_ha_entity_t *out, int max);  /* the picks, in order */
void home_ha_tap(const char *entity_id);            /* the natural action for its kind; queued */
void home_ha_brightness(const char *entity_id, int pct);   /* a light: 0 turns it off; queued (the latest wins) */
void home_ha_set_temp(const char *entity_id, float target); /* a thermostat's set point; queued (the latest wins) */
home_ha_kind_t home_ha_kind(const char *entity_id);
/* The entity picker: fetches every entity once (GET /api/states). Poll home_ha_list() for the result. */
void home_ha_list_request(void);
/* The list so far: count, or -1 while fetching; `err` says why it's empty. Sorted by kind, then name. */
int home_ha_list(home_ha_entity_t *out, int max, char *err, size_t errn);
/* Looks for Home Assistant on the LAN (mDNS _home-assistant._tcp). Poll home_ha_found(). */
void home_ha_discover(void);
/* 0 still looking, 1 found (url filled), -1 nothing answered. */
int home_ha_found(char *url, size_t n);

/* ---- the weather (Open-Meteo: no key) ---- */
typedef struct {
    bool configured;          /* a place is set */
    bool ok;
    float temp, hi, lo;       /* in the chosen unit */
    int code;                 /* WMO weather code */
    bool day;
    bool fahrenheit;
    char place[48];           /* as resolved ("Seattle, US" or "47.61, -122.33") */
    char err[64];
    unsigned gen;
} home_weather_t;
/* The forecast, from the same request: the next 24 hours and 7 days, for the weather app. */
#define HOME_WX_HOURS 24
#define HOME_WX_DAYS 7
typedef struct {
    bool ok;
    float feels;              /* apparent temperature now */
    int humidity;             /* %, -1 unknown */
    float wind;               /* km/h (mph with fahrenheit) */
    char sunrise[8], sunset[8]; /* "7:02", "" unknown */
    int nhours;
    struct { int hour; float temp; int code; int pop; bool day; } hour[HOME_WX_HOURS]; /* pop: rain chance % */
    int ndays;
    struct { int wday; int mday; float hi, lo; int code; int pop; } day[HOME_WX_DAYS]; /* wday 0 = sunday */
    unsigned gen;             /* the same generation as home_weather_t's */
} home_forecast_t;
/* `where`: "47.61,-122.33" or a place name ("Seattle"), resolved once through Open-Meteo's geocoder. */
void home_weather_config(const char *where, bool fahrenheit);
void home_weather_get(home_weather_t *out);
void home_weather_forecast(home_forecast_t *out);
void home_weather_refresh(void);                    /* read it again now (the weather app's "refresh") */
const char *home_weather_text(int code);            /* "clear", "rain", ... */

/* ---- the music player: MP3 and WAV from <sd>/CATOS/AUDIO, out through hal_play.h ---- */
#define HOME_TRACKS 64
typedef enum { HP_IDLE, HP_LOADING, HP_PLAYING, HP_PAUSED, HP_ERROR } home_play_state_t;
typedef struct {
    home_play_state_t state;
    int index;                /* the track, -1 none */
    char title[96];           /* the file name without its extension */
    double position, duration; /* seconds; duration < 0 unknown */
    char err[80];
    bool mp3;                 /* this build decodes MP3 (else WAV only) */
    unsigned gen;
} home_play_t;
/* Lists the folder again (on a worker; the list changes its generation when done). */
void home_player_rescan(void);
int home_player_tracks(char names[][64], int max, unsigned *gen); /* file names, sorted; -1 while scanning */
const char *home_player_dir(void);                 /* "/sdcard/CATOS/AUDIO", or NULL with no card */
void home_player_play(int index);
void home_player_toggle(void);                      /* pause / resume (plays the first track when idle) */
void home_player_skip(int dir);                     /* next (+1) / previous (-1) */
void home_player_stop(void);
void home_player_get(home_play_t *out);

/* ---- the timed photo: home mode's own snapshot, every few minutes, kept only up to a limit ----
 *
 * home_capture_tick() is called once a second from ui_home_mode.c's frame hook, whether or not home mode is
 * on screen; it does nothing unless `home_active` is true, the setting is on, the period has elapsed, there
 * is a card, and `camera_free` (nothing else has a window open: the lens app's own camera use, or just
 * someone using the tablet, in which case an unattended photo isn't the point anyway). When it's due, it
 * fires a one-shot worker (hal_thread): camera on, wait for a frame, encode and write it, camera off. The
 * camera is never left running between shots — a photo every five minutes doesn't justify keeping the ISP
 * and the P4's video pipeline powered the other four minutes and fifty-some seconds, and a lit camera is
 * exactly the thing this feature's own privacy story asks it not to be by default. */
#define HOME_CAP_KEEP_DEFAULT 300   /* a ring of the newest N; at ~50-150 KB a JPEG that's tens of MB, not GB */
#define HOME_CAP_MARGIN_MB 32       /* free space this feature tries to keep clear by trimming its own oldest */

void home_capture_config(bool enabled, int period_min, int keep); /* settings > home; period/keep <= 0 uses the default */
typedef struct {
    bool enabled;
    bool live;                 /* the camera is on for a shot right now: show something on screen while it is */
    double last_shot;          /* hal_seconds() of the last photo actually saved; 0 none yet */
    int kept;                  /* photos on the card under this feature's own tree, as of the last sweep */
    char reason[64];           /* "" when off or fine; else why the last attempt didn't save one, in words */
} home_capture_status_t;
void home_capture_status(home_capture_status_t *out);
void home_capture_tick(double now, bool home_active, bool camera_free);

/* The pure retention decision (home_capture_policy.c: no I/O, no hal — the part worth testing without a
 * card or a camera). `files` in any order; `del[i]` is marked true, oldest (smallest mtime) first, until the
 * count still standing is <= keep AND free_bytes plus whatever this call frees is >= margin_bytes (deleting
 * every file it was given if that still isn't enough: it only ever sees its own tree, never the whole card).
 * Returns how many it marked. */
typedef struct {
    double mtime;               /* newer is greater; only the ordering matters */
    uint64_t bytes;
} home_cap_file_t;
int home_capture_plan(const home_cap_file_t *files, int n, int keep, uint64_t free_bytes, uint64_t margin_bytes,
                      bool *del);

/* ---- room-occupied signal from the microphones' ambient level ----
 * NOT person detection: a fan, a TV or a dishwasher reads the same as a person talking. The honest
 * alternative was the camera's frame-to-frame difference, which is motion, not presence, and needs the
 * camera running far more often than the timed photo above does — fighting that feature's own "off unless
 * it's actually taking a photo" story. The microphones are cheap to sample instead, and this is a purely
 * passive reader of them: it never calls hal_mic_start() or hal_mic_stop() itself. Home mode's own wake
 * word already keeps the microphones open the whole time home mode is showing (ui_home_mode_showing() ->
 * voice_enable(), in ui_home_mode.c and assist's voice.c), so presence just samples the level that's
 * already being produced (hal_mic_on() + hal_mic_level()) on its own poll; when nothing is already
 * listening it takes no sample and waits, so it can never contend for the codec and never keeps it powered
 * on its own account (an earlier version that duty-cycled the mic itself did exactly that — opened and
 * closed it every few seconds regardless of the wake word — which was a real bug, not a style choice). It
 * doesn't get its own settings toggle the way the camera does: it never records or transmits audio, keeps
 * only a coarse level in RAM, and only ever polls while home mode itself is on screen (ui_home_mode.c's
 * go(), like the eyes' blink and the stand watcher already do without one). A future owner-facing switch
 * would be a one-line addition to home_presence_set_active's caller. */
typedef enum { HOME_PRESENCE_UNKNOWN, HOME_PRESENCE_EMPTY, HOME_PRESENCE_OCCUPIED } home_presence_state_t;

void home_presence_config(float threshold01, double poll_s, double confirm_s, double quiet_s); /* all tunable */
void home_presence_set_active(bool home_active); /* home mode entering/leaving screen: gates the poll */
/* UI thread, once a frame; internally throttled to poll_s. Never touches hal_mic_start/stop — see above. */
void home_presence_tick(double now);
home_presence_state_t home_presence_state(void);
double home_presence_last_seen(void);     /* hal_seconds() the level last crossed the threshold; 0 never */
/* now - last_seen, or a large number with no signal yet: what ui_shell.c's dim/sleep timer blends in, home
 * mode only (ui_home_mode_presence_idle_s) — so home mode brightens when the room's making noise and dims on
 * the same idle timer as everything else once it's been quiet a while, rather than a second timer of its own. */
double home_presence_idle_s(double now);

/* The pure hysteresis step (home_presence_policy.c: no I/O, no hal). CONFIRM_S of sustained level at or
 * above `threshold` before OCCUPIED; QUIET_S of sustained quiet before EMPTY (so one loud moment doesn't
 * flip it, and one quiet pause doesn't either); a fresh track goes straight to EMPTY on its first quiet
 * sample (absence is the default that needs no confirming) but still needs a confirm_s run of loud ones
 * before its first OCCUPIED. */
typedef struct {
    home_presence_state_t state;
    double above_since, below_since;  /* 0: not currently in that run */
    double last_seen;                  /* 0: never */
} home_presence_track_t;
void home_presence_step(home_presence_track_t *t, double now, float level, float threshold, double confirm_s,
                        double quiet_s);

#ifdef __cplusplus
}
#endif
