/* companion — desk mode: a face for the tablet on its stand.
 *
 * Two eyes, a one-line status under them, the assistant's last answer as a bubble, and a bar of quick
 * questions. It listens through the on-screen keyboard (the microphones stay off) and answers in text
 * with a short chime. The eyes blink, glance about, follow the last touch, start at a knock and roll
 * with a tilt (the IMU), grow sleepy when nobody is around, squint with pleasure when a task finishes and
 * turn amber when something needs the owner: the robot's faults, a question from the assistant, Claude
 * Code on the PC waiting for input.
 *
 * Claude mode: a panel lists Claude Code's sessions on the owner's PC as Catalyst Link reports them
 * (components/assist/ccwatch.c): what each is doing, how long it has been at it, when it will likely
 * finish (an estimate from the owner's own past turns, always labelled so), and when it has finished or
 * needs input: then the eyes react, a chime plays, the island says so, and the reminder repeats until
 * someone taps.
 *
 * Cheap to draw on the lean renderer: each eye is three small objects (the eye and two ground-coloured
 * covers that shape it into a lid or a smile) in one box; the gaze moves the box, a blink resizes the
 * eye, and nothing is touched in a frame where no whole pixel changed. Between glances nothing moves at
 * all. */
#include "ui_companion.h"

#include "assist.h"
#include "ccwatch.h"
#include "link.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define EW 150                 /* an eye at rest */
#define EH 190
#define BOX_W (EW + 70)        /* each eye's box: the eye grows into it (surprise, sleep) */
#define BOX_H (EH + 70)
#define EYE_GAP 330            /* centre to centre */
#define EYE_CY 262
#define GAZE_X 46              /* how far the eyes travel, px */
#define GAZE_Y 28
#define STATUS_Y 404
#define BAR_H 64
#define BAR_Y (H - PAD - BAR_H)
#define BUBBLE_Y 448
#define BUBBLE_H (BAR_Y - BZ_GAP - BUBBLE_Y)
#define PANEL_W 440
#define PANEL_X (W - PAD - PANEL_W)
#define PANEL_Y APP_Y
#define PANEL_H (BAR_Y - BZ_GAP - PANEL_Y)
#define ROWS CCW_MAX
#define SLEEPY_S 90.0          /* untouched this long, and nothing going on: drowsy */
#define ASLEEP_S 300.0

/* ================================================================== settings */

static ui_companion_cfg_t CFG;
static bool cfg_loaded;

ui_companion_cfg_t *ui_companion_cfg(void)
{
    if (!cfg_loaded) {
        cfg_loaded = true;
        char v[24];
        CFG.remind_s = 120;
        if (hal_kv_get("orb_opens", v, sizeof v)) CFG.orb_companion = !strcmp(v, "companion");
        if (hal_kv_get("desk_auto", v, sizeof v)) CFG.desk_auto = !strcmp(v, "1");
        if (hal_kv_get("cc_remind", v, sizeof v) && v[0]) CFG.remind_s = atoi(v);
    }
    return &CFG;
}

void ui_companion_cfg_save(void)
{
    char v[16];
    hal_kv_set("orb_opens", CFG.orb_companion ? "companion" : "assist");
    hal_kv_set("desk_auto", CFG.desk_auto ? "1" : "0");
    snprintf(v, sizeof v, "%d", CFG.remind_s);
    hal_kv_set("cc_remind", v);
}

/* ================================================================== chimes */

/* A few notes in sequence, played from the frame hook (hal_tone is one tone, non-blocking). */
static struct {
    double at[8];
    float hz[8];
    int ms[8];
    int n;
} CHM;

typedef enum { CHIME_DONE, CHIME_INPUT, CHIME_ERROR, CHIME_ANSWER, CHIME_ACK } chime_t;

static void note(double at, float hz, int ms)
{
    if (CHM.n == 8) return;
    CHM.at[CHM.n] = at;
    CHM.hz[CHM.n] = hz;
    CHM.ms[CHM.n++] = ms;
}

static void chime(chime_t k)
{
    double t = hal_seconds();
    switch (k) {
    case CHIME_DONE: note(t, 659, 110); note(t + 0.13, 880, 110); note(t + 0.26, 1319, 180); break;
    case CHIME_INPUT: note(t, 880, 90); note(t + 0.18, 880, 90); break;
    case CHIME_ERROR: note(t, 494, 160); note(t + 0.2, 370, 240); break;
    case CHIME_ANSWER: note(t, 1047, 70); note(t + 0.09, 1319, 110); break;
    case CHIME_ACK: note(t, 1568, 30); break;
    }
}

static void chime_tick(double now)
{
    for (int i = 0; i < CHM.n;) {
        if (CHM.at[i] > now) {
            i++;
            continue;
        }
        hal_tone(CHM.hz[i], CHM.ms[i], S.volume * 0.6f);
        CHM.n--;
        memmove(&CHM.at[i], &CHM.at[i + 1], (size_t)(CHM.n - i) * sizeof CHM.at[0]);
        memmove(&CHM.hz[i], &CHM.hz[i + 1], (size_t)(CHM.n - i) * sizeof CHM.hz[0]);
        memmove(&CHM.ms[i], &CHM.ms[i + 1], (size_t)(CHM.n - i) * sizeof CHM.ms[0]);
    }
}

/* ================================================================== the face */

typedef enum { EX_NEUTRAL, EX_FOCUSED, EX_LISTEN, EX_SLEEPY, EX_ASLEEP, EX_HAPPY, EX_WORRIED, EX_FAULT, EX_SURPRISED } expr_id_t;

typedef struct {
    float w, h;            /* scale of the eye */
    float lid;             /* 0 open .. 1 shut, from the top */
    float smile;           /* 0 .. 1: the bottom cover rises into a happy crescent */
    float y;               /* px, down */
    bz_color_role_t color;
} expr_t;

