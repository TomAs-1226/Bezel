#include "as_conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void as_hist_init(as_hist_t *h) { memset(h, 0, sizeof *h); }

void as_hist_free(as_hist_t *h)
{
    for (int i = 0; i < h->n; i++) aj_free(h->m[i].content);
    free(h->m);
    free(h->note);
    memset(h, 0, sizeof *h);
}

static as_hmsg_t *add(as_hist_t *h, bool user, bool exchange, aj_t *content)
{
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 16;
        as_hmsg_t *m = realloc(h->m, (size_t)cap * sizeof *m);
        if (!m) {
            aj_free(content);
            return NULL;
        }
        h->m = m;
        h->cap = cap;
    }
    as_hmsg_t *m = &h->m[h->n++];
    m->user = user;
    m->exchange = exchange;
    m->content = content;
    return m;
}

static aj_t *text_block(const char *s)
{
    aj_t *b = aj_new(AJ_OBJ);
    aj_set(b, "type", aj_new_str("text"));
    aj_set(b, "text", aj_new_str(s));
    return b;
}

void as_hist_user(as_hist_t *h, const char *note, const char *text)
{
    aj_t *c = aj_new(AJ_ARR);
    if (note && note[0]) aj_push(c, text_block(note));
    aj_push(c, text_block(text && text[0] ? text : "(empty)"));
    if (h->n == 0 && note && note[0]) {
        free(h->note);
        h->note = as_strdup(note);
    }
    add(h, true, true, c);
}

void as_hist_assistant(as_hist_t *h, aj_t *content) { add(h, false, false, content); }

void as_hist_results(as_hist_t *h, const as_result_t *r, int n)
{
    aj_t *c = aj_new(AJ_ARR);
    for (int i = 0; i < n; i++) {
        aj_t *b = aj_new(AJ_OBJ);
        aj_set(b, "type", aj_new_str("tool_result"));
        aj_set(b, "tool_use_id", aj_new_str(r[i].id));
        aj_set(b, "content", aj_new_str(r[i].content && r[i].content[0] ? r[i].content : "(no output)"));
        if (r[i].is_error) aj_set(b, "is_error", aj_new_bool(true));
        aj_push(c, b);
    }
    add(h, true, false, c);
}

static int last_exchange(const as_hist_t *h)
{
    for (int i = h->n - 1; i >= 0; i--) if (h->m[i].exchange) return i;
    return -1;
}

static void drop_from(as_hist_t *h, int from)
{
    for (int i = from; i < h->n; i++) aj_free(h->m[i].content);
    h->n = from;
}

void as_hist_rollback(as_hist_t *h)
{
    int k = last_exchange(h);
    if (k < 0) return;
    drop_from(h, k);
    if (h->n == 0) {
        free(h->note);
        h->note = NULL;
    }
}

/* ---- the request ---- */

static bool is_thinking(const aj_t *b)
{
    const char *t = aj_gets(b, "type");
    return t && (!strcmp(t, "thinking") || !strcmp(t, "redacted_thinking"));
}

