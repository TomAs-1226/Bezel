/* link — Catalyst Link on the PC (docs/link-api.md): status polling, mDNS discovery, cached lists, and an
 * outbox on microSD for everything sent while the PC is away.
 *
 * The outbox is one file per request in <sd>/link/outbox/, named by a counter in 8.3 form (FAT without
 * long names still sorts them): "POST <path>\n<json>" or "FILE <name>\n<sd path>". Written as .tmp and
 * renamed, so the poller never sends half a file. */
#include "link.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "as_json.h"
#include "ccwatch.h"
#include "hal.h"

#define POLL_S 3.0
#define MDNS_S 10.0
#define LIST_MAX 32
#define UPLOAD_MAX (12u << 20)

/* This tablet only ever pairs with one PC, so the address is baked in rather than found by mDNS or typed
 * by hand every time: the host and port aren't secrets — the Link already broadcasts both in the clear
 * over mDNS (docs/link-api.md, "Discovery and auth") and the port is documented in this repo. Only the
 * *token* authenticates, and that is never compiled in (this repo is public): it arrives from KEYS.ENV on
 * the SD card (docs/keys.md, LINK_TOKEN) or from pairing, either of which persists to kv and then always
 * wins — link_init() only reaches for this default when kv has no address of its own. A LINK_HOST key in
 * KEYS.ENV can replace the host too, without a rebuild. */
#define LINK_DEFAULT_HOST "ThomasRog.local"
#define LINK_DEFAULT_PORT 8765

static struct {
    pthread_mutex_t lock;
    char url[96], token[80];
    bool discovered;
    link_status_t st;
    link_item_t *inbox, *patches; /* LIST_MAX each, on the heap (static internal RAM is scarce on the P4) */
    int ninbox, npatches;
    double lists_read;     /* when something last read the lists: they refresh only while wanted */
    int outbox;
    bool started;
    volatile bool kick;    /* configuration changed: poll now */
} L = { .lock = PTHREAD_MUTEX_INITIALIZER, .lists_read = -1e9 };

static pthread_mutex_t g_outbox = PTHREAD_MUTEX_INITIALIZER; /* one flusher / writer at a time */

/* Pairing and discovery: asked for by the UI, done on the poller's thread (under L.lock). On the heap. */
#define PEERS_MAX 6
typedef struct {
    link_pair_t st;
    char id[40], code[8], device[48];
    char token[80];        /* the result, until link_pair_take() */
    bool want_start, want_code, taken;
    bool disc_req, disc_busy;
    link_peer_t peers[PEERS_MAX];
    int npeers;
} pair_t;
static pair_t *P;

/* "192.168.1.20" → "http://192.168.1.20:8765"; a trailing slash goes */
static void normalize(const char *in, char *out, size_t n)
{
    while (in && (*in == ' ' || *in == '\t')) in++;
    if (!in || !*in) {
        out[0] = 0;
        return;
    }
    bool scheme = !strncmp(in, "http://", 7) || !strncmp(in, "https://", 8);
    const char *host = scheme ? strstr(in, "://") + 3 : in;
    bool port = strchr(host, ':') != NULL;
    snprintf(out, n, "%s%s%s", scheme ? "" : "http://", in, port ? "" : ":8765");
    size_t l = strlen(out);
    while (l && (out[l - 1] == '/' || out[l - 1] == ' ' || out[l - 1] == '\n')) out[--l] = 0;
}

static int request(const char *method, const char *path, const char *ctype, const char *body, size_t blen, char *out,
                   int max, int timeout_ms)
{
    char base[96], token[80], url[512], hdr[200], scratch[512];
    pthread_mutex_lock(&L.lock);
    snprintf(base, sizeof base, "%s", L.url);
    snprintf(token, sizeof token, "%s", L.token);
    pthread_mutex_unlock(&L.lock);
    if (!out || max <= 0) {
        out = scratch;
        max = sizeof scratch;
    }
    out[0] = 0;
    if (!base[0]) return -1;
    snprintf(url, sizeof url, "%s%s", base, path);
    snprintf(hdr, sizeof hdr, "X-Link-Token: %s\r\nContent-Type: %s\r\nAccept: application/json\r\n", token,
             ctype ? ctype : "application/json");
    hal_http_req_t rq = { .method = method, .url = url, .headers = hdr, .body = body, .body_len = blen,
                          .timeout_ms = timeout_ms };
    int len;
    return hal_http_fetch(&rq, out, max, &len);
}

