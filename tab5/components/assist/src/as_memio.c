/* as_memio — the pools kept: see as_memio.h. */
#include "as_memio.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "as_json.h"
#include "assist.h"
#include "hal.h"

#define SAVE_RETRY_S 2.0
#define SAVE_TRIES 30          /* a minute of retries, then every 30 s */
#define LOAD_RETRY_S 2.0
#define FILE_MAX (256 * 1024)
#define BUDGET_TECH 3200       /* characters of memories in front of the technician */
#define BUDGET_COMP 2000       /* ... and the companion (spoken: its prompt is shorter-lived) */

static const char *const FILE_NAME[AS_MEM_POOLS] = { "MEM-ASSIST.JSON", "MEM-COMPANION.JSON" };
static const char *const KV_KEY[AS_MEM_POOLS] = { "mem_tech", "mem_comp" };

typedef enum { PS_NONE, PS_CARD, PS_KV, PS_FAILED } ps_t;

typedef struct {
    as_mem_t *m;
    ps_t state;
    bool dirty, saving, fresh;  /* fresh: started empty because no file was found (never written over one) */
    double retry_at;
    int tries;
    uint32_t gen;
    char note[96];              /* for the settings: a problem worth saying */
    char *kv_seed;              /* the kv copy, read on the UI thread at start-up */
} pool_t;

static struct {
    pthread_mutex_t lock;
    pool_t p[AS_MEM_POOLS];
    uint32_t gen;
} M = { .lock = PTHREAD_MUTEX_INITIALIZER };

long long as_mem_now(void)
{
    time_t t = time(NULL);
    return t > 1609459200 ? (long long)t : 0; /* before 2021: the clock was never set */
}

static bool path_of(int pool, const char *ext, char *out, size_t n)
{
    const char *sd = hal_sd_root();
    if (!sd) return false;
    if (!ext) snprintf(out, n, "%s/CATOS/DATA/%s", sd, FILE_NAME[pool]);
    else {
        char stem[40];
        snprintf(stem, sizeof stem, "%s", FILE_NAME[pool]);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        snprintf(out, n, "%s/CATOS/DATA/%s.%s", sd, stem, ext);
    }
    return true;
}

/* the whole file, NUL-terminated, or NULL (errno from the failing call) */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, FILE_MAX, f) : 0;
    bool err = ferror(f);
    fclose(f);
    if (!buf || err) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

static bool write_file(const char *path, const char *tmp, const char *data, size_t len)
{
    FILE *f = fopen(tmp, "wb");
    if (!f && errno == ENOENT) {
        /* the folders aren't there yet (a new card) */
        char dir[160];
        const char *sd = hal_sd_root();
        if (sd) {
            snprintf(dir, sizeof dir, "%s/CATOS", sd);
            mkdir(dir, 0775);
            snprintf(dir, sizeof dir, "%s/CATOS/DATA", sd);
            mkdir(dir, 0775);
        }
        f = fopen(tmp, "wb");
    }
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path); /* FAT won't rename over a file */
    return rename(tmp, path) == 0;
}

/* the folder answers: a missing file is really missing, not a card that hasn't woken */
static bool data_dir_readable(void)
{
    const char *sd = hal_sd_root();
    if (!sd) return false;
    char dir[160];
    snprintf(dir, sizeof dir, "%s/CATOS/DATA", sd);
    DIR *d = opendir(dir);
    if (d) {
        closedir(d);
        return true;
    }
    if (errno != ENOENT) return false;
    /* no DATA folder at all: fine if the card's root answers */
    d = opendir(sd);
    if (!d) return false;
    closedir(d);
    return true;
}

static void seed_from_kv(pool_t *p)
{
    if (p->kv_seed && p->kv_seed[0]) {
        if (!as_mem_from_json(p->m, p->kv_seed, strlen(p->kv_seed))) as_mem_init(p->m, p->m->pool);
    }
}

