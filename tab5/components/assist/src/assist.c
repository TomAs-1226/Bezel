/* assist — the conversation loop, on its own worker thread, and the state the UI reads.
 *
 * The UI thread calls assist_feed() with each model update, assist_send() with what the technician typed,
 * and reads the transcript under assist_lock(). The worker owns the history: it streams a response
 * (as_sse), decides from the stop reason what may happen next (as_conv), runs tool calls (as_tools, with
 * confirmation cards for anything that changes the robot or the code), sends every result of a turn back
 * in one message, and repeats until the answer is in. */
#include "assist.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "as_conv.h"
#include "as_oai.h"
#include "as_snap.h"
#include "as_sse.h"
#include "as_tools.h"
#include "hal.h"
#include "link.h"
#include "voice.h"

#define MAX_ENTRIES 400
#define CONFIRM_S 90.0
#define MAX_ROUNDS 24      /* model round trips per question: a runaway loop stops here */
#define MAX_BLOCKS 128
#define READ_TIMEOUT_MS 120000

static const char SYSTEM_PROMPT[] =
    "You are the pit technician built into Catalyst Tab, a handheld diagnostics tablet for FRC robots running "
    "FrcCatalyst on Systemcore (2.x) or a roboRIO (1.x). The person talking to you is standing at the robot cart "
    "in the pit, often minutes before a match.\n"
    "\n"
    "How to work:\n"
    "- Gather evidence with your tools before concluding: start broad (robot_overview, get_alerts), then narrow. "
    "Say what the data shows and how sure you are.\n"
    "- Answer short and concrete: the likely cause, the evidence, and the next physical thing to check or do. "
    "Plain text, a few bullets at most; the screen is small and the reader is busy.\n"
    "- Diagnostic routines (system check, zero wheels, find motor, wheel-radius calibration, SysId) are Driver "
    "Station Utility op modes. When one is needed, name the op mode for the technician to run; you can't run it.\n"
    "- Never say you changed, sent or verified something unless a tool result says so.\n"
    "\n"
    "What you can change, and how (the safety model):\n"
    "- You never command the robot: no enabling, no motion, no deploys, no routines. Catalyst exposes no command "
    "topics and neither do your tools.\n"
    "- Reading is free. set_tunable, select_auto, revert_snapshot and propose_patch each put a card on the tablet's "
    "screen, and nothing happens unless the technician approves it. A declined or unanswered card comes back as "
    "\"The technician declined this change\": accept that and carry on without it.\n"
    "- Before any tunable write the tablet snapshots every tunable, so revert_snapshot can put them back.\n"
    "- Code changes never touch the robot or the branch the team has checked out: propose_patch has Catalyst Link "
    "on the team's PC commit them on a new branch in its own worktree, not pushed and not deployed. "
    "create_work_order puts a problem in the PC's inbox, where the team's main coding agent works through it "
    "asynchronously; attach the patch id and any recordings or logs from the tablet's card. If the PC is "
    "unreachable, both wait on the tablet and go when it's back.\n"
    "- When the fix belongs in the code, prefer proposing a patch plus a work order over asking someone to edit "
    "code by hand at the cart. Keep patches minimal and exact: read the file first (code_search, code_read) so "
    "each edit's old text occurs exactly once.\n"
    "\n"
    "A technician message may start with a bracketed context note from the tablet (team number, connection "
    "state); the tablet writes it, not the technician.";

typedef struct {
    as_entry_t e;
    char *buf;
    size_t len, cap;
    char tool[32];
} ent_t;

static struct {
    pthread_mutex_t lock;          /* everything below */
    pthread_cond_t wake;
    ent_t *ent;
    int n, cap, dropped;           /* entries are addressed by absolute id: index + dropped */
    as_phase_t phase;
    as_confirm_t confirm;
    int answer;                    /* 0 waiting, 1 approved, 2 declined */
    as_usage_t usage;
    assist_config_t cfg;
    char base[128];                /* the direct route's base URL (tests point it at a fake) */
    char oai_base[128];            /* the OpenAI route's ("oai_base"; "" → https://api.openai.com) */
    char *pending;
    void (*job)(void *);           /* assist_post_job's: run by the worker between turns */
    void *job_arg;
    bool busy, stop, started;
    bool running;                  /* the worker is in an exchange (busy also counts a question not yet taken) */
    uint32_t gen;                 /* bumps on assist_reset: a stale worker's writes are dropped */
    int team;
    char sugg[5][72];
} A = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static pthread_mutex_t g_robot_lock = PTHREAD_MUTEX_INITIALIZER;
static cat_robot_t *g_fed;
static bool g_have_fed;
static uint32_t g_rev;

static void bump(void) { __atomic_add_fetch(&g_rev, 1, __ATOMIC_RELAXED); }
uint32_t assist_rev(void) { return __atomic_load_n(&g_rev, __ATOMIC_RELAXED); }

/* ---- the transcript (A.lock held) ---- */

static ent_t *ent_get(int id)
{
    int i = id - A.dropped;
    return id >= 0 && i >= 0 && i < A.n ? &A.ent[i] : NULL;
}

