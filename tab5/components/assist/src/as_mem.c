/* as_mem — the memory model, pure: see as_mem.h. */
#include "as_mem.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "as_json.h"

static const char *const KINDS[] = { "fact", "preference", "decision", "fix", "fault", "procedure",
                                     "event", "person", "plan", NULL };

static const char *const SELF_KEYS[AS_MEM_POOLS][AS_MEM_SELF_N] = {
    { "robot", "team", "season", "watchlist" },
    { "name", "personality", "relationship", "running_jokes" },
};

const char *const *as_mem_kinds(void) { return KINDS; }

const char *as_mem_self_key(int pool, int i)
{
    if (pool < 0 || pool >= AS_MEM_POOLS || i < 0 || i >= AS_MEM_SELF_N) return NULL;
    return SELF_KEYS[pool][i];
}

int as_mem_self_index(int pool, const char *key)
{
    for (int i = 0; key && i < AS_MEM_SELF_N; i++)
        if (as_mem_self_key(pool, i) && !strcmp(as_mem_self_key(pool, i), key)) return i;
    return -1;
}

const char *as_mem_pool_name(int pool) { return pool == AS_MEM_COMPANION ? "companion" : "technician"; }

void as_mem_init(as_mem_t *m, int pool)
{
    memset(m, 0, sizeof *m);
    m->pool = pool;
    m->next_id = 1;
}

/* ---- text ---- */

/* copies at most `cap - 1` bytes, never cutting a UTF-8 character; control characters become spaces */
static void put_text(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src ? src : "");
    if (n > cap - 1) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    for (size_t i = 0; i < n; i++) dst[i] = (unsigned char)src[i] < 0x20 ? ' ' : src[i];
    dst[n] = 0;
    as_fold_text(dst); /* curly quotes and the like: the faces that show memories don't carry them */
    /* no leading or trailing blanks */
    size_t a = 0;
    while (dst[a] == ' ') a++;
    if (a) memmove(dst, dst + a, strlen(dst + a) + 1);
    for (size_t l = strlen(dst); l && dst[l - 1] == ' '; l--) dst[l - 1] = 0;
}

static const char *kind_of(const char *k)
{
    for (int i = 0; k && KINDS[i]; i++)
        if (!strcmp(KINDS[i], k)) return KINDS[i];
    return "fact";
}

static int clamp_imp(int imp) { return imp < 1 ? 1 : imp > 3 ? 3 : imp; }

void as_mem_date(int64_t t, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    if (t <= 0) return;
    time_t tt = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&tt, &tm)) return;
    strftime(out, n, "%Y-%m-%d", &tm);
}

/* ASCII case-insensitive, for the few English words matched below (strncasecmp isn't on every host) */
static int ci_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* ---- words: what the near-duplicate rule and search compare ---- */

#define MAX_WORDS 64
typedef struct {
    char w[MAX_WORDS][24];
    int n;
} words_t;

static bool stop_word(const char *w)
{
    static const char *const STOP[] = { "the", "and", "for", "with", "that", "this", "was", "were", "are", "is",
                                        "of", "to", "in", "on", "at", "it", "an", "be", "has", "have", "had", "our",
                                        "we", "you", "he", "she", "they", "but", "not", "from", "by", "as", "or",
                                        "if", "its", "it's", "so", "do", "does", "did", "my", "me", "his", "her",
                                        "their", "them", "there", "than", "then", "when", "what", "which", "who",
                                        "will", "would", "can", "could", "should", "about", "into", "also", "just",
                                        "very", "one", "all", "any", "some", "no", "yes", NULL };
    for (int i = 0; STOP[i]; i++)
        if (!strcmp(STOP[i], w)) return true;
    return false;
}

static void words(const char *s, words_t *out)
{
    out->n = 0;
    char cur[24];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : "");; p++) {
        bool word = *p && (isalnum(*p) || *p >= 0x80 || *p == '_');
        if (word) {
            if (k < sizeof cur - 1) cur[k++] = (char)tolower(*p);
            continue;
        }
        if (k >= 2 && out->n < MAX_WORDS) {
            cur[k] = 0;
            bool dup = false;
            for (int i = 0; i < out->n && !dup; i++) dup = !strcmp(out->w[i], cur);
            if (!dup && !stop_word(cur)) memcpy(out->w[out->n++], cur, k + 1);
        }
        k = 0;
        if (!*p) break;
    }
}

