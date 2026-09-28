/* ui_actions — what the assistants are allowed to DO to the tablet, the PC and the house.
 *
 * Both AIs' AS_T_DEVICE tools arrive here (the technician's in Assist, the companion's in her voice):
 * open an app or a settings section, the tablet's own settings (brightness, volume, tone, accent, calm,
 * the flashlight, sleep and lock, home mode), its status, the timer, the clock's alarms, reminders with
 * words, the notes, the documents on the card, the checklist, the calendar, the weather, the
 * notifications, the music and Home Assistant. assist cannot call the ui (`ui` REQUIRES `assist` and
 * `home`, not the other way round), so the assistant takes a handler it is handed, exactly as it takes the
 * desk text and the TBA reader. Each app keeps its own data; the tools reach it through ui_sys.h's hooks,
 * so the companion's timer is the timer app's timer and her alarm is the clock's.
 *
 * The other half of the job is the thread. A tool runs on an assistant's worker; LVGL belongs to the UI
 * thread and corrupts itself if touched from anywhere else. So the worker parks the request under a lock
 * and waits, the 10 Hz refresh picks it up, does the work and writes the answer back. Reading the card
 * (documents, the calendar) and the weather need no UI and run on the worker itself.
 *
 * What would take the screen away from the companion -- another app, sleep, the lock screen, home mode --
 * waits until she has finished speaking: closing her face stops her mid-sentence and cancels the answer
 * she was still putting together, so "open the weather" used to open the weather and say nothing at all.
 *
 * Nothing here touches the robot. Every AS_T_CHANGE tool — a tunable, an auto choice, a patch — still
 * goes through its confirmation card; these do not, because a lamp that demanded a tap on a screen before
 * it would turn on is not worth having. */
#include "ui_internal.h"
#include "ui_home_mode.h"
#include "ui_storage.h"
#include "ui_sys.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "assist.h"
#include "hal.h"
#include "home.h"
#include "json_lite.h"
#include "voice.h"

#define WAIT_MS 4000       /* the UI runs at 10 Hz: ~40 frames; a timeout is reported */
#define DEFER_MAX_S 25.0   /* a screen change waiting on the companion's voice goes ahead after this anyway */
#define NREM 8             /* reminders kept */

/* One request, on the heap: its buffers can be kilobytes (a document), and a request the worker stopped
 * waiting for is finished and freed by the UI thread instead. */
typedef struct {
    char action[24];
    char *args;
    char *out;
    size_t outn;
    int pool;
    bool ok, taken, done, abandoned;
} job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static job_t *g_job; /* the request waiting for the UI thread, or being done by it */

/* ---------------------------------------------------------------- reading the arguments */

typedef struct {
    jl_tok_t toks[64];
    jl_doc_t d;
    bool ok;
} args_t;

static void args_parse(args_t *a, const char *json)
{
    int n = jl_parse(json, strlen(json), a->toks, 64);
    a->d.js = json;
    a->d.t = a->toks;
    a->d.n = n;
    a->ok = n > 0;
}

static bool args_str(args_t *a, const char *key, char *out, size_t n)
{
    int t = a->ok ? jl_get(&a->d, 0, key) : -1;
    return t >= 0 && jl_str(&a->d, t, out, n);
}

static bool args_num(args_t *a, const char *key, double *out)
{
    int t = a->ok ? jl_get(&a->d, 0, key) : -1;
    if (t < 0) return false;
    *out = jl_num(&a->d, t, 0);
    return true;
}

/* a boolean member: 1 true, 0 false, -1 absent */
static int args_bool(args_t *a, const char *key)
{
    int t = a->ok ? jl_get(&a->d, 0, key) : -1;
    if (t < 0) return -1;
    jl_type_t ty = a->d.t[t].type;
    return ty == JL_TRUE ? 1 : ty == JL_FALSE ? 0 : -1;
}

/* "14:30", "9:05" → 1430, 905; -1 otherwise */
static int parse_hhmm(const char *s)
{
    int h, m, used = 0;
    if (!s || sscanf(s, "%d:%d%n", &h, &m, &used) != 2 || s[used] || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 100 + m;
}

/* "once", "daily", "weekdays", "weekends", or days by their first two letters ("mo,we,fr", "monday tue"):
 * bit n is tm_wday n; 0 once; -1 unreadable */
static int parse_days(const char *s)
{
    if (!s || !s[0] || !strcasecmp(s, "once")) return 0;
    if (!strcasecmp(s, "daily") || !strcasecmp(s, "every day") || !strcasecmp(s, "everyday")) return 0x7f;
    if (!strcasecmp(s, "weekdays")) return 0x3e;
    if (!strcasecmp(s, "weekends")) return 0x41;
    static const char *const D[7] = { "su", "mo", "tu", "we", "th", "fr", "sa" };
    int bits = 0;
    for (const char *p = s; *p;) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;
        char w[3] = { (char)tolower((unsigned char)p[0]), (char)tolower((unsigned char)p[1]), 0 };
        int k = -1;
        for (int i = 0; i < 7; i++)
            if (!strcmp(w, D[i])) k = i;
        if (k < 0) return -1;
        bits |= 1 << k;
        while (*p && isalpha((unsigned char)*p)) p++;
    }
    return bits;
}

static bool now_tm(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
    return tm->tm_year > 120;
}

