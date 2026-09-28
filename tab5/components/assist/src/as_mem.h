/* as_mem — long-term memory for the two AIs on the tablet, one pool each.
 *
 * The technician (Assist) and the companion each keep their own pool; nothing here lets one read the
 * other's. A pool is:
 *   - a few "self" sections the model rewrites whole: the technician's season notebook (the robot, the
 *     team, the season so far, what it is watching), the companion's persona (her name, personality,
 *     how she and the owner get on, running jokes). They are always put in front of the model;
 *   - up to AS_MEM_MAX memories, one durable fact each, with a kind, an importance (1-3), and when it
 *     was made and last changed. The most important and most relevant go in front of the model at the
 *     start of a conversation, inside a character budget; the rest are found with the recall tool.
 *
 * This file is the pure part (no I/O, no locks, no clock): the model, its JSON on the card, the
 * near-duplicate rule, eviction when full, search, the digest, and the refusal of anything that looks
 * like a secret. as_memio.c keeps the two pools, loads and saves them (microSD, kv without a card) and
 * is what the tools and the UI call. Both are compiled into the host tests except as_memio.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AS_MEM_MAX 120          /* memories per pool: past this the least valuable one makes room */
#define AS_MEM_TEXT_CHARS 280   /* what a memory may say (the tools' schema says the same) */
#define AS_MEM_TEXT 480         /* bytes kept per memory: 280 characters of UTF-8, most of the time */
#define AS_MEM_SELF_N 4         /* self sections per pool */
#define AS_MEM_SELF_CHARS 600   /* a notebook section; a persona field is held to 400 by its schema */
#define AS_MEM_SELF_TEXT 1024
#define AS_MEM_KV_MAX 3800      /* the no-card copy in kv (NVS strings stop at 4000) */

enum { AS_MEM_ASSIST, AS_MEM_COMPANION, AS_MEM_POOLS };

typedef struct {
    int id;                     /* > 0, never reused within a pool */
    char kind[16];              /* one of as_mem_kinds() */
    char text[AS_MEM_TEXT];
    int imp;                    /* 1 minor, 2 useful, 3 important */
    int64_t created, updated;   /* unix seconds; 0 unknown (the clock wasn't set) */
    int uses;                   /* times recall returned it */
} as_mem_item_t;

typedef struct {
    int pool;
    int next_id;
    int n;
    int64_t updated;
    as_mem_item_t it[AS_MEM_MAX];
    char self[AS_MEM_SELF_N][AS_MEM_SELF_TEXT];
    int64_t self_at[AS_MEM_SELF_N];
} as_mem_t;

/* The kinds a memory may have, as the tools' schema lists them (NULL-terminated). */
const char *const *as_mem_kinds(void);
/* A pool's self sections: "robot", "team", "season", "watchlist" for the technician; "name",
 * "personality", "relationship", "running_jokes" for the companion. NULL past the last. */
const char *as_mem_self_key(int pool, int i);
int as_mem_self_index(int pool, const char *key);
const char *as_mem_pool_name(int pool);  /* "technician" / "companion" */

void as_mem_init(as_mem_t *m, int pool);
/* Reads a pool's JSON. Tolerant of fields it doesn't know and of memories it can't use (dropped);
 * false only when it is not a pool at all. The pool number stays the one init gave. */
bool as_mem_from_json(as_mem_t *m, const char *js, size_t len);
/* The pool as JSON (malloc'd). With max_bytes > 0, the least valuable memories are left out until it
 * fits (the kv copy without a card); the self sections always go. */
char *as_mem_to_json(const as_mem_t *m, size_t max_bytes, size_t *len);

/* A password, a key, a token: true when text looks like one. Memory refuses to keep those. */
bool as_mem_secret(const char *text);

/* How much a memory is worth keeping and showing: importance first, then how recently it changed,
 * then how often it was recalled. */
double as_mem_score(const as_mem_item_t *it, int64_t now);

typedef struct {
    int id;                     /* the memory written; 0 when refused */
    bool merged;                /* it said nearly what #id already said: #id was updated instead */
    int evicted;                /* the pool was full: this memory made room (0 none) */
    char evicted_text[AS_MEM_TEXT];
    char why[160];              /* why it was refused */
} as_mem_res_t;

/* A new memory. imp is clamped to 1..3, an unknown kind becomes "fact". A near-duplicate of an
 * existing memory updates that one; a full pool evicts its least valuable memory of no greater
 * importance, or refuses when every one matters more. The outcome goes to *r (by pointer: it is big,
 * and the companion's thread has a small stack); true when something was written. */
bool as_mem_add(as_mem_t *m, const char *kind, const char *text, int imp, int64_t now, as_mem_res_t *r);
/* Rewrites memory `id`: any of kind/text may be NULL, imp 0 leaves it. */
bool as_mem_update(as_mem_t *m, int id, const char *kind, const char *text, int imp, int64_t now, as_mem_res_t *r);
bool as_mem_forget(as_mem_t *m, int id);
void as_mem_clear(as_mem_t *m);            /* everything, the self sections too */
int as_mem_find(const as_mem_t *m, int id); /* its index, or -1 */
/* A self section rewritten whole ("" clears it). false with why for an unknown section or a secret. */
bool as_mem_set_self(as_mem_t *m, const char *key, const char *text, int64_t now, char *why, size_t wn);

/* The memories that best answer `query` (words; "" or NULL: the most valuable), of `kind` if given,
 * as indices into m->it, best first. Returns how many (at most max). */
int as_mem_search(const as_mem_t *m, const char *query, const char *kind, int *idx, int max, int64_t now);

/* What the model is shown at the start of a conversation: the self sections, then memories, the most
 * valuable (and most relevant to `query`, the first question, when given) first, as many whole lines
 * as fit in `budget` characters, then how many more there are. Returns the length written. */
size_t as_mem_digest(const as_mem_t *m, const char *query, int64_t now, size_t budget, char *out, size_t n);

/* "2026-09-20" for a unix time, "" for 0. */
void as_mem_date(int64_t t, char *out, size_t n);
