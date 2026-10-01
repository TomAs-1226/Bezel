/* Statbotics (api.statbotics.io/v3, no key) on the home worker: the team's EPA and rank this season, its EPA at
 * the event, and the win probability of each of our matches there (intel.h).
 *
 * Three requests, one per worker pass: the season every hour, the event and its matches every 5 minutes on a
 * day the event runs (every hour otherwise). Statbotics recomputes after each match, so faster is wasted.
 * A failure keeps what was there and tries again in a few minutes. */
#include "intel.h"
#include "intel_net.h"
#include "intel_priv.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal.h"

#define BASE "https://api.statbotics.io/v3"
#define BODY_MAX (256 * 1024)
#define YEAR_S 3600.0
#define EVENT_LIVE_S 300.0
#define EVENT_IDLE_S 3600.0
#define RETRY_S 300.0        /* an answer that wasn't one (a 500) */
#define RETRY_NET_S 60.0     /* no answer at all (no network yet, DNS) */
#define FIRST_S 20.0         /* after start-up: Wi-Fi and the clock */

enum { R_YEAR, R_EVENT, R_MATCHES, R_N };

typedef struct {
    int team;
    char event[20];
    bool live;
    unsigned cfg;
    double next[R_N];
    sb_state_t st, work;
} sb_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sb_t *B;

static sb_t *sb(void)
{
    if (!B) {
        B = calloc(1, sizeof *B);
        if (B) {
            B->st.epa = B->st.epa_sd = B->st.percentile = B->st.event_epa = NAN;
            for (int i = 0; i < R_N; i++) B->next[i] = hal_seconds() + FIRST_S;
        }
    }
    return B;
}

void sb_config(int team)
{
    pthread_mutex_lock(&g_lock);
    if (sb() && B->team != team) {
        B->team = team;
        B->cfg++;
        unsigned g = B->st.gen;
        memset(&B->st, 0, sizeof B->st);
        B->st.epa = B->st.epa_sd = B->st.percentile = B->st.event_epa = NAN;
        B->st.team = team;
        B->st.gen = g + 1;
        for (int i = 0; i < R_N; i++) B->next[i] = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void sb_follow(const char *event_key, bool live)
{
    pthread_mutex_lock(&g_lock);
    if (sb()) {
        const char *ev = event_key ? event_key : "";
        if (strcmp(B->event, ev)) {
            snprintf(B->event, sizeof B->event, "%s", ev);
            B->cfg++;
            B->st.have_event = false;
            B->st.nmatches = 0;
            B->st.event_epa = NAN;
            B->st.gen++;
            B->next[R_EVENT] = B->next[R_MATCHES] = 0;
        }
        if (live && !B->live) B->next[R_EVENT] = B->next[R_MATCHES] = 0; /* the event began: fresh numbers */
        B->live = live;
    }
    pthread_mutex_unlock(&g_lock);
}

void sb_refresh(void)
{
    pthread_mutex_lock(&g_lock);
    if (sb())
        for (int i = 0; i < R_N; i++) B->next[i] = 0; /* all three, one a pass */
    pthread_mutex_unlock(&g_lock);
}

unsigned sb_gen(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned g = B ? B->st.gen : 0;
    pthread_mutex_unlock(&g_lock);
    return g;
}

void sb_get(sb_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (B) *out = B->st;
    else {
        memset(out, 0, sizeof *out);
        out->epa = out->epa_sd = out->percentile = out->event_epa = NAN;
    }
    pthread_mutex_unlock(&g_lock);
}

bool sb_find(const char *label, sb_match_t *out)
{
    bool found = false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; B && label && i < B->st.nmatches && !found; i++)
        if (!strcmp(B->st.matches[i].label, label)) {
            if (out) *out = B->st.matches[i];
            found = true;
        }
    pthread_mutex_unlock(&g_lock);
    return found;
}

/* which request is due, -1 none (under the lock) */
static int due_locked(double now)
{
    if (!B || B->team <= 0) return -1;
    time_t w = time(NULL);
    struct tm tm;
    localtime_r(&w, &tm);
    if (tm.tm_year < 120) return -1; /* which season? */
    if (!ix_online()) return -1;
    for (int i = 0; i < R_N; i++) {
        if (i != R_YEAR && !B->event[0]) continue;
        if (now >= B->next[i]) return i;
    }
    return -1;
}

bool sb_due(double now)
{
    pthread_mutex_lock(&g_lock);
    bool d = due_locked(now) >= 0;
    pthread_mutex_unlock(&g_lock);
    return d;
}

bool sb_work(double now)
{
    pthread_mutex_lock(&g_lock);
    int r = due_locked(now);
    if (r < 0) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    unsigned cfg = B->cfg;
    int team = B->team;
    char event[20];
    snprintf(event, sizeof event, "%s", B->event);
    double every = r == R_YEAR ? YEAR_S : B->live ? EVENT_LIVE_S : EVENT_IDLE_S;
    B->next[r] = now + every;
    B->work = B->st;
    pthread_mutex_unlock(&g_lock);

    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    char url[160];
    if (r == R_YEAR) snprintf(url, sizeof url, BASE "/team_year/%d/%d", team, tm.tm_year + 1900);
    else if (r == R_EVENT) snprintf(url, sizeof url, BASE "/team_event/%d/%s", team, event);
    else snprintf(url, sizeof url, BASE "/matches?team=%d&event=%s&limit=100", team, event);
    char *body = NULL;
    size_t len = 0;
    double t0 = hal_seconds();
    int status = ix_get_url(url, "Accept: application/json\r\n", NULL, NULL, &body, &len, BODY_MAX, 10000);
    sb_state_t *w = &B->work;
    bool ok = status == 200 && body &&
              (r == R_YEAR ? sb_parse_team_year(body, len, w)
                           : r == R_EVENT ? sb_parse_team_event(body, len, w) : sb_parse_matches(body, len, team, w));
    w->team = team;
    w->year = tm.tm_year + 1900;
    if (ok) {
        w->phase = SB_READY;
        w->stale = false;
        w->err[0] = 0;
        w->fetched = wall;
        if (r != R_YEAR && !w->event_key[0]) snprintf(w->event_key, sizeof w->event_key, "%s", event);
    } else {
        w->stale = w->phase == SB_READY;
        if (w->phase != SB_READY) w->phase = SB_ERROR;
        if (status < 0) snprintf(w->err, sizeof w->err, "statbotics isn't answering");
        else if (status == 404) snprintf(w->err, sizeof w->err, "statbotics has no %s for %d yet",
                                         r == R_YEAR ? "season" : "event", team);
        else snprintf(w->err, sizeof w->err, "statbotics answered %d", status);
    }
    printf("statbotics: %s -> %d, %u bytes in %.1f s\n", r == R_YEAR ? "team_year" : r == R_EVENT ? "team_event" : "matches",
           status, (unsigned)len, hal_seconds() - t0);
    free(body);
    pthread_mutex_lock(&g_lock);
    if (!ok && status != 404) B->next[r] = now + (status < 0 ? RETRY_NET_S : RETRY_S);
    if (cfg == B->cfg) {
        w->gen = B->st.gen + 1;
        B->st = *w;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}