int link_get(const char *path, char *out, int max) { return request("GET", path, NULL, NULL, 0, out, max, 15000); }

/* ---- pairing (on the poller's thread) ---- */

static void pair_say(link_pair_state_t state, const char *msg)
{
    P->st.state = state;
    snprintf(P->st.msg, sizeof P->st.msg, "%s", msg);
    P->st.gen++;
}

/* POST without a token, to the Link being paired (not necessarily the configured one) */
static int pair_post(const char *base, const char *path, const char *json, char *out, int max)
{
    char url[160];
    snprintf(url, sizeof url, "%s%s", base, path);
    hal_http_req_t rq = { .method = "POST", .url = url,
                          .headers = "Content-Type: application/json\r\nAccept: application/json\r\n",
                          .body = json, .body_len = strlen(json), .timeout_ms = 6000 };
    int len;
    return hal_http_fetch(&rq, out, max, &len);
}

static void pair_discover(void)
{
    hal_service_t *sv = calloc(PEERS_MAX, sizeof *sv);
    int n = sv ? hal_mdns_browse("_catalyst-link", "_tcp", sv, PEERS_MAX, 2000) : 0;
    pthread_mutex_lock(&L.lock);
    P->npeers = 0;
    for (int i = 0; i < n; i++) {
        link_peer_t pe;
        snprintf(pe.url, sizeof pe.url, "http://%s:%d", sv[i].ip[0] ? sv[i].ip : sv[i].host, sv[i].port ? sv[i].port : 8765);
        snprintf(pe.name, sizeof pe.name, "%s", sv[i].name[0] ? sv[i].name : sv[i].host);
        bool dup = false;
        for (int k = 0; k < P->npeers; k++) dup = dup || !strcmp(P->peers[k].url, pe.url);
        if (!dup) P->peers[P->npeers++] = pe;
    }
    P->disc_busy = false;
    pthread_mutex_unlock(&L.lock);
    free(sv);
}

static void pair_begin(const char *base, const char *device)
{
    char body[128], dev[48];
    /* the device's name goes into JSON: plain characters only */
    size_t o = 0;
    for (const char *c = device; *c && o + 1 < sizeof dev; c++)
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ' || *c == '-')
            dev[o++] = *c;
    dev[o] = 0;
    snprintf(body, sizeof body, "{\"device\":\"%s\"}", dev);
    char *buf = malloc(1024);
    int st = buf ? pair_post(base, "/link/pair", body, buf, 1024) : -1;
    aj_t *d = st == 200 ? aj_parse(buf, strlen(buf), NULL, 0) : NULL;
    pthread_mutex_lock(&L.lock);
    const char *id = d ? aj_gets(d, "pairing") : NULL;
    if (id && id[0]) {
        snprintf(P->id, sizeof P->id, "%s", id);
        const char *nm = aj_gets(d, "name");
        if (nm && nm[0]) snprintf(P->st.name, sizeof P->st.name, "%s", nm);
        P->st.tries_left = 0;
        pair_say(LINK_PAIR_CODE, "type the code the pc shows");
    } else if (st == 403) {
        pair_say(LINK_PAIR_FAILED, "pairing is off on that pc: type its token instead");
    } else if (st == 429) {
        pair_say(LINK_PAIR_FAILED, "the pc asks to wait a moment: try again");
    } else if (st == 404 || st == 405) {
        pair_say(LINK_PAIR_FAILED, "that catalyst link is older: update it, or type its token");
    } else if (st < 0) {
        pair_say(LINK_PAIR_FAILED, "the pc isn't answering");
    } else {
        char m[48];
        snprintf(m, sizeof m, "the pc answered %d", st);
        pair_say(LINK_PAIR_FAILED, m);
    }
    pthread_mutex_unlock(&L.lock);
    aj_free(d);
    free(buf);
}