static void ent_text(ent_t *e, const char *s, size_t n, bool append)
{
    if (!append) e->len = 0;
    if (e->len + n + 1 > e->cap) {
        size_t cap = e->cap ? e->cap : 64;
        while (cap < e->len + n + 1) cap *= 2;
        char *p = realloc(e->buf, cap);
        if (!p) return;
        e->buf = p;
        e->cap = cap;
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = 0;
    e->e.text = e->buf;
    e->e.rev++;
}

static int ent_add(as_entry_kind_t kind, const char *text, const char *tool)
{
    if (A.n == MAX_ENTRIES) {
        /* the oldest goes; ids stay stable */
        free(A.ent[0].buf);
        memmove(A.ent, A.ent + 1, (size_t)(A.n - 1) * sizeof *A.ent);
        A.n--;
        A.dropped++;
        for (int i = 0; i < A.n; i++) {
            A.ent[i].e.text = A.ent[i].buf ? A.ent[i].buf : "";
            A.ent[i].e.tool = A.ent[i].tool;
        }
    }
    if (A.n == A.cap) {
        int cap = A.cap ? A.cap * 2 : 32;
        ent_t *p = realloc(A.ent, (size_t)cap * sizeof *p);
        if (!p) return -1;
        A.ent = p;
        A.cap = cap;
        for (int i = 0; i < A.n; i++) {
            A.ent[i].e.text = A.ent[i].buf ? A.ent[i].buf : "";
            A.ent[i].e.tool = A.ent[i].tool;
        }
    }
    ent_t *e = &A.ent[A.n++];
    memset(e, 0, sizeof *e);
    e->e.kind = kind;
    e->e.text = "";
    snprintf(e->tool, sizeof e->tool, "%s", tool ? tool : "");
    e->e.tool = e->tool;
    if (text) ent_text(e, text, strlen(text), false);
    return A.dropped + A.n - 1;
}

static void clear_locked(void)
{
    for (int i = 0; i < A.n; i++) free(A.ent[i].buf);
    A.dropped += A.n;
    A.n = 0;
}

/* ---- the worker's side: every write checks it still belongs to the current conversation ---- */

typedef struct {
    uint32_t gen;
    int ent[MAX_BLOCKS];           /* stream block index → transcript entry id */
    int cur;                       /* the tool entry being run */
    cat_robot_t *robot;
    bool fallback_noted;
} wctx_t;

/* This exchange still owns the transcript's end and the phase: not reset, and not stopped. A stopped exchange
 * may go on waiting on the network for a while; the technician already saw "Stopped." and may have asked
 * again, so nothing new of it shows (its rows already shown can still be marked). Under A.lock. */
static bool mine(const wctx_t *c) { return A.gen == c->gen && !A.stop; }

static int w_add(wctx_t *c, as_entry_kind_t kind, const char *text, const char *tool)
{
    pthread_mutex_lock(&A.lock);
    int id = mine(c) ? ent_add(kind, text, tool) : -1;
    pthread_mutex_unlock(&A.lock);
    bump();
    return id;
}

static void w_note(wctx_t *c, const char *text) { w_add(c, AS_E_NOTE, text, NULL); }
static void cb_note(void *user, const char *text) { w_note(user, text); }

static void w_set(wctx_t *c, int id, const char *text, int state)
{
    pthread_mutex_lock(&A.lock);
    ent_t *e = A.gen == c->gen ? ent_get(id) : NULL;
    if (e) {
        if (text) ent_text(e, text, strlen(text), false);
        if (state >= 0) {
            e->e.tool_state = (as_tool_state_t)state;
            e->e.rev++;
        }
    }
    pthread_mutex_unlock(&A.lock);
    bump();
}

static void w_phase(wctx_t *c, as_phase_t p)
{
    pthread_mutex_lock(&A.lock);
    if (mine(c)) A.phase = p;
    pthread_mutex_unlock(&A.lock);
    bump();
}

static bool w_stopped(void *user)
{
    wctx_t *c = user;
    pthread_mutex_lock(&A.lock);
    bool s = A.stop || A.gen != c->gen;
    pthread_mutex_unlock(&A.lock);
    return s;
}

/* ---- the robot, as fed ---- */

static void refresh(cat_robot_t *r)
{
    pthread_mutex_lock(&g_robot_lock);
    if (g_have_fed) memcpy(r, g_fed, sizeof *r);
    else {
        memset(r, 0, sizeof *r);
        r->battery_v = r->match_time = r->heading_deg = NAN;
        r->tag_seen = -1;
    }
    pthread_mutex_unlock(&g_robot_lock);
}

void assist_robot(cat_robot_t *out) { refresh(out); }

void assist_feed(const cat_robot_t *r)
{
    pthread_mutex_lock(&g_robot_lock);
    if (!g_fed) g_fed = malloc(sizeof *g_fed);
    if (g_fed) {
        memcpy(g_fed, r, sizeof *g_fed);
        g_have_fed = true;
    }
    pthread_mutex_unlock(&g_robot_lock);
}

/* ---- streaming callbacks: the transcript grows as the response arrives ---- */

static void cb_start(void *user, int index, const aj_t *block)
{
    wctx_t *c = user;
    if (index < 0 || index >= MAX_BLOCKS) return;
    const char *t = aj_gets(block, "type");
    c->ent[index] = -1;
    if (!t) return;
    if (!strcmp(t, "tool_use")) {
        c->ent[index] = w_add(c, AS_E_TOOL, "…", aj_gets(block, "name"));
        w_phase(c, AS_PHASE_TOOL);
    } else if (!strcmp(t, "fallback") && !c->fallback_noted) {
        char n[160];
        const char *to = aj_gets(aj_get(block, "to"), "model");
        snprintf(n, sizeof n, "Switched to %s mid-answer (a server-side fallback after a safety decline).", to ? to : "another model");
        w_note(c, n);
        c->fallback_noted = true;
    } else if (!strcmp(t, "thinking")) {
        w_phase(c, AS_PHASE_THINKING);
    }
}

static void cb_delta(void *user, int index, as_delta_t kind, const char *s, size_t n)
{
    wctx_t *c = user;
    if (index < 0 || index >= MAX_BLOCKS || kind == AS_D_INPUT || !n) return;
    if (c->ent[index] < 0) {
        /* made on the first words, so an empty block leaves no empty bubble */
        c->ent[index] = w_add(c, kind == AS_D_TEXT ? AS_E_TEXT : AS_E_THINKING, NULL, NULL);
    }
    pthread_mutex_lock(&A.lock);
    ent_t *e = mine(c) ? ent_get(c->ent[index]) : NULL;
    if (e) ent_text(e, s, n, true);
    if (mine(c)) A.phase = kind == AS_D_TEXT ? AS_PHASE_WRITING : AS_PHASE_THINKING;
    pthread_mutex_unlock(&A.lock);
    bump();
}

/* ---- confirmation cards ---- */

static bool cb_confirm(void *user, const char *kind, const char *title, const char *detail, const char *reason)
{
    wctx_t *c = user;
    pthread_mutex_lock(&A.lock);
    if (A.gen != c->gen || A.stop) {
        pthread_mutex_unlock(&A.lock);
        return false;
    }
    as_confirm_t *k = &A.confirm;
    memset(k, 0, sizeof *k);
    k->pending = true;
    snprintf(k->kind, sizeof k->kind, "%s", kind);
    snprintf(k->title, sizeof k->title, "%s", title);
    snprintf(k->detail, sizeof k->detail, "%s", detail);
    snprintf(k->reason, sizeof k->reason, "%s", reason ? reason : "");
    k->deadline = hal_seconds() + CONFIRM_S;
    A.answer = 0;
    A.phase = AS_PHASE_CONFIRM;
    ent_t *e = ent_get(c->cur);
    if (e) {
        e->e.tool_state = AS_TOOL_WAITING;
        e->e.rev++;
    }
    pthread_mutex_unlock(&A.lock);
    bump();
    int answer = 0;
    bool timed_out = false;
    for (;;) {
        usleep(50000);
        pthread_mutex_lock(&A.lock);
        answer = A.answer;
        bool gone = A.stop || A.gen != c->gen;
        timed_out = !answer && !gone && hal_seconds() > k->deadline;
        if (answer || gone || timed_out) {
            k->pending = false;
            if (mine(c)) A.phase = AS_PHASE_TOOL;
            if ((e = ent_get(c->cur)) && A.gen == c->gen) {
                e->e.tool_state = AS_TOOL_RUNNING;
                e->e.rev++;
            }
            pthread_mutex_unlock(&A.lock);
            break;
        }
        pthread_mutex_unlock(&A.lock);
    }
    bump();
    if (timed_out) w_note(c, "No answer on the card in 90 s: counted as declined.");
    return answer == 1;
}

bool assist_confirm_pending(as_confirm_t *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.confirm;
    bool p = A.confirm.pending;
    pthread_mutex_unlock(&A.lock);
    return p;
}

void assist_confirm(bool approve)
{
    pthread_mutex_lock(&A.lock);
    if (A.confirm.pending && !A.answer) A.answer = approve ? 1 : 2;
    pthread_mutex_unlock(&A.lock);
    bump();
}

/* ---- one request, streamed ---- */

typedef enum { ST_OK, ST_FAILED, ST_STOPPED } st_t;

static bool endpoint(char *url, size_t un, char *hdr, size_t hn, as_route_t *route)
{
    pthread_mutex_lock(&A.lock);
    assist_config_t cfg = A.cfg;
    char base[128];
    if (cfg.route == AS_ROUTE_OPENAI) snprintf(base, sizeof base, "%s", A.oai_base[0] ? A.oai_base : AS_OAI_DEFAULT_BASE);
    else snprintf(base, sizeof base, "%s", A.base[0] ? A.base : "https://api.anthropic.com");
    pthread_mutex_unlock(&A.lock);
    *route = cfg.route;
    if (cfg.route == AS_ROUTE_OPENAI) {
        size_t l = strlen(base);
        while (l && base[l - 1] == '/') base[--l] = 0;
        snprintf(url, un, "%s/v1/chat/completions", base);
        as_oai_headers(cfg.oai_key, hdr, hn);
        bool ok = cfg.oai_key[0] != 0;
        memset(&cfg, 0, sizeof cfg); /* the keys don't linger on the stack */
        return ok;
    }
    if (cfg.route == AS_ROUTE_DIRECT) {
        size_t l = strlen(base);
        while (l && base[l - 1] == '/') base[--l] = 0;
        snprintf(url, un, "%s/v1/messages", base);
        as_conv_headers(cfg.api_key, hdr, hn);
        return cfg.api_key[0] != 0;
    }
    /* through the Link: it adds the key and forwards the same SSE */
    char tok[160];
    if (!link_messages_endpoint(url, un, tok, sizeof tok)) return false;
    snprintf(hdr, hn, "%santhropic-version: 2023-06-01\r\ncontent-type: application/json\r\n"
                      "anthropic-beta: " AS_BETA "\r\naccept: text/event-stream\r\n", tok);
    return true;
}

/* the message in an error body: the API's {"error":{"message"}}, or the Link's {"error":"…"} */
static void error_message(const char *body, char *out, size_t n)
{
    aj_t *d = aj_parse(body, strlen(body), NULL, 0);
    const aj_t *e = aj_get(d, "error");
    const char *m = e && e->type == AJ_STR ? e->s : aj_gets(e, "message");
    snprintf(out, n, "%s", m ? m : body[0] ? body : "no details");
    if (strlen(out) > 200) strcpy(out + 197, "…");
    aj_free(d);
}

static const char *route_label(as_route_t r)
{
    return r == AS_ROUTE_DIRECT ? "the Claude API" : r == AS_ROUTE_OPENAI ? "the OpenAI API" : "Catalyst Link";
}

static st_t stream_once(wctx_t *c, const char *body, size_t len, as_msg_t *m)
{
    char url[256], hdr[640];
    as_route_t route;
    if (!endpoint(url, sizeof url, hdr, sizeof hdr, &route)) {
        w_add(c, AS_E_ERROR, route == AS_ROUTE_DIRECT ? "No API key: add one in settings, or use Catalyst Link."
                             : route == AS_ROUTE_OPENAI ? "No OpenAI key: add one in settings (assistant) or in the link app."
                                                        : "Catalyst Link isn't set up: add its address in settings.", NULL);
        return ST_FAILED;
    }
    bool oai = route == AS_ROUTE_OPENAI;
    as_oai_t ot;
    as_msg_cb_t cb = { .user = c, .start = cb_start, .delta = cb_delta };
    for (int attempt = 0; attempt < 2; attempt++) {
        if (w_stopped(c)) return ST_STOPPED;
        w_phase(c, AS_PHASE_SENDING);
        as_msg_init(m, &cb);
        as_oai_init(&ot, m);
        for (int i = 0; i < MAX_BLOCKS; i++) c->ent[i] = -1;
        hal_http_req_t rq = { .method = "POST", .url = url, .headers = hdr, .body = body, .body_len = len,
                              .timeout_ms = READ_TIMEOUT_MS };
        int status = -1;
        char err[96] = "";
        hal_http_t *h = hal_http_open(&rq, &status, err, sizeof err);
        bool retry = false;
        char msg[320];
        if (!h) {
            snprintf(msg, sizeof msg, "Couldn't reach %s (%s).%s", route_label(route), err,
                     route == AS_ROUTE_LINK ? "" : " Is the Wi-Fi on the internet?");
            retry = true;
        } else if (status != 200) {
            char ebody[4096];
            int n = 0, r;
            while (n < (int)sizeof ebody - 1 && (r = hal_http_read(h, ebody + n, (int)sizeof ebody - 1 - n)) > 0) n += r;
            ebody[n] = 0;
            hal_http_close(h);
            h = NULL;
            char em[240], code[48] = "";
            if (oai) as_oai_error(ebody, em, sizeof em, code, sizeof code);
            else error_message(ebody, em, sizeof em);
            retry = status == 429 || status == 529 || status >= 500;
            /* never the body on a 401: OpenAI's echoes part of the key */
            if (status == 401)
                snprintf(msg, sizeof msg, "%s", route == AS_ROUTE_DIRECT ? "The API key was refused (401): check the key in settings."
                                                : oai ? "OpenAI refused the key (401): check the OpenAI key in settings."
                                                      : "Catalyst Link refused the token (401): check it in settings.");
            else if (oai && status == 429 && !strcmp(code, "insufficient_quota")) {
                snprintf(msg, sizeof msg, "OpenAI says this key's account is out of credit (429): check billing on platform.openai.com.");
                retry = false; /* waiting won't fix it */
            } else if (oai && status == 429)
                snprintf(msg, sizeof msg, "OpenAI is rate-limiting this key (429): %s", em);
            else if (oai && (status == 404 || !strcmp(code, "model_not_found")))
                snprintf(msg, sizeof msg, "OpenAI doesn't offer that model to this key (HTTP %d): check the model name in settings.", status);
            else if (oai) snprintf(msg, sizeof msg, "OpenAI HTTP %d: %s", status, em);
            else snprintf(msg, sizeof msg, "HTTP %d: %s", status, em);
        } else {
            as_sse_t sse;
            as_sse_init(&sse);
            char *buf = malloc(4096);
            int r = -1;
            while (buf && (r = hal_http_read(h, buf, 4096)) > 0) {
                if (oai) as_sse_feed(&sse, buf, (size_t)r, as_oai_sse, &ot);
                else as_sse_feed(&sse, buf, (size_t)r, as_msg_sse, m);
                if (m->done || w_stopped(c)) break;
            }
            if (oai) as_oai_end(&ot);
            free(buf);
            as_sse_free(&sse);
            hal_http_close(h);
            if (w_stopped(c)) return ST_STOPPED;
            if (r < 0 && !m->done && !m->error) {
                snprintf(m->error_type, sizeof m->error_type, "connection");
                snprintf(m->error_msg, sizeof m->error_msg, "the connection dropped mid-answer");
                m->error = true;
            }
            /* an overloaded error before any output is as good as a 529 */
            if (m->error && m->n == 0 && attempt == 0 &&
                (!strcmp(m->error_type, "overloaded_error") || !strcmp(m->error_type, "api_error") ||
                 !strcmp(m->error_type, "server_error"))) {
                snprintf(msg, sizeof msg, "%s", m->error_msg);
                as_msg_free(m);
                retry = true;
            } else {
                return ST_OK;
            }
        }
        if (retry && attempt == 0) {
            char note[400];
            size_t l = strlen(msg);
            snprintf(note, sizeof note, "%s%s Trying once more in a few seconds.", msg,
                     l && (msg[l - 1] == '.' || msg[l - 1] == ')') ? "" : ".");
            w_note(c, note);
            for (int i = 0; i < 30 && !w_stopped(c); i++) usleep(100000);
            continue;
        }
        w_add(c, AS_E_ERROR, msg, NULL);
        return ST_FAILED;
    }
    return ST_FAILED;
}

/* ---- the exchange ---- */

static void context_note(const cat_robot_t *r, bool full, char *out, size_t n, char *state, size_t sn)
{
    link_status_t ls;
    link_status(&ls);
    int team;
    pthread_mutex_lock(&A.lock);
    team = A.team;
    pthread_mutex_unlock(&A.lock);
    if (r->connected)
        /* only what changes rarely: a later message gets a new note when this line changes */
        snprintf(state, sn, "robot %s connected at %s · %s · %d errors, %d warnings", r->name[0] ? r->name : "",
                 r->address, cat_mode_name(r), r->n_errors, r->n_warnings);
    else snprintf(state, sn, "robot not connected (last tried %s)", r->address[0] ? r->address : "nothing yet");
    if (!full) {
        snprintf(out, n, "[tablet: %s]", state);
        return;
    }
    char when[32] = "";
    struct tm tm;
    if (hal_rtc_get(&tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    char linkline[200];
    if (ls.reachable && !ls.auth)
        snprintf(linkline, sizeof linkline, "reachable but the tablet's token is wrong; patches and work orders queue on the tablet");
    else if (ls.reachable)
        snprintf(linkline, sizeof linkline, "reachable (%s, repo %s on %s)", ls.name, ls.repo, ls.branch);
    else if (ls.configured)
        snprintf(linkline, sizeof linkline, "not reachable right now; patches and work orders queue on the tablet");
    else snprintf(linkline, sizeof linkline, "not set up; patches and work orders queue on the tablet");
    snprintf(out, n,
             "[Catalyst Tab context, %s. Team %d. %s%s%s%s. Catalyst Link on the PC: %s.%s]", when,
             team > 0 ? team : r->team, state, r->connected && r->catalyst_version[0] ? " · Catalyst " : "",
             r->connected ? r->catalyst_version : "", r->connected ? (r->line2 ? " (Systemcore)" : " (roboRIO)") : "",
             linkline, ls.outbox ? " Items are waiting in the tablet's outbox." : "");
    /* the next match and the battery to use (get_matches / get_batteries say more) */
    char *now = assist_desk_get(AS_DESK_NOW);
    size_t l = strlen(out);
    if (now && l > 1 && l + strlen(now) + 4 < n) snprintf(out + l - 1, n - l + 1, " %s.]", now);
    free(now);
}

static void mark_unrun(wctx_t *c, const as_msg_t *m, const as_call_t *calls, int ncalls, const char *why)
{
    for (int i = 0; i < m->n && i < MAX_BLOCKS; i++) {
        if (c->ent[i] < 0 || !m->b[i].block) continue;
        const char *t = aj_gets(m->b[i].block, "type");
        if (!t || strcmp(t, "tool_use") != 0) continue;
        const char *id = aj_gets(m->b[i].block, "id");
        bool runs = false;
        for (int k = 0; k < ncalls && !runs; k++) runs = id && !strcmp(calls[k].id, id);
        if (!runs) w_set(c, c->ent[i], why, AS_TOOL_FAILED);
    }
}

static int call_entry(wctx_t *c, const as_msg_t *m, const char *id)
{
    for (int i = 0; i < m->n && i < MAX_BLOCKS; i++) {
        const char *bid = m->b[i].block ? aj_gets(m->b[i].block, "id") : NULL;
        if (bid && !strcmp(bid, id)) return c->ent[i];
    }
    return -1;
}

static void run_tools(wctx_t *c, as_hist_t *h, const as_msg_t *m, const as_call_t *calls, int n)
{
    as_result_t *res = calloc((size_t)n, sizeof *res);
    char **own = calloc((size_t)n, sizeof *own);
    if (!res || !own) {
        free(res);
        free(own);
        return;
    }
    pthread_mutex_lock(&A.lock);
    int team = A.team;
    pthread_mutex_unlock(&A.lock);
    as_env_t env = { .robot = c->robot, .refresh = refresh, .confirm = cb_confirm, .note = cb_note,
                     .stopped = w_stopped, .user = c, .team = team };
    for (int i = 0; i < n; i++) {
        const as_call_t *k = &calls[i];
        int ent = call_entry(c, m, k->id);
        c->cur = ent;
        res[i].id = k->id;
        char err[200], summary[200] = "";
        if (w_stopped(c)) {
            own[i] = as_strdup("{\"error\":\"stopped by the technician before this ran\"}");
            res[i].is_error = true;
            w_set(c, ent, "stopped", AS_TOOL_FAILED);
        } else if (!k->input) {
            /* the streamed input wasn't valid JSON: don't run it, show the model what arrived */
            own[i] = as_conv_invalid_json(k->raw);
            res[i].is_error = true;
            snprintf(summary, sizeof summary, "input didn't parse (%s)", k->err);
            w_set(c, ent, summary, AS_TOOL_FAILED);
        } else {
            const as_tooldef_t *def = as_tool_find(k->name);
            if (!def || !as_tool_check(def, k->input, err, sizeof err)) {
                /* parsed, but not what the schema says: don't run it, say what's wrong */
                if (!def) snprintf(err, sizeof err, "no tool named %.60s", k->name);
                else snprintf(summary, sizeof summary, "invalid input: %s", err);
                ab_t b;
                ab_init(&b);
                ab_puts(&b, "{\"error\":");
                ab_str(&b, def ? summary : err);
                ab_puts(&b, "}");
                own[i] = ab_take(&b);
                res[i].is_error = true;
                w_set(c, ent, def ? summary : err, AS_TOOL_FAILED);
            } else {
                w_phase(c, AS_PHASE_TOOL);
                w_set(c, ent, "running…", AS_TOOL_RUNNING);
                ab_t out;
                ab_init(&out);
                as_tres_t r = as_tool_run(&env, k->name, k->input, &out, summary, sizeof summary);
                own[i] = ab_take(&out);
                res[i].is_error = r != AS_TR_OK;
                w_set(c, ent, summary[0] ? summary : "done",
                      r == AS_TR_OK ? AS_TOOL_OK : r == AS_TR_DENIED ? AS_TOOL_DENIED : AS_TOOL_FAILED);
            }
        }
        res[i].content = own[i] ? own[i] : "";
    }
    /* every result of the turn in one user message */
    as_hist_results(h, res, n);
    for (int i = 0; i < n; i++) free(own[i]);
    free(own);
    free(res);
}

static void usage_add(wctx_t *c, const as_msg_t *m, const char *requested)
{
    /* Through Claude Code on the PC the model is Claude Code's choice, not the one requested: a different
     * name there is not a fallback. (link_status takes the Link's lock: read it before ours.) */
    link_status_t ls;
    link_status(&ls);
    bool own_model = !strcmp(ls.claude_via, "claude-code");
    pthread_mutex_lock(&A.lock);
    bool fell = false;
    if (A.gen == c->gen) {
        A.usage.input_tokens += m->input_tokens + m->cache_read + m->cache_write;
        A.usage.output_tokens += m->output_tokens;
        if (m->model[0]) snprintf(A.usage.model, sizeof A.usage.model, "%s", m->model);
        if (A.cfg.route != AS_ROUTE_LINK) own_model = false;
        /* OpenAI answers with a dated snapshot of the name asked for ("gpt-4o-mini-2024-07-18"): not a fallback */
        if (A.cfg.route == AS_ROUTE_OPENAI) own_model = true;
        fell = m->fallback || (!own_model && m->model[0] && strcmp(m->model, requested) != 0);
        if (fell) A.usage.fell_back = true;
    }
    pthread_mutex_unlock(&A.lock);
    if (fell && !c->fallback_noted) {
        char n[160];
        snprintf(n, sizeof n, "Answered by %s (server-side fallback).", m->model[0] ? m->model : "another model");
        w_note(c, n);
        c->fallback_noted = true;
    }
}

static void exchange(as_hist_t *h, const char *text, wctx_t *c, const char *tools, char *last_state, size_t lsn)
{
    refresh(c->robot);
    char note[900], state[200];
    context_note(c->robot, h->n == 0, note, sizeof note, state, sizeof state);
    bool changed = strcmp(state, last_state) != 0;
    snprintf(last_state, lsn, "%s", state);
    as_hist_user(h, h->n == 0 || changed ? note : NULL, text);

    pthread_mutex_lock(&A.lock);
    char model[48];
    bool oai = A.cfg.route == AS_ROUTE_OPENAI;
    if (oai) snprintf(model, sizeof model, "%s", A.cfg.oai_model[0] ? A.cfg.oai_model : AS_OAI_DEFAULT_MODEL);
    else snprintf(model, sizeof model, "%s", A.cfg.model[0] ? A.cfg.model : AS_DEFAULT_MODEL);
    pthread_mutex_unlock(&A.lock);
    as_req_t q = { .model = model, .max_tokens = AS_MAX_TOKENS, .effort = "medium", .system = SYSTEM_PROMPT, .tools = tools };
    if (as_conv_trim(h, &q, AS_TRIM_BYTES)) w_note(c, "The oldest part of the conversation was dropped to keep requests small.");

    bool budget_retried = false, ran_something = false;
    ab_t body;
    ab_init(&body);
    for (int round = 0; round < MAX_ROUNDS; round++) {
        c->fallback_noted = false;
        if (oai) as_oai_request(h, &q, &body);
        else as_conv_request(h, &q, &body);
        if (body.oom) {
            w_add(c, AS_E_ERROR, "Out of memory building the request.", NULL);
            w_phase(c, AS_PHASE_ERROR);
            break;
        }
        as_msg_t m;
        st_t st = stream_once(c, body.p, body.n, &m);
        if (st == ST_STOPPED) {
            as_msg_free(&m); /* ("Stopped." was said by assist_stop, the moment it was tapped) */
            if (!ran_something) as_hist_rollback(h);
            break;
        }
        if (st == ST_FAILED) {
            if (!ran_something) as_hist_rollback(h);
            w_phase(c, AS_PHASE_ERROR);
            break;
        }
        usage_add(c, &m, model);
        as_call_t calls[32];
        int ncalls = as_conv_calls(&m, calls, 32);
        as_next_t nx = as_conv_next(&m);
        if (nx != AS_NEXT_TOOLS) mark_unrun(c, &m, calls, 0, "not run");
        else mark_unrun(c, &m, calls, ncalls, "not run (dropped at a model switch)");

        if (nx == AS_NEXT_ERROR) {
            char e[320];
            snprintf(e, sizeof e, "%s", m.error_msg[0] ? m.error_msg : "The answer ended before it was complete.");
            w_add(c, AS_E_ERROR, e, NULL);
            if (!ran_something) as_hist_rollback(h);
            w_phase(c, AS_PHASE_ERROR);
            as_msg_free(&m);
            break;
        }
        if (nx == AS_NEXT_REFUSED) {
            /* nothing from a refused turn runs, and the question leaves the history so the conversation
             * can go on */
            char n[400];
            const char *cat = aj_gets(m.stop_details, "category"), *ex = aj_gets(m.stop_details, "explanation");
            char exs[240] = "";
            if (ex) {
                snprintf(exs, sizeof exs, ": %s", ex);
                size_t l = strlen(exs);
                while (l && (exs[l - 1] == '.' || exs[l - 1] == ' ')) exs[--l] = 0;
            }
            snprintf(n, sizeof n, "%s declined this request%s%s%s%s. Nothing from that answer was run.",
                     oai ? "The model" : "Claude", cat ? " (" : "", cat ? cat : "", cat ? ")" : "", exs);
            w_note(c, n);
            as_hist_rollback(h);
            as_msg_free(&m);
            break;
        }
        if (nx == AS_NEXT_TRUNCATED) {
            as_msg_free(&m);
            if (!budget_retried && !oai) { /* OpenAI already had its model's whole ceiling */
                budget_retried = true;
                q.max_tokens = AS_MAX_TOKENS_RETRY;
                w_note(c, "The answer ran out of room in the middle of a tool call; asking again with more room.");
                continue;
            }
            w_add(c, AS_E_ERROR, "The answer ran out of room twice in a tool call. Try a narrower question.", NULL);
            if (!ran_something) as_hist_rollback(h);
            w_phase(c, AS_PHASE_ERROR);
            break;
        }
        aj_t *echo = as_conv_echo(&m);
        if (!echo || echo->n == 0) {
            aj_free(echo);
            if (nx == AS_NEXT_DONE) w_note(c, "(no answer)");
            if (!ran_something) as_hist_rollback(h);
            as_msg_free(&m);
            break;
        }
        as_hist_assistant(h, echo);
        if (nx == AS_NEXT_DONE) {
            if (!strcmp(m.stop_reason, "max_tokens")) w_note(c, "(the answer was cut off at the length limit)");
            as_msg_free(&m);
            break;
        }
        if (nx == AS_NEXT_CONTINUE) {
            as_msg_free(&m);
            continue;
        }
        /* AS_NEXT_TOOLS */
        ran_something = true;
        run_tools(c, h, &m, calls, ncalls);
        as_msg_free(&m);
        if (w_stopped(c)) break;
        if (round == MAX_ROUNDS - 1) w_add(c, AS_E_ERROR, "Stopped after too many tool rounds.", NULL);
    }
    ab_free(&body);
}

static void *worker(void *arg)
{
    (void)arg;
    as_hist_t hist;
    as_hist_init(&hist);
    wctx_t *c = calloc(1, sizeof *c);
    cat_robot_t *robot = malloc(sizeof *robot);
    ab_t tb;
    ab_init(&tb);
    as_tools_json(&tb);
    char *tools = ab_take(&tb);
    char last_state[200] = "";
    uint32_t hist_gen = 0;
    if (!c || !robot || !tools) return NULL;
    pthread_mutex_lock(&A.lock);
    hist_gen = A.gen;
    pthread_mutex_unlock(&A.lock);
    for (;;) {
        pthread_mutex_lock(&A.lock);
        while (!A.pending && !A.job) pthread_cond_wait(&A.wake, &A.lock);
        if (A.job) {
            /* a one-shot job (a log analysis) between conversation turns, on this thread's stack */
            void (*job)(void *) = A.job;
            void *arg = A.job_arg;
            pthread_mutex_unlock(&A.lock);
            job(arg);
            pthread_mutex_lock(&A.lock);
            A.job = NULL;
            A.job_arg = NULL;
            pthread_mutex_unlock(&A.lock);
            continue;
        }
        char *text = A.pending;
        A.pending = NULL;
        A.stop = false;
        A.running = true;
        uint32_t gen = A.gen;
        pthread_mutex_unlock(&A.lock);
        if (gen != hist_gen) {
            as_hist_free(&hist);
            as_hist_init(&hist);
            last_state[0] = 0;
            hist_gen = gen;
        }
        memset(c, 0, sizeof *c);
        c->gen = gen;
        c->robot = robot;
        c->cur = -1;
        exchange(&hist, text, c, tools, last_state, sizeof last_state);
        free(text);
        pthread_mutex_lock(&A.lock);
        /* a question asked after a stop, while this one still let go of the network, is next */
        A.running = false;
        A.busy = A.pending != NULL;
        A.stop = false;
        A.confirm.pending = false;
        if (A.pending) A.phase = AS_PHASE_SENDING;
        else if (A.gen != gen || A.phase != AS_PHASE_ERROR) A.phase = AS_PHASE_IDLE;
        pthread_mutex_unlock(&A.lock);
        bump();
    }
    return NULL;
}

/* ---- the UI's side ---- */

/* keys arrive typed or pasted: spaces or a newline around one would be sent in the header */
static void trim_key(char *k)
{
    char *p = k;
    while (*p == ' ' || *p == '\t') p++;
    if (p != k) memmove(k, p, strlen(p) + 1);
    for (size_t i = strlen(k); i && (k[i - 1] == '\n' || k[i - 1] == '\r' || k[i - 1] == ' ' || k[i - 1] == '\t'); i--)
        k[i - 1] = 0;
}

/* ---- keys from the card ---- */

static char *g_import_note; /* heap (PSRAM on the tablet): what the island says once, names only */

/* The first line of the file, without a BOM or whitespace; NULL when there is no file. Caller wipes. */
static char *read_key_file(const char *path, size_t *filesize)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *k = calloc(1, 512);
    size_t n = k ? fread(k, 1, 511, f) : 0;
    fclose(f);
    if (!k) return NULL;
    *filesize = n;
    char *p = k;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t l = strcspn(p, "\r\n");
    while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
    memmove(k, p, l);
    memset(k + l, 0, 512 - l);
    return k;
}

/* Overwrites the file's bytes, then deletes it; renamed to .USED if the card won't delete it. */
static void burn_key_file(const char *path, size_t n)
{
    FILE *f = fopen(path, "r+b");
    if (f) {
        char z[64] = { 0 };
        for (size_t done = 0; done < n; done += sizeof z) fwrite(z, 1, n - done < sizeof z ? n - done : sizeof z, f);
        fclose(f);
    }
    if (remove(path) != 0) {
        char used[160];
        snprintf(used, sizeof used, "%.150s.USED", path);
        rename(path, used);
    }
}

/* KEYS.ENV: the names it knows, and the kv key each is kept under (the one its own settings screen reads).
 * WIFI_* go to the Wi-Fi driver instead (hal_wifi_join: the driver keeps the network in its own NVS).
 * LINK_HOST/LINK_TOKEN land in "link_url"/"link_token" — the same kv keys pairing and the link app's
 * settings screen already use (ui_app_pair.c, ui_app_assist.c) — so a card dropped on this one tablet
 * finishes the same "configure the link" job pairing does, without a token ever living in source
 * (link.c's LINK_DEFAULT_HOST covers the non-secret address; see its comment). */
enum { ENV_OPENAI, ENV_ANTHROPIC, ENV_TEAM = 8, ENV_WIFI_SSID, ENV_WIFI_PASS, ENV_LINK_HOST, ENV_LINK_TOKEN,
       ENV_COUNT };
static const struct {
    const char *name, *kv;
} ENV_KEYS[ENV_COUNT] = {
    { "OPENAI_API_KEY", "oai_key" },       /* the assistant's OpenAI route, the companion's voice */
    { "ANTHROPIC_API_KEY", "ai_key" },     /* the assistant's direct route */
    { "TBA_API_KEY", "tba_key" },          /* The Blue Alliance, read API v3 (X-TBA-Auth-Key) */
    { "HA_URL", "ha_url" },                /* Home Assistant, as home mode keeps it */
    { "HA_TOKEN", "ha_token" },
    { "NEXUS_API_KEY", "nexus_key" },      /* frc.nexus (Nexus-Api-Key): kept for later */
    { "FRC_EVENTS_USER", "frc_ev_user" },  /* FIRST's FRC Events API: kept for later */
    { "FRC_EVENTS_TOKEN", "frc_ev_token" },
    { "TEAM", "team" },                    /* the team number the shell looks for */
    { "WIFI_SSID", NULL },
    { "WIFI_PASS", NULL },
    { "LINK_HOST", "link_url" },           /* overrides the compiled-in pc address, e.g. a different pc */
    { "LINK_TOKEN", "link_token" },        /* Catalyst Link's per-tablet token: never compiled in */
};

/* One "NAME=value" line, dotenv style, cut in place: an optional "export ", spaces around the '=', a value
 * in single or double quotes (\" and \\ inside double) or bare with a " # comment" after it. false for a
 * blank line, a comment, or anything without an '='. */
static bool env_line(char *line, char **name, char **value)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return false;
    if (!strncmp(p, "export", 6) && (p[6] == ' ' || p[6] == '\t')) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
    }
    /* "NAME=value", or as people write it by hand, "name: value" (whichever comes first) */
    char *eq = strchr(p, '='), *colon = strchr(p, ':');
    if (colon && (!eq || colon < eq)) eq = colon;
    if (!eq || eq == p) return false;
    char *ne = eq;
    while (ne > p && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
    *ne = 0;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '"' || *v == '\'') {
        char q = *v++, *w = v, *r = v;
        for (; *r && *r != q; r++) {
            if (q == '"' && *r == '\\' && (r[1] == '"' || r[1] == '\\')) r++;
            *w++ = *r;
        }
        *w = 0;
    } else {
        for (char *c = v; *c; c++)
            if (*c == '#' && c > v && (c[-1] == ' ' || c[-1] == '\t')) {
                *c = 0;
                break;
            }
        size_t l = strlen(v);
        while (l && (v[l - 1] == ' ' || v[l - 1] == '\t' || v[l - 1] == '\r')) v[--l] = 0;
    }
    *name = p;
    *value = v;
    return true;
}

