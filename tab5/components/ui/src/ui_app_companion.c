/* companion — desk mode: a face for the tablet on its stand, and a voice.
 *
 * Two eyes that live: every feeling is a target shape (width, height, each upper lid's height and slant,
 * a happy crescent cut from below, a highlight, a tint) reached on springs; between feelings the eyes
 * glance about in saccades with small fixational jitters, breathe, and blink on a natural schedule with
 * the occasional double blink. While the companion speaks the eyes bob with the voice's loudness and a
 * small mouth moves with it; while it listens they open wide and a row of dots follows the microphones;
 * while it thinks they look up and aside.
 *
 * The voice (components/assist/voice.c): "Hi ESP" (the wake word, heard on the tablet) or a tap on the face
 * starts listening, the question goes to OpenAI, and the answer comes back spoken, shown, or both, with a
 * feeling the eyes take on. The microphones are on only while this screen is. Without an OpenAI key the
 * face still answers typed questions through the assistant, as before.
 *
 * The screen stays immersive: a status line appears only when something is worth saying, the answer's
 * caption fades after a while, and the controls (talk by keyboard, quick questions, Claude Code, assist)
 * come up with a tap and go again. Setup hints show only when nothing is configured.
 *
 * Claude mode: a panel lists Claude Code's sessions on the owner's PC as Catalyst Link reports them
 * (components/assist/ccwatch.c), with estimates always labelled so; a finished or waiting session makes
 * the eyes react, a chime play and the island say so, and the reminder repeats until someone taps.
 *
 * Cheap to draw on the lean renderer: each eye is one object that draws itself (a rounded rectangle, a
 * lid, a crescent and a highlight, clipped to the eye) and invalidates only the union of where the eye
 * was and where it is, and only when a whole pixel changed. Two eyes are at most ~70k changed pixels a
 * frame; at rest, between glances and breaths, nothing is redrawn at all. The mouth and the dots are a
 * few hundred pixels each. */
#include "ui_companion.h"

#include "assist.h"
#include "ccwatch.h"
#include "link.h"
#include "voice.h"
#include "ui_home_mode.h"

#include "lvgl_private.h" /* lv_area_intersect, and the layer's clip for drawing an eye */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EW 150                 /* an eye at rest */
#define EH 190
#define EYE_GAP 330            /* centre to centre */
#define EYE_CY 250
#define GAZE_X 44              /* how far the eyes travel, px */
#define GAZE_Y 26
#define CAN_W 290              /* each eye's canvas: the eye at its largest, anywhere it can look */
#define CAN_H 360
#define MOUTH_Y 438            /* the mouth's and the listening dots' centre line */
#define STATUS_Y 482
#define BAR_H 64
#define BAR_Y (H - PAD - BAR_H)
#define CAP_Y 526              /* the answer's caption */
#define CAP_H (H - 18 - CAP_Y)
#define PANEL_W 440
#define PANEL_X (W - PAD - PANEL_W)
#define PANEL_Y 104
#define PANEL_H (BAR_Y - BZ_GAP - PANEL_Y)
#define ROWS CCW_MAX
#define SLEEPY_S 90.0          /* nothing touched or said this long, and nothing going on: drowsy */
#define ASLEEP_S 300.0
#define CONTROLS_S 8.0         /* the controls stay this long after a tap */
#define CAPTION_S 25.0         /* an answer's caption stays this long after it's done */

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

/* ================================================================== feelings, as shapes */

/* The model's twelve feelings (voice.h's order), then the face's own states. */
enum {
    F_LISTEN = VO_EMO_COUNT, F_FOCUSED, F_FAULT, F_ASLEEP, F_COUNT
};

typedef struct {
    float w, h;            /* the eye's scale */
    float y;               /* px, down */
    float smile;           /* 0..1: a crescent cut from below (a happy eye) */
    float glint;           /* the highlight's scale (0: none) */
    float lid[2];          /* each upper lid's height at the eye's centre, 0 open .. 1 shut (left eye, right eye) */
    float tilt[2];         /* each lid's slant, in eye heights across the eye: + inner corner down (cross,
                              determined), - inner corner up (sad, worried) */
    float hs[2];           /* each eye's own height scale (curious, confused: one eye bigger) */
    uint32_t tint;         /* 0: the theme's ice */
    float tint_k;          /* how far toward the tint */
} shape_t;

#define ICE_ 0
static const shape_t SHAPE[F_COUNT] = {
    [VO_EMO_NEUTRAL] = { 1.00f, 1.00f, 0, 0, 1.0f, { 0, 0 }, { 0, 0 }, { 1, 1 }, ICE_, 0 },
    [VO_EMO_HAPPY] = { 1.04f, 0.96f, -5, 0.62f, 1.1f, { 0, 0 }, { -0.05f, -0.05f }, { 1, 1 }, 0x7BE495, 0.35f },
    [VO_EMO_EXCITED] = { 1.10f, 1.10f, -10, 0.38f, 1.35f, { 0, 0 }, { 0, 0 }, { 1, 1 }, 0xFFD166, 0.40f },
    [VO_EMO_CURIOUS] = { 1.02f, 1.04f, -4, 0, 1.15f, { 0.0f, 0.22f }, { -0.10f, 0.16f }, { 1.08f, 0.94f }, ICE_, 0 },
    [VO_EMO_THINKING] = { 0.96f, 0.90f, 0, 0, 0.9f, { 0.22f, 0.22f }, { 0.12f, 0.12f }, { 1.0f, 0.92f }, 0xA08CFF, 0.35f },
    [VO_EMO_SLEEPY] = { 1.04f, 0.86f, 10, 0, 0.6f, { 0.55f, 0.55f }, { -0.10f, -0.10f }, { 1, 1 }, 0x8C9BAA, 0.45f },
    [VO_EMO_SAD] = { 0.94f, 0.92f, 12, 0, 0.8f, { 0.28f, 0.28f }, { -0.45f, -0.45f }, { 1, 1 }, 0x6B8CFF, 0.45f },
    [VO_EMO_WORRIED] = { 0.90f, 0.98f, 4, 0, 0.9f, { 0.14f, 0.14f }, { -0.35f, -0.35f }, { 1, 1 }, 0xFFB347, 0.50f },
    [VO_EMO_SURPRISED] = { 1.12f, 1.24f, -8, 0, 0.7f, { 0, 0 }, { 0, 0 }, { 1, 1 }, 0xFFFFFF, 0.20f },
    [VO_EMO_LOVE] = { 1.06f, 1.00f, -4, 0.50f, 1.3f, { 0, 0 }, { -0.05f, -0.05f }, { 1, 1 }, 0xFF7AB6, 0.60f },
    [VO_EMO_PROUD] = { 1.04f, 0.90f, -12, 0.30f, 1.1f, { 0.26f, 0.26f }, { -0.08f, -0.08f }, { 1, 1 }, 0xFFC857, 0.40f },
    [VO_EMO_CONFUSED] = { 1.00f, 1.00f, 0, 0, 1.0f, { 0.30f, 0.0f }, { 0.35f, -0.25f }, { 0.90f, 1.10f }, 0xB9A7FF, 0.25f },
    [F_LISTEN] = { 1.05f, 1.08f, -4, 0, 1.25f, { 0, 0 }, { -0.04f, -0.04f }, { 1, 1 }, ICE_, 0 },
    [F_FOCUSED] = { 1.00f, 0.90f, 0, 0, 0.9f, { 0.18f, 0.18f }, { 0.10f, 0.10f }, { 1, 1 }, ICE_, 0 },
    [F_FAULT] = { 0.90f, 0.96f, 2, 0, 0.8f, { 0.20f, 0.20f }, { -0.25f, -0.25f }, { 1, 1 }, 0xFF5A5A, 0.60f },
    [F_ASLEEP] = { 1.08f, 0.90f, 16, 0, 0, { 0.92f, 0.92f }, { -0.05f, -0.05f }, { 1, 1 }, 0x6E7B88, 0.55f },
};

