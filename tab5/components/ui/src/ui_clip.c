/* ui_clip — the avatar's animations, played back rather than drawn.
 *
 * The tablet's own renderer never looked like the Avatar Lab's and cost frames it does not have. So the
 * real renderer runs on the PC (tab5-shots/pet/bake.mjs), every animation is rasterised there, and what
 * reaches the card is one file per animation holding, for each frame, only the rows that changed, run-length
 * coded over a small palette. Playing one is a read of a few kilobytes and a handful of fills.
 *
 * The file (little endian):  "TCL1"  u16 w  u16 h  u16 frames  u16 colours  u8 loop  u8 fps  u16 maxFrameBytes
 *   palette: colours x RGBA, straight alpha. The picture keeps its alpha (RGB565 and an A8 plane after it),
 *            because what is behind an avatar is not one colour: home mode paints its own ground, and a
 *            picture blended over the token's colour showed as a square a shade off. Dimming is the alpha
 *            table scaled, so it fades toward whatever is really there.
 *   frames:  u16 durationMs  u16 y0  u16 rows  u32 bytes, then (count, index) pairs, each row summing to w.
 *            Frame 0 is the whole picture, so a loop comes round by starting the file again. */
#include "ui_clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"
#include "hal.h"

struct ui_clip {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint16_t *px;      /* w*h RGB565 then w*h alpha, in PSRAM, only while it is on a screen that shows */
    uint8_t *al;
    /* One clip into the next. Every clip begins from the same calm pose, but the one it replaces is wherever
     * it had got to, so a switch is a short dissolve from the picture as it was (`old`) to the new clip as it
     * plays, shown from `disp`. Both exist only for the eighth of a second it takes. */
    uint8_t *old, *disp;
    int fade;
    uint8_t *scratch;  /* one frame's bytes */
    size_t nscratch;
    FILE *f;
    char *iobuf;
    int w, h, frames, frame, ncol;
    long data0;
    bool loop, once, done;
    double due;        /* when the frame on show stops being the one to show */
    uint8_t pal[256][4];
    uint16_t lut[256];
    uint8_t alut[256];
    float dim;
    uint32_t ground;
    char name[24];
};

static void lut_make(ui_clip_t *c)
{
    c->ground = bz_color(BZ_C_GROUND) & 0xFFFFFF;
    for (int i = 0; i < 256; i++) {
        int a = i < c->ncol ? c->pal[i][3] : 0;
        c->alut[i] = (uint8_t)((float)a * (1.0f - c->dim) + 0.5f);
        c->lut[i] = (uint16_t)((c->pal[i][0] >> 3) << 11 | (c->pal[i][1] >> 2) << 5 | (c->pal[i][2] >> 3));
    }
}

ui_clip_t *ui_clip_create(lv_obj_t *parent)
{
    ui_clip_t *c = lv_malloc_zeroed(sizeof *c);
    if (!c) return NULL;
    c->img = lv_image_create(parent);
    lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(c->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return c;
}

/* Off unless asked for (dev "pet clips 1"). A clip is a video: it cannot turn to a finger or change its mind
 * half way, and the owner saw exactly that. The avatar is drawn live; the clips stay for comparison. */
static bool s_off = true;
void ui_clip_enable(bool on) { s_off = !on; }

bool ui_clip_available(void)
{
    if (s_off) return false;
    static double looked = -100;
    static bool have;
    double now = hal_seconds();
    if (now - looked < 5) return have; /* a stat on the card, not every frame */
    looked = now;
    const char *sd = hal_sd_root();
    char p[96];
    have = false;
    if (sd) {
        snprintf(p, sizeof p, "%s/CATOS/PET/CLIPS/IDLE_QUIET.CLP", sd);
        FILE *f = fopen(p, "rb");
        if (f) {
            have = true;
            fclose(f);
        }
    }
    return have;
}

void ui_clip_stop(ui_clip_t *c)
{
    if (!c) return;
    if (c->f) fclose(c->f);
    c->f = NULL;
    free(c->iobuf);
    free(c->scratch);
    free(c->px);
    free(c->old);
    free(c->disp);
    c->old = c->disp = NULL;
    c->fade = 0;
    c->iobuf = NULL;
    c->scratch = NULL;
    c->px = NULL;
    c->nscratch = 0;
    c->name[0] = 0;
    lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);
}

#define FADE_N 8

