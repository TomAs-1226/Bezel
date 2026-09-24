/* ccwatch — Claude Code's sessions on the PC, polled from Catalyst Link (see ccwatch.h).
 *
 * Everything sizeable lives on the heap (PSRAM past 512 bytes): internal RAM is nearly gone. */
#include "ccwatch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_json.h"
#include "hal.h"
#include "link.h"

#define BODY_MAX (24 * 1024)
#define EVENTS 8
#define RETRY_MISSING_S 60.0  /* an older Link without the endpoint: ask again this often */
#define FRESH_S 180.0         /* on first sight, a change this recent still asks for a look */

typedef struct {
    char id[48];
    uint32_t seq;
} ack_t;

static struct {
    pthread_mutex_t lock;
    ccw_session_t *s;       /* CCW_MAX */
    int n;
    bool available, seen_once;
    double missing_at;      /* the endpoint answered 404: when */
    uint32_t rev;
    ccw_event_t *ev;        /* EVENTS, a ring */
    int ev_head, ev_n;
    ack_t *acks;            /* CCW_MAX: acknowledged (id, seq) pairs */
    int nacks;
    char (*pending_ack)[48]; /* CCW_MAX ids to tell the Link, from the poller */
    int npending;
    bool ack_all;
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .missing_at = -1e9 };

static bool ensure(void)
{
    if (!C.s) C.s = calloc(CCW_MAX, sizeof *C.s);
    if (!C.ev) C.ev = calloc(EVENTS, sizeof *C.ev);
    if (!C.acks) C.acks = calloc(CCW_MAX, sizeof *C.acks);
    if (!C.pending_ack) C.pending_ack = calloc(CCW_MAX, sizeof *C.pending_ack);
    return C.s && C.ev && C.acks && C.pending_ack;
}

const char *ccw_state_word(ccw_state_t s)
{
    switch (s) {
    case CCW_RUNNING: return "running";
    case CCW_WAITING: return "needs input";
    case CCW_DONE: return "done";
    default: return "error";
    }
}

static ccw_state_t parse_state(const char *s)
{
    if (!s) return CCW_RUNNING;
    if (!strcmp(s, "waiting_for_input")) return CCW_WAITING;
    if (!strcmp(s, "done")) return CCW_DONE;
    if (!strcmp(s, "error")) return CCW_ERROR;
    return CCW_RUNNING;
}

static void copy(char *out, size_t n, const aj_t *o, const char *key)
{
    const char *s = aj_gets(o, key);
    snprintf(out, n, "%s", s ? s : "");
}

/* (lock held) */
static bool acked(const char *id, uint32_t seq)
{
    for (int i = 0; i < C.nacks; i++) if (!strcmp(C.acks[i].id, id) && C.acks[i].seq == seq) return true;
    return false;
}

static void remember_ack(const char *id, uint32_t seq)
{
    for (int i = 0; i < C.nacks; i++)
        if (!strcmp(C.acks[i].id, id)) {
            C.acks[i].seq = seq;
            return;
        }
    if (C.nacks == CCW_MAX) {
        memmove(C.acks, C.acks + 1, (CCW_MAX - 1) * sizeof *C.acks);
        C.nacks--;
    }
    snprintf(C.acks[C.nacks].id, sizeof C.acks[C.nacks].id, "%s", id);
    C.acks[C.nacks++].seq = seq;
}

static void push_event(const ccw_session_t *s)
{
    int at = (C.ev_head + C.ev_n) % EVENTS;
    if (C.ev_n == EVENTS) { /* full: the oldest goes */
        C.ev_head = (C.ev_head + 1) % EVENTS;
        C.ev_n--;
        at = (C.ev_head + C.ev_n) % EVENTS;
    }
    ccw_event_t *e = &C.ev[at];
    snprintf(e->id, sizeof e->id, "%s", s->id);
    snprintf(e->title, sizeof e->title, "%s", s->title);
    snprintf(e->project, sizeof e->project, "%s", s->project);
    e->state = s->state;
    e->elapsed_s = s->elapsed_s;
    C.ev_n++;
}

