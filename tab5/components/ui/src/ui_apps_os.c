/* Catalyst OS's everyday apps: the ones a tablet is expected to have whatever the robot is doing.
 *
 * clock      the time large, three world clocks, and up to five alarms that ring with the app closed
 * calendar   a month at a glance; events in <sd>/CATOS/DATA/EVENTS.TXT, one "YYYY-MM-DD HH:MM text" a line
 * documents  .TXT and .MD files from <sd>/CATOS/DOCS, word-wrapped and scrolling
 * photos     the card's JPEGs (CATOS/PHOTOS and the lens app's snapshots) through the P4's JPEG decoder; also a
 *            full-screen slideshow (ui_photos_slideshow: home mode's screensaver) that skips the thumbnails
 * storage    what the card holds, folder by folder
 *
 * Card I/O runs on hal_thread workers, each handing its result back through a volatile state that its
 * app's refresh polls. App state lives in PSRAM's .bss and anything large in PSRAM heap (malloc routes
 * big blocks there), so none of this costs internal RAM. */
#include "ui_internal.h"
#include "ui_storage.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define OS_BSS EXT_RAM_BSS_ATTR /* zeroed at start, like any .bss: no initialisers */
#else
#define OS_BSS
#endif

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)

static lv_obj_t *head_right(lv_obj_t *body)
{
    lv_obj_t *r = bz_row(body, 14);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(r, 60);
    return r;
}

/* A row of chips that wraps inside `w` rather than running out of its tile. */
static lv_obj_t *chip_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *column_tile(lv_obj_t *b, int x, int w)
{
    lv_obj_t *t = bz_tile(b, w, APP_H);
    lv_obj_set_pos(t, x, APP_Y);
    return t;
}

/* the local time, and whether the clock has been set at all (an unset RTC starts in 1970 or 2000) */
static bool now_tm(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
    return tm->tm_year > 120;
}

/* the worker's writes land before the flag that says they're there */
#define PUBLISH(flag, v) do { __sync_synchronize(); (flag) = (v); } while (0)

/* ------------------------------------------------------------------ a scroller that can start over */

/* ui_scroller's feel (1:1 drag, projected coast, edge spring) for content replaced wholesale, a document
 * or a rescanned grid, which has to start again at the top. The content column sizes to its rows; the
 * app's frame ticks it. */
typedef struct {
    lv_obj_t *clip, *content;
    bz_motion_t y;
    float start;
    bool dragging;
} vs_t;

static float vs_min(vs_t *s)
{
    lv_obj_update_layout(s->content);
    float over = (float)lv_obj_get_height(s->content) - (float)lv_obj_get_height(s->clip);
    return over > 0 ? -over : 0;
}

static void vs_begin(lv_obj_t *o, lv_point_t p, void *u)
{
    (void)o; (void)p;
    vs_t *s = u;
    s->dragging = true;
    s->start = s->y.value;
    bz_motion_set(&s->y, s->y.value, 0);
}

static void vs_move(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)vx;
    vs_t *s = u;
    float y = bz_rubber_clamp(s->start + dy, vs_min(s), 0, (float)lv_obj_get_height(s->clip));
    bz_motion_set(&s->y, y, vy);
    bz_ui_scroll(s->clip, s->content, (int32_t)y);
}

static void vs_end(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dx; (void)dy; (void)vx;
    vs_t *s = u;
    s->dragging = false;
    float lo = vs_min(s), y = s->y.value;
    if (y > 0 || y < lo) {
        bz_motion_to_v(&s->y, y > 0 ? 0 : lo, BZ_EDGE, 0);
        return;
    }
    float aim = y + bz_project(vy, BZ_RATE_NORMAL);
    if (aim > 0 || aim < lo) bz_motion_to_v(&s->y, aim > 0 ? 0 : lo, BZ_EDGE, vy > 5000 ? 5000 : vy < -5000 ? -5000 : vy);
    else bz_motion_to_v(&s->y, aim, BZ_SMOOTH, vy);
}

