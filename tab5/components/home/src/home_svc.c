/* The home services' worker: one thread for every network poll home mode makes, alive only while wanted.
 *
 * Each poller keeps a `want` timestamp the UI refreshes at 10 Hz for what is on screen; a service whose
 * want is older than a few seconds stops polling, and the whole thread returns once nothing has wanted it
 * for WORKER_IDLE_S (its stack is internal RAM, the scarcest thing on the tablet). A command or a one-shot
 * fetch kicks it, starting it if it has ended. */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WANT_S 3.0          /* a service polls this long after the UI last asked */
#define WORKER_IDLE_S 15.0  /* the thread ends this long after anything last asked */
#define WORKER_STACK 12288  /* TLS (Open-Meteo, an https Home Assistant) wants >= 12 KB */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_want_at[4], g_last;
static bool g_running;

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        double now = hal_seconds();
        pthread_mutex_lock(&g_lock);
        bool alive = now - g_last < WORKER_IDLE_S;
        bool w_pc = now - g_want_at[0] < WANT_S, w_ha = now - g_want_at[1] < WANT_S,
             w_wx = now - g_want_at[2] < WANT_S, w_tba = now - g_want_at[3] < WANT_S;
        if (!alive) g_running = false;
        pthread_mutex_unlock(&g_lock);
        if (!alive) break;
        home_player_work(now);
        home_pc_work(now, w_pc);
        home_ha_work(now, w_ha);
        home_wx_work(now, w_wx);
        home_tba_work(now, w_tba);
        usleep(50 * 1000);
    }
    return NULL;
}

static void start_locked(bool *start)
{
    if (!g_running) {
        g_running = true;
        *start = true;
    }
}

static void maybe_start(bool start)
{
    if (!start) return;
    if (!hal_thread("home", worker, NULL, WORKER_STACK)) {
        pthread_mutex_lock(&g_lock);
        g_running = false;
        pthread_mutex_unlock(&g_lock);
    }
}

void home_want(unsigned mask)
{
    double now = hal_seconds();
    bool start = false;
    pthread_mutex_lock(&g_lock);
    if (mask & HOME_WANT_PC) g_want_at[0] = now;
    if (mask & HOME_WANT_HA) g_want_at[1] = now;
    if (mask & HOME_WANT_WEATHER) g_want_at[2] = now;
    if (mask & HOME_WANT_TBA) g_want_at[3] = now;
    if (mask) {
        g_last = now;
        start_locked(&start);
    }
    pthread_mutex_unlock(&g_lock);
    maybe_start(start);
}

void home_kick(void)
{
    bool start = false;
    pthread_mutex_lock(&g_lock);
    g_last = hal_seconds();
    start_locked(&start);
    pthread_mutex_unlock(&g_lock);
    maybe_start(start);
}

/* ---- helpers ---- */

bool home_json_parse(home_json_t *j, const char *js, size_t len)
{
    memset(j, 0, sizeof *j);
    int max = (int)(len / 4) + 32;
    if (max > 200000) max = 200000;
    j->toks = malloc((size_t)max * sizeof *j->toks);
    if (!j->toks) return false;
    int n = jl_parse(js, len, j->toks, max);
    if (n <= 0) {
        free(j->toks);
        j->toks = NULL;
        return false;
    }
    j->d = (jl_doc_t){ .js = js, .t = j->toks, .n = n };
    return true;
}

void home_json_free(home_json_t *j)
{
    free(j->toks);
    j->toks = NULL;
}

void home_json_str(const home_json_t *j, int obj, const char *key, char *out, size_t n)
{
    out[0] = 0;
    int t = obj < 0 ? -1 : jl_get(&j->d, obj, key);
    if (t < 0 || j->d.t[t].type == JL_NULL) return;
    jl_str(&j->d, t, out, n);
    home_fold_text(out);
}

double home_json_num(const home_json_t *j, int obj, const char *key, double fallback)
{
    int t = obj < 0 ? -1 : jl_get(&j->d, obj, key);
    if (t < 0 || j->d.t[t].type != JL_NUM) return fallback;
    return jl_num(&j->d, t, fallback);
}

void home_urlenc(const char *in, char *out, size_t n)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int home_b64_decode(const char *in, size_t len, unsigned char *out, size_t max)
{
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64v(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= max) return -1;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    return (int)o;
}

/* UTF-8 → what the faces carry: ASCII, Latin-1, and the — and · the UI itself uses. */
void home_fold_text(char *s)
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
            if ((r[k] & 0xC0) != 0x80) { len = k; break; } /* broken sequence: take what's there */
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
            if ((size_t)len >= rl) { /* never longer than what it replaces: in place is safe */
                memcpy(w, rep, rl);
                w += rl;
            }
        } else if (len == 2 && cp >= 0xA0 && cp <= 0xFF) {
            memcpy(w, r, 2);
            w += 2;
        }
        /* anything else (emoji, CJK, symbols the faces lack) is dropped */
        r += len;
    }
    *w = 0;
    /* no doubled or edge spaces where something was dropped */
    char *a = s, *b = s;
    while (*a == ' ') a++;
    for (; *a; a++)
        if (!(*a == ' ' && (a[1] == ' ' || a[1] == 0))) *b++ = *a;
    *b = 0;
}
