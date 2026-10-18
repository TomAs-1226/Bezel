#include "as_sse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- lexer (the WHATWG event-stream rules: CR, LF or CRLF end a line; a blank line dispatches) ---- */

void as_sse_init(as_sse_t *s) { memset(s, 0, sizeof *s); }

void as_sse_free(as_sse_t *s)
{
    ab_free(&s->line);
    ab_free(&s->data);
    memset(s, 0, sizeof *s);
}

static void line(as_sse_t *s, as_sse_fn fn, void *user)
{
    const char *l = s->line.p ? s->line.p : "";
    size_t n = s->line.n;
    if (n == 0) {
        if (s->have_data) {
            size_t dn = s->data.n;
            if (dn && s->data.p[dn - 1] == '\n') s->data.p[--dn] = 0;
            fn(user, s->event[0] ? s->event : "message", s->data.p ? s->data.p : "", dn);
        }
        ab_reset(&s->data);
        s->have_data = false;
        s->event[0] = 0;
        return;
    }
    if (l[0] == ':') return; /* a comment: keep-alives, proxies' notes */
    const char *colon = memchr(l, ':', n);
    size_t fl = colon ? (size_t)(colon - l) : n;
    const char *v = colon ? colon + 1 : l + n;
    size_t vl = (size_t)(l + n - v);
    if (vl && *v == ' ') {
        v++;
        vl--;
    }
    if (fl == 4 && !memcmp(l, "data", 4)) {
        ab_raw(&s->data, v, vl);
        ab_raw(&s->data, "\n", 1);
        s->have_data = true;
    } else if (fl == 5 && !memcmp(l, "event", 5)) {
        size_t k = vl < sizeof s->event - 1 ? vl : sizeof s->event - 1;
        memcpy(s->event, v, k);
        s->event[k] = 0;
    }
    /* id and retry mean nothing to a single POST */
}

void as_sse_feed(as_sse_t *s, const char *buf, size_t n, as_sse_fn fn, void *user)
{
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c != '\r' && c != '\n') {
            s->cr = false;
            continue;
        }
        ab_raw(&s->line, buf + run, i - run);
        run = i + 1;
        if (c == '\n' && s->cr) {
            s->cr = false; /* the LF of a CRLF whose CR already ended the line */
            continue;
        }
        s->cr = c == '\r';
        line(s, fn, user);
        ab_reset(&s->line);
    }
    ab_raw(&s->line, buf + run, n - run);
}

/* ---- the message ---- */

void as_msg_init(as_msg_t *m, const as_msg_cb_t *cb)
{
    memset(m, 0, sizeof *m);
    if (cb) m->cb = *cb;
}

void as_msg_free(as_msg_t *m)
{
    for (int i = 0; i < m->n; i++) {
        aj_free(m->b[i].block);
        ab_free(&m->b[i].partial);
    }
    free(m->b);
    aj_free(m->stop_details);
    memset(m, 0, sizeof *m);
}

static as_block_t *slot(as_msg_t *m, int index)
{
    if (index < 0 || index > 4096) return NULL;
    if (index >= m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        while (cap <= index) cap *= 2;
        as_block_t *b = realloc(m->b, (size_t)cap * sizeof *b);
        if (!b) return NULL;
        memset(b + m->cap, 0, (size_t)(cap - m->cap) * sizeof *b);
        m->b = b;
        m->cap = cap;
    }
    if (index >= m->n) m->n = index + 1;
    return &m->b[index];
}

static void set_error(as_msg_t *m, const char *type, const char *msg)
{
    m->error = true;
    snprintf(m->error_type, sizeof m->error_type, "%s", type ? type : "error");
    snprintf(m->error_msg, sizeof m->error_msg, "%s", msg ? msg : "");
}

static void read_usage(as_msg_t *m, const aj_t *u)
{
    if (!u) return;
    aj_t *v;
    if ((v = aj_get(u, "input_tokens")) && v->type == AJ_NUM) m->input_tokens = (int)v->num;
    if ((v = aj_get(u, "output_tokens")) && v->type == AJ_NUM) m->output_tokens = (int)v->num;
    if ((v = aj_get(u, "cache_read_input_tokens")) && v->type == AJ_NUM) m->cache_read = (int)v->num;
    if ((v = aj_get(u, "cache_creation_input_tokens")) && v->type == AJ_NUM) m->cache_write = (int)v->num;
    /* the served-by signal for a server-side fallback: a fallback_message attempt */
    const aj_t *it = aj_get(u, "iterations");
    for (int i = 0; it && i < it->n; i++) {
        const char *t = aj_gets(it->kid[i], "type");
        if (t && !strcmp(t, "fallback_message")) m->fallback = true;
    }
}

static void append_member(aj_t *block, const char *key, const char *s, size_t n)
{
    aj_t *v = aj_get(block, key);
    if (!v || v->type != AJ_STR) {
        aj_set(block, key, aj_new_strn(s, n));
        return;
    }
    aj_str_append(v, s, n);
}

