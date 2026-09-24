#include "mpack_lite.h"
#include <math.h>
#include <string.h>

void mp_reader_init(mp_reader_t *r, const uint8_t *buf, size_t len)
{
    r->p = buf;
    r->end = buf + len;
    r->error = false;
}

static bool need(mp_reader_t *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        r->error = true;
        return false;
    }
    return true;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static bool take_be(mp_reader_t *r, int n, uint64_t *v)
{
    if (!need(r, n)) return false;
    *v = be(r->p, n);
    r->p += n;
    return true;
}

static bool take_bytes(mp_reader_t *r, int lenw, mp_item_t *out, mp_kind_t kind)
{
    uint64_t n;
    if (!take_be(r, lenw, &n) || !need(r, n)) return false;
    out->kind = kind;
    out->bytes.p = r->p;
    out->bytes.n = (uint32_t)n;
    r->p += n;
    return true;
}

bool mp_read(mp_reader_t *r, mp_item_t *out)
{
    out->kind = MP_BAD;
    if (!need(r, 1)) return false;
    uint8_t t = *r->p++;
    uint64_t v;

    if (t <= 0x7f) { out->kind = MP_UINT; out->u = t; return true; }
    if (t >= 0xe0) { out->kind = MP_INT; out->i = (int8_t)t; return true; }
    if ((t & 0xf0) == 0x90) { out->kind = MP_ARRAY; out->count = t & 0x0f; return true; }
    if ((t & 0xf0) == 0x80) { out->kind = MP_MAP; out->count = t & 0x0f; return true; }
    if ((t & 0xe0) == 0xa0) {
        uint32_t n = t & 0x1f;
        if (!need(r, n)) return false;
        out->kind = MP_STR; out->bytes.p = r->p; out->bytes.n = n; r->p += n;
        return true;
    }
    switch (t) {
    case 0xc0: out->kind = MP_NIL; return true;
    case 0xc2: out->kind = MP_BOOL; out->b = false; return true;
    case 0xc3: out->kind = MP_BOOL; out->b = true; return true;
    case 0xc4: return take_bytes(r, 1, out, MP_BIN);
    case 0xc5: return take_bytes(r, 2, out, MP_BIN);
    case 0xc6: return take_bytes(r, 4, out, MP_BIN);
    case 0xca: {
        if (!take_be(r, 4, &v)) return false;
        uint32_t bits = (uint32_t)v; float f; memcpy(&f, &bits, 4);
        out->kind = MP_F32; out->f32 = f; return true;
    }
    case 0xcb: {
        if (!take_be(r, 8, &v)) return false;
        double d; memcpy(&d, &v, 8);
        out->kind = MP_F64; out->f64 = d; return true;
    }
    case 0xcc: if (!take_be(r, 1, &v)) return false; out->kind = MP_UINT; out->u = v; return true;
    case 0xcd: if (!take_be(r, 2, &v)) return false; out->kind = MP_UINT; out->u = v; return true;
    case 0xce: if (!take_be(r, 4, &v)) return false; out->kind = MP_UINT; out->u = v; return true;
    case 0xcf: if (!take_be(r, 8, &v)) return false; out->kind = MP_UINT; out->u = v; return true;
    case 0xd0: if (!take_be(r, 1, &v)) return false; out->kind = MP_INT; out->i = (int8_t)v; return true;
    case 0xd1: if (!take_be(r, 2, &v)) return false; out->kind = MP_INT; out->i = (int16_t)v; return true;
    case 0xd2: if (!take_be(r, 4, &v)) return false; out->kind = MP_INT; out->i = (int32_t)v; return true;
    case 0xd3: if (!take_be(r, 8, &v)) return false; out->kind = MP_INT; out->i = (int64_t)v; return true;
    case 0xd9: return take_bytes(r, 1, out, MP_STR);
    case 0xda: return take_bytes(r, 2, out, MP_STR);
    case 0xdb: return take_bytes(r, 4, out, MP_STR);
    case 0xdc: if (!take_be(r, 2, &v)) return false; out->kind = MP_ARRAY; out->count = (uint32_t)v; return true;
    case 0xdd: if (!take_be(r, 4, &v)) return false; out->kind = MP_ARRAY; out->count = (uint32_t)v; return true;
    case 0xde: if (!take_be(r, 2, &v)) return false; out->kind = MP_MAP; out->count = (uint32_t)v; return true;
    case 0xdf: if (!take_be(r, 4, &v)) return false; out->kind = MP_MAP; out->count = (uint32_t)v; return true;
    default:
        r->error = true; /* ext types: NT4 never sends them */
        return false;
    }
}