/* the same word, or one the other's stem ("brownout" / "brownouts", "jam" / "jammed") */
static bool word_match(const char *a, const char *b)
{
    if (!strcmp(a, b)) return true;
    size_t la = strlen(a), lb = strlen(b), m = la < lb ? la : lb;
    if (m < 4 || strncmp(a, b, m) != 0) return false;
    /* a stem takes letters ("jam" + "med"), never digits: "q1" is not "q12", "subject1" not "subject10" */
    for (const char *t = (la > lb ? a : b) + m; *t; t++)
        if (isdigit((unsigned char)*t)) return false;
    return true;
}

static int overlap(const words_t *a, const words_t *b)
{
    int k = 0;
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            if (word_match(a->w[i], b->w[j])) {
                k++;
                break;
            }
    return k;
}

/* near enough the same memory that the new one should replace it rather than sit beside it */
static bool same_memory(const char *x, const char *y)
{
    /* on the heap: this runs on the companion's thread, whose stack is small and internal */
    words_t *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    bool same = false;
    if (a && b) {
        words(x, a);
        words(y, b);
        if (!a->n || !b->n) same = strlen(x) == strlen(y) && !ci_ncmp(x, y, strlen(x));
        else {
            int k = overlap(a, b);
            int uni = a->n + b->n - k;
            same = uni > 0 && (double)k / uni >= 0.75 && (a->n >= 3 || a->n == b->n);
        }
    }
    free(a);
    free(b);
    return same;
}

/* ---- secrets ---- */

bool as_mem_secret(const char *text)
{
    if (!text) return false;
    static const char *const PREFIX[] = { "sk-", "ghp_", "gho_", "github_pat_", "xoxb-", "xoxp-", "AKIA", "AIza",
                                          "glpat-", "hf_", NULL };
    /* a well-known key's prefix with a long tail after it */
    for (int i = 0; PREFIX[i]; i++) {
        size_t lp = strlen(PREFIX[i]);
        for (const char *p = strstr(text, PREFIX[i]); p; p = strstr(p + 1, PREFIX[i])) {
            if (p > text && (isalnum((unsigned char)p[-1]))) continue;
            size_t k = 0;
            for (const char *q = p + lp; *q && (isalnum((unsigned char)*q) || *q == '-' || *q == '_'); q++) k++;
            if (k >= 12) return true;
        }
    }
    /* "password: ...", "the wifi password is ...", "api key = ...". Only a password's "is" gives it away:
     * "the Link token was wrong" is a fault worth remembering, and names no token. */
    static const char *const WORD[] = { "password", "passwd", "passcode", "pass code", "pin code", "api key",
                                        "apikey", "api_key", "secret key", "token", "bearer", NULL };
    for (int i = 0; WORD[i]; i++) {
        size_t lw = strlen(WORD[i]);
        bool said = i < 5; /* the password family: "is" and "was" count as well as ':' and '=' */
        for (const char *p = text; *p; p++) {
            if (ci_ncmp(p, WORD[i], lw)) continue;
            if (p > text && isalpha((unsigned char)p[-1])) continue; /* "passwordless" is fine, "mypassword" too */
            const char *q = p + lw;
            while (*q == 's') q++; /* "tokens", "passwords" */
            while (*q == ' ') q++;
            if (*q == ':' || *q == '=') {
                q++;
                while (*q == ' ' || *q == '"' || *q == '\'') q++;
                if (*q) return true;
            }
            if (said && (!ci_ncmp(q, "is ", 3) || !ci_ncmp(q, "was ", 4))) return true;
        }
    }
    /* a long run of letters and digits that is not a word: a key someone pasted without its name */
    const char *p = text;
    while (*p) {
        while (*p && !(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '+' || *p == '/' || *p == '='))
            p++;
        const char *s = p;
        int dig = 0, alpha = 0, up = 0, lo = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '+' || *p == '/' || *p == '=')) {
            unsigned char c = (unsigned char)*p;
            dig += isdigit(c) != 0;
            alpha += isalpha(c) != 0;
            up += isupper(c) != 0;
            lo += islower(c) != 0;
            p++;
        }
        size_t len = (size_t)(p - s);
        bool slashy = memchr(s, '/', len) != NULL;
        if (len >= 24 && dig >= 3 && alpha >= 3 && (!slashy || (up >= 3 && lo >= 3))) return true;
        if (len >= 32 && dig >= 6 && alpha >= 6) return true;
    }
    return false;
}

