/* Systemcore and Catalyst depth: systemcore (the controller through catalyst-agent), motors (every motor's
 * lifetime by serial number), states (mechanism state timelines), controls (the robot's own bindings
 * manifest) and recorder (a black box for test runs). The data side is cat_sc.h; docs/systemcore.md is
 * the contract. */
#include "ui_internal.h"
#include "cat_sc.h"
#include "link.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define CW (W - 2 * PAD)
#define COL3 ((CW - 2 * BZ_GAP) / 3)
#define COL2 (2 * COL3 + BZ_GAP)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)
#define DASH "\xe2\x80\x94"
#define MID "\xc2\xb7"
#define ARROW "\xe2\x86\x92"

static lv_obj_t *head_right(lv_obj_t *body)
{
    lv_obj_t *r = bz_row(body, 12);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -PAD, 18);
    lv_obj_set_height(r, 60);
    return r;
}

static uint32_t hash_str(uint32_t h, const char *s)
{
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ the data side's environment */

static int sc_http_get(const char *url, int timeout_ms, char *out, int max, int *len)
{
    hal_http_req_t req = { .url = url, .timeout_ms = timeout_ms };
    return hal_http_fetch(&req, out, max, len);
}

/* Starts the state recorder. Meant for boot (ui_init, after cat_model_init) so lanes cover the whole
 * session; the apps below also call it on first build, so nothing breaks while it isn't wired there. */
void ui_sc_boot(void)
{
    static const cat_sc_env_t env = { .http_get = sc_http_get, .spawn = hal_thread, .sd_root = hal_sd_root,
                                      .wall = hal_rtc_get };
    cat_sc_init(cat_nt(), &env);
}

/* ------------------------------------------------------------------ formatting */

/* Binary units, because that is what the kernel counted (Console core-format.js bytes()). */
static const char *fmt_bytes(char *b, size_t n, double v)
{
    if (!(v == v) || v < 0) return snprintf(b, n, DASH), b;
    static const char *const U[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    snprintf(b, n, i && v < 10 ? "%.1f %s" : "%.0f %s", v, U[i]);
    return b;
}

static const char *fmt_dur(char *b, size_t n, double s)
{
    if (!(s == s) || s < 0) snprintf(b, n, DASH);
    else if (s < 60) snprintf(b, n, "%.0f s", s);
    else if (s < 3600) snprintf(b, n, "%d min %02d s", (int)s / 60, (int)s % 60);
    else if (s < 86400) snprintf(b, n, "%d h %02d min", (int)s / 3600, (int)s / 60 % 60);
    else snprintf(b, n, "%d d %d h", (int)(s / 86400), (int)s / 3600 % 24);
    return b;
}

/* Console's hoursText: minutes when there are not many. */
static const char *fmt_hours(char *b, size_t n, double s)
{
    if (!(s > 0)) snprintf(b, n, "0");
    else if (s < 3600) snprintf(b, n, "%.0f min", s / 60);
    else snprintf(b, n, "%.1f h", s / 3600);
    return b;
}

static const char *fmt_revs(char *b, size_t n, double r)
{
    if (!(r > 0)) snprintf(b, n, "0");
    else if (r >= 1e6) snprintf(b, n, "%.2f M", r / 1e6);
    else if (r >= 1e3) snprintf(b, n, "%.1f k", r / 1e3);
    else snprintf(b, n, "%.0f", r);
    return b;
}

static const char *fmt_count(char *b, size_t n, double v)
{
    if (!(v == v)) snprintf(b, n, DASH);
    else if (v >= 1e9) snprintf(b, n, "%.2f G", v / 1e9);
    else if (v >= 1e6) snprintf(b, n, "%.1f M", v / 1e6);
    else if (v >= 1e4) snprintf(b, n, "%.0f k", v / 1e3);
    else snprintf(b, n, "%.0f", v);
    return b;
}

static const char *fmt_date(char *b, size_t n, double ms, bool trusted)
{
    if (!(ms > 0)) return snprintf(b, n, DASH), b;
    if (!trusted) return snprintf(b, n, "relative"), b;
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    static const char *const M[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    snprintf(b, n, "%s %d %d", M[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
    return b;
}

/* Console core-format.js level(): ok below warn, warn below crit. Absent is stale, never ok. */
static bz_status_t level(double v, double warn, double crit)
{
    if (!(v == v)) return BZ_STALE;
    return v >= crit ? BZ_FAULT : v >= warn ? BZ_WARN : BZ_OK;
}

static bz_color_role_t level_fill(double v, double warn, double crit)
{
    bz_status_t s = level(v, warn, crit);
    return s == BZ_FAULT ? BZ_C_FAULT : s == BZ_WARN ? BZ_C_WARN : BZ_C_ICE;
}

static double ntnum(const char *topic)
{
    double v;
    return cat_nt() && nt4_get_number(cat_nt(), topic, &v) ? v : NAN;
}

/* ------------------------------------------------------------------ springy bars
 *
 * bz_meter's look, but the fill glides to each new value on `smooth` instead of jumping: the agent
 * answers every 3 s, and a bar that snaps reads as a glitch where one that moves reads as a reading. */

typedef struct {
    lv_obj_t *m;
    bz_motion_t v;
    bz_color_role_t c;
} sbar_t;

#define MAX_SBARS 64
static sbar_t g_sbars[MAX_SBARS];
static int g_nsbars;

static sbar_t *sbar(lv_obj_t *parent, int w, int h)
{
    if (g_nsbars >= MAX_SBARS) return NULL;
    sbar_t *b = &g_sbars[g_nsbars++];
    b->m = bz_meter(parent, w, h);
    b->c = BZ_C_ICE;
    bz_motion_init(&b->v, 0, 0.001f);
    return b;
}

static void sbar_place(sbar_t *b)
{
    lv_obj_t *f = lv_obj_get_child(b->m, 0);
    int w = lv_obj_get_style_width(b->m, 0), h = lv_obj_get_style_height(b->m, 0);
    float v = b->v.value;
    if (v > 1) v = 1;
    int fw = v <= 0.002f ? 0 : h + (int)((w - h) * v);
    if (lv_obj_get_style_width(f, 0) != fw) lv_obj_set_width(f, fw);
}

static void sbar_set(sbar_t *b, double v, bz_color_role_t c)
{
    if (!b) return;
    if (!(v == v) || v < 0) v = 0;
    if (c != b->c) {
        b->c = c;
        bz_meter_set(b->m, b->v.value, c);
        sbar_place(b);
    }
    if (fabsf(b->v.target - (float)v) > 0.002f) bz_motion_to(&b->v, (float)v, BZ_SMOOTH);
}

static void sbars_frame(void)
{
    for (int i = 0; i < g_nsbars; i++)
        if (bz_motion_tick(&g_sbars[i].v)) {
            sbar_place(&g_sbars[i]);
            bz_ui_keep_alive();
        }
}

/* ------------------------------------------------------------------ small building blocks */

typedef struct { lv_obj_t *k, *v; } kv_t;

static void kv_rows(lv_obj_t *parent, kv_t *rows, int n, int w, int kw)
{
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = bz_row(parent, 10);
        lv_obj_set_width(r, w);
        rows[i].k = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, kw);
        lv_obj_set_width(rows[i].k, kw);
        rows[i].v = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, w - kw - 10);
    }
}

static void kv(kv_t *row, const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void kv(kv_t *row, const char *k, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_text(row->k, "%s", k);
    ui_text(row->v, "%s", buf[0] ? buf : DASH);
}

/* A big value with its unit beside it, baseline-aligned. */
typedef struct { lv_obj_t *row, *v, *u; } big_t;

static void big(lv_obj_t *parent, big_t *b, bz_font_role_t f, const char *unit)
{
    b->row = bz_row(parent, 6);
    lv_obj_set_flex_align(b->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    b->v = bz_label(b->row, DASH, f, BZ_C_INK);
    /* the unit a step down from the value; under a name-sized value it is body text */
    b->u = bz_label(b->row, unit, f == BZ_F_NAME ? BZ_F_BODY_S : BZ_F_NAME, BZ_C_DIM);
    lv_obj_set_style_pad_bottom(b->u, f == BZ_F_DISPLAY ? 14 : f == BZ_F_VALUE ? 8 : 3, 0);
}

/* A tile's head: the mono label left, an optional mark and note right. */
static lv_obj_t *tile_head(lv_obj_t *tile, const char *label, lv_obj_t **mark, lv_obj_t **note)
{
    lv_obj_t *l = bz_label(tile, label, BZ_F_LABEL, BZ_C_DIM);
    if (mark || note) {
        lv_obj_t *r = bz_row(tile, 8);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, 0);
        if (mark) *mark = bz_mark(r, BZ_STALE, 10);
        if (note) *note = bz_label(r, "", BZ_F_CAPTION, BZ_C_DIM);
    }
    return l;
}

static lv_obj_t *empty_tile(lv_obj_t *parent, int w, int h, const char *title, const char *text)
{
    lv_obj_t *t = bz_tile(parent, w, h);
    bz_label(t, title, BZ_F_NAME, BZ_C_INK);
    lv_obj_t *l = bz_label(t, text, BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(l, IN(w));
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return t;
}

/* ui_text holds 256 bytes; lists and paragraphs can be longer. Sets only on change, like ui_text. */
static void set_long(lv_obj_t *label, const char *text)
{
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text)) lv_label_set_text(label, text);
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ one frame hook for all five */

static void rc_glass_frame(double now);

static void sc_frame_hook(double now, double dt, void *user)
{
    (void)dt; (void)user;
    sbars_frame();
    rc_glass_frame(now);
}

static void sc_common(void)
{
    static bool done;
    if (done) return;
    done = true;
    ui_sc_boot();
    bz_ui_on_frame(sc_frame_hook, NULL);
}

/* ================================================================== systemcore */

#define SC_H 320
#define SC_CORES 4
#define SC_PROCS 6
#define SC_NICS 5
#define SC_CAMS 4
#define SC_LOG 13

static const char *const BUSES[5] = { "can_s0", "can_s1", "can_s2", "can_s3", "can_s4" };

static struct {
    cat_agent_t *a;
    uint32_t rev;
    lv_obj_t *head_mark, *head, *grid;
    lv_obj_t *install, *install_text;
    /* processor */
    big_t cpu;
    lv_obj_t *cpu_sub, *core_row[SC_CORES], *core_name[SC_CORES], *core_val[SC_CORES], *core_mhz[SC_CORES], *cpu_foot, *cpu_spark;
    double cpu_pushed;
    sbar_t *core_bar[SC_CORES];
    /* temperature and memory */
    big_t temp, mem;
    lv_obj_t *temp_mark, *temp_note, *zones, *mem_sub;
    sbar_t *mem_bar;
    /* robot program */
    lv_obj_t *t_prog, *prog_mark, *prog_note, *prog_state, *prog_last;
    kv_t prog[4];
    /* machine */
    lv_obj_t *t_mach;
    kv_t mach[8];
    /* can */
    lv_obj_t *can_note, *pair[3];
    struct { lv_obj_t *name, *pct, *rec, *tec, *off, *frames, *errs; sbar_t *bar; } bus[5];
    /* processes */
    lv_obj_t *t_procs, *procs_note;
    struct { lv_obj_t *row, *name, *cpu, *rss; } proc[SC_PROCS];
    /* storage */
    big_t disk;
    lv_obj_t *disk_mark, *disk_note, *disk_sub, *mounts, *dirs, *emmc;
    sbar_t *disk_bar;
    /* network */
    lv_obj_t *net_note, *net_foot, *net_empty;
    struct { lv_obj_t *row, *mark, *name, *addr, *sub; } nic[SC_NICS];
    /* cameras */
    lv_obj_t *t_cams, *cams_note, *cams_empty;
    struct { lv_obj_t *row, *mark, *name, *sub, *val; } cam[SC_CAMS];
    /* log */
    lv_obj_t *t_log, *log_note, *log[SC_LOG];
    bool agent_layout;
} SC;

static lv_obj_t *sc_tile(int w, const char *label, lv_obj_t **mark, lv_obj_t **note)
{
    lv_obj_t *t = bz_tile(SC.grid, w, SC_H);
    tile_head(t, label, mark, note);
    return t;
}

static void sc_build_cpu(void)
{
    lv_obj_t *t = sc_tile(COL3, "processor", NULL, NULL);
    big(t, &SC.cpu, BZ_F_VALUE, "%");
    lv_obj_set_pos(SC.cpu.row, 0, 20);
    SC.cpu_sub = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(SC.cpu_sub, LV_ALIGN_TOP_RIGHT, 0, 52);
    for (int i = 0; i < SC_CORES; i++) {
        lv_obj_t *r = bz_row(t, 10);
        lv_obj_set_width(r, IN(COL3));
        lv_obj_set_pos(r, 0, 90 + i * 30);
        SC.core_row[i] = r;
        SC.core_name[i] = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
        lv_obj_set_width(SC.core_name[i], 30);
        SC.core_bar[i] = sbar(r, IN(COL3) - 30 - 52 - 84 - 30, 10);
        SC.core_val[i] = bz_label(r, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(SC.core_val[i], 52);
        lv_obj_set_style_text_align(SC.core_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        SC.core_mhz[i] = bz_label(r, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(SC.core_mhz[i], 84);
        lv_obj_set_style_text_align(SC.core_mhz[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    /* with only the summary, the room the cores would take shows the last two minutes of it */
    SC.cpu_spark = bz_spark(t, IN(COL3), 80, 240);
    bz_spark_range(SC.cpu_spark, 0, 100);
    bz_spark_color(SC.cpu_spark, BZ_C_ICE);
    lv_obj_set_pos(SC.cpu_spark, 0, 128);
    SC.cpu_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.cpu_foot, IN(COL3));
    lv_obj_align(SC.cpu_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_temp(void)
{
    lv_obj_t *t = sc_tile(COL3, "temperature", &SC.temp_mark, &SC.temp_note);
    big(t, &SC.temp, BZ_F_VALUE, "\xc2\xb0""c");
    lv_obj_set_pos(SC.temp.row, 0, 20);
    SC.zones = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.zones, IN(COL3));
    lv_obj_set_pos(SC.zones, 0, 84);
    lv_obj_t *ml = bz_label(t, "memory", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(ml, 0, 130);
    big(t, &SC.mem, BZ_F_NAME, "");
    lv_obj_set_pos(SC.mem.row, 0, 152);
    SC.mem_bar = sbar(t, IN(COL3), 10);
    lv_obj_set_pos(SC.mem_bar->m, 0, 196);
    SC.mem_sub = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.mem_sub, IN(COL3));
    lv_obj_align(SC.mem_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_program(void)
{
    SC.t_prog = sc_tile(COL3, "robot program", &SC.prog_mark, &SC.prog_note);
    SC.prog_state = bz_label(SC.t_prog, DASH, BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(SC.prog_state, 0, 20);
    SC.prog_last = bz_label(SC.t_prog, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.prog_last, IN(COL3));
    lv_obj_set_style_text_line_space(SC.prog_last, 3, 0);
    lv_obj_set_pos(SC.prog_last, 0, 90);
    lv_obj_t *c = bz_col(SC.t_prog, 8);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    kv_rows(c, SC.prog, 4, IN(COL3), 110);
}

static void sc_build_machine(void)
{
    SC.t_mach = sc_tile(COL3, "machine", NULL, NULL);
    lv_obj_t *c = bz_col(SC.t_mach, 7);
    lv_obj_set_pos(c, 0, 30);
    kv_rows(c, SC.mach, 8, IN(COL3), 110);
}

static void sc_build_can(void)
{
    lv_obj_t *t = sc_tile(COL2, "can buses", NULL, NULL);
    /* column heads, over the columns they name */
    static const struct { const char *s; int x, w; } HEAD[] = {
        { "rec", 390, 56 }, { "tec", 450, 56 }, { "bus-off", 510, 70 }, { "frames", 584, 84 }, { "errors", 672, 88 },
    };
    for (size_t i = 0; i < sizeof HEAD / sizeof HEAD[0]; i++) {
        lv_obj_t *h = bz_label(t, HEAD[i].s, BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(h, HEAD[i].w);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(h, HEAD[i].x, 3);
    }
    /* Systemcore's five buses on three SPI hosts (Console docs/can-buses.md): s0+s1, s2, s3+s4 */
    static const int GROUP_OF[5] = { 0, 0, 1, 2, 2 };
    int y = 30;
    for (int b = 0; b < 5; b++) {
        if (b == 0 || GROUP_OF[b] != GROUP_OF[b - 1]) {
            SC.pair[GROUP_OF[b]] = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
            lv_obj_set_pos(SC.pair[GROUP_OF[b]], 0, y + 4);
            y += 24;
        }
        lv_obj_t *r = bz_row(t, 0);
        lv_obj_set_pos(r, 0, y);
        lv_obj_set_size(r, IN(COL2), 28);
        SC.bus[b].name = bz_label(r, BUSES[b], BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(SC.bus[b].name, 86);
        SC.bus[b].bar = sbar(r, 214, 10);
        SC.bus[b].pct = bz_label(r, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(SC.bus[b].pct, 70);
        lv_obj_set_style_text_align(SC.bus[b].pct, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t **cells[5] = { &SC.bus[b].rec, &SC.bus[b].tec, &SC.bus[b].off, &SC.bus[b].frames, &SC.bus[b].errs };
        for (int k = 0; k < 5; k++) {
            *cells[k] = bz_label(r, "", k < 3 ? BZ_F_LABEL : BZ_F_BODY_S, BZ_C_INK);
            lv_obj_set_width(*cells[k], HEAD[k].w);
            lv_obj_set_style_text_align(*cells[k], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(*cells[k], HEAD[k].x, 0);
            lv_obj_add_flag(*cells[k], LV_OBJ_FLAG_IGNORE_LAYOUT);
        }
        y += 30;
    }
    SC.can_note = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.can_note, IN(COL2));
    lv_obj_align(SC.can_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_procs(void)
{
    SC.t_procs = sc_tile(COL3, "top processes", NULL, &SC.procs_note);
    for (int i = 0; i < SC_PROCS; i++) {
        lv_obj_t *r = bz_row(SC.t_procs, 8);
        lv_obj_set_width(r, IN(COL3));
        lv_obj_set_pos(r, 0, 34 + i * 36);
        SC.proc[i].row = r;
        SC.proc[i].name = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_INK, IN(COL3) - 70 - 80 - 16);
        lv_obj_set_flex_grow(SC.proc[i].name, 1);
        SC.proc[i].rss = bz_label(r, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(SC.proc[i].rss, 80);
        lv_obj_set_style_text_align(SC.proc[i].rss, LV_TEXT_ALIGN_RIGHT, 0);
        SC.proc[i].cpu = bz_label(r, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(SC.proc[i].cpu, 70);
        lv_obj_set_style_text_align(SC.proc[i].cpu, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void sc_build_storage(void)
{
    lv_obj_t *t = sc_tile(COL3, "storage", &SC.disk_mark, &SC.disk_note);
    big(t, &SC.disk, BZ_F_VALUE, "%");
    lv_obj_set_pos(SC.disk.row, 0, 20);
    SC.disk_sub = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(SC.disk_sub, LV_ALIGN_TOP_RIGHT, 0, 52);
    SC.disk_bar = sbar(t, IN(COL3), 10);
    lv_obj_set_pos(SC.disk_bar->m, 0, 86);
    SC.mounts = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.mounts, IN(COL3));
    lv_obj_set_style_text_line_space(SC.mounts, 4, 0);
    lv_obj_set_pos(SC.mounts, 0, 108);
    SC.dirs = bz_label(t, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_width(SC.dirs, IN(COL3));
    lv_obj_set_style_text_line_space(SC.dirs, 4, 0);
    lv_obj_set_pos(SC.dirs, 0, 154);
    SC.emmc = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.emmc, IN(COL3));
    lv_obj_align(SC.emmc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_network(void)
{
    lv_obj_t *t = sc_tile(COL3, "network", NULL, &SC.net_note);
    for (int i = 0; i < SC_NICS; i++) {
        lv_obj_t *r = bz_row(t, 10);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_width(r, IN(COL3));
        lv_obj_set_pos(r, 0, 32 + i * 42);
        SC.nic[i].row = r;
        SC.nic[i].mark = bz_mark(r, BZ_STALE, 10);
        lv_obj_set_style_margin_top(SC.nic[i].mark, 6, 0);
        SC.nic[i].name = bz_label(r, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(SC.nic[i].name, 64);
        lv_obj_t *c = bz_col(r, 0);
        SC.nic[i].addr = bz_label_line(c, "", BZ_F_BODY_S, BZ_C_INK, IN(COL3) - 94);
        SC.nic[i].sub = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_DIM, IN(COL3) - 94);
    }
    SC.net_empty = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.net_empty, IN(COL3));
    lv_obj_set_pos(SC.net_empty, 0, 34);
    SC.net_foot = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.net_foot, IN(COL3));
    lv_obj_align(SC.net_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_cameras(void)
{
    SC.t_cams = sc_tile(COL3, "cameras", NULL, &SC.cams_note);
    for (int i = 0; i < SC_CAMS; i++) {
        lv_obj_t *r = bz_row(SC.t_cams, 10);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_width(r, IN(COL3));
        lv_obj_set_pos(r, 0, 34 + i * 60);
        SC.cam[i].row = r;
        SC.cam[i].mark = bz_mark(r, BZ_STALE, 10);
        lv_obj_set_style_margin_top(SC.cam[i].mark, 7, 0);
        lv_obj_t *c = bz_col(r, 2);
        SC.cam[i].name = bz_label_line(c, "", BZ_F_BODY_S, BZ_C_INK, IN(COL3) - 20);
        SC.cam[i].val = bz_label_line(c, "", BZ_F_LABEL, BZ_C_INK, IN(COL3) - 20);
        SC.cam[i].sub = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_DIM, IN(COL3) - 20);
    }
    SC.cams_empty = bz_label(SC.t_cams, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SC.cams_empty, IN(COL3));
    lv_obj_align(SC.cams_empty, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void sc_build_log(void)
{
    SC.t_log = sc_tile(COL2, "robot program log", NULL, &SC.log_note);
    for (int i = 0; i < SC_LOG; i++) {
        SC.log[i] = bz_label_line(SC.t_log, "", BZ_F_CAPTION, BZ_C_INK, IN(COL2));
        lv_obj_set_pos(SC.log[i], 0, 30 + i * 17);
    }
}

static void sc_build(lv_obj_t *b)
{
    sc_common();
    SC.a = calloc(1, sizeof *SC.a);
    lv_obj_t *r = head_right(b);
    SC.head_mark = bz_mark(r, BZ_STALE, 10);
    SC.head = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);

    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    lv_obj_t *scroll = ui_scroller(wrap, CW, APP_H);
    SC.grid = bz_row(scroll, BZ_GAP);
    lv_obj_set_flex_flow(SC.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(SC.grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(SC.grid, BZ_GAP, 0);
    lv_obj_set_width(SC.grid, CW);

    /* One line on how to get the detail, when the agent isn't answering. */
    SC.install = bz_tile(SC.grid, CW, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(SC.install, 18, 0);
    lv_obj_set_flex_flow(SC.install, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(SC.install, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(SC.install, 16, 0);
    bz_icon(SC.install, BZ_I_DOWNLOADING, 24, BZ_C_DIM);
    SC.install_text = bz_label(SC.install, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_flex_grow(SC.install_text, 1);
    lv_label_set_long_mode(SC.install_text, LV_LABEL_LONG_WRAP);

    sc_build_cpu();
    sc_build_temp();
    sc_build_program();
    sc_build_can();
    sc_build_procs();
    sc_build_storage();
    sc_build_network();
    sc_build_cameras();
    sc_build_log();
    sc_build_machine();
    SC.agent_layout = true;
}

static void sc_cpu(const cat_agent_t *a, bool agent)
{
    char b[24];
    show(SC.cpu_spark, !(agent && a->ncores));
    if (R->have_sc && ui_now() - SC.cpu_pushed >= 0.5) {
        SC.cpu_pushed = ui_now();
        bz_spark_push(SC.cpu_spark, (float)R->sc_cpu);
    }
    if (agent && a->ncores) {
        int busiest = 0;
        for (int i = 1; i < a->ncores; i++)
            if (a->cores[i].percent > a->cores[busiest].percent) busiest = i;
        ui_text(SC.cpu.v, "%s", bz_fmt(b, sizeof b, true, "%.0f", a->cores[busiest].percent));
        ui_text(SC.cpu_sub, "busiest " MID " core %d", a->cores[busiest].core);
        for (int i = 0; i < SC_CORES; i++) {
            show(SC.core_row[i], i < a->ncores);
            if (i >= a->ncores) continue;
            const cat_ag_core_t *c = &a->cores[i];
            ui_text(SC.core_name[i], "c%d", c->core);
            ui_text(SC.core_val[i], "%s", bz_fmt(b, sizeof b, true, "%.0f%%", c->percent));
            ui_text(SC.core_mhz[i], "%s", bz_fmt(b, sizeof b, true, "%.0f mhz", c->mhz));
            sbar_set(SC.core_bar[i], c->percent / 100, level_fill(c->percent, 85, 95));
        }
        char foot[160];
        size_t o = 0;
        foot[0] = 0;
        if (a->have_load) o += (size_t)snprintf(foot, sizeof foot, "load %.2f " MID " %.2f " MID " %.2f", a->load[0], a->load[1], a->load[2]);
        /* now and since-boot are different facts: a robot that throttled last match still carries the bit */
        const char *thr = a->thr_now ? "throttling now" : a->thr_under_now ? "under-voltage now"
                        : a->thr_since ? "throttled earlier this boot" : a->thr_under_since ? "under-voltage earlier this boot" : NULL;
        if (a->have_throttle && thr) snprintf(foot + o, sizeof foot - o, "%s%s", o ? "\n" : "", thr);
        ui_text(SC.cpu_foot, "%s", foot);
        bz_set_color(SC.cpu_foot, a->have_throttle && (a->thr_now || a->thr_under_now) ? BZ_C_FAULT
                                  : a->have_throttle && (a->thr_since || a->thr_under_since) ? BZ_C_WARN : BZ_C_DIM);
        return;
    }
    /* the NT summary: one figure for all the cores */
    const cat_robot_t *r = R;
    ui_text(SC.cpu.v, "%s", bz_fmt(b, sizeof b, r->have_sc, "%.0f", r->sc_cpu));
    ui_text(SC.cpu_sub, "all cores");
    show(SC.core_row[0], true);
    for (int i = 1; i < SC_CORES; i++) show(SC.core_row[i], false);
    ui_text(SC.core_name[0], "all");
    ui_text(SC.core_val[0], "%s", bz_fmt(b, sizeof b, r->have_sc, "%.0f%%", r->sc_cpu));
    ui_text(SC.core_mhz[0], " ");
    sbar_set(SC.core_bar[0], r->have_sc ? r->sc_cpu / 100 : 0, level_fill(r->have_sc ? r->sc_cpu : NAN, 85, 95));
    ui_text(SC.cpu_foot, "per core, clocks and throttling come from catalyst-agent");
    bz_set_color(SC.cpu_foot, BZ_C_DIM);
}

static void sc_temp_mem(const cat_agent_t *a, bool agent)
{
    char b[32], c[32];
    double temp = R->sc_temp;
    if (agent && a->nzones) {
        /* the hottest zone the kernel names; Console reads the same sensor off NT */
        temp = a->zones[0].celsius;
        for (int i = 1; i < a->nzones; i++) if (a->zones[i].celsius > temp) temp = a->zones[i].celsius;
    }
    ui_text(SC.temp.v, "%s", bz_fmt(b, sizeof b, temp == temp, "%.0f", temp));
    /* The CM5 throttles rather than saying so, so a hot controller shows up as loop overruns. 80 / 90 are
     * below where throttling starts (Console app.js paintCore). */
    bz_status_t ts = level(temp, 80, 90);
    bz_mark_set(SC.temp_mark, ts);
    ui_text(SC.temp_note, "%s", ts == BZ_FAULT ? "hot" : ts == BZ_WARN ? "warm" : ts == BZ_OK ? "ok" : "");
    if (agent && a->nzones) {
        char z[160];
        size_t o = 0;
        z[0] = 0;
        for (int i = 0; i < a->nzones && o < sizeof z - 40; i++)
            o += (size_t)snprintf(z + o, sizeof z - o, "%s%s %.0f\xc2\xb0", i ? "  " MID "  " : "", a->zones[i].zone, a->zones[i].celsius);
        ui_text(SC.zones, "%s", z);
    } else {
        ui_text(SC.zones, "%s", R->have_sc ? "the controller's own sensor" : "");
    }

    double used = NAN, total = NAN, frac = R->sc_ram;
    if (agent && a->mem_total == a->mem_total) {
        used = a->mem_used;
        total = a->mem_total;
        frac = used / total;
    } else {
        used = ntnum("/Catalyst/Systemcore/RamUsedBytes");
        total = ntnum("/Catalyst/Systemcore/RamTotalBytes");
    }
    ui_text(SC.mem.v, "%s", used == used ? fmt_bytes(b, sizeof b, used) : frac == frac ? bz_fmt(b, sizeof b, true, "%.0f %%", frac * 100) : DASH);
    ui_text(SC.mem.u, "%s", used == used && total == total ? (snprintf(c, sizeof c, "of %s", fmt_bytes(c + 16, 16, total)), c) : "");
    sbar_set(SC.mem_bar, frac, level_fill(frac * 100, 85, 95));
    if (frac == frac) {
        char cached[24];
        if (agent && a->mem_cached == a->mem_cached)
            ui_text(SC.mem_sub, "%.0f %% in use " MID " %s cached, handed back on demand", frac * 100, fmt_bytes(cached, sizeof cached, a->mem_cached));
        else ui_text(SC.mem_sub, "%.0f %% in use", frac * 100);
    } else {
        ui_text(SC.mem_sub, " ");
    }
}

static void sc_program(const cat_agent_t *a)
{
    char b[32];
    bool running = !strcmp(a->state, "active");
    ui_text(SC.prog_state, "%s", a->state[0] ? a->state : DASH);
    bz_set_color(SC.prog_state, running ? BZ_C_INK : BZ_C_FAULT);
    /* restarts are the signal people miss: a program that crashes and comes straight back looks fine
     * from the driver's station, and looks like this from here */
    bool restarted = a->restarts == a->restarts && a->restarts > 0;
    bz_mark_set(SC.prog_mark, !running ? BZ_FAULT : restarted ? BZ_WARN : BZ_OK);
    ui_text(SC.prog_note, "%s", a->substate);
    kv(&SC.prog[0], "restarts", "%s", bz_fmt(b, sizeof b, a->restarts == a->restarts, "%.0f", a->restarts));
    bz_set_color(SC.prog[0].v, restarted ? BZ_C_WARN : BZ_C_INK);
    kv(&SC.prog[1], "running for", "%s", fmt_dur(b, sizeof b, a->running_s));
    kv(&SC.prog[2], "memory", "%s", a->memory == a->memory ? fmt_bytes(b, sizeof b, a->memory) : "");
    kv(&SC.prog[3], "unit", "%s%s%.0d", a->unit, a->pid ? " " MID " pid " : "", a->pid);
    /* what it said last, without the date: the rest is in the log tile */
    char last[300];
    size_t o = 0;
    last[0] = 0;
    for (int i = a->nlog > 2 ? a->nlog - 2 : 0; i < a->nlog; i++) {
        const char *l = a->log[i];
        const char *msg = strstr(l, "]: ");
        o += (size_t)snprintf(last + o, sizeof last - o, "%s%s", o ? "\n" : "", msg ? msg + 3 : l);
    }
    set_long(SC.prog_last, last);
}

static void sc_machine(const cat_agent_t *a, bool agent)
{
    char b[48];
    double team = ntnum("/Catalyst/Systemcore/TeamNumber"), hsub = ntnum("/Catalyst/Systemcore/HardwareSubRev");
    double floor = ntnum("/Catalyst/Systemcore/BrownoutVolts"), rail = ntnum("/Catalyst/Systemcore/Rail3v3Amps");
    int k = 0;
    if (agent) {
        kv(&SC.mach[k++], "model", "%s", strncmp(a->model, "Raspberry Pi ", 13) ? a->model : a->model + 13);
        bool sc_os = !strncmp(a->os, "Systemcore OS ", 14);
        kv(&SC.mach[k++], sc_os ? "systemcore os" : "os", "%s", sc_os ? a->os + 14 : a->os);
        kv(&SC.mach[k++], "kernel", "%s", a->kernel);
        kv(&SC.mach[k++], "uptime", "%s", fmt_dur(b, sizeof b, a->uptime_s));
        kv(&SC.mach[k++], "hostname", "%s", a->hostname);
    }
    /* the team number on the Systemcore itself, which is not always the one in the code */
    kv(&SC.mach[k++], "team", "%s", bz_fmt(b, sizeof b, team == team, "%.0f", team));
    if (!agent) kv(&SC.mach[k++], "hardware rev", "%s", bz_fmt(b, sizeof b, hsub == hsub, "%.0f", hsub));
    kv(&SC.mach[k++], "brownout at", "%s", bz_fmt(b, sizeof b, floor == floor, "%.2f v", floor));
    kv(&SC.mach[k++], "3.3 v rail", "%s", bz_fmt(b, sizeof b, rail == rail, "%.2f a", rail));
    if (!agent) kv(&SC.mach[k++], "agent", "%s", "not answering");
    for (; k < 8; k++) kv(&SC.mach[k], "", "%s", " ");
}

static void sc_can(const cat_agent_t *a, bool agent)
{
    char b[32], p[24];
    const cat_robot_t *r = R;
    double util[5], dropped = 0;
    for (int i = 0; i < 5; i++) {
        /* The OS's figure wins for a Systemcore bus (it covers idle buses too); Phoenix's fills in where
         * the OS has none (Console docs/can-buses.md) */
        util[i] = i < r->ncan_util && r->ncan_util > 1 ? r->can_util[i] : NAN;
        char topic[64];
        snprintf(topic, sizeof topic, "/Catalyst/CAN/Health/%s/Utilization", BUSES[i]);
        if (!(util[i] == util[i])) util[i] = ntnum(topic);
        ui_text(SC.bus[i].pct, "%s", util[i] == util[i] ? (snprintf(p, sizeof p, "%.0f%%", util[i] * 100), p) : DASH);
        /* bars: amber from 70 %, red from 85 % (Console paintCoreCan) */
        sbar_set(SC.bus[i].bar, util[i], level_fill(util[i] * 100, 70, 85));
        bz_set_color(SC.bus[i].name, util[i] == util[i] && util[i] >= 0.01 ? BZ_C_INK : BZ_C_DIM);
        /* the error counters lead failures: 128 is error-passive (CANBusHealth.hasErrorActivity) */
        static const char *const K[3] = { "REC", "TEC", "BusOffCount" };
        lv_obj_t *cells[3] = { SC.bus[i].rec, SC.bus[i].tec, SC.bus[i].off };
        for (int k = 0; k < 3; k++) {
            snprintf(topic, sizeof topic, "/Catalyst/CAN/Health/%s/%s", BUSES[i], K[k]);
            double v = ntnum(topic);
            ui_text(cells[k], "%s", bz_fmt(b, sizeof b, v == v, "%.0f", v));
            bz_set_color(cells[k], k < 2 ? (v >= 128 ? BZ_C_FAULT : v >= 96 ? BZ_C_WARN : BZ_C_INK) : (v > 0 ? BZ_C_WARN : BZ_C_INK));
        }
        /* the agent's frame counters, straight off the interface */
        const cat_ag_can_t *c = NULL;
        for (int k = 0; agent && k < a->ncan; k++) if (!strcmp(a->can[k].name, BUSES[i])) c = &a->can[k];
        if (c) {
            ui_text(SC.bus[i].frames, "%s", fmt_count(b, sizeof b, c->rx + c->tx));
            double errs = (c->rx_err == c->rx_err ? c->rx_err : 0) + (c->tx_err == c->tx_err ? c->tx_err : 0);
            double drop = (c->rx_drop == c->rx_drop ? c->rx_drop : 0) + (c->tx_drop == c->tx_drop ? c->tx_drop : 0);
            ui_text(SC.bus[i].errs, "%.0f", errs);
            bz_set_color(SC.bus[i].errs, errs ? BZ_C_WARN : BZ_C_INK);
            if (drop > 0) dropped += drop;
        } else {
            ui_text(SC.bus[i].frames, DASH);
            ui_text(SC.bus[i].errs, DASH);
            bz_set_color(SC.bus[i].errs, BZ_C_INK);
        }
    }
    /* the shared SPI hosts: a pair's total against 100 % of one controller, and only when both were measured */
    static const char *const PAIR_NAME[3] = { "controller 1 " MID " can_s0 + can_s1 share an spi host", "controller 2 " MID " can_s2 alone",
                                              "controller 3 " MID " can_s3 + can_s4 share an spi host" };
    for (int g = 0; g < 3; g++) {
        if (g == 1) {
            ui_text(SC.pair[g], "%s", PAIR_NAME[g]);
            continue;
        }
        int a0 = g == 0 ? 0 : 3;
        double sum = util[a0] + util[a0 + 1];
        if (sum == sum) ui_text(SC.pair[g], "%s " MID " %.0f%% of the pair", PAIR_NAME[g], sum * 100);
        else ui_text(SC.pair[g], "%s", PAIR_NAME[g]);
        bz_set_color(SC.pair[g], sum >= 1.0 ? BZ_C_FAULT : sum >= 0.8 ? BZ_C_WARN : BZ_C_DIM);
    }
    /* counts since boot, not a live state: a bus that dropped and came back is a different problem */
    double down = ntnum("/Catalyst/Systemcore/CanDownCount"), unav = ntnum("/Catalyst/Systemcore/CanUnavailCount");
    char note[160];
    size_t o = 0;
    note[0] = 0;
    if (r->can_down) o += (size_t)snprintf(note + o, sizeof note - o, "a bus is down right now");
    if (down > 0) o += (size_t)snprintf(note + o, sizeof note - o, "%sdropped %.0f time%s since boot", o ? " " MID " " : "", down, down == 1 ? "" : "s");
    if (unav > 0) o += (size_t)snprintf(note + o, sizeof note - o, "%sunavailable %.0f time%s", o ? " " MID " " : "", unav, unav == 1 ? "" : "s");
    if (dropped > 0) o += (size_t)snprintf(note + o, sizeof note - o, "%s%.0f frame%s dropped at the interface", o ? " " MID " " : "", dropped, dropped == 1 ? "" : "s");
    if (!o) snprintf(note, sizeof note, "%s", agent ? "rec / tec / bus-off from the robot's CANBusHealth " MID " frames and errors from the agent"
                                                     : "rec / tec / bus-off from the robot's CANBusHealth " MID " frame counters need catalyst-agent");
    ui_text(SC.can_note, "%s", note);
    bz_set_color(SC.can_note, r->can_down ? BZ_C_FAULT : down > 0 || unav > 0 || dropped > 0 ? BZ_C_WARN : BZ_C_DIM);
}

static void sc_procs(const cat_agent_t *a)
{
    char b[24];
    ui_text(SC.procs_note, "%d running", a->proc_count);
    for (int i = 0; i < SC_PROCS; i++) {
        show(SC.proc[i].row, i < a->nprocs);
        if (i >= a->nprocs) continue;
        const cat_ag_proc_t *p = &a->procs[i];
        ui_text(SC.proc[i].name, "%s", p->name);
        /* per core: one process can read past 100 % on a four-core machine */
        ui_text(SC.proc[i].cpu, "%s", bz_fmt(b, sizeof b, p->cpu == p->cpu, "%.0f%%", p->cpu));
        bz_set_color(SC.proc[i].cpu, p->cpu >= 95 ? BZ_C_WARN : BZ_C_INK);
        ui_text(SC.proc[i].rss, "%s", fmt_bytes(b, sizeof b, p->rss));
    }
}

static void sc_storage(const cat_agent_t *a, bool agent)
{
    char b[24], c[24], line[256];
    double used = ntnum("/Catalyst/Systemcore/StorageUsedBytes"), total = ntnum("/Catalyst/Systemcore/StorageTotalBytes");
    double frac = R->sc_storage;
    const cat_ag_mount_t *root = NULL;
    for (int i = 0; agent && i < a->nmounts; i++) if (!strcmp(a->mounts[i].mount, "/")) root = &a->mounts[i];
    if (root && root->total > 0) {
        used = root->used;
        total = root->total;
        frac = used / total;
    }
    ui_text(SC.disk.v, "%s", bz_fmt(b, sizeof b, frac == frac, "%.0f", frac * 100));
    /* storage warns earlier than the rest: a full disk stops logging, then the program (85 / 93) */
    bz_status_t s = level(frac * 100, 85, 93);
    bz_mark_set(SC.disk_mark, s);
    ui_text(SC.disk_note, "%s", s == BZ_FAULT ? "nearly full" : s == BZ_WARN ? "filling" : "");
    ui_text(SC.disk_sub, "%s", used == used && total == total ? (snprintf(line, sizeof line, "%s of %s", fmt_bytes(b, sizeof b, used), fmt_bytes(c, sizeof c, total)), line) : "");
    sbar_set(SC.disk_bar, frac, level_fill(frac * 100, 85, 93));
    /* the other mounts, one line */
    size_t o = 0;
    line[0] = 0;
    for (int i = 0; agent && i < a->nmounts && o < sizeof line - 48; i++) {
        const cat_ag_mount_t *m = &a->mounts[i];
        if (m == root || !(m->total > 0)) continue;
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%s  %.0f%% of %s", o ? "\n" : "", m->mount, 100 * m->used / m->total, fmt_bytes(b, sizeof b, m->total));
    }
    ui_text(SC.mounts, "%s", o ? line : agent ? "/ is the only disk" : "what fills it: with catalyst-agent");
    /* directories, largest first: the answer to "what do I delete" is the first row */
    int idx[CAT_AG_DIRS], nd = 0;
    for (int i = 0; agent && i < a->ndirs; i++) if (a->dirs[i].bytes > 0) idx[nd++] = i;
    for (int i = 1; i < nd; i++)
        for (int j = i; j > 0 && a->dirs[idx[j]].bytes > a->dirs[idx[j - 1]].bytes; j--) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    o = 0;
    line[0] = 0;
    for (int i = 0; i < nd && i < 3; i++)
        o += (size_t)snprintf(line + o, sizeof line - o, "%s%s  %s", i ? "\n" : "", fmt_bytes(b, sizeof b, a->dirs[idx[i]].bytes), a->dirs[idx[i]].path);
    ui_text(SC.dirs, "%s", line);
    /* eMMC: a band, not a percentage (JEDEC gives ten steps), and the device's own pre-EOL opinion */
    double life = ntnum("/Catalyst/Systemcore/EmmcLifeUsed");
    int pre = R->sc_emmc;
    char wear[64] = "wear not reported";
    if (life == life) {
        int top = (int)fmin(100, round(life * 10) * 10), bottom = top >= 10 ? top - 10 : 0;
        snprintf(wear, sizeof wear, "about %d\xe2\x80\x93%d%% of its rated life used", bottom, top);
    }
    ui_text(SC.emmc, "emmc " MID " %s%s%s", wear, pre ? " " MID " " : "", pre == 1 ? "healthy" : pre == 2 ? "wearing out" : pre >= 3 ? "replace it" : "");
    bz_set_color(SC.emmc, pre >= 3 || life >= 0.9 ? BZ_C_FAULT : pre == 2 || life >= 0.7 ? BZ_C_WARN : BZ_C_DIM);
}

static void sc_network(const cat_agent_t *a, bool agent)
{
    int n = 0;
    if (agent) {
        for (int i = 0; i < a->nnics && n < SC_NICS; i++, n++) {
            const cat_ag_nic_t *c = &a->nics[i];
            show(SC.nic[n].row, true);
            bz_mark_set(SC.nic[n].mark, c->up ? BZ_OK : BZ_STALE);
            ui_text(SC.nic[n].name, "%s", c->name);
            ui_text(SC.nic[n].addr, "%s", c->addrs[0] ? c->addrs : c->up ? "no address" : "down");
            bz_set_color(SC.nic[n].addr, c->addrs[0] ? BZ_C_INK : BZ_C_DIM);
            /* usb0 is Systemcore's USB-C gadget: the tether */
            bool tether = !strncmp(c->name, "usb", 3);
            if (c->wireless) ui_text(SC.nic[n].sub, "wi-fi " MID " %.0f dbm " MID " quality %.0f", c->dbm, c->quality);
            else if (c->mbps == c->mbps) ui_text(SC.nic[n].sub, "%s%.0f mbit/s", tether ? "usb tether " MID " " : "", c->mbps);
            else ui_text(SC.nic[n].sub, "%s", tether ? "usb tether" : c->mac);
        }
        ui_text(SC.net_note, "%d interfaces", a->nnics);
    } else {
        /* passed through as the OS words it: reformatting would be guessing at an undocumented shape */
        static char buf[1024];
        const char *items[SC_NICS];
        int k = cat_nt() ? nt4_get_strings(cat_nt(), "/Catalyst/Systemcore/NetworkInterfaces", buf, sizeof buf, items, SC_NICS) : -1;
        for (int i = 0; i < k && n < SC_NICS; i++, n++) {
            show(SC.nic[n].row, true);
            bz_mark_set(SC.nic[n].mark, BZ_INFO);
            ui_text(SC.nic[n].name, " ");
            ui_text(SC.nic[n].addr, "%s", items[i]);
            bz_set_color(SC.nic[n].addr, BZ_C_INK);
            ui_text(SC.nic[n].sub, "as the os reports it");
        }
        ui_text(SC.net_note, "%s", k > 0 ? "from networktables" : "");
    }
    for (int i = n; i < SC_NICS; i++) show(SC.nic[i].row, false);
    /* how this tablet reaches it */
    hal_tether_t t;
    hal_tether(&t);
    hal_net_t net;
    hal_net(&net);
    if (t.up) ui_text(SC.net_foot, "this tablet: usb tether %s " MID " %s", t.kind, t.ip);
    else if (net.up) ui_text(SC.net_foot, "this tablet: wi-fi %s " MID " %s", net.ssid, net.ip);
    else ui_text(SC.net_foot, "this tablet: %s", net.link == HAL_LINK_SIM ? "simulator" : "no link");
    ui_text(SC.net_empty, "%s", n ? "" : "The robot doesn't publish /Catalyst/Systemcore/NetworkInterfaces; catalyst-agent lists every interface with its addresses.");
}

static void sc_cameras(const cat_agent_t *a, bool agent)
{
    char s[96];
    int n = 0;
    if (agent && a->ncams) {
        for (int i = 0; i < a->ncams && n < SC_CAMS; i++, n++) {
            const cat_ag_cam_t *c = &a->cams[i];
            /* Console paintAgentCameras: not answering, hot (>= 80 °C), talking to the robot, or not */
            bool hot = c->temp == c->temp && c->temp >= 80;
            bz_mark_set(SC.cam[n].mark, !c->reachable ? BZ_FAULT : hot || !c->nt ? BZ_WARN : BZ_OK);
            ui_text(SC.cam[n].name, "%s", c->name[0] ? c->name : c->ip);
            size_t o = 0;
            s[0] = 0;
            if (c->fps == c->fps) o += (size_t)snprintf(s + o, sizeof s - o, "%.0f fps", c->fps);
            if (c->temp == c->temp) o += (size_t)snprintf(s + o, sizeof s - o, "%s%.0f\xc2\xb0""c", o ? "  " MID "  " : "", c->temp);
            if (c->cpu == c->cpu) o += (size_t)snprintf(s + o, sizeof s - o, "%scpu %.0f%%", o ? "  " MID "  " : "", c->cpu);
            ui_text(SC.cam[n].val, "%s", o ? s : "not answering");
            bz_set_color(SC.cam[n].val, !c->reachable ? BZ_C_FAULT : hot ? BZ_C_WARN : BZ_C_INK);
            ui_text(SC.cam[n].sub, "%s " MID " %s", c->ip, !c->reachable ? "not answering" : hot ? "running hot"
                                                          : c->nt ? "talking to the robot" : "no networktables session");
        }
        ui_text(SC.cams_note, "from the os");
    } else {
        /* the robot's own vision health rows */
        const cat_robot_t *r = R;
        for (int i = 0; i < r->ncameras && n < SC_CAMS; i++, n++) {
            const cat_camera_t *c = &r->cameras[i];
            bool ok = !strcmp(c->state, "OK") || !strcmp(c->state, "NO_TARGETS");
            bz_mark_set(SC.cam[n].mark, !c->connected ? BZ_FAULT : ok ? BZ_OK : BZ_WARN);
            ui_text(SC.cam[n].name, "%s", c->name);
            char f[16], t[16];
            ui_text(SC.cam[n].val, "%s fps  " MID "  %s\xc2\xb0""c", bz_fmt(f, sizeof f, c->fps == c->fps, "%.0f", c->fps),
                    bz_fmt(t, sizeof t, c->temp_c == c->temp_c, "%.0f", c->temp_c));
            bz_set_color(SC.cam[n].val, BZ_C_INK);
            ui_text(SC.cam[n].sub, "%s", c->state);
        }
        ui_text(SC.cams_note, "%s", n ? "from networktables" : "");
    }
    for (int i = 0; i < SC_CAMS; i++) show(SC.cam[i].row, i < n);
    ui_text(SC.cams_empty, "%s", n ? "" : agent ? (a->cams_available ? "the os sees no limelight" : a->cams_reason) : "no cameras reported");
}

static void sc_log(const cat_agent_t *a)
{
    int from = a->nlog > SC_LOG ? a->nlog - SC_LOG : 0;
    for (int i = 0; i < SC_LOG; i++) {
        int k = from + i;
        if (k >= a->nlog) {
            ui_text(SC.log[i], " ");
            continue;
        }
        /* journalctl's short-iso: keep the time of day, drop the date and zone */
        const char *l = a->log[k];
        if (strlen(l) > 25 && l[4] == '-' && l[10] == 'T') ui_text(SC.log[i], "%.8s  %s", l + 11, l + 25);
        else ui_text(SC.log[i], "%s", l);
        bool bad = strstr(l, "Exception") || strstr(l, "FAILURE") || strstr(l, "Error") || strstr(l, "  at ");
        bz_set_color(SC.log[i], bad ? BZ_C_WARN : BZ_C_INK);
    }
    ui_text(SC.log_note, "last %d of %d lines, newest last", a->nlog < SC_LOG ? a->nlog : SC_LOG, a->nlog);
}

static void sc_layout(bool agent)
{
    if (agent == SC.agent_layout) return;
    SC.agent_layout = agent;
    show(SC.t_prog, agent);
    show(SC.t_procs, agent);
    show(SC.t_log, agent);
    show(SC.install, !agent);
    /* without the agent, the machine tile fills the processor row rather than trailing alone */
    lv_obj_move_to_index(SC.t_mach, agent ? -1 : 3);
}

static void sc_refresh(void)
{
    if (!SC.a) return;
    cat_agent_want();
    cat_ag_status_t st;
    cat_agent_status(&st);
    cat_agent_get(SC.a, &SC.rev);
    bool agent = st.state == CAT_AG_OK && SC.a->valid;
    const cat_robot_t *r = R;

    char age[24];
    if (!r->connected) {
        bz_mark_set(SC.head_mark, BZ_STALE);
        ui_text(SC.head, "no robot");
    } else if (agent) {
        bz_mark_set(SC.head_mark, st.age_s < 10 ? BZ_OK : BZ_STALE);
        ui_text(SC.head, "catalyst-agent %s " MID " %s ago", SC.a->agent_version, fmt_dur(age, sizeof age, st.age_s));
    } else if (st.state == CAT_AG_ABSENT) {
        bz_mark_set(SC.head_mark, BZ_INFO);
        ui_text(SC.head, "networktables summary");
    } else {
        bz_mark_set(SC.head_mark, BZ_STALE);
        ui_text(SC.head, "asking catalyst-agent\xe2\x80\xa6");
    }
    sc_layout(agent);
    if (!agent) {
        if (!r->connected) ui_text(SC.install_text, "Not connected. The summary arrives over NetworkTables; the detail from catalyst-agent on the robot, port %d.", CAT_AGENT_PORT);
        else if (!r->line2) ui_text(SC.install_text, "A Catalyst 1.x robot on a roboRIO publishes no Systemcore summary.");
        else set_long(SC.install_text, "For per-core load, the program's restarts and log, what fills the disk and CAN frame counters, install catalyst-agent on the Systemcore: FrcCatalyst agent/build.sh, then the Systemcore web UI's package manager (or opkg install).");
    }
    sc_cpu(SC.a, agent);
    sc_temp_mem(SC.a, agent);
    if (agent) {
        sc_program(SC.a);
        sc_procs(SC.a);
        sc_log(SC.a);
    }
    sc_machine(SC.a, agent);
    sc_can(SC.a, agent);
    sc_storage(SC.a, agent);
    sc_network(SC.a, agent);
    sc_cameras(SC.a, agent);
}

static void sc_open(void) { SC.rev = 0; }

const ui_app_t APP_SYSTEMCORE = { .name = "systemcore", .icon = BZ_I_DEVELOPER_BOARD, .build = sc_build, .open = sc_open,
                                  .refresh = sc_refresh };

/* ================================================================== motors */

#define MH_LIST_W 700
#define MH_DETAIL_W (CW - MH_LIST_W - BZ_GAP)
#define MH_ROWS 48

typedef enum { MH_LOADED, MH_TURNING, MH_HOT, MH_POWERED, MH_PEAK, MH_SORTS } mh_sort_t;
static const char *const MH_SORT_NAME[MH_SORTS] = { "loaded", "turning", "hot", "powered", "peak \xc2\xb0""c" };

static struct {
    cat_mh_motor_t *m;
    int n;
    cat_mh_meta_t meta;
    uint32_t rev, sig;
    int order[CAT_MH_MAX];
    int sort, sel;
    lv_obj_t *head, *chips[MH_SORTS], *list, *empty;
    struct { lv_obj_t *tile, *name, *sub, *val, *flag; sbar_t *bar; int m; } row[MH_ROWS];
    int nrows;
    /* detail */
    lv_obj_t *d_tile, *d_mark, *d_note, *d_name, *d_sub, *d_stat[9], *d_ids, *d_ids_head, *d_boots, *d_boots_head, *d_foot;
} MH;

static double mh_key(const cat_mh_motor_t *m, int sort)
{
    switch (sort) {
    case MH_TURNING: return m->running_s;
    case MH_HOT: return m->hot_s + m->peak_c * 1e-3; /* hot time, then the hottest peak */
    case MH_POWERED: return m->powered_s;
    case MH_PEAK: return m->peak_c;
    default: return m->loaded_s;
    }
}

static const char *mh_value(char *b, size_t n, const cat_mh_motor_t *m, int sort)
{
    switch (sort) {
    case MH_TURNING: return fmt_hours(b, n, m->running_s);
    case MH_HOT: return m->hot_s > 0 ? fmt_hours(b, n, m->hot_s) : (snprintf(b, n, DASH), b);
    case MH_POWERED: return fmt_hours(b, n, m->powered_s);
    case MH_PEAK: return m->peak_c > 0 ? (snprintf(b, n, "%.0f\xc2\xb0", m->peak_c), b) : (snprintf(b, n, DASH), b);
    default: return fmt_hours(b, n, m->loaded_s);
    }
}

static void mh_detail(void);

static void mh_pick(lv_obj_t *o, void *u)
{
    (void)o;
    int r = (int)(intptr_t)u;
    if (r >= MH.nrows) return;
    MH.sel = MH.row[r].m;
    for (int i = 0; i < MH.nrows; i++) bz_tile_set_fill(MH.row[i].tile, i == r ? BZ_C_SURFACE3 : BZ_C_SURFACE1);
    mh_detail();
    hal_tone(1500, 8, S.volume * 0.4f);
}

static void mh_rows(void)
{
    lv_obj_clean(MH.list);
    MH.nrows = 0;
    /* motors first, sorted by the chosen wear, descending; the rest (encoders, IMUs) after */
    int n = 0;
    for (int i = 0; i < MH.n; i++) if (!strcmp(MH.m[i].kind, "motor")) MH.order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && mh_key(&MH.m[MH.order[j]], MH.sort) > mh_key(&MH.m[MH.order[j - 1]], MH.sort); j--) {
            int t = MH.order[j]; MH.order[j] = MH.order[j - 1]; MH.order[j - 1] = t;
        }
    int motors = n;
    for (int i = 0; i < MH.n; i++) if (strcmp(MH.m[i].kind, "motor")) MH.order[n++] = i;
    double top = 1e-9;
    for (int i = 0; i < motors; i++) top = fmax(top, mh_key(&MH.m[MH.order[i]], MH.sort));
    for (int k = 0; k < n && MH.nrows < MH_ROWS; k++) {
        const cat_mh_motor_t *m = &MH.m[MH.order[k]];
        bool motor = k < motors;
        lv_obj_t *t = bz_tile(MH.list, MH_LIST_W, 78);
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_pad_ver(t, 14, 0);
        lv_obj_set_style_pad_hor(t, 20, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t, mh_pick, (void *)(intptr_t)MH.nrows);
        char b[48], sub[128];
        lv_obj_t *name = bz_label_line(t, m->name[0] ? m->name : "unnamed", BZ_F_BODY, m->name[0] ? BZ_C_INK : BZ_C_DIM, 300);
        lv_obj_set_pos(name, 0, -2);
        const char *serial = m->serial;
        size_t sl = strlen(serial);
        snprintf(sub, sizeof sub, "%s%s%s " MID " \xe2\x80\xa6%s " MID " %s", m->bus, m->id >= 0 ? " id " : "",
                 m->id >= 0 ? (snprintf(b, sizeof b, "%d", m->id), b) : "", sl > 8 ? serial + sl - 8 : serial, m->model);
        lv_obj_t *sl_ = bz_label_line(t, sub, BZ_F_CAPTION, BZ_C_DIM, 330);
        lv_obj_align(sl_, LV_ALIGN_BOTTOM_LEFT, 0, 2);
        /* flags: hot-worn and renumbered, as shape + word */
        lv_obj_t *fl = bz_row(t, 6);
        lv_obj_set_pos(fl, 340, 0);
        lv_obj_set_height(fl, 50);
        lv_obj_set_flex_flow(fl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(fl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        if (motor && cat_mh_hot(m)) {
            lv_obj_t *r = bz_row(fl, 6);
            bz_mark(r, BZ_WARN, 8);
            bz_label(r, m->hot_s > 0 ? "hot time" : "ran hot", BZ_F_CAPTION, BZ_C_WARN);
        }
        if (cat_mh_renumbered(m)) {
            lv_obj_t *r = bz_row(fl, 6);
            bz_mark(r, BZ_INFO, 8);
            snprintf(b, sizeof b, "%d past identit%s", m->identities - 1, m->identities == 2 ? "y" : "ies");
            bz_label(r, b, BZ_F_CAPTION, BZ_C_DIM);
        }
        if (motor) {
            lv_obj_t *v = bz_label(t, mh_value(b, sizeof b, m, MH.sort), BZ_F_NAME, BZ_C_INK);
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, -4);
            lv_obj_t *bar = bz_meter(t, 150, 8);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
            double k = mh_key(m, MH.sort);
            bz_meter_set(bar, (float)(k / top), MH.sort == MH_HOT && m->hot_s > 0 ? BZ_C_WARN : BZ_C_ICE);
        } else {
            lv_obj_t *v = bz_label(t, m->kind, BZ_F_LABEL, BZ_C_DIM);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        MH.row[MH.nrows].tile = t;
        MH.row[MH.nrows].m = MH.order[k];
        if (MH.order[k] == MH.sel) bz_tile_set_fill(t, BZ_C_SURFACE3);
        MH.nrows++;
    }
}

static void mh_sort(lv_obj_t *o, void *u)
{
    (void)o;
    MH.sort = (int)(intptr_t)u;
    for (int i = 0; i < MH_SORTS; i++) ui_chip_set(MH.chips[i], i == MH.sort);
    mh_rows();
    hal_tone(1500, 8, S.volume * 0.4f);
}

/* Recent boots: one bar per boot, height is revolutions, warn-coloured when it had hot time. */
static void mh_boots_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (MH.sel < 0 || MH.sel >= MH.n) return;
    const cat_mh_motor_t *m = &MH.m[MH.sel];
    if (!m->nsess) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    double top = 1e-9;
    for (int i = 0; i < m->nsess; i++) top = fmax(top, m->sess[i].revs);
    float slot = (float)lv_area_get_width(&a) / CAT_MH_SESSIONS, bw = slot * 0.62f;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 3;
    rd.bg_opa = LV_OPA_COVER;
    int h = lv_area_get_height(&a);
    for (int i = 0; i < m->nsess; i++) {
        const cat_mh_session_t *s = &m->sess[i];
        /* right-aligned: the newest boot is always at the right edge */
        float x = a.x1 + (CAT_MH_SESSIONS - m->nsess + i) * slot + (slot - bw) / 2;
        int bh = 4 + (int)((h - 4) * (s->revs / top));
        lv_area_t r = { (int32_t)x, a.y2 - bh + 1, (int32_t)(x + bw), a.y2 };
        rd.bg_color = bz_lv(s->hot_s > 0 ? BZ_C_WARN : BZ_C_ICE);
        lv_draw_rect(layer, &rd, &r);
    }
}

static void mh_detail(void)
{
    char b[64], c[64];
    bool have = MH.sel >= 0 && MH.sel < MH.n;
    show(MH.d_tile, have);
    if (!have) return;
    const cat_mh_motor_t *m = &MH.m[MH.sel];
    bool motor = !strcmp(m->kind, "motor");
    bz_mark_set(MH.d_mark, !motor ? BZ_INFO : cat_mh_hot(m) ? BZ_WARN : BZ_OK);
    ui_text(MH.d_note, "%s", !motor ? m->kind : cat_mh_hot(m) ? (m->hot_s > 0 ? "hot time on record" : "ran hot") : "cool");
    ui_text(MH.d_name, "%s", m->name[0] ? m->name : "unnamed");
    ui_text(MH.d_sub, "%s " MID " %s id %s " MID " firmware %s\nserial %s%s%s", m->model[0] ? m->model : "device", m->bus[0] ? m->bus : DASH,
            m->id >= 0 ? (snprintf(b, sizeof b, "%d", m->id), b) : DASH, m->firmware[0] ? m->firmware : DASH, m->serial,
            m->hw_rev[0] ? " " MID " hardware " : "", m->hw_rev);
    static const char *const K[9] = { "powered", "turning", "loaded", "revolutions", "peak current", "peak temp", "hot", "energy", "boots" };
    for (int i = 0; i < 9; i++) {
        lv_obj_t *cell = MH.d_stat[i];
        lv_obj_t *k = lv_obj_get_child(cell, 0), *v = lv_obj_get_child(cell, 1);
        ui_text(k, "%s", K[i]);
        bz_color_role_t col = BZ_C_INK;
        switch (i) {
        case 0: ui_text(v, "%s", fmt_hours(b, sizeof b, m->powered_s)); break;
        case 1: ui_text(v, "%s", motor ? fmt_hours(b, sizeof b, m->running_s) : DASH); break;
        case 2: ui_text(v, "%s", motor ? fmt_hours(b, sizeof b, m->loaded_s) : DASH); break;
        case 3: ui_text(v, "%s", motor ? fmt_revs(b, sizeof b, m->revs) : DASH); break;
        case 4: ui_text(v, "%s", m->peak_a > 0 ? (snprintf(b, sizeof b, "%.0f a", m->peak_a), b) : DASH); break;
        case 5:
            ui_text(v, "%s", m->peak_c > 0 ? (snprintf(b, sizeof b, "%.0f\xc2\xb0""c", m->peak_c), b) : DASH);
            if (m->peak_c >= CAT_MH_HOT_C) col = BZ_C_WARN;
            break;
        case 6:
            ui_text(v, "%s", m->hot_s > 0 ? fmt_hours(b, sizeof b, m->hot_s) : DASH);
            if (m->hot_s > 0) col = BZ_C_WARN;
            break;
        case 7: ui_text(v, "%s", m->energy_j > 0 ? (snprintf(b, sizeof b, "%.0f wh", m->energy_j / 3600), b) : DASH); break;
        case 8: ui_text(v, "%d", m->boots); break;
        }
        bz_set_color(v, col);
    }
    /* the motor's résumé: newest first */
    bool trusted = MH.meta.clock_trusted;
    if (m->nids) {
        ui_text(MH.d_ids_head, "identities " MID " %d on record", m->identities);
        char text[512];
        size_t o = 0;
        text[0] = 0;
        int shown = 0;
        for (int i = m->nids - 1; i >= 0 && shown < 3; i--, shown++) {
            const cat_mh_ident_t *id = &m->ids[i];
            o += (size_t)snprintf(text + o, sizeof text - o, "%s%s  id %d on %s " MID " %s " MID " %s", o ? "\n" : "", fmt_date(b, sizeof b, id->first_ms, trusted),
                                  id->id, id->bus, id->name[0] ? id->name : "unnamed", id->firmware[0] ? id->firmware : "firmware unknown");
        }
        if (m->identities > 3) snprintf(text + o, sizeof text - o, "\nand %d before those", m->identities - 3);
        set_long(MH.d_ids, text);
    } else {
        ui_text(MH.d_ids_head, "identities");
        ui_text(MH.d_ids, "%d on record %s the list of them is in the agent's file", m->identities, MID);
    }
    if (m->nsess) ui_text(MH.d_boots_head, "recent boots " MID " revolutions each " MID " %d of %d", m->nsess, m->boots);
    else ui_text(MH.d_boots_head, "recent boots " MID " %s", motor ? "in the agent's file" : "none for a sensor");
    lv_obj_invalidate(MH.d_boots);
    ui_text(MH.d_foot, "on record since %s%s%s", fmt_date(c, sizeof c, m->first_ms, trusted),
            m->sticky ? " " MID " sticky faults ever 0x" : "", m->sticky ? (snprintf(b, sizeof b, "%llx", (unsigned long long)m->sticky), b) : "");
}

static void mh_build(lv_obj_t *b)
{
    sc_common();
    MH.m = calloc(CAT_MH_MAX, sizeof *MH.m);
    MH.sel = -1;
    lv_obj_t *r = head_right(b);
    MH.head = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);

    lv_obj_t *chips = bz_row(b, 10);
    lv_obj_set_pos(chips, PAD, APP_Y);
    for (int i = 0; i < MH_SORTS; i++) MH.chips[i] = ui_chip(chips, MH_SORT_NAME[i], mh_sort, (void *)(intptr_t)i);
    ui_chip_set(MH.chips[0], true);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y + 56 + BZ_GAP);
    MH.list = ui_scroller(wrap, MH_LIST_W, APP_H - 56 - BZ_GAP);
    lv_obj_set_style_pad_row(MH.list, 10, 0);

    MH.empty = empty_tile(b, CW, 150, "no motor history",
                          "Catalyst's MotorHistory (2.x) keeps every motor by serial number: hours powered, turning and "
                          "loaded, revolutions, peaks, hot time and every id and name it has carried. It reaches the "
                          "tablet through catalyst-agent's /api/motor-history, or /Catalyst/MotorHistory/Rows.");
    lv_obj_set_pos(MH.empty, PAD, APP_Y);
    show(MH.empty, false);

    int x = PAD + MH_LIST_W + BZ_GAP, w = MH_DETAIL_W;
    MH.d_tile = bz_tile(b, w, APP_H);
    lv_obj_set_pos(MH.d_tile, x, APP_Y);
    tile_head(MH.d_tile, "motor", &MH.d_mark, &MH.d_note);
    MH.d_name = bz_label_line(MH.d_tile, "", BZ_F_TITLE, BZ_C_INK, IN(w));
    lv_obj_set_pos(MH.d_name, 0, 22);
    MH.d_sub = bz_label(MH.d_tile, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(MH.d_sub, IN(w));
    lv_label_set_long_mode(MH.d_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(MH.d_sub, 3, 0);
    lv_obj_set_pos(MH.d_sub, 0, 70);
    int cw = IN(w) / 3;
    for (int i = 0; i < 9; i++) {
        lv_obj_t *cell = bz_col(MH.d_tile, 0);
        lv_obj_set_width(cell, cw);
        lv_obj_set_pos(cell, (i % 3) * cw, 116 + (i / 3) * 52);
        bz_label(cell, "", BZ_F_CAPTION, BZ_C_DIM);
        bz_label(cell, DASH, BZ_F_NAME, BZ_C_INK);
        MH.d_stat[i] = cell;
    }
    MH.d_ids_head = bz_label(MH.d_tile, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(MH.d_ids_head, 0, 280);
    MH.d_ids = bz_label(MH.d_tile, "", BZ_F_CAPTION, BZ_C_INK);
    lv_obj_set_width(MH.d_ids, IN(w));
    lv_obj_set_style_text_line_space(MH.d_ids, 4, 0);
    lv_obj_set_pos(MH.d_ids, 0, 304);
    MH.d_boots_head = bz_label_line(MH.d_tile, "", BZ_F_LABEL, BZ_C_DIM, IN(w));
    lv_obj_set_pos(MH.d_boots_head, 0, 390);
    MH.d_boots = bz_box(MH.d_tile);
    lv_obj_set_size(MH.d_boots, IN(w), 80);
    lv_obj_set_pos(MH.d_boots, 0, 418);
    lv_obj_add_event_cb(MH.d_boots, mh_boots_draw, LV_EVENT_DRAW_MAIN, NULL);
    MH.d_foot = bz_label_line(MH.d_tile, "", BZ_F_CAPTION, BZ_C_DIM, IN(w));
    lv_obj_align(MH.d_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    show(MH.d_tile, false);
}

static void mh_refresh(void)
{
    if (!MH.m) return;
    cat_mh_want();
    int n = cat_mh_get(MH.m, CAT_MH_MAX, &MH.meta, &MH.rev);
    if (n >= 0) {
        MH.n = n;
        /* keep the selection on the same serial across a refresh */
        uint32_t sig = 2166136261u ^ (uint32_t)n;
        for (int i = 0; i < n; i++) sig = hash_str(sig, MH.m[i].serial);
        if (sig != MH.sig || MH.sel >= n) {
            MH.sig = sig;
            if (MH.sel >= n) MH.sel = -1;
        }
        if (MH.sel < 0 && n) {
            /* open on the most worn motor */
            mh_rows();
            MH.sel = MH.nrows ? MH.row[0].m : -1;
        }
        mh_rows();
        mh_detail();
        int motors = 0, hot = 0, moved = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(MH.m[i].kind, "motor")) continue;
            motors++;
            hot += cat_mh_hot(&MH.m[i]);
            moved += cat_mh_renumbered(&MH.m[i]);
        }
        if (n) ui_text(MH.head, "%d motors " MID " %d hot " MID " %d renumbered", motors, hot, moved);
        show(MH.empty, n == 0);
        show(lv_obj_get_parent(MH.chips[0]), n > 0);
        show(lv_obj_get_parent(lv_obj_get_parent(MH.list)), n > 0);
        if (!n) show(MH.d_tile, false);
    }
    if (!MH.n) {
        cat_ag_status_t st;
        cat_agent_status(&st);
        ui_text(MH.head, "%s", !R->connected ? "no robot" : st.state == CAT_AG_LOOKING ? "asking catalyst-agent\xe2\x80\xa6" : MH.meta.error[0] ? MH.meta.error : "nothing on record");
    }
}

static void mh_open(void) { MH.rev = 0; }

const ui_app_t APP_MOTORS = { .name = "motors", .icon = BZ_I_HISTORY, .build = mh_build, .open = mh_open, .refresh = mh_refresh };

/* ================================================================== states */

#define ST_LIST_W 812
#define ST_DETAIL_W (CW - ST_LIST_W - BZ_GAP)
#define ST_TEXT_W 214
#define ST_LANE_H 104

static const float ST_WINDOWS[3] = { 60, 300, 900 };

static struct {
    lv_obj_t *chips[3], *list, *axis[3], *empty;
    struct { lv_obj_t *tile, *name, *state, *since, *count, *tl; int lane; } lane[CAT_ST_LANES];
    int nlanes, sel, win;
    bz_motion_t span;                    /* the window's length in ms, springing between 1, 5 and 15 min */
    uint32_t order_sig, rev;
    double last_draw;
    /* detail */
    lv_obj_t *d_tile, *d_name, *d_now, *d_break, *d_counters, *d_list;
    uint32_t d_sig;
} STS;

/* A categorical palette for states: Bezel's accents and tones, never the status colours, which mean
 * status and nothing else. The robot's mode lane keeps its own meanings. */
static lv_color_t st_color(const cat_st_lane_t *l, int s)
{
    if (l->kind == CAT_LANE_MODE && s >= 0) {
        const char *n = l->names[s];
        if (!strcmp(n, "e-stop")) return bz_lv(BZ_C_FAULT);
        if (!strcmp(n, "teleop")) return bz_lv(BZ_C_LEAF);
        if (!strcmp(n, "auto")) return bz_lv(BZ_C_ICE);
        if (!strcmp(n, "test")) return bz_lv(BZ_C_AMBER);
        return bz_lv(BZ_C_FAINT);
    }
    static const bz_color_role_t base[4] = { BZ_C_ICE, BZ_C_LEAF, BZ_C_AMBER, BZ_C_DIM };
    uint32_t c = bz_color(base[s % 4]);
    if ((s / 4) % 2) c = bz_mix(c, bz_color(BZ_C_SURFACE3), 0.5f);
    return lv_color_hex(c);
}

static lv_color_t on_color(lv_color_t c)
{
    int lum = (c.red * 299 + c.green * 587 + c.blue * 114) / 1000;
    return lum > 140 ? lv_color_hex(0x121417) : lv_color_hex(0xF2F3F5);
}

static void st_timeline_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int li = (int)(intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = lv_area_get_width(&a);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 10;
    rd.bg_opa = LV_OPA_COVER;
    rd.bg_color = bz_lv(BZ_C_SURFACE2);
    lv_draw_rect(layer, &rd, &a);
    cat_states_lock();
    const cat_st_lane_t *l = cat_states_lane(li);
    if (!l) {
        cat_states_unlock();
        return;
    }
    uint32_t now = cat_states_now();
    float span = STS.span.value;
    double t0 = (double)now - span;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = bz_font(BZ_F_CAPTION);
    ld.text_local = 1;
    for (int k = l->count - 1; k >= 0; k--) {
        cat_st_edge_t ed, nx;
        cat_st_edge(l, k, &ed);
        double s0 = ed.ms, s1 = k ? (cat_st_edge(l, k - 1, &nx), (double)nx.ms) : (double)now;
        if (s1 <= t0 || ed.s < 0) continue;
        if (s0 < t0) s0 = t0;
        int x0 = a.x1 + (int)((s0 - t0) / span * w), x1 = a.x1 + (int)((s1 - t0) / span * w);
        if (x1 > a.x2) x1 = a.x2;
        if (x1 - x0 < 2) x1 = x0 + 2;
        lv_area_t seg = { x0, a.y1, x1, a.y2 };
        /* one pixel of ground between neighbours, so two states side by side read as two */
        if (k) seg.x2 -= 1;
        rd.bg_color = st_color(l, ed.s);
        rd.radius = 6;
        lv_draw_rect(layer, &rd, &seg);
        const char *name = l->names[ed.s];
        lv_point_t ts;
        lv_text_get_size(&ts, name, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (seg.x2 - seg.x1 > ts.x + 16) {
            ld.color = on_color(rd.bg_color);
            ld.text = name;
            lv_area_t ta = { seg.x1 + 8, a.y1 + (lv_area_get_height(&a) - ts.y) / 2, seg.x2 - 4, a.y2 };
            lv_draw_label(layer, &ld, &ta);
        }
    }
    cat_states_unlock();
}

static void st_detail(void);

static void st_pick(lv_obj_t *o, void *u)
{
    (void)o;
    STS.sel = (int)(intptr_t)u;
    for (int i = 0; i < STS.nlanes; i++) bz_tile_set_fill(STS.lane[i].tile, STS.lane[i].lane == STS.sel ? BZ_C_SURFACE3 : BZ_C_SURFACE1);
    STS.d_sig = 0;
    st_detail();
    hal_tone(1500, 8, S.volume * 0.4f);
}

static void st_window(lv_obj_t *o, void *u)
{
    (void)o;
    STS.win = (int)(intptr_t)u;
    for (int i = 0; i < 3; i++) ui_chip_set(STS.chips[i], i == STS.win);
    /* the lanes zoom rather than jump: the window's length rides `smooth` */
    bz_motion_to(&STS.span, ST_WINDOWS[STS.win] * 1000, BZ_SMOOTH);
    static const char *const AX[3][3] = { { "\xe2\x80\x93""1 min", "\xe2\x80\x93""30 s", "now" },
                                          { "\xe2\x80\x93""5 min", "\xe2\x80\x93""2.5 min", "now" },
                                          { "\xe2\x80\x93""15 min", "\xe2\x80\x93""7.5 min", "now" } };
    for (int i = 0; i < 3; i++) ui_text(STS.axis[i], "%s", AX[STS.win][i]);
    STS.d_sig = 0;
    hal_tone(1500, 8, S.volume * 0.4f);
}

static int lane_rank(cat_lane_kind_t k)
{
    return k == CAT_LANE_MODE ? 0 : k == CAT_LANE_MACHINE ? 1 : k == CAT_LANE_MECH ? 2 : k == CAT_LANE_AUTONOMY ? 3 : 4;
}

static void st_rebuild(void)
{
    lv_obj_clean(STS.list);
    STS.nlanes = 0;
    cat_states_lock();
    int n = cat_states_count();
    int order[CAT_ST_LANES];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && lane_rank(cat_states_lane(order[j])->kind) < lane_rank(cat_states_lane(order[j - 1])->kind); j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (int k = 0; k < n; k++) {
        const cat_st_lane_t *l = cat_states_lane(order[k]);
        lv_obj_t *t = bz_tile(STS.list, ST_LIST_W, ST_LANE_H);
        lv_obj_set_style_radius(t, 24, 0);
        lv_obj_set_style_pad_ver(t, 16, 0);
        lv_obj_set_style_pad_hor(t, 22, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t, st_pick, (void *)(intptr_t)order[k]);
        char nm[48];
        snprintf(nm, sizeof nm, "%s", l->name);
        for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        STS.lane[k].name = bz_label_line(t, nm, BZ_F_LABEL, BZ_C_DIM, ST_TEXT_W);
        STS.lane[k].state = bz_label_line(t, "", BZ_F_NAME, BZ_C_INK, ST_TEXT_W);
        lv_obj_set_pos(STS.lane[k].state, 0, 20);
        STS.lane[k].since = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, ST_TEXT_W);
        lv_obj_align(STS.lane[k].since, LV_ALIGN_BOTTOM_LEFT, 0, 4);
        STS.lane[k].tl = bz_box(t);
        lv_obj_set_size(STS.lane[k].tl, ST_LIST_W - 44 - ST_TEXT_W - 16, 44);
        lv_obj_align(STS.lane[k].tl, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(STS.lane[k].tl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(STS.lane[k].tl, st_timeline_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)order[k]);
        STS.lane[k].tile = t;
        STS.lane[k].lane = order[k];
        if (order[k] == STS.sel) bz_tile_set_fill(t, BZ_C_SURFACE3);
        STS.nlanes++;
    }
    cat_states_unlock();
    /* the axis sits over the timelines only */
    show(STS.empty, n <= 1);
}

static void st_detail(void)
{
    cat_states_lock();
    const cat_st_lane_t *l = cat_states_lane(STS.sel);
    if (!l) {
        cat_states_unlock();
        show(STS.d_tile, false);
        return;
    }
    show(STS.d_tile, true);
    uint32_t now = cat_states_now(), span = (uint32_t)(ST_WINDOWS[STS.win] * 1000);
    uint32_t t0 = now > span ? now - span : 0;
    char nm[48], b[32];
    snprintf(nm, sizeof nm, "%s", l->name);
    ui_text(STS.d_name, "%s", nm);
    const char *cur = cat_st_current(l);
    cat_st_edge_t e;
    bool have = cat_st_edge(l, 0, &e);
    ui_text(STS.d_now, "%s%s%s", cur ? cur : "no value", have ? " for " : "", have ? fmt_dur(b, sizeof b, (now - e.ms) / 1000.0) : "");
    /* time in each state over the window, biggest first */
    int idx[CAT_ST_NAMES], ni = 0;
    uint32_t tin[CAT_ST_NAMES];
    for (int s = 0; s < l->nnames; s++) {
        tin[s] = cat_st_time_in(l, s, t0, now, now);
        if (tin[s]) idx[ni++] = s;
    }
    for (int i = 1; i < ni; i++)
        for (int j = i; j > 0 && tin[idx[j]] > tin[idx[j - 1]]; j--) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    char text[400];
    size_t o = 0;
    text[0] = 0;
    uint32_t seen = now - t0;
    for (int i = 0; i < ni && i < 5; i++)
        o += (size_t)snprintf(text + o, sizeof text - o, "%s%3.0f%%  %s", i ? "\n" : "", seen ? 100.0 * tin[idx[i]] / seen : 0, l->names[idx[i]]);
    set_long(STS.d_break, o ? text : "nothing recorded in this window");
    /* a state machine's own counters, when it publishes them (CatalystStateMachineLog) */
    if (l->kind == CAT_LANE_MACHINE) {
        char base[96], topic[128];
        snprintf(base, sizeof base, "%s", l->topic);
        base[strlen(base) - 6] = 0;
        snprintf(topic, sizeof topic, "%s/Counters/Transitions", base);
        double tr = ntnum(topic);
        snprintf(topic, sizeof topic, "%s/Counters/Rejections", base);
        double rj = ntnum(topic);
        snprintf(topic, sizeof topic, "%s/Counters/Timeouts", base);
        double to = ntnum(topic);
        snprintf(topic, sizeof topic, "%s/Phase", base);
        char phase[24] = "";
        if (cat_nt()) nt4_get_string(cat_nt(), topic, phase, sizeof phase);
        char t1[16], t2[16], t3[16];
        ui_text(STS.d_counters, "phase %s " MID " the robot counts %s transitions, %s rejected, %s timed out", phase[0] ? phase : DASH,
                bz_fmt(t1, sizeof t1, tr == tr, "%.0f", tr), bz_fmt(t2, sizeof t2, rj == rj, "%.0f", rj), bz_fmt(t3, sizeof t3, to == to, "%.0f", to));
    } else {
        ui_text(STS.d_counters, "%d changes in the window " MID " %d since the tablet started", cat_st_count(l, t0, now + 1), cat_st_count(l, 0, now + 1));
    }
    /* the transition list, newest first */
    uint32_t sig = (uint32_t)l->count ^ l->total << 8 ^ (uint32_t)STS.sel << 24;
    if (sig != STS.d_sig) {
        STS.d_sig = sig;
        lv_obj_clean(STS.d_list);
        time_t tnow = time(NULL);
        for (int k = 0; k < l->count - 1 && k < 40; k++) {
            cat_st_edge_t to_e, from_e;
            cat_st_edge(l, k, &to_e);
            cat_st_edge(l, k + 1, &from_e);
            lv_obj_t *r = bz_row(STS.d_list, 10);
            lv_obj_set_width(r, IN(ST_DETAIL_W));
            /* the tablet's wall clock at the moment it saw the change */
            time_t at = tnow - (time_t)((now - to_e.ms) / 1000);
            struct tm tm;
            localtime_r(&at, &tm);
            snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            lv_obj_t *tl = bz_label(r, b, BZ_F_CAPTION, BZ_C_DIM);
            lv_obj_set_width(tl, 70);
            char line[96];
            snprintf(line, sizeof line, "%s " ARROW " %s", from_e.s >= 0 ? l->names[from_e.s] : "no value", to_e.s >= 0 ? l->names[to_e.s] : "no value");
            lv_obj_t *ll = bz_label_line(r, line, BZ_F_LABEL, to_e.s >= 0 ? BZ_C_INK : BZ_C_DIM, IN(ST_DETAIL_W) - 70 - 80 - 20);
            lv_obj_set_flex_grow(ll, 1);
            lv_obj_t *dl = bz_label(r, fmt_dur(b, sizeof b, (to_e.ms - from_e.ms) / 1000.0), BZ_F_CAPTION, BZ_C_DIM);
            lv_obj_set_width(dl, 80);
            lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_RIGHT, 0);
        }
        if (l->count < 2) bz_label(STS.d_list, "no transitions yet", BZ_F_CAPTION, BZ_C_DIM);
    }
    cat_states_unlock();
}

static void st_build(lv_obj_t *b)
{
    sc_common();
    bz_motion_init(&STS.span, ST_WINDOWS[1] * 1000, 1);
    STS.win = 1;
    STS.sel = 0;
    lv_obj_t *r = head_right(b);
    static const char *const W3[3] = { "1 min", "5 min", "15 min" };
    for (int i = 0; i < 3; i++) STS.chips[i] = ui_chip(r, W3[i], st_window, (void *)(intptr_t)i);
    ui_chip_set(STS.chips[1], true);

    /* the time axis over the lanes' timelines */
    int tl_x = PAD + 22 + ST_TEXT_W + 16, tl_w = ST_LIST_W - 44 - ST_TEXT_W - 16;
    for (int i = 0; i < 3; i++) {
        STS.axis[i] = bz_label(b, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(STS.axis[i], 120);
        lv_obj_set_style_text_align(STS.axis[i], i == 0 ? LV_TEXT_ALIGN_LEFT : i == 1 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(STS.axis[i], i == 0 ? tl_x : i == 1 ? tl_x + tl_w / 2 - 60 : tl_x + tl_w - 120, APP_Y);
    }
    ui_text(STS.axis[0], "\xe2\x80\x93""5 min");
    ui_text(STS.axis[1], "\xe2\x80\x93""2.5 min");
    ui_text(STS.axis[2], "now");
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y + 26);
    STS.list = ui_scroller(wrap, ST_LIST_W, APP_H - 26);
    lv_obj_set_style_pad_row(STS.list, 10, 0);
    STS.empty = empty_tile(b, ST_LIST_W, 150, "only the robot's mode so far",
                           "Every /Catalyst/<mechanism>/State is recorded from the moment the tablet starts, with a "
                           "state machine's Phase and counters, and Autonomy's decisions (/Catalyst/Autonomy/Tasks, "
                           "Chase, Intent, Authority) when the robot publishes them.");
    lv_obj_set_pos(STS.empty, PAD, APP_Y + 26 + ST_LANE_H + 10);
    show(STS.empty, false);

    int x = PAD + ST_LIST_W + BZ_GAP, w = ST_DETAIL_W;
    STS.d_tile = bz_tile(b, w, APP_H);
    lv_obj_set_pos(STS.d_tile, x, APP_Y);
    bz_label(STS.d_tile, "state", BZ_F_LABEL, BZ_C_DIM);
    STS.d_name = bz_label_line(STS.d_tile, "", BZ_F_TITLE, BZ_C_INK, IN(w));
    lv_obj_set_pos(STS.d_name, 0, 22);
    STS.d_now = bz_label_line(STS.d_tile, "", BZ_F_BODY, BZ_C_INK, IN(w));
    lv_obj_set_pos(STS.d_now, 0, 70);
    lv_obj_t *bl = bz_label(STS.d_tile, "time in each state", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(bl, 0, 110);
    STS.d_break = bz_label(STS.d_tile, "", BZ_F_LABEL, BZ_C_INK);
    lv_obj_set_width(STS.d_break, IN(w));
    lv_obj_set_style_text_line_space(STS.d_break, 5, 0);
    lv_obj_set_pos(STS.d_break, 0, 134);
    STS.d_counters = bz_label(STS.d_tile, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(STS.d_counters, IN(w));
    lv_obj_set_pos(STS.d_counters, 0, 262);
    lv_obj_t *tl = bz_label(STS.d_tile, "transitions, newest first", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(tl, 0, 306);
    lv_obj_t *lw = bz_box(STS.d_tile);
    lv_obj_set_pos(lw, 0, 332);
    STS.d_list = ui_scroller(lw, IN(w), APP_H - 2 * BZ_PAD_TILE - 332);
    lv_obj_set_style_pad_row(STS.d_list, 8, 0);
}

static void st_refresh(void)
{
    uint32_t rev = cat_states_rev();
    cat_states_lock();
    int n = cat_states_count();
    uint32_t sig = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) sig = hash_str(sig, cat_states_lane(i)->topic);
    cat_states_unlock();
    if (sig != STS.order_sig) {
        STS.order_sig = sig;
        st_rebuild();
    }
    uint32_t now = cat_states_now();
    cat_states_lock();
    for (int k = 0; k < STS.nlanes; k++) {
        const cat_st_lane_t *l = cat_states_lane(STS.lane[k].lane);
        if (!l) continue;
        const char *cur = cat_st_current(l);
        ui_text(STS.lane[k].state, "%s", cur ? cur : "no value");
        bz_set_color(STS.lane[k].state, cur ? BZ_C_INK : BZ_C_DIM);
        cat_st_edge_t e;
        char b[24];
        uint32_t span = (uint32_t)(ST_WINDOWS[STS.win] * 1000);
        int changes = cat_st_count(l, now > span ? now - span : 0, now + 1);
        if (cat_st_edge(l, 0, &e)) ui_text(STS.lane[k].since, "for %s " MID " %d change%s", fmt_dur(b, sizeof b, (now - e.ms) / 1000.0), changes, changes == 1 ? "" : "s");
        else ui_text(STS.lane[k].since, "not published yet");
    }
    cat_states_unlock();
    /* time flows left: redraw as fast as a pixel moves (10 Hz on the 1 min window, 2 Hz on the rest) */
    double t = ui_now();
    if (rev != STS.rev || t - STS.last_draw >= (STS.win == 0 ? 0.1 : 0.5) || STS.span.running) {
        STS.rev = rev;
        STS.last_draw = t;
        for (int k = 0; k < STS.nlanes; k++) lv_obj_invalidate(STS.lane[k].tl);
    }
    st_detail();
}

static void st_frame(double now, double dt)
{
    (void)now; (void)dt;
    if (bz_motion_tick(&STS.span)) {
        for (int k = 0; k < STS.nlanes; k++) lv_obj_invalidate(STS.lane[k].tl);
        bz_ui_keep_alive();
    }
}

static void st_open(void)
{
    STS.order_sig = 0;
    STS.d_sig = 0;
}

const ui_app_t APP_STATES = { .name = "states", .icon = BZ_I_ACCOUNT_TREE, .build = st_build, .open = st_open,
                              .refresh = st_refresh, .frame = st_frame };

/* ================================================================== controls */

#define CT_PAD_W 640
#define CT_LIST_W (CW - CT_PAD_W - BZ_GAP)

static struct {
    cat_ctl_t b[CAT_CTL_MAX];
    int n;
    uint32_t seq;
    char controllers[4][24];
    int ncontrollers, cur;
    lv_obj_t *head, *chips[4], *chip_row, *pad, *pad_tile, *pad_note, *list, *none;
    struct { lv_obj_t *tile; int b; } rows[CAT_CTL_MAX];
    int nrows, hl;                       /* the binding lit on the pad */
    bz_motion_t ring;                    /* its highlight rings in on `tick` */
    char *json;
} CT;

static bool ct_on_cur(int i) { return !strcmp(CT.b[i].controller, CT.controllers[CT.cur]); }

/* A gamepad outline and every input on it; bound inputs filled with their binding's number. */
static void ct_pad_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float W0 = (float)lv_area_get_width(&a), H0 = (float)lv_area_get_height(&a);
#define PX(fx) (a.x1 + (fx) * W0)
#define PY(fy) (a.y1 + (fy) * H0)
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    /* the body: a wide rounded slab and two grips */
    rd.bg_color = bz_lv(BZ_C_SURFACE2);
    rd.radius = (int32_t)(H0 * 0.2f);
    lv_area_t body = { (int32_t)PX(0.08f), (int32_t)PY(0.18f), (int32_t)PX(0.92f), (int32_t)PY(0.70f) };
    lv_draw_rect(layer, &rd, &body);
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t gl = { (int32_t)PX(0.06f), (int32_t)PY(0.40f), (int32_t)PX(0.30f), (int32_t)PY(0.98f) };
    lv_area_t gr = { (int32_t)PX(0.70f), (int32_t)PY(0.40f), (int32_t)PX(0.94f), (int32_t)PY(0.98f) };
    lv_draw_rect(layer, &rd, &gl);
    lv_draw_rect(layer, &rd, &gr);

    /* every input the pad has, then the bound ones drawn over */
    static const char *const ALL[] = { "L2", "R2", "L1", "R1", "Left stick", "Right stick", "Y", "X", "B", "A", "D-pad up",
                                       "D-pad down", "D-pad left", "D-pad right", "Back", "Start", "Guide", "Touchpad" };
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = bz_font(BZ_F_LABEL);
    ld.align = LV_TEXT_ALIGN_CENTER;
    ld.text_local = 1;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass == 0 ? (int)(sizeof ALL / sizeof ALL[0]) : CT.n;
        for (int i = 0; i < count; i++) {
            const char *name = pass == 0 ? ALL[i] : CT.b[i].control;
            if (pass == 1 && !ct_on_cur(i)) continue;
            float fx, fy;
            cat_input_kind_t k;
            if (!cat_controls_place(name, &fx, &fy, &k)) continue;
            float cx = PX(fx), cy = PY(fy);
            float hw, hh;
            switch (k) {
            case CAT_IN_STICK: hw = hh = H0 * 0.085f; break;
            case CAT_IN_TRIGGER: hw = W0 * 0.07f; hh = H0 * 0.035f; cy += hh; break;
            case CAT_IN_BUMPER: hw = W0 * 0.10f; hh = H0 * 0.03f; break;
            case CAT_IN_DPAD: hw = hh = H0 * 0.036f; break;
            case CAT_IN_PAD: hw = W0 * 0.09f; hh = H0 * 0.05f; break;
            default: hw = hh = H0 * (fy < 0.4f && fx > 0.35f && fx < 0.65f ? 0.028f : 0.042f); break;
            }
            lv_area_t r = { (int32_t)(cx - hw), (int32_t)(cy - hh), (int32_t)(cx + hw), (int32_t)(cy + hh) };
            rd.radius = k == CAT_IN_DPAD || k == CAT_IN_PAD ? 6 : LV_RADIUS_CIRCLE;
            if (pass == 0) {
                rd.bg_color = bz_lv(BZ_C_SURFACE3);
                lv_draw_rect(layer, &rd, &r);
                continue;
            }
            bool lit = i == CT.hl;
            if (lit) {
                /* the highlight: a ring that swells in and settles (tick) */
                float g = 5 + 7 * CT.ring.value;
                lv_area_t ring = { (int32_t)(r.x1 - g), (int32_t)(r.y1 - g), (int32_t)(r.x2 + g), (int32_t)(r.y2 + g) };
                lv_draw_rect_dsc_t rr;
                lv_draw_rect_dsc_init(&rr);
                rr.radius = rd.radius == 6 ? 10 : LV_RADIUS_CIRCLE;
                rr.bg_opa = LV_OPA_TRANSP;
                rr.border_color = bz_lv(BZ_C_SIGNAL);
                rr.border_width = 3;
                rr.border_opa = (lv_opa_t)(255 * fminf(1, fmaxf(0, CT.ring.value)));
                lv_draw_rect(layer, &rr, &ring);
            }
            rd.bg_color = bz_lv(lit ? BZ_C_SIGNAL : BZ_C_ICE);
            lv_draw_rect(layer, &rd, &r);
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            ld.text = num;
            ld.color = bz_lv(lit ? BZ_C_ON_SIGNAL : BZ_C_ON_ICE);
            int th = bz_font(BZ_F_LABEL)->line_height;
            lv_area_t ta = { r.x1 - 10, (int32_t)(cy - th / 2), r.x2 + 10, (int32_t)(cy + th / 2) };
            lv_draw_label(layer, &ld, &ta);
        }
    }
#undef PX
#undef PY
}

static void ct_pick(lv_obj_t *o, void *u)
{
    (void)o;
    int r = (int)(intptr_t)u;
    if (r >= CT.nrows) return;
    CT.hl = CT.rows[r].b;
    for (int i = 0; i < CT.nrows; i++) bz_tile_set_fill(CT.rows[i].tile, i == r ? BZ_C_SURFACE3 : BZ_C_SURFACE1);
    bz_motion_set(&CT.ring, 0, 0);
    bz_motion_to(&CT.ring, 1, BZ_TICK);
    lv_obj_invalidate(CT.pad);
    hal_tone(1700, 10, S.volume * 0.4f);
}

static void ct_list(void)
{
    lv_obj_clean(CT.list);
    CT.nrows = 0;
    for (int i = 0; i < CT.n && CT.nrows < CAT_CTL_MAX; i++) {
        if (!ct_on_cur(i)) continue;
        const cat_ctl_t *c = &CT.b[i];
        lv_obj_t *t = bz_tile(CT.list, CT_LIST_W, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(t, 20, 0);
        lv_obj_set_style_pad_ver(t, 14, 0);
        lv_obj_set_style_pad_hor(t, 18, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, 14, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t, ct_pick, (void *)(intptr_t)CT.nrows);
        /* the number on the pad */
        lv_obj_t *badge = bz_tile(t, 34, 34);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        float fx, fy;
        cat_input_kind_t k;
        bool placed = cat_controls_place(c->control, &fx, &fy, &k);
        bz_tile_set_fill(badge, placed ? BZ_C_ICE : BZ_C_SURFACE3);
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        lv_obj_t *nl = bz_label(badge, num, BZ_F_LABEL, placed ? BZ_C_ON_ICE : BZ_C_INK);
        lv_obj_center(nl);
        lv_obj_t *cl = bz_label(t, c->control, BZ_F_LABEL, BZ_C_INK);
        lv_obj_set_width(cl, 150);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);
        lv_obj_t *al = bz_label(t, c->action, BZ_F_BODY_S, BZ_C_INK);
        lv_obj_set_flex_grow(al, 1);
        lv_label_set_long_mode(al, LV_LABEL_LONG_WRAP);
        if (c->combo) bz_label(t, "combo", BZ_F_CAPTION, BZ_C_DIM);
        CT.rows[CT.nrows].tile = t;
        CT.rows[CT.nrows].b = i;
        CT.nrows++;
    }
}

static void ct_switch(lv_obj_t *o, void *u)
{
    (void)o;
    CT.cur = (int)(intptr_t)u;
    for (int i = 0; i < CT.ncontrollers; i++) ui_chip_set(CT.chips[i], i == CT.cur);
    CT.hl = -1;
    ct_list();
    lv_obj_invalidate(CT.pad);
    hal_tone(1500, 8, S.volume * 0.4f);
}

static void ct_build(lv_obj_t *b)
{
    sc_common();
    CT.json = malloc(16384);
    CT.hl = -1;
    bz_motion_init(&CT.ring, 0, 0.002f);
    lv_obj_t *r = head_right(b);
    CT.chip_row = bz_row(r, 10);
    CT.head = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);

    CT.pad_tile = bz_tile(b, CT_PAD_W, APP_H);
    lv_obj_set_pos(CT.pad_tile, PAD, APP_Y);
    tile_head(CT.pad_tile, "the pad", NULL, NULL);
    CT.pad = bz_box(CT.pad_tile);
    lv_obj_set_size(CT.pad, IN(CT_PAD_W) - 20, 400);
    lv_obj_set_pos(CT.pad, 10, 56);
    lv_obj_add_event_cb(CT.pad, ct_pad_draw, LV_EVENT_DRAW_MAIN, NULL);
    CT.pad_note = bz_label(CT.pad_tile, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(CT.pad_note, IN(CT_PAD_W));
    lv_obj_set_style_text_line_space(CT.pad_note, 4, 0);
    lv_obj_align(CT.pad_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD + CT_PAD_W + BZ_GAP, APP_Y);
    CT.list = ui_scroller(wrap, CT_LIST_W, APP_H);
    lv_obj_set_style_pad_row(CT.list, 10, 0);

    CT.none = bz_tile(b, CT_LIST_W, APP_H);
    lv_obj_set_pos(CT.none, PAD + CT_PAD_W + BZ_GAP, APP_Y);
    bz_label(CT.none, "no controls manifest", BZ_F_NAME, BZ_C_INK);
    lv_obj_t *l = bz_label(CT.none, "What each button does is the robot's own wiring, so it comes from the robot: one "
                                    "JSON topic, read and never written. Publish it and every driver who picks up the "
                                    "pad can check it here and in Console's Drivers panel.",
                           BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(l, IN(CT_LIST_W));
    lv_obj_set_pos(l, 0, 40);
    lv_obj_t *code = bz_label(CT.none, CAT_CTL_TOPIC "\n[{\"control\": \"R1\",\n  \"action\": \"Turret mode while held\",\n"
                                       "  \"controller\": \"Driver\",\n  \"combo\": false}, \xe2\x80\xa6]",
                              BZ_F_LABEL, BZ_C_INK);
    lv_obj_set_width(code, IN(CT_LIST_W));
    lv_obj_set_style_text_line_space(code, 6, 0);
    lv_obj_align(code, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void ct_refresh(void)
{
    const cat_robot_t *r = R;
    char type[24];
    uint32_t seq = 0;
    int64_t age;
    bool have = cat_nt() && r->connected && nt4_info(cat_nt(), CAT_CTL_TOPIC, type, sizeof type, &seq, &age) && seq;
    if (!have) seq = 0;
    if (seq != CT.seq) {
        CT.seq = seq;
        CT.n = 0;
        if (have && CT.json && nt4_get_string(cat_nt(), CAT_CTL_TOPIC, CT.json, 16384)) {
            int n = cat_controls_parse(CT.json, strlen(CT.json), CT.b, CAT_CTL_MAX);
            CT.n = n > 0 ? n : 0;
        }
        /* the controllers it names, in order of first mention */
        CT.ncontrollers = 0;
        for (int i = 0; i < CT.n; i++) {
            int k;
            for (k = 0; k < CT.ncontrollers; k++) if (!strcmp(CT.controllers[k], CT.b[i].controller)) break;
            if (k == CT.ncontrollers && k < 4) snprintf(CT.controllers[CT.ncontrollers++], sizeof CT.controllers[0], "%s", CT.b[i].controller);
        }
        if (!CT.ncontrollers) snprintf(CT.controllers[CT.ncontrollers++], sizeof CT.controllers[0], "Driver");
        if (CT.cur >= CT.ncontrollers) CT.cur = 0;
        lv_obj_clean(CT.chip_row);
        for (int i = 0; i < CT.ncontrollers && CT.ncontrollers > 1; i++) {
            char nm[24];
            snprintf(nm, sizeof nm, "%s", CT.controllers[i]);
            for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
            CT.chips[i] = ui_chip(CT.chip_row, nm, ct_switch, (void *)(intptr_t)i);
            ui_chip_set(CT.chips[i], i == CT.cur);
        }
        CT.hl = -1;
        ct_list();
        show(lv_obj_get_parent(lv_obj_get_parent(CT.list)), CT.n > 0);
        show(CT.none, CT.n == 0);
        lv_obj_invalidate(CT.pad);
    }
    int bound = 0, unplaced = 0;
    for (int i = 0; i < CT.n; i++) {
        if (!ct_on_cur(i)) continue;
        bound++;
        float x, y;
        cat_input_kind_t k;
        unplaced += !cat_controls_place(CT.b[i].control, &x, &y, &k);
    }
    if (CT.n) ui_text(CT.head, "%s", CT.ncontrollers > 1 ? "" : "driver");
    else ui_text(CT.head, "%s", r->connected ? "not published" : "no robot");
    char kind[48] = "";
    if (r->have_controller) snprintf(kind, sizeof kind, "%s %s", r->controller_kind, r->controller_connected ? "connected" : "not connected");
    char who[24];
    snprintf(who, sizeof who, "%s", CT.controllers[CT.cur]);
    for (char *p = who; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    ui_text(CT.pad_note, "%d binding%s on the %s's controller%s%s\n%s", bound, bound == 1 ? "" : "s", who, unplaced ? " " MID " " : "",
            unplaced ? "some only in the list (not on this layout)" : "", kind[0] ? kind : "the robot doesn't say which controller is plugged in");
}

static void ct_frame(double now, double dt)
{
    (void)now; (void)dt;
    if (bz_motion_tick(&CT.ring)) {
        lv_obj_invalidate(CT.pad);
        bz_ui_keep_alive();
    }
}

static void ct_open(void) { CT.seq = UINT32_MAX; }

const ui_app_t APP_CONTROLS = { .name = "controls", .icon = BZ_I_SPORTS_ESPORTS, .build = ct_build, .open = ct_open,
                                .refresh = ct_refresh, .frame = ct_frame };

/* ================================================================== recorder */

#define RC_LEFT_W 392
#define RC_MID_W 420
#define RC_RIGHT_W (CW - RC_LEFT_W - RC_MID_W - 2 * BZ_GAP)
#define RC_MID_X (PAD + RC_LEFT_W + BZ_GAP)
#define RC_RIGHT_X (RC_MID_X + RC_MID_W + BZ_GAP)
#define RC_SPARKS 4
#define RC_RUNS 24
#define RC_PRESETS (3 + CAT_MAX_MECHS)
#define RC_TOPICS 96

enum { P_DRIVE, P_POWER, P_CUSTOM, P_MECH0 };

static struct {
    int preset;
    cat_rec_col_t cols[CAT_REC_COLS];
    int ncols;
    int keys[RC_SPARKS];                 /* the columns the sparklines follow */
    lv_obj_t *preset_row, *preset_chips[RC_PRESETS], *chan_title, *chan_list, *custom_wrap, *custom_list;
    int npresets;
    uint32_t mech_sig;
    /* custom: the announced numeric topics and which are chosen */
    char topics[RC_TOPICS][96];
    bool chosen[RC_TOPICS];
    int ntopics;
    lv_obj_t *topic_marks[RC_TOPICS], *topic_names[RC_TOPICS];
    /* the run */
    lv_obj_t *state, *elapsed, *stats, *file, *spark[RC_SPARKS], *spark_name[RC_SPARKS], *spark_val[RC_SPARKS];
    lv_obj_t *mid;
    cat_rec_status_t st;
    bool was_active;
    /* glass: the record button and mark, floating over the run tile */
    lv_obj_t *rec_btn, *rec_dot, *mark_btn, *mark_label;
    bz_glass_t *rec_glass, *mark_glass;
    bz_motion_t morph;                   /* 0 a round record dot, 1 a square stop */
    bool glass_on;
    /* runs on the card */
    lv_obj_t *runs_title, *runs;
    cat_run_t list[RC_RUNS];
    int nruns;
    volatile int upload_result;          /* 0 idle, 1 sending, 2 sent, 3 refused */
    char upload_name[48], upload_path[160];   /* copies: the list may be rebuilt mid-send */
    bool built;
    lv_obj_t *body;
} RC;

/* A mechanism's topics, in the order a person reads them: where it is, where it's going, how hard. */
static const char *const MECH_KEYS[] = { "AngleDegrees", "PositionMeters", "VelocityRPS", "SetpointDegrees", "SetpointMeters",
                                         "SetpointRPS", "CurrentAmps", "StatorCurrentAmps", "TemperatureC", "AtSetpoint", "AtSpeed" };
static const char *const BATTERY_KEYS[] = { "/Catalyst/Status/BatteryVolts", "/Catalyst/Brownout/MeasuredVoltage",
                                            "/Catalyst/Systemcore/BatteryVolts" };

static bool announced(const char *topic)
{
    char type[24];
    uint32_t seq;
    int64_t age;
    return cat_nt() && nt4_info(cat_nt(), topic, type, sizeof type, &seq, &age);
}

/* Adds a scalar, or each element of a numeric array (as it is right now), as columns. */
static int rc_add(const char *topic, const char *const *element_names, int max_elems)
{
    if (!announced(topic) || RC.ncols >= CAT_REC_COLS) return 0;
    char type[24];
    uint32_t seq;
    int64_t age;
    nt4_info(cat_nt(), topic, type, sizeof type, &seq, &age);
    bool array = strchr(type, '[') || !strncmp(type, "struct:", 7);
    if (!array) {
        cat_rec_col_t *c = &RC.cols[RC.ncols++];
        snprintf(c->topic, sizeof c->topic, "%s", topic);
        c->index = -1;
        snprintf(c->label, sizeof c->label, "%s", topic + (strncmp(topic, "/Catalyst/", 10) ? 0 : 10));
        return 1;
    }
    double v[32];
    int n = nt4_get_numbers(cat_nt(), topic, v, 32);
    if (n > max_elems) n = max_elems;
    int added = 0;
    for (int i = 0; i < n && RC.ncols < CAT_REC_COLS; i++) {
        cat_rec_col_t *c = &RC.cols[RC.ncols++];
        snprintf(c->topic, sizeof c->topic, "%s", topic);
        c->index = i;
        const char *short_name = topic + (strncmp(topic, "/Catalyst/", 10) ? 0 : 10);
        if (element_names && element_names[i]) snprintf(c->label, sizeof c->label, "%.24s.%s", short_name, element_names[i]);
        else snprintf(c->label, sizeof c->label, "%.32s[%d]", short_name, i);
        added++;
    }
    return added;
}

static void rc_battery(void)
{
    for (int i = 0; i < 3; i++)
        if (rc_add(BATTERY_KEYS[i], NULL, 1)) return;
}

static void rc_columns(void)
{
    RC.ncols = 0;
    for (int i = 0; i < RC_SPARKS; i++) RC.keys[i] = -1;
    int k = 0;
#define KEY() do { if (k < RC_SPARKS && RC.ncols > 0) RC.keys[k++] = RC.ncols - 1; } while (0)
    if (RC.preset == P_DRIVE) {
        static const char *const MOD[] = { "fl_speed", "fl_angle", "fr_speed", "fr_angle", "bl_speed", "bl_angle", "br_speed", "br_angle" };
        static const char *const POSE[] = { "x", "y", "rad" };
        if (rc_add("/Catalyst/Swerve/ModuleStates", MOD, 8)) RC.keys[k++] = RC.ncols - 8 < 0 ? 0 : RC.ncols - 8;
        if (rc_add("/Catalyst/Swerve/ModuleTargets", MOD, 8) && k < RC_SPARKS) RC.keys[k++] = RC.ncols - 8 < 0 ? 0 : RC.ncols - 8;
        if (rc_add("/Catalyst/Swerve/HeadingDeg", NULL, 1)) KEY();
        if (!rc_add("/Catalyst/Physics/PoseArray", POSE, 3)) rc_add("/Catalyst/Swerve/Pose", POSE, 3);
        rc_battery();
        KEY();
        rc_add("/Catalyst/Brownout/TotalCurrent", NULL, 1);
    } else if (RC.preset == P_POWER) {
        rc_battery();
        KEY();
        if (rc_add("/Catalyst/Brownout/TotalCurrent", NULL, 1)) KEY();
        if (rc_add("/Catalyst/Brownout/PredictedVoltage", NULL, 1)) KEY();
        rc_add("/Catalyst/Brownout/AtRisk", NULL, 1);
        rc_add("/Catalyst/Systemcore/BrownedOut", NULL, 1);
        if (rc_add("/Catalyst/Loop/Robot/LastMs", NULL, 1)) KEY();
        static const char *const BUS[] = { "s0", "s1", "s2", "s3", "s4" };
        rc_add("/Catalyst/Systemcore/CanUtilization", BUS, 5);
        rc_add("/SmartDashboard/PDH/Voltage", NULL, 1);
        rc_add("/SmartDashboard/PDH/TotalCurrent", NULL, 1);
        for (int ch = 0; ch < 24; ch++) {
            char t[48];
            snprintf(t, sizeof t, "/SmartDashboard/PDH/Chan%d", ch);
            rc_add(t, NULL, 1);
        }
    } else if (RC.preset == P_CUSTOM) {
        for (int i = 0; i < RC.ntopics; i++)
            if (RC.chosen[i] && rc_add(RC.topics[i], NULL, 16)) KEY();
    } else {
        int m = RC.preset - P_MECH0;
        if (m < R->nmechs) {
            for (size_t i = 0; i < sizeof MECH_KEYS / sizeof MECH_KEYS[0]; i++) {
                char t[96];
                snprintf(t, sizeof t, "/Catalyst/%s/%s", R->mechs[m].name, MECH_KEYS[i]);
                if (rc_add(t, NULL, 1) && strncmp(MECH_KEYS[i], "At", 2)) KEY();
            }
            rc_battery();
        }
    }
#undef KEY
    /* fewer keys than sparklines: the first columns fill in */
    for (int i = 0; i < RC_SPARKS && k < RC_SPARKS && i < RC.ncols; i++) {
        bool used = false;
        for (int j = 0; j < k; j++) used |= RC.keys[j] == i;
        if (!used) RC.keys[k++] = i;
    }
}

static void rc_channels_show(void)
{
    if (RC.preset == P_CUSTOM) {
        ui_text(RC.chan_title, "%d chosen " MID " tap to add or remove", RC.ncols);
        show(RC.chan_list, false);
        show(RC.custom_wrap, true);
        return;
    }
    show(RC.custom_wrap, false);
    show(RC.chan_list, true);
    ui_text(RC.chan_title, "%d channel%s at %d hz", RC.ncols, RC.ncols == 1 ? "" : "s", CAT_REC_HZ);
    char text[1024];
    size_t o = 0;
    text[0] = 0;
    int shown = 0;
    for (int i = 0; i < RC.ncols && shown < 14; i++, shown++)
        o += (size_t)snprintf(text + o, sizeof text - o, "%s%s", i ? "\n" : "", RC.cols[i].label);
    if (RC.ncols > shown) snprintf(text + o, sizeof text - o, "\nand %d more", RC.ncols - shown);
    set_long(RC.chan_list, RC.ncols ? text : "nothing this preset reads is published by the robot");
}

static void rc_sparks_reset(void)
{
    for (int i = 0; i < RC_SPARKS; i++) {
        bz_spark_clear(RC.spark[i]);
        int c = RC.keys[i];
        show(lv_obj_get_parent(RC.spark[i]), c >= 0);
        if (c >= 0) ui_text(RC.spark_name[i], "%s", RC.cols[c].label);
    }
}

static void rc_preset(lv_obj_t *o, void *u)
{
    (void)o;
    cat_rec_status_t st;
    cat_rec_status(&st);
    if (st.active) {
        ui_island_say(BZ_I_FIBER_MANUAL_RECORD, "stop the run to change what it records");
        return;
    }
    RC.preset = (int)(intptr_t)u;
    for (int i = 0; i < RC.npresets; i++) ui_chip_set(RC.preset_chips[i], i == RC.preset);
    rc_columns();
    rc_channels_show();
    rc_sparks_reset();
    hal_tone(1500, 8, S.volume * 0.4f);
}

static void rc_topic_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i >= RC.ntopics) return;
    RC.chosen[i] = !RC.chosen[i];
    bz_icon_set(RC.topic_marks[i], RC.chosen[i] ? BZ_I_CHECK_CIRCLE : BZ_I_RADIO_BUTTON_UNCHECKED, 24, false);
    bz_set_color(RC.topic_marks[i], RC.chosen[i] ? BZ_C_ICE : BZ_C_FAINT);
    bz_set_color(RC.topic_names[i], RC.chosen[i] ? BZ_C_INK : BZ_C_DIM);
    hal_tone(RC.chosen[i] ? 1700 : 1300, 8, S.volume * 0.4f);
    rc_columns();
    rc_channels_show();
    rc_sparks_reset();
}

typedef struct { char (*names)[96]; int n, max; } topics_t;

static void rc_visit(const char *name, const char *type, void *user)
{
    topics_t *t = user;
    if (t->n >= t->max) return;
    /* numbers only: what a CSV column can hold */
    if (strcmp(type, "double") && strcmp(type, "float") && strcmp(type, "int") && strcmp(type, "boolean") && strcmp(type, "double[]"))
        return;
    snprintf(t->names[t->n++], 96, "%s", name);
}

static int rc_name_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void rc_custom_list(void)
{
    lv_obj_clean(RC.custom_list);
    topics_t t = { RC.topics, 0, RC_TOPICS };
    if (cat_nt()) nt4_list(cat_nt(), "/Catalyst/", rc_visit, &t);
    qsort(RC.topics, (size_t)t.n, sizeof RC.topics[0], rc_name_cmp);
    RC.ntopics = t.n;
    memset(RC.chosen, 0, sizeof RC.chosen);
    for (int i = 0; i < RC.ntopics; i++) {
        lv_obj_t *r = bz_row(RC.custom_list, 10);
        lv_obj_set_width(r, IN(RC_LEFT_W));
        lv_obj_set_height(r, 34);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(r, 4);
        bz_on_tap(r, rc_topic_tap, (void *)(intptr_t)i);
        RC.topic_marks[i] = bz_icon(r, BZ_I_RADIO_BUTTON_UNCHECKED, 24, BZ_C_FAINT);
        RC.topic_names[i] = bz_label_line(r, RC.topics[i] + 10, BZ_F_CAPTION, BZ_C_DIM, IN(RC_LEFT_W) - 34);
    }
    if (!RC.ntopics) bz_label(RC.custom_list, "no numeric topics announced", BZ_F_CAPTION, BZ_C_DIM);
}

static void rc_presets(void)
{
    const cat_robot_t *r = R;
    uint32_t sig = 2166136261u ^ (uint32_t)r->nmechs ^ (uint32_t)r->connected << 30;
    for (int i = 0; i < r->nmechs; i++) sig = hash_str(sig, r->mechs[i].name);
    if (sig == RC.mech_sig) return;
    RC.mech_sig = sig;
    lv_obj_clean(RC.preset_row);
    static const char *const FIXED[3] = { "drivetrain", "power", "custom" };
    RC.npresets = 0;
    for (int i = 0; i < 3 + r->nmechs && i < RC_PRESETS; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "%s", i < 3 ? FIXED[i] : r->mechs[i - 3].name);
        for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        RC.preset_chips[i] = ui_chip(RC.preset_row, nm, rc_preset, (void *)(intptr_t)i);
        RC.npresets++;
    }
    if (RC.preset >= RC.npresets) RC.preset = P_DRIVE;
    for (int i = 0; i < RC.npresets; i++) ui_chip_set(RC.preset_chips[i], i == RC.preset);
    rc_custom_list();
    rc_columns();
    rc_channels_show();
    rc_sparks_reset();
}

static void rc_runs(void)
{
    lv_obj_clean(RC.runs);
    RC.nruns = cat_runs_list(RC.list, RC_RUNS);
    if (RC.nruns < 0) {
        ui_text(RC.runs_title, "no microSD card");
        bz_label(RC.runs, "Runs are written to runs/ on the card.", BZ_F_CAPTION, BZ_C_DIM);
        return;
    }
    ui_text(RC.runs_title, "%d run%s on the card", RC.nruns, RC.nruns == 1 ? "" : "s");
}

static void rc_send(lv_obj_t *o, void *u);

static void rc_runs_fill(void)
{
    rc_runs();
    for (int i = 0; i < RC.nruns; i++) {
        const cat_run_t *run = &RC.list[i];
        lv_obj_t *t = bz_tile(RC.runs, IN(RC_RIGHT_W), LV_SIZE_CONTENT);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_radius(t, 18, 0);
        lv_obj_set_style_pad_all(t, 12, 0);
        lv_obj_set_style_pad_left(t, 16, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, 10, 0);
        lv_obj_t *c = bz_col(t, 2);
        lv_obj_set_flex_grow(c, 1);
        /* run-YYYYMMDD-HHMMSS.csv → "14 mar · 10:21:44" */
        int y, mo, d, hh, mm, ss;
        char when[48];
        if (sscanf(run->name, "run-%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &hh, &mm, &ss) == 6) {
            static const char *const M[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
            snprintf(when, sizeof when, "%02d:%02d:%02d " MID " %d %s", hh, mm, ss, d, M[(mo + 11) % 12]);
        } else {
            snprintf(when, sizeof when, "%s", run->name);
        }
        bz_label_line(c, when, BZ_F_BODY_S, BZ_C_INK, IN(RC_RIGHT_W) - 110);
        char dur[24], size[24], sub[80];
        snprintf(sub, sizeof sub, "%s " MID " %d ch " MID " %s", fmt_dur(dur, sizeof dur, run->duration_s), run->channels,
                 fmt_bytes(size, sizeof size, (double)run->bytes));
        bz_label_line(c, sub, BZ_F_CAPTION, BZ_C_DIM, IN(RC_RIGHT_W) - 110);
        lv_obj_t *send = ui_button(t, BZ_I_SEND, NULL, rc_send, (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(send, 16, 0);
        bz_tile_set_fill(send, BZ_C_SURFACE3);
    }
    if (!RC.nruns) {
        lv_obj_t *l = bz_label(RC.runs, "Nothing recorded yet. Pick what to record and tap the button.", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(l, IN(RC_RIGHT_W));
    }
}

static void *rc_upload_thread(void *arg)
{
    (void)arg;
    /* link_upload queues on microSD when the Link is away; false means it couldn't even do that */
    RC.upload_result = link_upload(RC.upload_path, RC.upload_name) ? 2 : 3;
    return NULL;
}

static void rc_send(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i >= RC.nruns || RC.upload_result == 1) return;
    snprintf(RC.upload_name, sizeof RC.upload_name, "%s", RC.list[i].name);
    snprintf(RC.upload_path, sizeof RC.upload_path, "%s", RC.list[i].path);
    RC.upload_result = 1;
    if (!hal_thread("rc-upload", rc_upload_thread, NULL, 8192)) RC.upload_result = 3;
    hal_tone(1800, 10, S.volume * 0.4f);
}

static void rc_record(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cat_rec_status_t st;
    cat_rec_status(&st);
    if (st.active) {
        cat_rec_stop();
        hal_tone(660, 80, S.volume);
        return;
    }
    if (!R->connected) {
        ui_island_say(BZ_I_LINK_OFF, "no robot to record");
        return;
    }
    rc_columns();
    char err[80];
    if (!cat_rec_start(RC.cols, RC.ncols, err, sizeof err)) {
        ui_island_say(BZ_I_SD_CARD, err);
        hal_tone(330, 200, S.volume);
        return;
    }
    rc_sparks_reset();
    hal_tone(990, 60, S.volume);
    hal_tone(1320, 90, S.volume);
}

static void rc_mark(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cat_rec_status_t st;
    cat_rec_status(&st);
    if (!st.active) {
        ui_island_say(BZ_I_BOOKMARK, "marks go into a run: start recording first");
        return;
    }
    cat_rec_mark();
    char msg[32];
    snprintf(msg, sizeof msg, "mark %u", (unsigned)st.marks + 1);
    ui_island_say(BZ_I_BOOKMARK, msg);
    hal_tone(2200, 25, S.volume * 0.6f);
}

/* The glass rides with the app: it materialises when the recorder opens and dissolves as it closes, and
 * the record dot morphs into a stop square on `detent` while a run is going. */
static void rc_glass_frame(double now)
{
    (void)now;
    if (!RC.built) return;
    /* shown once the window has nearly finished growing, gone as soon as it starts to shrink: the glass
     * floats over the run tile, never over the page the window grew out of */
    lv_obj_t *win = lv_obj_get_parent(RC.body);
    bool open = ui_app_is_open(&APP_RECORDER) && win && lv_obj_get_width(win) > W * 0.85f;
    if (open != RC.glass_on) {
        RC.glass_on = open;
        if (open) {
            show(RC.rec_btn, true);
            show(RC.mark_btn, true);
        }
        bz_glass_show(RC.rec_glass, open);
        bz_glass_show(RC.mark_glass, open);
    }
    if (!open && bz_glass_strength(RC.rec_glass) < 0.01f && !lv_obj_has_flag(RC.rec_btn, LV_OBJ_FLAG_HIDDEN)) {
        show(RC.rec_btn, false);
        show(RC.mark_btn, false);
    }
    cat_rec_status_t st;
    cat_rec_status(&st);
    if ((st.active ? 1.0f : 0.0f) != RC.morph.target) {
        bz_motion_to(&RC.morph, st.active ? 1 : 0, BZ_DETENT);
        bz_glass_set_tint(RC.rec_glass, 4, st.active ? 0.45f : 0);
    }
    if (bz_motion_tick(&RC.morph) || RC.morph.running) {
        float k = RC.morph.value;
        int size = (int)(46 - 12 * k);
        lv_obj_set_size(RC.rec_dot, size, size);
        lv_obj_set_style_radius(RC.rec_dot, (int)(size / 2 * (1 - k) + 8 * k), 0);
        lv_obj_center(RC.rec_dot);
        bz_ui_keep_alive();
    }
}

static void rc_build(lv_obj_t *b)
{
    sc_common();
    lv_obj_t *hr = head_right(b);
    RC.state = bz_label(hr, "", BZ_F_LABEL, BZ_C_DIM);

    /* what to record */
    lv_obj_t *left = bz_tile(b, RC_LEFT_W, APP_H);
    lv_obj_set_pos(left, PAD, APP_Y);
    bz_label(left, "what to record", BZ_F_LABEL, BZ_C_DIM);
    RC.preset_row = bz_row(left, 10);
    lv_obj_set_flex_flow(RC.preset_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(RC.preset_row, 10, 0);
    lv_obj_set_width(RC.preset_row, IN(RC_LEFT_W));
    lv_obj_set_pos(RC.preset_row, 0, 30);
    RC.chan_title = bz_label(left, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(RC.chan_title, 0, 238);
    RC.chan_list = bz_label(left, "", BZ_F_CAPTION, BZ_C_INK);
    lv_obj_set_width(RC.chan_list, IN(RC_LEFT_W));
    lv_obj_set_style_text_line_space(RC.chan_list, 3, 0);
    lv_obj_set_pos(RC.chan_list, 0, 266);
    RC.custom_wrap = bz_box(left);
    lv_obj_set_pos(RC.custom_wrap, 0, 266);
    RC.custom_list = ui_scroller(RC.custom_wrap, IN(RC_LEFT_W), APP_H - 2 * BZ_PAD_TILE - 266);
    lv_obj_set_style_pad_row(RC.custom_list, 2, 0);
    lv_obj_set_size(RC.custom_wrap, IN(RC_LEFT_W), APP_H - 2 * BZ_PAD_TILE - 266);
    show(RC.custom_wrap, false);

    /* the run */
    RC.mid = bz_tile(b, RC_MID_W, APP_H);
    lv_obj_set_pos(RC.mid, RC_MID_X, APP_Y);
    bz_label(RC.mid, "black box " MID " 50 hz to microsd", BZ_F_LABEL, BZ_C_DIM);
    RC.elapsed = bz_label(RC.mid, "0:00.0", BZ_F_DISPLAY, BZ_C_INK);
    lv_obj_set_pos(RC.elapsed, -4, 26);
    RC.stats = bz_label_line(RC.mid, "", BZ_F_LABEL, BZ_C_INK, IN(RC_MID_W));
    lv_obj_set_pos(RC.stats, 0, 106);
    RC.file = bz_label_line(RC.mid, "", BZ_F_CAPTION, BZ_C_DIM, IN(RC_MID_W));
    lv_obj_set_pos(RC.file, 0, 130);
    for (int i = 0; i < RC_SPARKS; i++) {
        lv_obj_t *c = bz_box(RC.mid);
        lv_obj_set_size(c, IN(RC_MID_W), 60);
        lv_obj_set_pos(c, 0, 162 + i * 64);
        RC.spark_name[i] = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_DIM, IN(RC_MID_W) - 100);
        RC.spark_val[i] = bz_label(c, "", BZ_F_LABEL, BZ_C_INK);
        lv_obj_align(RC.spark_val[i], LV_ALIGN_TOP_RIGHT, 0, -2);
        RC.spark[i] = bz_spark(c, IN(RC_MID_W), 40, 150);
        lv_obj_align(RC.spark[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        bz_spark_color(RC.spark[i], i == 0 ? BZ_C_INK : BZ_C_ICE);
    }

    /* runs on the card */
    lv_obj_t *right = bz_tile(b, RC_RIGHT_W, APP_H);
    lv_obj_set_pos(right, RC_RIGHT_X, APP_Y);
    RC.runs_title = bz_label(right, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *rw = bz_box(right);
    lv_obj_set_pos(rw, 0, 30);
    RC.runs = ui_scroller(rw, IN(RC_RIGHT_W), APP_H - 2 * BZ_PAD_TILE - 30);
    lv_obj_set_style_pad_row(RC.runs, 10, 0);

    /* the glass: a big round record button and a mark capsule, on the glass layer over the run tile */
    lv_obj_t *g = bz_ui_glass();
    int by = APP_Y + APP_H - BZ_PAD_TILE - 112;
    RC.rec_btn = lv_obj_create(g);
    lv_obj_remove_style_all(RC.rec_btn);
    lv_obj_set_size(RC.rec_btn, 112, 112);
    lv_obj_set_pos(RC.rec_btn, RC_MID_X + BZ_PAD_TILE, by);
    lv_obj_remove_flag(RC.rec_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(RC.rec_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(RC.rec_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    RC.rec_glass = bz_glass_attach(RC.rec_btn, 5, 56);
    bz_glass_show(RC.rec_glass, false);
    RC.rec_dot = lv_obj_create(RC.rec_btn);
    lv_obj_remove_style_all(RC.rec_dot);
    lv_obj_add_style(RC.rec_dot, bz_style_fill(BZ_C_FAULT), 0);
    lv_obj_set_size(RC.rec_dot, 46, 46);
    lv_obj_set_style_radius(RC.rec_dot, 23, 0);
    lv_obj_remove_flag(RC.rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(RC.rec_dot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(RC.rec_dot);
    bz_on_tap(RC.rec_btn, rc_record, NULL);
    bz_motion_init(&RC.morph, 0, 0.002f);

    RC.mark_btn = bz_row(g, 10);
    lv_obj_set_height(RC.mark_btn, 72);
    lv_obj_set_style_pad_hor(RC.mark_btn, 26, 0);
    lv_obj_set_pos(RC.mark_btn, RC_MID_X + BZ_PAD_TILE + 112 + 28, by + 20);
    lv_obj_add_flag(RC.mark_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(RC.mark_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    RC.mark_glass = bz_glass_attach(RC.mark_btn, 5, 36);
    bz_glass_show(RC.mark_glass, false);
    bz_icon(RC.mark_btn, BZ_I_BOOKMARK, 24, BZ_C_INK);
    RC.mark_label = bz_label(RC.mark_btn, "mark", BZ_F_LABEL, BZ_C_INK);
    bz_on_tap(RC.mark_btn, rc_mark, NULL);
    show(RC.rec_btn, false);
    show(RC.mark_btn, false);

    RC.preset = P_DRIVE;
    RC.mech_sig = 1;
    RC.body = b;
    RC.built = true;
}

static void rc_refresh(void)
{
    rc_presets();
    cat_rec_status(&RC.st);
    const cat_rec_status_t *st = &RC.st;
    char b[32], sz[24];
    if (st->active) {
        int mins = (int)(st->elapsed / 60);
        ui_text(RC.elapsed, "%d:%04.1f", mins, st->elapsed - mins * 60);
        bz_set_color(RC.elapsed, BZ_C_INK);
        ui_text(RC.state, "recording");
        ui_text(RC.stats, "%u rows " MID " %s " MID " %u mark%s", (unsigned)st->rows, fmt_bytes(sz, sizeof sz, (double)st->bytes),
                (unsigned)st->marks, st->marks == 1 ? "" : "s");
    } else {
        ui_text(RC.state, "%s", st->error[0] ? st->error : R->connected ? "ready" : "no robot");
        if (!st->path[0]) {
            ui_text(RC.elapsed, "0:00.0");
            bz_set_color(RC.elapsed, BZ_C_FAINT);
            ui_text(RC.stats, "ready " MID " 50 hz rows, a mark column");
        } else {
            ui_text(RC.stats, "saved " MID " %u rows " MID " %s", (unsigned)st->rows, fmt_bytes(sz, sizeof sz, (double)st->bytes));
        }
    }
    const char *root = hal_sd_root();
    ui_text(RC.file, "%s", st->path[0] ? st->path + (root && !strncmp(st->path, root, strlen(root)) ? strlen(root) + 1 : 0)
                                       : root ? "runs/run-yyyymmdd-hhmmss.csv" : "no microSD card");
    /* a run just ended: it's on the card now */
    if (RC.was_active && !st->active) {
        rc_runs_fill();
        char msg[64];
        snprintf(msg, sizeof msg, "saved %s", fmt_dur(b, sizeof b, st->elapsed));
        ui_island_say(BZ_I_SAVE, msg);
    }
    RC.was_active = st->active;
    /* sparklines: the run's own rows while recording, a live preview from NT before */
    double v[CAT_REC_COLS];
    bool have[CAT_REC_COLS];
    int n = 0;
    if (st->active) n = cat_rec_latest(v, have, CAT_REC_COLS);
    for (int i = 0; i < RC_SPARKS; i++) {
        int c = RC.keys[i];
        if (c < 0) continue;
        double x = NAN;
        if (st->active) {
            if (c < n && have[c]) x = v[c];
        } else if (R->connected) {
            const cat_rec_col_t *col = &RC.cols[c];
            double arr[32];
            if (col->index < 0) x = ntnum(col->topic);
            else if (cat_nt() && nt4_get_numbers(cat_nt(), col->topic, arr, 32) > col->index) x = arr[col->index];
        }
        if (x == x) bz_spark_push(RC.spark[i], (float)x);
        ui_text(RC.spark_val[i], "%s", bz_fmt(b, sizeof b, x == x, fabs(x) >= 100 ? "%.0f" : "%.2f", x));
    }
    /* send to PC */
    if (RC.upload_result >= 2) {
        char msg[80];
        if (RC.upload_result == 2) snprintf(msg, sizeof msg, "%s \xe2\x86\x92 pc", RC.upload_name);
        else snprintf(msg, sizeof msg, "Link not connected");
        ui_island_say(RC.upload_result == 2 ? BZ_I_CLOUD_UPLOAD : BZ_I_CLOUD_OFF, msg);
        RC.upload_result = 0;
    }
}

static void rc_open(void)
{
    rc_runs_fill();
    cat_rec_status(&RC.st);
    RC.was_active = RC.st.active;
}

const ui_app_t APP_RECORDER = { .name = "recorder", .icon = BZ_I_FIBER_MANUAL_RECORD, .build = rc_build, .open = rc_open,
                                .refresh = rc_refresh };