/* Loads pool `i` (lock held). */
static void load(int i)
{
    pool_t *p = &M.p[i];
    if (!p->m) {
        p->m = malloc(sizeof *p->m);
        if (!p->m) {
            p->state = PS_FAILED;
            p->retry_at = hal_seconds() + LOAD_RETRY_S;
            snprintf(p->note, sizeof p->note, "out of memory");
            return;
        }
    }
    as_mem_init(p->m, i);
    p->fresh = false;
    char path[160], tmp[160], bad[160];
    if (!path_of(i, NULL, path, sizeof path)) {
        /* no card: kv holds it */
        seed_from_kv(p);
        p->state = PS_KV;
        p->note[0] = 0;
        return;
    }
    path_of(i, "TMP", tmp, sizeof tmp);
    path_of(i, "BAD", bad, sizeof bad);
    struct stat st;
    const char *from = stat(path, &st) == 0 ? path : stat(tmp, &st) == 0 ? tmp : NULL;
    if (!from) {
        if (errno != ENOENT || !data_dir_readable()) {
            p->state = PS_FAILED;
            p->retry_at = hal_seconds() + LOAD_RETRY_S;
            snprintf(p->note, sizeof p->note, "the card isn't answering");
            return;
        }
        /* truly new: what kv kept while there was no card moves onto it */
        seed_from_kv(p);
        p->fresh = true;
        p->state = PS_CARD;
        p->dirty = p->m->n > 0 || p->m->self[0][0] || p->m->self[1][0];
        p->note[0] = 0;
        return;
    }
    size_t len = 0;
    char *js = read_file(from, &len);
    if (!js) {
        p->state = PS_FAILED;
        p->retry_at = hal_seconds() + LOAD_RETRY_S;
        snprintf(p->note, sizeof p->note, "the card didn't read");
        return;
    }
    if (!as_mem_from_json(p->m, js, len)) {
        /* not a pool: kept aside, never written over */
        printf("memory: %s isn't readable: kept as %s\n", from, bad);
        unlink(bad);
        rename(from, bad);
        as_mem_init(p->m, i);
        snprintf(p->note, sizeof p->note, "the old file was unreadable: kept as %s", strrchr(bad, '/') + 1);
        p->fresh = true;
    } else {
        p->note[0] = 0;
        if (from == tmp) p->dirty = true; /* recovered from a write cut short: put it back in place */
    }
    free(js);
    p->state = PS_CARD;
}

as_mem_t *as_pool_get(int pool, char *why, size_t wn)
{
    if (pool < 0 || pool >= AS_MEM_POOLS) {
        snprintf(why, wn, "no such memory");
        return NULL;
    }
    pthread_mutex_lock(&M.lock);
    pool_t *p = &M.p[pool];
    if (p->state == PS_NONE || (p->state == PS_FAILED && hal_seconds() >= p->retry_at)) load(pool);
    if (p->state == PS_CARD || p->state == PS_KV) return p->m;
    snprintf(why, wn, "memory can't be read right now (%s): try again in a moment", p->note[0] ? p->note : "the card");
    pthread_mutex_unlock(&M.lock);
    return NULL;
}

static void save_card(int pool);

void as_pool_put(int pool, bool changed)
{
    pool_t *p = &M.p[pool];
    bool card = false;
    if (changed) {
        p->gen++;
        M.gen++;
        p->dirty = true;
        p->tries = 0;
        p->retry_at = 0;
        card = p->state == PS_CARD;
    }
    pthread_mutex_unlock(&M.lock);
    if (card) save_card(pool);
}

