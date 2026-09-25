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
};
/* From the UI's refresh: keeps the worker (and those polls) going for a few seconds more. */
void home_want(unsigned mask);

/* Text from the network for fonts that carry Latin-1 and a little punctuation: curly quotes to straight,
 * an ellipsis to "...", dashes to "-", anything else outside Latin-1 (emoji, CJK) dropped. In place. */
void home_fold_text(char *s);

/* ---- the PC's media, through Catalyst Link (GET /media/now, /media/art, POST /media/control) ---- */
#define HOME_ART 160          /* album art: HOME_ART × HOME_ART RGB565 */
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
    unsigned gen;             /* changes with anything above */
} home_pc_t;
void home_pc_enable(bool on);                  /* settings: the Link media remote on/off */
void home_pc_get(home_pc_t *out);
/* Copies the current art (HOME_ART² RGB565) into `px`; false when there is none. */
bool home_pc_art(uint16_t *px);
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

#ifdef __cplusplus
}
#endif
