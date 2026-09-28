/* home_px — a small software rasterizer for pictures drawn once and shown many times: album art, home mode's
 * clock faces, the now-playing backdrop, the weather's sky.
 *
 * Why its own: LVGL draws what is on screen every time it is invalidated; these pictures are drawn once (a song,
 * a minute, a tone) into PSRAM and then only shown, as images. Everything here is antialiased by coverage from a
 * signed distance, clipped to a rectangle (so a moving hand redraws only its own box), and written either into an
 * RGB888 working picture (exact blending, for the art) or straight into RGB565 with an ordered dither (the faces:
 * gradients this faint band in 565 otherwise).
 *
 * Pure C with no LVGL and no libm transcendental in any per-pixel loop (sqrtf only on antialiased edges, where the
 * P4's FPU does it in hardware): it builds on the PC too, which is how the art's styles were tuned. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *p16;       /* RGB565 picture, or NULL */
    uint32_t *p32;       /* 0x00RRGGBB working picture, or NULL */
    int w, h, stride;    /* stride in pixels */
    int x1, y1, x2, y2;  /* the clip, inclusive: nothing is drawn outside it */
} hpx_t;

void hpx_init16(hpx_t *c, uint16_t *px, int w, int h, int stride);
void hpx_init32(hpx_t *c, uint32_t *px, int w, int h, int stride);
void hpx_clip(hpx_t *c, int x1, int y1, int x2, int y2); /* intersected with the picture */
void hpx_unclip(hpx_t *c);

static inline uint32_t hpx_rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
static inline int hpx_r(uint32_t c) { return (int)(c >> 16 & 255); }
static inline int hpx_g(uint32_t c) { return (int)(c >> 8 & 255); }
static inline int hpx_b(uint32_t c) { return (int)(c & 255); }
/* a + (b - a)·t/256, per channel */
uint32_t hpx_mix(uint32_t a, uint32_t b, int t256);
/* 565 → 888 with the low bits filled, and 888 → 565 rounded (no dither) */
static inline uint32_t hpx_from565(uint16_t v)
{
    int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    return (uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 2 | g >> 4) << 8 | (uint32_t)(b << 3 | b >> 2);
}
static inline uint16_t hpx_to565(uint32_t c)
{
    int r = (hpx_r(c) * 31 + 127) / 255, g = (hpx_g(c) * 63 + 127) / 255, b = (hpx_b(c) * 31 + 127) / 255;
    return (uint16_t)(r << 11 | g << 5 | b);
}
/* Luminance 0..255 (Rec. 709 weights on the encoded values: good enough to choose an ink) */
static inline int hpx_luma(uint32_t c) { return (hpx_r(c) * 54 + hpx_g(c) * 183 + hpx_b(c) * 19) >> 8; }

/* ---- pixels ---- */
/* One pixel blended at alpha a (0..255), dithered when the picture is 565. The clip is the caller's business. */
void hpx_put(hpx_t *c, int x, int y, uint32_t rgb, int a);
/* The clip filled (dithered in 565: a colour between two 565 levels comes out as that colour, not the nearer level) */
void hpx_fill(hpx_t *c, uint32_t rgb);
/* The clip copied from a picture of the same format and shape (what was under a moving hand) */
void hpx_copy_from(hpx_t *c, const void *src, int src_stride);

/* ---- shapes: antialiased; `soft` > 1 widens the edge into a blur that many px wide (shadows, glows) ---- */
void hpx_disc(hpx_t *c, float cx, float cy, float r, float soft, uint32_t rgb, int a);
/* a ring of width w centred on radius r */
void hpx_ring(hpx_t *c, float cx, float cy, float r, float w, float soft, uint32_t rgb, int a);
/* a capsule: the segment (x0,y0)-(x1,y1) swept by radius r (a hand; a stroke with round caps) */
void hpx_capsule(hpx_t *c, float x0, float y0, float x1, float y1, float r, float soft, uint32_t rgb, int a);
void hpx_rrect(hpx_t *c, float x, float y, float w, float h, float r, float soft, uint32_t rgb, int a);
void hpx_tri(hpx_t *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, int a);
/* the part of a disc between two angles (radians, clockwise from twelve o'clock): a quarter circle, a pie */
void hpx_sector(hpx_t *c, float cx, float cy, float r, float a0, float a1, uint32_t rgb, int a);
/* filled from a height line down to the bottom of the clip: top[x - x0] for each x of the clip (ridges, dunes) */
void hpx_ridge(hpx_t *c, const float *top, int x0, uint32_t rgb, int a);

/* ---- gradients (the whole clip) ---- */
/* Along (dx, dy) (a unit vector) from t = 0 at (ox, oy) to t = 1 `len` further on, through n stops at pos[] (0..256) */
void hpx_linear(hpx_t *c, float ox, float oy, float dx, float dy, float len, const uint32_t *stops, const int *pos,
                int n, int a);
/* a glow: rgb at alpha a in the middle, falling off like a gaussian to nothing at radius r */
void hpx_glow(hpx_t *c, float cx, float cy, float r, uint32_t rgb, int a);

/* ---- finishing ---- */
/* The working picture's clip into dst (565, the same shape) with film grain of ±grain/2 levels and a dither */
void hpx_finish565(const hpx_t *src32, uint16_t *dst, int dst_stride, int grain, uint32_t seed);
/* half size, box filtered: 32 → 32, and 32 → 565 with grain and a dither (the source is left as it was) */
void hpx_half32(const uint32_t *src, int sw, int sh, int sstride, uint32_t *dst, int dstride);
void hpx_half565(const uint32_t *src, int sw, int sh, int sstride, uint16_t *dst, int dstride, int grain, uint32_t seed);
/* the picture's colours averaged over an n × m grid, row-major (the tint's seed, the backdrop's mesh) */
void hpx_grid(const hpx_t *src, uint32_t *out, int n, int m);

/* ---- noise and hashing ---- */
uint32_t hpx_hash(uint32_t x);                    /* a 32-bit mix (lowbias32) */
uint32_t hpx_hash_str(const char *s, uint32_t h); /* FNV-1a over the bytes, folded through hpx_hash */
/* value noise, smooth (cubic fade), 0..1 */
float hpx_noise1(float x, uint32_t seed);
float hpx_noise2(float x, float y, uint32_t seed);
typedef struct { uint32_t s; } hpx_rng_t;
static inline uint32_t hpx_next(hpx_rng_t *r)
{
    /* mulberry32: the specimen's own covers use the same generator */
    uint32_t t = (r->s += 0x6D2B79F5u);
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return t ^ (t >> 14);
}
static inline float hpx_rand(hpx_rng_t *r) { return (float)(hpx_next(r) >> 8) * (1.0f / 16777216.0f); }
static inline float hpx_range(hpx_rng_t *r, float lo, float hi) { return lo + (hi - lo) * hpx_rand(r); }

/* ---- colour ---- */
/* OKLCH (L 0..1, C 0..~0.3, h degrees) to sRGB, its chroma reduced until it fits */
uint32_t hpx_oklch(float L, float C, float h);
/* and back */
void hpx_to_oklch(uint32_t rgb, float *L, float *C, float *h);
/* WCAG contrast ratio ×100 between two colours (450 = 4.5:1) */
int hpx_contrast(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif
