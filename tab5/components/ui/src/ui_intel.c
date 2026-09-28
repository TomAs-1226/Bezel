/* ui_intel — the intelligence core in the UI (intel.h does the thinking, this file the looking and saying).
 *
 *   the pass     every PASS_S on the UI thread's 10 Hz refresh (never per frame): a snapshot of everything the
 *                tablet knows (the robot, the last run, the schedule with Nexus's times, the battery pick,
 *                Statbotics, the robot's network, the weather, the tablet), the rules over it, the store's
 *                merge, and at most one island message (intel_next_say decides; the island keeps it as a
 *                notification). Timed: `assist intel` prints what a pass costs.
 *   the last run each enabled session is followed at 10 Hz for what the rules need afterwards: brownouts, the
 *                lowest voltage, and each swerve module's following of its command (speed along the commanded
 *                direction, as vectors, so a module reporting its 180-degree twin still follows).
 *   the sources  configured from here (Statbotics' team and event, the robot's address for its network) and
 *                pumped once a second: the home worker starts only when a request is due.
 *   the app      "insights": the ask bar, what the tablet noticed, the briefing (and a model's summary of it on
 *                request), the sources, and the core's settings.
 *   the ask bar  typed words are matched to what the tablet does at once (intel_intent: a timer, brightness,
 *                "next match", "battery for q34"...) and done through the same tools the AIs use
 *                (ui_actions_do); anything else goes to the technician or the companion, as chosen.
 *   the tile     the control center's "ask or do anything", with the top insight under it.
 *
 * Settings: kv "intel" = "cats,quiet_from,quiet_to,brief_hhmm,ai" and "intel_ai" = "assist" | "companion". */
#include "ui_intel.h"
#include "ui_companion.h"
#include "ui_home_mode.h"
#include "ui_sys.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "analyze.h"
#include "assist.h"
#include "cat_sc.h"
#include "home.h"
#include "tba.h"
#include "voice.h"

#define PASS_S 2.0          /* the rules' pass */
#define BRIEF_S 30.0        /* the briefing's text rebuilt */
#define AGENT_S 30.0        /* Systemcore's agent asked for one snapshot this often while connected */
#define SESSION_MIN_S 5.0   /* an enabled run shorter than this isn't a run */
#define FOLLOW_MIN_MPS 0.5  /* a module commanded slower can't be judged */
#define MAX_APPS 64

/* ------------------------------------------------------------------ state */

static struct {
    bool booted;
    intel_cfg_t cfg;
    bool ai_companion;         /* the ask bar's questions go to the companion */
    double next_1s, next_pass, next_brief, next_agent, next_slow;
    /* the sources as last copied (each only when its generation moves) */
    unsigned tba_gen, nx_gen, sb_gen, rn_gen;
    uint32_t ag_rev;
    tba_state_t *tba;          /* PSRAM */
    cat_agent_t *agent;        /* PSRAM */
    rn_state_t *rn;            /* PSRAM */
    char host[48];
    /* the last run */
    bool was_en;
    double en_at;
    int brown_n;
    bool brown_prev;
    double vmin;
    double sum_cmd[4], sum_along[4];
    int used[4], nmod;
    int session;
    double session_end;
    int brown_last;
    double vmin_last, follow_last[4];
    int mods_last;
    /* the pass */
    intel_in_t *in;            /* PSRAM */
    intel_insight_t *fresh;    /* PSRAM, INTEL_MAX */
    char *brief;               /* PSRAM, the briefing's text */
    char *brief_full;          /* PSRAM, and what the tablet noticed after it */
    char *list_text;           /* PSRAM, the insights for the tools */
    unsigned posted_gen;
    double pass_ms, pass_max_ms;
    int passes;
    char line[160];            /* ui_intel_line's */
    int line_prio;
    /* the daily briefing */
    int briefed_yday;
    const char *apps[MAX_APPS + 1];
} IX;

static void load_cfg(void)
{
    intel_cfg_default(&IX.cfg);
    char v[48];
    if (hal_kv_get("intel", v, sizeof v)) intel_cfg_parse(v, &IX.cfg);
    IX.ai_companion = hal_kv_get("intel_ai", v, sizeof v) && !strcmp(v, "companion");
}

static void save_cfg(void)
{
    char v[48];
    intel_cfg_format(&IX.cfg, v, sizeof v);
    hal_kv_set("intel", v);
    hal_kv_set("intel_ai", IX.ai_companion ? "companion" : "assist");
}

/* ------------------------------------------------------------------ the last run (10 Hz) */

static void session_tick(double now)
{
    const cat_robot_t *r = R;
    bool en = r->connected && r->enabled;
    if (en && !IX.was_en) {
        IX.en_at = now;
        IX.brown_n = 0;
        IX.brown_prev = r->browned_out;
        IX.vmin = NAN;
        memset(IX.sum_cmd, 0, sizeof IX.sum_cmd);
        memset(IX.sum_along, 0, sizeof IX.sum_along);
        memset(IX.used, 0, sizeof IX.used);
        IX.nmod = 0;
    }
    if (en) {
        if (r->have_battery && r->battery_v > 3 && (isnan(IX.vmin) || r->battery_v < IX.vmin)) IX.vmin = r->battery_v;
        if (r->browned_out && !IX.brown_prev) IX.brown_n++;
        IX.brown_prev = r->browned_out;
        if (r->have_targets && r->nmodules >= 2) {
            IX.nmod = r->nmodules > 4 ? 4 : r->nmodules;
            for (int i = 0; i < IX.nmod; i++) {
                double c = r->target_speed[i];
                if (fabs(c) < FOLLOW_MIN_MPS) continue;
                /* the actual velocity along the commanded one: v·c/|c| (a 180-degree twin is the same vector) */
                double along = r->module_speed[i] * cos(r->module_angle[i] - r->target_angle[i]) * (c < 0 ? -1 : 1);
                IX.sum_cmd[i] += fabs(c);
                IX.sum_along[i] += along;
                IX.used[i]++;
            }
        }
    }
    if (!en && IX.was_en && now - IX.en_at >= SESSION_MIN_S) {
        IX.session++;
        IX.session_end = now;
        IX.brown_last = IX.brown_n;
        IX.vmin_last = IX.vmin;
        IX.mods_last = IX.nmod;
        for (int i = 0; i < 4; i++)
            IX.follow_last[i] = i < IX.nmod && IX.used[i] >= 20 && IX.sum_cmd[i] > 0 ? 100 * IX.sum_along[i] / IX.sum_cmd[i] : NAN;
        printf("intel: run %d ended after %.0f s: %d brownouts, lowest %.2f V, following %.0f/%.0f/%.0f/%.0f %%\n",
               IX.session, now - IX.en_at, IX.brown_last, IX.vmin_last, IX.follow_last[0], IX.follow_last[1],
               IX.follow_last[2], IX.follow_last[3]);
    }
    IX.was_en = en;
}

/* ------------------------------------------------------------------ the sources (once a second) */

static void sources_tick(double now)
{
    intel_pump(now);
    /* the robot's network: its address while connected */
    char host[48] = "";
    if (R->connected) {
        char url[96];
        if (cat_agent_url(url, sizeof url, "")) {
            const char *h = strstr(url, "://");
            snprintf(host, sizeof host, "%s", h ? h + 3 : url);
            host[strcspn(host, ":/")] = 0;
        }
    }
    char cams[160] = "";
    for (int i = 0; i < R->ncameras && strlen(cams) < 120; i++) {
        size_t l = strlen(cams);
        snprintf(cams + l, sizeof cams - l, "%s%s", l ? "," : "", R->cameras[i].name);
    }
    rn_config(host, cams);
    sb_config(S.team);
    /* one snapshot from Systemcore's agent now and then, for the rules (its screens want it more often) */
    if (R->connected && R->line2 && now >= IX.next_agent) {
        IX.next_agent = now + AGENT_S;
        cat_agent_want();
    }
}

/* ------------------------------------------------------------------ the snapshot */

