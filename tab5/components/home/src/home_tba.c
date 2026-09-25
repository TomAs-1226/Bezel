/* The Blue Alliance, read API v3, on the home services' worker (tba.h).
 *
 * A poll walks four endpoints in order: the team's events this season (every 10 minutes: the list barely
 * changes), then, for the event picked from it, the team's matches, its status and the event's rankings.
 * Each endpoint keeps its last body (PSRAM) and Last-Modified; a request sends If-Modified-Since, a 304
 * keeps the body, and every poll parses what it has into one fresh tba_state_t, published whole. A body
 * that came over the network is also written to <sd>/CATOS/DATA/tba/<path>.json; an endpoint with no body
 * yet reads that file first, so the app shows the last known event offline. */
#include "home_priv.h"
#include "tba.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "https://www.thebluealliance.com/api/v3"
#define POLL_S 60.0
#define POLL_LIVE_S 20.0     /* on a day the event runs: a match every ~7 minutes */
#define EVENTS_S 600.0
#define RETRY_S 20.0
#define NO_CLOCK_S 10.0
#define BODY_MAX (768 * 1024)

enum { EP_EVENTS, EP_MATCHES, EP_STATUS, EP_RANKS, EP_N };

typedef struct {
    char url[192];
    char lm[40];             /* Last-Modified, sent back as If-Modified-Since */
    char *body;              /* the last answer, NUL-terminated; PSRAM */
    size_t len;
    time_t at;               /* when it was fetched (or the card file's time) */
} ep_t;

typedef struct {
    char key[128];
    int team;
    unsigned cfg_gen;
    bool kick;
    double next_poll, next_events;
    ep_t ep[EP_N];
    tba_state_t st;          /* published */
    tba_state_t work;        /* the worker's scratch */
} tb_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static tb_t *T;              /* calloc'd: ~20 KB, PSRAM on the tablet */

static tb_t *tb(void)
{
    if (!T) T = calloc(1, sizeof *T);
    return T;
}

/* ---- the public side (UI thread) ---- */

