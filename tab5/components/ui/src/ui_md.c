/* ui_md — markdown, charts and drawings, the way the model wrote them.
 *
 * Until now every answer went through a function that *stripped* markdown: the marks came off, the
 * headings became sentences, a table of numbers became a paragraph. That was honest while there was no
 * bold face and one label per answer. There is a bold face now (BZ_F_BODY_B), and LVGL's span group puts
 * several faces in one flowing paragraph, so the marks can mean something instead of being deleted.
 *
 * WHAT IT DRAWS. Headings (#, ##, ###), paragraphs with **bold** and `code` runs, fenced code, bullet
 * and numbered lists, block quotes and rules. Deliberately not: tables, images, links as links, nested
 * lists past one level, HTML. A model told to keep its answers short does not reach for those, and each
 * one costs objects on a tablet that redraws with a CPU.
 *
 * TWO FENCES DO MORE. ```chart is a line-oriented chart syntax a model can emit inside an answer in a few
 * dozen bytes — docs/render-blocks.md is the contract and the assistant's prompt teaches it. ```svg is
 * the subset of SVG this hardware can honestly rasterise (ui_md_svg, in ui_md_svg.c).
 *
 * MALFORMED INPUT IS SHOWN, NOT GUESSED. A chart whose series are different lengths, or whose numbers are
 * not numbers, becomes a card that says exactly that, with the block printed underneath as code. A wrong
 * chart of the robot's battery is worse than no chart: someone would act on it.
 *
 * COST. One label per paragraph; a span group only where a paragraph actually has a bold or code run
 * (they are dearer, and most lines have neither). Everything is a child of one flex column the caller
 * owns, sized to content, so it drops into a scroller. */
#include "ui_md.h"

#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MD_GAP 10           /* between blocks */
#define MD_CHART_H 190
#define MD_MAXPTS 96        /* points a series may carry */
#define MD_MAXSER 4
#define MD_YW 96            /* the chart's y-label gutter: wide enough for "12.60 V" */

/* ================================================================== helpers */

/* Nothing the renderer makes may swallow a drag: a document is scrolled by dragging anywhere on it,
 * including across a chart or a code card, and LVGL objects keep a press to themselves by default. */
static void passthrough(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static bool line_is(const char *s, const char *mark)
{
    size_t n = strlen(mark);
    return !strncmp(s, mark, n);
}

/* the run of characters up to (not including) the newline */
static size_t line_len(const char *s)
{
    const char *nl = strchr(s, '\n');
    return nl ? (size_t)(nl - s) : strlen(s);
}

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
}

/* a rule: three or more of - _ * and nothing else */
static bool is_rule(const char *s, size_t n)
{
    if (n < 3) return false;
    char c = s[0];
    if (c != '-' && c != '_' && c != '*') return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] != c && s[i] != ' ') return false;
    return true;
}

static bool has_marks(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '`') return true;
        if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') return true;
    }
    return false;
}

