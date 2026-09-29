/* Home mode's clock faces: the page left of the desk. Designed faces, calm, legible across a room, each true to
 * Bezel (one light from the top left, tonal, restrained), most of them showing something real besides the time:
 *
 *   type     the time as a poster: huge heavy numerals, a signal-orange colon, the day on a Swiss grid
 *   flip     a split-flap clock: four cards whose flaps fall over when a digit changes
 *   dial     a Braun-style dial: a domed plate, ink hands with their shadows, a signal second hand
 *   chrono   a chronograph: small seconds at nine, the tablet's battery at three, the next match at six
 *   sky      the sky as it is now, from the sun's real altitude, and the sun on today's real path
 *   moon     tonight's moon: its real phase, its seas where they are, lit from the side the sun is on
 *   orrery   the planets on their orbits where they really are today
 *   world    four clocks: here, Houston, London and Tokyo, each face light while the sun is up there
 *   tree     the season's tree: it grows with the team's matches (ui_face_tree.c)
 *   match    the next match: when, the alliance, partners and rank, from the blue alliance
 *   words    a word clock
 *   night    (not in the cycle) the time dim and warm on black, from 22:00 to 7:00 once nobody has touched it
 *
 * Each face is its own module (ui_face_*.c, the interface in ui_home_face.h); this file runs them: which shows,
 * their minute, their pictures (painted on the covers' worker and freed when a face goes), the index at the
 * right edge, and the step from one face to the next.
 *
 * A step is a push: a flick up brings the next face up from the bottom edge and the old one leaves by the top
 * (down, the other way), the platform moving both pictures by block copies (bz_ui_push_begin), the new face
 * drawn offscreen a band at a time as it comes into view, and the index, the page dots and the orb held still
 * over both. The new face's pictures are painted before it starts, so the motion never waits on the worker. */
#include "ui_home_face.h"

#include "home_scape.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

const char *const HM_FACE_NAME[HM_NFACES + 1] = { "type", "flip", "dial", "chrono", "sky", "moon",
                                                  "orrery", "world", "tree", "match", "words", "night" };

static const hm_face_t *const FACES[HM_NFACES + 1] = {
    &HM_F_TYPE, &HM_F_FLIP, &HM_F_DIAL, &HM_F_CHRONO, &HM_F_SKY, &HM_F_MOON,
    &HM_F_ORRERY, &HM_F_WORLD, &HM_F_TREE, &HM_F_MATCH, &HM_F_WORDS, &HM_F_NIGHT,
};

int hm_face_by_name(const char *s)
{
    if (!s || !*s) return -1;
    for (int i = 0; i <= HM_NFACES; i++)
        if (!strcmp(s, HM_FACE_NAME[i])) return i;
    /* the first round's faces: the horizon became the sky, the dots the poster */
    if (!strcmp(s, "horizon")) return HM_FACE_SKY;
    if (!strcmp(s, "dots")) return HM_FACE_TYPE;
    return -1;
}

static struct {
    lv_obj_t *pg, *box[HM_NFACES + 1], *index, *index_dot[HM_NFACES];
    int cur;
    int last_min;
    unsigned wx_gen, tba_gen;
    int dark;                 /* the tone the face was coloured for (-1 none yet) */
    bool force;               /* the accent or the options changed: the face again, colours and pictures */
    int index_shown;
    /* a step to another face */
    bool stepping, pushing;
    int step_to, step_dir;
    float step_v;
    double step_t0;
    bz_motion_t k;
    /* faces that went while the worker was painting for them: their memory back once it has finished */
    uint32_t leaving;
} FC = { .cur = -1, .last_min = -1, .dark = -1, .index_shown = -1 };

/* ================================================================ shared pieces */

bool hm_clock_set(const struct tm *tm) { return tm->tm_year > 120; }

void hm_hm(const struct tm *tm, char *out, size_t n)
{
    if (!hm_clock_set(tm)) snprintf(out, n, "--:--");
    else ui_fmt_hm(out, n, tm);
}

