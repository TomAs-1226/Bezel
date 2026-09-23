/* cat_sc — the pure half: catalyst-agent's JSON, motor history, the controls manifest, state lanes and
 * the recorder's CSV. No threads, no I/O; test/test_sc.c holds it to catalyst_agent.py's shapes. */
#include "cat_sc.h"
#include "json_lite.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ JSON helpers */

static double num(const jl_doc_t *d, int obj, const char *key) { return jl_num(d, jl_get(d, obj, key), NAN); }

static void str(const jl_doc_t *d, int obj, const char *key, char *out, size_t n)
{
    int t = jl_get(d, obj, key);
    /* null reads as absent, not as the text "null" */
    if (t < 0 || d->t[t].type == JL_NULL) {
        if (n) out[0] = 0;
        return;
    }
    jl_str(d, t, out, n);
}

static bool boolean(const jl_doc_t *d, int obj, const char *key) { return jl_bool(d, jl_get(d, obj, key), false); }

static int count(const jl_doc_t *d, int arr) { return arr >= 0 && d->t[arr].type == JL_ARR ? d->t[arr].size : 0; }

/* Tokenizes with a heap buffer sized to the text (as nt4.c does for announcements), retrying larger for
 * text that is mostly short numbers. */
static jl_tok_t *tokenize(const char *js, size_t len, int *ntok)
{
    for (size_t div = 4; div >= 1; div /= 2) {
        int max = (int)(len / div) + 32;
        jl_tok_t *t = malloc(sizeof(jl_tok_t) * (size_t)max);
        if (!t) return NULL;
        int n = jl_parse(js, len, t, max);
        if (n > 0) {
            *ntok = n;
            return t;
        }
        free(t);
        if (div == 1) break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ /api/system */

static void parse_identity(const jl_doc_t *d, int o, cat_agent_t *a)
{
    str(d, o, "hostname", a->hostname, sizeof a->hostname);
    str(d, o, "os", a->os, sizeof a->os);
    str(d, o, "osVersion", a->os_version, sizeof a->os_version);
    str(d, o, "kernel", a->kernel, sizeof a->kernel);
    str(d, o, "model", a->model, sizeof a->model);
    str(d, o, "agentVersion", a->agent_version, sizeof a->agent_version);
    a->uptime_s = num(d, o, "uptimeSeconds");
}

static void parse_cpu(const jl_doc_t *d, int o, cat_agent_t *a)
{
    int cores = jl_get(d, o, "cores");
    for (int i = 0; i < count(d, cores) && a->ncores < CAT_AG_CORES; i++) {
        int c = jl_at(d, cores, i);
        cat_ag_core_t *k = &a->cores[a->ncores++];
        k->core = (int)jl_num(d, jl_get(d, c, "core"), i);
        k->percent = num(d, c, "percent");
        k->mhz = num(d, c, "mhz");
    }
    int load = jl_get(d, o, "loadAverage");
    if (count(d, load) == 3) {
        a->have_load = true;
        for (int i = 0; i < 3; i++) a->load[i] = jl_num(d, jl_at(d, load, i), NAN);
    }
    str(d, o, "model", a->cpu_model, sizeof a->cpu_model);
    int t = jl_get(d, o, "throttling");
    if (t >= 0 && d->t[t].type == JL_OBJ) {
        a->have_throttle = true;
        a->thr_under_now = boolean(d, t, "underVoltageNow");
        a->thr_capped_now = boolean(d, t, "frequencyCappedNow");
        a->thr_now = boolean(d, t, "throttledNow");
        a->thr_soft_now = boolean(d, t, "softTempLimitNow");
        a->thr_since = boolean(d, t, "throttledSinceBoot");
        a->thr_under_since = boolean(d, t, "underVoltageSinceBoot");
    }
}

static void parse_storage(const jl_doc_t *d, int o, cat_agent_t *a)
{
    int mounts = jl_get(d, o, "mounts");
    for (int i = 0; i < count(d, mounts) && a->nmounts < CAT_AG_MOUNTS; i++) {
        int m = jl_at(d, mounts, i);
        cat_ag_mount_t *k = &a->mounts[a->nmounts++];
        str(d, m, "mount", k->mount, sizeof k->mount);
        str(d, m, "device", k->device, sizeof k->device);
        str(d, m, "filesystem", k->fs, sizeof k->fs);
        k->total = num(d, m, "totalBytes");
        k->used = num(d, m, "usedBytes");
        k->free = num(d, m, "freeBytes");
    }
    int dirs = jl_get(d, o, "directories");
    for (int i = 0; i < count(d, dirs) && a->ndirs < CAT_AG_DIRS; i++) {
        int m = jl_at(d, dirs, i);
        cat_ag_dir_t *k = &a->dirs[a->ndirs++];
        str(d, m, "path", k->path, sizeof k->path);
        k->bytes = num(d, m, "bytes");
    }
}

static void parse_procs(const jl_doc_t *d, int o, cat_agent_t *a)
{
    a->proc_count = (int)jl_num(d, jl_get(d, o, "count"), 0);
    int top = jl_get(d, o, "topByCpu");
    for (int i = 0; i < count(d, top) && a->nprocs < CAT_AG_PROCS; i++) {
        int p = jl_at(d, top, i);
        cat_ag_proc_t *k = &a->procs[a->nprocs++];
        k->pid = (int)jl_num(d, jl_get(d, p, "pid"), 0);
        str(d, p, "name", k->name, sizeof k->name);
        k->cpu = num(d, p, "cpuPercent");
        k->rss = num(d, p, "rssBytes");
    }
}

static void parse_can(const jl_doc_t *d, int arr, cat_agent_t *a)
{
    for (int i = 0; i < count(d, arr) && a->ncan < CAT_AG_CAN; i++) {
        int c = jl_at(d, arr, i);
        cat_ag_can_t *k = &a->can[a->ncan++];
        str(d, c, "name", k->name, sizeof k->name);
        k->up = boolean(d, c, "up");
        str(d, c, "state", k->state, sizeof k->state);
        k->bitrate = num(d, c, "bitrate");
        k->restarts = num(d, c, "restarts");
        k->rx = num(d, c, "rxPackets");
        k->tx = num(d, c, "txPackets");
        k->rx_err = num(d, c, "rxErrors");
        k->tx_err = num(d, c, "txErrors");
        k->rx_drop = num(d, c, "rxDropped");
        k->tx_drop = num(d, c, "txDropped");
    }
}

static void parse_network(const jl_doc_t *d, int arr, cat_agent_t *a)
{
    for (int i = 0; i < count(d, arr) && a->nnics < CAT_AG_NICS; i++) {
        int c = jl_at(d, arr, i);
        cat_ag_nic_t *k = &a->nics[a->nnics++];
        str(d, c, "name", k->name, sizeof k->name);
        k->up = boolean(d, c, "up");
        str(d, c, "mac", k->mac, sizeof k->mac);
        k->mbps = num(d, c, "speedMbps");
        if (k->mbps < 0) k->mbps = NAN; /* the kernel says -1 for "unknown" */
        int ad = jl_get(d, c, "addresses");
        size_t o = 0;
        k->addrs[0] = 0;
        for (int j = 0; j < count(d, ad) && o + 4 < sizeof k->addrs; j++) {
            char one[24];
            jl_str(d, jl_at(d, ad, j), one, sizeof one);
            o += (size_t)snprintf(k->addrs + o, sizeof k->addrs - o, "%s%s", j ? ", " : "", one);
        }
        int w = jl_get(d, c, "wireless");
        k->wireless = w >= 0 && d->t[w].type == JL_OBJ;
        k->quality = k->wireless ? num(d, w, "linkQuality") : NAN;
        k->dbm = k->wireless ? num(d, w, "signalDbm") : NAN;
    }
}

static void parse_program(const jl_doc_t *d, int o, cat_agent_t *a)
{
    if (o < 0 || d->t[o].type != JL_OBJ) return;
    str(d, o, "state", a->state, sizeof a->state);
    a->have_program = a->state[0] != 0;
    str(d, o, "unit", a->unit, sizeof a->unit);
    str(d, o, "subState", a->substate, sizeof a->substate);
    a->restarts = num(d, o, "restarts");
    a->running_s = num(d, o, "runningForSeconds");
    a->memory = num(d, o, "memoryBytes");
    a->pid = (int)jl_num(d, jl_get(d, o, "pid"), 0);
    int log = jl_get(d, o, "log");
    int n = count(d, log), from = n > CAT_AG_LOG ? n - CAT_AG_LOG : 0;
    for (int i = from; i < n; i++) jl_str(d, jl_at(d, log, i), a->log[a->nlog++], sizeof a->log[0]);
}

static void parse_cameras(const jl_doc_t *d, int o, cat_agent_t *a)
{
    if (o < 0 || d->t[o].type != JL_OBJ) return;
    a->cams_available = boolean(d, o, "available");
    str(d, o, "reason", a->cams_reason, sizeof a->cams_reason);
    int arr = jl_get(d, o, "cameras");
    for (int i = 0; i < count(d, arr) && a->ncams < CAT_AG_CAMS; i++) {
        int c = jl_at(d, arr, i);
        cat_ag_cam_t *k = &a->cams[a->ncams++];
        str(d, c, "name", k->name, sizeof k->name);
        if (!k->name[0]) str(d, c, "host", k->name, sizeof k->name);
        str(d, c, "ip", k->ip, sizeof k->ip);
        str(d, c, "type", k->type, sizeof k->type);
        str(d, c, "pipelineType", k->pipeline, sizeof k->pipeline);
        k->nt = boolean(d, c, "ntConnected");
        k->reachable = boolean(d, c, "statusReachable");
        k->fps = num(d, c, "fps");
        k->temp = num(d, c, "temperatureC");
        k->cpu = num(d, c, "cpuPercent");
        k->ram = num(d, c, "ramPercent");
    }
}

bool cat_agent_parse(const char *json, size_t len, cat_agent_t *a)
{
    memset(a, 0, sizeof *a);
    a->uptime_s = a->mem_total = a->mem_avail = a->mem_used = a->mem_cached = NAN;
    a->restarts = a->running_s = a->memory = a->sampled_at = NAN;
    int n = 0;
    jl_tok_t *t = tokenize(json, len, &n);
    if (!t) return false;
    jl_doc_t d = { json, t, n };
    bool ok = t[0].type == JL_OBJ && jl_get(&d, 0, "identity") >= 0;
    if (ok) {
        parse_identity(&d, jl_get(&d, 0, "identity"), a);
        parse_cpu(&d, jl_get(&d, 0, "cpu"), a);
        int th = jl_get(&d, 0, "thermal");
        for (int i = 0; i < count(&d, th) && a->nzones < CAT_AG_ZONES; i++) {
            int z = jl_at(&d, th, i);
            str(&d, z, "zone", a->zones[a->nzones].zone, sizeof a->zones[0].zone);
            a->zones[a->nzones++].celsius = num(&d, z, "celsius");
        }
        int m = jl_get(&d, 0, "memory");
        a->mem_total = num(&d, m, "totalBytes");
        a->mem_avail = num(&d, m, "availableBytes");
        a->mem_used = num(&d, m, "usedBytes");
        a->mem_cached = num(&d, m, "cachedBytes");
        parse_storage(&d, jl_get(&d, 0, "storage"), a);
        parse_procs(&d, jl_get(&d, 0, "processes"), a);
        parse_can(&d, jl_get(&d, 0, "can"), a);
        parse_network(&d, jl_get(&d, 0, "network"), a);
        parse_program(&d, jl_get(&d, 0, "robotProgram"), a);
        parse_cameras(&d, jl_get(&d, 0, "cameras"), a);
        int mh = jl_get(&d, 0, "motorHistory");
        int devs = jl_get(&d, mh, "devices");
        a->mh_devices = count(&d, devs);
        for (int i = 0; i < a->mh_devices; i++) {
            char kind[12];
            str(&d, jl_at(&d, devs, i), "kind", kind, sizeof kind);
            a->mh_motors += !strcmp(kind, "motor");
        }
        a->sampled_at = num(&d, 0, "sampledAt");
        a->valid = true;
    }
    free(t);
    return ok;
}

/* ------------------------------------------------------------------ motor history */

static void mh_clear(cat_mh_motor_t *m)
{
    memset(m, 0, sizeof *m);
    m->id = -1;
    snprintf(m->kind, sizeof m->kind, "device");
}

bool cat_mh_parse_row(const char *line, cat_mh_motor_t *m)
{
    mh_clear(m);
    if (!line) return false;
    /* 20 fields; a name is a string a team wrote and may hold a '|' of its own, so every separator
     * beyond the 19 the format has belongs to the name (field 5) */
    const char *f[40];
    size_t fl[40];
    int nf = 0;
    const char *p = line;
    for (;;) {
        const char *bar = strchr(p, '|');
        if (nf < 40) {
            f[nf] = p;
            fl[nf] = bar ? (size_t)(bar - p) : strlen(p);
            nf++;
        }
        if (!bar) break;
        p = bar + 1;
    }
    if (nf < 20) return false;
    int extra = nf - 20;
    if (extra) { /* rejoin the name */
        fl[5] = (size_t)(f[5 + extra] + fl[5 + extra] - f[5]);
        for (int i = 6; i + extra < nf; i++) {
            f[i] = f[i + extra];
            fl[i] = fl[i + extra];
        }
    }
#define FIELD(i, dst) snprintf(dst, sizeof dst, "%.*s", (int)fl[i], f[i])
    char buf[48];
#define NUM(i) (FIELD(i, buf), strtod(buf, NULL))
    FIELD(0, m->serial);
    FIELD(1, m->model);
    FIELD(2, m->kind);
    if (!m->kind[0]) snprintf(m->kind, sizeof m->kind, "device");
    FIELD(3, m->bus);
    FIELD(4, buf);
    m->id = buf[0] ? atoi(buf) : -1;
    FIELD(5, m->name);
    FIELD(6, m->firmware);
    m->powered_s = NUM(7);
    m->running_s = NUM(8);
    m->loaded_s = NUM(9);
    m->revs = NUM(10);
    m->peak_a = NUM(11);
    m->peak_c = NUM(12);
    m->hot_s = NUM(13);
    m->energy_j = NUM(14);
    m->boots = (int)NUM(15);
    m->first_ms = NUM(16);
    m->last_ms = NUM(17);
    m->identities = (int)NUM(18);
    m->sticky = (uint64_t)NUM(19);
#undef NUM
#undef FIELD
    return m->serial[0] != 0;
}

/* A small scanner over the document's top level, so each device is tokenized on its own. */
static size_t ws(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

static size_t skip_string(const char *s, size_t n, size_t i)
{
    for (i++; i < n; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == '"') return i + 1;
    }
    return n;
}

static size_t skip_value(const char *s, size_t n, size_t i)
{
    if (i >= n) return n;
    if (s[i] == '"') return skip_string(s, n, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < n) {
            char c = s[i];
            if (c == '"') {
                i = skip_string(s, n, i);
                continue;
            }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                if (--depth == 0) return i + 1;
            }
            i++;
        }
        return n;
    }
    while (i < n && !strchr(",]} \t\r\n", s[i])) i++;
    return i;
}

