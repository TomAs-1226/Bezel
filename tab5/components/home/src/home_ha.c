/* Home Assistant over its REST API, with a long-lived access token (Bearer).
 *
 * The picked entities are read in one request when the token may render templates (POST /api/template, one
 * line per entity: id|state|name|unit), else one GET /api/states/<id> each (a non-admin user's token).
 * A tap calls the entity's natural service (a light toggles, a scene turns on, a media player plays or
 * pauses); locks and alarm panels are shown but never worked from here. The picker reads GET /api/states
 * once, a top-level object at a time, so a house with a thousand entities costs its text and no more. */
#include "home_priv.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define POLL_S 5.0
#define RETRY_S 15.0
#define RESP_MAX 16384
#define LIST_CAP (3u << 20)     /* the whole /api/states, at most */
#define TAPS 8

/* a queued service call: the natural action ("" svc) or a light's level / a thermostat's set point */
typedef struct {
    char id[64];
    char svc[20];              /* "" the entity's natural action, "brightness", "temperature" */
    float value;
} ha_cmd_t;

typedef struct {
    char url[128], token[320];
    char picks[HOME_HA_PICKS][64];
    int npicks;
    unsigned cfg_gen, done_gen;
    home_ha_status_t st;
    home_ha_entity_t tiles[HOME_HA_PICKS];
    ha_cmd_t taps[TAPS];
    int ntaps;
    int template_ok;            /* 1 yes, 0 no (403), -1 not tried */
    double next_poll;
    /* the picker */
    bool list_req, listing;
    home_ha_entity_t *list;     /* HOME_HA_LIST */
    int nlist;
    char list_err[80];
    /* discovery */
    bool disc_req;
    int disc;                   /* 0 looking / idle, 1 found, -1 none */
    char disc_url[128];
} ha_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ha_t *A;

static ha_t *ha(void)
{
    if (!A) {
        A = calloc(1, sizeof *A);
        if (A) A->template_ok = -1;
    }
    return A;
}

/* ---- kinds and actions ---- */

static bool domain_is(const char *id, const char *d)
{
    size_t n = strlen(d);
    return !strncmp(id, d, n) && id[n] == '.';
}

home_ha_kind_t home_ha_kind(const char *id)
{
    if (domain_is(id, "light")) return HA_LIGHT;
    if (domain_is(id, "switch") || domain_is(id, "input_boolean") || domain_is(id, "siren") ||
        domain_is(id, "humidifier") || domain_is(id, "automation") || domain_is(id, "group"))
        return HA_SWITCH;
    if (domain_is(id, "fan")) return HA_FAN;
    if (domain_is(id, "scene")) return HA_SCENE;
    if (domain_is(id, "script")) return HA_SCRIPT;
    if (domain_is(id, "button") || domain_is(id, "input_button")) return HA_BUTTON;
    if (domain_is(id, "sensor")) return HA_SENSOR;
    if (domain_is(id, "binary_sensor")) return HA_BINARY;
    if (domain_is(id, "media_player")) return HA_MEDIA;
    if (domain_is(id, "cover")) return HA_COVER;
    if (domain_is(id, "climate") || domain_is(id, "water_heater")) return HA_CLIMATE;
    if (domain_is(id, "lock") || domain_is(id, "alarm_control_panel")) return HA_LOCK;
    return HA_OTHER;
}

/* the service a tap calls, or NULL: shown only */
static const char *tap_service(const char *id, char *domain, size_t dn)
{
    const char *dot = strchr(id, '.');
    if (!dot) return NULL;
    snprintf(domain, dn, "%.*s", (int)(dot - id), id);
    switch (home_ha_kind(id)) {
    case HA_LIGHT: case HA_SWITCH: case HA_FAN: case HA_COVER: return "toggle";
    case HA_SCENE: case HA_SCRIPT: return "turn_on";
    case HA_BUTTON: return "press";
    case HA_MEDIA: return "media_play_pause";
    default: return NULL;
    }
}

static void unknown_attrs(home_ha_entity_t *e)
{
    e->area[0] = 0;
    e->dimmable = false;
    e->brightness = -1;
    e->target = e->current = NAN;
    e->step = 0.5f;
}

