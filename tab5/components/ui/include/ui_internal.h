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

/* The frame every screen is laid out in. 1280x720 normally; 720x1280 in phone mode, which the owner turns
 * on in the control center and which takes effect on the next start (ui_phone_set). It never changes while
 * the firmware runs, so a layout may read it as a constant — but not in a constant expression. */
#define W hal_frame_w
#define H hal_frame_h
/* Portrait: for the screens that lay themselves out differently rather than just narrower. */
#define PORTRAIT (H > W)
#define PAD BZ_PAD_PAGE
/* The frame, top to bottom: the status band (page title and context on the left; link, battery and clock on
 * the right, then the island's orb in the corner), the body, and the dock. */
#define STATUS_H 76        /* the status band */
#define HEAD_Y 20          /* the page title's top, inside the band */
/* The island at rest: an orb in the top-right corner, over pages, apps and home mode alike. Whatever sits at
 * the band's right (the status cluster, an app head's buttons) ends ORB_CLEAR short of the page's right
 * padding: align it LV_ALIGN_TOP_RIGHT at HEAD_RIGHT_X. A message grows out of the orb leftwards for a few
 * seconds, over whatever is there, and shrinks back into it. */
#define ORB_D 48
#define ORB_Y 20
#define ORB_CLEAR (ORB_D + 16)
#define HEAD_RIGHT_X (-(PAD + ORB_CLEAR))
#define STATUS_W 240       /* the status cluster, at its widest */
#define BODY_Y 88          /* page bodies start under the band */
#define DOCK_BOTTOM 14     /* the dock's gap to the bottom edge */
#define DOCK_H 92          /* the dock's height: anything placing itself clear of it needs this */
#define DOCK_CLEAR 118     /* the dock's top edge from the bottom, and a gap: 14 + 92 + 12 */
#define BODY_BOTTOM (H - DOCK_CLEAR) /* 602: where a page's tiles end */

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
    int accent;            /* BZ_ACCENTS: the system's accent (kv "accent", by name) */
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
/* Phone mode: the whole interface laid out in the panel's own portrait frame (720x1280) instead of
 * landscape. It is the owner's choice of FRAME; auto-rotate is which way up that frame is held, and the two
 * are independent — phone mode never reads or writes S.auto_rotate. Because every screen is built once at
 * start-up from W and H, the choice takes effect on the next start: ui_phone_set() saves it, says so, and
 * restarts the tablet. hal_phone() is what this start was built as. */
void ui_phone_set(bool on);
void ui_set_accent(int accent); /* the accent everywhere (one full redraw), saved */
void ui_apply_addresses(void);

/* Called at 10 Hz after the model updates. Pages and apps register one each. */
typedef void (*ui_refresh_fn)(void *user);
void ui_on_refresh(ui_refresh_fn fn, void *user);
/* A page's refresh: runs only while that page is on screen (and once when it comes back). */
void ui_on_page_refresh(int page, ui_refresh_fn fn, void *user);

/* The island: an orb in the top-right corner, the robot's state in its colour and mark, a pip while there are
 * notifications not yet seen; a tap opens the control center. A message (kept as a notification) flashes it
 * amber and grows out of it as a capsule for ~3 s, then shrinks back in (Bezel's top island). */
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
/* The library (ui_pages2.c): every app in its order, with the label and hint it shows there. The home mode tile
 * is in it too (its app has no build: open it with ui_app_launch, not ui_app_open). */
typedef struct { const ui_app_t *app; const char *label; const char *hint; int group; } ui_app_entry_t;
int ui_app_count(void);
const ui_app_entry_t *ui_app_entry(int i);
const char *ui_app_label(const ui_app_t *app); /* the library's name for it, or NULL */
void ui_app_launch(const ui_app_t *app, lv_obj_t *from); /* ui_app_open, or home mode for its tile */
/* A launcher cell `w` wide: the app's icon (`icon` px) over its name, and the hint under it (NULL: none); a press
 * shows a platter; a tap launches it, the window growing out of the icon. */
lv_obj_t *ui_app_cell(lv_obj_t *parent, const ui_app_t *app, const char *label, const char *hint, int w, int icon);

/* App icons (ui_icon.c): each app a squircle of its own colour with its glyph, lit from the top left, baked once
 * per size into a small PSRAM picture that every place showing it shares. The style is settings > look > icons:
 * colour, tinted (the accent's tonal container, Material You's themed icons) or mono (surface and ink). */
