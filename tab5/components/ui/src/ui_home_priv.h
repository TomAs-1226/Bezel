/* Home mode's settings, shared by the surface (ui_home_mode.c), its settings pane (ui_home_settings.c), its
 * apps (music, smart home, weather) and the NFC tag. Private to the ui component. */
#pragma once
#include "home.h"
#include "ui_home_mode.h"

typedef struct {
    bool start_home;       /* "hm_start": the tablet starts in home mode */
    bool stand;            /* "hm_stand": home mode by itself on the desk stand */
    bool fahrenheit;       /* "hm_fahr" */
    bool pc;               /* "hm_pc": the PC's media through Catalyst Link */
    int saver_min;         /* "hm_saver": the card's photos after this many minutes untouched; 0 off */
    char tag[24];          /* "hm_tag": the NFC tag's UID (hex) that brings home mode; "" none */
    char place[64];        /* "hm_place": "47.61,-122.33" or "Seattle" */
    char ha_url[128];      /* "ha_url" */
    char ha_token[320];    /* "ha_token": never logged, never shown */
    char picks[HOME_HA_PICKS * 64 + 8]; /* "ha_picks": entity ids, comma separated */
} hm_cfg_t;

/* the picks home mode itself shows (the smart home app shows them all) */
#define HM_HOME_TILES 4

hm_cfg_t *hm_cfg(void);    /* loaded from the kv store on first use (UI thread) */
void hm_cfg_save(void);    /* writes it back and hands it to the services (UI thread: it writes NVS) */
void hm_cfg_apply(void);   /* hands it to the services without writing */
void hm_settings_request(void); /* the next settings open lands on the home section */
void hm_open_settings(lv_obj_t *from); /* the settings app, on its home section */

/* "3:07" (or "1:02:03"); "-:--" when unknown */
void hm_fmt_time(double s, char *out, size_t n);
/* The glyph for a WMO weather code (the icon font has sun, moon, bolt and a thermometer, no clouds). */
const char *hm_wx_icon(int code, bool day);
/* A Home Assistant entity's glyph and its state in a few words ("on", "21.5 °C", "tap"). */
const char *hm_ha_icon(home_ha_kind_t k, const char *unit);
void hm_ha_state_text(const home_ha_entity_t *e, char *out, size_t n);

/* The NFC tag (a Unit RFID 2 on Port A; hal_nfc_*): the next tag the reader sees becomes home mode's. */
void hm_tag_pair_begin(void);
void hm_tag_pair_cancel(void);
/* 0 nothing going on, 1 waiting for a tag, 2 a tag was just paired (once), -1 no reader */
int hm_tag_pair_state(void);
