/* Match alerts: an alarm before each of the team's matches, from The Blue Alliance's schedule.
 *
 * The data is tba.h's, on the home worker: this file only asks for a poll now and then (tba_want(), once per
 * poll: every POLL_LIVE_S on a day the event runs while a match is to come, every POLL_IDLE_S otherwise,
 * POLL_RETRY_S while there is nothing to go on) and reads our upcoming matches back through tba_upcoming()
 * when tba_gen() moves. Nothing here touches the network or holds a thread.
 *
 * Each upcoming match is tracked by its identity (level, set, number): its time (predicted, else scheduled),
 * the time the team was last told of, and which reminders have rung. At the queue lead (default 25 min: time
 * for the checklist) and the match lead (default 5 min) before it, the alarm screen comes up over everything
 * (the pages, apps, home mode, the lock screen: the glass layer's top), wakes the screen, and a burst of
 * chimes repeats every RING_EVERY_S, fuller and louder as it goes, until it is dismissed or RING_S passes (the
 * screen stays until dismissed). The sound is synthesized here (see "the sound" below) and played on the
 * speaker's PCM stream. A reminder rings late if the tablet only learns of the match late, but never once
 * the match's time has passed. A known match moving by MOVE_S or more, or a new one appearing, is a
 * notification with a chime, and its reminders are armed again for the new time.
 *
 * Alarms live as long as the firmware runs, whatever app is open; after a reboot they are derived again from
 * TBA's data (the card cache first, then the network). Settings: kv "matchalert" = "on,queue,match,sound"
 * (minutes; 0 turns that reminder off), on the UI thread. */
#include "ui_internal.h"
#include "ui_home_mode.h"
#include "tba.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MA_TRACK 24
#define MOVE_S 180           /* a move this big is news */
#define RING_S 120           /* the sound gives up after this */
#define RING_EVERY_S 2.0
#define ALARM_VOL 0.7f       /* the speaker's level while ringing, at least */
#define POLL_LIVE_S 120.0
#define POLL_IDLE_S 1800.0
#define POLL_RETRY_S 120.0
#define FIRST_POLL_S 8.0     /* after boot: Wi-Fi's time to come up */
#define STALE_S 600          /* the alarm screen goes by itself this long after the match's time */

enum { K_QUEUE, K_MATCH };

typedef struct {
    tba_match_t m;           /* as last seen */
    time_t when;             /* predicted, else scheduled; 0 unknown */
    time_t told;             /* the time the team last heard of: a move is measured from it */
    uint8_t fired;           /* bit 0 the queue reminder, bit 1 the match reminder */
    bool seen;
} tracked_t;

static const int Q_MIN[6] = { 0, 15, 20, 25, 30, 40 };
static const int M_MIN[4] = { 0, 3, 5, 10 };

static struct {
    bool booted;
    /* settings */
    bool on, sound;
    int queue_min, match_min;
    /* the schedule */
    int team;
    bool configured;         /* a key and a team: something to poll */
    unsigned tba_gen;
    tba_phase_t phase;
    bool live, offline, have_base;
    char event[20];
    double next_poll, next_check;
    int ntr;
    tracked_t tr[MA_TRACK];
    tba_match_t up[MA_TRACK]; /* tba_upcoming's answer */
    /* the alarm */
    bool up_now;             /* the screen is up */
    bool ringing;            /* the sound is going */
    tracked_t al;
    int kind;
    double t0, next_burst;
    int bursts;
    bool vol_raised;
    double test_at;
    int test_kind;
    long shown_min;
    bool playing;            /* the speaker's stream is ours (a burst or the chime) */
    /* the alarm screen */
    lv_obj_t *root, *bar, *kind_l, *title, *with_l, *vs_l, *batt_l, *check_btn, *dismiss_btn;
    /* the settings view (the tba app's) */
    lv_obj_t *on_chip, *sound_chip, *q_chips[6], *m_chips[4], *next_l;
} MA;

/* ------------------------------------------------------------------ settings */

static void ma_load(void)
{
    MA.on = true, MA.sound = true, MA.queue_min = 25, MA.match_min = 5;
    char v[32];
    int on, q, m, s;
    if (hal_kv_get("matchalert", v, sizeof v) && sscanf(v, "%d,%d,%d,%d", &on, &q, &m, &s) == 4 && q >= 0 && q <= 90 &&
        m >= 0 && m <= 30) {
        MA.on = on != 0, MA.queue_min = q, MA.match_min = m, MA.sound = s != 0;
    }
}