enum { UI_ICON_COLOUR, UI_ICON_TINTED, UI_ICON_MONO, UI_ICON_NSTYLES };
lv_obj_t *ui_app_icon(lv_obj_t *parent, const ui_app_t *app, int size); /* an lv_image, size x size */
/* By library label or window name ("music", "home mode"); NULL when there is no such icon. */
lv_obj_t *ui_icon_named(lv_obj_t *parent, const char *name, int size);
/* One app's icon in a given style whatever the setting (settings' style chooser). */
lv_obj_t *ui_icon_preview(lv_obj_t *parent, const char *name, int size, int style);
const lv_image_dsc_t *ui_icon_image(const ui_app_t *app, int size);
int ui_icon_style(void);
void ui_icon_set_style(int style); /* saved (kv "icons"), every icon baked again, one redraw */
size_t ui_icon_bytes(void);

/* The look preferences (ui_look.c, settings > look), read once from kv. */
#define UI_QUICK_MAX 6
enum { UI_GROUND_PLAIN, UI_GROUND_GLOW, UI_GROUND_AURORA, UI_GROUND_GRAIN, UI_GROUND_CONTOUR, UI_GROUND_COUNT };
typedef struct {
    bool h24;          /* kv clock24: 20:46, or 8:46 */
    bool batt_pct;     /* kv battpct: the status band's battery percent */
    bool dock_labels;  /* kv docklbl: the dock's words under its icons */
    char quick[UI_QUICK_MAX][16]; /* kv quick: the home page's quick-launch row, library labels, in order */
    int nquick;
    unsigned quick_gen; /* moves whenever the quick-launch apps change */
    bool tone_auto;    /* kv toneauto: light from 7 am, dark from 7 pm */
    int ground;        /* kv ground: UI_GROUND_* */
} ui_look_t;
const ui_look_t *ui_look(void);
ui_look_t *ui_look_edit(void); /* change, then ui_look_save() and apply */
void ui_look_save(void);
bool ui_look_is_quick(const char *label);
void ui_look_dev(const char *args); /* the dev console's "look <what> <value>" */
/* With the tone on its schedule, the tone the hour asks for (checked twice a minute; `now`: at once). */
void ui_look_tone_tick(bool now);
void ui_look_boot(void); /* once the pages are built: the ground, and the tone's schedule */
/* Settings > look's own choices (icons, ground, quick launch, status band and dock), into its scrolling column. */
void ui_look_settings(lv_obj_t *col, int w);
void ui_look_settings_open(void); /* the choices as they are now, when settings opens */
/* "20:46" or "8:46", as the owner reads the clock */
void ui_fmt_hm(char *out, size_t n, const struct tm *tm);
/* Who shows a look preference applies it when told (the settings pane calls these after a change). */
void ui_home_quick_changed(void);     /* ui_home.c: the quick-launch row laid out again */
void ui_status_look_changed(void);    /* ui_shell.c: the status band's clock and battery, the dock's labels */

/* The ground under the pages (ui_ground.c): plain or a picture baked from the tone and the accent. Lists that
 * scroll keep an opaque surface over it: a list moves pixels already drawn. */
void ui_ground_init(void);            /* once the pages are built */
void ui_ground_set(int ground);       /* saved, baked, one redraw */
bool ui_ground_active(void);
const char *ui_ground_name(int ground);
void ui_ground_thumb(int ground, uint16_t *buf, int w, int h); /* a chooser's picture of it */
/* The same ground behind another full-screen surface (the lock screen): layers at the back of `parent`, sharing the
 * baked pictures, kept in step with every change. At most three. */
lv_obj_t *ui_ground_layer(lv_obj_t *parent);
size_t ui_ground_bytes(void);

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
/* The settings app opened on a section by its name ("look", "display", "home"...): the dev console's
 * "settings <section>". */
void ui_settings_show(const char *section);
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
/* The same, choosing what: 0 the queue reminder, 1 the match reminder, 2 a schedule change's chime and message. */
void ui_match_test_kind(double delay_s, int kind);
void ui_match_fake(double minutes); /* dev console "match fake N": a made-up match of ours N min out; 0 removes it */
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

/* ---- the control center: pulled down from the top edge ----
 *
 * Its contents are a registry. A control says what it is called, its icon, how much of the sheet's grid it
 * takes and what it does; the control center lays out whatever is registered and knows nothing about any
 * particular one. Adding a control is a static ui_cc_control_t and one ui_cc_add() call, from any file.
 *
 * The grid is eight columns across the landscape frame and four across phone mode's, so the same controls
 * reflow rather than needing a second layout. Controls are placed in the order registered, each in the
 * first free cell it fits; a row is as tall as the tallest control that occupies exactly one of it; and
 * whatever the rows leave over goes to the one control marked `grow` (the notifications).
 *
 * ui_cc_add() may be called any time before ui_cc_init(); a control added then sits with the toggles,
 * after the system's own and before sleep, power and the notifications. */