/* A hand-written name to its ENV_KEYS entry: case, '-', '_', '.' and spaces don't matter, and the short
 * names people use ("gpt", "tba", "wifi", "pass") count; -1 unknown */
static int env_find(const char *name)
{
    char n[40];
    size_t k = 0;
    for (const char *c = name; *c && k < sizeof n - 1; c++)
        if (*c != '-' && *c != '_' && *c != '.' && *c != ' ') n[k++] = (char)tolower((unsigned char)*c);
    n[k] = 0;
    static const struct { const char *alias; int e; } ALIAS[] = {
        { "openaiapikey", ENV_OPENAI }, { "openaikey", ENV_OPENAI }, { "openai", ENV_OPENAI }, { "gpt", ENV_OPENAI },
        { "chatgpt", ENV_OPENAI }, { "gptkey", ENV_OPENAI },
        { "anthropicapikey", ENV_ANTHROPIC }, { "anthropickey", ENV_ANTHROPIC }, { "anthropic", ENV_ANTHROPIC },
        { "claude", ENV_ANTHROPIC }, { "claudekey", ENV_ANTHROPIC },
        { "tbaapikey", 2 }, { "tbakey", 2 }, { "tba", 2 }, { "bluealliance", 2 }, { "thebluealliance", 2 },
        { "haurl", 3 }, { "homeassistanturl", 3 }, { "hatoken", 4 }, { "homeassistanttoken", 4 }, { "homeassistant", 4 },
        { "nexusapikey", 5 }, { "nexus", 5 }, { "frceventsuser", 6 }, { "frceventstoken", 7 },
        { "team", ENV_TEAM }, { "teamnumber", ENV_TEAM },
        { "wifissid", ENV_WIFI_SSID }, { "wifi", ENV_WIFI_SSID }, { "ssid", ENV_WIFI_SSID }, { "network", ENV_WIFI_SSID },
        { "wifipass", ENV_WIFI_PASS }, { "wifipassword", ENV_WIFI_PASS }, { "pass", ENV_WIFI_PASS },
        { "password", ENV_WIFI_PASS },
        { "linkhost", ENV_LINK_HOST }, { "linkurl", ENV_LINK_HOST }, { "linkaddress", ENV_LINK_HOST },
        { "pcaddress", ENV_LINK_HOST },
        { "linktoken", ENV_LINK_TOKEN }, { "link", ENV_LINK_TOKEN },
    };
    for (size_t i = 0; i < sizeof ALIAS / sizeof ALIAS[0]; i++)
        if (!strcmp(n, ALIAS[i].alias)) return ALIAS[i].e;
    return -1;
}

