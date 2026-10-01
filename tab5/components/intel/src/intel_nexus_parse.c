/* Nexus for FRC's event document, and when a match's reminders ring. Pure: host-tested (test/test_intel.c).
 *
 * The document (GET https://frc.nexus/api/v1/event/{eventKey}, header Nexus-Api-Key) as Nexus's API page
 * describes it: dataAsOfTime, nowQueuing (a match's label or null), matches [{label, status, redTeams,
 * blueTeams, times {estimatedQueueTime, estimatedOnDeckTime, estimatedOnFieldTime, estimatedStartTime,
 * actualQueueTime, ...}, breakAfter, replayOf}], announcements [{id, announcement, postedTime}] and
 * partsRequests [{id, parts, requestedByTeam, postedTime}], times in epoch milliseconds. None of it could be
 * checked against a live answer when this was written, so every field is read defensively: a missing one is
 * absent, never zero; a time in seconds is taken as well as one in milliseconds; a few other spellings are
 * tried; a team is a number, "254" or "frc254". */
#include "intel.h"
#include "intel_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- labels */

static int number_after(const char *s, const char **rest)
{
    while (*s && !isdigit((unsigned char)*s)) s++;
    if (!*s) return -1;
    int v = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    if (rest) *rest = s;
    return v;
}

bool nx_label(const char *name, char *label, size_t n, int *level, int *set, int *number)
{
    int lv = -2, st = 1, nb = -1;
    if (label && n) label[0] = 0;
    if (!name || !name[0]) return false;
    const char *rest = NULL;
    int a = number_after(name, &rest);
    int b = rest ? number_after(rest, NULL) : -1;
    if (a < 0) return false;
    char lo[40];
    size_t k = 0;
    for (const char *p = name; *p && k < sizeof lo - 1; p++) lo[k++] = (char)tolower((unsigned char)*p);
    lo[k] = 0;
    if (strstr(lo, "practice")) lv = -1, nb = a;
    else if (strstr(lo, "qual") || (lo[0] == 'q' && isdigit((unsigned char)lo[1])) || !strncmp(lo, "q ", 2)) lv = 0, nb = a;
    else if (strstr(lo, "quarter")) lv = 2, st = a, nb = b > 0 ? b : 1;
    else if (strstr(lo, "final") && !strstr(lo, "semi")) lv = 4, st = 1, nb = a;
    else if (strstr(lo, "semi") || strstr(lo, "playoff") || strstr(lo, "elim") || strstr(lo, "match") ||
             strstr(lo, "upper") || strstr(lo, "lower") || strstr(lo, "round"))
        lv = 3, st = a, nb = 1;
    if (lv == -2) return false;
    if (level) *level = lv;
    if (set) *set = st;
    if (number) *number = nb;
    if (label && n) {
        if (lv == -1) snprintf(label, n, "P%d", nb);
        else if (lv == 0) snprintf(label, n, "Q%d", nb);
        else if (lv == 4) snprintf(label, n, "F%d", nb);
        else if (lv == 2) {
            if (nb <= 1) snprintf(label, n, "QF%d", st);
            else snprintf(label, n, "QF%d-%d", st, nb);
        } else snprintf(label, n, "SF%d", st);
    }
    return true;
}

const char *nx_status_short(const char *status)
{
    switch (nx_stage(status)) {
    case 1: return "queuing";
    case 2: return "on deck";
    case 3: return "on the field";
    default: break;
    }
    if (status && ix_has(status, "soon")) return "queuing soon";
    return status && status[0] ? status : "";
}

