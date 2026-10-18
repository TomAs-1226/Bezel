#include "nt4.h"
#include "json_lite.h"
#include "mpack_lite.h"
#include "ws_lite.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAME_MAX_LEN 128
#define TYPE_MAX_LEN 40
#define MAX_ADDRS 6
#define SETQ_LEN 32
#define MAX_NUMS 1024 /* a Pose2d[] path of 340 poses; anything longer is truncated */

typedef struct {
    char *name;
    char type[TYPE_MAX_LEN];
    int64_t id;          /* server topic id while announced, -1 otherwise */
    int64_t pubuid;      /* our publisher id this session, 0 if we have not published it */
    nt4_type_t wire;
    uint32_t seq;
    int64_t stamp_us;    /* local receive time */
    double num;
    bool has_num;
    double *nums;
    int nnums, cap_nums;
    char *str;
    size_t str_cap;
    char *strs;          /* NUL-separated string array */
    size_t strs_len, strs_cap;
    int nstrs;
} topic_t;

typedef struct {
    bool used;
    char name[NAME_MAX_LEN];
    nt4_type_t type;
    double num;
    int64_t i;
    char str[160];
} set_req_t;

struct nt4_client {
    pthread_mutex_t lock;
    char client_name[48];
    double period;
    char **prefixes;
    int nprefixes;

    topic_t *topics;
    int ntopics, cap;
    int32_t *by_name;    /* open addressing, size cap*2, -1 empty */
    int32_t *by_id;      /* open addressing on server id, size cap*2, -1 empty */

    char addrs[MAX_ADDRS][64];
    int naddrs;
    bool addrs_changed;

    set_req_t setq[SETQ_LEN];
    int64_t next_pubuid;

    volatile bool stop, kick;
    nt4_status_t status;
    uint32_t generation;
    ws_t ws;
};

int64_t nt4_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int find_name(nt4_client_t *c, const char *name)
{
    int mask = c->cap * 2 - 1;
    for (int i = fnv(name) & mask, n = 0; n <= mask; i = (i + 1) & mask, n++) {
        int32_t k = c->by_name[i];
        if (k < 0) return -1;
        if (strcmp(c->topics[k].name, name) == 0) return k;
    }
    return -1;
}

static int intern(nt4_client_t *c, const char *name)
{
    int k = find_name(c, name);
    if (k >= 0) return k;
    if (c->ntopics >= c->cap) return -1;
    k = c->ntopics++;
    topic_t *t = &c->topics[k];
    memset(t, 0, sizeof *t);
    t->name = strdup(name);
    t->id = -1;
    int mask = c->cap * 2 - 1;
    int i = fnv(name) & mask;
    while (c->by_name[i] >= 0) i = (i + 1) & mask;
    c->by_name[i] = k;
    return k;
}

static uint32_t id_hash(int64_t id) { return (uint32_t)(id * 2654435761u); }

static void id_rebuild(nt4_client_t *c)
{
    int size = c->cap * 2, mask = size - 1;
    for (int i = 0; i < size; i++) c->by_id[i] = -1;
    for (int k = 0; k < c->ntopics; k++) {
        if (c->topics[k].id < 0) continue;
        int i = id_hash(c->topics[k].id) & mask;
        while (c->by_id[i] >= 0) i = (i + 1) & mask;
        c->by_id[i] = k;
    }
}

static int find_id(nt4_client_t *c, int64_t id)
{
    int mask = c->cap * 2 - 1;
    for (int i = id_hash(id) & mask, n = 0; n <= mask; i = (i + 1) & mask, n++) {
        int32_t k = c->by_id[i];
        if (k < 0) return -1;
        if (c->topics[k].id == id) return k;
    }
    return -1;
}

