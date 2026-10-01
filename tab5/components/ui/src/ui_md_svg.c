/* ui_md_svg — the ```svg``` block's own renderer.
 *
 * LVGL's vector pipeline (ThorVG) is compiled out on this board, so there is no lv_draw_vector or
 * lv_svg_* to lean on: this file is a small hand-rolled SVG tokenizer paired with a software
 * rasteriser built from the same primitives every other Bezel screen draws with (lv_draw_rect,
 * lv_draw_line, lv_draw_arc, lv_draw_label) into an lv_canvas.
 *
 * The input is text a language model wrote, not a trusted document: nothing here allocates
 * without a fixed cap, nothing recurses without a depth limit, and every parse loop is written so
 * it always makes forward progress even when the tag soup is nonsense. A shape that cannot be
 * honoured (an elliptical arc, a gradient, anything past the subset the header documents) is
 * tracked in a bitmask and named in `why` — half-drawing something and staying quiet about the
 * rest would be worse than the plain warning.
 *
 * Fill rule: a general polygon (rect under rotation, ellipse, polyline/polygon, a flattened path
 * subpath) is filled with a per-scanline even-odd test, not nonzero — even-odd is the simpler of
 * the two winding rules and every shape this renderer is likely to see (icons, simple glyphs) reads
 * the same under either rule. Each scanline span is painted with a 1px-tall lv_draw_rect rather than
 * lv_draw_line, so there is no line-cap/width bookkeeping to get wrong for a fill. */
#include "ui_internal.h"
#include "ui_md.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- limits */

#define MAX_DEPTH 16          /* <g> nesting the transform/style stack tracks before bailing out */
#define MAX_PRIMS 4000         /* shapes actually drawn, across the whole document */
#define MAX_POLY_PTS 256       /* points kept for one polygon/polyline/path subpath */
/* The hard cap on one canvas's pixel buffer. It was 1.5 MB, which is most of a transcript: the UI has
 * about 6.5 MB of PSRAM to spend once the renderer and the panel have taken theirs, and a chat holds up
 * to 60 rows, so one drawing being allowed a quarter of everything is not a cap at all -- eight answers
 * with a chart and a drawing each took 2.3 MB and left the tablet with 1.1 MB free, which is where the
 * glass cache starts failing and the stalls begin. 384 KB is 800 x 245 at 16 bpp, which is larger than
 * any drawing the 800 px-wide transcript actually shows; anything bigger is scaled down to fit it. */
#define MAX_CANVAS_BYTES (384u * 1024u)
/* And a floor under the whole thing, because the cap alone does not help when memory has already gone:
 * below this much free PSRAM a drawing is not allocated at all and the reader is told why, which is far
 * better than taking the last of the heap for a picture and stalling everything that comes after. */
#define CANVAS_PSRAM_FLOOR (1280u * 1024u)

/* What the subset can't honour, tracked as it's met so `why` never has to guess afterwards. */
enum {
    SKIP_ARC          = 1u << 0,  /* A/a in path data */
    SKIP_GRADIENT     = 1u << 1,  /* linearGradient/radialGradient/pattern/stop */
    SKIP_CLIP         = 1u << 2,  /* clipPath/mask */
    SKIP_FILTER       = 1u << 3,  /* filter and fe* primitives */
    SKIP_DASH         = 1u << 4,  /* stroke-dasharray other than none */
    SKIP_JOIN         = 1u << 5,  /* stroke-linejoin other than round */
    SKIP_STYLE        = 1u << 6,  /* a <style> block, or a class= relying on one */
    SKIP_USE          = 1u << 7,  /* use/defs/symbol/marker */
    SKIP_IMAGE        = 1u << 8,  /* <image> */
    SKIP_TEXTPATH     = 1u << 9,  /* text following a path */
    SKIP_ROTATED_TEXT = 1u << 10, /* text under a rotation, drawn upright instead */
    SKIP_DEEP         = 1u << 11, /* group nesting past MAX_DEPTH, contents dropped */
};

static const struct { unsigned bit; const char *text; } SKIP_MSGS[] = {
    { SKIP_ARC, "elliptical arcs in path data" },
    { SKIP_GRADIENT, "a gradient or pattern fill" },
    { SKIP_CLIP, "a clip path or mask" },
    { SKIP_FILTER, "a filter" },
    { SKIP_DASH, "a dash pattern" },
    { SKIP_JOIN, "a stroke join other than round" },
    { SKIP_STYLE, "a <style> block or a css class" },
    { SKIP_USE, "<use>, <defs>, <symbol> or <marker>" },
    { SKIP_IMAGE, "an image" },
    { SKIP_TEXTPATH, "text following a path" },
    { SKIP_ROTATED_TEXT, "rotated text, drawn upright instead" },
    { SKIP_DEEP, "groups nested more than 16 deep" },
};

/* ---------------------------------------------------------------- small helpers */

/* ASCII case-insensitive compare: colour and tag names are ours to be lenient about, unlike
 * strcasecmp this needs no header beyond ctype.h that isn't already pulled in. */
static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static lv_opa_t opa_of(float f)
{
    if (f <= 0) return 0;
    if (f >= 1) return 255;
    return (lv_opa_t)lroundf(f * 255.0f);
}

/* Coordinates come from a model and can be nonsense (huge, or "nan"/"inf" parsed literally by
 * strtof); clamp before handing anything to lroundf, which is undefined past its range. */
static int32_t clampi(float v)
{
    if (!isfinite(v)) return 0;
    if (v < -100000.0f) v = -100000.0f;
    if (v > 100000.0f) v = 100000.0f;
    return (int32_t)lroundf(v);
}

/* ---------------------------------------------------------------- colour */

