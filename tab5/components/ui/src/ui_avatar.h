/* ui_avatar — a face on a blob: the companion and the pet.
 *
 * One object that draws itself. The body is baked once, at the size it is shown, into two small alpha
 * masks (the body's silhouette, and a soft light on its upper left) that LVGL colours as it blits, so a body
 * is about a byte a pixel, takes its colour from whatever the colour is now, and costs nothing to hold still.
 * Per frame only the eyes (and the ears, when the head turns) change, and only the rectangles they covered
 * and now cover are invalidated. Ambient motion (blinks, saccades, a slow drift) is capped at 20 fps, and at
 * 10 fps while it is calm; when nothing moves nothing is drawn.
 *
 * The character is data (ui_avatar_def.h): a Bible Strong `.avatar.json` on the card, or the built-in one.
 * What the app means ("hungry", "listening", "petted") is mapped to whatever the character defines in one
 * table (ui_avatar.c, MAP): its animation of that name, else its expression, else ours borrowed. */
#pragma once
#include "lvgl.h"

#include "ui_avatar_def.h"

typedef struct ui_avatar ui_avatar_t;

/* What the app means. The base state is held (looping, or settled on its last step); a reaction is played
 * once and the base state returns. */
typedef enum {
    AV_S_IDLE, AV_S_QUIET, AV_S_LISTEN, AV_S_THINK, AV_S_SPEAK, AV_S_HAPPY, AV_S_CONFUSED, AV_S_SURPRISED,
    AV_S_SLEEP, AV_S_WAKE, AV_S_DROWSY, AV_S_SAD, AV_S_HUNGRY, AV_S_EAT, AV_S_PETTED, AV_S_LOVE, AV_S_FED,
    AV_S_CURIOUS, AV_S_COUNT
} av_state_t;
extern const char *const AV_STATE_NAME[AV_S_COUNT];

/* A square box `size` px across (the format's 300-unit box); put it where you like. */
ui_avatar_t *ui_avatar_create(lv_obj_t *parent, int size);
lv_obj_t *ui_avatar_obj(ui_avatar_t *a);
/* Shown and running: bakes the body on the way in, gives it back on the way out. Call when the page it is on
 * shows or hides; a hidden avatar is never drawn or ticked. */
void ui_avatar_active(ui_avatar_t *a, bool on);
/* Every frame while active. True while anything is moving (the caller keeps the loop awake). */
bool ui_avatar_frame(ui_avatar_t *a);

void ui_avatar_state(ui_avatar_t *a, av_state_t st);
void ui_avatar_react(ui_avatar_t *a, av_state_t st);
/* A named expression held as it is (the dev console's contact sheet); NULL goes back to the state. */
bool ui_avatar_show_expr(ui_avatar_t *a, const char *name);
/* The head and eyes turn toward (x, y) in -1..1 (down positive) for `hold` s, then come back. */
void ui_avatar_look(ui_avatar_t *a, float x, float y, float hold);
void ui_avatar_level(ui_avatar_t *a, float level); /* the voice's loudness, 0..1: the eyes bob with it */
void ui_avatar_calm(ui_avatar_t *a, float calm);   /* 0 lively .. 1 as still as it gets */
void ui_avatar_tint(ui_avatar_t *a, uint32_t rgb); /* the built-in character's body colour (0: the accent) */
void ui_avatar_dim(ui_avatar_t *a, float dim);     /* 0..0.9: every colour that far toward the ground's */
bool ui_avatar_reacting(ui_avatar_t *a);

/* ---- the character, shared by every avatar ---- */
/* Loads <path> (a Bible Strong .avatar.json); on failure keeps what there was and says why in `why`. */
bool ui_avatar_load(const char *path, char *why, size_t wn);
/* Called now and then (the 10 Hz refresh): loads the file when it appears or changes, and goes back to the
 * built-in character when it is gone. One line is logged each time. */
void ui_avatar_poll(const char *path);
const char *ui_avatar_name(void);   /* "Tabi", "built-in" */
bool ui_avatar_custom(void);
size_t ui_avatar_bytes(void);       /* what the baked bodies hold, for the dev console */
const av_def_t *ui_avatar_def(void);
/* which of the app's meanings the character answers with its own animation / expression / ours */
void ui_avatar_dump(void);
