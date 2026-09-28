/* The owner's look preferences (settings > look): the clock's hours, the battery's percent, the dock's labels,
 * the apps on the home page's quick-launch row. Each one kv key, read once, written when it changes; whoever
 * shows one reads it here (ui_look()) and is told to apply it by the settings pane, never by polling. */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

static ui_look_t g_look;
static bool g_loaded;

/* the quick-launch row as it shipped: what's reached for most in the pit */
static const char *const QUICK_DEFAULT[] = { "preflight", "checklist", "timer", "batteries", "notes", "assist" };

static void quick_parse(const char *s)
{
    g_look.quick_gen++;
    g_look.nquick = 0;
    while (*s && g_look.nquick < UI_QUICK_MAX) {
        const char *e = strchr(s, ',');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n > 0 && n < sizeof g_look.quick[0]) {
            char name[sizeof g_look.quick[0]];
            memcpy(name, s, n);
            name[n] = 0;
            /* only what the library still has */
            for (int i = 0; i < ui_app_count(); i++)
                if (!strcmp(ui_app_entry(i)->label, name)) {
                    snprintf(g_look.quick[g_look.nquick++], sizeof g_look.quick[0], "%s", name);
                    break;
                }
        }
        if (!e) break;
        s = e + 1;
    }
}

static void look_load(void)
{
    g_loaded = true;
    char buf[160];
    g_look.h24 = true;
    if (hal_kv_get("clock24", buf, sizeof buf)) g_look.h24 = buf[0] == '1';
    g_look.batt_pct = true;
    if (hal_kv_get("battpct", buf, sizeof buf)) g_look.batt_pct = buf[0] == '1';
    g_look.dock_labels = true;
    if (hal_kv_get("docklbl", buf, sizeof buf)) g_look.dock_labels = buf[0] == '1';
    if (hal_kv_get("quick", buf, sizeof buf)) quick_parse(buf);
    else {
        for (int i = 0; i < (int)(sizeof QUICK_DEFAULT / sizeof QUICK_DEFAULT[0]); i++)
            snprintf(g_look.quick[i], sizeof g_look.quick[0], "%s", QUICK_DEFAULT[i]);
        g_look.nquick = (int)(sizeof QUICK_DEFAULT / sizeof QUICK_DEFAULT[0]);
    }
    g_look.tone_auto = hal_kv_get("toneauto", buf, sizeof buf) && buf[0] == '1';
    g_look.ground = 0;
    if (hal_kv_get("ground", buf, sizeof buf))
        for (int i = 0; i < UI_GROUND_COUNT; i++)
            if (!strcmp(buf, ui_ground_name(i))) g_look.ground = i;
}

const ui_look_t *ui_look(void)
{
    if (!g_loaded) look_load();
    return &g_look;
}

ui_look_t *ui_look_edit(void)
{
    if (!g_loaded) look_load();
    return &g_look;
}

void ui_look_save(void)
{
    hal_kv_set("clock24", g_look.h24 ? "1" : "0");
    hal_kv_set("battpct", g_look.batt_pct ? "1" : "0");
    hal_kv_set("docklbl", g_look.dock_labels ? "1" : "0");
    char q[UI_QUICK_MAX * 17 + 1] = "";
    size_t o = 0;
    for (int i = 0; i < g_look.nquick && o < sizeof q - 1; i++)
        o += (size_t)snprintf(q + o, sizeof q - o, "%s%s", i ? "," : "", g_look.quick[i]);
    hal_kv_set("quick", q);
    hal_kv_set("ground", ui_ground_name(g_look.ground));
    hal_kv_set("toneauto", g_look.tone_auto ? "1" : "0");
}

/* The tone on a schedule: light from 7 am, dark from 7 pm. Looked at twice a minute from the 10 Hz refresh; a
 * change is the one full redraw a tone change always is, and it waits while anything slides or the screen sleeps. */
static void tone_watch(void *u)
{
    (void)u;
    ui_look_tone_tick(false);
}

void ui_look_tone_tick(bool now)
{
    static double last;
    static bool hooked;
    if (!hooked) {
        hooked = true;
        ui_on_refresh(tone_watch, NULL);
    }
    if (!ui_look()->tone_auto) return;
    double t = ui_now();
    if (!now && t - last < 30) return;
    if (!now && (ui_asleep() || bz_ui_sliding() || bz_ui_sheeting())) return;
    last = t;
    time_t wall = time(NULL);
    struct tm tm;
    localtime_r(&wall, &tm);
    if (tm.tm_year <= 120) return; /* no clock yet: leave the tone as it is */
    bool dark = tm.tm_hour >= 19 || tm.tm_hour < 7;
    if (dark != S.dark) ui_set_tone(dark, S.calm);
}