static const expr_t EXPR[] = {
    [EX_NEUTRAL] = { 1.00f, 1.00f, 0.00f, 0.00f, 0, BZ_C_ICE },
    [EX_FOCUSED] = { 1.00f, 0.86f, 0.18f, 0.00f, 0, BZ_C_ICE },
    [EX_LISTEN] = { 1.04f, 1.04f, 0.00f, 0.00f, 6, BZ_C_ICE },
    [EX_SLEEPY] = { 1.06f, 0.90f, 0.52f, 0.00f, 10, BZ_C_DIM },
    [EX_ASLEEP] = { 1.10f, 0.10f, 0.00f, 0.00f, 24, BZ_C_FAINT },
    [EX_HAPPY] = { 1.06f, 1.00f, 0.00f, 0.62f, -8, BZ_C_LEAF },
    [EX_WORRIED] = { 0.88f, 0.92f, 0.30f, 0.00f, 4, BZ_C_AMBER },
    [EX_FAULT] = { 0.90f, 0.96f, 0.22f, 0.00f, 0, BZ_C_FAULT },
    [EX_SURPRISED] = { 1.16f, 1.20f, 0.00f, 0.00f, -6, BZ_C_ICE },
};

struct crow {
    char id[48];
    lv_obj_t *row, *title, *sub, *step, *eta, *mark;
};

typedef struct {
    lv_obj_t *box, *eye, *lid, *smile;
    int bx, by, ex, ey, ew, eh, er, lh, sx, sy, sw, sh; /* as applied: only whole-pixel changes are written */
    bool lid_on, smile_on;
} eye_t;

static struct {
    bool built, open;
    lv_obj_t *body, *face, *status, *bubble_wrap, *bubble_box, *asked, *answer, *bar, *chips, *cc_btn, *cc_label;
    lv_obj_t *panel, *list, *panel_note, *panel_head;
    ui_kb_t *kb;
    eye_t eye[2];
    bz_color_role_t color;
    /* the face as drawn, easing toward the expression */
    float w, h, lid, smile, y, gx, gy, bounce;
    int face_cx;
    /* behaviour */
    double blink_at, blink_t0, wander_at, touch_at, mood_until, glance_until, tap_at, bounce_t0;
    int blinks_left;
    float wx, wy, tx, ty;
    expr_id_t mood;        /* transient: happy, surprised, worried */
    uint32_t rng;
    /* the IMU: gravity low-passed, for knocks and tilt */
    float gxl, gyl, gzl, tilt;
    bool imu_seeded;
    int imu_div;
    /* the conversation */
    uint32_t arev;
    double bubble_at;
    as_phase_t last_phase;
    bool bubble_shown;
    /* the claude panel */
    bool panel_open;
    uint32_t crev;
    int nrows;
    struct crow *rows;     /* ROWS, on the heap: internal RAM is nearly gone */
    const char *sugg[3];
    char sugg_keep[3][80];
    int nsugg;
    double last_seen;      /* when the companion last closed (desk mode waits a while before coming back) */
} CP;

static float rnd(void)
{
    CP.rng = CP.rng * 1664525u + 1013904223u;
    return (float)(CP.rng >> 8) / 16777216.0f;
}

static float ease(float v, float target, float rate, double dt)
{
    return v + (target - v) * (1.0f - expf(-(float)dt * rate));
}

static void set_fill(lv_obj_t *o, bz_color_role_t from, bz_color_role_t to)
{
    lv_obj_remove_style(o, bz_style_fill(from), 0);
    lv_obj_add_style(o, bz_style_fill(to), 0);
}

