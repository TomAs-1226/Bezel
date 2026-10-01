/* Glass surfaces baked into images. See bz_frost.h for why the tablet needs them. */
#include "bz_frost.h"

#include "bz_theme.h"
#include "bz_tokens.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BIG_FREE(p) heap_caps_free(p)
#else
#define BIG_ALLOC(n) malloc(n)
#define BIG_FREE(p) free(p)
#endif

/* ------------------------------------------------------------------ grain */

#define GRAIN_N 64
static int8_t GRAIN[GRAIN_N * GRAIN_N];
static bool grain_ready;

static float grain_hash(int x, int y)
{
    uint32_t h = (uint32_t)(x & (GRAIN_N - 1)) * 374761393u + (uint32_t)(y & (GRAIN_N - 1)) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)(h ^ (h >> 16)) / 4294967296.0f;
}

/* one octave, sampled every `step` px and smoothed, wrapping at GRAIN_N whatever the step */
static float grain_octave(int x, int y, int step)
{
    int x0 = x / step, y0 = y / step, w = GRAIN_N / step;
    float fx = (float)(x - x0 * step) / step, fy = (float)(y - y0 * step) / step;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = grain_hash(x0 % w, y0 % w), b = grain_hash((x0 + 1) % w, y0 % w);
    float c = grain_hash(x0 % w, (y0 + 1) % w), d = grain_hash((x0 + 1) % w, (y0 + 1) % w);
    float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

static void grain_init(void)
{
    if (grain_ready) return;
    grain_ready = true;
    for (int y = 0; y < GRAIN_N; y++)
        for (int x = 0; x < GRAIN_N; x++) {
            float v = grain_octave(x, y, 4) * 0.62f + grain_octave(x, y, 2) * 0.38f;
            int s = (int)lrintf((v - 0.5f) * 2 * 127);
            GRAIN[y * GRAIN_N + x] = (int8_t)(s < -127 ? -127 : s > 127 ? 127 : s);
        }
}

int8_t bz_grain_at(int x, int y)
{
    grain_init();
    return GRAIN[((y & (GRAIN_N - 1)) << 6) | (x & (GRAIN_N - 1))];
}

/* ------------------------------------------------------------------ baking */

/* Enough for the chrome that is on screen at once -- the dock, the orb, the island, a few cards -- with
 * room for both a light and a dark palette's worth before anything is evicted. */
#define FROST_MAX 20

typedef struct {
    bool used;
    int refs;         /* glass pieces showing it: never evicted while any does */
    int w, h, radius, opa;
    uint32_t base;
    uint32_t age; /* when it was last handed out, for eviction */
    lv_image_dsc_t dsc;
    uint8_t *px;      /* one allocation: the RGB565 pixels, then the A8 plane after them */
    uint16_t *rgb;
    uint8_t *alpha;
} frost_t;

static frost_t F[FROST_MAX];
static uint32_t frost_clock;

/* Signed distance to a rounded rectangle, negative inside. The compositor's sd_rrect, in the piece's own
 * coordinates rather than the screen's. */
static float sd_rrect(float px, float py, float w, float h, float r)
{
    float hw = w * 0.5f - r, hh = h * 0.5f - r;
    float qx = fabsf(px - w * 0.5f) - hw, qy = fabsf(py - h * 0.5f) - hh;
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float outside = sqrtf(mx * mx + my * my);
    float inside = (qx > qy ? qx : qy);
    return (outside > 0 ? outside : (inside < 0 ? inside : 0)) - r;
}

/* The same bevel profile the compositor uses, (1 - (1 - x)^4)^(1/4), so a piece baked here and the same
 * piece rendered there are the same shape of light and the design does not fork. */
static float profile(float x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float om = 1 - x, om4 = om * om * om * om;
    return sqrtf(sqrtf(1 - om4)); /* the fourth root: powf was a quarter of a bake */
}

static void bake(frost_t *f)
{
    const int w = f->w, h = f->h;
    const float r = (float)f->radius;
    /* the bevel's width, as band_of() in the compositor: wide enough to read, capped so a big panel does
     * not become all edge */
    float bw = 0.9f * r;
    if (bw < 11) bw = 11;
    if (bw > 40) bw = 40;

    const int br = bz_r(f->base), bg = bz_g(f->base), bb = bz_b(f->base);
    const uint32_t rimc = bz_pal->rim; /* the same rim light the compositor uses */
    const int rr = bz_r(rimc), rg = bz_g(rimc), rb = bz_b(rimc);

    static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    /* Deeper than the bevel every term but the top-lift is zero, and the lift depends on the row alone: those
     * pixels are one colour a row (four, with the dither), filled rather than worked out. On a control-center
     * panel that is most of it; the full distance-field work is only for the rim and the bevel. The first pull
     * of the control center baked its panels this way at ~377 ms a frame, and the finger's drag was lost. */
    int m = (int)ceilf(bw > r ? bw : r) + 1;
    const uint8_t a_in = (uint8_t)(255 * f->opa / 255);
    for (int y = 0; y < h; y++) {
        bool rows_in = y >= m && y < h - m && w > 2 * m;
        uint16_t fill[4] = { 0, 0, 0, 0 };
        if (rows_in) {
            float k = 1.0f + 0.10f * (1 - (float)y / h);
            int cr = (int)lrintf(br * k), cg = (int)lrintf(bg * k), cb = (int)lrintf(bb * k);
            cr = cr > 255 ? 255 : cr;
            cg = cg > 255 ? 255 : cg;
            cb = cb > 255 ? 255 : cb;
            for (int q = 0; q < 4; q++) {
                int bay = BAYER[((y & 3) << 2) | q];
                int r5 = (cr * 31 * 16 + bay * 255) / (255 * 16), g6 = (cg * 63 * 16 + bay * 255) / (255 * 16);
                int b5 = (cb * 31 * 16 + bay * 255) / (255 * 16);
                r5 = r5 > 31 ? 31 : r5;
                g6 = g6 > 63 ? 63 : g6;
                b5 = b5 > 31 ? 31 : b5;
                fill[q] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
        }
        for (int x = 0; x < w; x++) {
            if (rows_in && x == m) {
                uint16_t *o = f->rgb + (size_t)y * w;
                for (; x < w - m; x++) o[x] = fill[x & 3];
                memset(f->alpha + (size_t)y * w + m, a_in, (size_t)(w - 2 * m));
                x = w - m - 1;
                continue;
            }
            float d = sd_rrect(x + 0.5f, y + 0.5f, (float)w, (float)h, r);
            /* coverage: one pixel of antialiasing across the boundary, which the flat fill never had */
            float cov = 0.5f - d;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            if (cov <= 0) {
                f->rgb[(size_t)y * w + x] = 0;
                f->alpha[(size_t)y * w + x] = 0;
                continue;
            }
            float depth = d < 0 ? -d : 0;
            float t = depth / bw;
            float prof = profile(t > 1 ? 1 : t);

            /* Which way this part of the edge faces, from the gradient of the distance field. Only the
             * vertical part is used: the light is above, as it is everywhere else in Bezel, so the top
             * edge catches it and the bottom edge falls away. */
            /* (the field's gradient in closed form: radial in a corner, straight off an edge -- four more
             * distance evaluations a pixel before) */
            float ny;
            {
                float cx = x + 0.5f - w * 0.5f, cy = y + 0.5f - h * 0.5f;
                float qx = fabsf(cx) - (w * 0.5f - r), qy = fabsf(cy) - (h * 0.5f - r);
                float sy = cy < 0 ? -1.0f : 1.0f;
                if (qx > 0 && qy > 0) {
                    float ql = sqrtf(qx * qx + qy * qy);
                    ny = ql > 1e-6f ? sy * qy / ql : 0;
                } else {
                    ny = qy >= qx ? sy : 0;
                }
            }

            /* a hairline rim right at the boundary, and a soft inner band behind it */
            float rt = (depth - 0.35f) / (1.5f - 0.35f);
            rt = rt < 0 ? 0 : rt > 1 ? 1 : rt;
            float rim = 1 - rt * rt * (3 - 2 * rt);
            float band = 1 - (t > 1 ? 1 : t);
            band *= band; /* tighter to the edge than the rim's falloff, so the two read apart */

            /* Up-facing edges bright, down-facing edges dark: -ny is +1 at the top, -1 at the bottom.
             *
             * The band's share is small and its downward share smaller still. It reaches a bevel-width
             * into the piece, which on a 92-tall dock is most of the lower half, so anything but a whisper
             * there stops reading as an edge and starts reading as a gradient across the whole panel --
             * which is what it did at 0.22. */
            float face = -ny;
            /* 0.26 on the rim, because at full strength it came out as a hard white hairline -- 246 on a
             * surface of 32. A rim light is a hint of where the edge is, not a stroke around it. */
            float lit = 0.26f * rim * (0.55f + 0.45f * face) + band * 0.07f * (face > 0 ? face : face * 0.35f);

            /* No grain in a baked panel, whatever BZ_GLASS_GRAIN says -- that token is the compositor's.
             *
             * Two things went wrong with it here and both are about scale. The shared field tiles every
             * 64 px, which the compositor never notices because it modulates a bevel a few pixels wide,
             * but a baked dock is 660 across: the same motif repeats ten times and reads as a printed
             * pattern rather than a surface. And these panels are a dark blue-grey, 32/36/41 measured off
             * the glass, so the amplitude that is visible at all is a large fraction of the colour, and
             * adding it equally to three channels walks the surface toward neutral -- an olive cast over
             * the whole dock. Detiling would want a field as big as the largest piece, which is a lot of
             * PSRAM for something nearly invisible at this brightness.
             *
             * What makes this read as glass is the bevel and the antialiased edge, which a flat fill has
             * neither of. The grain was not paying for itself. Anything that does get added below is a
             * multiplier on the surface, never levels added to it, so it cannot shift the hue at all. */
            float k = 1.0f;
            /* the interior lifts very slightly toward the top, the way a thick sheet does */
            k += 0.10f * (1 - (float)y / h) * prof;

            int cr = (int)lrintf(br * k), cg = (int)lrintf(bg * k), cb = (int)lrintf(bb * k);
            /* The rim is the one term allowed to change the colour, because that is what it is: light
             * off the edge, in the palette's rim colour rather than in the surface's. */
            int l = (int)lrintf(lit * 255);
            if (l > 0) {
                cr += (rr * l) >> 8;
                cg += (rg * l) >> 8;
                cb += (rb * l) >> 8;
            } else if (l < 0) {
                cr += (cr * l) >> 8;
                cg += (cg * l) >> 8;
                cb += (cb * l) >> 8;
            }
            cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
            cg = cg < 0 ? 0 : cg > 255 ? 255 : cg;
            cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;

            /* Quantized here, with a dither, rather than handing LVGL ARGB8888 and letting it round.
             *
             * The panel is RGB565: five bits of red, six of green, five of blue. A gentle brightness
             * ramp across a dark surface therefore crosses a step in green while red and blue do not
             * move at all, and the ramp arrives on the glass as a green cast -- measured, the interior
             * went from r5=4 g6=9 b5=5 to r5=4 g6=11 b5=5, which is the whole of the olive tint. Nothing
             * about the colour maths was wrong; the rounding was. A 4x4 ordered dither spreads each
             * channel's error over neighbouring pixels so the ramp survives, and since the output is
             * RGB565A8 rather than ARGB8888 it also costs three bytes a pixel instead of four. */
            int bay = BAYER[((y & 3) << 2) | (x & 3)];
            int r5 = (cr * 31 * 16 + bay * 255) / (255 * 16);
            int g6 = (cg * 63 * 16 + bay * 255) / (255 * 16);
            int b5 = (cb * 31 * 16 + bay * 255) / (255 * 16);
            r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
            g6 = g6 < 0 ? 0 : g6 > 63 ? 63 : g6;
            b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
            uint16_t c565 = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            f->rgb[(size_t)y * w + x] = c565;
            f->alpha[(size_t)y * w + x] = (uint8_t)((int)lrintf(cov * 255) * f->opa / 255);
        }
    }
}

/* Milliseconds since boot, for the starvation hold-off. LVGL's tick is the one clock both the tablet and
 * the simulator already keep, and nothing here needs better than a tenth of a second. */
static uint32_t now_ms(void) { return lv_tick_get(); }
static uint32_t s_starved_until;

const lv_image_dsc_t *bz_frost(int w, int h, int radius, uint32_t base, int opa)
{
    if (w <= 0 || h <= 0 || opa <= 0) return NULL;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 0) radius = 0;

    frost_clock++;
    frost_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < FROST_MAX; i++) {
        frost_t *f = &F[i];
        if (!f->used) {
            if (!free_slot) free_slot = f;
            continue;
        }
        if (f->w == w && f->h == h && f->radius == radius && f->base == base && f->opa == opa) {
            f->age = frost_clock;
            f->refs++;
            return &f->dsc;
        }
        /* only a surface nobody shows may go. Evicting by age alone recycled the control center's link panel
         * (asked for once, so always the oldest) into a pill-sized picture while the panel still pointed at it:
         * the panel lost its surface and a stray capsule appeared in the middle of it */
        if (f->refs <= 0 && (!oldest || f->age < oldest->age)) oldest = f;
    }

    /* A surface already baked is free to hand out and was returned above; a new one costs an allocation
     * and a bake, and while PSRAM is starved neither is worth attempting. */
    if (s_starved_until && (int32_t)(now_ms() - s_starved_until) < 0) return NULL;
    s_starved_until = 0;

    frost_t *f = free_slot;
    if (!f) {
        /* every slot taken: the unused one that has gone longest without being asked for goes */
        if (!oldest) return NULL; /* all in use: the caller keeps its flat fill */
        f = oldest;
        BIG_FREE(f->px);
        memset(f, 0, sizeof *f);
    }
    size_t n = (size_t)w * h * 3; /* 2 bytes of RGB565 and 1 of alpha */
    f->px = BIG_ALLOC(n);
    if (!f->px) {
        memset(f, 0, sizeof *f);
        /* PSRAM is short. The caller keeps its flat fill, which is what it had before -- but it asks
         * again on its next frame, and asking is not free: the slot it would have taken was handed back,
         * so a surface that cannot fit is attempted sixty times a second, each attempt walking the heap.
         * Worse, when the allocation *just* fits, it fits by evicting a surface that is itself wanted
         * again a frame later, and each of those costs a bake -- tens of milliseconds. That trade is
         * what the stalls were. So a failure is remembered: for the next second nothing is baked at all
         * and every piece keeps its flat fill, which is honest, cheap and stable. */
        s_starved_until = now_ms() + 1000;
        return NULL;
    }
    f->rgb = (uint16_t *)f->px;
    f->alpha = f->px + (size_t)w * h * 2;
    f->used = true;
    f->w = w;
    f->h = h;
    f->radius = radius;
    f->base = base;
    f->opa = opa;
    f->age = frost_clock;
    f->refs = 1;
#ifdef ESP_PLATFORM
    int64_t t0 = esp_timer_get_time();
    bake(f);
    int64_t us = esp_timer_get_time() - t0;
    if (us > 10000) printf("frost: %dx%d r%d baked in %d ms\n", w, h, radius, (int)(us / 1000));
#else
    bake(f);
#endif

    f->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    f->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    f->dsc.header.w = (uint32_t)w;
    f->dsc.header.h = (uint32_t)h;
    f->dsc.header.stride = (uint32_t)w * 2; /* the RGB plane's stride; the A8 plane follows it */
    f->dsc.data_size = (uint32_t)n;
    f->dsc.data = f->px;
    return &f->dsc;
}

