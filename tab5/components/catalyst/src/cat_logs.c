#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cat_logs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    case CAT_LOG_BATTERY: if (!(o->min_battery == o->min_battery) || v < o->min_battery) o->min_battery = v; break;
    case CAT_LOG_TRIP: if (v > o->max_trip_ms) o->max_trip_ms = v; break;
    case CAT_LOG_CAN: if (v > o->max_can) o->max_can = v; break;
    case CAT_LOG_CPU: if (v > o->max_cpu) o->max_cpu = v; break;
    default: break;
    }
}

static void event(cat_log_t *o, double t, const char *text, size_t len)
{
    if (o->nevents >= CAT_LOG_EVENTS) return;
    cat_log_event_t *e = &o->events[o->nevents++];
    e->t = t;
    if (len >= sizeof e->text) len = sizeof e->text - 1;
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if ((unsigned char)c < 32) continue;
        e->text[k++] = c;
    }
    e->text[k] = 0;
    /* dslog.rs levels from the words */
    char low[100];
    for (size_t i = 0; i <= k; i++) low[i] = (char)tolower((unsigned char)e->text[i]);
    if (strstr(low, "error") || strstr(low, "estop") || strstr(low, "brownout")) e->level = 2;
    else if (strstr(low, "warning") || strstr(low, "lost") || strstr(low, "dropped") || strstr(low, "watchdog")) e->level = 1;
    else e->level = 0;
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

typedef struct {
    uint32_t id;
    int series;     /* cat_log_series_t, or -1 */
    bool event;     /* a string entry worth reading as events */
    bool is_int, is_float_arr, is_bool;
    bool brownout;
} entry_t;

static bool ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static void classify(entry_t *e, const char *name, const char *type)
{
    e->series = -1;
    e->event = false;
    e->is_int = !strcmp(type, "int64");
    e->is_float_arr = !strcmp(type, "float[]") || !strcmp(type, "double[]");
    e->is_bool = !strcmp(type, "boolean");
    e->brownout = false;
    if (ends(name, "/Battery") || ends(name, "BatteryVolts") || ends(name, "BatteryVoltage") || ends(name, "MeasuredVoltage"))
        e->series = CAT_LOG_BATTERY;
    else if (ends(name, "/PacketTime")) e->series = CAT_LOG_TRIP;
    else if (ends(name, "/CPU") || ends(name, "CpuPercent")) e->series = CAT_LOG_CPU;
    else if (ends(name, "CanBusUtilizations") || ends(name, "CanUtilization")) e->series = CAT_LOG_CAN;
    else if (ends(name, "BrownedOut") || ends(name, "Brownout")) e->brownout = e->is_bool;
    if (!strcmp(type, "string") &&
        (!strcmp(name, "messages") || strstr(name, "Message") || strstr(name, "Event") || strstr(name, "console")))
        e->event = true;
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
    for (int pass = 0; pass < 2; pass++) {
        size_t p = start;
        bool last_brown = false;
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
            if (e->series >= 0) {
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
            } else if (e->brownout && size >= 1) {
                bool b = pl[0] != 0;
                if (b && !last_brown) o->brownouts++;
                last_brown = b;
            } else if (e->event) {
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
        snprintf(o->error, sizeof o->error, "dslog version %u", ver);
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
        if (brown && !last_brown) o->brownouts++;
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
    if (len >= 6 && !memcmp(data, "WPILOG", 6)) return wpilog(data, len, out);
    if (ends(name, ".dsevents")) return dsevents(data, len, out);
    if (ends(name, ".dslog")) return dslog(data, len, out);
    init(out, "unknown");
    snprintf(out->error, sizeof out->error, "not a log this reads");
    return false;
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
