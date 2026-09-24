/* as_tools — the assistant's tools, run on the worker thread against its own copy of the robot.
 *
 * Results are compact JSON under ~8 KB. Reading tools touch only the copy, nt4's thread-safe getters,
 * files on microSD and HTTP; the four that change something show a confirmation card first and do
 * nothing unless the technician approves it. */
#include "as_tools.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "as_snap.h"
#include "cat_logs.h"
#include "cat_preflight.h"
#include "hal.h"
#include "link.h"
#include "nt4.h"

#define AGENT_PORT 9010

static void sum(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void sum(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, n, fmt, ap);
    va_end(ap);
}

/* ---- JSON helpers: `,"key":value` for members after the first; each caller opens its own object ---- */

static void key(ab_t *o, const char *k)
{
    if (o->n && o->p[o->n - 1] != '{' && o->p[o->n - 1] != '[') ab_puts(o, ",");
    ab_str(o, k);
    ab_puts(o, ":");
}
static void kstr(ab_t *o, const char *k, const char *v) { key(o, k); ab_str(o, v ? v : ""); }
static void kbool(ab_t *o, const char *k, bool v) { key(o, k); ab_puts(o, v ? "true" : "false"); }
static void kint(ab_t *o, const char *k, long v) { key(o, k); ab_fmt(o, "%ld", v); }
/* a number rounded to `dp` places; null when absent (NaN) */
static void knum(ab_t *o, const char *k, double v, int dp)
{
    key(o, k);
    if (!isfinite(v)) ab_puts(o, "null");
    else ab_fmt(o, "%.*f", dp, v);
}
/* a number as short as it can be written */
static void kval(ab_t *o, const char *k, double v)
{
    key(o, k);
    ab_num(o, v);
}
static void comma(ab_t *o)
{
    if (o->n && o->p[o->n - 1] != '{' && o->p[o->n - 1] != '[') ab_puts(o, ",");
}

static const char *sev_name(cat_sev_t s) { return s == CAT_SEV_ERROR ? "error" : s == CAT_SEV_WARN ? "warning" : "info"; }
static const char *band_name(double v)
{
    int b = cat_battery_band(v);
    return b == 2 ? "charged" : b == 1 ? "swap before a match" : "low";
}

static bool robot_online(as_env_t *e, ab_t *out, char *s, size_t sn)
{
    if (e->robot->connected) return true;
    ab_puts(out, "{");
    kbool(out, "connected", false);
    kstr(out, "tried", e->robot->address);
    kstr(out, "note", "No NetworkTables connection to the robot: nothing live to read. Check the radio or "
                      "tether, that the robot is on, and the team number in settings.");
    ab_puts(out, "}");
    sum(s, sn, "the robot isn't connected");
    return false;
}

static void robot_brief(ab_t *o, const cat_robot_t *r)
{
    ab_puts(o, "{");
    kint(o, "team", r->team);
    kstr(o, "name", r->name);
    kstr(o, "mode", cat_mode_name(r));
    knum(o, "battery", r->have_battery ? r->battery_v : NAN, 2);
    kint(o, "errors", r->n_errors);
    kint(o, "warnings", r->n_warnings);
    kstr(o, "catalyst", r->catalyst_version);
    kstr(o, "code", r->code_version);
    ab_puts(o, "}");
}

/* ---- the robot ---- */

static as_tres_t t_overview(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    kbool(o, "connected", r->connected);
    kstr(o, "address", r->address);
    if (r->connected) {
        knum(o, "rtt_ms", r->rtt_ms, 1);
        kint(o, "topics", (long)r->topics);
        knum(o, "connected_s", r->connected_s, 0);

        key(o, "identity");
        ab_puts(o, "{");
        kstr(o, "name", r->name);
        kint(o, "team", r->team);
        kstr(o, "controller", r->controller[0] ? r->controller : r->line2 ? "Systemcore" : "roboRIO");
        kstr(o, "catalyst_line", r->line2 ? "2.x" : "1.x");
        kstr(o, "catalyst", r->catalyst_version);
        kstr(o, "robot_code", r->code_version);
        kstr(o, "wpilib", r->wpilib_version);
        kstr(o, "git_sha", r->git_sha);
        if (r->build_commit[0]) {
            kstr(o, "deployed_commit", r->build_commit);
            kbool(o, "deployed_dirty", r->build_dirty);
        }
        ab_puts(o, "}");

        key(o, "mode");
        ab_puts(o, "{");
        kstr(o, "mode", cat_mode_name(r));
        kbool(o, "program_running", r->have_mode);
        kbool(o, "enabled", r->enabled);
        kbool(o, "estop", r->estop);
        kbool(o, "ds_attached", r->ds);
        kbool(o, "fms", r->fms);
        if (r->have_alliance) {
            key(o, "alliance");
            ab_fmt(o, "\"%s %d\"", r->red ? "red" : "blue", r->station);
        }
        if (r->opmode[0]) kstr(o, "opmode", r->opmode);
        knum(o, "match_time", r->match_time, 1);
        ab_puts(o, "}");

        key(o, "power");
        ab_puts(o, "{");
        if (r->have_battery) {
            knum(o, "battery_v", r->battery_v, 2);
            kstr(o, "band", band_name(r->battery_v));
        } else kstr(o, "battery_v", "not published");
        knum(o, "brownout_floor_v", r->brownout_v, 2);
        knum(o, "predicted_v", r->predicted_v, 2);
        knum(o, "total_current_a", r->total_current, 1);
        kbool(o, "browned_out_now", r->browned_out);
        kbool(o, "brownout_risk", r->brownout_risk);
        ab_puts(o, "}");

        if (r->have_loop) {
            key(o, "loop");
            ab_puts(o, "{");
            knum(o, "last_ms", r->loop_last_ms, 1);
            knum(o, "avg_ms", r->loop_avg_ms, 1);
            knum(o, "max_ms", r->loop_max_ms, 1);
            kbool(o, "over_budget", r->loop_over);
            ab_puts(o, "}");
        }

        key(o, "can");
        ab_puts(o, "{");
        key(o, "utilization");
        ab_puts(o, "[");
        for (int i = 0; i < r->ncan_util; i++) {
            comma(o);
            ab_fmt(o, "%.2f", r->can_util[i]);
        }
        ab_puts(o, "]");
        kbool(o, "bus_down", r->can_down);
        kint(o, "devices", r->ndevices);
        if (r->have_roster) {
            key(o, "motors");
            ab_fmt(o, "\"%d/%d connected\"", r->motors_connected, r->motors_expected);
            key(o, "cameras");
            ab_fmt(o, "\"%d/%d connected\"", r->cams_connected, r->cams_expected);
        }
        if (r->have_controller) {
            key(o, "gamepad");
            ab_fmt(o, "\"%s, %s\"", r->controller_kind, r->controller_connected ? "connected" : "NOT connected");
        }
        ab_puts(o, "}");

        key(o, "alerts");
        ab_puts(o, "{");
        kint(o, "errors", r->n_errors);
        kint(o, "warnings", r->n_warnings);
        kint(o, "info", r->n_infos);
        ab_puts(o, "}");

        if (r->have_sc) {
            key(o, "systemcore");
            ab_puts(o, "{");
            knum(o, "cpu_pct", r->sc_cpu, 0);
            knum(o, "temp_c", r->sc_temp, 1);
            knum(o, "ram_pct", r->sc_ram * 100, 0);
            knum(o, "storage_pct", r->sc_storage * 100, 0);
            kint(o, "emmc_pre_eol", r->sc_emmc);
            ab_puts(o, "}");
        }
        if (r->have_vision) {
            key(o, "vision");
            ab_puts(o, "{");
            kstr(o, "level", r->vision_level == 0 ? "OK" : r->vision_level == 1 ? "DEGRADED" : "BLIND");
            kstr(o, "summary", r->vision_summary);
            ab_puts(o, "}");
        }
        key(o, "robot_checks");
        ab_puts(o, "{");
        if (r->have_preflight) {
            kbool(o, "preflight_ready", r->preflight_ready);
            kstr(o, "preflight_summary", r->preflight_summary);
        }
        if (r->have_systemcheck) {
            kbool(o, "systemcheck_ready", r->systemcheck_ready);
            kstr(o, "systemcheck_report", r->systemcheck_report);
        }
        if (r->have_wheel_radius) kstr(o, "wheel_radius", r->wheel_radius_status);
        if (r->have_start_check) kbool(o, "auto_start_ready", r->start_ready);
        ab_puts(o, "}");
        if (r->have_autos) kstr(o, "auto_selected", r->auto_selected);
        kint(o, "tunables", r->ntunables);
    }

    hal_net_t net;
    hal_tether_t th;
    hal_net(&net);
    hal_tether(&th);
    key(o, "tablet");
    ab_puts(o, "{");
    kstr(o, "path_to_robot", net.link == HAL_LINK_USB ? "usb tether" : net.link == HAL_LINK_WIFI ? "wifi" : "simulator");
    kstr(o, "wifi", net.up ? net.ssid : "not connected");
    if (net.up) kint(o, "rssi_dbm", net.rssi);
    if (th.present) {
        key(o, "tether");
        ab_fmt(o, "\"%s %s%s\"", th.kind, th.up ? th.ip : "down", th.up && !th.dhcp ? " (fallback address)" : "");
    }
    ab_puts(o, "}");

    link_status_t ls;
    link_status(&ls);
    key(o, "catalyst_link");
    ab_puts(o, "{");
    kbool(o, "configured", ls.configured);
    kbool(o, "reachable", ls.reachable);
    if (ls.reachable) kbool(o, "token_ok", ls.auth);
    if (ls.reachable && ls.auth) {
        kstr(o, "pc", ls.name);
        kstr(o, "repo", ls.repo);
        kstr(o, "branch", ls.branch);
        kint(o, "inbox_open", ls.inbox_open);
        kint(o, "patches", ls.patches);
    }
    kint(o, "outbox_waiting", ls.outbox);
    ab_puts(o, "}");
    ab_puts(o, "}");
    if (!r->connected) sum(s, sn, "robot offline");
    else if (r->have_battery) sum(s, sn, "%s · %s · %.2f V", r->name[0] ? r->name : "robot", cat_mode_name(r), r->battery_v);
    else sum(s, sn, "%s · %s · no battery reading", r->name[0] ? r->name : "robot", cat_mode_name(r));
    return AS_TR_OK;
}

