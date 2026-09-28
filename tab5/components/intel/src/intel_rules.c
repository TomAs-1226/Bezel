/* The rules: what the tablet makes of what it knows (intel.h). Pure and cheap: a pass over one snapshot, a
 * few dozen comparisons, no I/O; host-tested with made-up days.
 *
 * Each rule raises an insight with a key that names the thing it is about ("prep:Q34", "lag:3:fl"), so the
 * store never says one thing twice. The priority decides whether the island says it: 3 act now, 2 worth a
 * look, 1 only listed. A rule says prio 2 only when there is something to DO that nothing else on the tablet
 * already says: the match alarm rings for the queue by itself, so the prep line is prio 2 only when it carries
 * a battery to pick or a fault to look at. */
#include "intel.h"
#include "intel_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const MOD[4] = { "FL", "FR", "BL", "BR" };

static intel_insight_t *add(intel_insight_t *out, int *n, int max, int cat, int prio, const char *key)
{
    if (*n >= max) return NULL;
    intel_insight_t *x = &out[(*n)++];
    memset(x, 0, sizeof *x);
    x->cat = cat;
    x->prio = prio;
    snprintf(x->key, sizeof x->key, "%s", key);
    return x;
}

/* a short stable number for a text, for keys ("err:1234") */
static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h % 100000;
}

/* the one thing most worth checking on the robot before the next match, "" none; its prio in *p */
static void robot_worry(const intel_in_t *in, char *out, size_t n, int *p)
{
    out[0] = 0;
    *p = 0;
    if (in->can_down) {
        snprintf(out, n, "a CAN bus is down");
        *p = 3;
        return;
    }
    if (in->mods > 0) {
        int worst = -1, good = 0;
        for (int i = 0; i < in->mods && i < 4; i++) {
            if (isnan(in->follow_pct[i])) continue;
            if (in->follow_pct[i] >= 88) good++;
            if (worst < 0 || in->follow_pct[i] < in->follow_pct[worst]) worst = i;
        }
        if (worst >= 0 && in->follow_pct[worst] < 75 && good >= 2) {
            snprintf(out, n, "%s lagged %.0f%% last run", MOD[worst], 100 - in->follow_pct[worst]);
            *p = 2;
            return;
        }
    }
    if (in->brownouts_last > 0) {
        snprintf(out, n, "%d brownout%s last run", in->brownouts_last, in->brownouts_last == 1 ? "" : "s");
        *p = 2;
        return;
    }
    if (in->devices_missing > 0) {
        snprintf(out, n, "%d CAN device%s not answering", in->devices_missing, in->devices_missing == 1 ? "" : "s");
        *p = 2;
        return;
    }
    if (in->cams_bad > 0 && in->cam_bad[0]) {
        snprintf(out, n, "%s", in->cam_bad);
        *p = 2;
        return;
    }
    if (in->errors > 0) {
        snprintf(out, n, "%d robot error%s", in->errors, in->errors == 1 ? "" : "s");
        *p = 2;
    }
}