static int hexval(char c)
{
    c = (char)tolower((unsigned char)c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

/* The 16 basic CSS colour keywords, plus the "grey" spelling and "orange" (common in icon SVG,
 * missing from the basic 16). */
static const struct { const char *name; uint32_t rgb; } COLOR_NAMES[] = {
    { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "white", 0xFFFFFF }, { "maroon", 0x800000 }, { "red", 0xFF0000 }, { "purple", 0x800080 },
    { "fuchsia", 0xFF00FF }, { "green", 0x008000 }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
    { "yellow", 0xFFFF00 }, { "navy", 0x000080 }, { "blue", 0x0000FF }, { "teal", 0x008080 },
    { "aqua", 0x00FFFF }, { "orange", 0xFFA500 },
};

static bool color_parse(const char *s, lv_color_t *out)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#') {
        s++;
        char hex[7];
        size_t n = 0;
        while (n < 6 && isxdigit((unsigned char)s[n])) { hex[n] = s[n]; n++; }
        hex[n] = 0;
        uint32_t rgb;
        if (n == 3) {
            uint32_t r = (uint32_t)hexval(hex[0]), g = (uint32_t)hexval(hex[1]), b = (uint32_t)hexval(hex[2]);
            rgb = ((r << 4 | r) << 16) | ((g << 4 | g) << 8) | (b << 4 | b);
        } else if (n == 6) {
            rgb = (uint32_t)strtoul(hex, NULL, 16);
        } else {
            return false;
        }
        *out = bz_lv_rgb(rgb);
        return true;
    }
    if (ieq(s, "currentColor")) { *out = bz_lv(BZ_C_INK); return true; }
    for (size_t i = 0; i < sizeof COLOR_NAMES / sizeof COLOR_NAMES[0]; i++) {
        if (ieq(s, COLOR_NAMES[i].name)) { *out = bz_lv_rgb(COLOR_NAMES[i].rgb); return true; }
    }
    return false;
}

/* ---------------------------------------------------------------- numbers */

static int parse_floats(const char *s, float *out, int max)
{
    int n = 0;
    const char *p = s;
    while (p && *p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        char *end;
        float v = strtof(p, &end);
        if (end == p) break;
        if (isfinite(v)) out[n++] = v;
        p = end;
    }
    return n;
}

/* ---------------------------------------------------------------- affine transforms */

typedef struct { float a, b, c, d, e, f; } mat_t;

static mat_t mat_mul(mat_t a, mat_t b)
{
    mat_t r;
    r.a = a.a * b.a + a.c * b.b;
    r.b = a.b * b.a + a.d * b.b;
    r.c = a.a * b.c + a.c * b.d;
    r.d = a.b * b.c + a.d * b.d;
    r.e = a.a * b.e + a.c * b.f + a.e;
    r.f = a.b * b.e + a.d * b.f + a.f;
    return r;
}

static void mat_apply(mat_t m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

static float mat_scale(mat_t m) { return sqrtf(fabsf(m.a * m.d - m.b * m.c)); }
static bool mat_axis_aligned(mat_t m) { return fabsf(m.b) < 1e-3f && fabsf(m.c) < 1e-3f; }

static mat_t parse_transform(const char *s)
{
    mat_t r = { 1, 0, 0, 1, 0, 0 };
    if (!s) return r;
    const char *p = s;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char fn[16];
        size_t ni = 0;
        while (*p && *p != '(' && !isspace((unsigned char)*p) && ni < sizeof fn - 1) fn[ni++] = *p++;
        fn[ni] = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '(') break;
        p++;
        float args[6];
        int n = 0;
        while (*p && *p != ')' && n < 6) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
            char *end;
            float v = strtof(p, &end);
            if (end == p || !isfinite(v)) break;
            args[n++] = v;
            p = end;
        }
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
        mat_t fnm = { 1, 0, 0, 1, 0, 0 };
        if (ieq(fn, "translate")) {
            fnm.e = n > 0 ? args[0] : 0;
            fnm.f = n > 1 ? args[1] : 0;
        } else if (ieq(fn, "scale")) {
            fnm.a = n > 0 ? args[0] : 1;
            fnm.d = n > 1 ? args[1] : fnm.a;
        } else if (ieq(fn, "rotate")) {
            float deg = n > 0 ? args[0] : 0;
            float rad = deg * 0.0174532925f;
            float cs = cosf(rad), sn = sinf(rad);
            if (n >= 3) {
                mat_t t1 = { 1, 0, 0, 1, args[1], args[2] };
                mat_t rr = { cs, sn, -sn, cs, 0, 0 };
                mat_t t2 = { 1, 0, 0, 1, -args[1], -args[2] };
                fnm = mat_mul(mat_mul(t1, rr), t2);
            } else {
                fnm.a = cs; fnm.b = sn; fnm.c = -sn; fnm.d = cs;
            }
        } else if (ieq(fn, "matrix") && n == 6) {
            fnm.a = args[0]; fnm.b = args[1]; fnm.c = args[2];
            fnm.d = args[3]; fnm.e = args[4]; fnm.f = args[5];
        }
        /* an unrecognised function (skewX/Y, or garbage) leaves fnm as identity: quietly ignored,
         * since skew isn't in the documented subset either way */
        r = mat_mul(r, fnm);
    }
    return r;
}

/* ---------------------------------------------------------------- style */

typedef struct {
    lv_color_t fill;
    bool fill_none;
    lv_color_t stroke;
    bool stroke_none;
    float stroke_width;
    float opacity, fill_opacity, stroke_opacity;
    float font_size;
    int text_anchor; /* 0 start, 1 middle, 2 end */
} style_t;

static style_t default_style(void)
{
    style_t st;
    st.fill = bz_lv(BZ_C_INK); /* the spec's default is black; on this dark UI, ink reads instead */
    st.fill_none = false;
    st.stroke = bz_lv(BZ_C_INK);
    st.stroke_none = true; /* SVG's own default: no stroke unless one is named */
    st.stroke_width = 1.0f;
    st.opacity = st.fill_opacity = st.stroke_opacity = 1.0f;
    st.font_size = 16.0f;
    st.text_anchor = 0;
    return st;
}

static float parse_opacity(const char *s, float cur)
{
    char *end;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return cur;
    while (*end == ' ') end++;
    if (*end == '%') v /= 100.0f;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return v;
}

/* ---------------------------------------------------------------- tags */

typedef struct {
    char name[24];
    bool self_close;
    const char *abegin; /* the attribute text, [abegin, abegin+alen) */
    size_t alen;
} tag_t;

/* Parses one opening (or self-closing) tag starting at `lt` (which must point at '<'). Returns the
 * position right after its '>', or NULL if the tag never closes (the input ran out first). */