/* ---- value ---- */

double as_mem_score(const as_mem_item_t *it, int64_t now)
{
    double s = it->imp * 10.0;
    int64_t t = it->updated ? it->updated : it->created;
    if (now > 0 && t > 0) {
        double days = (double)(now - t) / 86400.0;
        if (days < 0) days = 0;
        s += 4.0 * exp(-days / 21.0); /* a three-week half-life, roughly: this season beats last */
    }
    s += (it->uses > 5 ? 5 : it->uses) * 0.4;
    return s;
}

int as_mem_find(const as_mem_t *m, int id)
{
    for (int i = 0; i < m->n; i++)
        if (m->it[i].id == id) return i;
    return -1;
}

static void res_refuse(as_mem_res_t *r, const char *why)
{
    r->id = 0;
    snprintf(r->why, sizeof r->why, "%s", why);
}

bool as_mem_add(as_mem_t *m, const char *kind, const char *text, int imp, int64_t now, as_mem_res_t *rp)
{
    as_mem_res_t *r = rp;
    memset(r, 0, sizeof *r);
    char clean[AS_MEM_TEXT];
    put_text(clean, sizeof clean, text);
    if (strlen(clean) < 4) {
        res_refuse(r, "nothing to remember: write the memory as a sentence");
        return r->id > 0;
    }
    if (as_mem_secret(clean)) {
        res_refuse(r, "that looks like a password, key or token: secrets are never kept in memory");
        return r->id > 0;
    }
    imp = clamp_imp(imp);
    /* the same thing said again: the old memory takes the new words (they are usually better) */
    for (int i = 0; i < m->n; i++) {
        if (!same_memory(m->it[i].text, clean)) continue;
        as_mem_item_t *it = &m->it[i];
        snprintf(it->text, sizeof it->text, "%s", clean);
        snprintf(it->kind, sizeof it->kind, "%s", kind_of(kind));
        if (imp > it->imp) it->imp = imp;
        it->updated = now;
        m->updated = now;
        r->id = it->id;
        r->merged = true;
        return r->id > 0;
    }
    if (m->n == AS_MEM_MAX) {
        /* the least valuable memory that matters no more than this one makes room */
        int worst = -1;
        double ws = 1e18;
        for (int i = 0; i < m->n; i++) {
            if (m->it[i].imp > imp) continue;
            double s = as_mem_score(&m->it[i], now);
            if (s < ws) {
                ws = s;
                worst = i;
            }
        }
        if (worst < 0) {
            res_refuse(r, "memory is full of more important things: forget something first, or save this as more important");
            return r->id > 0;
        }
        r->evicted = m->it[worst].id;
        snprintf(r->evicted_text, sizeof r->evicted_text, "%s", m->it[worst].text);
        memmove(&m->it[worst], &m->it[worst + 1], (size_t)(m->n - worst - 1) * sizeof m->it[0]);
        m->n--;
    }
    as_mem_item_t *it = &m->it[m->n++];
    memset(it, 0, sizeof *it);
    if (m->next_id < 1) m->next_id = 1;
    it->id = m->next_id++;
    snprintf(it->kind, sizeof it->kind, "%s", kind_of(kind));
    snprintf(it->text, sizeof it->text, "%s", clean);
    it->imp = imp;
    it->created = it->updated = now;
    m->updated = now;
    r->id = it->id;
    return r->id > 0;
}

