#include "as_oai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_tools.h"

/* ---- the request ---- */

static const char *btype(const aj_t *b) { return aj_gets(b, "type"); }

/* The text blocks of a message, joined by blank lines. */
static void join_text(const aj_t *content, ab_t *out)
{
    bool any = false;
    for (int k = 0; content && k < content->n; k++) {
        const aj_t *b = content->kid[k];
        const char *t = btype(b);
        const char *s = aj_gets(b, "text");
        if (!t || strcmp(t, "text") != 0 || !s || !s[0]) continue;
        if (any) ab_puts(out, "\n\n");
        ab_puts(out, s);
        any = true;
    }
}

/* A tool_result's content: a string, or an array of text blocks. */
static void result_text(const aj_t *b, ab_t *out)
{
    const aj_t *c = aj_get(b, "content");
    if (c && c->type == AJ_STR) ab_raw(out, c->s, c->len);
    else if (c && c->type == AJ_ARR) join_text(c, out);
    if (!out->n) ab_puts(out, "(no output)");
    if (aj_is(aj_get(b, "is_error"), AJ_TRUE)) {
        /* Chat Completions has no error flag on a tool message: say it in the text */
        ab_t t;
        ab_init(&t);
        ab_puts(&t, "ERROR: ");
        ab_raw(&t, out->p, out->n);
        ab_reset(out);
        ab_raw(out, t.p, t.n);
        ab_free(&t);
    }
}

static void user_msgs(ab_t *out, const aj_t *content, bool *first)
{
    ab_t t;
    ab_init(&t);
    /* tool results first: they must follow the assistant message that asked for them */
    for (int k = 0; content && k < content->n; k++) {
        const aj_t *b = content->kid[k];
        const char *ty = btype(b);
        if (!ty || strcmp(ty, "tool_result") != 0) continue;
        ab_reset(&t);
        result_text(b, &t);
        ab_puts(out, *first ? "" : ",");
        *first = false;
        ab_puts(out, "{\"role\":\"tool\",\"tool_call_id\":");
        ab_str(out, aj_gets(b, "tool_use_id") ? aj_gets(b, "tool_use_id") : "");
        ab_puts(out, ",\"content\":");
        ab_strn(out, t.p ? t.p : "", t.n);
        ab_puts(out, "}");
    }
    ab_reset(&t);
    join_text(content, &t);
    if (t.n) {
        ab_puts(out, *first ? "" : ",");
        *first = false;
        ab_puts(out, "{\"role\":\"user\",\"content\":");
        ab_strn(out, t.p, t.n);
        ab_puts(out, "}");
    }
    ab_free(&t);
}

static void assistant_msg(ab_t *out, const aj_t *content, bool *first)
{
    ab_t t;
    ab_init(&t);
    join_text(content, &t);
    int calls = 0;
    for (int k = 0; content && k < content->n; k++) {
        const char *ty = btype(content->kid[k]);
        calls += ty && !strcmp(ty, "tool_use");
    }
    if (!t.n && !calls) { /* only thinking (a Claude turn): nothing Chat Completions can take */
        ab_free(&t);
        return;
    }
    ab_puts(out, *first ? "" : ",");
    *first = false;
    ab_puts(out, "{\"role\":\"assistant\",\"content\":");
    if (t.n) ab_strn(out, t.p, t.n);
    else ab_puts(out, "null");
    if (calls) {
        ab_puts(out, ",\"tool_calls\":[");
        bool fc = true;
        for (int k = 0; k < content->n; k++) {
            const aj_t *b = content->kid[k];
            const char *ty = btype(b);
            if (!ty || strcmp(ty, "tool_use") != 0) continue;
            ab_puts(out, fc ? "" : ",");
            fc = false;
            ab_puts(out, "{\"id\":");
            ab_str(out, aj_gets(b, "id") ? aj_gets(b, "id") : "");
            ab_puts(out, ",\"type\":\"function\",\"function\":{\"name\":");
            ab_str(out, aj_gets(b, "name") ? aj_gets(b, "name") : "");
            ab_puts(out, ",\"arguments\":");
            const aj_t *in = aj_get(b, "input");
            char *args = in ? aj_dump(in) : NULL;
            ab_str(out, args ? args : "{}");
            free(args);
            ab_puts(out, "}}");
        }
        ab_puts(out, "]");
    }
    ab_puts(out, "}");
    ab_free(&t);
}