static const char *parse_open_tag(const char *lt, tag_t *t)
{
    const char *p = lt + 1;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t ni = 0;
    while (*p && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) {
        if (ni < sizeof t->name - 1) t->name[ni++] = *p;
        p++;
    }
    t->name[ni] = 0;
    const char *ab = p;
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p) p++;
            continue;
        }
        if (*p == '>') break;
        p++;
    }
    if (*p != '>') return NULL;
    const char *ae = p;
    const char *trim_end = ae;
    while (trim_end > ab && isspace((unsigned char)trim_end[-1])) trim_end--;
    t->self_close = (trim_end > ab && trim_end[-1] == '/');
    if (t->self_close) trim_end--;
    t->abegin = ab;
    t->alen = (size_t)(trim_end - ab);
    return p + 1;
}

/* Scans a tag's attribute text for `key`; tolerates a missing quote (reads to the next space) and
 * stray whitespace around '='. Never reads past [t->abegin, t->abegin+t->alen). */
static bool tag_attr_raw(const tag_t *t, const char *key, const char **vs_out, size_t *vlen_out)
{
    const char *p = t->abegin, *end = t->abegin + t->alen;
    size_t klen = strlen(key);
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *ns = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p)) p++;
        size_t nlen = (size_t)(p - ns);
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *vs;
        size_t vlen;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < end && *p != q) p++;
                vlen = (size_t)(p - vs);
                if (p < end) p++;
            } else {
                vs = p;
                while (p < end && !isspace((unsigned char)*p)) p++;
                vlen = (size_t)(p - vs);
            }
        } else {
            vs = ns;
            vlen = 0;
        }
        if (nlen == klen && strncmp(ns, key, klen) == 0) {
            *vs_out = vs;
            *vlen_out = vlen;
            return true;
        }
    }
    return false;
}

static bool tag_attr(const tag_t *t, const char *key, char *out, size_t outsz)
{
    const char *vs;
    size_t vlen;
    if (!tag_attr_raw(t, key, &vs, &vlen)) return false;
    if (vlen >= outsz) vlen = outsz - 1;
    memcpy(out, vs, vlen);
    out[vlen] = 0;
    return true;
}

/* A numeric attribute, defaulting to `def` when absent, unparsable or non-finite ("nan"/"inf" are
 * valid strtof input and not valid geometry). */
static float numattr(const tag_t *t, const char *key, float def)
{
    char v[64];
    if (!tag_attr(t, key, v, sizeof v)) return def;
    char *end;
    float f = strtof(v, &end);
    if (end == v || !isfinite(f)) return def;
    return f;
}

static void style_apply(style_t *st, const tag_t *t, unsigned *skip)
{
    char v[160];
    if (tag_attr(t, "fill", v, sizeof v)) {
        trim(v);
        if (ieq(v, "none")) st->fill_none = true;
        else { lv_color_t c; if (color_parse(v, &c)) { st->fill_none = false; st->fill = c; } }
    }
    if (tag_attr(t, "stroke", v, sizeof v)) {
        trim(v);
        if (ieq(v, "none")) st->stroke_none = true;
        else { lv_color_t c; if (color_parse(v, &c)) { st->stroke_none = false; st->stroke = c; } }
    }
    float sw = numattr(t, "stroke-width", -1.0f);
    if (sw > 0) st->stroke_width = sw;
    if (tag_attr(t, "opacity", v, sizeof v)) st->opacity = parse_opacity(v, st->opacity);
    if (tag_attr(t, "fill-opacity", v, sizeof v)) st->fill_opacity = parse_opacity(v, st->fill_opacity);
    if (tag_attr(t, "stroke-opacity", v, sizeof v)) st->stroke_opacity = parse_opacity(v, st->stroke_opacity);
    float fs = numattr(t, "font-size", -1.0f);
    if (fs > 0) st->font_size = fs;
    if (tag_attr(t, "text-anchor", v, sizeof v)) {
        trim(v);
        if (ieq(v, "middle")) st->text_anchor = 1;
        else if (ieq(v, "end")) st->text_anchor = 2;
        else st->text_anchor = 0;
    }
    if (tag_attr(t, "stroke-dasharray", v, sizeof v)) { trim(v); if (v[0] && !ieq(v, "none")) *skip |= SKIP_DASH; }
    if (tag_attr(t, "stroke-linejoin", v, sizeof v)) { trim(v); if (v[0] && !ieq(v, "round")) *skip |= SKIP_JOIN; }
    if (tag_attr(t, "class", v, sizeof v)) { if (v[0]) *skip |= SKIP_STYLE; }
}

static bool is_leaf_shape(const char *name)
{
    static const char *const L[] = { "line", "rect", "circle", "ellipse", "polyline", "polygon", "path", "text" };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) if (ieq(name, L[i])) return true;
    return false;
}

static unsigned classify_skip_tag(const char *name)
{
    if (ieq(name, "defs") || ieq(name, "symbol") || ieq(name, "use") || ieq(name, "marker")) return SKIP_USE;
    if (ieq(name, "clipPath") || ieq(name, "mask")) return SKIP_CLIP;
    if (ieq(name, "filter") || (tolower((unsigned char)name[0]) == 'f' && tolower((unsigned char)name[1]) == 'e'))
        return SKIP_FILTER;
    if (ieq(name, "linearGradient") || ieq(name, "radialGradient") || ieq(name, "pattern") || ieq(name, "stop"))
        return SKIP_GRADIENT;
    if (ieq(name, "style")) return SKIP_STYLE;
    if (ieq(name, "image")) return SKIP_IMAGE;
    if (ieq(name, "textPath")) return SKIP_TEXTPATH;
    return 0;
}

/* ---------------------------------------------------------------- the canvas + budget */

typedef struct {
    lv_layer_t *layer;
    int cw, ch;
    int prims;
    bool cut_short;
    bool any_drawn;
} rctx_t;

static bool prim_budget(rctx_t *r)
{
    if (r->prims >= MAX_PRIMS) { r->cut_short = true; return false; }
    r->prims++;
    r->any_drawn = true;
    return true;
}

/* Even-odd scanline fill: one 1px-tall lv_draw_rect per span, allocation-free (a fixed intersection
 * array bounded by the same MAX_POLY_PTS every subpath is already capped to). */