void bz_frost_release(const lv_image_dsc_t *d)
{
    if (!d) return;
    for (int i = 0; i < FROST_MAX; i++)
        if (F[i].used && &F[i].dsc == d && F[i].refs > 0) F[i].refs--;
}

void bz_frost_flush(void)
{
    for (int i = 0; i < FROST_MAX; i++) {
        if (!F[i].used) continue;
        BIG_FREE(F[i].px);
        memset(&F[i], 0, sizeof F[i]);
    }
}

size_t bz_frost_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < FROST_MAX; i++)
        if (F[i].used) n += (size_t)F[i].w * F[i].h * 3;
    return n;
}

size_t bz_frost_trim(size_t want)
{
    /* Only surfaces nobody is showing: a piece still pointing at one would lose its background, which is
     * the bug that made bz_frost count references in the first place. Biggest first -- the caller wants
     * bytes back, and one 300 KB panel nobody is looking at is worth more than every pill on the screen.
     * Dropping one is free; it is baked again if it is asked for, which is the trade being made. */
    size_t got = 0;
    while (got < want) {
        frost_t *big = NULL;
        for (int i = 0; i < FROST_MAX; i++) {
            frost_t *f = &F[i];
            if (!f->used || f->refs > 0) continue;
            if (!big || (size_t)f->w * f->h > (size_t)big->w * big->h) big = f;
        }
        if (!big) break;
        got += (size_t)big->w * big->h * 3;
        BIG_FREE(big->px);
        memset(big, 0, sizeof *big);
    }
    if (got) s_starved_until = 0; /* room again: bake when asked */
    return got;
}