bool ui_md_is_rich(const char *md)
{
    if (!md) return false;
    for (const char *p = md; *p; p++) {
        if (*p == '`') return true;
        if (p[0] == '*' && p[1] == '*') return true;
        if ((p == md || p[-1] == '\n') && (*p == '#' || *p == '>' || (*p == '|' && strchr(p, '\n')))) return true;
        if ((p == md || p[-1] == '\n') && (p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') return true;
        if ((p == md || p[-1] == '\n') && p[0] >= '1' && p[0] <= '9' && p[1] == '.' && p[2] == ' ') return true;
    }
    return false;
}

/* ================================================================== inline runs */

/* A paragraph with its faces: **bold** in the bold body, `code` in the mono face on a faint ground, the
 * rest in `base`. A run of plain text with no marks is a plain label, which is a quarter of the cost. */
static lv_obj_t *rich(lv_obj_t *p, const char *s, size_t n, int w, bz_font_role_t base, bz_color_role_t col)
{
    if (!has_marks(s, n)) {
        char *buf = lv_malloc(n + 1);
        if (!buf) return NULL;
        /* the line breaks inside a paragraph are soft: markdown reflows them, and a label that honoured
         * them would wrap where the model happened to hit eighty columns */
        for (size_t i = 0; i < n; i++) buf[i] = s[i] == '\n' ? ' ' : s[i];
        buf[n] = 0;
        lv_obj_t *l = bz_label(p, buf, base, col);
        lv_free(buf);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, w);
        return l;
    }
    lv_obj_t *sg = lv_spangroup_create(p);
    lv_obj_remove_style_all(sg);
    lv_obj_set_width(sg, w);
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_spangroup_set_overflow(sg, LV_SPAN_OVERFLOW_CLIP);

    char *buf = lv_malloc(n + 1);
    if (!buf) return sg;
    size_t o = 0;
    bool bold = false, code = false;
    /* flushes what has been gathered as one span in the face the marks say */
    for (size_t i = 0; i <= n; i++) {
        bool at_bold = i + 1 < n && s[i] == '*' && s[i + 1] == '*';
        bool at_code = i < n && s[i] == '`';
        if (i == n || at_bold || at_code) {
            if (o) {
                buf[o] = 0;
                lv_span_t *sp = lv_spangroup_add_span(sg);
                lv_span_set_text(sp, buf);
                lv_style_t *st = lv_span_get_style(sp);
                lv_style_set_text_font(st, bz_font(code ? BZ_F_LABEL : bold ? BZ_F_BODY_B : base));
                lv_style_set_text_color(st, bz_lv(code ? BZ_C_ICE : col));
                o = 0;
            }
            if (at_bold) {
                bold = !bold;
                i++;
            } else if (at_code) {
                code = !code;
            }
            continue;
        }
        buf[o++] = s[i] == '\n' ? ' ' : s[i];
    }
    lv_free(buf);
    return sg;
}

/* ================================================================== blocks */

static void heading(lv_obj_t *p, const char *s, size_t n, int w, int level)
{
    bz_font_role_t f = level <= 1 ? BZ_F_TITLE : level == 2 ? BZ_F_NAME : BZ_F_BODY_B;
    lv_obj_t *l = rich(p, s, n, w, f, BZ_C_INK);
    if (l) lv_obj_set_style_pad_top(l, level <= 2 ? 6 : 2, 0);
}

static void rule(lv_obj_t *p, int w)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, 2);
    lv_obj_add_style(r, bz_style_fill(BZ_C_FAINT), 0);
    lv_obj_set_style_radius(r, 1, 0);
}

/* "— text" / "1. text": the mark in its own column so the text lines up under itself when it wraps */
static void bullet(lv_obj_t *p, const char *mark, const char *s, size_t n, int w)
{
    lv_obj_t *row = bz_row(p, 8);
    lv_obj_set_width(row, w);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(row, 6, 0);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_START, 0); /* the mark on the first line */
    lv_obj_t *m = bz_label(row, mark, BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_width(m, 26);
    rich(row, s, n, w - 26 - 8 - 6, BZ_F_BODY, BZ_C_INK);
}

static void quote(lv_obj_t *p, const char *s, size_t n, int w)
{
    lv_obj_t *row = bz_row(p, 12);
    lv_obj_set_width(row, w);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 3, LV_PCT(100));
    lv_obj_add_style(bar, bz_style_fill(BZ_C_ICE), 0);
    lv_obj_set_style_radius(bar, 2, 0);
    rich(row, s, n, w - 15, BZ_F_BODY, BZ_C_DIM);
}

static lv_obj_t *code_card(lv_obj_t *p, const char *s, int w, bz_color_role_t ink)
{
    lv_obj_t *t = lv_obj_create(p);
    lv_obj_remove_style_all(t);
    lv_obj_set_width(t, w);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_add_style(t, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 14, 0);
    passthrough(t);
    lv_obj_t *l = bz_label(t, s, BZ_F_LABEL, ink);
    /* code wraps rather than scrolling sideways: a line you cannot reach is a line you cannot read */
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w - 28);
    return t;
}