static double scalar(const char *s, size_t n)
{
    jl_tok_t t[2];
    if (jl_parse(s, n, t, 2) != 1) return NAN;
    jl_doc_t d = { s, t, 1 };
    return jl_num(&d, 0, NAN);
}

static void mh_device(const jl_doc_t *d, cat_mh_motor_t *m)
{
    mh_clear(m);
    str(d, 0, "serial", m->serial, sizeof m->serial);
    str(d, 0, "model", m->model, sizeof m->model);
    str(d, 0, "kind", m->kind, sizeof m->kind);
    if (!m->kind[0]) snprintf(m->kind, sizeof m->kind, "device");
    str(d, 0, "hardwareRev", m->hw_rev, sizeof m->hw_rev);
    str(d, 0, "manufactured", m->made, sizeof m->made);
    m->first_ms = jl_num(d, jl_get(d, 0, "firstSeenMs"), 0);
    m->last_ms = jl_num(d, jl_get(d, 0, "lastSeenMs"), 0);
    m->boots = (int)jl_num(d, jl_get(d, 0, "boots"), 0);
    /* the file writes totals it has; a motor with none yet has zeros, which here are true zeros */
    int t = jl_get(d, 0, "totals");
    m->powered_s = jl_num(d, jl_get(d, t, "poweredSeconds"), 0);
    m->running_s = jl_num(d, jl_get(d, t, "runningSeconds"), 0);
    m->loaded_s = jl_num(d, jl_get(d, t, "loadedSeconds"), 0);
    m->revs = jl_num(d, jl_get(d, t, "revolutions"), 0);
    m->energy_j = jl_num(d, jl_get(d, t, "energyJoules"), 0);
    m->peak_a = jl_num(d, jl_get(d, t, "peakStatorAmps"), 0);
    m->peak_c = jl_num(d, jl_get(d, t, "peakTempC"), 0);
    m->hot_s = jl_num(d, jl_get(d, t, "hotSeconds"), 0);
    m->sticky = (uint64_t)jl_num(d, jl_get(d, t, "stickyFaults"), 0);
    int ids = jl_get(d, 0, "identities");
    int ni = count(d, ids);
    m->identities = ni;
    for (int i = ni > CAT_MH_IDS ? ni - CAT_MH_IDS : 0; i < ni; i++) {
        int o = jl_at(d, ids, i);
        cat_mh_ident_t *k = &m->ids[m->nids++];
        k->id = (int)jl_num(d, jl_get(d, o, "id"), -1);
        str(d, o, "name", k->name, sizeof k->name);
        str(d, o, "bus", k->bus, sizeof k->bus);
        str(d, o, "firmware", k->firmware, sizeof k->firmware);
        k->first_ms = jl_num(d, jl_get(d, o, "firstSeenMs"), 0);
        k->last_ms = jl_num(d, jl_get(d, o, "lastSeenMs"), 0);
    }
    if (m->nids) {
        const cat_mh_ident_t *last = &m->ids[m->nids - 1];
        m->id = last->id;
        snprintf(m->name, sizeof m->name, "%s", last->name);
        snprintf(m->bus, sizeof m->bus, "%s", last->bus);
        snprintf(m->firmware, sizeof m->firmware, "%s", last->firmware);
    }
    int ss = jl_get(d, 0, "sessions");
    int ns = count(d, ss);
    for (int i = ns > CAT_MH_SESSIONS ? ns - CAT_MH_SESSIONS : 0; i < ns; i++) {
        int o = jl_at(d, ss, i);
        cat_mh_session_t *k = &m->sess[m->nsess++];
        k->start_ms = jl_num(d, jl_get(d, o, "startMs"), 0);
        k->seconds = jl_num(d, jl_get(d, o, "seconds"), 0);
        k->running_s = jl_num(d, jl_get(d, o, "runningSeconds"), 0);
        k->revs = jl_num(d, jl_get(d, o, "revolutions"), 0);
        k->peak_a = jl_num(d, jl_get(d, o, "peakAmps"), 0);
        k->peak_c = jl_num(d, jl_get(d, o, "peakTempC"), 0);
        k->hot_s = jl_num(d, jl_get(d, o, "hotSeconds"), 0);
    }
}

