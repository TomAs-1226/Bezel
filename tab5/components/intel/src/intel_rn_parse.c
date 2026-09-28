/* The robot network's own documents: Systemcore's vision aggregator (:4810/api/cameras), a Limelight's REST
 * status (:5807/status) and CTRE's Phoenix diagnostics server (:1250/?action=getdevices). Pure: host-tested.
 *
 * The aggregator's list is read by the names catalyst-agent passes through from it (docs/systemcore.md:
 * name, host, ip, ntConnected, pipelineType, and the agent's own fps/temperatureC/cpuPercent/ramPercent), as a
 * bare array or under "cameras". A Limelight's /status is {name, fps, cpu, ram, temp, pipelineIndex,
 * pipelineType, ...}. Phoenix's getdevices is {DeviceArray: [{Model, ID, Name, CurrentVers, ...}]}; Phoenix 6's
 * server may name them in lower case, which is tried too. */
#include "intel.h"
#include "intel_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const K_NAME[] = { "name", "ntName", "host", "hostname", NULL };
static const char *const K_IP[] = { "ip", "address", "ipAddress", NULL };
static const char *const K_NT[] = { "ntConnected", "nt", "connected", NULL };
static const char *const K_FPS[] = { "fps", NULL };
static const char *const K_TEMP[] = { "temperatureC", "temp", "temperature", NULL };
static const char *const K_CPU[] = { "cpuPercent", "cpu", NULL };
static const char *const K_RAM[] = { "ramPercent", "ram", NULL };
static const char *const K_PIPE[] = { "pipelineIndex", "pipeline", NULL };
static const char *const K_PTYPE[] = { "pipelineType", "pipeType", NULL };

static void cam_init(rn_cam_t *c)
{
    memset(c, 0, sizeof *c);
    c->fps = c->temp = c->cpu = c->ram = c->age_s = NAN;
    c->pipeline = -1;
}

static void cam_fill(const ix_json_t *j, int o, rn_cam_t *c)
{
    char b[32];
    if (ix_tok_str(j, ix_get_any(j, o, K_NAME), b, sizeof b)) snprintf(c->name, sizeof c->name, "%s", b);
    if (ix_tok_str(j, ix_get_any(j, o, K_IP), b, sizeof b)) {
        /* "10.58.5.11/24" or "10.58.5.11:5807": the address alone */
        b[strcspn(b, "/:")] = 0;
        snprintf(c->ip, sizeof c->ip, "%s", b);
    }
    int nt = ix_get_any(j, o, K_NT);
    if (nt >= 0) c->nt = j->d.t[nt].type == JL_TRUE || (j->d.t[nt].type == JL_NUM && ix_tok_num(j, nt, 0) != 0);
    double v;
    if (!isnan(v = ix_tok_num(j, ix_get_any(j, o, K_FPS), NAN))) c->fps = v;
    if (!isnan(v = ix_tok_num(j, ix_get_any(j, o, K_TEMP), NAN))) c->temp = v;
    if (!isnan(v = ix_tok_num(j, ix_get_any(j, o, K_CPU), NAN))) c->cpu = v;
    if (!isnan(v = ix_tok_num(j, ix_get_any(j, o, K_RAM), NAN))) c->ram = v;
    if (!isnan(v = ix_tok_num(j, ix_get_any(j, o, K_PIPE), NAN))) c->pipeline = (int)v;
    ix_tok_str(j, ix_get_any(j, o, K_PTYPE), c->pipe_type, sizeof c->pipe_type);
}

int rn_parse_cameras(const char *js, size_t len, rn_cam_t *out, int max)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return -1;
    int arr = j.d.t[0].type == JL_ARR ? 0 : ix_get(&j, 0, "cameras");
    if (arr >= 0 && j.d.t[arr].type == JL_OBJ) arr = ix_get(&j, arr, "cameras"); /* the agent's {cameras:{cameras}} */
    if (arr < 0 || j.d.t[arr].type != JL_ARR) {
        ix_free(&j);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < j.d.t[arr].size && n < max; i++) {
        int o = jl_at(&j.d, arr, i);
        if (o < 0 || j.d.t[o].type != JL_OBJ) continue;
        cam_init(&out[n]);
        cam_fill(&j, o, &out[n]);
        if (out[n].name[0] || out[n].ip[0]) n++;
    }
    ix_free(&j);
    return n;
}

bool rn_parse_ll_status(const char *js, size_t len, rn_cam_t *cam)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return false;
    bool ok = j.d.t[0].type == JL_OBJ && (ix_get(&j, 0, "fps") >= 0 || ix_get(&j, 0, "temp") >= 0 ||
                                          ix_get(&j, 0, "pipelineIndex") >= 0);
    if (ok) {
        char keep[32];
        snprintf(keep, sizeof keep, "%s", cam->name);
        cam_fill(&j, 0, cam);
        if (keep[0]) snprintf(cam->name, sizeof cam->name, "%s", keep); /* the aggregator's name wins */
        cam->answered = true;
    }
    ix_free(&j);
    return ok;
}

static const char *const K_ARR[] = { "DeviceArray", "deviceArray", "devices", "Devices", NULL };
static const char *const K_MODEL[] = { "Model", "model", "Type", "type", NULL };
static const char *const K_DNAME[] = { "Name", "name", NULL };
static const char *const K_FW[] = { "CurrentVers", "currentVers", "FirmwareVersion", "firmwareVersion", "firmware",
                                    "Version", "version", NULL };
static const char *const K_ID[] = { "ID", "id", "Id", "DeviceId", "deviceId", NULL };
static const char *const K_BUS[] = { "CANbus", "canbus", "CanBus", "bus", "Bus", NULL };

int rn_parse_phoenix(const char *js, size_t len, rn_dev_t *out, int max)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return -1;
    int arr = j.d.t[0].type == JL_ARR ? 0 : ix_get_any(&j, 0, K_ARR);
    if (arr < 0 || j.d.t[arr].type != JL_ARR) {
        ix_free(&j);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < j.d.t[arr].size && n < max; i++) {
        int o = jl_at(&j.d, arr, i);
        if (o < 0 || j.d.t[o].type != JL_OBJ) continue;
        rn_dev_t *d = &out[n];
        memset(d, 0, sizeof *d);
        ix_tok_str(&j, ix_get_any(&j, o, K_MODEL), d->model, sizeof d->model);
        ix_tok_str(&j, ix_get_any(&j, o, K_DNAME), d->name, sizeof d->name);
        ix_tok_str(&j, ix_get_any(&j, o, K_FW), d->firmware, sizeof d->firmware);
        ix_tok_str(&j, ix_get_any(&j, o, K_BUS), d->bus, sizeof d->bus);
        d->id = (int)ix_tok_num(&j, ix_get_any(&j, o, K_ID), -1);
        if (d->model[0] || d->name[0]) n++;
    }
    ix_free(&j);
    return n;
}
