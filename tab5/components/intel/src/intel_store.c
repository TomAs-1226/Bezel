/* The insights the tablet keeps, and when the island may say one (intel.h). Pure: no clock of its own (the
 * caller passes hal_seconds()), no I/O; host-tested. The UI thread's alone.
 *
 * Never nag: an insight is said at most once (its key remembers it until it has been gone a while), a prio 2
 * one at most every SAY_GAP_S, and no more than SAY_HOUR in any hour whatever their priority; nothing in the
 * quiet hours, nothing of a category the owner turned off; a dismissed one stays dismissed. The list shows
 * them all, the most important first. */
#include "intel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAY_GAP_S 600.0      /* between two prio-2 messages */
#define SAY_HOUR 6           /* messages in any hour */
#define REMEMBER_S 3600.0    /* a said or dismissed key, gone from the rules, isn't said again for this long */
#define NGONE 32

static intel_insight_t S[INTEL_MAX];
static int NS;
static unsigned GEN;
static double SAID[SAY_HOUR];    /* when the last few were said, a ring */
static int SAID_I;
static double LAST_SAY;
/* keys said or dismissed that the rules have dropped: remembered, so a flapping rule isn't said twice */
static struct { char key[40]; double at; bool dismissed; } GONE[NGONE];

void intel_cfg_default(intel_cfg_t *c)
{
    c->cats = IC_ALL;
    c->quiet_from = 22;
    c->quiet_to = 7;
    c->brief_hhmm = -1;
    c->ai = false;
}

void intel_cfg_format(const intel_cfg_t *c, char *out, size_t n)
{
    snprintf(out, n, "%u,%d,%d,%d,%d", c->cats, c->quiet_from, c->quiet_to, c->brief_hhmm, c->ai ? 1 : 0);
}

bool intel_cfg_parse(const char *s, intel_cfg_t *c)
{
    unsigned cats;
    int qf, qt, br, ai;
    if (!s || sscanf(s, "%u,%d,%d,%d,%d", &cats, &qf, &qt, &br, &ai) != 5) return false;
    if (qf < 0 || qf > 23 || qt < 0 || qt > 23 || br < -1 || br > 2359 || (br >= 0 && br % 100 > 59)) return false;
    c->cats = cats & IC_ALL;
    c->quiet_from = qf;
    c->quiet_to = qt;
    c->brief_hhmm = br;
    c->ai = ai != 0;
    return true;
}

static int gone_find(const char *key)
{
    for (int i = 0; i < NGONE; i++)
        if (GONE[i].key[0] && !strcmp(GONE[i].key, key)) return i;
    return -1;
}

static void gone_add(const intel_insight_t *x, double now)
{
    if (!x->said && !x->dismissed) return;
    int k = gone_find(x->key);
    if (k < 0) {
        k = 0;
        for (int i = 1; i < NGONE; i++)
            if (GONE[i].at < GONE[k].at) k = i;
    }
    snprintf(GONE[k].key, sizeof GONE[k].key, "%s", x->key);
    GONE[k].at = now;
    GONE[k].dismissed = x->dismissed;
}

static int rank(const intel_insight_t *a)
{
    return (a->dismissed ? 0 : 100) + a->prio * 10;
}

static int cmp(const void *pa, const void *pb)
{
    const intel_insight_t *a = pa, *b = pb;
    int d = rank(b) - rank(a);
    if (d) return d;
    return a->first < b->first ? 1 : a->first > b->first ? -1 : 0; /* newer first */
}

