/* Shared plumbing for Catalyst Tab's screens: the robot model, refresh hooks, the island, app windows. */
#pragma once
#include "bz_theme.h"
#include "bz_ui.h"
#include "cat_model.h"
#include "hal.h"
#include "ui.h"

#include <time.h>

#include <stdbool.h>
#include <stdio.h>

#define W HAL_W
#define H HAL_H
#define PAD BZ_PAD_PAGE
/* The frame, top to bottom: the status band (page title and context on the left, the island centred,
 * link, battery and clock on the right), the body, and the dock. */
#define STATUS_H 76        /* the status band */
#define HEAD_Y 20          /* the page title's top, inside the band; the island floats at 12 */
#define BODY_Y 88          /* page bodies start under the band */
#define DOCK_BOTTOM 14     /* the dock's gap to the bottom edge */
#define DOCK_CLEAR 118     /* the dock's top edge from the bottom, and a gap: 14 + 92 + 12 */
#define BODY_BOTTOM (H - DOCK_CLEAR) /* 602: where a page's tiles end */
#define ISLAND_HALF 200    /* half the island's widest: page context stays left of W/2 - this */

/* The robot as of the last model update (10 Hz), and the tablet's own state. */
extern cat_robot_t *R;
typedef struct {
    int team;
    char address[64];      /* override: tried first */
    float brightness, volume;
    bool dark, calm;
    bool perf;             /* the frame-time overlay */
    bool auto_rotate;      /* turn the picture to whichever way up the tablet is held */
    bool flip;             /* which way up now (and the fixed choice when auto_rotate is off) */
    int dim_s;             /* the panel dims after this long untouched (0: never) */
    int sleep_s;           /* the screen goes off after this long untouched (0: never); a tap wakes it */
    bool lock;             /* the screen wakes to the lock screen (a push up opens it) */
    bool clicks;           /* a soft tick on taps */
    int tz;                /* index into the time zones settings offers */
    char wifi_ssid[33];
} ui_settings_t;
extern ui_settings_t S;
void ui_settings_save(void);
/* The screen off now (the robot link stays up); the next tap wakes it without pressing anything. */
void ui_sleep_now(void);

/* Notifications: every island message, kept (newest first) for the control center and the lock screen. */
typedef struct {
    const char *icon;
    char text[96];
    time_t at;       /* wall clock, for anything older than a few hours */
    double mono;     /* hal_seconds() when it came */
} ui_note_t;
void ui_notify_add(const char *icon, const char *text);
int ui_notify_count(void);
unsigned ui_notify_gen(void);            /* changes whenever the list does */
const ui_note_t *ui_notify_get(int i);   /* 0 = newest */
void ui_notify_clear(void);
void ui_notify_age(const ui_note_t *n, char *out, size_t len);
/* The lock screen (ui_lock.c): shown as the screen goes to sleep, lifted by a push up. */
void ui_lock_init(void);
void ui_lock_show(void);
void ui_lock_lift(void);   /* gone at once, without the push (an alarm's button opening an app) */
bool ui_locked(void);
bool ui_overlay_up(void);  /* the lock screen or the control center covers the pages */
bool ui_asleep(void);
void ui_set_flip(bool flip); /* turns the picture 180° and redraws everything */
void ui_apply_addresses(void);

/* Called at 10 Hz after the model updates. Pages and apps register one each. */
typedef void (*ui_refresh_fn)(void *user);
void ui_on_refresh(ui_refresh_fn fn, void *user);
/* A page's refresh: runs only while that page is on screen (and once when it comes back). */
void ui_on_page_refresh(int page, ui_refresh_fn fn, void *user);

/* The island: robot status at rest; a message morphs it for 2.4 s (controls.js:284-323). */
void ui_island_say(const char *icon, const char *text);

