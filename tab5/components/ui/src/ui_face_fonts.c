/* Home mode's clock-face type, from the microSD card (ui_face_fonts.h).
 *
 * <sd>/CATOS/FONTS/<POSTER|FLIP|THIN|MATCH>.BIN are LVGL binary fonts (tools/make_face_fonts.py). One is read when
 * a face that uses it is about to show, held while it is the face on screen (or the one arriving), and freed when
 * it goes: at most the poster's 350 KB at a time, where built in they were 760 KB always. Two are held for the
 * moment of a step from one face to the next, because the old face is still on the glass until the push has
 * started, and the new one has to be loaded before it does.
 *
 * The card is read on the covers' worker (hm_art_job), as the faces' pictures are painted, never on the UI thread:
 * a font is 65-360 KB and the parse reads it a few bytes at a time. The UI thread only starts a load, asks whether
 * it has finished and swaps the font into the labels that show it. Reading is LVGL's own binary font loader over
 * its POSIX file system (lv_conf.h, drive 'S'), straight into the font's one allocation: reading the file into a
 * buffer first and creating from that (lv_binfont_create_from_buffer) would hold the font twice.
 *
 * A face never waits for the card to have the file. Missing, unreadable or too big for the PSRAM left, the font
 * is the stand-in (a bezel face that is built in), one line says which file and why, and the face is laid out in
 * that. A file copied to the card later is picked up the next time its face is shown. */
#include "ui_face_fonts.h"

#include "bz_fonts.h"
#include "hal.h"
#include "ui_home_priv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define MAX_USERS 16
#define HEADROOM (24 * 1024) /* the font's tables and the file system's read cache, besides its bitmaps */

typedef enum { FF_NONE, FF_LOADING, FF_READY, FF_MISSING } ff_state_t;

typedef struct {
    const char *file;                  /* in <sd>/CATOS/FONTS */
    const lv_font_t *stand_in;         /* what a label shows until, or without, the card's font */
    const char *stand_name;
    lv_font_t *font;                   /* the card's, adopted: in the labels */
    lv_font_t *fresh;                  /* what the worker made, until tick() adopts it */
    const char *why;                   /* the worker's reason when fresh is NULL */
    unsigned ticket;                   /* the worker's job while FF_LOADING */
    ff_state_t state;
    bool wanted;
    bool told;                         /* the missing-file line has been printed, until the font loads */
    uint32_t bytes;
    float ms;
    int nusers;
    lv_obj_t *users[MAX_USERS];
} slot_t;

/* The stand-ins: the nearest bezel face with the glyphs. The poster and the flaps are far bigger than anything
 * built in, so a missing file shows a clock that is small, but a clock. The match label's "?" is the one glyph
 * the 92 px face doesn't have. */
static slot_t FF[UI_FF_COUNT] = {
    [UI_FF_POSTER] = { .file = "POSTER.BIN", .stand_in = &bz_font_display_92, .stand_name = "bz_font_display_92" },
    [UI_FF_FLIP] = { .file = "FLIP.BIN", .stand_in = &bz_font_display_92, .stand_name = "bz_font_display_92" },
    [UI_FF_THIN] = { .file = "THIN.BIN", .stand_in = &bz_font_display_92, .stand_name = "bz_font_display_92" },
    [UI_FF_MATCH] = { .file = "MATCH.BIN", .stand_in = &bz_font_display_92, .stand_name = "bz_font_display_92" },
};

static const lv_font_t *current(const slot_t *s) { return s->state == FF_READY && s->font ? s->font : s->stand_in; }

const lv_font_t *ui_face_font(ui_ff_t which)
{
    if ((unsigned)which >= UI_FF_COUNT) return &bz_font_display_92;
    return current(&FF[which]);
}

static void apply(slot_t *s)
{
    const lv_font_t *f = current(s);
    for (int i = 0; i < s->nusers; i++)
        if (lv_obj_get_style_text_font(s->users[i], 0) != f) lv_obj_set_style_text_font(s->users[i], f, 0);
}

void ui_face_font_bind(lv_obj_t *label, ui_ff_t which)
{
    if ((unsigned)which >= UI_FF_COUNT || !label) return;
    slot_t *s = &FF[which];
    if (s->nusers < MAX_USERS) s->users[s->nusers++] = label;
    else printf("face fonts: more than %d labels on %s\n", MAX_USERS, s->file);
    lv_obj_set_style_text_font(label, current(s), 0);
}

/* ---- the worker ---- */