static void copy_tba(void)
{
    unsigned g = tba_gen();
    if (g == IX.tba_gen || !IX.tba) return;
    IX.tba_gen = g;
    tba_get(IX.tba);
    const tba_state_t *s = IX.tba;
    sb_follow(s->phase == TBA_READY && s->have_event ? s->event_key : "", s->live);
}

/* The calendar lives on the card: read on the assistant's worker every CAL_S (a job between its turns, the
 * calendar tool's own reader), never on this thread. The next event of today and tomorrow is kept. */
#define CAL_S 600.0
static pthread_mutex_t g_cal_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char text[80];             /* "18:00 team meeting", "" none */
    time_t at;
    bool busy;
    double next;
} CAL;

static void cal_job(void *u)
{
    (void)u;
    as_action_fn act = assist_action_handler();
    char *out = malloc(4096);
    char best[80] = "";
    time_t best_at = 0, now = time(NULL);
    if (act && out && act("calendar", "{\"action\":\"list\",\"days\":2}", AS_POOL_ASSIST, out, 4096)) {
        /* "events from ...:\n2026-09-28 18:00 team meeting\n..." */
        for (char *l = strchr(out, '\n'); l; l = strchr(l + 1, '\n')) {
            int y, mo, d, hh, mi, used = 0;
            if (sscanf(l + 1, "%4d-%2d-%2d %2d:%2d %n", &y, &mo, &d, &hh, &mi, &used) != 5 || !used) continue;
            struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = hh, .tm_min = mi, .tm_isdst = -1 };
            time_t at = mktime(&tm);
            if (at <= now || (best_at && at >= best_at)) continue;
            best_at = at;
            const char *text = l + 1 + used;
            snprintf(best, sizeof best, "%d:%02d %.*s", hh, mi, (int)strcspn(text, "\n"), text);
        }
    }
    free(out);
    pthread_mutex_lock(&g_cal_lock);
    snprintf(CAL.text, sizeof CAL.text, "%s", best);
    CAL.at = best_at;
    CAL.busy = false;
    pthread_mutex_unlock(&g_cal_lock);
}

static void cal_tick(double now)
{
    if (now < CAL.next) return;
    pthread_mutex_lock(&g_cal_lock);
    bool busy = CAL.busy;
    if (!busy) CAL.busy = true;
    pthread_mutex_unlock(&g_cal_lock);
    if (busy) return;
    if (assist_post_job(cal_job, NULL)) CAL.next = now + CAL_S;
    else {
        pthread_mutex_lock(&g_cal_lock);
        CAL.busy = false; /* the worker is busy with another job: again in a minute */
        pthread_mutex_unlock(&g_cal_lock);
        CAL.next = now + 60;
    }
}

/* the first alarm that is on, as the clock lists it ("06:30 weekdays"); in memory, cheap */
static char g_alarm[24];
static void alarm_line(void)
{
    char out[400];
    g_alarm[0] = 0;
    if (!ui_alarm_do("list", 0, 0, 0, out, sizeof out)) return;
    for (char *l = out; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len > 5 && !strncmp(l + len - 4, ", on", 4)) {
            const char *dot = strstr(l, ". ");
            if (dot && dot < l + len) {
                snprintf(g_alarm, sizeof g_alarm, "%.*s", (int)(l + len - 4 - (dot + 2)), dot + 2);
                return;
            }
        }
    }
}

static void fill(intel_in_t *in, double now)
{
    memset(in, 0, sizeof *in);
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    in->now = tm.tm_year > 120 ? wall : 0;
    in->mono = now;
    in->team = S.team;
    /* the event */
    const tba_state_t *s = IX.tba;
    if (s && s->phase == TBA_READY && s->have_event) {
        in->event_live = s->live;
        snprintf(in->event_key, sizeof in->event_key, "%s", s->event_key);
        snprintf(in->event_name, sizeof in->event_name, "%s", s->event_name);
        if (s->have_status) {
            in->rank = s->rank;
            in->num_teams = s->num_teams;
            in->wins = s->wins;
            in->losses = s->losses;
            in->ties = s->ties;
        }
    }
    in->nup = ui_match_upcoming(in->up, 6, &in->behind_min);
    if (in->nup > 0) {
        char bl[160];
        if (ui_batt_alarm_line(in->up[0].label, bl, sizeof bl)) {
            if (!strncmp(bl, "battery in:", 11)) snprintf(in->batt_pick, sizeof in->batt_pick, "%.23s", bl + 12);
            else snprintf(in->batt_line, sizeof in->batt_line, "%.99s", bl);
        }
    }
    /* Nexus */
    nx_state_t *nx = malloc(sizeof *nx);
    if (nx) {
        nx_get(nx);
        in->nexus = nx->phase;
        in->nexus_key = nx->phase != NX_OFF;
        if (nx->phase == NX_READY && nx_age_s() < 600) {
            snprintf(in->now_queuing, sizeof in->now_queuing, "%s", nx->now_label[0] ? nx->now_label : nx->now_queuing);
            if (nx->nann) {
                snprintf(in->ann, sizeof in->ann, "%s", nx->ann[0].text);
                in->ann_at = nx->ann[0].at;
            }
            if (nx->nparts) {
                snprintf(in->part, sizeof in->part, "%s", nx->parts[0].text);
                in->part_team = nx->parts[0].team;
                in->part_at = nx->parts[0].at;
            }
        }
        free(nx);
    }
    /* Statbotics */
    sb_state_t *sb = malloc(sizeof *sb);
    if (sb) {
        sb_get(sb);
        in->have_epa = sb->have_year && !isnan(sb->epa);
        in->epa = sb->epa;
        in->percentile = sb->percentile;
        in->epa_rank = sb->rank;
        in->epa_count = sb->team_count;
        free(sb);
    }
    /* the robot */
    const cat_robot_t *r = R;
    in->connected = r->connected;
    in->enabled = r->enabled;
    in->session = IX.session;
    in->session_end = IX.session_end;
    in->battery_v = r->have_battery ? r->battery_v : NAN;
    in->brownouts_last = IX.session ? IX.brown_last : -1;
    in->vmin_last = IX.session ? IX.vmin_last : NAN;
    in->mods = IX.session ? IX.mods_last : 0;
    for (int i = 0; i < 4; i++) in->follow_pct[i] = IX.session ? IX.follow_last[i] : NAN;
    in->errors = r->n_errors;
    in->warnings = r->n_warnings;
    for (int i = 0; i < r->nalerts; i++)
        if (r->alerts[i].sev == CAT_SEV_ERROR) {
            snprintf(in->worst_alert, sizeof in->worst_alert, "%s%s%s", r->alerts[i].source,
                     r->alerts[i].source[0] ? " " : "", r->alerts[i].text);
            break;
        }
    in->can_down = r->connected && r->can_down;
    if (r->have_roster && r->motors_expected > 0) {
        in->devices_total = r->motors_expected;
        in->devices_missing = r->motors_expected - r->motors_connected;
        if (in->devices_missing < 0) in->devices_missing = 0;
    }
    in->loop_over = r->have_loop && r->loop_over;
    in->loop_max_ms = r->have_loop ? r->loop_max_ms : NAN;
    snprintf(in->code_sha, sizeof in->code_sha, "%s", r->build_commit[0] ? r->build_commit : r->git_sha);
    in->code_dirty = r->build_dirty;
    /* Systemcore's agent, when it has answered lately */
    if (IX.agent) cat_agent_get(IX.agent, &IX.ag_rev); /* copies only when newer */
    if (IX.agent && IX.agent->valid && r->connected) {
        cat_ag_status_t st;
        cat_agent_status(&st);
        if (st.state == CAT_AG_OK && st.age_s < 120) {
            const cat_agent_t *a = IX.agent;
            in->have_agent = true;
            in->throttled = a->have_throttle && (a->thr_now || a->thr_capped_now || a->thr_under_now);
            in->core_temp = NAN;
            for (int i = 0; i < a->nzones; i++)
                if (isnan(in->core_temp) || a->zones[i].celsius > in->core_temp) in->core_temp = a->zones[i].celsius;
            in->disk_free_pct = NAN;
            for (int i = 0; i < a->nmounts; i++)
                if (!strcmp(a->mounts[i].mount, "/") && a->mounts[i].total > 0)
                    in->disk_free_pct = 100 * a->mounts[i].free / a->mounts[i].total;
        }
    }
    if (!in->have_agent) in->core_temp = in->disk_free_pct = NAN;
    /* cameras: the robot's own vision state, then the network's */
    in->cam_hot = NAN;
    for (int i = 0; r->connected && i < r->ncameras; i++) {
        const cat_camera_t *c = &r->cameras[i];
        bool bad = !c->connected || !strcmp(c->state, "DISCONNECTED") || !strcmp(c->state, "STALE") ||
                   !strcmp(c->state, "LOW_FPS");
        if (bad && !in->cams_bad++)
            snprintf(in->cam_bad, sizeof in->cam_bad, "%.30s: %s", c->name, c->connected ? c->state : "disconnected");
        if (!isnan(c->temp_c) && (isnan(in->cam_hot) || c->temp_c > in->cam_hot)) {
            in->cam_hot = c->temp_c;
            snprintf(in->cam_hot_name, sizeof in->cam_hot_name, "%s", c->name);
        }
    }
    unsigned rg = rn_gen();
    if (IX.rn && rg != IX.rn_gen) {
        IX.rn_gen = rg;
        rn_get(IX.rn);
    }
    if (IX.rn && IX.rn->host_known && r->connected) {
        const rn_state_t *n = IX.rn;
        for (int i = 0; i < n->ncams; i++) {
            const rn_cam_t *c = &n->cams[i];
            if (n->agg == 1 && !c->nt && !in->cams_bad++)
                snprintf(in->cam_bad, sizeof in->cam_bad, "%.30s: no NT session", c->name);
            if (c->answered && !isnan(c->temp) && (isnan(in->cam_hot) || c->temp > in->cam_hot)) {
                in->cam_hot = c->temp;
                snprintf(in->cam_hot_name, sizeof in->cam_hot_name, "%s", c->name);
            }
        }
        /* one model on more than one firmware */
        for (int i = 0; n->phoenix == 1 && i < n->ndevs && !in->fw_mixed; i++) {
            int same = 0, other = 0;
            const char *fw2 = NULL;
            for (int k = 0; k < n->ndevs; k++) {
                if (strcmp(n->devs[k].model, n->devs[i].model)) continue;
                if (!strcmp(n->devs[k].firmware, n->devs[i].firmware)) same++;
                else other++, fw2 = n->devs[k].firmware;
            }
            if (other && fw2) {
                in->fw_mixed = 1;
                snprintf(in->fw_line, sizeof in->fw_line, "%.18s on two firmwares: %.14s x%d, %.14s x%d", n->devs[i].model,
                         n->devs[i].firmware, same, fw2, other);
            }
        }
    }
    /* the day */
    pthread_mutex_lock(&g_cal_lock);
    snprintf(in->next_event, sizeof in->next_event, "%s", CAL.text);
    in->next_event_at = CAL.at;
    pthread_mutex_unlock(&g_cal_lock);
    snprintf(in->next_alarm, sizeof in->next_alarm, "%s", g_alarm);
    home_weather_t w;
    home_weather_get(&w);
    in->rain_hour = -1;
    if (w.ok) {
        in->have_weather = true;
        in->temp = w.temp;
        in->hi = w.hi;
        in->lo = w.lo;
        snprintf(in->weather_word, sizeof in->weather_word, "%s", home_weather_text(w.code));
        home_forecast_t *f = malloc(sizeof *f);
        if (f) {
            home_weather_forecast(f);
            for (int i = 0; f->ok && i < f->nhours && i < 6; i++)
                if (f->hour[i].pop >= 40) {
                    in->rain_hour = f->hour[i].hour;
                    in->rain_pct = f->hour[i].pop;
                    break;
                }
            free(f);
        }
    }
    hal_battery_t b;
    memset(&b, 0, sizeof b);
    in->tablet_pct = hal_battery(&b) && b.ok ? b.percent : -1;
    in->tablet_charging = b.charging || b.external;
}

