/* ui_avatar_fx — the mouth, the blush and the particles drawn live around the avatar's face (ui_avatar_fx.c). */
#pragma once
#include "lvgl.h"

#include "ui_avatar.h"

typedef struct {
    float cx, cy;        /* the avatar's centre, screen px */
    float u;             /* px per unit of the format's 300-unit box */
    float ex[2], ey[2];  /* the eyes' centres, screen px: the mouth and the blush hang off them, so they turn with the head */
    uint32_t eyes, body; /* the colours on show now */
    float level;         /* the voice's loudness, 0..1 */
} av_fx_face_t;

/* How often the extras of state `s`, `t` s in, need drawing again: 0 when they are still. */
int av_fx_rate(av_state_t s, double t, float level);
/* Which parts of the avatar the extras of state `s` are moving in, so only those are drawn again and not the
 * whole 420 px of it: redrawing all of it every frame held a reaction to 20 fps. */
enum { AV_R_MOUTH = 1, AV_R_CHEEKS = 2, AV_R_TOP = 4, AV_R_SIDES = 8, AV_R_RTOP = 16, AV_R_LTOP = 32, AV_R_BOTTOM = 64, AV_R_TEAR = 128 };
unsigned av_fx_regions(av_state_t s, double t);
void av_fx_draw(lv_layer_t *layer, const av_fx_face_t *f, av_state_t s, double t);