nt4_client_t *nt4_create(const nt4_config_t *cfg)
{
    nt4_client_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    pthread_mutex_init(&c->lock, NULL);
    snprintf(c->client_name, sizeof c->client_name, "%s", cfg && cfg->client_name ? cfg->client_name : "catalyst-tab");
    c->period = cfg && cfg->period_s > 0 ? cfg->period_s : 0.05;
    int cap = 2048;
    if (cfg && cfg->max_topics > 0) {
        cap = 64;
        while (cap < cfg->max_topics) cap *= 2;
    }
    c->cap = cap;
    c->topics = calloc((size_t)cap, sizeof(topic_t));
    c->by_name = malloc(sizeof(int32_t) * (size_t)cap * 2);
    c->by_id = malloc(sizeof(int32_t) * (size_t)cap * 2);
    if (!c->topics || !c->by_name || !c->by_id) {
        free(c->topics); free(c->by_name); free(c->by_id); free(c);
        return NULL;
    }
    for (int i = 0; i < cap * 2; i++) c->by_name[i] = c->by_id[i] = -1;
    const char *const *p = cfg ? cfg->prefixes : NULL;
    static const char *const all[] = { "/", NULL };
    if (!p) p = all;
    while (p[c->nprefixes]) c->nprefixes++;
    c->prefixes = calloc((size_t)c->nprefixes, sizeof(char *));
    for (int i = 0; i < c->nprefixes; i++) c->prefixes[i] = strdup(p[i]);
    c->next_pubuid = 100;
    c->ws.fd = -1;
    return c;
}

void nt4_set_addresses(nt4_client_t *c, const char *const *addrs, int n)
{
    pthread_mutex_lock(&c->lock);
    if (n > MAX_ADDRS) n = MAX_ADDRS;
    bool same = n == c->naddrs;
    for (int i = 0; i < n && same; i++) same = strcmp(c->addrs[i], addrs[i]) == 0;
    if (!same) {
        for (int i = 0; i < n; i++) snprintf(c->addrs[i], sizeof c->addrs[i], "%s", addrs[i]);
        c->naddrs = n;
        c->addrs_changed = true;
    }
    pthread_mutex_unlock(&c->lock);
}

void nt4_stop(nt4_client_t *c) { c->stop = true; }
void nt4_reconnect(nt4_client_t *c) { c->kick = true; }

void nt4_status(nt4_client_t *c, nt4_status_t *out)
{
    pthread_mutex_lock(&c->lock);
    *out = c->status;
    out->rx_bytes = c->ws.rx_bytes;
    out->tx_bytes = c->ws.tx_bytes;
    pthread_mutex_unlock(&c->lock);
}

int64_t nt4_server_time_us(nt4_client_t *c)
{
    return nt4_now_us() + c->status.offset_us;
}

uint32_t nt4_generation(nt4_client_t *c) { return c->generation; }

/* ---------------------------------------------------------------- value storage */

static void set_num(topic_t *t, double v)
{
    t->num = v;
    t->has_num = true;
}

static bool ensure_nums(topic_t *t, int n)
{
    if (n > MAX_NUMS) n = MAX_NUMS;
    if (n > t->cap_nums) {
        double *p = realloc(t->nums, sizeof(double) * (size_t)n);
        if (!p) return false;
        t->nums = p;
        t->cap_nums = n;
    }
    return true;
}

static void set_str(topic_t *t, const uint8_t *p, size_t n)
{
    if (n + 1 > t->str_cap) {
        char *s = realloc(t->str, n + 1);
        if (!s) return;
        t->str = s;
        t->str_cap = n + 1;
    }
    memcpy(t->str, p, n);
    t->str[n] = 0;
}

/* wpimath structs are runs of little-endian doubles; the width in bytes is what tells them apart,
 * exactly as Console's decode_struct does. */
static int struct_width(const char *name)
{
    static const struct { const char *n; int w; } W[] = {
        { "Pose2d", 24 }, { "Translation2d", 16 }, { "Rotation2d", 8 }, { "Twist2d", 24 },
        { "ChassisSpeeds", 24 }, { "ChassisVelocities", 24 }, { "SwerveModuleState", 16 },
        { "SwerveModuleVelocity", 16 }, { "SwerveModulePosition", 16 }, { "Translation3d", 24 },
        { "Quaternion", 32 }, { "Rotation3d", 32 }, { "Pose3d", 56 },
    };
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) if (strcmp(W[i].n, name) == 0) return W[i].w;
    return 0;
}

static double le_double(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    double d;
    memcpy(&d, &v, 8);
    return d;
}

/* 2027's struct:ControlWord as the 2026 FMSControlData bits: enabled 1, auto 2, test 4, e-stop 8,
 * FMS 16, DS 32 (Console nt4.rs control_word). */