/* the insights as text, for the tools and every question's context */
static void post_text(void)
{
    intel_insight_t *l = IX.fresh; /* reused as scratch: the pass is done with it */
    int n = intel_list(l, INTEL_MAX);
    size_t cap = 4096;
    char *t = IX.list_text;
    if (!t) return;
    t[0] = 0;
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (l[i].dismissed) continue;
        size_t o = strlen(t);
        snprintf(t + o, cap - o, "%sprio %d \xc2\xb7 %s (%s)", shown++ ? "\n" : "", l[i].prio, l[i].text, l[i].why);
    }
    intel_post_text(t, IX.brief_full);
    /* the top one in every question's context line */
    char ctx[200] = "";
    for (int i = 0; i < n; i++)
        if (!l[i].dismissed && l[i].prio >= 2) {
            snprintf(ctx, sizeof ctx, "noticed: %.180s", l[i].text);
            break;
        }
    assist_desk_post(AS_DESK_INTEL, ctx);
}

static const char *cat_icon(int cat)
{
    switch (cat) {
    case IC_MATCH: return BZ_I_STADIUM;
    case IC_ROBOT: return BZ_I_SMART_TOY;
    case IC_POWER: return BZ_I_BATTERY_ALERT;
    case IC_EVENT: return BZ_I_LEADERBOARD;
    case IC_DAY: return BZ_I_CALENDAR_MONTH;
    case IC_SERVICE: return BZ_I_LINK_OFF;
    default: return BZ_I_INSIGHTS;
    }
}

static void pass(double now)
{
    if (!IX.in || !IX.fresh) return;
    double t0 = hal_seconds();
    copy_tba();
    fill(IX.in, now);
    int n = intel_rules(IX.in, IX.fresh, INTEL_MAX);
    for (int i = 0; i < n; i++) IX.fresh[i].wall = IX.in->now;
    intel_merge(IX.fresh, n, now);
    /* the island: at most one, as the store allows */
    struct tm tm;
    time_t wall = time(NULL);
    localtime_r(&wall, &tm);
    intel_insight_t say;
    if (!ui_alarm_up() && intel_next_say(&IX.cfg, tm.tm_year > 120 ? tm.tm_hour : -1, now, &say)) {
        ui_island_say(cat_icon(say.cat), say.text);
        printf("intel: said \"%s\" (prio %d, %s)\n", say.text, say.prio, say.key);
    }
    /* ui_intel_line's */
    intel_insight_t top;
    IX.line[0] = 0;
    IX.line_prio = 0;
    if (intel_top(&top) && (top.prio >= 2 || !strncmp(top.key, "prep:", 5))) {
        snprintf(IX.line, sizeof IX.line, "%s", top.text);
        IX.line_prio = top.prio;
    }
    if (intel_gen() != IX.posted_gen) {
        IX.posted_gen = intel_gen();
        post_text();
    }
    double ms = (hal_seconds() - t0) * 1000;
    IX.pass_ms = ms;
    if (ms > IX.pass_max_ms) IX.pass_max_ms = ms;
    IX.passes++;
}

/* The briefing: the facts (IX.brief, what the app shows beside the list), and the same with what the tablet
 * noticed after them (IX.brief_full, for the AIs' tool and the model's summary, which have no list beside) */
static void make_brief(void)
{
    if (!IX.in || !IX.brief || !IX.brief_full) return;
    intel_briefing(IX.in, IX.brief, 3072);
    snprintf(IX.brief_full, 3584, "%s", IX.brief);
    intel_insight_t *l = IX.fresh;
    int n = intel_list(l, INTEL_MAX), shown = 0;
    for (int i = 0; i < n && shown < 4; i++)
        if (!l[i].dismissed && l[i].prio >= 2) {
            size_t o = strlen(IX.brief_full);
            snprintf(IX.brief_full + o, 3584 - o, "%s%s", shown++ ? "\n  " : "\nnoticed: ", l[i].text);
        }
    intel_post_text(IX.list_text, IX.brief_full);
}