/* Writes pool `pool` to the card from this thread, when it is waiting and nobody else is writing it. */
static void save_card(int pool)
{
    pool_t *p = &M.p[pool];
    char path[160], tmp[160];
    if (!path_of(pool, NULL, path, sizeof path) || !path_of(pool, "TMP", tmp, sizeof tmp)) return;
    pthread_mutex_lock(&M.lock);
    if (p->state != PS_CARD || !p->dirty || p->saving) {
        pthread_mutex_unlock(&M.lock);
        return;
    }
    if (p->fresh) {
        /* started empty because no file was found: if one has appeared since (a card slow to wake), its
         * memories are read and this pool's joined to them, rather than an empty pool written over them */
        struct stat st;
        if (stat(path, &st) == 0) {
            as_mem_t *mine = p->m;
            p->m = NULL;
            p->state = PS_NONE;
            load(pool);
            as_mem_res_t *res = malloc(sizeof *res);
            if (p->state == PS_CARD && p->m && mine && res) {
                for (int k = 0; k < mine->n; k++)
                    as_mem_add(p->m, mine->it[k].kind, mine->it[k].text, mine->it[k].imp, mine->it[k].updated, res);
                for (int k = 0; k < AS_MEM_SELF_N; k++)
                    if (!p->m->self[k][0] && mine->self[k][0]) {
                        char w[8];
                        as_mem_set_self(p->m, as_mem_self_key(pool, k), mine->self[k], mine->self_at[k], w, sizeof w);
                    }
                p->dirty = true;
            }
            free(res);
            free(mine);
            if (p->state != PS_CARD) {
                pthread_mutex_unlock(&M.lock);
                return;
            }
        }
        p->fresh = false;
    }
    size_t len = 0;
    char *js = as_mem_to_json(p->m, 0, &len);
    uint32_t gen = p->gen;
    p->saving = js != NULL;
    pthread_mutex_unlock(&M.lock);
    if (!js) return;
    bool ok = write_file(path, tmp, js, len);
    free(js);
    pthread_mutex_lock(&M.lock);
    p->saving = false;
    if (ok) {
        if (p->gen == gen) p->dirty = false;
        p->tries = 0;
        p->note[0] = 0;
    } else {
        p->tries++;
        p->retry_at = hal_seconds() + (p->tries < SAVE_TRIES ? SAVE_RETRY_S : 30.0);
        if (p->tries == SAVE_TRIES) snprintf(p->note, sizeof p->note, "couldn't write %s to the card", FILE_NAME[pool]);
        printf("memory: writing %s failed (try %d)\n", FILE_NAME[pool], p->tries);
    }
    pthread_mutex_unlock(&M.lock);
}

void as_memio_boot(void)
{
    char *v = malloc(4096);
    for (int i = 0; i < AS_MEM_POOLS; i++) {
        if (v && hal_kv_get(KV_KEY[i], v, 4096) && v[0]) {
            pthread_mutex_lock(&M.lock);
            free(M.p[i].kv_seed);
            M.p[i].kv_seed = as_strdup(v);
            pthread_mutex_unlock(&M.lock);
        }
    }
    free(v);
    if (!hal_sd_root()) {
        /* no card: the pools are in kv, and this is the thread allowed to have read it */
        for (int i = 0; i < AS_MEM_POOLS; i++) {
            char why[8];
            if (as_pool_get(i, why, sizeof why)) as_pool_put(i, false);
        }
    }
}

void assist_mem_tick(void)
{
    double now = hal_seconds();
    for (int i = 0; i < AS_MEM_POOLS; i++) {
        pool_t *p = &M.p[i];
        pthread_mutex_lock(&M.lock);
        bool card = p->state == PS_CARD && p->dirty && !p->saving && now >= p->retry_at;
        char *kv = NULL;
        if (p->state == PS_KV && p->dirty && p->m) {
            size_t len = 0;
            kv = as_mem_to_json(p->m, AS_MEM_KV_MAX, &len);
            p->dirty = false;
        }
        pthread_mutex_unlock(&M.lock);
        if (card) save_card(i);
        if (kv) {
            hal_kv_set(KV_KEY[i], kv); /* the UI thread: NVS is safe here */
            pthread_mutex_lock(&M.lock);
            free(p->kv_seed);
            p->kv_seed = kv;
            pthread_mutex_unlock(&M.lock);
        }
    }
}

char *as_memio_digest(int pool, const char *query)
{
    char why[160];
    size_t cap = 12 * 1024;
    char *out = malloc(cap);
    if (!out) return NULL;
    as_mem_t *m = as_pool_get(pool, why, sizeof why);
    if (!m) {
        snprintf(out, cap,
                 "[Your long-term memory: it couldn't be read just now (%s). remember and recall will say when "
                 "it is back; don't assume you have no history.]",
                 why);
        return out;
    }
    int k = snprintf(out, cap,
                     "[Your long-term memory, written by you in earlier conversations and yours alone (%d memor%s in "
                     "all).\n",
                     m->n, m->n == 1 ? "y" : "ies");
    size_t n = as_mem_digest(m, query, as_mem_now(), pool == AS_MEM_ASSIST ? BUDGET_TECH : BUDGET_COMP, out + k,
                             cap - (size_t)k - 2);
    as_pool_put(pool, false);
    size_t l = (size_t)k + n;
    while (l && out[l - 1] == '\n') l--;
    snprintf(out + l, cap - l, "]");
    return out;
}

