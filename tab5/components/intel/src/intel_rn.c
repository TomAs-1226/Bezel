/* The robot's own network, on the home worker (intel.h): Systemcore's vision aggregator, each Limelight's
 * REST status, and CTRE's Phoenix diagnostics server. Plain HTTP on the robot's LAN, short timeouts, one
 * request a pass, and only while a robot is connected.
 *
 *   aggregator  http://<robot>:4810/api/cameras every 20 s (every 2 min once it hasn't answered: a roboRIO
 *               has none), the cameras' names, addresses and whether each holds an NT session
 *   Limelights  http://<camera>:5807/status every 20 s each: fps, temperature, cpu, pipeline. The cameras are
 *               the aggregator's, else the ones NetworkTables names (tried as <name>.local)
 *   Phoenix     http://<robot>:1250/?action=getdevices every minute (5 once absent): each device's model
 *               and firmware, so mixed firmware across one model is caught before it bites
 *
 * Nothing here is required: an absent service is remembered as absent and asked again rarely. */
#include "intel.h"
#include "intel_net.h"
#include "intel_priv.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal.h"

#define AGG_S 20.0
#define AGG_ABSENT_S 120.0
#define CAM_S 20.0
#define PHX_S 60.0
#define PHX_ABSENT_S 300.0
#define BODY_MAX (96 * 1024)
#define TIMEOUT_MS 2500

typedef struct {
    char host[48];
    char nt_cams[160];
    unsigned cfg;
    double next_agg, next_phx, next_cam[RN_CAMS];
    double seen[RN_CAMS];      /* hal_seconds() of each camera's last answer */
    rn_state_t st, work;
} rn_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rn_t *N;

static rn_t *rn(void)
{
    if (!N) {
        N = calloc(1, sizeof *N);
        if (N) N->st.agg = N->st.phoenix = -1;
    }
    return N;
}

/* the NT names as cameras to ask directly, when there is no aggregator list (under the lock) */
static void nt_cams_locked(void)
{
    if (N->st.agg == 1 && N->st.ncams) return;
    int n = 0;
    const char *p = N->nt_cams;
    while (*p && n < RN_CAMS) {
        size_t l = strcspn(p, ",");
        if (l && l < 31) {
            rn_cam_t *c = &N->st.cams[n++];
            memset(c, 0, sizeof *c);
            c->fps = c->temp = c->cpu = c->ram = c->age_s = NAN;
            c->pipeline = -1;
            snprintf(c->name, sizeof c->name, "%.*s", (int)l, p);
        }
        p += l + (p[l] == ',');
    }
    N->st.ncams = n;
}

