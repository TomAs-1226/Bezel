/* as_diag — triage and swerve tracking: see as_diag.h. */
#include "as_diag.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *const AS_MOD_NAME[4] = { "fl", "fr", "bl", "br" };

const char *as_dg_sev_name(as_dg_sev_t s) { return s == AS_DG_FAULT ? "fault" : s == AS_DG_WARN ? "warn" : "info"; }

/* ---------------------------------------------------------------- triage */

/* the triage order: what, found wrong, makes everything after it moot */
enum { O_LINK, O_PROGRAM, O_POWER, O_CAN, O_LOOP, O_CHECKS, O_ALERTS, O_MECH, O_SWERVE, O_VISION, O_CORE, O_MISC };
static const char *const AREA[] = { "link", "program", "power", "can", "loop", "checks", "alerts", "mech",
                                    "swerve", "vision", "core", "misc" };

typedef struct {
    as_finding_t *out;
    int n, max;
} fl_t;

static void find(fl_t *f, as_dg_sev_t sev, int order, const char *next, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void find(fl_t *f, as_dg_sev_t sev, int order, const char *next, const char *fmt, ...)
{
    if (f->n >= f->max) {
        /* full: a worse finding replaces the least of the ones kept */
        int least = -1;
        for (int i = 0; i < f->n; i++)
            if (f->out[i].sev < sev && (least < 0 || f->out[i].sev < f->out[least].sev ||
                                        (f->out[i].sev == f->out[least].sev && f->out[i].order > f->out[least].order)))
                least = i;
        if (least < 0) return;
        f->out[least] = f->out[--f->n];
    }
    as_finding_t *x = &f->out[f->n++];
    memset(x, 0, sizeof *x);
    x->sev = sev;
    x->order = order;
    snprintf(x->area, sizeof x->area, "%s", AREA[order]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->what, sizeof x->what, fmt, ap);
    va_end(ap);
    snprintf(x->next, sizeof x->next, "%s", next ? next : "");
}

static int by_rank(const void *a, const void *b)
{
    const as_finding_t *x = a, *y = b;
    if (x->sev != y->sev) return (int)y->sev - (int)x->sev;
    return x->order - y->order;
}

static bool fin(double v) { return isfinite(v); }

/* the angle difference, wrapped to (-pi, pi] */
static double wrap(double a)
{
    while (a > M_PI) a -= 2 * M_PI;
    while (a <= -M_PI) a += 2 * M_PI;
    return a;
}

/* One module's commanded state (ts, ta) against its actual (as, aa): the actual speed along the commanded
 * direction and the steering error, with either state allowed to be the other's 180-degree twin. */
static void compare(double ts, double ta, double as, double aa, double *cmd, double *along, double *angle)
{
    if (ts < 0) {
        ts = -ts;
        ta += M_PI;
    }
    double d = wrap(aa - ta);
    if (fabs(d) > M_PI / 2) {
        as = -as;
        d = wrap(d + M_PI);
    }
    *cmd = ts;
    *along = as * cos(d);
    *angle = fabs(d);
}

static void triage_swerve(fl_t *f, const cat_robot_t *r)
{
    if (!r->nmodules) return;
    if (!r->have_targets) {
        find(f, AS_DG_INFO, O_SWERVE, "publish /Catalyst/Swerve/ModuleTargets (SwerveSubsystem does) to compare",
             "swerve modules are published but their commanded targets aren't: following can't be judged");
        return;
    }
    if (!r->enabled) {
        find(f, AS_DG_INFO, O_SWERVE, "swerve_check while it drives, or on a drivetrain recording",
             "robot disabled: whether each module follows its command can only be judged while it drives");
        return;
    }
    double cmd[4], along[4], ang[4];
    int judged = 0;
    for (int i = 0; i < r->nmodules && i < 4; i++) {
        compare(r->target_speed[i], r->target_angle[i], r->module_speed[i], r->module_angle[i], &cmd[i], &along[i],
                &ang[i]);
        judged += cmd[i] >= AS_TRACK_MIN_MPS;
    }
    if (judged < 2) return; /* standing still: nothing to see in one instant */
    for (int i = 0; i < r->nmodules && i < 4; i++) {
        if (cmd[i] < AS_TRACK_MIN_MPS) continue;
        double pct = 100 * along[i] / cmd[i];
        if (pct < 70)
            find(f, AS_DG_WARN, O_SWERVE, "swerve_check over a few seconds of driving; then that module by hand",
                 "module %s is doing %.0f%% of its commanded %.2f m/s right now (one sample)", AS_MOD_NAME[i], pct, cmd[i]);
        if (ang[i] * 180 / M_PI > 20)
            find(f, AS_DG_WARN, O_SWERVE, "swerve_check; then the azimuth: CANcoder offset, magnet, steering gear",
                 "module %s points %.0f deg off its command right now (one sample)", AS_MOD_NAME[i], ang[i] * 180 / M_PI);
    }
}

int as_diag_triage(const cat_robot_t *r, as_finding_t *out, int max)
{
    fl_t f = { out, 0, max };
    if (!r->connected) {
        find(&f, AS_DG_FAULT, O_LINK, "radio or tether cable, the robot powered, the team number in settings",
             "no NetworkTables connection to the robot (last tried %s)", r->address[0] ? r->address : "nothing yet");
        return f.n;
    }

    /* the program and the mode */
    if (!r->have_mode)
        find(&f, AS_DG_FAULT, O_PROGRAM,
             "systemcore_health: the robot program's state and restarts, and its last log lines",
             "the robot program isn't publishing its mode: not running, crashing, or restarting");
    if (r->estop)
        find(&f, AS_DG_FAULT, O_PROGRAM, "why it was e-stopped, then restart the robot to clear it",
             "the robot is e-stopped: it stays latched until the robot restarts");
    if (r->have_mode && !r->ds)
        find(&f, AS_DG_WARN, O_PROGRAM, "the Driver Station laptop, its link to the robot",
             "no Driver Station attached");
    if (r->have_controller && !r->controller_connected)
        find(&f, AS_DG_WARN, O_PROGRAM, "the gamepad's USB on the Driver Station, and the port it is assigned",
             "the %s controller is not connected", r->controller_kind[0] ? r->controller_kind : "driver's");

    /* power */
    if (r->browned_out)
        find(&f, AS_DG_FAULT, O_POWER, "battery (a fresh one), the main breaker and battery leads for heat or looseness",
             "the controller reports it is browned out now");
    if (r->have_battery && fin(r->battery_v)) {
        bool loaded = r->enabled && fin(r->total_current) && r->total_current > 25;
        if (r->battery_v < 11.8)
            find(&f, AS_DG_FAULT, O_POWER, "swap the battery; get_batteries for the next one to use",
                 "battery %.2f V%s: below preflight's 11.8 V floor", r->battery_v, loaded ? " (under load)" : "");
        else if (r->battery_v < 12.2)
            find(&f, AS_DG_WARN, O_POWER, "swap before a match; get_batteries for the next one",
                 "battery %.2f V%s: swap before a match (under 12.2 V)", r->battery_v, loaded ? " under load" : "");
        else if (r->battery_v < 12.5 && !loaded)
            find(&f, AS_DG_INFO, O_POWER, NULL, "battery %.2f V resting: usable, not fully charged", r->battery_v);
    } else {
        find(&f, AS_DG_INFO, O_POWER, "BrownoutMonitor, or HealthMonitor.systemCoreChecks() on Systemcore",
             "no battery voltage is published: the tablet can't judge power");
    }
    if (r->brownout_risk)
        find(&f, AS_DG_WARN, O_POWER, "what is drawing current (get_power); current limits; battery",
             "Catalyst's brownout monitor predicts a brownout (%.2f V predicted, floor %.2f V)", r->predicted_v,
             r->brownout_v);

    /* CAN */
    if (r->can_down)
        find(&f, AS_DG_FAULT, O_CAN, "the CAN wiring from the controller outward: a loose or broken connector",
             "a CAN bus is down");
    for (int i = 0; i < r->ncan_util && i < 5; i++) {
        double u = r->can_util[i];
        if (!fin(u)) continue;
        char bus[16];
        snprintf(bus, sizeof bus, r->ncan_util > 1 ? "can_s%d" : "the CAN bus", i);
        if (u > 0.85)
            find(&f, AS_DG_FAULT, O_CAN, "status frame rates, devices per bus (CANBusPlanner)", "%s at %.0f%% utilisation",
                 bus, u * 100);
        else if (u > 0.70)
            find(&f, AS_DG_WARN, O_CAN, "status frame rates, devices per bus", "%s at %.0f%% utilisation", bus, u * 100);
    }
    if (r->ncan_util == 5) {
        /* s0+s1 and s3+s4 share an SPI controller each: the pair's total is what saturates */
        static const int PAIR[2][2] = { { 0, 1 }, { 3, 4 } };
        for (int p = 0; p < 2; p++) {
            double a = r->can_util[PAIR[p][0]], b = r->can_util[PAIR[p][1]];
            if (!fin(a) || !fin(b)) continue;
            if (a + b > 1.0)
                find(&f, AS_DG_FAULT, O_CAN, "move devices to can_s2 or the other pair",
                     "can_s%d + can_s%d share one controller and total %.0f%%", PAIR[p][0], PAIR[p][1], (a + b) * 100);
            else if (a + b > 0.8)
                find(&f, AS_DG_WARN, O_CAN, "move devices to can_s2 or the other pair",
                     "can_s%d + can_s%d share one controller and total %.0f%%", PAIR[p][0], PAIR[p][1], (a + b) * 100);
        }
    }
    int missing = 0;
    char names[120] = "";
    for (int i = 0; i < r->ndevices; i++) {
        const cat_device_t *d = &r->devices[i];
        if (d->connected != 0) continue;
        missing++;
        size_t l = strlen(names);
        if (l < sizeof names - 40)
            snprintf(names + l, sizeof names - l, "%s%s %s id %d", l ? ", " : "", d->name[0] ? d->name : d->type,
                     d->bus[0] ? d->bus : "", d->id);
    }
    if (missing)
        find(&f, AS_DG_FAULT, O_CAN,
             "that device's power and CAN leads; if several in a row are out, the connector before the first",
             "%d CAN device%s not answering: %s", missing, missing == 1 ? "" : "s", names);
    if (r->have_roster && r->motors_connected < r->motors_expected && !missing)
        find(&f, AS_DG_FAULT, O_CAN, "get_can for which one; its power and CAN leads", "%d of %d motors connected",
             r->motors_connected, r->motors_expected);
    if (r->have_roster && r->cams_connected < r->cams_expected)
        find(&f, AS_DG_WARN, O_VISION, "get_vision; the camera's Ethernet and its static address (10.TE.AM.11-14)",
             "%d of %d cameras connected", r->cams_connected, r->cams_expected);

    /* the loop */
    if (r->have_loop) {
        if (r->loop_over || (fin(r->loop_avg_ms) && r->loop_avg_ms > 20))
            find(&f, AS_DG_WARN, O_LOOP,
                 "Systemcore temperature (throttling), blocking calls in periodic, too many status reads",
                 "the loop is over its 20 ms budget (last %.1f, average %.1f, worst %.1f ms)", r->loop_last_ms,
                 r->loop_avg_ms, r->loop_max_ms);
        else if (fin(r->loop_avg_ms) && r->loop_avg_ms > 15)
            find(&f, AS_DG_INFO, O_LOOP, NULL, "the loop averages %.1f ms: over three quarters of its budget",
                 r->loop_avg_ms);
    }

    /* the robot's own checks */
    if (r->have_preflight && !r->preflight_ready)
        find(&f, AS_DG_FAULT, O_CHECKS, "get_alerts lists every finding", "the robot's own preflight says not ready: %s",
             r->preflight_summary);
    for (int i = 0, k = 0; i < r->nfindings && k < 3; i++)
        if (r->findings[i].sev == CAT_SEV_ERROR) {
            find(&f, AS_DG_FAULT, O_CHECKS, NULL, "preflight blocker: %s", r->findings[i].text);
            k++;
        }
    if (r->have_systemcheck && !r->systemcheck_ready)
        find(&f, AS_DG_WARN, O_CHECKS, "the System Check report (read_topics on /Catalyst/SystemCheck/...)",
             "the last System Check did not pass: %s", r->systemcheck_report);
    if (r->have_start_check && !r->start_ready && !r->enabled)
        find(&f, AS_DG_WARN, O_CHECKS, "place the robot on the selected auto's start pose; check the auto choice",
             "the robot isn't at the selected auto's start (%.2f m, %.0f deg off)", r->start_dist_m, r->start_heading_deg);
    if (r->build_commit[0] && r->build_dirty)
        find(&f, AS_DG_INFO, O_CHECKS, NULL, "the deployed build (%s) came from a tree with uncommitted changes",
             r->build_commit);

    /* alerts: the robot's own words, errors first */
    int ke = 0, kw = 0;
    for (int i = 0; i < r->nalerts; i++) {
        const cat_alert_t *a = &r->alerts[i];
        if (a->sev == CAT_SEV_ERROR && ke < 6) {
            find(&f, AS_DG_FAULT, O_ALERTS, NULL, "%s %s%s", a->source, a->text, a->health ? " (health check)" : "");
            ke++;
        } else if (a->sev == CAT_SEV_WARN && kw < 6) {
            find(&f, AS_DG_WARN, O_ALERTS, NULL, "%s %s%s", a->source, a->text, a->health ? " (health check)" : "");
            kw++;
        }
    }
    if (r->n_errors > ke || r->n_warnings > kw)
        find(&f, AS_DG_INFO, O_ALERTS, "get_alerts for all of them", "%d more alerts not listed here",
             r->n_errors - ke + r->n_warnings - kw);

    /* mechanisms */
    for (int i = 0; i < r->nmechs; i++) {
        const cat_mech_t *m = &r->mechs[i];
        if (fin(m->temp_c) && m->temp_c >= 80)
            find(&f, AS_DG_FAULT, O_MECH, "let it cool; a binding mechanism or a current limit set too high heats a motor",
                 "%s motor at %.0f C", m->name, m->temp_c);
        else if (fin(m->temp_c) && m->temp_c >= 70)
            find(&f, AS_DG_WARN, O_MECH, "cool it before the match; look for binding", "%s motor at %.0f C", m->name,
                 m->temp_c);
        if (r->enabled && fin(m->current_a) && m->current_a > 30 && fin(m->vel) && fabs(m->vel) < 0.02)
            find(&f, AS_DG_WARN, O_MECH, "by hand, disabled: does it move freely? a hard stop, a jam, a chain off",
                 "%s draws %.0f A without moving: stalled", m->name, m->current_a);
    }
    triage_swerve(&f, r);

    /* vision */
    if (r->have_vision && r->vision_level >= 1) {
        char bad[100] = "";
        for (int i = 0; i < r->ncameras; i++) {
            if (!strcmp(r->cameras[i].state, "OK") || !strcmp(r->cameras[i].state, "NO_TARGETS")) continue;
            size_t l = strlen(bad);
            if (l < sizeof bad - 30)
                snprintf(bad + l, sizeof bad - l, "%s%s %s", l ? ", " : "", r->cameras[i].name, r->cameras[i].state);
        }
        find(&f, r->vision_level >= 2 ? AS_DG_FAULT : AS_DG_WARN, O_VISION,
             "a DISCONNECTED camera on Systemcore is nearly always its address; a camera on Limelight OS 2026 reads nothing",
             "vision %s: %s%s%s", r->vision_level >= 2 ? "BLIND" : "DEGRADED", r->vision_summary, bad[0] ? " · " : "", bad);
    }

    /* Systemcore */
    if (r->have_sc) {
        if (fin(r->sc_temp) && r->sc_temp >= 80)
            find(&f, r->sc_temp >= 90 ? AS_DG_FAULT : AS_DG_WARN, O_CORE,
                 "airflow over the controller; a throttling core shows up as loop overruns",
                 "Systemcore at %.0f C", r->sc_temp);
        if (fin(r->sc_cpu) && r->sc_cpu >= 85)
            find(&f, r->sc_cpu >= 95 ? AS_DG_FAULT : AS_DG_WARN, O_CORE, "systemcore_health: which process",
                 "Systemcore CPU at %.0f%%", r->sc_cpu);
        if (fin(r->sc_ram) && r->sc_ram >= 0.85)
            find(&f, r->sc_ram >= 0.95 ? AS_DG_FAULT : AS_DG_WARN, O_CORE, "systemcore_health: the robot program's memory",
                 "Systemcore memory %.0f%% used", r->sc_ram * 100);
        if (fin(r->sc_storage) && r->sc_storage >= 0.85)
            find(&f, r->sc_storage >= 0.93 ? AS_DG_FAULT : AS_DG_WARN, O_CORE,
                 "systemcore_health: which directory (logs, usually); a full disk stops logging, then the program",
                 "Systemcore storage %.0f%% full", r->sc_storage * 100);
        if (r->sc_emmc >= 2)
            find(&f, r->sc_emmc >= 3 ? AS_DG_FAULT : AS_DG_WARN, O_CORE, "order a replacement controller; it runs for now",
                 "Systemcore flash wear pre-EOL %d (%s)", r->sc_emmc, r->sc_emmc >= 3 ? "urgent" : "warning");
    }

    qsort(out, (size_t)f.n, sizeof *out, by_rank);
    return f.n;
}

/* ---------------------------------------------------------------- swerve tracking */

void as_track_init(as_track_t *t, int nmod)
{
    memset(t, 0, sizeof *t);
    t->nmod = nmod < 0 ? 0 : nmod > 4 ? 4 : nmod;
    for (int i = 0; i < 4; i++) t->worst_ratio[i] = NAN;
}

void as_track_add(as_track_t *t, const double cs[4], const double ca[4], const double as[4], const double aa[4])
{
    t->samples++;
    for (int i = 0; i < t->nmod; i++) {
        if (!fin(cs[i]) || !fin(ca[i]) || !fin(as[i]) || !fin(aa[i])) continue;
        double cmd, along, ang;
        compare(cs[i], ca[i], as[i], aa[i], &cmd, &along, &ang);
        if (cmd < AS_TRACK_MIN_MPS) continue;
        t->used[i]++;
        t->sum_cmd[i] += cmd;
        t->sum_along[i] += along;
        t->sum_err2[i] += (along - cmd) * (along - cmd);
        double deg = ang * 180 / M_PI;
        t->sum_angle[i] += deg;
        if (deg > t->max_angle[i]) t->max_angle[i] = deg;
        double ratio = along / cmd;
        if (!fin(t->worst_ratio[i]) || ratio < t->worst_ratio[i]) t->worst_ratio[i] = ratio;
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

void as_track_judge(const as_track_t *t, as_track_verdict_t *v)
{
    memset(v, 0, sizeof *v);
    v->nmod = t->nmod;
    int judged = 0;
    double pct[4], med_pool[4];
    for (int i = 0; i < t->nmod; i++) {
        as_track_mod_t *m = &v->mod[i];
        m->used = t->used[i];
        if (t->used[i] < 5) {
            m->follow_pct = m->rms_err = m->angle_err_deg = m->worst_angle_deg = m->worst_follow_pct = NAN;
            m->verdict = "can't judge";
            pct[i] = NAN;
            continue;
        }
        m->follow_pct = 100 * t->sum_along[i] / t->sum_cmd[i];
        m->rms_err = sqrt(t->sum_err2[i] / t->used[i]);
        m->angle_err_deg = t->sum_angle[i] / t->used[i];
        m->worst_angle_deg = t->max_angle[i];
        m->worst_follow_pct = 100 * t->worst_ratio[i];
        pct[i] = m->follow_pct;
        med_pool[judged++] = m->follow_pct;
    }
    if (judged < 2) {
        snprintf(v->summary, sizeof v->summary,
                 "not enough driving to judge: fewer than two modules were commanded over %.1f m/s for five samples. "
                 "Drive it (or pick a recording where it drove) and look again.",
                 AS_TRACK_MIN_MPS);
        for (int i = 0; i < t->nmod; i++)
            if (!v->mod[i].verdict) v->mod[i].verdict = "can't judge";
        return;
    }
    qsort(med_pool, (size_t)judged, sizeof med_pool[0], cmp_d);
    double med = judged % 2 ? med_pool[judged / 2] : (med_pool[judged / 2 - 1] + med_pool[judged / 2]) / 2;
    int lag = -1, steer = -1, nlag = 0;
    for (int i = 0; i < t->nmod; i++) {
        as_track_mod_t *m = &v->mod[i];
        if (m->verdict) continue;
        bool lags = pct[i] < med - 12 || pct[i] < 75;
        bool steers = m->angle_err_deg > 8;
        m->verdict = steers ? "steering" : lags ? "lags" : "follows";
        if (lags) {
            nlag++;
            if (lag < 0 || pct[i] < pct[lag]) lag = i;
        }
        if (steers && (steer < 0 || m->angle_err_deg > v->mod[steer].angle_err_deg)) steer = i;
    }
    if (steer >= 0)
        snprintf(v->summary, sizeof v->summary,
                 "module %s steers %.0f deg off its command on average (worst %.0f): a steering fault, not a drive one. "
                 "Check its CANcoder offset and magnet, the azimuth motor and its gear by hand; then its steer gains.",
                 AS_MOD_NAME[steer], v->mod[steer].angle_err_deg, v->mod[steer].worst_angle_deg);
    else if (lag >= 0 && pct[lag] < 0)
        snprintf(v->summary, sizeof v->summary,
                 "module %s runs against its command (%.0f%%): its drive direction is inverted somewhere, or its "
                 "reported speed is. Check that module's drive inversion and encoder before anything else.",
                 AS_MOD_NAME[lag], pct[lag]);
    else if (nlag == 1 || (nlag >= 1 && nlag < judged && med >= 85))
        snprintf(v->summary, sizeof v->summary,
                 "module %s does %.0f%% of its commanded speed while the others do about %.0f%%: one module that doesn't "
                 "follow is hardware until proven otherwise. By hand, disabled: does that wheel spin freely, is the wheel "
                 "or pulley loose on its shaft (set screw, key), tread worn, the gearbox binding? Then that drive motor's "
                 "temperature, current and CAN. Don't retune the drivetrain for one module.",
                 AS_MOD_NAME[lag], pct[lag], med);
    else if (nlag == judged || med < 85)
        snprintf(v->summary, sizeof v->summary,
                 "every module under-follows alike (about %.0f%% of command): that is global, not one wheel. Suspects, "
                 "in order: battery sag under load, drive current limits (slip current), drive kV/kS feedforward, "
                 "the configured wheel radius or gear ratio, then the velocity gains.",
                 med);
    else
        snprintf(v->summary, sizeof v->summary,
                 "all modules follow their commands (about %.0f%% of commanded speed, steering within a few degrees): the "
                 "drivetrain hardware looks healthy in this data; look at the path, the driver input or odometry instead.",
                 med);
}

/* ---- the recorder's CSV ---- */

static int field_index(const char *line, const char *name)
{
    int col = 0;
    size_t ln = strlen(name);
    const char *p = line;
    for (;;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strcspn(p, "\r\n");
        if (len == ln && !strncmp(p, name, ln)) return col;
        if (!e) return -1;
        p = e + 1;
        col++;
    }
}

bool as_track_csv_header(const char *line, int cols[16])
{
    static const char *const EL[8] = { "fl_speed", "fl_angle", "fr_speed", "fr_angle",
                                       "bl_speed", "bl_angle", "br_speed", "br_angle" };
    char name[64];
    bool any_state = false, any_target = false;
    for (int i = 0; i < 16; i++) {
        snprintf(name, sizeof name, "Swerve/%s.%s", i < 8 ? "ModuleStates" : "ModuleTargets", EL[i % 8]);
        cols[i] = field_index(line, name);
        if (cols[i] >= 0) {
            if (i < 8) any_state = true;
            else any_target = true;
        }
    }
    return any_state && any_target;
}

bool as_track_csv_row(as_track_t *t, const char *line, const int cols[16])
{
    double v[16];
    for (int i = 0; i < 16; i++) v[i] = NAN;
    int col = 0;
    const char *p = line;
    for (;;) {
        const char *e = strchr(p, ',');
        for (int i = 0; i < 16; i++) {
            if (cols[i] != col) continue;
            if (*p && *p != ',' && *p != '\r' && *p != '\n') {
                char *end;
                double x = strtod(p, &end);
                if (end != p) v[i] = x;
            }
        }
        if (!e) break;
        p = e + 1;
        col++;
    }
    double cs[4], ca[4], as[4], aa[4];
    bool any = false;
    for (int m = 0; m < 4; m++) {
        as[m] = v[2 * m];
        aa[m] = v[2 * m + 1];
        cs[m] = v[8 + 2 * m];
        ca[m] = v[8 + 2 * m + 1];
        any |= fin(as[m]) && fin(cs[m]);
    }
    if (!any) return false;
    as_track_add(t, cs, ca, as, aa);
    return true;
}