static double control_word(const uint8_t *b)
{
    uint64_t w = 0;
    for (int i = 7; i >= 0; i--) w = w << 8 | b[i];
    unsigned mode = (unsigned)(w >> 56) & 3, legacy = 0;
    if ((w >> 58) & 1) legacy |= 1;
    if (mode == 1) legacy |= 2;
    if (mode == 3) legacy |= 4;
    if ((w >> 59) & 1) legacy |= 8;
    if ((w >> 60) & 1) legacy |= 16;
    if ((w >> 61) & 1) legacy |= 32;
    return legacy;
}

static void store_struct(topic_t *t, const uint8_t *p, size_t n)
{
    const char *name = t->type + 7; /* past "struct:" */
    char base[TYPE_MAX_LEN];
    snprintf(base, sizeof base, "%s", name);
    size_t bl = strlen(base);
    bool array = bl > 2 && strcmp(base + bl - 2, "[]") == 0;
    if (array) base[bl - 2] = 0;
    if (!array && strcmp(base, "ControlWord") == 0 && n == 8) {
        set_num(t, control_word(p));
        return;
    }
    int w = struct_width(base);
    if (!w || n % (size_t)w || (!array && n != (size_t)w)) return; /* ragged or unknown: left out */
    int count = (int)(n / 8);
    if (!ensure_nums(t, count)) return;
    if (count > t->cap_nums) count = t->cap_nums;
    for (int i = 0; i < count; i++) t->nums[i] = le_double(p + 8 * i);
    t->nnums = count;
    if (!array && w == 8) set_num(t, t->nums[0]);
}

/* Reads one value item (already known to be the 4th element) into the topic. */
static bool store_value(topic_t *t, mp_reader_t *r, int64_t type)
{
    mp_item_t it;
    if (!mp_read(r, &it)) return false;
    t->wire = (nt4_type_t)type;
    switch (it.kind) {
    case MP_BOOL: case MP_INT: case MP_UINT: case MP_F32: case MP_F64:
        set_num(t, mp_as_double(&it));
        return true;
    case MP_STR:
        set_str(t, it.bytes.p, it.bytes.n);
        return true;
    case MP_BIN:
        if (strncmp(t->type, "struct:", 7) == 0) store_struct(t, it.bytes.p, it.bytes.n);
        else if (strcmp(t->type, "json") == 0 || strcmp(t->type, "string") == 0) set_str(t, it.bytes.p, it.bytes.n);
        return true;
    case MP_ARRAY: {
        uint32_t n = it.count;
        if (type == NT4_STRING_ARRAY) {
            t->strs_len = 0;
            t->nstrs = 0;
            for (uint32_t i = 0; i < n; i++) {
                mp_item_t s;
                if (!mp_read(r, &s)) return false;
                if (s.kind != MP_STR) continue;
                size_t need = t->strs_len + s.bytes.n + 1;
                if (need > t->strs_cap) {
                    size_t cap = t->strs_cap ? t->strs_cap : 256;
                    while (cap < need) cap *= 2;
                    char *b = realloc(t->strs, cap);
                    if (!b) return false;
                    t->strs = b;
                    t->strs_cap = cap;
                }
                memcpy(t->strs + t->strs_len, s.bytes.p, s.bytes.n);
                t->strs_len += s.bytes.n;
                t->strs[t->strs_len++] = 0;
                t->nstrs++;
            }
            return true;
        }
        ensure_nums(t, (int)n);
        int kept = 0;
        for (uint32_t i = 0; i < n; i++) {
            mp_item_t e;
            if (!mp_read(r, &e)) return false;
            if (kept < t->cap_nums) t->nums[kept++] = mp_as_double(&e);
        }
        t->nnums = kept;
        return true;
    }
    default:
        return true;
    }
}

/* ---------------------------------------------------------------- protocol */