static as_tres_t t_alerts(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    kint(o, "errors", r->n_errors);
    kint(o, "warnings", r->n_warnings);
    kint(o, "info", r->n_infos);
    key(o, "alerts");
    ab_puts(o, "[");
    for (int sev = CAT_SEV_ERROR; sev >= CAT_SEV_INFO; sev--) {
        for (int i = 0; i < r->nalerts; i++) {
            const cat_alert_t *a = &r->alerts[i];
            if ((int)a->sev != sev) continue;
            comma(o);
            ab_puts(o, "{");
            kstr(o, "sev", sev_name(a->sev));
            kstr(o, "source", a->source);
            kstr(o, "text", a->text);
            if (a->health) kbool(o, "health_check", true);
            ab_puts(o, "}");
        }
    }
    ab_puts(o, "]");
    if (r->have_preflight) {
        key(o, "robot_preflight");
        ab_puts(o, "{");
        kbool(o, "ready", r->preflight_ready);
        kstr(o, "summary", r->preflight_summary);
        key(o, "findings");
        ab_puts(o, "[");
        for (int i = 0; i < r->nfindings; i++) {
            char f[140];
            snprintf(f, sizeof f, "%s: %s", r->findings[i].sev == CAT_SEV_ERROR ? "BLOCKER" : sev_name(r->findings[i].sev),
                     r->findings[i].text);
            comma(o);
            ab_str(o, f);
        }
        ab_puts(o, "]}");
    }
    ab_puts(o, "}");
    sum(s, sn, "%d errors, %d warnings", r->n_errors, r->n_warnings);
    return AS_TR_OK;
}

static const char *mech_kind(cat_mech_kind_t k)
{
    return k == CAT_MECH_ROTATIONAL ? "rotational" : k == CAT_MECH_LINEAR ? "linear" : k == CAT_MECH_FLYWHEEL ? "flywheel" : "other";
}

static as_tres_t t_mechs(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    key(o, "mechanisms");
    ab_puts(o, "[");
    for (int i = 0; i < r->nmechs; i++) {
        const cat_mech_t *m = &r->mechs[i];
        comma(o);
        ab_puts(o, "{");
        kstr(o, "name", m->name);
        kstr(o, "kind", mech_kind(m->kind));
        if (m->state[0]) kstr(o, "state", m->state);
        kstr(o, "unit", m->unit);
        knum(o, "pos", m->pos, 3);
        knum(o, "goal", m->setpoint, 3);
        if (isfinite(m->pos) && isfinite(m->setpoint)) knum(o, "error", m->pos - m->setpoint, 3);
        knum(o, "vel", m->vel, 2);
        knum(o, "current_a", m->current_a, 1);
        knum(o, "temp_c", m->temp_c, 1);
        key(o, "at_goal");
        ab_puts(o, m->at_goal < 0 ? "null" : m->at_goal ? "true" : "false");
        ab_puts(o, "}");
    }
    ab_puts(o, "]");
    if (r->nmodules || r->have_pose) {
        key(o, "swerve");
        ab_puts(o, "{");
        knum(o, "heading_deg", r->heading_deg, 1);
        if (r->have_pose) {
            key(o, "pose");
            ab_fmt(o, "[%.2f,%.2f,%.1f]", r->pose_x, r->pose_y, r->pose_rad * 180 / M_PI);
        }
        key(o, "modules");
        ab_puts(o, "[");
        for (int i = 0; i < r->nmodules && i < 4; i++) {
            comma(o);
            ab_fmt(o, "{\"speed\":%.2f,\"angle_deg\":%.1f", r->module_speed[i], r->module_angle[i] * 180 / M_PI);
            if (r->have_targets)
                ab_fmt(o, ",\"target_speed\":%.2f,\"target_angle_deg\":%.1f", r->target_speed[i], r->target_angle[i] * 180 / M_PI);
            ab_puts(o, "}");
        }
        ab_puts(o, "]}");
    }
    ab_puts(o, "}");
    sum(s, sn, "%d mechanisms", r->nmechs);
    return AS_TR_OK;
}

static as_tres_t t_power(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    knum(o, "battery_v", r->have_battery ? r->battery_v : NAN, 2);
    if (r->have_battery) kstr(o, "band", band_name(r->battery_v));
    knum(o, "brownout_floor_v", r->brownout_v, 2);
    knum(o, "predicted_v", r->predicted_v, 2);
    knum(o, "total_current_a", r->total_current, 1);
    kbool(o, "browned_out_now", r->browned_out);
    kbool(o, "brownout_risk", r->brownout_risk);
    kstr(o, "module", r->pd_module);
    kbool(o, "pdh_live", r->have_pd_live);
    if (r->have_pd_live) {
        knum(o, "pdh_voltage", r->pd_voltage, 2);
        knum(o, "pdh_total_a", r->pd_total, 1);
    }
    key(o, "channels");
    ab_puts(o, "[");
    bool listed[24] = { 0 };
    for (int i = 0; i < r->nchannels; i++) {
        const cat_channel_t *c = &r->channels[i];
        comma(o);
        ab_fmt(o, "{\"ch\":%d,\"feeds\":", c->channel);
        ab_str(o, c->what);
        double a = c->channel >= 0 && c->channel < 24 ? r->pd_amps[c->channel] : NAN;
        if (isfinite(a)) ab_fmt(o, ",\"amps\":%.1f", a);
        ab_puts(o, "}");
        if (c->channel >= 0 && c->channel < 24) listed[c->channel] = true;
    }
    /* current on a channel nobody declared is worth knowing about */
    for (int ch = 0; r->have_pd_live && ch < 24; ch++) {
        if (listed[ch] || !(r->pd_amps[ch] > 0.5)) continue;
        comma(o);
        ab_fmt(o, "{\"ch\":%d,\"feeds\":\"(undeclared)\",\"amps\":%.1f}", ch, r->pd_amps[ch]);
    }
    ab_puts(o, "]}");
    if (r->have_battery) sum(s, sn, "%.2f V, %.0f A", r->battery_v, r->total_current);
    else sum(s, sn, "no battery reading");
    return AS_TR_OK;
}

static as_tres_t t_can(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    key(o, "utilization");
    ab_puts(o, "{");
    for (int i = 0; i < r->ncan_util; i++) {
        char k[16];
        snprintf(k, sizeof k, r->ncan_util > 1 ? "can_s%d" : "bus%d", i);
        knum(o, k, r->can_util[i], 2);
    }
    ab_puts(o, "}");
    kbool(o, "bus_down", r->can_down);
    if (r->have_roster) {
        key(o, "roster");
        ab_fmt(o, "{\"motors\":\"%d/%d\",\"cameras\":\"%d/%d\"}", r->motors_connected, r->motors_expected,
               r->cams_connected, r->cams_expected);
    }
    kstr(o, "device_columns", "[id, type, name, connected (null: this robot doesn't report it), temp_c, amps]");
    key(o, "devices_by_bus");
    ab_puts(o, "{");
    int missing = 0;
    for (int i = 0; i < r->ndevices; i++) {
        const cat_device_t *d = &r->devices[i];
        bool first = true;
        for (int k = 0; k < i; k++) if (!strcmp(r->devices[k].bus, d->bus)) { first = false; break; }
        if (!first) continue;
        key(o, d->bus[0] ? d->bus : "?");
        ab_puts(o, "[");
        for (int k = i; k < r->ndevices; k++) {
            const cat_device_t *x = &r->devices[k];
            if (strcmp(x->bus, d->bus) != 0) continue;
            comma(o);
            ab_fmt(o, "[%d,", x->id);
            ab_str(o, x->type);
            ab_puts(o, ",");
            ab_str(o, x->name);
            ab_puts(o, x->connected < 0 ? ",null" : x->connected ? ",true" : ",false");
            if (isfinite(x->temp_c) || isfinite(x->current_a)) {
                ab_puts(o, ",");
                ab_num(o, isfinite(x->temp_c) ? round(x->temp_c * 10) / 10 : NAN);
                ab_puts(o, ",");
                ab_num(o, isfinite(x->current_a) ? round(x->current_a * 10) / 10 : NAN);
            }
            ab_puts(o, "]");
            missing += x->connected == 0;
        }
        ab_puts(o, "]");
    }
    ab_puts(o, "}}");
    sum(s, sn, "%d devices, %d not answering", r->ndevices, missing);
    return AS_TR_OK;
}