bool mp_skip(mp_reader_t *r)
{
    mp_item_t it;
    if (!mp_read(r, &it)) return false;
    if (it.kind == MP_ARRAY) {
        for (uint32_t i = 0; i < it.count; i++) if (!mp_skip(r)) return false;
    } else if (it.kind == MP_MAP) {
        for (uint32_t i = 0; i < it.count * 2; i++) if (!mp_skip(r)) return false;
    }
    return true;
}

double mp_as_double(const mp_item_t *it)
{
    switch (it->kind) {
    case MP_INT: return (double)it->i;
    case MP_UINT: return (double)it->u;
    case MP_F32: return it->f32;
    case MP_F64: return it->f64;
    case MP_BOOL: return it->b ? 1.0 : 0.0;
    default: return NAN;
    }
}

int64_t mp_as_int(const mp_item_t *it)
{
    switch (it->kind) {
    case MP_INT: return it->i;
    case MP_UINT: return (int64_t)it->u;
    case MP_F32: return (int64_t)it->f32;
    case MP_F64: return (int64_t)it->f64;
    case MP_BOOL: return it->b;
    default: return 0;
    }
}

void mp_writer_init(mp_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void put(mp_writer_t *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put_be(mp_writer_t *w, uint8_t tag, uint64_t v, int n)
{
    uint8_t b[9];
    b[0] = tag;
    for (int i = 0; i < n; i++) b[1 + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    put(w, b, 1 + n);
}

void mp_write_array(mp_writer_t *w, uint32_t n)
{
    if (n < 16) { uint8_t t = 0x90 | n; put(w, &t, 1); }
    else if (n <= 0xffff) put_be(w, 0xdc, n, 2);
    else put_be(w, 0xdd, n, 4);
}

void mp_write_uint(mp_writer_t *w, uint64_t v)
{
    if (v < 128) { uint8_t t = (uint8_t)v; put(w, &t, 1); }
    else if (v <= 0xff) put_be(w, 0xcc, v, 1);
    else if (v <= 0xffff) put_be(w, 0xcd, v, 2);
    else if (v <= 0xffffffffu) put_be(w, 0xce, v, 4);
    else put_be(w, 0xcf, v, 8);
}

void mp_write_int(mp_writer_t *w, int64_t v)
{
    if (v >= 0) { mp_write_uint(w, (uint64_t)v); return; }
    if (v >= -32) { uint8_t t = (uint8_t)(int8_t)v; put(w, &t, 1); }
    else if (v >= INT8_MIN) put_be(w, 0xd0, (uint8_t)(int8_t)v, 1);
    else if (v >= INT16_MIN) put_be(w, 0xd1, (uint16_t)(int16_t)v, 2);
    else if (v >= INT32_MIN) put_be(w, 0xd2, (uint32_t)(int32_t)v, 4);
    else put_be(w, 0xd3, (uint64_t)v, 8);
}

void mp_write_bool(mp_writer_t *w, bool v)
{
    uint8_t t = v ? 0xc3 : 0xc2;
    put(w, &t, 1);
}

void mp_write_f64(mp_writer_t *w, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    put_be(w, 0xcb, bits, 8);
}

void mp_write_f32(mp_writer_t *w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put_be(w, 0xca, bits, 4);
}

void mp_write_str(mp_writer_t *w, const char *s, size_t n)
{
    if (n < 32) { uint8_t t = 0xa0 | (uint8_t)n; put(w, &t, 1); }
    else if (n <= 0xff) put_be(w, 0xd9, n, 1);
    else if (n <= 0xffff) put_be(w, 0xda, n, 2);
    else put_be(w, 0xdb, n, 4);
    put(w, s, n);
}

void mp_write_bin(mp_writer_t *w, const uint8_t *p, size_t n)
{
    if (n <= 0xff) put_be(w, 0xc4, n, 1);
    else if (n <= 0xffff) put_be(w, 0xc5, n, 2);
    else put_be(w, 0xc6, n, 4);
    put(w, p, n);
}