static lv_obj_t *shape(lv_obj_t *parent, bz_color_role_t fill)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, bz_style_fill(fill), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void eye_build(eye_t *e, lv_obj_t *face)
{
    e->box = lv_obj_create(face);
    lv_obj_remove_style_all(e->box);
    lv_obj_set_size(e->box, BOX_W, BOX_H);
    lv_obj_remove_flag(e->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(e->box, LV_OBJ_FLAG_SCROLLABLE);
    e->eye = shape(e->box, BZ_C_ICE);
    e->lid = shape(e->box, BZ_C_GROUND);
    e->smile = shape(e->box, BZ_C_GROUND);
    lv_obj_set_style_radius(e->smile, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(e->lid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(e->smile, LV_OBJ_FLAG_HIDDEN);
    e->bx = e->by = e->ex = e->ey = e->ew = e->eh = e->er = e->lh = e->sx = e->sy = e->sw = e->sh = -9999;
}

static void place(lv_obj_t *o, int *cx, int *cy, int *cw, int *ch, int x, int y, int w, int h)
{
    if (x != *cx || y != *cy) {
        lv_obj_set_pos(o, x, y);
        *cx = x;
        *cy = y;
    }
    if (w != *cw || h != *ch) {
        lv_obj_set_size(o, w, h);
        *cw = w;
        *ch = h;
    }
}

static void show(lv_obj_t *o, bool *on, bool want)
{
    if (want == *on) return;
    *on = want;
    if (want) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Draws both eyes from the eased values; writes only what changed by a whole pixel. */
static void eyes_apply(double now)
{
    float blink = 0;
    if (CP.blink_t0 > 0) {
        float k = (float)((now - CP.blink_t0) / 0.17);
        if (k >= 1) CP.blink_t0 = 0;
        else blink = sinf(k * 3.14159f);
    }
    float bounce = 0;
    if (CP.bounce_t0 > 0) {
        float k = (float)((now - CP.bounce_t0) / 0.9);
        if (k >= 1) CP.bounce_t0 = 0;
        else bounce = -14.0f * fabsf(sinf(k * 3.14159f * 3)) * (1 - k);
    }
    int ew = (int)lroundf(EW * CP.w);
    int eh = (int)lroundf(EH * CP.h * (1 - 0.93f * blink));
    if (eh < 10) eh = 10;
    if (ew > BOX_W) ew = BOX_W;
    if (eh > BOX_H) eh = BOX_H;
    int ex = (BOX_W - ew) / 2, ey = (BOX_H - eh) / 2 + (int)lroundf(CP.y * 0.5f);
    if (ey < 0) ey = 0;
    if (ey + eh > BOX_H) ey = BOX_H - eh;
    int r = (ew < eh ? ew : eh) / 2;
    int lh = ey + (int)lroundf(CP.lid * eh);
    int sw = (int)lroundf(ew * 1.5f), sh = eh;
    int sy = ey + eh - (int)lroundf(CP.smile * eh * 0.62f);
    for (int i = 0; i < 2; i++) {
        eye_t *e = &CP.eye[i];
        int bx = CP.face_cx + (i ? EYE_GAP / 2 : -EYE_GAP / 2) - BOX_W / 2 + (int)lroundf(CP.gx * GAZE_X);
        int by = EYE_CY - BOX_H / 2 + (int)lroundf(CP.gy * GAZE_Y + CP.y * 0.5f + bounce);
        int dummy_w = BOX_W, dummy_h = BOX_H;
        place(e->box, &e->bx, &e->by, &dummy_w, &dummy_h, bx, by, BOX_W, BOX_H);
        place(e->eye, &e->ex, &e->ey, &e->ew, &e->eh, ex, ey, ew, eh);
        if (r != e->er) {
            lv_obj_set_style_radius(e->eye, r, 0);
            e->er = r;
        }
        show(e->lid, &e->lid_on, CP.lid > 0.02f);
        if (e->lid_on) {
            int lw = BOX_W, zero = 0;
            place(e->lid, &zero, &zero, &lw, &e->lh, 0, 0, BOX_W, lh > 0 ? lh : 1);
        }
        show(e->smile, &e->smile_on, CP.smile > 0.02f);
        if (e->smile_on) place(e->smile, &e->sx, &e->sy, &e->sw, &e->sh, (BOX_W - sw) / 2, sy, sw, sh);
    }
}

static void eyes_color(bz_color_role_t c)
{
    if (c == CP.color) return;
    for (int i = 0; i < 2; i++) set_fill(CP.eye[i].eye, CP.color, c);
    CP.color = c;
}

/* ================================================================== what it knows */

static bool robot_fault(void) { return R && R->connected && (R->n_errors > 0 || R->browned_out); }

static const char *fmt_dur(double s, char *buf, size_t n)
{
    if (s < 60) snprintf(buf, n, "under a minute");
    else if (s < 3600) snprintf(buf, n, "%d min", (int)(s / 60 + 0.5));
    else snprintf(buf, n, "%dh %02dm", (int)(s / 3600), (int)(fmod(s, 3600) / 60));
    return buf;
}

/* "about 4 min left (est., 2 to 7)" or why there's no estimate */
static void eta_text(const ccw_session_t *s, double now, char *out, size_t n)
{
    char a[24], b[24], c[24];
    if (s->state == CCW_WAITING) {
        snprintf(out, n, "waiting for you");
        return;
    }
    if (s->state != CCW_RUNNING) {
        snprintf(out, n, "%s %s ago", ccw_state_word(s->state), fmt_dur(s->since_s + (now - s->polled_at), a, sizeof a));
        return;
    }
    if (!s->have_eta) {
        snprintf(out, n, "%s", s->basis[0] ? s->basis : "no estimate yet");
        return;
    }
    double gone = now - s->polled_at, left = s->eta_s - gone;
    if (left < 30) snprintf(out, n, "any moment now (est.)");
    else
        snprintf(out, n, "about %s left (est., %s to %s)", fmt_dur(left, a, sizeof a),
                 fmt_dur(s->eta_lo_s - gone > 0 ? s->eta_lo_s - gone : 0, b, sizeof b), fmt_dur(s->eta_hi_s - gone, c, sizeof c));
}

/* The one line under the eyes: the most important thing going on. */
static void status_line(double now, char *out, size_t n)
{
    as_phase_t ph = assist_phase();
    ccw_session_t *list = NULL, att;
    int natt = ccw_attention(&att);
    if (ph == AS_PHASE_CONFIRM) {
        snprintf(out, n, "the assistant wants to change something: open assist to approve or decline");
        return;
    }
    if (ph == AS_PHASE_THINKING || ph == AS_PHASE_SENDING) {
        snprintf(out, n, "thinking...");
        return;
    }
    if (ph == AS_PHASE_TOOL || ph == AS_PHASE_WRITING) {
        char line[120] = "";
        assist_lock();
        int c = assist_count();
        for (int i = c - 1; i >= 0 && i >= c - 3; i--) {
            const as_entry_t *e = assist_entry(i);
            if (e->kind == AS_E_TOOL) {
                snprintf(line, sizeof line, "%s", e->text ? e->text : "");
                break;
            }
        }
        assist_unlock();
        snprintf(out, n, "%s%s%s", ph == AS_PHASE_TOOL ? "looking" : "answering", line[0] ? ": " : "...", line);
        return;
    }
    if (natt) {
        const char *what = att.state == CCW_DONE ? "Claude finished" : att.state == CCW_WAITING ? "Claude needs you" : "Claude hit an error";
        snprintf(out, n, "%s in %s: %s%s", what, att.project[0] ? att.project : "a session",
                 att.state == CCW_DONE ? att.title : att.step, natt > 1 ? " (and more: tap to clear)" : " (tap to clear)");
        return;
    }
    list = malloc(sizeof *list * CCW_MAX);
    int nl = list ? ccw_list(list, CCW_MAX) : 0;
    for (int i = 0; i < nl; i++) {
        if (list[i].state != CCW_RUNNING) continue;
        char eta[96];
        eta_text(&list[i], now, eta, sizeof eta);
        int running = 0;
        for (int k = 0; k < nl; k++) running += list[k].state == CCW_RUNNING;
        snprintf(out, n, "Claude is working in %s: %s · %s%s", list[i].project[0] ? list[i].project : "a session",
                 list[i].step[0] ? list[i].step : "thinking", list[i].have_eta ? eta : "no estimate yet",
                 running > 1 ? " · more in the panel" : "");
        free(list);
        return;
    }
    free(list);
    if (robot_fault()) {
        snprintf(out, n, "the robot has %d error%s%s: ask me about it", R->n_errors, R->n_errors == 1 ? "" : "s",
                 R->browned_out ? " and browned out" : "");
        return;
    }
    if (ph == AS_PHASE_ERROR) {
        assist_lock();
        int c = assist_count();
        const as_entry_t *e = c ? assist_entry(c - 1) : NULL;
        snprintf(out, n, "%s", e && e->kind == AS_E_ERROR && e->text ? e->text : "the last question failed");
        assist_unlock();
        return;
    }
    double idle = bz_ui_idle_s();
    if (idle > ASLEEP_S) snprintf(out, n, "zzz · tap to wake me");
    else if (idle > SLEEPY_S) snprintf(out, n, "getting sleepy · tap to talk");
    else {
        char why[96];
        if (!assist_ready(why, sizeof why)) snprintf(out, n, "tap a question, or type one · %s", why);
        else snprintf(out, n, "tap a question, or type one");
    }
}

/* ================================================================== the conversation */

static void plain_into(char *out, size_t n, const char *in)
{
    size_t o = 0;
    bool line_start = true;
    for (const char *p = in; *p && o + 4 < n; p++) {
        if (p[0] == '*' && p[1] == '*') { p++; continue; }
        if (*p == '`') continue;
        if (line_start && (p[0] == '-' || p[0] == '*') && p[1] == ' ') {
            memcpy(out + o, "\xe2\x80\x94", 3); /* the face has no bullet: an em dash */
            o += 3;
            line_start = false;
            continue;
        }
        if (line_start && p[0] == '#') {
            while (p[1] == '#') p++;
            if (p[1] == ' ') p++;
            continue;
        }
        out[o++] = *p;
        line_start = *p == '\n';
    }
    out[o] = 0;
}

static void bubble_sync(double now)
{
    uint32_t rev = assist_rev();
    if (rev == CP.arev || now - CP.bubble_at < 0.1) return; /* ten updates a second while it streams */
    CP.arev = rev;
    CP.bubble_at = now;
    char asked[160] = "";
    char *answer = NULL;
    assist_lock();
    int n = assist_count(), last_user = -1;
    for (int i = n - 1; i >= 0; i--)
        if (assist_entry(i)->kind == AS_E_USER) {
            last_user = i;
            break;
        }
    if (last_user >= 0) {
        const as_entry_t *u = assist_entry(last_user);
        snprintf(asked, sizeof asked, "you: %s", u->text ? u->text : "");
        for (int i = n - 1; i > last_user; i--) {
            const as_entry_t *e = assist_entry(i);
            if (e->kind == AS_E_TEXT && e->text && e->text[0]) {
                size_t l = strlen(e->text) + 8;
                answer = malloc(l);
                if (answer) plain_into(answer, l, e->text);
                break;
            }
        }
    }
    assist_unlock();
    bool want = last_user >= 0;
    if (want != CP.bubble_shown) {
        CP.bubble_shown = want;
        if (want) lv_obj_remove_flag(CP.bubble_wrap, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(CP.bubble_wrap, LV_OBJ_FLAG_HIDDEN);
    }
    ui_text(CP.asked, "%s", asked);
    const char *cur = lv_label_get_text(CP.answer);
    const char *nxt = answer ? answer : "...";
    if (!cur || strcmp(cur, nxt) != 0) {
        lv_label_set_text(CP.answer, nxt);
        ui_scroller_follow(CP.bubble_box);
    }
    free(answer);
}

static void ask(const char *q)
{
    char why[96];
    if (!assist_ready(why, sizeof why)) {
        ui_island_say(BZ_I_LINK_OFF, why);
        CP.mood = EX_WORRIED;
        CP.mood_until = hal_seconds() + 2.5;
        return;
    }
    if (assist_send(q)) {
        hal_tone(1300, 12, S.volume * 0.4f);
        CP.mood = EX_LISTEN;
        CP.mood_until = hal_seconds() + 0.8;
        CP.arev = 0;
    } else ui_island_say(BZ_I_HOURGLASS_TOP, "still working on the last one");
}

static void typed(const char *text, void *u)
{
    (void)u;
    while (*text == ' ' || *text == '\n') text++;
    if (*text) ask(text);
}

static void talk_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    as_phase_t p = assist_phase();
    if (p != AS_PHASE_IDLE && p != AS_PHASE_ERROR) {
        assist_stop();
        ui_island_say(BZ_I_STOP_CIRCLE, "stopped");
        return;
    }
    ui_kb_show(CP.kb, "talk to the tablet", "", false, false, typed, NULL);
}

static void chip_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i < CP.nsugg) ask(CP.sugg[i]);
}

static void assist_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_ASSIST, o);
}

