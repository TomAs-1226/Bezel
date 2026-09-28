/* Nexus for FRC, on the home worker: the event's live queue (intel.h).
 *
 * One endpoint, GET {base}/event/{eventKey} with the header Nexus-Api-Key, asked as often as the UI says
 * (nx_follow: ui_match.c asks every 30 s while a match of ours is within the hour on an event day). The
 * answer is parsed on the worker into a fresh nx_state_t and published whole; a failed request keeps what
 * was there and marks it stale. The key comes from the UI thread (kv nexus_key, from KEYS.ENV) and is never
 * logged; without one nothing is asked and the state says what's missing. */
#include "intel.h"
#include "intel_net.h"
#include "intel_priv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal.h"

#define BASE "https://frc.nexus/api/v1"
#define BODY_MAX (384 * 1024)
#define RETRY_S 30.0

typedef struct {
    char key[160];
    char base[96];
    int team;
    char event[20];
    double every;
    bool kick;
    double next;
    unsigned cfg;          /* bumps with the key, team, base or event: a request under way for the old ones
                              doesn't publish */
    char *inject;          /* the dev console's document, taken on the next turn */
    bool injected;         /* what's published came from the dev console: kept until a real answer or a new key */
    nx_state_t st;         /* published */
    nx_state_t work;       /* the worker's */
} nx_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nx_t *X;

static nx_t *nx(void)
{
    if (!X) X = calloc(1, sizeof *X); /* ~13 KB: PSRAM */
    return X;
}

/* the phase with nothing to ask (UI thread or worker, under the lock) */
static void idle_phase_locked(void)
{
    nx_state_t *s = &X->st;
    if (X->injected) return;
    nx_phase_t ph = !X->key[0] && !X->base[0] ? NX_OFF : !X->event[0] ? NX_NO_EVENT : s->phase;
    if (ph == NX_OFF || ph == NX_NO_EVENT) {
        if (s->phase != ph) {
            unsigned g = s->gen;
            memset(s, 0, sizeof *s);
            s->phase = ph;
            s->now_index = -1;
            s->gen = g + 1;
        }
    } else if (s->phase == NX_OFF || s->phase == NX_NO_EVENT) {
        s->phase = NX_LOADING;
        snprintf(s->event_key, sizeof s->event_key, "%s", X->event);
        s->gen++;
    }
}

