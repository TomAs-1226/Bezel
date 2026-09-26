/* ui_md — the one renderer for everything a model writes.
 *
 * The assistant, the companion's answers and the documents app all draw the same way: a block of
 * markdown becomes a column of LVGL objects — headings, paragraphs with **bold** and `code` runs, fenced
 * code, bullet and numbered lists, rules — rather than a single label with the marks scrubbed off it.
 *
 * Two fenced languages are more than text. ```chart draws an lv_chart from a line-oriented syntax small
 * enough for a model to emit inside an answer (docs/render-blocks.md is the contract, and the assistant's
 * system prompt teaches it). ```svg draws the subset of SVG this tablet can honestly rasterise; anything
 * outside the subset is reported, never silently half-drawn.
 *
 * Everything here runs on the UI thread and allocates from PSRAM through LVGL. */
#pragma once
#include "lvgl.h"

#include <stddef.h>

/* Renders `md` into `parent` as a column `width` px wide, and returns the height it used.
 *
 * `parent` is emptied first, given a vertical flex layout and sized to the content, so a caller can put
 * it in a scroller and forget about it. Returns 0 for empty input. A malformed chart or svg block becomes
 * a visible card saying what is wrong with it — a wrong drawing is worse than no drawing. */
int ui_md_render(lv_obj_t *parent, const char *md, int width);

/* True when `md` holds anything markdown would render differently from plain text: a caller that only
 * wants the cost when it buys something (a one-line answer does not) can ask first. */
bool ui_md_is_rich(const char *md);

/* ---- the pieces, for a caller that lays out its own column ---- */

/* One ```chart block's body (without the fences). Draws into a new chart under `parent`, `width` px wide,
 * and returns it; NULL when the block is malformed, with the reason in `why`. */
lv_obj_t *ui_md_chart(lv_obj_t *parent, const char *body, int width, char *why, size_t nwhy);

/* One ```svg block's body, or the contents of an .svg file. Draws the supported subset into a canvas
 * under `parent`, scaled to fit within `max_w` × `max_h`, and returns the image object.
 *
 * Supported: <svg viewBox|width|height>, <g> with transform translate/scale/rotate/matrix, <line>,
 * <rect> (incl. rx/ry), <circle>, <ellipse>, <polyline>, <polygon>, <path> with M m L l H h V v C c S s
 * Q q T t Z z, <text>, and the presentation attributes fill, stroke, stroke-width, opacity,
 * font-size and text-anchor. Colours may be #rgb, #rrggbb, none, or the basic CSS colour names.
 *
 * NOT supported, and each one is reported rather than skipped silently: elliptical arcs in path data
 * (A/a), gradients and patterns, clip paths and masks, filters, dash patterns, stroke joins and mitres
 * beyond a plain round join, CSS <style> blocks and class selectors, <use>/<defs>/<symbol>, images, and
 * text on a path. `why` (may be NULL) is filled either way: with the reason nothing was drawn, or with a
 * note naming what was left out of a drawing that did appear. Returns NULL only when nothing at all
 * could be drawn. */
lv_obj_t *ui_md_svg(lv_obj_t *parent, const char *svg, int max_w, int max_h, char *why, size_t nwhy);