/* What went wrong with a block, where the block would have been, with the block itself underneath. */
static void bad_block(lv_obj_t *p, const char *why, const char *body, int w)
{
    lv_obj_t *t = lv_obj_create(p);
    lv_obj_remove_style_all(t);
    lv_obj_set_width(t, w);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_add_style(t, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 14, 0);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_border_color(t, bz_lv(BZ_C_WARN), 0);
    lv_obj_set_style_border_opa(t, LV_OPA_60, 0);
    lv_obj_set_layout(t, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 8, 0);
    passthrough(t);
    lv_obj_t *l = bz_label(t, why, BZ_F_BODY_S, BZ_C_WARN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w - 28);
    if (body && body[0]) {
        lv_obj_t *c = bz_label(t, body, BZ_F_CAPTION, BZ_C_DIM);
        lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(c, w - 28);
    }
}

/* How wide a short ASCII string actually is in a font, kerning included.
 *
 * lv_text_get_width would do it, but it lives in a private header; summing the glyph advances is the
 * public way and is exact for the series names, which are ASCII by the time the parser has them. */
static int text_w(const char *s, const lv_font_t *f)
{
    int w = 0;
    for (; *s; s++)
        w += lv_font_get_glyph_width(f, (uint32_t)(unsigned char)s[0],
                                     s[1] ? (uint32_t)(unsigned char)s[1] : 0);
    return w;
}

/* ================================================================== charts */

typedef struct {
    char kind;                  /* 'l' line, 'b' bar, 's' scatter */
    char title[64];
    char unit[16];
    char name[MD_MAXSER][24];
    float x[MD_MAXPTS];
    int nx;
    float y[MD_MAXSER][MD_MAXPTS];
    int ny[MD_MAXSER];
    int nser;
    float lo, hi;
    bool have_lim;
    bool clipped;               /* a series was longer than we draw */
    char labels[96];
} chart_t;

/* One whitespace/comma-separated number. false (with the offending word) when it is not one. */
static bool numbers(const char *s, float *out, int max, int *n, char *bad, size_t nbad)
{
    *n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s || *s == '\n') break;
        char *end = NULL;
        float v = strtof(s, &end);
        if (end == s) {
            size_t k = 0;
            while (s[k] && s[k] != ' ' && s[k] != ',' && s[k] != '\n' && k + 1 < nbad) k++;
            if (bad && nbad) {
                memcpy(bad, s, k);
                bad[k] = 0;
            }
            return false;
        }
        if (*n < max) out[(*n)++] = v;
        else return true; /* the caller decides what to do with the overflow */
        s = end;
    }
    return true;
}