/* springs: the face changes feeling with a little life in it; the gaze jumps like an eye does */
static const bz_spring_t SP_SHAPE = { 320.0f, 24.0f, 1 };  /* ~0.35 s, slight overshoot */
static const bz_spring_t SP_GAZE = { 1800.0f, 76.0f, 1 };  /* a saccade: ~0.1 s, nearly critical */
static const bz_spring_t SP_DRIFT = { 260.0f, 30.0f, 1 };  /* following a finger or a tilt */
static const bz_spring_t SP_POP = { 620.0f, 20.0f, 1 };    /* a bright feeling arrives with a bounce */
static const bz_spring_t SP_SLOW = { 110.0f, 19.0f, 1 };   /* a heavy one sinks in, ~0.6 s, no bounce */

/* ================================================================== the eye objects */

typedef struct {
    int x, y, w, h, r;         /* the eye, in its canvas */
    int lid_l, lid_r;          /* the upper lid's edge at the eye's left and right sides (<= y: no lid) */
    int cut_y, cut_r;          /* the crescent: a circle centred under the eye (cut_r 0: none) */
    int gx, gy, gr;            /* the highlight (gr 0: none) */
    int g2x, g2y, g2r;         /* a second, smaller one below it */
    uint32_t rgb;
} eyedraw_t;

struct crow {
    char id[48];
    lv_obj_t *row, *title, *sub, *step, *eta, *mark;
};

static struct {
    bool built, open;
    lv_obj_t *body, *face, *can[2], *mouth, *dots[3], *status, *cap, *cap_box, *asked, *answer, *bar, *chips,
        *cc_btn, *cc_label, *talk_btn;
    lv_obj_t *panel, *list, *panel_note, *panel_head;
    ui_kb_t *kb;
    eyedraw_t ed[2];
    int face_cx;
    /* the face's springs */
    bz_motion_t w, h, y, smile, glint, lid[2], tilt[2], hs[2], cr, cg, cb, gx, gy;
    int feel;                  /* the shape being reached */
    float feel_k;              /* its intensity */
    /* behaviour */
    double blink_t0, blink_at, sacc_at, fix_until, touch_at, mood_until, bounce_t0, tap_at, open_at, active_at;
    int blinks_left;
    float blink_depth;         /* 1 a full blink; now and then a half one */
    double hop_at;             /* excited: the next little hop */
    float fx, fy, tx, ty;      /* the fixation point; the last touch */
    int mood;                  /* transient feeling, until mood_until */
    float mood_k;
    uint32_t rng;
    float talk;                /* the voice's loudness, smoothed */
    /* the IMU: gravity low-passed, for knocks and tilt */
    float gxl, gyl, gzl, tilt_x;
    bool imu_seeded;
    int imu_div;
    /* the mouth and dots, as applied */
    int mw, mh, dh[3], dstate;
    uint32_t mrgb;
    bool mouth_on, dots_on, cap_on, status_on, bar_on;
    /* the conversation */
    uint32_t vrev, arev, reply_seq;
    double cap_at;             /* when the caption last changed; it hides CAPTION_S after its answer is done */
    bool cap_has;              /* there is a caption to show */
    vo_state_t vstate;
    as_phase_t last_phase;
    double controls_until;
    /* the claude panel */
    bool panel_open;
    uint32_t crev;
    int nrows;
    struct crow *rows;         /* ROWS, on the heap: internal RAM is nearly gone */
    const char *sugg[3];
    char sugg_keep[3][80];
    int nsugg;
    double last_seen;          /* when the companion last closed (desk mode waits a while before coming back) */
    bool voice_on;
} CP;

static float rnd(void)
{
    CP.rng = CP.rng * 1664525u + 1013904223u;
    return (float)(CP.rng >> 8) / 16777216.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sp_init(bz_motion_t *m, float v)
{
    bz_motion_init(m, v, 0.002f);
    m->keep = true; /* the lean renderer makes other motions instant; a face must move */
}

static void sp_to(bz_motion_t *m, float t, bz_spring_t s)
{
    if (fabsf(m->target - t) > 1e-4f || (!m->running && fabsf(m->value - t) > 1e-4f)) {
        bz_motion_to(m, t, s);
        m->keep = true;
    }
}

static bool sp_tick(bz_motion_t *m) { return bz_motion_tick(m); }

/* The eye draws itself: the eye, its highlight, then the lid and the crescent in the ground's colour,
 * all clipped to the eye. */
static void eye_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const eyedraw_t *d = &CP.ed[o == CP.can[1]];
    if (d->w <= 0 || d->h <= 0) return;
    lv_area_t co;
    lv_obj_get_coords(o, &co);
    lv_area_t eye = { co.x1 + d->x, co.y1 + d->y, co.x1 + d->x + d->w - 1, co.y1 + d->y + d->h - 1 };
    lv_area_t clip_was = layer->_clip_area, clip;
    if (!lv_area_intersect(&clip, &clip_was, &eye)) return;
    layer->_clip_area = clip;

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(d->rgb);
    r.bg_opa = LV_OPA_COVER;
    r.radius = d->r;
    lv_draw_rect(layer, &r, &eye);
    if (d->gr > 0) {
        /* the highlight: the eye's colour toward white */
        uint32_t c = d->rgb;
        uint32_t hl = (((c >> 16 & 0xff) + 0xff * 2) / 3) << 16 | (((c >> 8 & 0xff) + 0xff * 2) / 3) << 8 | ((c & 0xff) + 0xff * 2) / 3;
        lv_draw_rect_dsc_t g;
        lv_draw_rect_dsc_init(&g);
        g.bg_color = lv_color_hex(hl);
        g.bg_opa = LV_OPA_COVER;
        g.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = { co.x1 + d->gx - d->gr, co.y1 + d->gy - d->gr, co.x1 + d->gx + d->gr, co.y1 + d->gy + d->gr };
        lv_draw_rect(layer, &g, &a);
        if (d->g2r > 0) {
            lv_area_t b = { co.x1 + d->g2x - d->g2r, co.y1 + d->g2y - d->g2r, co.x1 + d->g2x + d->g2r, co.y1 + d->g2y + d->g2r };
            lv_draw_rect(layer, &g, &b);
        }
    }
    lv_color_t ground = bz_lv(BZ_C_GROUND);
    if (d->lid_l > d->y || d->lid_r > d->y) {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = ground;
        t.opa = LV_OPA_COVER;
        int x0 = eye.x1 - 2, x1 = eye.x2 + 2, top = eye.y1 - 2;
        int yl = co.y1 + d->lid_l, yr = co.y1 + d->lid_r;
        /* the lid's edge, carried past the eye's sides so the slant reaches the corners */
        float slope = (float)(yr - yl) / (float)(d->w > 1 ? d->w : 1);
        float yl2 = yl - slope * 2, yr2 = yr + slope * 2;
        t.p[0].x = x0; t.p[0].y = top;
        t.p[1].x = x1; t.p[1].y = top;
        t.p[2].x = x1; t.p[2].y = (lv_value_precise_t)yr2;
        lv_draw_triangle(layer, &t);
        t.p[0].x = x0; t.p[0].y = top;
        t.p[1].x = x1; t.p[1].y = (lv_value_precise_t)yr2;
        t.p[2].x = x0; t.p[2].y = (lv_value_precise_t)yl2;
        lv_draw_triangle(layer, &t);
    }
    if (d->cut_r > 0) {
        lv_draw_rect_dsc_t c;
        lv_draw_rect_dsc_init(&c);
        c.bg_color = ground;
        c.bg_opa = LV_OPA_COVER;
        c.radius = LV_RADIUS_CIRCLE;
        int cx = co.x1 + d->x + d->w / 2, cy = co.y1 + d->cut_y;
        lv_area_t a = { cx - d->cut_r, cy - d->cut_r, cx + d->cut_r, cy + d->cut_r };
        lv_draw_rect(layer, &c, &a);
    }
    layer->_clip_area = clip_was;
}