bool as_mem_update(as_mem_t *m, int id, const char *kind, const char *text, int imp, int64_t now,
                   as_mem_res_t *rp)
{
    as_mem_res_t *r = rp;
    memset(r, 0, sizeof *r);
    int i = as_mem_find(m, id);
    if (i < 0) {
        snprintf(r->why, sizeof r->why, "there is no memory #%d (recall lists them)", id);
        return r->id > 0;
    }
    as_mem_item_t *it = &m->it[i];
    if (text) {
        char clean[AS_MEM_TEXT];
        put_text(clean, sizeof clean, text);
        if (strlen(clean) < 4) {
            res_refuse(r, "nothing to remember: write the memory as a sentence");
            return r->id > 0;
        }
        if (as_mem_secret(clean)) {
            res_refuse(r, "that looks like a password, key or token: secrets are never kept in memory");
            return r->id > 0;
        }
        snprintf(it->text, sizeof it->text, "%s", clean);
    }
    if (kind) snprintf(it->kind, sizeof it->kind, "%s", kind_of(kind));
    if (imp) it->imp = clamp_imp(imp);
    it->updated = now;
    m->updated = now;
    r->id = id;
    /* a rewrite that now says what another memory says: the other one goes, so there is one */
    for (int k = 0; k < m->n; k++) {
        if (k == i || !same_memory(m->it[k].text, m->it[i].text)) continue;
        if (m->it[k].imp > m->it[i].imp) m->it[i].imp = m->it[k].imp;
        r->evicted = m->it[k].id;
        snprintf(r->evicted_text, sizeof r->evicted_text, "%s", m->it[k].text);
        memmove(&m->it[k], &m->it[k + 1], (size_t)(m->n - k - 1) * sizeof m->it[0]);
        m->n--;
        break;
    }
    return r->id > 0;
}

bool as_mem_forget(as_mem_t *m, int id)
{
    int i = as_mem_find(m, id);
    if (i < 0) return false;
    memmove(&m->it[i], &m->it[i + 1], (size_t)(m->n - i - 1) * sizeof m->it[0]);
    m->n--;
    return true;
}

void as_mem_clear(as_mem_t *m)
{
    int pool = m->pool, next = m->next_id;
    as_mem_init(m, pool);
    m->next_id = next; /* ids are never reused: an old id in a transcript can't name a new memory */
}

bool as_mem_set_self(as_mem_t *m, const char *key, const char *text, int64_t now, char *why, size_t wn)
{
    int i = as_mem_self_index(m->pool, key);
    if (i < 0) {
        snprintf(why, wn, "no section called \"%.30s\" here", key ? key : "");
        return false;
    }
    char *clean = malloc(AS_MEM_SELF_TEXT);
    if (!clean) {
        snprintf(why, wn, "out of memory");
        return false;
    }
    put_text(clean, AS_MEM_SELF_TEXT, text);
    if (as_mem_secret(clean)) {
        free(clean);
        snprintf(why, wn, "that looks like it holds a password, key or token: secrets are never kept in memory");
        return false;
    }
    snprintf(m->self[i], sizeof m->self[i], "%s", clean);
    free(clean);
    m->self_at[i] = now;
    m->updated = now;
    return true;
}

/* ---- search ---- */

typedef struct {
    int i;
    double s;
} ranked_t;

static int by_score(const void *a, const void *b)
{
    double x = ((const ranked_t *)a)->s, y = ((const ranked_t *)b)->s;
    return x < y ? 1 : x > y ? -1 : ((const ranked_t *)a)->i - ((const ranked_t *)b)->i;
}

/* every memory ranked: value, plus relevance to the query's words (required when `strict`) */
static int rank(const as_mem_t *m, const char *query, const char *kind, bool strict, int64_t now, ranked_t *out)
{
    words_t *q = malloc(sizeof *q), *w = malloc(sizeof *w);
    int n = 0;
    if (!q || !w) {
        free(q);
        free(w);
        return 0;
    }
    words(query, q);
    for (int i = 0; i < m->n; i++) {
        const as_mem_item_t *it = &m->it[i];
        if (kind && kind[0] && strcmp(it->kind, kind) != 0) continue;
        double rel = 0;
        if (q->n) {
            words(it->text, w);
            int k = overlap(q, w);
            /* the kind's own name counts as a word of the memory ("fixes" finds the fixes) */
            for (int j = 0; j < q->n; j++)
                if (word_match(q->w[j], it->kind)) k++;
            rel = (double)k / q->n;
            if (strict && k == 0) continue;
        }
        out[n].i = i;
        out[n].s = as_mem_score(it, now) + rel * 30.0;
        n++;
    }
    free(q);
    free(w);
    qsort(out, (size_t)n, sizeof *out, by_score);
    return n;
}