static void pair_confirm(const char *base, const char *id, const char *code)
{
    char body[128];
    snprintf(body, sizeof body, "{\"pairing\":\"%s\",\"code\":\"%s\"}", id, code);
    char *buf = malloc(1024);
    int st = buf ? pair_post(base, "/link/pair/confirm", body, buf, 1024) : -1;
    aj_t *d = st > 0 ? aj_parse(buf, strlen(buf), NULL, 0) : NULL;
    pthread_mutex_lock(&L.lock);
    const char *tok = st == 200 && d ? aj_gets(d, "token") : NULL;
    if (tok && tok[0] && strlen(tok) < sizeof P->token) {
        snprintf(P->token, sizeof P->token, "%s", tok);
        const char *nm = aj_gets(d, "name");
        if (nm && nm[0]) snprintf(P->st.name, sizeof P->st.name, "%s", nm);
        P->taken = false;
        pair_say(LINK_PAIR_DONE, "paired");
    } else if (st == 403) {
        int left = d ? (int)aj_getn(d, "attempts_left", 0) : 0;
        P->st.tries_left = left;
        char m[64];
        snprintf(m, sizeof m, "that isn't the code: %d %s left", left, left == 1 ? "try" : "tries");
        pair_say(LINK_PAIR_CODE, m);
    } else if (st == 410) {
        pair_say(LINK_PAIR_FAILED, "that code has run out: start again for a new one");
    } else {
        pair_say(LINK_PAIR_FAILED, st < 0 ? "the pc isn't answering" : "the pc refused the code");
    }
    pthread_mutex_unlock(&L.lock);
    aj_free(d); /* the token was in there, and in buf: neither outlives this */
    if (buf) memset(buf, 0, 1024);
    free(buf);
}

static void pair_work(void)
{
    if (!P) return;
    char url[96], dev[48], id[40], code[8];
    pthread_mutex_lock(&L.lock);
    bool disc = P->disc_req, start = P->want_start, confirm = P->want_code;
    P->disc_req = P->want_start = P->want_code = false;
    snprintf(url, sizeof url, "%s", P->st.url);
    snprintf(dev, sizeof dev, "%s", P->device);
    snprintf(id, sizeof id, "%s", P->id);
    snprintf(code, sizeof code, "%s", P->code);
    pthread_mutex_unlock(&L.lock);
    if (disc) pair_discover();
    if (start) pair_begin(url, dev);
    if (confirm) pair_confirm(url, id, code);
}

void link_discover(void)
{
    if (!P) return;
    pthread_mutex_lock(&L.lock);
    P->disc_req = true;
    P->disc_busy = true;
    pthread_mutex_unlock(&L.lock);
    L.kick = true;
}

int link_peers(link_peer_t *out, int max, bool *busy)
{
    if (!P) {
        if (busy) *busy = false;
        return 0;
    }
    pthread_mutex_lock(&L.lock);
    int n = P->npeers < max ? P->npeers : max;
    memcpy(out, P->peers, (size_t)(n > 0 ? n : 0) * sizeof *out);
    if (busy) *busy = P->disc_busy;
    pthread_mutex_unlock(&L.lock);
    return n;
}

void link_pair_start(const char *url, const char *device)
{
    if (!P) return;
    pthread_mutex_lock(&L.lock);
    normalize(url, P->st.url, sizeof P->st.url);
    snprintf(P->device, sizeof P->device, "%s", device ? device : "catalyst tab");
    P->st.name[0] = 0;
    P->id[0] = 0;
    P->want_start = true;
    P->want_code = false;
    pair_say(LINK_PAIR_ASKING, "asking the pc for a code");
    pthread_mutex_unlock(&L.lock);
    L.kick = true;
}