void as_msg_event(as_msg_t *m, const char *event, const char *data, size_t len)
{
    char err[96];
    aj_t *d = aj_parse(data, len, err, sizeof err);
    if (!d) {
        char msg[160];
        snprintf(msg, sizeof msg, "unreadable %s event (%s)", event, err);
        set_error(m, "stream_error", msg);
        return;
    }
    const char *type = aj_gets(d, "type");
    if (!type) type = event;
    int index = (int)aj_getn(d, "index", -1);

    if (!strcmp(type, "message_start")) {
        const aj_t *msg = aj_get(d, "message");
        m->started = true;
        snprintf(m->id, sizeof m->id, "%s", aj_gets(msg, "id") ? aj_gets(msg, "id") : "");
        snprintf(m->model, sizeof m->model, "%s", aj_gets(msg, "model") ? aj_gets(msg, "model") : "");
        read_usage(m, aj_get(msg, "usage"));
    } else if (!strcmp(type, "content_block_start")) {
        as_block_t *b = slot(m, index);
        const aj_t *cb = aj_get(d, "content_block");
        if (b && cb && cb->type == AJ_OBJ) {
            aj_free(b->block);
            ab_free(&b->partial);
            memset(b, 0, sizeof *b);
            b->block = aj_clone(cb);
            free(b->block->key);
            b->block->key = NULL;
            b->open = true;
            const char *bt = aj_gets(cb, "type");
            if (bt && !strcmp(bt, "fallback")) m->fallback = true;
            if (m->cb.start) m->cb.start(m->cb.user, index, b->block);
        }
    } else if (!strcmp(type, "content_block_delta")) {
        as_block_t *b = index >= 0 && index < m->n ? &m->b[index] : NULL;
        const aj_t *dl = aj_get(d, "delta");
        const char *dt = aj_gets(dl, "type");
        if (b && b->block && dt) {
            const aj_t *v;
            if (!strcmp(dt, "text_delta") && (v = aj_get(dl, "text")) && v->type == AJ_STR) {
                append_member(b->block, "text", v->s, v->len);
                if (m->cb.delta) m->cb.delta(m->cb.user, index, AS_D_TEXT, v->s, v->len);
            } else if (!strcmp(dt, "thinking_delta") && (v = aj_get(dl, "thinking")) && v->type == AJ_STR) {
                append_member(b->block, "thinking", v->s, v->len);
                if (m->cb.delta) m->cb.delta(m->cb.user, index, AS_D_THINKING, v->s, v->len);
            } else if (!strcmp(dt, "signature_delta") && (v = aj_get(dl, "signature")) && v->type == AJ_STR) {
                append_member(b->block, "signature", v->s, v->len);
            } else if (!strcmp(dt, "input_json_delta") && (v = aj_get(dl, "partial_json")) && v->type == AJ_STR) {
                ab_raw(&b->partial, v->s, v->len);
                if (m->cb.delta) m->cb.delta(m->cb.user, index, AS_D_INPUT, v->s, v->len);
            } else if (!strcmp(dt, "citations_delta") && (v = aj_get(dl, "citation"))) {
                aj_t *c = aj_get(b->block, "citations");
                if (!c || c->type != AJ_ARR) {
                    aj_set(b->block, "citations", aj_new(AJ_ARR));
                    c = aj_get(b->block, "citations");
                }
                aj_t *cl = aj_clone(v);
                free(cl->key);
                cl->key = NULL;
                aj_push(c, cl);
            }
        }
    } else if (!strcmp(type, "content_block_stop")) {
        as_block_t *b = index >= 0 && index < m->n ? &m->b[index] : NULL;
        if (b && b->block) {
            b->open = false;
            const char *bt = aj_gets(b->block, "type");
            bool tool = bt && (!strcmp(bt, "tool_use") || !strcmp(bt, "server_tool_use"));
            if (tool && b->partial.n) {
                /* Eager input streaming: the API didn't validate these fragments, so parse them strictly
                 * here; a failure is kept (with the raw text) for an INVALID_JSON tool result. */
                aj_t *in = aj_parse(b->partial.p, b->partial.n, b->input_err, sizeof b->input_err);
                if (in && in->type == AJ_OBJ) {
                    aj_set(b->block, "input", in);
                } else {
                    if (in) snprintf(b->input_err, sizeof b->input_err, "input is not a JSON object");
                    aj_free(in);
                    b->bad_input = true;
                }
            } else if (tool && !aj_get(b->block, "input")) {
                aj_set(b->block, "input", aj_new(AJ_OBJ));
            }
            if (m->cb.stop) m->cb.stop(m->cb.user, index, b);
        }
    } else if (!strcmp(type, "message_delta")) {
        const aj_t *dl = aj_get(d, "delta");
        const char *sr = aj_gets(dl, "stop_reason");
        if (sr) snprintf(m->stop_reason, sizeof m->stop_reason, "%s", sr);
        const aj_t *sd = aj_get(dl, "stop_details");
        if (!sd) sd = aj_get(d, "stop_details");
        if (sd && sd->type == AJ_OBJ) {
            aj_free(m->stop_details);
            m->stop_details = aj_clone(sd);
        }
        read_usage(m, aj_get(d, "usage"));
    } else if (!strcmp(type, "message_stop")) {
        m->done = true;
    } else if (!strcmp(type, "error")) {
        const aj_t *e = aj_get(d, "error");
        set_error(m, aj_gets(e, "type"), aj_gets(e, "message"));
    }
    /* ping, and event types this client doesn't know yet, are ignored */
    aj_free(d);
}

void as_msg_sse(void *msg, const char *event, const char *data, size_t len) { as_msg_event(msg, event, data, len); }