static void fill(home_ha_entity_t *e)
{
    e->kind = home_ha_kind(e->id);
    char d[32];
    e->actionable = tap_service(e->id, d, sizeof d) != NULL && strcmp(e->state, "unavailable") != 0;
    e->on = !strcmp(e->state, "on") || !strcmp(e->state, "open") || !strcmp(e->state, "opening") ||
            !strcmp(e->state, "playing") || !strcmp(e->state, "home") || !strcmp(e->state, "unlocked") ||
            (e->kind == HA_CLIMATE && strcmp(e->state, "off") != 0 && strcmp(e->state, "unavailable") != 0);
    if (!e->name[0]) snprintf(e->name, sizeof e->name, "%s", strchr(e->id, '.') ? strchr(e->id, '.') + 1 : e->id);
    if (e->kind == HA_LIGHT && strcmp(e->state, "on") != 0) e->brightness = e->dimmable ? 0 : -1;
}

/* ---- the UI's side ---- */

void home_ha_config(const char *url, const char *token, const char *picks)
{
    pthread_mutex_lock(&g_lock);
    if (!ha()) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    char u[128];
    snprintf(u, sizeof u, "%s", url ? url : "");
    size_t l = strlen(u);
    while (l && (u[l - 1] == '/' || u[l - 1] == ' ')) u[--l] = 0;
    bool same_server = !strcmp(u, A->url) && !strcmp(token ? token : "", A->token);
    snprintf(A->url, sizeof A->url, "%s", u);
    snprintf(A->token, sizeof A->token, "%s", token ? token : "");
    if (!same_server) A->template_ok = -1;
    /* the picks, keeping what's known of the ones that stay */
    home_ha_entity_t *old = malloc(sizeof A->tiles);
    int nold = old ? A->npicks : 0;
    if (old) memcpy(old, A->tiles, sizeof A->tiles);
    A->npicks = 0;
    const char *p = picks ? picks : "";
    while (*p && A->npicks < HOME_HA_PICKS) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        if (e > p && (size_t)(e - p) < sizeof A->picks[0]) {
            char id[64];
            snprintf(id, sizeof id, "%.*s", (int)(e - p), p);
            home_ha_entity_t *t = &A->tiles[A->npicks];
            memset(t, 0, sizeof *t);
            unknown_attrs(t);
            snprintf(t->id, sizeof t->id, "%s", id);
            for (int i = 0; i < nold; i++)
                if (same_server && !strcmp(old[i].id, id)) *t = old[i];
            fill(t);
            snprintf(A->picks[A->npicks++], sizeof A->picks[0], "%s", id);
        }
        p = e;
    }
    free(old);
    A->st.configured = A->url[0] && A->token[0];
    A->cfg_gen++;
    A->next_poll = 0;
    A->st.gen++;
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

void home_ha_status(home_ha_status_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (ha()) *out = A->st;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_lock);
}