/* "2026-09-27", "today", "tomorrow" → 20260927; 0 otherwise */
static int parse_date(const char *s)
{
    struct tm tm;
    if (!s || !s[0] || !strcasecmp(s, "today") || !strcasecmp(s, "tomorrow")) {
        if (!now_tm(&tm)) return 0;
        if (s && !strcasecmp(s, "tomorrow")) {
            tm.tm_mday++;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            localtime_r(&t, &tm);
        }
        return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    }
    int y, m, d, used = 0;
    if (sscanf(s, "%d-%d-%d%n", &y, &m, &d, &used) != 3 || s[used] || y < 2000 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    return y * 10000 + m * 100 + d;
}

static int clamp_pct(double v) { return v < 0 ? 0 : v > 100 ? 100 : (int)(v + 0.5); }

/* appends to out, never past n */
static void addf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void addf(char *out, size_t n, const char *fmt, ...)
{
    size_t l = strlen(out);
    if (l + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + l, n - l, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- the screen, when the companion is done */

typedef enum { DF_NONE, DF_APP, DF_SETTINGS, DF_MEMORY, DF_LIGHT, DF_SLEEP, DF_LOCK, DF_HOME_ON, DF_HOME_OFF } defer_t;
static struct {
    defer_t what;
    const ui_app_t *app;
    char section[20];
    int pool;
    double since;
} DF;

/* The companion is in the middle of a turn: thinking (its tools run then) or saying the answer. */
static bool companion_busy(void)
{
    vo_state_t v = voice_state();
    return v == VO_THINKING || v == VO_SPEAKING || v == VO_HEARING;
}

static void screen_do(defer_t what, const ui_app_t *app, const char *section, int pool)
{
    switch (what) {
    case DF_APP: ui_app_open(app, NULL); break;
    case DF_SETTINGS: ui_settings_show(section); break;
    case DF_MEMORY: ui_memory_open(pool, NULL); break;
    case DF_LIGHT: if (!ui_app_is_open(&APP_LIGHT)) ui_app_open(&APP_LIGHT, NULL); break;
    case DF_SLEEP: ui_sleep_now(); break;
    case DF_LOCK: ui_lock_show(); break;
    case DF_HOME_ON: if (!ui_home_mode_active()) ui_home_mode_enter(); break;
    case DF_HOME_OFF: if (ui_home_mode_active()) ui_home_mode_exit(); break;
    default: break;
    }
}

/* Now, or (for the companion, while she is talking) once she has finished. Returns true when deferred. */
static bool screen(defer_t what, const ui_app_t *app, const char *section, int pool)
{
    if (pool == AS_POOL_COMPANION && companion_busy()) {
        DF.what = what;
        DF.app = app;
        snprintf(DF.section, sizeof DF.section, "%s", section ? section : "");
        DF.pool = pool;
        DF.since = hal_seconds();
        return true;
    }
    screen_do(what, app, section, pool);
    return false;
}

static void defer_tick(double now)
{
    if (DF.what == DF_NONE) return;
    /* a moment after her last word, so the answer's tail isn't cut; or after a while whatever happens */
    static double quiet_since;
    if (companion_busy()) quiet_since = 0;
    else if (!quiet_since) quiet_since = now;
    if ((quiet_since && now - quiet_since > 0.6) || now - DF.since > DEFER_MAX_S) {
        defer_t w = DF.what;
        DF.what = DF_NONE;
        quiet_since = 0;
        screen_do(w, DF.app, DF.section, DF.pool);
    }
}

/* ---------------------------------------------------------------- the actions (UI thread) */

static bool do_open_app(args_t *a, int pool, char *out, size_t n)
{
    char name[40], section[24] = "";
    if (!args_str(a, "app", name, sizeof name) || !name[0]) {
        snprintf(out, n, "which app?");
        return false;
    }
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    args_str(a, "section", section, sizeof section);
    bool later;
    if (!strcmp(name, "memory") || !strcmp(name, "memories")) {
        later = screen(DF_MEMORY, NULL, NULL, pool);
        snprintf(out, n, "%s the memory screen, on %s memory%s", later ? "will open" : "opened",
                 pool == AS_POOL_COMPANION ? "the companion's" : "the technician's", later ? " as soon as you finish speaking" : "");
        return true;
    }
    if (section[0] || !strcmp(name, "settings")) {
        later = screen(DF_SETTINGS, NULL, section[0] ? section : "display", pool);
        snprintf(out, n, "%s settings%s%s%s", later ? "will open" : "opened", section[0] ? " on " : "", section,
                 later ? " as soon as you finish speaking" : "");
        return true;
    }
    const ui_app_t *app = ui_app_find(name);
    if (!app) {
        /* Name what was asked for: the model can then try the label it actually saw on the icon. */
        snprintf(out, n, "there is no app called \"%.20s\" on this tablet", name);
        return false;
    }
    if (app == &APP_COMPANION && pool == AS_POOL_COMPANION) {
        snprintf(out, n, "the companion is already on screen");
        return true;
    }
    later = screen(DF_APP, app, NULL, pool);
    snprintf(out, n, "%s %s%s", later ? "will open" : "opened", app->name ? app->name : name,
             later ? " as soon as you finish speaking" : "");
    return true;
}

static bool do_set_tablet(args_t *a, int pool, char *out, size_t n)
{
    double v;
    char s[16];
    bool any = false, save = false;
    out[0] = 0;
    if (args_num(a, "brightness", &v)) {
        int pct = clamp_pct(v);
        if (pct < 3) pct = 3; /* a screen at 0 looks broken, not dim */
        S.brightness = pct / 100.0f;
        hal_set_brightness(S.brightness);
        addf(out, n, "%sbrightness %d%%", any ? ", " : "", pct);
        any = save = true;
    }
    if (args_num(a, "volume", &v)) {
        int pct = clamp_pct(v);
        S.volume = pct / 100.0f;
        hal_set_volume(S.volume);
        addf(out, n, "%svolume %d%%", any ? ", " : "", pct);
        any = save = true;
    }
    if (save) ui_settings_save(); /* S kept: the next dim and wake come back to it, as the control center's do */
    int calm = args_bool(a, "calm");
    char tone[12] = "";
    bool has_tone = args_str(a, "tone", tone, sizeof tone);
    if (has_tone || calm >= 0) {
        bool dark = has_tone ? strcmp(tone, "light") != 0 : S.dark;
        ui_set_tone(dark, calm >= 0 ? calm == 1 : S.calm);
        addf(out, n, "%s%s tone%s", any ? ", " : "", dark ? "dark" : "light", S.calm ? ", calm" : "");
        any = true;
    }
    if (args_str(a, "accent", s, sizeof s)) {
        int k = -1;
        for (int i = 0; i < BZ_NACCENTS; i++)
            if (!strcmp(s, BZ_ACCENTS[i].name)) k = i;
        if (k < 0) {
            snprintf(out, n, "no accent called %.12s", s);
            return false;
        }
        ui_set_accent(k);
        addf(out, n, "%saccent %s", any ? ", " : "", s);
        any = true;
    }
    int light = args_bool(a, "flashlight");
    if (light == 1) {
        bool later = screen(DF_LIGHT, NULL, NULL, pool);
        addf(out, n, "%sflashlight on%s", any ? ", " : "", later ? " once you finish speaking (back closes it)" : " (back closes it)");
        any = true;
    } else if (light == 0) {
        if (ui_app_is_open(&APP_LIGHT)) ui_app_close();
        addf(out, n, "%sflashlight off", any ? ", " : "");
        any = true;
    }
    int home = args_bool(a, "home_mode");
    if (home >= 0) {
        bool later = screen(home ? DF_HOME_ON : DF_HOME_OFF, NULL, NULL, pool);
        addf(out, n, "%shome mode %s%s", any ? ", " : "", home ? "on" : "off", later ? " once you finish speaking" : "");
        any = true;
    }
    if (args_str(a, "screen", s, sizeof s)) {
        bool lock = !strcmp(s, "lock");
        /* the screen goes after whoever asked has finished: the technician's answer is read on it too */
        DF.what = lock ? DF_LOCK : DF_SLEEP;
        DF.pool = pool;
        DF.since = hal_seconds() - (pool == AS_POOL_COMPANION ? 0 : DEFER_MAX_S - 6);
        addf(out, n, "%sthe screen will %s in a moment", any ? ", " : "", lock ? "lock" : "go to sleep (a tap wakes it)");
        any = true;
    }
    if (!any) snprintf(out, n, "nothing to change: give what to set");
    return any;
}

static const char *play_word(home_play_state_t s)
{
    switch (s) {
    case HP_PLAYING: return "playing";
    case HP_PAUSED: return "paused";
    case HP_LOADING: return "loading";
    case HP_ERROR: return "stuck";
    default: return "stopped";
    }
}

/* The music player, either the tablet's own or the PC's through Catalyst Link's media remote. */
static bool do_music(args_t *a, char *out, size_t n)
{
    char act[16], where[12] = "";
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which action?");
        return false;
    }
    args_str(a, "where", where, sizeof where);
    bool pc = !strcmp(where, "pc");

    if (pc) {
        home_pc_t st;
        home_pc_get(&st);
        if (!st.available) {
            snprintf(out, n, "the PC's media isn't reachable: %s",
                     st.reason[0] ? st.reason : "Catalyst Link isn't answering");
            return false;
        }
        if (!strcmp(act, "status")) {
            if (!st.have) snprintf(out, n, "nothing is playing on the PC");
            else snprintf(out, n, "%s on the PC: %s%s%s", st.state, st.title,
                          st.artist[0] ? " — " : "", st.artist);
            return true;
        }
        /* The remote's own vocabulary; "play"/"pause" both map to toggle, which is what Windows
         * actually offers, and saying so beats pretending we can force a direction. */
        const char *cmd = !strcmp(act, "next") ? "next"
                        : !strcmp(act, "previous") ? "previous"
                        : (!strcmp(act, "play") || !strcmp(act, "pause") || !strcmp(act, "stop")) ? "toggle"
                        : NULL;
        if (!cmd) {
            snprintf(out, n, "no such action");
            return false;
        }
        home_pc_cmd(cmd);
        snprintf(out, n, "%s on the PC", !strcmp(cmd, "toggle") ? "play/pause" : cmd);
        return true;
    }

    home_play_t st;
    if (!strcmp(act, "status")) {
        home_player_get(&st);
        if (st.state == HP_IDLE) snprintf(out, n, "nothing is playing on the tablet");
        else snprintf(out, n, "%s: %s", play_word(st.state), st.title[0] ? st.title : "a track");
        return true;
    }
    if (!strcmp(act, "play") || !strcmp(act, "pause")) home_player_toggle();
    else if (!strcmp(act, "next")) home_player_skip(1);
    else if (!strcmp(act, "previous")) home_player_skip(-1);
    else if (!strcmp(act, "stop")) home_player_stop();
    else {
        snprintf(out, n, "no such action");
        return false;
    }
    /* Report what was asked for, not a reading taken a microsecond later: the player works on its
     * own thread, so a fresh read here is still the old state and looks like the command failed. */
    home_player_get(&st);
    snprintf(out, n, "%s%s%s", act, st.title[0] ? ": " : "", st.title[0] ? st.title : "");
    return true;
}

static const char *ha_kind_word(home_ha_kind_t k)
{
    switch (k) {
    case HA_LIGHT: return "light";
    case HA_SWITCH: return "switch";
    case HA_FAN: return "fan";
    case HA_SCENE: return "scene";
    case HA_SCRIPT: return "script";
    case HA_BUTTON: return "button";
    case HA_SENSOR: return "sensor";
    case HA_BINARY: return "sensor";
    case HA_MEDIA: return "media player";
    case HA_COVER: return "cover";
    case HA_CLIMATE: return "thermostat";
    case HA_LOCK: return "lock";
    default: return "entity";
    }
}

static bool do_smart_home(args_t *a, char *out, size_t n)
{
    char act[16], ent[80];
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which action?");
        return false;
    }
    home_ha_entity_t *tiles = malloc(sizeof *tiles * HOME_HA_PICKS);
    if (!tiles) return false;
    int nt = home_ha_tiles(tiles, HOME_HA_PICKS);

    bool ok;
    if (!strcmp(act, "list")) {
        if (nt <= 0) {
            home_ha_status_t st;
            home_ha_status(&st);
            snprintf(out, n, st.configured ? "no entities are picked in home mode's settings%s%s"
                                           : "Home Assistant isn't set up on this tablet",
                     st.err[0] ? " — " : "", st.err[0] ? st.err : "");
            free(tiles);
            return true;
        }
        out[0] = 0;
        for (int i = 0; i < nt; i++)
            addf(out, n, "%s%s (%s, %s) = %s%s", i ? "; " : "", tiles[i].id, tiles[i].name, ha_kind_word(tiles[i].kind),
                 tiles[i].state[0] ? tiles[i].state : "?", tiles[i].dimmable ? ", dimmable" : "");
        free(tiles);
        return true;
    }

    if (!args_str(a, "entity", ent, sizeof ent) || !ent[0]) {
        snprintf(out, n, "which entity? call list first");
        free(tiles);
        return false;
    }
    /* Only what home mode already shows. The picks ARE the boundary: an id the model invented, or
     * one belonging to something in the house nobody chose to expose here, is refused. */
    int found = -1;
    for (int i = 0; i < nt; i++) if (!strcmp(tiles[i].id, ent)) { found = i; break; }
    if (found < 0) {
        snprintf(out, n, "\"%.40s\" is not one of the entities set up here; call list", ent);
        free(tiles);
        return false;
    }
    double v;
    if (!strcmp(act, "toggle")) {
        if (!tiles[found].actionable) {
            snprintf(out, n, "%s is a %s: there is nothing to toggle", tiles[found].name,
                     ha_kind_word(tiles[found].kind));
            ok = false;
        } else {
            home_ha_tap(ent);
            snprintf(out, n, "toggled %s", tiles[found].name);
            ok = true;
        }
    } else if (!strcmp(act, "brightness")) {
        if (!args_num(a, "value", &v)) {
            snprintf(out, n, "brightness needs a value, 0 to 100");
            ok = false;
        } else if (!tiles[found].dimmable) {
            snprintf(out, n, "%s has no brightness; toggle it instead", tiles[found].name);
            ok = false;
        } else {
            int pct = clamp_pct(v);
            home_ha_brightness(ent, pct);
            if (pct) snprintf(out, n, "%s to %d%%", tiles[found].name, pct);
            else snprintf(out, n, "%s off", tiles[found].name);
            ok = true;
        }
    } else if (!strcmp(act, "temperature")) {
        if (!args_num(a, "value", &v)) {
            snprintf(out, n, "a set point is needed");
            ok = false;
        } else if (tiles[found].kind != HA_CLIMATE) {
            snprintf(out, n, "%s is not a thermostat", tiles[found].name);
            ok = false;
        } else {
            home_ha_set_temp(ent, (float)v);
            snprintf(out, n, "%s set to %.1f", tiles[found].name, v);
            ok = true;
        }
    } else {
        snprintf(out, n, "no such action");
        ok = false;
    }
    free(tiles);
    return ok;
}

/* ---- reminders: a few words at a time of day, kept in kv "reminders" ("id|unix|text" a line) ---- */

typedef struct {
    int id;
    long long due;
    char text[96];
} rem_t;
static rem_t REM[NREM];
static int g_nrem, g_rem_next = 1;
static bool g_rem_loaded;

static void rem_save(void)
{
    char *v = malloc(NREM * 120 + 1);
    if (!v) return;
    v[0] = 0;
    size_t k = 0;
    for (int i = 0; i < g_nrem; i++)
        k += (size_t)snprintf(v + k, NREM * 120 + 1 - k, "%d|%lld|%s\n", REM[i].id, REM[i].due, REM[i].text);
    hal_kv_set("reminders", v);
    free(v);
}

static void rem_load(void)
{
    if (g_rem_loaded) return;
    g_rem_loaded = true;
    char *v = malloc(NREM * 120 + 1);
    if (!v) return;
    if (hal_kv_get("reminders", v, NREM * 120 + 1)) {
        for (char *p = v; *p && g_nrem < NREM;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            int id, used = 0;
            long long due;
            if (sscanf(p, "%d|%lld|%n", &id, &due, &used) == 2 && used) {
                rem_t *r = &REM[g_nrem++];
                r->id = id;
                r->due = due;
                snprintf(r->text, sizeof r->text, "%s", p + used);
                if (id >= g_rem_next) g_rem_next = id + 1;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    free(v);
}

static void when_text(long long due, char *out, size_t n)
{
    time_t t = (time_t)due, now = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    long long in = due - (long long)now;
    char hm[8];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    if (in < 3600) snprintf(out, n, "%s (in %lld min)", hm, in / 60 + (in % 60 ? 1 : 0));
    else snprintf(out, n, "%s%s", in < 86400 ? "" : "tomorrow ", hm);
}

static bool do_reminder(args_t *a, char *out, size_t n)
{
    rem_load();
    char act[12], text[96] = "", at[8] = "";
    if (!args_str(a, "action", act, sizeof act)) act[0] = 0;
    struct tm tm;
    bool clock = now_tm(&tm);
    if (!strcmp(act, "list")) {
        if (!g_nrem) {
            snprintf(out, n, "no reminders are pending");
            return true;
        }
        out[0] = 0;
        for (int i = 0; i < g_nrem; i++) {
            char w[40];
            when_text(REM[i].due, w, sizeof w);
            addf(out, n, "%s#%d at %s: %s", i ? "\n" : "", REM[i].id, w, REM[i].text);
        }
        return true;
    }
    if (!strcmp(act, "cancel")) {
        double idd = 0;
        args_num(a, "id", &idd);
        for (int i = 0; i < g_nrem; i++) {
            if (REM[i].id != (int)idd) continue;
            snprintf(out, n, "cancelled reminder #%d: %s", REM[i].id, REM[i].text);
            memmove(&REM[i], &REM[i + 1], (size_t)(g_nrem - i - 1) * sizeof REM[0]);
            g_nrem--;
            rem_save();
            return true;
        }
        snprintf(out, n, "there is no reminder #%d (list shows them)", (int)idd);
        return false;
    }
    if (strcmp(act, "add") != 0) {
        snprintf(out, n, "which action: add, list or cancel?");
        return false;
    }
    if (!clock) {
        snprintf(out, n, "the tablet's clock isn't set, so it can't keep a reminder: use the timer instead");
        return false;
    }
    if (!args_str(a, "text", text, sizeof text) || !text[0]) {
        snprintf(out, n, "what should the reminder say?");
        return false;
    }
    if (g_nrem >= NREM) {
        snprintf(out, n, "%d reminders are already pending, the most kept: cancel one first", NREM);
        return false;
    }
    double mins = 0;
    long long now = (long long)time(NULL), due;
    if (args_num(a, "minutes", &mins) && mins > 0) {
        due = now + (long long)(mins * 60 + 0.5);
    } else if (args_str(a, "at", at, sizeof at) && parse_hhmm(at) >= 0) {
        int hm = parse_hhmm(at);
        tm.tm_hour = hm / 100;
        tm.tm_min = hm % 100;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        due = (long long)mktime(&tm);
        if (due <= now) due += 86400;
    } else {
        snprintf(out, n, "when? give minutes from now, or at \"HH:MM\"");
        return false;
    }
    for (char *p = text; *p; p++) if (*p == '\n' || *p == '|') *p = ' ';
    rem_t *r = &REM[g_nrem++];
    r->id = g_rem_next++;
    r->due = due;
    snprintf(r->text, sizeof r->text, "%s", text);
    rem_save();
    char w[40];
    when_text(due, w, sizeof w);
    snprintf(out, n, "reminder #%d set for %s: %s", r->id, w, r->text);
    return true;
}

/* The due ones: a chime, the screen woken, the words on the island (and kept in the notifications). */
static struct {
    double at[4];
    float hz[4];
    int n;
} CH;

static void rem_tick(double now)
{
    for (int i = 0; i < CH.n; i++) {
        if (now < CH.at[i]) continue;
        hal_tone(CH.hz[i], 170, S.volume > 0.4f ? S.volume : 0.4f);
        memmove(&CH.at[i], &CH.at[i + 1], (size_t)(CH.n - i - 1) * sizeof CH.at[0]);
        memmove(&CH.hz[i], &CH.hz[i + 1], (size_t)(CH.n - i - 1) * sizeof CH.hz[0]);
        CH.n--;
        break;
    }
    static double next;
    if (now < next) return;
    next = now + 1.0;
    rem_load();
    long long t = (long long)time(NULL);
    if (t < 1609459200) return; /* the clock isn't set */
    for (int i = 0; i < g_nrem; i++) {
        if (REM[i].due > t) continue;
        char msg[112];
        snprintf(msg, sizeof msg, "reminder: %s", REM[i].text);
        bz_ui_wake();
        ui_island_say(BZ_I_NOTIFICATIONS, msg);
        static const float NOTE[4] = { 784, 1047, 784, 1047 }; /* G5 C6, twice */
        CH.n = 4;
        for (int k = 0; k < 4; k++) {
            CH.at[k] = now + k * 0.22 + (k >= 2 ? 0.5 : 0);
            CH.hz[k] = NOTE[k];
        }
        memmove(&REM[i], &REM[i + 1], (size_t)(g_nrem - i - 1) * sizeof REM[0]);
        g_nrem--;
        rem_save();
        break; /* one a second: two due together are two messages, not one lost */
    }
}

/* ---- the tablet's own state ---- */

static bool do_status(char *out, size_t n)
{
    out[0] = 0;
    struct tm tm;
    char when[40] = "the clock isn't set";
    if (now_tm(&tm)) strftime(when, sizeof when, "%A %Y-%m-%d %H:%M", &tm);
    double up = hal_seconds();
    addf(out, n, "time: %s; up %dh %02dm", when, (int)(up / 3600), (int)(up / 60) % 60);
    hal_battery_t b;
    if (hal_battery(&b) && b.ok)
        addf(out, n, "\nbattery: %d%% (%.2f V)%s", b.percent, b.volts,
             b.charging ? ", charging" : b.external ? ", on external power" : ", on battery");
    hal_net_t net;
    hal_net(&net);
    if (net.up) addf(out, n, "\nwi-fi: %s, %d dBm, %s", net.ssid, net.rssi, net.ip);
    else addf(out, n, "\nwi-fi: not connected%s%s%s", net.want[0] ? " (looking for " : "", net.want,
              net.want[0] ? (net.absent ? ", which isn't in range)" : ")") : "");
    hal_tether_t th;
    hal_tether(&th);
    if (th.present) addf(out, n, "\nusb tether: %s %s", th.kind, th.up ? th.ip : "down");
    hal_sys_t sy;
    hal_sys(&sy);
    addf(out, n, "\nmemory free: %u KB internal, %u KB psram; chip %.0f C", (unsigned)(sy.sram_free / 1024),
         (unsigned)(sy.psram_free / 1024), sy.temp_c);
    uint64_t tot = 0, fr = 0;
    if (hal_sd_space(&tot, &fr)) addf(out, n, "\ncard: %.1f of %.1f GB free", fr / 1e9, tot / 1e9);
    else addf(out, n, "\ncard: none");
    addf(out, n, "\nscreen: brightness %d%%, %s tone%s, accent %s%s; volume %d%%", (int)(S.brightness * 100 + 0.5),
         S.dark ? "dark" : "light", S.calm ? " (calm)" : "", BZ_ACCENTS[S.accent].name,
         ui_home_mode_active() ? "; home mode on" : "", (int)(S.volume * 100 + 0.5));
    addf(out, n, "\n%s; %d notification%s", ui_app_any_open() ? "an app is open" : "on the pages",
         ui_notify_count(), ui_notify_count() == 1 ? "" : "s");
    char t[160];
    ui_timer_do("status", 0, t, sizeof t);
    addf(out, n, "\ntimer: %s", t);
    char *al = malloc(400);
    if (al && ui_alarm_do("list", 0, 0, 0, al, 400)) {
        for (char *p = al; *p; p++) if (*p == '\n') *p = ';';
        addf(out, n, "\nalarms: %s", al);
    }
    free(al);
    rem_load();
    if (g_nrem) {
        char w[40];
        when_text(REM[0].due, w, sizeof w);
        addf(out, n, "\nreminders: %d pending, the next at %s", g_nrem, w);
    }
    return true;
}

static bool do_notifications(args_t *a, char *out, size_t n)
{
    char act[12] = "read";
    args_str(a, "action", act, sizeof act);
    if (!strcmp(act, "clear")) {
        int c = ui_notify_count();
        ui_notify_clear();
        snprintf(out, n, "cleared %d notification%s", c, c == 1 ? "" : "s");
        return true;
    }
    double lim = 10;
    args_num(a, "limit", &lim);
    int c = ui_notify_count(), k = (int)lim < c ? (int)lim : c;
    if (!c) {
        snprintf(out, n, "no notifications");
        return true;
    }
    snprintf(out, n, "%d notification%s, newest first:", c, c == 1 ? "" : "s");
    for (int i = 0; i < k; i++) {
        const ui_note_t *no = ui_notify_get(i);
        if (!no) break;
        char age[24];
        ui_notify_age(no, age, sizeof age);
        addf(out, n, "\n%s: %s", age, no->text);
    }
    return true;
}

static bool do_timer(args_t *a, char *out, size_t n)
{
    char act[16];
    double secs = 0;
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which timer action?");
        return false;
    }
    args_num(a, "seconds", &secs);
    return ui_timer_do(act, secs, out, n);
}

static bool do_alarm(args_t *a, char *out, size_t n)
{
    char act[12], t[8] = "", d[32] = "";
    double idx = 0;
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which alarm action?");
        return false;
    }
    args_str(a, "time", t, sizeof t);
    args_str(a, "days", d, sizeof d);
    args_num(a, "index", &idx);
    int hhmm = parse_hhmm(t), days = parse_days(d);
    if (!strcmp(act, "add") && hhmm < 0) {
        snprintf(out, n, "give the time as \"HH:MM\", 24-hour");
        return false;
    }
    if (days < 0) {
        snprintf(out, n, "days is once, daily, weekdays, weekends, or days like \"mo,we,fr\"");
        return false;
    }
    return ui_alarm_do(act, hhmm, days, (int)idx, out, n);
}

static bool do_notes(args_t *a, char *out, size_t n)
{
    char act[12];
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which notes action?");
        return false;
    }
    /* the text can be as long as the notebook: its own buffer */
    char *text = malloc(3200);
    if (!text) return false;
    if (!args_str(a, "text", text, 3200)) text[0] = 0;
    bool ok = ui_notes_do(act, text, out, n);
    free(text);
    return ok;
}

static bool do_checklist(args_t *a, char *out, size_t n)
{
    char act[12], item[64] = "";
    if (!args_str(a, "action", act, sizeof act)) {
        snprintf(out, n, "which checklist action?");
        return false;
    }
    args_str(a, "item", item, sizeof item);
    return ui_check_do(act, item, out, n);
}

static bool do_calendar_add(args_t *a, char *out, size_t n)
{
    char date[16] = "", t[8] = "", text[80] = "";
    args_str(a, "date", date, sizeof date);
    args_str(a, "time", t, sizeof t);
    args_str(a, "text", text, sizeof text);
    int ymd = parse_date(date), hhmm = parse_hhmm(t);
    if (!ymd) {
        snprintf(out, n, "give the date as YYYY-MM-DD, today or tomorrow%s", date[0] ? "" : " (and the clock must be set)");
        return false;
    }
    if (hhmm < 0) {
        snprintf(out, n, "give the time as \"HH:MM\", 24-hour");
        return false;
    }
    if (!text[0]) {
        snprintf(out, n, "what is the event?");
        return false;
    }
    return ui_calendar_add(ymd, hhmm, text, out, n);
}

/* The one place UI work happens, and it happens on the UI thread. */
static void apply(job_t *j)
{
    args_t *a = malloc(sizeof *a); /* 64 tokens: off the UI task's stack */
    if (!a) {
        snprintf(j->out, j->outn, "out of memory");
        j->ok = false;
        return;
    }
    args_parse(a, j->args);
    char *o = j->out;
    size_t n = j->outn;
    if (!strcmp(j->action, "open_app")) j->ok = do_open_app(a, j->pool, o, n);
    else if (!strcmp(j->action, "set_tablet")) j->ok = do_set_tablet(a, j->pool, o, n);
    else if (!strcmp(j->action, "music")) j->ok = do_music(a, o, n);
    else if (!strcmp(j->action, "smart_home")) j->ok = do_smart_home(a, o, n);
    else if (!strcmp(j->action, "tablet_status")) j->ok = do_status(o, n);
    else if (!strcmp(j->action, "timer")) j->ok = do_timer(a, o, n);
    else if (!strcmp(j->action, "alarm")) j->ok = do_alarm(a, o, n);
    else if (!strcmp(j->action, "reminder")) j->ok = do_reminder(a, o, n);
    else if (!strcmp(j->action, "notes")) j->ok = do_notes(a, o, n);
    else if (!strcmp(j->action, "checklist")) j->ok = do_checklist(a, o, n);
    else if (!strcmp(j->action, "calendar")) j->ok = do_calendar_add(a, o, n);
    else if (!strcmp(j->action, "notifications")) j->ok = do_notifications(a, o, n);
    else {
        snprintf(o, n, "this tablet has no action called \"%.20s\"", j->action);
        j->ok = false;
    }
    free(a);
}

static void tick(void *user)
{
    (void)user;
    double now = hal_seconds();
    assist_mem_tick();  /* memory saves that are waiting (a failed write, kv without a card) */
    rem_tick(now);
    defer_tick(now);
    pthread_mutex_lock(&g_lock);
    job_t *j = g_job;
    if (!j || j->taken) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    j->taken = true;                 /* the lock dropped before any LVGL work: apply() builds app */
    pthread_mutex_unlock(&g_lock);   /* windows, and the worker waits on g_lock */

    j->out[0] = 0;
    j->ok = false;
    apply(j);

    pthread_mutex_lock(&g_lock);
    g_job = NULL;
    if (j->abandoned) {
        /* the worker stopped waiting: this request is the UI's to free */
        free(j->args);
        free(j->out);
        free(j);
    } else {
        j->done = true;
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- the worker's side */

static bool doc_name_ok(const char *s)
{
    return s && s[0] && !strchr(s, '/') && !strchr(s, '\\') && !strstr(s, "..") && strlen(s) < 60;
}

/* documents: all card work, none of it UI, so it runs here on the worker */
static bool do_documents(args_t *a, char *out, size_t n)
{
    char act[12] = "", name[64] = "";
    args_str(a, "action", act, sizeof act);
    args_str(a, "name", name, sizeof name);
    char dir[96];
    if (!cstore_path(CS_DOCS, NULL, dir, sizeof dir)) {
        snprintf(out, n, "no microSD card: documents live on the card");
        return false;
    }
    if (!strcmp(act, "list")) {
        cstore_entry_t *e = malloc(sizeof *e * 48);
        if (!e) return false;
        int k = cstore_list(dir, ".TXT.MD", true, e, 48);
        if (k <= 0) {
            snprintf(out, n, k < 0 ? "the documents folder can't be read" : "no documents yet (CATOS/DOCS is empty)");
            free(e);
            return k == 0;
        }
        snprintf(out, n, "%d document%s, newest first:", k, k == 1 ? "" : "s");
        for (int i = 0; i < k; i++) {
            char sz[16], d[16] = "";
            struct tm tm;
            if (e[i].mtime > 0 && localtime_r(&e[i].mtime, &tm)) strftime(d, sizeof d, "%Y-%m-%d", &tm);
            addf(out, n, "\n%s (%s%s%s)", e[i].name, cstore_size(e[i].size, sz, sizeof sz), d[0] ? ", " : "", d);
        }
        free(e);
        return true;
    }
    if (!doc_name_ok(name)) {
        snprintf(out, n, "give a document's name (no folders)");
        return false;
    }
    if (!strcmp(act, "write")) {
        int t = a->ok ? jl_get(&a->d, 0, "markdown") : -1;
        size_t cap = t >= 0 ? (size_t)(a->d.t[t].end - a->d.t[t].start) + 16 : 0;
        char *md = cap ? malloc(cap) : NULL;
        if (!md || !jl_str(&a->d, t, md, cap) || !md[0]) {
            free(md);
            snprintf(out, n, "nothing to write: give the markdown");
            return false;
        }
        char why[128] = "";
        bool ok = ui_docs_write(name, md, why, sizeof why);
        free(md);
        if (ok) snprintf(out, n, "saved %s in CATOS/DOCS (the documents app shows it)", name);
        else snprintf(out, n, "not saved: %s", why[0] ? why : "the card refused it");
        return ok;
    }
    if (strcmp(act, "read") != 0) {
        snprintf(out, n, "which action: list, read or write?");
        return false;
    }
    /* the name as given, else with .MD or .TXT, else any file of that name in another case */
    static const char *const EXT[3] = { "", ".MD", ".TXT" };
    char path[200];
    char *txt = NULL;
    size_t len = 0;
    bool trunc = false;
    for (int i = 0; i < 3 && !txt; i++) {
        snprintf(path, sizeof path, "%s/%s%s", dir, name, EXT[i]);
        txt = cstore_read(path, n > 200 ? n - 200 : n, &len, &trunc);
    }
    if (!txt) {
        cstore_entry_t *e = malloc(sizeof *e * 48);
        int k = e ? cstore_list(dir, ".TXT.MD", false, e, 48) : 0;
        for (int i = 0; i < k && !txt; i++) {
            char stem[64];
            snprintf(stem, sizeof stem, "%s", e[i].name);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            if (strcasecmp(e[i].name, name) && strcasecmp(stem, name)) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e[i].name);
            txt = cstore_read(path, n > 200 ? n - 200 : n, &len, &trunc);
        }
        free(e);
    }
    if (!txt) {
        snprintf(out, n, "no document called %s (list shows them)", name);
        return false;
    }
    snprintf(out, n, "%s%s", txt, trunc ? "\n…(the rest is cut: it is longer than a tool result can be)" : "");
    free(txt);
    return true;
}

/* the calendar's events from the card, read here on the worker */
static bool do_calendar_list(args_t *a, char *out, size_t n)
{
    char date[16] = "";
    double days = 1;
    args_str(a, "date", date, sizeof date);
    args_num(a, "days", &days);
    if (days < 1) days = 1;
    if (days > 31) days = 31;
    int from = parse_date(date);
    if (!from) {
        snprintf(out, n, "give the date as YYYY-MM-DD, today or tomorrow%s", date[0] ? "" : " (and the clock must be set)");
        return false;
    }
    struct tm t0 = { .tm_year = from / 10000 - 1900, .tm_mon = from / 100 % 100 - 1, .tm_mday = from % 100 + (int)days,
                     .tm_hour = 12, .tm_isdst = -1 };
    time_t tt = mktime(&t0);
    struct tm t1;
    localtime_r(&tt, &t1);
    int to = (t1.tm_year + 1900) * 10000 + (t1.tm_mon + 1) * 100 + t1.tm_mday; /* exclusive */
    char path[96];
    if (!cstore_path(CS_DATA, "EVENTS.TXT", path, sizeof path)) {
        snprintf(out, n, "no microSD card: the calendar lives on the card");
        return false;
    }
    size_t len = 0;
    char *txt = cstore_read(path, 64 * 1024, &len, NULL);
    struct stat st;
    if (!txt && stat(path, &st) == 0) {
        snprintf(out, n, "the card didn't read the calendar: try again in a moment");
        return false;
    }
    snprintf(out, n, "events from %04d-%02d-%02d for %d day%s:", from / 10000, from / 100 % 100, from % 100, (int)days,
             (int)days == 1 ? "" : "s");
    int found = 0;
    for (char *line = txt; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int y, mo, d, hh, mi, used = 0;
        if (sscanf(line, "%4d-%2d-%2d %2d:%2d %n", &y, &mo, &d, &hh, &mi, &used) == 5 && used) {
            int ymd = y * 10000 + mo * 100 + d;
            if (ymd >= from && ymd < to) {
                addf(out, n, "\n%04d-%02d-%02d %02d:%02d %s", y, mo, d, hh, mi, line + used);
                found++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!found) addf(out, n, " none");
    free(txt);
    return true;
}

/* the weather, from home's worker (asked for, and waited on for a few seconds, when it has nothing yet) */
static bool do_weather(char *out, size_t n)
{
    home_weather_t w;
    home_weather_get(&w);
    if (!w.configured) {
        snprintf(out, n, "no place is set for the weather: settings, home");
        return false;
    }
    for (int i = 0; !w.ok && i < 40; i++) {
        home_want(HOME_WANT_WEATHER);
        usleep(250000);
        home_weather_get(&w);
    }
    if (!w.ok) {
        snprintf(out, n, "the weather didn't come%s%s", w.err[0] ? ": " : "", w.err);
        return false;
    }
    home_forecast_t *f = malloc(sizeof *f);
    const char *u = w.fahrenheit ? "F" : "C";
    snprintf(out, n, "%s: now %.0f %s, %s; today %.0f to %.0f %s", w.place, w.temp, u, home_weather_text(w.code), w.lo, w.hi, u);
    if (f) {
        home_weather_forecast(f);
        if (f->ok) {
            addf(out, n, "; feels like %.0f, humidity %d%%, wind %.0f %s", f->feels, f->humidity, f->wind,
                 w.fahrenheit ? "mph" : "km/h");
            if (f->sunrise[0]) addf(out, n, "; sunrise %s, sunset %s", f->sunrise, f->sunset);
            addf(out, n, "\nnext hours:");
            for (int i = 0; i < f->nhours && i < 12; i += 2)
                addf(out, n, " %02d:00 %.0f %s,", f->hour[i].hour, f->hour[i].temp, home_weather_text(f->hour[i].code));
            for (int i = 0; i < f->nhours && i < 12; i += 2)
                if (f->hour[i].pop >= 30) {
                    addf(out, n, "\nrain chance from %02d:00: %d%%", f->hour[i].hour, f->hour[i].pop);
                    break;
                }
            static const char *const WD[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
            addf(out, n, "\nthe week:");
            for (int i = 0; i < f->ndays; i++)
                addf(out, n, "\n%s %d: %.0f to %.0f, %s, rain %d%%", WD[f->day[i].wday % 7], f->day[i].mday, f->day[i].lo,
                     f->day[i].hi, home_weather_text(f->day[i].code), f->day[i].pop);
        }
        free(f);
    }
    return true;
}

/* Runs here on the worker, never parked for the UI: card reads and writes, and the weather's wait. */
static bool worker_side(const char *action, const char *args_json, char *out, size_t n, bool *ok)
{
    bool docs = !strcmp(action, "documents"), weather = !strcmp(action, "weather");
    bool cal_list = false;
    if (!docs && !weather && strcmp(action, "calendar") != 0) return false;
    args_t *a = malloc(sizeof *a);
    if (!a) {
        snprintf(out, n, "out of memory");
        *ok = false;
        return true;
    }
    args_parse(a, args_json);
    char act[12] = "";
    args_str(a, "action", act, sizeof act);
    cal_list = !strcmp(action, "calendar") && strcmp(act, "add") != 0;
    if (docs) *ok = do_documents(a, out, n);
    else if (weather) *ok = do_weather(out, n);
    else if (cal_list) *ok = do_calendar_list(a, out, n);
    free(a);
    return docs || weather || cal_list;
}

static bool handle(const char *action, const char *args_json, int pool, char *out, size_t n)
{
    if (!action || !out || n == 0) return false;
    out[0] = 0;
    bool ok = false;
    if (worker_side(action, args_json ? args_json : "{}", out, n, &ok)) return ok;

    job_t *j = calloc(1, sizeof *j);
    if (!j) return false;
    snprintf(j->action, sizeof j->action, "%s", action);
    j->args = strdup(args_json ? args_json : "{}");
    j->outn = n < 7000 ? n : 7000;
    j->out = malloc(j->outn);
    j->pool = pool;
    if (!j->args || !j->out) {
        free(j->args);
        free(j->out);
        free(j);
        return false;
    }
    j->out[0] = 0;
    pthread_mutex_lock(&g_lock);
    if (g_job) {
        pthread_mutex_unlock(&g_lock);
        free(j->args);
        free(j->out);
        free(j);
        snprintf(out, n, "the tablet is still doing the last thing; ask again in a moment");
        return false;
    }
    g_job = j;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(WAIT_MS % 1000) * 1000000L;
    ts.tv_sec += WAIT_MS / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    int rc = 0;
    while (!j->done && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&g_cond, &g_lock, &ts);

    if (j->done) {
        snprintf(out, n, "%s", j->out);
        ok = j->ok;
        free(j->args);
        free(j->out);
        free(j);
    } else if (!j->taken) {
        /* never started: withdrawn, so it can't happen after "it didn't" was said */
        g_job = NULL;
        free(j->args);
        free(j->out);
        free(j);
        snprintf(out, n, "the screen didn't answer in time: nothing was done");
    } else {
        /* under way on the UI thread: it finishes there and frees itself. Say so rather than claim
         * success; a companion that reports "done" for something that never happened is worse. */
        j->abandoned = true;
        snprintf(out, n, "the screen didn't answer in time; it may still happen");
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void ui_actions_init(void)
{
    ui_on_refresh(tick, NULL);
    assist_set_action_handler(handle);
}
