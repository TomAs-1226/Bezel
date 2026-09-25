/* The tablet's own utilities, the ones a pocket tool needs whatever the robot is doing: a match timer and
 * stopwatch, a calculator with the unit conversions a pit asks for, pit notes, a pit checklist, a
 * flashlight, the tablet's own system state, and the microSD card's files. */
#include "ui_internal.h"
#include "ui_boot.h" /* CATALYST_TAB_VERSION */
#include "ui_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)

static lv_obj_t *head_right(lv_obj_t *body)
{
    lv_obj_t *r = bz_row(body, 14);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(r, 60);
    return r;
}

/* A row of chips that wraps inside `w` rather than running out of its tile. */
static lv_obj_t *chip_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static void beep(float hz, int ms) { hal_tone(hz, ms, S.volume * 0.6f); }

/* ================================================================== timer */

enum { TM_MATCH, TM_STOPWATCH, TM_COUNTDOWN };

static struct {
    lv_obj_t *mode_chips[3], *big, *phase, *sub, *start_lbl, *laps, *presets, *auto_chips[2];
    int mode;
    bool running;
    double started, acc;       /* running since, and time banked before it */
    double count_s;            /* the countdown's length */
    int auto_s;                /* the match's autonomous period */
    int last_beep;
    int nlaps;
} TM = { .count_s = 120, .auto_s = 20, .last_beep = -1 };

#define TELEOP_S 140

static double tm_elapsed(void) { return TM.acc + (TM.running ? hal_seconds() - TM.started : 0); }

