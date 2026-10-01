/* The intel component's JSON and text helpers (intel_priv.h). Pure. */
#include "intel_priv.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ix_parse(ix_json_t *j, const char *js, size_t len)
{
    memset(j, 0, sizeof *j);
    if (!js || !len) return false;
    for (int div = 4; div >= 2; div -= 2) {
        int max = (int)(len / (size_t)div) + 64;
        j->toks = malloc((size_t)max * sizeof *j->toks);
        if (!j->toks) return false;
        int n = jl_parse(js, len, j->toks, max);
        if (n > 0) {
            j->d = (jl_doc_t){ .js = js, .t = j->toks, .n = n };
            return true;
        }
        free(j->toks);
        j->toks = NULL;
    }
    return false;
}

void ix_free(ix_json_t *j)
{
    free(j->toks);
    j->toks = NULL;
}

int ix_get(const ix_json_t *j, int obj, const char *key)
{
    if (obj < 0 || j->d.t[obj].type != JL_OBJ) return -1;
    int t = jl_get(&j->d, obj, key);
    if (t < 0 || j->d.t[t].type == JL_NULL) return -1;
    return t;
}

int ix_get_any(const ix_json_t *j, int obj, const char *const *keys)
{
    for (; keys && *keys; keys++) {
        int t = ix_get(j, obj, *keys);
        if (t >= 0) return t;
    }
    return -1;
}

bool ix_tok_str(const ix_json_t *j, int tok, char *out, size_t n)
{
    if (!n) return false;
    out[0] = 0;
    if (tok < 0 || j->d.t[tok].type == JL_NULL || j->d.t[tok].type == JL_OBJ || j->d.t[tok].type == JL_ARR) return false;
    jl_str(&j->d, tok, out, n);
    ix_fold(out);
    return out[0] != 0;
}

bool ix_str(const ix_json_t *j, int obj, const char *key, char *out, size_t n)
{
    if (n) out[0] = 0;
    return ix_tok_str(j, ix_get(j, obj, key), out, n);
}

double ix_tok_num(const ix_json_t *j, int tok, double fallback)
{
    if (tok < 0) return fallback;
    jl_type_t ty = j->d.t[tok].type;
    if (ty == JL_NUM) return jl_num(&j->d, tok, fallback);
    if (ty == JL_STR) {
        char b[32];
        jl_str(&j->d, tok, b, sizeof b);
        char *e;
        double v = strtod(b, &e);
        return e != b ? v : fallback;
    }
    return fallback;
}

double ix_num(const ix_json_t *j, int obj, const char *key, double fallback)
{
    return ix_tok_num(j, ix_get(j, obj, key), fallback);
}

time_t ix_time(const ix_json_t *j, int tok)
{
    double v = ix_tok_num(j, tok, 0);
    if (v <= 0) return 0;
    if (v > 1e11) v /= 1000.0; /* milliseconds */
    return (time_t)(v + 0.5);
}

int ix_team(const ix_json_t *j, int tok)
{
    if (tok < 0) return 0;
    if (j->d.t[tok].type == JL_NUM) return (int)jl_num(&j->d, tok, 0);
    char b[16];
    if (!ix_tok_str(j, tok, b, sizeof b)) return 0;
    const char *p = b;
    if (!strncmp(p, "frc", 3)) p += 3;
    int t = atoi(p);
    return t > 0 && t < 100000 ? t : 0;
}

void ix_fold(char *s)
{
    if (!s) return;
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    while (*r) {
        unsigned char c = *r;
        if (c < 0x80) {
            *w++ = (c < 0x20) ? ' ' : c;
            r++;
            continue;
        }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        for (int k = 1; k < len; k++)
            if ((r[k] & 0xC0) != 0x80) { len = k; break; }
        unsigned cp = 0;
        if (len == 2) cp = ((c & 0x1Fu) << 6) | (r[1] & 0x3Fu);
        else if (len == 3) cp = ((c & 0x0Fu) << 12) | ((r[1] & 0x3Fu) << 6) | (r[2] & 0x3Fu);
        const char *rep = NULL;
        switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: rep = "'"; break;
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: rep = "\""; break;
        case 0x2026: rep = "..."; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: rep = "-"; break;
        case 0x2022: case 0x00B7: rep = "\xc2\xb7"; break;
        case 0x2014: rep = "\xe2\x80\x94"; break;
        default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if ((size_t)len >= rl) {
                memcpy(w, rep, rl);
                w += rl;
            }
        } else if (len == 2 && cp >= 0xA0 && cp <= 0xFF) {
            memcpy(w, r, 2);
            w += 2;
        }
        r += len;
    }
    *w = 0;
    char *a = s, *b = s;
    while (*a == ' ') a++;
    for (; *a; a++)
        if (!(*a == ' ' && (a[1] == ' ' || a[1] == 0))) *b++ = *a;
    *b = 0;
}

void ix_addf(char *out, size_t n, const char *fmt, ...)
{
    size_t l = strlen(out);
    if (l + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + l, n - l, fmt, ap);
    va_end(ap);
}

bool ix_has(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}

void ix_hm(time_t t, char *out, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%d:%02d", tm.tm_hour, tm.tm_min);
}