static as_tres_t t_vision(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    kbool(o, "published", r->have_vision);
    if (r->have_vision) {
        kstr(o, "level", r->vision_level == 0 ? "OK" : r->vision_level == 1 ? "DEGRADED" : "BLIND");
        kstr(o, "summary", r->vision_summary);
    }
    key(o, "cameras");
    ab_puts(o, "[");
    for (int i = 0; i < r->ncameras; i++) {
        const cat_camera_t *c = &r->cameras[i];
        comma(o);
        ab_puts(o, "{");
        kstr(o, "name", c->name);
        kstr(o, "state", c->state);
        kstr(o, "detail", c->detail);
        knum(o, "fps", c->fps, 0);
        knum(o, "temp_c", c->temp_c, 0);
        kbool(o, "connected", c->connected);
        ab_puts(o, "}");
    }
    ab_puts(o, "]");
    key(o, "tag_in_view");
    if (r->tag_seen >= 0) ab_fmt(o, "%d", r->tag_seen);
    else ab_puts(o, "null");
    ab_puts(o, "}");
    sum(s, sn, "%d cameras, %s", r->ncameras, r->have_vision ? r->vision_summary : "no vision health published");
    return AS_TR_OK;
}

static as_tres_t t_preflight(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    cat_preflight_t *p = malloc(sizeof *p);
    if (!p) return AS_TR_ERROR;
    int team = e->team > 0 ? e->team : e->robot->team;
    cat_preflight_start(p, hal_seconds(), team);
    double end = hal_seconds() + 6;
    bool done = false;
    while (!done && hal_seconds() < end) {
        if (e->stopped && e->stopped(e->user)) break;
        e->refresh(e->robot);
        done = cat_preflight_step(p, e->robot, hal_seconds());
        if (!done) usleep(100000);
    }
    if (!done) {
        free(p);
        ab_puts(o, "{\"error\":\"preflight didn't finish\"}");
        sum(s, sn, "didn't finish");
        return AS_TR_ERROR;
    }
    static const char *vn[] = { "PASS", "WARN", "FAIL", "INFO" };
    ab_puts(o, "{");
    kstr(o, "verdict", p->fails ? "NO-GO" : "GO");
    kint(o, "fails", p->fails);
    kint(o, "warnings", p->warns);
    kint(o, "passes", p->passes);
    key(o, "checks");
    ab_puts(o, "[");
    static const cat_verdict_t order[] = { CAT_FAIL, CAT_WARN, CAT_INFO, CAT_PASS };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < p->n; i++) {
            if (p->c[i].v != order[k]) continue;
            comma(o);
            ab_fmt(o, "[\"%s\",", vn[p->c[i].v]);
            ab_str(o, p->c[i].what);
            ab_puts(o, ",");
            ab_str(o, p->c[i].detail);
            ab_puts(o, "]");
        }
    }
    ab_puts(o, "]}");
    sum(s, sn, "%s · %d fail, %d warn", p->fails ? "NO-GO" : "GO", p->fails, p->warns);
    free(p);
    return AS_TR_OK;
}

/* ---- NetworkTables directly ---- */

typedef struct {
    ab_t *o;
    int n, max;
} visit_t;

static void visit(const char *name, const char *type, void *user)
{
    visit_t *v = user;
    if (v->n++ >= v->max) return;
    comma(v->o);
    ab_puts(v->o, "[");
    ab_str(v->o, name);
    ab_puts(v->o, ",");
    ab_str(v->o, type);
    ab_puts(v->o, "]");
}

static as_tres_t t_list_topics(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    nt4_client_t *nt = cat_nt();
    if (!nt || !robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const char *prefix = aj_gets(in, "prefix");
    ab_puts(o, "{");
    kstr(o, "prefix", prefix);
    key(o, "topics");
    ab_puts(o, "[");
    visit_t v = { o, 0, 200 };
    nt4_list(nt, prefix, visit, &v);
    ab_puts(o, "]");
    kint(o, "count", v.n);
    if (v.n > v.max) kbool(o, "truncated", true);
    ab_puts(o, "}");
    sum(s, sn, "%d topics under %s", v.n, prefix);
    return AS_TR_OK;
}

static as_tres_t t_read_topics(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    nt4_client_t *nt = cat_nt();
    if (!nt || !robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const aj_t *names = aj_get(in, "names");
    char *buf = malloc(4096);
    if (!buf) return AS_TR_ERROR;
    int found = 0;
    ab_puts(o, "{");
    for (int i = 0; i < names->n; i++) {
        const char *name = names->kid[i]->s;
        char type[48] = "";
        uint32_t seq = 0;
        int64_t age = 0;
        key(o, name);
        if (!nt4_info(nt, name, type, sizeof type, &seq, &age) || !seq) {
            ab_puts(o, "null");
            continue;
        }
        found++;
        ab_puts(o, "{");
        kstr(o, "type", type);
        knum(o, "age_s", age / 1e6, 1);
        key(o, "value");
        size_t tl = strlen(type);
        bool arr = tl > 2 && !strcmp(type + tl - 2, "[]");
        double num;
        bool b;
        if (!strcmp(type, "string[]")) {
            const char *items[64];
            int k = nt4_get_strings(nt, name, buf, 4096, items, 64);
            ab_puts(o, "[");
            for (int j = 0; j < k; j++) {
                comma(o);
                ab_str(o, items[j]);
            }
            ab_puts(o, "]");
        } else if (arr || (!strncmp(type, "struct:", 7) && !(nt4_get_numbers(nt, name, &num, 1) <= 0 && nt4_get_number(nt, name, &num)))) {
            /* decoded structs read as runs of doubles; a one-number struct (ControlWord) falls through */
            double v[64];
            int k = nt4_get_numbers(nt, name, v, 64);
            ab_puts(o, "[");
            for (int j = 0; j < k; j++) {
                comma(o);
                ab_num(o, v[j]);
            }
            ab_puts(o, "]");
        } else if (!strcmp(type, "boolean") && nt4_get_bool(nt, name, &b)) {
            ab_puts(o, b ? "true" : "false");
        } else if ((!strcmp(type, "string") || !strcmp(type, "json")) && nt4_get_string(nt, name, buf, 4096)) {
            if (strlen(buf) > 1500) strcpy(buf + 1497, "…");
            ab_str(o, buf);
        } else if (nt4_get_number(nt, name, &num)) {
            ab_num(o, num);
        } else {
            ab_puts(o, "\"(binary)\"");
        }
        ab_puts(o, "}");
    }
    ab_puts(o, "}");
    free(buf);
    sum(s, sn, "%d of %d topics", found, names->n);
    return AS_TR_OK;
}

/* ---- tuning and autos ---- */

static int tunable_index(const cat_robot_t *r, const char *k)
{
    for (int i = 0; i < r->ntunables; i++) if (!strcmp(r->tunables[i].key, k)) return i;
    return -1;
}

static void fmt_value(char *out, size_t n, double v, bool is_bool)
{
    if (is_bool) snprintf(out, n, "%s", v != 0 ? "true" : "false");
    else if (!isfinite(v)) snprintf(out, n, "—");
    else snprintf(out, n, "%g", v);
}

static as_tres_t t_list_tunables(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    kbool(o, "manifest", r->have_manifest);
    key(o, "tunables");
    ab_puts(o, "[");
    for (int i = 0; i < r->ntunables; i++) {
        const cat_tunable_t *t = &r->tunables[i];
        comma(o);
        ab_puts(o, "{");
        kstr(o, "key", t->key);
        if (t->name[0]) kstr(o, "name", t->name);
        if (t->group[0]) kstr(o, "group", t->group);
        if (t->unit[0]) kstr(o, "unit", t->unit);
        if (t->is_bool) kbool(o, "bool", true);
        else {
            if (isfinite(t->min)) kval(o, "min", t->min);
            if (isfinite(t->max)) kval(o, "max", t->max);
            if (isfinite(t->step)) kval(o, "step", t->step);
        }
        key(o, "value");
        if (!t->have) ab_puts(o, "null");
        else if (t->is_bool) ab_puts(o, t->value != 0 ? "true" : "false");
        else ab_num(o, t->value);
        ab_puts(o, "}");
    }
    ab_puts(o, "]}");
    sum(s, sn, "%d tunables", r->ntunables);
    return AS_TR_OK;
}

static as_tres_t t_list_autos(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const cat_robot_t *r = e->robot;
    ab_puts(o, "{");
    kbool(o, "chooser", r->have_autos);
    key(o, "options");
    ab_puts(o, "[");
    for (int i = 0; i < r->nautos; i++) {
        comma(o);
        ab_str(o, r->autos[i]);
    }
    ab_puts(o, "]");
    kstr(o, "selected", r->auto_selected);
    kstr(o, "active", r->auto_active);
    ab_puts(o, "}");
    sum(s, sn, "%d autos, selected %s", r->nautos, r->auto_selected[0] ? r->auto_selected : "none");
    return AS_TR_OK;
}

static as_tres_t t_list_snapshots(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    snap_info_t *list = malloc(SNAP_MAX * sizeof *list);
    char *diff = malloc(1024);
    if (!list || !diff) {
        free(list);
        free(diff);
        return AS_TR_ERROR;
    }
    int n = snap_list(list, SNAP_MAX);
    ab_puts(o, "{");
    key(o, "snapshots");
    ab_puts(o, "[");
    for (int i = 0; i < n; i++) {
        comma(o);
        ab_puts(o, "{");
        kint(o, "id", list[i].id);
        kstr(o, "when", list[i].when);
        kstr(o, "reason", list[i].reason);
        kint(o, "tunables", list[i].count);
        if (i < 5 && e->robot->connected) {
            int d = snap_diff(e->robot, list[i].id, diff, 1024);
            kint(o, "differ_now", d);
            if (d > 0) kstr(o, "differences", diff);
        }
        ab_puts(o, "}");
    }
    ab_puts(o, "]}");
    free(list);
    free(diff);
    sum(s, sn, "%d snapshots", n);
    return AS_TR_OK;
}

/* ---- logs on microSD ---- */

static bool log_name_ok(const char *n)
{
    return n && n[0] && !strchr(n, '/') && !strchr(n, '\\') && !strstr(n, "..");
}

static bool is_log(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcmp(dot, ".wpilog") || !strcmp(dot, ".dslog") || !strcmp(dot, ".dsevents"));
}

static int cmp_desc(const void *a, const void *b) { return -strcmp(*(char *const *)a, *(char *const *)b); }

static as_tres_t t_list_logs(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    (void)e;
    const char *root = hal_sd_root();
    char dir[160];
    if (!root) {
        ab_puts(o, "{\"error\":\"no microSD card\"}");
        sum(s, sn, "no microSD card");
        return AS_TR_ERROR;
    }
    snprintf(dir, sizeof dir, "%s/logs", root);
    DIR *d = opendir(dir);
    char *names[200];
    int n = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < 200) if (is_log(de->d_name)) names[n++] = as_strdup(de->d_name);
        closedir(d);
    }
    qsort(names, (size_t)n, sizeof *names, cmp_desc);
    ab_puts(o, "{");
    kstr(o, "folder", "logs/");
    key(o, "logs");
    ab_puts(o, "[");
    for (int i = 0; i < n; i++) {
        if (i < 60) {
            char p[320];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            comma(o);
            ab_puts(o, "[");
            ab_str(o, names[i]);
            ab_fmt(o, ",%ld]", stat(p, &st) == 0 ? (long)st.st_size : -1L);
        }
        free(names[i]);
    }
    ab_puts(o, "]");
    kstr(o, "columns", "[name, bytes]");
    if (n > 60) kint(o, "more", n - 60);
    ab_puts(o, "}");
    sum(s, sn, "%d logs", n);
    return AS_TR_OK;
}