static void handle_text(nt4_client_t *c, const char *text, size_t len)
{
    /* Announce batches on connect can run to hundreds of topics; parse message by message by
     * tokenizing the whole array once with a heap token buffer sized to the text. */
    int maxtok = (int)(len / 4) + 16;
    jl_tok_t *tok = malloc(sizeof(jl_tok_t) * (size_t)maxtok);
    if (!tok) return;
    int n = jl_parse(text, len, tok, maxtok);
    if (n <= 0 || tok[0].type != JL_ARR) {
        free(tok);
        return;
    }
    jl_doc_t d = { text, tok, n };
    pthread_mutex_lock(&c->lock);
    bool ids_changed = false;
    for (int m = 0; m < tok[0].size; m++) {
        int msg = jl_at(&d, 0, m);
        int method = jl_get(&d, msg, "method");
        int params = jl_get(&d, msg, "params");
        char name[NAME_MAX_LEN];
        if (jl_eq(&d, method, "announce")) {
            jl_str(&d, jl_get(&d, params, "name"), name, sizeof name);
            int k = intern(c, name);
            if (k < 0) continue;
            topic_t *t = &c->topics[k];
            t->id = (int64_t)jl_num(&d, jl_get(&d, params, "id"), -1);
            jl_str(&d, jl_get(&d, params, "type"), t->type, sizeof t->type);
            ids_changed = true;
        } else if (jl_eq(&d, method, "unannounce")) {
            jl_str(&d, jl_get(&d, params, "name"), name, sizeof name);
            int k = find_name(c, name);
            if (k >= 0) {
                c->topics[k].id = -1;
                ids_changed = true;
            }
        }
    }
    if (ids_changed) {
        id_rebuild(c);
        int announced = 0;
        for (int k = 0; k < c->ntopics; k++) announced += c->topics[k].id >= 0;
        c->status.topics = (uint32_t)announced;
        c->generation++;
    }
    pthread_mutex_unlock(&c->lock);
    free(tok);
}

static void handle_binary(nt4_client_t *c, const uint8_t *buf, size_t len, int64_t *last_ping_sent)
{
    mp_reader_t r;
    mp_reader_init(&r, buf, len);
    int64_t now = nt4_now_us();
    pthread_mutex_lock(&c->lock);
    while (!mp_done(&r)) {
        mp_item_t arr, id, ts, ty;
        if (!mp_read(&r, &arr) || arr.kind != MP_ARRAY || arr.count != 4) break;
        if (!mp_read(&r, &id) || !mp_read(&r, &ts) || !mp_read(&r, &ty)) break;
        int64_t tid = mp_as_int(&id);
        if (tid == -1) {
            /* [-1, serverTime, type, clientTimeWeSent]: the round trip is now minus what we sent */
            mp_item_t sent;
            if (!mp_read(&r, &sent)) break;
            int64_t rtt = now - mp_as_int(&sent);
            if (rtt >= 0 && rtt < 5000000) {
                c->status.rtt_us = rtt;
                c->status.offset_us = mp_as_int(&ts) + rtt / 2 - now;
            }
            (void)last_ping_sent;
            continue;
        }
        int k = find_id(c, tid);
        if (k < 0) {
            if (!mp_skip(&r)) break;
            continue;
        }
        topic_t *t = &c->topics[k];
        if (!store_value(t, &r, mp_as_int(&ty))) break;
        t->seq++;
        t->stamp_us = now;
        c->status.updates++;
        c->generation++;
    }
    pthread_mutex_unlock(&c->lock);
}

static void json_escape(char *out, size_t n, const char *s)
{
    size_t o = 0;
    while (*s && o + 3 < n) {
        char ch = *s++;
        if (ch == '"' || ch == '\\') out[o++] = '\\';
        out[o++] = ch;
    }
    out[o] = 0;
}

static bool send_subscribe(nt4_client_t *c)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "[{\"method\":\"subscribe\",\"params\":{\"topics\":[");
    for (int i = 0; i < c->nprefixes && n < (int)sizeof buf - 64; i++) {
        char esc[NAME_MAX_LEN * 2];
        json_escape(esc, sizeof esc, c->prefixes[i]);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\"%s\"", i ? "," : "", esc);
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n,
                  "],\"subuid\":1,\"options\":{\"prefix\":true,\"periodic\":%.3f}}}]", c->period);
    return ws_send(&c->ws, WS_TEXT, buf, (size_t)n);
}