int nx_stage(const char *s)
{
    if (!s || !s[0]) return 0;
    if (ix_has(s, "field") || ix_has(s, "playing") || ix_has(s, "in progress")) return 3;
    if (ix_has(s, "deck")) return 2;
    if (ix_has(s, "soon")) return 0;
    if (ix_has(s, "queu")) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the document */

static void teams(const ix_json_t *j, int arr, int *out)
{
    for (int i = 0; i < 3; i++) out[i] = 0;
    if (arr < 0 || j->d.t[arr].type != JL_ARR) return;
    int k = 0;
    for (int i = 0; i < j->d.t[arr].size && k < 3; i++) {
        int t = ix_team(j, jl_at(&j->d, arr, i));
        if (t) out[k++] = t;
    }
}

/* a time under `times`, or on the match itself */
static time_t mtime(const ix_json_t *j, int m, int times, const char *const *keys)
{
    int t = times >= 0 ? ix_get_any(j, times, keys) : -1;
    if (t < 0) t = ix_get_any(j, m, keys);
    return ix_time(j, t);
}

static const char *const K_QUEUE[] = { "estimatedQueueTime", "queueTime", "estimatedQueuingTime", NULL };
static const char *const K_DECK[] = { "estimatedOnDeckTime", "onDeckTime", NULL };
static const char *const K_FIELD[] = { "estimatedOnFieldTime", "onFieldTime", NULL };
static const char *const K_START[] = { "estimatedStartTime", "startTime", "estimatedMatchTime", "scheduledStartTime",
                                       "scheduledTime", NULL };
static const char *const K_QUEUED[] = { "actualQueueTime", NULL };
static const char *const K_STARTED[] = { "actualOnFieldTime", "actualStartTime", NULL };
static const char *const K_RED[] = { "redTeams", "red", NULL };
static const char *const K_BLUE[] = { "blueTeams", "blue", NULL };
static const char *const K_LABEL[] = { "label", "name", "matchName", NULL };
static const char *const K_ANN[] = { "announcement", "text", "message", "body", NULL };
static const char *const K_PARTS[] = { "parts", "text", "request", "message", NULL };
static const char *const K_POSTED[] = { "postedTime", "time", "createdAt", "timestamp", NULL };
static const char *const K_BY[] = { "requestedByTeam", "team", "teamNumber", NULL };

bool nx_parse(const char *js, size_t len, int team, nx_state_t *out)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return false;
    if (j.d.t[0].type != JL_OBJ) {
        ix_free(&j);
        return false;
    }
    int matches = ix_get(&j, 0, "matches");
    if (matches >= 0 && j.d.t[matches].type != JL_ARR) matches = -1;
    int ann = ix_get(&j, 0, "announcements"), parts = ix_get(&j, 0, "partsRequests");
    if (matches < 0 && ann < 0 && ix_get(&j, 0, "nowQueuing") < 0) { /* not an event document (an error's JSON) */
        ix_free(&j);
        return false;
    }
    out->as_of = ix_time(&j, ix_get(&j, 0, "dataAsOfTime"));
    ix_str(&j, 0, "nowQueuing", out->now_queuing, sizeof out->now_queuing);
    out->now_label[0] = 0;
    out->now_index = -1;
    if (out->now_queuing[0]) nx_label(out->now_queuing, out->now_label, sizeof out->now_label, NULL, NULL, NULL);
    out->nmatches = 0;
    out->nours = 0;
    for (int i = 0; matches >= 0 && i < j.d.t[matches].size; i++) {
        int m = jl_at(&j.d, matches, i);
        if (m < 0 || j.d.t[m].type != JL_OBJ) continue;
        int idx = out->nmatches++;
        char name[32];
        ix_tok_str(&j, ix_get_any(&j, m, K_LABEL), name, sizeof name);
        if (out->now_queuing[0] && !strcmp(name, out->now_queuing)) out->now_index = idx;
        int red[3], blue[3];
        int rt = ix_get_any(&j, m, K_RED), bt = ix_get_any(&j, m, K_BLUE);
        /* {"red": {"teams": [...]}} as well as a bare array */
        if (rt >= 0 && j.d.t[rt].type == JL_OBJ) rt = ix_get(&j, rt, "teams");
        if (bt >= 0 && j.d.t[bt].type == JL_OBJ) bt = ix_get(&j, bt, "teams");
        teams(&j, rt, red);
        teams(&j, bt, blue);
        int ours = 0;
        for (int k = 0; k < 3 && team > 0; k++) {
            if (red[k] == team) ours = 1;
            if (blue[k] == team) ours = 2;
        }
        if (!ours || out->nours >= NX_OURS) continue;
        nx_match_t *x = &out->ours[out->nours++];
        memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", name);
        x->level = x->set = x->number = -1;
        if (!nx_label(name, x->label, sizeof x->label, &x->level, &x->set, &x->number)) x->level = -1;
        ix_str(&j, m, "status", x->status, sizeof x->status);
        memcpy(x->red, red, sizeof red);
        memcpy(x->blue, blue, sizeof blue);
        x->ours = ours;
        x->index = idx;
        int times = ix_get(&j, m, "times");
        if (times >= 0 && j.d.t[times].type != JL_OBJ) times = -1;
        x->queue_at = mtime(&j, m, times, K_QUEUE);
        x->deck_at = mtime(&j, m, times, K_DECK);
        x->field_at = mtime(&j, m, times, K_FIELD);
        x->start_at = mtime(&j, m, times, K_START);
        x->queued = mtime(&j, m, times, K_QUEUED);
        x->started = mtime(&j, m, times, K_STARTED);
        x->replay = ix_get(&j, m, "replayOf") >= 0;
    }
    /* the newest few announcements and parts requests, newest first whatever order they came in */
    out->nann = 0;
    for (int i = 0; ann >= 0 && j.d.t[ann].type == JL_ARR && i < j.d.t[ann].size; i++) {
        int a = jl_at(&j.d, ann, i);
        char text[160];
        if (!ix_tok_str(&j, ix_get_any(&j, a, K_ANN), text, sizeof text) &&
            !(j.d.t[a].type == JL_STR && ix_tok_str(&j, a, text, sizeof text)))
            continue;
        time_t at = ix_time(&j, ix_get_any(&j, a, K_POSTED));
        int k;
        if (out->nann < NX_ANN) k = out->nann++;
        else if (at <= out->ann[(k = NX_ANN - 1)].at) continue; /* older than every one kept */
        out->ann[k].at = at;
        snprintf(out->ann[k].text, sizeof out->ann[k].text, "%s", text);
        for (; k > 0 && out->ann[k].at > out->ann[k - 1].at; k--) { /* insertion, newest first */
            nx_ann_t t = out->ann[k];
            out->ann[k] = out->ann[k - 1];
            out->ann[k - 1] = t;
        }
    }
    out->nparts = 0;
    for (int i = 0; parts >= 0 && j.d.t[parts].type == JL_ARR && i < j.d.t[parts].size; i++) {
        int a = jl_at(&j.d, parts, i);
        char text[96];
        if (!ix_tok_str(&j, ix_get_any(&j, a, K_PARTS), text, sizeof text)) continue;
        time_t at = ix_time(&j, ix_get_any(&j, a, K_POSTED));
        int k;
        if (out->nparts < NX_PARTS) k = out->nparts++;
        else if (at <= out->parts[(k = NX_PARTS - 1)].at) continue;
        out->parts[k].at = at;
        out->parts[k].team = ix_team(&j, ix_get_any(&j, a, K_BY));
        snprintf(out->parts[k].text, sizeof out->parts[k].text, "%s", text);
        for (; k > 0 && out->parts[k].at > out->parts[k - 1].at; k--) {
            nx_part_t t = out->parts[k];
            out->parts[k] = out->parts[k - 1];
            out->parts[k - 1] = t;
        }
    }
    ix_free(&j);
    return true;
}