static as_tres_t t_summarize_log(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    (void)e;
    const char *name = aj_gets(in, "name"), *root = hal_sd_root();
    if (!root || !log_name_ok(name) || !is_log(name)) {
        ab_puts(o, "{\"error\":\"give a .wpilog, .dslog or .dsevents name from list_logs\"}");
        sum(s, sn, "not a log name");
        return AS_TR_ERROR;
    }
    char p[320];
    snprintf(p, sizeof p, "%s/logs/%s", root, name);
    cat_log_t *lg = malloc(sizeof *lg);
    if (!lg) return AS_TR_ERROR;
    if (!cat_log_read(p, lg)) {
        ab_puts(o, "{");
        kstr(o, "error", lg->error[0] ? lg->error : "can't read it");
        ab_puts(o, "}");
        sum(s, sn, "%s: %s", name, lg->error[0] ? lg->error : "unreadable");
        free(lg);
        return AS_TR_ERROR;
    }
    ab_puts(o, "{");
    kstr(o, "name", name);
    kstr(o, "kind", lg->kind);
    knum(o, "duration_s", lg->duration_s, 1);
    if (lg->start_unix) {
        char when[32];
        time_t t = (time_t)lg->start_unix;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%SZ", &tm);
        kstr(o, "started", when);
    }
    kint(o, "records", (long)lg->records);
    if (lg->have[CAT_LOG_BATTERY]) {
        knum(o, "min_battery_v", lg->min_battery, 2);
        /* the battery's shape: 24 buckets, each its minimum (the dips are what matter) */
        key(o, "battery_min_24");
        ab_puts(o, "[");
        for (int b = 0; b < 24; b++) {
            float m = NAN;
            for (int k = b * 10; k < b * 10 + 10; k++)
                if (lg->series[CAT_LOG_BATTERY][k] == lg->series[CAT_LOG_BATTERY][k] && !(m <= lg->series[CAT_LOG_BATTERY][k]))
                    m = lg->series[CAT_LOG_BATTERY][k];
            comma(o);
            if (m == m) ab_fmt(o, "%.2f", m);
            else ab_puts(o, "null");
        }
        ab_puts(o, "]");
    }
    kint(o, "brownouts_flagged", lg->brownouts);
    kint(o, "dips_below_6v8", lg->dips);
    if (lg->have[CAT_LOG_TRIP]) knum(o, "max_trip_ms", lg->max_trip_ms, 1);
    if (lg->have[CAT_LOG_LOSS]) knum(o, "avg_packet_loss", lg->avg_loss, 3);
    if (lg->have[CAT_LOG_CAN]) knum(o, "max_can", lg->max_can, 2);
    if (lg->have[CAT_LOG_CPU]) knum(o, "max_cpu", lg->max_cpu, 2);
    key(o, "events");
    ab_puts(o, "[");
    int shown = 0;
    for (int lvl = 2; lvl >= 0; lvl--) {
        for (int i = 0; i < lg->nevents && shown < 25; i++) {
            if (lg->events[i].level != lvl) continue;
            comma(o);
            ab_fmt(o, "[%.1f,\"%s\",", lg->events[i].t, lvl == 2 ? "error" : lvl == 1 ? "warn" : "info");
            ab_str(o, lg->events[i].text);
            ab_puts(o, "]");
            shown++;
        }
    }
    ab_puts(o, "]");
    if (lg->nevents > shown) kint(o, "more_events", lg->nevents - shown);
    ab_puts(o, "}");
    sum(s, sn, "%s · %.0f s · min %.2f V · %d brownouts", name, lg->duration_s, lg->min_battery, lg->brownouts);
    free(lg);
    return AS_TR_OK;
}

/* ---- Systemcore, via catalyst-agent ---- */

static aj_t *agent_get(as_env_t *e, const char *route, int max, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return NULL;
    /* the address NetworkTables actually reached the robot at (robot.local, 172.26.0.1, 10.TE.AM.2…) */
    char addr[64];
    nt4_status_t st;
    if (cat_nt()) {
        nt4_status(cat_nt(), &st);
        snprintf(addr, sizeof addr, "%s", st.address);
    } else {
        snprintf(addr, sizeof addr, "%s", e->robot->address);
    }
    char url[128];
    snprintf(url, sizeof url, "http://%s:%d%s", addr, AGENT_PORT, route);
    char *buf = malloc((size_t)max);
    if (!buf) return NULL;
    hal_http_req_t rq = { .method = "GET", .url = url, .timeout_ms = 8000 };
    int len = 0;
    int status = hal_http_fetch(&rq, buf, max, &len);
    aj_t *d = status > 0 ? aj_parse(buf, (size_t)len, NULL, 0) : NULL;
    free(buf);
    if (status < 0) {
        ab_puts(o, "{");
        kstr(o, "error", "catalyst-agent didn't answer");
        kstr(o, "url", url);
        kstr(o, "note", e->robot->line2 ? "Is catalyst-agent installed and running on Systemcore?"
                                        : "This robot looks like a roboRIO (1.x); catalyst-agent runs on Systemcore.");
        ab_puts(o, "}");
        sum(s, sn, "catalyst-agent didn't answer");
        return NULL;
    }
    if (!d || status != 200) {
        ab_puts(o, "{");
        kint(o, "status", status);
        kstr(o, "error", d && aj_gets(d, "error") ? aj_gets(d, "error") : "unexpected answer");
        ab_puts(o, "}");
        sum(s, sn, "catalyst-agent: %s", d && aj_gets(d, "error") ? aj_gets(d, "error") : "error");
        aj_free(d);
        return NULL;
    }
    return d;
}

/* Copies a member through unchanged (numbers, strings, small objects). */
static void kcopy(ab_t *o, const char *k, const aj_t *v)
{
    if (!v) return;
    key(o, k);
    aj_write(o, v);
}