int as_mem_search(const as_mem_t *m, const char *query, const char *kind, int *idx, int max, int64_t now)
{
    ranked_t *r = malloc(sizeof *r * (m->n ? m->n : 1));
    if (!r) return 0;
    int n = rank(m, query, kind, true, now, r);
    int k = n < max ? n : max;
    for (int i = 0; i < k; i++) idx[i] = r[i].i;
    free(r);
    return k;
}

/* ---- the digest ---- */

static size_t add(char *out, size_t n, size_t at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t add(char *out, size_t n, size_t at, const char *fmt, ...)
{
    if (at >= n) return at;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + at, n - at, fmt, ap);
    va_end(ap);
    if (k < 0) return at;
    return at + (size_t)k >= n ? n - 1 : at + (size_t)k;
}

static int line(const as_mem_item_t *it, char *out, size_t n)
{
    char d[16];
    as_mem_date(it->updated ? it->updated : it->created, d, sizeof d);
    return snprintf(out, n, "- #%d [%s, %s%s%s] %s\n", it->id, it->kind,
                    it->imp == 3 ? "important" : it->imp == 2 ? "useful" : "minor", d[0] ? ", " : "", d, it->text);
}

size_t as_mem_digest(const as_mem_t *m, const char *query, int64_t now, size_t budget, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    size_t at = 0;
    bool tech = m->pool == AS_MEM_ASSIST;
    at = add(out, n, at, "%s\n", tech ? "Your season notebook (you wrote it; update_notebook rewrites a section):"
                                      : "Who you are (you wrote this yourself; update_persona rewrites a field):");
    for (int i = 0; i < AS_MEM_SELF_N; i++) {
        const char *k = as_mem_self_key(m->pool, i);
        at = add(out, n, at, "- %s: %s\n", k, m->self[i][0] ? m->self[i] : "(empty so far)");
    }
    if (!m->n) {
        at = add(out, n, at, "Memories: none yet.\n");
        return at;
    }
    ranked_t *r = malloc(sizeof *r * (size_t)m->n);
    if (!r) return at;
    int nr = rank(m, query, NULL, false, now, r);
    char *ln = malloc(AS_MEM_TEXT + 96);
    size_t used = 0;
    int shown = 0;
    size_t head = at;
    at = add(out, n, at, "Memories, most important first:\n");
    for (int i = 0; ln && i < nr; i++) {
        int k = line(&m->it[r[i].i], ln, AS_MEM_TEXT + 96);
        if (k <= 0) continue;
        if (used + (size_t)k > budget || at + (size_t)k + 80 >= n) continue; /* a shorter one may still fit */
        at = add(out, n, at, "%s", ln);
        used += (size_t)k;
        shown++;
    }
    if (shown < m->n) at = add(out, n, at, "(%d more not shown: recall finds them by words or kind.)\n", m->n - shown);
    else if (shown == 0) {
        at = head;
        out[at] = 0;
    }
    free(ln);
    free(r);
    return at;
}

/* ---- JSON ---- */