int cat_mh_parse_doc(const char *s, size_t n, cat_mh_motor_t *out, int max, cat_mh_meta_t *meta)
{
    memset(meta, 0, sizeof *meta);
    meta->updated_ms = NAN;
    meta->clock_trusted = true;
    size_t i = ws(s, n, 0);
    if (i >= n || s[i] != '{') {
        snprintf(meta->error, sizeof meta->error, "not a motor history file");
        return -1;
    }
    enum { TOK = 2048 };
    jl_tok_t *tok = NULL;
    int got = 0;
    bool devices = false;
    i++;
    for (;;) {
        i = ws(s, n, i);
        if (i >= n || s[i] != '"') break;
        size_t k0 = i + 1;
        i = skip_string(s, n, i);
        size_t k1 = i - 1;
        i = ws(s, n, i);
        if (i >= n || s[i] != ':') break;
        i = ws(s, n, i + 1);
        size_t v0 = i;
        i = skip_value(s, n, i);
        size_t kl = k1 - k0;
#define KEY(k) (kl == strlen(k) && !memcmp(s + k0, k, kl))
        if (KEY("updatedMs")) meta->updated_ms = scalar(s + v0, i - v0);
        else if (KEY("clockTrusted")) meta->clock_trusted = !(i - v0 == 5 && !memcmp(s + v0, "false", 5));
        else if (KEY("error") && s[v0] == '"') {
            size_t l = i - v0 >= 2 ? i - v0 - 2 : 0;
            snprintf(meta->error, sizeof meta->error, "%.*s", (int)l, s + v0 + 1);
        } else if (KEY("devices") && s[v0] == '[') {
            devices = true;
            if (!tok) tok = malloc(sizeof(jl_tok_t) * TOK);
            if (!tok) break;
            size_t j = v0 + 1;
            for (;;) {
                j = ws(s, n, j);
                if (j >= i || s[j] == ']') break;
                size_t e0 = j;
                j = skip_value(s, n, j);
                int nt = jl_parse(s + e0, j - e0, tok, TOK);
                if (nt > 0 && tok[0].type == JL_OBJ && got < max) {
                    jl_doc_t d = { s + e0, tok, nt };
                    mh_device(&d, &out[got]);
                    if (out[got].serial[0]) got++;
                }
                j = ws(s, n, j);
                if (j < n && s[j] == ',') j++;
            }
        }
#undef KEY
        i = ws(s, n, i);
        if (i < n && s[i] == ',') {
            i++;
            continue;
        }
        break;
    }
    free(tok);
    meta->n = got;
    if (!devices && !meta->error[0]) snprintf(meta->error, sizeof meta->error, "no devices in the file");
    if (!devices) return -1;
    return got;
}

