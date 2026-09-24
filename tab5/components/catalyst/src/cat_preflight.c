#include "cat_preflight.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LISTEN_S 3.0

static void add(cat_preflight_t *p, cat_verdict_t v, const char *what, const char *fmt, ...)
{
    if (p->n >= CAT_MAX_CHECKS) return;
    cat_check_t *c = &p->c[p->n++];
    c->v = v;
    snprintf(c->what, sizeof c->what, "%s", what);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->detail, sizeof c->detail, fmt, ap);
    va_end(ap);
    if (v == CAT_FAIL) p->fails++;
    else if (v == CAT_WARN) p->warns++;
    else if (v == CAT_PASS) p->passes++;
}

void cat_preflight_start(cat_preflight_t *p, double now, int expect_team)
{
    memset(p, 0, sizeof *p);
    p->running = true;
    p->started = now;
    p->expect_team = expect_team;
}

double cat_preflight_progress(const cat_preflight_t *p, double now)
{
    if (p->done) return 1;
    if (!p->running) return 0;
    double k = (now - p->started) / LISTEN_S;
    return k < 0 ? 0 : k > 1 ? 1 : k;
}

static void listen(cat_preflight_t *p, const cat_robot_t *r)
{
    /* NT4 drops repeated values and a live Pigeon jitters, so two or fewer distinct headings over the
     * window means the gyro is frozen (preflight.py:141-147). */
    if (!(r->heading_deg == r->heading_deg)) return;
    for (int i = 0; i < p->nheadings; i++) if (fabs(p->headings[i] - r->heading_deg) < 1e-9) return;
    if (p->nheadings < 16) p->headings[p->nheadings++] = r->heading_deg;
}