/* Reads one KEYS.ENV into kv, then burns it. Returns a bit per ENV_KEYS entry taken; the Wi-Fi pair lands
 * in ssid (33 bytes) and pass (65) for the caller. */
static unsigned import_env(const char *path, char *ssid, char *pass)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    size_t size = (size_t)st.st_size, cap = size < 16384 ? size : 16384;
    size_t alloc = cap + 1 < 1024 ? 1024 : cap + 1; /* >= 1 KB: PSRAM on the tablet, not internal RAM */
    char *buf = calloc(1, alloc);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    if (!f) {
        free(buf);
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = 0;
    unsigned got = 0;
    char *p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        char *eol = p + strcspn(p, "\r\n");
        char *next = *eol ? eol + 1 : eol;
        *eol = 0;
        char *name, *value;
        if (env_line(p, &name, &value) && value[0]) {
            int want = env_find(name);
            /* a name it doesn't know, but a value that says what it is */
            if (want < 0 && !strncmp(value, "sk-ant-", 7)) want = ENV_ANTHROPIC;
            else if (want < 0 && !strncmp(value, "sk-", 3)) want = ENV_OPENAI;
            for (int i = 0; i < ENV_COUNT; i++) {
                if (i != want) continue;
                if (i == ENV_WIFI_SSID) snprintf(ssid, 33, "%s", value);
                else if (i == ENV_WIFI_PASS) snprintf(pass, 65, "%s", value);
                else if (i == ENV_TEAM) {
                    int t = atoi(value);
                    if (t <= 0 || t > 99999) break; /* not a team number: not taken */
                    char num[8];
                    snprintf(num, sizeof num, "%d", t);
                    hal_kv_set(ENV_KEYS[i].kv, num);
                } else {
                    if (!strcmp(ENV_KEYS[i].kv, "ha_url")) { /* home mode appends /api/...: no trailing / */
                        size_t l = strlen(value);
                        while (l > 1 && value[l - 1] == '/') value[--l] = 0;
                    }
                    hal_kv_set(ENV_KEYS[i].kv, value);
                }
                got |= 1u << i;
                break;
            }
        }
        p = next;
    }
    memset(buf, 0, alloc);
    free(buf);
    burn_key_file(path, size);
    return got;
}

