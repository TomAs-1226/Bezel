/* The briefing: the day, the event and the robot in a few lines (intel.h). Pure: host-tested.
 *
 * Deterministic on purpose: it is read at a glance in the pit, and every line says where its numbers come
 * from. A model's summary of it is optional and separate (ui_intel.c, on the assistant's worker). One line
 * per subject, the subject first ("next:", "robot:"), nothing about a subject the tablet knows nothing of. */
#include "intel.h"
#include "intel_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void teams(char *out, size_t n, const char *word, const int *t, int k)
{
    bool first = true;
    for (int i = 0; i < k; i++) {
        if (!t[i]) continue;
        ix_addf(out, n, "%s%d", first ? word : ", ", t[i]);
        first = false;
    }
}

static void match_line(const intel_in_t *in, const intel_up_t *u, char *out, size_t n, const char *head)
{
    char hm[8];
    ix_addf(out, n, "%s %s", head, u->label);
    if (u->start) {
        ix_hm(u->start, hm, sizeof hm);
        ix_addf(out, n, " ~%s (%s)", hm, mt_src_name(u->src));
    }
    if (u->ours) {
        ix_addf(out, n, " \xc2\xb7 %s", u->ours == 1 ? "red" : "blue");
        teams(out, n, " with ", u->partners, 2);
        teams(out, n, " against ", u->opponents, 3);
    }
    if (!isnan(u->win)) ix_addf(out, n, " \xc2\xb7 %.0f%% to win", u->win * 100);
    if (u->status[0]) ix_addf(out, n, " \xc2\xb7 %s", nx_status_short(u->status));
    ix_addf(out, n, "\n");
    (void)in;
}

size_t intel_briefing(const intel_in_t *in, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    char b[64];
    if (in->now) {
        struct tm tm;
        localtime_r(&in->now, &tm);
        strftime(b, sizeof b, "%A %d %B", &tm);
        char hm[8];
        ix_hm(in->now, hm, sizeof hm);
        ix_addf(out, n, "%s \xc2\xb7 %s\n", b, hm);
    }
    if (in->have_weather) {
        ix_addf(out, n, "weather: %.0f\xc2\xb0 %s, %.0f-%.0f\xc2\xb0", in->temp, in->weather_word, in->lo, in->hi);
        if (in->rain_hour >= 0) ix_addf(out, n, ", rain likely from %d:00 (%d%%)", in->rain_hour, in->rain_pct);
        ix_addf(out, n, "\n");
    }
    if (in->event_key[0]) {
        ix_addf(out, n, "event: %s%s", in->event_name[0] ? in->event_name : in->event_key, in->event_live ? " (today)" : "");
        if (in->rank > 0) ix_addf(out, n, " \xc2\xb7 rank %d of %d (%d-%d-%d)", in->rank, in->num_teams, in->wins, in->losses, in->ties);
        ix_addf(out, n, "\n");
    }
    if (in->have_epa && !isnan(in->epa)) {
        ix_addf(out, n, "statbotics: EPA %.1f", in->epa);
        if (in->epa_rank > 0) ix_addf(out, n, ", #%d of %d", in->epa_rank, in->epa_count);
        if (!isnan(in->percentile)) {
            double top = (1 - in->percentile) * 100;
            ix_addf(out, n, " (top %.0f%%)", top < 1 ? 1 : top);
        }
        ix_addf(out, n, "\n");
    }
    for (int i = 0; i < in->nup && i < 3; i++) match_line(in, &in->up[i], out, n, i == 0 ? "next:" : "then:");
    if (in->nexus == NX_READY && in->now_queuing[0]) {
        ix_addf(out, n, "queue: now queuing %s", in->now_queuing);
        if (in->nup > 0 && in->up[0].away > 0)
            ix_addf(out, n, " \xc2\xb7 %s is %d match%s after it", in->up[0].label, in->up[0].away, in->up[0].away == 1 ? "" : "es");
        else if (in->nup > 0 && in->up[0].away == 0) ix_addf(out, n, " \xc2\xb7 that's %s", in->up[0].label);
        if (in->behind_min >= 5) ix_addf(out, n, " \xc2\xb7 the field runs ~%d min behind", in->behind_min);
        ix_addf(out, n, " (nexus)\n");
    } else if (in->event_live && !in->nexus_key) {
        ix_addf(out, n, "queue: no live times (NEXUS_API_KEY isn't set); the blue alliance's lag the field\n");
    }
    if (in->ann[0]) ix_addf(out, n, "announced: %s\n", in->ann);
    if (in->batt_pick[0]) ix_addf(out, n, "battery: %s is in\n", in->batt_pick);
    else if (in->batt_line[0]) ix_addf(out, n, "%s\n", in->batt_line);
    /* the robot: its state, then what the last run showed */
    if (in->connected) {
        ix_addf(out, n, "robot: connected%s", in->enabled ? ", enabled" : "");
        if (!isnan(in->battery_v)) ix_addf(out, n, ", %.2f V", in->battery_v);
        if (in->errors) ix_addf(out, n, ", %d error%s", in->errors, in->errors == 1 ? "" : "s");
        if (in->devices_missing) ix_addf(out, n, ", %d CAN device%s missing", in->devices_missing, in->devices_missing == 1 ? "" : "s");
        if (in->code_sha[0]) ix_addf(out, n, ", code %s%s", in->code_sha, in->code_dirty ? " (uncommitted)" : "");
        ix_addf(out, n, "\n");
    } else {
        ix_addf(out, n, "robot: not connected\n");
    }
    if (in->session > 0) {
        char last[160] = "";
        if (in->brownouts_last > 0)
            ix_addf(last, sizeof last, "%d brownout%s", in->brownouts_last, in->brownouts_last == 1 ? "" : "s");
        if (!isnan(in->vmin_last)) ix_addf(last, sizeof last, "%slowest %.1f V", last[0] ? ", " : "", in->vmin_last);
        static const char *const M[4] = { "FL", "FR", "BL", "BR" };
        for (int i = 0; i < in->mods && i < 4; i++)
            if (!isnan(in->follow_pct[i]) && in->follow_pct[i] < 80)
                ix_addf(last, sizeof last, "%s%s followed %.0f%%", last[0] ? ", " : "", M[i], in->follow_pct[i]);
        if (last[0]) ix_addf(out, n, "last run: %s\n", last);
    }
    if (in->cams_bad && in->cam_bad[0]) ix_addf(out, n, "vision: %s\n", in->cam_bad);
    if (in->fw_line[0]) ix_addf(out, n, "firmware: %s\n", in->fw_line);
    if (in->next_event[0]) ix_addf(out, n, "calendar: %s\n", in->next_event);
    if (in->next_alarm[0]) ix_addf(out, n, "alarm: %s\n", in->next_alarm);
    if (in->tablet_pct >= 0) ix_addf(out, n, "tablet: %d%%%s\n", in->tablet_pct, in->tablet_charging ? ", charging" : "");
    size_t l = strlen(out);
    while (l && out[l - 1] == '\n') out[--l] = 0;
    return l;
}