typedef enum {
    UI_CC_TOGGLE, /* a round glass tile that lights when on() says so; tap() does it */
    UI_CC_LEVEL,  /* a Bezel level, get() and set() its value */
    UI_CC_PANEL,  /* a glass module build() fills */
    UI_CC_TILE,   /* the same on a solid tonal surface, for content rather than a control */
} ui_cc_kind_t;

typedef struct {
    const char *name;     /* under a toggle's icon, and what the dev console calls it: one short word */
    const char *icon;
    ui_cc_kind_t kind;
    int cols, rows;       /* grid cells; cols is clamped to the sheet's width */
    int h;                /* a one-row control's own height, centred in its row; 0: the row's */
    bool grow;            /* takes the height the rows left over (one control only) */
    bool (*avail)(void);  /* NULL, or false to leave it out of this build's sheet entirely */
    bool (*on)(void);                         /* TOGGLE */
    void (*tap)(void);                        /* TOGGLE */
    float (*get)(void);                       /* LEVEL */
    void (*set)(float v, bool final);         /* LEVEL */
    void (*build)(lv_obj_t *m, int w, int h); /* PANEL, TILE: once */
    void (*refresh)(lv_obj_t *m);             /* PANEL, TILE: at 10 Hz while the sheet is open */
} ui_cc_control_t;

void ui_cc_add(const ui_cc_control_t *c); /* the struct must outlive the call: a static one */
/* Controls that belong to another file, asked for their registration as the sheet is built. */
void ui_light_cc_add(void);     /* ui_apps_util.c: the flashlight */
void ui_home_mode_cc_add(void); /* ui_home_mode.c: home mode */

void ui_cc_init(void);
/* The assistant's orb, over every screen (ui_app_assist.c). */
void ui_orb_init(void);
void ui_cc_open(void);
void ui_cc_close(void);         /* put it away (a control that opens an app) */
/* Put it away and then do this, once the sheet is gone: a control that opens an app or a surface, which
 * wants the screen to itself rather than a second sheet sliding over the first. */
void ui_cc_after_close(void (*fn)(void));
bool ui_cc_is_open(void);       /* the control center, as if pulled down */
void ui_orb_show(bool show); /* the orb floats over the pages; an open app takes its corner */
/* Starts the state recorder, so the states app has a timeline from boot (ui_apps_sc.c). */
void ui_sc_boot(void);

/* Helpers. */
int ui_head_width(const char *title); /* room for context beside a page title, before the status cluster */
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
/* An empty state: a tile `w` wide that grows with its words, a title (name face) over a caption, 12 px apart.
 * One look for every "not connected" / "nothing yet" in the robot's pages and apps (ui_pages.c). */
lv_obj_t *ui_empty(lv_obj_t *parent, int w, const char *title, const char *text);
/* The same with its own glyph in the disc (NULL: none). Across an app's whole width it fills the body and centres;
 * ui_empty_row keeps the row (a card among others). */
lv_obj_t *ui_empty_icon(lv_obj_t *parent, int w, const char *icon, const char *title, const char *text);
lv_obj_t *ui_empty_row(lv_obj_t *parent, int w, const char *icon, const char *title, const char *text);
/* No card of its own, for a surface that is one already: centred in w x h. */
lv_obj_t *ui_empty_center(lv_obj_t *parent, int w, int h, const char *icon, const char *title, const char *text);
/* A vertical scroller: returns the content container; drags coast and rubber-band at the ends. */
lv_obj_t *ui_scroller(lv_obj_t *parent, int w, int h);
/* Brings a scroller's end into view on the `smooth` spring (a transcript following its newest line).
 * Returns false, and does nothing, while a finger holds it or when it's been scrolled away from the end
 * (the reader is looking back). */
bool ui_scroller_follow(lv_obj_t *content);
/* Back to the top at once (a page's list, reset while the page is off screen). */
void ui_scroller_reset(lv_obj_t *content);

/* A keyboard sheet over the bottom of an app, for one entry at a time. */
typedef struct ui_kb ui_kb_t;
typedef void (*ui_kb_done_fn)(const char *text, void *user);
ui_kb_t *ui_kb_create(lv_obj_t *body, int h);
void ui_kb_show(ui_kb_t *k, const char *title, const char *text, bool secret, bool one_line, ui_kb_done_fn done,
                void *user);
bool ui_kb_open(const ui_kb_t *k);
void ui_kb_hide(ui_kb_t *k);
void ui_kb_dismiss(void); /* whoever is typing, put the keyboard away (the screen changed under them) */