void tba_config(const char *key, int team)
{
    pthread_mutex_lock(&g_lock);
    tb_t *t = tb();
    if (t && (strcmp(t->key, key ? key : "") != 0 || t->team != team)) {
        memset(t->key, 0, sizeof t->key);
        snprintf(t->key, sizeof t->key, "%s", key ? key : "");
        t->team = team;
        t->cfg_gen++;
        t->kick = true;
        t->next_events = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void tba_want(void) { home_want(HOME_WANT_TBA); }

void tba_refresh(void)
{
    pthread_mutex_lock(&g_lock);
    if (tb()) tb()->kick = true;
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

unsigned tba_gen(void)
{
    pthread_mutex_lock(&g_lock);
    unsigned g = T ? T->st.gen : 0;
    pthread_mutex_unlock(&g_lock);
    return g;
}

void tba_get(tba_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (T) *out = T->st;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_lock);
}

bool tba_next_match(tba_match_t *out, time_t *when)
{
    bool found = false;
    pthread_mutex_lock(&g_lock);
    if (T && T->st.phase == TBA_READY) {
        for (int i = 0; i < T->st.nmatches && !found; i++) {
            const tba_match_t *m = &T->st.matches[i];
            if (m->played || !m->ours) continue;
            if (out) *out = *m;
            if (when) *when = m->predicted ? m->predicted : m->time;
            found = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

tba_phase_t tba_upcoming(tba_match_t *out, int max, int *n, bool *live, bool *offline, char *event_key, size_t kn)
{
    int k = 0;
    pthread_mutex_lock(&g_lock);
    const tba_state_t *s = T ? &T->st : NULL;
    tba_phase_t ph = s ? s->phase : TBA_IDLE;
    if (live) *live = s && s->live;
    if (offline) *offline = s && s->offline;
    if (event_key && kn) snprintf(event_key, kn, "%s", s && s->have_event ? s->event_key : "");
    for (int i = 0; s && ph == TBA_READY && i < s->nmatches && k < max; i++)
        if (!s->matches[i].played && s->matches[i].ours) out[k++] = s->matches[i];
    pthread_mutex_unlock(&g_lock);
    *n = k;
    return ph;
}

/* ---- the card cache ---- */

/* "<sd>/CATOS/DATA/tba/team_frc5805_events_2026_simple.json": the URL's path under /api/v3, '/' → '_' */
static bool card_path(const ep_t *e, char *out, size_t n)
{
    const char *sd = hal_sd_root();
    const char *p = strstr(e->url, "/api/v3/");
    if (!sd || !p) return false;
    int k = snprintf(out, n, "%s/CATOS/DATA/tba/", sd);
    if (k < 0 || (size_t)k >= n) return false;
    for (p += 8; *p && (size_t)k + 6 < n; p++) out[k++] = *p == '/' ? '_' : *p;
    snprintf(out + k, n - (size_t)k, ".json");
    return true;
}

static void card_save(const ep_t *e)
{
    char path[160], tmp[168];
    if (!card_path(e, path, sizeof path)) return;
    char dir[96];
    snprintf(dir, sizeof dir, "%s/CATOS", hal_sd_root());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/CATOS/DATA", hal_sd_root());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/CATOS/DATA/tba", hal_sd_root());
    mkdir(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(e->body, 1, e->len, f) == e->len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return;
    }
    remove(path); /* FAT won't rename over a file */
    rename(tmp, path);
}

static void card_load(ep_t *e)
{
    char path[160];
    if (!card_path(e, path, sizeof path)) return;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > BODY_MAX) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *b = malloc((size_t)st.st_size + 1);
    size_t n = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!b || n == 0) {
        free(b);
        return;
    }
    b[n] = 0;
    free(e->body);
    e->body = b;
    e->len = n;
    e->lm[0] = 0; /* not from the network: the next request is unconditional */
    e->at = st.st_mtime;
}

/* ---- the network ---- */

/* Points the endpoint at `url`; a new URL drops what the old one had and looks on the card. */
static void ep_aim(ep_t *e, const char *url)
{
    if (!strcmp(e->url, url)) return;
    snprintf(e->url, sizeof e->url, "%s", url);
    free(e->body);
    e->body = NULL;
    e->len = 0;
    e->lm[0] = 0;
    e->at = 0;
    card_load(e);
}

/* GET with If-Modified-Since. 200 replaces the body (and saves it to the card), 304 keeps it. Returns the
 * status, -1 when TBA couldn't be reached or the body didn't arrive whole. */
static int ep_fetch(ep_t *e, const char *key)
{
    /* at least 1.5 s between requests: a poll's four downloads back to back were the burst after which the
     * C6 most often stopped answering */
    static double last;
    double since = hal_seconds() - last;
    if (last > 0 && since < 1.5) usleep((useconds_t)((1.5 - since) * 1e6));
    last = hal_seconds();
    char hdr[288];
    snprintf(hdr, sizeof hdr, "X-TBA-Auth-Key: %s\r\nAccept: application/json\r\n%s%s%s", key,
             e->body && e->lm[0] ? "If-Modified-Since: " : "", e->body && e->lm[0] ? e->lm : "",
             e->body && e->lm[0] ? "\r\n" : "");
    hal_http_req_t rq = { .url = e->url, .headers = hdr, .timeout_ms = 8000 };
    int status = -1;
    char err[64];
    hal_http_t *h = hal_http_open(&rq, &status, err, sizeof err);
    memset(hdr, 0, sizeof hdr); /* the key */
    if (!h) return -1;
    if (status == 200) {
        size_t cap = 16384, n = 0;
        char *b = malloc(cap);
        bool whole = false;
        while (b) {
            if (n + 4096 + 1 > cap) {
                if (cap >= BODY_MAX) break;
                char *nb = realloc(b, cap * 2);
                if (!nb) break;
                b = nb;
                cap *= 2;
            }
            int r = hal_http_read(h, b + n, (int)(cap - n - 1));
            if (r == 0) whole = true;
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (b && whole && n) {
            b[n] = 0;
            free(e->body);
            e->body = b;
            e->len = n;
            e->at = time(NULL);
            snprintf(e->lm, sizeof e->lm, "%s", hal_http_last_modified(h));
            card_save(e);
        } else {
            free(b);
            status = -1;
        }
    } else if (status == 304) {
        e->at = time(NULL);
    }
    hal_http_close(h);
    return status;
}

/* ---- parsing ---- */

/* Tokens sized to the text, with a second, roomier try for number-dense bodies. */
static bool parse(home_json_t *j, const ep_t *e)
{
    memset(j, 0, sizeof *j);
    if (!e->body) return false;
    for (int div = 4; div >= 2; div -= 2) {
        int max = (int)(e->len / (size_t)div) + 64;
        j->toks = malloc((size_t)max * sizeof *j->toks);
        if (!j->toks) return false;
        int n = jl_parse(e->body, e->len, j->toks, max);
        if (n > 0) {
            j->d = (jl_doc_t){ .js = e->body, .t = j->toks, .n = n };
            return true;
        }
        free(j->toks);
        j->toks = NULL;
    }
    return false;
}

static int num(const home_json_t *j, int obj, const char *key, int fallback)
{
    int t = obj < 0 ? -1 : jl_get(&j->d, obj, key);
    if (t < 0 || j->d.t[t].type == JL_NULL) return fallback;
    return (int)jl_num(&j->d, t, fallback);
}

static int team_of(const home_json_t *j, int tok)
{
    char k[16];
    jl_str(&j->d, tok, k, sizeof k);
    return !strncmp(k, "frc", 3) ? atoi(k + 3) : atoi(k);
}

/* Picks the event: the one running today, else the latest past, else the next coming. */
static bool pick_event(const ep_t *e, const char *today, tba_state_t *s)
{
    home_json_t j;
    if (!parse(&j, e)) return false;
    int best = -1, kind = 0; /* 3 today, 2 past, 1 coming */
    char bs[11] = "", be[11] = "";
    if (j.d.t[0].type == JL_ARR) {
        for (int i = 0; i < j.d.t[0].size; i++) {
            int ev = jl_at(&j.d, 0, i);
            char st[11], en[11];
            home_json_str(&j, ev, "start_date", st, sizeof st);
            home_json_str(&j, ev, "end_date", en, sizeof en);
            if (!st[0] || !en[0]) continue;
            int k = strcmp(st, today) <= 0 && strcmp(today, en) <= 0 ? 3 : strcmp(en, today) < 0 ? 2 : 1;
            bool better = k > kind || (k == kind && ((k == 2 && strcmp(en, be) > 0) || (k == 1 && strcmp(st, bs) < 0)));
            if (!better) continue;
            best = ev;
            kind = k;
            snprintf(bs, sizeof bs, "%s", st);
            snprintf(be, sizeof be, "%s", en);
        }
    }
    if (best >= 0) {
        s->have_event = true;
        s->live = kind == 3;
        home_json_str(&j, best, "key", s->event_key, sizeof s->event_key);
        home_json_str(&j, best, "name", s->event_name, sizeof s->event_name);
        char city[40], sp[24], co[24];
        home_json_str(&j, best, "city", city, sizeof city);
        home_json_str(&j, best, "state_prov", sp, sizeof sp);
        home_json_str(&j, best, "country", co, sizeof co);
        snprintf(s->event_where, sizeof s->event_where, "%s%s%s", city, city[0] && (sp[0] || co[0]) ? ", " : "",
                 sp[0] ? sp : co);
        snprintf(s->start, sizeof s->start, "%s", bs);
        snprintf(s->end, sizeof s->end, "%s", be);
    }
    home_json_free(&j);
    return true;
}

static int level_of(const char *cl)
{
    static const char *const L[5] = { "qm", "ef", "qf", "sf", "f" };
    for (int i = 0; i < 5; i++) if (!strcmp(cl, L[i])) return i;
    return 5;
}

static int by_order(const void *a, const void *b)
{
    const tba_match_t *x = a, *y = b;
    if (x->level != y->level) return x->level - y->level;
    if (x->set != y->set) return x->set - y->set;
    return x->number - y->number;
}

static void alliance(const home_json_t *j, int all, int *teams, int *score)
{
    int keys = all >= 0 ? jl_get(&j->d, all, "team_keys") : -1;
    for (int i = 0; i < 3; i++) teams[i] = keys >= 0 && i < j->d.t[keys].size ? team_of(j, jl_at(&j->d, keys, i)) : 0;
    *score = num(j, all, "score", -1);
}

static void parse_matches(const ep_t *e, int team, tba_state_t *s)
{
    home_json_t j;
    if (!parse(&j, e)) return;
    int n = 0;
    if (j.d.t[0].type == JL_ARR) {
        for (int i = 0; i < j.d.t[0].size && n < TBA_MATCHES; i++) {
            int mt = jl_at(&j.d, 0, i);
            tba_match_t *m = &s->matches[n++];
            memset(m, 0, sizeof *m);
            char cl[4];
            home_json_str(&j, mt, "comp_level", cl, sizeof cl);
            m->level = level_of(cl);
            m->set = num(&j, mt, "set_number", 1);
            m->number = num(&j, mt, "match_number", 0);
            int al = jl_get(&j.d, mt, "alliances");
            alliance(&j, al >= 0 ? jl_get(&j.d, al, "red") : -1, m->red, &m->red_score);
            alliance(&j, al >= 0 ? jl_get(&j.d, al, "blue") : -1, m->blue, &m->blue_score);
            for (int k = 0; k < 3; k++) {
                if (m->red[k] == team) m->ours = 1;
                if (m->blue[k] == team) m->ours = 2;
            }
            m->played = m->red_score >= 0 && m->blue_score >= 0;
            char win[8];
            home_json_str(&j, mt, "winning_alliance", win, sizeof win);
            if (m->played && m->ours) {
                const char *us = m->ours == 1 ? "red" : "blue";
                m->result = !win[0] ? 0 : !strcmp(win, us) ? 1 : -1;
            }
            m->time = (time_t)num(&j, mt, "time", 0);
            m->predicted = (time_t)num(&j, mt, "predicted_time", 0);
            /* double elimination numbers its playoff rounds by set ("SF7"), the finals by match ("F2") */
            if (m->level == 0) snprintf(m->label, sizeof m->label, "Q%d", m->number);
            else if (m->level == 4) snprintf(m->label, sizeof m->label, "F%d", m->number);
            else if (m->number <= 1)
                snprintf(m->label, sizeof m->label, "%s%d", m->level == 1 ? "E" : m->level == 2 ? "QF" : "SF", m->set);
            else
                snprintf(m->label, sizeof m->label, "%s%d-%d", m->level == 1 ? "E" : m->level == 2 ? "QF" : "SF", m->set,
                         m->number);
        }
    }
    qsort(s->matches, (size_t)n, sizeof s->matches[0], by_order);
    s->nmatches = n;
    home_json_free(&j);
}

/* HTML (TBA's "<b>Rank 5/40</b>") to plain text: tags dropped, the common entities decoded, spaces folded. */
static void strip_tags(char *s)
{
    static const struct { const char *e; char c; } ENT[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                                                           { "&quot;", '"' }, { "&#39;", '\'' }, { "&nbsp;", ' ' } };
    char *w = s;
    bool tag = false;
    for (char *r = s; *r; r++) {
        if (tag) {
            if (*r == '>') tag = false;
            continue;
        }
        if (*r == '<') {
            tag = true;
            continue;
        }
        char c = *r;
        if (c == '&') {
            for (size_t k = 0; k < sizeof ENT / sizeof ENT[0]; k++) {
                size_t l = strlen(ENT[k].e);
                if (!strncmp(r, ENT[k].e, l)) {
                    c = ENT[k].c;
                    r += l - 1;
                    break;
                }
            }
        }
        if ((c == ' ' || c == '\n' || c == '\t') && (w == s || w[-1] == ' ')) continue;
        *w++ = c == '\n' || c == '\t' ? ' ' : c;
    }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}

static void parse_status(const ep_t *e, tba_state_t *s)
{
    home_json_t j;
    if (!parse(&j, e)) return;
    if (j.d.t[0].type == JL_OBJ) { /* null: not at this event, or nothing known yet */
        s->have_status = true;
        int q = jl_get(&j.d, 0, "qual");
        int rk = q >= 0 ? jl_get(&j.d, q, "ranking") : -1;
        int rec = rk >= 0 ? jl_get(&j.d, rk, "record") : -1;
        s->rank = num(&j, rk, "rank", 0);
        s->num_teams = num(&j, q, "num_teams", 0);
        s->wins = num(&j, rec, "wins", 0);
        s->losses = num(&j, rec, "losses", 0);
        s->ties = num(&j, rec, "ties", 0);
        int al = jl_get(&j.d, 0, "alliance");
        if (al >= 0 && j.d.t[al].type == JL_OBJ) {
            int pick = num(&j, al, "pick", -1), number = num(&j, al, "number", 0);
            static const char *const P[4] = { "captain", "first pick", "second pick", "third pick" };
            snprintf(s->alliance, sizeof s->alliance, "alliance %d%s%s", number, pick >= 0 ? " " : "",
                     pick >= 0 && pick < 4 ? P[pick] : pick >= 4 ? "backup" : "");
        }
        int po = jl_get(&j.d, 0, "playoff");
        if (po >= 0 && j.d.t[po].type == JL_OBJ) {
            char lv[4], st[16];
            home_json_str(&j, po, "level", lv, sizeof lv);
            home_json_str(&j, po, "status", st, sizeof st);
            static const char *const LV[5] = { "quals", "eighths", "quarterfinals", "semifinals", "finals" };
            int l = level_of(lv);
            snprintf(s->playoff, sizeof s->playoff, "playoffs: %s%s%s", st, l < 5 ? " in the " : "",
                     l < 5 ? LV[l] : "");
            if (!strcmp(st, "won")) snprintf(s->playoff, sizeof s->playoff, "won the event");
        }
        home_json_str(&j, 0, "overall_status_str", s->status_line, sizeof s->status_line);
        strip_tags(s->status_line);
    }
    home_json_free(&j);
}

static void parse_ranks(const ep_t *e, tba_state_t *s)
{
    home_json_t j;
    if (!parse(&j, e)) return;
    if (j.d.t[0].type == JL_OBJ) {
        int info = jl_get(&j.d, 0, "sort_order_info");
        int i0 = info >= 0 ? jl_at(&j.d, info, 0) : -1;
        home_json_str(&j, i0, "name", s->sort_name, sizeof s->sort_name);
        for (char *p = s->sort_name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        s->sort_precision = num(&j, i0, "precision", 2);
        int arr = jl_get(&j.d, 0, "rankings");
        int n = 0;
        for (int i = 0; arr >= 0 && j.d.t[arr].type == JL_ARR && i < j.d.t[arr].size && n < TBA_RANKS; i++) {
            int r = jl_at(&j.d, arr, i);
            tba_rank_t *k = &s->ranks[n++];
            k->rank = num(&j, r, "rank", n);
            k->team = team_of(&j, jl_get(&j.d, r, "team_key"));
            int rec = jl_get(&j.d, r, "record");
            k->wins = num(&j, rec, "wins", 0);
            k->losses = num(&j, rec, "losses", 0);
            k->ties = num(&j, rec, "ties", 0);
            k->played = num(&j, r, "matches_played", 0);
            k->dq = num(&j, r, "dq", 0) > 0;
            int so = jl_get(&j.d, r, "sort_orders");
            int s0 = so >= 0 && j.d.t[so].type == JL_ARR ? jl_at(&j.d, so, 0) : -1;
            k->sort1 = jl_num(&j.d, s0, NAN);
        }
        s->nranks = n;
    }
    home_json_free(&j);
}

/* ---- the poll ---- */

static void say(tba_state_t *s, int status)
{
    if (status == 401) snprintf(s->err, sizeof s->err, "the blue alliance refused the key");
    else if (status < 0) snprintf(s->err, sizeof s->err, "the blue alliance isn't answering");
    else snprintf(s->err, sizeof s->err, "the blue alliance answered %d", status);
}

void home_tba_work(double now, bool want)
{
    pthread_mutex_lock(&g_lock);
    tb_t *t = tb();
    if (!t || !want || (!t->kick && now < t->next_poll)) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    char key[128];
    memcpy(key, t->key, sizeof key);
    int team = t->team;
    unsigned gen = t->cfg_gen;
    bool events_due = t->kick || now >= t->next_events, events_fetched = false;
    t->kick = false;
    t->next_poll = now + RETRY_S; /* whatever happens, not again straight away */
    pthread_mutex_unlock(&g_lock);

    tba_state_t *s = &t->work; /* the worker's own: only this thread touches it */
    memset(s, 0, sizeof *s);
    s->team = team;
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    double next = POLL_S;
    int worst = 200;
    if (!key[0]) s->phase = TBA_NO_KEY;
    else if (team <= 0) s->phase = TBA_NO_TEAM;
    else if (tm.tm_year < 120) {
        s->phase = TBA_NO_CLOCK;
        next = NO_CLOCK_S;
    } else {
        s->year = tm.tm_year + 1900;
        char today[11], url[192];
        strftime(today, sizeof today, "%Y-%m-%d", &tm);
        ep_t *ev = &t->ep[EP_EVENTS];
        snprintf(url, sizeof url, BASE "/team/frc%d/events/%d/simple", team, s->year);
        ep_aim(ev, url);
        if (events_due || !ev->body) {
            int st = ep_fetch(ev, key);
            if (st == 200 || st == 304) events_fetched = true;
            else worst = st;
        }
        pick_event(ev, today, s);
        if (s->have_event) {
            for (int i = 0; i < 3 && worst != 401; i++) {
                ep_t *e = &t->ep[EP_MATCHES + i];
                if (i == 0) snprintf(url, sizeof url, BASE "/team/frc%d/event/%s/matches/simple", team, s->event_key);
                else if (i == 1) snprintf(url, sizeof url, BASE "/team/frc%d/event/%s/status", team, s->event_key);
                else snprintf(url, sizeof url, BASE "/event/%s/rankings", s->event_key);
                ep_aim(e, url);
                int st = ep_fetch(e, key);
                if (st != 200 && st != 304 && st != 404) worst = st;
            }
            parse_matches(&t->ep[EP_MATCHES], team, s);
            parse_status(&t->ep[EP_STATUS], s);
            parse_ranks(&t->ep[EP_RANKS], s);
            if (s->live) next = POLL_LIVE_S;
        }
        for (int i = 0; i < EP_N; i++)
            if (t->ep[i].body && t->ep[i].at > s->as_of) s->as_of = t->ep[i].at;
        s->offline = worst != 200;
        if (s->offline) say(s, worst);
        if (worst == 401 && !s->have_event) s->phase = TBA_REFUSED;
        else if (s->have_event) s->phase = TBA_READY;
        else if (ev->body) s->phase = TBA_NO_EVENT;
        else s->phase = worst == 401 ? TBA_REFUSED : TBA_LOADING;
        if (s->offline) next = RETRY_S;
    }

    pthread_mutex_lock(&g_lock);
    if (gen == t->cfg_gen) {
        s->gen = t->st.gen + 1;
        t->st = *s;
        t->next_poll = now + next;
        if (events_fetched) t->next_events = now + EVENTS_S;
    }
    pthread_mutex_unlock(&g_lock);
    memset(key, 0, sizeof key);
}