static void ma_save(void)
{
    char v[32];
    snprintf(v, sizeof v, "%d,%d,%d,%d", MA.on, MA.queue_min, MA.match_min, MA.sound);
    hal_kv_set("matchalert", v);
}

/* the key is kv's (NVS: this thread); handed to tba.h, then wiped from here */
static void ma_config(void)
{
    MA.team = S.team;
    char *key = calloc(1, 1024);
    if (!key) return;
    if (!hal_kv_get("tba_key", key, 1024)) key[0] = 0;
    size_t l = strlen(key);
    while (l && (key[l - 1] == ' ' || key[l - 1] == '\r' || key[l - 1] == '\n')) key[--l] = 0;
    MA.configured = key[0] && S.team > 0;
    tba_config(key, S.team);
    memset(key, 0, 1024);
    free(key);
}

/* ------------------------------------------------------------------ words */

static time_t when_of(const tba_match_t *m) { return m->predicted ? m->predicted : m->time; }

static void hhmm(time_t t, char *b, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(b, n, "%d:%02d", tm.tm_hour, tm.tm_min);
}

/* "red with 1234, 5678" (our alliance and the partners), "against 111, 222, 333" */
static void partners(const tba_match_t *m, int team, char *with, size_t wn, char *vs, size_t vn)
{
    const int *us = m->ours == 2 ? m->blue : m->red, *them = m->ours == 2 ? m->red : m->blue;
    int o = snprintf(with, wn, "%s", m->ours == 2 ? "blue" : m->ours == 1 ? "red" : "not ours");
    bool first = true;
    for (int i = 0; i < 3 && o > 0 && (size_t)o < wn; i++) {
        if (!us[i] || us[i] == team) continue;
        o += snprintf(with + o, wn - (size_t)o, "%s%d", first ? " with " : ", ", us[i]);
        first = false;
    }
    o = snprintf(vs, vn, "against");
    first = true;
    for (int i = 0; i < 3 && o > 0 && (size_t)o < vn; i++) {
        if (!them[i]) continue;
        o += snprintf(vs + o, vn - (size_t)o, "%s%d", first ? " " : ", ", them[i]);
        first = false;
    }
    if (first) snprintf(vs, vn, "opponents not known yet");
}

/* ------------------------------------------------------------------ the sound
 *
 * Bell-like notes, synthesized: each is four partials (the fundamental, the octave, the twelfth and a faint
 * inharmonic shimmer at 4.2x, the upper ones decaying faster, as a struck bar's do) under a 6 ms raised-cosine
 * attack and an exponential decay, summed into a PSRAM buffer and run through a soft limiter, then streamed to
 * the speaker at 24 kHz (hal_play_*; the output task mixes hal_tone's ticks over it). Oscillators are rotations,
 * not sinf per sample: a burst of eight notes costs a few milliseconds on the P4, once per burst.
 *
 *   queue reminder   a rising E-major arpeggio, E5 G#5 B5 E6: "time to go"; later bursts add a double tap on
 *                    the top note, then a second arpeggio a fifth higher, faster
 *   match reminder   an urgent A5/E6 two-tone, four notes, then six faster, then eight with a brighter D6/A6 end
 *   schedule change  a soft two-note chime, G5 then D6, once
 *
 * Louder over the first minute (the old tone bursts' ramp, 55 % to full), with the speaker raised to at least
 * ALARM_VOL while it rings. When the stream can't be had, the old hal_tone bursts ring instead. */

#define SR 24000
#define PCM_MAX_S 1.8f

typedef struct {
    float hz, at, tau, amp;
} note_t;

static float *s_mix;       /* PCM_MAX_S of float mix, PSRAM */
static int16_t *s_pcm;     /* the same as 16-bit, PSRAM */