static void refresh_chips(void)
{
    const char *s[3];
    int n = 0;
    const char *robot[2];
    int nr = assist_suggestions(robot, 1);
    if (nr) s[n++] = robot[0];
    s[n++] = "What is Claude doing on the PC?";
    s[n++] = robot_fault() ? "What should I fix first?" : "Is the robot ready?";
    bool same = n == CP.nsugg;
    for (int i = 0; i < n && same; i++) same = !strcmp(s[i], CP.sugg[i]);
    if (same) return;
    lv_obj_clean(CP.chips);
    CP.nsugg = n;
    lv_obj_update_layout(CP.chips); /* its width, not zero before the first layout */
    int share = (lv_obj_get_width(CP.chips) - 8 * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        snprintf(CP.sugg_keep[i], sizeof CP.sugg_keep[i], "%s", s[i]);
        CP.sugg[i] = CP.sugg_keep[i];
        lv_obj_t *c = ui_chip(CP.chips, CP.sugg_keep[i], chip_tap, (void *)(intptr_t)i);
        lv_obj_set_style_max_width(c, share, 0);
        lv_obj_t *l = lv_obj_get_child(c, -1);
        if (l && lv_obj_check_type(l, &lv_label_class)) {
            /* one line, ending in "…": dots only cut a label whose height is set */
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(l, LV_SIZE_CONTENT);
            lv_obj_set_style_max_width(l, share - 48, 0);
            lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
        }
    }
}

/* ================================================================== the claude panel */