void as_oai_request(const as_hist_t *h, const as_req_t *q, ab_t *out)
{
    ab_reset(out);
    ab_puts(out, "{\"model\":");
    ab_str(out, q->model && q->model[0] ? q->model : AS_OAI_DEFAULT_MODEL);
    /* no max_completion_tokens: the model's own ceiling (a small cap would cut tool calls short) */
    ab_puts(out, ",\"stream\":true,\"stream_options\":{\"include_usage\":true}");
    if (q->tools && q->tools[0]) {
        ab_puts(out, ",\"tools\":[");
        for (int i = 0; i < AS_NTOOLS; i++) {
            if (i) ab_puts(out, ",");
            ab_puts(out, "{\"type\":\"function\",\"function\":{\"name\":");
            ab_str(out, AS_TOOLS[i].name);
            ab_puts(out, ",\"description\":");
            ab_str(out, AS_TOOLS[i].description);
            ab_puts(out, ",\"parameters\":");
            ab_puts(out, AS_TOOLS[i].schema);
            ab_puts(out, "}}");
        }
        ab_puts(out, "]");
    }
    ab_puts(out, ",\"messages\":[{\"role\":\"system\",\"content\":");
    ab_str(out, q->system ? q->system : "");
    ab_puts(out, "}");
    bool first = false; /* the system message is already in */
    for (int i = 0; i < h->n; i++) {
        if (h->m[i].user) user_msgs(out, h->m[i].content, &first);
        else assistant_msg(out, h->m[i].content, &first);
    }
    ab_puts(out, "]}");
}

void as_oai_headers(const char *key, char *out, size_t n)
{
    snprintf(out, n, "Authorization: Bearer %s\r\ncontent-type: application/json\r\naccept: text/event-stream\r\n",
             key ? key : "");
}

/* ---- the stream: each chunk becomes the Messages API events it stands for ---- */

void as_oai_init(as_oai_t *o, as_msg_t *m)
{
    memset(o, 0, sizeof *o);
    o->m = m;
    o->text = -1;
    for (int i = 0; i < AS_OAI_MAX_CALLS; i++) o->call[i] = -1;
}

static void emit(as_oai_t *o, const char *type, ab_t *b)
{
    if (b->oom) return;
    as_msg_event(o->m, type, b->p ? b->p : "{}", b->n);
    ab_reset(b);
}

static void start(as_oai_t *o, const aj_t *d, ab_t *b)
{
    if (o->started) return;
    o->started = true;
    const char *id = aj_gets(d, "id"), *model = aj_gets(d, "model");
    ab_puts(b, "{\"type\":\"message_start\",\"message\":{\"id\":");
    ab_str(b, id ? id : "");
    ab_puts(b, ",\"type\":\"message\",\"role\":\"assistant\",\"model\":");
    ab_str(b, model ? model : "");
    ab_puts(b, ",\"content\":[],\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}");
    emit(o, "message_start", b);
}

static void text_delta(as_oai_t *o, const aj_t *v, ab_t *b)
{
    if (!v || v->type != AJ_STR || !v->len) return;
    if (o->text < 0) {
        o->text = o->next++;
        ab_fmt(b, "{\"type\":\"content_block_start\",\"index\":%d,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}", o->text);
        emit(o, "content_block_start", b);
    }
    ab_fmt(b, "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":\"text_delta\",\"text\":", o->text);
    ab_strn(b, v->s, v->len);
    ab_puts(b, "}}");
    emit(o, "content_block_delta", b);
}

static void tool_delta(as_oai_t *o, const aj_t *tc, int pos, ab_t *b)
{
    int idx = (int)aj_getn(tc, "index", pos);
    if (idx < 0 || idx >= AS_OAI_MAX_CALLS) return;
    const aj_t *fn = aj_get(tc, "function");
    if (o->call[idx] < 0) {
        o->call[idx] = o->next++;
        const char *id = aj_gets(tc, "id"), *name = aj_gets(fn, "name");
        char synth[32];
        if (!id || !id[0]) {
            snprintf(synth, sizeof synth, "call_tab_%d", idx);
            id = synth;
        }
        ab_fmt(b, "{\"type\":\"content_block_start\",\"index\":%d,\"content_block\":{\"type\":\"tool_use\",\"id\":", o->call[idx]);
        ab_str(b, id);
        ab_puts(b, ",\"name\":");
        ab_str(b, name ? name : "");
        ab_puts(b, ",\"input\":{}}}");
        emit(o, "content_block_start", b);
    }
    const aj_t *args = aj_get(fn, "arguments");
    if (args && args->type == AJ_STR && args->len) {
        ab_fmt(b, "{\"type\":\"content_block_delta\",\"index\":%d,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":",
               o->call[idx]);
        ab_strn(b, args->s, args->len);
        ab_puts(b, "}}");
        emit(o, "content_block_delta", b);
    }
}

static const char *stop_reason(const char *finish, bool refusal)
{
    if (refusal || !strcmp(finish, "content_filter")) return "refusal";
    if (!strcmp(finish, "tool_calls") || !strcmp(finish, "function_call")) return "tool_use";
    if (!strcmp(finish, "length")) return "max_tokens";
    return "end_turn";
}