static void match_rules(const intel_in_t *in, intel_insight_t *out, int *n, int max)
{
    if (!in->now || in->nup <= 0) return;
    char key[40], hm[8];
    const intel_up_t *u = &in->up[0];
    if (!u->start) return;
    long to_start = (long)(u->start - in->now);
    if (to_start > 75 * 60 || to_start < -10 * 60) return;
    /* when it queues: Nexus's estimate, else a guess 20 min before the match */
    time_t q = u->queue_at ? u->queue_at : u->start - 20 * 60;
    long to_q = (long)(q - in->now);
    int stage = nx_stage(u->status);
    char worry[80];
    int wp;
    robot_worry(in, worry, sizeof worry, &wp);
    bool pick_needed = in->batt_line[0] && !in->batt_pick[0];
    snprintf(key, sizeof key, "prep:%s", u->label);
    /* The match alarm rings for the queue itself (ui_match.c): once the match is queuing, only a fault on the
     * robot is worth saying over it; before, 45 minutes out is when a battery pick or a check still fits */
    int prio = 1;
    if (stage >= 1 && stage <= 2) prio = wp >= 2 ? 3 : 1;
    else if (to_q <= 45 * 60 && to_q > 0 && (pick_needed || wp >= 2)) prio = 2;
    intel_insight_t *x = add(out, n, max, IC_MATCH, prio, key);
    if (!x) return;
    ix_hm(q, hm, sizeof hm);
    if (stage >= 1) snprintf(x->text, sizeof x->text, "%s is %s", u->label, nx_status_short(u->status));
    else if (to_q > 60) snprintf(x->text, sizeof x->text, "%s queues ~%s (%ld min)", u->label, hm, (to_q + 59) / 60);
    else snprintf(x->text, sizeof x->text, "%s queues now", u->label);
    /* "battery: #7 · lowest resistance" reads as "put in #7 · lowest resistance" here */
    const char *bl = in->batt_line;
    if (!strncmp(bl, "battery: ", 9)) bl += 9;
    if (pick_needed) ix_addf(x->text, sizeof x->text, " \xc2\xb7 put in %s", bl);
    else if (in->batt_pick[0]) ix_addf(x->text, sizeof x->text, " \xc2\xb7 battery %s in", in->batt_pick);
    if (wp) ix_addf(x->text, sizeof x->text, " \xc2\xb7 %s", worry);
    if (!isnan(u->win)) ix_addf(x->text, sizeof x->text, " \xc2\xb7 %.0f%% to win", u->win * 100);
    snprintf(x->why, sizeof x->why, "time from %s", u->queue_at ? "nexus's queue estimate" : mt_src_name(u->src));
    if (u->away >= 0 && in->now_queuing[0])
        ix_addf(x->why, sizeof x->why, "; now queuing %s, we are %d away", in->now_queuing, u->away);
    if (!isnan(u->win)) ix_addf(x->why, sizeof x->why, "; win chance from statbotics");
    snprintf(x->app, sizeof x->app, "%s", pick_needed ? "checklist" : wp ? "alerts" : "blue alliance");
    x->until = 0;

    /* the field running late against the schedule: listed (the alarms follow it by themselves) */
    if (in->behind_min >= 10) {
        snprintf(key, sizeof key, "late:%d", in->behind_min / 10);
        x = add(out, n, max, IC_EVENT, 1, key);
        if (x) {
            ix_hm(u->start, hm, sizeof hm);
            snprintf(x->text, sizeof x->text, "the field runs ~%d min behind: %s now ~%s", in->behind_min, u->label, hm);
            snprintf(x->why, sizeof x->why, "nexus's estimate against the schedule");
            snprintf(x->app, sizeof x->app, "blue alliance");
        }
    }
}

static void power_rules(const intel_in_t *in, intel_insight_t *out, int *n, int max)
{
    char key[40];
    intel_insight_t *x;
    /* a low battery in the robot with a match coming */
    long to_start = in->nup > 0 && in->up[0].start && in->now ? (long)(in->up[0].start - in->now) : -1;
    if (in->connected && !in->enabled && !isnan(in->battery_v) && in->battery_v > 6 && in->battery_v < 12.3 &&
        to_start > 0 && to_start < 60 * 60) {
        snprintf(key, sizeof key, "batt-low:%s", in->up[0].label);
        x = add(out, n, max, IC_POWER, to_start < 25 * 60 ? 3 : 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "the robot's battery reads %.2f V: swap it before %s", in->battery_v,
                     in->up[0].label);
            snprintf(x->why, sizeof x->why, "resting voltage under 12.3 V with %ld min to the match", to_start / 60);
            snprintf(x->app, sizeof x->app, "batteries");
        }
    }
    if (in->session > 0 && in->brownouts_last > 0) {
        snprintf(key, sizeof key, "brown:%d", in->session);
        x = add(out, n, max, IC_POWER, 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "%d brownout%s last run", in->brownouts_last,
                     in->brownouts_last == 1 ? "" : "s");
            if (!isnan(in->vmin_last)) ix_addf(x->text, sizeof x->text, ", lowest %.1f V", in->vmin_last);
            ix_addf(x->text, sizeof x->text, ": check the battery and the main breaker");
            snprintf(x->why, sizeof x->why, "the robot's brownout flag during the last enabled run");
            snprintf(x->app, sizeof x->app, "batteries");
            x->until = in->session_end + 45 * 60;
        }
    }
    if (in->tablet_pct >= 0 && !in->tablet_charging && (in->tablet_pct < 20 || (in->event_live && in->tablet_pct < 35))) {
        x = add(out, n, max, IC_DAY, 2, "tablet-low");
        if (x) {
            snprintf(x->text, sizeof x->text, "this tablet is at %d%%: charge it%s", in->tablet_pct,
                     in->event_live ? " before the next match" : "");
            snprintf(x->why, sizeof x->why, "the tablet's own battery");
        }
    }
}