/* ---------------------------------------------------------------- the screens' side (assist.h) */

char *assist_mem_digest(as_pool_t pool, const char *query) { return as_memio_digest((int)pool, query); }

uint32_t assist_mem_gen(void)
{
    pthread_mutex_lock(&M.lock);
    uint32_t g = M.gen;
    pthread_mutex_unlock(&M.lock);
    return g;
}

typedef struct {
    int i;
    double s;
} rk_t;

static int by_s(const void *a, const void *b)
{
    double x = ((const rk_t *)a)->s, y = ((const rk_t *)b)->s;
    return x < y ? 1 : x > y ? -1 : 0;
}

int assist_mem_list(as_pool_t pool, as_memory_t *out, int max)
{
    char why[160];
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) return -1;
    rk_t *r = malloc(sizeof *r * (size_t)(m->n ? m->n : 1));
    int k = 0;
    if (r) {
        long long now = as_mem_now();
        for (int i = 0; i < m->n; i++) {
            r[i].i = i;
            r[i].s = as_mem_score(&m->it[i], now);
        }
        qsort(r, (size_t)m->n, sizeof *r, by_s);
        for (; k < m->n && k < max; k++) {
            const as_mem_item_t *it = &m->it[r[k].i];
            out[k].id = it->id;
            snprintf(out[k].kind, sizeof out[k].kind, "%s", it->kind);
            snprintf(out[k].text, sizeof out[k].text, "%s", it->text);
            out[k].imp = it->imp;
            out[k].updated = it->updated ? it->updated : it->created;
        }
        free(r);
    }
    as_pool_put((int)pool, false);
    return k;
}

bool assist_mem_self(as_pool_t pool, int i, char *key, size_t kn, char *text, size_t tn)
{
    const char *k = as_mem_self_key((int)pool, i);
    if (!k) return false;
    if (kn) snprintf(key, kn, "%s", k);
    char why[160];
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) {
        if (tn) text[0] = 0;
        return false;
    }
    if (tn) snprintf(text, tn, "%s", m->self[i]);
    as_pool_put((int)pool, false);
    return true;
}

bool assist_mem_forget(as_pool_t pool, int id)
{
    char why[160];
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) return false;
    bool ok = as_mem_forget(m, id);
    as_pool_put((int)pool, ok);
    return ok;
}

bool assist_mem_clear_self(as_pool_t pool, int i)
{
    char why[160];
    const char *k = as_mem_self_key((int)pool, i);
    if (!k) return false;
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) return false;
    bool had = m->self[i][0] != 0;
    if (had) as_mem_set_self(m, k, "", as_mem_now(), why, sizeof why);
    as_pool_put((int)pool, had);
    return had;
}

void assist_mem_forget_all(as_pool_t pool)
{
    char why[160];
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) return;
    as_mem_clear(m);
    m->updated = as_mem_now();
    as_pool_put((int)pool, true);
}

void assist_mem_status(as_pool_t pool, char *out, size_t n)
{
    char why[160];
    as_mem_t *m = as_pool_get((int)pool, why, sizeof why);
    if (!m) {
        snprintf(out, n, "%s", why);
        return;
    }
    pool_t *p = &M.p[pool];
    int self = 0;
    for (int i = 0; i < AS_MEM_SELF_N; i++) self += m->self[i][0] != 0;
    snprintf(out, n, "%d memor%s of %d · %s%s%s", m->n, m->n == 1 ? "y" : "ies", AS_MEM_MAX,
             p->state == PS_KV ? "no card: kept in the tablet (the most important only)"
                               : p->dirty ? "saving to the card" : "on the card, CATOS/DATA",
             p->note[0] ? " · " : "", p->note);
    (void)self;
    as_pool_put((int)pool, false);
}