static void fill_polygon(rctx_t *r, const lv_point_precise_t *p, int n, lv_color_t color, lv_opa_t opa)
{
    if (n < 3 || opa == 0) return;
    float ymin = 1e30f, ymax = -1e30f;
    for (int i = 0; i < n; i++) {
        if (p[i].y < ymin) ymin = p[i].y;
        if (p[i].y > ymax) ymax = p[i].y;
    }
    int y0 = (int)floorf(ymin);
    int y1 = (int)ceilf(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > r->ch) y1 = r->ch;
    if (y0 >= y1) return;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = color;
    rd.bg_opa = opa;
    rd.radius = 0;
    rd.border_width = 0;
    float xs[MAX_POLY_PTS];
    for (int y = y0; y < y1; y++) {
        float fy = (float)y + 0.5f;
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            float y1p = p[i].y, y2p = p[(i + 1) % n].y;
            if ((y1p <= fy && y2p > fy) || (y2p <= fy && y1p > fy)) {
                float x1p = p[i].x, x2p = p[(i + 1) % n].x;
                float t = (fy - y1p) / (y2p - y1p);
                if (cnt < MAX_POLY_PTS) xs[cnt++] = x1p + t * (x2p - x1p);
            }
        }
        for (int i = 1; i < cnt; i++) {
            float k = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > k) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = k;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            int32_t xa = clampi(xs[i]), xb = clampi(xs[i + 1]);
            if (xa < 0) xa = 0;
            if (xb > r->cw) xb = r->cw;
            if (xb <= xa) continue;
            lv_area_t a = { xa, y, xb - 1, y };
            lv_draw_rect(r->layer, &rd, &a);
        }
    }
}

static void stroke_polyline(rctx_t *r, const lv_point_precise_t *p, int n, bool closed, lv_color_t color,
                            float width, lv_opa_t opa)
{
    if (n < 2 || opa == 0 || width <= 0) return;
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = color;
    ld.opa = opa;
    ld.width = (int32_t)lroundf(width);
    if (ld.width < 1) ld.width = 1;
    ld.round_start = 1;
    ld.round_end = 1;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        ld.p1.x = p[i].x; ld.p1.y = p[i].y;
        ld.p2.x = p[j].x; ld.p2.y = p[j].y;
        lv_draw_line(r->layer, &ld);
    }
}

/* ---------------------------------------------------------------- fonts */

static const lv_font_t *pick_font(float px)
{
    static const struct { float sz; bz_font_role_t r; } T[] = {
        { 13, BZ_F_CAPTION }, { 17, BZ_F_BODY_S }, { 20, BZ_F_BODY }, { 24, BZ_F_NAME }, { 36, BZ_F_TITLE },
    };
    size_t best = 0;
    float bd = 1e9f;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        float dd = fabsf(px - T[i].sz);
        if (dd < bd) { bd = dd; best = i; }
    }
    return bz_font(T[best].r);
}

/* ---------------------------------------------------------------- shapes */

static void draw_line_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag)
{
    lv_opa_t sopa = opa_of(st.opacity * st.stroke_opacity);
    if (st.stroke_none || sopa == 0 || st.stroke_width <= 0) return;
    if (!prim_budget(r)) return;
    float x1 = numattr(tag, "x1", 0), y1 = numattr(tag, "y1", 0);
    float x2 = numattr(tag, "x2", 0), y2 = numattr(tag, "y2", 0);
    lv_point_precise_t pts[2];
    mat_apply(m, x1, y1, &pts[0].x, &pts[0].y);
    mat_apply(m, x2, y2, &pts[1].x, &pts[1].y);
    stroke_polyline(r, pts, 2, false, st.stroke, st.stroke_width * mat_scale(m), sopa);
}

static void draw_rect_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag)
{
    float x = numattr(tag, "x", 0), y = numattr(tag, "y", 0);
    float w = numattr(tag, "width", 0), h = numattr(tag, "height", 0);
    if (w <= 0 || h <= 0) return;
    float rx = numattr(tag, "rx", -1), ry = numattr(tag, "ry", -1);
    if (rx < 0 && ry >= 0) rx = ry;
    if (ry < 0 && rx >= 0) ry = rx;
    if (rx < 0) { rx = 0; ry = 0; }
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    lv_opa_t fopa = opa_of(st.opacity * st.fill_opacity), sopa = opa_of(st.opacity * st.stroke_opacity);
    bool dofill = !st.fill_none && fopa > 0;
    bool dostroke = !st.stroke_none && sopa > 0 && st.stroke_width > 0;
    if (!dofill && !dostroke) return;
    if (!prim_budget(r)) return;
    float scale = mat_scale(m);
    if (mat_axis_aligned(m)) {
        float ax, ay, bx, by;
        mat_apply(m, x, y, &ax, &ay);
        mat_apply(m, x + w, y + h, &bx, &by);
        int32_t x0 = clampi(fminf(ax, bx)), x1 = clampi(fmaxf(ax, bx));
        int32_t y0 = clampi(fminf(ay, by)), y1 = clampi(fmaxf(ay, by));
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = (int32_t)lroundf((rx + ry) * 0.5f * scale);
        d.bg_color = st.fill;
        d.bg_opa = dofill ? fopa : LV_OPA_TRANSP;
        d.border_color = st.stroke;
        d.border_width = dostroke ? (int32_t)lroundf(st.stroke_width * scale) : 0;
        if (dostroke && d.border_width < 1) d.border_width = 1;
        d.border_opa = dostroke ? sopa : LV_OPA_TRANSP;
        lv_area_t a = { x0, y0, x1 - 1, y1 - 1 };
        lv_draw_rect(r->layer, &d, &a);
    } else {
        /* rx/ry can't survive a rotation without becoming a general rounded shape; draw the plain
         * quad instead of pretending the corners are still circular arcs */
        lv_point_precise_t pts[4];
        float px[4] = { x, x + w, x + w, x }, py[4] = { y, y, y + h, y + h };
        for (int i = 0; i < 4; i++) mat_apply(m, px[i], py[i], &pts[i].x, &pts[i].y);
        if (dofill) fill_polygon(r, pts, 4, st.fill, fopa);
        if (dostroke) stroke_polyline(r, pts, 4, true, st.stroke, st.stroke_width * scale, sopa);
    }
}