static void layout(void)
{
    int face_right = CP.panel_open ? PANEL_X - BZ_GAP : W - PAD;
    CP.face_cx = (PAD + face_right) / 2;
    int w = face_right - PAD;
    if (w > 900) w = 900;
    lv_obj_set_width(CP.status, w);
    lv_obj_set_pos(CP.status, CP.face_cx - w / 2, STATUS_Y);
    lv_obj_set_pos(CP.bubble_wrap, CP.face_cx - w / 2, BUBBLE_Y);
    lv_obj_set_width(CP.bubble_wrap, w);
    lv_obj_set_width(lv_obj_get_parent(CP.bubble_box), w - 2 * BZ_PAD_TILE); /* the scroller's clip */
    lv_obj_set_width(CP.bubble_box, w - 2 * BZ_PAD_TILE);
    lv_obj_set_width(CP.asked, w - 2 * BZ_PAD_TILE);
    lv_obj_set_width(CP.answer, w - 2 * BZ_PAD_TILE);
    if (CP.panel_open) lv_obj_remove_flag(CP.panel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(CP.panel, LV_OBJ_FLAG_HIDDEN);
    ui_chip_set(CP.cc_btn, CP.panel_open);
    CP.crev = 0xFFFFFFFFu;
}

static void row_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i >= CP.nrows || !CP.rows) return;
    ccw_ack(CP.rows[i].id);
    chime(CHIME_ACK);
    CP.crev = 0xFFFFFFFFu;
}

static void rows_build(const ccw_session_t *s, int n)
{
    if (!CP.rows) return;
    lv_obj_clean(CP.list);
    CP.nrows = n;
    int lw = PANEL_W - 2 * BZ_PAD_TILE;
    for (int i = 0; i < n; i++) {
        snprintf(CP.rows[i].id, sizeof CP.rows[i].id, "%s", s[i].id);
        lv_obj_t *r = bz_tile(CP.list, lw, LV_SIZE_CONTENT);
        bz_tile_set_fill(r, s[i].attention ? BZ_C_SURFACE3 : BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_set_style_radius(r, 20, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 3, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(r, row_tap, (void *)(intptr_t)i);
        lv_obj_t *top = bz_row(r, 10);
        CP.rows[i].mark = bz_mark(top, BZ_STALE, 12);
        CP.rows[i].title = bz_label_line(top, "", BZ_F_BODY_S, BZ_C_INK, lw - 32 - 30);
        CP.rows[i].sub = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, lw - 32);
        CP.rows[i].step = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_DIM, lw - 32);
        CP.rows[i].eta = bz_label(r, "", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_set_width(CP.rows[i].eta, lw - 32);
        lv_label_set_long_mode(CP.rows[i].eta, LV_LABEL_LONG_WRAP);
        CP.rows[i].row = r;
    }
}

static void rows_fill(const ccw_session_t *s, int n, double now)
{
    char a[24], eta[140];
    for (int i = 0; CP.rows && i < n && i < CP.nrows; i++) {
        bz_status_t st = s[i].state == CCW_RUNNING ? BZ_INFO : s[i].state == CCW_DONE ? BZ_OK
                         : s[i].state == CCW_WAITING ? BZ_WARN : BZ_FAULT;
        bz_mark_set(CP.rows[i].mark, s[i].attention || s[i].state == CCW_RUNNING ? st : BZ_STALE);
        ui_text(CP.rows[i].title, "%s", s[i].title[0] ? s[i].title : s[i].id);
        double el = s[i].elapsed_s + (s[i].state == CCW_RUNNING ? now - s[i].polled_at : 0);
        ui_text(CP.rows[i].sub, "%s · %s · %s · %d tool%s", s[i].project[0] ? s[i].project : "?", ccw_state_word(s[i].state),
                fmt_dur(el, a, sizeof a), s[i].tools, s[i].tools == 1 ? "" : "s");
        ui_text(CP.rows[i].step, "%s", s[i].step);
        eta_text(&s[i], now, eta, sizeof eta);
        ui_text(CP.rows[i].eta, "%s%s", eta, s[i].attention ? " · tap to clear" : "");
    }
}

static void panel_sync(double now)
{
    static double last;
    uint32_t rev = ccw_rev();
    bool tick = now - last > 5; /* elapsed and eta move on by the minute */
    if (rev == CP.crev && !tick) return;
    last = now;
    ccw_session_t *s = malloc(sizeof *s * CCW_MAX);
    if (!s) return;
    int n = ccw_list(s, CCW_MAX), att = 0, running = 0;
    for (int i = 0; i < n; i++) {
        att += s[i].attention;
        running += s[i].state == CCW_RUNNING;
    }
    if (att) ui_text(CP.cc_label, "claude %d!", att);
    else if (running) ui_text(CP.cc_label, "claude %d", running);
    else ui_text(CP.cc_label, "claude");
    if (CP.panel_open) {
        bool same = n == CP.nrows;
        for (int i = 0; i < n && same && CP.rows; i++) same = !strcmp(CP.rows[i].id, s[i].id);
        if (rev != CP.crev) {
            /* the fill shows who wants a look: rebuild when that or the set of sessions changes */
            static uint32_t att_sig;
            uint32_t sig = 0;
            for (int i = 0; i < n; i++) sig = sig * 31 + (uint32_t)s[i].attention;
            if (!same || sig != att_sig) rows_build(s, n);
            att_sig = sig;
        }
        rows_fill(s, n, now);
        link_status_t ls;
        link_status(&ls);
        ui_text(CP.panel_head, "claude code on %s", ls.reachable && ls.name[0] ? ls.name : ls.reachable ? "the pc" : "the pc (away)");
        if (!ccw_available())
            ui_text(CP.panel_note, "Catalyst Link isn't reporting Claude Code. On the PC run catalyst-link hook-settings and add the "
                                   "hooks it prints to Claude Code's settings.json (tab5/link/README.md).");
        else if (!n) ui_text(CP.panel_note, "No Claude Code sessions yet. They appear here as soon as one starts.");
        else ui_text(CP.panel_note, "%s", "");
    }
    CP.crev = rev;
    free(s);
}

static void cc_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    CP.panel_open = !CP.panel_open;
    layout();
    panel_sync(hal_seconds());
}

/* ================================================================== behaviour */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool point(lv_point_t *p)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return false;
    lv_indev_get_point(in, p);
    return true;
}