/* UI thread at boot, before anything reads the settings (it writes NVS: the UI task's stack is internal RAM). */
void assist_import_card(void)
{
    const char *sd = hal_sd_root();
    if (!sd) return;
    unsigned got = 0;
    static const char *const FILES[2] = { "OPENAI.TXT", "ANTHROPIC.TXT" };
    for (int i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof path, "%s/CATOS/KEYS/%s", sd, FILES[i]);
        size_t n = 0;
        char *k = read_key_file(path, &n);
        if (!k) continue;
        int e = i == 0 ? ENV_OPENAI : ENV_ANTHROPIC;
        if (strlen(k) >= 20) { /* a key, not an empty or placeholder file */
            hal_kv_set(ENV_KEYS[e].kv, k);
            got |= 1u << e;
        }
        memset(k, 0, 512);
        free(k);
        burn_key_file(path, n);
    }
    char *wifi = calloc(1, 1024); /* ssid[33] and pass[65], off the UI task's stack */
    if (wifi) {
        /* Windows hides the extension it adds, so keys.env saved from Notepad is often keys.env.txt */
        static const char *const ENVS[6] = { "CATOS/KEYS.ENV", "KEYS.ENV", "CATOS/KEYS.ENV.TXT", "KEYS.ENV.TXT",
                                             "CATOS/KEYS.TXT", "KEYS.TXT" };
        for (int i = 0; i < 6; i++) {
            char path[128];
            snprintf(path, sizeof path, "%s/%s", sd, ENVS[i]);
            got |= import_env(path, wifi, wifi + 64);
        }
        if (got & 1u << ENV_WIFI_SSID) hal_wifi_join(wifi, wifi + 64); /* no WIFI_PASS: an open network */
        memset(wifi, 0, 1024);
        free(wifi);
    }
    if (!got) return;
    /* the island names what came in, never a value; assist_init() and the settings read the values from kv */
    g_import_note = calloc(1, 1024);
    if (!g_import_note) return;
    int len = snprintf(g_import_note, 1024, "from the card:"), shown = 0, more = 0;
    for (int i = 0; i < ENV_COUNT; i++) {
        if (!(got & 1u << i)) continue;
        if (len + (int)strlen(ENV_KEYS[i].name) + 2 > 84) more++; /* the island's line, and a note's 96 */
        else len += snprintf(g_import_note + len, 1024 - (size_t)len, "%s %s", shown++ ? "," : "", ENV_KEYS[i].name);
    }
    if (more) snprintf(g_import_note + len, 1024 - (size_t)len, " +%d more", more);
}