/* ------------------------------------------------------------------ controls */

int cat_controls_parse(const char *json, size_t len, cat_ctl_t *out, int max)
{
    int n = 0;
    jl_tok_t *t = tokenize(json, len, &n);
    if (!t) return -1;
    jl_doc_t d = { json, t, n };
    if (t[0].type != JL_ARR) {
        free(t);
        return -1;
    }
    int got = 0;
    for (int i = 0; i < t[0].size && got < max; i++) {
        int o = jl_at(&d, 0, i);
        if (d.t[o].type != JL_OBJ) continue;
        cat_ctl_t *c = &out[got];
        int ct = jl_get(&d, o, "control"), at = jl_get(&d, o, "action");
        if (ct < 0 || at < 0 || d.t[ct].type != JL_STR || d.t[at].type != JL_STR) continue;
        jl_str(&d, ct, c->control, sizeof c->control);
        jl_str(&d, at, c->action, sizeof c->action);
        if (!c->control[0] || !c->action[0]) continue;
        int cr = jl_get(&d, o, "controller");
        c->controller[0] = 0;
        if (cr >= 0 && d.t[cr].type == JL_STR) jl_str(&d, cr, c->controller, sizeof c->controller);
        /* a robot that names no controller has one, and it is the driver's (Console drivers.js) */
        if (!c->controller[0]) snprintf(c->controller, sizeof c->controller, "Driver");
        int cb = jl_get(&d, o, "combo");
        c->combo = cb >= 0 && d.t[cb].type == JL_TRUE;
        got++;
    }
    free(t);
    return got;
}

