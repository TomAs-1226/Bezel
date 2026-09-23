/* Shared plumbing for Catalyst Tab's screens: the robot model, refresh hooks, the island, app windows. */
#pragma once
#include "bz_theme.h"
#include "bz_ui.h"
#include "cat_model.h"
#include "hal.h"
#include "ui.h"

#include <stdbool.h>
#include <stdio.h>

#define W HAL_W
#define H HAL_H
#define PAD BZ_PAD_PAGE
#define HEAD_Y 30          /* page heads start here; the island floats at 18 */
#define BODY_Y 104         /* page bodies start under the head */
#define DOCK_CLEAR 132     /* the dock's top edge from the bottom: 30 + 92 + 10 */

/* The robot as of the last model update (10 Hz), and the tablet's own state. */
extern cat_robot_t *R;
typedef struct {
    int team;
    char address[64];      /* override: tried first */
    float brightness, volume;
    bool dark, calm;
    char wifi_ssid[33];
} ui_settings_t;
extern ui_settings_t S;
void ui_settings_save(void);
void ui_apply_addresses(void);

/* Called at 10 Hz after the model updates. Pages and apps register one each. */
typedef void (*ui_refresh_fn)(void *user);
void ui_on_refresh(ui_refresh_fn fn, void *user);

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

/* Pages. */
void ui_go(int page);
int ui_page(void);
lv_obj_t *ui_page_body(int page); /* the page's container, W×H, content layer */

void ui_page_overview(lv_obj_t *page);
void ui_page_devices(lv_obj_t *page);
void ui_page_power(lv_obj_t *page);
void ui_page_motion(lv_obj_t *page);
void ui_page_tools(lv_obj_t *page);

/* Apps. */
extern const ui_app_t APP_PREFLIGHT, APP_ALERTS, APP_TUNE, APP_AUTO, APP_FIELD, APP_LISTEN, APP_LEVEL,
    APP_LENS, APP_CANTAP, APP_LOGS, APP_ROBOT, APP_SETTINGS;

/* The control center: pulled down from the top edge. */
void ui_cc_init(void);

/* Helpers. */
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
