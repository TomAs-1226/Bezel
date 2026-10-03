/* ui_clip — an avatar animation baked on the PC and played from the card (see ui_clip.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

typedef struct ui_clip ui_clip_t;

ui_clip_t *ui_clip_create(lv_obj_t *parent); /* an image centred in `parent`, hidden until something plays */
void ui_clip_enable(bool on);                /* dev: off plays nothing, so the drawn avatar shows */
bool ui_clip_available(void);                /* the card holds a set of clips (CATOS/PET/CLIPS) */
/* <card>/CATOS/PET/CLIPS/<name>.CLP. `once`: play it through and stop, whatever the file says about looping.
 * Asking again for the loop already playing changes nothing. False: no such clip, or no memory. */
bool ui_clip_play(ui_clip_t *c, const char *name, bool once);
bool ui_clip_frame(ui_clip_t *c);            /* every frame; true while frames are coming */
bool ui_clip_done(ui_clip_t *c);
void ui_clip_dim(ui_clip_t *c, float dim);   /* 0..0.9 toward the ground */
void ui_clip_stop(ui_clip_t *c);             /* off the screen: the file closed, the picture given back */
size_t ui_clip_bytes(ui_clip_t *c);