/* Takes the new drawing of eye i; invalidates the union of where it was and where it is, if anything
 * changed by a whole pixel. */
static void eye_set(int i, const eyedraw_t *n)
{
    eyedraw_t *o = &CP.ed[i];
    if (!memcmp(o, n, sizeof *n)) return;
    lv_area_t co;
    lv_obj_get_coords(CP.can[i], &co);
    int x1 = n->x, y1 = n->y, x2 = n->x + n->w, y2 = n->y + n->h;
    if (o->w > 0) {
        if (o->x < x1) x1 = o->x;
        if (o->y < y1) y1 = o->y;
        if (o->x + o->w > x2) x2 = o->x + o->w;
        if (o->y + o->h > y2) y2 = o->y + o->h;
    }
    lv_area_t a = { co.x1 + x1 - 2, co.y1 + y1 - 2, co.x1 + x2 + 2, co.y1 + y2 + 2 };
    *o = *n;
    lv_obj_invalidate_area(CP.can[i], &a);
}

static lv_obj_t *canvas(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, CAN_W, CAN_H);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, eye_draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

static lv_obj_t *blob(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void shown(lv_obj_t *o, bool *on, bool want)
{
    if (want == *on) return;
    *on = want;
    if (want) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================== what it knows */

static bool robot_fault(void) { return R && R->connected && (R->n_errors > 0 || R->browned_out); }

static bool voice_ok(void)
{
    char why[80];
    return voice_ready(why, sizeof why);
}

/* No Link and no key of any kind: the only time the face shows how to set it up. */
static bool unconfigured(void)
{
    link_status_t ls;
    link_status(&ls);
    if (ls.configured) return false;
    assist_config_t *c = malloc(sizeof *c);
    if (!c) return false;
    assist_config(c);
    bool none = !c->api_key[0] && !c->oai_key[0];
    memset(c, 0, sizeof *c);
    free(c);
    return none;
}

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

/* The one line under the face, only when there is something worth saying; "" otherwise. */
static void status_line(double now, char *out, size_t n)
{
    out[0] = 0;
    as_phase_t ph = assist_phase();
    if (ph == AS_PHASE_CONFIRM) {
        snprintf(out, n, "the assistant wants to change something: open assist to approve or decline");
        return;
    }
    char vn[160];
    double at = voice_note(vn, sizeof vn);
    if (vn[0] && now - at < 5.0) {
        snprintf(out, n, "%s", vn);
        return;
    }
    ccw_session_t att;
    int natt = ccw_attention(&att);
    if (natt) {
        const char *what = att.state == CCW_DONE ? "Claude finished" : att.state == CCW_WAITING ? "Claude needs you" : "Claude hit an error";
        snprintf(out, n, "%s in %s: %s%s", what, att.project[0] ? att.project : "a session",
                 att.state == CCW_DONE ? att.title : att.step, natt > 1 ? " (and more: tap to clear)" : " (tap to clear)");
        return;
    }
    /* the assistant's own turn (typed questions without an OpenAI key go there) */
    if (ph == AS_PHASE_THINKING || ph == AS_PHASE_SENDING) {
        snprintf(out, n, "thinking...");
        return;
    }
    if (ph == AS_PHASE_TOOL) {
        snprintf(out, n, "looking...");
        return;
    }
    if (ph == AS_PHASE_ERROR && now - CP.cap_at < 8) {
        assist_lock();
        int c = assist_count();
        const as_entry_t *e = c ? assist_entry(c - 1) : NULL;
        snprintf(out, n, "%s", e && e->kind == AS_E_ERROR && e->text ? e->text : "the last question failed");
        assist_unlock();
        return;
    }
    if (unconfigured()) {
        snprintf(out, n, "set me up in settings, assistant: an OpenAI key lets me listen and talk");
        return;
    }
    if (robot_fault()) {
        snprintf(out, n, "the robot has %d error%s%s", R->n_errors, R->n_errors == 1 ? "" : "s",
                 R->browned_out ? " and browned out" : "");
        return;
    }
    /* for a moment after coming on screen: how to talk to it */
    if (now - CP.open_at < 5.0 && voice_ok()) {
        const char *w = voice_wake_word();
        if (w) snprintf(out, n, "say \"%s\", or tap me", w);
        else snprintf(out, n, "tap me to talk");
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

static void caption(const char *asked, const char *answer, double now)
{
    ui_text(CP.asked, "%s", asked);
    const char *cur = lv_label_get_text(CP.answer);
    if (!cur || strcmp(cur, answer) != 0) {
        lv_label_set_text(CP.answer, answer);
        ui_scroller_follow(CP.cap_box);
    }
    CP.cap_at = now;
    CP.cap_has = true;
}

/* The answer from the companion's own conversation. */
static void voice_sync(double now)
{
    uint32_t rev = voice_rev();
    vo_state_t st = voice_state();
    if (rev == CP.vrev && st == CP.vstate) return;
    CP.vrev = rev;
    vo_state_t was = CP.vstate;
    CP.vstate = st;
    if (st != VO_IDLE && st != VO_OFF) CP.active_at = now;
    vo_reply_t *r = malloc(sizeof *r);
    if (!r) return;
    if (voice_reply(r) && r->seq != CP.reply_seq) {
        CP.reply_seq = r->seq;
        voice_config_t c;
        voice_config(&c);
        char asked[300];
        snprintf(asked, sizeof asked, "you: %s", r->heard);
        if (!r->say[0]) {
            if (c.out != VO_OUT_SPEAK) caption(asked, "...", now);
        } else {
            /* a feeling to wear for the answer: while it's spoken, and a little after */
            CP.mood = r->emotion;
            CP.mood_k = r->intensity;
            CP.mood_until = now + (r->spoken ? 60.0 : 6.0); /* cut short when the speech ends */
            float lx = 0, ly = 0;
            switch (r->look) {
            case VO_LOOK_LEFT: lx = -0.75f; break;
            case VO_LOOK_RIGHT: lx = 0.75f; break;
            case VO_LOOK_UP: ly = -0.75f; break;
            case VO_LOOK_DOWN: ly = 0.65f; break;
            default: break;
            }
            CP.fx = lx;
            CP.fy = ly;
            CP.fix_until = now + (r->look == VO_LOOK_CENTER ? 0.5 : 1.6);
            if (r->emotion == VO_EMO_EXCITED || r->emotion == VO_EMO_HAPPY || r->emotion == VO_EMO_LOVE) CP.bounce_t0 = now;
            if (rnd() < 0.4f) CP.blink_t0 = now;
            if (c.out != VO_OUT_SPEAK) {
                size_t l = strlen(r->say) + 8;
                char *a = malloc(l);
                if (a) {
                    plain_into(a, l, r->say);
                    caption(asked, a, now);
                    free(a);
                }
            }
            if (!r->spoken) chime(CHIME_ANSWER);
        }
    }
    free(r);
    /* the answer's feeling outlasts the voice by a little; a follow-up question takes over at once */
    if (st != VO_THINKING && st != VO_SPEAKING && st != VO_HEARING && CP.mood < VO_EMO_COUNT && CP.mood_until > now + 3.0)
        CP.mood_until = st == VO_LISTEN ? now + 0.8 : now + 3.0;
    if (st == VO_LISTEN && was != VO_LISTEN) {
        CP.fx = 0;
        CP.fy = -0.08f;
        CP.fix_until = now + 0.4;
        if (rnd() < 0.5f) CP.blink_t0 = now;
    }
}

/* The assistant's last answer (typed questions without an OpenAI key). */
static void assist_sync(double now)
{
    uint32_t rev = assist_rev();
    if (rev == CP.arev) return;
    CP.arev = rev;
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
    if (last_user >= 0) caption(asked, answer ? answer : "...", now);
    free(answer);
}

static void ask(const char *q)
{
    double now = hal_seconds();
    CP.active_at = now;
    if (voice_ok()) {
        if (voice_ask(q)) hal_tone(1300, 12, S.volume * 0.4f);
        else ui_island_say(BZ_I_HOURGLASS_TOP, "still answering the last one");
        return;
    }
    char why[96];
    if (!assist_ready(why, sizeof why)) {
        ui_island_say(BZ_I_LINK_OFF, why);
        CP.mood = VO_EMO_WORRIED;
        CP.mood_k = 0.6f;
        CP.mood_until = now + 2.5;
        return;
    }
    if (assist_send(q)) {
        hal_tone(1300, 12, S.volume * 0.4f);
        CP.mood = F_LISTEN;
        CP.mood_k = 1;
        CP.mood_until = now + 0.8;
        CP.arev = 0;
    } else ui_island_say(BZ_I_HOURGLASS_TOP, "still working on the last one");
}

static void typed(const char *text, void *u)
{
    (void)u;
    while (*text == ' ' || *text == '\n') text++;
    if (*text) ask(text);
}

static bool busy_now(void)
{
    as_phase_t p = assist_phase();
    vo_state_t v = voice_state();
    return (p != AS_PHASE_IDLE && p != AS_PHASE_ERROR) || v == VO_LISTEN || v == VO_HEARING || v == VO_THINKING ||
           v == VO_SPEAKING;
}

static void talk_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (busy_now()) {
        assist_stop();
        voice_cancel();
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
    for (int i = 0; i < 2; i++) {
        int cx = CP.face_cx + (i ? EYE_GAP / 2 : -EYE_GAP / 2);
        lv_obj_set_pos(CP.can[i], cx - CAN_W / 2, EYE_CY - CAN_H / 2);
    }
    for (int i = 0; i < 3; i++) CP.dh[i] = -1;
    CP.mw = -1;
    lv_obj_set_width(CP.status, w);
    lv_obj_set_pos(CP.status, CP.face_cx - w / 2, STATUS_Y);
    lv_obj_set_pos(CP.cap, CP.face_cx - w / 2, CAP_Y);
    lv_obj_set_width(CP.cap, w);
    lv_obj_set_width(lv_obj_get_parent(CP.cap_box), w - 2 * BZ_PAD_TILE); /* the scroller's clip */
    lv_obj_set_width(CP.cap_box, w - 2 * BZ_PAD_TILE);
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
    CP.controls_until = hal_seconds() + CONTROLS_S;
    layout();
    panel_sync(hal_seconds());
}

/* ================================================================== touch */

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
    CP.active_at = CP.touch_at;
    CP.tx = clampf((p.x - CP.face_cx) / 380.0f, -1, 1);
    CP.ty = clampf((p.y - EYE_CY) / 240.0f, -1, 1);
}

static void face_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    double now = hal_seconds();
    CP.controls_until = now + CONTROLS_S;
    if (ccw_attention(NULL)) {
        /* the reminder is heard: stop it */
        ccw_ack(NULL);
        chime(CHIME_ACK);
        ui_island_say(BZ_I_TASK_ALT, "got it");
        CP.mood = VO_EMO_HAPPY;
        CP.mood_k = 0.8f;
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
        CP.mood = F_FOCUSED;
        CP.mood_k = 1;
        CP.mood_until = now + 0.7;
        return;
    }
    vo_state_t v = voice_state();
    if (v == VO_LISTEN || v == VO_HEARING || v == VO_THINKING || v == VO_SPEAKING) {
        voice_cancel(); /* a tap while it talks or listens: hush */
        CP.mood = VO_EMO_SURPRISED;
        CP.mood_k = 0.5f;
        CP.mood_until = now + 0.6;
        return;
    }
    if (voice_ok()) {
        voice_listen();
        return;
    }
    CP.mood = VO_EMO_HAPPY;
    CP.mood_k = 0.7f;
    CP.mood_until = now + 1.2;
    CP.bounce_t0 = now;
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
    if (jolt > 0.35f && now - CP.tap_at > 0.8 && voice_state() != VO_SPEAKING) {
        CP.tap_at = now;
        CP.active_at = now;
        CP.mood = VO_EMO_SURPRISED;
        CP.mood_k = 1;
        CP.mood_until = now + 0.9;
        CP.blink_at = now + 0.5;
    }
    /* gravity's x in the screen's frame: the right side down is positive; small tilts do nothing */
    float t = CP.gxl;
    t = fabsf(t) < 0.08f ? 0 : t - (t > 0 ? 0.08f : -0.08f);
    CP.tilt_x = clampf(t * 2.2f, -1, 1);
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

/* ================================================================== the face, frame by frame */

/* What to feel now, and how strongly. */
static int pick(double now, float *k)
{
    *k = 1;
    vo_state_t v = voice_state();
    as_phase_t ph = assist_phase();
    double idle = now - CP.active_at;
    double touch_idle = bz_ui_idle_s();
    if (touch_idle < idle) idle = touch_idle;
    if (CP.mood_until > now) {
        *k = CP.mood_k;
        return CP.mood;
    }
    if (v == VO_LISTEN || ui_kb_open(CP.kb)) return F_LISTEN;
    if (v == VO_HEARING || v == VO_THINKING) {
        *k = 0.8f;
        return VO_EMO_THINKING;
    }
    if (ph == AS_PHASE_CONFIRM) return VO_EMO_WORRIED;
    ccw_session_t a;
    if (ccw_attention(&a)) {
        *k = 0.8f;
        return a.state == CCW_DONE ? VO_EMO_HAPPY : VO_EMO_WORRIED;
    }
    if (ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR) return F_FOCUSED;
    if (robot_fault()) return F_FAULT;
    if (ph == AS_PHASE_ERROR && now - CP.cap_at < 20) {
        *k = 0.6f;
        return VO_EMO_WORRIED;
    }
    if (running_sessions() && idle > SLEEPY_S) return F_FOCUSED; /* watching Claude work: not sleepy */
    if (idle > ASLEEP_S) return F_ASLEEP;
    if (idle > SLEEPY_S) return VO_EMO_SLEEPY;
    return VO_EMO_NEUTRAL;
}

static void shape_to(int f, float k)
{
    const shape_t *t = &SHAPE[f], *n = &SHAPE[VO_EMO_NEUTRAL];
    /* intensity scales the step from neutral: a mild feeling is still visible */
    float s = f < VO_EMO_COUNT ? 0.45f + 0.55f * clampf(k, 0, 1) : 1.0f;
    /* how it arrives: bright feelings pop in with a bounce, heavy ones sink in slowly */
    bool pop = f == VO_EMO_SURPRISED || f == VO_EMO_EXCITED || f == VO_EMO_HAPPY || f == VO_EMO_LOVE || f == F_LISTEN;
    bool slow = f == VO_EMO_SAD || f == VO_EMO_SLEEPY || f == F_ASLEEP;
    bz_spring_t sp = pop ? SP_POP : slow ? SP_SLOW : SP_SHAPE;
#define MIX(field) (n->field + (t->field - n->field) * s)
    sp_to(&CP.w, MIX(w), sp);
    sp_to(&CP.h, MIX(h), sp);
    sp_to(&CP.y, MIX(y), sp);
    sp_to(&CP.smile, MIX(smile), sp);
    sp_to(&CP.glint, MIX(glint), sp);
    for (int i = 0; i < 2; i++) {
        sp_to(&CP.lid[i], MIX(lid[i]), sp);
        sp_to(&CP.tilt[i], MIX(tilt[i]), sp);
        sp_to(&CP.hs[i], MIX(hs[i]), sp);
    }
#undef MIX
    if (f == VO_EMO_SURPRISED || f == VO_EMO_EXCITED) {
        /* a pop: the eyes are thrown open past their mark and spring back */
        bz_motion_to_v(&CP.h, CP.h.target, SP_POP, CP.h.velocity + 3.0f * s);
        bz_motion_to_v(&CP.w, CP.w.target, SP_POP, CP.w.velocity + 1.6f * s);
        CP.h.keep = CP.w.keep = true;
    }
    uint32_t base = bz_color(BZ_C_ICE) & 0xFFFFFF;
    float tk = t->tint ? t->tint_k * s : 0;
    uint32_t tint = t->tint ? t->tint : base;
    float r = ((base >> 16) & 0xff) * (1 - tk) + ((tint >> 16) & 0xff) * tk;
    float g = ((base >> 8) & 0xff) * (1 - tk) + ((tint >> 8) & 0xff) * tk;
    float b = (base & 0xff) * (1 - tk) + (tint & 0xff) * tk;
    sp_to(&CP.cr, r, BZ_EFFECT);
    sp_to(&CP.cg, g, BZ_EFFECT);
    sp_to(&CP.cb, b, BZ_EFFECT);
}

/* The next blink: every 2 to 7 s (a few more when drowsy, fewer while attending), one in seven doubled. */
static void blink_schedule(double now, int f)
{
    float mean = f == VO_EMO_SLEEPY ? 2.4f : f == F_LISTEN ? 5.0f : 3.8f;
    float gap = -logf(1.0f - rnd() * 0.95f) * (mean - 1.2f) + 1.2f;
    if (gap > 9) gap = 9;
    CP.blink_depth = rnd() < 0.1f ? 0.55f : 1.0f; /* a half blink now and then */
    if (CP.blinks_left > 0) {
        CP.blinks_left--;
        CP.blink_at = now + gap;
    } else if (rnd() < 0.14f) {
        CP.blinks_left = 1;
        CP.blink_at = now + 0.22 + rnd() * 0.08f;
    } else {
        CP.blink_at = now + gap;
    }
}

/* 0 open .. 1 shut: a quick close (70 ms), a moment shut, a slower open (120 ms); slower when drowsy. */
static float blink_amount(double now, bool slow)
{
    if (CP.blink_t0 <= 0) return 0;
    float s = slow ? 1.8f : 1.0f;
    float t = (float)(now - CP.blink_t0);
    float c = 0.07f * s, hold = 0.03f * s, o = 0.12f * s;
    float d = CP.blink_depth > 0 ? CP.blink_depth : 1;
    if (t < c) {
        float u = t / c;
        return u * u * d;
    }
    if (t < c + hold) return d;
    if (t < c + hold + o) {
        float u = (t - c - hold) / o;
        return (1 - u * (2 - u)) * d;
    }
    CP.blink_t0 = 0;
    return 0;
}

/* Where to look: a fixation held for a while with tiny jitters around it, or a finger, or a tilt. */
static void gaze_step(double now, int f, bool attending)
{
    float gx, gy;
    bool drift = false;
    if (ui_kb_open(CP.kb)) {
        gx = 0;
        gy = 0.9f;
        drift = true;
    } else if (now - CP.touch_at < 2.5) {
        gx = CP.tx;
        gy = CP.ty;
        drift = true;
    } else if (f == F_ASLEEP) {
        gx = 0;
        gy = 0.3f;
        drift = true;
    } else {
        if (now > CP.fix_until) {
            /* a new fixation: somewhere, or back to you */
            /* where each feeling tends to look: sad down, proud up, thinking up and aside */
            float by = f == VO_EMO_SAD ? 0.45f : f == VO_EMO_PROUD ? -0.25f : f == VO_EMO_SLEEPY ? 0.3f : 0;
            if (attending) {
                CP.fx = (rnd() - 0.5f) * 0.18f;
                CP.fy = -0.05f + by * 0.6f + (rnd() - 0.5f) * 0.12f;
                CP.fix_until = now + 0.9 + rnd() * 1.6f;
            } else if (f == VO_EMO_WORRIED || f == F_FAULT) {
                /* darting: short looks here and there */
                CP.fx = (rnd() - 0.5f) * 1.0f;
                CP.fy = (rnd() - 0.5f) * 0.5f;
                CP.fix_until = now + 0.35 + rnd() * 0.55f;
            } else if (f == VO_EMO_CURIOUS) {
                /* looking all around */
                CP.fx = (rnd() - 0.5f) * 1.8f;
                CP.fy = (rnd() - 0.5f) * 1.1f;
                CP.fix_until = now + 0.8 + rnd() * 1.2f;
            } else if (f == VO_EMO_SAD || f == VO_EMO_PROUD) {
                CP.fx = (rnd() - 0.5f) * 0.6f;
                CP.fy = by + (rnd() - 0.5f) * 0.2f;
                CP.fix_until = now + 2.0 + rnd() * 2.0f;
            } else if (f == VO_EMO_THINKING) {
                CP.fx = -0.55f + (rnd() - 0.5f) * 0.3f;
                CP.fy = -0.65f + (rnd() - 0.5f) * 0.2f;
                CP.fix_until = now + 0.8 + rnd() * 1.5f;
            } else {
                bool centre = rnd() < 0.35f;
                float nx = centre ? 0 : (rnd() * 1.5f - 0.75f), ny = centre ? 0 : (rnd() * 0.9f - 0.45f);
                /* now and then a look at the panel while Claude works */
                if (CP.panel_open && running_sessions() && rnd() < 0.3f) {
                    nx = 0.9f;
                    ny = 0;
                }
                if (fabsf(nx - CP.fx) + fabsf(ny - CP.fy) > 0.6f && rnd() < 0.3f && CP.blink_t0 <= 0) CP.blink_t0 = now;
                CP.fx = nx;
                CP.fy = ny;
                CP.fix_until = now + (f == VO_EMO_SLEEPY ? 3.5 : 1.2) + rnd() * 3.0f;
            }
            CP.sacc_at = now + 0.3 + rnd() * 0.7f;
        }
        gx = CP.fx;
        gy = CP.fy;
        /* fixational jitter: a small hop around the point now and then */
        if (now > CP.sacc_at) {
            CP.sacc_at = now + 0.35 + rnd() * 1.1f;
            float a = attending ? 0.035f : 0.05f;
            gx += (rnd() - 0.5f) * a * 2;
            gy += (rnd() - 0.5f) * a * 2;
            CP.fx = gx;
            CP.fy = gy;
        }
    }
    gx = clampf(gx + CP.tilt_x * 0.8f, -1.2f, 1.2f);
    gy = clampf(gy, -1.1f, 1.1f);
    sp_to(&CP.gx, gx, drift ? SP_DRIFT : SP_GAZE);
    sp_to(&CP.gy, gy, drift ? SP_DRIFT : SP_GAZE);
}

static uint32_t rgb_now(void)
{
    int r = (int)clampf(CP.cr.value, 0, 255), g = (int)clampf(CP.cg.value, 0, 255), b = (int)clampf(CP.cb.value, 0, 255);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* Each feeling's own small, continuous life on top of its shape: a happy bob, love's heartbeat, a sad sway,
 * curiosity's lids working, drowsy lids drooping, confusion trading sides, a worried shiver. All of it
 * moves the eyes by a few pixels, so a frame redraws only the eyes. */
typedef struct {
    float dw, dh, dx, dy, dlid[2], dhs[2];
    bool on;
} life_t;

static void life(double now, int f, float k, life_t *L)
{
    memset(L, 0, sizeof *L);
    float t = (float)fmod(now, 3600.0), a = f < VO_EMO_COUNT ? 0.5f + 0.5f * clampf(k, 0, 1) : 1.0f;
    switch (f) {
    case VO_EMO_HAPPY:
        L->dy = -3.5f * a * (0.5f + 0.5f * sinf(t * 4.4f));
        break;
    case VO_EMO_EXCITED:
        L->dw = 0.03f * a * sinf(t * 13.0f);
        L->dh = 0.03f * a * sinf(t * 13.0f + 1.6f);
        break;
    case VO_EMO_LOVE: {
        /* a heartbeat: two quick swells, then a rest */
        float p = fmodf(t, 1.1f), b1 = (p - 0.08f) / 0.055f, b2 = (p - 0.30f) / 0.055f;
        float beat = expf(-b1 * b1) + 0.7f * expf(-b2 * b2);
        L->dw = L->dh = 0.07f * a * beat;
        break;
    }
    case VO_EMO_SAD:
        L->dy = 3.0f * a * sinf(t * 0.9f);
        L->dlid[0] = L->dlid[1] = 0.03f * a * (0.5f + 0.5f * sinf(t * 0.7f));
        break;
    case VO_EMO_CURIOUS:
        L->dlid[0] = 0.06f * a * (0.5f + 0.5f * sinf(t * 1.3f));
        L->dlid[1] = 0.06f * a * (0.5f - 0.5f * sinf(t * 1.3f));
        L->dhs[0] = 0.03f * a * sinf(t * 1.3f);
        break;
    case VO_EMO_THINKING:
        L->dlid[0] = L->dlid[1] = 0.04f * a * (0.5f + 0.5f * sinf(t * 2.1f));
        break;
    case VO_EMO_SLEEPY:
    case F_ASLEEP: {
        /* the lids sag, catch themselves, sag again */
        float droop = 0.5f + 0.5f * sinf(t * (f == F_ASLEEP ? 0.9f : 1.8f));
        droop = droop * droop * droop;
        L->dlid[0] = L->dlid[1] = (f == F_ASLEEP ? 0.05f : 0.16f) * droop;
        if (f == F_ASLEEP) L->dy = 3.0f * sinf(t * 1.2f);
        break;
    }
    case VO_EMO_CONFUSED: {
        float q = sinf(t * 1.7f) * 2.0f;
        q = q > 1 ? 1 : q < -1 ? -1 : q;
        L->dhs[0] = 0.06f * a * q;
        L->dhs[1] = -0.06f * a * q;
        break;
    }
    case VO_EMO_PROUD:
        L->dy = -2.0f * a * sinf(t * 1.1f);
        break;
    case VO_EMO_WORRIED:
    case F_FAULT:
        L->dx = fmodf(t, 2.6f) < 0.25f ? 1.6f * a * sinf(t * 23.0f) : 0; /* a little shiver now and then */
        break;
    case F_LISTEN:
        L->dh = 0.02f * sinf(t * 3.0f);
        break;
    default:
        return;
    }
    L->on = true;
}

/* The drawing of both eyes, from the springs' values. */
static void eyes_compose(double now, float blink, float bounce, float breath, const life_t *L)
{
    float gx = CP.gx.value, gy = CP.gy.value, talk = CP.talk;
    /* squash and stretch: in a fast glance the eyes stretch along the way they move */
    float vx = CP.gx.velocity * GAZE_X, vy = CP.gy.velocity * GAZE_Y, sp = sqrtf(vx * vx + vy * vy);
    float st = clampf(sp / 2600.0f, 0, 0.14f), sx = 0, sy = 0;
    if (sp > 1) {
        sx = st * (fabsf(vx) / sp) - 0.5f * st * (fabsf(vy) / sp);
        sy = st * (fabsf(vy) / sp) - 0.5f * st * (fabsf(vx) / sp);
    }
    float listen = voice_state() == VO_LISTEN ? voice_mic_level() : 0;
    uint32_t rgb = rgb_now();
    for (int i = 0; i < 2; i++) {
        float side = i ? 1.0f : -1.0f;
        /* depth: the eye on the side looked toward is a touch larger, and both narrow looking aside */
        float w = EW * (CP.w.value + L->dw) * (1 - 0.05f * fabsf(gx)) * (1 + 0.035f * gx * side) * (1 + 0.04f * listen) *
                  (1 + sx);
        float h = EH * (CP.h.value + L->dh) * (CP.hs[i].value + L->dhs[i]) * breath * (1 + 0.05f * talk) *
                  (1 + 0.035f * gx * side) * (1 + sy);
        h *= 1 - 0.94f * blink;
        if (h < 8) h = 8;
        if (w > CAN_W - 8) w = CAN_W - 8;
        if (h > CAN_H - 8) h = CAN_H - 8;
        float cx = CAN_W / 2.0f + gx * GAZE_X + L->dx;
        float cy = CAN_H / 2.0f + gy * GAZE_Y + CP.y.value + L->dy + bounce - 5 * talk + blink * EH * CP.h.value * 0.08f;
        eyedraw_t d = { 0 };
        d.w = (int)lroundf(w);
        d.h = (int)lroundf(h);
        d.x = (int)lroundf(cx - w / 2);
        d.y = (int)lroundf(cy - h / 2);
        if (d.x < 2) d.x = 2;
        if (d.y < 2) d.y = 2;
        if (d.x + d.w > CAN_W - 2) d.x = CAN_W - 2 - d.w;
        if (d.y + d.h > CAN_H - 2) d.y = CAN_H - 2 - d.h;
        int m = d.w < d.h ? d.w : d.h;
        d.r = (int)(m * 0.45f);
        /* the lid: its height at the centre, slanted; the inner corner is toward the other eye */
        float full = EH * CP.h.value * CP.hs[i].value; /* the lid moves with the open eye, not the blink */
        float lc = d.y + clampf(CP.lid[i].value + L->dlid[i], 0, 1) * full * (1 - blink);
        float sl = CP.tilt[i].value * full * 0.5f;
        float inner = lc + sl, outer = lc - sl;
        float ll = i ? inner : outer, lr = i ? outer : inner;
        if (CP.lid[i].value + L->dlid[i] > 0.01f || fabsf(CP.tilt[i].value) > 0.01f) {
            d.lid_l = (int)lroundf(ll);
            d.lid_r = (int)lroundf(lr);
            if (d.lid_l <= d.y && d.lid_r <= d.y) d.lid_l = d.lid_r = 0;
        }
        /* the crescent: a circle rising from below */
        if (CP.smile.value > 0.02f) {
            d.cut_r = (int)lroundf(d.w * 0.78f);
            float top = d.y + d.h * (1 - 0.55f * CP.smile.value);
            d.cut_y = (int)lroundf(top + d.cut_r);
        }
        /* the highlight: up and to the side, moving a little more than the eye (it's nearer the glass) */
        float gs = CP.glint.value * (1 - blink);
        if (gs > 0.05f && d.h > 30) {
            d.gr = (int)lroundf(11 * gs);
            d.gx = (int)lroundf(d.x + d.w * 0.30f + gx * 7);
            d.gy = (int)lroundf(d.y + d.h * 0.26f + gy * 5);
            if (d.gr < 2) d.gr = 0;
            d.g2r = d.gr > 4 ? (int)lroundf(d.gr * 0.45f) : 0;
            d.g2x = d.gx + (int)lroundf(d.gr * 1.5f);
            d.g2y = d.gy + (int)lroundf(d.gr * 1.7f);
        }
        d.rgb = rgb;
        eye_set(i, &d);
    }
    (void)now;
}

/* The mouth while it speaks; the dots while it listens, thinks, or waits for its wake word. */
static void mouth_and_dots(double now)
{
    vo_state_t v = voice_state();
    uint32_t rgb = rgb_now();
    bool speak = v == VO_SPEAKING;
    shown(CP.mouth, &CP.mouth_on, speak);
    if (speak) {
        /* louder opens it rounder: taller, a little narrower */
        int mw = (int)lroundf(66 + 14 * CP.talk - 16 * CP.talk * CP.talk), mh = (int)lroundf(6 + 34 * CP.talk);
        if (mw != CP.mw || mh != CP.mh) {
            lv_obj_set_size(CP.mouth, mw, mh);
            lv_obj_set_pos(CP.mouth, CP.face_cx - mw / 2, MOUTH_Y - mh / 2);
            CP.mw = mw;
            CP.mh = mh;
        }
        if (rgb >> 4 != CP.mrgb >> 4) { /* only a visible change recolours it */
            lv_obj_set_style_bg_color(CP.mouth, lv_color_hex(rgb), 0);
            CP.mrgb = rgb;
        }
    }
    /* dots: 1 the wake word is on (one faint dot), 2 listening (a small level meter), 3 thinking (a ripple) */
    int st = 0;
    if (v == VO_LISTEN) st = 2;
    else if (v == VO_HEARING || v == VO_THINKING) st = 3;
    else if (v == VO_IDLE && voice_wake_word()) st = 1;
    bool on = st != 0;
    if (on != CP.dots_on) {
        CP.dots_on = on;
        CP.dstate = 0; /* which of them show is decided again below */
        for (int i = 0; i < 3; i++) {
            if (on) lv_obj_remove_flag(CP.dots[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(CP.dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!on) return;
    if (st != CP.dstate) {
        CP.dstate = st;
        for (int i = 0; i < 3; i++) {
            lv_obj_remove_style(CP.dots[i], bz_style_fill(BZ_C_ICE), 0);
            lv_obj_remove_style(CP.dots[i], bz_style_fill(BZ_C_FAINT), 0);
            lv_obj_add_style(CP.dots[i], bz_style_fill(st == 1 ? BZ_C_FAINT : BZ_C_ICE), 0);
            if (st == 1 && i != 1) lv_obj_add_flag(CP.dots[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(CP.dots[i], LV_OBJ_FLAG_HIDDEN);
            CP.dh[i] = -1;
        }
    }
    float lvl = voice_mic_level();
    for (int i = 0; i < 3; i++) {
        int hgt;
        if (st == 1) hgt = 8;
        else if (st == 2) hgt = (int)lroundf(10 + 26 * clampf(lvl * (i == 1 ? 1.4f : 1.0f) * 1.6f, 0, 1));
        else hgt = (int)lroundf(10 + 6 * (0.5f + 0.5f * sinf((float)now * 7.0f - i * 1.1f)));
        if (hgt != CP.dh[i]) {
            int wd = st == 3 ? hgt : 10;
            lv_obj_set_size(CP.dots[i], wd, hgt);
            lv_obj_set_pos(CP.dots[i], CP.face_cx + (i - 1) * 24 - wd / 2, MOUTH_Y - hgt / 2);
            CP.dh[i] = hgt;
        }
    }
}

static void comp_frame(double now, double dt)
{
    if (dt > 0.1) dt = 0.1;
    imu_step(now);
    voice_sync(now);
    assist_sync(now);

    /* the assistant's own answer lands (typed questions without an OpenAI key) */
    as_phase_t ph = assist_phase();
    bool abusy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    bool was = CP.last_phase != AS_PHASE_IDLE && CP.last_phase != AS_PHASE_ERROR && CP.last_phase != (as_phase_t)-1;
    if (was && !abusy) {
        CP.mood = ph == AS_PHASE_IDLE ? VO_EMO_HAPPY : VO_EMO_WORRIED;
        CP.mood_k = 0.8f;
        CP.mood_until = now + (ph == AS_PHASE_IDLE ? 2.5 : 3);
        if (ph == AS_PHASE_IDLE) {
            chime(CHIME_ANSWER);
            CP.bounce_t0 = now;
        }
    }
    CP.last_phase = ph;

    float k;
    int f = pick(now, &k);
    if (f != CP.feel || fabsf(k - CP.feel_k) > 0.05f) {
        if (f != CP.feel && rnd() < 0.35f && CP.blink_t0 <= 0) CP.blink_t0 = now; /* a change of heart, a blink */
        CP.feel = f;
        CP.feel_k = k;
        shape_to(f, k);
    }
    vo_state_t v = voice_state();
    bool attending = v == VO_LISTEN || v == VO_SPEAKING || f == F_LISTEN;
    gaze_step(now, f, attending);

    /* the voice's loudness: quick to rise, slower to fall, like a jaw */
    float lv = v == VO_SPEAKING ? voice_speak_level() : 0;
    CP.talk += (lv - CP.talk) * (lv > CP.talk ? 0.55f : 0.22f);
    if (CP.talk < 0.004f) CP.talk = 0;

    /* blinks: none asleep */
    if (f != F_ASLEEP && CP.blink_t0 <= 0 && now > CP.blink_at) {
        CP.blink_t0 = now;
        blink_schedule(now, f);
    }
    float blink = blink_amount(now, f == VO_EMO_SLEEPY || f == VO_EMO_PROUD);
    /* excited: little hops, over and over */
    if (f == VO_EMO_EXCITED && CP.bounce_t0 <= 0 && now > CP.hop_at) {
        CP.bounce_t0 = now;
        CP.hop_at = now + 1.0 + rnd() * 0.8f;
    }
    float bounce = 0;
    if (CP.bounce_t0 > 0) {
        float u = (float)((now - CP.bounce_t0) / 0.9);
        if (u >= 1) CP.bounce_t0 = 0;
        else bounce = -14.0f * fabsf(sinf(u * 3.14159f * 3)) * (1 - u);
    }
    /* breathing: 1.2 % of the height over ~4 s — about a pixel's change every few frames, no more */
    float breath = 1 + 0.012f * sinf((float)now * (2 * 3.14159f / 4.2f));

    bool moving = false;
    bz_motion_t *all[] = { &CP.w, &CP.h, &CP.y, &CP.smile, &CP.glint, &CP.lid[0], &CP.lid[1], &CP.tilt[0], &CP.tilt[1],
                           &CP.hs[0], &CP.hs[1], &CP.cr, &CP.cg, &CP.cb, &CP.gx, &CP.gy };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) moving |= sp_tick(all[i]);
    life_t L;
    life(now, f, k, &L);
    if (bz_ui_calm()) memset(&L, 0, sizeof L); /* reduced motion: the shapes, without the fidgets */
    eyes_compose(now, blink, bounce, breath, &L);
    mouth_and_dots(now);
    if ((moving || L.on || CP.blink_t0 > 0 || CP.bounce_t0 > 0 || CP.talk > 0 || CP.dots_on) && !bz_ui_calm())
        bz_ui_keep_alive();
    (void)dt;
}

/* 10 Hz: the status line, the caption, the controls, the panel, and the microphones' permission. */
static void comp_refresh(void)
{
    double now = hal_seconds();
    char line[256];
    status_line(now, line, sizeof line);
    shown(CP.status, &CP.status_on, line[0] != 0);
    if (line[0]) ui_text(CP.status, "%s", line);

    bool setup = unconfigured();
    bool controls = setup || ui_kb_open(CP.kb) || CP.panel_open || now < CP.controls_until;
    shown(CP.bar, &CP.bar_on, controls);
    if (controls) refresh_chips();
    /* the caption: while its answer is fresh, and never under the controls */
    vo_state_t v = voice_state();
    bool fresh = now - CP.cap_at < CAPTION_S || v == VO_THINKING || v == VO_SPEAKING ||
                 (assist_phase() != AS_PHASE_IDLE && assist_phase() != AS_PHASE_ERROR);
    shown(CP.cap, &CP.cap_on, CP.cap_has && fresh && !controls);
    if (CP.cap_has && (v == VO_SPEAKING || v == VO_THINKING)) CP.cap_at = now; /* the countdown starts when it's done */
    panel_sync(now);

    bool busy = busy_now();
    static int shown_busy = -1;
    if (shown_busy != (int)busy) {
        shown_busy = busy;
        bz_icon_set(lv_obj_get_child(CP.talk_btn, 0), busy ? BZ_I_STOP_CIRCLE : BZ_I_KEYBOARD, 24, false);
        ui_text(lv_obj_get_child(CP.talk_btn, 1), "%s", busy ? "stop" : "type");
    }
}

static void comp_open(void)
{
    double now = hal_seconds();
    CP.open = true;
    CP.open_at = now;
    CP.active_at = now;
    CP.arev = assist_rev(); /* an old answer isn't news */
    CP.vrev = 0;
    CP.crev = 0xFFFFFFFFu;
    CP.last_phase = assist_phase();
    CP.blink_at = now + 1.0;
    CP.fix_until = now + 0.6;
    CP.imu_seeded = false;
    CP.mood = VO_EMO_SURPRISED; /* hello */
    CP.mood_k = 0.7f;
    CP.mood_until = now + 0.6;
    CP.nsugg = -1;
    CP.controls_until = 0;
    CP.cap_at = 0;
    CP.cap_has = false;
    CP.cap_on = true;
    shown(CP.cap, &CP.cap_on, false);
    vo_reply_t *r = malloc(sizeof *r);
    if (r) {
        voice_reply(r);
        CP.reply_seq = r->seq; /* likewise */
        free(r);
    }
    layout();
    voice_enable(true);
    CP.voice_on = true;
}

static void comp_close(void)
{
    CP.open = false;
    ui_kb_hide(CP.kb);
    CP.last_seen = hal_seconds();
    voice_enable(false); /* the microphones go off with the face */
    CP.voice_on = false;
}

static void comp_build(lv_obj_t *b)
{
    CP.body = b;
    CP.built = true;
    CP.rng = (uint32_t)(hal_seconds() * 1000.0) | 1u;
    CP.rows = lv_malloc_zeroed(ROWS * sizeof *CP.rows);
    CP.feel = -1;
    const shape_t *n = &SHAPE[VO_EMO_NEUTRAL];
    sp_init(&CP.w, n->w);
    sp_init(&CP.h, n->h);
    sp_init(&CP.y, 0);
    sp_init(&CP.smile, 0);
    sp_init(&CP.glint, n->glint);
    for (int i = 0; i < 2; i++) {
        sp_init(&CP.lid[i], 0);
        sp_init(&CP.tilt[i], 0);
        sp_init(&CP.hs[i], 1);
    }
    uint32_t ice = bz_color(BZ_C_ICE);
    sp_init(&CP.cr, (float)((ice >> 16) & 0xff));
    sp_init(&CP.cg, (float)((ice >> 8) & 0xff));
    sp_init(&CP.cb, (float)(ice & 0xff));
    sp_init(&CP.gx, 0);
    sp_init(&CP.gy, 0);
    CP.gx.eps = CP.gy.eps = 0.004f;
    CP.cr.eps = CP.cg.eps = CP.cb.eps = 0.5f;

    /* the face: the whole upper screen takes touches (the eyes follow a finger), and lets drags through */
    CP.face = bz_box(b);
    lv_obj_set_pos(CP.face, 0, 0);
    lv_obj_set_size(CP.face, W, CAP_Y);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSING, NULL);
    bz_on_tap(CP.face, face_tap, NULL);
    for (int i = 0; i < 2; i++) CP.can[i] = canvas(CP.face);
    CP.mouth = blob(CP.face);
    for (int i = 0; i < 3; i++) {
        CP.dots[i] = blob(CP.face);
        lv_obj_add_style(CP.dots[i], bz_style_fill(BZ_C_FAINT), 0);
    }

    CP.status = bz_label_line(b, "", BZ_F_BODY_S, BZ_C_DIM, 900);
    lv_obj_set_style_text_align(CP.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(CP.status, LV_OBJ_FLAG_HIDDEN);

    /* the last answer, as a caption */
    CP.cap = bz_tile(b, 900, CAP_H);
    lv_obj_set_style_pad_ver(CP.cap, 12, 0);
    lv_obj_set_style_radius(CP.cap, 28, 0);
    CP.cap_box = ui_scroller(CP.cap, 900 - 2 * BZ_PAD_TILE, CAP_H - 24);
    lv_obj_set_style_pad_row(CP.cap_box, 4, 0);
    lv_obj_set_style_pad_bottom(CP.cap_box, 8, 0);
    CP.asked = bz_label_line(CP.cap_box, "", BZ_F_LABEL, BZ_C_DIM, 900 - 2 * BZ_PAD_TILE);
    CP.answer = bz_label(CP.cap_box, "", BZ_F_BODY, BZ_C_INK);
    lv_label_set_long_mode(CP.answer, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(CP.cap, LV_OBJ_FLAG_HIDDEN);

    /* the controls: type, three quick questions, claude, assist (up with a tap, gone after a while) */
    CP.bar = bz_row(b, 10);
    lv_obj_set_pos(CP.bar, PAD, BAR_Y);
    lv_obj_set_size(CP.bar, W - 2 * PAD, BAR_H);
    lv_obj_set_style_clip_corner(CP.bar, false, 0);
    CP.talk_btn = ui_button(CP.bar, BZ_I_KEYBOARD, "type", talk_tap, NULL);
    ui_chip_set(CP.talk_btn, true);
    lv_obj_set_width(CP.talk_btn, 140);
    CP.chips = bz_row(CP.bar, 8);
    lv_obj_set_size(CP.chips, W - 2 * PAD - 140 - 170 - 150 - 3 * 10, BAR_H);
    lv_obj_set_style_clip_corner(CP.chips, false, 0);
    CP.cc_btn = ui_button(CP.bar, BZ_I_TERMINAL, "claude", cc_tap, NULL);
    lv_obj_set_width(CP.cc_btn, 170);
    CP.cc_label = lv_obj_get_child(CP.cc_btn, 1);
    lv_obj_t *as = ui_button(CP.bar, BZ_I_AUTO_AWESOME, "assist", assist_tap, NULL);
    lv_obj_set_width(as, 150);
    lv_obj_add_flag(CP.bar, LV_OBJ_FLAG_HIDDEN);

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
    CP.vstate = VO_OFF;
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
    if (!CFG.desk_auto || CP.open || ui_app_any_open() || ui_asleep() || !g_power || ui_home_mode_active()) {
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
    const char *imported = assist_import_note(); /* keys found on the card at boot: say so, once */
    if (imported) ui_island_say(BZ_I_AUTO_AWESOME, imported);
    /* the microphones follow the face: off while the screen sleeps or the lock screen covers it */
    bool want = CP.open && !ui_asleep() && !ui_locked();
    if (want != CP.voice_on) {
        CP.voice_on = want;
        voice_enable(want);
    }
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
            CP.mood = e.state == CCW_DONE ? VO_EMO_HAPPY : VO_EMO_WORRIED;
            CP.mood_k = 0.9f;
            CP.mood_until = now + 3;
            CP.active_at = now;
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