static void robot_rules(const intel_in_t *in, intel_insight_t *out, int *n, int max)
{
    char key[40];
    intel_insight_t *x;
    if (in->session > 0 && in->mods > 0) {
        int good = 0, bad = -1;
        double sum = 0;
        int k = 0;
        for (int i = 0; i < in->mods && i < 4; i++) {
            if (isnan(in->follow_pct[i])) continue;
            sum += in->follow_pct[i];
            k++;
            if (in->follow_pct[i] >= 88) good++;
            if (in->follow_pct[i] < 75 && (bad < 0 || in->follow_pct[i] < in->follow_pct[bad])) bad = i;
        }
        if (bad >= 0 && good >= 2) {
            snprintf(key, sizeof key, "lag:%d:%s", in->session, MOD[bad]);
            x = add(out, n, max, IC_ROBOT, 2, key);
            if (x) {
                snprintf(x->text, sizeof x->text, "%s module followed %.0f%% of its command last run: check it before "
                                                  "queuing",
                         MOD[bad], in->follow_pct[bad]);
                snprintf(x->why, sizeof x->why, "one module short while the others follow: a jammed drive, a loose wheel or "
                                                "pulley, or that motor (swerve_check says more)");
                snprintf(x->app, sizeof x->app, "assist");
                x->until = in->session_end + 60 * 60;
            }
        } else if (k >= 3 && sum / k < 80) {
            snprintf(key, sizeof key, "lagall:%d", in->session);
            x = add(out, n, max, IC_ROBOT, 1, key);
            if (x) {
                snprintf(x->text, sizeof x->text, "every module under-followed last run (~%.0f%%)", sum / k);
                snprintf(x->why, sizeof x->why, "all alike is global: battery sag, current limits, feedforward, wheel radius");
                x->until = in->session_end + 60 * 60;
            }
        }
    }
    if (!in->connected) return;
    if (in->can_down) {
        x = add(out, n, max, IC_ROBOT, 3, "can-down");
        if (x) {
            snprintf(x->text, sizeof x->text, "a CAN bus is down on the robot");
            snprintf(x->why, sizeof x->why, "the robot reports a bus down: a cable, a terminator, a device shorting it");
            snprintf(x->app, sizeof x->app, "alerts");
        }
    } else if (in->devices_missing > 0) {
        snprintf(key, sizeof key, "can-miss:%d", in->devices_missing);
        x = add(out, n, max, IC_ROBOT, 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "%d of %d CAN devices aren't answering", in->devices_missing,
                     in->devices_total);
            snprintf(x->why, sizeof x->why, "the robot's device roster");
            snprintf(x->app, sizeof x->app, "alerts");
        }
    }
    if (in->errors > 0 && in->worst_alert[0]) {
        snprintf(key, sizeof key, "err:%u", hash(in->worst_alert));
        x = add(out, n, max, IC_ROBOT, 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "robot error: %.120s", in->worst_alert);
            if (in->errors > 1) ix_addf(x->text, sizeof x->text, " (+%d more)", in->errors - 1);
            snprintf(x->why, sizeof x->why, "the robot's alerts");
            snprintf(x->app, sizeof x->app, "alerts");
        }
    }
    if (in->loop_over && in->loop_max_ms > 40) {
        x = add(out, n, max, IC_ROBOT, 1, "loop");
        if (x) {
            snprintf(x->text, sizeof x->text, "the robot's loop overran (worst %.0f ms)", in->loop_max_ms);
            snprintf(x->why, sizeof x->why, "loop overruns delay every control output");
        }
    }
    if (in->code_dirty && in->code_sha[0]) {
        snprintf(key, sizeof key, "dirty:%s", in->code_sha);
        x = add(out, n, max, IC_ROBOT, in->event_live ? 2 : 1, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "the robot runs uncommitted code (%s)", in->code_sha);
            snprintf(x->why, sizeof x->why, "deployed with changes not in git: nobody can say later what ran");
        }
    }
    if (in->have_agent && in->throttled) {
        x = add(out, n, max, IC_ROBOT, 2, "throttle");
        if (x) {
            snprintf(x->text, sizeof x->text, "Systemcore is throttling");
            snprintf(x->why, sizeof x->why, "catalyst-agent: under-voltage or heat caps the CPU");
            snprintf(x->app, sizeof x->app, "systemcore");
        }
    }
    if (in->have_agent && !isnan(in->core_temp) && in->core_temp >= 80) {
        x = add(out, n, max, IC_ROBOT, 2, "core-hot");
        if (x) {
            snprintf(x->text, sizeof x->text, "Systemcore at %.0f \xc2\xb0""C", in->core_temp);
            snprintf(x->why, sizeof x->why, "catalyst-agent's hottest zone");
            snprintf(x->app, sizeof x->app, "systemcore");
        }
    }
    if (in->have_agent && !isnan(in->disk_free_pct) && in->disk_free_pct < 10) {
        x = add(out, n, max, IC_ROBOT, 2, "disk");
        if (x) {
            snprintf(x->text, sizeof x->text, "Systemcore's storage is %.0f%% free: logs will stop", in->disk_free_pct);
            snprintf(x->why, sizeof x->why, "catalyst-agent's mounts");
            snprintf(x->app, sizeof x->app, "systemcore");
        }
    }
    if (in->cams_bad > 0 && in->cam_bad[0]) {
        snprintf(key, sizeof key, "cam:%u", hash(in->cam_bad));
        x = add(out, n, max, IC_ROBOT, 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "%s", in->cam_bad);
            if (in->cams_bad > 1) ix_addf(x->text, sizeof x->text, " (+%d more)", in->cams_bad - 1);
            snprintf(x->why, sizeof x->why, "the robot's vision state and the camera's own status");
            snprintf(x->app, sizeof x->app, "field");
        }
    }
    if (!isnan(in->cam_hot) && in->cam_hot >= 80) {
        x = add(out, n, max, IC_ROBOT, 1, "cam-hot");
        if (x) {
            snprintf(x->text, sizeof x->text, "%s at %.0f \xc2\xb0""C", in->cam_hot_name, in->cam_hot);
            snprintf(x->why, sizeof x->why, "Limelight 4s run 65-85 C in free air; past that they throttle");
        }
    }
    if (in->fw_mixed > 0 && in->fw_line[0]) {
        snprintf(key, sizeof key, "fw:%u", hash(in->fw_line));
        x = add(out, n, max, IC_ROBOT, in->event_live ? 2 : 1, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "%s", in->fw_line);
            snprintf(x->why, sizeof x->why, "phoenix's diagnostics server: one model on more than one firmware");
        }
    }
}

