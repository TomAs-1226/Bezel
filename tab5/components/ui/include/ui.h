/* ui — Catalyst Tab's screens, built on Bezel (bz_ui, bz_theme) and fed by the Catalyst model. */
#pragma once
#include "nt4.h"

#include <stdbool.h>

typedef struct {
    nt4_client_t *nt;
    int team;                  /* 0: read from settings */
    const char *sim_address;   /* the simulator's robot; NULL on the tablet */
} ui_config_t;

void ui_init(const ui_config_t *cfg);

/* Hands the assistant a handler for its AS_T_DEVICE tools — open an app, set brightness and
 * volume, drive the music, work the Home Assistant entities home mode already shows — and the
 * 10 Hz hook that runs them on the UI thread (ui_actions.c). Once, after ui_init. Without it those
 * tools say so rather than failing obscurely, so a build that leaves it out still works. */
void ui_actions_init(void);

/* The assistant writes a document onto the card, into CATOS/DOCS, where the documents app renders it.
 * `name` is a bare file name with no directory parts; the extension is forced to .MD. `markdown` is the
 * whole document. Returns false with a plain-words reason in `why` (which may be NULL): a name that tries
 * to leave the folder, no card, a full card, or nothing to write. Safe to call from a worker thread — it
 * touches the card and nothing else; the island says what happened on the next UI tick. */
bool ui_docs_write(const char *name, const char *markdown, char *why, size_t nwhy);
/* Tone and calm as the settings remember them (the simulator's `mode` and `calm` go through here). */
void ui_set_tone(bool dark, bool calm);