void rn_config(const char *host, const char *nt_cams)
{
    pthread_mutex_lock(&g_lock);
    if (rn()) {
        const char *h = host ? host : "";
        if (strcmp(N->host, h)) {
            snprintf(N->host, sizeof N->host, "%s", h);
            N->cfg++;
            unsigned g = N->st.gen;
            memset(&N->st, 0, sizeof N->st);
            N->st.agg = N->st.phoenix = -1;
            N->st.host_known = h[0] != 0;
            snprintf(N->st.host, sizeof N->st.host, "%s", h);
            N->st.gen = g + 1;
            N->next_agg = N->next_phx = 0;
            for (int i = 0; i < RN_CAMS; i++) N->next_cam[i] = 0, N->seen[i] = 0;
        }
        if (strcmp(N->nt_cams, nt_cams ? nt_cams : "")) {
            snprintf(N->nt_cams, sizeof N->nt_cams, "%s", nt_cams ? nt_cams : "");
            nt_cams_locked();
            N->st.gen++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void rn_refresh(void)
{
    pthread_mutex_lock(&g_lock);
    if (rn()) {
        N->next_agg = N->next_phx = 0;
        for (int i = 0; i < RN_CAMS; i++) N->next_cam[i] = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

unsigned rn_gen(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned g = N ? N->st.gen : 0;
    pthread_mutex_unlock(&g_lock);
    return g;
}

void rn_get(rn_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (N) {
        *out = N->st;
        double now = hal_seconds();
        for (int i = 0; i < out->ncams; i++) out->cams[i].age_s = N->seen[i] > 0 ? now - N->seen[i] : NAN;
    } else {
        memset(out, 0, sizeof *out);
        out->agg = out->phoenix = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

/* 0 aggregator, 1 phoenix, 2+i camera i; -1 nothing due (under the lock) */
static int due_locked(double now)
{
    if (!N || !N->host[0]) return -1;
    if (now >= N->next_agg) return 0;
    for (int i = 0; i < N->st.ncams; i++)
        if (now >= N->next_cam[i] && (N->st.cams[i].ip[0] || N->st.cams[i].name[0])) return 2 + i;
    if (now >= N->next_phx) return 1;
    return -1;
}

bool rn_due(double now)
{
    pthread_mutex_lock(&g_lock);
    bool d = due_locked(now) >= 0;
    pthread_mutex_unlock(&g_lock);
    return d;
}

bool rn_work(double now)
{
    pthread_mutex_lock(&g_lock);
    int what = due_locked(now);
    if (what < 0) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    unsigned cfg = N->cfg;
    char url[128];
    if (what == 0) {
        N->next_agg = now + AGG_S;
        snprintf(url, sizeof url, "http://%s:4810/api/cameras", N->host);
    } else if (what == 1) {
        N->next_phx = now + PHX_S;
        snprintf(url, sizeof url, "http://%s:1250/?action=getdevices", N->host);
    } else {
        const rn_cam_t *c = &N->st.cams[what - 2];
        N->next_cam[what - 2] = now + CAM_S;
        if (c->ip[0]) snprintf(url, sizeof url, "http://%s:5807/status", c->ip);
        else snprintf(url, sizeof url, "http://%s.local:5807/status", c->name);
    }
    pthread_mutex_unlock(&g_lock);

    char *body = NULL;
    size_t len = 0;
    int status = ix_get_url(url, NULL, NULL, NULL, &body, &len, BODY_MAX, TIMEOUT_MS);
    rn_state_t *w = malloc(sizeof *w);
    if (!w) {
        free(body);
        return true;
    }
    pthread_mutex_lock(&g_lock);
    *w = N->st;
    pthread_mutex_unlock(&g_lock);
    bool changed = false;
    if (what == 0) {
        rn_cam_t *cams = malloc(sizeof(rn_cam_t) * RN_CAMS);
        int n = status == 200 && body && cams ? rn_parse_cameras(body, len, cams, RN_CAMS) : -1;
        if (n >= 0) {
            /* keep each camera's status numbers across a new list, by address */
            for (int i = 0; i < n; i++)
                for (int k = 0; k < w->ncams; k++)
                    if (!strcmp(cams[i].ip, w->cams[k].ip) && cams[i].ip[0] && isnan(cams[i].fps)) {
                        bool nt = cams[i].nt;
                        char name[32];
                        snprintf(name, sizeof name, "%s", cams[i].name);
                        cams[i] = w->cams[k];
                        cams[i].nt = nt;
                        snprintf(cams[i].name, sizeof cams[i].name, "%s", name[0] ? name : w->cams[k].name);
                    }
            memcpy(w->cams, cams, sizeof(rn_cam_t) * (size_t)n);
            w->ncams = n;
            changed = true;
            w->agg = 1;
        } else if (w->agg != 0) {
            w->agg = 0;
            changed = true;
        }
        free(cams);
    } else if (what == 1) {
        rn_dev_t *devs = malloc(sizeof(rn_dev_t) * RN_DEVS);
        int n = status == 200 && body && devs ? rn_parse_phoenix(body, len, devs, RN_DEVS) : -1;
        if (n >= 0) {
            memcpy(w->devs, devs, sizeof(rn_dev_t) * (size_t)n);
            w->ndevs = n;
            w->phoenix = 1;
            changed = true;
        } else if (w->phoenix != 0) {
            w->phoenix = 0;
            changed = true;
        }
        free(devs);
    } else if (what - 2 < w->ncams) {
        rn_cam_t *c = &w->cams[what - 2];
        bool ok = status == 200 && body && rn_parse_ll_status(body, len, c);
        if (ok != c->answered || ok) changed = true;
        c->answered = ok;
    }
    w->checked = time(NULL);
    free(body);
    pthread_mutex_lock(&g_lock);
    if (cfg == N->cfg) {
        if (what == 0 && w->agg == 0) {
            N->next_agg = now + AGG_ABSENT_S;
            N->st = *w;
            nt_cams_locked(); /* no aggregator: the cameras NetworkTables names */
            *w = N->st;
        }
        if (what == 1 && w->phoenix == 0) N->next_phx = now + PHX_ABSENT_S;
        if (what >= 2 && what - 2 < w->ncams && w->cams[what - 2].answered) N->seen[what - 2] = hal_seconds();
        if (changed) {
            w->gen = N->st.gen + 1;
            N->st = *w;
        } else {
            N->st.checked = w->checked;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(w);
    return true;
}