int home_ha_tiles(home_ha_entity_t *out, int max)
{
    pthread_mutex_lock(&g_lock);
    int n = 0;
    if (ha())
        for (; n < A->npicks && n < max; n++) out[n] = A->tiles[n];
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* Queues a call; a level or set point replaces one of its kind still waiting for the same entity. UI thread,
 * under g_lock. */
static bool enqueue(const char *id, const char *svc, float value)
{
    if (!ha()) return false;
    for (int i = 0; svc[0] && i < A->ntaps; i++)
        if (!strcmp(A->taps[i].id, id) && !strcmp(A->taps[i].svc, svc)) {
            A->taps[i].value = value;
            return true;
        }
    if (A->ntaps >= TAPS) return false;
    ha_cmd_t *t = &A->taps[A->ntaps++];
    snprintf(t->id, sizeof t->id, "%s", id);
    snprintf(t->svc, sizeof t->svc, "%s", svc);
    t->value = value;
    return true;
}

static home_ha_entity_t *tile_of(const char *id)
{
    for (int i = 0; i < A->npicks; i++)
        if (!strcmp(A->tiles[i].id, id)) return &A->tiles[i];
    return NULL;
}

void home_ha_brightness(const char *id, int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    pthread_mutex_lock(&g_lock);
    if (enqueue(id, "brightness", (float)pct)) {
        home_ha_entity_t *t = tile_of(id);
        if (t) { /* shows at once; the next read confirms it */
            t->brightness = pct;
            t->on = pct > 0;
            snprintf(t->state, sizeof t->state, "%s", pct > 0 ? "on" : "off");
            t->pending = true;
        }
        A->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

void home_ha_set_temp(const char *id, float target)
{
    pthread_mutex_lock(&g_lock);
    if (enqueue(id, "temperature", target)) {
        home_ha_entity_t *t = tile_of(id);
        if (t) {
            t->target = target;
            t->pending = true;
        }
        A->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

void home_ha_tap(const char *id)
{
    pthread_mutex_lock(&g_lock);
    if (ha() && enqueue(id, "", 0)) {
        for (int i = 0; i < A->npicks; i++) {
            home_ha_entity_t *t = &A->tiles[i];
            if (strcmp(t->id, id) != 0 || !t->actionable) continue;
            t->pending = true;
            /* a toggle shows at once; the next read confirms it */
            if (t->kind == HA_LIGHT || t->kind == HA_SWITCH || t->kind == HA_FAN || t->kind == HA_MEDIA) t->on = !t->on;
        }
        A->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

void home_ha_list_request(void)
{
    pthread_mutex_lock(&g_lock);
    if (ha() && !A->listing) {
        A->list_req = true;
        A->listing = true;
        A->list_err[0] = 0;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

int home_ha_list(home_ha_entity_t *out, int max, char *err, size_t errn)
{
    pthread_mutex_lock(&g_lock);
    int n = -1;
    if (ha() && !A->listing) {
        n = 0;
        for (; A->list && n < A->nlist && n < max; n++) out[n] = A->list[n];
    }
    if (err) snprintf(err, errn, "%s", A ? A->list_err : "");
    pthread_mutex_unlock(&g_lock);
    return n;
}

void home_ha_discover(void)
{
    pthread_mutex_lock(&g_lock);
    if (ha()) {
        A->disc_req = true;
        A->disc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

int home_ha_found(char *url, size_t n)
{
    pthread_mutex_lock(&g_lock);
    int r = A ? A->disc : -1;
    if (r == 1) snprintf(url, n, "%s", A->disc_url);
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* ---- the worker's side ---- */

typedef struct {
    char url[128], token[320];
} creds_t;

static int request(const creds_t *c, const char *method, const char *path, const char *body, char *out, int max)
{
    char *url = malloc(640), *hdr = malloc(420);
    int status = -1;
    if (url && hdr) {
        snprintf(url, 640, "%s%s", c->url, path);
        snprintf(hdr, 420, "Authorization: Bearer %s\r\nContent-Type: application/json\r\nAccept: application/json\r\n",
                 c->token);
        hal_http_req_t rq = { .method = method, .url = url, .headers = hdr, .body = body,
                              .body_len = body ? strlen(body) : 0, .timeout_ms = 6000 };
        int len;
        status = hal_http_fetch(&rq, out, max, &len);
    }
    free(url);
    free(hdr);
    return status;
}

static void say_status(int status, char *err, size_t n)
{
    if (status == 401) snprintf(err, n, "Home Assistant refused the token");
    else if (status == 403) snprintf(err, n, "this token may not read that");
    else if (status == 404) snprintf(err, n, "not found: is the URL Home Assistant's?");
    else if (status < 0) snprintf(err, n, "Home Assistant isn't answering");
    else snprintf(err, n, "Home Assistant answered %d", status);
}

/* a template field: "None" (or empty) is no value */
static bool none(const char *f) { return !f[0] || !strcmp(f, "None"); }

static float num_or_nan(const char *f)
{
    if (none(f)) return NAN;
    char *e;
    float v = strtof(f, &e);
    return e == f ? NAN : v;
}

/* one line of the template's output: id|state|unit|area|bri|dim|target|current|step|name (the name last: it
 * may itself hold a |) */
#define TFIELDS 10
static void apply_line(char *line, home_ha_entity_t *tiles, int n)
{
    char *f[TFIELDS];
    f[0] = line;
    for (int k = 1; k < TFIELDS; k++) {
        char *bar = strchr(f[k - 1], '|');
        if (!bar) return;
        *bar = 0;
        f[k] = bar + 1;
    }
    for (int i = 0; i < n; i++) {
        home_ha_entity_t *t = &tiles[i];
        if (strcmp(t->id, f[0]) != 0) continue;
        snprintf(t->state, sizeof t->state, "%s", f[1]);
        snprintf(t->unit, sizeof t->unit, "%s", none(f[2]) ? "" : f[2]);
        snprintf(t->area, sizeof t->area, "%s", none(f[3]) ? "" : f[3]);
        float bri = num_or_nan(f[4]);
        t->dimmable = !strcmp(f[5], "True");
        t->brightness = isnan(bri) ? -1 : (int)lroundf(bri * 100.0f / 255.0f);
        t->target = num_or_nan(f[6]);
        t->current = num_or_nan(f[7]);
        float step = num_or_nan(f[8]);
        t->step = isnan(step) || step <= 0 ? 0.5f : step;
        if (!none(f[9])) snprintf(t->name, sizeof t->name, "%s", f[9]);
        home_fold_text(t->name);
        home_fold_text(t->unit);
        home_fold_text(t->area);
        t->pending = false;
        fill(t);
    }
}

static int poll_template(const creds_t *c, home_ha_entity_t *tiles, int n, char *buf)
{
    /* the entity ids are [a-z0-9_.]: safe inside single quotes, and nothing in it needs JSON escaping */
    size_t cap = 900 + (size_t)n * 68;
    char *body = malloc(cap);
    if (!body) return -1;
    int o = snprintf(body, cap, "{\"template\":\"{%% for e in [");
    for (int i = 0; i < n; i++) o += snprintf(body + o, cap - (size_t)o, "%s'%s'", i ? "," : "", tiles[i].id);
    snprintf(body + o, cap - (size_t)o,
             "] %%}{{ e }}|{{ states(e) }}|{{ state_attr(e, 'unit_of_measurement') }}|{{ area_name(e) }}|"
             "{{ state_attr(e, 'brightness') }}|"
             "{{ (state_attr(e, 'supported_color_modes') or []) | reject('eq', 'onoff') | list | count > 0 }}|"
             "{{ state_attr(e, 'temperature') }}|{{ state_attr(e, 'current_temperature') }}|"
             "{{ state_attr(e, 'target_temp_step') }}|{{ state_attr(e, 'friendly_name') }}\\n{%% endfor %%}\"}");
    int status = request(c, "POST", "/api/template", body, buf, RESP_MAX);
    free(body);
    if (status != 200) return status;
    for (char *line = buf, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        apply_line(line, tiles, n);
    }
    return 200;
}

static void entity_from_json(const home_json_t *j, int obj, home_ha_entity_t *e)
{
    home_json_str(j, obj, "entity_id", e->id, sizeof e->id);
    home_json_str(j, obj, "state", e->state, sizeof e->state);
    int at = jl_get(&j->d, obj, "attributes");
    home_json_str(j, at, "friendly_name", e->name, sizeof e->name);
    home_json_str(j, at, "unit_of_measurement", e->unit, sizeof e->unit);
    /* the room needs the template API (an admin's token): one by one, it stays unknown */
    char area[32];
    snprintf(area, sizeof area, "%s", e->area);
    unknown_attrs(e);
    snprintf(e->area, sizeof e->area, "%s", area);
    double bri = home_json_num(j, at, "brightness", -1);
    if (bri >= 0) e->brightness = (int)lround(bri * 100.0 / 255.0);
    int modes = at < 0 ? -1 : jl_get(&j->d, at, "supported_color_modes");
    for (int k = 0; modes >= 0 && j->d.t[modes].type == JL_ARR; k++) {
        int m = jl_at(&j->d, modes, k);
        if (m < 0) break;
        char mode[24];
        jl_str(&j->d, m, mode, sizeof mode);
        if (strcmp(mode, "onoff") != 0) e->dimmable = true;
    }
    double tg = home_json_num(j, at, "temperature", NAN), cu = home_json_num(j, at, "current_temperature", NAN);
    e->target = (float)tg;
    e->current = (float)cu;
    double st = home_json_num(j, at, "target_temp_step", 0.5);
    e->step = st > 0 ? (float)st : 0.5f;
    e->pending = false;
    fill(e);
}

static int poll_each(const creds_t *c, home_ha_entity_t *tiles, int n, char *buf)
{
    int worst = 200;
    for (int i = 0; i < n; i++) {
        char path[96];
        snprintf(path, sizeof path, "/api/states/%s", tiles[i].id);
        int status = request(c, "GET", path, NULL, buf, RESP_MAX);
        if (status == 200) {
            home_json_t j;
            if (home_json_parse(&j, buf, strlen(buf))) {
                char id[64];
                snprintf(id, sizeof id, "%s", tiles[i].id);
                entity_from_json(&j, 0, &tiles[i]);
                snprintf(tiles[i].id, sizeof tiles[i].id, "%s", id);
                home_json_free(&j);
            }
        } else if (status == 404) {
            snprintf(tiles[i].state, sizeof tiles[i].state, "not found");
            tiles[i].actionable = tiles[i].on = tiles[i].pending = false;
        } else {
            worst = status;
            if (status == 401 || status < 0) break;
        }
    }
    return worst;
}

static void poll(const creds_t *c, char *buf)
{
    home_ha_entity_t *tiles = malloc(sizeof(home_ha_entity_t) * HOME_HA_PICKS); /* off the worker's stack */
    if (!tiles) return;
    pthread_mutex_lock(&g_lock);
    int n = A->npicks;
    unsigned gen = A->cfg_gen;
    int tmpl = A->template_ok;
    memcpy(tiles, A->tiles, sizeof A->tiles);
    pthread_mutex_unlock(&g_lock);

    int status = 200;
    if (n > 0) {
        if (tmpl != 0) {
            status = poll_template(c, tiles, n, buf);
            if (status == 200) tmpl = 1;
            else if (status == 403 || status == 400 || status == 405) { /* not an admin's token: one by one */
                tmpl = 0;
                status = poll_each(c, tiles, n, buf);
            }
        } else {
            status = poll_each(c, tiles, n, buf);
        }
    } else {
        status = request(c, "GET", "/api/", NULL, buf, RESP_MAX); /* nothing picked: just check the way in */
        if (status == 201) status = 200;
    }

    pthread_mutex_lock(&g_lock);
    if (gen == A->cfg_gen) { /* the settings didn't change under the request */
        A->template_ok = tmpl;
        memcpy(A->tiles, tiles, sizeof A->tiles);
        A->st.status = status;
        A->st.ok = status == 200;
        if (A->st.ok) {
            A->st.err[0] = 0;
            A->st.last_ok = hal_seconds();
        } else {
            say_status(status, A->st.err, sizeof A->st.err);
        }
        A->st.gen++;
        A->next_poll = hal_seconds() + (A->st.ok ? POLL_S : RETRY_S);
    }
    pthread_mutex_unlock(&g_lock);
    free(tiles);
}

static void call(const creds_t *c, const ha_cmd_t *cmd, char *buf)
{
    char domain[32], path[96], body[144];
    const char *id = cmd->id;
    if (!strcmp(cmd->svc, "brightness")) {
        int pct = (int)lroundf(cmd->value);
        snprintf(path, sizeof path, "/api/services/light/%s", pct > 0 ? "turn_on" : "turn_off");
        if (pct > 0) snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", id, pct);
        else snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", id);
    } else if (!strcmp(cmd->svc, "temperature")) {
        snprintf(path, sizeof path, "/api/services/climate/set_temperature");
        snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"temperature\":%.1f}", id, cmd->value);
    } else {
        const char *svc = tap_service(id, domain, sizeof domain);
        if (!svc) return;
        snprintf(path, sizeof path, "/api/services/%s/%s", domain, svc);
        snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", id);
    }
    int status = request(c, "POST", path, body, buf, RESP_MAX);
    if (status != 200) {
        pthread_mutex_lock(&g_lock);
        say_status(status, A->st.err, sizeof A->st.err);
        A->st.gen++;
        pthread_mutex_unlock(&g_lock);
    }
}

/* Top-level objects of a JSON array, one at a time: [start, end) of the next, or false at the end. */
static bool next_object(const char *s, size_t len, size_t *pos, size_t *start, size_t *end)
{
    size_t i = *pos;
    while (i < len && s[i] != '{') {
        if (s[i] == ']') return false;
        i++;
    }
    if (i >= len) return false;
    *start = i;
    int depth = 0;
    bool str = false;
    for (; i < len; i++) {
        char c = s[i];
        if (str) {
            if (c == '\\') i++;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && --depth == 0) {
            *end = i + 1;
            *pos = i + 1;
            return true;
        }
    }
    return false;
}

static int by_kind_name(const void *a, const void *b)
{
    const home_ha_entity_t *x = a, *y = b;
    if (x->kind != y->kind) return (int)x->kind - (int)y->kind;
    return strcasecmp(x->name, y->name);
}

static void fetch_list(const creds_t *c)
{
    char err[80] = "";
    home_ha_entity_t *all = NULL;
    int n = 0, cap = 0;
    char *body = NULL;
    size_t len = 0;
    char *url = malloc(256), *hdr = malloc(420);
    if (!url || !hdr) {
        snprintf(err, sizeof err, "out of memory");
        goto done;
    }
    snprintf(url, 256, "%s/api/states", c->url);
    snprintf(hdr, 420, "Authorization: Bearer %s\r\nAccept: application/json\r\n", c->token);
    hal_http_req_t rq = { .method = "GET", .url = url, .headers = hdr, .timeout_ms = 10000 };
    int status = -1;
    char e2[64];
    hal_http_t *h = hal_http_open(&rq, &status, e2, sizeof e2);
    if (!h || status != 200) {
        say_status(h ? status : -1, err, sizeof err);
        if (h) hal_http_close(h);
        goto done;
    }
    size_t bcap = 256 * 1024;
    body = malloc(bcap);
    while (body) {
        if (len + 8192 + 1 > bcap) {
            if (bcap >= LIST_CAP) break;
            char *nb = realloc(body, bcap * 2);
            if (!nb) break;
            body = nb;
            bcap *= 2;
        }
        int r = hal_http_read(h, body + len, (int)(bcap - len - 1));
        if (r <= 0) break;
        len += (size_t)r;
    }
    hal_http_close(h);
    if (!body) {
        snprintf(err, sizeof err, "out of memory");
        goto done;
    }
    body[len] = 0;
    size_t pos = 0, s0, s1;
    while (next_object(body, len, &pos, &s0, &s1)) {
        home_json_t j;
        if (!home_json_parse(&j, body + s0, s1 - s0)) continue;
        home_ha_entity_t e = { 0 };
        entity_from_json(&j, 0, &e);
        e.area[0] = 0;
        home_json_free(&j);
        if (!e.id[0] || e.kind == HA_OTHER) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 128;
            home_ha_entity_t *na = realloc(all, (size_t)nc * sizeof *na);
            if (!na) break;
            all = na;
            cap = nc;
        }
        all[n++] = e;
    }
    if (n == 0) snprintf(err, sizeof err, "Home Assistant listed nothing that can be shown here");
    else qsort(all, (size_t)n, sizeof *all, by_kind_name);
done:
    free(url);
    free(hdr);
    free(body);
    pthread_mutex_lock(&g_lock);
    if (!A->list) A->list = malloc(sizeof *A->list * HOME_HA_LIST);
    A->nlist = 0;
    for (int i = 0; A->list && i < n && i < HOME_HA_LIST; i++) A->list[A->nlist++] = all[i];
    if (n > HOME_HA_LIST) snprintf(err, sizeof err, "showing %d of %d entities", HOME_HA_LIST, n);
    snprintf(A->list_err, sizeof A->list_err, "%s", err);
    A->listing = false;
    pthread_mutex_unlock(&g_lock);
    free(all);
}

static void discover(void)
{
    hal_service_t sv[2];
    int n = hal_mdns_browse("_home-assistant", "_tcp", sv, 2, 3000);
    pthread_mutex_lock(&g_lock);
    if (n > 0) {
        snprintf(A->disc_url, sizeof A->disc_url, "http://%s:%d", sv[0].ip[0] ? sv[0].ip : sv[0].host,
                 sv[0].port ? sv[0].port : 8123);
        A->disc = 1;
    } else {
        A->disc = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

void home_ha_work(double now, bool want)
{
    static char *buf;
    static creds_t *c; /* the worker's own copy, made once */
    if (!c) c = malloc(sizeof *c);
    pthread_mutex_lock(&g_lock);
    if (!A || !c) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(c->url, sizeof c->url, "%s", A->url);
    snprintf(c->token, sizeof c->token, "%s", A->token);
    bool configured = A->st.configured;
    ha_cmd_t tap = { 0 };
    if (A->ntaps) {
        tap = A->taps[0];
        memmove(&A->taps[0], &A->taps[1], sizeof A->taps[0] * (size_t)(A->ntaps - 1));
        A->ntaps--;
    }
    bool list = A->list_req, disc = A->disc_req;
    A->list_req = A->disc_req = false;
    bool due = want && configured && now >= A->next_poll;
    pthread_mutex_unlock(&g_lock);

    if (!buf) buf = malloc(RESP_MAX);
    if (disc) discover();
    if (buf && configured) {
        if (tap.id[0]) {
            call(c, &tap, buf);
            pthread_mutex_lock(&g_lock);
            A->next_poll = 0; /* read it back straight away */
            pthread_mutex_unlock(&g_lock);
            due = true;
        }
        if (list) fetch_list(c);
        if (due) poll(c, buf);
    } else if (list) {
        pthread_mutex_lock(&g_lock);
        A->listing = false;
        snprintf(A->list_err, sizeof A->list_err, "set Home Assistant's address and token first");
        pthread_mutex_unlock(&g_lock);
    }
}
