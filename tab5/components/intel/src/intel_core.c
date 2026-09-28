/* The core's shared side: start-up, the services list, and the AIs' tools (intel.h).
 *
 * The tools run on an AI's worker. They answer from what the sources hold; a source older than STALE_S is
 * asked again (the home worker is started for it) and waited on for a few seconds, so "what's queuing"
 * never answers with the queue of ten minutes ago when a fresher one is a second away. The insights and the
 * briefing are the UI's (its rules pass, ui_intel.c), posted here as text. */
#include "intel.h"
#include "intel_net.h"
#include "intel_priv.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "analyze.h"
#include "assist.h"
#include "hal.h"
#include "tba.h"

#define STALE_S 90
#define WAIT_S 8.0

void intel_net_init(void); /* intel_net.c */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_insights, *g_brief;

void intel_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    intel_net_init();
}

void intel_post_text(const char *insights, const char *briefing)
{
    char *a = insights ? strdup(insights) : NULL, *b = briefing ? strdup(briefing) : NULL;
    pthread_mutex_lock(&g_lock);
    free(g_insights);
    free(g_brief);
    g_insights = a;
    g_brief = b;
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- the services, a line each */

int intel_services(char lines[][96], int max)
{
    int k = 0;
    nx_state_t *nx = malloc(sizeof *nx);
    sb_state_t *sb = malloc(sizeof *sb);
    rn_state_t *rn = malloc(sizeof *rn);
    if (!nx || !sb || !rn) {
        free(nx);
        free(sb);
        free(rn);
        return 0;
    }
    nx_get(nx);
    sb_get(sb);
    rn_get(rn);
    char hm[8];
    if (k < max) {
        char *l = lines[k++];
        switch (nx->phase) {
        case NX_OFF: snprintf(l, 96, "nexus: off \xc2\xb7 add NEXUS_API_KEY to KEYS.ENV"); break;
        case NX_NO_EVENT: snprintf(l, 96, "nexus: key set \xc2\xb7 no event to follow yet"); break;
        case NX_LOADING: snprintf(l, 96, "nexus: asking about %s", nx->event_key); break;
        case NX_READY:
            ix_hm(nx->fetched, hm, sizeof hm);
            snprintf(l, 96, "nexus: %s \xc2\xb7 %d of ours \xc2\xb7 as of %s%s", nx->event_key, nx->nours, hm,
                     nx->stale ? " (offline)" : "");
            break;
        case NX_UNKNOWN: snprintf(l, 96, "nexus: %s isn't on nexus", nx->event_key); break;
        case NX_REFUSED: snprintf(l, 96, "nexus: the key was refused \xc2\xb7 check NEXUS_API_KEY"); break;
        default: snprintf(l, 96, "nexus: %s", nx->err[0] ? nx->err : "not answering"); break;
        }
    }
    if (k < max) {
        char *l = lines[k++];
        if (sb->phase == SB_READY && sb->have_year && !isnan(sb->epa)) {
            snprintf(l, 96, "statbotics: EPA %.1f", sb->epa);
            if (sb->rank > 0) ix_addf(l, 96, " \xc2\xb7 #%d of %d", sb->rank, sb->team_count);
            if (sb->stale) ix_addf(l, 96, " (offline)");
        } else if (sb->phase == SB_ERROR || sb->err[0]) {
            snprintf(l, 96, "statbotics: %s", sb->err[0] ? sb->err : "not answering");
        } else if (sb->phase == SB_READY) {
            snprintf(l, 96, "statbotics: no EPA for %d this season yet", sb->team);
        } else {
            snprintf(l, 96, "statbotics: waiting (no key needed)");
        }
    }
    if (k < max) {
        char *l = lines[k++];
        if (!rn->host_known) snprintf(l, 96, "robot network: no robot connected");
        else {
            snprintf(l, 96, "robot network: %s", rn->host);
            if (rn->agg == 1) ix_addf(l, 96, " \xc2\xb7 aggregator %d camera%s", rn->ncams, rn->ncams == 1 ? "" : "s");
            else if (rn->agg == 0) ix_addf(l, 96, " \xc2\xb7 no aggregator");
            if (rn->phoenix == 1) ix_addf(l, 96, " \xc2\xb7 phoenix %d device%s", rn->ndevs, rn->ndevs == 1 ? "" : "s");
            else if (rn->phoenix == 0) ix_addf(l, 96, " \xc2\xb7 no phoenix server");
        }
    }
    free(nx);
    free(sb);
    free(rn);
    return k;
}

/* ---------------------------------------------------------------- the tools */

bool intel_is_tool(const char *name)
{
    return name && (!strcmp(name, "insights") || !strcmp(name, "event_live") || !strcmp(name, "robot_network"));
}

static bool arg_bool(const char *args, const char *key)
{
    if (!args) return false;
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(args, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return !strncmp(p, "true", 4);
}

static void ask_again(bool nexus, bool stat, bool robot)
{
    if (nexus) nx_refresh();
    if (stat) sb_refresh();
    if (robot) rn_refresh();
    tba_ext_want();
}

static void tool_insights(const char *args, char *out, size_t n)
{
    bool brief = !strstr(args ? args : "", "\"briefing\"") || arg_bool(args, "briefing");
    pthread_mutex_lock(&g_lock);
    /* the first line is the transcript's summary of the call: what matters most */
    int k = 0;
    for (const char *p = g_insights; p && *p; p++) k += *p == '\n';
    if (g_insights && g_insights[0])
        snprintf(out, n, "%d noticed, the most important first (prio 3 act now, 2 worth a look, 1 good to know):\n%s", k + 1,
                 g_insights);
    else snprintf(out, n, "nothing noticed right now");
    if (brief) ix_addf(out, n, "\n\nbriefing:\n%s", g_brief && g_brief[0] ? g_brief : "not made yet");
    pthread_mutex_unlock(&g_lock);
    char (*lines)[96] = malloc(sizeof *lines * 4);
    if (lines) {
        int k = intel_services(lines, 4);
        ix_addf(out, n, "\n\nsources:");
        for (int i = 0; i < k; i++) ix_addf(out, n, "\n%s", lines[i]);
        free(lines);
    }
}

static void wait_for(unsigned (*gen)(void), unsigned g0)
{
    for (double t = 0; t < WAIT_S && gen() == g0; t += 0.25) usleep(250000);
}

static void tool_event(const char *args, char *out, size_t n)
{
    nx_state_t *nx = malloc(sizeof *nx);
    sb_state_t *sb = malloc(sizeof *sb);
    if (!nx || !sb) {
        free(nx);
        free(sb);
        snprintf(out, n, "out of memory");
        return;
    }
    nx_get(nx);
    sb_get(sb);
    time_t now = time(NULL);
    bool cached = arg_bool(args, "cached"); /* the UI's own reading: never waits */
    bool nx_old = (nx->phase == NX_READY || nx->phase == NX_LOADING) && (!nx->fetched || now - nx->fetched > STALE_S);
    bool sb_old = !sb->fetched || now - sb->fetched > 15 * 60;
    if (!cached && (arg_bool(args, "refresh") || nx_old || sb_old)) {
        unsigned g0 = nx_gen(), s0 = sb_gen();
        ask_again(arg_bool(args, "refresh") || nx_old, arg_bool(args, "refresh") || sb_old, false);
        if (nx->phase != NX_OFF && nx->phase != NX_NO_EVENT) wait_for(nx_gen, g0);
        if (sb_old) wait_for(sb_gen, s0);
        nx_get(nx);
        sb_get(sb);
    }
    char hm[8], hm2[8];
    out[0] = 0;
    /* Nexus: the queue */
    switch (nx->phase) {
    case NX_OFF:
        ix_addf(out, n, "nexus (live queue): not set up; the owner adds NEXUS_API_KEY to CATOS/KEYS.ENV on the card\n");
        break;
    case NX_NO_EVENT: ix_addf(out, n, "nexus: no event to follow (the blue alliance names none yet)\n"); break;
    case NX_READY: {
        ix_hm(nx->fetched, hm, sizeof hm);
        ix_addf(out, n, "nexus, %s as of %s%s: ", nx->event_key, hm, nx->stale ? " (offline: the last answer)" : "");
        ix_addf(out, n, "now queuing %s\n", nx->now_queuing[0] ? nx->now_queuing : "nothing");
        for (int i = 0; i < nx->nours; i++) {
            const nx_match_t *m = &nx->ours[i];
            if (m->started && now - m->started > 30 * 60) continue; /* long done */
            ix_addf(out, n, "%s (%s): %s", m->label[0] ? m->label : m->name, m->ours == 1 ? "red" : "blue",
                    m->status[0] ? m->status : "not yet");
            if (m->queue_at) {
                ix_hm(m->queue_at, hm, sizeof hm);
                ix_addf(out, n, ", queue ~%s", hm);
            }
            if (m->start_at) {
                ix_hm(m->start_at, hm2, sizeof hm2);
                ix_addf(out, n, ", start ~%s", hm2);
            }
            if (nx->now_index >= 0 && m->index >= nx->now_index) ix_addf(out, n, ", %d matches after the one queuing", m->index - nx->now_index);
            if (m->replay) ix_addf(out, n, ", a replay");
            ix_addf(out, n, "\n");
        }
        for (int i = 0; i < nx->nann; i++) {
            ix_hm(nx->ann[i].at, hm, sizeof hm);
            ix_addf(out, n, "announcement %s: %s\n", nx->ann[i].at ? hm : "", nx->ann[i].text);
        }
        for (int i = 0; i < nx->nparts; i++)
            ix_addf(out, n, "parts request from %d: %s\n", nx->parts[i].team, nx->parts[i].text);
        break;
    }
    default: ix_addf(out, n, "nexus: %s\n", nx->err[0] ? nx->err : "no answer yet"); break;
    }
    /* Statbotics */
    if (sb->have_year && !isnan(sb->epa)) {
        ix_addf(out, n, "statbotics %d: EPA %.1f", sb->year, sb->epa);
        if (!isnan(sb->epa_sd)) ix_addf(out, n, " (sd %.1f)", sb->epa_sd);
        if (sb->rank > 0) ix_addf(out, n, ", world rank %d of %d", sb->rank, sb->team_count);
        if (!isnan(sb->percentile)) ix_addf(out, n, ", percentile %.0f", sb->percentile * 100);
        if (sb->state_rank > 0) ix_addf(out, n, ", state rank %d of %d", sb->state_rank, sb->state_count);
        if (sb->wins + sb->losses + sb->ties) ix_addf(out, n, ", record %d-%d-%d", sb->wins, sb->losses, sb->ties);
        ix_addf(out, n, "\n");
    } else {
        ix_addf(out, n, "statbotics: %s\n", sb->err[0] ? sb->err : "no season numbers yet");
    }
    if (sb->have_event) {
        ix_addf(out, n, "at %s: EPA %.1f", sb->event_key, sb->event_epa);
        if (sb->event_rank > 0) ix_addf(out, n, ", rank %d of %d", sb->event_rank, sb->event_teams);
        ix_addf(out, n, ", record %d-%d-%d\n", sb->ev_wins, sb->ev_losses, sb->ev_ties);
    }
    for (int i = 0; i < sb->nmatches; i++) {
        const sb_match_t *m = &sb->matches[i];
        ix_addf(out, n, "%s (%s)", m->label, m->ours == 1 ? "red" : "blue");
        if (m->played && m->red_score >= 0)
            ix_addf(out, n, ": played, red %d blue %d", m->red_score, m->blue_score);
        else if (!isnan(m->win)) {
            ix_addf(out, n, ": %.0f%% to win", m->win * 100);
            if (!isnan(m->red_pred)) ix_addf(out, n, ", predicted red %.0f blue %.0f", m->red_pred, m->blue_pred);
        }
        ix_addf(out, n, "\n");
    }
    free(nx);
    free(sb);
}

static void tool_robot(const char *args, char *out, size_t n)
{
    rn_state_t *rn = malloc(sizeof *rn);
    if (!rn) {
        snprintf(out, n, "out of memory");
        return;
    }
    rn_get(rn);
    if (rn->host_known && !arg_bool(args, "cached") &&
        (arg_bool(args, "refresh") || !rn->checked || time(NULL) - rn->checked > 60)) {
        unsigned g0 = rn_gen();
        ask_again(false, false, true);
        wait_for(rn_gen, g0);
        rn_get(rn);
    }
    out[0] = 0;
    if (!rn->host_known) {
        snprintf(out, n, "no robot connected: the robot's network (cameras, the aggregator, phoenix) can't be asked");
        free(rn);
        return;
    }
    ix_addf(out, n, "robot at %s\n", rn->host);
    ix_addf(out, n, "vision aggregator (:4810): %s\n", rn->agg == 1 ? "answering" : rn->agg == 0 ? "absent (a roboRIO has none)" : "not asked yet");
    for (int i = 0; i < rn->ncams; i++) {
        const rn_cam_t *c = &rn->cams[i];
        ix_addf(out, n, "camera %s%s%s: %s", c->name, c->ip[0] ? " at " : "", c->ip, c->nt ? "in NetworkTables" : "no NT session");
        if (c->answered) {
            if (!isnan(c->fps)) ix_addf(out, n, ", %.0f fps", c->fps);
            if (!isnan(c->temp)) ix_addf(out, n, ", %.0f C", c->temp);
            if (!isnan(c->cpu)) ix_addf(out, n, ", cpu %.0f%%", c->cpu);
            if (c->pipeline >= 0) ix_addf(out, n, ", pipeline %d %s", c->pipeline, c->pipe_type);
        } else {
            ix_addf(out, n, ", its :5807/status isn't answering");
        }
        ix_addf(out, n, "\n");
    }
    if (rn->phoenix == 1) {
        ix_addf(out, n, "phoenix diagnostics (:1250): %d devices\n", rn->ndevs);
        for (int i = 0; i < rn->ndevs; i++)
            ix_addf(out, n, "%s id %d%s%s firmware %s%s%s\n", rn->devs[i].model, rn->devs[i].id, rn->devs[i].name[0] ? " " : "",
                    rn->devs[i].name, rn->devs[i].firmware[0] ? rn->devs[i].firmware : "?", rn->devs[i].bus[0] ? " on " : "",
                    rn->devs[i].bus);
    } else {
        ix_addf(out, n, "phoenix diagnostics (:1250): %s\n", rn->phoenix == 0 ? "not answering (no Phoenix server running)" : "not asked yet");
    }
    free(rn);
}

bool intel_tool(const char *name, const char *args, char *out, size_t n)
{
    if (!out || n < 64) return false;
    out[0] = 0;
    if (!strcmp(name, "insights")) tool_insights(args, out, n);
    else if (!strcmp(name, "event_live")) tool_event(args, out, n);
    else if (!strcmp(name, "robot_network")) tool_robot(args, out, n);
    else return false;
    return true;
}

/* ---------------------------------------------------------------- the briefing's summary, by a model
 *
 * Only when asked (the briefing's button, or the daily briefing with "ai summary" on), and at most
 * SUM_PER_DAY a day: the briefing itself is the tablet's own and costs nothing. On the assistant's worker
 * (assist_post_job), after any conversation turn in flight. */

#define SUM_PER_DAY 8

static struct {
    int state;          /* 0 none, 1 asking, 2 done, 3 failed */
    char *text;         /* the answer, or why not */
    char *in;           /* the briefing sent */
    int team;
    int day, used;
    unsigned gen;
} SM;

static void sum_job(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    char *in = SM.in;
    SM.in = NULL;
    int team = SM.team;
    pthread_mutex_unlock(&g_lock);
    char sys[600], why[128] = "";
    snprintf(sys, sizeof sys,
             "You are the pit's chief of staff for FRC team %d. The tablet's briefing follows: facts it gathered "
             "from The Blue Alliance, Nexus (the live queue), Statbotics, the robot and the pit. Turn it into at most "
             "three short sentences for the pit lead: first what to do next (queue, battery, a check), then what is "
             "worth knowing. Use only what the briefing says and never invent a number or a time. Plain text: no "
             "lists, no markdown, no greeting.",
             team);
    char *a = in ? assist_complete(sys, in, 60000, why, sizeof why) : NULL;
    free(in);
    pthread_mutex_lock(&g_lock);
    free(SM.text);
    SM.text = a ? a : strdup(why[0] ? why : "no answer");
    SM.state = a ? 2 : 3;
    SM.gen++;
    pthread_mutex_unlock(&g_lock);
    printf("intel: the briefing's summary %s\n", a ? "came back" : why);
}

bool intel_summarize(const char *briefing, int team, int yday, char *why, size_t wn)
{
    pthread_mutex_lock(&g_lock);
    if (SM.day != yday) SM.day = yday, SM.used = 0;
    bool ok = SM.state != 1 && SM.used < SUM_PER_DAY && briefing && briefing[0];
    if (!ok && why) snprintf(why, wn, "%s", SM.state == 1 ? "already asking" : SM.used >= SUM_PER_DAY ? "today's summaries are used up" : "nothing to summarize");
    if (ok) {
        free(SM.in);
        SM.in = strdup(briefing);
        SM.team = team;
        SM.state = 1;
        SM.used++;
        SM.gen++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) return false;
    if (!assist_post_job(sum_job, NULL)) {
        pthread_mutex_lock(&g_lock);
        SM.state = 3;
        SM.used--;
        free(SM.text);
        SM.text = strdup("the assistant is busy with another job: try again in a moment");
        SM.gen++;
        pthread_mutex_unlock(&g_lock);
        if (why) snprintf(why, wn, "the assistant is busy");
        return false;
    }
    return true;
}

int intel_summary(char *out, size_t n)
{
    pthread_mutex_lock(&g_lock);
    int st = SM.state;
    if (out && n) snprintf(out, n, "%s", SM.text && st >= 2 ? SM.text : "");
    pthread_mutex_unlock(&g_lock);
    return st;
}

unsigned intel_summary_gen(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned g = SM.gen;
    pthread_mutex_unlock(&g_lock);
    return g;
}