/* the daily briefing: at the set time, or on an event day's morning, once a day */
static void brief_due(void)
{
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    if (tm.tm_year < 120 || IX.briefed_yday == tm.tm_yday + 1) return;
    int hhmm = tm.tm_hour * 100 + tm.tm_min;
    bool at_time = IX.cfg.brief_hhmm >= 0 && hhmm >= IX.cfg.brief_hhmm && hhmm < IX.cfg.brief_hhmm + 100;
    bool event_morning = IX.in && IX.in->event_live && tm.tm_hour >= 6 && tm.tm_hour < 11;
    if (!at_time && !event_morning) return;
    IX.briefed_yday = tm.tm_yday + 1;
    make_brief();
    ui_island_say(BZ_I_ARTICLE, event_morning ? "the event day's briefing is ready: insights" : "your briefing is ready: insights");
    if (IX.cfg.ai) intel_summarize(IX.brief_full, S.team, tm.tm_yday, NULL, 0);
    printf("intel: the daily briefing\n");
}

static void tick(void *u)
{
    (void)u;
    double now = hal_seconds();
    session_tick(now);
    if (now >= IX.next_1s) {
        IX.next_1s = now + 1;
        sources_tick(now);
        cal_tick(now);
    }
    /* not while a finger moves something or a page slides: a pass is under a millisecond, but a frame in a
     * gesture has none to spare, and the pass can wait half a second */
    if (now >= IX.next_pass && !bz_drag_active() && !bz_ui_sliding() && !bz_ui_sheeting()) {
        IX.next_pass = now + PASS_S;
        pass(now);
    }
    if (now >= IX.next_brief) {
        IX.next_brief = now + BRIEF_S;
        alarm_line();
        make_brief();
        brief_due();
    }
}

bool ui_intel_line(char *out, size_t n)
{
    if (!IX.line[0]) return false;
    snprintf(out, n, "%s", IX.line);
    return true;
}

/* ------------------------------------------------------------------ the ask bar */

static struct {
    lv_obj_t *ask, *ask_text, *result, *to[2];
    lv_obj_t *chips[4], *views[4];
    lv_obj_t *list, *list_empty, *brief, *summary, *sum_btn, *sources_text, *brief_col;
    lv_obj_t *set_col;
    ui_kb_t *kb;
    int view;
    bool want_kb;
    int want_view;             /* a view asked for before the app opened, + 1 */
    unsigned list_gen, sum_gen;
    char brief_shown[3072];
    double src_next;
    /* settings */
    lv_obj_t *cat_chips[IC_N], *quiet_chips[4], *brief_chips[6], *ai_chip, *set_state;
} AP;

static void result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void result(const char *fmt, ...)
{
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (AP.result) ui_text(AP.result, "%s", b);
    printf("intel: ask -> %s\n", b);
}

/* one of the tablet's own tools, as a model would run it; the answer's first line on the result line */
static bool tool(const char *name, const char *json)
{
    char *out = malloc(2048);
    if (!out) return false;
    bool ok = ui_actions_do(name, json, AS_POOL_ASSIST, out, 2048);
    out[strcspn(out, "\n")] = 0;
    result("%s", out[0] ? out : ok ? "done" : "that didn't work");
    free(out);
    return ok;
}

static void answer_next(void)
{
    intel_up_t up[2];
    int behind = 0, n = ui_match_upcoming(up, 2, &behind);
    if (!n) {
        char nx[120];
        if (ui_match_next_line(nx, sizeof nx)) result("%s", nx);
        else result("no match of ours to come (the blue alliance has none scheduled)");
        return;
    }
    char hm[8] = "?", q[8];
    struct tm tm;
    localtime_r(&up[0].start, &tm);
    snprintf(hm, sizeof hm, "%d:%02d", tm.tm_hour, tm.tm_min);
    char line[200];
    snprintf(line, sizeof line, "%s ~%s (%s) \xc2\xb7 %s", up[0].label, hm, mt_src_name(up[0].src),
             up[0].ours == 1 ? "red" : up[0].ours == 2 ? "blue" : "");
    for (int i = 0; i < 2; i++)
        if (up[0].partners[i]) snprintf(line + strlen(line), sizeof line - strlen(line), "%s%d", i ? ", " : " with ", up[0].partners[i]);
    if (up[0].queue_at) {
        localtime_r(&up[0].queue_at, &tm);
        snprintf(q, sizeof q, "%d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 queue ~%s", q);
    }
    if (!isnan(up[0].win)) snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 %.0f%% to win", up[0].win * 100);
    result("%s", line);
}

static void answer_battery(const char *label)
{
    char l[160];
    intel_up_t up[1];
    int b = 0;
    const char *m = label && label[0] ? label : ui_match_upcoming(up, 1, &b) ? up[0].label : "";
    if (ui_batt_alarm_line(m, l, sizeof l)) result("%s%s%s", m[0] ? m : "", m[0] ? ": " : "", l);
    else result("the battery manager has no pick yet (batteries app)");
}

static void answer_queue(void)
{
    nx_state_t *nx = malloc(sizeof *nx);
    if (!nx) return;
    nx_get(nx);
    if (nx->phase == NX_OFF) result("no live queue: NEXUS_API_KEY isn't in KEYS.ENV");
    else if (nx->phase != NX_READY) result("nexus: %s", nx->err[0] ? nx->err : "no answer yet");
    else {
        char line[200];
        snprintf(line, sizeof line, "now queuing %s", nx->now_queuing[0] ? nx->now_queuing : "nothing");
        time_t now = time(NULL);
        for (int i = 0; i < nx->nours; i++) {
            const nx_match_t *x = &nx->ours[i];
            if (x->started) continue;
            snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 ours: %s %s", x->label,
                     x->status[0] ? x->status : "");
            if (x->queue_at > now)
                snprintf(line + strlen(line), sizeof line - strlen(line), " queues in %ld min", (long)(x->queue_at - now + 59) / 60);
            if (nx->now_index >= 0 && x->index >= nx->now_index)
                snprintf(line + strlen(line), sizeof line - strlen(line), " (%d away)", x->index - nx->now_index);
            break;
        }
        result("%s", line);
    }
    free(nx);
}

static void answer_epa(void)
{
    sb_state_t *sb = malloc(sizeof *sb);
    if (!sb) return;
    sb_get(sb);
    if (sb->have_year && !isnan(sb->epa)) {
        char line[200];
        snprintf(line, sizeof line, "EPA %.1f", sb->epa);
        if (sb->rank > 0) snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 #%d of %d", sb->rank, sb->team_count);
        if (!isnan(sb->percentile))
            snprintf(line + strlen(line), sizeof line - strlen(line), " (top %.0f%%)", fmax(1, (1 - sb->percentile) * 100));
        if (sb->have_event) snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 %.1f at %s", sb->event_epa, sb->event_key);
        if (IX.in && IX.in->rank > 0) snprintf(line + strlen(line), sizeof line - strlen(line), " \xc2\xb7 rank %d of %d", IX.in->rank, IX.in->num_teams);
        result("%s (statbotics)", line);
    } else {
        result("statbotics: %s", sb->err[0] ? sb->err : "no numbers yet");
    }
    free(sb);
}

static void show_view(int v);

static void ask_ai(const char *q)
{
    if (IX.ai_companion) {
        result("asked the companion");
        ui_companion_open(false, NULL);
        voice_ask(q);
    } else {
        result("asked assist");
        ui_assist_ask(q, NULL);
    }
}