void link_pair_code(const char *code)
{
    if (!P) return;
    pthread_mutex_lock(&L.lock);
    if (P->st.state == LINK_PAIR_CODE && P->id[0]) {
        size_t o = 0;
        for (const char *c = code; *c && o + 1 < sizeof P->code; c++)
            if (*c >= '0' && *c <= '9') P->code[o++] = *c;
        P->code[o] = 0;
        P->want_code = true;
        pair_say(LINK_PAIR_CHECKING, "checking the code");
    }
    pthread_mutex_unlock(&L.lock);
    L.kick = true;
}

void link_pair_cancel(void)
{
    if (!P) return;
    pthread_mutex_lock(&L.lock);
    P->want_start = P->want_code = false;
    P->id[0] = 0;
    memset(P->token, 0, sizeof P->token);
    pair_say(LINK_PAIR_IDLE, "");
    pthread_mutex_unlock(&L.lock);
}

void link_pair_status(link_pair_t *out)
{
    if (!P) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&L.lock);
    *out = P->st;
    pthread_mutex_unlock(&L.lock);
}

bool link_pair_take(char *url, size_t un, char *token, size_t tn, char *name, size_t nn)
{
    if (!P) return false;
    pthread_mutex_lock(&L.lock);
    bool ok = P->st.state == LINK_PAIR_DONE && !P->taken && P->token[0];
    if (ok) {
        snprintf(url, un, "%s", P->st.url);
        snprintf(token, tn, "%s", P->token);
        snprintf(name, nn, "%s", P->st.name);
        memset(P->token, 0, sizeof P->token);
        P->taken = true;
    }
    pthread_mutex_unlock(&L.lock);
    return ok;
}

int link_post(const char *path, const char *json, char *out, int max)
{
    /* a patch runs the Link's compile check before it answers */
    int timeout = !strncmp(path, "/code/patch", 11) ? 180000 : 15000;
    return request("POST", path, NULL, json, json ? strlen(json) : 0, out, max, timeout);
}

/* ---- outbox ---- */

static bool outbox_dir(char *out, size_t n)
{
    const char *root = hal_sd_root();
    if (!root) return false;
    char d[160];
    snprintf(d, sizeof d, "%s/link", root);
    mkdir(d, 0755);
    snprintf(out, n, "%s/link/outbox", root);
    mkdir(out, 0755);
    return true;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The queued requests' names, oldest first. Caller frees each and the array. */
static int outbox_list(char ***names)
{
    char dir[180];
    *names = NULL;
    if (!outbox_dir(dir, sizeof dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".req") != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **g = realloc(*names, (size_t)cap * sizeof *g);
            if (!g) break;
            *names = g;
        }
        (*names)[n++] = as_strdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(*names, (size_t)n, sizeof **names, cmp_names);
    return n;
}

static void free_list(char **names, int n)
{
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

static bool enqueue(const char *head, const char *body)
{
    char dir[180], tmp[220], final[220];
    if (!outbox_dir(dir, sizeof dir)) return false;
    pthread_mutex_lock(&g_outbox);
    char **names;
    int n = outbox_list(&names);
    unsigned next = n ? (unsigned)strtoul(names[n - 1], NULL, 16) + 1 : 1;
    free_list(names, n);
    snprintf(tmp, sizeof tmp, "%s/%08X.tmp", dir, next);
    snprintf(final, sizeof final, "%s/%08X.req", dir, next);
    FILE *f = fopen(tmp, "w");
    bool ok = f && fputs(head, f) >= 0 && fputc('\n', f) != EOF && fputs(body, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, final) == 0;
    else remove(tmp);
    if (ok) {
        pthread_mutex_lock(&L.lock);
        L.outbox = n + 1;
        pthread_mutex_unlock(&L.lock);
    }
    pthread_mutex_unlock(&g_outbox);
    return ok;
}

static char *read_file(const char *path, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > max) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[sz] = 0;
        *len = (size_t)sz;
    }
    return b;
}