/* ---------------------------------------------------------------- the alarms' timing */

void mt_times(time_t pred, time_t sched, const nx_match_t *nx, mt_times_t *o)
{
    memset(o, 0, sizeof *o);
    if (nx && nx->start_at) o->start = nx->start_at, o->src = MT_SRC_NEXUS;
    else if (nx && nx->field_at) o->start = nx->field_at, o->src = MT_SRC_NEXUS;
    else if (pred) o->start = pred, o->src = MT_SRC_PREDICTED;
    else if (sched) o->start = sched, o->src = MT_SRC_SCHEDULED;
    if (nx) {
        o->queue_at = nx->queue_at;
        o->stage = nx_stage(nx->status);
        if (nx->started) o->stage = 3;
        else if (nx->queued && o->stage < 1) o->stage = 1;
    }
}

time_t mt_ring_at(const mt_times_t *t, int kind, int lead_min)
{
    if (lead_min <= 0) return 0;
    time_t at = t->start ? t->start - (time_t)lead_min * 60 : 0;
    if (kind == 0 && t->queue_at) {
        time_t q = t->queue_at - NX_WALK_S;
        if (!at || q < at) at = q;
    }
    return at;
}

int mt_due(const mt_times_t *t, time_t now, int queue_min, int match_min, unsigned fired)
{
    if (t->stage >= 3) return 0;                 /* on the field: an alarm now is noise */
    bool passed = t->start && now >= t->start;   /* its time has gone, and Nexus doesn't say otherwise */
    time_t mq = mt_ring_at(t, 0, queue_min), mm = mt_ring_at(t, 1, match_min);
    bool m = match_min > 0 && !(fired & 2) &&
             (t->stage == 2 || (mm && now >= mm && !passed));
    bool q = queue_min > 0 && !(fired & 1) &&
             (t->stage == 1 || (mq && now >= mq && !passed));
    if (m) return 2;
    if (q) return 1;
    return 0;
}

const char *mt_src_name(mt_src_t s)
{
    switch (s) {
    case MT_SRC_NEXUS: return "nexus";
    case MT_SRC_PREDICTED: return "tba predicted";
    case MT_SRC_SCHEDULED: return "scheduled";
    default: return "";
    }
}
