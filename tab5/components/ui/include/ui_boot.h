/* ui_boot — the Catalyst start-up screen.
 *
 * The Catalyst family's banner (FrcCatalyst's and Catalyst Console's docs/assets/banner.svg), drawn full
 * screen and brought in piece by piece while the tablet starts: the diagonal ground with its crimson key
 * light and white rim light, the bolt, "Catalyst Tab" in light grey and bold white, the hairline, the
 * tagline, the version pill. On the right, the family's line — here telemetry — draws itself toward a
 * Tab5 as start-up progresses, and when everything is ready it arrives, the tablet lights, and the
 * whole card fades to the ground the UI starts on.
 *
 * It draws straight into an RGB565 buffer with its own small rasterizer (no LVGL, so it runs on one core
 * while the other builds the UI) and says where it drew each frame, so only that is presented.
 *
 * Threads: ui_boot_frame() on the task that presents; ui_boot_set()/ui_boot_finish() on the task that
 * starts the tablet (the one that owns LVGL — glyph layout happens there). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bz_comp.h"
#include "hal.h"

#define CATALYST_TAB_VERSION "1.0.0"

typedef struct ui_boot ui_boot_t;

/* `buf` is w×h RGB565 the animation owns until ui_boot_frame() returns false. `prev` (may be NULL) is
 * how the last start ended: a failed one is written under the mark, in amber. */
ui_boot_t *ui_boot_create(uint16_t *buf, int w, int h, const hal_boot_t *prev, bool safe_mode);
/* Progress 0..1 and what's happening, in a few lowercase words. */
void ui_boot_set(ui_boot_t *b, float progress, const char *status);
/* Everything's ready: the progress arc completes and the mark clears (never before the intro ends). */
void ui_boot_finish(ui_boot_t *b);
/* Draws the frame at `t` seconds since the start into the buffer; `damage` is where it drew. false once
 * the outro has finished and the buffer is plain ground (that last frame is still drawn and damaged). */
bool ui_boot_frame(ui_boot_t *b, double t, bz_area_t *damage);
/* The same frame's damage as separate boxes, up to `max` of them, for a presenter that takes a list:
 * `damage` above spans all of them, and most of that span didn't change. */
int ui_boot_damage(const ui_boot_t *b, bz_area_t *out, int max);
void ui_boot_destroy(ui_boot_t *b);