static int upload_now(const char *sd_path, const char *name)
{
    size_t len = 0;
    char *data = read_file(sd_path, &len, UPLOAD_MAX);
    if (!data) return 400; /* gone or too big: nothing a retry fixes */
    char enc[200], path[240];
    as_urlencode(name, enc, sizeof enc);
    snprintf(path, sizeof path, "/files?name=%s", enc);
    int st = request("POST", path, "application/octet-stream", data, len, NULL, 0, 60000);
    free(data);
    return st;
}

/* reachable and taking our token: only then is anything sent (a wrong token would reject it all) */
static bool reachable_now(void)
{
    pthread_mutex_lock(&L.lock);
    bool ok = L.st.reachable && L.st.auth && hal_seconds() - L.st.last_ok < 10;
    pthread_mutex_unlock(&L.lock);
    return ok;
}

/* Sends what waits, oldest first; stops at the first request the Link doesn't take for now. */
static void flush_outbox(void)
{
    char dir[180];
    if (!outbox_dir(dir, sizeof dir)) return;
    pthread_mutex_lock(&g_outbox);
    char **names;
    int n = outbox_list(&names), left = n;
    for (int i = 0; i < n; i++) {
        char file[220];
        snprintf(file, sizeof file, "%s/%s", dir, names[i]);
        size_t len;
        char *req = read_file(file, &len, 1u << 20);
        char *nl = req ? strchr(req, '\n') : NULL;
        int st = 400;
        if (nl) {
            *nl = 0;
            if (!strncmp(req, "POST ", 5)) st = request("POST", req + 5, NULL, nl + 1, strlen(nl + 1), NULL, 0, 30000);
            else if (!strncmp(req, "FILE ", 5)) st = upload_now(nl + 1, req + 5);
        }
        free(req);
        /* still away, unwell, or the token changed under us: try again later */
        if (st < 0 || st >= 500 || st == 401) break;
        if (st >= 200 && st < 300) remove(file); /* a re-send the Link already had answers "duplicate": fine */
        else {                               /* refused: set it aside rather than block the queue */
            char bad[224];
            snprintf(bad, sizeof bad, "%.*s.bad", (int)(strlen(file) - 4), file);
            rename(file, bad);
        }
        left--;
    }
    free_list(names, n);
    pthread_mutex_lock(&L.lock);
    L.outbox = left;
    pthread_mutex_unlock(&L.lock);
    pthread_mutex_unlock(&g_outbox);
}

bool link_post_queued(const char *path, const char *json)
{
    if (reachable_now()) {
        int st = link_post(path, json, NULL, 0);
        if (st >= 200 && st < 300) return true;
        if (st >= 400 && st < 500 && st != 401) return false;
    }
    char head[160];
    snprintf(head, sizeof head, "POST %s", path);
    return enqueue(head, json);
}

bool link_upload(const char *sd_path, const char *name)
{
    if (reachable_now()) {
        int st = upload_now(sd_path, name);
        if (st >= 200 && st < 300) return true;
        if (st >= 400 && st < 500 && st != 401) return false;
    }
    char head[160];
    snprintf(head, sizeof head, "FILE %s", name);
    return enqueue(head, sd_path);
}

int link_outbox_count(void)
{
    pthread_mutex_lock(&L.lock);
    int n = L.outbox;
    pthread_mutex_unlock(&L.lock);
    return n;
}

/* ---- status and lists ---- */

static void copy_str(char *out, size_t n, const aj_t *o, const char *key)
{
    const char *s = aj_gets(o, key);
    snprintf(out, n, "%s", s ? s : "");
}