void hm_hm_t(time_t t, char *out, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    hm_hm(&tm, out, n);
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

void hm_date(const struct tm *tm, char *out, size_t n, bool shortform)
{
    if (!hm_clock_set(tm)) {
        snprintf(out, n, "%s", "the clock isn't set");
        return;
    }
    strftime(out, n, shortform ? "%a %e %b" : "%A, %B %e", tm);
    lower(out);
    char *dd = strstr(out, "  ");
    if (dd) memmove(dd, dd + 1, strlen(dd));
}

void hm_weather_line(char *out, size_t n)
{
    home_weather_t w;
    home_weather_get(&w);
    if (w.configured && w.ok) snprintf(out, n, "%.0f\xc2\xb0 %s", w.temp, home_weather_text(w.code));
    else out[0] = 0;
}

void hm_tone(hs_tone_t *t)
{
    t->dark = bz_ui_dark();
    t->ground = bz_color(BZ_C_GROUND);
    t->surface1 = bz_color(BZ_C_SURFACE1);
    t->surface2 = bz_color(BZ_C_SURFACE2);
    t->ink = bz_color(BZ_C_INK);
    t->dim = bz_color(BZ_C_DIM);
    t->faint = bz_color(BZ_C_FAINT);
    t->signal = bz_color(BZ_C_SIGNAL);
    t->accent = bz_color(t->dark ? BZ_C_ICE : BZ_C_ON_ICE);
    t->ground565 = hm_565(t->ground);
}

bool hm_where(double *lat, double *lon)
{
    if (home_weather_where(lat, lon)) return true;
    *lat = 33.60; /* Mission Viejo, the weather's default place, until the weather has resolved one */
    *lon = -117.67;
    return false;
}

double hm_tz_s(void)
{
    time_t t = time(NULL);
    struct tm l, g;
    localtime_r(&t, &l);
    gmtime_r(&t, &g);
    int d = (l.tm_yday - g.tm_yday);
    if (d > 1) d = -1;
    if (d < -1) d = 1;
    return (double)d * 86400 + (l.tm_hour - g.tm_hour) * 3600.0 + (l.tm_min - g.tm_min) * 60.0;
}

const tba_state_t *hm_tba(void)
{
    static tba_state_t *copy; /* ~9 KB, once, in PSRAM (the ui component's statics are there too) */
    static unsigned gen;
    unsigned g = tba_gen();
    if (!g) return NULL;
    if (!copy) copy = malloc(sizeof *copy);
    if (!copy) return NULL;
    if (g != gen) {
        tba_get(copy);
        gen = g;
    }
    return copy;
}

void hm_color(lv_obj_t *o, uint32_t rgb)
{
    lv_color_t c = bz_lv_rgb(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) return;
    lv_obj_set_style_text_color(o, c, 0);
}

void hm_pos(lv_obj_t *o, int x, int y)
{
    if (lv_obj_get_x(o) == x && lv_obj_get_y(o) == y) return;
    lv_obj_set_pos(o, x, y);
}

int hm_text_w(const char *s, const lv_font_t *f)
{
    lv_point_t p;
    lv_text_get_size(&p, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

void hm_baseline(lv_obj_t *label, int x, int y)
{
    const lv_font_t *f = lv_obj_get_style_text_font(label, 0);
    hm_pos(label, x, y - (f->line_height - f->base_line));
}

/* ---- pictures ---- */

void hm_pic_init(hm_pic_t *p, lv_obj_t *parent, int x, int y, int w, int h)
{
    memset(p, 0, sizeof *p);
    p->w = w;
    p->h = h;
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p->dsc.header.w = (uint32_t)w;
    p->dsc.header.h = (uint32_t)h;
    p->dsc.header.stride = (uint32_t)w * 2;
    p->dsc.data_size = (uint32_t)(w * h * 2);
    p->img = lv_image_create(parent);
    lv_obj_set_pos(p->img, x, y);
    lv_obj_add_flag(p->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(p->img, LV_OBJ_FLAG_CLICKABLE);
}

bool hm_pic_start(hm_pic_t *p, void (*fn)(void *), void *arg)
{
    if (!p->px) {
        p->px = malloc(sizeof(uint16_t) * (size_t)p->w * (size_t)p->h);
        if (!p->px) {
            printf("hm: no memory for a %dx%d face picture\n", p->w, p->h);
            return false;
        }
        p->dsc.data = (const uint8_t *)p->px;
        p->painted = false;
    }
    p->ticket = hm_art_job(fn, arg);
    return p->ticket != 0;
}

bool hm_pic_busy(const hm_pic_t *p) { return p->ticket && !hm_art_job_done(p->ticket); }

bool hm_pic_poll(hm_pic_t *p)
{
    if (!p->px || !p->ticket || !hm_art_job_done(p->ticket)) return false;
    p->ticket = 0;
    p->painted = true;
    lv_image_cache_drop(&p->dsc);
    if (lv_image_get_src(p->img) != &p->dsc) lv_image_set_src(p->img, &p->dsc);
    lv_obj_invalidate(p->img);
    hm_show(p->img, true);
    return true;
}

bool hm_pic_free(hm_pic_t *p)
{
    if (!p->px) return true;
    if (hm_pic_busy(p)) return false;
    hm_show(p->img, false);
    lv_image_cache_drop(&p->dsc);
    free(p->px);
    p->px = NULL;
    p->dsc.data = NULL;
    p->painted = false;
    p->ticket = 0;
    return true;
}

/* ================================================================ the faces together */

static void face_build(int f)
{
    if (FC.box[f]) return;
    lv_obj_t *b = bz_box(FC.pg);
    lv_obj_set_size(b, W, H);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    FC.box[f] = b;
    FACES[f]->build(b);
}

static void face_minute(int f, bool force)
{
    if (f < 0 || !FC.box[f]) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    FACES[f]->minute(&tm, force);
}

static bool face_ready(int f)
{
    if (FACES[f]->tick) FACES[f]->tick();
    return !FACES[f]->ready || FACES[f]->ready();
}

/* a face that went: its pictures' memory back (now, or once the worker has finished with them) */
static void face_leave(int f)
{
    if (f < 0 || !FC.box[f]) return;
    hm_show(FC.box[f], false);
    if (FACES[f]->leave) FACES[f]->leave();
    if (FACES[f]->held && FACES[f]->held()) FC.leaving |= 1u << f;
}

static void leaving_tick(void)
{
    for (int f = 0; FC.leaving && f <= HM_NFACES; f++) {
        if (!(FC.leaving >> f & 1) || f == FC.cur || (FC.stepping && f == FC.step_to)) continue;
        if (FACES[f]->leave) FACES[f]->leave();
        if (!FACES[f]->held || !FACES[f]->held()) FC.leaving &= ~(1u << f);
    }
}

/* ---- the index: where you are among the faces, at the right edge ---- */

static void index_show(void)
{
    int faces = hm_cfg()->faces, n = 0;
    for (int i = 0; i < HM_NFACES; i++) n += faces >> i & 1;
    int shown = FC.stepping ? FC.step_to : FC.cur;
    int sig = shown == HM_FACE_NIGHT || shown < 0 || n < 2 ? -2 : shown * 4096 + faces;
    sig ^= bz_ui_dark() ? 0x40000000 : 0;
    if (sig == FC.index_shown) return;
    FC.index_shown = sig;
    bool on_ = !(shown == HM_FACE_NIGHT || shown < 0 || n < 2);
    hm_show(FC.index, on_);
    if (!on_) return;
    int y = 0;
    for (int i = 0; i < HM_NFACES; i++) {
        bool in = faces >> i & 1;
        hm_show(FC.index_dot[i], in);
        if (!in) continue;
        bool on = i == shown;
        lv_obj_set_size(FC.index_dot[i], 6, on ? 20 : 6);
        lv_obj_set_pos(FC.index_dot[i], 0, y);
        lv_obj_set_style_bg_color(FC.index_dot[i], bz_lv(on ? BZ_C_INK : BZ_C_FAINT), 0);
        y += (on ? 20 : 6) + 7;
    }
    y -= 7;
    lv_obj_set_size(FC.index, 6, y);
    lv_obj_set_pos(FC.index, W - 24, (H - y) / 2);
}

/* The index is part of the clock page (it slides away with it), but held still like the page dots while a push
 * moves the faces: it rides on the top layer for as long as the push lasts. */
static void index_lift(bool up)
{
    lv_obj_t *want = up ? bz_ui_glass() : FC.pg;
    if (lv_obj_get_parent(FC.index) == want) return;
    lv_obj_set_parent(FC.index, want);
    if (up) lv_obj_move_to_index(FC.index, 0); /* among the first 32 the chrome hiders count */
    lv_obj_set_pos(FC.index, W - 24, (H - lv_obj_get_height(FC.index)) / 2);
    if (!up) lv_obj_move_foreground(FC.index);
}

static void flick(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx; (void)u;
    if (dy < -50 || vy < -500) {
        FC.step_v = fabsf(vy) / H;
        hm_faces_step(1);
    } else if (dy > 50 || vy > 500) {
        FC.step_v = fabsf(vy) / H;
        hm_faces_step(-1);
    }
}

static void theme_changed(void) { FC.force = true; }

lv_obj_t *hm_faces_build(lv_obj_t *parent)
{
    FC.pg = bz_box(parent);
    lv_obj_set_size(FC.pg, W, H);
    lv_obj_add_style(FC.pg, bz_style_fill(BZ_C_GROUND), 0);
    FC.index = bz_box(FC.pg);
    lv_obj_remove_flag(FC.index, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < HM_NFACES; i++) {
        lv_obj_t *d = bz_box(FC.index);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        FC.index_dot[i] = d;
    }
    bz_motion_init(&FC.k, 0, 0.001f);
    FC.k.keep = true;
    /* a flick up or down: the next face, or the one before (sideways goes on to home mode's own pages) */
    bz_drag_t d = { .end = flick, .axis = 2, .slop = 12 };
    bz_drag_attach(FC.pg, &d);
    bz_theme_on_change(theme_changed);
    return FC.pg;
}

void hm_faces_set(int f)
{
    if (f < 0 || f > HM_FACE_NIGHT) f = HM_FACE_TYPE;
    if (f == FC.cur && FC.box[f]) return;
    int old = FC.cur;
    face_build(f);
    FC.cur = f;
    FC.dark = bz_ui_dark();
    face_minute(f, true);
    hm_show(FC.box[f], true);
    if (old >= 0 && old != f) face_leave(old);
    lv_obj_move_foreground(FC.index);
    FC.index_shown = -1;
    index_show();
}

int hm_faces_face(void) { return FC.cur; }

void hm_faces_look(void)
{
    FC.last_min = -1;
    FC.force = true;
}

bool hm_faces_night(void)
{
    if (!hm_cfg()->night) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return hm_clock_set(&tm) && (tm.tm_hour >= 22 || tm.tm_hour < 7);
}

void hm_faces_step(int dir)
{
    if (FC.stepping || FC.cur == HM_FACE_NIGHT || FC.cur < 0) return;
    int faces = hm_cfg()->faces & HM_FACES_ALL;
    int f = FC.cur;
    for (int i = 0; i < HM_NFACES; i++) {
        f = (f + (dir > 0 ? 1 : HM_NFACES - 1)) % HM_NFACES;
        if (faces >> f & 1) break;
    }
    if (f == FC.cur || !(faces >> f & 1)) return;
    FC.stepping = true;
    FC.pushing = false;
    FC.step_to = f;
    FC.step_dir = dir;
    FC.step_t0 = hal_seconds();
    face_build(f);
    FC.leaving &= ~(1u << f);
    face_minute(f, true); /* its pictures start painting now; the push waits for them */
    hm_cfg()->face = f;
    hm_cfg_save();
    bz_ui_keep_alive();
}

bool hm_faces_moving(void) { return FC.stepping; }

void hm_faces_refresh(bool shown)
{
    leaving_tick();
    if (FC.cur < 0) return;
    bool dark = bz_ui_dark();
    bool tone = (int)dark != FC.dark || FC.force;
    FC.dark = dark;
    FC.force = false;
    (void)shown; /* hidden, its words change without drawing: kept current so a slide shows it right */
    if (FACES[FC.cur]->tick && !FC.pushing) FACES[FC.cur]->tick();
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    home_weather_t w;
    home_weather_get(&w);
    unsigned tg = tba_gen();
    if (tm.tm_min != FC.last_min || tone || w.gen != FC.wx_gen || tg != FC.tba_gen) {
        FC.last_min = tm.tm_min;
        FC.wx_gen = w.gen;
        FC.tba_gen = tg;
        FACES[FC.cur]->minute(&tm, tone);
    }
    index_show();
}

/* The step: the new face's pictures painted, then a push the platform composes, the new face drawn offscreen a
 * band at a time as it comes into view; the old face's memory given back as soon as it is off the glass. */
static void step_frame(void)
{
    if (!FC.stepping) return;
    bz_ui_keep_alive();
    if (!FC.pushing) {
        /* waiting for its pictures (a second and a half at most: then it comes as it is) */
        bool ready = face_ready(FC.step_to);
        if (!ready && hal_seconds() - FC.step_t0 < 1.5) return;
        int old = FC.cur;
        /* the index moves to where the flick is going at once, and stays still while the faces move */
        FC.index_shown = -1;
        index_show();
        index_lift(true);
        bool ok = bz_ui_can_push() && bz_ui_push_begin(FC.step_dir > 0, NULL, NULL);
        /* the state switches under the glass */
        FC.cur = FC.step_to;
        hm_show(FC.box[FC.cur], true);
        face_leave(old);
        if (!ok) {
            index_lift(false);
            FC.stepping = false;
            return;
        }
        FC.pushing = true;
        bz_motion_init(&FC.k, 0, 0.001f);
        FC.k.keep = true;
        float v = FC.step_v > 6 ? 6 : FC.step_v;
        bz_motion_to_v(&FC.k, 1, BZ_PAGE, v);
        return;
    }
    bool moving = bz_motion_tick(&FC.k);
    float k = FC.k.value < 0 ? 0 : FC.k.value > 1 ? 1 : FC.k.value;
    if (moving) {
        bz_ui_sheet((int)(k * H + 0.5f));
        return;
    }
    if (bz_ui_sheet_shown() != H) {
        bz_ui_sheet(H);
        return;
    }
    bz_ui_sheet_end();
    index_lift(false);
    FC.pushing = FC.stepping = false;
    FC.last_min = -1; /* its minute again, now that it's on the glass: anything that changed meanwhile */
}

void hm_faces_frame(double now)
{
    step_frame();
    if (FC.stepping || FC.cur < 0) return;
    if (FACES[FC.cur]->frame) FACES[FC.cur]->frame(now);
}

void hm_faces_release(void)
{
    for (int f = 0; f <= HM_FACE_NIGHT; f++)
        if (FC.box[f] && FACES[f]->leave) {
            FACES[f]->leave();
            if (FACES[f]->held && FACES[f]->held()) FC.leaving |= 1u << f;
        }
    FC.last_min = -1;
    FC.force = true;
}

bool hm_faces_released(void)
{
    leaving_tick();
    for (int f = 0; f <= HM_FACE_NIGHT; f++)
        if (FC.box[f] && FACES[f]->held && FACES[f]->held()) return false;
    return true;
}

/* the dev console's "hm step up|down" and the faces' own words */
void hm_faces_dev(const char *a)
{
    while (*a == ' ') a++;
    if (!strncmp(a, "step", 4)) {
        const char *s = a + 4;
        while (*s == ' ') s++;
        FC.step_v = 0;
        hm_faces_step(!strcmp(s, "down") ? -1 : 1);
        printf("hm: step %s\n", !strcmp(s, "down") ? "down" : "up");
        return;
    }
    printf("hm faces: %s, %s%s, index %s, leaving 0x%x\n", FC.cur >= 0 ? HM_FACE_NAME[FC.cur] : "none",
           FC.stepping ? "stepping to " : "at rest", FC.stepping ? HM_FACE_NAME[FC.step_to] : "",
           lv_obj_get_parent(FC.index) == FC.pg ? "on the page" : "lifted", (unsigned)FC.leaving);
}
