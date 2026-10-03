/* companion — desk mode: a face for the tablet on its stand, and a voice.
 *
 * The face is the avatar (ui_avatar.c): a small blob with two eyes, the same character as the pet's, drawn from the
 * Bible Strong file on the card or the built-in one. What it does is a state of the avatar: listening, thinking,
 * speaking (the eyes bob with the voice's loudness), and after an answer the feeling the model chose, mapped to
 * the nearest thing the character can show (happy, confused, surprised, curious, sad, love); asleep when nobody has
 * touched it for a while. Between feelings the avatar blinks and glances by itself; at rest nothing is redrawn.
 *
 * The voice (components/assist/voice.c): "Hi ESP" (the wake word, heard on the tablet) or a tap on the face
 * starts listening, the question goes to OpenAI, and the answer comes back spoken, shown, or both, with a
 * feeling the face takes on. The microphones are on only while this screen is. Without an OpenAI key the
 * face still answers typed questions through the assistant, as before.
 *
 * The screen stays immersive: a status line appears only when something is worth saying, the answer's
 * caption fades after a while, and the controls (talk by keyboard, quick questions, Claude Code, assist)
 * come up with a tap and go again. Setup hints show only when nothing is configured.
 *
 * Claude mode: a panel lists Claude Code's sessions on the owner's PC as Catalyst Link reports them
 * (components/assist/ccwatch.c), with estimates always labelled so; a finished or waiting session makes
 * the face react, a chime play and the island say so, and the reminder repeats until someone taps.
 *
 * The listening dots are a few hundred pixels, drawn only while the microphones are on. */
#include "ui_companion.h"

#include "ui_avatar.h"

#include "assist.h"
#include "ccwatch.h"
#include "link.h"
#include "voice.h"
#include "ui_home_mode.h"
#include "ui_md.h"
#include "ui_sys.h"


#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AVS 420                /* the avatar's box (the same size on every screen: one baked body serves them all) */
#define FACE_CY 230            /* where the head's centre sits */
#define MOUTH_Y 438            /* the listening dots' centre line */
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
struct crow {
    char id[48];
    lv_obj_t *row, *title, *sub, *step, *eta, *mark;
};

