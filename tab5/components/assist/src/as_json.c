#include "as_json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *as_strdup(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *d = malloc(n + 1);
    if (d) memcpy(d, s ? s : "", n + 1);
    return d;
}

void as_urlencode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!n) return;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 4 < n; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* ---- writer ---- */

void ab_init(ab_t *b) { memset(b, 0, sizeof *b); }
void ab_free(ab_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}
void ab_reset(ab_t *b)
{
    b->n = 0;
    b->oom = false;
    if (b->p) b->p[0] = 0;
}

static bool grow(ab_t *b, size_t more)
{
    if (b->oom) return false;
    if (b->n + more + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) {
        b->oom = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

void ab_raw(ab_t *b, const char *s, size_t n)
{
    if (!grow(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void ab_puts(ab_t *b, const char *s) { ab_raw(b, s, strlen(s)); }

void ab_vfmt(ab_t *b, const char *fmt, va_list ap)
{
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0 || !grow(b, (size_t)n)) return;
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    b->n += (size_t)n;
}

void ab_fmt(ab_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ab_vfmt(b, fmt, ap);
    va_end(ap);
}

void ab_strn(ab_t *b, const char *s, size_t n)
{
    ab_raw(b, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[8];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                snprintf(u, sizeof u, "\\u%04x", c);
                esc = u;
            }
        }
        if (esc) {
            ab_raw(b, s + i - run, run);
            run = 0;
            ab_puts(b, esc);
        } else {
            run++;
        }
    }
    ab_raw(b, s + n - run, run);
    ab_raw(b, "\"", 1);
}

void ab_str(ab_t *b, const char *s) { ab_strn(b, s ? s : "", strlen(s ? s : "")); }

void ab_num(ab_t *b, double v)
{
    if (!isfinite(v)) {
        ab_puts(b, "null");
        return;
    }
    if (v == (double)(long long)v && fabs(v) < 1e15) ab_fmt(b, "%lld", (long long)v);
    else ab_fmt(b, "%.6g", v);
}

char *ab_take(ab_t *b)
{
    if (!b->p) grow(b, 0);
    char *p = b->p;
    if (p) p[b->n] = 0;
    memset(b, 0, sizeof *b);
    return p;
}

/* ---- tree ---- */

aj_t *aj_new(aj_type_t t)
{
    aj_t *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

aj_t *aj_new_strn(const char *s, size_t n)
{
    aj_t *v = aj_new(AJ_STR);
    if (!v) return NULL;
    v->s = malloc(n + 1);
    if (!v->s) {
        free(v);
        return NULL;
    }
    memcpy(v->s, s, n);
    v->s[n] = 0;
    v->len = n;
    v->cap = n + 1;
    return v;
}

aj_t *aj_new_str(const char *s) { return aj_new_strn(s ? s : "", strlen(s ? s : "")); }

aj_t *aj_new_num(double x)
{
    aj_t *v = aj_new(AJ_NUM);
    if (!v) return NULL;
    ab_t b;
    ab_init(&b);
    ab_num(&b, x);
    v->s = ab_take(&b);
    v->len = v->s ? strlen(v->s) : 0;
    v->num = x;
    return v;
}

aj_t *aj_new_bool(bool x) { return aj_new(x ? AJ_TRUE : AJ_FALSE); }

void aj_free(aj_t *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) aj_free(v->kid[i]);
    free(v->kid);
    free(v->s);
    free(v->key);
    free(v);
}

aj_t *aj_clone(const aj_t *v)
{
    if (!v) return NULL;
    aj_t *c = aj_new(v->type);
    if (!c) return NULL;
    c->num = v->num;
    if (v->s) {
        c->s = malloc(v->len + 1);
        if (c->s) {
            memcpy(c->s, v->s, v->len + 1);
            c->len = v->len;
            c->cap = v->len + 1;
        }
    }
    if (v->key) c->key = as_strdup(v->key);
    for (int i = 0; i < v->n; i++) aj_push(c, aj_clone(v->kid[i]));
    return c;
}

static bool reserve_kids(aj_t *a, int n)
{
    if (n <= a->kcap) return true;
    int cap = a->kcap ? a->kcap * 2 : 4;
    while (cap < n) cap *= 2;
    aj_t **k = realloc(a->kid, (size_t)cap * sizeof *k);
    if (!k) return false;
    a->kid = k;
    a->kcap = cap;
    return true;
}

void aj_insert(aj_t *a, int at, aj_t *v)
{
    if (!a || !v) {
        aj_free(v);
        return;
    }
    if (!reserve_kids(a, a->n + 1)) {
        aj_free(v);
        return;
    }
    if (at < 0 || at > a->n) at = a->n;
    memmove(a->kid + at + 1, a->kid + at, (size_t)(a->n - at) * sizeof *a->kid);
    a->kid[at] = v;
    a->n++;
}

void aj_push(aj_t *a, aj_t *v) { aj_insert(a, -1, v); }

void aj_remove(aj_t *a, int at)
{
    if (!a || at < 0 || at >= a->n) return;
    aj_free(a->kid[at]);
    memmove(a->kid + at, a->kid + at + 1, (size_t)(a->n - at - 1) * sizeof *a->kid);
    a->n--;
}

void aj_set(aj_t *o, const char *key, aj_t *v)
{
    if (!o || !v) {
        aj_free(v);
        return;
    }
    free(v->key);
    v->key = as_strdup(key);
    for (int i = 0; i < o->n; i++) {
        if (o->kid[i]->key && !strcmp(o->kid[i]->key, key)) {
            aj_free(o->kid[i]);
            o->kid[i] = v;
            return;
        }
    }
    aj_push(o, v);
}

void aj_str_append(aj_t *v, const char *s, size_t n)
{
    if (!v || v->type != AJ_STR || !n) return;
    if (v->len + n + 1 > v->cap) {
        size_t cap = v->cap ? v->cap : 32;
        while (cap < v->len + n + 1) cap *= 2;
        char *p = realloc(v->s, cap);
        if (!p) return;
        v->s = p;
        v->cap = cap;
    }
    memcpy(v->s + v->len, s, n);
    v->len += n;
    v->s[v->len] = 0;
}

aj_t *aj_get(const aj_t *o, const char *key)
{
    if (!o || o->type != AJ_OBJ) return NULL;
    for (int i = 0; i < o->n; i++)
        if (o->kid[i]->key && !strcmp(o->kid[i]->key, key)) return o->kid[i];
    return NULL;
}

aj_t *aj_at(const aj_t *a, int i)
{
    if (!a || (a->type != AJ_ARR && a->type != AJ_OBJ) || i < 0 || i >= a->n) return NULL;
    return a->kid[i];
}

const char *aj_gets(const aj_t *o, const char *key)
{
    aj_t *v = aj_get(o, key);
    return v && v->type == AJ_STR ? v->s : NULL;
}

double aj_getn(const aj_t *o, const char *key, double fallback)
{
    aj_t *v = aj_get(o, key);
    return v && v->type == AJ_NUM ? v->num : fallback;
}

bool aj_is(const aj_t *v, aj_type_t t) { return v && v->type == t; }
bool aj_is_bool(const aj_t *v) { return v && (v->type == AJ_TRUE || v->type == AJ_FALSE); }

void aj_write(ab_t *b, const aj_t *v)
{
    if (!v) {
        ab_puts(b, "null");
        return;
    }
    switch (v->type) {
    case AJ_NULL: ab_puts(b, "null"); break;
    case AJ_FALSE: ab_puts(b, "false"); break;
    case AJ_TRUE: ab_puts(b, "true"); break;
    case AJ_NUM: ab_raw(b, v->s, v->len); break;
    case AJ_STR: ab_strn(b, v->s, v->len); break;
    case AJ_ARR:
    case AJ_OBJ:
        ab_raw(b, v->type == AJ_ARR ? "[" : "{", 1);
        for (int i = 0; i < v->n; i++) {
            if (i) ab_raw(b, ",", 1);
            if (v->type == AJ_OBJ) {
                ab_str(b, v->kid[i]->key);
                ab_raw(b, ":", 1);
            }
            aj_write(b, v->kid[i]);
        }
        ab_raw(b, v->type == AJ_ARR ? "]" : "}", 1);
        break;
    }
}

char *aj_dump(const aj_t *v)
{
    ab_t b;
    ab_init(&b);
    aj_write(&b, v);
    return ab_take(&b);
}

/* ---- parser ---- */

typedef struct {
    const char *s;
    size_t i, n;
    char *err;
    size_t errn;
    bool failed;
} px_t;

#define MAX_DEPTH 48

static void fail(px_t *p, const char *what)
{
    if (p->failed) return;
    p->failed = true;
    if (p->err && p->errn) snprintf(p->err, p->errn, "%s at byte %zu", what, p->i);
}

static void ws(px_t *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static int hex4(px_t *p)
{
    if (p->i + 4 > p->n) return -1;
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    p->i += 4;
    return v;
}

static void utf8(ab_t *b, uint32_t cp)
{
    char o[4];
    int n;
    if (cp < 0x80) { o[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
        o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    ab_raw(b, o, (size_t)n);
}

/* p->i sits on the opening quote */
static char *string(px_t *p, size_t *len)
{
    ab_t b;
    ab_init(&b);
    p->i++;
    size_t run = p->i;
    for (;;) {
        if (p->i >= p->n) {
            fail(p, "unterminated string");
            break;
        }
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            ab_raw(&b, p->s + run, p->i - run);
            p->i++;
            if (!b.p) ab_raw(&b, "", 0);
            if (b.oom) break;
            *len = b.n;
            return ab_take(&b);
        }
        if (c < 0x20) {
            fail(p, "control character in string");
            break;
        }
        if (c != '\\') {
            p->i++;
            continue;
        }
        ab_raw(&b, p->s + run, p->i - run);
        p->i++;
        if (p->i >= p->n) {
            fail(p, "unterminated escape");
            break;
        }
        char e = p->s[p->i++];
        switch (e) {
        case '"': ab_raw(&b, "\"", 1); break;
        case '\\': ab_raw(&b, "\\", 1); break;
        case '/': ab_raw(&b, "/", 1); break;
        case 'b': ab_raw(&b, "\b", 1); break;
        case 'f': ab_raw(&b, "\f", 1); break;
        case 'n': ab_raw(&b, "\n", 1); break;
        case 'r': ab_raw(&b, "\r", 1); break;
        case 't': ab_raw(&b, "\t", 1); break;
        case 'u': {
            int u = hex4(p);
            if (u < 0) {
                fail(p, "bad \\u escape");
                goto out;
            }
            uint32_t cp = (uint32_t)u;
            if (cp >= 0xD800 && cp < 0xDC00) {
                /* a high surrogate wants its low half; alone it becomes U+FFFD */
                if (p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    size_t save = p->i;
                    p->i += 2;
                    int lo = hex4(p);
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                    else {
                        p->i = save;
                        cp = 0xFFFD;
                    }
                } else cp = 0xFFFD;
            } else if (cp >= 0xDC00 && cp < 0xE000) cp = 0xFFFD;
            utf8(&b, cp);
            break;
        }
        default:
            p->i--;
            fail(p, "bad escape");
            goto out;
        }
        run = p->i;
    }
out:
    ab_free(&b);
    return NULL;
}

static bool digits(px_t *p)
{
    size_t start = p->i;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    return p->i > start;
}

static aj_t *value(px_t *p, int depth);

static aj_t *number(px_t *p)
{
    size_t start = p->i;
    if (p->s[p->i] == '-') p->i++;
    if (p->i < p->n && p->s[p->i] == '0') {
        p->i++;
        if (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
            fail(p, "leading zero");
            return NULL;
        }
    } else if (!digits(p)) {
        fail(p, "bad number");
        return NULL;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (!digits(p)) {
            fail(p, "bad fraction");
            return NULL;
        }
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (!digits(p)) {
            fail(p, "bad exponent");
            return NULL;
        }
    }
    aj_t *v = aj_new(AJ_NUM);
    if (!v) return NULL;
    v->len = p->i - start;
    v->s = malloc(v->len + 1);
    if (!v->s) {
        free(v);
        return NULL;
    }
    memcpy(v->s, p->s + start, v->len);
    v->s[v->len] = 0;
    v->num = strtod(v->s, NULL);
    return v;
}

static bool lit(px_t *p, const char *w)
{
    size_t n = strlen(w);
    if (p->i + n > p->n || memcmp(p->s + p->i, w, n) != 0) return false;
    p->i += n;
    return true;
}

static aj_t *container(px_t *p, int depth, bool obj)
{
    aj_t *v = aj_new(obj ? AJ_OBJ : AJ_ARR);
    if (!v) return NULL;
    p->i++;
    ws(p);
    if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) {
        p->i++;
        return v;
    }
    for (;;) {
        char *key = NULL;
        size_t kl;
        if (obj) {
            ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') {
                fail(p, "expected a member name");
                break;
            }
            key = string(p, &kl);
            if (!key) break;
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') {
                free(key);
                fail(p, "expected ':'");
                break;
            }
            p->i++;
        }
        aj_t *k = value(p, depth + 1);
        if (!k) {
            free(key);
            break;
        }
        k->key = key;
        aj_push(v, k);
        ws(p);
        if (p->i >= p->n) {
            fail(p, "unterminated container");
            break;
        }
        char c = p->s[p->i++];
        if (c == ',') continue;
        if (c == (obj ? '}' : ']')) return v;
        p->i--;
        fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        break;
    }
    aj_free(v);
    return NULL;
}

static aj_t *value(px_t *p, int depth)
{
    if (depth > MAX_DEPTH) {
        fail(p, "nested too deeply");
        return NULL;
    }
    ws(p);
    if (p->i >= p->n) {
        fail(p, "unexpected end");
        return NULL;
    }
    char c = p->s[p->i];
    if (c == '{') return container(p, depth, true);
    if (c == '[') return container(p, depth, false);
    if (c == '"') {
        size_t len;
        char *s = string(p, &len);
        if (!s) return NULL;
        aj_t *v = aj_new(AJ_STR);
        if (!v) {
            free(s);
            return NULL;
        }
        v->s = s;
        v->len = len;
        v->cap = len + 1;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return number(p);
    if (lit(p, "true")) return aj_new(AJ_TRUE);
    if (lit(p, "false")) return aj_new(AJ_FALSE);
    if (lit(p, "null")) return aj_new(AJ_NULL);
    fail(p, "unexpected character");
    return NULL;
}

aj_t *aj_parse(const char *s, size_t len, char *err, size_t errn)
{
    if (err && errn) err[0] = 0;
    px_t p = { .s = s ? s : "", .n = s ? len : 0, .err = err, .errn = errn };
    aj_t *v = value(&p, 0);
    if (v) {
        ws(&p);
        if (p.i != p.n) {
            fail(&p, "trailing characters");
            aj_free(v);
            return NULL;
        }
    } else if (!p.failed) {
        fail(&p, "out of memory");
    }
    return v;
}

/* ---- schema ---- */

static const char *type_name(const aj_t *v)
{
    switch (v->type) {
    case AJ_NULL: return "null";
    case AJ_TRUE:
    case AJ_FALSE: return "boolean";
    case AJ_NUM: return "number";
    case AJ_STR: return "string";
    case AJ_ARR: return "array";
    default: return "object";
    }
}

static bool type_ok(const char *want, const aj_t *v)
{
    if (!strcmp(want, "integer")) return v->type == AJ_NUM && v->num == floor(v->num) && isfinite(v->num);
    return !strcmp(want, type_name(v));
}

/* UTF-8 characters, not bytes, for length limits */
static size_t chars(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static bool check(const aj_t *sc, const aj_t *v, char *path, size_t pl, char *err, size_t errn)
{
    const aj_t *t = aj_get(sc, "type");
    if (t) {
        bool ok = false;
        if (t->type == AJ_STR) ok = type_ok(t->s, v);
        else for (int i = 0; i < t->n && !ok; i++) ok = t->kid[i]->type == AJ_STR && type_ok(t->kid[i]->s, v);
        if (!ok) {
            snprintf(err, errn, "%s: expected %s, got %s", path[0] ? path : "input", t->type == AJ_STR ? t->s : "one of the allowed types",
                     type_name(v));
            return false;
        }
    }
    const aj_t *en = aj_get(sc, "enum");
    if (en) {
        bool ok = false;
        for (int i = 0; i < en->n && !ok; i++) {
            const aj_t *e = en->kid[i];
            if (e->type == AJ_STR && v->type == AJ_STR) ok = !strcmp(e->s, v->s);
            else if (e->type == AJ_NUM && v->type == AJ_NUM) ok = e->num == v->num;
        }
        if (!ok) {
            ab_t b;
            ab_init(&b);
            aj_write(&b, en);
            snprintf(err, errn, "%s: must be one of %s", path[0] ? path : "input", b.p ? b.p : "the listed values");
            ab_free(&b);
            return false;
        }
    }
    if (v->type == AJ_NUM) {
        double mn = aj_getn(sc, "minimum", NAN), mx = aj_getn(sc, "maximum", NAN);
        if (!isfinite(v->num) || (mn == mn && v->num < mn) || (mx == mx && v->num > mx)) {
            snprintf(err, errn, "%s: %s is out of range [%g, %g]", path, v->s, mn, mx);
            return false;
        }
    }
    if (v->type == AJ_STR) {
        double mn = aj_getn(sc, "minLength", NAN), mx = aj_getn(sc, "maxLength", NAN);
        size_t c = chars(v->s, v->len);
        if ((mn == mn && c < mn) || (mx == mx && c > mx)) {
            snprintf(err, errn, "%s: length %zu is outside [%g, %g]", path, c, mn == mn ? mn : 0, mx == mx ? mx : INFINITY);
            return false;
        }
    }
    if (v->type == AJ_ARR) {
        double mn = aj_getn(sc, "minItems", NAN), mx = aj_getn(sc, "maxItems", NAN);
        if ((mn == mn && v->n < mn) || (mx == mx && v->n > mx)) {
            snprintf(err, errn, "%s: %d items is outside [%g, %g]", path, v->n, mn == mn ? mn : 0, mx == mx ? mx : INFINITY);
            return false;
        }
        const aj_t *items = aj_get(sc, "items");
        if (items) {
            size_t base = strlen(path);
            for (int i = 0; i < v->n; i++) {
                snprintf(path + base, pl - base, "[%d]", i);
                if (!check(items, v->kid[i], path, pl, err, errn)) return false;
            }
            path[base] = 0;
        }
    }
    if (v->type == AJ_OBJ) {
        const aj_t *props = aj_get(sc, "properties"), *req = aj_get(sc, "required"), *add = aj_get(sc, "additionalProperties");
        size_t base = strlen(path);
        for (int i = 0; req && i < req->n; i++) {
            if (req->kid[i]->type == AJ_STR && !aj_get(v, req->kid[i]->s)) {
                snprintf(err, errn, "%s%smissing required field \"%s\"", path, base ? ": " : "", req->kid[i]->s);
                return false;
            }
        }
        for (int i = 0; i < v->n; i++) {
            const aj_t *m = v->kid[i];
            const aj_t *ps = aj_get(props, m->key);
            snprintf(path + base, pl - base, "%s%s", base ? "." : "", m->key);
            if (!ps) {
                if (add && add->type == AJ_FALSE) {
                    snprintf(err, errn, "unexpected field \"%s\"", path);
                    return false;
                }
                continue;
            }
            if (!check(ps, m, path, pl, err, errn)) return false;
        }
        path[base] = 0;
    }
    return true;
}

bool aj_validate(const aj_t *schema, const aj_t *v, char *err, size_t errn)
{
    char path[160] = "";
    char tmp[1];
    if (!err || !errn) {
        err = tmp;
        errn = sizeof tmp;
    }
    err[0] = 0;
    if (!schema || !v) {
        snprintf(err, errn, "no input");
        return false;
    }
    return check(schema, v, path, sizeof path, err, errn);
}