static void vs_init(vs_t *s, lv_obj_t *parent, int w, int h, int gap)
{
    s->clip = bz_box(parent);
    lv_obj_set_size(s->clip, w, h);
    s->content = bz_col(s->clip, gap);
    lv_obj_set_width(s->content, w);
    bz_motion_init(&s->y, 0, 0.1f);
    s->y.keep = true;
    bz_drag_t d = { .begin = vs_begin, .move = vs_move, .end = vs_end, .user = s, .axis = 2, .slop = 10 };
    bz_drag_attach(s->clip, &d);
    lv_obj_add_flag(s->clip, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void vs_tick(vs_t *s)
{
    if (!s->clip || s->dragging) return;
    if (bz_motion_tick(&s->y)) {
        bz_ui_scroll(s->clip, s->content, (int32_t)s->y.value);
        bz_ui_keep_alive();
    }
}

static void vs_top(vs_t *s)
{
    bz_motion_set(&s->y, 0, 0);
    lv_obj_set_y(s->content, 0);
}

/* ================================================================== clock */

#define NALARM 5
#define RING_S 60       /* an alarm nobody stops gives up after a minute */
#define SNOOZE_MIN 5

typedef struct {
    int hhmm;           /* 0730 */
    bool on;
    uint8_t days;       /* bit n: tm_wday n (0 Sunday); none: rings once, then turns itself off */
} alarm_t;

/* POSIX TZ strings: the tablet's own zone is settings' */
static const struct { const char *city, *tz; } ZONE[] = {
    { "los angeles", "PST8PDT,M3.2.0,M11.1.0" }, { "denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "chicago", "CST6CDT,M3.2.0,M11.1.0" },     { "new york", "EST5EDT,M3.2.0,M11.1.0" },
    { "honolulu", "HST10" },                     { "mexico city", "CST6" },
    { "sao paulo", "<-03>3" },                   { "utc", "UTC0" },
    { "london", "GMT0BST,M3.5.0/1,M10.5.0" },    { "paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "istanbul", "<+03>-3" },                   { "mumbai", "IST-5:30" },
    { "shanghai", "CST-8" },                     { "taipei", "CST-8" },
    { "tokyo", "JST-9" },                        { "sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
#define NZONE (int)(sizeof ZONE / sizeof ZONE[0])
static const char *const DAY2[7] = { "su", "mo", "tu", "we", "th", "fr", "sa" };

static OS_BSS struct {
    /* the alarms: these run whether the app is open or not */
    alarm_t al[NALARM];
    int nal;
    bool booted;
    long last_min;          /* minutes since the epoch, last looked at */
    bool ringing;
    int ring_hhmm;
    double ring_t0, ring_next, say_next;
    int beeps;
    long snooze_at;         /* minutes since the epoch a snooze rings, 0 none */
    int snooze_hhmm;
    bool list_dirty;        /* an alarm changed behind the app's back (a one-off turned itself off) */
    int zone[3];
    /* the app */
    lv_obj_t *time, *secs, *date, *wc_city[3], *wc_time[3], *wc_note[3];
    lv_obj_t *rows[NALARM], *row_time[NALARM], *row_days[NALARM], *row_sw[NALARM], *empty;
    lv_obj_t *edit, *edit_h, *edit_m, *day_chips[7], *ring_box, *ring_time, *add_btn;
    int sel;                /* the alarm being edited, -1 none */
    bool held;              /* a held minute button changed it: saved on release */
    int row_sel;            /* the row drawn selected */
    time_t shown;
    long wc_min;
    bool ring_shown;
} CL;

static void al_save(void)
{
    char v[NALARM * 16 + 1] = "";
    for (int i = 0; i < CL.nal; i++) {
        char one[16];
        snprintf(one, sizeof one, "%s%04d:%d:%d", i ? "," : "", CL.al[i].hhmm, CL.al[i].on, CL.al[i].days);
        strcat(v, one);
    }
    hal_kv_set("alarms", v);
}

static void al_load(void)
{
    char v[NALARM * 16 + 1];
    CL.nal = 0;
    if (!hal_kv_get("alarms", v, sizeof v)) return;
    for (char *p = v; *p && CL.nal < NALARM;) {
        int hhmm, on, days;
        if (sscanf(p, "%d:%d:%d", &hhmm, &on, &days) == 3 && hhmm >= 0 && hhmm < 2400 && hhmm % 100 < 60)
            CL.al[CL.nal++] = (alarm_t){ hhmm, on != 0, (uint8_t)(days & 0x7f) };
        char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
}

static void zones_load(void)
{
    CL.zone[0] = 3, CL.zone[1] = 8, CL.zone[2] = 14; /* new york, london, tokyo */
    char v[24];
    int a, b, c;
    if (hal_kv_get("wclock", v, sizeof v) && sscanf(v, "%d,%d,%d", &a, &b, &c) == 3 && a >= 0 && a < NZONE && b >= 0 &&
        b < NZONE && c >= 0 && c < NZONE)
        CL.zone[0] = a, CL.zone[1] = b, CL.zone[2] = c;
}

static float ring_volume(void) { return S.volume > 0.5f ? S.volume : 0.5f; } /* an alarm is meant to be heard */

static void ring_start(int hhmm)
{
    double now = hal_seconds();
    CL.ringing = true;
    CL.ring_hhmm = hhmm;
    CL.ring_t0 = CL.ring_next = CL.say_next = now;
    CL.beeps = 0;
    bz_ui_wake();
    /* over the pages, the clock comes up with its stop button; over another app, the island and the tone
     * say it, so nothing half-typed is closed under someone */
    if (!ui_app_any_open()) ui_app_open(&APP_CLOCK, NULL);
}

static void ring_stop(void) { CL.ringing = false; }

/* The alarms' 10 Hz hook: once a minute it looks for one due; while one rings, the tone and the island. */
static void alarm_tick(void *u)
{
    (void)u;
    time_t t = time(NULL);
    long m = (long)(t / 60);
    if (m != CL.last_min) {
        CL.last_min = m;
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_year > 120) {
            int hhmm = tm.tm_hour * 100 + tm.tm_min;
            for (int i = 0; i < CL.nal; i++) {
                alarm_t *a = &CL.al[i];
                if (!a->on || a->hhmm != hhmm || (a->days && !(a->days >> tm.tm_wday & 1))) continue;
                if (!a->days) {
                    a->on = false;
                    al_save();
                    CL.list_dirty = true;
                }
                ring_start(a->hhmm);
            }
            if (CL.snooze_at && m >= CL.snooze_at) {
                CL.snooze_at = 0;
                ring_start(CL.snooze_hhmm);
            }
        }
    }
    if (!CL.ringing) return;
    double now = hal_seconds();
    if (now - CL.ring_t0 > RING_S) {
        ring_stop();
        ui_island_say(BZ_I_NOTIFICATIONS, "alarm stopped after a minute");
        return;
    }
    if (now >= CL.ring_next) {
        /* two quick notes, then a rest */
        int k = CL.beeps++ % 3;
        if (k < 2) hal_tone(k ? 1175 : 880, 140, ring_volume());
        CL.ring_next = now + (k < 2 ? 0.22 : 0.6);
    }
    if (now >= CL.say_next) {
        char msg[48];
        snprintf(msg, sizeof msg, "alarm %02d:%02d · stop it in clock", CL.ring_hhmm / 100, CL.ring_hhmm % 100);
        ui_island_say(BZ_I_NOTIFICATIONS, msg);
        CL.say_next = now + 3;
    }
}

void ui_os_boot(void)
{
    if (CL.booted) return;
    CL.booted = true;
    CL.sel = CL.row_sel = -1;
    al_load();
    zones_load();
    ui_on_refresh(alarm_tick, NULL);
}

static const char *days_text(uint8_t d, char *b, size_t n)
{
    if (!d) return "once";
    if (d == 0x7f) return "every day";
    if (d == 0x3e) return "weekdays";
    if (d == 0x41) return "weekends";
    b[0] = 0;
    for (int i = 0; i < 7; i++)
        if (d >> i & 1) {
            if (b[0] && strlen(b) + 1 < n) strcat(b, " ");
            if (strlen(b) + 2 < n) strcat(b, DAY2[i]);
        }
    return b;
}

static void cl_show_alarms(void)
{
    CL.list_dirty = false;
    for (int i = 0; i < NALARM; i++) {
        if (i >= CL.nal) {
            lv_obj_add_flag(CL.rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(CL.rows[i], LV_OBJ_FLAG_HIDDEN);
        char b[32];
        ui_text(CL.row_time[i], "%02d:%02d", CL.al[i].hhmm / 100, CL.al[i].hhmm % 100);
        bz_set_color(CL.row_time[i], CL.al[i].on ? BZ_C_INK : BZ_C_DIM);
        ui_text(CL.row_days[i], "%s", days_text(CL.al[i].days, b, sizeof b));
        ui_chip_set(CL.row_sw[i], CL.al[i].on);
        ui_text(lv_obj_get_child(CL.row_sw[i], 0), "%s", CL.al[i].on ? "on" : "off");
    }
    if (CL.row_sel != CL.sel) {
        if (CL.row_sel >= 0) bz_tile_set_fill(CL.rows[CL.row_sel], BZ_C_SURFACE2);
        if (CL.sel >= 0) bz_tile_set_fill(CL.rows[CL.sel], BZ_C_SURFACE3);
        CL.row_sel = CL.sel;
    }
    if (CL.nal) lv_obj_add_flag(CL.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(CL.empty, LV_OBJ_FLAG_HIDDEN);
    if (CL.sel < 0 || CL.sel >= CL.nal) {
        lv_obj_add_flag(CL.edit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(CL.edit, LV_OBJ_FLAG_HIDDEN);
        const alarm_t *a = &CL.al[CL.sel];
        ui_text(CL.edit_h, "%02d", a->hhmm / 100);
        ui_text(CL.edit_m, "%02d", a->hhmm % 100);
        for (int d = 0; d < 7; d++) ui_chip_set(CL.day_chips[d], a->days >> d & 1);
    }
}

/* the world clocks: each zone's local time through TZ, then the tablet's own zone put back (UI thread) */
static void cl_world(time_t t)
{
    const char *cur = getenv("TZ");
    char keep[64];
    snprintf(keep, sizeof keep, "%s", cur ? cur : "");
    struct tm here, there[3];
    localtime_r(&t, &here);
    for (int i = 0; i < 3; i++) {
        setenv("TZ", ZONE[CL.zone[i]].tz, 1);
        tzset();
        localtime_r(&t, &there[i]);
    }
    if (cur) setenv("TZ", keep, 1);
    else unsetenv("TZ");
    tzset();
    for (int i = 0; i < 3; i++) {
        struct tm *z = &there[i];
        int dd = z->tm_year != here.tm_year ? (z->tm_year > here.tm_year ? 1 : -1) : z->tm_yday - here.tm_yday;
        int diff = (dd * 24 + z->tm_hour - here.tm_hour) * 60 + z->tm_min - here.tm_min;
        char off[16];
        int a = abs(diff);
        if (!diff) snprintf(off, sizeof off, "same time");
        else if (a % 60) snprintf(off, sizeof off, "%c%d:%02d h", diff < 0 ? '-' : '+', a / 60, a % 60);
        else snprintf(off, sizeof off, "%c%d h", diff < 0 ? '-' : '+', a / 60);
        ui_text(CL.wc_city[i], "%s", ZONE[CL.zone[i]].city);
        ui_text(CL.wc_time[i], "%02d:%02d", z->tm_hour, z->tm_min);
        ui_text(CL.wc_note[i], "%s · %s", dd > 0 ? "tomorrow" : dd < 0 ? "yesterday" : "today", off);
    }
}

static void cl_show_time(bool force)
{
    time_t t = time(NULL);
    if (t == CL.shown && !force) return;
    CL.shown = t;
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year <= 120) {
        ui_text(CL.time, "--:--");
        ui_text(CL.secs, " ");
        ui_text(CL.date, "the clock isn't set: join Wi-Fi, or set it in settings");
        return;
    }
    ui_text(CL.time, "%02d:%02d", tm.tm_hour, tm.tm_min);
    ui_text(CL.secs, "%02d", tm.tm_sec);
    char d[48];
    strftime(d, sizeof d, "%A, %B %e", &tm);
    for (char *p = d; *p; p++) *p = (char)tolower((unsigned char)*p);
    ui_text(CL.date, "%s", d);
    if (force || t / 60 != CL.wc_min) {
        CL.wc_min = (long)(t / 60);
        cl_world(t);
    }
}

static void cl_zone(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    CL.zone[i] = (CL.zone[i] + 1) % NZONE;
    char v[24];
    snprintf(v, sizeof v, "%d,%d,%d", CL.zone[0], CL.zone[1], CL.zone[2]);
    hal_kv_set("wclock", v);
    cl_show_time(true);
}

static void cl_select(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    CL.sel = CL.sel == i ? -1 : i;
    cl_show_alarms();
}

static void cl_toggle(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i >= CL.nal) return;
    CL.al[i].on = !CL.al[i].on;
    CL.sel = i;
    al_save();
    cl_show_alarms();
}

static void cl_add(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (CL.nal >= NALARM) {
        ui_island_say(BZ_I_NOTIFICATIONS, "five alarms is the most");
        return;
    }
    struct tm tm;
    int hhmm = 700;
    if (now_tm(&tm)) hhmm = ((tm.tm_hour + 1) % 24) * 100; /* the next whole hour */
    CL.al[CL.nal] = (alarm_t){ hhmm, true, 0 };
    CL.sel = CL.nal++;
    al_save();
    cl_show_alarms();
}

static void cl_delete(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (CL.sel < 0 || CL.sel >= CL.nal) {
        ui_island_say(BZ_I_NOTIFICATIONS, "pick an alarm first");
        return;
    }
    for (int i = CL.sel; i < CL.nal - 1; i++) CL.al[i] = CL.al[i + 1];
    CL.nal--;
    CL.sel = -1;
    al_save();
    cl_show_alarms();
}

/* hours step by one, minutes by one (held: repeating) */
static void cl_step(int dmin)
{
    if (CL.sel < 0 || CL.sel >= CL.nal) return;
    alarm_t *a = &CL.al[CL.sel];
    int m = (a->hhmm / 100 * 60 + a->hhmm % 100 + dmin + 24 * 60) % (24 * 60);
    a->hhmm = m / 60 * 100 + m % 60;
    a->on = true;
    cl_show_alarms();
}

static void cl_step_tap(lv_obj_t *o, void *u)
{
    (void)o;
    cl_step((int)(intptr_t)u);
    al_save();
}

static void cl_step_hold(lv_event_t *e)
{
    cl_step((int)(intptr_t)lv_event_get_user_data(e));
    CL.held = true;
}

static void cl_step_release(lv_event_t *e)
{
    (void)e;
    if (CL.held) al_save();
    CL.held = false;
}

static void cl_day(lv_obj_t *o, void *u)
{
    (void)o;
    if (CL.sel < 0 || CL.sel >= CL.nal) return;
    CL.al[CL.sel].days ^= (uint8_t)(1u << (int)(intptr_t)u);
    CL.al[CL.sel].on = true;
    al_save();
    cl_show_alarms();
}

static void cl_stop(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ring_stop();
}

static void cl_snooze(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!CL.ringing) return;
    CL.snooze_hhmm = CL.ring_hhmm;
    CL.snooze_at = (long)(time(NULL) / 60) + SNOOZE_MIN;
    ring_stop();
    ui_island_say(BZ_I_NOTIFICATIONS, "snoozed for 5 minutes");
}

static lv_obj_t *step_button(lv_obj_t *r, const char *icon, int dmin, bool repeat)
{
    lv_obj_t *b = ui_button(r, icon, NULL, cl_step_tap, (void *)(intptr_t)dmin);
    if (repeat) {
        lv_obj_add_event_cb(b, cl_step_hold, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dmin);
        lv_obj_add_event_cb(b, cl_step_release, LV_EVENT_RELEASED, NULL);
    }
    return b;
}

static void clock_build(lv_obj_t *b)
{
    ui_os_boot();
    lv_obj_t *hr = head_right(b);
    CL.add_btn = ui_button(hr, BZ_I_ADD, "add alarm", cl_add, NULL);
    ui_button(hr, BZ_I_CLOSE, "delete", cl_delete, NULL);

    int lw = 560, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = column_tile(b, PAD, lw);
    lv_obj_t *tr = bz_row(t, 10);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    CL.time = bz_label(tr, "--:--", BZ_F_CLOCK, BZ_C_INK);
    CL.secs = bz_label(tr, "00", BZ_F_TITLE, BZ_C_DIM);
    lv_obj_set_style_pad_bottom(CL.secs, 20, 0);
    CL.date = bz_label_line(t, "", BZ_F_NAME, BZ_C_DIM, IN(lw));
    lv_obj_set_pos(CL.date, 0, 140);
    lv_obj_t *wl = bz_label(t, "world · tap one to change the city", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(wl, 0, 198);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *w = bz_tile(t, IN(lw), 88);
        lv_obj_set_pos(w, 0, 230 + i * 98);
        bz_tile_set_fill(w, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(w, 12, 0);
        lv_obj_set_style_pad_hor(w, 20, 0);
        lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);
        CL.wc_city[i] = bz_label_line(w, "", BZ_F_BODY, BZ_C_INK, 260);
        CL.wc_note[i] = bz_label_line(w, "", BZ_F_CAPTION, BZ_C_DIM, 280);
        lv_obj_align(CL.wc_note[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        CL.wc_time[i] = bz_label(w, "", BZ_F_VALUE, BZ_C_INK);
        lv_obj_align(CL.wc_time[i], LV_ALIGN_RIGHT_MID, 0, 0);
        bz_on_tap(w, cl_zone, (void *)(intptr_t)i);
    }

    lv_obj_t *r = column_tile(b, PAD + lw + BZ_GAP, rw);
    bz_label(r, "alarms · they ring with the app closed", BZ_F_LABEL, BZ_C_DIM);
    CL.empty = bz_label(r, "No alarms yet. Tap add alarm.", BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_pos(CL.empty, 0, 40);
    for (int i = 0; i < NALARM; i++) {
        lv_obj_t *row = bz_tile(r, IN(rw), 64);
        lv_obj_set_pos(row, 0, 30 + i * 72);
        bz_tile_set_fill(row, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 20, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        CL.row_time[i] = bz_label(row, "", BZ_F_NAME, BZ_C_INK);
        lv_obj_align(CL.row_time[i], LV_ALIGN_LEFT_MID, 0, 0);
        CL.row_days[i] = bz_label_line(row, "", BZ_F_LABEL, BZ_C_DIM, IN(rw) - 40 - 100 - 110);
        lv_obj_align(CL.row_days[i], LV_ALIGN_LEFT_MID, 100, 0);
        CL.row_sw[i] = ui_chip(row, "on", cl_toggle, (void *)(intptr_t)i);
        lv_obj_set_height(CL.row_sw[i], 44);
        lv_obj_align(CL.row_sw[i], LV_ALIGN_RIGHT_MID, 0, 0);
        bz_on_tap(row, cl_select, (void *)(intptr_t)i);
        CL.rows[i] = row;
    }
    /* the editor, for the selected alarm */
    CL.edit = bz_box(r);
    lv_obj_set_pos(CL.edit, 0, 400);
    lv_obj_set_width(CL.edit, IN(rw));
    lv_obj_t *er = bz_row(CL.edit, 10);
    bz_label(er, "hour", BZ_F_LABEL, BZ_C_DIM);
    step_button(er, BZ_I_REMOVE, -60, false);
    CL.edit_h = bz_label(er, "00", BZ_F_NAME, BZ_C_INK);
    step_button(er, BZ_I_ADD, 60, false);
    lv_obj_t *gap = bz_box(er);
    lv_obj_set_width(gap, 10);
    bz_label(er, "min", BZ_F_LABEL, BZ_C_DIM);
    step_button(er, BZ_I_REMOVE, -1, true);
    CL.edit_m = bz_label(er, "00", BZ_F_NAME, BZ_C_INK);
    step_button(er, BZ_I_ADD, 1, true);
    lv_obj_t *dr = chip_row(CL.edit, IN(rw));
    lv_obj_set_pos(dr, 0, 66);
    for (int d = 0; d < 7; d++) CL.day_chips[d] = ui_chip(dr, DAY2[d], cl_day, (void *)(intptr_t)d);

    /* ringing: over the alarms, the time and the two answers */
    CL.ring_box = bz_tile(b, rw, APP_H);
    lv_obj_set_pos(CL.ring_box, PAD + lw + BZ_GAP, APP_Y);
    bz_tile_set_fill(CL.ring_box, BZ_C_SURFACE3);
    bz_label(CL.ring_box, "alarm", BZ_F_LABEL, BZ_C_DIM);
    CL.ring_time = bz_label(CL.ring_box, "", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_set_pos(CL.ring_time, 0, 40);
    lv_obj_t *rr = bz_row(CL.ring_box, 14);
    lv_obj_align(rr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *stop = ui_button(rr, BZ_I_STOP_CIRCLE, "stop", cl_stop, NULL);
    bz_tile_set_fill(stop, BZ_C_ICE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(stop); i++) bz_set_color(lv_obj_get_child(stop, (int32_t)i), BZ_C_ON_ICE);
    ui_button(rr, BZ_I_SCHEDULE, "snooze 5 min", cl_snooze, NULL);
    lv_obj_add_flag(CL.ring_box, LV_OBJ_FLAG_HIDDEN);
    CL.ring_shown = false;
    CL.row_sel = -1;
    cl_show_alarms();
    cl_show_time(true);
}

static void clock_refresh(void)
{
    cl_show_time(false);
    if (CL.list_dirty) cl_show_alarms();
    bool ring = CL.ringing;
    if (ring != CL.ring_shown) {
        CL.ring_shown = ring;
        if (ring) {
            ui_text(CL.ring_time, "%02d:%02d", CL.ring_hhmm / 100, CL.ring_hhmm % 100);
            lv_obj_remove_flag(CL.ring_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(CL.ring_box);
        } else {
            lv_obj_add_flag(CL.ring_box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void clock_open(void)
{
    cl_show_alarms();
    cl_show_time(true);
}

const ui_app_t APP_CLOCK = { .name = "clock", .icon = BZ_I_SCHEDULE, .build = clock_build, .open = clock_open,
                             .refresh = clock_refresh };

/* ================================================================== calendar */

#define EV_MAX 400
#define EV_TEXT 72
#define EV_FILE_MAX (64 * 1024)

typedef struct {
    int ymd;            /* 20260924 */
    int hhmm;
    char text[EV_TEXT];
} event_t;

typedef struct {
    lv_obj_t *cell[42], *num[42], *dots[42];
    int ymd[42];
    signed char look[42]; /* how each cell is drawn now, so a redraw restyles only what changed */
} cal_cells_t;

static OS_BSS struct {
    event_t *ev;             /* sorted by day and time; PSRAM */
    int nev;
    event_t *got;            /* the reader's result, until the refresh takes it */
    int ngot;
    volatile int load;       /* 0 idle, 1 reading, 2 read, 3 no card, 4 failed */
    volatile int save;       /* 0 idle, 1 writing, 2 written, 3 failed */
    char *save_buf;          /* handed to the writer, which frees it */
    size_t save_len;
    bool dirty, loaded;
    int y, m, sel;           /* the month shown (m 1..12) and the day picked */
    int pending_del;         /* 1 + the event a first tap on × marked; 0 none */
    int load_seen, list_w;
    double pending_t;
    cal_cells_t *cc;
    lv_obj_t *title, *list, *day_title, *status;
    ui_kb_t *kb;
} CAL;

static int ymd_of(int y, int m, int d) { return y * 10000 + m * 100 + d; }
static int dim_of(int y, int m)
{
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : D[m - 1];
}
/* 0 Sunday (Sakamoto) */
static int dow_of(int y, int m, int d)
{
    static const int T[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

static int ev_cmp(const void *a, const void *b)
{
    const event_t *x = a, *y = b;
    if (x->ymd != y->ymd) return x->ymd < y->ymd ? -1 : 1;
    return x->hhmm - y->hhmm;
}

static void *cal_reader(void *u)
{
    (void)u;
    char path[96];
    if (!cstore_path(CS_DATA, "EVENTS.TXT", path, sizeof path)) {
        CAL.load = 3;
        return NULL;
    }
    cstore_layout();
    event_t *ev = malloc(EV_MAX * sizeof *ev);
    if (!ev) {
        CAL.load = 4;
        return NULL;
    }
    int n = 0;
    struct stat st;
    if (stat(path, &st) == 0) {
        char *txt = cstore_read(path, EV_FILE_MAX, NULL, NULL);
        if (!txt) {
            free(ev);
            CAL.load = 4;
            return NULL;
        }
        for (char *line = txt; line && *line && n < EV_MAX;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            int y, mo, d, hh, mi, used = 0;
            if (sscanf(line, "%4d-%2d-%2d %2d:%2d %n", &y, &mo, &d, &hh, &mi, &used) == 5 && used > 0 && mo >= 1 &&
                mo <= 12 && d >= 1 && d <= 31 && hh >= 0 && hh < 24 && mi >= 0 && mi < 60) {
                event_t *e = &ev[n++];
                e->ymd = ymd_of(y, mo, d);
                e->hhmm = hh * 100 + mi;
                snprintf(e->text, sizeof e->text, "%s", line + used);
                size_t k = strlen(e->text);
                while (k && (e->text[k - 1] == '\r' || e->text[k - 1] == ' ')) e->text[--k] = 0;
            }
            line = nl ? nl + 1 : NULL;
        }
        free(txt);
    }
    qsort(ev, (size_t)n, sizeof *ev, ev_cmp);
    CAL.got = ev;
    CAL.ngot = n;
    PUBLISH(CAL.load, 2);
    return NULL;
}

static void *cal_writer(void *u)
{
    (void)u;
    char path[96];
    bool ok = cstore_path(CS_DATA, "EVENTS.TXT", path, sizeof path) && cstore_write(path, CAL.save_buf, CAL.save_len);
    free(CAL.save_buf);
    CAL.save_buf = NULL;
    CAL.save = ok ? 2 : 3;
    return NULL;
}

static void cal_save(void)
{
    if (CAL.save == 1) {
        CAL.dirty = true; /* the writer's still busy: write again when it's done */
        return;
    }
    CAL.dirty = false;
    size_t cap = (size_t)CAL.nev * (EV_TEXT + 20) + 1, k = 0;
    char *buf = malloc(cap);
    if (!buf) {
        ui_island_say(BZ_I_SD_CARD, "out of memory saving events");
        return;
    }
    for (int i = 0; i < CAL.nev; i++) {
        const event_t *e = &CAL.ev[i];
        k += (size_t)snprintf(buf + k, cap - k, "%04d-%02d-%02d %02d:%02d %s\n", e->ymd / 10000, e->ymd / 100 % 100,
                              e->ymd % 100, e->hhmm / 100, e->hhmm % 100, e->text);
    }
    CAL.save_buf = buf;
    CAL.save_len = k;
    CAL.save = 1;
    if (!hal_thread("cal-save", cal_writer, NULL, 4096)) {
        free(buf);
        CAL.save_buf = NULL;
        CAL.save = 3;
    }
}

static void cal_show_day(void);

static void cal_show_month(void)
{
    static const char *const MON[12] = { "january", "february", "march", "april", "may", "june", "july", "august",
                                         "september", "october", "november", "december" };
    ui_text(CAL.title, "%s %d", MON[CAL.m - 1], CAL.y);
    struct tm tm;
    int today = now_tm(&tm) ? ymd_of(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) : 0;
    int first = dow_of(CAL.y, CAL.m, 1), dim = dim_of(CAL.y, CAL.m);
    int py = CAL.m == 1 ? CAL.y - 1 : CAL.y, pm = CAL.m == 1 ? 12 : CAL.m - 1;
    int ny = CAL.m == 12 ? CAL.y + 1 : CAL.y, nm = CAL.m == 12 ? 1 : CAL.m + 1;
    cal_cells_t *c = CAL.cc;
    for (int i = 0; i < 42; i++) {
        int d = i - first + 1, ymd;
        bool in = d >= 1 && d <= dim;
        if (d < 1) ymd = ymd_of(py, pm, dim_of(py, pm) + d);
        else if (d > dim) ymd = ymd_of(ny, nm, d - dim);
        else ymd = ymd_of(CAL.y, CAL.m, d);
        c->ymd[i] = ymd;
        ui_text(c->num[i], "%d", ymd % 100);
        int n = 0;
        for (int k = 0; k < CAL.nev; k++) n += CAL.ev[k].ymd == ymd;
        ui_text(c->dots[i], "%s", n == 0 ? " " : n == 1 ? "\xc2\xb7" : n == 2 ? "\xc2\xb7\xc2\xb7" : "\xc2\xb7\xc2\xb7\xc2\xb7");
        /* today ice; the picked day lifted; the neighbours' days faint */
        int look = ymd == today ? 3 : ymd == CAL.sel ? 2 : in ? 1 : 0;
        if (look == c->look[i]) continue;
        c->look[i] = (signed char)look;
        bz_tile_set_fill(c->cell[i], look == 3 ? BZ_C_ICE : look == 2 ? BZ_C_SURFACE3 : BZ_C_SURFACE2);
        bz_color_role_t ink = look == 3 ? BZ_C_ON_ICE : look ? BZ_C_INK : BZ_C_FAINT;
        bz_set_color(c->num[i], ink);
        bz_set_color(c->dots[i], look == 3 ? BZ_C_ON_ICE : BZ_C_ICE);
    }
    cal_show_day();
}

static void cal_del(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    double now = hal_seconds();
    if (CAL.pending_del != i + 1 || now - CAL.pending_t > 4) {
        CAL.pending_del = i + 1;
        CAL.pending_t = now;
        ui_text(CAL.status, "tap \xc3\x97 again to delete it");
        return;
    }
    CAL.pending_del = 0;
    if (i < 0 || i >= CAL.nev || CAL.load == 1) return; /* a read in flight would bring the event back */
    memmove(&CAL.ev[i], &CAL.ev[i + 1], (size_t)(CAL.nev - i - 1) * sizeof *CAL.ev);
    CAL.nev--;
    cal_save();
    cal_show_month();
}

static void cal_show_day(void)
{
    static const char *const WD[7] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
    int y = CAL.sel / 10000, m = CAL.sel / 100 % 100, d = CAL.sel % 100;
    ui_text(CAL.day_title, "%s %d/%d", WD[dow_of(y, m, d)], m, d);
    lv_obj_clean(CAL.list);
    CAL.pending_del = 0;
    int n = 0, rw = CAL.list_w;
    for (int i = 0; i < CAL.nev; i++) {
        if (CAL.ev[i].ymd != CAL.sel) continue;
        n++;
        lv_obj_t *t = bz_tile(CAL.list, rw, LV_SIZE_CONTENT);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_pad_all(t, 16, 0);
        lv_obj_t *tl = bz_label(t, "", BZ_F_NAME, BZ_C_INK);
        ui_text(tl, "%02d:%02d", CAL.ev[i].hhmm / 100, CAL.ev[i].hhmm % 100);
        lv_obj_t *tx = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
        lv_label_set_text(tx, CAL.ev[i].text);
        lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tx, rw - 32 - 60);
        lv_obj_set_pos(tx, 0, 34);
        lv_obj_t *x = ui_button(t, BZ_I_CLOSE, NULL, cal_del, (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(x, 14, 0);
        lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    if (!n) bz_label(CAL.list, "Nothing on this day.", BZ_F_BODY, BZ_C_DIM);
    if (CAL.load == 1) ui_text(CAL.status, "reading the card…");
    else if (CAL.load == 3) ui_text(CAL.status, "no microSD card: events live on the card");
    else if (CAL.load == 4) ui_text(CAL.status, "couldn't read CATOS/DATA/EVENTS.TXT");
    else {
        int mon = 0;
        for (int i = 0; i < CAL.nev; i++) mon += CAL.ev[i].ymd / 100 == CAL.y * 100 + CAL.m;
        ui_text(CAL.status, "%d event%s · %d this month", n, n == 1 ? "" : "s", mon);
    }
}

static void cal_go(int dm)
{
    int m = CAL.y * 12 + (CAL.m - 1) + dm;
    CAL.y = m / 12;
    CAL.m = m % 12 + 1;
    int d = CAL.sel % 100, dim = dim_of(CAL.y, CAL.m);
    CAL.sel = ymd_of(CAL.y, CAL.m, d > dim ? dim : d);
    cal_show_month();
}

static void cal_prev(lv_obj_t *o, void *u) { (void)o; (void)u; cal_go(-1); }
static void cal_next(lv_obj_t *o, void *u) { (void)o; (void)u; cal_go(1); }

static void cal_today(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    struct tm tm;
    if (!now_tm(&tm)) {
        ui_island_say(BZ_I_SCHEDULE, "the clock isn't set");
        return;
    }
    CAL.y = tm.tm_year + 1900;
    CAL.m = tm.tm_mon + 1;
    CAL.sel = ymd_of(CAL.y, CAL.m, tm.tm_mday);
    cal_show_month();
}

static void cal_pick(lv_obj_t *o, void *u)
{
    (void)o;
    int ymd = CAL.cc->ymd[(intptr_t)u];
    CAL.sel = ymd;
    if (ymd / 100 != CAL.y * 100 + CAL.m) { /* a neighbour month's day: go there */
        CAL.y = ymd / 10000;
        CAL.m = ymd / 100 % 100;
    }
    cal_show_month();
}

static void cal_swipe(lv_obj_t *o, int dx, int dy, float vx, float vy, void *u)
{
    (void)o; (void)dy; (void)vy; (void)u;
    if (dx < -80 || vx < -600) cal_go(1);
    else if (dx > 80 || vx > 600) cal_go(-1);
}

static void cal_added(const char *text, void *u)
{
    (void)u;
    while (*text == ' ') text++;
    int hh, mi, used = 0;
    if (sscanf(text, "%2d:%2d%n", &hh, &mi, &used) != 2 || hh < 0 || hh > 23 || mi < 0 || mi > 59) {
        ui_island_say(BZ_I_SCHEDULE, "start with a time, e.g. 14:30");
        ui_kb_show(CAL.kb, "new event · a time, then what: 14:30 pit meeting", text, false, true, cal_added, NULL);
        return;
    }
    const char *what = text + used;
    while (*what == ' ') what++;
    if (!*what) what = "event";
    if (CAL.nev >= EV_MAX) {
        ui_island_say(BZ_I_SCHEDULE, "the calendar is full");
        return;
    }
    event_t e = { .ymd = CAL.sel, .hhmm = hh * 100 + mi };
    snprintf(e.text, sizeof e.text, "%s", what);
    for (char *p = e.text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    int at = CAL.nev;
    while (at > 0 && ev_cmp(&CAL.ev[at - 1], &e) > 0) at--;
    memmove(&CAL.ev[at + 1], &CAL.ev[at], (size_t)(CAL.nev - at) * sizeof e);
    CAL.ev[at] = e;
    CAL.nev++;
    cal_save();
    cal_show_month();
}

static void cal_add(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!hal_sd_root() || CAL.load == 3) {
        ui_island_say(BZ_I_SD_CARD, "no microSD card: events live on the card");
        return;
    }
    if (CAL.load == 4 && !CAL.loaded) {
        ui_island_say(BZ_I_SD_CARD, "couldn't read CATOS/DATA/EVENTS.TXT");
        return;
    }
    if (!CAL.loaded || !CAL.ev || CAL.load == 1) {
        ui_island_say(BZ_I_SD_CARD, "still reading the calendar");
        return;
    }
    ui_kb_show(CAL.kb, "new event · a time, then what: 14:30 pit meeting", "", false, true, cal_added, NULL);
}

static void cal_load(void)
{
    if (CAL.load == 1 || CAL.save == 1 || CAL.dirty) return; /* never read over what's still being written */
    CAL.load = 1;
    if (!hal_thread("cal-read", cal_reader, NULL, 6144)) CAL.load = 4;
}

static void calendar_build(lv_obj_t *b)
{
    CAL.cc = lv_malloc_zeroed(sizeof *CAL.cc);
    for (int i = 0; i < 42; i++) CAL.cc->look[i] = -1;
    lv_obj_t *hr = head_right(b);
    ui_button(hr, BZ_I_ADD, "add event", cal_add, NULL);

    int lw = 760, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = column_tile(b, PAD, lw);
    lv_obj_t *top = bz_row(t, 10);
    lv_obj_set_width(top, IN(lw));
    ui_button(top, BZ_I_ARROW_BACK, NULL, cal_prev, NULL);
    CAL.title = bz_label_line(top, "", BZ_F_TITLE, BZ_C_INK, 380);
    lv_obj_set_style_text_align(CAL.title, LV_TEXT_ALIGN_CENTER, 0);
    ui_button(top, BZ_I_ARROW_FORWARD, NULL, cal_next, NULL);
    ui_chip(top, "today", cal_today, NULL);
    int gap = 8, cw = (IN(lw) - 6 * gap) / 7, ch = (APP_H - 2 * BZ_PAD_TILE - 100 - 5 * gap) / 6;
    static const char *const WD[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = bz_label_line(t, WD[i], BZ_F_LABEL, BZ_C_DIM, cw);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, i * (cw + gap), 70);
    }
    /* the grid swipes sideways for the next or previous month */
    lv_obj_t *grid = bz_box(t);
    lv_obj_set_pos(grid, 0, 100);
    lv_obj_set_size(grid, IN(lw), 6 * ch + 5 * gap);
    bz_drag_t d = { .end = cal_swipe, .axis = 1, .slop = 16 };
    bz_drag_attach(grid, &d);
    for (int i = 0; i < 42; i++) {
        lv_obj_t *c = bz_tile(grid, cw, ch);
        lv_obj_set_pos(c, i % 7 * (cw + gap), i / 7 * (ch + gap));
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_pad_all(c, 8, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        CAL.cc->num[i] = bz_label(c, "", BZ_F_BODY, BZ_C_INK);
        CAL.cc->dots[i] = bz_label(c, " ", BZ_F_NAME, BZ_C_ICE);
        lv_obj_align(CAL.cc->dots[i], LV_ALIGN_BOTTOM_RIGHT, 0, 6);
        bz_on_tap(c, cal_pick, (void *)(intptr_t)i);
        CAL.cc->cell[i] = c;
    }

    lv_obj_t *r = column_tile(b, PAD + lw + BZ_GAP, rw);
    CAL.day_title = bz_label_line(r, "", BZ_F_NAME, BZ_C_INK, IN(rw));
    CAL.status = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, IN(rw));
    lv_obj_set_pos(CAL.status, 0, 36);
    lv_obj_t *wrap = bz_box(r);
    lv_obj_set_pos(wrap, 0, 70);
    CAL.list_w = IN(rw);
    CAL.list = ui_scroller(wrap, CAL.list_w, APP_H - 2 * BZ_PAD_TILE - 70);
    CAL.kb = ui_kb_create(b, 420);

    struct tm tm;
    if (now_tm(&tm)) {
        CAL.y = tm.tm_year + 1900;
        CAL.m = tm.tm_mon + 1;
        CAL.sel = ymd_of(CAL.y, CAL.m, tm.tm_mday);
    } else {
        CAL.y = 2026;
        CAL.m = 1;
        CAL.sel = ymd_of(2026, 1, 1);
    }
    cal_show_month();
}

static void calendar_open(void)
{
    cal_load();
    cal_show_month();
}

static void calendar_refresh(void)
{
    int l = CAL.load;
    if (l == 2) {
        free(CAL.ev);
        CAL.ev = CAL.got;
        CAL.nev = CAL.ngot;
        CAL.got = NULL;
        CAL.load = l = 0;
        CAL.loaded = true;
        cal_show_month();
    } else if (l >= 3 && l != CAL.load_seen) {
        cal_show_day(); /* says why there's nothing */
    }
    CAL.load_seen = l;
    if (CAL.save >= 2) {
        if (CAL.save == 3) ui_island_say(BZ_I_SD_CARD, "couldn't save the calendar to the card");
        CAL.save = 0;
        if (CAL.dirty) cal_save();
    }
}

const ui_app_t APP_CALENDAR = { .name = "calendar", .icon = BZ_I_GRID_VIEW, .build = calendar_build,
                                .open = calendar_open, .refresh = calendar_refresh };

/* ================================================================== documents */

#define DOC_MAX (256 * 1024)
#define DOC_FILES 64
#define DOC_CHUNK 2048 /* one label each: LVGL draws only the ones in view */

static OS_BSS struct {
    cstore_entry_t *files;   /* PSRAM */
    int nfiles;
    volatile int list_state; /* 0 idle, 1 listing, 2 listed, 3 no card, 4 failed */
    volatile int read_state; /* 0 idle, 1 reading, 2 read, 3 failed */
    char path[160], name[64];
    char *text;              /* the open document, NUL-separated into chunks; the labels point into it */
    size_t len;
    bool trunc;
    char *got;               /* the reader's result, until the refresh takes it */
    size_t got_len;
    bool got_trunc;
    lv_obj_t *list, *title, *meta;
    int list_w, text_w;
    vs_t vs;
} DC;

/* The body faces have no bullet, en dash, minus sign or curly quotes: each becomes one they have. Also
 * drops \r and turns tabs into spaces. In place (nothing grows); returns the new length. */
static size_t doc_clean(char *s, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n;) {
        unsigned char c = (unsigned char)s[r];
        if (c == '\r') { r++; continue; }
        if (c == '\t') { s[w++] = ' '; r++; continue; }
        if (c == 0xE2 && r + 2 < n) {
            unsigned char b = (unsigned char)s[r + 1], d = (unsigned char)s[r + 2];
            if (b == 0x80 && d == 0xA2) { s[w++] = (char)0xC2; s[w++] = (char)0xB7; r += 3; continue; } /* • → · */
            if (b == 0x80 && d == 0x93) { s[w++] = (char)0xE2; s[w++] = (char)0x80; s[w++] = (char)0x94; r += 3; continue; } /* – → — */
            if (b == 0x88 && d == 0x92) { s[w++] = '-'; r += 3; continue; }                                  /* − */
            if (b == 0x80 && (d == 0x98 || d == 0x99)) { s[w++] = '\''; r += 3; continue; }
            if (b == 0x80 && (d == 0x9C || d == 0x9D)) { s[w++] = '"'; r += 3; continue; }
        }
        if (c == 0) { s[w++] = ' '; r++; continue; } /* a stray NUL would end the text early */
        s[w++] = s[r++];
    }
    s[w] = 0;
    return w;
}

/* Cuts the text into chunks of about DOC_CHUNK bytes with NULs, at a line break where there is one, else a
 * space. A run with neither (not really text) is cut at a character boundary, losing that character. */
static void doc_chunk(char *s, size_t n)
{
    size_t at = 0;
    while (n - at > DOC_CHUNK) {
        size_t lo = at + DOC_CHUNK / 2, hi = at + DOC_CHUNK, cut = 0;
        for (size_t i = hi; i > lo && !cut; i--) if (s[i] == '\n') cut = i;
        for (size_t i = hi; i > lo && !cut; i--) if (s[i] == ' ') cut = i;
        if (!cut) {
            cut = hi;
            while (cut > lo && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        }
        s[cut] = 0;
        at = cut + 1;
    }
}

static void *doc_lister(void *u)
{
    (void)u;
    char dir[96];
    if (!cstore_path(CS_DOCS, NULL, dir, sizeof dir)) {
        DC.list_state = 3;
        return NULL;
    }
    cstore_layout();
    int n = cstore_list(dir, ".TXT.MD", false, DC.files, DOC_FILES);
    DC.nfiles = n < 0 ? 0 : n;
    PUBLISH(DC.list_state, n < 0 ? 4 : 2);
    return NULL;
}

static void *doc_reader(void *u)
{
    (void)u;
    size_t len = 0;
    bool trunc = false;
    char *t = cstore_read(DC.path, DOC_MAX, &len, &trunc);
    if (!t) {
        DC.read_state = 3;
        return NULL;
    }
    if (trunc) /* don't end on half a character */
        while (len && ((unsigned char)t[len - 1] & 0xC0) == 0x80) len--;
    if (trunc && len && ((unsigned char)t[len - 1] & 0xC0) == 0xC0) len--;
    len = doc_clean(t, len);
    doc_chunk(t, len);
    DC.got = t;
    DC.got_len = len;
    DC.got_trunc = trunc;
    PUBLISH(DC.read_state, 2);
    return NULL;
}

static void doc_list_start(void)
{
    if (DC.list_state == 1) return;
    if (!DC.files) DC.files = lv_malloc(DOC_FILES * sizeof *DC.files);
    if (!DC.files) {
        DC.list_state = 4;
        return;
    }
    DC.list_state = 1;
    lv_obj_clean(DC.list);
    bz_label(DC.list, "reading the card…", BZ_F_CAPTION, BZ_C_DIM);
    if (!hal_thread("docs-list", doc_lister, NULL, 4096)) DC.list_state = 4;
}

static void doc_open(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (DC.read_state == 1 || i < 0 || i >= DC.nfiles) return;
    char dir[96];
    if (!cstore_path(CS_DOCS, NULL, dir, sizeof dir)) return;
    snprintf(DC.path, sizeof DC.path, "%s/%s", dir, DC.files[i].name);
    snprintf(DC.name, sizeof DC.name, "%s", DC.files[i].name);
    ui_text(DC.title, "%s", DC.name);
    ui_text(DC.meta, "reading…");
    DC.read_state = 1;
    if (!hal_thread("docs-read", doc_reader, NULL, 4096)) {
        DC.read_state = 0;
        ui_text(DC.meta, "couldn't start reading");
    }
}

static void doc_show_list(void)
{
    lv_obj_clean(DC.list);
    int lw = DC.list_w;
    if (DC.list_state == 3) {
        bz_label(DC.list, "No microSD card.", BZ_F_BODY, BZ_C_DIM);
        return;
    }
    if (DC.list_state == 4) {
        bz_label(DC.list, "Couldn't open CATOS/DOCS.", BZ_F_BODY, BZ_C_DIM);
        return;
    }
    if (!DC.nfiles) {
        lv_obj_t *l = bz_label(DC.list, "No .TXT or .MD files in CATOS/DOCS yet. Notes saved to the card appear here.",
                               BZ_F_BODY, BZ_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lw);
        return;
    }
    for (int i = 0; i < DC.nfiles; i++) {
        lv_obj_t *t = bz_tile(DC.list, lw, 72);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(t, 10, 0);
        lv_obj_set_style_pad_hor(t, 16, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_label_line(t, DC.files[i].name, BZ_F_BODY, BZ_C_INK, lw - 32);
        char sz[24], when[24] = "";
        struct tm tm;
        localtime_r(&DC.files[i].mtime, &tm);
        if (tm.tm_year > 100) strftime(when, sizeof when, " · %b %e %H:%M", &tm);
        lv_obj_t *m = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, lw - 32);
        ui_text(m, "%s%s", cstore_size(DC.files[i].size, sz, sizeof sz), when);
        lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        bz_on_tap(t, doc_open, (void *)(intptr_t)i);
    }
}

static void doc_show_text(void)
{
    lv_obj_clean(DC.vs.content); /* the labels point into the old text: gone before it is */
    free(DC.text);
    DC.text = DC.got;
    DC.len = DC.got_len;
    DC.trunc = DC.got_trunc;
    DC.got = NULL;
    vs_top(&DC.vs);
    char sz[24];
    ui_text(DC.meta, "%s%s", cstore_size(DC.len, sz, sizeof sz), DC.trunc ? " · the first 256 kb of a longer file" : "");
    int w = DC.text_w;
    if (!DC.len) {
        bz_label(DC.vs.content, "This file is empty.", BZ_F_BODY, BZ_C_DIM);
        return;
    }
    for (const char *p = DC.text; p < DC.text + DC.len; p += strlen(p) + 1) {
        lv_obj_t *l = bz_label(DC.vs.content, "", BZ_F_BODY, BZ_C_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, w);
        lv_label_set_text_static(l, p);
    }
}

static void doc_top(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    bz_motion_to(&DC.vs.y, 0, BZ_SMOOTH);
}

static void doc_rescan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    doc_list_start();
}

static void docs_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    ui_button(hr, BZ_I_EXPAND_LESS, "top", doc_top, NULL);
    ui_button(hr, BZ_I_REFRESH, "rescan", doc_rescan, NULL);
    int lw = 360, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    DC.list_w = lw;
    DC.list = ui_scroller(wrap, lw, APP_H);
    lv_obj_t *t = column_tile(b, PAD + lw + BZ_GAP, rw);
    DC.title = bz_label_line(t, "pick a document", BZ_F_NAME, BZ_C_INK, IN(rw));
    DC.meta = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, IN(rw));
    lv_obj_set_pos(DC.meta, 0, 34);
    lv_obj_t *box = bz_box(t);
    lv_obj_set_pos(box, 0, 66);
    DC.text_w = IN(rw);
    vs_init(&DC.vs, box, DC.text_w, APP_H - 2 * BZ_PAD_TILE - 66, 0);
}

static void docs_open(void) { doc_list_start(); }

static void docs_refresh(void)
{
    if (DC.list_state >= 2) {
        doc_show_list();
        DC.list_state = 0;
    }
    if (DC.read_state == 2) {
        DC.read_state = 0;
        doc_show_text();
    } else if (DC.read_state == 3) {
        DC.read_state = 0;
        ui_text(DC.meta, "couldn't read %s", DC.name);
    }
}

static void docs_frame(double now, double dt)
{
    (void)now; (void)dt;
    vs_tick(&DC.vs);
}

const ui_app_t APP_DOCS = { .name = "documents", .icon = BZ_I_DESCRIPTION, .build = docs_build, .open = docs_open,
                            .refresh = docs_refresh, .frame = docs_frame };

/* ================================================================== photos */

#define PH_MAX 40
#define TH_W 232
#define TH_H 130

typedef struct {
    char path[128];
    char name[40];
} ph_file_t;

typedef struct {
    ph_file_t files[PH_MAX];
    hal_picture_t thumb[PH_MAX];
    lv_image_dsc_t dsc[PH_MAX];
    lv_obj_t *img[PH_MAX], *note[PH_MAX];
    volatile uint8_t tstate[PH_MAX]; /* 0 waiting, 1 decoded, 2 failed */
    uint8_t shown[PH_MAX];
    cstore_entry_t scan[PH_MAX];
    time_t mtime[PH_MAX];       /* the listing's, beside files[] */
} ph_mem_t;

static OS_BSS struct {
    ph_mem_t *mem;             /* PSRAM, kept once made */
    int n;
    volatile int stage;        /* 0 idle, 1 listing, 2 listed (the refresh builds the grid), 3 no card */
    volatile bool running, quit;
    bool rescan, grid_built;
    /* full screen */
    volatile int full_state;   /* 0 none, 1 decoding, 2 decoded, 3 failed */
    int full_idx;
    hal_picture_t full_got;    /* the worker's result, until the refresh takes it */
    char full_err[96];
    hal_picture_t shown;       /* on screen, the UI's */
    lv_image_dsc_t full_dsc;
    bool viewing;
    lv_obj_t *count, *viewer, *view_img, *view_cap, *view_msg;
    vs_t vs;
    /* the slideshow: no grid, one picture after another; the one on screen stays until the next is decoded */
    bool show_req, show;
    double show_next;
} PH;

#define SHOW_S 12.0          /* a picture's time on screen */

static void ph_add_dir(const char *dir, int *k)
{
    ph_mem_t *m = PH.mem;
    int n = cstore_list(dir, ".JPG.JPEG", true, m->scan, PH_MAX);
    for (int i = 0; i < n; i++) {
        /* the newest PH_MAX of both folders: insert by time, drop what falls off the end */
        int at = *k;
        time_t t = m->scan[i].mtime;
        while (at > 0 && m->mtime[at - 1] < t) at--;
        if (at >= PH_MAX) continue;
        int last = *k < PH_MAX ? *k : PH_MAX - 1;
        memmove(&m->files[at + 1], &m->files[at], (size_t)(last - at) * sizeof m->files[0]);
        memmove(&m->mtime[at + 1], &m->mtime[at], (size_t)(last - at) * sizeof m->mtime[0]);
        snprintf(m->files[at].path, sizeof m->files[at].path, "%s/%s", dir, m->scan[i].name);
        snprintf(m->files[at].name, sizeof m->files[at].name, "%s", m->scan[i].name);
        m->mtime[at] = t;
        if (*k < PH_MAX) (*k)++;
    }
}

static void ph_free_thumbs(void)
{
    for (int i = 0; i < PH_MAX; i++) {
        hal_picture_free(&PH.mem->thumb[i]);
        PH.mem->tstate[i] = 0;
    }
}

/* one worker: the listing (when asked), then each thumbnail, with a full-screen request served in between */
static void ph_serve_full(void)
{
    if (PH.full_state != 1) return;
    int i = PH.full_idx;
    hal_picture_t p;
    char err[96];
    bool ok = i >= 0 && i < PH.n && hal_jpeg_load(PH.mem->files[i].path, W, H, &p, err, sizeof err);
    if (!ok && (i < 0 || i >= PH.n)) snprintf(err, sizeof err, "gone from the list");
    if (PH.quit) {
        if (ok) hal_picture_free(&p);
        PH.full_state = 0;
        return;
    }
    if (ok) PH.full_got = p;
    else snprintf(PH.full_err, sizeof PH.full_err, "%s", err);
    PUBLISH(PH.full_state, ok ? 2 : 3);
}

static void *ph_worker(void *u)
{
    (void)u;
    if (PH.stage == 1) {
        const char *sd = hal_sd_root();
        if (!sd) {
            PH.stage = 3;
            PH.running = false;
            return NULL;
        }
        cstore_layout();
        ph_free_thumbs();
        int k = 0;
        char dir[96];
        if (cstore_path(CS_PHOTOS, NULL, dir, sizeof dir)) ph_add_dir(dir, &k);
        snprintf(dir, sizeof dir, "%s/lens", sd); /* the lens app's snapshots */
        ph_add_dir(dir, &k);
        PH.n = k;
        PUBLISH(PH.stage, 2);
    }
    for (int i = 0; i < PH.n && !PH.quit && !PH.show; i++) {
        ph_serve_full();
        if (PH.mem->tstate[i]) continue;
        char err[96];
        bool ok = hal_jpeg_load(PH.mem->files[i].path, TH_W, TH_H, &PH.mem->thumb[i], err, sizeof err);
        PUBLISH(PH.mem->tstate[i], ok ? 1 : 2);
    }
    if (!PH.quit) ph_serve_full();
    if (PH.quit) {
        ph_free_thumbs(); /* the app closed: its grid is gone, so are the pictures */
        if (PH.full_state == 1) PH.full_state = 0;
    }
    PUBLISH(PH.running, false);
    return NULL;
}

static bool ph_spawn(void)
{
    PH.quit = false;
    PH.running = true;
    if (hal_thread("photos", ph_worker, NULL, 6144)) return true;
    PH.running = false;
    return false;
}

/* the grid's images and the viewer's, off before the pixels under them go */
static void ph_drop_grid(void)
{
    for (int i = 0; i < PH_MAX; i++) {
        if (PH.mem->shown[i]) lv_image_cache_drop(&PH.mem->dsc[i]);
        PH.mem->shown[i] = 0;
        PH.mem->img[i] = PH.mem->note[i] = NULL;
    }
    lv_obj_clean(PH.vs.content);
    PH.grid_built = false;
}

static void ph_drop_view(void)
{
    if (PH.shown.px) {
        lv_image_set_src(PH.view_img, NULL);
        lv_image_cache_drop(&PH.full_dsc);
        hal_picture_free(&PH.shown);
    }
}

static void ph_scan(void)
{
    if (PH.running) {
        PH.rescan = true; /* when the worker finishes */
        return;
    }
    PH.rescan = false;
    ph_drop_grid();
    PH.stage = 1;
    ui_text(PH.count, "looking…");
    if (!ph_spawn()) {
        PH.stage = 0;
        ui_text(PH.count, "couldn't start");
    }
}

static void ph_rescan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ph_scan();
}

static void ph_view_close(lv_obj_t *o, void *u)
{
    (void)u;
    PH.viewing = false;
    ph_drop_view();
    lv_obj_add_flag(PH.viewer, LV_OBJ_FLAG_HIDDEN);
    if (PH.show && o) ui_app_close(); /* a tap ends the slideshow, back to whatever opened it */
}

static void ph_view(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= PH.n || PH.full_state == 1) return;
    bool keep = PH.show && PH.shown.px; /* a slideshow keeps the last picture up while the next decodes */
    if (!keep) ph_drop_view();
    PH.viewing = true;
    PH.full_idx = i;
    lv_obj_remove_flag(PH.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(PH.viewer);
    if (!keep) {
        lv_obj_add_flag(PH.view_img, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(PH.view_msg, "decoding…");
        lv_obj_remove_flag(PH.view_msg, LV_OBJ_FLAG_HIDDEN);
    }
    if (PH.show) ui_text(PH.view_cap, "%s", "");
    else ui_text(PH.view_cap, "%s · tap to close", PH.mem->files[i].name);
    PH.full_state = 1;
    if (!PH.running && !ph_spawn()) {
        PH.full_state = 0;
        lv_label_set_text(PH.view_msg, "couldn't start the decoder");
    }
}

static void ph_build_grid(void)
{
    ph_mem_t *m = PH.mem;
    lv_obj_t *grid = bz_row(PH.vs.content, BZ_GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BZ_GAP, 0);
    lv_obj_set_width(grid, W - 2 * PAD);
    for (int i = 0; i < PH.n; i++) {
        lv_obj_t *t = bz_tile(grid, TH_W, TH_H + 34);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_clip_corner(t, true, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *box = bz_box(t);
        lv_obj_set_size(box, TH_W, TH_H);
        lv_obj_set_style_bg_color(box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        m->img[i] = lv_image_create(box);
        lv_obj_add_flag(m->img[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(m->img[i], LV_OBJ_FLAG_HIDDEN);
        m->note[i] = bz_label(box, "…", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_center(m->note[i]);
        lv_obj_t *cap = bz_label_line(t, m->files[i].name, BZ_F_CAPTION, BZ_C_DIM, TH_W - 20);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, 10, -8);
        bz_on_tap(t, ph_view, (void *)(intptr_t)i);
    }
    if (!PH.n) {
        lv_obj_t *l = bz_label(PH.vs.content,
                               "No JPEGs yet. Put pictures in CATOS/PHOTOS on the card, or take snapshots in the lens app.",
                               BZ_F_BODY, BZ_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, W - 2 * PAD);
    }
    /* room to scroll the last row clear of the bottom edge */
    lv_obj_t *sp = bz_box(PH.vs.content);
    lv_obj_set_height(sp, 20);
    PH.grid_built = true;
    vs_top(&PH.vs);
}

static void photos_build(lv_obj_t *b)
{
    PH.mem = lv_malloc_zeroed(sizeof *PH.mem);
    lv_obj_t *hr = head_right(b);
    PH.count = bz_label_line(hr, "", BZ_F_LABEL, BZ_C_DIM, 200);
    ui_button(hr, BZ_I_REFRESH, "rescan", ph_rescan, NULL);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    vs_init(&PH.vs, wrap, W - 2 * PAD, H - APP_Y, 0);

    PH.viewer = bz_box(b);
    lv_obj_set_size(PH.viewer, W, H);
    lv_obj_set_style_bg_color(PH.viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(PH.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(PH.viewer, LV_OBJ_FLAG_CLICKABLE);
    PH.view_img = lv_image_create(PH.viewer);
    lv_obj_add_flag(PH.view_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(PH.view_img);
    PH.view_msg = bz_label(PH.viewer, "", BZ_F_BODY, BZ_C_DIM);
    lv_label_set_long_mode(PH.view_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(PH.view_msg, 900);
    lv_obj_set_style_text_align(PH.view_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(PH.view_msg);
    PH.view_cap = bz_label_line(PH.viewer, "", BZ_F_LABEL, BZ_C_DIM, W - 2 * PAD);
    lv_obj_set_style_text_align(PH.view_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(PH.view_cap, LV_ALIGN_BOTTOM_MID, 0, -16);
    bz_on_tap(PH.viewer, ph_view_close, NULL);
    lv_obj_add_flag(PH.viewer, LV_OBJ_FLAG_HIDDEN);
}

static void photos_open(void)
{
    if (!PH.mem) return;
    PH.show = PH.show_req;
    PH.show_req = false;
    ph_scan();
}

void ui_photos_slideshow(lv_obj_t *from)
{
    PH.show_req = true;
    ui_app_open(&APP_PHOTOS, from);
}

static void photos_close(void)
{
    if (!PH.mem) return;
    PH.quit = true;
    PH.rescan = false;
    ph_view_close(NULL, NULL);
    PH.show = false;
    ph_drop_grid();
    if (!PH.running) {
        ph_free_thumbs(); /* else the worker frees them as it stops */
        PH.full_state = 0;
    }
}

static void ph_show_full(void)
{
    if (PH.full_state == 2) {
        hal_picture_t p = PH.full_got;
        PH.full_got = (hal_picture_t){ 0 };
        PH.full_state = 0;
        if (!PH.viewing) {
            hal_picture_free(&p);
            return;
        }
        ph_drop_view(); /* the slideshow's previous picture, now that the next is ready */
        PH.shown = p;
        PH.show_next = hal_seconds() + SHOW_S;
        PH.full_dsc = (lv_image_dsc_t){ 0 };
        PH.full_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        PH.full_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        PH.full_dsc.header.w = (uint32_t)p.w;
        PH.full_dsc.header.h = (uint32_t)p.h;
        PH.full_dsc.header.stride = (uint32_t)p.w * 2;
        PH.full_dsc.data = (const uint8_t *)p.px;
        PH.full_dsc.data_size = (uint32_t)(p.w * p.h * 2);
        lv_image_cache_drop(&PH.full_dsc);
        lv_image_set_src(PH.view_img, &PH.full_dsc);
        lv_obj_center(PH.view_img);
        lv_obj_remove_flag(PH.view_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(PH.view_msg, LV_OBJ_FLAG_HIDDEN);
        if (!PH.show)
            ui_text(PH.view_cap, "%s · %d \xc3\x97 %d · tap to close", PH.mem->files[PH.full_idx].name, p.src_w, p.src_h);
    } else if (PH.full_state == 3) {
        PH.full_state = 0;
        if (!PH.viewing) return;
        if (PH.show) { /* a picture that won't decode: the next one soon */
            PH.show_next = hal_seconds() + 1.0;
            return;
        }
        char msg[160];
        snprintf(msg, sizeof msg, "%s\ncouldn't be shown: %s", PH.mem->files[PH.full_idx].name, PH.full_err);
        lv_label_set_text(PH.view_msg, msg);
    }
}

static void photos_refresh(void)
{
    if (!PH.mem) return;
    if (PH.stage == 2 && PH.show) {
        PH.stage = 0;
        if (PH.n) {
            ph_view(NULL, (void *)(intptr_t)0);
        } else {
            ui_island_say(BZ_I_CAMERA, "no photos on the card for the slideshow");
            ui_app_close();
        }
    } else if (PH.stage == 2) {
        PH.stage = 0;
        ph_build_grid();
    } else if (PH.stage == 3 && PH.show) {
        PH.stage = 0;
        ui_app_close();
    } else if (PH.stage == 3) {
        PH.stage = 0;
        ph_drop_grid();
        bz_label(PH.vs.content, "No microSD card.", BZ_F_BODY, BZ_C_DIM);
        ui_text(PH.count, "no card");
    }
    if (PH.rescan && !PH.running) ph_scan();
    /* a full-screen request made while the worker was finishing */
    if (PH.full_state == 1 && !PH.running && !ph_spawn()) PH.full_state = 3;
    ph_show_full();
    /* the slideshow's next picture (not while the screen is off: nobody is watching) */
    if (PH.show && PH.viewing && PH.full_state == 0 && PH.n && hal_seconds() >= PH.show_next && !ui_asleep()) {
        PH.show_next = hal_seconds() + SHOW_S; /* whatever happens, not again straight away */
        ph_view(NULL, (void *)(intptr_t)((PH.full_idx + 1) % PH.n));
    }
    if (!PH.grid_built) return;
    ph_mem_t *m = PH.mem;
    int done = 0;
    for (int i = 0; i < PH.n; i++) {
        uint8_t st = m->tstate[i];
        done += st != 0;
        if (!st || m->shown[i]) continue;
        m->shown[i] = 1;
        if (st == 2) {
            lv_label_set_text(m->note[i], "can't decode");
            continue;
        }
        hal_picture_t *p = &m->thumb[i];
        lv_image_dsc_t *d = &m->dsc[i];
        *d = (lv_image_dsc_t){ 0 };
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = (uint32_t)p->w;
        d->header.h = (uint32_t)p->h;
        d->header.stride = (uint32_t)p->w * 2;
        d->data = (const uint8_t *)p->px;
        d->data_size = (uint32_t)(p->w * p->h * 2);
        lv_image_cache_drop(d);
        lv_image_set_src(m->img[i], d);
        lv_obj_center(m->img[i]);
        lv_obj_remove_flag(m->img[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m->note[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (done < PH.n) ui_text(PH.count, "%d photo%s · decoding %d", PH.n, PH.n == 1 ? "" : "s", PH.n - done);
    else ui_text(PH.count, "%d photo%s%s", PH.n, PH.n == 1 ? "" : "s", PH.n == PH_MAX ? " (newest)" : "");
}

static void photos_frame(double now, double dt)
{
    (void)now; (void)dt;
    vs_tick(&PH.vs);
}

const ui_app_t APP_PHOTOS = { .name = "photos", .icon = BZ_I_CAMERA, .build = photos_build, .open = photos_open,
                              .close = photos_close, .refresh = photos_refresh, .frame = photos_frame };

/* ================================================================== storage */

#define SG_ROWS 14

typedef struct {
    char name[40];
    cstore_usage_t u;
    bool os;             /* one of CATOS's own folders */
} sg_row_t;

static OS_BSS struct {
    sg_row_t *rows;      /* PSRAM */
    int n;
    uint64_t total, free_b, loose;
    int loose_files;
    bool have_space;
    volatile int state;  /* 0 idle, 1 counting, 2 counted, 3 no card */
    lv_obj_t *total_lbl, *free_lbl, *meter, *list, *note;
    int list_w;
} SG;

static void *sg_worker(void *u)
{
    (void)u;
    const char *sd = hal_sd_root();
    if (!sd) {
        SG.state = 3;
        return NULL;
    }
    cstore_layout();
    SG.have_space = hal_sd_space(&SG.total, &SG.free_b);
    int n = 0;
    char path[160];
    for (int i = 0; i < CS_NDIRS && n < SG_ROWS; i++) {
        sg_row_t *r = &SG.rows[n++];
        snprintf(r->name, sizeof r->name, "CATOS/%s", CS_DIR_NAME[i]);
        r->os = true;
        if (cstore_path((cs_dir_t)i, NULL, path, sizeof path)) cstore_usage(path, &r->u);
        else memset(&r->u, 0, sizeof r->u);
    }
    /* everything else at the card's top: its folders one by one, loose files together */
    SG.loose = 0;
    SG.loose_files = 0;
    cstore_entry_t *top = malloc(64 * sizeof *top);
    int k = top ? cstore_list(sd, NULL, false, top, 64) : -1;
    for (int i = 0; i < k; i++) {
        if (!top[i].dir) {
            SG.loose += top[i].size;
            SG.loose_files++;
            continue;
        }
        if (!strcasecmp(top[i].name, "CATOS") || n >= SG_ROWS) continue;
        sg_row_t *r = &SG.rows[n++];
        snprintf(r->name, sizeof r->name, "%s", top[i].name);
        r->os = false;
        snprintf(path, sizeof path, "%s/%s", sd, top[i].name);
        cstore_usage(path, &r->u);
    }
    free(top);
    SG.n = n;
    PUBLISH(SG.state, 2);
    return NULL;
}

static void sg_start(void)
{
    if (SG.state == 1) return;
    if (!SG.rows) SG.rows = lv_malloc_zeroed(SG_ROWS * sizeof *SG.rows);
    if (!SG.rows) return;
    SG.state = 1;
    ui_text(SG.note, "adding up the card…");
    if (!hal_thread("storage", sg_worker, NULL, 6144)) {
        SG.state = 0;
        ui_text(SG.note, "couldn't start");
    }
}

static void sg_rescan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    sg_start();
}

static void sg_show(void)
{
    char a[24], b[24], c[24];
    lv_obj_clean(SG.list);
    if (SG.state == 3) {
        ui_text(SG.total_lbl, "no card");
        ui_text(SG.free_lbl, " ");
        bz_meter_set(SG.meter, 0, BZ_C_ICE);
        ui_text(SG.note, "Put a microSD card in the slot on the tablet's edge.");
        return;
    }
    uint64_t used = SG.have_space && SG.total > SG.free_b ? SG.total - SG.free_b : 0;
    if (SG.have_space) {
        ui_text(SG.total_lbl, "%s", cstore_size(used, a, sizeof a));
        ui_text(SG.free_lbl, "%s free of %s", cstore_size(SG.free_b, c, sizeof c), cstore_size(SG.total, b, sizeof b));
        float f = SG.total ? (float)((double)used / (double)SG.total) : 0;
        bz_meter_set(SG.meter, f, f > 0.9f ? BZ_C_WARN : BZ_C_ICE);
    } else {
        ui_text(SG.total_lbl, "—");
        ui_text(SG.free_lbl, "the card didn't say its size");
    }
    uint64_t biggest = 1;
    for (int i = 0; i < SG.n; i++) if (SG.rows[i].u.bytes > biggest) biggest = SG.rows[i].u.bytes;
    int lw = SG.list_w;
    for (int i = 0; i < SG.n; i++) {
        const sg_row_t *r = &SG.rows[i];
        lv_obj_t *t = bz_tile(SG.list, lw, 84);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(t, 12, 0);
        lv_obj_set_style_pad_hor(t, 20, 0);
        bz_icon(t, r->os ? BZ_I_FOLDER_OPEN : BZ_I_FOLDER, 24, r->os ? BZ_C_ICE : BZ_C_DIM);
        lv_obj_t *nm = bz_label_line(t, r->name, BZ_F_BODY, BZ_C_INK, 360);
        lv_obj_set_pos(nm, 40, 0);
        lv_obj_t *sz = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
        ui_text(sz, "%s", cstore_size(r->u.bytes, a, sizeof a));
        lv_obj_align(sz, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *meta = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, 300);
        ui_text(meta, "%d file%s%s", r->u.files, r->u.files == 1 ? "" : "s", r->u.truncated ? " · partial count" : "");
        lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 40, 0);
        lv_obj_t *bar = bz_meter(t, lw - 40 - 400, 8);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
        bz_meter_set(bar, (float)((double)r->u.bytes / (double)biggest), r->os ? BZ_C_ICE : BZ_C_DIM);
    }
    if (SG.loose_files) {
        lv_obj_t *l = bz_label(SG.list, "", BZ_F_CAPTION, BZ_C_DIM);
        ui_text(l, "%d loose file%s at the top of the card · %s", SG.loose_files, SG.loose_files == 1 ? "" : "s",
                cstore_size(SG.loose, a, sizeof a));
    }
    ui_text(SG.note, "CATOS holds Catalyst OS's documents, photos, audio, app data and logs. The other folders "
                     "belong to the robot tools: lens clips, recordings, logs.");
}

static void storage_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    ui_button(hr, BZ_I_REFRESH, "recount", sg_rescan, NULL);
    int lw = 400, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = column_tile(b, PAD, lw);
    bz_icon(t, BZ_I_SD_CARD, 40, BZ_C_ICE);
    lv_obj_t *cap = bz_label(t, "microSD", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(cap, 56, 12);
    /* the value size, not display: "320.0 kb" at 76 px ran past the tile's 352 */
    SG.total_lbl = bz_label(t, "—", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(SG.total_lbl, 0, 84);
    SG.free_lbl = bz_label_line(t, "", BZ_F_BODY, BZ_C_DIM, IN(lw));
    lv_obj_set_pos(SG.free_lbl, 0, 164);
    SG.meter = bz_meter(t, IN(lw), 14);
    lv_obj_set_pos(SG.meter, 0, 206);
    SG.note = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_label_set_long_mode(SG.note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(SG.note, IN(lw));
    lv_obj_set_pos(SG.note, 0, 250);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD + lw + BZ_GAP, APP_Y);
    SG.list_w = rw;
    SG.list = ui_scroller(wrap, rw, APP_H);
}

static void storage_open(void) { sg_start(); }

static void storage_refresh(void)
{
    if (SG.state >= 2) {
        sg_show();
        SG.state = 0;
    }
}

const ui_app_t APP_STORAGE = { .name = "storage", .icon = BZ_I_SD_CARD, .build = storage_build, .open = storage_open,
                               .refresh = storage_refresh };