/* a finger on the face: the eyes follow it */
static void face_press(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    if (!point(&p)) return;
    CP.touch_at = hal_seconds();
    CP.tx = clampf((p.x - CP.face_cx) / 380.0f, -1, 1);
    CP.ty = clampf((p.y - EYE_CY) / 240.0f, -1, 1);
}

static void face_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    double now = hal_seconds();
    if (ccw_attention(NULL)) {
        /* the reminder is heard: stop it */
        ccw_ack(NULL);
        chime(CHIME_ACK);
        ui_island_say(BZ_I_TASK_ALT, "got it");
        CP.mood = EX_HAPPY;
        CP.mood_until = now + 1.5;
        CP.crev = 0xFFFFFFFFu;
        return;
    }
    lv_point_t p;
    if (!point(&p)) return;
    bool on_eye = fabsf((float)(p.y - EYE_CY)) < EH / 2 && (fabsf((float)(p.x - (CP.face_cx - EYE_GAP / 2))) < EW / 2 ||
                                                           fabsf((float)(p.x - (CP.face_cx + EYE_GAP / 2))) < EW / 2);
    if (on_eye) { /* poked in the eye: a blink and a squint */
        CP.blink_t0 = now;
        CP.mood = EX_FOCUSED;
        CP.mood_until = now + 0.7;
    } else {
        CP.mood = EX_HAPPY;
        CP.mood_until = now + 1.2;
        CP.bounce_t0 = now;
    }
}

/* The IMU at ~20 Hz: a knock on the desk or the tablet makes it start; a tilt rolls the eyes downhill. */
static void imu_step(double now)
{
    if (++CP.imu_div < 3) return;
    CP.imu_div = 0;
    hal_imu_t m;
    if (!hal_imu(&m) || !m.ok) return;
    if (!CP.imu_seeded) {
        CP.gxl = m.ax;
        CP.gyl = m.ay;
        CP.gzl = m.az;
        CP.imu_seeded = true;
    }
    float dx = m.ax - CP.gxl, dy = m.ay - CP.gyl, dz = m.az - CP.gzl;
    float jolt = sqrtf(dx * dx + dy * dy + dz * dz);
    CP.gxl += dx * 0.08f;
    CP.gyl += dy * 0.08f;
    CP.gzl += dz * 0.08f;
    if (jolt > 0.35f && now - CP.tap_at > 0.8) {
        CP.tap_at = now;
        CP.mood = EX_SURPRISED;
        CP.mood_until = now + 0.9;
        CP.blink_at = now + 0.5;
    }
    /* gravity's x in the screen's frame: the right side down is positive; small tilts do nothing */
    float t = CP.gxl;
    t = fabsf(t) < 0.08f ? 0 : t - (t > 0 ? 0.08f : -0.08f);
    CP.tilt = clampf(t * 2.2f, -1, 1);
}

static int running_sessions(void)
{
    static double at;
    static int n;
    double now = hal_seconds();
    if (now - at < 1) return n;
    at = now;
    ccw_session_t *s = malloc(sizeof *s * CCW_MAX);
    if (!s) return n;
    int k = ccw_list(s, CCW_MAX);
    n = 0;
    for (int i = 0; i < k; i++) n += s[i].state == CCW_RUNNING;
    free(s);
    return n;
}

static expr_id_t pick(double now, as_phase_t ph, int att, const ccw_session_t *a, double idle)
{
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    if (CP.mood_until > now) return CP.mood;
    if (ui_kb_open(CP.kb)) return EX_LISTEN;
    if (ph == AS_PHASE_CONFIRM) return EX_WORRIED;
    if (att) return a->state == CCW_DONE ? EX_HAPPY : EX_WORRIED;
    if (busy) return EX_FOCUSED;
    if (robot_fault()) return EX_FAULT;
    if (ph == AS_PHASE_ERROR && idle < 30) return EX_WORRIED;
    if (running_sessions() && idle > SLEEPY_S) return EX_FOCUSED; /* watching Claude work: not sleepy */
    if (idle > ASLEEP_S) return EX_ASLEEP;
    if (idle > SLEEPY_S) return EX_SLEEPY;
    return EX_NEUTRAL;
}