static void load_job(void *arg)
{
    slot_t *s = arg;
    s->fresh = NULL;
    s->why = NULL;
    const char *sd = hal_sd_root();
    if (!sd) {
        s->why = "there is no card";
        return;
    }
    char path[96], lvp[100];
    snprintf(path, sizeof path, "%s/CATOS/FONTS/%s", sd, s->file);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        s->why = "it is not on the card";
        return;
    }
    s->bytes = (uint32_t)st.st_size;
#ifdef ESP_PLATFORM
    /* LVGL halts on a failed allocation (LV_ASSERT_MALLOC): ask first, so a full PSRAM costs a smaller clock */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < s->bytes + HEADROOM) {
        s->why = "there is not enough PSRAM left for it";
        return;
    }
#endif
    snprintf(lvp, sizeof lvp, "S:%s", path);
    double t0 = hal_seconds();
    s->fresh = lv_binfont_create(lvp);
    s->ms = (float)((hal_seconds() - t0) * 1000.0);
    if (!s->fresh) s->why = "LVGL could not read it";
}

static void start(slot_t *s)
{
    s->ticket = hm_art_job(load_job, s);
    s->state = s->ticket ? FF_LOADING : FF_NONE; /* a full queue: tick() asks again */
}

/* the card's font out of the labels, then out of memory */
static void drop(slot_t *s)
{
    if (s->state == FF_READY) {
        s->state = FF_NONE;
        apply(s);
        lv_binfont_destroy(s->font);
        s->font = NULL;
    }
    s->state = FF_NONE;
}

/* ---- the UI thread ---- */

void ui_face_fonts_want(uint32_t mask)
{
    for (int i = 0; i < UI_FF_COUNT; i++) {
        slot_t *s = &FF[i];
        bool want = mask >> i & 1;
        s->wanted = want;
        if (want && s->state == FF_NONE) start(s);
        else if (!want && s->state != FF_LOADING) drop(s); /* a load in flight is dropped by tick() when it lands */
    }
}

bool ui_face_fonts_ready(uint32_t mask)
{
    for (int i = 0; i < UI_FF_COUNT; i++)
        if ((mask >> i & 1) && FF[i].state != FF_READY && FF[i].state != FF_MISSING) return false;
    return true;
}

bool ui_face_fonts_tick(void)
{
    bool changed = false;
    for (int i = 0; i < UI_FF_COUNT; i++) {
        slot_t *s = &FF[i];
        if (s->state == FF_NONE && s->wanted) {
            start(s);
            continue;
        }
        if (s->state != FF_LOADING || !hm_art_job_done(s->ticket)) continue;
        s->ticket = 0;
        lv_font_t *f = s->fresh;
        s->fresh = NULL;
        if (!f) {
            s->state = FF_MISSING;
            if (!s->told) {
                s->told = true;
                printf("face fonts: CATOS/FONTS/%s: %s; the face is set in %s (copy tab5/sdcard with tab5_dev.py deploy)\n",
                       s->file, s->why ? s->why : "no font", s->stand_name);
            }
            continue;
        }
        if (!s->wanted) {
            lv_binfont_destroy(f); /* the face went while it loaded */
            s->state = FF_NONE;
            continue;
        }
        s->font = f;
        s->state = FF_READY;
        s->told = false;
        apply(s);
        changed = true;
        printf("face fonts: %s %u KB in %.0f ms\n", s->file, (unsigned)((s->bytes + 512) / 1024), (double)s->ms);
    }
    return changed;
}

void ui_face_fonts_release(void) { ui_face_fonts_want(0); }

bool ui_face_fonts_released(void)
{
    ui_face_fonts_tick();
    for (int i = 0; i < UI_FF_COUNT; i++)
        if (FF[i].state == FF_LOADING || FF[i].state == FF_READY) return false;
    return true;
}

void ui_face_fonts_dev(void)
{
    static const char *const ST[] = { "not held", "loading", "loaded", "not on the card" };
    for (int i = 0; i < UI_FF_COUNT; i++) {
        slot_t *s = &FF[i];
        printf("face font %-9s %-15s %s, %d labels%s", s->file, ST[s->state], s->wanted ? "wanted" : "unwanted", s->nusers,
               s->state == FF_READY ? "" : "\n");
        if (s->state == FF_READY)
            printf(", %u KB, loaded in %.0f ms, line %d\n", (unsigned)((s->bytes + 512) / 1024), (double)s->ms,
                   (int)s->font->line_height);
    }
}