static as_tres_t t_systemcore(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    aj_t *d = agent_get(e, "/api/system", 256 * 1024, o, s, sn);
    if (!d) return AS_TR_ERROR;
    const aj_t *id = aj_get(d, "identity"), *cpu = aj_get(d, "cpu"), *mem = aj_get(d, "memory");
    const aj_t *rp = aj_get(d, "robotProgram");
    ab_puts(o, "{");
    key(o, "identity");
    ab_puts(o, "{");
    kcopy(o, "hostname", aj_get(id, "hostname"));
    kcopy(o, "os", aj_get(id, "os"));
    kcopy(o, "kernel", aj_get(id, "kernel"));
    kcopy(o, "uptime_s", aj_get(id, "uptimeSeconds"));
    kcopy(o, "agent", aj_get(id, "agentVersion"));
    ab_puts(o, "}");
    key(o, "cpu");
    ab_puts(o, "{");
    const aj_t *cores = aj_get(cpu, "cores");
    key(o, "core_pct");
    ab_puts(o, "[");
    for (int i = 0; cores && i < cores->n; i++) {
        comma(o);
        aj_write(o, aj_get(cores->kid[i], "percent"));
    }
    ab_puts(o, "]");
    kcopy(o, "load", aj_get(cpu, "loadAverage"));
    kcopy(o, "throttling", aj_get(cpu, "throttling"));
    ab_puts(o, "}");
    const aj_t *th = aj_get(d, "thermal");
    key(o, "thermal_c");
    ab_puts(o, "{");
    for (int i = 0; th && i < th->n && i < 8; i++) {
        const char *z = aj_gets(th->kid[i], "zone");
        kcopy(o, z ? z : "zone", aj_get(th->kid[i], "celsius"));
    }
    ab_puts(o, "}");
    double tot = aj_getn(mem, "totalBytes", NAN), av = aj_getn(mem, "availableBytes", NAN);
    key(o, "memory");
    ab_fmt(o, "{\"total_mb\":%.0f,\"available_mb\":%.0f}", tot / 1048576, av / 1048576);
    const aj_t *mounts = aj_get(aj_get(d, "storage"), "mounts");
    key(o, "storage");
    ab_puts(o, "[");
    for (int i = 0; mounts && i < mounts->n && i < 6; i++) {
        const aj_t *m = mounts->kid[i];
        double t = aj_getn(m, "totalBytes", NAN), u = aj_getn(m, "usedBytes", NAN);
        comma(o);
        ab_puts(o, "{");
        kcopy(o, "mount", aj_get(m, "mount"));
        knum(o, "used_pct", 100 * u / t, 0);
        knum(o, "free_mb", (t - u) / 1048576, 0);
        ab_puts(o, "}");
    }
    ab_puts(o, "]");
    kcopy(o, "data_dirs", aj_get(aj_get(d, "storage"), "directories"));
    const aj_t *procs = aj_get(d, "processes");
    key(o, "top_cpu");
    ab_puts(o, "[");
    const aj_t *tc = aj_get(procs, "topByCpu");
    for (int i = 0; tc && i < tc->n && i < 5; i++) {
        comma(o);
        ab_puts(o, "[");
        aj_write(o, aj_get(tc->kid[i], "name"));
        ab_puts(o, ",");
        aj_write(o, aj_get(tc->kid[i], "cpuPercent"));
        ab_fmt(o, ",%.0f]", aj_getn(tc->kid[i], "rssBytes", 0) / 1048576);
    }
    ab_puts(o, "]");
    kstr(o, "top_cpu_columns", "[name, cpu %, rss MB]");
    const aj_t *can = aj_get(d, "can");
    key(o, "can_interfaces");
    ab_puts(o, "[");
    for (int i = 0; can && i < can->n; i++) {
        const aj_t *c = can->kid[i];
        comma(o);
        ab_puts(o, "{");
        kcopy(o, "name", aj_get(c, "name"));
        kcopy(o, "up", aj_get(c, "up"));
        kcopy(o, "state", aj_get(c, "state"));
        kcopy(o, "bitrate", aj_get(c, "bitrate"));
        kcopy(o, "restarts", aj_get(c, "restarts"));
        kcopy(o, "rx_errors", aj_get(c, "rxErrors"));
        kcopy(o, "tx_errors", aj_get(c, "txErrors"));
        kcopy(o, "rx_dropped", aj_get(c, "rxDropped"));
        ab_puts(o, "}");
    }
    ab_puts(o, "]");
    const aj_t *net = aj_get(d, "network");
    key(o, "network");
    ab_puts(o, "[");
    for (int i = 0; net && i < net->n && i < 6; i++) {
        const aj_t *n = net->kid[i];
        comma(o);
        ab_puts(o, "{");
        kcopy(o, "name", aj_get(n, "name"));
        kcopy(o, "up", aj_get(n, "up"));
        kcopy(o, "addresses", aj_get(n, "addresses"));
        kcopy(o, "wireless", aj_get(n, "wireless"));
        ab_puts(o, "}");
    }
    ab_puts(o, "]");
    if (rp) {
        key(o, "robot_program");
        ab_puts(o, "{");
        kcopy(o, "state", aj_get(rp, "state"));
        kcopy(o, "sub_state", aj_get(rp, "subState"));
        kcopy(o, "restarts", aj_get(rp, "restarts"));
        kcopy(o, "running_for_s", aj_get(rp, "runningForSeconds"));
        double mb = aj_getn(rp, "memoryBytes", NAN);
        knum(o, "memory_mb", mb / 1048576, 0);
        const aj_t *log = aj_get(rp, "log");
        key(o, "log_tail");
        ab_puts(o, "[");
        int from = log && log->n > 15 ? log->n - 15 : 0;
        for (int i = from; log && i < log->n; i++) {
            comma(o);
            const aj_t *l = log->kid[i];
            if (l->type == AJ_STR && l->len > 220) {
                ab_strn(o, l->s, 220);
            } else aj_write(o, l);
        }
        ab_puts(o, "]}");
    }
    const aj_t *mh = aj_get(d, "motorHistory");
    if (mh) {
        const aj_t *dev = aj_get(mh, "devices");
        key(o, "motor_history");
        ab_fmt(o, "{\"present\":%s,\"devices\":%d}", aj_is(aj_get(mh, "present"), AJ_TRUE) ? "true" : "false", dev ? dev->n : 0);
    }
    ab_puts(o, "}");
    sum(s, sn, "%s · program %s", aj_gets(id, "hostname") ? aj_gets(id, "hostname") : "systemcore",
        aj_gets(rp, "state") ? aj_gets(rp, "state") : "?");
    aj_free(d);
    return AS_TR_OK;
}

typedef struct {
    const aj_t *d;
    double score;
} wear_t;