/* App windows grow out of the icon that opened them and can be dragged down to close. */
typedef struct {
    const char *name;      /* shown on the back pill */
    const char *icon;
    void (*build)(lv_obj_t *body);   /* once, into a W×H container */
    void (*open)(void);              /* each time it opens */
    void (*close)(void);             /* each time it closes */
    void (*refresh)(void);           /* 10 Hz while open */
    void (*frame)(double now, double dt); /* every frame while open */
} ui_app_t;
void ui_app_open(const ui_app_t *app, lv_obj_t *from);
void ui_app_close(void);
bool ui_app_is_open(const ui_app_t *app);
bool ui_app_any_open(void); /* some app is open (or opening) */
const ui_app_t *ui_app_find(const char *name); /* by its library label or window name */

/* Pages. */
void ui_go(int page);
int ui_page(void);
lv_obj_t *ui_page_body(int page); /* the page's container, W×H, content layer */

/* The pages, left to right: the home screen, the robot's four, and the app library. */
enum { PG_HOME, PG_ROBOT, PG_DEVICES, PG_POWER, PG_MOTION, PG_APPS, PG_COUNT };
void ui_page_home(lv_obj_t *page);
void ui_page_overview(lv_obj_t *page);
void ui_page_devices(lv_obj_t *page);
void ui_page_power(lv_obj_t *page);
void ui_page_motion(lv_obj_t *page);
void ui_page_tools(lv_obj_t *page);

/* Apps. */
extern const ui_app_t APP_PREFLIGHT, APP_ALERTS, APP_TUNE, APP_AUTO, APP_FIELD, APP_LEVEL, APP_LENS,
    APP_CANTAP, APP_LOGS, APP_ROBOT, APP_SETTINGS;
/* Systemcore and Catalyst depth (ui_apps_sc.c): the controller's own health through catalyst-agent,
 * every motor's lifetime, mechanism state timelines, the controls manifest, and the run recorder. */
extern const ui_app_t APP_SYSTEMCORE, APP_MOTORS, APP_STATES, APP_CONTROLS, APP_RECORDER;
/* The assistant and the PC (ui_app_assist.c): the AI technician, and Catalyst Link's inbox and patches. */
extern const ui_app_t APP_ASSIST, APP_LINK;
/* Pairing with the PC's Catalyst Link by a code it shows (ui_app_pair.c). */
extern const ui_app_t APP_PAIR;
/* Desk mode (ui_app_companion.c): a face with two eyes, quick questions, and Claude Code on the PC. */
extern const ui_app_t APP_COMPANION;
/* Settings > assistant: the model and its keys, what the orb opens, desk mode, Claude Code reminders.
 * Built into the settings pane `pane` (a flex column; it takes the rest of it), keyboard on `body`. */
void ui_assist_settings(lv_obj_t *pane, lv_obj_t *body, int w);
void ui_assist_settings_open(void);
/* The companion is on screen and the tablet on power: the screen shouldn't go off by itself. */
bool ui_companion_keeps_awake(void);
/* ui_apps_util.c: the tablet's own utilities */
extern const ui_app_t APP_TIMER, APP_CALC, APP_NOTES, APP_CHECK, APP_LIGHT, APP_SYSMON, APP_FILES;
extern const ui_app_t APP_GEAR, APP_RULER, APP_REF; /* ui_apps_shop.c */
/* The Blue Alliance (ui_app_tba.c): the team's event, its next match, standing and the rankings. */
extern const ui_app_t APP_TBA;
/* Match alerts (ui_match.c): alarms before the team's matches from The Blue Alliance, polled in the background,
 * and schedule changes as notifications. ui_match_boot once at start-up, after the lock screen. */
void ui_match_boot(void);
bool ui_alarm_up(void);                          /* the alarm screen covers everything */
void ui_match_test(double delay_s);              /* a made-up match's alarm in delay_s (dev console "alarm test") */
bool ui_match_next_line(char *out, size_t n);    /* "next: Q34 · 14:52 · red with 1234, 5678"; false: none */
void ui_match_settings(lv_obj_t *pane, int w);   /* the tba app's alerts view, into a flex column */
void ui_match_settings_refresh(void);            /* its status line, while shown */
/* The battery fleet (ui_batt.c): the roster and histories in <sd>/CATOS/DATA/batteries.json, the checklist's
 * "which battery goes in", each use's numbers from the logs and the robot, and the one to put in next.
 * ui_batt_boot once at start-up. */