static void judge(cat_preflight_t *p, const cat_robot_t *r)
{
    if (!r->connected) {
        add(p, CAT_FAIL, "robot link", "no NetworkTables connection (tried %s)", r->address[0] ? r->address : "nothing");
        return;
    }
    add(p, CAT_PASS, "robot link", "%s, %.0f ms round trip", r->address, r->rtt_ms);
    if (!r->have_mode) {
        add(p, CAT_FAIL, "robot program", "no control word: the robot program isn't running");
    } else {
        add(p, CAT_PASS, "robot program", "%s%s", r->line2 ? "Systemcore · " : "roboRIO · ", cat_mode_name(r));
        if (r->estop) add(p, CAT_FAIL, "e-stop", "e-stopped; it latches until the program restarts");
        if (!r->ds) add(p, CAT_WARN, "driver station", "no Driver Station attached");
        if (r->enabled) add(p, CAT_WARN, "enabled", "the robot is enabled: don't deploy, don't type on the DS laptop");
    }
    if (r->have_alliance) add(p, CAT_INFO, "alliance", "%s %d", r->red ? "red" : "blue", r->station);

    if (!r->have_battery) add(p, CAT_WARN, "battery", "the robot doesn't publish its battery voltage");
    else if (r->battery_v < 11.8) add(p, CAT_FAIL, "battery", "%.2f V: swap it", r->battery_v);
    else if (r->battery_v < 12.4) add(p, CAT_WARN, "battery", "%.2f V: swap before a match", r->battery_v);
    else add(p, CAT_PASS, "battery", "%.2f V", r->battery_v);
    if (r->browned_out) add(p, CAT_FAIL, "brownout", "the controller is browned out right now");
    else if (r->brownout_risk) add(p, CAT_WARN, "brownout", "brownout monitor says at risk (predicted %.1f V)", r->predicted_v);

    if (r->can_down) add(p, CAT_FAIL, "can", "a CAN bus is down");
    if (r->ncan_util) {
        int worst = 0;
        for (int i = 1; i < r->ncan_util; i++) if (r->can_util[i] > r->can_util[worst]) worst = i;
        double u = r->can_util[worst];
        add(p, u > 0.70 ? CAT_WARN : CAT_PASS, "can load", "busiest %s%d at %.0f %%", r->ncan_util > 1 ? "can_s" : "bus ",
            worst, u * 100);
    }
    if (r->have_loop) {
        cat_verdict_t v = r->loop_avg_ms > 20 ? CAT_FAIL : r->loop_avg_ms > 15 ? CAT_WARN : CAT_PASS;
        add(p, v, "loop", "%.1f ms average, %.1f ms worst (20 ms budget)", r->loop_avg_ms, r->loop_max_ms);
    }

    if (r->have_roster) {
        cat_verdict_t v = r->motors_connected < r->motors_expected ? CAT_FAIL : CAT_PASS;
        char missing[96] = "";
        size_t o = 0;
        for (int i = 0; i < r->ndevices; i++) {
            if (r->devices[i].connected == 0 && o < sizeof missing - 8)
                o += (size_t)snprintf(missing + o, sizeof missing - o, "%s%d", o ? ", " : "", r->devices[i].id);
        }
        add(p, v, "motors", "%d of %d answering%s%s", r->motors_connected, r->motors_expected, missing[0] ? "; missing " : "",
            missing);
        if (r->cams_expected)
            add(p, r->cams_connected < r->cams_expected ? CAT_FAIL : CAT_PASS, "cameras", "%d of %d answering",
                r->cams_connected, r->cams_expected);
    } else if (r->ndevices) {
        add(p, CAT_INFO, "devices", "%d CAN devices declared (1.x publishes no per-device state)", r->ndevices);
    }
    if (r->have_controller && !r->controller_connected) add(p, CAT_WARN, "controller", "%s not connected", r->controller_kind);

    int shown = 0;
    for (int i = 0; i < r->nalerts && shown < 12; i++) {
        const cat_alert_t *a = &r->alerts[i];
        if (a->sev == CAT_SEV_INFO) continue;
        add(p, a->sev == CAT_SEV_ERROR ? CAT_FAIL : CAT_WARN, a->health ? "health" : "alert", "%s%s%s",
            a->source[0] ? a->source : "", a->source[0] ? ": " : "", a->text);
        shown++;
    }
    if (!r->n_errors && !r->n_warnings) add(p, CAT_PASS, "alerts", "no errors or warnings");

    if (p->expect_team > 0 && r->team > 0 && r->team != p->expect_team)
        add(p, CAT_WARN, "team number", "robot says %d, the tablet expects %d", r->team, p->expect_team);

    if (r->heading_deg == r->heading_deg) {
        if (p->nheadings <= 2) add(p, CAT_WARN, "gyro", "heading frozen at %.1f°: stale gyro frames?", r->heading_deg);
        else add(p, CAT_PASS, "gyro", "alive, %d distinct headings in %.0f s", p->nheadings, LISTEN_S);
    } else {
        add(p, CAT_WARN, "gyro", "no heading published");
    }

    if (r->have_pose) {
        if (fabs(r->pose_x) < 0.01 && fabs(r->pose_y) < 0.01) add(p, CAT_WARN, "pose", "at the origin: never seeded");
        else if (fabs(r->pose_x - 8.2705) < 0.05 && fabs(r->pose_y - 4.0345) < 0.05)
            add(p, CAT_WARN, "pose", "at field centre: vision placed it from a tag it can't place");
        else add(p, CAT_PASS, "pose", "(%.2f, %.2f) m, %.0f°", r->pose_x, r->pose_y, r->pose_rad * 57.2958);
    }

    for (int i = 0; i < r->ncameras; i++) {
        const cat_camera_t *c = &r->cameras[i];
        bool ok = c->connected && (!strcmp(c->state, "OK") || !strcmp(c->state, "NO_TARGETS"));
        cat_verdict_t v = !c->connected ? CAT_FAIL : ok ? CAT_PASS : CAT_WARN;
        add(p, v, "camera", "%s: %s%s%s", c->name, c->state, c->detail[0] ? " — " : "", c->detail);
    }

    if (r->have_manifest) add(p, CAT_PASS, "tunables", "%d declared", r->ntunables);
    else if (r->ntunables) add(p, CAT_INFO, "tunables", "%d 1.x TunableNumbers, no manifest", r->ntunables);

    if (r->have_wheel_radius && !strcmp(r->wheel_radius_status, "done")) {
        cat_verdict_t v = fabs(r->wheel_radius_change) > 15 ? CAT_WARN : CAT_INFO;
        add(p, v, "wheel radius", "%.3f in (%+.1f %%)", r->wheel_radius_in, r->wheel_radius_change);
    }

    if (r->have_autos) {
        if (!r->auto_selected[0]) add(p, CAT_WARN, "auto", "no auto chosen");
        else add(p, CAT_PASS, "auto", "%s", r->auto_selected);
    }
    if (r->have_start_check)
        add(p, r->start_ready ? CAT_PASS : CAT_WARN, "auto start", r->start_ready ? "on the start pose" : "%.2f m, %.0f° off the start pose",
            r->start_dist_m, r->start_heading_deg);

    if (r->have_sc) {
        if (r->sc_temp >= 80) add(p, CAT_WARN, "systemcore", "%.0f °C", r->sc_temp);
        if (r->sc_storage >= 0.95) add(p, CAT_FAIL, "storage", "%.0f %% full", r->sc_storage * 100);
        else if (r->sc_storage >= 0.85) add(p, CAT_WARN, "storage", "%.0f %% full", r->sc_storage * 100);
        if (r->sc_emmc >= 3) add(p, CAT_FAIL, "emmc", "replace the controller's storage (pre-EOL 3)");
    }

    if (r->have_preflight) {
        add(p, r->preflight_ready ? CAT_PASS : CAT_FAIL, "robot preflight", "%s", r->preflight_summary[0] ? r->preflight_summary : (r->preflight_ready ? "ready" : "not ready"));
        for (int i = 0; i < r->nfindings; i++) {
            const cat_finding_t *f = &r->findings[i];
            if (f->sev == CAT_SEV_INFO) continue;
            add(p, f->sev == CAT_SEV_ERROR ? CAT_FAIL : CAT_WARN, "robot preflight", "%s", f->text);
        }
    }
    if (r->have_systemcheck)
        add(p, r->systemcheck_ready ? CAT_PASS : CAT_WARN, "system check", "%s",
            r->systemcheck_report[0] ? r->systemcheck_report : (r->systemcheck_ready ? "passed" : "run Utility → System check on the DS"));
}

bool cat_preflight_step(cat_preflight_t *p, const cat_robot_t *r, double now)
{
    if (!p->running) return p->done;
    listen(p, r);
    if (now - p->started < LISTEN_S && r->connected) return false;
    judge(p, r);
    /* failures first, then warnings, then the rest: the list reads as a to-do */
    for (int i = 1; i < p->n; i++) {
        cat_check_t c = p->c[i];
        int rank = c.v == CAT_FAIL ? 0 : c.v == CAT_WARN ? 1 : 2;
        int j = i - 1;
        while (j >= 0) {
            int rj = p->c[j].v == CAT_FAIL ? 0 : p->c[j].v == CAT_WARN ? 1 : 2;
            if (rj <= rank) break;
            p->c[j + 1] = p->c[j];
            j--;
        }
        p->c[j + 1] = c;
    }
    p->running = false;
    p->done = true;
    return true;
}
