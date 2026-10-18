/* mpack_lite — the slice of MessagePack NetworkTables 4 uses, as a cursor reader and a buffer writer.
 *
 * NT4 binary frames are one or more concatenated msgpack arrays `[id, timestamp, type, value]`. The
 * value can be any msgpack scalar, a bin (raw / struct bytes), a str, or an array of scalars. That is
 * all this reads and writes: no maps, no extension types (NT4 never sends them), no allocation. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MP_NIL, MP_BOOL, MP_INT, MP_UINT, MP_F32, MP_F64, MP_STR, MP_BIN, MP_ARRAY, MP_MAP, MP_BAD,
} mp_kind_t;

typedef struct {
    mp_kind_t kind;
    union {
        bool b;
        int64_t i;
        uint64_t u;
        float f32;
        double f64;
        struct { const uint8_t *p; uint32_t n; } bytes; /* MP_STR / MP_BIN: points into the frame */
        uint32_t count;                                  /* MP_ARRAY / MP_MAP: element count */
    };
} mp_item_t;

typedef struct {
    const uint8_t *p, *end;
    bool error;
} mp_reader_t;

void mp_reader_init(mp_reader_t *r, const uint8_t *buf, size_t len);
/* Reads the next item's header. Arrays and maps only report their count: the caller reads the
 * elements that follow. Returns false (and sets r->error) on truncation or an unsupported type. */
bool mp_read(mp_reader_t *r, mp_item_t *out);
/* Skips one complete item, recursing into arrays and maps. */
bool mp_skip(mp_reader_t *r);
static inline bool mp_done(const mp_reader_t *r) { return r->p >= r->end || r->error; }

/* Numeric view of a scalar item (ints, uints, floats, bools); NAN for anything else. */
double mp_as_double(const mp_item_t *it);
int64_t mp_as_int(const mp_item_t *it);

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool overflow;
} mp_writer_t;

void mp_writer_init(mp_writer_t *w, uint8_t *buf, size_t cap);
void mp_write_array(mp_writer_t *w, uint32_t n);
void mp_write_int(mp_writer_t *w, int64_t v);
void mp_write_uint(mp_writer_t *w, uint64_t v);
void mp_write_bool(mp_writer_t *w, bool v);
void mp_write_f64(mp_writer_t *w, double v);
void mp_write_f32(mp_writer_t *w, float v);
void mp_write_str(mp_writer_t *w, const char *s, size_t n);
void mp_write_bin(mp_writer_t *w, const uint8_t *p, size_t n);