static void draw_ellipse_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag, bool ellipse)
{
    float cx = numattr(tag, "cx", 0), cy = numattr(tag, "cy", 0);
    float rx, ry;
    if (ellipse) {
        rx = numattr(tag, "rx", 0);
        ry = numattr(tag, "ry", 0);
    } else {
        rx = ry = numattr(tag, "r", 0);
    }
    if (rx <= 0 || ry <= 0) return;
    lv_opa_t fopa = opa_of(st.opacity * st.fill_opacity), sopa = opa_of(st.opacity * st.stroke_opacity);
    bool dofill = !st.fill_none && fopa > 0;
    bool dostroke = !st.stroke_none && sopa > 0 && st.stroke_width > 0;
    if (!dofill && !dostroke) return;
    if (!prim_budget(r)) return;
    float scale = mat_scale(m);
    if (mat_axis_aligned(m)) {
        float sx = fabsf(m.a), sy = fabsf(m.d);
        float trx = rx * sx, try_ = ry * sy;
        float tcx, tcy;
        mat_apply(m, cx, cy, &tcx, &tcy);
        if (fabsf(trx - try_) < 0.75f) {
            int32_t rad = (int32_t)lroundf((trx + try_) * 0.5f);
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.radius = LV_RADIUS_CIRCLE;
            d.bg_color = st.fill;
            d.bg_opa = dofill ? fopa : LV_OPA_TRANSP;
            d.border_color = st.stroke;
            d.border_width = dostroke ? (int32_t)lroundf(st.stroke_width * scale) : 0;
            if (dostroke && d.border_width < 1) d.border_width = 1;
            d.border_opa = dostroke ? sopa : LV_OPA_TRANSP;
            lv_area_t a = { clampi(tcx - (float)rad), clampi(tcy - (float)rad), clampi(tcx + (float)rad) - 1,
                            clampi(tcy + (float)rad) - 1 };
            lv_draw_rect(r->layer, &d, &a);
            return;
        }
        lv_point_precise_t pts[48];
        for (int i = 0; i < 48; i++) {
            float t = (float)i / 48.0f * 6.2831853f;
            pts[i].x = tcx + cosf(t) * trx;
            pts[i].y = tcy + sinf(t) * try_;
        }
        if (dofill) fill_polygon(r, pts, 48, st.fill, fopa);
        if (dostroke) stroke_polyline(r, pts, 48, true, st.stroke, st.stroke_width * scale, sopa);
        return;
    }
    lv_point_precise_t pts[48];
    for (int i = 0; i < 48; i++) {
        float t = (float)i / 48.0f * 6.2831853f;
        mat_apply(m, cx + cosf(t) * rx, cy + sinf(t) * ry, &pts[i].x, &pts[i].y);
    }
    if (dofill) fill_polygon(r, pts, 48, st.fill, fopa);
    if (dostroke) stroke_polyline(r, pts, 48, true, st.stroke, st.stroke_width * scale, sopa);
}

static void draw_poly_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag, bool closed)
{
    const char *vs;
    size_t vlen;
    if (!tag_attr_raw(tag, "points", &vs, &vlen)) return;
    char buf[1024];
    if (vlen >= sizeof buf) vlen = sizeof buf - 1;
    memcpy(buf, vs, vlen);
    buf[vlen] = 0;
    float nums[2 * MAX_POLY_PTS];
    int cnt = parse_floats(buf, nums, 2 * MAX_POLY_PTS);
    int n = cnt / 2;
    if (n < 2) return;
    lv_opa_t fopa = opa_of(st.opacity * st.fill_opacity), sopa = opa_of(st.opacity * st.stroke_opacity);
    bool dofill = !st.fill_none && fopa > 0;
    bool dostroke = !st.stroke_none && sopa > 0 && st.stroke_width > 0;
    if (!dofill && !dostroke) return;
    if (!prim_budget(r)) return;
    lv_point_precise_t pts[MAX_POLY_PTS];
    for (int i = 0; i < n; i++) mat_apply(m, nums[2 * i], nums[2 * i + 1], &pts[i].x, &pts[i].y);
    if (dofill) fill_polygon(r, pts, n, st.fill, fopa);
    if (dostroke) stroke_polyline(r, pts, n, closed, st.stroke, st.stroke_width * mat_scale(m), sopa);
}

/* ---------------------------------------------------------------- path data */

typedef struct {
    rctx_t *r;
    mat_t m;
    style_t st;
    lv_point_precise_t pts[MAX_POLY_PTS];
    int npts;
    float cx, cy;
    float start_x, start_y;
    float last_cubic_cx, last_cubic_cy;
    bool have_cubic_ctrl;
    float last_quad_cx, last_quad_cy;
    bool have_quad_ctrl;
    bool closed;
} path_ctx_t;

static void path_push(path_ctx_t *pc, float lx, float ly)
{
    if (pc->npts < MAX_POLY_PTS) {
        mat_apply(pc->m, lx, ly, &pc->pts[pc->npts].x, &pc->pts[pc->npts].y);
        pc->npts++;
    }
}

static void path_finalize(path_ctx_t *pc)
{
    if (pc->npts >= 1) {
        lv_opa_t fopa = opa_of(pc->st.opacity * pc->st.fill_opacity);
        lv_opa_t sopa = opa_of(pc->st.opacity * pc->st.stroke_opacity);
        bool dofill = !pc->st.fill_none && fopa > 0 && pc->npts >= 3;
        bool dostroke = !pc->st.stroke_none && sopa > 0 && pc->st.stroke_width > 0 && pc->npts >= 2;
        if ((dofill || dostroke) && prim_budget(pc->r)) {
            if (dofill) fill_polygon(pc->r, pc->pts, pc->npts, pc->st.fill, fopa);
            if (dostroke)
                stroke_polyline(pc->r, pc->pts, pc->npts, pc->closed, pc->st.stroke,
                                pc->st.stroke_width * mat_scale(pc->m), sopa);
        }
    }
    pc->npts = 0;
    pc->closed = false;
}

static void path_cubic(path_ctx_t *pc, float x1, float y1, float x2, float y2, float x, float y)
{
    const int N = 16;
    float x0 = pc->cx, y0 = pc->cy;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / N, u = 1 - t;
        float bx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
        float by = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
        path_push(pc, bx, by);
    }
    pc->last_cubic_cx = x2; pc->last_cubic_cy = y2; pc->have_cubic_ctrl = true;
    pc->have_quad_ctrl = false;
    pc->cx = x; pc->cy = y;
}

