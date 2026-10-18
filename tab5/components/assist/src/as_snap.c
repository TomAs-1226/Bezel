/* as_snap — tunable snapshots, kept in a ring in memory and on microSD at <sd>/link/snapshots.json. */
#include "as_snap.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "as_json.h"
#include "hal.h"

typedef struct {
    char key[96];
    double value;
    bool is_bool;
} snap_val_t;

typedef struct {
    snap_info_t info;
    snap_val_t v[CAT_MAX_TUNABLES];
} snap_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static snap_t *g_snap;     /* SNAP_MAX, oldest first; PSRAM on the tablet */
static int g_n, g_next = 1;
static bool g_loaded;

static bool path(char *out, size_t n, bool dir)
{
    const char *root = hal_sd_root();
    if (!root) return false;
    if (dir) snprintf(out, n, "%s/link", root);
    else snprintf(out, n, "%s/link/snapshots.json", root);
    return true;
}

static bool alloc_locked(void)
{
    if (!g_snap) g_snap = calloc(SNAP_MAX, sizeof *g_snap);
    return g_snap != NULL;
}

static void save_locked(void)
{
    char dir[160], file[180], tmp[190];
    if (!path(dir, sizeof dir, true) || !path(file, sizeof file, false)) return;
    mkdir(dir, 0755);
    ab_t b;
    ab_init(&b);
    ab_fmt(&b, "{\"next\":%d,\"snapshots\":[", g_next);
    for (int i = 0; i < g_n; i++) {
        const snap_t *s = &g_snap[i];
        if (i) ab_puts(&b, ",");
        ab_fmt(&b, "\n{\"id\":%d,\"t\":%.3f,\"when\":", s->info.id, s->info.t);
        ab_str(&b, s->info.when);
        ab_puts(&b, ",\"reason\":");
        ab_str(&b, s->info.reason);
        ab_puts(&b, ",\"values\":[");
        for (int k = 0; k < s->info.count; k++) {
            if (k) ab_puts(&b, ",");
            ab_puts(&b, "{\"key\":");
            ab_str(&b, s->v[k].key);
            ab_puts(&b, ",\"value\":");
            ab_num(&b, s->v[k].value);
            if (s->v[k].is_bool) ab_puts(&b, ",\"bool\":true");
            ab_puts(&b, "}");
        }
        ab_puts(&b, "]}");
    }
    ab_puts(&b, "]}\n");
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    if (f && !b.oom) {
        bool ok = fwrite(b.p, 1, b.n, f) == b.n;
        ok = fclose(f) == 0 && ok;
        /* FAT won't rename over an existing file */
        if (ok) {
            remove(file);
            rename(tmp, file);
        }
    } else if (f) {
        fclose(f);
    }
    ab_free(&b);
}

static void load_locked(void)
{
    char file[180];
    g_loaded = true;
    if (!alloc_locked() || !path(file, sizeof file, false)) return;
    FILE *f = fopen(file, "r");
    if (!f) return;
    ab_t b;
    ab_init(&b);
    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) ab_raw(&b, chunk, r);
    fclose(f);
    aj_t *d = b.p ? aj_parse(b.p, b.n, NULL, 0) : NULL;
    ab_free(&b);
    if (!d) return;
    g_n = 0;
    const aj_t *list = aj_get(d, "snapshots");
    int start = list && list->n > SNAP_MAX ? list->n - SNAP_MAX : 0;
    for (int i = start; list && i < list->n; i++) {
        const aj_t *s = list->kid[i];
        snap_t *o = &g_snap[g_n];
        memset(o, 0, sizeof *o);
        o->info.id = (int)aj_getn(s, "id", 0);
        o->info.t = aj_getn(s, "t", 0);
        snprintf(o->info.when, sizeof o->info.when, "%s", aj_gets(s, "when") ? aj_gets(s, "when") : "");
        snprintf(o->info.reason, sizeof o->info.reason, "%s", aj_gets(s, "reason") ? aj_gets(s, "reason") : "");
        const aj_t *vals = aj_get(s, "values");
        for (int k = 0; vals && k < vals->n && o->info.count < CAT_MAX_TUNABLES; k++) {
            const char *key = aj_gets(vals->kid[k], "key");
            if (!key) continue;
            snap_val_t *v = &o->v[o->info.count++];
            snprintf(v->key, sizeof v->key, "%s", key);
            v->value = aj_getn(vals->kid[k], "value", NAN);
            v->is_bool = aj_is(aj_get(vals->kid[k], "bool"), AJ_TRUE);
        }
        if (o->info.id > 0) g_n++;
    }
    g_next = (int)aj_getn(d, "next", 1);
    for (int i = 0; i < g_n; i++) if (g_snap[i].info.id >= g_next) g_next = g_snap[i].info.id + 1;
    aj_free(d);
}