int intel_merge(const intel_insight_t *fresh, int n, double now)
{
    bool changed = false;
    bool seen[INTEL_MAX] = { 0 };
    for (int i = 0; i < n; i++) {
        const intel_insight_t *f = &fresh[i];
        int k = -1;
        for (int j = 0; j < NS; j++)
            if (!strcmp(S[j].key, f->key)) k = j;
        if (k >= 0) {
            /* known: the words and priority may move on; when it was raised and whether it was said stay */
            intel_insight_t *x = &S[k];
            if (strcmp(x->text, f->text) || x->prio != f->prio || strcmp(x->why, f->why)) changed = true;
            bool said = x->said, dism = x->dismissed;
            double first = x->first;
            time_t wall = x->wall;
            int oldprio = x->prio;
            *x = *f;
            x->first = first;
            x->wall = wall;
            x->last = now;
            x->dismissed = dism;
            /* a rise to "act now" is worth saying even if the milder version was said */
            x->said = said && !(f->prio == 3 && oldprio < 3);
            seen[k] = true;
            continue;
        }
        /* new: unless it's one said or dismissed a moment ago that the rules had dropped */
        int g = gone_find(f->key);
        intel_insight_t x = *f;
        x.first = x.last = now;
        x.said = g >= 0 && now - GONE[g].at < REMEMBER_S;
        x.dismissed = g >= 0 && GONE[g].dismissed && now - GONE[g].at < REMEMBER_S;
        if (NS < INTEL_MAX) {
            S[NS] = x;
            seen[NS++] = true;
        } else {
            /* full: the least important kept one makes room, if it is less important */
            int w = 0;
            for (int j = 1; j < NS; j++)
                if (rank(&S[j]) < rank(&S[w])) w = j;
            if (rank(&S[w]) >= rank(&x)) continue;
            gone_add(&S[w], now);
            S[w] = x;
            seen[w] = true;
        }
        changed = true;
    }
    /* what the rules no longer raise: kept until its `until`, then gone (remembered if said) */
    int k = 0;
    for (int j = 0; j < NS; j++) {
        if (seen[j] || (S[j].until > 0 && now < S[j].until)) {
            S[k++] = S[j];
            continue;
        }
        gone_add(&S[j], now);
        changed = true;
    }
    NS = k;
    if (changed) {
        qsort(S, (size_t)NS, sizeof S[0], cmp);
        GEN++;
    }
    return NS;
}

static bool quiet(const intel_cfg_t *c, int hour)
{
    if (c->quiet_from == c->quiet_to || hour < 0) return false;
    if (c->quiet_from < c->quiet_to) return hour >= c->quiet_from && hour < c->quiet_to;
    return hour >= c->quiet_from || hour < c->quiet_to; /* across midnight */
}

bool intel_next_say(const intel_cfg_t *cfg, int hour, double now, intel_insight_t *out)
{
    if (quiet(cfg, hour)) return false;
    /* the hour's budget: the oldest of the last SAY_HOUR said within the hour means it's spent */
    double oldest = SAID[SAID_I];
    if (oldest > 0 && now - oldest < 3600) return false;
    for (int j = 0; j < NS; j++) {
        intel_insight_t *x = &S[j];
        if (x->said || x->dismissed || x->prio < 2 || !(cfg->cats & (1u << x->cat))) continue;
        if (x->prio < 3 && LAST_SAY > 0 && now - LAST_SAY < SAY_GAP_S) continue;
        x->said = true;
        LAST_SAY = now;
        SAID[SAID_I] = now;
        SAID_I = (SAID_I + 1) % SAY_HOUR;
        if (out) *out = *x;
        GEN++;
        return true;
    }
    return false;
}

int intel_list(intel_insight_t *out, int max)
{
    int n = NS < max ? NS : max;
    memcpy(out, S, sizeof S[0] * (size_t)n);
    return n;
}

bool intel_top(intel_insight_t *out)
{
    for (int j = 0; j < NS; j++)
        if (!S[j].dismissed) {
            if (out) *out = S[j];
            return true;
        }
    return false;
}

void intel_dismiss(const char *key)
{
    for (int j = 0; j < NS; j++)
        if (!strcmp(S[j].key, key) && !S[j].dismissed) {
            S[j].dismissed = true;
            qsort(S, (size_t)NS, sizeof S[0], cmp);
            GEN++;
            return;
        }
}

void intel_clear(void)
{
    NS = 0;
    memset(GONE, 0, sizeof GONE);
    memset(SAID, 0, sizeof SAID);
    SAID_I = 0;
    LAST_SAY = 0;
    GEN++;
}

unsigned intel_gen(void) { return GEN; }
