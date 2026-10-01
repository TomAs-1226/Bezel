/* The intel component's shared plumbing: JSON helpers over json_lite, text folding, bounded appends.
 * Pure: the parsers and rules built on it are compiled into the host tests. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "json_lite.h"

typedef struct {
    jl_doc_t d;
    jl_tok_t *toks;
} ix_json_t;

/* Tokens sized to the text (a second, roomier try for number-dense bodies). false if it isn't JSON. */
bool ix_parse(ix_json_t *j, const char *js, size_t len);
void ix_free(ix_json_t *j);
/* A member of `obj`, -1 when absent or null. */
int ix_get(const ix_json_t *j, int obj, const char *key);
/* The first of several names that is present (APIs we couldn't read the documentation of). */
int ix_get_any(const ix_json_t *j, int obj, const char *const *keys);
/* A string member into `out` ("" absent or null), folded for the tablet's faces. */
bool ix_str(const ix_json_t *j, int obj, const char *key, char *out, size_t n);
bool ix_tok_str(const ix_json_t *j, int tok, char *out, size_t n);
double ix_num(const ix_json_t *j, int obj, const char *key, double fallback);
double ix_tok_num(const ix_json_t *j, int tok, double fallback); /* a number, or a string holding one */
/* A time: epoch milliseconds or seconds (either), or an ISO-8601 string; 0 when absent. */
time_t ix_time(const ix_json_t *j, int tok);
/* A team: 254, "254" or "frc254"; 0 otherwise. */
int ix_team(const ix_json_t *j, int tok);

/* UTF-8 to what the faces carry (as home_fold_text): curly quotes straight, dashes, emoji dropped. In place. */
void ix_fold(char *s);
/* Appends printf-style, never past n. */
void ix_addf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* Case-insensitive substring. */
bool ix_has(const char *hay, const char *needle);
/* "14:05" */
void ix_hm(time_t t, char *out, size_t n);
