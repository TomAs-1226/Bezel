/* ui_sys — what the assistants' system tools (ui_actions.c) need from the apps that own the data.
 *
 * Each app keeps its own state and its own store; the tools go through these hooks so a timer the
 * companion starts is the timer app's timer, an alarm it sets is the clock's, a note lands in the notes
 * app. Every hook works whether or not its app has ever been opened (apps are built on first open), runs
 * on the UI thread, and answers in `out` with a sentence for a model. false: it didn't happen, and why. */
#pragma once
#include "ui_internal.h"

/* ui_apps_util.c: the timer. act: countdown (seconds), match (seconds: autonomous 15 or 20), stopwatch,
 * pause, resume, reset, status. */
bool ui_timer_do(const char *act, double seconds, char *out, size_t n);
/* ui_apps_util.c: the notes app's notebook. act: read, append, replace. */
bool ui_notes_do(const char *act, const char *text, char *out, size_t n);
/* ui_apps_util.c: the pre-match checklist. act: status, check, uncheck, reset; item: "3" or words from it. */
bool ui_check_do(const char *act, const char *item, char *out, size_t n);
/* ui_apps_os.c: the clock's alarms. act: list, add (hhmm, days: bit n = tm_wday n, 0 once), on, off, remove
 * (index 1-based). */
bool ui_alarm_do(const char *act, int hhmm, int days, int index, char *out, size_t n);
/* ui_apps_os.c: an event into the calendar (ymd 20260927, hhmm 1430). */
bool ui_calendar_add(int ymd, int hhmm, const char *text, char *out, size_t n);

/* ui_app_assist.c: the memory screen, opened on a pool (0 the technician's, 1 the companion's). */
void ui_memory_open(int pool, lv_obj_t *from);
extern const ui_app_t APP_MEMORY;
/* ui_app_assist.c: the dev console's "assist ask|new|dump" and "memo ..." (ui_shell.c hands them over). */
void ui_assist_dev(const char *cmd);
