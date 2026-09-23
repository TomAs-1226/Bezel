/* ui — Catalyst Tab's screens, built on Bezel (bz_ui, bz_theme) and fed by the Catalyst model. */
#pragma once
#include "nt4.h"

typedef struct {
    nt4_client_t *nt;
    int team;                  /* 0: read from settings */
    const char *sim_address;   /* the simulator's robot; NULL on the tablet */
} ui_config_t;

void ui_init(const ui_config_t *cfg);
