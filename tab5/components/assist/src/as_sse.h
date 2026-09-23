/* as_sse — Server-Sent Events, and a Messages API response assembled from them.
 *
 * The lexer takes bytes as the network hands them over (an event may be split anywhere, even inside a
 * CRLF) and dispatches whole events. The assembler applies the Messages API's streaming events to one
 * message: content blocks as trees, text / thinking / signature deltas appended in place, and each
 * tool_use block's input_json_delta fragments accumulated and parsed strictly when the block stops. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "as_json.h"

typedef void (*as_sse_fn)(void *user, const char *event, const char *data, size_t len);

typedef struct {
    ab_t line, data;
    char event[64];
    bool cr;               /* the last byte was CR: an LF right after it ends nothing new */
    bool have_data;
} as_sse_t;

void as_sse_init(as_sse_t *s);
void as_sse_free(as_sse_t *s);
void as_sse_feed(as_sse_t *s, const char *buf, size_t n, as_sse_fn fn, void *user);

/* ---- the message ---- */

typedef enum { AS_D_TEXT, AS_D_THINKING, AS_D_INPUT } as_delta_t;

typedef struct {
    aj_t *block;           /* the content block as it will be echoed */
    ab_t partial;          /* tool_use: input_json_delta fragments */
    bool open, bad_input;  /* bad_input: the fragments didn't parse; `partial` keeps them */
    char input_err[96];
} as_block_t;

typedef struct {
    void *user;
    void (*start)(void *user, int index, const aj_t *block);
    void (*delta)(void *user, int index, as_delta_t kind, const char *s, size_t n);
    void (*stop)(void *user, int index, const as_block_t *b);
} as_msg_cb_t;

typedef struct {
    as_block_t *b;
    int n, cap;
    char id[64], model[64];
    char stop_reason[32];
    aj_t *stop_details;
    int input_tokens, output_tokens, cache_read, cache_write;
    bool started, done;    /* message_start / message_stop seen */
    bool fallback;         /* a fallback block, or a fallback_message in usage.iterations */
    bool error;            /* an `error` event, or JSON that wouldn't parse */
    char error_type[48], error_msg[256];
    as_msg_cb_t cb;
} as_msg_t;

void as_msg_init(as_msg_t *m, const as_msg_cb_t *cb);
void as_msg_free(as_msg_t *m);
void as_msg_event(as_msg_t *m, const char *event, const char *data, size_t len);
/* An as_sse_fn that feeds an as_msg_t. */
void as_msg_sse(void *msg, const char *event, const char *data, size_t len);
