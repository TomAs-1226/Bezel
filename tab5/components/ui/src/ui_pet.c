/* ui_pet — see ui_pet.h. */
#include "ui_pet.h"

#include "assist.h"
#include "home_pet.h"
#include "ui_avatar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AV_PX 420 /* the avatar's box: one size on every screen, so one baked body serves them all */

/* ================================================================== what the pet feels, as the avatar shows it */

static av_state_t base_state(const home_pet_t *p, bool idle_face)
{
    switch (home_pet_feel(p)) {
    case PET_FEEL_ASLEEP: return AV_S_SLEEP;
    case PET_FEEL_HUNGRY: return AV_S_HUNGRY;
    case PET_FEEL_SLEEPY: return AV_S_DROWSY;
    case PET_FEEL_SAD: return AV_S_SAD;
    default: return idle_face ? AV_S_QUIET : AV_S_IDLE;
    }
}

static bool point(lv_point_t *p)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return false;
    lv_indev_get_point(in, p);
    return true;
}

/* ================================================================== the pet page */

enum { M_FED, M_ENERGY, M_MOOD, M_BOND, M_N };
static const char *const M_LABEL[M_N] = { "fed", "energy", "mood", "bond" };

static struct {
    lv_obj_t *page, *touch, *name, *line, *meter[M_N], *mlabel[M_N], *feed, *play, *sleep, *foods, *food[PET_FOODS],
        *note, *hint;
    ui_avatar_t *av;
    bool built, near, current;
    unsigned ev_seen;
    double foods_until, note_until;
    int pct[M_N];
    int shown_asleep;
    av_state_t state;
    pet_feel_t feel;
    bool have_feel;
    /* the finger on the avatar */
    bool pressed;
    int px, py, moved, total;
    double stroke_at;
} PG;

static void page_note(const char *text)
{
    ui_text(PG.note, "%s", text);
    hm_show(PG.note, true);
    PG.note_until = hal_seconds() + 3.5;
}

static void avatar_sync(const home_pet_t *p)
{
    if (!PG.av) return;
    av_state_t st = base_state(p, false);
    pet_feel_t f = home_pet_feel(p);
    if (PG.have_feel && PG.feel == PET_FEEL_ASLEEP && f != PET_FEEL_ASLEEP) ui_avatar_react(PG.av, AV_S_WAKE);
    PG.feel = f;
    PG.have_feel = true;
    if (st != PG.state || !PG.near) {
        PG.state = st;
        ui_avatar_state(PG.av, st);
    }
    ui_avatar_calm(PG.av, f == PET_FEEL_ASLEEP ? 0.6f : 0.0f);
}

static void feed_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    bool on = PG.foods_until < hal_seconds();
    hm_show(PG.foods, on);
    PG.foods_until = on ? hal_seconds() + 8.0 : 0;
}

static void food_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int food = (int)(intptr_t)u;
    char say[96];
    if (home_pet_do_feed(food, say, sizeof say)) {
        ui_avatar_react(PG.av, AV_S_EAT);
        hm_show(PG.foods, false);
        PG.foods_until = 0;
    }
    page_note(say);
}

static void play_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    char say[96];
    if (home_pet_do_play(say, sizeof say)) ui_avatar_react(PG.av, AV_S_HAPPY);
    page_note(say);
}

static void sleep_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    home_pet_t p;
    home_pet_get(&p);
    char say[96];
    bool ok = home_pet_do_sleep(!p.asleep, say, sizeof say);
    if (ok && p.asleep) ui_avatar_react(PG.av, AV_S_WAKE);
    page_note(say);
}

/* A finger on the avatar: it looks at it, and a stroke (some distance moved) is petting. The overlay does not pass
 * the drag on, so stroking sideways never turns the page; the rest of the screen still does. */
static void touch_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    if (!point(&p)) return;
    lv_area_t a;
    lv_obj_get_coords(PG.touch, &a);
    float nx = ((float)(p.x - (a.x1 + a.x2) / 2)) / ((a.x2 - a.x1) / 2.0f);
    float ny = ((float)(p.y - (a.y1 + a.y2) / 2)) / ((a.y2 - a.y1) / 2.0f);
    double now = hal_seconds();
    if (c == LV_EVENT_PRESSED) {
        PG.pressed = true;
        PG.px = p.x;
        PG.py = p.y;
        PG.moved = PG.total = 0;
        ui_avatar_look(PG.av, nx, ny, 1.0f);
    } else if (c == LV_EVENT_PRESSING && PG.pressed) {
        int d = abs(p.x - PG.px) + abs(p.y - PG.py);
        PG.px = p.x;
        PG.py = p.y;
        PG.moved += d;
        PG.total += d;
        ui_avatar_look(PG.av, nx, ny, 0.8f);
        if (PG.moved > 70 && now - PG.stroke_at > 1.5) {
            PG.moved = 0;
            PG.stroke_at = now;
            home_pet_do_stroke();
            if (!ui_avatar_reacting(PG.av)) ui_avatar_react(PG.av, AV_S_PETTED);
        }
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (PG.pressed && PG.total < 14 && !ui_avatar_reacting(PG.av)) ui_avatar_react(PG.av, AV_S_SURPRISED); /* a poke */
        PG.pressed = false;
    }
}