static void comp_frame(double now, double dt)
{
    if (dt > 0.1) dt = 0.1;
    imu_step(now);
    as_phase_t ph = assist_phase();
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    bool was = CP.last_phase != AS_PHASE_IDLE && CP.last_phase != AS_PHASE_ERROR && CP.last_phase != (as_phase_t)-1;
    if (was && !busy) {
        if (ph == AS_PHASE_IDLE) { /* an answer is in: a chime and a happy squint */
            chime(CHIME_ANSWER);
            CP.mood = EX_HAPPY;
            CP.mood_until = now + 2.5;
            CP.bounce_t0 = now;
        } else {
            CP.mood = EX_WORRIED;
            CP.mood_until = now + 3;
        }
    }
    CP.last_phase = ph;

    ccw_session_t a;
    int att = ccw_attention(&a);
    double idle = bz_ui_idle_s();
    expr_id_t ex = pick(now, ph, att, &a, idle);
    const expr_t *t = &EXPR[ex];
    eyes_color(t->color);
    float r = 9;
    float ow = CP.w, oh = CP.h, ol = CP.lid, os = CP.smile, oy = CP.y, ogx = CP.gx, ogy = CP.gy;
    CP.w = ease(CP.w, t->w, r, dt);
    CP.h = ease(CP.h, t->h, r, dt);
    CP.lid = ease(CP.lid, t->lid, r, dt);
    CP.smile = ease(CP.smile, t->smile, r, dt);
    CP.y = ease(CP.y, t->y, r, dt);

    /* where to look */
    float gx, gy;
    if (ui_kb_open(CP.kb)) {
        gx = 0;
        gy = 0.9f;
    } else if (now - CP.touch_at < 2.5) {
        gx = CP.tx;
        gy = CP.ty;
    } else if (ex == EX_ASLEEP) {
        gx = 0;
        gy = 0.3f;
    } else if (busy) {
        gx = -0.45f; /* thinking: up and to the side */
        gy = -0.7f;
    } else if (att && CP.panel_open) {
        gx = 0.95f; /* at the panel: over there */
        gy = 0.1f;
    } else {
        if (now > CP.wander_at) {
            bool centre = rnd() < 0.35f;
            CP.wx = centre ? 0 : (rnd() * 1.4f - 0.7f);
            CP.wy = centre ? 0 : (rnd() * 0.8f - 0.4f);
            /* now and then a look at the panel while Claude works */
            if (CP.panel_open && running_sessions() && rnd() < 0.3f) {
                CP.wx = 0.9f;
                CP.wy = 0;
            }
            CP.wander_at = now + (ex == EX_SLEEPY ? 3.5 : 1.2) + rnd() * 3.2f;
        }
        gx = CP.wx;
        gy = CP.wy;
    }
    gx = clampf(gx + CP.tilt * 0.8f, -1.2f, 1.2f);
    CP.gx = ease(CP.gx, gx, 18, dt);
    CP.gy = ease(CP.gy, gy, 18, dt);

    /* blinks: every few seconds, sometimes twice; none asleep */
    if (ex != EX_ASLEEP && now > CP.blink_at && CP.blink_t0 == 0) {
        CP.blink_t0 = now;
        if (CP.blinks_left > 0) {
            CP.blinks_left--;
            CP.blink_at = now + 2.2 + rnd() * 4.0f;
        } else if (rnd() < 0.15f) {
            CP.blinks_left = 1;
            CP.blink_at = now + 0.3;
        } else {
            CP.blink_at = now + (ex == EX_SLEEPY ? 1.6 : 2.2) + rnd() * 4.0f;
        }
    }
    eyes_apply(now);
    bubble_sync(now);

    bool moving = fabsf(CP.w - ow) + fabsf(CP.h - oh) + fabsf(CP.lid - ol) + fabsf(CP.smile - os) > 0.0005f ||
                  fabsf(CP.y - oy) > 0.05f || fabsf(CP.gx - ogx) + fabsf(CP.gy - ogy) > 0.001f || CP.blink_t0 > 0 ||
                  CP.bounce_t0 > 0;
    if (moving && !bz_ui_calm()) bz_ui_keep_alive();
}

static void comp_refresh(void)
{
    double now = hal_seconds();
    char line[256];
    status_line(now, line, sizeof line);
    ui_text(CP.status, "%s", line);
    refresh_chips();
    panel_sync(now);
    as_phase_t ph = assist_phase();
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    static int shown = -1;
    if (shown != (int)busy) {
        shown = busy;
        lv_obj_t *talk = lv_obj_get_child(CP.bar, 0);
        bz_icon_set(lv_obj_get_child(talk, 0), busy ? BZ_I_STOP_CIRCLE : BZ_I_KEYBOARD, 24, false);
        ui_text(lv_obj_get_child(talk, 1), "%s", busy ? "stop" : "talk");
    }
}

static void comp_open(void)
{
    double now = hal_seconds();
    CP.open = true;
    CP.arev = 0;
    CP.crev = 0xFFFFFFFFu;
    CP.last_phase = assist_phase();
    CP.blink_at = now + 1.0;
    CP.wander_at = now + 0.6;
    CP.imu_seeded = false;
    CP.mood = EX_SURPRISED; /* hello */
    CP.mood_until = now + 0.6;
    CP.nsugg = -1;
    layout();
}

static void comp_close(void)
{
    CP.open = false;
    ui_kb_hide(CP.kb);
    CP.last_seen = hal_seconds();
}