static void event_rules(const intel_in_t *in, intel_insight_t *out, int *n, int max)
{
    char key[40];
    intel_insight_t *x;
    if (in->ann[0] && in->now && (!in->ann_at || in->now - in->ann_at < 45 * 60)) {
        snprintf(key, sizeof key, "ann:%u", hash(in->ann));
        x = add(out, n, max, IC_EVENT, 2, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "event: %.140s", in->ann);
            snprintf(x->why, sizeof x->why, "an announcement on nexus");
        }
    }
    /* another team needs a part: good to know, the pit may have one */
    if (in->part[0] && in->part_team != in->team && in->now && (!in->part_at || in->now - in->part_at < 30 * 60)) {
        snprintf(key, sizeof key, "part:%u", hash(in->part));
        x = add(out, n, max, IC_EVENT, 1, key);
        if (x) {
            if (in->part_team > 0) snprintf(x->text, sizeof x->text, "team %d needs %.80s", in->part_team, in->part);
            else snprintf(x->text, sizeof x->text, "a team needs %.80s", in->part);
            snprintf(x->why, sizeof x->why, "a parts request on nexus");
        }
    }
    if (in->have_epa && !isnan(in->epa)) {
        x = add(out, n, max, IC_EVENT, 1, "epa");
        if (x) {
            snprintf(x->text, sizeof x->text, "EPA %.1f", in->epa);
            if (in->epa_rank > 0 && in->epa_count > 0) ix_addf(x->text, sizeof x->text, ", #%d of %d", in->epa_rank, in->epa_count);
            if (!isnan(in->percentile)) ix_addf(x->text, sizeof x->text, " (top %.0f%%)", (1 - in->percentile) * 100 < 1 ? 1 : (1 - in->percentile) * 100);
            if (in->rank > 0) ix_addf(x->text, sizeof x->text, " \xc2\xb7 rank %d of %d here", in->rank, in->num_teams);
            snprintf(x->why, sizeof x->why, "statbotics; the event rank from the blue alliance");
            snprintf(x->app, sizeof x->app, "blue alliance");
        }
    }
    if (in->event_live && !in->nexus_key) {
        x = add(out, n, max, IC_SERVICE, 1, "svc:nexus");
        if (x) {
            snprintf(x->text, sizeof x->text, "live queue times need NEXUS_API_KEY in KEYS.ENV");
            snprintf(x->why, sizeof x->why, "until then the alarms use the blue alliance's times, which lag the field");
        }
    } else if (in->nexus == NX_REFUSED) {
        x = add(out, n, max, IC_SERVICE, 2, "svc:nexus-refused");
        if (x) {
            snprintf(x->text, sizeof x->text, "nexus refused the key: check NEXUS_API_KEY");
            snprintf(x->why, sizeof x->why, "the alarms use the blue alliance's times meanwhile");
        }
    }
}