const char *assist_import_note(void)
{
    static char *handed; /* the last one given out: freed on the next call */
    free(handed);
    handed = g_import_note;
    g_import_note = NULL;
    return handed;
}

void assist_init(void)
{
    char v[160];
    char *k = malloc(sizeof A.cfg.oai_key); /* off the caller's stack: the UI task's is internal RAM */
    pthread_mutex_lock(&A.lock);
    bool start = !A.started;
    A.started = true;
    if (hal_kv_get("ai_key", v, sizeof v)) snprintf(A.cfg.api_key, sizeof A.cfg.api_key, "%s", v);
    if (hal_kv_get("ai_model", v, sizeof v)) snprintf(A.cfg.model, sizeof A.cfg.model, "%s", v);
    if (hal_kv_get("ai_base", v, sizeof v)) snprintf(A.base, sizeof A.base, "%s", v);
    if (k && hal_kv_get("oai_key", k, sizeof A.cfg.oai_key)) snprintf(A.cfg.oai_key, sizeof A.cfg.oai_key, "%s", k);
    if (hal_kv_get("oai_model", v, sizeof v)) snprintf(A.cfg.oai_model, sizeof A.cfg.oai_model, "%s", v);
    if (hal_kv_get("oai_base", v, sizeof v)) snprintf(A.oai_base, sizeof A.oai_base, "%s", v);
    memset(v, 0, sizeof v);
    if (hal_kv_get("team", v, sizeof v)) A.team = atoi(v);
    /* the route: as set, else straight to the API when there's a key, else through the Link */
    if (hal_kv_get("ai_route", v, sizeof v) && v[0])
        A.cfg.route = !strcmp(v, "direct") ? AS_ROUTE_DIRECT : !strcmp(v, "openai") ? AS_ROUTE_OPENAI : AS_ROUTE_LINK;
    else A.cfg.route = A.cfg.api_key[0] ? AS_ROUTE_DIRECT : A.cfg.oai_key[0] ? AS_ROUTE_OPENAI : AS_ROUTE_LINK;
    trim_key(A.cfg.api_key);
    trim_key(A.cfg.oai_key);
    trim_key(A.cfg.oai_model);
    A.phase = AS_PHASE_IDLE;
    pthread_mutex_unlock(&A.lock);
    if (k) {
        memset(k, 0, sizeof A.cfg.oai_key);
        free(k);
    }
    if (!start) return;
    snap_init();
    link_init();
    /* TLS plus a tool's buffers (a 4 KB error body at most): 48 KB of internal RAM used ~0.5 KB, and the
     * network stack starved for buffers (DNS and connects failing) with it taken */
    hal_thread("assist", worker, NULL, 20 * 1024);
    voice_init(); /* the companion's own conversation, spoken (voice.h) */
}