static void do_ask(const char *text)
{
    while (*text == ' ') text++;
    if (!*text) return;
    intel_intent_t it = intel_intent(text, IX.apps);
    char j[256];
    switch (it.kind) {
    case IN_OPEN:
        snprintf(j, sizeof j, "{\"app\":\"%.60s\"}", it.arg);
        tool("open_app", j);
        break;
    case IN_TIMER:
        if (it.arg[0]) snprintf(j, sizeof j, "{\"action\":\"countdown\",\"seconds\":%.0f,\"label\":\"%.24s\"}", it.seconds, it.arg);
        else snprintf(j, sizeof j, "{\"action\":\"countdown\",\"seconds\":%.0f}", it.seconds);
        tool("timer", j);
        break;
    case IN_STOPWATCH: tool("timer", "{\"action\":\"stopwatch\"}"); break;
    case IN_BRIGHT: {
        int v = it.delta ? (int)lroundf(S.brightness * 100) + it.value : it.value;
        snprintf(j, sizeof j, "{\"brightness\":%d}", v < 3 ? 3 : v > 100 ? 100 : v);
        tool("set_tablet", j);
        break;
    }
    case IN_VOLUME: {
        int v = it.delta ? (int)lroundf(S.volume * 100) + it.value : it.value;
        snprintf(j, sizeof j, "{\"volume\":%d}", v < 0 ? 0 : v > 100 ? 100 : v);
        tool("set_tablet", j);
        break;
    }
    case IN_MUTE: tool("set_tablet", it.value ? "{\"volume\":0}" : "{\"volume\":50}"); break;
    case IN_TONE:
        snprintf(j, sizeof j, "{\"tone\":\"%s\"}", it.arg);
        tool("set_tablet", j);
        break;
    case IN_TORCH: tool("set_tablet", "{\"flashlight\":true}"); break;
    case IN_HOME: tool("set_tablet", "{\"home_mode\":true}"); break;
    case IN_SLEEP:
        snprintf(j, sizeof j, "{\"screen\":\"%s\"}", it.arg);
        tool("set_tablet", j);
        break;
    case IN_NEXT: answer_next(); break;
    case IN_BATTERY: answer_battery(it.arg); break;
    case IN_QUEUE: answer_queue(); break;
    case IN_EPA: answer_epa(); break;
    case IN_BRIEF:
        make_brief();
        show_view(1);
        result("the briefing, as of now");
        break;
    case IN_INSIGHTS:
        show_view(0);
        result("what the tablet noticed");
        break;
    case IN_WEATHER: {
        home_weather_t w;
        home_weather_get(&w);
        if (w.ok) result("%s: %.0f\xc2\xb0 %s, %.0f-%.0f\xc2\xb0", w.place, w.temp, home_weather_text(w.code), w.lo, w.hi);
        else tool("open_app", "{\"app\":\"weather\"}");
        break;
    }
    case IN_NOTE: {
        char esc[200];
        size_t o = 0;
        for (const char *p = it.arg; *p && o + 2 < sizeof esc; p++) {
            if (*p == '"' || *p == '\\') esc[o++] = '\\';
            esc[o++] = *p;
        }
        esc[o] = 0;
        snprintf(j, sizeof j, "{\"action\":\"append\",\"text\":\"%s\"}", esc);
        tool("notes", j);
        break;
    }
    case IN_REMIND: {
        char esc[100];
        size_t o = 0;
        for (const char *p = it.arg; *p && o + 2 < sizeof esc && o < 88; p++) {
            if (*p == '"' || *p == '\\') esc[o++] = '\\';
            esc[o++] = *p;
        }
        esc[o] = 0;
        if (it.seconds > 0) snprintf(j, sizeof j, "{\"action\":\"add\",\"text\":\"%s\",\"minutes\":%.1f}", esc, it.seconds / 60.0 < 0.5 ? 0.5 : it.seconds / 60.0);
        else snprintf(j, sizeof j, "{\"action\":\"add\",\"text\":\"%s\",\"at\":\"%d:%02d\"}", esc, it.value / 100, it.value % 100);
        tool("reminder", j);
        break;
    }
    case IN_ALARM:
        snprintf(j, sizeof j, "{\"action\":\"add\",\"time\":\"%d:%02d\"}", it.value / 100, it.value % 100);
        tool("alarm", j);
        break;
    case IN_ROBOT: ask_ai("Run triage on the robot and tell me what's wrong, worst first, in a few lines."); break;
    default: ask_ai(text); break;
    }
}

static void typed(const char *text, void *u)
{
    (void)u;
    do_ask(text);
}

static void ask_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (AP.kb) ui_kb_show(AP.kb, "ask or do anything: \"timer 5 min\", \"next match\", or a question", "", false, true, typed, NULL);
}

static void to_tap(lv_obj_t *o, void *u)
{
    (void)o;
    IX.ai_companion = (intptr_t)u != 0;
    ui_chip_set(AP.to[0], !IX.ai_companion);
    ui_chip_set(AP.to[1], IX.ai_companion);
    save_cfg();
}

/* ------------------------------------------------------------------ the app */

#define APP_Y 104
#define ASK_H 72
#define RES_Y (APP_Y + ASK_H + 10)
#define BODY2_Y (RES_Y + 40)
#define BODY2_H (H - BODY2_Y - PAD)
#define LW2 (PORTRAIT ? W - 2 * PAD : 600)
#define IN_W(w) ((w) - 2 * BZ_PAD_TILE)

static void view_tap(lv_obj_t *o, void *u)
{
    (void)o;
    show_view((int)(intptr_t)u);
}

static void row_tap(lv_obj_t *o, void *u)
{
    const char *app = (const char *)u;
    if (!app || !app[0]) return;
    const ui_app_t *a = ui_app_find(app);
    if (a) ui_app_open(a, o);
}

static void row_x(lv_obj_t *o, void *u)
{
    (void)o;
    intel_dismiss((const char *)u);
}

