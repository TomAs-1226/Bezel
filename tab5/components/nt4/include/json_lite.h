/* json_lite — a flat, allocation-free JSON tokenizer (jsmn-shaped) with lookup helpers.
 *
 * NT4 control frames (announce / unannounce / properties) and Catalyst's JSON-string topics (the spec
 * sheet, the device roster) are small and read once, so a token array on the stack is enough. Each
 * token records where its subtree ends, which makes key lookup and array walking O(n) without a tree. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { JL_OBJ, JL_ARR, JL_STR, JL_NUM, JL_TRUE, JL_FALSE, JL_NULL } jl_type_t;

typedef struct {
    jl_type_t type;
    int start, end;  /* byte span in the source; strings exclude their quotes */
    int size;        /* children: members for objects (key+value pairs count once), items for arrays */
    int skip;        /* index of the first token after this one's subtree */
} jl_tok_t;

typedef struct {
    const char *js;
    const jl_tok_t *t;
    int n;
} jl_doc_t;

/* Tokenizes `js`; returns the number of tokens, or -1 if malformed or more than `max` are needed. */
int jl_parse(const char *js, size_t len, jl_tok_t *toks, int max);

/* The value token for `key` in object token `obj`, or -1. */
int jl_get(const jl_doc_t *d, int obj, const char *key);
/* The i-th element token of array token `arr`, or -1. */
int jl_at(const jl_doc_t *d, int arr, int i);
/* Walks a dotted path of keys ("params.name"), from `obj`. */
int jl_path(const jl_doc_t *d, int obj, const char *path);

bool jl_eq(const jl_doc_t *d, int tok, const char *s);
/* Copies a string token (unescaped) or the raw text of a scalar; returns false if tok < 0. */
bool jl_str(const jl_doc_t *d, int tok, char *out, size_t n);
double jl_num(const jl_doc_t *d, int tok, double fallback);
bool jl_bool(const jl_doc_t *d, int tok, bool fallback);