static int synth(const note_t *n, int nn, float len_s, float gain, float bright)
{
    int cap = (int)(PCM_MAX_S * SR);
    int len = (int)(len_s * SR);
    if (len > cap) len = cap;
    if (!s_mix) s_mix = calloc((size_t)cap, sizeof *s_mix);
    if (!s_pcm) s_pcm = calloc((size_t)cap, sizeof *s_pcm);
    if (!s_mix || !s_pcm) return 0;
    memset(s_mix, 0, (size_t)len * sizeof *s_mix);
    static const float MUL[4] = { 1.0f, 2.0f, 3.0f, 4.2f };
    const float AMP[4] = { 1.0f, 0.38f * bright, 0.16f * bright, 0.05f * bright };
    const int attack = SR * 6 / 1000;
    for (int i = 0; i < nn; i++) {
        int start = (int)(n[i].at * SR);
        for (int p = 0; p < 4; p++) {
            float hz = n[i].hz * MUL[p];
            if (hz > SR * 0.45f) continue;
            float w = 2 * (float)M_PI * hz / SR, cw = cosf(w), sw = sinf(w);
            float x = 1, y = 0;                                  /* cos and sin of the phase */
            float tau = n[i].tau / (1 + 0.7f * p);               /* the upper partials ring shorter */
            float d = expf(-1.0f / (tau * SR)), e = n[i].amp * AMP[p];
            int end = start + (int)(6 * tau * SR);
            if (end > len) end = len;
            for (int k = start; k < end; k++) {
                int t = k - start;
                float a = t < attack ? 0.5f - 0.5f * cosf((float)M_PI * t / attack) : 1;
                s_mix[k] += y * e * a;
                e *= d;
                float nx = x * cw - y * sw;
                y = x * sw + y * cw;
                x = nx;
                if ((t & 1023) == 1023) { /* keep the rotation on the unit circle */
                    float r = 1.5f - 0.5f * (x * x + y * y);
                    x *= r;
                    y *= r;
                }
            }
        }
    }
    /* a soft limiter: loud without the square edges of clipping; a 4 ms fade at the very end */
    int fade = SR / 250;
    for (int k = 0; k < len; k++) {
        float v = s_mix[k] * gain;
        v = v / sqrtf(1 + v * v);
        if (k > len - fade) v *= (float)(len - k) / fade;
        s_pcm[k] = (int16_t)(v * 30000);
    }
    return len;
}

/* The stream for a burst. A stream already going gives way (the music, the assistant's speech, or our own last
 * burst still ringing out); false: no stream, ring with tones. */
static bool play(int len)
{
    if (len <= 0) return false;
    if (hal_play_busy()) hal_play_stop();
    if (!hal_play_start(SR)) return false;
    MA.playing = true;
    hal_play_write(s_pcm, len, 0); /* the queue holds seconds: this never waits */
    hal_play_end();
    return true;
}

/* A schedule change: soft, once, at the tablet's own volume (a little at least). */
static void chime(void)
{
    if (!MA.sound) return;
    float v = S.volume > 0.3f ? S.volume : 0.3f;
    static const note_t N[2] = { { 784.0f, 0, 0.45f, 1 }, { 1174.7f, 0.16f, 0.8f, 0.9f } };
    /* a stream that's talking isn't cut for a chime: tones mix over it */
    if (!hal_play_busy() && play(synth(N, 2, 1.4f, 0.9f * v, 0.6f))) return;
    hal_tone(988, 70, v);
    hal_tone(1319, 110, v);
}

/* ------------------------------------------------------------------ the alarm screen */

static void ma_dismiss(lv_obj_t *o, void *u);
static void ma_checklist(lv_obj_t *o, void *u);

static lv_obj_t *big_button(lv_obj_t *parent, const char *icon, const char *text, bz_color_role_t fill,
                            bz_color_role_t ink, bz_tap_fn fn)
{
    lv_obj_t *b = bz_tile(parent, 520, 124);
    bz_tile_set_fill(b, fill);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 16, 0);
    bz_icon(b, icon, 40, ink);
    bz_label(b, text, BZ_F_TITLE, ink);
    bz_on_tap(b, fn, NULL);
    return b;
}

static void screen_build(void)
{
    lv_obj_t *r = lv_obj_create(bz_ui_glass());
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, W, H);
    lv_obj_add_style(r, bz_style_fill(BZ_C_GROUND), 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); /* nothing under it takes a tap */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    MA.root = r;
    /* the alliance's colour: a bar down the left edge, the ground tinted with it (set per alarm) */
    MA.bar = bz_box(r);
    lv_obj_set_size(MA.bar, 24, H);
    lv_obj_set_pos(MA.bar, 0, 0);
    lv_obj_set_style_bg_opa(MA.bar, LV_OPA_COVER, 0);
    int x = 80, tw = W - x - 60;
    MA.kind_l = bz_label_line(r, "", BZ_F_NAME, BZ_C_SIGNAL, tw);
    lv_obj_set_pos(MA.kind_l, x, 64);
    MA.title = bz_label_line(r, "", BZ_F_DISPLAY, BZ_C_INK, tw);
    lv_obj_set_pos(MA.title, x, 104);
    MA.with_l = bz_label_line(r, "", BZ_F_TITLE, BZ_C_INK, tw);
    lv_obj_set_pos(MA.with_l, x, 236);
    MA.vs_l = bz_label_line(r, "", BZ_F_BODY, BZ_C_DIM, tw);
    lv_obj_set_pos(MA.vs_l, x, 300);
    MA.batt_l = bz_label_line(r, "", BZ_F_NAME, BZ_C_ICE, tw);
    lv_obj_set_pos(MA.batt_l, x, 352);
    lv_obj_t *row = bz_row(r, 40);
    lv_obj_set_size(row, W, 124);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -70);
    MA.check_btn = big_button(row, BZ_I_CHECKLIST, "open checklist", BZ_C_SIGNAL, BZ_C_ON_SIGNAL, ma_checklist);
    MA.dismiss_btn = big_button(row, BZ_I_CLOSE, "dismiss", BZ_C_SURFACE3, BZ_C_INK, ma_dismiss);
}