static void list_build(void)
{
    lv_obj_clean(AP.list);
    static intel_insight_t rows[INTEL_MAX]; /* static: the keys and apps the rows point at live here */
    int n = intel_list(rows, INTEL_MAX), shown = 0;
    int w = LW2;
    for (int i = 0; i < n; i++) {
        const intel_insight_t *x = &rows[i];
        if (x->dismissed) continue;
        lv_obj_t *t = bz_tile(AP.list, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(t, 18, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, 14, 0);
        bz_mark(t, x->prio >= 3 ? BZ_FAULT : x->prio == 2 ? BZ_WARN : BZ_INFO, 14);
        lv_obj_t *c = bz_col(t, 4);
        lv_obj_set_width(c, IN_W(w) - 14 - 14 - 48 - 14);
        lv_obj_t *l = bz_label(c, x->text, BZ_F_BODY, BZ_C_INK);
        lv_obj_set_width(l, IN_W(w) - 14 - 14 - 48 - 14);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        l = bz_label(c, x->why, BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(l, IN_W(w) - 14 - 14 - 48 - 14);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_t *xb = bz_box(t);
        lv_obj_set_size(xb, 48, 48);
        lv_obj_set_style_radius(xb, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_style(xb, bz_style_fill(BZ_C_SURFACE2), 0);
        lv_obj_center(bz_icon(xb, BZ_I_CLOSE, 24, BZ_C_DIM));
        lv_obj_set_ext_click_area(xb, 10);
        bz_on_tap(xb, row_x, (void *)x->key);
        if (x->app[0]) bz_on_tap(t, row_tap, (void *)x->app);
        shown++;
    }
    if (!shown) ui_empty_row(AP.list, w, BZ_I_INSIGHTS, "nothing needs you", "what the tablet notices shows here: the queue, the "
                                                                              "battery, the robot's last run, the day");
}

static void settings_show(void);

static void show_view(int v)
{
    AP.view = v;
    for (int i = 0; i < 4; i++) {
        if (!AP.chips[i]) continue;
        ui_chip_set(AP.chips[i], i == v);
        bool on = i == v || (v == 0 && i == 1 && !PORTRAIT);
        if (AP.views[i]) {
            if (on) lv_obj_remove_flag(AP.views[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(AP.views[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (v == 3) settings_show();
    AP.src_next = 0;
}

static void sum_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    make_brief();
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    char why[96];
    if (intel_summarize(IX.brief_full, S.team, tm.tm_yday, why, sizeof why)) result("asking for a summary");
    else result("no summary: %s", why);
}

static void brief_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    nx_refresh();
    sb_refresh();
    rn_refresh();
    make_brief();
    result("asked the sources again");
}

static void intel_build(lv_obj_t *b)
{
    int n = 0;
    for (int i = 0; i < ui_app_count() && n < MAX_APPS; i++) IX.apps[n++] = ui_app_entry(i)->label;
    IX.apps[n] = NULL;
    /* the head: the views */
    lv_obj_t *hr = bz_row(b, 12);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(hr, 60);
    static const char *const V[4] = { "now", "briefing", "sources", "settings" };
    for (int i = 0; i < 4; i++) {
        if (i == 1 && !PORTRAIT) {
            AP.chips[i] = NULL; /* landscape shows the briefing beside the insights */
            continue;
        }
        AP.chips[i] = ui_chip(hr, V[i], view_tap, (void *)(intptr_t)i);
    }
    /* the ask bar: a wide pill, and who answers questions */
    int tow = 360;
    AP.ask = bz_tile(b, W - 2 * PAD - tow - BZ_GAP, ASK_H);
    lv_obj_set_pos(AP.ask, PAD, APP_Y);
    bz_tile_set_fill(AP.ask, BZ_C_SURFACE2);
    lv_obj_set_style_radius(AP.ask, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_ver(AP.ask, 0, 0);
    lv_obj_set_flex_flow(AP.ask, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(AP.ask, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(AP.ask, 14, 0);
    bz_icon(AP.ask, BZ_I_AUTO_AWESOME, 28, BZ_C_ICE);
    AP.ask_text = bz_label_line(AP.ask, "ask or do anything", BZ_F_BODY, BZ_C_DIM, W - 2 * PAD - tow - 160);
    bz_on_tap(AP.ask, ask_tap, NULL);
    lv_obj_t *tr = bz_row(b, 10);
    lv_obj_set_pos(tr, W - PAD - tow, APP_Y + (ASK_H - 56) / 2);
    char her[32] = "companion", key[16], name[64];
    if (assist_mem_self(AS_POOL_COMPANION, 0, key, sizeof key, name, sizeof name) && name[0]) snprintf(her, sizeof her, "%.20s", name);
    AP.to[0] = ui_chip(tr, "assist", to_tap, (void *)0);
    AP.to[1] = ui_chip(tr, her, to_tap, (void *)1);
    ui_chip_set(AP.to[0], !IX.ai_companion);
    ui_chip_set(AP.to[1], IX.ai_companion);
    AP.result = bz_label_line(b, "timers, brightness, the next match, a battery, the queue, a note; anything else goes to the ai",
                              BZ_F_LABEL, BZ_C_DIM, W - 2 * PAD - 20);
    lv_obj_set_pos(AP.result, PAD + 20, RES_Y + 6);

    /* now: what the tablet noticed */
    AP.views[0] = bz_box(b);
    lv_obj_set_pos(AP.views[0], PAD, BODY2_Y);
    lv_obj_set_size(AP.views[0], LW2, BODY2_H);
    AP.list = ui_scroller(AP.views[0], LW2, BODY2_H);
    /* the briefing: beside it in landscape */
    int bw = PORTRAIT ? W - 2 * PAD : W - 2 * PAD - LW2 - BZ_GAP;
    AP.views[1] = bz_tile(b, bw, BODY2_H);
    lv_obj_set_pos(AP.views[1], PORTRAIT ? PAD : PAD + LW2 + BZ_GAP, BODY2_Y);
    lv_obj_set_flex_flow(AP.views[1], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(AP.views[1], 10, 0);
    lv_obj_t *bh = bz_row(AP.views[1], 10);
    lv_obj_set_width(bh, IN_W(bw));
    lv_obj_t *bl = bz_label(bh, "briefing", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_flex_grow(bl, 1);
    ui_button(bh, BZ_I_REFRESH, NULL, brief_tap, NULL);
    AP.sum_btn = ui_button(bh, BZ_I_AUTO_AWESOME, "summarize", sum_tap, NULL);
    lv_obj_t *bcol = ui_scroller(AP.views[1], IN_W(bw), BODY2_H - 2 * BZ_PAD_TILE - 66);
    lv_obj_set_style_pad_row(bcol, 12, 0);
    AP.summary = bz_label(bcol, "", BZ_F_BODY, BZ_C_ICE);
    lv_obj_set_width(AP.summary, IN_W(bw));
    lv_label_set_long_mode(AP.summary, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(AP.summary, LV_OBJ_FLAG_HIDDEN);
    AP.brief = bz_label(bcol, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(AP.brief, IN_W(bw));
    lv_label_set_long_mode(AP.brief, LV_LABEL_LONG_WRAP);
    /* sources */
    AP.views[2] = bz_tile(b, W - 2 * PAD, BODY2_H);
    lv_obj_set_pos(AP.views[2], PAD, BODY2_Y);
    lv_obj_t *scol = ui_scroller(AP.views[2], IN_W(W - 2 * PAD), BODY2_H - 2 * BZ_PAD_TILE);
    AP.sources_text = bz_label(scol, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(AP.sources_text, IN_W(W - 2 * PAD));
    lv_label_set_long_mode(AP.sources_text, LV_LABEL_LONG_WRAP);
    /* settings */
    AP.views[3] = bz_tile(b, W - 2 * PAD, BODY2_H);
    lv_obj_set_pos(AP.views[3], PAD, BODY2_Y);
    AP.set_col = ui_scroller(AP.views[3], IN_W(W - 2 * PAD), BODY2_H - 2 * BZ_PAD_TILE);
    ui_intel_settings(AP.set_col, IN_W(W - 2 * PAD));

    AP.kb = ui_kb_create(b, 420);
    AP.list_gen = ~0u;
    AP.sum_gen = ~0u;
    show_view(0);
}

static void intel_open(void)
{
    AP.list_gen = ~0u;
    make_brief();
    AP.brief_shown[0] = 0;
    if (AP.want_view) {
        show_view(AP.want_view - 1);
        AP.want_view = 0;
    }
    if (AP.want_kb) {
        AP.want_kb = false;
        ask_tap(NULL, NULL);
    }
}

static void intel_close(void)
{
    if (AP.kb) ui_kb_hide(AP.kb);
}

static void sources_fill(void)
{
    char *t = malloc(6144);
    char *part = malloc(4096);
    if (!t || !part) {
        free(t);
        free(part);
        return;
    }
    t[0] = 0;
    char lines[4][96];
    int k = intel_services(lines, 4);
    for (int i = 0; i < k; i++) snprintf(t + strlen(t), 6144 - strlen(t), "%s\n", lines[i]);
    /* the same detail the AIs read, without asking the network (the tools do that) */
    if (intel_tool("event_live", "{\"cached\":true}", part, 4096)) snprintf(t + strlen(t), 6144 - strlen(t), "\n%s", part);
    if (R->connected && intel_tool("robot_network", "{\"cached\":true}", part, 4096))
        snprintf(t + strlen(t), 6144 - strlen(t), "\n%s", part);
    snprintf(t + strlen(t), 6144 - strlen(t), "\nthe rules' pass: %.2f ms (worst %.2f ms over %d)", IX.pass_ms, IX.pass_max_ms, IX.passes);
    lv_label_set_text(AP.sources_text, t);
    free(t);
    free(part);
}

static void intel_refresh(void)
{
    if (AP.view == 0 && AP.list_gen != intel_gen()) {
        AP.list_gen = intel_gen();
        list_build();
    }
    if ((AP.view == 0 && !PORTRAIT) || AP.view == 1) {
        if (IX.brief && strcmp(AP.brief_shown, IX.brief)) {
            snprintf(AP.brief_shown, sizeof AP.brief_shown, "%s", IX.brief);
            lv_label_set_text(AP.brief, AP.brief_shown);
        }
        unsigned sg = intel_summary_gen();
        if (sg != AP.sum_gen) {
            AP.sum_gen = sg;
            char s[600];
            int st = intel_summary(s, sizeof s);
            if (st == 1) lv_label_set_text(AP.summary, "asking for a summary...");
            else if (st >= 2) lv_label_set_text(AP.summary, s);
            static int was;
            if (was == 1 && st == 2) result("the summary is in, above the briefing");
            else if (was == 1 && st == 3) result("no summary: %.80s", s);
            was = st;
            if (st) lv_obj_remove_flag(AP.summary, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (AP.view == 2 && hal_seconds() >= AP.src_next) {
        AP.src_next = hal_seconds() + 3;
        sources_fill();
    }
}

const ui_app_t APP_INTEL = { .name = "insights", .icon = BZ_I_INSIGHTS, .build = intel_build, .open = intel_open,
                             .close = intel_close, .refresh = intel_refresh };

void ui_intel_open_view(int view, lv_obj_t *from)
{
    AP.want_view = view + 1;
    if (ui_app_is_open(&APP_INTEL)) {
        AP.want_view = 0;
        show_view(view);
        return;
    }
    ui_app_open(&APP_INTEL, from);
}

void ui_intel_ask(lv_obj_t *from)
{
    if (ui_app_is_open(&APP_INTEL)) {
        ask_tap(NULL, NULL);
        return;
    }
    AP.want_kb = true;
    ui_app_open(&APP_INTEL, from);
}

/* ------------------------------------------------------------------ settings */

static const char *const CAT_L[IC_N] = { "matches", "the robot", "power", "the event", "the day", "sources" };
static const int QUIET_F[4] = { 0, 22, 23, 21 }, QUIET_T[4] = { 0, 7, 7, 8 };
static const char *const QUIET_L[4] = { "no quiet hours", "22-7", "23-7", "21-8" };
static const int BRIEF_T[6] = { -1, 630, 700, 730, 800, 900 };
static const char *const BRIEF_L[6] = { "off", "6:30", "7:00", "7:30", "8:00", "9:00" };

static void settings_show(void)
{
    if (!AP.ai_chip) return;
    for (int i = 0; i < IC_N; i++) ui_chip_set(AP.cat_chips[i], IX.cfg.cats & (1u << i));
    for (int i = 0; i < 4; i++)
        ui_chip_set(AP.quiet_chips[i], i ? IX.cfg.quiet_from == QUIET_F[i] && IX.cfg.quiet_to == QUIET_T[i]
                                         : IX.cfg.quiet_from == IX.cfg.quiet_to);
    for (int i = 0; i < 6; i++) ui_chip_set(AP.brief_chips[i], IX.cfg.brief_hhmm == BRIEF_T[i]);
    ui_chip_set(AP.ai_chip, IX.cfg.ai);
    char lines[4][96], t[400] = "";
    int k = intel_services(lines, 4);
    for (int i = 0; i < k; i++) snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", i ? "\n" : "", lines[i]);
    lv_label_set_text(AP.set_state, t);
}

static void st_cat(lv_obj_t *o, void *u)
{
    (void)o;
    IX.cfg.cats ^= 1u << (int)(intptr_t)u;
    save_cfg();
    settings_show();
}

static void st_quiet(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    IX.cfg.quiet_from = QUIET_F[i];
    IX.cfg.quiet_to = QUIET_T[i];
    save_cfg();
    settings_show();
}

static void st_brief(lv_obj_t *o, void *u)
{
    (void)o;
    IX.cfg.brief_hhmm = BRIEF_T[(int)(intptr_t)u];
    save_cfg();
    settings_show();
}

static void st_ai(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    IX.cfg.ai = !IX.cfg.ai;
    save_cfg();
    settings_show();
}

static void st_clear(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    intel_clear();
    result("the insights are cleared; the rules start again");
}

static lv_obj_t *wrap(lv_obj_t *col, int w)
{
    lv_obj_t *r = bz_row(col, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *caption(lv_obj_t *col, int w, const char *text)
{
    lv_obj_t *n = bz_label(col, text, BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    return n;
}

void ui_intel_settings(lv_obj_t *col, int w)
{
    lv_obj_set_style_pad_row(col, 12, 0);
    bz_label(col, "the island may say", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = wrap(col, w);
    for (int i = 0; i < IC_N; i++) AP.cat_chips[i] = ui_chip(r, CAT_L[i], st_cat, (void *)(intptr_t)i);
    caption(col, w, "What the tablet notices is always listed here; these choose what may also appear on the island (and "
                    "stay in the notifications). Only what needs doing is said, each thing once, at most one every ten "
                    "minutes unless it can't wait.");
    bz_label(col, "quiet hours", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    for (int i = 0; i < 4; i++) AP.quiet_chips[i] = ui_chip(r, QUIET_L[i], st_quiet, (void *)(intptr_t)i);
    bz_label(col, "a briefing every morning at", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    for (int i = 0; i < 6; i++) AP.brief_chips[i] = ui_chip(r, BRIEF_L[i], st_brief, (void *)(intptr_t)i);
    r = wrap(col, w);
    AP.ai_chip = ui_chip(r, "with an ai summary (openai, a few a day)", st_ai, NULL);
    ui_button(r, BZ_I_CLOSE, "clear the insights", st_clear, NULL);
    caption(col, w, "On an event day the briefing also comes by itself in the morning. The summary is the only part that "
                    "uses a model; everything else is worked out on the tablet.");
    bz_label(col, "sources", BZ_F_LABEL, BZ_C_DIM);
    AP.set_state = bz_label(col, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(AP.set_state, w);
    lv_label_set_long_mode(AP.set_state, LV_LABEL_LONG_WRAP);
    caption(col, w, "Nexus (the live queue, which times the match alarms) needs NEXUS_API_KEY in CATOS/KEYS.ENV on the card. "
                    "Statbotics needs no key. The robot's network (Systemcore's camera aggregator, each Limelight, CTRE's "
                    "Phoenix server) is asked while a robot is connected.");
}

/* ------------------------------------------------------------------ the control center's tile */

static struct {
    lv_obj_t *line;
    char shown[160];
} CT;

static void cc_open_ask(void) { ui_intel_ask(NULL); }

static void cc_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_cc_after_close(cc_open_ask);
}

static void cc_build(lv_obj_t *m, int w, int h)
{
    (void)h;
    lv_obj_set_style_radius(m, 36, 0);
    lv_obj_set_style_pad_ver(m, 14, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m, 4, 0);
    lv_obj_t *r = bz_row(m, 12);
    bz_icon(r, BZ_I_AUTO_AWESOME, 24, BZ_C_ICE);
    bz_label_line(r, "ask or do anything", BZ_F_BODY, BZ_C_INK, w - 2 * BZ_PAD_TILE - 40);
    CT.line = bz_label_line(m, "", BZ_F_CAPTION, BZ_C_DIM, w - 2 * BZ_PAD_TILE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(m, cc_tap, NULL);
}

static void cc_refresh(lv_obj_t *m)
{
    (void)m;
    char l[160];
    if (!ui_intel_line(l, sizeof l)) snprintf(l, sizeof l, "\"timer 5 min\" \xc2\xb7 \"next match\" \xc2\xb7 a question");
    if (strcmp(l, CT.shown)) {
        snprintf(CT.shown, sizeof CT.shown, "%s", l);
        ui_text(CT.line, "%s", l);
    }
}

void ui_intel_cc_add(void)
{
    static const ui_cc_control_t CTL = {
        .name = "ask", .icon = BZ_I_AUTO_AWESOME, .kind = UI_CC_TILE, .cols = 4, .rows = 1, .h = 84,
        .build = cc_build, .refresh = cc_refresh,
    };
    ui_cc_add(&CTL);
}

/* ------------------------------------------------------------------ start-up and the dev console */

void ui_intel_boot(void)
{
    if (IX.booted) return;
    IX.booted = true;
    load_cfg();
    IX.tba = calloc(1, sizeof *IX.tba);
    IX.agent = calloc(1, sizeof *IX.agent);
    IX.rn = calloc(1, sizeof *IX.rn);
    IX.in = calloc(1, sizeof *IX.in);
    IX.fresh = calloc(INTEL_MAX, sizeof *IX.fresh);
    IX.brief = calloc(1, 3072);
    IX.brief_full = calloc(1, 3584);
    IX.list_text = calloc(1, 4096);
    IX.vmin = IX.vmin_last = NAN;
    for (int i = 0; i < 4; i++) IX.follow_last[i] = NAN;
    intel_init();
    sb_config(S.team);
    double now = hal_seconds();
    IX.next_pass = now + 6; /* after the first TBA read from the card */
    CAL.next = now + 45;    /* the card's EIO spell at start-up is over by then */
    IX.next_brief = now + 8;
    ui_on_refresh(tick, NULL);
}

/* "assist nexus fake <queue in min> [status words]": a Nexus document made up around a Q99 of ours, fed through
 * the parser as if it came over the network (pair it with "match fake" for TBA's side, or not: Nexus alone
 * tracks it). Times are from now. */
static void nexus_fake(const char *a)
{
    double q = atof(a);
    if (q <= 0) q = 20;
    while (*a && *a != ' ') a++;
    while (*a == ' ') a++;
    char st[32];
    snprintf(st, sizeof st, "%s", *a ? a : "Queuing soon");
    long long now = (long long)time(NULL) * 1000;
    long long qa = now + (long long)(q * 60000), start = qa + 15 * 60000;
    int team = S.team > 0 ? S.team : 5805;
    char *j = malloc(2048);
    if (!j) return;
    snprintf(j, 2048,
             "{\"eventKey\":\"fake\",\"dataAsOfTime\":%lld,\"nowQueuing\":\"Qualification 97\",\"matches\":["
             "{\"label\":\"Qualification 97\",\"status\":\"Now queuing\",\"redTeams\":[\"111\",\"222\",\"333\"],"
             "\"blueTeams\":[\"444\",\"555\",\"666\"],\"times\":{\"estimatedQueueTime\":%lld,\"estimatedStartTime\":%lld}},"
             "{\"label\":\"Qualification 98\",\"status\":\"Queuing soon\",\"redTeams\":[\"777\",\"888\",\"999\"],"
             "\"blueTeams\":[\"1111\",\"2222\",\"3333\"],\"times\":{\"estimatedQueueTime\":%lld,\"estimatedStartTime\":%lld}},"
             "{\"label\":\"Qualification 99\",\"status\":\"%s\",\"redTeams\":[\"971\",\"1323\",\"4414\"],"
             "\"blueTeams\":[\"%d\",\"1678\",\"254\"],\"times\":{\"estimatedQueueTime\":%lld,\"estimatedOnDeckTime\":%lld,"
             "\"estimatedOnFieldTime\":%lld,\"estimatedStartTime\":%lld},\"breakAfter\":null,\"replayOf\":null}],"
             "\"announcements\":[{\"id\":\"a1\",\"announcement\":\"test: the field is running about 10 minutes behind\","
             "\"postedTime\":%lld}],"
             "\"partsRequests\":[{\"id\":\"p1\",\"parts\":\"a 1/2 in hex bearing\",\"requestedByTeam\":\"1234\",\"postedTime\":%lld}]}",
             now, now - 300000, now + 600000, now + 120000, now + 900000, st, team, qa, start - 300000, start - 120000,
             start, now - 60000, now - 120000);
    nx_inject(j);
    tba_ext_want(); /* the worker takes it on its next pass */
    printf("NEXUS fake: Q99 of ours queues in %.0f min, starts in %.0f min, status \"%s\"\n", q, q + 15, st);
    free(j);
}

static void nexus_print(void)
{
    nx_state_t *nx = malloc(sizeof *nx);
    if (!nx) return;
    nx_get(nx);
    static const char *const PH[] = { "off (no key)", "no event", "loading", "ready", "event unknown to nexus", "key refused", "error" };
    printf("NEXUS phase %s%s, event %s, age %.0f s, err \"%s\"\n", PH[nx->phase], nx->stale ? " (stale)" : "", nx->event_key,
           nx_age_s(), nx->err);
    printf("NEXUS now queuing \"%s\" (%s, index %d), %d matches, %d ours\n", nx->now_queuing, nx->now_label, nx->now_index,
           nx->nmatches, nx->nours);
    time_t now = time(NULL);
    for (int i = 0; i < nx->nours; i++) {
        const nx_match_t *m = &nx->ours[i];
        printf("NEXUS %s \"%s\" %s: queue %+lds start %+lds, status \"%s\"\n", m->label, m->name, m->ours == 1 ? "red" : "blue",
               m->queue_at ? (long)(m->queue_at - now) : 0, m->start_at ? (long)(m->start_at - now) : 0, m->status);
    }
    for (int i = 0; i < nx->nann; i++) printf("NEXUS announcement: %s\n", nx->ann[i].text);
    for (int i = 0; i < nx->nparts; i++) printf("NEXUS parts request from %d: %s\n", nx->parts[i].team, nx->parts[i].text);
    free(nx);
}

void ui_intel_dev(const char *args)
{
    /* args: "intel ..." or "nexus ..." */
    if (!strncmp(args, "nexus", 5)) {
        const char *a = args + 5;
        while (*a == ' ') a++;
        if (!strncmp(a, "fake", 4)) nexus_fake(a + 4 + (a[4] == ' '));
        else if (!strncmp(a, "base", 4)) {
            /* "assist nexus base http://192.168.0.20:8799/api/v1" points the client at a fake; "off" back */
            const char *u = a + 4;
            while (*u == ' ') u++;
            hal_kv_set("nexus_base", strcmp(u, "off") ? u : "");
            ui_match_nexus_config();
            printf("NEXUS base %s\n", strcmp(u, "off") ? u : "the real one");
        } else if (!strncmp(a, "event", 5)) {
            const char *e = a + 5;
            while (*e == ' ') e++;
            hal_kv_set("nexus_event", strcmp(e, "off") ? e : "");
            ui_match_nexus_config();
            printf("NEXUS event %s\n", strcmp(e, "off") ? e : "TBA's");
        } else if (!strcmp(a, "refresh")) {
            nx_refresh();
            tba_ext_want();
            printf("NEXUS asked again\n");
        } else {
            nexus_print();
        }
        return;
    }
    const char *a = args + 5;
    while (*a == ' ') a++;
    if (!strncmp(a, "ask ", 4)) {
        do_ask(a + 4);
    } else if (!strcmp(a, "open")) {
        ui_app_open(&APP_INTEL, NULL);
    } else if (!strncmp(a, "view", 4)) {
        if (!ui_app_is_open(&APP_INTEL)) ui_app_open(&APP_INTEL, NULL);
        show_view(atoi(a + 4));
    } else if (!strcmp(a, "brief")) {
        make_brief();
        for (char *s = IX.brief_full, *nl; s && *s; s = nl ? nl + 1 : NULL) {
            nl = strchr(s, '\n');
            printf("BRIEF | %.*s\n", nl ? (int)(nl - s) : (int)strlen(s), s);
        }
    } else if (!strcmp(a, "summary")) {
        sum_tap(NULL, NULL);
    } else if (!strcmp(a, "clear")) {
        intel_clear();
        printf("INTEL cleared\n");
    } else if (!strncmp(a, "run", 3)) {
        /* "assist intel run 70 2": a made-up last run (FL following at 70 %, 2 brownouts) for the rules */
        double f = atof(a + 3);
        const char *b = a + 3;
        while (*b == ' ') b++;
        while (*b && *b != ' ') b++;
        IX.session++;
        IX.session_end = hal_seconds();
        IX.brown_last = atoi(b);
        IX.vmin_last = IX.brown_last ? 6.4 : 11.2;
        IX.mods_last = 4;
        IX.follow_last[0] = f > 0 ? f : 70;
        IX.follow_last[1] = IX.follow_last[2] = IX.follow_last[3] = 97;
        IX.next_pass = 0;
        printf("INTEL a made-up run %d: FL %.0f %%, %d brownouts\n", IX.session, IX.follow_last[0], IX.brown_last);
    } else {
        char lines[4][96];
        int k = intel_services(lines, 4);
        for (int i = 0; i < k; i++) printf("INTEL %s\n", lines[i]);
        printf("INTEL pass %.2f ms (worst %.2f ms, %d passes), run %d\n", IX.pass_ms, IX.pass_max_ms, IX.passes, IX.session);
        intel_insight_t *l = malloc(sizeof *l * INTEL_MAX);
        int n = l ? intel_list(l, INTEL_MAX) : 0;
        for (int i = 0; i < n; i++)
            printf("INTEL %d%s%s %s: %s | %s\n", l[i].prio, l[i].said ? " said" : "", l[i].dismissed ? " dismissed" : "", l[i].key,
                   l[i].text, l[i].why);
        free(l);
    }
}