static void poll_status(void)
{
    char *buf = malloc(4096);
    if (!buf) return;
    /* a short timeout: a dead address mustn't hold pairing up on this thread for long */
    int st = request("GET", "/link/status", NULL, NULL, 0, buf, 4096, 5000);
    aj_t *d = st == 200 ? aj_parse(buf, strlen(buf), NULL, 0) : NULL;
    pthread_mutex_lock(&L.lock);
    L.st.reachable = d && aj_is(aj_get(d, "ok"), AJ_TRUE);
    if (L.st.reachable) {
        L.st.last_ok = hal_seconds();
        /* without a good token the Link answers only its name, with auth false */
        L.st.auth = aj_is(aj_get(d, "auth"), AJ_TRUE);
        L.st.claude = aj_is(aj_get(d, "claude"), AJ_TRUE) && L.st.auth;
        copy_str(L.st.claude_via, sizeof L.st.claude_via, d, "claude_via");
        copy_str(L.st.name, sizeof L.st.name, d, "name");
        copy_str(L.st.repo, sizeof L.st.repo, d, "repo");
        copy_str(L.st.branch, sizeof L.st.branch, d, "branch");
        L.st.inbox_open = (int)aj_getn(d, "inbox_open", 0);
        L.st.patches = (int)aj_getn(d, "patches", 0);
    }
    pthread_mutex_unlock(&L.lock);
    aj_free(d);
    free(buf);
}

static int parse_items(const char *body, const char *list, link_item_t *out, int max)
{
    aj_t *d = aj_parse(body, strlen(body), NULL, 0);
    const aj_t *a = aj_get(d, list);
    int n = 0;
    for (int i = 0; a && i < a->n && n < max; i++) {
        const aj_t *o = a->kid[i];
        link_item_t *it = &out[n++];
        memset(it, 0, sizeof *it);
        copy_str(it->id, sizeof it->id, o, "id");
        copy_str(it->title, sizeof it->title, o, "title");
        copy_str(it->status, sizeof it->status, o, "status");
        copy_str(it->branch, sizeof it->branch, o, "branch");
        copy_str(it->when, sizeof it->when, o, "when");
        copy_str(it->check, sizeof it->check, o, "check"); /* a word in the patches list */
    }
    aj_free(d);
    return n;
}

static void refresh_lists(void)
{
    enum { N = 32 * 1024 };
    char *buf = malloc(N);
    link_item_t *tmp = malloc(LIST_MAX * sizeof *tmp);
    if (buf && tmp) {
        if (link_get("/inbox?status=open", buf, N) == 200) {
            int n = parse_items(buf, "items", tmp, LIST_MAX);
            pthread_mutex_lock(&L.lock);
            if (L.inbox) memcpy(L.inbox, tmp, (size_t)n * sizeof *tmp);
            L.ninbox = n;
            pthread_mutex_unlock(&L.lock);
        }
        if (link_get("/code/patches", buf, N) == 200) {
            int n = parse_items(buf, "patches", tmp, LIST_MAX);
            pthread_mutex_lock(&L.lock);
            if (L.patches) memcpy(L.patches, tmp, (size_t)n * sizeof *tmp);
            L.npatches = n;
            pthread_mutex_unlock(&L.lock);
        }
    }
    free(buf);
    free(tmp);
}

static void *poller(void *arg)
{
    (void)arg;
    double last_mdns = -1e9;
    for (;;) {
        pair_work(); /* first: someone is waiting at the screen for it */
        pthread_mutex_lock(&L.lock);
        bool have = L.url[0] != 0;
        bool want_lists = hal_seconds() - L.lists_read < 30;
        pthread_mutex_unlock(&L.lock);
        if (!have && hal_seconds() - last_mdns > MDNS_S) {
            last_mdns = hal_seconds();
            hal_service_t sv;
            if (hal_mdns_browse("_catalyst-link", "_tcp", &sv, 1, 1500) > 0) {
                pthread_mutex_lock(&L.lock);
                if (!L.url[0]) {
                    snprintf(L.url, sizeof L.url, "http://%s:%d", sv.ip[0] ? sv.ip : sv.host, sv.port ? sv.port : 8765);
                    L.discovered = true;
                    if (sv.name[0]) snprintf(L.st.name, sizeof L.st.name, "%s", sv.name);
                }
                pthread_mutex_unlock(&L.lock);
                have = true;
            }
        }
        if (have) {
            pair_work();
            poll_status();
            if (reachable_now()) {
                flush_outbox();
                if (want_lists) refresh_lists();
                ccw_poll(); /* Claude Code on the PC: the companion's Claude panel and its reminders */
            }
        }
        if (!reachable_now()) {
            ccw_offline();
            /* keep the count honest while nothing can be sent */
            pthread_mutex_lock(&g_outbox);
            char **names;
            int n = outbox_list(&names);
            free_list(names, n);
            pthread_mutex_unlock(&g_outbox);
            pthread_mutex_lock(&L.lock);
            L.outbox = n;
            pthread_mutex_unlock(&L.lock);
        }
        for (int i = 0; i < (int)(POLL_S * 10) && !L.kick; i++) usleep(100000);
        L.kick = false;
    }
    return NULL;
}