static struct {
    bool built, open;
    lv_obj_t *cap_rich;            /* an answer with markdown in it, drawn instead of the plain label */
    lv_obj_t *body, *face, *dots[3], *status, *cap, *cap_box, *asked, *answer, *bar, *chips,
        *cc_btn, *cc_label, *talk_btn;
    lv_obj_t *panel, *list, *panel_note, *panel_head;
    ui_kb_t *kb;
    int face_cx;
    ui_avatar_t *av;
    av_state_t state;          /* the state the avatar was last put in */
    double touch_at, mood_until, tap_at, open_at, active_at;
    int mood;                  /* transient feeling, until mood_until */
    float mood_k;
    uint32_t rng;
    float talk;                /* the voice's loudness, smoothed */
    /* the IMU: gravity low-passed, for knocks */
    float gxl, gyl, gzl;
    bool imu_seeded;
    int imu_div;
    /* the mouth and dots, as applied */
    int dh[3], dstate;
    bool dots_on, cap_on, status_on, bar_on;
    int cap_h;                 /* the caption's height as set: shorter while the controls are up */
    /* the conversation */
    uint32_t vrev, arev, reply_seq;
    uint32_t bg_seq;               /* the last reply said out of the island while the face was closed */
    double err_at;             /* the voice's last failure note, as already shown */
    bool talk_on_open;         /* opened by "tap to talk": listen as soon as it's up */
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

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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
    bool verr = false;
    double at = voice_note(vn, sizeof vn, &verr);
    if (vn[0] && now - at < (verr ? 10.0 : 5.0)) {
        snprintf(out, n, "%s", vn);
        return;
    }
    /* what the voice is doing: a face alone doesn't say it heard you */
    vo_state_t vs = voice_state();
    if (vs == VO_LISTEN || vs == VO_HEARING || vs == VO_THINKING) {
        hal_net_t net;
        hal_net(&net);
        if (vs != VO_LISTEN && !net.up)
            snprintf(out, n, "the Wi-Fi dropped: waiting for it to come back (tap to stop)");
        else
            snprintf(out, n, "%s", vs == VO_LISTEN ? "listening..." : vs == VO_HEARING ? "hearing you..." : "thinking...");
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


static void caption(const char *asked, const char *answer, const char *show, double now)
{
    ui_text(CP.asked, "%s", asked);
    /* An answer meant to be spoken is plain prose, and a label is the cheapest thing that draws it. Two
     * things can put markdown on screen instead. The model may fill "show" — a chart, a short list —
     * which is drawn under the prose and never read aloud, so both appear. Or it may reach for markdown
     * in the answer itself despite being told not to, and then the renderer draws that and the plain
     * label steps aside. Either way it is ui_md.c, the same renderer the assist app uses. */
    bool drawn = show && show[0];
    bool rich = !drawn && ui_md_is_rich(answer);
    if (drawn || rich) {
        if (!CP.cap_rich) {
            CP.cap_rich = bz_box(CP.cap_box);
            lv_obj_set_width(CP.cap_rich, lv_obj_get_width(CP.answer));
        }
        lv_obj_remove_flag(CP.cap_rich, LV_OBJ_FLAG_HIDDEN);
        ui_md_render(CP.cap_rich, drawn ? show : answer, lv_obj_get_width(CP.answer));
    }
    if (rich) {
        lv_obj_add_flag(CP.answer, LV_OBJ_FLAG_HIDDEN);
        ui_scroller_follow(CP.cap_box);
    } else {
        /* only when nothing was drawn at all: with "show" the prose and the block both stay */
        if (CP.cap_rich && !drawn) lv_obj_add_flag(CP.cap_rich, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(CP.answer, LV_OBJ_FLAG_HIDDEN);
        const char *cur = lv_label_get_text(CP.answer);
        if (!cur || strcmp(cur, answer) != 0) {
            lv_label_set_text(CP.answer, answer);
            ui_scroller_follow(CP.cap_box);
        }
    }
    CP.cap_at = now;
    CP.cap_has = true;
}

/* Something went wrong: a worried face for a while, and one low chime (not again while it's still worried). */
static void worry(double now, double hold)
{
    if (!(CP.mood == VO_EMO_WORRIED && CP.mood_until > now)) chime(CHIME_ERROR);
    CP.mood = VO_EMO_WORRIED;
    CP.mood_k = 0.7f;
    CP.mood_until = now + hold;
    CP.active_at = now;
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
        /* a failure before the words were ever transcribed has nothing to quote back: no empty "you:" */
        if (r->heard[0]) snprintf(asked, sizeof asked, "you: %s", r->heard);
        else asked[0] = 0;
        if (r->failed) {
            /* no answer: why, where it would have been, and a face and a sound that say so */
            caption(asked, r->say, NULL, now);
            worry(now, 4.0);
        } else if (!r->say[0]) {
            if (r->shown) caption(asked, "...", NULL, now);
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
            if (CP.av) ui_avatar_look(CP.av, lx, ly, r->look == VO_LOOK_CENTER ? 0.5f : 1.6f);
            /* caption() decides: plain prose into the label, markdown through the renderer */
            if (r->shown) caption(asked, r->say, r->show, now);
            if (!r->spoken) {
                chime(CHIME_ANSWER);
                /* meant to be heard, but the sound is off: say where the answer went, once per answer */
                if (c.out != VO_OUT_SHOW && S.volume < 0.02f && CP.open)
                    ui_island_say(BZ_I_VOLUME_OFF, "the sound is off: my answer is on screen");
            }
        }
    }
    free(r);
    /* a failure before any answer (the microphones, hearing you): the face and a low chime, once per note */
    char vn[8];
    bool verr = false;
    double at = voice_note(vn, sizeof vn, &verr);
    if (verr && at != CP.err_at) {
        CP.err_at = at;
        if (now - at < 2.0 && st != VO_THINKING) worry(now, 3.0);
    }
    /* the answer's feeling outlasts the voice by a little; a follow-up question takes over at once */
    if (st != VO_THINKING && st != VO_SPEAKING && st != VO_HEARING && CP.mood < VO_EMO_COUNT && CP.mood_until > now + 3.0)
        CP.mood_until = st == VO_LISTEN ? now + 0.8 : now + 3.0;
    if (st == VO_LISTEN && was != VO_LISTEN && CP.av) ui_avatar_look(CP.av, 0, -0.08f, 0.4f);
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
                {
                    size_t l = strlen(e->text) + 1;
                    answer = malloc(l);
                    if (answer) {
                        memcpy(answer, e->text, l); /* the lock goes below; caption reads it after */
                        assist_fold_text(answer);
                    }
                }
                break;
            }
        }
    }
    assist_unlock();
    if (last_user >= 0) caption(asked, answer ? answer : "...", NULL, now);
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
        CP.mood = VO_EMO_THINKING; /* it has taken the question */
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

