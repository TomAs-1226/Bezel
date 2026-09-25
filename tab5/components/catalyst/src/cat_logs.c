#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cat_logs.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cat_sc.h" /* cat_csv_last_t */

#define LABVIEW_EPOCH 2082844800LL /* seconds from 1904-01-01 to 1970-01-01 */

static void init(cat_log_t *o, const char *kind)
{
    memset(o, 0, sizeof *o);
    snprintf(o->kind, sizeof o->kind, "%s", kind);
    for (int s = 0; s < CAT_LOG_SERIES; s++)
        for (int i = 0; i < CAT_LOG_POINTS; i++) o->series[s][i] = NAN;
    o->min_battery = NAN;
}

/* Battery dips below the controller's brownout floor, counted as events, for logs that carry no
 * brownout flag of their own (6.75 V is Systemcore's floor; a roboRIO's is 6.8 V). */
static void dip(cat_log_t *o, float v)
{
    bool below = v < 6.8f;
    if (below && !o->below) o->dips++;
    o->below = below;
}

static void put(cat_log_t *o, cat_log_series_t s, double t, double duration, float v)
{
    if (!(v == v)) return;
    int i = duration > 0 ? (int)(t / duration * (CAT_LOG_POINTS - 1)) : 0;
    if (i < 0) i = 0;
    if (i >= CAT_LOG_POINTS) i = CAT_LOG_POINTS - 1;
    float *p = &o->series[s][i];
    o->have[s] = true;
    if (!(*p == *p)) *p = v;
    else if (s == CAT_LOG_BATTERY) *p = v < *p ? v : *p;
    else *p = v > *p ? v : *p;
    if (s == CAT_LOG_BATTERY) dip(o, v);
    switch (s) {
    case CAT_LOG_BATTERY:
        if (!(o->min_battery == o->min_battery) || v < o->min_battery) o->min_battery = v;
        o->batt_sum += v;
        o->batt_n++;
        break;
    case CAT_LOG_TRIP: if (v > o->max_trip_ms) o->max_trip_ms = v; break;
    case CAT_LOG_CAN: if (v > o->max_can) o->max_can = v; break;
    case CAT_LOG_CPU: if (v > o->max_cpu) o->max_cpu = v; break;
    default: break;
    }
}

/* words that mark a message as CAN trouble (CTRE's "CAN frame not received", a bus-off, a timeout) */
static bool can_words(const char *low)
{
    static const char *const W[] = { "can frame", "can bus", "canbus", "can timeout", "can error", "can id", "bus off",
                                     "busoff", "can_s", "[can", "can receive", "can transmit", "cantimeout" };
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++)
        if (strstr(low, W[i])) return true;
    return false;
}

/* An event. The level is `level`, or from the words when it is -1. Once the list is full an info event is
 * dropped, and a warning or an error takes the place of the latest info one (the list is put back in time
 * order at the end), so a long log keeps what went wrong rather than its first minute. */
static void event_lv(cat_log_t *o, double t, const char *text, size_t len, int level)
{
    cat_log_event_t e;
    e.t = t;
    if (len >= sizeof e.text) len = sizeof e.text - 1;
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if ((unsigned char)c < 32) continue;
        e.text[k++] = c;
    }
    e.text[k] = 0;
    /* dslog.rs levels from the words */
    char low[100];
    for (size_t i = 0; i <= k; i++) low[i] = (char)tolower((unsigned char)e.text[i]);
    if (level >= 0) e.level = (uint8_t)level;
    else if (strstr(low, "error") || strstr(low, "estop") || strstr(low, "brownout")) e.level = 2;
    else if (strstr(low, "warning") || strstr(low, "lost") || strstr(low, "dropped") || strstr(low, "watchdog")) e.level = 1;
    else e.level = 0;
    o->events_seen++;
    if (e.level == 2) o->errors++;
    if (e.level == 1) o->warnings++;
    if (level < 0 && strstr(low, "overrun")) o->overruns++;
    if (level < 0 && can_words(low)) o->can_faults++;
    if (o->nevents < CAT_LOG_EVENTS) {
        o->events[o->nevents++] = e;
        return;
    }
    if (!e.level) return;
    for (int i = o->nevents - 1; i >= 0; i--)
        if (o->events[i].level == 0) {
            o->events[i] = e;
            return;
        }
}

