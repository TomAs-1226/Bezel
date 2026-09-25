/* The companion's hooks for ui_app_assist.c (the orb and the assistant's settings); private to this component. */
#pragma once
#include "ui_internal.h"

typedef struct {
    bool orb_companion;    /* the orb opens the companion instead of the assistant ("orb_opens") */
    bool desk_auto;        /* opens by itself when the tablet stands still, upright and charging ("desk_auto") */
    int remind_s;          /* repeat a Claude Code reminder this often until it's tapped; 0 once ("cc_remind") */
} ui_companion_cfg_t;

ui_companion_cfg_t *ui_companion_cfg(void);   /* loaded from the kv store on first use */
void ui_companion_cfg_save(void);             /* UI thread only (it writes NVS) */

/* Every frame, whatever is on screen (the orb's frame hook calls it): Claude Code's finish and needs-input
 * events (island, chime, reminders), the chime sequencer, and desk mode's automatic start. */
void ui_companion_tick(double now, double dt);
/* Opens the companion; with `claude` its Claude Code panel is shown. */
void ui_companion_open(bool claude, lv_obj_t *from);
/* Opens the companion listening, as a tap on its face would (home mode's "tap to talk"). */
void ui_companion_talk(lv_obj_t *from);
