/* Home mode: the tablet on a desk stand, not in the pit. A calm full-screen surface of its own — the time,
 * the weather, what's playing (on the PC through Catalyst Link, or from the microSD card), a row of Home
 * Assistant tiles and the companion — in place of Catalyst mode's pages, dock and status band.
 *
 * It is a surface, not an app window: a full-screen box on the content layer over the pages, so an app
 * opened from it (music, the companion, settings) opens over it as usual and closes back to it.
 *
 * WIRING (the shell, ui_shell.c):
 *   ui_home_mode_init(on_change)   once, at the end of ui_init() (after ui_orb_init). It loads the settings,
 *                                  starts the stand watcher and, with "start in home mode" set, enters it.
 *   on_change(active)              called on every enter and exit (and at init when it starts there): the
 *                                  shell hides the dock, the status band and the orb while active, and shows
 *                                  them again after. The island may stay (it's where messages arrive).
 *   ui_home_mode_active()          while true: skip page refreshes (the pages are hidden under it), keep
 *                                  the orb hidden when an app closes (windows_frame re-shows it today),
 *                                  and don't start a page swipe.
 *   ui_home_mode_keeps_awake()     like ui_companion_keeps_awake(): in home mode on external power the
 *                                  screen only dims, it doesn't go off.
 *   ui_home_mode_enter()/exit()    for a control-center toggle ("home" / "catalyst").
 *
 * Triggers: by hand (the app library's "home mode", Settings > home, the "catalyst" button to leave), at
 * boot (a setting), on the stand (upright, still and on external power for a minute, untouched; it leaves
 * again when lifted off or unplugged), the NFC tag (an M5Stack Unit RFID 2 on Port A seeing the tag paired in
 * Settings > home: ui_home_mode_trigger(UI_HOME_BY_TAG, true) on arrival, (..., false) 1.5 s after it goes),
 * and ui_home_mode_trigger() for anything added later. */
#pragma once
#include "ui_internal.h"

typedef enum {
    UI_HOME_BY_HAND,   /* a button: enter/exit exactly as asked */
    UI_HOME_BY_BOOT,   /* "start in home mode" */
    UI_HOME_BY_STAND,  /* the stand watcher (IMU + power) */
    UI_HOME_BY_TAG,    /* an external sensor: NFC tag in the stand, a dock contact, a magnet switch */
} ui_home_why_t;

typedef void (*ui_home_mode_fn)(bool active);

void ui_home_mode_init(ui_home_mode_fn on_change);
void ui_home_mode_enter(void);
void ui_home_mode_exit(void);
bool ui_home_mode_active(void);
bool ui_home_mode_keeps_awake(void);
/* An automatic trigger: `present` true asks for home mode, false says the reason went away (home mode then
 * leaves only if that same trigger brought it). UI thread only. */
void ui_home_mode_trigger(ui_home_why_t why, bool present);

/* Settings > home: built into the settings pane `pane` (a flex column; it takes the rest of it), keyboard
 * on `body`. _open() when the settings app opens; _wanted() says the settings should open on this section
 * (true once after "set up" was tapped in home mode). */
void ui_home_settings(lv_obj_t *pane, lv_obj_t *body, int w);
void ui_home_settings_open(void);
bool ui_home_settings_wanted(void);

/* The app library's "home mode" tile. */
void ui_home_mode_tap(lv_obj_t *o, void *u);

/* Home mode's own apps. Music (ui_app_music.c): the microSD card's songs, and the PC's player. Smart home
 * (ui_app_smarthome.c): every picked Home Assistant entity by room, with toggles, light levels and set points.
 * Weather (ui_app_weather.c): now, the next 24 hours and the week, from Open-Meteo. */
extern const ui_app_t APP_MUSIC, APP_SMARTHOME, APP_WEATHER;