/* Lowercase, letters and digits only: "D-pad Up" → "dpadup". */
static void squash(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 1 < n; in++)
        if (isalnum((unsigned char)*in)) out[o++] = (char)tolower((unsigned char)*in);
    out[o] = 0;
}

bool cat_controls_place(const char *control, float *x, float *y, cat_input_kind_t *kind)
{
    /* A combination is drawn at the input that completes it: "Left bumper + Y" lights Y. */
    const char *plus = strrchr(control, '+');
    char key[40];
    squash(plus ? plus + 1 : control, key, sizeof key);
    /* An Xbox-shaped pad: left stick high, d-pad low left, face buttons high right, right stick low.
     * The PlayStation names land on the same places. Coordinates are fractions of the pad's box. */
    static const struct { const char *names[6]; float x, y; cat_input_kind_t k; } P[] = {
        { { "leftstick", "ls", "l3", "leftstickpress", "leftstickbutton", "leftjoystick" }, 0.27f, 0.42f, CAT_IN_STICK },
        { { "rightstick", "rs", "r3", "rightstickpress", "rightstickbutton", "rightjoystick" }, 0.62f, 0.66f, CAT_IN_STICK },
        { { "a", "cross", NULL }, 0.76f, 0.52f, CAT_IN_BUTTON },
        { { "b", "circle", NULL }, 0.84f, 0.42f, CAT_IN_BUTTON },
        { { "x", "square", NULL }, 0.68f, 0.42f, CAT_IN_BUTTON },
        { { "y", "triangle", NULL }, 0.76f, 0.32f, CAT_IN_BUTTON },
        { { "dpadup", "povup", "up", NULL }, 0.38f, 0.58f, CAT_IN_DPAD },
        { { "dpaddown", "povdown", "down", NULL }, 0.38f, 0.74f, CAT_IN_DPAD },
        { { "dpadleft", "povleft", "left", NULL }, 0.33f, 0.66f, CAT_IN_DPAD },
        { { "dpadright", "povright", "right", NULL }, 0.43f, 0.66f, CAT_IN_DPAD },
        { { "l1", "lb", "leftbumper", "leftshoulder", NULL }, 0.25f, 0.12f, CAT_IN_BUMPER },
        { { "r1", "rb", "rightbumper", "rightshoulder", NULL }, 0.75f, 0.12f, CAT_IN_BUMPER },
        { { "l2", "lt", "lefttrigger", NULL }, 0.22f, 0.02f, CAT_IN_TRIGGER },
        { { "r2", "rt", "righttrigger", NULL }, 0.78f, 0.02f, CAT_IN_TRIGGER },
        { { "options", "start", "menu", NULL }, 0.59f, 0.34f, CAT_IN_BUTTON },
        { { "create", "share", "back", "view", "select", NULL }, 0.41f, 0.34f, CAT_IN_BUTTON },
        { { "ps", "guide", "home", "xbox", "psbutton", NULL }, 0.50f, 0.46f, CAT_IN_BUTTON },
        { { "touchpad", "touchpadpress", NULL }, 0.50f, 0.28f, CAT_IN_PAD },
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        for (int k = 0; k < 6 && P[i].names[k]; k++)
            if (!strcmp(key, P[i].names[k])) {
                *x = P[i].x;
                *y = P[i].y;
                *kind = P[i].k;
                return true;
            }
    return false;
}

/* ------------------------------------------------------------------ state lanes */

void cat_st_lane_init(cat_st_lane_t *l, const char *name, const char *topic, cat_lane_kind_t kind)
{
    memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name);
    snprintf(l->topic, sizeof l->topic, "%s", topic);
    l->kind = kind;
}

