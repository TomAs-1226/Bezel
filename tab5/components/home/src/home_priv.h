/* The home services' shared plumbing: the one worker, and small helpers. Private to components/home. */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "hal.h"
#include "home.h"
#include "json_lite.h"

/* Wakes (or starts) the worker for queued work: a command, a one-shot fetch. */
void home_kick(void);

/* Each service's turn on the worker, ~20 times a second. `want` is whether the UI still shows it. */
void home_pc_work(double now, bool want);
void home_ha_work(double now, bool want);
void home_wx_work(double now, bool want);
void home_player_work(double now);

/* A JSON document on the heap: tokens sized to the text. false if it isn't JSON. Free with home_json_free. */
typedef struct {
    jl_doc_t d;
    jl_tok_t *toks;
} home_json_t;
bool home_json_parse(home_json_t *j, const char *js, size_t len);
void home_json_free(home_json_t *j);
/* A string member of `obj` into `out` ("" when absent or null), folded for the fonts. */
void home_json_str(const home_json_t *j, int obj, const char *key, char *out, size_t n);
double home_json_num(const home_json_t *j, int obj, const char *key, double fallback);

/* Percent-encodes `in` for a URL query. */
void home_urlenc(const char *in, char *out, size_t n);
/* Standard base64 into `out`; returns the bytes written, or -1 on bad input or no room. */
int home_b64_decode(const char *in, size_t len, unsigned char *out, size_t max);