static void event(cat_log_t *o, double t, const char *text, size_t len) { event_lv(o, t, text, len, -1); }
static void event_s(cat_log_t *o, double t, const char *text, int level) { event_lv(o, t, text, strlen(text), level); }

static int by_time(const void *a, const void *b)
{
    double x = ((const cat_log_event_t *)a)->t, y = ((const cat_log_event_t *)b)->t;
    return x < y ? -1 : x > y;
}

static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

static double le_f64(const uint8_t *p)
{
    uint64_t v = le(p, 8);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static float le_f32(const uint8_t *p)
{
    uint32_t v = (uint32_t)le(p, 4);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* ------------------------------------------------------------------ wpilog */

/* What an entry is read as. Besides the series and the console, the Catalyst topics a robot log carries
 * when NetworkTables is logged (docs/catalyst-integration.md): its alerts, its health checks firing, its
 * mechanisms' and state machines' State, the loop monitor, and Systemcore's CanDown; and WPILib's DS:
 * mode flags. */
enum { K_NONE, K_SERIES, K_EVENT, K_BROWN, K_MODE, K_OVER, K_CANDOWN, K_FIRING, K_LOOPMS, K_STATE, K_ALERTS };

#define PREV_MAX 8

typedef struct {
    uint32_t id;
    uint8_t kind;
    int8_t series;      /* K_SERIES: cat_log_series_t */
    uint8_t sub;        /* K_MODE: 0 enabled, 1 autonomous, 2 test, 3 estop; K_ALERTS: the level */
    bool is_int, is_float_arr;
    bool last;          /* a boolean's last value */
    uint32_t last_hash; /* K_STATE: the last string's */
    int nprev;          /* K_ALERTS: the last array's strings, hashed */
    uint32_t prev[PREV_MAX];
    char label[40];     /* how its events name it: "Elevator", "Elevator/OverTemp" */
} entry_t;

static bool ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static bool ends_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

static uint32_t hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the part of `name` between `from` and `to` (both may be absent) into the label */
static void label_between(entry_t *e, const char *name, const char *from, const char *to)
{
    const char *a = from ? strstr(name, from) : NULL;
    a = a ? a + strlen(from) : name;
    while (*a == '/') a++;
    size_t l = strlen(a);
    if (to && l >= strlen(to) && !strcmp(a + l - strlen(to), to)) l -= strlen(to);
    if (l >= sizeof e->label) l = sizeof e->label - 1;
    memcpy(e->label, a, l);
    e->label[l] = 0;
}

static void classify(entry_t *e, const char *name, const char *type)
{
    uint32_t id = e->id;
    memset(e, 0, sizeof *e);
    e->id = id;
    e->series = -1;
    e->is_int = !strcmp(type, "int64");
    e->is_float_arr = !strcmp(type, "float[]") || !strcmp(type, "double[]");
    bool is_bool = !strcmp(type, "boolean"), is_str = !strcmp(type, "string"), is_num = !strcmp(type, "double") ||
                                                                                   !strcmp(type, "float") || e->is_int;
    if (ends(name, "/Battery") || ends(name, "BatteryVolts") || ends(name, "BatteryVoltage") || ends(name, "MeasuredVoltage"))
        e->series = CAT_LOG_BATTERY;
    else if (ends(name, "/PacketTime")) e->series = CAT_LOG_TRIP;
    else if (ends(name, "/CPU") || ends(name, "CpuPercent")) e->series = CAT_LOG_CPU;
    else if (ends(name, "CanBusUtilizations") || ends(name, "CanUtilization")) e->series = CAT_LOG_CAN;
    if (e->series >= 0) {
        e->kind = K_SERIES;
        return;
    }
    if (is_bool && (ends(name, "BrownedOut") || ends(name, "Brownout"))) e->kind = K_BROWN;
    else if (is_bool && !strncmp(name, "DS:", 3)) {
        static const char *const M[4] = { "enabled", "autonomous", "test", "estop" };
        for (int i = 0; i < 4; i++)
            if (ends_ci(name, M[i])) {
                e->kind = K_MODE;
                e->sub = (uint8_t)i;
            }
    } else if (is_bool && ends(name, "/OverBudget")) e->kind = K_OVER;
    else if (is_bool && ends(name, "CanDown")) e->kind = K_CANDOWN;
    else if (is_bool && strstr(name, "/Health/") && ends(name, "/firing")) {
        e->kind = K_FIRING;
        label_between(e, name, "/Health/", "/firing");
    } else if (is_num && strstr(name, "/Loop/") && (ends(name, "LastMs") || ends(name, "MaxMs"))) e->kind = K_LOOPMS;
    else if (is_str && strstr(name, "/Catalyst/") && ends(name, "/State")) {
        e->kind = K_STATE;
        label_between(e, name, "/Catalyst/", "/State");
    } else if (!strcmp(type, "string[]") && strstr(name, "/Alerts/") && (ends(name, "/Errors") || ends(name, "/Warnings"))) {
        e->kind = K_ALERTS;
        e->sub = ends(name, "/Errors") ? 2 : 1;
    } else if (is_str && (!strcmp(name, "messages") || strstr(name, "Message") || strstr(name, "Event") || strstr(name, "console")))
        e->kind = K_EVENT;
}

/* A boolean record: true when it rose from false. */
static bool rose(entry_t *e, const uint8_t *pl, uint32_t size)
{
    bool v = size >= 1 && pl[0] != 0, up = v && !e->last;
    e->last = v;
    return up;
}

/* The DS: mode flags as one line, on any change: "mode: enabled auto", "mode: disabled", "mode: estop". */
static void mode_event(cat_log_t *o, double t, const bool *m)
{
    char b[48];
    if (m[3]) snprintf(b, sizeof b, "mode: estop");
    else if (!m[0]) snprintf(b, sizeof b, "mode: disabled");
    else snprintf(b, sizeof b, "mode: enabled %s", m[1] ? "auto" : m[2] ? "test" : "teleop");
    event_lv(o, t, b, strlen(b), m[3] ? 2 : 0);
    o->mode_changes++;
}

/* A string[] of alerts ("[Elevator] not zeroed"): each string that wasn't in the last array is an event. */
static void alerts_event(cat_log_t *o, entry_t *e, double t, const uint8_t *pl, uint32_t size)
{
    uint32_t prev[PREV_MAX];
    int nprev = e->nprev;
    memcpy(prev, e->prev, sizeof prev);
    e->nprev = 0;
    if (size < 4) return;
    uint32_t count = (uint32_t)le(pl, 4), off = 4;
    for (uint32_t i = 0; i < count && off + 4 <= size; i++) {
        uint32_t l = (uint32_t)le(pl + off, 4);
        off += 4;
        if (off + l > size) break;
        uint32_t h = hash(pl + off, l);
        bool seen = false;
        for (int k = 0; k < nprev; k++) seen |= prev[k] == h;
        if (e->nprev < PREV_MAX) e->prev[e->nprev++] = h;
        if (!seen) {
            char b[100];
            snprintf(b, sizeof b, "alert %s: %.*s", e->sub == 2 ? "error" : "warning", (int)(l < 80 ? l : 80),
                             (const char *)pl + off);
            event_s(o, t, b, e->sub);
        }
        off += l;
    }
}

static bool wpilog(const uint8_t *d, size_t len, cat_log_t *o)
{
    init(o, "wpilog");
    if (len < 12 || memcmp(d, "WPILOG", 6)) {
        snprintf(o->error, sizeof o->error, "not a WPILOG file");
        return false;
    }
    size_t start = 12 + (size_t)le(d + 8, 4);
    if (start > len) {
        snprintf(o->error, sizeof o->error, "truncated header");
        return false;
    }
    entry_t *entries = NULL;
    int nent = 0, cap = 0;
    double t0 = -1, t1 = 0;
    /* pass 0 measures the time span, pass 1 buckets into it */
    bool mode[4] = { false, false, false, false };
    for (int pass = 0; pass < 2; pass++) {
        size_t p = start;
        while (p < len) {
            uint8_t bf = d[p];
            int idl = (bf & 3) + 1, szl = ((bf >> 2) & 3) + 1, tsl = ((bf >> 4) & 7) + 1;
            if (p + 1 + idl + szl + tsl > len) break;
            uint32_t id = (uint32_t)le(d + p + 1, idl);
            uint32_t size = (uint32_t)le(d + p + 1 + idl, szl);
            double ts = (double)le(d + p + 1 + idl + szl, tsl) / 1e6;
            const uint8_t *pl = d + p + 1 + idl + szl + tsl;
            p += 1 + (size_t)idl + szl + tsl + size;
            if (p > len) break;
            if (id == 0) {
                if (pass == 1 || size < 5 || pl[0] != 0) continue;
                /* start: entry id, name, type, metadata — each a u32-length-prefixed string */
                uint32_t eid = (uint32_t)le(pl + 1, 4);
                uint32_t nl = (uint32_t)le(pl + 5, 4);
                if (9 + nl + 4 > size) continue;
                uint32_t tl = (uint32_t)le(pl + 9 + nl, 4);
                if (13 + nl + tl > size) continue;
                char name[128], type[32];
                snprintf(name, sizeof name, "%.*s", (int)(nl < 127 ? nl : 127), (const char *)pl + 9);
                snprintf(type, sizeof type, "%.*s", (int)(tl < 31 ? tl : 31), (const char *)pl + 13 + nl);
                if (nent == cap) {
                    cap = cap ? cap * 2 : 64;
                    entry_t *n = realloc(entries, sizeof(entry_t) * (size_t)cap);
                    if (!n) break;
                    entries = n;
                }
                entry_t *e = &entries[nent++];
                e->id = eid;
                classify(e, name, type);
                continue;
            }
            if (pass == 0) {
                if (t0 < 0 || ts < t0) t0 = ts;
                if (ts > t1) t1 = ts;
                o->records++;
                continue;
            }
            entry_t *e = NULL;
            for (int i = 0; i < nent; i++) if (entries[i].id == id) { e = &entries[i]; break; }
            if (!e) continue;
            double t = ts - t0;
            if (e->kind == K_SERIES) {
                float v = NAN;
                if (e->is_float_arr) {
                    /* per-bus percent: keep the busiest bus */
                    bool dbl = size % 8 == 0 && size >= 8 && e->series != CAT_LOG_CAN;
                    int w = dbl ? 8 : 4;
                    for (uint32_t k = 0; k + (uint32_t)w <= size; k += (uint32_t)w) {
                        float x = dbl ? (float)le_f64(pl + k) : le_f32(pl + k);
                        if (!(v == v) || x > v) v = x;
                    }
                    if (e->series == CAT_LOG_CAN && v == v && v > 1.5f) v /= 100.0f;
                } else if (size == 8) {
                    v = e->is_int ? (float)(int64_t)le(pl, 8) : (float)le_f64(pl);
                    if (e->series == CAT_LOG_TRIP && e->is_int) v /= 1000.0f; /* µs → ms */
                    if (e->series == CAT_LOG_CPU && v > 1.5f) v /= 100.0f;
                }
                put(o, e->series, t, o->duration_s, v);
            } else if (e->kind == K_BROWN) {
                if (rose(e, pl, size)) {
                    o->brownouts++;
                    event_s(o, t, "brownout (the controller's flag)", 2);
                }
            } else if (e->kind == K_MODE) {
                bool v = size >= 1 && pl[0] != 0;
                if (v != mode[e->sub]) {
                    mode[e->sub] = v;
                    mode_event(o, t, mode);
                }
            } else if (e->kind == K_OVER) {
                if (rose(e, pl, size)) o->overruns++;
            } else if (e->kind == K_CANDOWN) {
                if (rose(e, pl, size)) {
                    o->can_faults++;
                    event_s(o, t, "CAN bus down (Systemcore CanDown)", 2);
                }
            } else if (e->kind == K_FIRING) {
                if (rose(e, pl, size)) {
                    char b[64];
                    snprintf(b, sizeof b, "health check firing: %s", e->label);
                    o->health_firings++;
                    event_s(o, t, b, 1);
                }
            } else if (e->kind == K_LOOPMS) {
                float v = size == 8 ? (e->is_int ? (float)(int64_t)le(pl, 8) : (float)le_f64(pl)) : size == 4 ? le_f32(pl) : NAN;
                if (v == v && v > o->max_loop_ms && v < 10000) o->max_loop_ms = v;
            } else if (e->kind == K_STATE) {
                uint32_t h = hash(pl, size) | 1;
                if (h != e->last_hash) {
                    if (e->last_hash) o->state_changes++;
                    e->last_hash = h;
                    char b[100];
                    snprintf(b, sizeof b, "state: %s = %.*s", e->label, (int)(size < 40 ? size : 40), (const char *)pl);
                    event_s(o, t, b, 0);
                }
            } else if (e->kind == K_ALERTS) {
                alerts_event(o, e, t, pl, size);
            } else if (e->kind == K_EVENT) {
                event(o, t, (const char *)pl, size);
            }
        }
        if (pass == 0) o->duration_s = t1 > t0 && t0 >= 0 ? t1 - t0 : 0;
    }
    free(entries);
    return true;
}

/* ------------------------------------------------------------------ NI dslog */

/* The per-record power-distribution trailer depends on the PD type (a CTRE PDP and a REV PDH differ),
 * which Console's fixed 35-byte stride doesn't handle. Rather than trust one layout, try the plausible
 * strides and keep the one whose battery readings look like a battery. */
static double plausibility(const uint8_t *d, size_t len, size_t stride)
{
    int good = 0, total = 0;
    double last = -1;
    for (size_t p = 20; p + stride <= len && total < 300; p += stride, total++) {
        double v = d[p + 2] + d[p + 3] / 256.0;
        if (v >= 4 && v <= 16 && (last < 0 || fabs(v - last) < 1.5)) good++;
        last = v;
    }
    return total ? (double)good / total : 0;
}

static bool dslog(const uint8_t *d, size_t len, cat_log_t *o)
{
    init(o, "dslog");
    if (len < 20) {
        snprintf(o->error, sizeof o->error, "too short");
        return false;
    }
    uint32_t ver = (uint32_t)be(d, 4);
    if (ver != 3 && ver != 4) {
        snprintf(o->error, sizeof o->error, "dslog version %u", (unsigned)ver);
        return false;
    }
    o->start_unix = (int64_t)be(d + 4, 8) - LABVIEW_EPOCH;
    static const size_t strides[] = { 35, 10 + 4 + 33, 10 + 4, 10 };
    size_t stride = 35;
    double best = -1;
    for (size_t i = 0; i < sizeof strides / sizeof strides[0]; i++) {
        double q = plausibility(d, len, strides[i]);
        if (q > best + 0.02) {
            best = q;
            stride = strides[i];
        }
    }
    o->records = (uint32_t)((len - 20) / stride);
    o->duration_s = o->records * 0.02;
    double loss_sum = 0;
    bool last_brown = false;
    uint32_t i = 0;
    for (size_t p = 20; p + stride <= len; p += stride, i++) {
        const uint8_t *r = d + p;
        double t = i * 0.02;
        put(o, CAT_LOG_TRIP, t, o->duration_s, r[0] * 0.5f);
        float loss = (int8_t)r[1] * 4 / 100.0f;
        if (loss < 0) loss = 0;
        put(o, CAT_LOG_LOSS, t, o->duration_s, loss);
        loss_sum += loss;
        put(o, CAT_LOG_BATTERY, t, o->duration_s, r[2] + r[3] / 256.0f);
        put(o, CAT_LOG_CPU, t, o->duration_s, r[4] * 0.5f / 100.0f);
        /* status byte is inverted; bit 7 brownout */
        bool brown = !(r[5] & 0x80);
        if (brown && !last_brown) {
            o->brownouts++;
            event_s(o, t, "brownout (the DS status flag)", 2);
        }
        last_brown = brown;
        put(o, CAT_LOG_CAN, t, o->duration_s, r[6] * 0.5f / 100.0f);
    }
    o->avg_loss = o->records ? (float)(loss_sum / o->records) : 0;
    return true;
}

/* ------------------------------------------------------------------ NI dsevents */

static bool dsevents(const uint8_t *d, size_t len, cat_log_t *o)
{
    init(o, "dsevents");
    if (len < 20) {
        snprintf(o->error, sizeof o->error, "too short");
        return false;
    }
    o->start_unix = (int64_t)be(d + 4, 8) - LABVIEW_EPOCH;
    double t0 = (double)(int64_t)be(d + 4, 8) + be(d + 12, 8) / 18446744073709551616.0;
    size_t p = 20;
    while (p + 20 <= len) {
        double ts = (double)(int64_t)be(d + p, 8) + be(d + p + 8, 8) / 18446744073709551616.0;
        uint32_t n = (uint32_t)be(d + p + 16, 4);
        if (n > 8192 || p + 20 + n > len) break;
        double t = ts - t0;
        if (t > o->duration_s) o->duration_s = t;
        /* the DS wraps its messages in <TagVersion>…<message> markup; keep the words */
        const char *s = (const char *)d + p + 20;
        const char *m = memmem(s, n, "<message> ", 10);
        size_t off = m ? (size_t)(m - s) + 10 : 0;
        event(o, t, s + off, n - off);
        o->records++;
        p += 20 + n;
    }
    return true;
}

bool cat_log_parse(const char *name, const uint8_t *data, size_t len, cat_log_t *out)
{
    bool ok;
    if (len >= 6 && !memcmp(data, "WPILOG", 6)) ok = wpilog(data, len, out);
    else if (ends(name, ".dsevents")) ok = dsevents(data, len, out);
    else if (ends(name, ".dslog")) ok = dslog(data, len, out);
    else {
        init(out, "unknown");
        snprintf(out->error, sizeof out->error, "not a log this reads");
        return false;
    }
    /* warnings and errors may have taken info events' places out of order */
    qsort(out->events, (size_t)out->nevents, sizeof out->events[0], by_time);
    return ok;
}

bool cat_log_read(const char *path, cat_log_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        init(out, "unknown");
        snprintf(out->error, sizeof out->error, "can't open");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64L * 1024 * 1024) {
        fclose(f);
        init(out, "unknown");
        snprintf(out->error, sizeof out->error, n <= 0 ? "empty" : "larger than 64 MB");
        return false;
    }
    uint8_t *buf = malloc((size_t)n); /* PSRAM on the tablet */
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) ok = cat_log_parse(path, buf, (size_t)n, out);
    else {
        init(out, "unknown");
        snprintf(out->error, sizeof out->error, "read failed");
    }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ digests */

#define SLICES 24

typedef struct {
    char *p;
    size_t n, o;
} out_t;

static void outf(out_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(out_t *w, const char *fmt, ...)
{
    if (w->o + 1 >= w->n) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(w->p + w->o, w->n - w->o, fmt, ap);
    va_end(ap);
    if (k > 0) w->o += (size_t)k;
    if (w->o >= w->n) w->o = w->n - 1;
}

static void clock_len(char *b, size_t n, double s)
{
    int t = (int)(s + 0.5);
    snprintf(b, n, "%d:%02d", t / 60, t % 60);
}

size_t cat_log_digest(const cat_log_t *l, const char *name, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    out_t w = { out, n, 0 };
    char d[16];
    clock_len(d, sizeof d, l->duration_s);
    outf(&w, "file: %s (%s)\n", name, l->kind);
    if (l->start_unix > 0) {
        time_t t0 = (time_t)l->start_unix;
        struct tm tm;
        gmtime_r(&t0, &tm);
        outf(&w, "started: %04d-%02d-%02d %02d:%02d UTC\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
    outf(&w, "length: %s (%u records)\n", d, (unsigned)l->records);
    if (l->have[CAT_LOG_BATTERY])
        outf(&w, "battery: lowest %.2f V, mean %.2f V; brownouts flagged %d; dips under 6.8 V %d\n", l->min_battery,
             l->batt_n ? l->batt_sum / l->batt_n : 0.0, l->brownouts, l->dips);
    else outf(&w, "battery: not in this log; brownouts flagged %d\n", l->brownouts);
    if (l->have[CAT_LOG_TRIP]) outf(&w, "driver station trip time: worst %.1f ms\n", l->max_trip_ms);
    if (l->have[CAT_LOG_LOSS]) outf(&w, "packet loss: mean %.1f %%\n", l->avg_loss * 100);
    if (l->have[CAT_LOG_CAN]) outf(&w, "CAN utilisation: peak %.0f %%\n", l->max_can * 100);
    if (l->have[CAT_LOG_CPU]) outf(&w, "CPU: peak %.0f %%\n", l->max_cpu * 100);
    outf(&w, "loop overruns: %d", l->overruns);
    if (l->max_loop_ms > 0) outf(&w, " (worst loop %.1f ms)", l->max_loop_ms);
    outf(&w, "\nCAN faults: %d; mode changes: %d; state changes: %d; health checks fired: %d\n", l->can_faults,
         l->mode_changes, l->state_changes, l->health_firings);
    outf(&w, "events: %d seen (%d errors, %d warnings); %d kept below\n", l->events_seen, l->errors, l->warnings,
         l->nevents);

    /* each series in SLICES slices: battery the lowest of a slice (the dips matter), the rest the highest */
    static const char *const SN[CAT_LOG_SERIES] = { "battery V", "CPU %", "CAN %", "trip ms", "packet loss %" };
    static const float SCALE[CAT_LOG_SERIES] = { 1, 100, 100, 1, 100 };
    for (int s = 0; s < CAT_LOG_SERIES; s++) {
        if (!l->have[s]) continue;
        outf(&w, "%s in %d slices of the log, the %s of each: ", SN[s], SLICES, s == CAT_LOG_BATTERY ? "lowest" : "highest");
        for (int k = 0; k < SLICES; k++) {
            float v = NAN;
            for (int i = k * CAT_LOG_POINTS / SLICES; i < (k + 1) * CAT_LOG_POINTS / SLICES; i++) {
                float x = l->series[s][i];
                if (!(x == x)) continue;
                if (!(v == v) || (s == CAT_LOG_BATTERY ? x < v : x > v)) v = x;
            }
            if (v == v) outf(&w, s == CAT_LOG_BATTERY ? "%.1f " : "%.0f ", v * SCALE[s]);
            else outf(&w, "- ");
        }
        outf(&w, "\n");
    }
    if (l->nevents) {
        outf(&w, "events (seconds from the start; E error, W warning, I info):\n");
        for (int i = 0; i < l->nevents; i++) {
            const cat_log_event_t *e = &l->events[i];
            outf(&w, "%7.1f %c %s\n", e->t, e->level == 2 ? 'E' : e->level == 1 ? 'W' : 'I', e->text);
        }
    }
    return w.o;
}

#define CSV_COLS (CAT_REC_COLS + 2)
#define CSV_LINE 8192
#define CSV_SLICES 12
#define CSV_MARKS 24

typedef struct {
    char name[44];
    uint32_t n;
    double min, max, tmin, tmax, sum, last;
    double ssum[CSV_SLICES];
    uint32_t sn[CSV_SLICES];
} csv_col_t;

/* the header's fields, unquoted, into cols[].name; returns how many */
static int csv_names(const char *line, csv_col_t *cols, int max)
{
    int k = 0;
    const char *p = line;
    while (*p && *p != '\n' && *p != '\r' && k < max) {
        char *o = cols[k].name;
        size_t on = 0;
        bool q = *p == '"';
        if (q) p++;
        for (; *p; p++) {
            if (q && *p == '"') {
                if (p[1] == '"') {
                    p++;
                } else {
                    q = false;
                    continue;
                }
            } else if (!q && (*p == ',' || *p == '\n' || *p == '\r')) {
                break;
            }
            if (on + 1 < sizeof cols[k].name) o[on++] = *p;
        }
        o[on] = 0;
        k++;
        if (*p == ',') p++;
        else break;
    }
    return k;
}

bool cat_csv_digest(const char *path, char *out, size_t n)
{
    if (!n) return false;
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, n, "can't open the file");
        return false;
    }
    /* the length first, from the last complete row, so the trend's slices are known before the pass */
    double dur = 0;
    char tail[512];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof tail - 1 ? size - (long)(sizeof tail - 1) : 0;
    if (fseek(f, from, SEEK_SET) == 0) {
        size_t k = fread(tail, 1, sizeof tail - 1, f);
        if (!cat_csv_last_t(tail, k, &dur)) dur = 0;
    }
    fseek(f, 0, SEEK_SET);
    char *line = malloc(CSV_LINE);
    csv_col_t *cols = calloc(CSV_COLS, sizeof *cols);
    if (!line || !cols) {
        free(line);
        free(cols);
        fclose(f);
        snprintf(out, n, "out of memory");
        return false;
    }
    int ncols = fgets(line, CSV_LINE, f) ? csv_names(line, cols, CSV_COLS) : 0;
    if (ncols < 3 || strcmp(cols[0].name, "t")) {
        free(line);
        free(cols);
        fclose(f);
        snprintf(out, n, "not a recorder run (no t,mark header)");
        return false;
    }
    for (int c = 0; c < ncols; c++) cols[c].min = INFINITY, cols[c].max = -INFINITY;
    double marks[CSV_MARKS];
    int nmarks = 0, allmarks = 0;
    uint32_t rows = 0;
    while (fgets(line, CSV_LINE, f)) {
        const char *p = line;
        char *e;
        double t = strtod(p, &e);
        if (e == p) continue;
        rows++;
        int slice = dur > 0 ? (int)(t / dur * CSV_SLICES) : 0;
        if (slice < 0) slice = 0;
        if (slice >= CSV_SLICES) slice = CSV_SLICES - 1;
        p = e;
        for (int c = 1; c < ncols && *p == ','; c++) {
            p++;
            double v = strtod(p, &e);
            if (e == p) continue; /* an empty field: absent, never 0 */
            p = e;
            if (c == 1) {
                allmarks++;
                if (nmarks < CSV_MARKS) marks[nmarks++] = t;
                continue;
            }
            csv_col_t *k = &cols[c];
            k->n++;
            if (v < k->min) k->min = v, k->tmin = t;
            if (v > k->max) k->max = v, k->tmax = t;
            k->sum += v;
            k->last = v;
            k->ssum[slice] += v;
            k->sn[slice]++;
        }
    }
    fclose(f);
    free(line);

    out_t w = { out, n, 0 };
    const char *base = strrchr(path, '/');
    char d[16];
    clock_len(d, sizeof d, dur);
    outf(&w, "file: %s (a Catalyst Tab run recording: the robot's NetworkTables topics sampled at %d Hz)\n",
         base ? base + 1 : path, CAT_REC_HZ);
    outf(&w, "length: %s, %u rows, %d columns\n", d, (unsigned)rows, ncols - 2);
    if (allmarks) {
        outf(&w, "marks the technician set (s): ");
        for (int i = 0; i < nmarks; i++) outf(&w, "%.1f ", marks[i]);
        if (allmarks > nmarks) outf(&w, "(+%d more)", allmarks - nmarks);
        outf(&w, "\n");
    }
    outf(&w, "per column: samples, min @time, max @time, mean, last; then the mean of each 1/%d of the run\n", CSV_SLICES);
    for (int c = 2; c < ncols; c++) {
        const csv_col_t *k = &cols[c];
        if (!k->n) {
            outf(&w, "%s: no samples\n", k->name);
            continue;
        }
        outf(&w, "%s: n %u, min %.4g @%.1fs, max %.4g @%.1fs, mean %.4g, last %.4g; trend:", k->name, (unsigned)k->n,
             k->min, k->tmin, k->max, k->tmax, k->sum / k->n, k->last);
        for (int s = 0; s < CSV_SLICES; s++) {
            if (k->sn[s]) outf(&w, " %.3g", k->ssum[s] / k->sn[s]);
            else outf(&w, " -");
        }
        outf(&w, "\n");
    }
    free(cols);
    return true;
}
