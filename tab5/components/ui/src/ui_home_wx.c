/* The weather drawn rather than named: a small picture of a condition (the sun, the moon, clouds, rain, snow, fog,
 * a storm) for the weather app, where the icon font has only a sun, a moon, a bolt and a thermometer, so an
 * overcast day showed a thermometer. Drawn with home_px on a flat ground of the exact 565 value the tile around
 * it is painted in, so the picture has no edge; lit from up and to the left, as everything in Bezel is. A few
 * ms on the UI thread for the sizes it's used at. */
#include "ui_home_priv.h"

#include "home_px.h"

/* a cloud: a few discs on a flat-bottomed capsule, its underside a shade darker (the light is above) */
static void cloud(hpx_t *c, float x, float y, float s, uint32_t top, uint32_t under)
{
    hpx_rrect(c, x - s * 1.1f, y - s * 0.12f, s * 2.25f, s * 0.64f, s * 0.32f, 0, under, 255);
    hpx_disc(c, x - s * 0.52f, y, s * 0.46f, 0, under, 255);
    hpx_disc(c, x + s * 0.2f, y - s * 0.2f, s * 0.62f, 0, under, 255);
    hpx_rrect(c, x - s * 1.08f, y - s * 0.16f, s * 2.2f, s * 0.56f, s * 0.28f, 0, top, 255);
    hpx_disc(c, x - s * 0.52f, y - s * 0.04f, s * 0.42f, 0, top, 255);
    hpx_disc(c, x + s * 0.2f, y - s * 0.24f, s * 0.58f, 0, top, 255);
}

void hm_wx_glyph(uint16_t *px, int w, int h, uint16_t ground, int code, bool day, bool dark)
{
    for (int i = 0; i < w * h; i++) px[i] = ground;
    hpx_t c;
    hpx_init16(&c, px, w, h, w);
    float s = (float)(w < h ? w : h) * 0.5f; /* the glyph's half size */
    float cx = w * 0.5f, cy = h * 0.5f;
    bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    bool storm = code >= 95, fog = code == 45 || code == 48;
    bool partly = code == 1 || code == 2, clear = code == 0;
    bool overcast = code == 3 || rain || snow || storm;
    uint32_t sun = dark ? 0xFFC45A : 0xF0A020, moon = dark ? 0xE6E4DA : 0xA9B0BA;
    uint32_t top = dark ? 0xDDE2E8 : 0xB9C1CA, under = dark ? 0xA9B1BA : 0x98A1AC;
    if (storm) {
        top = dark ? 0x8C939C : 0x7D8590;
        under = dark ? 0x6B727B : 0x646C77;
    }
    if (clear || partly) {
        /* the sun (or the moon), up and to the right when a cloud shares it */
        float ox = partly ? cx + s * 0.28f : cx, oy = partly ? cy - s * 0.26f : cy, r = partly ? s * 0.42f : s * 0.5f;
        if (day) {
            hpx_glow(&c, ox, oy, r * 2.0f, sun, dark ? 90 : 60);
            hpx_disc(&c, ox, oy, r, 0, sun, 255);
        } else {
            /* a crescent: the moon with the ground cut out of it */
            hpx_disc(&c, ox, oy, r, 0, moon, 255);
            hpx_disc(&c, ox + r * 0.42f, oy - r * 0.3f, r * 0.86f, 0, hpx_from565(ground), 255);
        }
    }
    if (partly) cloud(&c, cx - s * 0.12f, cy + s * 0.3f, s * 0.62f, top, under);
    if (overcast) {
        if (code == 3) cloud(&c, cx + s * 0.28f, cy - s * 0.24f, s * 0.46f, hpx_mix(top, under, 110), under);
        cloud(&c, cx - s * 0.06f, cy + (rain || snow || storm ? -s * 0.08f : s * 0.14f), s * 0.64f, top, under);
    }
    if (fog)
        for (int i = 0; i < 3; i++)
            hpx_rrect(&c, cx - s * (0.8f - 0.12f * (i & 1)), cy - s * 0.42f + i * s * 0.38f, s * (1.6f - 0.24f * (i & 1)),
                      s * 0.18f, s * 0.09f, 0, top, 255);
    uint32_t drop = dark ? 0x8FB4F0 : 0x4A78C0;
    if (rain)
        for (int i = 0; i < 3; i++) {
            float x = cx - s * 0.42f + i * s * 0.4f, y = cy + s * 0.52f;
            hpx_capsule(&c, x, y, x - s * 0.1f, y + s * 0.28f, s * 0.06f, 0, drop, 255);
        }
    if (snow)
        for (int i = 0; i < 3; i++) hpx_disc(&c, cx - s * 0.42f + i * s * 0.42f, cy + s * 0.62f + (i & 1) * s * 0.1f, s * 0.09f, 0, top, 255);
    if (storm) {
        uint32_t bolt = 0xFFB340;
        hpx_tri(&c, cx + s * 0.06f, cy + s * 0.28f, cx - s * 0.2f, cy + s * 0.68f, cx + s * 0.02f, cy + s * 0.64f, bolt, 255);
        hpx_tri(&c, cx + s * 0.02f, cy + s * 0.56f, cx + s * 0.2f, cy + s * 0.56f, cx - s * 0.1f, cy + s * 0.98f, bolt, 255);
    }
}