static int lane_index(cat_st_lane_t *l, const char *s)
{
    for (int i = 0; i < l->nnames; i++)
        if (!strcmp(l->names[i], s)) return i;
    if (l->nnames < CAT_ST_NAMES - 1) {
        snprintf(l->names[l->nnames], sizeof l->names[0], "%s", s);
        return l->nnames++;
    }
    /* past the table: everything else is one "other" state rather than a lost transition */
    snprintf(l->names[CAT_ST_NAMES - 1], sizeof l->names[0], "\xe2\x80\xa6");
    l->nnames = CAT_ST_NAMES;
    return CAT_ST_NAMES - 1;
}

/* The k-th newest edge; k must be < count. */
static cat_st_edge_t edge_at(const cat_st_lane_t *l, int k) { return l->e[(l->head - 1 - k + 2 * CAT_ST_RING) % CAT_ST_RING]; }

bool cat_st_edge(const cat_st_lane_t *l, int k, cat_st_edge_t *out)
{
    if (k < 0 || k >= l->count) return false;
    *out = edge_at(l, k);
    return true;
}

bool cat_st_feed(cat_st_lane_t *l, uint32_t ms, const char *state)
{
    int s = state && state[0] ? lane_index(l, state) : -1;
    cat_st_edge_t last;
    if (cat_st_edge(l, 0, &last) && last.s == s) return false;
    if (!l->count && s < 0) return false; /* nothing yet, and still nothing */
    l->e[l->head] = (cat_st_edge_t){ .ms = ms, .s = (int16_t)s };
    l->head = (l->head + 1) % CAT_ST_RING;
    if (l->count < CAT_ST_RING) l->count++;
    if (s >= 0) l->total++;
    return true;
}

