/* ui_pet — the pet's screens: its page in home mode, the idle face, and what keeps the pet going (private to ui).
 *
 * The character is ui_avatar.c's; the life is home_pet.c's. This file is the glue and the two screens:
 *   - the pet page, right of "now playing": the avatar large, its name and mood in words, four quiet meters, and
 *     feed / play / sleep, with a finger stroking the avatar itself;
 *   - the idle face (HM_F_PET, one of the clock faces): the avatar alone on the ground, a small clock in a
 *     corner, doing quiet things on a long loop; it dims and settles the longer nobody is there. */
#pragma once
#include "ui_home_face.h"

/* once, from ui_home_mode_init: the pet's tick (its clock, its file, the avatar file on the card) */
void ui_pet_init(void);

/* the pet page (a page of home mode) */
lv_obj_t *hm_pet_build(lv_obj_t *parent);
void hm_pet_refresh(bool shown);       /* 10 Hz while home mode is up */
void hm_pet_frame(bool current);       /* every frame; `current`: it is the page on screen */
void hm_pet_near(bool near);           /* the page is on screen or beside it: its body is baked, or given back */

/* the dev console's "pet ..." is ui_pet_dev (ui_home_mode.h): "pet", "pet show", "pet idle", "pet face <name>",
 * "pet state <name>", "pet react <name>", "pet avatar", "pet set hunger 0.9", "pet feed fish" ... */
/* the character that shows on screen right now, for the dev console's contact sheet */
bool ui_pet_show_expr(const char *name);

/* the companion's avatar, shared with ui_app_companion.c */
struct ui_avatar;