extern const ui_app_t APP_BATT;
void ui_batt_boot(void);
void ui_batt_check_row(lv_obj_t *col, lv_obj_t *body, int w); /* the checklist's battery row and its picker */
void ui_batt_check_refresh(void);                             /* from the checklist's refresh */
void ui_batt_logs_changed(void);                              /* the card's logs may have changed: read new ones */
/* the alarm screen's line: "battery in: #7" (picked for match_label) or "battery: #7 · lowest resistance..." */
bool ui_batt_alarm_line(const char *match_label, char *out, size_t n);
void ui_batt_dev(const char *args);                           /* the dev console's "bms ..." */
/* The logs app straight onto a log's (or a recorder run's) analysis: the recorder's analyze button. */
void ui_logs_analyze(const char *path);
/* Catalyst OS's everyday apps (ui_apps_os.c), on the card's CATOS layout (ui_storage.h). */
extern const ui_app_t APP_CLOCK, APP_CALENDAR, APP_DOCS, APP_PHOTOS, APP_STORAGE;
/* The photos app straight into a full-screen slideshow of the card's pictures (home mode's screensaver); a tap
 * ends it. */
void ui_photos_slideshow(lv_obj_t *from);
/* Loads the alarms and starts the hook that rings them with the clock app closed. Once, at start-up, after
 * the settings (the clock app also calls it, for a shell that doesn't). */
void ui_os_boot(void);

/* The control center: pulled down from the top edge. */
void ui_cc_init(void);
/* The assistant's orb, over every screen (ui_app_assist.c). */
void ui_orb_init(void);
void ui_cc_open(void);
bool ui_cc_is_open(void);       /* the control center, as if pulled down */
void ui_orb_show(bool show); /* the orb floats over the pages; an open app takes its corner */
/* Starts the state recorder, so the states app has a timeline from boot (ui_apps_sc.c). */
void ui_sc_boot(void);

/* Helpers. */
int ui_head_width(const char *title); /* room for context beside a page title, before the island */
lv_obj_t *ui_head(lv_obj_t *page, const char *title, const char *label); /* returns the right-hand row */
/* Sets a label's text only when it changes (a set always invalidates). printf-style. */
void ui_text(lv_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bz_status_t ui_sev_status(cat_sev_t s);
bz_status_t ui_battery_status(double v);
double ui_now(void);
/* Bezel's tonal button: a 56 px capsule on surface2, icon 24 + label. */
lv_obj_t *ui_button(lv_obj_t *parent, const char *icon, const char *text, bz_tap_fn fn, void *user);
/* A chip that is on (ice fill, on-ice ink) or off (surface2). */
lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, bz_tap_fn fn, void *user);
void ui_chip_set(lv_obj_t *chip, bool on);
/* A vertical scroller: returns the content container; drags coast and rubber-band at the ends. */
lv_obj_t *ui_scroller(lv_obj_t *parent, int w, int h);
/* Brings a scroller's end into view on the `smooth` spring (a transcript following its newest line).
 * Returns false, and does nothing, while a finger holds it or when it's been scrolled away from the end
 * (the reader is looking back). */
bool ui_scroller_follow(lv_obj_t *content);

/* A keyboard sheet over the bottom of an app, for one entry at a time. */
typedef struct ui_kb ui_kb_t;
typedef void (*ui_kb_done_fn)(const char *text, void *user);
ui_kb_t *ui_kb_create(lv_obj_t *body, int h);
void ui_kb_show(ui_kb_t *k, const char *title, const char *text, bool secret, bool one_line, ui_kb_done_fn done,
                void *user);
bool ui_kb_open(const ui_kb_t *k);
void ui_kb_hide(ui_kb_t *k);