static bool parse_chart(const char *body, chart_t *c, char *why, size_t nwhy)
{
    memset(c, 0, sizeof *c);
    c->kind = 0;
    const char *p = body;
    while (*p == '\n' || *p == ' ') p++;
    /* the first line names the kind and, after it, the title */
    size_t hl = line_len(p);
    char head[160];
    size_t hn = hl < sizeof head - 1 ? hl : sizeof head - 1;
    memcpy(head, p, hn);
    head[hn] = 0;
    trim_end(head);
    char *sp = strchr(head, ' ');
    if (sp) {
        *sp = 0;
        snprintf(c->title, sizeof c->title, "%s", sp + 1);
    }
    if (!strcmp(head, "line")) c->kind = 'l';
    else if (!strcmp(head, "bar")) c->kind = 'b';
    else if (!strcmp(head, "scatter")) c->kind = 's';
    else {
        snprintf(why, nwhy, "chart: the first word must be line, bar or scatter, not \"%.20s\"", head);
        return false;
    }
    p += hl;

    char bad[24] = "";
    int pending_name = -1;
    while (*p) {
        if (*p == '\n') p++;
        if (!*p) break;
        size_t l = line_len(p);
        char ln[512];
        size_t n = l < sizeof ln - 1 ? l : sizeof ln - 1;
        memcpy(ln, p, n);
        ln[n] = 0;
        p += l;
        trim_end(ln);
        char *arg = strchr(ln, ' ');
        if (arg) *arg++ = 0;
        else arg = ln + strlen(ln);
        while (*arg == ' ') arg++;
        if (!ln[0]) continue;
        if (!strcmp(ln, "x")) {
            if (!numbers(arg, c->x, MD_MAXPTS, &c->nx, bad, sizeof bad)) {
                snprintf(why, nwhy, "chart: x has \"%s\" in it, which is not a number", bad);
                return false;
            }
        } else if (!strcmp(ln, "y")) {
            if (c->nser >= MD_MAXSER) {
                snprintf(why, nwhy, "chart: more than %d series; this tablet draws %d", MD_MAXSER, MD_MAXSER);
                return false;
            }
            int got = 0;
            if (!numbers(arg, c->y[c->nser], MD_MAXPTS, &got, bad, sizeof bad)) {
                snprintf(why, nwhy, "chart: y has \"%s\" in it, which is not a number", bad);
                return false;
            }
            if (got == MD_MAXPTS) c->clipped = true;
            c->ny[c->nser] = got;
            if (pending_name == -2) snprintf(c->name[c->nser], sizeof c->name[0], "%s", c->name[MD_MAXSER - 1]);
            c->nser++;
            pending_name = -1;
        } else if (!strcmp(ln, "name")) {
            /* names the series that follows, or the one just given when it comes after */
            if (c->nser > 0 && pending_name == -1) snprintf(c->name[c->nser - 1], sizeof c->name[0], "%s", arg);
            else {
                snprintf(c->name[c->nser < MD_MAXSER ? c->nser : MD_MAXSER - 1], sizeof c->name[0], "%s", arg);
                pending_name = c->nser;
            }
        } else if (!strcmp(ln, "unit")) {
            snprintf(c->unit, sizeof c->unit, "%s", arg);
        } else if (!strcmp(ln, "labels")) {
            snprintf(c->labels, sizeof c->labels, "%s", arg);
        } else if (!strcmp(ln, "ylim")) {
            float v[2];
            int got = 0;
            if (!numbers(arg, v, 2, &got, bad, sizeof bad) || got != 2) {
                snprintf(why, nwhy, "chart: ylim wants two numbers, a low and a high");
                return false;
            }
            c->lo = v[0];
            c->hi = v[1];
            c->have_lim = true;
        } else {
            snprintf(why, nwhy, "chart: \"%.16s\" is not one of x, y, name, unit, labels, ylim", ln);
            return false;
        }
    }
    if (!c->nser) {
        snprintf(why, nwhy, "chart: there is no y line, so there is nothing to draw");
        return false;
    }
    for (int i = 0; i < c->nser; i++) {
        if (c->ny[i] < 2) {
            snprintf(why, nwhy, "chart: a series needs at least two points; series %d has %d", i + 1, c->ny[i]);
            return false;
        }
        if (c->ny[i] != c->ny[0]) {
            snprintf(why, nwhy, "chart: the series are different lengths (%d and %d)", c->ny[0], c->ny[i]);
            return false;
        }
    }
    if (c->kind == 's') {
        if (c->nx != c->ny[0]) {
            snprintf(why, nwhy, "chart: a scatter needs one x for every y (%d x, %d y)", c->nx, c->ny[0]);
            return false;
        }
    } else if (c->nx && c->nx != c->ny[0]) {
        snprintf(why, nwhy, "chart: %d x values against %d y values", c->nx, c->ny[0]);
        return false;
    }
    return true;
}

