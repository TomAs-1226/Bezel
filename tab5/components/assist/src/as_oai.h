/* as_oai — the assistant on OpenAI's Chat Completions API, as a translation layer.
 *
 * The conversation stays in the Messages API's shape (as_conv's history, as_msg_t responses), so
 * everything after the wire — the stop-reason rules, tool validation, confirmation cards, the
 * transcript — runs unchanged whichever model answers. This file only translates:
 *
 *   out: the history, system prompt and tools → one Chat Completions request (stream, tool calling);
 *   in:  each streamed chat.completion.chunk → the Messages API events it corresponds to, applied to an
 *        as_msg_t with as_msg_event (text deltas, one tool_use block per tool call with its arguments
 *        as input_json_delta fragments, a stop reason from finish_reason, usage).
 *
 * Pure (no I/O, no threads), like as_conv, so unit tests can drive it with canned streams. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "as_conv.h"
#include "as_json.h"
#include "as_sse.h"

#define AS_OAI_DEFAULT_MODEL "gpt-4o-mini"
#define AS_OAI_DEFAULT_BASE "https://api.openai.com"
#define AS_OAI_MAX_CALLS 32

/* The request body: model, stream with usage, the system prompt as the first message, the history
 * (thinking blocks dropped; tool_use → assistant tool_calls; tool_result → role "tool" messages) and the
 * tools as functions. `q->tools` is the Messages API tools array (as_tools_json); it is re-shaped here. */
void as_oai_request(const as_hist_t *h, const as_req_t *q, ab_t *out);
/* "Authorization: Bearer …" and the content headers. The key never goes anywhere else. */
void as_oai_headers(const char *api_key, char *out, size_t n);

/* ---- the stream ---- */

typedef struct {
    as_msg_t *m;
    bool started;          /* message_start sent */
    bool finished;         /* finish_reason seen: blocks stopped, message_delta sent */
    bool stopped;          /* message_stop sent */
    bool refusal;          /* the model streamed a refusal instead of content */
    int next;              /* the next block index to hand out */
    int text;              /* the text block's index, or -1 */
    int call[AS_OAI_MAX_CALLS]; /* tool_calls[i].index → block index, or -1 */
    int prompt_tokens, completion_tokens, cached_tokens;
} as_oai_t;

void as_oai_init(as_oai_t *o, as_msg_t *m);
/* An as_sse_fn: feed it the response's SSE events (user = the as_oai_t). */
void as_oai_sse(void *oai, const char *event, const char *data, size_t len);
/* The body ended: completes the message if a finish_reason arrived without the closing [DONE]. */
void as_oai_end(as_oai_t *o);

/* The message and code from an OpenAI error body ({"error":{"message","type","code"}}). */
void as_oai_error(const char *body, char *msg, size_t mn, char *code, size_t cn);