static void fade_end(ui_clip_t *c)
{
    free(c->old);
    free(c->disp);
    c->old = c->disp = NULL;
    c->fade = 0;
    c->dsc.data = (const uint8_t *)c->px;
    lv_obj_invalidate(c->img);
}

/* `disp` = the old picture fading into the new one. Weighted by alpha, so an edge that is leaving does not
 * drag the colour of nothing into the mix; where both are clear there is nothing to do. */
static void fade_mix(ui_clip_t *c)
{
    size_t n = (size_t)c->w * c->h;
    const uint16_t *po = (const uint16_t *)c->old, *pn = c->px;
    const uint8_t *ao = c->old + n * 2, *an = c->al;
    uint16_t *pd = (uint16_t *)c->disp;
    uint8_t *ad = c->disp + n * 2;
    int k = 256 - c->fade * 256 / FADE_N; /* 0 old .. 256 new */
    for (size_t i = 0; i < n; i++) {
        int a0 = ao[i], a1 = an[i];
        if (!(a0 | a1)) {
            ad[i] = 0;
            continue;
        }
        if (a0 == a1 && po[i] == pn[i]) {
            pd[i] = pn[i];
            ad[i] = (uint8_t)a1;
            continue;
        }
        int w0 = a0 * (256 - k), w1 = a1 * k, ws = w0 + w1;
        if (!ws) {
            ad[i] = 0;
            continue;
        }
        uint16_t o = po[i], q = pn[i];
        int r = ((o >> 11) * w0 + (q >> 11) * w1) / ws, g = ((o >> 5 & 63) * w0 + (q >> 5 & 63) * w1) / ws,
            b = ((o & 31) * w0 + (q & 31) * w1) / ws;
        pd[i] = (uint16_t)(r << 11 | g << 5 | b);
        ad[i] = (uint8_t)(ws >> 8);
    }
    lv_obj_invalidate(c->img);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* The next frame off the card, onto the picture. False at the end of the file or on a bad one. */
static bool step(ui_clip_t *c)
{
    uint8_t h[10];
    if (c->frame >= c->frames || fread(h, 1, 10, c->f) != 10) return false;
    int dur = rd16(h), y0 = rd16(h + 2), rows = rd16(h + 4);
    uint32_t n = (uint32_t)h[6] | (uint32_t)h[7] << 8 | (uint32_t)h[8] << 16 | (uint32_t)h[9] << 24;
    c->frame++;
    c->due += dur / 1000.0;
    if (!n || !rows) return true; /* a hold carried on */
    if (n > c->nscratch || y0 + rows > c->h || fread(c->scratch, 1, n, c->f) != n) return false;
    const uint8_t *s = c->scratch, *e = s + n;
    for (int y = y0; y < y0 + rows; y++) {
        uint16_t *d = c->px + (size_t)y * c->w;
        uint8_t *da = c->al + (size_t)y * c->w;
        int x = 0;
        while (x < c->w && s + 2 <= e) {
            int k = s[0];
            uint16_t v = c->lut[s[1]];
            uint8_t va = c->alut[s[1]];
            s += 2;
            if (x + k > c->w) k = c->w - x;
            for (int i = 0; i < k; i++) d[x + i] = v;
            memset(da + x, va, (size_t)k);
            x += k;
        }
    }
    lv_area_t a;
    lv_obj_get_coords(c->img, &a);
    a.y1 += y0;
    a.y2 = a.y1 + rows - 1;
    lv_obj_invalidate_area(c->img, &a);
    return true;
}

bool ui_clip_play(ui_clip_t *c, const char *name, bool once)
{
    if (!c) return false;
    if (!once && c->f && !strcmp(c->name, name) && !c->once) return true; /* already looping it */
    const char *sd = hal_sd_root();
    if (!sd) return false;
    char p[112];
    snprintf(p, sizeof p, "%s/CATOS/PET/CLIPS/%s.CLP", sd, name);
    FILE *f = fopen(p, "rb");
    uint8_t h[16];
    if (!f) return false;
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "TCL1", 4)) {
        fclose(f);
        return false;
    }
    int w = rd16(h + 4), hh = rd16(h + 6), ncol = rd16(h + 10);
    size_t need = (size_t)rd16(h + 14) + 16;
    if (w <= 0 || hh <= 0 || w > 512 || hh > 512 || ncol > 256) {
        fclose(f);
        return false;
    }
    /* what is on show now, kept to dissolve from: only when there is a picture of the same size, and memory */
    bool fade = c->f && c->px && c->w == w && c->h == hh;
    if (fade) {
        size_t n3 = (size_t)w * hh * 3;
        uint8_t *o = malloc(n3), *d = c->disp ? c->disp : malloc(n3);
        if (o && d) {
            memcpy(o, c->disp ? (const void *)c->disp : (const void *)c->px, n3);
            free(c->old);
            c->old = o;
            c->disp = d;
        } else {
            free(o);
            if (!c->disp) free(d);
            fade = false;
        }
    }
    if (!fade && c->old) fade_end(c);
    if (c->f) fclose(c->f);
    c->f = f;
    if (!c->iobuf) c->iobuf = malloc(8192);
    if (c->iobuf) setvbuf(f, c->iobuf, _IOFBF, 8192);
    if (!c->px || c->w != w || c->h != hh) {
        free(c->px);
        c->px = malloc((size_t)w * hh * 3); /* over 512 bytes: PSRAM */
    }
    if (need > c->nscratch) {
        free(c->scratch);
        c->scratch = malloc(need);
        c->nscratch = c->scratch ? need : 0;
    }
    c->al = c->px ? (uint8_t *)c->px + (size_t)w * hh * 2 : NULL;
    if (!c->px || !c->scratch) {
        ui_clip_stop(c);
        return false;
    }
    c->w = w;
    c->h = hh;
    c->frames = rd16(h + 8);
    c->ncol = ncol;
    c->loop = h[12] != 0;
    c->once = once;
    c->done = false;
    memset(c->pal, 0, sizeof c->pal);
    if (fread(c->pal, 4, (size_t)ncol, f) != (size_t)ncol) {
        ui_clip_stop(c);
        return false;
    }
    c->data0 = 16 + 4L * ncol;
    lut_make(c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->frame = 0;
    c->due = hal_seconds();
    c->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    c->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    c->dsc.header.w = (uint32_t)w;
    c->dsc.header.h = (uint32_t)hh;
    c->dsc.header.stride = (uint32_t)w * 2;
    c->dsc.data_size = (uint32_t)w * hh * 3;
    c->dsc.data = fade ? c->disp : (const uint8_t *)c->px;
    c->fade = fade ? FADE_N : 0;
    lv_image_set_src(c->img, &c->dsc);
    lv_obj_center(c->img);
    lv_obj_remove_flag(c->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(c->img);
    if (!step(c)) {
        ui_clip_stop(c);
        return false;
    }
    if (c->fade) fade_mix(c);
    return true;
}

bool ui_clip_frame(ui_clip_t *c)
{
    if (!c || !c->f) return false;
    if (c->done) {
        if (c->fade) fade_end(c);
        return false;
    }
    double now = hal_seconds();
    if (now - c->due > 0.5) c->due = now; /* fell behind (the screen was busy): no sprint to catch up */
    int guard = 4;
    while (now >= c->due && guard--) {
        if (c->frame >= c->frames) {
            if (c->once || !c->loop) {
                c->done = true;
                return false;
            }
            fseek(c->f, c->data0, SEEK_SET);
            c->frame = 0;
        }
        if (!step(c)) {
            c->done = true;
            return false;
        }
    }
    if (c->fade) {
        if (--c->fade > 0) fade_mix(c);
        else fade_end(c);
        return true;
    }
    /* awake only while frames are coming: through a nine-second hold the loop may sleep, and the 10 Hz
     * refresh is early enough to start the blink that ends it */
    return c->due - now < 0.15;
}

bool ui_clip_done(ui_clip_t *c) { return !c || c->done || !c->f; }

void ui_clip_dim(ui_clip_t *c, float dim)
{
    if (!c) return;
    dim = dim < 0 ? 0 : dim > 0.9f ? 0.9f : dim;
    if (dim == c->dim) return;
    c->dim = dim;
    if (c->f) { /* the picture again through the new table: from the top of the file, which is whole */
        lut_make(c);
        if (!c->once) {
            fseek(c->f, c->data0, SEEK_SET);
            c->frame = 0;
            c->due = hal_seconds();
        }
    }
}

size_t ui_clip_bytes(ui_clip_t *c) { return c && c->px ? (size_t)c->w * c->h * 3 + c->nscratch + 8192 : 0; }