void as_conv_request(const as_hist_t *h, const as_req_t *q, ab_t *out)
{
    ab_reset(out);
    ab_puts(out, "{\"model\":");
    ab_str(out, q->model && q->model[0] ? q->model : AS_DEFAULT_MODEL);
    ab_fmt(out, ",\"max_tokens\":%d,\"stream\":true", q->max_tokens > 0 ? q->max_tokens : AS_MAX_TOKENS);
    /* On this model raw thinking is never returned; "summarized" gives a summary worth showing. */
    ab_puts(out, ",\"thinking\":{\"type\":\"adaptive\",\"display\":\"summarized\"}");
    ab_puts(out, ",\"output_config\":{\"effort\":");
    ab_str(out, q->effort && q->effort[0] ? q->effort : "medium");
    ab_puts(out, "}");
    /* A safety classifier's decline re-runs server-side on the recommended model for its category. */
    ab_puts(out, ",\"fallbacks\":\"default\"");
    /* The tools and the system prompt are a large, stable prefix: cache it. */
    ab_puts(out, ",\"system\":[{\"type\":\"text\",\"text\":");
    ab_str(out, q->system ? q->system : "");
    ab_puts(out, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");
    if (q->tools && q->tools[0]) {
        ab_puts(out, ",\"tools\":");
        ab_puts(out, q->tools);
    }
    ab_puts(out, ",\"messages\":[");
    for (int i = 0; i < h->n; i++) {
        const as_hmsg_t *m = &h->m[i];
        if (i) ab_puts(out, ",");
        ab_puts(out, m->user ? "{\"role\":\"user\",\"content\":[" : "{\"role\":\"assistant\",\"content\":[");
        /* the conversation so far is cached up to the last block of the last message (not a thinking
         * block, which can't carry cache_control) */
        int mark = -1;
        if (i == h->n - 1)
            for (int k = m->content->n - 1; k >= 0 && mark < 0; k--) if (!is_thinking(m->content->kid[k])) mark = k;
        for (int k = 0; k < m->content->n; k++) {
            if (k) ab_puts(out, ",");
            const aj_t *b = m->content->kid[k];
            aj_write(out, b);
            if (k == mark && out->n && out->p[out->n - 1] == '}') {
                out->n--;
                ab_puts(out, b->n ? ",\"cache_control\":{\"type\":\"ephemeral\"}}" : "\"cache_control\":{\"type\":\"ephemeral\"}}");
            }
        }
        ab_puts(out, "]}");
    }
    ab_puts(out, "]}");
}

void as_conv_headers(const char *key, char *out, size_t n)
{
    snprintf(out, n,
             "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\ncontent-type: application/json\r\n"
             "anthropic-beta: " AS_BETA "\r\naccept: text/event-stream\r\n",
             key ? key : "");
}

int as_conv_trim(as_hist_t *h, const as_req_t *q, size_t limit)
{
    int dropped = 0;
    ab_t b;
    ab_init(&b);
    for (;;) {
        as_conv_request(h, q, &b);
        if (b.n <= limit) break;
        int current = last_exchange(h), next = -1;
        for (int i = 1; i < h->n; i++) if (h->m[i].exchange) { next = i; break; }
        /* the exchange in progress stays whole, even if it alone is over the limit */
        if (next < 0 || next > current || !h->m[0].exchange) break;
        for (int i = 0; i < next; i++) aj_free(h->m[i].content);
        memmove(h->m, h->m + next, (size_t)(h->n - next) * sizeof *h->m);
        h->n -= next;
        dropped++;
        if (h->note) {
            const aj_t *first = aj_at(h->m[0].content, 0);
            const char *t = aj_gets(first, "text");
            if (!t || strcmp(t, h->note) != 0) aj_insert(h->m[0].content, 0, text_block(h->note));
        }
    }
    ab_free(&b);
    if (dropped) {
        /* A thinking block's signature covers the conversation before it; with the front gone those
         * before the current exchange would no longer verify, so they go (the text and tool calls stay). */
        int current = last_exchange(h);
        for (int i = 0; i < current; i++) {
            aj_t *c = h->m[i].content;
            if (h->m[i].user) continue;
            int keep = 0;
            for (int k = 0; k < c->n; k++) keep += !is_thinking(c->kid[k]);
            if (!keep) continue; /* never leave an assistant message empty */
            for (int k = c->n - 1; k >= 0; k--) if (is_thinking(c->kid[k])) aj_remove(c, k);
        }
        h->trimmed += dropped;
    }
    return dropped;
}

/* ---- after a response ---- */

static const char *btype(const as_block_t *b) { return b->block ? aj_gets(b->block, "type") : NULL; }

static int last_fallback(const as_msg_t *m)
{
    int fb = -1;
    for (int i = 0; i < m->n; i++) {
        const char *t = btype(&m->b[i]);
        if (t && !strcmp(t, "fallback")) fb = i;
    }
    return fb;
}

/* Whether block i goes back in the history. */
static bool echoed(const as_msg_t *m, int i, int fb)
{
    const char *t = btype(&m->b[i]);
    if (!t || !strcmp(t, "fallback")) return false;
    if (!strcmp(t, "text")) {
        const char *s = aj_gets(m->b[i].block, "text");
        return s && s[0]; /* the API refuses empty text blocks */
    }
    /* before a mid-output fallback only text carries over: the declined model's thinking and calls
     * are not part of the conversation the serving model continued */
    return i > fb;
}

aj_t *as_conv_echo(const as_msg_t *m)
{
    aj_t *c = aj_new(AJ_ARR);
    int fb = last_fallback(m);
    for (int i = 0; i < m->n; i++) {
        if (!echoed(m, i, fb)) continue;
        aj_t *b = aj_clone(m->b[i].block);
        if (m->b[i].bad_input) aj_set(b, "input", aj_new(AJ_OBJ));
        aj_push(c, b);
    }
    return c;
}

int as_conv_calls(const as_msg_t *m, as_call_t *out, int max)
{
    int n = 0, fb = last_fallback(m);
    for (int i = 0; i < m->n && n < max; i++) {
        const char *t = btype(&m->b[i]);
        if (!t || strcmp(t, "tool_use") != 0 || !echoed(m, i, fb)) continue;
        as_call_t *c = &out[n++];
        c->id = aj_gets(m->b[i].block, "id");
        c->name = aj_gets(m->b[i].block, "name");
        c->input = m->b[i].bad_input ? NULL : aj_get(m->b[i].block, "input");
        c->raw = m->b[i].partial.p ? m->b[i].partial.p : "";
        c->err = m->b[i].input_err;
        if (!c->id) c->id = "";
        if (!c->name) c->name = "";
    }
    return n;
}

as_next_t as_conv_next(const as_msg_t *m)
{
    if (m->error || !m->started || !m->done) return AS_NEXT_ERROR;
    const char *sr = m->stop_reason;
    if (!strcmp(sr, "refusal")) return AS_NEXT_REFUSED;
    bool tool = false;
    for (int i = 0; i < m->n && !tool; i++) {
        const char *t = btype(&m->b[i]);
        tool = t && !strcmp(t, "tool_use");
    }
    if (!strcmp(sr, "max_tokens")) return tool ? AS_NEXT_TRUNCATED : AS_NEXT_DONE;
    if (!strcmp(sr, "pause_turn")) return AS_NEXT_CONTINUE;
    if (!strcmp(sr, "tool_use")) {
        as_call_t c[1];
        return as_conv_calls(m, c, 1) ? AS_NEXT_TOOLS : AS_NEXT_DONE;
    }
    return AS_NEXT_DONE;
}

char *as_conv_invalid_json(const char *raw)
{
    ab_t b;
    ab_init(&b);
    ab_puts(&b, "{\"INVALID_JSON\":");
    ab_str(&b, raw ? raw : "");
    ab_puts(&b, "}");
    return ab_take(&b);
}