static bool send_timestamp(nt4_client_t *c)
{
    uint8_t buf[32];
    mp_writer_t w;
    mp_writer_init(&w, buf, sizeof buf);
    mp_write_array(&w, 4);
    mp_write_int(&w, -1);
    mp_write_int(&w, 0);
    mp_write_int(&w, NT4_INT);
    mp_write_int(&w, nt4_now_us());
    return ws_send(&c->ws, WS_BINARY, buf, w.len);
}

static const char *type_name(nt4_type_t t)
{
    switch (t) {
    case NT4_BOOLEAN: return "boolean";
    case NT4_INT: return "int";
    case NT4_FLOAT: return "float";
    case NT4_STRING: return "string";
    default: return "double";
    }
}

static nt4_type_t type_from_name(const char *type, nt4_type_t fallback)
{
    if (strcmp(type, "boolean") == 0) return NT4_BOOLEAN;
    if (strcmp(type, "int") == 0) return NT4_INT;
    if (strcmp(type, "float") == 0) return NT4_FLOAT;
    if (strcmp(type, "double") == 0) return NT4_DOUBLE;
    if (strcmp(type, "string") == 0 && fallback == NT4_STRING) return NT4_STRING;
    return fallback;
}

/* Sends every queued write. Publishing is per session: a reconnect starts pubuids over, because the
 * server forgot us too. */
static bool drain_sets(nt4_client_t *c)
{
    for (int i = 0; i < SETQ_LEN; i++) {
        pthread_mutex_lock(&c->lock);
        set_req_t req = c->setq[i];
        c->setq[i].used = false;
        int k = req.used ? intern(c, req.name) : -1;
        int64_t pubuid = 0;
        bool announce = false;
        if (k >= 0) {
            topic_t *t = &c->topics[k];
            if (!t->pubuid) {
                t->pubuid = c->next_pubuid++;
                announce = true;
            }
            pubuid = t->pubuid;
            /* Publish in the type the robot announced: a number typed into an int tunable must not
             * arrive as a double the server refuses. */
            nt4_type_t want = type_from_name(t->type, req.type);
            if (want != req.type) {
                if (want == NT4_INT) req.i = req.type == NT4_INT ? req.i : (int64_t)llround(req.num);
                else if (req.type == NT4_INT) req.num = (double)req.i;
                req.type = want;
            }
        }
        pthread_mutex_unlock(&c->lock);
        if (k < 0) continue;

        if (announce) {
            char esc[NAME_MAX_LEN * 2], buf[NAME_MAX_LEN * 2 + 128];
            json_escape(esc, sizeof esc, req.name);
            int n = snprintf(buf, sizeof buf,
                             "[{\"method\":\"publish\",\"params\":{\"name\":\"%s\",\"pubuid\":%lld,"
                             "\"type\":\"%s\",\"properties\":{}}}]",
                             esc, (long long)pubuid, type_name(req.type));
            if (!ws_send(&c->ws, WS_TEXT, buf, (size_t)n)) return false;
        }
        uint8_t buf[256];
        mp_writer_t w;
        mp_writer_init(&w, buf, sizeof buf);
        mp_write_array(&w, 4);
        mp_write_int(&w, pubuid);
        int64_t stamp = nt4_now_us() + c->status.offset_us;
        mp_write_int(&w, stamp > 0 ? stamp : 0);
        mp_write_int(&w, req.type);
        switch (req.type) {
        case NT4_BOOLEAN: mp_write_bool(&w, req.num != 0); break;
        case NT4_INT: mp_write_int(&w, req.i); break;
        case NT4_STRING: mp_write_str(&w, req.str, strlen(req.str)); break;
        case NT4_FLOAT: mp_write_f32(&w, (float)req.num); break;
        default: mp_write_f64(&w, req.num); break;
        }
        if (!w.overflow && !ws_send(&c->ws, WS_BINARY, buf, w.len)) return false;
    }
    return true;
}

static void forget_session(nt4_client_t *c)
{
    pthread_mutex_lock(&c->lock);
    for (int k = 0; k < c->ntopics; k++) {
        c->topics[k].id = -1;
        c->topics[k].pubuid = 0;
    }
    id_rebuild(c);
    c->status.topics = 0;
    c->status.state = NT4_CONNECTING;
    c->generation++;
    pthread_mutex_unlock(&c->lock);
}