void nx_config(const char *key, int team, const char *base)
{
    pthread_mutex_lock(&g_lock);
    if (nx()) {
        bool changed = strcmp(X->key, key ? key : "") || X->team != team || strcmp(X->base, base ? base : "");
        if (changed) {
            memset(X->key, 0, sizeof X->key);
            snprintf(X->key, sizeof X->key, "%s", key ? key : "");
            snprintf(X->base, sizeof X->base, "%s", base ? base : "");
            X->team = team;
            X->cfg++;
            X->kick = true;
            X->injected = false;
            X->st.phase = NX_LOADING;
            idle_phase_locked();
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void nx_follow(const char *event_key, double every_s)
{
    pthread_mutex_lock(&g_lock);
    if (nx()) {
        const char *ev = event_key ? event_key : "";
        /* the dev console's kv nexus_event names another event (a replay of a past one) */
        if (strcmp(X->event, ev) && !X->injected) {
            snprintf(X->event, sizeof X->event, "%s", ev);
            X->cfg++;
            X->kick = true;
            unsigned g = X->st.gen;
            memset(&X->st, 0, sizeof X->st);
            X->st.phase = NX_LOADING;
            X->st.now_index = -1;
            snprintf(X->st.event_key, sizeof X->st.event_key, "%s", ev);
            X->st.gen = g + 1;
        }
        /* sooner, when the new cadence is quicker than what's pending */
        if (every_s > 0 && (X->every <= 0 || every_s < X->every) && X->next > hal_seconds() + every_s)
            X->next = hal_seconds() + every_s;
        X->every = every_s;
        idle_phase_locked();
    }
    pthread_mutex_unlock(&g_lock);
}

void nx_refresh(void)
{
    pthread_mutex_lock(&g_lock);
    if (nx()) X->kick = true;
    pthread_mutex_unlock(&g_lock);
}

unsigned nx_gen(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned g = X ? X->st.gen : 0;
    pthread_mutex_unlock(&g_lock);
    return g;
}

void nx_get(nx_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (X) *out = X->st;
    else {
        memset(out, 0, sizeof *out);
        out->now_index = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

bool nx_find(const char *label, nx_match_t *out)
{
    bool found = false;
    pthread_mutex_lock(&g_lock);
    if (X && X->st.phase == NX_READY && label && label[0])
        for (int i = 0; i < X->st.nours && !found; i++)
            if (!strcmp(X->st.ours[i].label, label)) {
                if (out) *out = X->st.ours[i];
                found = true;
            }
    pthread_mutex_unlock(&g_lock);
    return found;
}

double nx_age_s(void)
{
    pthread_mutex_lock(&g_lock);
    time_t f = X && X->st.phase == NX_READY ? X->st.fetched : 0;
    pthread_mutex_unlock(&g_lock);
    return f ? difftime(time(NULL), f) : 1e9;
}

void nx_inject(const char *json)
{
    char *c = json ? strdup(json) : NULL;
    pthread_mutex_lock(&g_lock);
    if (nx()) {
        free(X->inject);
        X->inject = c;
        c = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    free(c);
}

bool nx_due(double now)
{
    pthread_mutex_lock(&g_lock);
    bool due = X && (X->inject || ((X->key[0] || X->base[0]) && X->event[0] &&
                                   (X->kick || (X->every > 0 && now >= X->next))));
    pthread_mutex_unlock(&g_lock);
    return due && (X->inject || ix_online());
}

/* publishes the worker's result, unless the configuration changed while it was asked */
static void publish(unsigned cfg)
{
    pthread_mutex_lock(&g_lock);
    if (cfg == X->cfg) {
        X->work.gen = X->st.gen + 1;
        X->st = X->work;
    }
    pthread_mutex_unlock(&g_lock);
}

bool nx_work(double now)
{
    pthread_mutex_lock(&g_lock);
    if (!X) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    char *inject = X->inject;
    X->inject = NULL;
    unsigned cfg = X->cfg;
    int team = X->team;
    if (inject) {
        /* the dev console's: parsed and published as if it came over the network */
        X->work = X->st;
        pthread_mutex_unlock(&g_lock);
        nx_state_t *w = &X->work;
        if (nx_parse(inject, strlen(inject), team, w)) {
            w->phase = NX_READY;
            w->stale = false;
            w->err[0] = 0;
            w->fetched = time(NULL);
            if (!w->event_key[0]) snprintf(w->event_key, sizeof w->event_key, "injected");
            pthread_mutex_lock(&g_lock);
            X->injected = true;
            pthread_mutex_unlock(&g_lock);
            publish(cfg);
            printf("nexus: injected document, %d matches, %d ours, now queuing %s\n", w->nmatches, w->nours,
                   w->now_queuing[0] ? w->now_queuing : "nothing");
        } else {
            printf("nexus: the injected document didn't parse\n");
        }
        free(inject);
        return false;
    }
    bool due = (X->key[0] || X->base[0]) && X->event[0] && (X->kick || (X->every > 0 && now >= X->next));
    if (!due) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    X->kick = false;
    X->next = now + (X->every > 0 ? X->every : RETRY_S); /* whatever happens, not again at once */
    char url[192], *hdr = malloc(256);
    snprintf(url, sizeof url, "%s/event/%s", X->base[0] ? X->base : BASE, X->event);
    if (hdr) snprintf(hdr, 256, "Nexus-Api-Key: %s\r\nAccept: application/json\r\n", X->key[0] ? X->key : "test");
    X->work = X->st;
    pthread_mutex_unlock(&g_lock);
    if (!hdr) return false;

    char *body = NULL;
    size_t len = 0;
    double t0 = hal_seconds();
    int status = ix_get_url(url, hdr, NULL, NULL, &body, &len, BODY_MAX, 8000);
    memset(hdr, 0, 256);
    free(hdr);
    nx_state_t *w = &X->work; /* the worker's own: only this thread touches it */
    bool had = w->phase == NX_READY || w->stale;
    if (status == 200 && body && nx_parse(body, len, team, w)) {
        pthread_mutex_lock(&g_lock);
        X->injected = false; /* a real answer replaces a made-up one */
        pthread_mutex_unlock(&g_lock);
        w->phase = NX_READY;
        w->stale = false;
        w->err[0] = 0;
        w->fetched = time(NULL);
    } else {
        w->stale = had;
        if (status == 401 || status == 403) {
            w->phase = NX_REFUSED;
            snprintf(w->err, sizeof w->err, "nexus refused the key (%d)", status);
        } else if (status == 404) {
            w->phase = NX_UNKNOWN;
            snprintf(w->err, sizeof w->err, "this event isn't on nexus");
        } else {
            if (!had) w->phase = NX_ERROR;
            if (status < 0) snprintf(w->err, sizeof w->err, "nexus isn't answering");
            else if (status == 200) snprintf(w->err, sizeof w->err, "nexus's answer didn't parse");
            else snprintf(w->err, sizeof w->err, "nexus answered %d", status);
        }
    }
    printf("nexus: GET /event/%s -> %d, %u bytes in %.1f s%s\n", w->event_key[0] ? w->event_key : "?", status,
           (unsigned)len, hal_seconds() - t0, w->phase == NX_READY && !w->stale ? "" : " (kept the last)");
    free(body);
    pthread_mutex_lock(&g_lock);
    if (status != 200) X->next = now + RETRY_S; /* an error is tried again sooner, a success at the cadence */
    pthread_mutex_unlock(&g_lock);
    publish(cfg);
    return true;
}
