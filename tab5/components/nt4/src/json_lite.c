#include "json_lite.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *js;
    size_t len, pos;
    jl_tok_t *t;
    int max, n;
} parser_t;

static void ws_skip(parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static int alloc(parser_t *p, jl_type_t type, int start)
{
    if (p->n >= p->max) return -1;
    int i = p->n++;
    p->t[i] = (jl_tok_t){ .type = type, .start = start, .end = start, .size = 0, .skip = i + 1 };
    return i;
}

static int value(parser_t *p, int depth);

static int string_tok(parser_t *p)
{
    size_t start = ++p->pos; /* past the opening quote */
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == '\\') {
            p->pos += 2;
            continue;
        }
        if (c == '"') {
            int i = alloc(p, JL_STR, (int)start);
            if (i < 0) return -1;
            p->t[i].end = (int)p->pos;
            p->pos++;
            return i;
        }
        p->pos++;
    }
    return -1;
}

static int container(parser_t *p, int depth, bool obj)
{
    if (depth > 32) return -1;
    int i = alloc(p, obj ? JL_OBJ : JL_ARR, (int)p->pos);
    if (i < 0) return -1;
    p->pos++;
    char close = obj ? '}' : ']';
    ws_skip(p);
    if (p->pos < p->len && p->js[p->pos] == close) {
        p->pos++;
        p->t[i].end = (int)p->pos;
        p->t[i].skip = p->n;
        return i;
    }
    for (;;) {
        ws_skip(p);
        if (obj) {
            if (p->pos >= p->len || p->js[p->pos] != '"') return -1;
            if (string_tok(p) < 0) return -1;
            ws_skip(p);
            if (p->pos >= p->len || p->js[p->pos] != ':') return -1;
            p->pos++;
            ws_skip(p);
        }
        if (value(p, depth + 1) < 0) return -1;
        p->t[i].size++;
        ws_skip(p);
        if (p->pos >= p->len) return -1;
        char c = p->js[p->pos++];
        if (c == ',') continue;
        if (c == close) break;
        return -1;
    }
    p->t[i].end = (int)p->pos;
    p->t[i].skip = p->n;
    return i;
}

static int value(parser_t *p, int depth)
{
    ws_skip(p);
    if (p->pos >= p->len) return -1;
    char c = p->js[p->pos];
    if (c == '{') return container(p, depth, true);
    if (c == '[') return container(p, depth, false);
    if (c == '"') return string_tok(p);
    size_t start = p->pos;
    while (p->pos < p->len) {
        char d = p->js[p->pos];
        if (d == ',' || d == ']' || d == '}' || d == ' ' || d == '\n' || d == '\r' || d == '\t') break;
        p->pos++;
    }
    if (p->pos == start) return -1;
    jl_type_t type = JL_NUM;
    if (c == 't') type = JL_TRUE;
    else if (c == 'f') type = JL_FALSE;
    else if (c == 'n') type = JL_NULL;
    int i = alloc(p, type, (int)start);
    if (i < 0) return -1;
    p->t[i].end = (int)p->pos;
    return i;
}

int jl_parse(const char *js, size_t len, jl_tok_t *toks, int max)
{
    parser_t p = { .js = js, .len = len, .t = toks, .max = max };
    if (value(&p, 0) < 0) return -1;
    return p.n;
}

bool jl_eq(const jl_doc_t *d, int tok, const char *s)
{
    if (tok < 0 || tok >= d->n) return false;
    size_t n = (size_t)(d->t[tok].end - d->t[tok].start);
    return strlen(s) == n && memcmp(d->js + d->t[tok].start, s, n) == 0;
}

int jl_get(const jl_doc_t *d, int obj, const char *key)
{
    if (obj < 0 || obj >= d->n || d->t[obj].type != JL_OBJ) return -1;
    int i = obj + 1;
    for (int m = 0; m < d->t[obj].size; m++) {
        int v = i + 1;
        if (jl_eq(d, i, key)) return v;
        i = d->t[v].skip;
    }
    return -1;
}

int jl_at(const jl_doc_t *d, int arr, int idx)
{
    if (arr < 0 || arr >= d->n || d->t[arr].type != JL_ARR || idx < 0 || idx >= d->t[arr].size) return -1;
    int i = arr + 1;
    for (int k = 0; k < idx; k++) i = d->t[i].skip;
    return i;
}

int jl_path(const jl_doc_t *d, int obj, const char *path)
{
    char key[64];
    int cur = obj;
    while (*path && cur >= 0) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof key) return -1;
        memcpy(key, path, n);
        key[n] = 0;
        cur = jl_get(d, cur, key);
        path += n + (dot ? 1 : 0);
    }
    return cur;
}

bool jl_str(const jl_doc_t *d, int tok, char *out, size_t n)
{
    if (!n) return false;
    out[0] = 0;
    if (tok < 0 || tok >= d->n) return false;
    const char *s = d->js + d->t[tok].start, *e = d->js + d->t[tok].end;
    size_t o = 0;
    while (s < e && o + 1 < n) {
        char c = *s++;
        if (c == '\\' && s < e && d->t[tok].type == JL_STR) {
            char x = *s++;
            switch (x) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* Basic Multilingual Plane escapes to UTF-8; surrogate pairs are rare in robot names */
                unsigned cp = 0;
                for (int k = 0; k < 4 && s < e; k++, s++) {
                    char h = *s;
                    cp = cp * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                }
                if (cp < 0x80) { c = (char)cp; break; }
                if (cp < 0x800) {
                    if (o + 2 >= n) goto done;
                    out[o++] = (char)(0xc0 | (cp >> 6));
                    c = (char)(0x80 | (cp & 0x3f));
                    break;
                }
                if (o + 3 >= n) goto done;
                out[o++] = (char)(0xe0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                c = (char)(0x80 | (cp & 0x3f));
                break;
            }
            default: c = x; break;
            }
        }
        out[o++] = c;
    }
done:
    out[o] = 0;
    return true;
}

double jl_num(const jl_doc_t *d, int tok, double fallback)
{
    if (tok < 0 || tok >= d->n) return fallback;
    if (d->t[tok].type == JL_TRUE) return 1;
    if (d->t[tok].type == JL_FALSE) return 0;
    if (d->t[tok].type != JL_NUM && d->t[tok].type != JL_STR) return fallback;
    char buf[40];
    jl_str(d, tok, buf, sizeof buf);
    char *end;
    double v = strtod(buf, &end);
    return end == buf ? fallback : v;
}

bool jl_bool(const jl_doc_t *d, int tok, bool fallback)
{
    if (tok < 0 || tok >= d->n) return fallback;
    if (d->t[tok].type == JL_TRUE) return true;
    if (d->t[tok].type == JL_FALSE) return false;
    if (d->t[tok].type == JL_NUM) return jl_num(d, tok, 0) != 0;
    return fallback;
}