static void memory_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_memory_open(AS_POOL_COMPANION, o);
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
    lv_obj_set_pos(ui_avatar_obj(CP.av), CP.face_cx - AVS / 2, FACE_CY - AVS / 2);
    for (int i = 0; i < 3; i++) CP.dh[i] = -1;
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

/* a finger on the face: it looks at it */
static void face_press(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    if (!point(&p)) return;
    CP.touch_at = hal_seconds();
    CP.active_at = CP.touch_at;
    /* the head turns toward the finger */
    ui_avatar_look(CP.av, clampf((p.x - CP.face_cx) / 380.0f, -1, 1), clampf((p.y - FACE_CY) / 240.0f, -1, 1), 2.5f);
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
    vo_state_t v = voice_state();
    if (v == VO_LISTEN || v == VO_HEARING || v == VO_THINKING || v == VO_SPEAKING) {
        voice_cancel(); /* a tap while it talks or listens: hush */
        CP.mood = VO_EMO_SURPRISED;
        CP.mood_k = 0.5f;
        CP.mood_until = now + 0.6;
        return;
    }
    char why[80];
    if (voice_ready(why, sizeof why)) {
        voice_listen();
        return;
    }
    /* it can't listen: say why (no Wi-Fi, no key) rather than a cheerful bounce that looks like it works */
    ui_island_say(strstr(why, "Wi-Fi") ? BZ_I_WIFI_OFF : BZ_I_AUTO_AWESOME, why);
    CP.mood = VO_EMO_SAD;
    CP.mood_k = 0.6f;
    CP.mood_until = now + 1.5;
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
    }
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

/* The feeling the model chose for an answer, as the nearest thing the avatar shows. */
static av_state_t mood_state(int m)
{
    switch (m) {
    case VO_EMO_HAPPY: case VO_EMO_EXCITED: case VO_EMO_PROUD: return AV_S_HAPPY;
    case VO_EMO_CURIOUS: return AV_S_CURIOUS;
    case VO_EMO_THINKING: return AV_S_THINK;
    case VO_EMO_SLEEPY: return AV_S_DROWSY;
    case VO_EMO_SAD: return AV_S_SAD;
    case VO_EMO_WORRIED: case VO_EMO_CONFUSED: return AV_S_CONFUSED;
    case VO_EMO_SURPRISED: return AV_S_SURPRISED;
    case VO_EMO_LOVE: return AV_S_LOVE;
    default: return voice_state() == VO_SPEAKING ? AV_S_SPEAK : AV_S_IDLE;
    }
}