static int parse(const char *body, ccw_session_t *out)
{
    aj_t *d = aj_parse(body, strlen(body), NULL, 0);
    const aj_t *a = aj_get(d, "sessions");
    int n = 0;
    double now = hal_seconds();
    for (int i = 0; a && a->type == AJ_ARR && i < a->n && n < CCW_MAX; i++) {
        const aj_t *o = a->kid[i];
        ccw_session_t *s = &out[n];
        memset(s, 0, sizeof *s);
        copy(s->id, sizeof s->id, o, "id");
        if (!s->id[0]) continue;
        copy(s->title, sizeof s->title, o, "title");
        copy(s->project, sizeof s->project, o, "project");
        copy(s->step, sizeof s->step, o, "step");
        s->state = parse_state(aj_gets(o, "state"));
        s->seq = (uint32_t)aj_getn(o, "seq", 0);
        s->tools = (int)aj_getn(o, "tools", 0);
        s->elapsed_s = aj_getn(o, "elapsed_s", 0);
        s->quiet_s = aj_getn(o, "quiet_s", 0);
        s->since_s = aj_getn(o, "since_s", 0);
        const aj_t *eta = aj_get(o, "eta");
        s->have_eta = eta && eta->type == AJ_OBJ && aj_get(eta, "remaining_s") && aj_get(eta, "remaining_s")->type == AJ_NUM;
        if (s->have_eta) {
            s->eta_s = aj_getn(eta, "remaining_s", 0);
            s->eta_lo_s = aj_getn(eta, "low_s", s->eta_s);
            s->eta_hi_s = aj_getn(eta, "high_s", s->eta_s);
            s->eta_samples = (int)aj_getn(eta, "samples", 0);
        }
        copy(s->basis, sizeof s->basis, eta && eta->type == AJ_OBJ ? eta : o, eta && eta->type == AJ_OBJ ? "basis" : "eta_basis");
        /* the Link's own acknowledgement (another tablet, or the CLI) counts too */
        if (aj_is(aj_get(o, "acked"), AJ_TRUE)) s->attention_at = -1;
        s->polled_at = now;
        n++;
    }
    aj_free(d);
    return n;
}

static void send_acks(void)
{
    char ids[CCW_MAX][48];
    int n;
    bool all;
    pthread_mutex_lock(&C.lock);
    n = C.npending;
    all = C.ack_all;
    if (n) memcpy(ids, C.pending_ack, (size_t)n * sizeof *ids);
    C.npending = 0;
    C.ack_all = false;
    pthread_mutex_unlock(&C.lock);
    char path[128], enc[100];
    if (all) link_post("/v1/claude/sessions/ack", "{}", NULL, 0);
    for (int i = 0; i < n && !all; i++) {
        as_urlencode(ids[i], enc, sizeof enc);
        snprintf(path, sizeof path, "/v1/claude/sessions/%s/ack", enc);
        link_post(path, "{}", NULL, 0);
    }
}