void assist_configure(const assist_config_t *c)
{
    pthread_mutex_lock(&A.lock);
    A.cfg = *c;
    A.cfg.api_key[sizeof A.cfg.api_key - 1] = 0;
    A.cfg.model[sizeof A.cfg.model - 1] = 0;
    A.cfg.oai_key[sizeof A.cfg.oai_key - 1] = 0;
    A.cfg.oai_model[sizeof A.cfg.oai_model - 1] = 0;
    trim_key(A.cfg.api_key);
    trim_key(A.cfg.oai_key);
    pthread_mutex_unlock(&A.lock);
    bump();
}

void assist_config(assist_config_t *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.cfg;
    pthread_mutex_unlock(&A.lock);
}

static const char *route_word(as_route_t r) { return r == AS_ROUTE_DIRECT ? "direct" : r == AS_ROUTE_OPENAI ? "openai" : "link"; }

void assist_use(as_route_t route)
{
    pthread_mutex_lock(&A.lock);
    bool changed = A.cfg.route != route;
    A.cfg.route = route;
    pthread_mutex_unlock(&A.lock);
    hal_kv_set("ai_route", route_word(route));
    if (changed) assist_reset(); /* one model's history (thinking signatures, call ids) isn't another's */
    bump();
}

/* hal_kv_set with a copy taken under the lock; the copy is wiped after */
static void store_key(const char *kv, const char *field, size_t n)
{
    char *k = malloc(n);
    if (!k) return;
    pthread_mutex_lock(&A.lock);
    snprintf(k, n, "%s", field);
    pthread_mutex_unlock(&A.lock);
    hal_kv_set(kv, k);
    memset(k, 0, n);
    free(k);
}

void assist_set_anthropic(const char *key, const char *model)
{
    pthread_mutex_lock(&A.lock);
    if (key) {
        snprintf(A.cfg.api_key, sizeof A.cfg.api_key, "%s", key);
        trim_key(A.cfg.api_key);
    }
    if (model) {
        snprintf(A.cfg.model, sizeof A.cfg.model, "%s", model);
        trim_key(A.cfg.model);
    }
    pthread_mutex_unlock(&A.lock);
    if (key) store_key("ai_key", A.cfg.api_key, sizeof A.cfg.api_key);
    if (model) store_key("ai_model", A.cfg.model, sizeof A.cfg.model);
    bump();
}

void assist_set_openai(const char *key, const char *model)
{
    pthread_mutex_lock(&A.lock);
    if (key) {
        snprintf(A.cfg.oai_key, sizeof A.cfg.oai_key, "%s", key);
        trim_key(A.cfg.oai_key);
    }
    if (model) {
        snprintf(A.cfg.oai_model, sizeof A.cfg.oai_model, "%s", model);
        trim_key(A.cfg.oai_model);
    }
    pthread_mutex_unlock(&A.lock);
    if (key) store_key("oai_key", A.cfg.oai_key, sizeof A.cfg.oai_key);
    if (model) store_key("oai_model", A.cfg.oai_model, sizeof A.cfg.oai_model);
    bump();
}

const char *assist_provider_name(as_route_t r)
{
    static char name[80];
    if (r == AS_ROUTE_LINK) return "Claude through the PC";
    if (r == AS_ROUTE_DIRECT) return "Claude, key on the tablet";
    pthread_mutex_lock(&A.lock);
    snprintf(name, sizeof name, "OpenAI %s", A.cfg.oai_model[0] ? A.cfg.oai_model : AS_OAI_DEFAULT_MODEL);
    pthread_mutex_unlock(&A.lock);
    return name;
}