static void fmt_num(char *out, size_t n, float v, const char *unit)
{
    if (!isfinite(v)) v = 0;
    float a = fabsf(v);
    /* a value that is a whole number is written as one: "0 to 120", not "0.00 to 120.0" */
    bool whole = a < 1e9f && v == (float)(long)v;
    const char *fmt = whole || a >= 1000 ? "%.0f%s%s" : a >= 100 ? "%.1f%s%s" : "%.2f%s%s";
    snprintf(out, n, fmt, (double)v, unit && unit[0] ? " " : "", unit ? unit : "");
}

lv_obj_t *ui_md_chart(lv_obj_t *parent, const char *body, int width, char *why, size_t nwhy)
{
    char local[160] = "";
    if (!why || !nwhy) {
        why = local;
        nwhy = sizeof local;
    }
    why[0] = 0;
    chart_t *c = lv_malloc(sizeof *c);
    if (!c) {
        snprintf(why, nwhy, "chart: no memory");
        return NULL;
    }
    if (!parse_chart(body, c, why, nwhy)) {
        lv_free(c);
        return NULL;
    }

    /* the range the axis covers, and the map from it onto the chart's integers */
    float lo = c->have_lim ? c->lo : c->y[0][0], hi = c->have_lim ? c->hi : c->y[0][0];
    if (!c->have_lim) {
        for (int s = 0; s < c->nser; s++)
            for (int i = 0; i < c->ny[s]; i++) {
                if (c->y[s][i] < lo) lo = c->y[s][i];
                if (c->y[s][i] > hi) hi = c->y[s][i];
            }
        float pad = (hi - lo) * 0.08f;
        if (pad <= 0) pad = fabsf(hi) * 0.05f + 0.5f;
        lo -= pad;
        hi += pad;
    }
    if (hi <= lo) hi = lo + 1;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    passthrough(card);
    int in_w = width - 28;

    int y = 0;
    if (c->title[0]) {
        lv_obj_t *t = bz_label_line(card, c->title, BZ_F_LABEL, BZ_C_DIM, in_w);
        lv_obj_set_pos(t, 0, 0);
        y = 24;
    }

    /* the value gutter: the top and bottom of the axis, written out, because LVGL's chart draws no ticks */
    char buf[32];
    fmt_num(buf, sizeof buf, hi, c->unit);
    lv_obj_t *hl = bz_label_line(card, buf, BZ_F_CAPTION, BZ_C_DIM, MD_YW);
    lv_obj_set_pos(hl, 0, y + 2);
    fmt_num(buf, sizeof buf, lo, c->unit);
    lv_obj_t *ll = bz_label_line(card, buf, BZ_F_CAPTION, BZ_C_DIM, MD_YW);
    lv_obj_set_pos(ll, 0, y + MD_CHART_H - 14);

    lv_obj_t *ch = lv_chart_create(card);
    lv_obj_remove_style_all(ch);
    lv_obj_set_pos(ch, MD_YW, y);
    lv_obj_set_size(ch, in_w - MD_YW, MD_CHART_H);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_line_color(ch, bz_lv(BZ_C_FAINT), LV_PART_MAIN); /* the division lines */
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_size(ch, c->kind == 's' ? 6 : 4, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 3, LV_PART_ITEMS);
    passthrough(ch);
    lv_chart_set_type(ch, c->kind == 'b'   ? LV_CHART_TYPE_BAR
                          : c->kind == 's' ? LV_CHART_TYPE_SCATTER
                                           : LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_chart_set_point_count(ch, (uint32_t)c->ny[0]);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);

    static const bz_color_role_t SER[MD_MAXSER] = { BZ_C_ICE, BZ_C_OK, BZ_C_AMBER, BZ_C_WARN };
    float xlo = 0, xhi = 1;
    if (c->kind == 's' && c->nx) {
        xlo = xhi = c->x[0];
        for (int i = 0; i < c->nx; i++) {
            if (c->x[i] < xlo) xlo = c->x[i];
            if (c->x[i] > xhi) xhi = c->x[i];
        }
        if (xhi <= xlo) xhi = xlo + 1;
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_X, 0, 1000);
    }
    for (int s = 0; s < c->nser; s++) {
        lv_chart_series_t *ser = lv_chart_add_series(ch, bz_lv(SER[s]), LV_CHART_AXIS_PRIMARY_Y);
        if (!ser) break;
        for (int i = 0; i < c->ny[s]; i++) {
            int32_t v = (int32_t)((c->y[s][i] - lo) / (hi - lo) * 1000.0f + 0.5f);
            if (v < 0) v = 0;
            if (v > 1000) v = 1000;
            if (c->kind == 's') {
                int32_t xv = (int32_t)((c->x[i] - xlo) / (xhi - xlo) * 1000.0f + 0.5f);
                lv_chart_set_series_value_by_id2(ch, ser, (uint32_t)i, xv, v);
            } else {
                lv_chart_set_series_value_by_id(ch, ser, (uint32_t)i, v);
            }
        }
    }

    y += MD_CHART_H + 4;
    /* what runs along the bottom: the given labels, or the first and last x, or just the count */
    char foot[128];
    if (c->labels[0]) {
        snprintf(foot, sizeof foot, "%s", c->labels);
    } else if (c->nx >= 2) {
        char a[24], b[24];
        fmt_num(a, sizeof a, c->x[0], "");
        fmt_num(b, sizeof b, c->x[c->nx - 1], "");
        snprintf(foot, sizeof foot, "%s to %s", a, b);
    } else {
        snprintf(foot, sizeof foot, "%d points", c->ny[0]);
    }
    if (c->clipped) {
        size_t l = strlen(foot);
        snprintf(foot + l, sizeof foot - l, " · cut at %d, the most this draws", MD_MAXPTS);
    }
    lv_obj_t *fl = bz_label_line(card, foot, BZ_F_CAPTION, BZ_C_DIM, in_w - MD_YW);
    lv_obj_set_pos(fl, MD_YW, y);
    y += 20;

    if (c->nser > 1) {
        /* Measured, not counted. Stepping x by 8 px a character assumed a narrower face than the caption
         * actually is (Sans Code 13 advances 9.6), so "commanded" ran 14 px under the next series' dot and
         * the legend read "commande*actual". */
        const lv_font_t *cf = bz_font(BZ_F_CAPTION);
        int x = MD_YW;
        for (int s = 0; s < c->nser; s++) {
            lv_obj_t *dot = lv_obj_create(card);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 10, 10);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, bz_lv(SER[s]), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_pos(dot, x, y + 5);
            const char *nm = c->name[s][0] ? c->name[s] : "series";
            int nw = text_w(nm, cf);
            lv_obj_t *nl = bz_label_line(card, nm, BZ_F_CAPTION, BZ_C_DIM, nw + 2);
            lv_obj_set_pos(nl, x + 16, y);
            x += 16 + 10 + nw;
            if (x > in_w - 60) break;
        }
        y += 20;
    } else if (c->name[0][0]) {
        lv_obj_t *nl = bz_label_line(card, c->name[0], BZ_F_CAPTION, BZ_C_DIM, in_w - MD_YW);
        lv_obj_set_pos(nl, MD_YW, y);
        y += 20;
    }
    lv_obj_set_height(card, y + 28);
    lv_free(c);
    return card;
}