void ccw_poll(void)
{
    pthread_mutex_lock(&C.lock);
    bool ok = ensure();
    bool skip = hal_seconds() - C.missing_at < RETRY_MISSING_S;
    pthread_mutex_unlock(&C.lock);
    if (!ok || skip) return;
    send_acks();
    char *buf = malloc(BODY_MAX);
    ccw_session_t *fresh = calloc(CCW_MAX, sizeof *fresh);
    if (!buf || !fresh) {
        free(buf);
        free(fresh);
        return;
    }
    int st = link_get("/v1/claude/sessions", buf, BODY_MAX);
    if (st == 404) {
        pthread_mutex_lock(&C.lock);
        C.missing_at = hal_seconds();
        if (C.available) C.rev++;
        C.available = false;
        pthread_mutex_unlock(&C.lock);
    } else if (st == 200) {
        int n = parse(buf, fresh);
        pthread_mutex_lock(&C.lock);
        double now = hal_seconds();
        for (int i = 0; i < n; i++) {
            ccw_session_t *s = &fresh[i];
            const ccw_session_t *old = NULL;
            for (int k = 0; k < C.n && !old; k++) if (!strcmp(C.s[k].id, s->id)) old = &C.s[k];
            bool link_acked = s->attention_at < 0;
            s->attention_at = 0;
            bool settled = s->state != CCW_RUNNING;
            if (link_acked && settled) remember_ack(s->id, s->seq);
            if (settled && !acked(s->id, s->seq)) {
                /* news: a change seen happening, or on first sight (a boot, a new session) a recent one */
                bool news = old ? old->seq != s->seq || old->state != s->state : s->since_s < FRESH_S;
                if (!old && !news) {
                    remember_ack(s->id, s->seq); /* an old finish isn't news */
                } else {
                    s->attention = true;
                    s->attention_at = old && old->attention && old->seq == s->seq ? old->attention_at : now;
                    if (news) push_event(s);
                }
            }
        }
        bool diff = n != C.n;
        for (int i = 0; i < n && !diff; i++)
            diff = strcmp(fresh[i].id, C.s[i].id) || fresh[i].seq != C.s[i].seq || fresh[i].state != C.s[i].state ||
                   strcmp(fresh[i].step, C.s[i].step) || fresh[i].attention != C.s[i].attention ||
                   fresh[i].tools != C.s[i].tools || (int)fresh[i].eta_s / 30 != (int)C.s[i].eta_s / 30;
        memcpy(C.s, fresh, (size_t)n * sizeof *fresh);
        C.n = n;
        if (diff || !C.available) C.rev++;
        C.available = true;
        C.seen_once = true;
        pthread_mutex_unlock(&C.lock);
    }
    free(buf);
    free(fresh);
}

void ccw_offline(void)
{
    pthread_mutex_lock(&C.lock);
    if (C.available) C.rev++;
    C.available = false;
    pthread_mutex_unlock(&C.lock);
}

bool ccw_available(void)
{
    pthread_mutex_lock(&C.lock);
    bool a = C.available;
    pthread_mutex_unlock(&C.lock);
    return a;
}

uint32_t ccw_rev(void)
{
    pthread_mutex_lock(&C.lock);
    uint32_t r = C.rev;
    pthread_mutex_unlock(&C.lock);
    return r;
}

int ccw_list(ccw_session_t *out, int max)
{
    pthread_mutex_lock(&C.lock);
    int n = C.s ? (C.n < max ? C.n : max) : 0;
    if (n > 0) memcpy(out, C.s, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&C.lock);
    return n;
}

int ccw_attention(ccw_session_t *newest)
{
    pthread_mutex_lock(&C.lock);
    int count = 0, best = -1;
    for (int i = 0; C.s && i < C.n; i++) {
        if (!C.s[i].attention) continue;
        count++;
        if (best < 0 || C.s[i].attention_at > C.s[best].attention_at) best = i;
    }
    if (newest && best >= 0) *newest = C.s[best];
    pthread_mutex_unlock(&C.lock);
    return count;
}

bool ccw_next_event(ccw_event_t *out)
{
    pthread_mutex_lock(&C.lock);
    bool have = C.ev && C.ev_n > 0;
    if (have) {
        *out = C.ev[C.ev_head];
        C.ev_head = (C.ev_head + 1) % EVENTS;
        C.ev_n--;
    }
    pthread_mutex_unlock(&C.lock);
    return have;
}

void ccw_ack(const char *id)
{
    pthread_mutex_lock(&C.lock);
    if (!ensure()) {
        pthread_mutex_unlock(&C.lock);
        return;
    }
    bool any = false;
    for (int i = 0; i < C.n; i++) {
        if (id && strcmp(C.s[i].id, id) != 0) continue;
        if (C.s[i].attention) any = true;
        C.s[i].attention = false;
        remember_ack(C.s[i].id, C.s[i].seq);
        if (id && C.npending < CCW_MAX) snprintf(C.pending_ack[C.npending++], 48, "%s", C.s[i].id);
    }
    if (!id) C.ack_all = true;
    if (any) C.rev++;
    pthread_mutex_unlock(&C.lock);
}