void assist_set_base_url(const char *url)
{
    pthread_mutex_lock(&A.lock);
    snprintf(A.base, sizeof A.base, "%s", url ? url : "");
    pthread_mutex_unlock(&A.lock);
}

bool assist_post_job(void (*fn)(void *), void *arg)
{
    pthread_mutex_lock(&A.lock);
    bool ok = A.started && !A.job;
    if (ok) {
        A.job = fn;
        A.job_arg = arg;
        pthread_cond_signal(&A.wake);
    }
    pthread_mutex_unlock(&A.lock);
    return ok;
}

bool assist_oai_endpoint(char *url, size_t un, char *hdr, size_t hn, char *model, size_t mn)
{
    pthread_mutex_lock(&A.lock);
    snprintf(url, un, "%s", A.oai_base[0] ? A.oai_base : AS_OAI_DEFAULT_BASE);
    snprintf(model, mn, "%s", A.cfg.oai_model[0] ? A.cfg.oai_model : AS_OAI_DEFAULT_MODEL);
    bool ok = A.cfg.oai_key[0] != 0;
    /* not as_oai_headers': this answer isn't streamed */
    snprintf(hdr, hn, "Authorization: Bearer %s\r\ncontent-type: application/json\r\naccept: application/json\r\n",
             A.cfg.oai_key);
    pthread_mutex_unlock(&A.lock);
    size_t l = strlen(url);
    while (l && url[l - 1] == '/') url[--l] = 0;
    snprintf(url + l, un - l, "/v1/chat/completions");
    return ok;
}

bool assist_ready(char *why, size_t n)
{
    pthread_mutex_lock(&A.lock);
    as_route_t route = A.cfg.route;
    bool have_key = route == AS_ROUTE_OPENAI ? A.cfg.oai_key[0] != 0 : A.cfg.api_key[0] != 0;
    pthread_mutex_unlock(&A.lock);
    if (route != AS_ROUTE_LINK) {
        if (!have_key) {
            snprintf(why, n, "%s", route == AS_ROUTE_OPENAI ? "no OpenAI key: add one in settings (assistant)"
                                                              : "no API key: add one in settings, or use Catalyst Link");
            return false;
        }
        hal_net_t net;
        hal_net(&net);
        if (!net.up) {
            snprintf(why, n, "Wi-Fi isn't connected: %s needs the internet", route == AS_ROUTE_OPENAI ? "OpenAI" : "Claude");
            return false;
        }
        if (n) why[0] = 0;
        return true;
    }
    link_status_t ls;
    link_status(&ls);
    if (!ls.configured) snprintf(why, n, "Catalyst Link isn't set up (or add an API key)");
    else if (!ls.reachable) snprintf(why, n, "Catalyst Link at %s isn't answering", ls.url);
    else if (!ls.auth) snprintf(why, n, "Catalyst Link at %s doesn't take this token: check it in settings", ls.url);
    else if (!ls.claude && !strcmp(ls.claude_via, "claude-code"))
        snprintf(why, n, "Claude Code on the PC isn't ready: run `catalyst-link claude-check` there");
    else if (!ls.claude) snprintf(why, n, "Catalyst Link can't reach Claude: log in Claude Code on the PC, or set ANTHROPIC_API_KEY");
    else {
        if (n) why[0] = 0;
        return true;
    }
    return false;
}

bool assist_send(const char *text)
{
    if (!text || !text[0]) return false;
    pthread_mutex_lock(&A.lock);
    /* busy, unless the exchange under way was stopped and is only letting go of the network (a request can
     * take its timeout to give up): then this question waits its turn behind it instead of being refused */
    bool draining = A.running && A.stop && !A.pending;
    if ((A.busy && !draining) || !A.started) {
        pthread_mutex_unlock(&A.lock);
        return false;
    }
    char *t = as_strdup(text);
    if (!t) {
        pthread_mutex_unlock(&A.lock);
        return false;
    }
    ent_add(AS_E_USER, text, NULL);
    A.busy = true;
    if (!draining) A.stop = false; /* (the stopped exchange must stay stopped; the worker clears it after) */
    A.phase = AS_PHASE_SENDING;
    A.pending = t;
    pthread_cond_signal(&A.wake);
    pthread_mutex_unlock(&A.lock);
    bump();
    return true;
}

void assist_stop(void)
{
    pthread_mutex_lock(&A.lock);
    bool was = A.busy;
    if (A.pending) {
        /* not started yet: it never will */
        free(A.pending);
        A.pending = NULL;
    }
    if (A.running) A.stop = true;
    A.busy = A.running;
    if (A.confirm.pending && !A.answer) A.answer = 2;
    if (was) {
        /* stopped now, as far as the technician can tell: the rings rest, and the next question may be asked
         * while the old request, if it is stuck on the network, gives up in the background */
        ent_add(AS_E_NOTE, "Stopped.", NULL);
        A.phase = AS_PHASE_IDLE;
    }
    pthread_mutex_unlock(&A.lock);
    bump();
}

void assist_reset(void)
{
    pthread_mutex_lock(&A.lock);
    if (A.busy) A.stop = true;
    if (A.confirm.pending && !A.answer) A.answer = 2;
    A.confirm.pending = false;
    A.gen++;
    clear_locked();
    memset(&A.usage, 0, sizeof A.usage);
    if (A.pending) {
        free(A.pending);
        A.pending = NULL;
    }
    A.busy = A.running; /* an exchange still letting go of the network is stopped (its gen is old) */
    A.phase = AS_PHASE_IDLE;
    pthread_mutex_unlock(&A.lock);
    bump();
}

as_phase_t assist_phase(void)
{
    pthread_mutex_lock(&A.lock);
    as_phase_t p = A.phase;
    pthread_mutex_unlock(&A.lock);
    return p;
}

void assist_lock(void) { pthread_mutex_lock(&A.lock); }
void assist_unlock(void) { pthread_mutex_unlock(&A.lock); }
int assist_count(void) { return A.n; }
int assist_first(void) { return A.dropped; }
const as_entry_t *assist_entry(int i) { return i >= 0 && i < A.n ? &A.ent[i].e : NULL; }

void assist_usage(as_usage_t *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.usage;
    pthread_mutex_unlock(&A.lock);
}

int assist_suggestions(const char **out, int max)
{
    char s[5][72];
    int n = 0;
    pthread_mutex_lock(&g_robot_lock);
    const cat_robot_t *r = g_have_fed ? g_fed : NULL;
    if (!r || !r->connected) {
        snprintf(s[n++], 72, "Why can't we connect to the robot?");
    } else {
        if (r->browned_out || r->brownout_risk) snprintf(s[n++], 72, "Why did we brown out?");
        else if (r->have_battery && r->battery_v < 12.2) snprintf(s[n++], 72, "Is this battery OK for a match?");
        snprintf(s[n++], 72, "Run preflight");
        /* the first erroring subsystem by name, from an error alert's source */
        for (int i = 0; i < r->nalerts && n < 4; i++) {
            if (r->alerts[i].sev != CAT_SEV_ERROR) continue;
            char src[32];
            snprintf(src, sizeof src, "%s", r->alerts[i].source);
            char *p = src;
            if (*p == '[') p++;
            size_t l = strlen(p);
            if (l && p[l - 1] == ']') p[--l] = 0;
            if (l) {
                snprintf(s[n++], 72, "What's wrong with %s?", p);
                break;
            }
        }
        if (n < 4 && r->loop_over) snprintf(s[n++], 72, "Why is the loop overrunning?");
        if (r->n_errors + r->n_warnings > 0) snprintf(s[n++], 72, "Send today's issues to the PC");
    }
    pthread_mutex_unlock(&g_robot_lock);
    if (n > 5) n = 5;
    pthread_mutex_lock(&A.lock);
    for (int i = 0; i < n; i++) memcpy(A.sugg[i], s[i], sizeof s[i]);
    pthread_mutex_unlock(&A.lock);
    int k = n < max ? n : max;
    /* valid until the next call */
    for (int i = 0; i < k; i++) out[i] = A.sugg[i];
    return k;
}
