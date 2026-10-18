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
/* Tone and calm as the settings remember them (the simulator's `mode` and `calm` go through here). */
void ui_set_tone(bool dark, bool calm);