/* the countdown, once a minute */
static void screen_text(time_t now)
{
    long mins = MA.al.when > now ? (long)((MA.al.when - now + 59) / 60) : 0;
    if (mins == MA.shown_min) return;
    MA.shown_min = mins;
    if (mins > 0) ui_text(MA.title, "%s in %ld min", MA.al.m.label, mins);
    else ui_text(MA.title, "%s now", MA.al.m.label);
}

static void screen_show(void)
{
    if (!MA.root) screen_build();
    const tba_match_t *m = &MA.al.m;
    ui_text(MA.kind_l, "%s", MA.kind == K_QUEUE ? "time to queue: run the checklist" : "match reminder");
    char with[64], vs[64], at[16];
    partners(m, S.team, with, sizeof with, vs, sizeof vs);
    hhmm(MA.al.when, at, sizeof at);
    ui_text(MA.with_l, "%s", with);
    ui_text(MA.vs_l, "%s \xc2\xb7 %s %s", vs, m->predicted ? "predicted" : "scheduled", at);
    /* the battery for it: the one picked, else the one recommended (ui_batt.c) */
    char bl[160];
    if (ui_batt_alarm_line(m->label, bl, sizeof bl)) ui_text(MA.batt_l, "%s", bl);
    else ui_text(MA.batt_l, " ");
    /* The alliance the team plays on, unmistakably: FRC's own red and blue down the edge, on the line that
     * names the partners and on the checklist button, and the whole screen tinted with it. An alarm across
     * the pit is read by its colour before its words. (Neither alliance known: the plain ground.) */
    bool red = m->ours == 1, blue = m->ours == 2;
    lv_color_t hue = red ? lv_color_hex(0xED1C24) : blue ? lv_color_hex(0x2F6BFF) : bz_lv(BZ_C_SIGNAL);
    lv_color_t tint = red ? lv_color_hex(0x2A0B0D) : blue ? lv_color_hex(0x0B1430) : bz_lv(BZ_C_GROUND);
    lv_obj_set_style_bg_color(MA.root, tint, 0);
    lv_obj_set_style_bg_color(MA.bar, hue, 0);
    lv_obj_set_style_bg_opa(MA.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(MA.with_l, hue, 0);
    lv_obj_set_style_text_color(MA.kind_l, (red || blue) ? lv_color_white() : bz_lv(BZ_C_SIGNAL), 0);
    lv_obj_set_style_bg_color(MA.check_btn, hue, 0);
    /* its icon and words white on the alliance's colour, dark on the plain signal colour */
    lv_color_t on = (red || blue) ? lv_color_white() : bz_lv(BZ_C_ON_SIGNAL);
    for (uint32_t i = 0; i < lv_obj_get_child_count(MA.check_btn); i++)
        lv_obj_set_style_text_color(lv_obj_get_child(MA.check_btn, (int32_t)i), on, 0);
    if (MA.kind == K_MATCH) lv_obj_add_flag(MA.check_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(MA.check_btn, LV_OBJ_FLAG_HIDDEN);
    MA.shown_min = -1;
    screen_text(time(NULL));
    lv_obj_move_foreground(MA.root);
    lv_obj_remove_flag(MA.root, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ ringing */

static void sound_stop(void)
{
    MA.ringing = false;
    if (MA.playing) {
        MA.playing = false;
        if (hal_play_busy()) hal_play_stop();
    }
    if (MA.vol_raised) {
        MA.vol_raised = false;
        hal_set_volume(S.volume);
    }
}

static void ring(tracked_t *t, int kind)
{
    MA.al = *t;
    MA.kind = kind;
    screen_show();
    MA.up_now = true;
    double now = hal_seconds();
    MA.t0 = MA.next_burst = now;
    MA.bursts = 0;
    MA.ringing = MA.sound;
    /* the tablet's volume, but never too quiet to hear across a pit */
    if (MA.ringing && S.volume < ALARM_VOL) {
        hal_set_volume(ALARM_VOL);
        MA.vol_raised = true;
    }
    bz_ui_wake();           /* on, and lit, if it was asleep */
    bz_ui_swallow_cancel(); /* the first tap presses the alarm's buttons */
    char with[64], vs[64], msg[96];
    partners(&t->m, S.team, with, sizeof with, vs, sizeof vs);
    long mins = t->when > time(NULL) ? (long)((t->when - time(NULL) + 59) / 60) : 0;
    snprintf(msg, sizeof msg, "%s in %ld min%s \xc2\xb7 %s", t->m.label, mins, kind == K_QUEUE ? " \xc2\xb7 queue now" : "",
             with);
    ui_island_say(BZ_I_STADIUM, msg);
}

static void ma_close(void)
{
    sound_stop();
    MA.up_now = false;
    if (MA.root) lv_obj_add_flag(MA.root, LV_OBJ_FLAG_HIDDEN);
}

static void ma_dismiss(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ma_close();
}

static void ma_checklist(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ma_close();
    ui_lock_lift();
    ui_app_open(&APP_CHECK, NULL);
}

/* A burst every RING_EVERY_S, escalating in three steps (the first 5 bursts, the next 10, then the rest: fuller,
 * faster, brighter) and louder over the first minute. */
static int burst_pcm(int kind, int level, float v)
{
    note_t n[12];
    int k = 0;
    float len;
    if (kind == K_QUEUE) {
        static const float ARP[4] = { 659.3f, 830.6f, 987.8f, 1318.5f };
        float step = level < 2 ? 0.12f : 0.09f;
        for (int i = 0; i < 4; i++) n[k++] = (note_t){ ARP[i], i * step, i == 3 ? 0.6f : 0.4f, i == 3 ? 1.0f : 0.85f };
        len = 1.4f;
        if (level == 1) {
            n[k++] = (note_t){ 1318.5f, 0.62f, 0.22f, 0.8f };
            n[k++] = (note_t){ 1318.5f, 0.78f, 0.45f, 0.9f };
            len = 1.5f;
        } else if (level == 2) {
            static const float UP[4] = { 987.8f, 1244.5f, 1480.0f, 1975.5f }; /* B major, a fifth up */
            for (int i = 0; i < 4; i++) n[k++] = (note_t){ UP[i], 0.46f + i * step, i == 3 ? 0.6f : 0.3f, 0.9f };
            len = 1.6f;
        }
    } else {
        int notes = level == 0 ? 4 : level == 1 ? 6 : 8;
        float step = level == 0 ? 0.16f : level == 1 ? 0.13f : 0.11f;
        for (int i = 0; i < notes; i++) {
            bool hi = i & 1, late = level == 2 && i >= 4;
            float hz = late ? (hi ? 1760.0f : 1174.7f) : (hi ? 1318.5f : 880.0f);
            n[k++] = (note_t){ hz, i * step, i == notes - 1 ? 0.35f : 0.16f, hi ? 0.9f : 1.0f };
        }
        len = notes * step + 0.5f;
    }
    /* gains measured offline (the same synth in numpy): single notes stay under the limiter's knee, the
     * overlaps are what it rounds off; the match reminder sits ~20 % hotter than the queue one */
    return synth(n, k, len, (kind == K_QUEUE ? 0.62f : 0.8f) * v, level == 2 ? 1.3f : 1.0f);
}

static void burst(double el)
{
    float v = 0.55f + (float)(el / 60.0) * 0.45f;
    if (v > 1) v = 1;
    int level = MA.bursts < 5 ? 0 : MA.bursts < 15 ? 1 : 2;
    if (!play(burst_pcm(MA.kind, level, v))) {
        /* no stream: tone bursts. Gaps are silent tones, so a burst is queued whole (the queue holds eight) */
        int notes = level + 2;
        static const float HZ[4] = { 880, 1175, 1480, 1760 };
        int ms = notes < 4 ? 150 : 110;
        for (int i = 0; i < notes; i++) {
            if (i) hal_tone(0, 70, 0.02f);
            hal_tone(HZ[i], ms, v);
        }
    }
    MA.bursts++;
}

static void ring_tick(double now, time_t wall)
{
    if (MA.up_now) {
        /* over whatever went up since (the lock screen as the screen slept, the control center) */
        lv_obj_t *p = lv_obj_get_parent(MA.root);
        if (lv_obj_get_index(MA.root) != (int32_t)lv_obj_get_child_count(p) - 1) lv_obj_move_foreground(MA.root);
        screen_text(wall);
        if (MA.al.when && wall > MA.al.when + STALE_S) ma_close();
    }
    if (!MA.ringing) return;
    bz_ui_wake(); /* lit while it rings */
    double el = now - MA.t0;
    if (el > RING_S) {
        sound_stop();
        char msg[64];
        snprintf(msg, sizeof msg, "the %s alarm rang for %d min", MA.al.m.label, RING_S / 60);
        ui_notify_add(BZ_I_STADIUM, msg);
        return;
    }
    if (now >= MA.next_burst) {
        burst(el);
        MA.next_burst = now + RING_EVERY_S;
    }
}

bool ui_alarm_up(void) { return MA.up_now; }

/* ------------------------------------------------------------------ the schedule */

static tracked_t *find(const tba_match_t *m)
{
    for (int i = 0; i < MA.ntr; i++) {
        const tba_match_t *k = &MA.tr[i].m;
        if (k->level == m->level && k->set == m->set && k->number == m->number) return &MA.tr[i];
    }
    return NULL;
}

static void news(const char *msg)
{
    ui_island_say(BZ_I_SCHEDULE, msg);
    chime();
}

static void ma_update(void)
{
    int n = 0;
    char ev[20];
    MA.phase = tba_upcoming(MA.up, MA_TRACK, &n, &MA.live, &MA.offline, ev, sizeof ev);
    if (MA.phase != TBA_READY) return; /* keep what is tracked: a key or team change clears it below */
    if (strcmp(ev, MA.event)) {
        snprintf(MA.event, sizeof MA.event, "%s", ev);
        MA.ntr = 0;
        MA.have_base = false;
    }
    /* News only against a schedule already known (not the first one seen, nor a whole schedule appearing), and
     * one line per update: the whole event running late moves every match of ours at once. */
    bool news_ok = MA.have_base && MA.ntr > 0 && MA.on;
    int nnew = 0, nmoved = 0;
    char first_new[96] = "", first_moved[96] = "";
    for (int i = 0; i < MA.ntr; i++) MA.tr[i].seen = false;
    for (int i = 0; i < n; i++) {
        const tba_match_t *m = &MA.up[i];
        time_t w = when_of(m);
        tracked_t *t = find(m);
        char at[16], with[64], vs[64];
        if (!t) {
            if (MA.ntr >= MA_TRACK) continue;
            t = &MA.tr[MA.ntr++];
            memset(t, 0, sizeof *t);
            t->m = *m;
            t->when = t->told = w;
            t->seen = true;
            if (w && !nnew++) {
                hhmm(w, at, sizeof at);
                partners(m, S.team, with, sizeof with, vs, sizeof vs);
                snprintf(first_new, sizeof first_new, "new match: %s at %s \xc2\xb7 %s", m->label, at, with);
            }
            continue;
        }
        t->m = *m;
        t->seen = true;
        if (w && t->told && labs((long)(w - t->told)) >= MOVE_S) {
            if (!nmoved++) {
                hhmm(w, at, sizeof at);
                snprintf(first_moved, sizeof first_moved, "%s moved to %s (%+ld min)", m->label, at, (long)(w - t->told) / 60);
            }
            t->told = w;
            t->fired = 0; /* its reminders ring again for the new time */
        } else if (!t->told) {
            t->told = w;
        }
        t->when = w;
    }
    if (news_ok && (nnew || nmoved)) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s", nmoved ? first_moved : first_new);
        int more = (nmoved ? nmoved - 1 : 0) + (nmoved ? nnew : nnew - 1);
        if (more > 0) {
            size_t l = strlen(msg);
            snprintf(msg + l, sizeof msg - l, " \xc2\xb7 %d more of ours changed", more);
        }
        news(msg);
    }
    /* played (or gone from the schedule): no longer tracked */
    int k = 0;
    for (int i = 0; i < MA.ntr; i++)
        if (MA.tr[i].seen) MA.tr[k++] = MA.tr[i];
    MA.ntr = k;
    MA.have_base = true;
}

/* once a second: a reminder due rings (the match one wins when both are, so a late start rings once) */
static void ma_check(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    if (!MA.on || tm.tm_year < 120) return;
    for (int i = 0; i < MA.ntr; i++) {
        tracked_t *t = &MA.tr[i];
        if (!t->when || now >= t->when) continue;
        bool q = MA.queue_min && !(t->fired & 1) && now >= t->when - MA.queue_min * 60;
        bool m = MA.match_min && !(t->fired & 2) && now >= t->when - MA.match_min * 60;
        if (m) {
            t->fired |= 3;
            ring(t, K_MATCH);
        } else if (q) {
            t->fired |= 1;
            ring(t, K_QUEUE);
        }
    }
}

static double poll_every(void)
{
    if (MA.phase != TBA_READY || MA.offline) return POLL_RETRY_S;
    return MA.live && MA.ntr > 0 ? POLL_LIVE_S : POLL_IDLE_S;
}

static void ma_tick(void *u)
{
    (void)u;
    double now = hal_seconds();
    time_t wall = time(NULL);
    if (MA.up_now || MA.ringing) ring_tick(now, wall);
    if (MA.test_at && now >= MA.test_at) {
        /* a made-up match, 25 minutes out (5 for the match reminder), so a reminder's screen and sound can be
         * seen; or a made-up schedule change, its message and chime */
        MA.test_at = 0;
        tracked_t t;
        memset(&t, 0, sizeof t);
        snprintf(t.m.label, sizeof t.m.label, "Q34");
        t.m.ours = 1;
        t.m.red[0] = S.team > 0 ? S.team : 5805, t.m.red[1] = 1234, t.m.red[2] = 5678;
        t.m.blue[0] = 111, t.m.blue[1] = 222, t.m.blue[2] = 333;
        t.when = t.m.predicted = wall + (MA.test_kind == 1 ? 5 : 25) * 60;
        if (MA.test_kind == 2) {
            char at[16], msg[64];
            hhmm(t.when, at, sizeof at);
            snprintf(msg, sizeof msg, "test: Q34 moved to %s (+8 min)", at);
            news(msg);
        } else if (MA.test_kind == 1) {
            ring(&t, K_MATCH);
            ui_text(MA.kind_l, "test alarm \xc2\xb7 match reminder");
        } else {
            ring(&t, K_QUEUE); /* the queue reminder's screen, checklist button and all */
            ui_text(MA.kind_l, "test alarm \xc2\xb7 time to queue: run the checklist");
        }
    }
    if (S.team != MA.team) {
        ma_config();
        MA.ntr = 0;
        MA.event[0] = 0;
        MA.next_poll = now + 1;
    }
    unsigned g = tba_gen();
    if (g != MA.tba_gen) {
        MA.tba_gen = g;
        ma_update();
    }
    if (MA.on && MA.configured && now >= MA.next_poll) {
        tba_want(); /* one poll: the worker ends itself again ~15 s after */
        MA.next_poll = now + poll_every();
    }
    if (now >= MA.next_check) {
        MA.next_check = now + 1;
        ma_check(wall);
    }
}

void ui_match_boot(void)
{
    if (MA.booted) return;
    MA.booted = true;
    MA.shown_min = -1;
    ma_load();
    ma_config();
    MA.next_poll = hal_seconds() + FIRST_POLL_S;
    ui_on_refresh(ma_tick, NULL);
}

void ui_match_test_kind(double delay_s, int kind)
{
    MA.test_kind = kind < 0 || kind > 2 ? 0 : kind;
    MA.test_at = hal_seconds() + (delay_s > 0 ? delay_s : 0.01);
}

void ui_match_test(double delay_s) { ui_match_test_kind(delay_s, 0); }

bool ui_match_next_line(char *out, size_t n)
{
    time_t now = time(NULL);
    const tracked_t *best = NULL;
    for (int i = 0; i < MA.ntr; i++) {
        const tracked_t *t = &MA.tr[i];
        if (t->when && t->when + STALE_S > now && (!best || t->when < best->when)) best = t;
    }
    if (!best) return false;
    char at[16], with[64], vs[64];
    hhmm(best->when, at, sizeof at);
    partners(&best->m, S.team, with, sizeof with, vs, sizeof vs);
    snprintf(out, n, "next: %s \xc2\xb7 %s \xc2\xb7 %s", best->m.label, at, with);
    return true;
}

/* ------------------------------------------------------------------ settings (the tba app's alerts view) */

static void set_chips(void)
{
    if (!MA.on_chip) return;
    ui_chip_set(MA.on_chip, MA.on);
    ui_chip_set(MA.sound_chip, MA.sound);
    for (int i = 0; i < 6; i++) ui_chip_set(MA.q_chips[i], MA.queue_min == Q_MIN[i]);
    for (int i = 0; i < 4; i++) ui_chip_set(MA.m_chips[i], MA.match_min == M_MIN[i]);
}

static void st_toggle(lv_obj_t *o, void *u)
{
    (void)o;
    if (u) MA.sound = !MA.sound;
    else {
        MA.on = !MA.on;
        if (MA.on) MA.next_poll = 0;
    }
    ma_save();
    set_chips();
}

static void st_queue(lv_obj_t *o, void *u)
{
    (void)o;
    MA.queue_min = Q_MIN[(int)(intptr_t)u];
    /* a new lead: reminders that haven't rung yet use it; ones already due ring now */
    for (int i = 0; i < MA.ntr; i++) MA.tr[i].fired &= (uint8_t)~1;
    ma_save();
    set_chips();
}

static void st_match(lv_obj_t *o, void *u)
{
    (void)o;
    MA.match_min = M_MIN[(int)(intptr_t)u];
    ma_save();
    set_chips();
}

static void st_test(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_match_test(5);
    ui_island_say(BZ_I_STADIUM, "a test alarm in 5 s");
}

static lv_obj_t *wrap_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

void ui_match_settings(lv_obj_t *pane, int w)
{
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 12, 0);
    bz_label(pane, "match alerts", BZ_F_NAME, BZ_C_INK);
    lv_obj_t *c = bz_label(pane, "An alarm before each of the team's matches, from the blue alliance's schedule (its "
                                 "predicted times when it has them), over whatever is on screen. Schedule changes "
                                 "come as notifications.",
                           BZ_F_CAPTION, BZ_C_DIM);
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c, w);
    lv_obj_t *r = wrap_row(pane, w);
    MA.on_chip = ui_chip(r, "alerts on", st_toggle, NULL);
    MA.sound_chip = ui_chip(r, "sound", st_toggle, (void *)1);
    ui_button(r, BZ_I_STADIUM, "test alarm", st_test, NULL);
    bz_label(pane, "queue reminder: time for the checklist", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(pane, w);
    for (int i = 0; i < 6; i++) {
        char b[16];
        if (Q_MIN[i]) snprintf(b, sizeof b, "%d min", Q_MIN[i]);
        else snprintf(b, sizeof b, "off");
        MA.q_chips[i] = ui_chip(r, b, st_queue, (void *)(intptr_t)i);
    }
    bz_label(pane, "match reminder", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(pane, w);
    for (int i = 0; i < 4; i++) {
        char b[16];
        if (M_MIN[i]) snprintf(b, sizeof b, "%d min", M_MIN[i]);
        else snprintf(b, sizeof b, "off");
        MA.m_chips[i] = ui_chip(r, b, st_match, (void *)(intptr_t)i);
    }
    MA.next_l = bz_label(pane, "", BZ_F_BODY_S, BZ_C_INK);
    lv_label_set_long_mode(MA.next_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(MA.next_l, w);
    set_chips();
}

void ui_match_settings_refresh(void)
{
    if (!MA.next_l) return;
    char b[160];
    time_t now = time(NULL);
    const tracked_t *best = NULL;
    time_t best_at = 0;
    int best_kind = 0;
    for (int i = 0; MA.on && i < MA.ntr; i++) {
        const tracked_t *t = &MA.tr[i];
        if (!t->when || now >= t->when) continue;
        for (int k = 0; k < 2; k++) {
            int lead = k ? MA.match_min : MA.queue_min;
            if (!lead || (t->fired >> k & 1)) continue;
            time_t at = t->when - lead * 60;
            if (!best || at < best_at) best = t, best_at = at, best_kind = k;
        }
    }
    if (!MA.configured) snprintf(b, sizeof b, "needs a blue alliance key (TBA_API_KEY) and the team number");
    else if (!MA.on) snprintf(b, sizeof b, "alerts are off");
    else if (best) {
        char at[16], m[16];
        hhmm(best_at > now ? best_at : now, at, sizeof at);
        hhmm(best->when, m, sizeof m);
        snprintf(b, sizeof b, "next alarm: %s %s reminder at %s (the match at %s)", best->m.label,
                 best_kind ? "match" : "queue", at, m);
    } else if (MA.ntr) snprintf(b, sizeof b, "every reminder for the matches ahead has rung");
    else snprintf(b, sizeof b, "no matches to come at the current event");
    ui_text(MA.next_l, "%s", b);
}