/* What the face does now. */
static av_state_t pick(double now)
{
    vo_state_t v = voice_state();
    as_phase_t ph = assist_phase();
    double idle = now - CP.active_at;
    double touch_idle = bz_ui_idle_s();
    if (touch_idle < idle) idle = touch_idle;
    if (CP.mood_until > now) return mood_state(CP.mood);
    if (v == VO_LISTEN || ui_kb_open(CP.kb)) return AV_S_LISTEN;
    if (v == VO_HEARING || v == VO_THINKING) return AV_S_THINK;
    if (v == VO_SPEAKING) return AV_S_SPEAK;
    if (ph == AS_PHASE_CONFIRM) return AV_S_CONFUSED;
    ccw_session_t a;
    if (ccw_attention(&a)) return a.state == CCW_DONE ? AV_S_HAPPY : AV_S_CONFUSED;
    if (ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR) return AV_S_THINK;
    if (robot_fault()) return AV_S_SAD;
    if (ph == AS_PHASE_ERROR && now - CP.cap_at < 20) return AV_S_CONFUSED;
    if (running_sessions() && idle > SLEEPY_S) return AV_S_THINK; /* watching Claude work: not sleepy */
    if (idle > ASLEEP_S) return AV_S_SLEEP;
    if (idle > SLEEPY_S) return AV_S_DROWSY;
    return AV_S_IDLE;
}

/* The dots while it listens, thinks, or waits for its wake word. */
static void dots_update(double now)
{
    vo_state_t v = voice_state();
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
    (void)dt;
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
        if (ph == AS_PHASE_IDLE) chime(CHIME_ANSWER);
    }
    CP.last_phase = ph;

    av_state_t st = pick(now);
    if (st != CP.state) {
        CP.state = st;
        ui_avatar_state(CP.av, st);
    }
    /* the voice's loudness: quick to rise, slower to fall, like a jaw */
    vo_state_t v = voice_state();
    float lv = v == VO_SPEAKING ? voice_speak_level() : 0;
    CP.talk += (lv - CP.talk) * (lv > CP.talk ? 0.55f : 0.22f);
    if (CP.talk < 0.004f) CP.talk = 0;
    ui_avatar_level(CP.av, CP.talk);
    ui_avatar_calm(CP.av, st == AV_S_SLEEP ? 0.8f : st == AV_S_DROWSY ? 0.4f : 0.0f);
    bool moving = ui_avatar_frame(CP.av);
    dots_update(now);
    if ((moving || CP.talk > 0 || CP.dots_on) && !bz_ui_calm()) bz_ui_keep_alive();
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
    /* the caption: while its answer is fresh; shorter, above them, while the controls are up (a tap that
     * starts listening brings the controls too, and the answer mustn't wait for them to go) */
    vo_state_t v = voice_state();
    bool fresh = now - CP.cap_at < CAPTION_S || v == VO_THINKING || v == VO_SPEAKING ||
                 (assist_phase() != AS_PHASE_IDLE && assist_phase() != AS_PHASE_ERROR);
    int cap_h = controls ? BAR_Y - BZ_GAP - CAP_Y : CAP_H;
    if (cap_h != CP.cap_h) {
        CP.cap_h = cap_h;
        lv_obj_set_height(CP.cap, cap_h);
        lv_obj_set_height(lv_obj_get_parent(CP.cap_box), cap_h - 24); /* the scroller's clip */
        ui_scroller_follow(CP.cap_box);
    }
    shown(CP.cap, &CP.cap_on, CP.cap_has && fresh);
    if (CP.cap_has && (v == VO_SPEAKING || v == VO_THINKING)) CP.cap_at = now; /* the countdown starts when it's done */
    panel_sync(now);

    bool busy = busy_now();
    /* a conversation is use, as a touch is: the screen neither dims nor sleeps in the middle of one (it
     * slept mid-answer on battery, and going dark cut the answer off) */
    if (v == VO_LISTEN || v == VO_HEARING || v == VO_THINKING || v == VO_SPEAKING) bz_ui_wake();
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
    CP.state = AV_S_COUNT;
    ui_avatar_active(CP.av, true);
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
        CP.bg_seq = r->seq;
        free(r);
    }
    layout();
    voice_enable(true);
    CP.voice_on = true;
    CP.err_at = 0;
    if (CP.talk_on_open) {
        /* "tap to talk" from home mode: it listens at once, rather than waiting for a second tap */
        CP.talk_on_open = false;
        char why[80];
        if (voice_ready(why, sizeof why)) {
            voice_listen();
            CP.mood_until = 0; /* no "hello" surprise: it's listening */
        }
    }
}

