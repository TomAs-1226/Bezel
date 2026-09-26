/* ui_actions — what the assistant is allowed to DO to the tablet, the PC and the house.
 *
 * The assistant's AS_T_DEVICE tools (open_app, set_tablet, music, smart_home) all arrive here,
 * because assist cannot call the ui: `ui` REQUIRES `assist` and `home`, not the other way round, so
 * the assistant takes a handler it is handed, exactly as it takes the desk text and the TBA reader.
 *
 * The other half of the job is the thread. A tool runs on the assistant's worker; LVGL belongs to
 * the UI thread and corrupts itself if touched from anywhere else. So the worker parks the request
 * under a lock and waits, the 10 Hz refresh picks it up, does the work and writes the answer back.
 * One request is in flight at a time, which is all one conversation can produce.
 *
 * Nothing here touches the robot. Every AS_T_CHANGE tool — a tunable, an auto choice, a patch —
 * still goes through its confirmation card as before; these do not, because a lamp that demanded a
 * tap on a screen before it would turn on is not worth having.
 */
#include "ui_internal.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "assist.h"
#include "hal.h"
#include "home.h"
#include "json_lite.h"

#define WAIT_MS 2000       /* the UI runs at 10 Hz, so this is ~20 frames; a timeout is reported */
#define ARGS_MAX 512
#define OUT_MAX 512

typedef struct {
    char action[24];
    char args[ARGS_MAX];
    char out[OUT_MAX];
    bool ok;
} job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static bool g_pending, g_done;
static job_t g_job;

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

/* ---------------------------------------------------------------- the actions (UI thread) */

static int clamp_pct(double v) { return v < 0 ? 0 : v > 100 ? 100 : (int)(v + 0.5); }

static bool do_open_app(args_t *a, char *out, size_t n)
{
    char name[40];
    if (!args_str(a, "app", name, sizeof name) || !name[0]) {
        snprintf(out, n, "which app?");
        return false;
    }
    const ui_app_t *app = ui_app_find(name);
    if (!app) {
        /* Name what was asked for: the model can then try the label it actually saw on the icon. */
        snprintf(out, n, "there is no app called \"%.20s\" on this tablet", name);
        return false;
    }
    ui_app_open(app, NULL);
    snprintf(out, n, "opened %s", app->name ? app->name : name);
    return true;
}

static bool do_set_tablet(args_t *a, char *out, size_t n)
{
    double v;
    char *p = out;
    size_t left = n;
    bool any = false;
    if (args_num(a, "brightness", &v)) {
        int pct = clamp_pct(v);
        hal_set_brightness(pct / 100.0f);
        int k = snprintf(p, left, "brightness %d%%", pct);
        if (k > 0 && (size_t)k < left) { p += k; left -= (size_t)k; }
        any = true;
    }
    if (args_num(a, "volume", &v)) {
        int pct = clamp_pct(v);
        hal_set_volume(pct / 100.0f);
        snprintf(p, left, "%svolume %d%%", any ? ", " : "", pct);
        any = true;
    }
    if (!any) snprintf(out, n, "nothing to set: give brightness, volume, or both");
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
        char *p = out;
        size_t left = n;
        for (int i = 0; i < nt && left > 1; i++) {
            int k = snprintf(p, left, "%s%s (%s, %s) = %s%s", i ? "; " : "",
                             tiles[i].id, tiles[i].name, ha_kind_word(tiles[i].kind),
                             tiles[i].state[0] ? tiles[i].state : "?",
                             tiles[i].dimmable ? ", dimmable" : "");
            if (k <= 0 || (size_t)k >= left) break;
            p += k;
            left -= (size_t)k;
        }
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

/* The one place the work happens, and it happens on the UI thread. */
static void apply(job_t *j)
{
    args_t a;
    args_parse(&a, j->args);
    if (!strcmp(j->action, "open_app")) j->ok = do_open_app(&a, j->out, sizeof j->out);
    else if (!strcmp(j->action, "set_tablet")) j->ok = do_set_tablet(&a, j->out, sizeof j->out);
    else if (!strcmp(j->action, "music")) j->ok = do_music(&a, j->out, sizeof j->out);
    else if (!strcmp(j->action, "smart_home")) j->ok = do_smart_home(&a, j->out, sizeof j->out);
    else {
        snprintf(j->out, sizeof j->out, "this tablet has no action called \"%.20s\"", j->action);
        j->ok = false;
    }
}

static void tick(void *user)
{
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (!g_pending) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    job_t j = g_job;                 /* copied out, and the lock dropped before any LVGL work: */
    pthread_mutex_unlock(&g_lock);   /* apply() builds app windows, and the worker waits on g_lock */

    j.out[0] = 0;
    j.ok = false;
    apply(&j);

    pthread_mutex_lock(&g_lock);
    g_job = j;
    g_pending = false;
    g_done = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- the handler (worker thread) */

static bool handle(const char *action, const char *args_json, char *out, size_t n)
{
    if (!action || !out || n == 0) return false;
    pthread_mutex_lock(&g_lock);
    if (g_pending) {
        pthread_mutex_unlock(&g_lock);
        snprintf(out, n, "the tablet is still doing the last thing; ask again in a moment");
        return false;
    }
    memset(&g_job, 0, sizeof g_job);
    snprintf(g_job.action, sizeof g_job.action, "%s", action);
    snprintf(g_job.args, sizeof g_job.args, "%s", args_json ? args_json : "{}");
    g_pending = true;
    g_done = false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(WAIT_MS % 1000) * 1000000L;
    ts.tv_sec += WAIT_MS / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    int rc = 0;
    while (!g_done && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&g_cond, &g_lock, &ts);

    bool done = g_done, ok = g_job.ok;
    if (done) {
        snprintf(out, n, "%s", g_job.out);
    } else {
        /* Say so rather than claim success. The screen may still catch up a moment later, and a
         * companion that reports "done" for something that never happened is worse than a slow one. */
        g_pending = false;
        snprintf(out, n, "the screen didn't answer in time; it may still happen");
    }
    pthread_mutex_unlock(&g_lock);
    return done && ok;
}

void ui_actions_init(void)
{
    ui_on_refresh(tick, NULL);
    assist_set_action_handler(handle);
}