bool ui_look_is_quick(const char *label)
{
    const ui_look_t *l = ui_look();
    for (int i = 0; i < l->nquick; i++)
        if (!strcmp(l->quick[i], label)) return true;
    return false;
}

/* The dev console's "look <what> <value>": the look settings without the settings app, for screenshots.
 * ground plain|glow|aurora|grain|contour · icons colour|tinted|mono · tone dark|light · h24 0|1 · pct 0|1 ·
 * labels 0|1 · quick a,b,c (library labels) */
void ui_look_dev(const char *args)
{
    char what[16] = "", val[128] = "";
    if (sscanf(args, "%15s %127[^\n]", what, val) < 2) return;
    ui_look_t *l = ui_look_edit();
    if (!strcmp(what, "ground")) {
        for (int i = 0; i < UI_GROUND_COUNT; i++)
            if (!strcmp(val, ui_ground_name(i))) ui_ground_set(i);
    } else if (!strcmp(what, "icons")) {
        static const char *const ST[UI_ICON_NSTYLES] = { "colour", "tinted", "mono" };
        for (int i = 0; i < UI_ICON_NSTYLES; i++)
            if (!strcmp(val, ST[i])) ui_icon_set_style(i);
    } else if (!strcmp(what, "tone")) {
        ui_set_tone(strcmp(val, "light") != 0, S.calm);
    } else if (!strcmp(what, "h24") || !strcmp(what, "pct") || !strcmp(what, "labels")) {
        bool on = val[0] == '1';
        if (!strcmp(what, "h24")) l->h24 = on;
        else if (!strcmp(what, "pct")) l->batt_pct = on;
        else l->dock_labels = on;
        ui_look_save();
        ui_status_look_changed();
    } else if (!strcmp(what, "mem")) {
        /* "look mem 1": what the look holds in PSRAM */
        printf("LOOK icons %u bytes (style %d), ground %u bytes (%s)\n", (unsigned)ui_icon_bytes(), ui_icon_style(),
               (unsigned)ui_ground_bytes(), ui_ground_name(l->ground));
    } else if (!strcmp(what, "quick")) {
        quick_parse(val);
        ui_look_save();
        ui_home_quick_changed();
    }
}

void ui_fmt_hm(char *out, size_t n, const struct tm *tm)
{
    if (ui_look()->h24) snprintf(out, n, "%d:%02d", tm->tm_hour, tm->tm_min);
    else snprintf(out, n, "%d:%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min);
}

/* ------------------------------------------------------------------ settings > look */

/* The choices after the tone and the accent: the icons' style, the ground, the home page's quick-launch row,
 * and the status band and dock. Each is a picture of itself where it can be (the three icon styles side by side,
 * the grounds as small screens), ringed in ink when chosen, as the accent swatches are. */

#define THUMB_W 128
#define THUMB_H 72
#define FAV_ICON 64

static struct {
    lv_obj_t *style_tile[UI_ICON_NSTYLES];
    lv_obj_t *ground_tile[UI_GROUND_COUNT];
    lv_image_dsc_t thumb_dsc[UI_GROUND_COUNT];
    uint16_t *thumbs;
    lv_obj_t *fav_cell[64], *fav_badge[64], *fav_num[64];
    int nfav;
    lv_obj_t *fav_state;
    lv_obj_t *h24, *pct, *labels;
    bool hooked;
} LS;

static void ring(lv_obj_t *t, bool on)
{
    bz_tile_set_fill(t, on ? BZ_C_SURFACE3 : BZ_C_SURFACE2);
    /* every option has the ring, the unpicked ones clear: a border only on the picked one would move its
     * contents off their neighbours' line */
    if (lv_obj_get_style_border_width(t, 0) != 3) {
        lv_obj_set_style_border_width(t, 3, 0);
        lv_obj_set_style_border_color(t, bz_lv(BZ_C_INK), 0);
    }
    lv_opa_t want = on ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_border_opa(t, 0) != want) lv_obj_set_style_border_opa(t, want, 0);
}