static void comp_build(lv_obj_t *b)
{
    CP.body = b;
    CP.built = true;
    CP.rng = (uint32_t)(hal_seconds() * 1000.0) | 1u;
    CP.w = CP.h = 1;
    CP.color = BZ_C_ICE;
    CP.rows = lv_malloc_zeroed(ROWS * sizeof *CP.rows);

    /* the face: the whole upper screen takes touches (the eyes follow a finger), and lets drags through */
    CP.face = bz_box(b);
    lv_obj_set_pos(CP.face, 0, 0);
    lv_obj_set_size(CP.face, W, BUBBLE_Y);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSING, NULL);
    bz_on_tap(CP.face, face_tap, NULL);
    for (int i = 0; i < 2; i++) eye_build(&CP.eye[i], CP.face);

    lv_obj_t *head = bz_col(b, 2);
    lv_obj_align(head, LV_ALIGN_TOP_RIGHT, -PAD, 26);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    bz_label(head, "desk mode", BZ_F_LABEL, BZ_C_DIM);
    bz_label(head, "Companion", BZ_F_TITLE, BZ_C_INK);

    CP.status = bz_label_line(b, "", BZ_F_NAME, BZ_C_INK, 900);
    lv_obj_set_style_text_align(CP.status, LV_TEXT_ALIGN_CENTER, 0);

    /* the last answer, as a bubble */
    CP.bubble_wrap = bz_tile(b, 900, BUBBLE_H);
    lv_obj_set_style_pad_ver(CP.bubble_wrap, 14, 0);
    lv_obj_set_style_radius(CP.bubble_wrap, 28, 0);
    CP.bubble_box = ui_scroller(CP.bubble_wrap, 900 - 2 * BZ_PAD_TILE, BUBBLE_H - 28);
    lv_obj_set_style_pad_row(CP.bubble_box, 4, 0);
    lv_obj_set_style_pad_bottom(CP.bubble_box, 8, 0);
    CP.asked = bz_label_line(CP.bubble_box, "", BZ_F_LABEL, BZ_C_DIM, 900 - 2 * BZ_PAD_TILE);
    CP.answer = bz_label(CP.bubble_box, "", BZ_F_BODY, BZ_C_INK);
    lv_label_set_long_mode(CP.answer, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(CP.bubble_wrap, LV_OBJ_FLAG_HIDDEN);

    /* the bar: talk, three quick questions, claude, assist */
    CP.bar = bz_row(b, 10);
    lv_obj_set_pos(CP.bar, PAD, BAR_Y);
    lv_obj_set_size(CP.bar, W - 2 * PAD, BAR_H);
    lv_obj_set_style_clip_corner(CP.bar, false, 0);
    lv_obj_t *talk = ui_button(CP.bar, BZ_I_KEYBOARD, "talk", talk_tap, NULL);
    ui_chip_set(talk, true);
    lv_obj_set_width(talk, 140);
    CP.chips = bz_row(CP.bar, 8);
    lv_obj_set_size(CP.chips, W - 2 * PAD - 140 - 170 - 150 - 3 * 10, BAR_H);
    lv_obj_set_style_clip_corner(CP.chips, false, 0);
    CP.cc_btn = ui_button(CP.bar, BZ_I_TERMINAL, "claude", cc_tap, NULL);
    lv_obj_set_width(CP.cc_btn, 170);
    CP.cc_label = lv_obj_get_child(CP.cc_btn, 1);
    lv_obj_t *as = ui_button(CP.bar, BZ_I_AUTO_AWESOME, "assist", assist_tap, NULL);
    lv_obj_set_width(as, 150);

    /* claude code's sessions */
    CP.panel = bz_tile(b, PANEL_W, PANEL_H);
    lv_obj_set_pos(CP.panel, PANEL_X, PANEL_Y);
    lv_obj_set_flex_flow(CP.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(CP.panel, 10, 0);
    CP.panel_head = bz_label_line(CP.panel, "claude code", BZ_F_LABEL, BZ_C_DIM, PANEL_W - 2 * BZ_PAD_TILE);
    CP.panel_note = bz_label(CP.panel, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(CP.panel_note, PANEL_W - 2 * BZ_PAD_TILE);
    lv_label_set_long_mode(CP.panel_note, LV_LABEL_LONG_WRAP);
    CP.list = ui_scroller(CP.panel, PANEL_W - 2 * BZ_PAD_TILE, 200);
    lv_obj_set_flex_grow(lv_obj_get_parent(CP.list), 1); /* the rest of the panel, whatever the note takes */
    lv_obj_set_style_pad_row(CP.list, 10, 0);
    lv_obj_add_flag(CP.panel, LV_OBJ_FLAG_HIDDEN);

    CP.kb = ui_kb_create(b, 440);
    CP.last_phase = (as_phase_t)-1;
    layout();
}

const ui_app_t APP_COMPANION = { .name = "companion", .icon = BZ_I_VISIBILITY, .build = comp_build, .open = comp_open,
                                 .close = comp_close, .refresh = comp_refresh, .frame = comp_frame };

/* ================================================================== everywhere */

static bool g_power; /* on a charger or the robot's 12 V, as of the last second */

void ui_companion_open(bool claude, lv_obj_t *from)
{
    if (claude) CP.panel_open = true;
    if (CP.open) {
        if (CP.built) layout();
        return;
    }
    ui_app_open(&APP_COMPANION, from);
}

bool ui_companion_keeps_awake(void) { return CP.open && g_power; }

/* Desk mode by itself: standing (landscape, upright), still and on power for a minute, nobody touching it,
 * nothing else open, and not closed by hand in the last five minutes. */
static void desk_tick(double now)
{
    static double next, still_since;
    if (now < next) return;
    next = now + 1.0;
    hal_battery_t bat;
    g_power = hal_battery(&bat) && bat.ok && (bat.charging || bat.external);
    if (!CFG.desk_auto || CP.open || ui_app_any_open() || ui_asleep() || !g_power) {
        still_since = 0;
        return;
    }
    hal_imu_t m;
    bool still = hal_imu(&m) && m.ok && fabsf(m.ay) > 0.72f && fabsf(m.ax) < 0.35f &&
                 sqrtf(m.gx * m.gx + m.gy * m.gy + m.gz * m.gz) < 4.0f;
    if (!still) {
        still_since = 0;
        return;
    }
    if (still_since == 0) still_since = now;
    if (now - still_since > 60 && bz_ui_idle_s() > 45 && (CP.last_seen == 0 || now - CP.last_seen > 300)) {
        still_since = 0;
        ui_companion_open(false, NULL);
    }
}

void ui_companion_tick(double now, double dt)
{
    (void)dt;
    static double last_remind;
    ui_companion_cfg();
    now = hal_seconds(); /* chimes and reminders run on the tablet's own clock */
    chime_tick(now);
    ccw_event_t e;
    while (ccw_next_event(&e)) {
        char msg[96];
        const char *p = e.project[0] ? e.project : "claude code";
        const char *icon = BZ_I_TASK_ALT;
        if (e.state == CCW_DONE) {
            snprintf(msg, sizeof msg, "claude finished in %s", p);
            chime(CHIME_DONE);
        } else if (e.state == CCW_WAITING) {
            snprintf(msg, sizeof msg, "claude needs you in %s", p);
            icon = BZ_I_NOTIFICATIONS;
            chime(CHIME_INPUT);
        } else {
            snprintf(msg, sizeof msg, "claude hit an error in %s", p);
            icon = BZ_I_ERROR;
            chime(CHIME_ERROR);
        }
        ui_island_say(icon, msg);
        last_remind = now;
        if (CP.open) {
            CP.mood = e.state == CCW_DONE ? EX_HAPPY : EX_WORRIED;
            CP.mood_until = now + 3;
            if (e.state == CCW_DONE) CP.bounce_t0 = now;
        }
    }
    /* until someone taps, say it again every so often */
    if (CFG.remind_s > 0 && now - last_remind > CFG.remind_s) {
        ccw_session_t a;
        int n = ccw_attention(&a);
        last_remind = now;
        if (n) {
            char msg[96];
            snprintf(msg, sizeof msg, "still waiting: claude %s in %s%s",
                     a.state == CCW_DONE ? "finished" : a.state == CCW_WAITING ? "needs you" : "hit an error",
                     a.project[0] ? a.project : "a session", n > 1 ? " (+more)" : "");
            ui_island_say(a.state == CCW_DONE ? BZ_I_TASK_ALT : BZ_I_NOTIFICATIONS, msg);
            chime(a.state == CCW_DONE ? CHIME_DONE : a.state == CCW_WAITING ? CHIME_INPUT : CHIME_ERROR);
            if (CP.open) CP.bounce_t0 = now;
        }
    }
    desk_tick(now);
}