static void tm_show(void)
{
    double e = tm_elapsed();
    char b[32];
    bz_color_role_t col = BZ_C_INK;
    if (TM.mode == TM_STOPWATCH) {
        int cs = (int)(e * 100);
        snprintf(b, sizeof b, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
        ui_text(TM.phase, "stopwatch");
        ui_text(TM.sub, "%d lap%s", TM.nlaps, TM.nlaps == 1 ? "" : "s");
    } else if (TM.mode == TM_COUNTDOWN) {
        double left = TM.count_s - e;
        if (left < 0) left = 0;
        int s = (int)ceil(left);
        snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
        ui_text(TM.phase, left > 0 ? "countdown" : "done");
        ui_text(TM.sub, "of %d:%02d", (int)TM.count_s / 60, (int)TM.count_s % 60);
        if (left <= 10 && left > 0) col = BZ_C_WARN;
        if (left <= 0) col = BZ_C_FAULT;
    } else {
        /* the match: autonomous, then teleop; the last 30 s of teleop is the endgame */
        double total = TM.auto_s + TELEOP_S, left, phase_left;
        const char *ph;
        if (e < TM.auto_s) {
            ph = "autonomous";
            phase_left = TM.auto_s - e;
        } else if (e < total) {
            phase_left = total - e;
            ph = phase_left <= 30 ? "endgame" : "teleop";
            if (phase_left <= 30) col = BZ_C_WARN;
        } else {
            ph = "match over";
            phase_left = 0;
            col = BZ_C_DIM;
        }
        left = total - e < 0 ? 0 : total - e;
        int s = (int)ceil(phase_left);
        snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
        ui_text(TM.phase, "%s", ph);
        int m = (int)ceil(left);
        ui_text(TM.sub, "match %d:%02d left · auto %d s · teleop %d:%02d", m / 60, m % 60, TM.auto_s, TELEOP_S / 60,
                TELEOP_S % 60);
        /* the field's cues: start, auto to teleop, endgame, end */
        int cue = e < 0.05 ? -1 : e < TM.auto_s ? 0 : e < total - 30 ? 1 : e < total ? 2 : 3;
        if (TM.running && cue != TM.last_beep && cue >= 0) {
            TM.last_beep = cue;
            beep(cue == 3 ? 520 : 880, cue == 3 ? 500 : 180);
        }
        if (TM.running && e >= total) {
            TM.acc = total;
            TM.running = false;
        }
    }
    if (TM.mode == TM_COUNTDOWN && TM.running && tm_elapsed() >= TM.count_s) {
        TM.acc = TM.count_s;
        TM.running = false;
        beep(660, 600);
    }
    ui_text(TM.big, "%s", b);
    bz_set_color(TM.big, col);
    ui_text(TM.start_lbl, "%s", TM.running ? "pause" : tm_elapsed() > 0 ? "resume" : "start");
}

static void tm_mode(lv_obj_t *o, void *u)
{
    (void)o;
    TM.mode = (int)(intptr_t)u;
    TM.running = false;
    TM.acc = 0;
    TM.nlaps = 0;
    TM.last_beep = -1;
    lv_obj_clean(TM.laps);
    for (int i = 0; i < 3; i++) ui_chip_set(TM.mode_chips[i], i == TM.mode);
    if (TM.mode == TM_COUNTDOWN) lv_obj_remove_flag(TM.presets, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(TM.presets, LV_OBJ_FLAG_HIDDEN);
    tm_show();
}

static void tm_start(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (TM.running) {
        TM.acc = tm_elapsed();
        TM.running = false;
    } else {
        if (TM.mode == TM_COUNTDOWN && tm_elapsed() >= TM.count_s) TM.acc = 0;
        TM.started = hal_seconds();
        TM.running = true;
    }
    beep(1200, 20);
    tm_show();
}

static void tm_reset(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    TM.running = false;
    TM.acc = 0;
    TM.nlaps = 0;
    TM.last_beep = -1;
    lv_obj_clean(TM.laps);
    tm_show();
}

static void tm_lap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (TM.mode != TM_STOPWATCH || !TM.running) return;
    int cs = (int)(tm_elapsed() * 100);
    char b[48];
    snprintf(b, sizeof b, "lap %d   %d:%02d.%02d", ++TM.nlaps, cs / 6000, cs / 100 % 60, cs % 100);
    lv_obj_t *l = bz_label(TM.laps, b, BZ_F_BODY, BZ_C_INK);
    lv_obj_move_to_index(l, 0); /* newest first */
    beep(1500, 15);
}

static void tm_preset(lv_obj_t *o, void *u)
{
    (void)o;
    TM.count_s = (double)(intptr_t)u;
    TM.acc = 0;
    TM.running = false;
    tm_show();
}

static void tm_auto(lv_obj_t *o, void *u)
{
    (void)o;
    TM.auto_s = (int)(intptr_t)u;
    ui_chip_set(TM.auto_chips[0], TM.auto_s == 15);
    ui_chip_set(TM.auto_chips[1], TM.auto_s == 20);
    tm_show();
}

static void timer_build(lv_obj_t *b)
{
    int lw = 760, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = bz_tile(b, lw, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    lv_obj_t *mr = chip_row(t, IN(lw));
    static const char *const MODES[3] = { "match", "stopwatch", "countdown" };
    for (int i = 0; i < 3; i++) TM.mode_chips[i] = ui_chip(mr, MODES[i], tm_mode, (void *)(intptr_t)i);
    TM.phase = bz_label(t, "", BZ_F_NAME, BZ_C_DIM);
    lv_obj_set_pos(TM.phase, 0, 96);
    TM.big = bz_label(t, "0:00", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(TM.big, 0, 140);
    TM.sub = bz_label_line(t, "", BZ_F_LABEL, BZ_C_DIM, IN(lw));
    lv_obj_set_pos(TM.sub, 0, 290);
    lv_obj_t *cr = bz_row(t, 14);
    lv_obj_align(cr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *sb = ui_button(cr, BZ_I_PLAY_ARROW, "start", tm_start, NULL);
    TM.start_lbl = lv_obj_get_child(sb, 1);
    ui_button(cr, BZ_I_FLAG, "lap", tm_lap, NULL);
    ui_button(cr, BZ_I_RESTART_ALT, "reset", tm_reset, NULL);

    lv_obj_t *r = bz_tile(b, rw, APP_H);
    lv_obj_set_pos(r, PAD + lw + BZ_GAP, APP_Y);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 12, 0);
    bz_label(r, "match autonomous", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *ar = chip_row(r, IN(rw));
    TM.auto_chips[0] = ui_chip(ar, "15 s", tm_auto, (void *)15);
    TM.auto_chips[1] = ui_chip(ar, "20 s", tm_auto, (void *)20);
    TM.presets = chip_row(r, IN(rw));
    static const int PRE[6] = { 30, 60, 120, 180, 300, 600 };
    for (int i = 0; i < 6; i++) {
        char n[16];
        if (PRE[i] < 60) snprintf(n, sizeof n, "%d s", PRE[i]);
        else snprintf(n, sizeof n, "%d min", PRE[i] / 60);
        ui_chip(TM.presets, n, tm_preset, (void *)(intptr_t)PRE[i]);
    }
    bz_label(r, "laps", BZ_F_LABEL, BZ_C_DIM);
    TM.laps = bz_col(r, 6);
    tm_auto(NULL, (void *)20);
    tm_mode(NULL, (void *)TM_MATCH);
}

static void timer_frame(double now, double dt)
{
    (void)now; (void)dt;
    if (TM.running) {
        tm_show();
        bz_ui_keep_alive();
    }
}

const ui_app_t APP_TIMER = { .name = "timer", .icon = BZ_I_TIMER, .build = timer_build, .frame = timer_frame };

/* ================================================================== calculator */

static struct {
    lv_obj_t *expr, *result, *conv_note;
    char buf[64];
    double last;
} CA;

/* A small recursive-descent evaluator: + - * / ^, parentheses, unary minus. */
static const char *ca_p;
static double ca_expr(void);
static void ca_ws(void) { while (*ca_p == ' ') ca_p++; }
static double ca_atom(void)
{
    ca_ws();
    if (*ca_p == '(') {
        ca_p++;
        double v = ca_expr();
        ca_ws();
        if (*ca_p == ')') ca_p++;
        return v;
    }
    if (*ca_p == '-') {
        ca_p++;
        return -ca_atom();
    }
    char *end;
    double v = strtod(ca_p, &end);
    if (end == ca_p) return NAN;
    ca_p = end;
    return v;
}
static double ca_pow(void)
{
    double v = ca_atom();
    ca_ws();
    if (*ca_p == '^') {
        ca_p++;
        v = pow(v, ca_pow());
    }
    return v;
}
static double ca_term(void)
{
    double v = ca_pow();
    for (;;) {
        ca_ws();
        if (*ca_p == '*') { ca_p++; v *= ca_pow(); }
        else if (*ca_p == '/') { ca_p++; v /= ca_pow(); }
        else return v;
    }
}
static double ca_expr(void)
{
    double v = ca_term();
    for (;;) {
        ca_ws();
        if (*ca_p == '+') { ca_p++; v += ca_term(); }
        else if (*ca_p == '-') { ca_p++; v -= ca_term(); }
        else return v;
    }
}

static void ca_show(void)
{
    ui_text(CA.expr, "%s", CA.buf[0] ? CA.buf : "0");
    ca_p = CA.buf;
    double v = CA.buf[0] ? ca_expr() : 0;
    if (v == v && !*ca_p) {
        CA.last = v;
        ui_text(CA.result, "= %.10g", v);
    } else {
        ui_text(CA.result, " ");
    }
}

static void ca_key(lv_obj_t *o, void *u)
{
    (void)o;
    const char *k = u;
    size_t n = strlen(CA.buf);
    if (!strcmp(k, "C")) CA.buf[0] = 0;
    else if (!strcmp(k, "<")) { if (n) CA.buf[n - 1] = 0; }
    else if (!strcmp(k, "=")) {
        ca_show();
        snprintf(CA.buf, sizeof CA.buf, "%.10g", CA.last);
    } else if (n + strlen(k) < sizeof CA.buf - 1) {
        strcat(CA.buf, k);
    }
    beep(2000, 6);
    ca_show();
}

static const struct { const char *name; double k, off; const char *unit; } CONV[] = {
    { "in → mm", 25.4, 0, "mm" },       { "mm → in", 1 / 25.4, 0, "in" },  { "lb → kg", 0.45359237, 0, "kg" },
    { "kg → lb", 2.20462262, 0, "lb" }, { "ft → m", 0.3048, 0, "m" },     { "m → ft", 3.2808399, 0, "ft" },
    { "rpm → rad/s", 0.10471976, 0, "rad/s" }, { "°f → °c", 5.0 / 9, -32, "°c" }, { "lbf·in → N·m", 0.11298483, 0, "N·m" },
};

static void ca_conv(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    ca_p = CA.buf;
    double v = CA.buf[0] ? ca_expr() : CA.last;
    if (!(v == v)) return;
    double r = (v + CONV[i].off) * CONV[i].k;
    ui_text(CA.conv_note, "%.6g %s", r, CONV[i].unit);
    snprintf(CA.buf, sizeof CA.buf, "%.8g", r);
    ca_show();
}

static void calc_build(lv_obj_t *b)
{
    int lw = 700, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = bz_tile(b, lw, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    CA.expr = bz_label_line(t, "0", BZ_F_VALUE, BZ_C_INK, IN(lw));
    lv_obj_set_style_text_align(CA.expr, LV_TEXT_ALIGN_RIGHT, 0);
    CA.result = bz_label_line(t, " ", BZ_F_NAME, BZ_C_DIM, IN(lw));
    lv_obj_set_style_text_align(CA.result, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(CA.result, 0, 70);
    static const char *const KEYS[5][5] = {
        { "7", "8", "9", "/", "C" }, { "4", "5", "6", "*", "<" }, { "1", "2", "3", "-", "(" },
        { "0", ".", "^", "+", ")" }, { "", "", "", "", "=" },
    };
    int kw = (IN(lw) - 4 * 10) / 5, kh = 70;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++) {
            if (!KEYS[r][c][0]) continue;
            lv_obj_t *k = bz_tile(t, r == 4 ? IN(lw) : kw, kh);
            lv_obj_set_pos(k, r == 4 ? 0 : c * (kw + 10), 124 + r * (kh + 10));
            bz_tile_set_fill(k, isdigit((unsigned char)KEYS[r][c][0]) || KEYS[r][c][0] == '.' ? BZ_C_SURFACE2 : BZ_C_SURFACE3);
            if (r == 4) bz_tile_set_fill(k, BZ_C_ICE);
            lv_obj_set_style_radius(k, 20, 0);
            lv_obj_set_style_pad_all(k, 0, 0);
            lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *l = bz_label(k, KEYS[r][c], BZ_F_NAME, r == 4 ? BZ_C_ON_ICE : BZ_C_INK);
            if (!strcmp(KEYS[r][c], "<")) {
                lv_label_set_text(l, BZ_I_BACKSPACE);
                lv_obj_set_style_text_font(l, bz_font(BZ_F_NAME), 0);
                lv_obj_delete(l);
                l = bz_icon(k, BZ_I_BACKSPACE, 32, BZ_C_INK);
            }
            lv_obj_center(l);
            bz_on_tap(k, ca_key, (void *)KEYS[r][c]);
        }

    lv_obj_t *r = bz_tile(b, rw, APP_H);
    lv_obj_set_pos(r, PAD + lw + BZ_GAP, APP_Y);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 14, 0);
    bz_label(r, "convert what's entered", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *cr = chip_row(r, IN(rw));
    for (size_t i = 0; i < sizeof CONV / sizeof CONV[0]; i++) ui_chip(cr, CONV[i].name, ca_conv, (void *)(intptr_t)i);
    CA.conv_note = bz_label_line(r, " ", BZ_F_NAME, BZ_C_INK, IN(rw));
    lv_obj_t *h = bz_label(r, "Gear ratios: enter the stages as a product, e.g. 60/12*48/16 for a 15:1.", BZ_F_CAPTION,
                           BZ_C_DIM);
    lv_obj_set_width(h, IN(rw));
    ca_show();
}

const ui_app_t APP_CALC = { .name = "calculator", .icon = BZ_I_CALCULATE, .build = calc_build };

/* ================================================================== notes */

#define NOTES_MAX 3800

static struct {
    lv_obj_t *text, *meta, *card_chip;
    ui_kb_t *kb;
    bool to_card;              /* also keep a copy in <sd>/CATOS/DOCS/NOTES.TXT, for the documents app and a PC */
    char buf[NOTES_MAX + 1];
} NT = { .to_card = true };

static void notes_save(void)
{
    hal_kv_set("notes", NT.buf); /* the tablet's copy, always */
    char path[96];
    bool card = NT.to_card && cstore_path(CS_DOCS, "NOTES.TXT", path, sizeof path);
    bool ok = card && cstore_write(path, NT.buf, strlen(NT.buf));
    ui_text(NT.meta, "%u characters · saved%s", (unsigned)strlen(NT.buf),
            !NT.to_card ? "" : ok ? " · and on the card, CATOS/DOCS/NOTES.TXT" : card ? " · the card copy failed" : " · no card for the copy");
}

static void notes_card(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    NT.to_card = !NT.to_card;
    ui_chip_set(NT.card_chip, NT.to_card);
    hal_kv_set("notes_sd", NT.to_card ? "1" : "0");
    if (NT.to_card) notes_save();
}

static void notes_done(const char *text, void *u)
{
    (void)u;
    snprintf(NT.buf, sizeof NT.buf, "%s", text);
    lv_label_set_text(NT.text, NT.buf[0] ? NT.buf : "Nothing written yet. Tap edit.");
    notes_save();
}

static void notes_edit(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_kb_show(NT.kb, "pit notes", NT.buf, false, false, notes_done, NULL);
}

static void notes_stamp(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    struct tm tm;
    char line[48];
    if (hal_rtc_get(&tm)) strftime(line, sizeof line, "\n— %a %H:%M —\n", &tm);
    else snprintf(line, sizeof line, "\n— entry —\n");
    if (strlen(NT.buf) + strlen(line) < NOTES_MAX) strcat(NT.buf, line);
    lv_label_set_text(NT.text, NT.buf);
    notes_save();
}

static void notes_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    ui_button(hr, BZ_I_SCHEDULE, "stamp", notes_stamp, NULL);
    ui_button(hr, BZ_I_EDIT_NOTE, "edit", notes_edit, NULL);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    lv_obj_t *col = ui_scroller(wrap, W - 2 * PAD, APP_H);
    lv_obj_t *t = bz_tile(col, W - 2 * PAD, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(t, APP_H, 0);
    NT.meta = bz_label_line(t, "", BZ_F_LABEL, BZ_C_DIM, IN(W - 2 * PAD) - 220);
    NT.card_chip = ui_chip(t, "copy to card", notes_card, NULL);
    lv_obj_align(NT.card_chip, LV_ALIGN_TOP_RIGHT, 0, -12);
    NT.text = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_width(NT.text, IN(W - 2 * PAD));
    lv_obj_set_pos(NT.text, 0, 56);
    lv_label_set_long_mode(NT.text, LV_LABEL_LONG_WRAP);
    NT.kb = ui_kb_create(b, 420);
    char v[4];
    if (hal_kv_get("notes_sd", v, sizeof v)) NT.to_card = v[0] == '1';
    ui_chip_set(NT.card_chip, NT.to_card);
    if (!hal_kv_get("notes", NT.buf, sizeof NT.buf)) NT.buf[0] = 0;
    lv_label_set_text(NT.text, NT.buf[0] ? NT.buf : "Nothing written yet. Tap edit.");
    ui_text(NT.meta, "%u characters", (unsigned)strlen(NT.buf));
}

const ui_app_t APP_NOTES = { .name = "notes", .icon = BZ_I_EDIT_NOTE, .build = notes_build };

/* ================================================================== checklist */

static const char *const CHECK[] = {
    "battery charged, voltage checked, strapped down",
    "battery leads and main breaker tight",
    "bumpers on, the right colour, secure",
    "robot code deployed, the right build",
    "Systemcore / radio powered and linked",
    "controllers paired, the right ports",
    "no loose bolts, chains tensioned, tread OK",
    "mechanisms move freely by hand",
    "cameras mounted and seeing",
    "preflight is a go",
    "e-stop and A-stop checked",
    "tools and spares packed for the queue",
};
#define NCHECK (int)(sizeof CHECK / sizeof CHECK[0])

static struct {
    lv_obj_t *rows[NCHECK], *marks[NCHECK], *count, *meter;
    uint32_t bits;
} CK;

static void ck_show(void)
{
    int n = 0;
    for (int i = 0; i < NCHECK; i++) {
        bool on = CK.bits >> i & 1;
        n += on;
        bz_icon_set(CK.marks[i], on ? BZ_I_CHECK_CIRCLE : BZ_I_RADIO_BUTTON_UNCHECKED, 32, on);
        bz_set_color(CK.marks[i], on ? BZ_C_OK : BZ_C_DIM);
    }
    ui_text(CK.count, "%d of %d", n, NCHECK);
    bz_meter_set(CK.meter, (float)n / NCHECK, n == NCHECK ? BZ_C_OK : BZ_C_ICE);
    char v[16];
    snprintf(v, sizeof v, "%lu", (unsigned long)CK.bits);
    hal_kv_set("checklist", v);
}

static void ck_tap(lv_obj_t *o, void *u)
{
    (void)o;
    CK.bits ^= 1u << (int)(intptr_t)u;
    beep(1800, 12);
    ck_show();
}

static void ck_reset(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    CK.bits = 0;
    ck_show();
}

static void check_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    /* the band's right holds ~380 px beside the island: the count and the reset, the meter goes in the body */
    CK.count = bz_label(hr, "", BZ_F_NAME, BZ_C_INK);
    ui_button(hr, BZ_I_RESTART_ALT, "new match", ck_reset, NULL);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    lv_obj_t *col = ui_scroller(wrap, W - 2 * PAD, APP_H);
    int cw = (W - 2 * PAD - BZ_GAP) / 2;
    ui_batt_check_row(col, b, W - 2 * PAD); /* which battery goes in: first, since it decides the first item */
    CK.meter = bz_meter(col, W - 2 * PAD, 12);
    lv_obj_t *grid = bz_row(col, BZ_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BZ_GAP, 0);
    lv_obj_set_width(grid, W - 2 * PAD);
    for (int i = 0; i < NCHECK; i++) {
        lv_obj_t *t = bz_tile(grid, cw, 84);
        lv_obj_set_style_pad_ver(t, 0, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, 16, 0);
        CK.marks[i] = bz_icon(t, BZ_I_RADIO_BUTTON_UNCHECKED, 32, BZ_C_DIM);
        /* two lines when it needs them (the tile has the height): an item cut short is a check skipped */
        lv_obj_t *l = bz_label(t, CHECK[i], BZ_F_BODY, BZ_C_INK);
        lv_obj_set_width(l, IN(cw) - 56);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        bz_on_tap(t, ck_tap, (void *)(intptr_t)i);
        CK.rows[i] = t;
    }
    char v[16];
    CK.bits = hal_kv_get("checklist", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    ck_show();
}

static void check_refresh(void) { ui_batt_check_refresh(); }

const ui_app_t APP_CHECK = { .name = "checklist", .icon = BZ_I_CHECKLIST_RTL, .build = check_build,
                             .refresh = check_refresh };

/* ================================================================== flashlight */

static struct {
    lv_obj_t *field, *label;
    bool red;
} FL;

static void fl_paint(void)
{
    lv_obj_set_style_bg_color(FL.field, FL.red ? lv_color_hex(0xB0000A) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(FL.label, FL.red ? lv_color_hex(0x40000A) : lv_color_hex(0x9AA1A8), 0);
}

static void fl_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    FL.red = !FL.red; /* red keeps night vision, and doesn't blind the next pit */
    fl_paint();
}

static void light_build(lv_obj_t *b)
{
    FL.field = lv_obj_create(b);
    lv_obj_remove_style_all(FL.field);
    lv_obj_set_size(FL.field, W, H);
    lv_obj_set_style_bg_opa(FL.field, LV_OPA_COVER, 0);
    lv_obj_add_flag(FL.field, LV_OBJ_FLAG_CLICKABLE);
    FL.label = bz_label(FL.field, "tap for red · back to close", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(FL.label, LV_ALIGN_BOTTOM_MID, 0, -24);
    bz_on_tap(FL.field, fl_tap, NULL);
    fl_paint();
}

static void light_open(void) { hal_set_brightness(1.0f); }
static void light_close(void) { hal_set_brightness(S.brightness); }

const ui_app_t APP_LIGHT = { .name = "flashlight", .icon = BZ_I_FLASHLIGHT_ON, .build = light_build,
                             .open = light_open, .close = light_close };

/* ================================================================== system */

static struct {
    lv_obj_t *v[16], *cpu_meter[2], *ram_meter, *psram_meter;
} SY;

enum { SY_UPTIME, SY_TEMP, SY_CPU0, SY_CPU1, SY_SRAM, SY_PSRAM, SY_BATT, SY_POWER, SY_WIFI, SY_IP, SY_TETHER, SY_FPS,
       SY_FRAME, SY_FW, SY_SD, SY_BOOT };

static lv_obj_t *sy_kv(lv_obj_t *col, const char *k, int w)
{
    lv_obj_t *r = bz_row(col, 12);
    lv_obj_set_width(r, w);
    lv_obj_t *kl = bz_label(r, k, BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_width(kl, 150);
    return bz_label_line(r, "—", BZ_F_BODY, BZ_C_INK, w - 162);
}

static lv_obj_t *sy_tile(lv_obj_t *grid, const char *title, int w)
{
    lv_obj_t *t = bz_tile(grid, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0);
    lv_obj_set_style_min_height(t, APP_H, 0);
    bz_label(t, title, BZ_F_LABEL, BZ_C_DIM);
    return t;
}

static void sys_build(lv_obj_t *b)
{
    int cw = (W - 2 * PAD - 2 * BZ_GAP) / 3;
    lv_obj_t *grid = bz_row(b, BZ_GAP);
    lv_obj_set_pos(grid, PAD, APP_Y);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *t = sy_tile(grid, "the tablet", cw);
    SY.v[SY_UPTIME] = sy_kv(t, "uptime", IN(cw));
    SY.v[SY_TEMP] = sy_kv(t, "chip temp", IN(cw));
    SY.v[SY_CPU0] = sy_kv(t, "core 0", IN(cw));
    SY.cpu_meter[0] = bz_meter(t, IN(cw), 10);
    SY.v[SY_CPU1] = sy_kv(t, "core 1", IN(cw));
    SY.cpu_meter[1] = bz_meter(t, IN(cw), 10);
    SY.v[SY_SRAM] = sy_kv(t, "internal ram", IN(cw));
    SY.ram_meter = bz_meter(t, IN(cw), 10);
    SY.v[SY_PSRAM] = sy_kv(t, "psram", IN(cw));
    SY.psram_meter = bz_meter(t, IN(cw), 10);
    t = sy_tile(grid, "power and links", cw);
    SY.v[SY_BATT] = sy_kv(t, "battery", IN(cw));
    SY.v[SY_POWER] = sy_kv(t, "power", IN(cw));
    SY.v[SY_WIFI] = sy_kv(t, "wi-fi", IN(cw));
    SY.v[SY_IP] = sy_kv(t, "address", IN(cw));
    SY.v[SY_TETHER] = sy_kv(t, "usb tether", IN(cw));
    SY.v[SY_SD] = sy_kv(t, "microsd", IN(cw));
    t = sy_tile(grid, "software", cw);
    SY.v[SY_FPS] = sy_kv(t, "frame rate", IN(cw));
    SY.v[SY_FRAME] = sy_kv(t, "frame work", IN(cw));
    SY.v[SY_FW] = sy_kv(t, "firmware", IN(cw));
    SY.v[SY_BOOT] = sy_kv(t, "last start", IN(cw));
}

static void sys_refresh(void)
{
    hal_sys_t s;
    hal_sys(&s);
    double up = hal_seconds();
    ui_text(SY.v[SY_UPTIME], "%dh %02dm %02ds", (int)up / 3600, (int)up / 60 % 60, (int)up % 60);
    ui_text(SY.v[SY_TEMP], "%.0f °c", s.temp_c);
    for (int i = 0; i < 2; i++) {
        ui_text(SY.v[SY_CPU0 + i], "%.0f %%", s.cpu[i] * 100);
        bz_meter_set(SY.cpu_meter[i], s.cpu[i], s.cpu[i] > 0.9f ? BZ_C_WARN : BZ_C_ICE);
    }
    ui_text(SY.v[SY_SRAM], "%u kb free", (unsigned)(s.sram_free / 1024));
    bz_meter_set(SY.ram_meter, 1.0f - s.sram_free / 786432.0f, s.sram_free < 32768 ? BZ_C_WARN : BZ_C_ICE);
    ui_text(SY.v[SY_PSRAM], "%.1f mb free", s.psram_free / 1048576.0);
    bz_meter_set(SY.psram_meter, 1.0f - s.psram_free / 33554432.0f, BZ_C_ICE);
    hal_battery_t bt;
    if (hal_battery(&bt) && bt.ok) {
        ui_text(SY.v[SY_BATT], "%d %% · %.2f v", bt.percent, bt.volts);
        ui_text(SY.v[SY_POWER], "%s%s", bt.charging ? "charging" : "on battery", bt.external ? " · external power" : "");
    }
    hal_net_t n;
    hal_net(&n);
    ui_text(SY.v[SY_WIFI], "%s", n.up ? n.ssid : "not connected");
    ui_text(SY.v[SY_IP], "%s", n.up ? n.ip : "—");
    hal_tether_t te;
    hal_tether(&te);
    ui_text(SY.v[SY_TETHER], "%s", te.up ? te.ip : te.present ? "attached, no address" : "none");
    ui_text(SY.v[SY_SD], "%s", hal_sd_root() ? "mounted" : "no card");
    bz_ui_perf_t pf;
    bz_ui_perf(&pf);
    ui_text(SY.v[SY_FPS], "%.0f fps", pf.fps);
    ui_text(SY.v[SY_FRAME], "%.1f ms", pf.frame_ms);
    ui_text(SY.v[SY_FW], "%s", CATALYST_TAB_VERSION);
    hal_boot_t pb;
    hal_boot_prev(&pb);
    ui_text(SY.v[SY_BOOT], "%s%s", pb.failed ? "failed: " : "", pb.reason);
}

const ui_app_t APP_SYSMON = { .name = "system", .icon = BZ_I_MONITORING, .build = sys_build, .refresh = sys_refresh };

/* ================================================================== files */

static struct {
    lv_obj_t *list, *path_lbl;
    char path[160];
} FI;

static void files_load(void);

static void fi_open(lv_obj_t *o, void *u)
{
    (void)o;
    const char *name = u;
    size_t n = strlen(FI.path);
    if (!strcmp(name, "..")) {
        char *s = strrchr(FI.path, '/');
        const char *root = hal_sd_root();
        if (s && root && strlen(FI.path) > strlen(root)) *s = 0;
    } else if (n + strlen(name) + 2 < sizeof FI.path) {
        strcat(FI.path, "/");
        strcat(FI.path, name);
    }
    files_load();
}

static void files_load(void)
{
    lv_obj_clean(FI.list);
    const char *root = hal_sd_root();
    if (!root) {
        ui_text(FI.path_lbl, "no microSD card");
        lv_obj_t *t = bz_tile(FI.list, W - 2 * PAD, LV_SIZE_CONTENT);
        bz_label(t, "Put a card in the slot on the tablet's edge; logs, recordings, clips and notes go there.",
                 BZ_F_BODY, BZ_C_DIM);
        return;
    }
    if (!FI.path[0] || strncmp(FI.path, root, strlen(root))) snprintf(FI.path, sizeof FI.path, "%s", root);
    ui_text(FI.path_lbl, "%s", FI.path);
    DIR *d = opendir(FI.path);
    if (!d) return;
    static char names[64][64];
    int k = 0;
    if (strlen(FI.path) > strlen(root)) {
        snprintf(names[k], sizeof names[k], "..");
        lv_obj_t *t = ui_button(FI.list, BZ_I_ARROW_BACK, "up a folder", fi_open, names[k]);
        (void)t;
        k++;
    }
    struct dirent *e;
    while ((e = readdir(d)) && k < 64) {
        char full[240];
        snprintf(full, sizeof full, "%s/%s", FI.path, e->d_name);
        struct stat st;
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(names[k], sizeof names[k], "%s", e->d_name);
        lv_obj_t *t = bz_tile(FI.list, W - 2 * PAD, 72);
        lv_obj_set_style_pad_ver(t, 0, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, 16, 0);
        bz_icon(t, dir ? BZ_I_FOLDER : BZ_I_DESCRIPTION, 32, dir ? BZ_C_ICE : BZ_C_DIM);
        bz_label_line(t, e->d_name, BZ_F_BODY, BZ_C_INK, 760);
        char sz[24] = "";
        if (!dir) {
            double s = st.st_size;
            if (s < 1024) snprintf(sz, sizeof sz, "%.0f b", s);
            else if (s < 1048576) snprintf(sz, sizeof sz, "%.1f kb", s / 1024);
            else snprintf(sz, sizeof sz, "%.1f mb", s / 1048576);
        }
        bz_label(t, dir ? "folder" : sz, BZ_F_LABEL, BZ_C_DIM);
        if (dir) {
            lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
            bz_on_tap(t, fi_open, names[k]);
        }
        k++;
    }
    closedir(d);
}

static void files_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    FI.path_lbl = bz_label_line(hr, "", BZ_F_LABEL, BZ_C_DIM, 420);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    FI.list = ui_scroller(wrap, W - 2 * PAD, APP_H);
}

static void files_open(void) { files_load(); }

const ui_app_t APP_FILES = { .name = "files", .icon = BZ_I_FOLDER, .build = files_build, .open = files_open };
