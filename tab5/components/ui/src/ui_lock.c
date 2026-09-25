/* The shell's own OS pieces: the notification store, and the lock screen.
 *
 * Notifications: every island message is kept (the last NOTE_MAX, newest first) with the time it came,
 * so what flashed past while you weren't looking is still there: the control center lists them, the
 * lock screen shows the latest.
 *
 * Lock screen: when the screen goes to sleep, it goes under a lock screen — the time, the date, the
 * tablet's and the robot's state, the latest notifications — and wakes to it. A push up anywhere lifts
 * it away: a top sheet the platform slides in hardware (bz_ui_sheet_*), following the finger, with the
 * page drawn under it a band at a time. Not a security lock: it is there so a tablet on a belt or a desk
 * doesn't act on the first brush of a hand. */
#include "ui_internal.h"
#include "ui_home_mode.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================== notifications */

#define NOTE_MAX 16

static struct {
    ui_note_t n[NOTE_MAX];
    int head, count;
    unsigned gen;
} NT;

void ui_notify_add(const char *icon, const char *text)
{
    if (!text || !text[0]) return;
    ui_note_t *n = &NT.n[NT.head];
    n->icon = icon ? icon : BZ_I_INFO;
    snprintf(n->text, sizeof n->text, "%s", text);
    n->at = time(NULL);
    n->mono = hal_seconds();
    NT.head = (NT.head + 1) % NOTE_MAX;
    if (NT.count < NOTE_MAX) NT.count++;
    NT.gen++;
}

int ui_notify_count(void) { return NT.count; }
unsigned ui_notify_gen(void) { return NT.gen; }

const ui_note_t *ui_notify_get(int i)
{
    if (i < 0 || i >= NT.count) return NULL;
    return &NT.n[(NT.head - 1 - i + NOTE_MAX) % NOTE_MAX];
}

void ui_notify_clear(void)
{
    NT.count = 0;
    NT.gen++;
}

/* "now", "4 min", "2 h", or the clock time for anything older */
void ui_notify_age(const ui_note_t *n, char *out, size_t len)
{
    double s = hal_seconds() - n->mono;
    if (s < 60) snprintf(out, len, "now");
    else if (s < 3600) snprintf(out, len, "%d min", (int)(s / 60));
    else if (s < 6 * 3600) snprintf(out, len, "%d h", (int)(s / 3600));
    else {
        struct tm tm;
        localtime_r(&n->at, &tm);
        strftime(out, len, "%H:%M", &tm);
    }
}

/* ================================================================== lock screen */

#define LK_NOTES 3
#define LK_SLOP 12

static struct {
    lv_obj_t *root, *clock, *date, *status, *hint, *notes[LK_NOTES], *note_icon[LK_NOTES], *note_text[LK_NOTES],
        *note_age[LK_NOTES];
    bool locked;      /* the lock screen is up (the real thing) */
    bool sheet;       /* a push or a settle under way: the platform shows a picture of it */
    bool dragging;
    int y0;
    float vy, last_y;
    double last_t;
    bz_motion_t k;    /* 1 = covering the screen, 0 = gone */
    unsigned seen_gen;
    int last_min;
} LK;