void snap_init(void)
{
    pthread_mutex_lock(&g_lock);
    load_locked();
    pthread_mutex_unlock(&g_lock);
}

static void ensure_locked(void)
{
    if (!g_loaded) load_locked();
}

static const snap_t *find_locked(int id)
{
    for (int i = 0; i < g_n; i++) if (g_snap[i].info.id == id) return &g_snap[i];
    return NULL;
}

int snap_take(const cat_robot_t *r, const char *reason)
{
    if (!r || !r->connected || r->ntunables == 0) return -1;
    pthread_mutex_lock(&g_lock);
    ensure_locked();
    if (!alloc_locked()) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int have = 0;
    for (int i = 0; i < r->ntunables; i++) have += r->tunables[i].have;
    if (!have) { /* a value the robot never published can't be put back */
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    if (g_n == SNAP_MAX) {
        memmove(g_snap, g_snap + 1, (SNAP_MAX - 1) * sizeof *g_snap);
        g_n--;
    }
    /* filled in place: a snapshot is a few KB, too much for a UI task's stack */
    snap_t *s = &g_snap[g_n++];
    memset(s, 0, sizeof *s);
    for (int i = 0; i < r->ntunables; i++) {
        const cat_tunable_t *t = &r->tunables[i];
        if (!t->have) continue;
        snap_val_t *v = &s->v[s->info.count++];
        snprintf(v->key, sizeof v->key, "%s", t->key);
        v->value = t->value;
        v->is_bool = t->is_bool;
    }
    s->info.id = g_next++;
    s->info.t = hal_seconds();
    struct tm tm;
    if (hal_rtc_get(&tm)) strftime(s->info.when, sizeof s->info.when, "%H:%M:%S", &tm);
    snprintf(s->info.reason, sizeof s->info.reason, "%s", reason ? reason : "");
    int id = s->info.id;
    save_locked();
    pthread_mutex_unlock(&g_lock);
    return id;
}

static snap_t *copy_locked(int id)
{
    const snap_t *s = find_locked(id);
    snap_t *c = s ? malloc(sizeof *c) : NULL;
    if (c) *c = *s;
    return c;
}

static int index_of(const cat_robot_t *r, const char *key)
{
    for (int i = 0; i < r->ntunables; i++) if (!strcmp(r->tunables[i].key, key)) return i;
    return -1;
}

bool snap_revert(const cat_robot_t *r, int id)
{
    if (!r || !r->connected) return false;
    pthread_mutex_lock(&g_lock);
    ensure_locked();
    snap_t *copy = copy_locked(id);
    pthread_mutex_unlock(&g_lock);
    if (!copy) return false;
    int written = 0;
    for (int k = 0; k < copy->info.count; k++) {
        int i = index_of(r, copy->v[k].key);
        if (i < 0 || copy->v[k].value != copy->v[k].value) continue;
        /* only what differs: a revert shouldn't rewrite values nobody changed */
        const cat_tunable_t *t = &r->tunables[i];
        if (t->have && fabs(t->value - copy->v[k].value) <= 1e-9 * fmax(1, fabs(copy->v[k].value))) {
            written++;
            continue;
        }
        if (cat_set_tunable(r, i, copy->v[k].value)) written++;
    }
    free(copy);
    return written > 0;
}

int snap_list(snap_info_t *out, int max)
{
    pthread_mutex_lock(&g_lock);
    ensure_locked();
    int n = 0;
    for (int i = g_n - 1; i >= 0 && n < max; i--) out[n++] = g_snap[i].info;
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void fmt_val(char *out, size_t n, double v, bool is_bool)
{
    if (is_bool) snprintf(out, n, "%s", v != 0 ? "true" : "false");
    else if (v != v) snprintf(out, n, "—");
    else snprintf(out, n, "%g", v);
}

int snap_diff(const cat_robot_t *r, int id, char *out, int max)
{
    if (max > 0) out[0] = 0;
    pthread_mutex_lock(&g_lock);
    ensure_locked();
    snap_t *copy = copy_locked(id);
    pthread_mutex_unlock(&g_lock);
    if (!copy) return -1;
    int lines = 0, len = 0;
    for (int k = 0; k < copy->info.count; k++) {
        const snap_val_t *v = &copy->v[k];
        int i = r ? index_of(r, v->key) : -1;
        bool have = i >= 0 && r->tunables[i].have;
        double now = have ? r->tunables[i].value : NAN;
        if (have && fabs(now - v->value) <= 1e-9 * fmax(1, fabs(v->value))) continue;
        char a[32], b[32];
        fmt_val(a, sizeof a, v->value, v->is_bool);
        fmt_val(b, sizeof b, now, v->is_bool);
        if (max > len) {
            int w = snprintf(out + len, (size_t)(max - len), "%s%s  %s → %s", lines ? "\n" : "", v->key, a, b);
            len = w < max - len ? len + w : max - 1;
        }
        lines++;
    }
    free(copy);
    return lines;
}