static void path_quad(path_ctx_t *pc, float x1, float y1, float x, float y)
{
    const int N = 16;
    float x0 = pc->cx, y0 = pc->cy;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / N, u = 1 - t;
        float bx = u * u * x0 + 2 * u * t * x1 + t * t * x;
        float by = u * u * y0 + 2 * u * t * y1 + t * t * y;
        path_push(pc, bx, by);
    }
    pc->last_quad_cx = x1; pc->last_quad_cy = y1; pc->have_quad_ctrl = true;
    pc->have_cubic_ctrl = false;
    pc->cx = x; pc->cy = y;
}

static void draw_path_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag, unsigned *skip)
{
    const char *ds;
    size_t dlen;
    if (!tag_attr_raw(tag, "d", &ds, &dlen)) return;
    const char *p = ds, *end = ds + dlen;
    path_ctx_t pc;
    memset(&pc, 0, sizeof pc);
    pc.r = r; pc.m = m; pc.st = st;
    char cmd = 0;
    int guard = 0;
    while (p < end && guard++ < 20000) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;
        if (isalpha((unsigned char)*p)) { cmd = *p; p++; }
        if (cmd == 0) break;
        if (cmd == 'Z' || cmd == 'z') {
            if (pc.npts > 0) { path_push(&pc, pc.start_x, pc.start_y); pc.closed = true; path_finalize(&pc); }
            pc.cx = pc.start_x; pc.cy = pc.start_y;
            cmd = 0;
            continue;
        }
        if (cmd == 'A' || cmd == 'a') {
            *skip |= SKIP_ARC;
            break;
        }
        int nargs;
        switch (tolower((unsigned char)cmd)) {
        case 'm': case 'l': case 't': nargs = 2; break;
        case 'h': case 'v': nargs = 1; break;
        case 'c': nargs = 6; break;
        case 's': case 'q': nargs = 4; break;
        default: nargs = -1; break;
        }
        if (nargs < 0) break; /* an unknown letter: stop cleanly rather than guess its arity */
        float args[6];
        int got = 0;
        while (got < nargs && p < end) {
            while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
            char *ep;
            float v = strtof(p, &ep);
            if (ep == p || !isfinite(v)) break;
            args[got++] = v;
            p = ep;
        }
        if (got < nargs) break;
        bool rel = islower((unsigned char)cmd) != 0;
        switch (tolower((unsigned char)cmd)) {
        case 'm': {
            float x = args[0], y = args[1];
            if (rel) { x += pc.cx; y += pc.cy; }
            if (pc.npts > 0) path_finalize(&pc);
            pc.cx = x; pc.cy = y; pc.start_x = x; pc.start_y = y;
            path_push(&pc, x, y);
            pc.have_cubic_ctrl = false; pc.have_quad_ctrl = false;
            cmd = rel ? 'l' : 'L';
            break;
        }
        case 'l': {
            float x = args[0], y = args[1];
            if (rel) { x += pc.cx; y += pc.cy; }
            path_push(&pc, x, y);
            pc.cx = x; pc.cy = y;
            pc.have_cubic_ctrl = false; pc.have_quad_ctrl = false;
            break;
        }
        case 'h': {
            float x = args[0];
            if (rel) x += pc.cx;
            path_push(&pc, x, pc.cy);
            pc.cx = x;
            pc.have_cubic_ctrl = false; pc.have_quad_ctrl = false;
            break;
        }
        case 'v': {
            float y = args[0];
            if (rel) y += pc.cy;
            path_push(&pc, pc.cx, y);
            pc.cy = y;
            pc.have_cubic_ctrl = false; pc.have_quad_ctrl = false;
            break;
        }
        case 'c': {
            float x1 = args[0], y1 = args[1], x2 = args[2], y2 = args[3], x = args[4], y = args[5];
            if (rel) { x1 += pc.cx; y1 += pc.cy; x2 += pc.cx; y2 += pc.cy; x += pc.cx; y += pc.cy; }
            path_cubic(&pc, x1, y1, x2, y2, x, y);
            break;
        }
        case 's': {
            float x2 = args[0], y2 = args[1], x = args[2], y = args[3];
            if (rel) { x2 += pc.cx; y2 += pc.cy; x += pc.cx; y += pc.cy; }
            float x1, y1;
            if (pc.have_cubic_ctrl) { x1 = 2 * pc.cx - pc.last_cubic_cx; y1 = 2 * pc.cy - pc.last_cubic_cy; }
            else { x1 = pc.cx; y1 = pc.cy; }
            path_cubic(&pc, x1, y1, x2, y2, x, y);
            break;
        }
        case 'q': {
            float x1 = args[0], y1 = args[1], x = args[2], y = args[3];
            if (rel) { x1 += pc.cx; y1 += pc.cy; x += pc.cx; y += pc.cy; }
            path_quad(&pc, x1, y1, x, y);
            break;
        }
        case 't': {
            float x = args[0], y = args[1];
            if (rel) { x += pc.cx; y += pc.cy; }
            float x1, y1;
            if (pc.have_quad_ctrl) { x1 = 2 * pc.cx - pc.last_quad_cx; y1 = 2 * pc.cy - pc.last_quad_cy; }
            else { x1 = pc.cx; y1 = pc.cy; }
            path_quad(&pc, x1, y1, x, y);
            break;
        }
        default: break;
        }
        if (r->cut_short) break;
    }
    path_finalize(&pc);
}

/* ---------------------------------------------------------------- text */

static void extract_text(const char *src, size_t n, char *out, size_t outsz, unsigned *skip)
{
    size_t oi = 0;
    bool in_tag = false;
    for (size_t i = 0; i < n && oi + 1 < outsz; i++) {
        char c = src[i];
        if (c == '<') {
            in_tag = true;
            if (i + 9 <= n && !strncmp(src + i, "<textPath", 9)) *skip |= SKIP_TEXTPATH;
            continue;
        }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (c == '&') {
            if (!strncmp(src + i, "&amp;", 5)) { out[oi++] = '&'; i += 4; continue; }
            if (!strncmp(src + i, "&lt;", 4)) { out[oi++] = '<'; i += 3; continue; }
            if (!strncmp(src + i, "&gt;", 4)) { out[oi++] = '>'; i += 3; continue; }
            if (!strncmp(src + i, "&quot;", 6)) { out[oi++] = '"'; i += 5; continue; }
            if (!strncmp(src + i, "&apos;", 6)) { out[oi++] = '\''; i += 5; continue; }
            out[oi++] = c;
            continue;
        }
        out[oi++] = c;
    }
    out[oi] = 0;
    trim(out);
}

