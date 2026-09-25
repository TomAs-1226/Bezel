#include "cat_model.h"
#include "json_lite.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nt4_client_t *g_nt;

void cat_model_init(nt4_client_t *nt) { g_nt = nt; }
nt4_client_t *cat_nt(void) { return g_nt; }

/* ------------------------------------------------------------------ helpers */

static bool num(const char *topic, double *out) { return nt4_get_number(g_nt, topic, out); }

static double num_or(const char *topic, double fallback)
{
    double v;
    return num(topic, &v) ? v : fallback;
}

static bool flag(const char *topic, bool fallback)
{
    bool v;
    return nt4_get_bool(g_nt, topic, &v) ? v : fallback;
}

static bool str(const char *topic, char *buf, size_t n)
{
    buf[0] = 0;
    return nt4_get_string(g_nt, topic, buf, n);
}

static void copy(char *dst, size_t n, const char *src, size_t len)
{
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* Topic names under a prefix, copied out of the table so they can be read afterwards. */
#define MAX_NAMES 512
typedef struct {
    char (*names)[128];
    int n;
    const char *suffix;
} names_t;

static void visit(const char *name, const char *type, void *user)
{
    (void)type;
    names_t *c = user;
    if (c->n >= MAX_NAMES) return;
    if (c->suffix) {
        size_t nl = strlen(name), sl = strlen(c->suffix);
        if (nl < sl || strcmp(name + nl - sl, c->suffix) != 0) return;
    }
    snprintf(c->names[c->n++], 128, "%s", name);
}

static char (*g_names)[128];

static int collect(const char *prefix, const char *suffix, names_t *out)
{
    if (!g_names) g_names = malloc(sizeof(char[128]) * MAX_NAMES);
    out->names = g_names;
    out->n = 0;
    out->suffix = suffix;
    if (!g_names) return 0;
    nt4_list(g_nt, prefix, visit, out);
    return out->n;
}

/* Splits "a|b|c" into at most `max` fields; the last field keeps any further '|' (a device name may
 * contain one: can-model.js rejoins fields 3+). */
static int split(char *s, char **fields, int max)
{
    int n = 0;
    fields[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == '|') {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void norm_bus(char *dst, size_t n, const char *bus)
{
    if (!bus[0] || !strcmp(bus, "0")) snprintf(dst, n, "can_s0");
    else snprintf(dst, n, "%s", bus);
}

static const char *const RESERVED[] = {
    "Robot", "Alerts", "Health", "Physics", "Swerve", "Vision", "Loop", "Safety", "Calibration", "SystemCheck",
    "Preflight", "Auto", "Systemcore", "CAN", "Devices", "MotorHistory", "Tunables", "Tuning", "Controls",
    "Brownout", "Status", "Match", "Drive", "Superstructure", NULL,
};

static bool reserved(const char *name)
{
    for (int i = 0; RESERVED[i]; i++) if (!strcmp(RESERVED[i], name)) return true;
    return false;
}

/* ------------------------------------------------------------------ sections */

static void read_identity(cat_robot_t *r)
{
    r->have_identity = str("/Catalyst/Robot/Identity/Name", r->name, sizeof r->name);
    r->team = (int)num_or("/Catalyst/Robot/Identity/TeamNumber", 0);
    str("/Catalyst/Robot/Software/CatalystVersion", r->catalyst_version, sizeof r->catalyst_version);
    str("/Catalyst/Robot/Software/RobotCodeVersion", r->code_version, sizeof r->code_version);
    str("/Catalyst/Robot/Software/WPILibVersion", r->wpilib_version, sizeof r->wpilib_version);
    char sha[48];
    if (str("/Catalyst/Robot/Software/CatalystGitSha", sha, sizeof sha)) copy(r->git_sha, sizeof r->git_sha, sha, 7);
    else r->git_sha[0] = 0;
    str("/Catalyst/Robot/Identity/Controller", r->controller, sizeof r->controller);
}

static void read_mode(cat_robot_t *r)
{
    double w;
    r->line2 = false;
    r->have_mode = false;
    if (num("/FMSInfo/ControlWord", &w)) {
        r->line2 = true;
        r->have_mode = true;
    } else if (num("/FMSInfo/FMSControlData", &w)) {
        r->have_mode = true;
    }
    if (r->have_mode) {
        unsigned b = (unsigned)w;
        r->enabled = b & 1;
        r->autonomous = b & 2;
        r->test = b & 4;
        r->estop = b & 8;
        r->fms = b & 16;
        r->ds = b & 32;
    }
    bool red;
    r->have_alliance = nt4_get_bool(g_nt, "/FMSInfo/IsRedAlliance", &red);
    r->red = red;
    r->station = (int)num_or("/FMSInfo/StationNumber", 0);
    str("/FMSInfo/OpMode", r->opmode, sizeof r->opmode);
    r->match_time = NAN;
    const char *clocks[] = { "/Catalyst/Match/TimeLeft", "/SmartDashboard/MatchTime", "/FMSInfo/MatchTime" };
    for (int i = 0; i < 3; i++) {
        double t;
        if (num(clocks[i], &t) && t >= 0) {
            r->match_time = t;
            break;
        }
    }
}

static void read_power(cat_robot_t *r)
{
    const char *batt[] = { "/Catalyst/Status/BatteryVolts", "/Catalyst/Brownout/MeasuredVoltage",
                           "/Catalyst/Systemcore/BatteryVolts" };
    r->have_battery = false;
    for (int i = 0; i < 3; i++) {
        double v;
        if (num(batt[i], &v) && v > 0) {
            r->battery_v = v;
            r->have_battery = true;
            break;
        }
    }
    r->brownout_v = num_or("/Catalyst/Robot/Power/BrownoutVolts", NAN);
    r->predicted_v = num_or("/Catalyst/Brownout/PredictedVoltage", NAN);
    r->total_current = num_or("/Catalyst/Brownout/TotalCurrent", NAN);
    r->brownout_risk = flag("/Catalyst/Brownout/AtRisk", false);
    r->browned_out = flag("/Catalyst/Systemcore/BrownedOut", false);

    str("/Catalyst/Robot/Power/Module", r->pd_module, sizeof r->pd_module);
    /* the battery model RobotIdentity declares (.battery("MK ES17-12")): which one of the team's is in, the tablet knows */
    str("/Catalyst/Robot/Power/Battery", r->battery_model, sizeof r->battery_model);
    r->nchannels = 0;
    char buf[2048];
    const char *items[CAT_MAX_CHANNELS];
    int n = nt4_get_strings(g_nt, "/Catalyst/Robot/Power/ChannelsInUse", buf, sizeof buf, items, CAT_MAX_CHANNELS);
    for (int i = 0; i < n && r->nchannels < CAT_MAX_CHANNELS; i++) {
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s", items[i]);
        char *f[2];
        if (split(tmp, f, 2) < 2) continue;
        cat_channel_t *c = &r->channels[r->nchannels++];
        c->channel = atoi(f[0]);
        snprintf(c->what, sizeof c->what, "%s", f[1]);
        c->amps = NAN;
    }
    /* Live channel currents exist only if the team publishes its PDH (SmartDashboard.putData(pdh)). */
    names_t nm;
    r->have_pd_live = false;
    if (collect("/SmartDashboard/", "/TotalCurrent", &nm) > 0) {
        char base[128];
        snprintf(base, sizeof base, "%s", nm.names[0]);
        base[strlen(base) - strlen("/TotalCurrent")] = 0;
        char t[160];
        r->have_pd_live = true;
        snprintf(t, sizeof t, "%s/TotalCurrent", base);
        r->pd_total = num_or(t, NAN);
        snprintf(t, sizeof t, "%s/Voltage", base);
        r->pd_voltage = num_or(t, NAN);
        for (int ch = 0; ch < 24; ch++) {
            snprintf(t, sizeof t, "%s/Chan%d", base, ch);
            r->pd_amps[ch] = num_or(t, NAN);
            for (int k = 0; k < r->nchannels; k++) if (r->channels[k].channel == ch) r->channels[k].amps = r->pd_amps[ch];
        }
    } else {
        for (int ch = 0; ch < 24; ch++) r->pd_amps[ch] = NAN;
    }
}

static void read_loop(cat_robot_t *r)
{
    r->have_loop = num("/Catalyst/Loop/Robot/AverageMs", &r->loop_avg_ms);
    r->loop_last_ms = num_or("/Catalyst/Loop/Robot/LastMs", NAN);
    r->loop_max_ms = num_or("/Catalyst/Loop/Robot/MaxMs", NAN);
    r->loop_over = flag("/Catalyst/Loop/Robot/OverBudget", false);
}

static void add_device(cat_robot_t *r, const char *bus, int id, const char *type, const char *name, int connected)
{
    char b[12];
    norm_bus(b, sizeof b, bus);
    for (int i = 0; i < r->ndevices; i++) {
        cat_device_t *d = &r->devices[i];
        if (d->id == id && !strcmp(d->bus, b) && (!type[0] || !d->type[0] || !strcmp(d->type, type) || connected >= 0)) {
            if (connected >= 0) d->connected = connected;
            if (name[0] && !d->name[0]) snprintf(d->name, sizeof d->name, "%s", name);
            if (type[0] && !d->type[0]) snprintf(d->type, sizeof d->type, "%s", type);
            return;
        }
    }
    if (r->ndevices >= CAT_MAX_DEVICES) return;
    cat_device_t *d = &r->devices[r->ndevices++];
    snprintf(d->bus, sizeof d->bus, "%s", b);
    d->id = id;
    snprintf(d->type, sizeof d->type, "%s", type);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->connected = connected;
    d->temp_c = d->current_a = NAN;
}

static void read_can(cat_robot_t *r)
{
    double util[5];
    int n = nt4_get_numbers(g_nt, "/Catalyst/Systemcore/CanUtilization", util, 5);
    if (n > 0) {
        r->ncan_util = n;
        memcpy(r->can_util, util, sizeof(double) * (size_t)n);
    } else {
        double u;
        r->ncan_util = 0;
        if (num("/Catalyst/Status/CanUtilization", &u)) {
            r->ncan_util = 1;
            r->can_util[0] = u > 1.5 ? u / 100 : u;
        }
    }
    r->can_down = flag("/Catalyst/Systemcore/CanDown", false);

    r->ndevices = 0;
    static char buf[8192];
    const char *items[CAT_MAX_DEVICES];
    n = nt4_get_strings(g_nt, "/Catalyst/CAN/Devices", buf, sizeof buf, items, CAT_MAX_DEVICES);
    for (int i = 0; i < n; i++) {
        char tmp[128], *f[4];
        snprintf(tmp, sizeof tmp, "%s", items[i]);
        int k = split(tmp, f, 4);
        if (k < 3) continue;
        add_device(r, f[0], atoi(f[1]), f[2], k > 3 ? f[3] : "", -1);
    }
    /* 2.x roster: per-motor connection state */
    double v;
    r->have_roster = num("/Catalyst/Devices/Motors/Expected", &v);
    if (r->have_roster) {
        r->motors_expected = (int)v;
        r->motors_connected = (int)num_or("/Catalyst/Devices/Motors/Connected", 0);
        r->cams_expected = (int)num_or("/Catalyst/Devices/Cameras/Expected", 0);
        r->cams_connected = (int)num_or("/Catalyst/Devices/Cameras/Connected", 0);
        n = nt4_get_strings(g_nt, "/Catalyst/Devices/Motors/Rows", buf, sizeof buf, items, CAT_MAX_DEVICES);
        for (int i = 0; i < n; i++) {
            char tmp[128], *f[4];
            snprintf(tmp, sizeof tmp, "%s", items[i]);
            if (split(tmp, f, 4) < 4) continue;
            add_device(r, f[1], atoi(f[2]), "", f[0], !strcmp(f[3], "true") || !strcmp(f[3], "1"));
        }
    }
    r->have_controller = str("/Catalyst/Devices/Controller/Kind", r->controller_kind, sizeof r->controller_kind);
    r->controller_connected = flag("/Catalyst/Devices/Controller/Connected", false);
}

static void add_alert(cat_robot_t *r, cat_sev_t sev, const char *text, bool health, const char *source)
{
    if (r->nalerts >= CAT_MAX_ALERTS) return;
    cat_alert_t *a = &r->alerts[r->nalerts++];
    a->sev = sev;
    a->health = health;
    a->source[0] = 0;
    const char *t = text;
    if (source) snprintf(a->source, sizeof a->source, "%s", source);
    else if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (close) {
            copy(a->source, sizeof a->source, text + 1, (size_t)(close - text - 1));
            t = close + 1;
            while (*t == ' ') t++;
        }
    }
    snprintf(a->text, sizeof a->text, "%s", t);
    if (sev == CAT_SEV_ERROR) r->n_errors++;
    else if (sev == CAT_SEV_WARN) r->n_warnings++;
    else r->n_infos++;
}

static void read_alert_group(cat_robot_t *r, const char *base, bool lower)
{
    static const char *const up[] = { "Errors", "Warnings", "Info" };
    static const char *const lo[] = { "errors", "warnings", "infos" };
    static char buf[4096];
    for (int s = 0; s < 3; s++) {
        char t[128];
        snprintf(t, sizeof t, "%s/%s", base, lower ? lo[s] : up[s]);
        const char *items[CAT_MAX_ALERTS];
        int n = nt4_get_strings(g_nt, t, buf, sizeof buf, items, CAT_MAX_ALERTS);
        for (int i = 0; i < n; i++) add_alert(r, s == 0 ? CAT_SEV_ERROR : s == 1 ? CAT_SEV_WARN : CAT_SEV_INFO, items[i], false, NULL);
    }
}

static void read_alerts(cat_robot_t *r)
{
    r->nalerts = r->n_errors = r->n_warnings = r->n_infos = 0;
    read_alert_group(r, "/Catalyst/Alerts", false);
    /* WPILib Alerts groups: /SmartDashboard/<group>/{errors,warnings,infos} */
    names_t nm;
    int n = collect("/SmartDashboard/", "/errors", &nm);
    for (int i = 0; i < n; i++) {
        char base[128];
        snprintf(base, sizeof base, "%s", nm.names[i]);
        base[strlen(base) - 7] = 0;
        read_alert_group(r, base, true);
    }
    /* HealthMonitor checks that are firing */
    n = collect("/Catalyst/Health/", "/firing", &nm);
    char (*firing)[128] = malloc(sizeof(char[128]) * (size_t)(n ? n : 1));
    if (!firing) return;
    memcpy(firing, nm.names, sizeof(char[128]) * (size_t)n);
    for (int i = 0; i < n; i++) {
        if (!flag(firing[i], false)) continue;
        char base[128], t[160], sev[16], desc[80], detail[80], sub[32] = "";
        snprintf(base, sizeof base, "%s", firing[i]);
        base[strlen(base) - 7] = 0;
        /* /Catalyst/Health/<subsystem>/<id> */
        const char *p = base + strlen("/Catalyst/Health/");
        const char *slash = strchr(p, '/');
        if (slash) copy(sub, sizeof sub, p, (size_t)(slash - p));
        snprintf(t, sizeof t, "%s/severity", base);
        str(t, sev, sizeof sev);
        snprintf(t, sizeof t, "%s/description", base);
        str(t, desc, sizeof desc);
        snprintf(t, sizeof t, "%s/detail", base);
        str(t, detail, sizeof detail);
        char text[168];
        snprintf(text, sizeof text, "%s%s%s", desc[0] ? desc : (slash ? slash + 1 : base), detail[0] ? " — " : "", detail);
        add_alert(r, !strcmp(sev, "ERROR") ? CAT_SEV_ERROR : !strcmp(sev, "WARN") ? CAT_SEV_WARN : CAT_SEV_INFO, text, true, sub);
    }
    free(firing);
}

static void read_mechs(cat_robot_t *r)
{
    static const char *const keys[] = { "AngleDegrees", "PositionMeters", "VelocityRPS", "VelocityMPS", NULL };
    names_t nm;
    int n = collect("/Catalyst/", NULL, &nm);
    char found[CAT_MAX_MECHS][32];
    cat_mech_kind_t kinds[CAT_MAX_MECHS];
    int nf = 0;
    for (int i = 0; i < n && nf < CAT_MAX_MECHS; i++) {
        const char *p = nm.names[i] + strlen("/Catalyst/");
        const char *slash = strchr(p, '/');
        if (!slash || strchr(slash + 1, '/')) continue;
        for (int k = 0; keys[k]; k++) {
            if (strcmp(slash + 1, keys[k])) continue;
            char mech[32];
            copy(mech, sizeof mech, p, (size_t)(slash - p));
            if (reserved(mech)) break;
            int j;
            for (j = 0; j < nf; j++) if (!strcmp(found[j], mech)) break;
            if (j == nf) {
                snprintf(found[nf], sizeof found[nf], "%s", mech);
                kinds[nf] = k == 0 ? CAT_MECH_ROTATIONAL : k == 1 ? CAT_MECH_LINEAR : CAT_MECH_FLYWHEEL;
                nf++;
            }
            break;
        }
    }
    r->nmechs = 0;
    for (int i = 0; i < nf; i++) {
        cat_mech_t *m = &r->mechs[r->nmechs++];
        char t[128];
        snprintf(m->name, sizeof m->name, "%s", found[i]);
        m->kind = kinds[i];
#define K(s) (snprintf(t, sizeof t, "/Catalyst/%s/%s", m->name, s), t)
        str(K("State"), m->state, sizeof m->state);
        switch (m->kind) {
        case CAT_MECH_ROTATIONAL:
            m->pos = num_or(K("AngleDegrees"), NAN);
            m->setpoint = num_or(K("SetpointDegrees"), NAN);
            m->vel = num_or(K("AngularVelocityDPS"), NAN);
            snprintf(m->unit, sizeof m->unit, "°");
            break;
        case CAT_MECH_LINEAR:
            m->pos = num_or(K("PositionMeters"), NAN);
            m->setpoint = num_or(K("SetpointMeters"), NAN);
            m->vel = num_or(K("VelocityMPS"), NAN);
            snprintf(m->unit, sizeof m->unit, "m");
            break;
        default:
            m->pos = num_or(K("VelocityRPS"), NAN);
            m->setpoint = num_or(K("SetpointRPS"), NAN);
            m->vel = m->pos;
            snprintf(m->unit, sizeof m->unit, "rps");
            break;
        }
        m->current_a = num_or(K("StatorCurrentAmps"), num_or(K("CurrentAmps"), NAN));
        m->temp_c = num_or(K("TemperatureC"), NAN);
        bool at;
        if (nt4_get_bool(g_nt, K("AtSetpoint"), &at) || nt4_get_bool(g_nt, K("AtSpeed"), &at)) m->at_goal = at;
        else m->at_goal = -1;
#undef K
    }
}

static void read_pose(cat_robot_t *r)
{
    double p[3];
    int n = nt4_get_numbers(g_nt, "/Catalyst/Physics/PoseArray", p, 3);
    if (n < 3) n = nt4_get_numbers(g_nt, "/Catalyst/Swerve/Pose", p, 3);
    r->have_pose = n >= 3;
    if (r->have_pose) {
        r->pose_x = p[0];
        r->pose_y = p[1];
        r->pose_rad = p[2];
    }
    double m[8];
    n = nt4_get_numbers(g_nt, "/Catalyst/Swerve/ModuleStates", m, 8);
    r->nmodules = n >= 8 ? 4 : n / 2;
    for (int i = 0; i < r->nmodules; i++) {
        r->module_speed[i] = m[2 * i];
        r->module_angle[i] = m[2 * i + 1];
    }
    n = nt4_get_numbers(g_nt, "/Catalyst/Swerve/ModuleTargets", m, 8);
    r->have_targets = n >= 8;
    for (int i = 0; r->have_targets && i < 4; i++) {
        r->target_speed[i] = m[2 * i];
        r->target_angle[i] = m[2 * i + 1];
    }
    r->heading_deg = num_or("/Catalyst/Swerve/HeadingDeg", NAN);
    n = nt4_get_numbers(g_nt, "/PathPlanner/activePath", r->path, 64 * 3);
    r->npath = n > 0 ? n / 3 : 0;
}

static void read_vision(cat_robot_t *r)
{
    double lvl;
    r->have_vision = num("/Catalyst/Vision/Health/Level", &lvl);
    r->vision_level = r->have_vision ? (int)lvl : 0;
    str("/Catalyst/Vision/Health/Summary", r->vision_summary, sizeof r->vision_summary);
    r->ncameras = 0;
    static char buf[2048];
    const char *items[CAT_MAX_CAMERAS];
    int n = nt4_get_strings(g_nt, "/Catalyst/Vision/Health/Rows", buf, sizeof buf, items, CAT_MAX_CAMERAS);
    for (int i = 0; i < n; i++) {
        char tmp[160], *f[6];
        snprintf(tmp, sizeof tmp, "%s", items[i]);
        if (split(tmp, f, 6) < 6) continue;
        cat_camera_t *c = &r->cameras[r->ncameras++];
        snprintf(c->name, sizeof c->name, "%s", f[0]);
        snprintf(c->state, sizeof c->state, "%s", f[1]);
        snprintf(c->detail, sizeof c->detail, "%s", f[2]);
        c->fps = atof(f[3]);
        c->temp_c = atof(f[4]);
        c->connected = !strcmp(f[5], "true") || !strcmp(f[5], "1");
    }
    /* Limelights without VisionHealth (1.x): their own tables say whether they answer */
    names_t nm;
    int nl = collect("/limelight", "/tv", &nm);
    r->tag_seen = -1;
    for (int i = 0; i < nl; i++) {
        char name[64], t[96];
        snprintf(name, sizeof name, "%s", nm.names[i] + 1);
        name[strlen(name) - 3] = 0;
        double tid, tv;
        snprintf(t, sizeof t, "/%s/tv", name);
        bool have_tv = num(t, &tv);
        snprintf(t, sizeof t, "/%s/tid", name);
        if (have_tv && tv > 0 && num(t, &tid)) r->tag_seen = (int)tid;
        if (r->ncameras >= CAT_MAX_CAMERAS) continue;
        bool known = false;
        for (int k = 0; k < r->ncameras; k++) if (!strcmp(r->cameras[k].name, name)) known = true;
        if (known) continue;
        cat_camera_t *c = &r->cameras[r->ncameras++];
        snprintf(c->name, sizeof c->name, "%s", name);
        snprintf(c->state, sizeof c->state, "%s", have_tv ? (tv > 0 ? "OK" : "NO_TARGETS") : "STALE");
        c->detail[0] = 0;
        c->fps = c->temp_c = NAN;
        c->connected = have_tv;
    }
}

static void read_systemcore(cat_robot_t *r)
{
    r->have_sc = num("/Catalyst/Systemcore/CpuPercent", &r->sc_cpu);
    r->sc_temp = num_or("/Catalyst/Systemcore/TempCelsius", NAN);
    r->sc_ram = num_or("/Catalyst/Systemcore/RamFraction", NAN);
    r->sc_storage = num_or("/Catalyst/Systemcore/StorageFraction", NAN);
    r->sc_emmc = (int)num_or("/Catalyst/Systemcore/EmmcPreEol", 0);
    if (r->have_sc) r->line2 = true;
}

static void read_tunables(cat_robot_t *r)
{
    static char manifest[8192];
    r->ntunables = 0;
    r->have_manifest = str("/Catalyst/Tunables/.manifest", manifest, sizeof manifest);
    if (r->have_manifest) {
        static jl_tok_t tok[1024];
        int n = jl_parse(manifest, strlen(manifest), tok, 1024);
        jl_doc_t d = { manifest, tok, n };
        if (n > 0 && tok[0].type == JL_ARR) {
            for (int i = 0; i < tok[0].size && r->ntunables < CAT_MAX_TUNABLES; i++) {
                int e = jl_at(&d, 0, i);
                cat_tunable_t *t = &r->tunables[r->ntunables];
                if (!jl_str(&d, jl_get(&d, e, "key"), t->key, sizeof t->key) || !t->key[0]) continue;
                if (t->key[0] != '/') continue;
                if (!jl_str(&d, jl_get(&d, e, "name"), t->name, sizeof t->name)) {
                    const char *slash = strrchr(t->key, '/');
                    snprintf(t->name, sizeof t->name, "%s", slash ? slash + 1 : t->key);
                }
                jl_str(&d, jl_get(&d, e, "group"), t->group, sizeof t->group);
                jl_str(&d, jl_get(&d, e, "unit"), t->unit, sizeof t->unit);
                t->min = jl_num(&d, jl_get(&d, e, "min"), NAN);
                t->max = jl_num(&d, jl_get(&d, e, "max"), NAN);
                t->step = jl_num(&d, jl_get(&d, e, "step"), NAN);
                r->ntunables++;
            }
        }
    } else {
        /* 1.x TunableNumbers: no manifest, no ranges. Console can't tune these; the tablet can. */
        names_t nm;
        int n = collect("/Catalyst/Tuning/", NULL, &nm);
        for (int i = 0; i < n && r->ntunables < CAT_MAX_TUNABLES; i++) {
            cat_tunable_t *t = &r->tunables[r->ntunables++];
            memset(t, 0, sizeof *t);
            snprintf(t->key, sizeof t->key, "%s", nm.names[i]);
            const char *p = nm.names[i] + strlen("/Catalyst/Tuning/");
            const char *slash = strrchr(p, '/');
            snprintf(t->name, sizeof t->name, "%s", slash ? slash + 1 : p);
            if (slash) copy(t->group, sizeof t->group, p, (size_t)(slash - p));
            t->min = t->max = t->step = NAN;
        }
    }
    for (int i = 0; i < r->ntunables; i++) {
        cat_tunable_t *t = &r->tunables[i];
        char type[24] = "";
        nt4_info(g_nt, t->key, type, sizeof type, NULL, NULL);
        t->is_bool = !strcmp(type, "boolean");
        t->have = num(t->key, &t->value);
    }
}

static void read_autos(cat_robot_t *r)
{
    static const char *const bases[] = { "/Auto Selector", "/SmartDashboard/Auto Selector" };
    static char buf[2048];
    r->have_autos = false;
    r->nautos = 0;
    for (int b = 0; b < 2 && !r->have_autos; b++) {
        char t[80];
        const char *items[CAT_MAX_AUTOS];
        snprintf(t, sizeof t, "%s/options", bases[b]);
        int n = nt4_get_strings(g_nt, t, buf, sizeof buf, items, CAT_MAX_AUTOS);
        if (n < 0) continue;
        r->have_autos = true;
        snprintf(r->auto_base, sizeof r->auto_base, "%s", bases[b]);
        for (int i = 0; i < n; i++) snprintf(r->autos[r->nautos++], sizeof r->autos[0], "%s", items[i]);
        snprintf(t, sizeof t, "%s/selected", bases[b]);
        str(t, r->auto_selected, sizeof r->auto_selected);
        snprintf(t, sizeof t, "%s/active", bases[b]);
        str(t, r->auto_active, sizeof r->auto_active);
    }
}

static void read_checks(cat_robot_t *r)
{
    bool ready;
    r->have_preflight = nt4_get_bool(g_nt, "/Catalyst/Preflight/Ready", &ready);
    r->preflight_ready = ready;
    str("/Catalyst/Preflight/Summary", r->preflight_summary, sizeof r->preflight_summary);
    r->nfindings = 0;
    static char buf[4096];
    const char *items[CAT_MAX_FINDINGS];
    int n = nt4_get_strings(g_nt, "/Catalyst/Preflight/Findings", buf, sizeof buf, items, CAT_MAX_FINDINGS);
    for (int i = 0; i < n; i++) {
        cat_finding_t *f = &r->findings[r->nfindings++];
        const char *s = items[i];
        f->sev = !strncmp(s, "[BLOCKER]", 9) ? CAT_SEV_ERROR : !strncmp(s, "[warn]", 6) ? CAT_SEV_WARN : CAT_SEV_INFO;
        const char *close = strchr(s, ']');
        s = close ? close + 1 : s;
        while (*s == ' ') s++;
        snprintf(f->text, sizeof f->text, "%s", s);
    }
    names_t nm;
    r->have_systemcheck = collect("/Catalyst/SystemCheck/", "/Ready", &nm) > 0;
    if (r->have_systemcheck) {
        char base[128], t[160];
        snprintf(base, sizeof base, "%s", nm.names[0]);
        base[strlen(base) - 6] = 0;
        r->systemcheck_ready = flag(nm.names[0], false);
        snprintf(t, sizeof t, "%s/Report", base);
        str(t, r->systemcheck_report, sizeof r->systemcheck_report);
    }
    r->have_wheel_radius = str("/Catalyst/Calibration/WheelRadius/Status", r->wheel_radius_status, sizeof r->wheel_radius_status);
    r->wheel_radius_in = num_or("/Catalyst/Calibration/WheelRadius/CorrectedRadiusInches", NAN);
    r->wheel_radius_change = num_or("/Catalyst/Calibration/WheelRadius/PercentChange", NAN);
    bool sr;
    r->have_start_check = nt4_get_bool(g_nt, "/Catalyst/Auto/StartCheck/Ready", &sr);
    r->start_ready = sr;
    r->start_dist_m = num_or("/Catalyst/Auto/StartCheck/DistanceMeters", NAN);
    r->start_heading_deg = num_or("/Catalyst/Auto/StartCheck/HeadingErrorDeg", NAN);
}

void cat_model_update(cat_robot_t *r)
{
    nt4_status_t st;
    nt4_status(g_nt, &st);
    r->connected = st.state == NT4_CONNECTED;
    snprintf(r->address, sizeof r->address, "%s", st.address);
    r->rtt_ms = st.rtt_us / 1000.0;
    r->topics = st.topics;
    r->connected_s = r->connected ? (nt4_now_us() - st.connected_since_us) / 1e6 : 0;
    if (!r->connected) {
        /* Console clears its store on disconnect: a stale value is never shown as live */
        char addr[64];
        double rtt = r->rtt_ms;
        snprintf(addr, sizeof addr, "%s", r->address);
        memset(r, 0, sizeof *r);
        snprintf(r->address, sizeof r->address, "%s", addr);
        r->rtt_ms = rtt;
        r->match_time = r->brownout_v = r->predicted_v = r->total_current = NAN;
        r->loop_last_ms = r->loop_avg_ms = r->loop_max_ms = r->heading_deg = NAN;
        r->pd_voltage = r->pd_total = NAN;
        for (int i = 0; i < 24; i++) r->pd_amps[i] = NAN;
        r->tag_seen = -1;
        return;
    }
    read_identity(r);
    read_mode(r);
    read_systemcore(r);
    read_power(r);
    read_loop(r);
    read_can(r);
    read_alerts(r);
    read_mechs(r);
    read_pose(r);
    read_vision(r);
    read_tunables(r);
    read_autos(r);
    read_checks(r);
}

int cat_addresses(int team, const char *override_addr, char out[][64], int max)
{
    int n = 0;
    if (override_addr && override_addr[0] && n < max) snprintf(out[n++], 64, "%s", override_addr);
    if (n < max) snprintf(out[n++], 64, "robot.local");
    /* Systemcore's USB-C gadget: its RNDIS side answers at 172.26.0.1, its ECM side at 172.27.0.1 */
    if (n < max) snprintf(out[n++], 64, "172.26.0.1");
    if (n < max) snprintf(out[n++], 64, "172.27.0.1");
    if (n < max) snprintf(out[n++], 64, "172.30.0.1");
    if (team > 0 && team < 10000) {
        if (n < max) snprintf(out[n++], 64, "10.%d.%d.2", team / 100, team % 100);
        if (n < max) snprintf(out[n++], 64, "roborio-%d-frc.local", team);
    }
    if (n < max) snprintf(out[n++], 64, "172.22.11.2");
    return n;
}

bool cat_set_tunable(const cat_robot_t *r, int i, double v)
{
    if (i < 0 || i >= r->ntunables) return false;
    const cat_tunable_t *t = &r->tunables[i];
    /* Console's write guard (main.rs:281-305): absolute, never FMSInfo or schema */
    if (t->key[0] != '/' || !strncmp(t->key, "/FMSInfo", 8) || !strncmp(t->key, "/.schema", 8)) return false;
    if (t->min == t->min && v < t->min) v = t->min;
    if (t->max == t->max && v > t->max) v = t->max;
    if (t->is_bool) nt4_set_bool(g_nt, t->key, v != 0);
    else nt4_set_double(g_nt, t->key, v);
    return true;
}

bool cat_select_auto(const cat_robot_t *r, const char *name)
{
    if (!r->have_autos) return false;
    char t[80];
    snprintf(t, sizeof t, "%s/selected", r->auto_base);
    nt4_set_string(g_nt, t, name);
    return true;
}

void cat_blink_limelight(const char *name, bool on)
{
    char t[96];
    snprintf(t, sizeof t, "/%s/ledMode", name);
    nt4_set_double(g_nt, t, on ? 2 : 0);
}

int cat_battery_band(double v)
{
    if (v >= 12.5) return 2;
    if (v >= 12.2) return 1;
    return 0;
}

const char *cat_mode_name(const cat_robot_t *r)
{
    if (!r->connected) return "offline";
    if (!r->have_mode) return "no program";
    if (r->estop) return "e-stopped";
    if (!r->enabled) return "disabled";
    if (r->autonomous) return "auto";
    if (r->test) return r->line2 ? "utility" : "test";
    return "teleop";
}