void link_init(void)
{
    char url[96] = "", token[80] = "";
    hal_kv_get("link_url", url, sizeof url);
    hal_kv_get("link_token", token, sizeof token);
    /* nothing saved (never paired, no KEYS.ENV yet): the compiled default, so the tablet never needs
     * mDNS discovery or the "pair the pc" screen to reach the one PC it will ever talk to. A saved
     * address — from pairing, from typing one in settings, or from a KEYS.ENV LINK_HOST — always wins. */
    if (!url[0]) snprintf(url, sizeof url, "%s:%d", LINK_DEFAULT_HOST, LINK_DEFAULT_PORT);
    pthread_mutex_lock(&L.lock);
    if (!P) P = calloc(1, sizeof *P);
    if (!L.inbox) {
        L.inbox = calloc(LIST_MAX, sizeof *L.inbox);
        L.patches = calloc(LIST_MAX, sizeof *L.patches);
    }
    bool start = !L.started && L.inbox && L.patches;
    L.started = start || L.started;
    if (!L.url[0]) normalize(url, L.url, sizeof L.url);
    if (!L.token[0]) snprintf(L.token, sizeof L.token, "%s", token);
    pthread_mutex_unlock(&L.lock);
    if (start) hal_thread("link", poller, NULL, 10 * 1024); /* ~3.5 KB used at most, measured */
}

void link_configure(const char *url, const char *token)
{
    pthread_mutex_lock(&L.lock);
    normalize(url, L.url, sizeof L.url);
    snprintf(L.token, sizeof L.token, "%s", token ? token : "");
    L.discovered = false;
    L.st.reachable = false;
    L.st.auth = false;
    L.st.claude = false;
    L.st.claude_via[0] = 0;
    L.st.last_ok = 0;
    pthread_mutex_unlock(&L.lock);
    L.kick = true;
}

void link_status(link_status_t *out)
{
    pthread_mutex_lock(&L.lock);
    *out = L.st;
    out->configured = L.url[0] != 0;
    snprintf(out->url, sizeof out->url, "%s", L.url);
    out->reachable = L.st.reachable && hal_seconds() - L.st.last_ok < 10;
    if (!out->reachable) out->claude = out->auth = false;
    out->outbox = L.outbox;
    pthread_mutex_unlock(&L.lock);
}

int link_inbox(link_item_t *out, int max)
{
    pthread_mutex_lock(&L.lock);
    L.lists_read = hal_seconds();
    int n = L.ninbox < max ? L.ninbox : max;
    if (L.inbox) memcpy(out, L.inbox, (size_t)(n > 0 ? n : 0) * sizeof *out);
    else n = 0;
    pthread_mutex_unlock(&L.lock);
    return n;
}

int link_patches(link_item_t *out, int max)
{
    pthread_mutex_lock(&L.lock);
    L.lists_read = hal_seconds();
    int n = L.npatches < max ? L.npatches : max;
    if (L.patches) memcpy(out, L.patches, (size_t)(n > 0 ? n : 0) * sizeof *out);
    else n = 0;
    pthread_mutex_unlock(&L.lock);
    return n;
}

bool link_messages_endpoint(char *url, size_t n, char *headers, size_t hn)
{
    pthread_mutex_lock(&L.lock);
    bool ok = L.url[0] != 0;
    if (ok) {
        snprintf(url, n, "%s/v1/messages", L.url);
        snprintf(headers, hn, "X-Link-Token: %s\r\n", L.token);
    }
    pthread_mutex_unlock(&L.lock);
    return ok;
}