static void handle_text_el(rctx_t *r, mat_t m, style_t st, const tag_t *tag, const char **pp, bool do_draw,
                           unsigned *skip)
{
    const char *p = *pp;
    char text[220] = "";
    if (!tag->self_close) {
        const char *close = strstr(p, "</text>");
        if (close) {
            extract_text(p, (size_t)(close - p), text, sizeof text, skip);
            *pp = close + 7;
        }
        /* no closing tag: leave the cursor where it is and treat this as empty text */
    }
    if (!do_draw || !text[0]) return;
    lv_opa_t fopa = opa_of(st.opacity * st.fill_opacity);
    if (st.fill_none || fopa == 0) return;
    if (!prim_budget(r)) return;
    float x = numattr(tag, "x", 0), y = numattr(tag, "y", 0);
    float tx, ty;
    mat_apply(m, x, y, &tx, &ty);
    bool rotated = !mat_axis_aligned(m);
    if (rotated) *skip |= SKIP_ROTATED_TEXT; /* drawn upright at the transformed origin regardless */
    float scale = mat_scale(m);
    const lv_font_t *font = pick_font(st.font_size * scale);
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    float dx = tx;
    if (st.text_anchor == 1) dx -= (float)sz.x / 2.0f;
    else if (st.text_anchor == 2) dx -= (float)sz.x;
    /* SVG's y is the text baseline; this renderer has no font metrics handy, so the top of the box
     * is placed a fixed fraction above y as a stand-in for the ascent */
    float dy = ty - (float)sz.y * 0.8f;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.text_local = 1; /* the layer's draw tasks outlive this stack buffer until finish_layer returns */
    d.font = font;
    d.color = st.fill;
    d.opa = fopa;
    d.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t a = { clampi(dx), clampi(dy), clampi(dx) + sz.x, clampi(dy) + sz.y };
    lv_draw_label(r->layer, &d, &a);
}

/* ---------------------------------------------------------------- the parser stack */

typedef struct {
    char name[24];
    mat_t m;
    style_t st;
    bool skip;
} frame_t;

typedef struct {
    frame_t stack[MAX_DEPTH];
    int sp;
    int overflow;
    mat_t cur_m;
    style_t cur_st;
    bool cur_skip;
} parser_state_t;

/* ---------------------------------------------------------------- the note */

static void build_note(unsigned skip, bool cut_short, char *out, size_t n)
{
    char list[256];
    size_t lo = 0;
    list[0] = 0;
    for (size_t i = 0; i < sizeof SKIP_MSGS / sizeof SKIP_MSGS[0]; i++) {
        if (skip & SKIP_MSGS[i].bit) {
            int w = snprintf(list + lo, sizeof list - lo, "%s%s", lo ? ", " : "", SKIP_MSGS[i].text);
            if (w > 0) lo += (size_t)w < sizeof list - lo ? (size_t)w : sizeof list - lo - 1;
        }
    }
    if (cut_short && lo) snprintf(out, n, "the drawing was too long and was cut short; left out: %s", list);
    else if (cut_short) snprintf(out, n, "the drawing was too long and was cut short");
    else if (lo) snprintf(out, n, "left out: %s", list);
    else out[0] = 0;
}

static const char *find_svg_tag(const char *s)
{
    const char *p = s;
    while ((p = strstr(p, "<svg")) != NULL) {
        char c = p[4];
        if (c == 0 || isspace((unsigned char)c) || c == '>' || c == '/') return p;
        p += 4;
    }
    return NULL;
}

static void clamp_canvas(int *w, int *h)
{
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    for (int i = 0; i < 8; i++) {
        uint32_t sz = LV_CANVAS_BUF_SIZE(*w, *h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
        if (sz <= MAX_CANVAS_BYTES) return;
        double f = sqrt((double)MAX_CANVAS_BYTES / (double)sz) * 0.95;
        int nw = (int)((double)*w * f), nh = (int)((double)*h * f);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (nw == *w && nh == *h) { nw = *w > 1 ? *w - 1 : 1; nh = *h > 1 ? *h - 1 : 1; }
        *w = nw; *h = nh;
    }
    /* still too big after 8 halvings of a degenerate aspect ratio: force something safe */
    if (LV_CANVAS_BUF_SIZE(*w, *h, 16, LV_DRAW_BUF_STRIDE_ALIGN) > MAX_CANVAS_BYTES) { *w = 64; *h = 64; }
}

static void svg_canvas_delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    void *buf = lv_obj_get_user_data(o);
    if (buf) lv_free(buf);
}

/* ---------------------------------------------------------------- ui_md_svg */