static void finish(as_oai_t *o, const char *reason, ab_t *b)
{
    if (o->finished) return;
    o->finished = true;
    for (int i = 0; i < o->next; i++) {
        ab_fmt(b, "{\"type\":\"content_block_stop\",\"index\":%d}", i);
        emit(o, "content_block_stop", b);
    }
    ab_puts(b, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":");
    ab_str(b, stop_reason(reason ? reason : "stop", o->refusal));
    ab_puts(b, "}}");
    emit(o, "message_delta", b);
}

static void usage(as_oai_t *o, const aj_t *u, ab_t *b)
{
    if (!u || u->type != AJ_OBJ) return;
    o->prompt_tokens = (int)aj_getn(u, "prompt_tokens", o->prompt_tokens);
    o->completion_tokens = (int)aj_getn(u, "completion_tokens", o->completion_tokens);
    o->cached_tokens = (int)aj_getn(aj_get(u, "prompt_tokens_details"), "cached_tokens", o->cached_tokens);
    int fresh = o->prompt_tokens - o->cached_tokens;
    ab_fmt(b, "{\"type\":\"message_delta\",\"delta\":{},\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d,"
              "\"cache_read_input_tokens\":%d}}",
           fresh < 0 ? 0 : fresh, o->completion_tokens, o->cached_tokens);
    emit(o, "message_delta", b);
}

static void stop(as_oai_t *o, ab_t *b)
{
    if (o->stopped) return;
    o->stopped = true;
    ab_puts(b, "{\"type\":\"message_stop\"}");
    emit(o, "message_stop", b);
}

void as_oai_sse(void *user, const char *event, const char *data, size_t len)
{
    (void)event;
    as_oai_t *o = user;
    ab_t b;
    ab_init(&b);
    while (len && (*data == ' ' || *data == '\n')) {
        data++;
        len--;
    }
    if (len == 6 && !memcmp(data, "[DONE]", 6)) {
        /* the end: complete what finish_reason already closed (without one, the message stays open and
         * counts as cut off) */
        if (o->finished) stop(o, &b);
        ab_free(&b);
        return;
    }
    char err[96];
    aj_t *d = aj_parse(data, len, err, sizeof err);
    if (!d) {
        ab_puts(&b, "{\"type\":\"error\",\"error\":{\"type\":\"stream_error\",\"message\":");
        char msg[160];
        snprintf(msg, sizeof msg, "unreadable chunk from OpenAI (%s)", err);
        ab_str(&b, msg);
        ab_puts(&b, "}}");
        emit(o, "error", &b);
        ab_free(&b);
        return;
    }
    const aj_t *e = aj_get(d, "error");
    if (e && e->type == AJ_OBJ) {
        const char *t = aj_gets(e, "type"), *c = aj_gets(e, "code"), *msg = aj_gets(e, "message");
        ab_puts(&b, "{\"type\":\"error\",\"error\":{\"type\":");
        ab_str(&b, c ? c : t ? t : "api_error");
        ab_puts(&b, ",\"message\":");
        ab_str(&b, msg ? msg : "OpenAI reported an error mid-answer");
        ab_puts(&b, "}}");
        emit(o, "error", &b);
        aj_free(d);
        ab_free(&b);
        return;
    }
    start(o, d, &b);
    const aj_t *ch = aj_at(aj_get(d, "choices"), 0);
    if (ch) {
        const aj_t *dl = aj_get(ch, "delta");
        text_delta(o, aj_get(dl, "content"), &b);
        const aj_t *rf = aj_get(dl, "refusal");
        if (rf && rf->type == AJ_STR && rf->len) {
            o->refusal = true;
            text_delta(o, rf, &b);
        }
        const aj_t *tcs = aj_get(dl, "tool_calls");
        for (int i = 0; tcs && tcs->type == AJ_ARR && i < tcs->n; i++) tool_delta(o, tcs->kid[i], i, &b);
        const char *fr = aj_gets(ch, "finish_reason");
        if (fr) finish(o, fr, &b);
    }
    usage(o, aj_get(d, "usage"), &b);
    aj_free(d);
    ab_free(&b);
}

void as_oai_end(as_oai_t *o)
{
    ab_t b;
    ab_init(&b);
    if (o->finished) stop(o, &b);
    ab_free(&b);
}

void as_oai_error(const char *body, char *msg, size_t mn, char *code, size_t cn)
{
    aj_t *d = aj_parse(body, strlen(body), NULL, 0);
    const aj_t *e = aj_get(d, "error");
    const char *m = e && e->type == AJ_STR ? e->s : aj_gets(e, "message");
    const char *c = aj_gets(e, "code");
    if (!c) c = aj_gets(e, "type");
    if (mn) snprintf(msg, mn, "%s", m ? m : body[0] ? body : "no details");
    if (cn) snprintf(code, cn, "%s", c ? c : "");
    if (mn > 200 && strlen(msg) > 200) strcpy(msg + 197, "...");
    aj_free(d);
}