lv_obj_t *hm_pet_build(lv_obj_t *parent)
{
    lv_obj_t *pg = bz_box(parent);
    lv_obj_set_size(pg, W, H);
    lv_obj_add_style(pg, bz_style_fill(BZ_C_GROUND), 0);
    PG.page = pg;
    PG.av = ui_avatar_create(pg, AV_PX);
    lv_obj_set_pos(ui_avatar_obj(PG.av), 50, 150);
    PG.touch = bz_box(pg);
    lv_obj_set_pos(PG.touch, 50 + 40, 150 + 50);
    lv_obj_set_size(PG.touch, AV_PX - 80, AV_PX - 100);
    lv_obj_add_flag(PG.touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(PG.touch, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(PG.touch, touch_ev, LV_EVENT_ALL, NULL);

    const int X = 600, WCOL = W - X - 70;
    PG.name = bz_label_line(pg, "", BZ_F_TITLE, BZ_C_INK, WCOL);
    lv_obj_set_pos(PG.name, X, 120);
    PG.line = bz_label_line(pg, "", BZ_F_BODY, BZ_C_DIM, WCOL);
    lv_obj_set_pos(PG.line, X, 170);
    for (int i = 0; i < M_N; i++) {
        int y = 250 + i * 44;
        PG.mlabel[i] = bz_label(pg, M_LABEL[i], BZ_F_LABEL, BZ_C_DIM);
        lv_obj_set_pos(PG.mlabel[i], X, y - 2);
        PG.meter[i] = bz_meter(pg, WCOL - 130, 14);
        lv_obj_set_pos(PG.meter[i], X + 130, y + 3);
        PG.pct[i] = -1;
    }
    PG.feed = ui_button(pg, BZ_I_BATTERY_FULL, "feed", feed_tap, NULL);
    PG.play = ui_button(pg, BZ_I_SPORTS_ESPORTS, "play", play_tap, NULL);
    PG.sleep = ui_button(pg, BZ_I_BEDTIME, "sleep", sleep_tap, NULL);
    lv_obj_set_pos(PG.feed, X, 450);
    lv_obj_set_pos(PG.play, X + 168, 450);
    lv_obj_set_pos(PG.sleep, X + 336, 450);
    PG.foods = bz_row(pg, 10);
    lv_obj_set_pos(PG.foods, X, 520);
    lv_obj_set_size(PG.foods, WCOL, 56);
    for (int i = 0; i < PET_FOODS; i++) PG.food[i] = ui_chip(PG.foods, PET_FOOD_NAME[i], food_tap, (void *)(intptr_t)i);
    hm_show(PG.foods, false);
    PG.note = bz_label_line(pg, "", BZ_F_BODY_S, BZ_C_INK, WCOL);
    lv_obj_set_pos(PG.note, X, 596);
    hm_show(PG.note, false);
    PG.hint = bz_label_line(pg, "stroke the pet with a finger", BZ_F_CAPTION, BZ_C_FAINT, WCOL);
    lv_obj_set_pos(PG.hint, X, 628);
    PG.state = AV_S_COUNT;
    PG.built = true;
    return pg;
}

void hm_pet_near(bool near)
{
    if (!PG.built || near == PG.near) return;
    PG.near = near;
    ui_avatar_active(PG.av, near);
    PG.state = AV_S_COUNT;
    if (near) {
        home_pet_t p;
        home_pet_get(&p);
        PG.have_feel = false;
        avatar_sync(&p);
    }
}

void hm_pet_refresh(bool shown)
{
    if (!PG.built) return;
    PG.current = shown;
    double now = hal_seconds();
    if (PG.foods_until && now > PG.foods_until) {
        PG.foods_until = 0;
        hm_show(PG.foods, false);
    }
    if (PG.note_until && now > PG.note_until) {
        PG.note_until = 0;
        hm_show(PG.note, false);
    }
    if (!PG.near) return;
    home_pet_t p;
    home_pet_get(&p);
    char w[48];
    home_pet_words(&p, w, sizeof w);
    ui_text(PG.name, "%s", p.name);
    int days = home_pet_age_days(&p, time(NULL));
    ui_text(PG.line, "%s  \xC2\xB7  %d day%s old", w, days, days == 1 ? "" : "s");
    float v[M_N] = { 1 - p.hunger, p.energy, p.mood, p.affection };
    for (int i = 0; i < M_N; i++) {
        int pct = (int)(v[i] * 100 + 0.5f);
        if (pct == PG.pct[i]) continue;
        PG.pct[i] = pct;
        /* low is amber, never an alarm */
        bz_meter_set(PG.meter[i], v[i], (i == M_FED || i == M_ENERGY) && v[i] < 0.35f ? BZ_C_AMBER : BZ_C_ICE);
    }
    if (PG.shown_asleep != (int)p.asleep + 1) {
        PG.shown_asleep = (int)p.asleep + 1;
        ui_text(lv_obj_get_child(PG.sleep, 1), "%s", p.asleep ? "wake" : "sleep");
        bz_icon_set(lv_obj_get_child(PG.sleep, 0), p.asleep ? BZ_I_LIGHT_MODE : BZ_I_BEDTIME, 24, false);
    }
    avatar_sync(&p);
    /* what the AI (or anything else) did to it: the screen reacts */
    int ev = home_pet_event(&PG.ev_seen);
    if (ev == PET_EV_FED) ui_avatar_react(PG.av, AV_S_EAT);
    else if (ev == PET_EV_PLAYED) ui_avatar_react(PG.av, AV_S_HAPPY);
}

void hm_pet_frame(bool current)
{
    if (!PG.built || !PG.near) return;
    PG.current = current;
    /* the page beside it keeps its body baked and nothing else: only the page on screen moves */
    if (current && ui_avatar_frame(PG.av)) bz_ui_keep_alive();
}

/* ================================================================== the idle face */

#define IDLE_QUIET_S 120.0   /* untouched this long: it begins to settle */
#define IDLE_DEEP_S 720.0    /* and this long: as still and dim as it gets */

static struct {
    lv_obj_t *box, *clock;
    ui_avatar_t *av;
    bool active;
    unsigned ev_seen;
    av_state_t state;
    double react_at;
    float dim;
    int clock_min;
    uint32_t rng;
} IF;

static void idle_build(lv_obj_t *b)
{
    IF.box = b;
    IF.av = ui_avatar_create(b, AV_PX);
    lv_obj_set_pos(ui_avatar_obj(IF.av), (W - AV_PX) / 2, 150);
    IF.clock = bz_label(b, "", BZ_F_NAME, BZ_C_FAINT);
    IF.state = AV_S_COUNT;
    IF.clock_min = -1;
    IF.rng = (uint32_t)hal_seconds() * 2654435761u | 1u;
}

static void idle_clock(const struct tm *tm)
{
    char t[16];
    hm_hm(tm, t, sizeof t);
    ui_text(IF.clock, "%s", t);
    lv_obj_align(IF.clock, LV_ALIGN_TOP_RIGHT, -PAD - 6, PAD);
}

static void idle_activate(bool on)
{
    if (on == IF.active) return;
    IF.active = on;
    ui_avatar_active(IF.av, on);
    IF.state = AV_S_COUNT;
}

/* 10 Hz: the pet's state, how settled it is, and now and then a small thing to do */
static void idle_tick(void)
{
    if (!IF.box) return;
    /* only while the face is actually on the glass: hidden, it holds nothing */
    bool vis = lv_obj_is_visible(IF.box);
    idle_activate(vis);
    if (!vis) return;
    double now = hal_seconds(), idle = bz_ui_idle_s();
    home_pet_t p;
    home_pet_get(&p);
    float settle = (float)((idle - IDLE_QUIET_S) / (IDLE_DEEP_S - IDLE_QUIET_S));
    settle = settle < 0 ? 0 : settle > 1 ? 1 : settle;
    ui_avatar_calm(IF.av, p.asleep ? 1.0f : settle);
    /* dimmer in steps of a twentieth, and a good deal at night: a lamp left on a desk, not a screen */
    float dim = settle * 0.5f + (p.asleep ? 0.25f : 0);
    dim = floorf(dim * 20.0f) / 20.0f;
    if (dim != IF.dim) {
        IF.dim = dim;
        ui_avatar_dim(IF.av, dim);
    }
    av_state_t st = base_state(&p, true);
    if (st != IF.state) {
        bool woke = IF.state == AV_S_SLEEP && st != AV_S_SLEEP;
        IF.state = st;
        ui_avatar_state(IF.av, st);
        if (woke) ui_avatar_react(IF.av, AV_S_WAKE);
    }
    int ev = home_pet_event(&IF.ev_seen);
    if (ev == PET_EV_FED) ui_avatar_react(IF.av, AV_S_EAT);
    else if (ev == PET_EV_PLAYED) ui_avatar_react(IF.av, AV_S_HAPPY);
    /* now and then, a small thing: a look around, a flicker of fondness; never when it is asleep or settled deep */
    if (now > IF.react_at) {
        IF.rng = IF.rng * 1664525u + 1013904223u;
        float r = (float)(IF.rng >> 8) / 16777216.0f;
        IF.react_at = now + 300 + 420 * r;
        if (!p.asleep && settle < 0.8f && !ui_avatar_reacting(IF.av)) {
            if (p.mood > 0.8f && r < 0.3f) ui_avatar_react(IF.av, AV_S_LOVE);
            else if (r < 0.7f) ui_avatar_react(IF.av, AV_S_CURIOUS);
        }
    }
}

static void idle_minute(const struct tm *tm, bool force)
{
    (void)force;
    idle_clock(tm);
    idle_tick();
}

static void idle_frame(double now)
{
    (void)now;
    if (!IF.active) return;
    if (ui_avatar_frame(IF.av)) bz_ui_keep_alive();
}

static void idle_leave(void) { idle_activate(false); }

const hm_face_t HM_F_PET = { .name = "pet", .build = idle_build, .minute = idle_minute, .tick = idle_tick,
                             .frame = idle_frame, .leave = idle_leave };

/* ================================================================== always */

static void pet_tick(void *user)
{
    (void)user;
    double now = hal_seconds();
    /* the room is quiet when nobody has touched the tablet and the microphones heard nothing */
    double q = bz_ui_idle_s(), pr = home_presence_idle_s(now);
    home_pet_set_quiet(q < pr ? q : pr);
    home_pet_tick(now);
    const char *sd = hal_sd_root();
    char path[96];
    if (sd) snprintf(path, sizeof path, "%s/CATOS/PET/AVATAR.JSON", sd);
    ui_avatar_poll(sd ? path : NULL);
}

static void pet_context(char *out, size_t n) { home_pet_context(out, n); }

void ui_pet_init(void)
{
    ui_on_refresh(pet_tick, NULL);
    assist_set_pet_context(pet_context);
    home_pet_boot();
}

/* ================================================================== the dev console */

bool ui_pet_show_expr(const char *name)
{
    if (!PG.built) return false;
    return ui_avatar_show_expr(PG.av, name);
}

static int state_by_name(const char *s)
{
    for (int i = 0; i < AV_S_COUNT; i++)
        if (!strcmp(s, AV_STATE_NAME[i])) return i;
    return -1;
}

void ui_pet_dev(const char *a)
{
    while (*a == ' ') a++;
    if (!strncmp(a, "show", 4)) {
        ui_home_mode_dev("page 3"); /* home mode, on the pet page */
    } else if (!strncmp(a, "idle", 4)) {
        ui_home_mode_dev("face pet"); /* the idle face, on the clock page */
    } else if (!strncmp(a, "face ", 5)) {
        const char *n = a + 5;
        bool ok = !strcmp(n, "off") ? ui_avatar_show_expr(PG.av, NULL) : ui_avatar_show_expr(PG.av, n);
        printf("pet: face %s %s\n", n, ok ? "shown" : "unknown");
    } else if (!strncmp(a, "state ", 6) || !strncmp(a, "react ", 6)) {
        bool react = a[1] == 'e' && a[2] == 'a';
        int s = state_by_name(a + 6);
        if (s < 0) {
            printf("pet: no state \"%s\"\n", a + 6);
            return;
        }
        if (react) ui_avatar_react(PG.av, (av_state_t)s);
        else {
            PG.state = (av_state_t)s;
            ui_avatar_show_expr(PG.av, NULL);
            ui_avatar_state(PG.av, (av_state_t)s);
        }
        printf("pet: %s %s\n", react ? "react" : "state", AV_STATE_NAME[s]);
    } else if (!strncmp(a, "avatar", 6)) {
        ui_avatar_dump();
    } else {
        home_pet_dev(a);
    }
}