lv_obj_t *ui_md_svg(lv_obj_t *parent, const char *svg, int max_w, int max_h, char *why, size_t nwhy)
{
    if (why && nwhy) why[0] = 0;
    if (!svg) svg = "";

    const char *svg_start = find_svg_tag(svg);
    if (!svg_start) {
        if (why) snprintf(why, nwhy, "no <svg> element");
        return NULL;
    }
    tag_t svgtag;
    const char *body = parse_open_tag(svg_start, &svgtag);
    if (!body || !ieq(svgtag.name, "svg")) {
        if (why) snprintf(why, nwhy, "no <svg> element");
        return NULL;
    }

    float vx = 0, vy = 0, vw = 100, vh = 100;
    char vb[128];
    if (tag_attr(&svgtag, "viewBox", vb, sizeof vb)) {
        float fv[4];
        if (parse_floats(vb, fv, 4) == 4 && fv[2] > 0 && fv[3] > 0) { vx = fv[0]; vy = fv[1]; vw = fv[2]; vh = fv[3]; }
    } else {
        float w = numattr(&svgtag, "width", 0), h = numattr(&svgtag, "height", 0);
        if (w > 0 && h > 0) { vw = w; vh = h; }
    }

    int cw = max_w > 0 ? max_w : 100, ch = max_h > 0 ? max_h : 100;
    clamp_canvas(&cw, &ch);

    size_t bufsize = LV_CANVAS_BUF_SIZE(cw, ch, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    /* Not "is there room for this?" but "is there room left afterwards?". lv_malloc would succeed with
     * 400 KB free and leave the glass cache, the next window and the slide's own buffers with nothing. */
    size_t m[4];
    hal_mem_psram(m);
    if (m[1] < bufsize + CANVAS_PSRAM_FLOOR) {
        if (why) snprintf(why, nwhy, "no room to draw it (%u KB free)", (unsigned)(m[1] / 1024));
        return NULL;
    }
    void *buf = lv_malloc(bufsize);
    if (!buf) {
        if (why) snprintf(why, nwhy, "out of memory");
        return NULL;
    }
    lv_obj_t *canvas = lv_canvas_create(parent);
    if (!canvas) {
        lv_free(buf);
        if (why) snprintf(why, nwhy, "out of memory");
        return NULL;
    }
    lv_canvas_set_buffer(canvas, buf, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_user_data(canvas, buf);
    lv_obj_add_event_cb(canvas, svg_canvas_delete_cb, LV_EVENT_DELETE, NULL);
    lv_canvas_fill_bg(canvas, bz_lv(BZ_C_GROUND), LV_OPA_COVER);

    /* the root transform: fit the viewBox into the canvas, uniformly scaled and centred */
    float sc = fminf((float)cw / vw, (float)ch / vh);
    mat_t root = { sc, 0, 0, sc, ((float)cw - vw * sc) * 0.5f - vx * sc, ((float)ch - vh * sc) * 0.5f - vy * sc };

    rctx_t rctx;
    memset(&rctx, 0, sizeof rctx);
    rctx.cw = cw; rctx.ch = ch;
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    rctx.layer = &layer;

    unsigned skip_mask = 0;

    if (!svgtag.self_close) {
        parser_state_t ps;
        memset(&ps, 0, sizeof ps);
        ps.cur_m = root;
        ps.cur_st = default_style();
        ps.cur_skip = false;

        const char *p = body;
        int guard = 0;
        while (*p && !rctx.cut_short && guard < 20000) {
            guard++;
            const char *lt = strchr(p, '<');
            if (!lt) break;
            if (lt[1] == '!') {
                if (lt[2] == '-' && lt[3] == '-') {
                    const char *e = strstr(lt + 4, "-->");
                    p = e ? e + 3 : lt + strlen(lt);
                } else {
                    const char *e = strchr(lt + 1, '>');
                    p = e ? e + 1 : lt + strlen(lt);
                }
                continue;
            }
            if (lt[1] == '?') {
                const char *e = strstr(lt + 2, "?>");
                p = e ? e + 2 : lt + strlen(lt);
                continue;
            }
            if (lt[1] == '/') {
                const char *q = lt + 2;
                while (*q && isspace((unsigned char)*q)) q++;
                char nm[24];
                size_t ni = 0;
                while (*q && *q != '>' && !isspace((unsigned char)*q) && ni < sizeof nm - 1) nm[ni++] = *q++;
                nm[ni] = 0;
                const char *gt = strchr(q, '>');
                if (!gt) break;
                p = gt + 1;
                if (ps.overflow > 0) {
                    ps.overflow--;
                } else {
                    for (int k = ps.sp - 1; k >= 0; k--) {
                        if (ieq(ps.stack[k].name, nm)) {
                            ps.cur_m = ps.stack[k].m;
                            ps.cur_st = ps.stack[k].st;
                            ps.cur_skip = ps.stack[k].skip;
                            ps.sp = k;
                            break;
                        }
                    }
                }
                continue;
            }
            tag_t tag;
            const char *after = parse_open_tag(lt, &tag);
            if (!after) break;
            p = after;
            bool skip_now = ps.cur_skip || ps.overflow > 0;
            if (is_leaf_shape(tag.name)) {
                mat_t m = ps.cur_m;
                style_t st = ps.cur_st;
                char v[160];
                if (tag_attr(&tag, "transform", v, sizeof v)) m = mat_mul(ps.cur_m, parse_transform(v));
                style_apply(&st, &tag, &skip_mask);
                if (ieq(tag.name, "text")) {
                    handle_text_el(&rctx, m, st, &tag, &p, !skip_now, &skip_mask);
                } else if (!skip_now) {
                    if (ieq(tag.name, "line")) draw_line_el(&rctx, m, st, &tag);
                    else if (ieq(tag.name, "rect")) draw_rect_el(&rctx, m, st, &tag);
                    else if (ieq(tag.name, "circle")) draw_ellipse_el(&rctx, m, st, &tag, false);
                    else if (ieq(tag.name, "ellipse")) draw_ellipse_el(&rctx, m, st, &tag, true);
                    else if (ieq(tag.name, "polyline")) draw_poly_el(&rctx, m, st, &tag, false);
                    else if (ieq(tag.name, "polygon")) draw_poly_el(&rctx, m, st, &tag, true);
                    else if (ieq(tag.name, "path")) draw_path_el(&rctx, m, st, &tag, &skip_mask);
                }
            } else {
                unsigned bit = classify_skip_tag(tag.name);
                skip_mask |= bit;
                if (!tag.self_close) {
                    if (ps.sp < MAX_DEPTH) {
                        mat_t m = ps.cur_m;
                        style_t st = ps.cur_st;
                        char v[160];
                        if (tag_attr(&tag, "transform", v, sizeof v)) m = mat_mul(ps.cur_m, parse_transform(v));
                        style_apply(&st, &tag, &skip_mask);
                        frame_t *fr = &ps.stack[ps.sp];
                        snprintf(fr->name, sizeof fr->name, "%s", tag.name);
                        fr->m = ps.cur_m; fr->st = ps.cur_st; fr->skip = ps.cur_skip;
                        ps.sp++;
                        ps.cur_m = m; ps.cur_st = st; ps.cur_skip = skip_now || bit != 0;
                    } else {
                        ps.overflow++;
                        skip_mask |= SKIP_DEEP;
                    }
                }
            }
        }
    }

    lv_canvas_finish_layer(canvas, &layer);

    if (!rctx.any_drawn) {
        lv_obj_delete(canvas); /* the LV_EVENT_DELETE hook frees buf */
        if (why) {
            char note[220];
            build_note(skip_mask, false, note, sizeof note);
            if (note[0]) snprintf(why, nwhy, "the drawing is empty (%s)", note);
            else snprintf(why, nwhy, "the drawing is empty");
        }
        return NULL;
    }

    if (why) build_note(skip_mask, rctx.cut_short, why, nwhy);
    return canvas;
}