const char *cat_st_current(const cat_st_lane_t *l)
{
    cat_st_edge_t e;
    if (!cat_st_edge(l, 0, &e) || e.s < 0) return NULL;
    return l->names[e.s];
}

int cat_st_count(const cat_st_lane_t *l, uint32_t t0, uint32_t t1)
{
    /* A change is an edge into a real state whose last real state was a different one: the first value
     * seen is not a transition, and a state that comes back after a dropped link is not one either. */
    int n = 0;
    int prev_real = -1;
    for (int k = l->count - 1; k >= 0; k--) {
        cat_st_edge_t e = edge_at(l, k);
        if (e.s < 0) continue;
        if (prev_real >= 0 && prev_real != e.s && e.ms >= t0 && e.ms < t1) n++;
        prev_real = e.s;
    }
    return n;
}

uint32_t cat_st_time_in(const cat_st_lane_t *l, int s, uint32_t t0, uint32_t t1, uint32_t now)
{
    uint32_t total = 0;
    if (t1 > now) t1 = now;
    for (int k = l->count - 1; k >= 0; k--) {
        cat_st_edge_t e = edge_at(l, k);
        if (e.s != s) continue;
        uint32_t a = e.ms, b = k ? edge_at(l, k - 1).ms : now;
        if (a < t0) a = t0;
        if (b > t1) b = t1;
        if (b > a) total += b - a;
    }
    return total;
}