bool as_mem_from_json(as_mem_t *m, const char *js, size_t len)
{
    aj_t *d = aj_parse(js, len, NULL, 0);
    if (!d || d->type != AJ_OBJ) {
        aj_free(d);
        return false;
    }
    int pool = m->pool;
    as_mem_init(m, pool);
    m->next_id = (int)aj_getn(d, "next_id", 1);
    m->updated = (int64_t)aj_getn(d, "updated", 0);
    const aj_t *self = aj_get(d, "self");
    for (int i = 0; i < AS_MEM_SELF_N; i++) {
        const aj_t *s = aj_get(self, as_mem_self_key(pool, i));
        const char *t = s && s->type == AJ_STR ? s->s : aj_gets(s, "text");
        if (t) put_text(m->self[i], sizeof m->self[i], t);
        m->self_at[i] = (int64_t)aj_getn(s, "at", 0);
    }
    const aj_t *list = aj_get(d, "memories");
    int max_id = 0;
    for (int i = 0; list && list->type == AJ_ARR && i < list->n && m->n < AS_MEM_MAX; i++) {
        const aj_t *e = list->kid[i];
        const char *t = aj_gets(e, "text");
        int id = (int)aj_getn(e, "id", 0);
        if (!t || !t[0] || id <= 0 || as_mem_find(m, id) >= 0) continue;
        as_mem_item_t *it = &m->it[m->n++];
        memset(it, 0, sizeof *it);
        it->id = id;
        snprintf(it->kind, sizeof it->kind, "%s", kind_of(aj_gets(e, "kind")));
        put_text(it->text, sizeof it->text, t);
        it->imp = clamp_imp((int)aj_getn(e, "imp", 2));
        it->created = (int64_t)aj_getn(e, "created", 0);
        it->updated = (int64_t)aj_getn(e, "updated", (double)it->created);
        it->uses = (int)aj_getn(e, "uses", 0);
        if (id > max_id) max_id = id;
    }
    if (m->next_id <= max_id) m->next_id = max_id + 1;
    aj_free(d);
    return true;
}

static void item_json(ab_t *b, const as_mem_item_t *it)
{
    ab_fmt(b, "{\"id\":%d,\"kind\":", it->id);
    ab_str(b, it->kind);
    ab_fmt(b, ",\"imp\":%d,\"text\":", it->imp);
    ab_str(b, it->text);
    ab_fmt(b, ",\"created\":%lld,\"updated\":%lld,\"uses\":%d}", (long long)it->created, (long long)it->updated, it->uses);
}

char *as_mem_to_json(const as_mem_t *m, size_t max_bytes, size_t *len)
{
    bool *keep = malloc(sizeof *keep * AS_MEM_MAX);
    if (!keep) return NULL;
    for (int i = 0; i < AS_MEM_MAX; i++) keep[i] = i < m->n;
    ab_t b;
    ab_init(&b);
    ab_fmt(&b, "{\"version\":1,\"pool\":\"%s\",\"next_id\":%d,\"updated\":%lld,\"self\":{", as_mem_pool_name(m->pool),
           m->next_id, (long long)m->updated);
    for (int i = 0; i < AS_MEM_SELF_N; i++) {
        if (i) ab_puts(&b, ",");
        ab_str(&b, as_mem_self_key(m->pool, i));
        ab_puts(&b, ":{\"text\":");
        ab_str(&b, m->self[i]);
        ab_fmt(&b, ",\"at\":%lld}", (long long)m->self_at[i]);
    }
    ab_puts(&b, "},\"memories\":[");
    size_t head = b.n;
    if (max_bytes) {
        /* only what fits, the most valuable first; each memory's own size measured, not guessed */
        ranked_t *r = malloc(sizeof *r * (size_t)(m->n ? m->n : 1));
        for (int i = 0; i < m->n; i++) keep[i] = false;
        if (r) {
            for (int i = 0; i < m->n; i++) {
                r[i].i = i;
                r[i].s = as_mem_score(&m->it[i], m->updated);
            }
            qsort(r, (size_t)m->n, sizeof *r, by_score);
            ab_t one;
            ab_init(&one);
            size_t total = head + 4;
            for (int k = 0; k < m->n; k++) {
                ab_reset(&one);
                item_json(&one, &m->it[r[k].i]);
                if (total + one.n + 1 > max_bytes) continue;
                total += one.n + 1;
                keep[r[k].i] = true;
            }
            ab_free(&one);
            free(r);
        }
    }
    bool first = true;
    for (int i = 0; i < m->n; i++) {
        if (!keep[i]) continue;
        if (!first) ab_puts(&b, ",");
        first = false;
        item_json(&b, &m->it[i]);
    }
    ab_puts(&b, "]}");
    free(keep);
    if (b.oom) {
        ab_free(&b);
        return NULL;
    }
    if (len) *len = b.n;
    return ab_take(&b);
}