static void session(nt4_client_t *c, const char *addr)
{
    char path[80];
    snprintf(path, sizeof path, "/nt/%s", c->client_name);
    char proto[48];
    pthread_mutex_lock(&c->lock);
    snprintf(c->status.address, sizeof c->status.address, "%s", addr);
    c->status.state = NT4_CONNECTING;
    pthread_mutex_unlock(&c->lock);
    if (!ws_connect(&c->ws, addr, 5810, path, "v4.1.networktables.first.wpi.edu, networktables.first.wpi.edu",
                    proto, sizeof proto, 1200)) {
        pthread_mutex_lock(&c->lock);
        snprintf(c->status.last_error, sizeof c->status.last_error, "%s: %s", addr, c->ws.error);
        pthread_mutex_unlock(&c->lock);
        return;
    }
    pthread_mutex_lock(&c->lock);
    c->status.state = NT4_CONNECTED;
    c->status.updates = 0;
    c->status.connected_since_us = nt4_now_us();
    snprintf(c->status.protocol, sizeof c->status.protocol, "%s", proto);
    c->status.last_error[0] = 0;
    c->generation++;
    pthread_mutex_unlock(&c->lock);

    int64_t last_ts = 0;
    bool ok = send_subscribe(c);
    while (ok && !c->stop && !c->kick && !c->addrs_changed) {
        int64_t now = nt4_now_us();
        if (now - last_ts > 1000000) {
            last_ts = now;
            ok = send_timestamp(c);
        }
        if (ok) ok = drain_sets(c);
        int op;
        const uint8_t *data;
        size_t len;
        int r = ok ? ws_recv(&c->ws, 20, &op, &data, &len) : -1;
        if (r < 0) {
            pthread_mutex_lock(&c->lock);
            snprintf(c->status.last_error, sizeof c->status.last_error, "%s", c->ws.error);
            pthread_mutex_unlock(&c->lock);
            break;
        }
        if (r == 0) continue;
        if (op == WS_TEXT) handle_text(c, (const char *)data, len);
        else if (op == WS_BINARY) handle_binary(c, data, len, &last_ts);
    }
    ws_close(&c->ws);
    c->status.reconnects++;
    forget_session(c);
}

void nt4_run(nt4_client_t *c)
{
    int next = 0;
    while (!c->stop) {
        pthread_mutex_lock(&c->lock);
        c->addrs_changed = false;
        int n = c->naddrs;
        char addr[64] = "";
        if (n) snprintf(addr, sizeof addr, "%s", c->addrs[next++ % n]);
        pthread_mutex_unlock(&c->lock);
        c->kick = false;
        if (!n) {
            c->status.state = NT4_IDLE;
            usleep(200000);
            continue;
        }
        session(c, addr);
        if (!c->stop && c->status.state != NT4_CONNECTED) usleep(150000);
    }
    c->status.state = NT4_IDLE;
}

/* ---------------------------------------------------------------- getters */

bool nt4_get_number(nt4_client_t *c, const char *name, double *out)
{
    pthread_mutex_lock(&c->lock);
    int k = find_name(c, name);
    bool ok = k >= 0 && c->topics[k].has_num;
    if (ok) *out = c->topics[k].num;
    pthread_mutex_unlock(&c->lock);
    return ok;
}

bool nt4_get_bool(nt4_client_t *c, const char *name, bool *out)
{
    double v;
    if (!nt4_get_number(c, name, &v)) return false;
    *out = v != 0;
    return true;
}

