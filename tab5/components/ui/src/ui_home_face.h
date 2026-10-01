/* Home mode's clock faces, one module each (ui_face_*.c), run by ui_home_faces.c: private to the ui component.
 *
 * A face is a full-screen box of LVGL objects over, at most, a picture or two painted on the covers' worker
 * (hm_pic_t). It is built the first time it shows; its objects stay (a few KB) but its pictures are freed when it
 * goes. Nothing in a face animates at rest unless it is small (the dial's second hand, a split flap, a falling
 * leaf), and nothing is set that hasn't changed (ui_text, the guarded setters below). */
#pragma once
#include "ui_home_priv.h"

#include "home_astro.h"
#include "home_px.h"
#include "home_scape.h"
#include "tba.h"
#include "ui_home_fonts.h"

#include <time.h>

typedef struct hm_face {
    const char *name;                                /* the settings' chip and the dev console's name */
    void (*build)(lv_obj_t *box);                    /* its objects, once, into its full-screen box */
    /* up to the minute (and the weather's and the blue alliance's latest); `force`: it has just come, or the tone
     * changed: colours and pictures again */
    void (*minute)(const struct tm *tm, bool force);
    void (*tick)(void);                              /* optional: 10 Hz while it shows (a picture done, a countdown) */
    void (*frame)(double now);                       /* optional: every frame while it shows at rest */
    bool (*ready)(void);                             /* optional: its pictures are painted (a step waits for them) */
    void (*leave)(void);                             /* optional: it went: its pictures' memory back when it can */
    bool (*held)(void);                              /* optional: it still holds memory */
} hm_face_t;

void hm_tree_dev(void); /* ui_face_tree.c: "hm tree" */

extern const hm_face_t HM_F_TYPE, HM_F_FLIP, HM_F_DIAL, HM_F_CHRONO, HM_F_SKY, HM_F_MOON, HM_F_ORRERY, HM_F_WORLD,
    HM_F_TREE, HM_F_MATCH, HM_F_WORDS, HM_F_NIGHT;

/* ---- a picture painted on the covers' worker (ui_home_art.c's hm_art_job) ---- */
typedef struct {
    uint16_t *px;
    lv_image_dsc_t dsc;
    lv_obj_t *img;
    unsigned ticket;
    bool painted;         /* px holds a finished picture, set on the image */
    int w, h;
    hs_tone_t tone;       /* what it was asked to be painted for (the worker reads it) */
} hm_pic_t;
/* an image object at (x, y) in `parent`, hidden until something is painted; nothing allocated yet */
void hm_pic_init(hm_pic_t *p, lv_obj_t *parent, int x, int y, int w, int h);
/* allocates if needed and queues fn(arg) on the worker to paint p->px; false without memory */
bool hm_pic_start(hm_pic_t *p, void (*fn)(void *), void *arg);
/* a painting finished: shown (the image invalidated); true when it just was */
bool hm_pic_poll(hm_pic_t *p);
bool hm_pic_busy(const hm_pic_t *p);  /* queued or being painted */
bool hm_pic_free(hm_pic_t *p);        /* false while the worker may still be painting into it */
static inline bool hm_pic_held(const hm_pic_t *p) { return p->px != NULL; }

/* ---- shared pieces (ui_home_faces.c) ---- */
bool hm_clock_set(const struct tm *tm);
void hm_hm(const struct tm *tm, char *out, size_t n);            /* "20:46" or "8:46", the owner's setting */
void hm_hm_t(time_t t, char *out, size_t n);                     /* the same for a unix time */
void hm_date(const struct tm *tm, char *out, size_t n, bool shortform); /* "monday, september 28" / "mon 28 sep" */
void hm_weather_line(char *out, size_t n);                       /* "72° clear", "" unknown */
void hm_tone(hs_tone_t *t);                                      /* the theme's colours for a painter */
/* where the tablet is (the weather's place): false until it has been resolved; Mission Viejo meanwhile */
bool hm_where(double *lat, double *lon);
double hm_tz_s(void);                                            /* the local zone, seconds east of UTC */
/* the blue alliance's state (a PSRAM copy, refreshed when its generation moves); NULL when there's nothing */
const tba_state_t *hm_tba(void);
/* a label's colour and opacity, and an object's position, set only when they change */
void hm_color(lv_obj_t *o, uint32_t rgb);
void hm_pos(lv_obj_t *o, int x, int y);
/* how wide a line of text is in a font */
int hm_text_w(const char *s, const lv_font_t *f);
/* a label whose baseline sits at y (the font's own ascent above it) */
void hm_baseline(lv_obj_t *label, int x, int y);
