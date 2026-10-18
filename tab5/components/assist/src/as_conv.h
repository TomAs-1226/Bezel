/* as_conv — the conversation as the Messages API sees it: the history, the request body, what to do
 * after each streamed response, and how the history is kept bounded. Pure (no I/O, no threads) so the
 * unit tests can drive it with canned streams. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "as_json.h"
#include "as_sse.h"

#define AS_DEFAULT_MODEL "claude-opus-5"
#define AS_BETA "server-side-fallback-2026-07-01"
#define AS_MAX_TOKENS 32000
#define AS_MAX_TOKENS_RETRY 64000
#define AS_TRIM_BYTES (400 * 1024)

typedef struct {
    bool user;             /* role */
    bool exchange;         /* a user message the technician typed: starts an exchange */
    aj_t *content;         /* array of content blocks */
} as_hmsg_t;

typedef struct {
    as_hmsg_t *m;
    int n, cap;
    char *note;            /* the context note on the first user message; survives trimming */
    int trimmed;           /* exchanges dropped so far */
} as_hist_t;

void as_hist_init(as_hist_t *h);
void as_hist_free(as_hist_t *h);
/* A new exchange: the technician's words, preceded by a context note block when `note` isn't NULL.
 * The first exchange's note is remembered and kept on the first message whatever gets trimmed. */
void as_hist_user(as_hist_t *h, const char *note, const char *text);
/* The assistant's turn, echoed as the API wants it back (as_conv_echo). Takes `content`. */
void as_hist_assistant(as_hist_t *h, aj_t *content);
/* Drops everything from the last exchange's first message on (a refused question). */
void as_hist_rollback(as_hist_t *h);

typedef struct {
    const char *id;
    const char *content;   /* text (usually JSON) */
    bool is_error;
} as_result_t;
/* All of one turn's tool results, in one user message (parallel tool calls). */
void as_hist_results(as_hist_t *h, const as_result_t *r, int n);

typedef struct {
    const char *model;
    int max_tokens;
    const char *effort;    /* "medium" */
    const char *system;
    const char *tools;     /* the tools array as JSON text */
} as_req_t;

/* The request body: stream, adaptive thinking with summaries, effort, server-side fallbacks, the system
 * block and the last message's last block marked for prompt caching. */
void as_conv_request(const as_hist_t *h, const as_req_t *q, ab_t *out);
/* The headers for the direct route (each "Name: value\r\n"). */
void as_conv_headers(const char *api_key, char *out, size_t n);

/* Drops the oldest whole exchanges until the request fits in `limit` bytes. Never drops the exchange in
 * progress, never separates a tool_use from its tool_result (exchanges are dropped whole), keeps the
 * first message's context note, and strips thinking from what remains before the current exchange
 * (their signatures were bound to the dropped prefix). Returns the number of exchanges dropped. */
int as_conv_trim(as_hist_t *h, const as_req_t *q, size_t limit);

/* ---- after a response ---- */

typedef enum {
    AS_NEXT_DONE,          /* end_turn (or anything else final): the answer is in */
    AS_NEXT_TOOLS,         /* tool_use: run them, send the results */
    AS_NEXT_CONTINUE,      /* pause_turn: send it back as it is to continue */
    AS_NEXT_REFUSED,       /* refusal: run nothing from this turn */
    AS_NEXT_TRUNCATED,     /* max_tokens with a tool call in it: the input is probably cut off */
    AS_NEXT_ERROR,         /* an error event, or the stream ended before message_stop */
} as_next_t;

as_next_t as_conv_next(const as_msg_t *m);

/* The assistant's content as it goes back in the history: every block unchanged (thinking with its
 * signature, redacted_thinking), except after a mid-output server-side fallback, where thinking,
 * redacted_thinking, tool_use and other model-internal blocks before the last `fallback` block are
 * left out; `fallback` blocks themselves are dropped. A tool_use whose input didn't parse goes back
 * with an empty input (its result carries the raw text). */
aj_t *as_conv_echo(const as_msg_t *m);

typedef struct {
    const char *id, *name;
    const aj_t *input;     /* NULL when it didn't parse */
    const char *raw;       /* the fragments as received */
    const char *err;       /* why it didn't parse */
} as_call_t;
/* The tool calls to run: the tool_use blocks that survive as_conv_echo. Pointers into `m`. */
int as_conv_calls(const as_msg_t *m, as_call_t *out, int max);

/* The tool_result content for input that didn't parse: {"INVALID_JSON": "<raw>"}. Caller frees. */
char *as_conv_invalid_json(const char *raw);
