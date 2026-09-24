/* Home mode's settings, shared by the surface (ui_home_mode.c), its settings pane (ui_home_settings.c) and
 * the music app (ui_app_music.c). Private to the ui component. */
#pragma once
#include "home.h"
#include "ui_home_mode.h"

typedef struct {
    bool start_home;       /* "hm_start": the tablet starts in home mode */
    bool stand;            /* "hm_stand": home mode by itself on the desk stand */
    bool fahrenheit;       /* "hm_fahr" */
    bool pc;               /* "hm_pc": the PC's media through Catalyst Link */
    char place[64];        /* "hm_place": "47.61,-122.33" or "Seattle" */
    char ha_url[128];      /* "ha_url" */
    char ha_token[320];    /* "ha_token": never logged, never shown */
    char picks[HOME_HA_PICKS * 64 + 8]; /* "ha_picks": entity ids, comma separated */
} hm_cfg_t;

hm_cfg_t *hm_cfg(void);    /* loaded from the kv store on first use (UI thread) */
void hm_cfg_save(void);    /* writes it back and hands it to the services (UI thread: it writes NVS) */
void hm_cfg_apply(void);   /* hands it to the services without writing */
void hm_settings_request(void); /* the next settings open lands on the home section */

/* "3:07" (or "1:02:03"); "-:--" when unknown */
void hm_fmt_time(double s, char *out, size_t n);