bool nt4_get_string(nt4_client_t *c, const char *name, char *buf, size_t n)
{
    pthread_mutex_lock(&c->lock);
    int k = find_name(c, name);
    bool ok = k >= 0 && c->topics[k].str;
    if (ok) snprintf(buf, n, "%s", c->topics[k].str);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int nt4_get_numbers(nt4_client_t *c, const char *name, double *out, int max)
{
    pthread_mutex_lock(&c->lock);
    int k = find_name(c, name), count = -1;
    if (k >= 0 && (c->topics[k].nums || c->topics[k].seq)) {
        topic_t *t = &c->topics[k];
        count = t->nnums < max ? t->nnums : max;
        if (count > 0) memcpy(out, t->nums, sizeof(double) * (size_t)count);
        if (!t->nums) count = -1;
    }
    pthread_mutex_unlock(&c->lock);
    return count;
}

int nt4_get_strings(nt4_client_t *c, const char *name, char *buf, size_t buflen, const char **items, int max)
{
    pthread_mutex_lock(&c->lock);
    int k = find_name(c, name), count = -1;
    if (k >= 0 && c->topics[k].strs) {
        topic_t *t = &c->topics[k];
        size_t copy = t->strs_len < buflen ? t->strs_len : buflen;
        memcpy(buf, t->strs, copy);
        if (copy && buf[copy - 1]) buf[copy - 1] = 0;
        count = 0;
        for (size_t off = 0; off < copy && count < max && count < t->nstrs;) {
            items[count++] = buf + off;
            off += strlen(buf + off) + 1;
        }
    } else if (k >= 0 && c->topics[k].seq && c->topics[k].wire == NT4_STRING_ARRAY) {
        count = 0;
    }
    pthread_mutex_unlock(&c->lock);
    return count;
}

bool nt4_info(nt4_client_t *c, const char *name, char *type, size_t type_len, uint32_t *seq, int64_t *age_us)
{
    pthread_mutex_lock(&c->lock);
    int k = find_name(c, name);
    if (k >= 0) {
        topic_t *t = &c->topics[k];
        if (type) snprintf(type, type_len, "%s", t->type);
        if (seq) *seq = t->seq;
        if (age_us) *age_us = t->seq ? nt4_now_us() - t->stamp_us : INT64_MAX;
    }
    pthread_mutex_unlock(&c->lock);
    return k >= 0;
}

int nt4_list(nt4_client_t *c, const char *prefix, nt4_visit_fn fn, void *user)
{
    size_t pl = strlen(prefix);
    int n = 0;
    pthread_mutex_lock(&c->lock);
    for (int k = 0; k < c->ntopics; k++) {
        topic_t *t = &c->topics[k];
        if (t->id < 0 && !t->seq) continue;
        if (strncmp(t->name, prefix, pl) != 0) continue;
        fn(t->name, t->type, user);
        n++;
    }
    pthread_mutex_unlock(&c->lock);
    return n;
}

/* ---------------------------------------------------------------- setters */

static void enqueue(nt4_client_t *c, const char *name, nt4_type_t type, double num, int64_t i, const char *s)
{
    pthread_mutex_lock(&c->lock);
    /* Coalesce: a slider dragged across a hundred values only needs its latest sent. */
    int slot = -1;
    for (int k = 0; k < SETQ_LEN; k++) {
        if (c->setq[k].used && strcmp(c->setq[k].name, name) == 0) { slot = k; break; }
        if (!c->setq[k].used && slot < 0) slot = k;
    }
    if (slot >= 0) {
        set_req_t *r = &c->setq[slot];
        r->used = true;
        snprintf(r->name, sizeof r->name, "%s", name);
        r->type = type;
        r->num = num;
        r->i = i;
        snprintf(r->str, sizeof r->str, "%s", s ? s : "");
    }
    /* The local copy updates at once, so the control that wrote it never flickers back. */
    int k = intern(c, name);
    if (k >= 0) {
        topic_t *t = &c->topics[k];
        if (type == NT4_STRING) set_str(t, (const uint8_t *)s, strlen(s));
        else set_num(t, type == NT4_INT ? (double)i : num);
        if (!t->type[0]) snprintf(t->type, sizeof t->type, "%s", type_name(type));
        t->wire = type;
        t->seq++;
        t->stamp_us = nt4_now_us();
        c->generation++;
    }
    pthread_mutex_unlock(&c->lock);
}

void nt4_set_double(nt4_client_t *c, const char *name, double v) { enqueue(c, name, NT4_DOUBLE, v, 0, NULL); }
void nt4_set_int(nt4_client_t *c, const char *name, int64_t v) { enqueue(c, name, NT4_INT, 0, v, NULL); }
void nt4_set_bool(nt4_client_t *c, const char *name, bool v) { enqueue(c, name, NT4_BOOLEAN, v ? 1 : 0, 0, NULL); }
void nt4_set_string(nt4_client_t *c, const char *name, const char *v) { enqueue(c, name, NT4_STRING, 0, 0, v); }