static void comp_close(void)
{
    CP.open = false;
    ui_avatar_active(CP.av, false);
    ui_kb_hide(CP.kb);
    CP.last_seen = hal_seconds();
    CP.bg_seq = CP.reply_seq; /* what the face already showed is not announced again from home mode */
    voice_enable(false); /* the microphones go off with the face (home mode turns them back on next tick) */
    CP.voice_on = false;
}

static void comp_build(lv_obj_t *b)
{
    CP.body = b;
    CP.built = true;
    CP.rng = (uint32_t)(hal_seconds() * 1000.0) | 1u;
    CP.rows = lv_malloc_zeroed(ROWS * sizeof *CP.rows);
    CP.state = AV_S_COUNT;

    /* the face: the whole upper screen takes touches (it looks at a finger), and lets drags through */
    CP.face = bz_box(b);
    lv_obj_set_pos(CP.face, 0, 0);
    lv_obj_set_size(CP.face, W, CAP_Y);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(CP.face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(CP.face, face_press, LV_EVENT_PRESSING, NULL);
    bz_on_tap(CP.face, face_tap, NULL);
    CP.av = ui_avatar_create(CP.face, AVS);
    ui_avatar_calm(CP.av, 0);
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
    lv_obj_set_size(CP.chips, W - 2 * PAD - 140 - 170 - 150 - 64 - 4 * 10, BAR_H);
    lv_obj_set_style_clip_corner(CP.chips, false, 0);
    CP.cc_btn = ui_button(CP.bar, BZ_I_TERMINAL, "claude", cc_tap, NULL);
    lv_obj_set_width(CP.cc_btn, 170);
    CP.cc_label = lv_obj_get_child(CP.cc_btn, 1);
    lv_obj_t *as = ui_button(CP.bar, BZ_I_AUTO_AWESOME, "assist", assist_tap, NULL);
    lv_obj_set_width(as, 150);
    /* what she remembers (her own pool; the technician's is apart) */
    lv_obj_t *mb = ui_button(CP.bar, BZ_I_PSYCHOLOGY, NULL, memory_tap, NULL);
    lv_obj_set_style_pad_hor(mb, 0, 0);
    lv_obj_set_width(mb, 64);
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

void ui_companion_talk(lv_obj_t *from)
{
    if (CP.open) {
        char why[80];
        if (voice_ready(why, sizeof why)) voice_listen();
        return;
    }
    CP.talk_on_open = true;
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
    /* The microphones follow the face: off while the screen sleeps or the lock screen covers it.
     *
     * Home mode listens too. The tablet on its stand is meant to answer "Hi ESP" from across the room with
     * nothing touched, so while home mode is the thing on screen the wake word stays live — behind the same
     * `v_wake` setting, so it can be turned off, and only while nothing else is open over it. Anything that
     * hides home mode (an app, the lock screen, sleep) closes the microphones again, so the ES7210 is never
     * left powered with neither face on screen. */
    voice_config_t vc;
    voice_config(&vc);
    bool home_ears = ui_home_mode_showing() && vc.wake;
    bool want = (CP.open || home_ears) && !ui_asleep() && !ui_locked();
    if (want != CP.voice_on) {
        CP.voice_on = want;
        voice_enable(want);
    }
    /* With the face closed nothing draws the answer -- home mode listening, the owner having closed her
     * mid-answer (the turn goes on: voice.h), a question typed elsewhere -- so the island says it: every
     * reply, and above all every failure, instead of vanishing. Looked at only when the voice has news. */
    static uint32_t bg_rev;
    if (!CP.open && voice_rev() != bg_rev) {
        bg_rev = voice_rev();
        vo_reply_t *br = malloc(sizeof *br);
        if (br) {
            if (voice_reply(br) && br->seq != CP.bg_seq) {
                CP.bg_seq = br->seq;
                if (br->failed && br->say[0]) ui_island_say(BZ_I_ERROR, br->say);
                else if (br->say[0] && !br->spoken) ui_island_say(BZ_I_AUTO_AWESOME, br->say);
            }
            free(br);
        }
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
        }
    }
    desk_tick(now);
}