static int by_wear(const void *a, const void *b)
{
    double x = ((const wear_t *)a)->score, y = ((const wear_t *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

static as_tres_t t_motor_history(as_env_t *e, ab_t *o, char *s, size_t sn)
{
    aj_t *d = agent_get(e, "/api/motor-history", 1024 * 1024, o, s, sn);
    if (!d) return AS_TR_ERROR;
    const aj_t *dev = aj_get(d, "devices");
    int n = dev ? dev->n : 0;
    wear_t *w = calloc((size_t)(n ? n : 1), sizeof *w);
    if (!w) {
        aj_free(d);
        return AS_TR_ERROR;
    }
    /* Worn: hours under load, with time spent hot and faults counting extra. */
    for (int i = 0; i < n; i++) {
        const aj_t *t = aj_get(dev->kid[i], "totals");
        w[i].d = dev->kid[i];
        w[i].score = aj_getn(t, "loadedSeconds", 0) + aj_getn(t, "runningSeconds", 0) * 0.25 +
                     aj_getn(t, "hotSeconds", 0) * 10 + aj_getn(t, "stickyFaults", 0) * 600;
    }
    qsort(w, (size_t)n, sizeof *w, by_wear);
    ab_puts(o, "{");
    kint(o, "motors", n);
    kcopy(o, "clock_trusted", aj_get(d, "clockTrusted"));
    kstr(o, "columns", "[name, bus, id, model, serial, running_h, loaded_h, hot_min, peak_temp_c, peak_stator_a, "
                       "revolutions, sticky_faults, boots]");
    key(o, "most_worn");
    ab_puts(o, "[");
    for (int i = 0; i < n && i < 10; i++) {
        const aj_t *x = w[i].d, *t = aj_get(x, "totals"), *ids = aj_get(x, "identities");
        const aj_t *last = ids && ids->n ? ids->kid[ids->n - 1] : NULL;
        comma(o);
        ab_puts(o, "[");
        aj_write(o, aj_get(last, "name"));
        ab_puts(o, ",");
        aj_write(o, aj_get(last, "bus"));
        ab_puts(o, ",");
        aj_write(o, aj_get(last, "id"));
        ab_puts(o, ",");
        aj_write(o, aj_get(x, "model"));
        ab_puts(o, ",");
        aj_write(o, aj_get(x, "serial"));
        ab_fmt(o, ",%.1f,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f]", aj_getn(t, "runningSeconds", 0) / 3600,
               aj_getn(t, "loadedSeconds", 0) / 3600, aj_getn(t, "hotSeconds", 0) / 60, aj_getn(t, "peakTempC", NAN),
               aj_getn(t, "peakStatorAmps", NAN), aj_getn(t, "revolutions", 0), aj_getn(t, "stickyFaults", 0),
               aj_getn(x, "boots", 0));
    }
    ab_puts(o, "]}");
    sum(s, sn, "%d motors", n);
    free(w);
    aj_free(d);
    return AS_TR_OK;
}

/* ---- the code, through the Link ---- */

static as_tres_t link_call(const char *path, ab_t *o, char *s, size_t sn, const char *what)
{
    enum { N = 48 * 1024 };
    char *buf = malloc(N);
    if (!buf) return AS_TR_ERROR;
    int st = link_get(path, buf, N);
    as_tres_t r = AS_TR_OK;
    if (st < 0 || st == 401) {
        link_status_t ls;
        link_status(&ls);
        ab_puts(o, "{");
        kstr(o, "error", st == 401 ? "Catalyst Link doesn't take the tablet's token (check it in settings)"
                         : ls.configured ? "Catalyst Link on the PC isn't reachable" : "Catalyst Link isn't set up");
        kstr(o, "note", "The code lives on the PC; start catalyst_link.py there, or set its address in settings.");
        ab_puts(o, "}");
        sum(s, sn, "Catalyst Link unreachable");
        r = AS_TR_ERROR;
    } else {
        size_t n = strlen(buf);
        if (n > AS_RESULT_MAX - 64) {
            ab_raw(o, buf, AS_RESULT_MAX - 64);
            ab_puts(o, "…(truncated: ask for less)");
        } else ab_raw(o, buf, n);
        if (st != 200) r = AS_TR_ERROR;
        sum(s, sn, "%s%s", what, st == 200 ? "" : " (refused)");
    }
    free(buf);
    return r;
}

static as_tres_t t_code(const char *name, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    char path[512], a[256], b[256];
    if (!strcmp(name, "code_tree")) {
        as_urlencode(aj_gets(in, "path") ? aj_gets(in, "path") : "", a, sizeof a);
        snprintf(path, sizeof path, "/code/tree?path=%s&depth=%d", a, (int)aj_getn(in, "depth", 2));
        snprintf(b, sizeof b, "tree of %s", aj_gets(in, "path") && aj_gets(in, "path")[0] ? aj_gets(in, "path") : "/");
    } else if (!strcmp(name, "code_read")) {
        as_urlencode(aj_gets(in, "path"), a, sizeof a);
        int start = (int)aj_getn(in, "start", 1), end = (int)aj_getn(in, "end", start + 199);
        snprintf(path, sizeof path, "/code/read?path=%s&start=%d&end=%d", a, start, end);
        snprintf(b, sizeof b, "%s:%d–%d", aj_gets(in, "path"), start, end);
    } else if (!strcmp(name, "code_search")) {
        as_urlencode(aj_gets(in, "query"), a, sizeof a);
        char p[256];
        as_urlencode(aj_gets(in, "path") ? aj_gets(in, "path") : "", p, sizeof p);
        snprintf(path, sizeof path, "/code/search?q=%s&path=%s&max=40", a, p);
        snprintf(b, sizeof b, "search \"%s\"", aj_gets(in, "query"));
    } else if (!strcmp(name, "list_patches")) {
        snprintf(path, sizeof path, "/code/patches");
        snprintf(b, sizeof b, "patches");
    } else if (!strcmp(name, "claude_sessions")) {
        snprintf(path, sizeof path, "/v1/claude/sessions");
        snprintf(b, sizeof b, "claude code on the pc");
    } else {
        const char *st = aj_gets(in, "status");
        snprintf(path, sizeof path, "/inbox?status=%s", st && strcmp(st, "all") != 0 ? st : st ? "" : "open");
        snprintf(b, sizeof b, "work orders");
    }
    return link_call(path, o, s, sn, b);
}

/* ---- changes ---- */

static const char *declined = "The technician declined this change";

static as_tres_t t_set_tunable(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const char *k = aj_gets(in, "key"), *why = aj_gets(in, "reason");
    const aj_t *val = aj_get(in, "value");
    int i = tunable_index(e->robot, k);
    if (i < 0) {
        ab_puts(o, "{\"error\":\"not a declared tunable\",\"declared\":[");
        for (int j = 0; j < e->robot->ntunables; j++) {
            comma(o);
            ab_str(o, e->robot->tunables[j].key);
        }
        ab_puts(o, "]}");
        sum(s, sn, "%s isn't declared", k);
        return AS_TR_ERROR;
    }
    cat_tunable_t t = e->robot->tunables[i];
    double v;
    if (t.is_bool) {
        if (aj_is_bool(val)) v = val->type == AJ_TRUE;
        else if (val->type == AJ_NUM && (val->num == 0 || val->num == 1)) v = val->num;
        else {
            ab_puts(o, "{\"error\":\"this tunable is a boolean: give true or false\"}");
            sum(s, sn, "%s wants true or false", t.name[0] ? t.name : k);
            return AS_TR_ERROR;
        }
    } else {
        if (val->type != AJ_NUM || !isfinite(val->num)) {
            ab_puts(o, "{\"error\":\"this tunable is a number\"}");
            sum(s, sn, "%s wants a number", t.name[0] ? t.name : k);
            return AS_TR_ERROR;
        }
        v = val->num;
        /* the robot's declared range is the rule; out of range is refused, not quietly clamped */
        if ((isfinite(t.min) && v < t.min) || (isfinite(t.max) && v > t.max)) {
            ab_fmt(o, "{\"error\":\"%g is outside the declared range [%g, %g]\"}", v, t.min, t.max);
            sum(s, sn, "%g is out of range", v);
            return AS_TR_ERROR;
        }
    }
    char before[32], after[32], detail[1024], reason[128];
    fmt_value(before, sizeof before, t.have ? t.value : NAN, t.is_bool);
    fmt_value(after, sizeof after, v, t.is_bool);
    snprintf(detail, sizeof detail, "%s   %s → %s%s%s\n%s\nwhy: %s%s", t.name[0] ? t.name : t.key, before, after,
             t.unit[0] ? " " : "", t.unit, t.key, why,
             e->robot->enabled ? "\n\nThe robot is ENABLED." : "");
    sum(s, sn, "%s %s → %s (waiting)", t.name[0] ? t.name : t.key, before, after);
    if (!e->confirm(e->user, "set_tunable", "Change a tunable", detail, why)) {
        ab_puts(o, "{");
        kstr(o, "declined", declined);
        ab_puts(o, "}");
        sum(s, sn, "%s: declined", t.name[0] ? t.name : t.key);
        return AS_TR_DENIED;
    }
    /* the robot may have moved on while the card was up: snapshot and write against it as it is now */
    e->refresh(e->robot);
    i = tunable_index(e->robot, k);
    if (i < 0 || !e->robot->connected) {
        ab_puts(o, "{\"error\":\"the robot went away before the write\"}");
        sum(s, sn, "robot gone before the write");
        return AS_TR_ERROR;
    }
    t = e->robot->tunables[i];
    fmt_value(before, sizeof before, t.have ? t.value : NAN, t.is_bool);
    snprintf(reason, sizeof reason, "before %s %s → %s (assist)", t.name[0] ? t.name : t.key, before, after);
    int snap = snap_take(e->robot, reason);
    if (snap < 0) {
        ab_puts(o, "{\"error\":\"couldn't snapshot the tunables first, so nothing was written\"}");
        sum(s, sn, "no snapshot: not written");
        return AS_TR_ERROR;
    }
    char note[160];
    snprintf(note, sizeof note, "Snapshot #%d of every tunable taken before the change.", snap);
    if (e->note) e->note(e->user, note);
    if (!cat_set_tunable(e->robot, i, v)) {
        ab_fmt(o, "{\"error\":\"the write was refused\",\"snapshot\":%d}", snap);
        sum(s, sn, "write refused");
        return AS_TR_ERROR;
    }
    /* read it back: the robot echoes a declared tunable on its next update */
    double echo = NAN;
    bool landed = false;
    for (int w = 0; w < 10 && !landed; w++) {
        usleep(100000);
        if (w < 4) continue; /* ~0.5 s before the first look */
        e->refresh(e->robot);
        int j = tunable_index(e->robot, k);
        if (j >= 0 && e->robot->tunables[j].have) {
            echo = e->robot->tunables[j].value;
            landed = fabs(echo - v) <= 1e-9 * fmax(1, fabs(v));
        }
    }
    char got[32];
    fmt_value(got, sizeof got, echo, t.is_bool);
    ab_puts(o, "{");
    kbool(o, "ok", landed);
    kstr(o, "key", t.key);
    kstr(o, "before", before);
    kstr(o, "requested", after);
    kstr(o, "robot_echo", got);
    kint(o, "snapshot", snap);
    if (!landed) kstr(o, "note", "The robot hasn't echoed the new value: it may not be listening to this key, or it clamps it.");
    ab_puts(o, "}");
    sum(s, sn, "%s %s → %s, robot says %s", t.name[0] ? t.name : t.key, before, after, got);
    return landed ? AS_TR_OK : AS_TR_ERROR;
}

static as_tres_t t_select_auto(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    const char *name = aj_gets(in, "name"), *why = aj_gets(in, "reason");
    const cat_robot_t *r = e->robot;
    bool ok = false;
    for (int i = 0; i < r->nautos && !ok; i++) ok = !strcmp(r->autos[i], name);
    if (!r->have_autos || !ok) {
        ab_puts(o, "{\"error\":\"not one of the chooser's options\",\"options\":[");
        for (int i = 0; i < r->nautos; i++) {
            comma(o);
            ab_str(o, r->autos[i]);
        }
        ab_puts(o, "]}");
        sum(s, sn, "\"%s\" isn't an option", name);
        return AS_TR_ERROR;
    }
    char detail[1024], was[40];
    snprintf(was, sizeof was, "%s", r->auto_selected[0] ? r->auto_selected : "(none)");
    snprintf(detail, sizeof detail, "auto   %s → %s\nwhy: %s%s", was, name, why,
             r->enabled ? "\n\nThe robot is ENABLED." : "");
    sum(s, sn, "auto %s → %s (waiting)", was, name);
    if (!e->confirm(e->user, "select_auto", "Pick the auto", detail, why)) {
        ab_puts(o, "{");
        kstr(o, "declined", declined);
        ab_puts(o, "}");
        sum(s, sn, "auto: declined");
        return AS_TR_DENIED;
    }
    e->refresh(e->robot);
    if (!cat_select_auto(e->robot, name)) {
        ab_puts(o, "{\"error\":\"the chooser went away\"}");
        sum(s, sn, "chooser gone");
        return AS_TR_ERROR;
    }
    usleep(500000);
    e->refresh(e->robot);
    bool landed = !strcmp(e->robot->auto_selected, name);
    ab_puts(o, "{");
    kbool(o, "ok", landed);
    kstr(o, "before", was);
    kstr(o, "requested", name);
    kstr(o, "robot_selected", e->robot->auto_selected);
    kstr(o, "robot_active", e->robot->auto_active);
    ab_puts(o, "}");
    sum(s, sn, "auto %s → %s, robot says %s", was, name, e->robot->auto_selected);
    return landed ? AS_TR_OK : AS_TR_ERROR;
}

static as_tres_t t_revert(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    if (!robot_online(e, o, s, sn)) return AS_TR_ERROR;
    int id = (int)aj_getn(in, "id", 0);
    const char *why = aj_gets(in, "reason");
    char diff[700], detail[1024];
    int d = snap_diff(e->robot, id, diff, sizeof diff);
    if (d < 0) {
        ab_fmt(o, "{\"error\":\"no snapshot #%d (list_snapshots has them)\"}", id);
        sum(s, sn, "no snapshot #%d", id);
        return AS_TR_ERROR;
    }
    if (d == 0) {
        ab_fmt(o, "{\"ok\":true,\"note\":\"the robot already matches snapshot #%d: nothing to write\"}", id);
        sum(s, sn, "already matches #%d", id);
        return AS_TR_OK;
    }
    snprintf(detail, sizeof detail, "back to snapshot #%d (%d value%s; each line: snapshot → now)\n%s\nwhy: %s", id, d,
             d == 1 ? "" : "s", diff, why);
    sum(s, sn, "revert to #%d (waiting)", id);
    if (!e->confirm(e->user, "revert", "Revert tunables", detail, why)) {
        ab_puts(o, "{");
        kstr(o, "declined", declined);
        ab_puts(o, "}");
        sum(s, sn, "revert #%d: declined", id);
        return AS_TR_DENIED;
    }
    e->refresh(e->robot);
    char reason[128];
    snprintf(reason, sizeof reason, "before reverting to #%d (assist)", id);
    int snap = snap_take(e->robot, reason);
    if (snap < 0) {
        ab_puts(o, "{\"error\":\"couldn't snapshot the current values first, so nothing was written\"}");
        sum(s, sn, "no snapshot: not reverted");
        return AS_TR_ERROR;
    }
    char note[160];
    snprintf(note, sizeof note, "Snapshot #%d taken before reverting to #%d.", snap, id);
    if (e->note) e->note(e->user, note);
    if (!snap_revert(e->robot, id)) {
        ab_fmt(o, "{\"error\":\"nothing could be written back\",\"snapshot\":%d}", snap);
        sum(s, sn, "revert failed");
        return AS_TR_ERROR;
    }
    usleep(600000);
    e->refresh(e->robot);
    int left = snap_diff(e->robot, id, diff, sizeof diff);
    ab_puts(o, "{");
    kbool(o, "ok", left == 0);
    kint(o, "reverted_to", id);
    kint(o, "snapshot_before_revert", snap);
    kint(o, "still_different", left);
    if (left > 0) kstr(o, "differences", diff);
    ab_puts(o, "}");
    sum(s, sn, "reverted to #%d%s", id, left == 0 ? ", robot matches" : ", some values differ");
    return left == 0 ? AS_TR_OK : AS_TR_ERROR;
}

/* the first line of `s`, cut to `max` characters, into out */
static void first_line(char *out, size_t n, const char *s, int max)
{
    int len = 0, lines = 1;
    while (s[len] && s[len] != '\n') len++;
    for (const char *p = s; *p; p++) lines += *p == '\n';
    if (len > max) len = max;
    snprintf(out, n, "%.*s%s", len, s, s[len] && s[len] != '\n' ? "…" : "");
    if (lines > 1) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, " (+%d lines)", lines - 1);
    }
}

static bool path_ok(const char *p)
{
    return p && p[0] && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\') && strncmp(p, ".git", 4) != 0;
}

static as_tres_t t_patch(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    const char *title = aj_gets(in, "title"), *summary = aj_gets(in, "summary");
    const aj_t *edits = aj_get(in, "edits");
    size_t bytes = 0;
    for (int i = 0; i < edits->n; i++) {
        const aj_t *ed = edits->kid[i];
        if (!path_ok(aj_gets(ed, "path"))) {
            ab_fmt(o, "{\"error\":\"edit %d: paths are relative to the repo root, with no '..'\"}", i);
            sum(s, sn, "bad path in edit %d", i);
            return AS_TR_ERROR;
        }
        bytes += strlen(aj_gets(ed, "old")) + strlen(aj_gets(ed, "new"));
    }
    if (bytes > 64 * 1024) {
        ab_puts(o, "{\"error\":\"a patch is at most 64 KB of edits\"}");
        sum(s, sn, "patch too big");
        return AS_TR_ERROR;
    }
    /* the card: what, why, and each edit's old → new, cut short */
    char detail[1024];
    int len = snprintf(detail, sizeof detail, "%s\n", title);
    char line[160];
    for (int i = 0; i < edits->n && len < (int)sizeof detail - 1; i++) {
        const aj_t *ed = edits->kid[i];
        const char *old = aj_gets(ed, "old"), *nw = aj_gets(ed, "new");
        len += snprintf(detail + len, sizeof detail - (size_t)len, "\n%s\n", aj_gets(ed, "path"));
        if (len >= (int)sizeof detail) break;
        if (old[0]) {
            first_line(line, sizeof line, old, 70);
            len += snprintf(detail + len, sizeof detail - (size_t)len, "  - %s\n", line);
        } else {
            len += snprintf(detail + len, sizeof detail - (size_t)len, "  (new file)\n");
        }
        if (len >= (int)sizeof detail) break;
        first_line(line, sizeof line, nw, 70);
        len += snprintf(detail + len, sizeof detail - (size_t)len, "  + %s\n", line);
    }
    if (len < (int)sizeof detail - 1)
        snprintf(detail + len, sizeof detail - (size_t)len, "\nLands on a new branch on the PC: not pushed, not deployed.");
    sum(s, sn, "patch \"%s\", %d edit%s (waiting)", title, edits->n, edits->n == 1 ? "" : "s");
    if (!e->confirm(e->user, "propose_patch", "Propose a code change", detail, summary)) {
        ab_puts(o, "{");
        kstr(o, "declined", declined);
        ab_puts(o, "}");
        sum(s, sn, "patch \"%s\": declined", title);
        return AS_TR_DENIED;
    }
    e->refresh(e->robot);
    ab_t body;
    ab_init(&body);
    ab_puts(&body, "{\"title\":");
    ab_str(&body, title);
    ab_puts(&body, ",\"summary\":");
    ab_str(&body, summary);
    ab_puts(&body, ",\"edits\":");
    aj_write(&body, edits);
    ab_puts(&body, ",\"robot\":");
    robot_brief(&body, e->robot);
    ab_puts(&body, ",\"from\":\"catalyst-tab\"}");
    enum { N = 16 * 1024 };
    char *resp = malloc(N);
    int st = resp && !body.oom ? link_post("/code/patch", body.p, resp, N) : -2;
    as_tres_t r = AS_TR_OK;
    if (st == -1) {
        /* the PC is away: the patch waits on microSD and goes when the Link is back */
        bool q = link_post_queued("/code/patch", body.p);
        ab_puts(o, "{");
        kbool(o, "queued", q);
        kstr(o, "note", q ? "Catalyst Link is unreachable; the patch waits on the tablet's microSD and is sent when "
                            "the PC is back. No branch exists yet."
                          : "Catalyst Link is unreachable and the patch couldn't be queued (no microSD?).");
        ab_puts(o, "}");
        sum(s, sn, "patch \"%s\" %s", title, q ? "queued for the PC" : "not sent");
        if (q && e->note) e->note(e->user, "Patch queued on microSD: it goes to the PC when Catalyst Link is back.");
        r = q ? AS_TR_OK : AS_TR_ERROR;
    } else if (st == 200) {
        aj_t *d = aj_parse(resp, strlen(resp), NULL, 0);
        ab_puts(o, "{\"ok\":true");
        kcopy(o, "id", aj_get(d, "id"));
        kcopy(o, "branch", aj_get(d, "branch"));
        kcopy(o, "files", aj_get(d, "files"));
        kcopy(o, "diffstat", aj_get(d, "diffstat"));
        kcopy(o, "check", aj_get(d, "check"));
        if (aj_is(aj_get(d, "duplicate"), AJ_TRUE)) kbool(o, "duplicate", true);
        kstr(o, "note", "Committed on its own branch in the Link's worktree; the checked-out branch is untouched, "
                        "nothing pushed or deployed.");
        ab_puts(o, "}");
        char note[200];
        snprintf(note, sizeof note, "Patch on branch %s (on the PC; not pushed, not deployed).",
                 aj_gets(d, "branch") ? aj_gets(d, "branch") : "?");
        if (e->note) e->note(e->user, note);
        sum(s, sn, "patch on %s", aj_gets(d, "branch") ? aj_gets(d, "branch") : "a new branch");
        aj_free(d);
    } else {
        /* 409/403: which edit and why (not_found, not_unique, missing_file, exists, no_change, path), so the
         * model can read the file again and fix that edit */
        aj_t *d = resp ? aj_parse(resp, strlen(resp), NULL, 0) : NULL;
        ab_fmt(o, "{\"status\":%d", st);
        kcopy(o, "edit", aj_get(d, "edit"));
        kcopy(o, "why", aj_get(d, "why"));
        kcopy(o, "error", aj_get(d, "error"));
        if (!d) kstr(o, "link_said", resp ? resp : "");
        if (aj_gets(d, "why") && strcmp(aj_gets(d, "why"), "path") != 0)
            kstr(o, "hint", "each edit's old text must occur exactly once in the file as it is now: code_read it again");
        ab_puts(o, "}");
        if (aj_gets(d, "why")) sum(s, sn, "patch refused: edit %d %s", (int)aj_getn(d, "edit", -1), aj_gets(d, "why"));
        else sum(s, sn, "patch refused (%d)", st);
        aj_free(d);
        r = AS_TR_ERROR;
    }
    free(resp);
    ab_free(&body);
    return r;
}

static as_tres_t t_work_order(as_env_t *e, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    const aj_t *files = aj_get(in, "sd_files");
    const char *root = hal_sd_root();
    ab_t names;
    ab_init(&names);
    ab_puts(&names, "[");
    /* attachments first: the outbox keeps order, so the PC has the files before the order naming them */
    for (int i = 0; files && i < files->n; i++) {
        const char *rel = files->kid[i]->s;
        char full[320];
        if (!root || !path_ok(rel)) {
            ab_fmt(o, "{\"error\":\"sd_files[%d]: give a path relative to the microSD card, no '..'\"}", i);
            sum(s, sn, "bad file path");
            ab_free(&names);
            return AS_TR_ERROR;
        }
        snprintf(full, sizeof full, "%s/%s", root, rel);
        struct stat st;
        if (stat(full, &st) != 0) {
            ab_fmt(o, "{\"error\":\"no such file on the card: %s\"}", rel);
            sum(s, sn, "no file %s", rel);
            ab_free(&names);
            return AS_TR_ERROR;
        }
        const char *base = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
        if (!link_upload(full, base)) {
            ab_fmt(o, "{\"error\":\"couldn't send or queue %s\"}", rel);
            sum(s, sn, "couldn't attach %s", rel);
            ab_free(&names);
            return AS_TR_ERROR;
        }
        if (names.n > 1) ab_puts(&names, ",");
        ab_str(&names, base);
    }
    ab_puts(&names, "]");
    e->refresh(e->robot);
    ab_t body;
    ab_init(&body);
    ab_puts(&body, "{\"title\":");
    ab_str(&body, aj_gets(in, "title"));
    ab_puts(&body, ",\"body\":");
    ab_str(&body, aj_gets(in, "body"));
    ab_puts(&body, ",\"kind\":");
    ab_str(&body, aj_gets(in, "kind"));
    ab_puts(&body, ",\"priority\":");
    ab_str(&body, aj_gets(in, "priority"));
    if (aj_gets(in, "patch") && aj_gets(in, "patch")[0]) {
        ab_puts(&body, ",\"patch\":");
        ab_str(&body, aj_gets(in, "patch"));
    }
    ab_puts(&body, ",\"files\":");
    ab_raw(&body, names.p, names.n);
    ab_puts(&body, ",\"robot\":");
    robot_brief(&body, e->robot);
    ab_puts(&body, ",\"from\":\"catalyst-tab\"}");
    ab_free(&names);
    char resp[1024];
    int st = link_outbox_count() == 0 ? link_post("/inbox", body.p, resp, sizeof resp) : -1;
    as_tres_t r = AS_TR_OK;
    if (st == 200) {
        aj_t *d = aj_parse(resp, strlen(resp), NULL, 0);
        ab_puts(o, "{\"ok\":true");
        kcopy(o, "id", aj_get(d, "id"));
        kcopy(o, "path", aj_get(d, "path"));
        kstr(o, "note", "In the PC's inbox; its agent works through it asynchronously.");
        ab_puts(o, "}");
        sum(s, sn, "sent: %s", aj_gets(d, "id") ? aj_gets(d, "id") : aj_gets(in, "title"));
        aj_free(d);
    } else if (st >= 400 && st < 500 && st != 401) {
        ab_fmt(o, "{\"status\":%d,\"link_said\":", st);
        ab_str(o, resp);
        ab_puts(o, "}");
        sum(s, sn, "refused by the Link (%d)", st);
        r = AS_TR_ERROR;
    } else {
        /* the PC is away (or files are still queued ahead of it): store and forward */
        bool q = link_post_queued("/inbox", body.p);
        ab_puts(o, "{");
        kbool(o, "queued", q);
        kint(o, "outbox", link_outbox_count());
        kstr(o, "note", q ? "Catalyst Link is unreachable: the work order waits on the tablet's microSD and is sent "
                            "when the PC is back."
                          : "Catalyst Link is unreachable and the work order couldn't be queued (no microSD?).");
        ab_puts(o, "}");
        sum(s, sn, "%s: %s", aj_gets(in, "title"), q ? "queued for the PC" : "not sent");
        if (q && e->note) e->note(e->user, "Work order queued on microSD: it goes to the PC when Catalyst Link is back.");
        r = q ? AS_TR_OK : AS_TR_ERROR;
    }
    ab_free(&body);
    return r;
}

/* ---- dispatch ---- */

as_tres_t as_tool_run(as_env_t *e, const char *name, const aj_t *in, ab_t *o, char *s, size_t sn)
{
    s[0] = 0;
    e->refresh(e->robot);
    as_tres_t r;
    if (!strcmp(name, "robot_overview")) r = t_overview(e, o, s, sn);
    else if (!strcmp(name, "get_alerts")) r = t_alerts(e, o, s, sn);
    else if (!strcmp(name, "get_mechanisms")) r = t_mechs(e, o, s, sn);
    else if (!strcmp(name, "get_power")) r = t_power(e, o, s, sn);
    else if (!strcmp(name, "get_can")) r = t_can(e, o, s, sn);
    else if (!strcmp(name, "get_vision")) r = t_vision(e, o, s, sn);
    else if (!strcmp(name, "run_preflight")) r = t_preflight(e, o, s, sn);
    else if (!strcmp(name, "list_topics")) r = t_list_topics(e, in, o, s, sn);
    else if (!strcmp(name, "read_topics")) r = t_read_topics(e, in, o, s, sn);
    else if (!strcmp(name, "list_tunables")) r = t_list_tunables(e, o, s, sn);
    else if (!strcmp(name, "list_autos")) r = t_list_autos(e, o, s, sn);
    else if (!strcmp(name, "list_snapshots")) r = t_list_snapshots(e, o, s, sn);
    else if (!strcmp(name, "list_logs")) r = t_list_logs(e, o, s, sn);
    else if (!strcmp(name, "summarize_log")) r = t_summarize_log(e, in, o, s, sn);
    else if (!strcmp(name, "systemcore_health")) r = t_systemcore(e, o, s, sn);
    else if (!strcmp(name, "motor_history")) r = t_motor_history(e, o, s, sn);
    else if (!strncmp(name, "code_", 5) || !strcmp(name, "list_patches") || !strcmp(name, "list_work_orders") ||
             !strcmp(name, "claude_sessions"))
        r = t_code(name, in, o, s, sn);
    else if (!strcmp(name, "set_tunable")) r = t_set_tunable(e, in, o, s, sn);
    else if (!strcmp(name, "select_auto")) r = t_select_auto(e, in, o, s, sn);
    else if (!strcmp(name, "revert_snapshot")) r = t_revert(e, in, o, s, sn);
    else if (!strcmp(name, "propose_patch")) r = t_patch(e, in, o, s, sn);
    else if (!strcmp(name, "create_work_order")) r = t_work_order(e, in, o, s, sn);
    else {
        ab_fmt(o, "{\"error\":\"no tool named %s\"}", name);
        sum(s, sn, "unknown tool");
        r = AS_TR_ERROR;
    }
    if (o->n > AS_RESULT_MAX) {
        /* keep the conversation small: a result past the budget is cut (as text; ask narrower) */
        o->n = AS_RESULT_MAX;
        while (o->n && ((unsigned char)o->p[o->n] & 0xC0) == 0x80) o->n--; /* not mid-character */
        o->p[o->n] = 0;
        ab_puts(o, "…(truncated)");
    }
    if (o->oom) {
        ab_free(o);
        ab_puts(o, "{\"error\":\"out of memory\"}");
        r = AS_TR_ERROR;
    }
    return r;
}
