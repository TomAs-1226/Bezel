/* as_json — the assistant's JSON: a growable writer, a strict parser into a small tree, and a validator
 * for the subset of JSON Schema the tools use.
 *
 * json_lite (nt4) tokenizes in place and is lenient, which suits NT4's control frames. The assistant
 * needs more: tool inputs arrive as fragments the API no longer validates (eager input streaming), so
 * they must be parsed strictly and checked against the tool's schema; and the assistant's own content
 * blocks are kept as trees, grown by deltas and written back unchanged. Numbers keep their literal text
 * so a block round-trips byte for byte. Everything here is allocation-backed (PSRAM on the tablet). */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- writer ---- */
typedef struct {
    char *p;
    size_t n, cap;
    bool oom;
} ab_t;

void ab_init(ab_t *b);
void ab_free(ab_t *b);
void ab_reset(ab_t *b);
void ab_raw(ab_t *b, const char *s, size_t n);
void ab_puts(ab_t *b, const char *s);
void ab_fmt(ab_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ab_vfmt(ab_t *b, const char *fmt, va_list ap);
/* A quoted, escaped JSON string. */
void ab_str(ab_t *b, const char *s);
void ab_strn(ab_t *b, const char *s, size_t n);
/* A number, or null when it isn't finite (JSON has no NaN). */
void ab_num(ab_t *b, double v);
/* Takes the buffer (NUL-terminated); the writer is left empty. */
char *ab_take(ab_t *b);

/* ---- tree ---- */
typedef enum { AJ_NULL, AJ_FALSE, AJ_TRUE, AJ_NUM, AJ_STR, AJ_ARR, AJ_OBJ } aj_type_t;

typedef struct aj aj_t;
struct aj {
    aj_type_t type;
    char *key;             /* the member's name when it sits in an object */
    char *s;               /* AJ_STR: the unescaped text; AJ_NUM: the literal as written */
    size_t len, cap;
    double num;
    aj_t **kid;            /* AJ_ARR / AJ_OBJ */
    int n, kcap;
};

/* Strict RFC 8259: one value, nothing but whitespace after it, no trailing commas, no bare control
 * characters in strings, escapes checked, depth limited. NULL on failure, with the reason in err. */
aj_t *aj_parse(const char *s, size_t len, char *err, size_t errn);
void aj_free(aj_t *v);
aj_t *aj_clone(const aj_t *v);

aj_t *aj_new(aj_type_t t);
aj_t *aj_new_str(const char *s);
aj_t *aj_new_strn(const char *s, size_t n);
aj_t *aj_new_num(double v);
aj_t *aj_new_bool(bool v);
/* Appends to an array, or sets `key` in an object (replacing a member of that name). Takes `v`. */
void aj_push(aj_t *arr, aj_t *v);
void aj_set(aj_t *obj, const char *key, aj_t *v);
void aj_insert(aj_t *arr, int at, aj_t *v);
void aj_remove(aj_t *arr, int at);         /* frees the removed value */
/* Grows a string value in place (text deltas). */
void aj_str_append(aj_t *v, const char *s, size_t n);

aj_t *aj_get(const aj_t *obj, const char *key);
aj_t *aj_at(const aj_t *arr, int i);
const char *aj_gets(const aj_t *obj, const char *key);   /* a string member, or NULL */
double aj_getn(const aj_t *obj, const char *key, double fallback);
bool aj_is(const aj_t *v, aj_type_t t);
bool aj_is_bool(const aj_t *v);

void aj_write(ab_t *b, const aj_t *v);
/* The whole value as a new string. */
char *aj_dump(const aj_t *v);

/* ---- schema ----
 * type (string or array of names; "integer" is a number without a fraction), properties, required,
 * additionalProperties: false, enum (strings and numbers), minimum, maximum, minLength, maxLength,
 * minItems, maxItems, items. Enough for tool inputs; the message names the offending path. */
bool aj_validate(const aj_t *schema, const aj_t *v, char *err, size_t errn);

char *as_strdup(const char *s);
/* Percent-encodes a URL query value (RFC 3986 unreserved characters, and '/', pass through). */
void as_urlencode(const char *in, char *out, size_t n);