/* ================================================================== the walk */

static void column(lv_obj_t *p, int w)
{
    lv_obj_clean(p);
    lv_obj_set_width(p, w);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_layout(p, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, MD_GAP, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
}

int ui_md_render(lv_obj_t *parent, const char *md, int width)
{
    if (!parent) return 0;
    column(parent, width);
    if (!md || !md[0]) return 0;

    const char *p = md;
    while (*p) {
        size_t l = line_len(p);
        const char *line = p;
        p += l;
        if (*p == '\n') p++;

        /* a fence runs to the closing fence, or to the end when the model never closed it */
        if (l >= 3 && line_is(line, "```")) {
            char lang[16] = "";
            size_t ln = l - 3 < sizeof lang - 1 ? l - 3 : sizeof lang - 1;
            memcpy(lang, line + 3, ln);
            lang[ln] = 0;
            trim_end(lang);
            const char *start = p, *end = NULL;
            while (*p) {
                size_t k = line_len(p);
                if (k >= 3 && line_is(p, "```")) {
                    end = p;
                    p += k;
                    if (*p == '\n') p++;
                    break;
                }
                p += k;
                if (*p == '\n') p++;
            }
            if (!end) end = p;
            size_t blen = (size_t)(end - start);
            char *block = lv_malloc(blen + 1);
            if (!block) continue;
            memcpy(block, start, blen);
            block[blen] = 0;
            char why[160] = "";
            if (!strcmp(lang, "chart")) {
                if (!ui_md_chart(parent, block, width, why, sizeof why)) bad_block(parent, why, block, width);
            } else if (!strcmp(lang, "svg")) {
                lv_obj_t *img = ui_md_svg(parent, block, width, 360, why, sizeof why);
                if (!img) bad_block(parent, why[0] ? why : "svg: nothing could be drawn", block, width);
                else if (why[0]) {
                    lv_obj_t *n = bz_label(parent, why, BZ_F_CAPTION, BZ_C_WARN);
                    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(n, width);
                }
            } else {
                code_card(parent, block, width, BZ_C_INK);
            }
            lv_free(block);
            continue;
        }

        /* strip the trailing spaces markdown uses for a hard break; they are noise here */
        while (l && (line[l - 1] == ' ' || line[l - 1] == '\r')) l--;
        if (!l) continue;
        if (is_rule(line, l)) {
            rule(parent, width);
            continue;
        }
        if (line[0] == '#') {
            int lvl = 0;
            while ((size_t)lvl < l && line[lvl] == '#' && lvl < 6) lvl++;
            size_t s = (size_t)lvl;
            while (s < l && line[s] == ' ') s++;
            if (s > (size_t)lvl) {
                heading(parent, line + s, l - s, width, lvl);
                continue;
            }
        }
        if (line[0] == '>') {
            size_t s = 1;
            while (s < l && line[s] == ' ') s++;
            quote(parent, line + s, l - s, width);
            continue;
        }
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && l > 1 && line[1] == ' ') {
            /* an em dash, not a bullet: the body face carries no • */
            bullet(parent, "\xe2\x80\x94", line + 2, l - 2, width);
            continue;
        }
        if (line[0] >= '0' && line[0] <= '9') {
            size_t d = 0;
            while (d < l && line[d] >= '0' && line[d] <= '9') d++;
            if (d < l && (line[d] == '.' || line[d] == ')') && d + 1 < l && line[d + 1] == ' ') {
                char mark[8];
                snprintf(mark, sizeof mark, "%.*s.", (int)d, line);
                bullet(parent, mark, line + d + 2, l - d - 2, width);
                continue;
            }
        }
        /* an ordinary paragraph: gather the lines that follow until a blank one or a new block */
        const char *para = line;
        size_t plen = l;
        while (*p) {
            size_t k = line_len(p);
            while (k && (p[k - 1] == ' ' || p[k - 1] == '\r')) k--;
            if (!k) break;
            if (p[0] == '#' || p[0] == '>' || line_is(p, "```") || is_rule(p, k)) break;
            if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') break;
            if (p[0] >= '1' && p[0] <= '9' && (p[1] == '.' || p[1] == ')') && p[2] == ' ') break;
            plen = (size_t)(p - para) + k;
            p += line_len(p);
            if (*p == '\n') p++;
        }
        rich(parent, para, plen, width, BZ_F_BODY, BZ_C_INK);
    }
    lv_obj_update_layout(parent);
    return lv_obj_get_height(parent);
}