static void lock_text(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_min != LK.last_min) {
        LK.last_min = tm.tm_min;
        char b[48];
        strftime(b, sizeof b, "%H:%M", &tm);
        ui_text(LK.clock, "%s", b);
        strftime(b, sizeof b, "%A, %B %d", &tm);
        ui_text(LK.date, "%s", b);
    }
    hal_battery_t bt;
    const cat_robot_t *r = R;
    char batt[40] = "";
    if (hal_battery(&bt)) snprintf(batt, sizeof batt, "%d %%%s", bt.percent, bt.charging ? " \xc2\xb7 charging" : "");
    if (r->connected) ui_text(LK.status, "%s \xc2\xb7 robot %s \xc2\xb7 %s", batt, cat_mode_name(r), r->address);
    else ui_text(LK.status, "%s \xc2\xb7 looking for team %d", batt, S.team);
    if (LK.seen_gen == ui_notify_gen()) return;
    LK.seen_gen = ui_notify_gen();
    for (int i = 0; i < LK_NOTES; i++) {
        const ui_note_t *n = ui_notify_get(i);
        if (!n) {
            lv_obj_add_flag(LK.notes[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char age[16];
        ui_notify_age(n, age, sizeof age);
        lv_label_set_text(LK.note_icon[i], n->icon);
        ui_text(LK.note_text[i], "%s", n->text);
        ui_text(LK.note_age[i], "%s", age);
        lv_obj_remove_flag(LK.notes[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void lock_visible(bool on)
{
    if (on) {
        lv_obj_move_foreground(LK.root);
        lv_obj_remove_flag(LK.root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(LK.root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* drawing the page under the lock as it lifts: the lock out of the way, then back */
static void lock_prep(bool before, void *u)
{
    (void)u;
    lock_visible(before ? false : LK.locked);
}

void ui_lock_show(void)
{
    if (!S.lock || LK.locked || !LK.root) return;
    LK.locked = true;
    LK.seen_gen = ~0u;
    LK.last_min = -1;
    lock_text();
    lock_visible(true);
    bz_motion_set(&LK.k, 1, 0);
}

void ui_lock_lift(void)
{
    if (!LK.locked || LK.sheet) return;
    LK.locked = false;
    lock_visible(false);
    bz_motion_set(&LK.k, 0, 0);
}

bool ui_locked(void) { return LK.locked; }

/* something covers the pages (the orb, which floats over them, stays out of its way) */
bool ui_overlay_up(void) { return LK.locked || ui_cc_is_open() || ui_home_mode_active(); }

static void lock_frame(double now, double dt, void *user)
{
    (void)dt; (void)user;
    if (!LK.locked) return;
    if (ui_asleep() || (ui_alarm_up() && !LK.sheet)) return; /* a push on the alarm screen isn't the lock's */
    int x0, y0, x, y;
    bool down = bz_ui_press(&x0, &y0, &x, &y);
    if (!LK.dragging && !LK.sheet && down && y0 - y > LK_SLOP && y0 - y > abs(x - x0)) {
        /* the push that lifts it: the press is the lock's, and the page under it is drawn as it shows */
        bz_ui_take_press();
        if (bz_ui_sheet_begin(false, H, false, lock_prep, NULL)) {
            LK.sheet = true;
            LK.dragging = true;
            LK.y0 = y0;
            LK.vy = 0;
            LK.last_y = (float)y;
            LK.last_t = now;
        } else {
            /* no platform sheet: it just goes */
            LK.locked = false;
            lock_visible(false);
            return;
        }
    }
    if (LK.dragging) {
        if (down) {
            double et = now - LK.last_t;
            if (et > 0.001) {
                float v = (float)((y - LK.last_y) / et);
                LK.vy += (v - LK.vy) * 0.5f;
                LK.last_y = (float)y;
                LK.last_t = now;
            }
            float k = 1 + (float)(y - LK.y0) / H;
            bz_motion_set(&LK.k, k < 0 ? 0 : k > 1 ? 1 : k, LK.vy / H);
        } else {
            LK.dragging = false;
            float v = LK.vy / H;
            float aim = LK.k.value + bz_project(v * 1000, BZ_RATE_FAST) / 1000;
            bz_motion_to_v(&LK.k, aim < 0.75f ? 0 : 1, BZ_RELEASE, v);
        }
    }
    if (LK.sheet) {
        bool moving = bz_motion_tick(&LK.k);
        float k = LK.k.value < 0 ? 0 : LK.k.value > 1 ? 1 : LK.k.value;
        bz_ui_keep_alive();
        if (moving || LK.dragging) {
            bz_ui_sheet((int)(k * H + 0.5f));
            return;
        }
        int rest = LK.k.target > 0.5f ? H : 0;
        if (bz_ui_sheet_shown() != rest) {
            bz_ui_sheet(rest);
            return;
        }
        LK.locked = rest > 0;
        lock_visible(LK.locked);
        bz_ui_sheet_end();
        LK.sheet = false;
        if (!LK.locked) hal_tone(1400, 8, S.volume * 0.3f);
    }
}

static void lock_refresh(void *u)
{
    (void)u;
    if (LK.locked && !LK.sheet) lock_text();
}

void ui_lock_init(void)
{
    lv_obj_t *g = bz_ui_glass();
    LK.root = lv_obj_create(g);
    lv_obj_remove_style_all(LK.root);
    lv_obj_set_size(LK.root, W, H);
    lv_obj_add_style(LK.root, bz_style_fill(BZ_C_GROUND), 0);
    lv_obj_add_flag(LK.root, LV_OBJ_FLAG_CLICKABLE); /* nothing under it takes a tap */
    lv_obj_remove_flag(LK.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(LK.root, LV_OBJ_FLAG_HIDDEN);

    LK.clock = bz_label(LK.root, "", BZ_F_CLOCK, BZ_C_INK);
    lv_obj_align(LK.clock, LV_ALIGN_TOP_MID, 0, 70);
    LK.date = bz_label(LK.root, "", BZ_F_NAME, BZ_C_DIM);
    lv_obj_align(LK.date, LV_ALIGN_TOP_MID, 0, 210);
    LK.status = bz_label_line(LK.root, "", BZ_F_LABEL, BZ_C_DIM, W - 2 * PAD);
    lv_obj_set_style_text_align(LK.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(LK.status, LV_ALIGN_TOP_MID, 0, 258);

    /* the latest notifications, as cards */
    int cw = 640;
    for (int i = 0; i < LK_NOTES; i++) {
        lv_obj_t *c = bz_tile(LK.root, cw, 64);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 316 + i * 74);
        lv_obj_set_style_pad_ver(c, 0, 0);
        lv_obj_set_style_radius(c, 22, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 14, 0);
        LK.note_icon[i] = bz_icon(c, BZ_I_INFO, 24, BZ_C_DIM);
        LK.note_text[i] = bz_label_line(c, "", BZ_F_BODY_S, BZ_C_INK, cw - 2 * BZ_PAD_TILE - 24 - 14 - 80 - 14);
        LK.note_age[i] = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_DIM, 80);
        lv_obj_set_style_text_align(LK.note_age[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        LK.notes[i] = c;
    }

    lv_obj_t *handle = lv_obj_create(LK.root);
    lv_obj_remove_style_all(handle);
    lv_obj_add_style(handle, bz_style_fill(BZ_C_FAINT), 0);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(handle, 120, 8);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -22);
    LK.hint = bz_label(LK.root, "push up to open", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(LK.hint, LV_ALIGN_BOTTOM_MID, 0, -44);

    bz_motion_init(&LK.k, 0, 0.001f);
    LK.k.keep = true;
    bz_ui_on_frame(lock_frame, NULL);
    ui_on_refresh(lock_refresh, NULL);
}