static void day_rules(const intel_in_t *in, intel_insight_t *out, int *n, int max)
{
    char key[40];
    intel_insight_t *x;
    if (in->next_event[0] && in->next_event_at && in->now) {
        long to = (long)(in->next_event_at - in->now);
        if (to > 0 && to <= 60 * 60) {
            snprintf(key, sizeof key, "cal:%ld", (long)in->next_event_at);
            x = add(out, n, max, IC_DAY, to <= 20 * 60 ? 2 : 1, key);
            if (x) {
                snprintf(x->text, sizeof x->text, "in %ld min: %.100s", (to + 59) / 60, in->next_event);
                snprintf(x->why, sizeof x->why, "the calendar");
                snprintf(x->app, sizeof x->app, "calendar");
            }
        }
    }
    if (in->have_weather && in->rain_hour >= 0 && in->rain_pct >= 50) {
        snprintf(key, sizeof key, "rain:%d", in->rain_hour);
        x = add(out, n, max, IC_DAY, 1, key);
        if (x) {
            snprintf(x->text, sizeof x->text, "rain likely from %d:00 (%d%%)", in->rain_hour, in->rain_pct);
            snprintf(x->why, sizeof x->why, "the forecast");
            snprintf(x->app, sizeof x->app, "weather");
        }
    }
}

int intel_rules(const intel_in_t *in, intel_insight_t *out, int max)
{
    int n = 0;
    match_rules(in, out, &n, max);
    power_rules(in, out, &n, max);
    robot_rules(in, out, &n, max);
    event_rules(in, out, &n, max);
    day_rules(in, out, &n, max);
    return n;
}
