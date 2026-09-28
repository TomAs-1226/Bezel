/* ui_intel — the intelligence core's side in the UI: the rules' pass, the island, the insights app with its
 * ask bar, the control center's ask tile, and the few lines other screens borrow. Private to components/ui. */
#pragma once
#include "intel.h"
#include "ui_internal.h"

/* Once, at start-up (ui_match_boot calls it): the sources configured, the 10 Hz hook registered. */
void ui_intel_boot(void);
/* The insights app; ui_intel_ask opens it with the ask bar's keyboard up. */
extern const ui_app_t APP_INTEL;
void ui_intel_ask(lv_obj_t *from);
/* The insights app on a view: 0 now, 1 the briefing (phone mode), 2 sources, 3 settings. */
void ui_intel_open_view(int view, lv_obj_t *from);
/* Settings for the core, into a flex column (the insights app's settings view). */
void ui_intel_settings(lv_obj_t *col, int w);
/* The control center's ask tile (ui_cc_init asks for it). */
void ui_intel_cc_add(void);
/* One line for the home page and home mode: the most important insight worth a glance (prio 2 or more, or the
 * next match's prep line), "" none. Cheap: a copy of the last pass. */
bool ui_intel_line(char *out, size_t n);
/* The dev console's "assist intel ..." and "assist nexus ...". */
void ui_intel_dev(const char *args);

/* ui_match.c: our tracked matches to come, soonest first, as the rules see them (Nexus's times first); how far
 * behind the schedule the field runs by Nexus's estimate (minutes, 0 unknown). */
int ui_match_upcoming(intel_up_t *out, int max, int *behind_min);
/* ui_match.c: Nexus's key, base URL and event read again from kv (the dev console changed one). */
void ui_match_nexus_config(void);
/* ui_app_assist.c: a question to the technician (its app opened). */
void ui_assist_ask(const char *q, lv_obj_t *from);