/* ------------------------------------------------------------------ CSV */

size_t cat_csv_field(char *out, size_t n, const char *s)
{
    bool quote = strpbrk(s, ",\"\r\n") != NULL;
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) out[o] = (c); o++; } while (0)
    if (quote) PUT('"');
    for (; *s; s++) {
        if (*s == '"') PUT('"');
        PUT(*s);
    }
    if (quote) PUT('"');
#undef PUT
    if (n) out[o < n ? o : n - 1] = 0;
    return o;
}

size_t cat_csv_header(char *out, size_t n, const cat_rec_col_t *cols, int ncols)
{
    size_t o = (size_t)snprintf(out, n, "t,mark");
    for (int i = 0; i < ncols && o < n; i++) {
        if (o + 1 < n) out[o++] = ',';
        o += cat_csv_field(out + o, n - o, cols[i].label);
    }
    if (o + 2 >= n) return 0;
    out[o++] = '\n';
    out[o] = 0;
    return o;
}

size_t cat_csv_row(char *out, size_t n, double t, int mark, const double *v, const bool *have, int ncols)
{
    int w = mark > 0 ? snprintf(out, n, "%.3f,%d", t, mark) : snprintf(out, n, "%.3f,", t);
    if (w < 0 || (size_t)w >= n) return 0;
    size_t o = (size_t)w;
    for (int i = 0; i < ncols; i++) {
        /* an absent value is an empty field, never 0 */
        w = have[i] && v[i] == v[i] ? snprintf(out + o, n - o, ",%.6g", v[i]) : snprintf(out + o, n - o, ",");
        if (w < 0 || o + (size_t)w >= n) return 0;
        o += (size_t)w;
    }
    if (o + 2 >= n) return 0;
    out[o++] = '\n';
    out[o] = 0;
    return o;
}

bool cat_csv_last_t(const char *s, size_t n, double *t)
{
    /* the last line that ends in a newline: a row being written may be half there */
    size_t end = n;
    while (end > 0 && s[end - 1] != '\n') end--;
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r')) end--;
    if (!end) return false;
    size_t start = end;
    while (start > 0 && s[start - 1] != '\n') start--;
    char buf[32];
    size_t l = end - start < sizeof buf - 1 ? end - start : sizeof buf - 1;
    memcpy(buf, s + start, l);
    buf[l] = 0;
    char *e;
    double v = strtod(buf, &e);
    if (e == buf || (*e && *e != ',')) return false;
    *t = v;
    return true;
}

int cat_csv_count_cols(const char *line)
{
    int n = 1;
    bool q = false;
    for (; *line && *line != '\n' && *line != '\r'; line++) {
        if (*line == '"') q = !q;
        else if (*line == ',' && !q) n++;
    }
    return n;
}