static void thumbs_bake(void)
{
    if (!LS.thumbs) return;
    for (int g = 0; g < UI_GROUND_COUNT; g++) {
        uint16_t *px = LS.thumbs + (size_t)g * THUMB_W * THUMB_H;
        ui_ground_thumb(g, px, THUMB_W, THUMB_H);
        lv_image_dsc_t *d = &LS.thumb_dsc[g];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = THUMB_W;
        d->header.h = THUMB_H;
        d->header.stride = THUMB_W * 2;
        d->data_size = THUMB_W * THUMB_H * 2;
        d->data = (const uint8_t *)px;
    }
}

/* the tone or the accent changed: the thumbnails are pictures of the ground in them */
static void look_theme_changed(void) { thumbs_bake(); }

static void fav_show(void)
{
    const ui_look_t *l = ui_look();
    for (int i = 0; i < LS.nfav; i++) {
        const ui_app_entry_t *e = ui_app_entry(i);
        int at = -1;
        for (int k = 0; k < l->nquick; k++)
            if (!strcmp(l->quick[k], e->label)) at = k;
        if (at >= 0) {
            lv_obj_remove_flag(LS.fav_badge[i], LV_OBJ_FLAG_HIDDEN);
            ui_text(LS.fav_num[i], "%d", at + 1);
        } else {
            lv_obj_add_flag(LS.fav_badge[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_text(LS.fav_state, "%d of %d chosen · they sit along the foot of the home page, in the order picked",
            l->nquick, UI_QUICK_MAX);
}

void ui_look_settings_open(void)
{
    if (!LS.fav_state) return;
    const ui_look_t *l = ui_look();
    for (int i = 0; i < UI_ICON_NSTYLES; i++) ring(LS.style_tile[i], i == ui_icon_style());
    for (int i = 0; i < UI_GROUND_COUNT; i++) ring(LS.ground_tile[i], i == l->ground);
    ui_chip_set(LS.h24, l->h24);
    ui_chip_set(LS.pct, l->batt_pct);
    ui_chip_set(LS.labels, l->dock_labels);
    fav_show();
}

static void style_tap(lv_obj_t *o, void *u)
{
    (void)o;
    ui_icon_set_style((int)(intptr_t)u);
    ui_look_settings_open();
}

static void ground_tap(lv_obj_t *o, void *u)
{
    (void)o;
    ui_ground_set((int)(intptr_t)u);
    ui_look_settings_open();
}

static void fav_tap(lv_obj_t *o, void *u)
{
    (void)o;
    const ui_app_entry_t *e = ui_app_entry((int)(intptr_t)u);
    ui_look_t *l = ui_look_edit();
    int at = -1;
    for (int k = 0; k < l->nquick; k++)
        if (!strcmp(l->quick[k], e->label)) at = k;
    if (at >= 0) {
        for (int k = at; k < l->nquick - 1; k++) memcpy(l->quick[k], l->quick[k + 1], sizeof l->quick[0]);
        l->nquick--;
    } else if (l->nquick < UI_QUICK_MAX) {
        snprintf(l->quick[l->nquick++], sizeof l->quick[0], "%s", e->label);
    } else {
        ui_island_say(BZ_I_STAR, "six at most: take one off first");
        return;
    }
    l->quick_gen++;
    ui_look_save();
    ui_home_quick_changed();
    fav_show();
}

static void toggle_tap(lv_obj_t *o, void *u)
{
    (void)o;
    ui_look_t *l = ui_look_edit();
    int k = (int)(intptr_t)u;
    if (k == 0) l->h24 = !l->h24;
    else if (k == 1) l->batt_pct = !l->batt_pct;
    else l->dock_labels = !l->dock_labels;
    ui_look_save();
    ui_status_look_changed();
    ui_look_settings_open();
}

static lv_obj_t *option(lv_obj_t *row, int w, int h, const char *name, bz_tap_fn fn, int i)
{
    lv_obj_t *t = bz_tile(row, w, h);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_style_radius(t, 24, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 10, 0);
    bz_on_tap(t, fn, (void *)(intptr_t)i);
    (void)name;
    return t;
}

static lv_obj_t *wrap_row(lv_obj_t *col, int w, int gap)
{
    lv_obj_t *r = bz_row(col, gap);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, gap, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *caption(lv_obj_t *col, const char *text, int w)
{
    lv_obj_t *c = bz_label(col, text, BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(c, w);
    return c;
}

void ui_look_settings(lv_obj_t *col, int w)
{
    if (!LS.hooked) {
        LS.hooked = true;
        bz_theme_on_change(look_theme_changed);
    }
    /* the icons: each style as two icons in it */
    bz_label(col, "icons", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = wrap_row(col, w, 12);
    static const char *const STYLE_L[UI_ICON_NSTYLES] = { "colour", "tinted", "mono" };
    static const char *const SAMPLE[2] = { "preflight", "assist" };
    for (int s = 0; s < UI_ICON_NSTYLES; s++) {
        lv_obj_t *t = option(r, 176, 118, STYLE_L[s], style_tap, s);
        lv_obj_t *ic = bz_row(t, 10);
        for (int k = 0; k < 2; k++) ui_icon_preview(ic, SAMPLE[k], 48, s);
        bz_label(t, STYLE_L[s], BZ_F_CAPTION, BZ_C_INK);
        LS.style_tile[s] = t;
    }
    caption(col, "Colour gives every app its own. Tinted draws them all in the accent, the way Material You themes "
                 "icons; mono is the surface and the glyph.", w);

    /* the ground: each as a small screen */
    bz_label(col, "ground", BZ_F_LABEL, BZ_C_DIM);
    if (!LS.thumbs) LS.thumbs = lv_malloc((size_t)UI_GROUND_COUNT * THUMB_W * THUMB_H * 2);
    thumbs_bake();
    r = wrap_row(col, w, 12);
    for (int g = 0; g < UI_GROUND_COUNT; g++) {
        lv_obj_t *t = option(r, THUMB_W + 24, THUMB_H + 58, ui_ground_name(g), ground_tap, g);
        lv_obj_t *img = lv_image_create(t);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        if (LS.thumbs) lv_image_set_src(img, &LS.thumb_dsc[g]);
        lv_obj_set_size(img, THUMB_W, THUMB_H);
        /* a little screen: its corners rounded like the tablet's */
        lv_obj_set_style_radius(img, 10, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        bz_label(t, ui_ground_name(g), BZ_F_CAPTION, BZ_C_INK);
        LS.ground_tile[g] = t;
    }
    caption(col, "Behind the pages, made from the tone and the accent. Lists keep their own surface over it, and apps "
                 "open on plain ground.", w);

    /* the home page's quick-launch row */
    bz_label(col, "home page · quick launch", BZ_F_LABEL, BZ_C_DIM);
    LS.fav_state = caption(col, "", w);
    r = wrap_row(col, w, 0);
    int cols = w / 108;
    int cw = w / (cols > 0 ? cols : 1);
    LS.nfav = ui_app_count() < 64 ? ui_app_count() : 64;
    for (int i = 0; i < LS.nfav; i++) {
        const ui_app_entry_t *e = ui_app_entry(i);
        lv_obj_t *c = bz_col(r, 6);
        lv_obj_set_width(c, cw);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(c, 8, 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_add_style(c, bz_style_fill(BZ_C_SURFACE3), LV_STATE_PRESSED);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *holder = bz_box(c);
        lv_obj_set_size(holder, FAV_ICON + 12, FAV_ICON + 6);
        lv_obj_t *ic = ui_app_icon(holder, e->app, FAV_ICON);
        lv_obj_align(ic, LV_ALIGN_BOTTOM_MID, 0, 0);
        /* chosen: a badge on the icon's shoulder with its place in the row */
        lv_obj_t *b = bz_box(holder);
        lv_obj_set_size(b, 28, 28);
        lv_obj_add_style(b, bz_style_fill(BZ_C_ICE), 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *n = bz_label(b, "", BZ_F_CAPTION, BZ_C_ON_ICE);
        lv_obj_center(n);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *lb = bz_label_line(c, e->label, BZ_F_CAPTION, BZ_C_INK, cw - 8);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        bz_on_tap(c, fav_tap, (void *)(intptr_t)i);
        LS.fav_cell[i] = c;
        LS.fav_badge[i] = b;
        LS.fav_num[i] = n;
    }

    /* the status band and the dock */
    bz_label(col, "status band and dock", BZ_F_LABEL, BZ_C_DIM);
    r = wrap_row(col, w, 10);
    LS.h24 = ui_chip(r, "24-hour clock", toggle_tap, (void *)(intptr_t)0);
    LS.pct = ui_chip(r, "battery percent", toggle_tap, (void *)(intptr_t)1);
    LS.labels = ui_chip(r, "dock labels", toggle_tap, (void *)(intptr_t)2);
    ui_look_settings_open();
}

void ui_look_boot(void)
{
    ui_ground_init();
    ui_look_tone_tick(true);
}
