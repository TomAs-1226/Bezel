/* Apps that use the tablet's own hardware: listen (mics), level (IMU), lens (camera), can tap (TWAI),
 * logs (microSD) and settings. */
#include "ui_internal.h"
#include "cat_can.h"
#include "cat_dsp.h"
#include "cat_logs.h"

#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)

static lv_obj_t *head_right(lv_obj_t *body)
{
    lv_obj_t *r = bz_row(body, 14);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -PAD, 18);
    lv_obj_set_height(r, 60);
    return r;
}

/* A big reading: value in a fixed, right-aligned box (so it never re-flows), unit and caption beside. */
typedef struct { lv_obj_t *label, *value, *unit, *caption; } reading_t;

static void reading(lv_obj_t *tile, reading_t *r, const char *label, bz_font_role_t f, const char *unit, int y)
{
    r->label = bz_label(tile, label, BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(r->label, 0, y);
    r->value = bz_label(tile, "\xe2\x80\x94", f, BZ_C_INK);
    lv_obj_set_pos(r->value, -4, y + 20);
    r->unit = bz_label(tile, unit ? unit : "", BZ_F_NAME, BZ_C_DIM);
    lv_obj_align_to(r->unit, r->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -12);
}

static void reading_set(reading_t *r, const char *fmt, double v, bool have)
{
    char b[32];
    ui_text(r->value, "%s", bz_fmt(b, sizeof b, have, fmt, v));
    lv_obj_align_to(r->unit, r->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -12);
}

/* ================================================================== listen */

#define LS_N 1024
#define LS_BARS 128
#define LS_MAX_HZ 4000.0f

static struct {
    int16_t ring[LS_N], block[LS_N];
    int head, fill;
    cat_spectrum_t sp;
    float bars[LS_BARS], hold[LS_BARS];
    bool frozen;
    int events;           /* events per revolution: teeth, blades, poles */
    float peak_hz, peak_smooth;
    lv_obj_t *spec, *frz, *ev_label;
    reading_t hz, rpm;
    lv_obj_t *level;
} LS;

static void listen_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    float bw = (float)w / LS_BARS;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 2;
    for (int i = 0; i < LS_BARS; i++) {
        float k = (LS.bars[i] + 90) / 80;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        int bh = (int)(k * (h - 24));
        lv_area_t r = { (int32_t)(a.x1 + i * bw + 1), a.y2 - 24 - bh, (int32_t)(a.x1 + (i + 1) * bw - 2), a.y2 - 24 };
        rd.bg_color = bz_lv(BZ_C_ICE);
        rd.bg_opa = LV_OPA_COVER;
        if (bh > 0) lv_draw_rect(layer, &rd, &r);
        float hk = (LS.hold[i] + 90) / 80;
        hk = hk < 0 ? 0 : hk > 1 ? 1 : hk;
        int hy = a.y2 - 24 - (int)(hk * (h - 24));
        lv_area_t hr = { r.x1, hy - 1, r.x2, hy + 1 };
        rd.bg_color = bz_lv(BZ_C_DIM);
        lv_draw_rect(layer, &rd, &hr);
    }
    /* the peak: one mark in signal, the focus of the screen */
    if (LS.peak_hz > 0 && LS.peak_hz < LS_MAX_HZ) {
        int x = a.x1 + (int)(LS.peak_hz / LS_MAX_HZ * w);
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = bz_lv(BZ_C_SIGNAL);
        ld.width = 3;
        ld.p1.x = x; ld.p1.y = a.y1;
        ld.p2.x = x; ld.p2.y = a.y2 - 24;
        lv_draw_line(layer, &ld);
    }
    /* frequency axis every 500 Hz */
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = bz_font(BZ_F_CAPTION);
    td.color = bz_lv(BZ_C_DIM);
    for (int f = 0; f <= 4000; f += 500) {
        char t[8];
        snprintf(t, sizeof t, f ? "%d" : "0 hz", f);
        td.text = t;
        td.text_local = 1;
        int x = a.x1 + (int)(f / LS_MAX_HZ * (w - 40));
        lv_area_t la = { x, a.y2 - 18, x + 60, a.y2 };
        lv_draw_label(layer, &td, &la);
    }
}

static void listen_frame(double now, double dt)
{
    (void)now; (void)dt;
    int16_t tmp[512];
    int rate = 16000, n;
    while ((n = hal_mic_read(tmp, 512, &rate)) > 0) {
        if (LS.frozen) continue;
        for (int i = 0; i < n; i++) {
            LS.ring[LS.head] = tmp[i];
            LS.head = (LS.head + 1) % LS_N;
        }
        LS.fill += n;
    }
    bz_ui_keep_alive();
    if (LS.frozen || LS.fill < LS_N / 4) return;
    LS.fill = 0;
    /* the newest LS_N samples, oldest first; a new spectrum every quarter block (75 % overlap) */
    for (int i = 0; i < LS_N; i++) LS.block[i] = LS.ring[(LS.head + i) % LS_N];
    cat_spectrum(LS.block, LS_N, rate, 60, &LS.sp);
    float hz_per_bin = (float)rate / LS_N;
    int per_bar = (int)(LS_MAX_HZ / hz_per_bin / LS_BARS + 0.5f);
    if (per_bar < 1) per_bar = 1;
    for (int b = 0; b < LS_BARS; b++) {
        float m = -120;
        for (int k = 0; k < per_bar; k++) {
            int bin = b * per_bar + k;
            if (bin < LS_N / 2 && LS.sp.db[bin] > m) m = LS.sp.db[bin];
        }
        LS.bars[b] += (m - LS.bars[b]) * 0.5f;
        LS.hold[b] = m > LS.hold[b] ? m : LS.hold[b] - 0.6f;
    }
    LS.peak_hz = LS.sp.peak_hz;
    LS.peak_smooth = LS.peak_smooth ? LS.peak_smooth + (LS.peak_hz - LS.peak_smooth) * 0.35f : LS.peak_hz;
    lv_obj_invalidate(LS.spec);
    reading_set(&LS.hz, "%.0f", LS.peak_smooth, true);
    reading_set(&LS.rpm, "%.0f", LS.peak_smooth * 60 / LS.events, true);
    float lvl = (LS.sp.rms_db + 70) / 70;
    bz_meter_set(LS.level, lvl, lvl > 0.9f ? BZ_C_WARN : BZ_C_ICE);
}

static void ls_freeze(lv_obj_t *o, void *u)
{
    (void)u;
    LS.frozen = !LS.frozen;
    ui_chip_set(o, LS.frozen);
}

static void ls_events(lv_obj_t *o, void *u)
{
    (void)o;
    LS.events += (int)(intptr_t)u;
    if (LS.events < 1) LS.events = 1;
    if (LS.events > 200) LS.events = 200;
    ui_text(LS.ev_label, "%d per rev", LS.events);
    hal_tone(2000, 8, S.volume * 0.4f);
}

static void listen_open(void)
{
    LS.fill = 0;
    for (int i = 0; i < LS_BARS; i++) LS.bars[i] = LS.hold[i] = -100;
}

static void listen_close(void) { hal_mic_stop(); }

static void listen_build(lv_obj_t *b)
{
    LS.events = 1;
    lv_obj_t *t = bz_tile(b, W - 2 * PAD, 330);
    lv_obj_set_pos(t, PAD, APP_Y);
    bz_label(t, "spectrum · 0–4 khz", BZ_F_LABEL, BZ_C_DIM);
    LS.level = bz_meter(t, 160, 10);
    lv_obj_align(LS.level, LV_ALIGN_TOP_RIGHT, 0, 4);
    LS.spec = bz_box(t);
    lv_obj_set_size(LS.spec, W - 2 * PAD - 2 * BZ_PAD_TILE, 260);
    lv_obj_set_pos(LS.spec, 0, 32);
    lv_obj_add_event_cb(LS.spec, listen_draw, LV_EVENT_DRAW_MAIN, NULL);

    int y = APP_Y + 330 + BZ_GAP, hgt = H - y - PAD;
    lv_obj_t *a = bz_tile(b, 400, hgt);
    lv_obj_set_pos(a, PAD, y);
    reading(a, &LS.hz, "loudest", BZ_F_DISPLAY, "hz", 0);
    lv_obj_t *c = bz_tile(b, 400, hgt);
    lv_obj_set_pos(c, PAD + 400 + BZ_GAP, y);
    reading(c, &LS.rpm, "shaft speed", BZ_F_DISPLAY, "rpm", 0);
    lv_obj_t *d = bz_tile(b, W - 2 * PAD - 800 - 2 * BZ_GAP, hgt);
    lv_obj_set_pos(d, PAD + 800 + 2 * BZ_GAP, y);
    bz_label(d, "events per revolution", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *row = bz_row(d, 10);
    lv_obj_set_pos(row, 0, 34);
    ui_button(row, BZ_I_REMOVE, NULL, ls_events, (void *)(intptr_t)-1);
    LS.ev_label = bz_label(row, "1 per rev", BZ_F_NAME, BZ_C_INK);
    ui_button(row, BZ_I_ADD, NULL, ls_events, (void *)(intptr_t)1);
    lv_obj_t *hint = bz_label(d, "teeth in the mesh, belt teeth, fan blades", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_pos(hint, 0, 104);
    LS.frz = ui_chip(d, "freeze", ls_freeze, NULL);
    lv_obj_align(LS.frz, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

const ui_app_t APP_LISTEN = { .name = "listen", .icon = BZ_I_GRAPHIC_EQ, .build = listen_build, .open = listen_open,
                              .close = listen_close, .frame = listen_frame };

/* ================================================================== level */

static struct {
    float edge, pitch, roll, zero;
    bool flat, held;
    int mech;
    reading_t main, cmp_robot, cmp_diff;
    lv_obj_t *dial, *mode, *hold_chip, *label_main, *mech_row, *mech_chips[CAT_MAX_MECHS], *cmp_note;
    uint32_t mech_sig;
} LV;

static void level_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    float rad = fminf(lv_area_get_width(&a), lv_area_get_height(&a)) / 2.0f - 6;
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = (int32_t)cx;
    ad.center.y = (int32_t)cy;
    ad.radius = (uint16_t)rad;
    ad.width = 2;
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.color = bz_lv(BZ_C_FAINT);
    lv_draw_arc(layer, &ad);
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.round_start = ld.round_end = 1;
    /* ticks every 15°, like the climate dial's ring */
    for (int d = 0; d < 360; d += 15) {
        float an = d * 0.0174533f, r0 = rad - (d % 90 ? 10 : 18);
        ld.color = bz_lv(d % 90 ? BZ_C_FAINT : BZ_C_DIM);
        ld.width = 2;
        ld.p1.x = cx + cosf(an) * r0; ld.p1.y = cy + sinf(an) * r0;
        ld.p2.x = cx + cosf(an) * (rad - 2); ld.p2.y = cy + sinf(an) * (rad - 2);
        lv_draw_line(layer, &ld);
    }
    if (LV.flat) {
        /* a bubble: where "up" leans */
        float bx = cx + fmaxf(-1, fminf(1, LV.roll / 30.0f)) * (rad - 30);
        float by = cy + fmaxf(-1, fminf(1, LV.pitch / 30.0f)) * (rad - 30);
        ad.center.x = (int32_t)bx;
        ad.center.y = (int32_t)by;
        ad.radius = 26;
        ad.width = 26;
        ad.color = bz_lv(fabsf(LV.pitch) < 0.5f && fabsf(LV.roll) < 0.5f ? BZ_C_OK : BZ_C_ICE);
        lv_draw_arc(layer, &ad);
    } else {
        /* the horizon, turned by the edge angle */
        float an = -(LV.edge - LV.zero) * 0.0174533f;
        ld.color = bz_lv(BZ_C_INK);
        ld.width = 5;
        ld.p1.x = cx - cosf(an) * (rad - 24); ld.p1.y = cy - sinf(an) * (rad - 24);
        ld.p2.x = cx + cosf(an) * (rad - 24); ld.p2.y = cy + sinf(an) * (rad - 24);
        lv_draw_line(layer, &ld);
        ld.color = bz_lv(BZ_C_FAINT);
        ld.width = 2;
        ld.p1.x = cx - rad + 24; ld.p1.y = cy;
        ld.p2.x = cx + rad - 24; ld.p2.y = cy;
        lv_draw_line(layer, &ld);
    }
}

static void level_frame(double now, double dt)
{
    (void)now; (void)dt;
    hal_imu_t m;
    if (!hal_imu(&m) || !m.ok || LV.held) return;
    float edge = atan2f(m.ax, m.ay) * 57.2958f;
    /* lying face up, gravity points into the glass (-z) */
    float pitch = atan2f(m.ay, -m.az) * 57.2958f, roll = atan2f(m.ax, -m.az) * 57.2958f;
    const float k = 0.15f; /* a light low-pass: hands shake */
    LV.edge += (edge - LV.edge) * k;
    LV.pitch += (pitch - LV.pitch) * k;
    LV.roll += (roll - LV.roll) * k;
    LV.flat = fabsf(m.az) > 0.8f;
    lv_obj_invalidate(LV.dial);
    bz_ui_keep_alive();
}

static void level_refresh(void)
{
    const cat_robot_t *r = R;
    if (LV.flat) {
        ui_text(LV.label_main, "tilt from level");
        ui_text(LV.mode, "lying flat · pitch %.1f° roll %.1f°", LV.pitch, LV.roll);
        reading_set(&LV.main, "%.1f", hypotf(LV.pitch, LV.roll), true);
    } else {
        ui_text(LV.label_main, "edge angle");
        ui_text(LV.mode, "on edge%s", LV.zero != 0 ? " · zeroed" : "");
        reading_set(&LV.main, "%.1f", LV.edge - LV.zero, true);
    }
    /* compare with a rotational mechanism: an arm's encoder against gravity */
    uint32_t sig = 2166136261u ^ (uint32_t)r->nmechs;
    for (int i = 0; i < r->nmechs; i++) for (const char *p = r->mechs[i].name; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
    if (sig != LV.mech_sig) {
        LV.mech_sig = sig;
        lv_obj_clean(LV.mech_row);
        LV.mech = -1;
        for (int i = 0; i < r->nmechs && i < CAT_MAX_MECHS; i++) {
            LV.mech_chips[i] = NULL;
            if (r->mechs[i].kind != CAT_MECH_ROTATIONAL) continue;
            LV.mech_chips[i] = ui_chip(LV.mech_row, r->mechs[i].name, NULL, NULL);
            if (LV.mech < 0) LV.mech = i;
        }
    }
    for (int i = 0; i < r->nmechs && i < CAT_MAX_MECHS; i++) if (LV.mech_chips[i]) ui_chip_set(LV.mech_chips[i], i == LV.mech);
    if (LV.flat) {
        reading_set(&LV.cmp_robot, "%.1f", 0, false);
        reading_set(&LV.cmp_diff, "%.1f", 0, false);
        ui_text(LV.cmp_note, "stand the tablet on its long edge against the mechanism to compare");
    } else if (LV.mech >= 0 && LV.mech < r->nmechs) {
        const cat_mech_t *m = &r->mechs[LV.mech];
        double tablet = LV.edge - LV.zero;
        reading_set(&LV.cmp_robot, "%.1f", m->pos, m->pos == m->pos);
        reading_set(&LV.cmp_diff, "%+.1f", m->pos - tablet, m->pos == m->pos);
        ui_text(LV.cmp_note, "hold the tablet's long edge along the %s, screen facing you", m->name);
    } else {
        reading_set(&LV.cmp_robot, "%.1f", 0, false);
        reading_set(&LV.cmp_diff, "%.1f", 0, false);
        ui_text(LV.cmp_note, "%s", r->connected ? "no rotational mechanism published" : "connect to compare with a mechanism");
    }
}

static void lv_zero(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    LV.zero = LV.zero != 0 ? 0 : LV.edge;
    ui_island_say(BZ_I_STRAIGHTEN, LV.zero != 0 ? "level zeroed here" : "level zero cleared");
}

static void lv_hold(lv_obj_t *o, void *u)
{
    (void)u;
    LV.held = !LV.held;
    ui_chip_set(o, LV.held);
}

static void level_build(lv_obj_t *b)
{
    lv_obj_t *t = bz_tile(b, 560, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    reading(t, &LV.main, "angle", BZ_F_CLOCK, "°", 0);
    LV.label_main = LV.main.label;
    LV.mode = bz_label(t, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_width(LV.mode, 480);
    lv_obj_set_pos(LV.mode, 0, 166);
    lv_obj_t *row = bz_row(t, 10);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    ui_button(row, BZ_I_ADJUST, "zero here", lv_zero, NULL);
    LV.hold_chip = ui_chip(row, "hold", lv_hold, NULL);
    LV.dial = bz_box(t);
    lv_obj_set_size(LV.dial, 250, 250);
    lv_obj_align(LV.dial, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(LV.dial, level_draw, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *c = bz_tile(b, W - 2 * PAD - 560 - BZ_GAP, APP_H);
    lv_obj_set_pos(c, PAD + 560 + BZ_GAP, APP_Y);
    bz_label(c, "against the robot", BZ_F_LABEL, BZ_C_DIM);
    LV.mech_row = bz_row(c, 8);
    lv_obj_set_flex_flow(LV.mech_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(LV.mech_row, W - 2 * PAD - 560 - BZ_GAP - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(LV.mech_row, 0, 34);
    reading(c, &LV.cmp_robot, "encoder says", BZ_F_VALUE, "°", 120);
    reading(c, &LV.cmp_diff, "encoder minus gravity", BZ_F_VALUE, "°", 230);
    LV.cmp_note = bz_label(c, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(LV.cmp_note, W - 2 * PAD - 560 - BZ_GAP - 2 * BZ_PAD_TILE);
    lv_obj_align(LV.cmp_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    LV.mech_sig = 1;
}

const ui_app_t APP_LEVEL = { .name = "level", .icon = BZ_I_STRAIGHTEN, .build = level_build, .refresh = level_refresh,
                             .frame = level_frame };

/* ================================================================== lens */

static struct {
    lv_obj_t *img, *frame_box, *frz, *info;
    lv_image_dsc_t dsc;
    bool frozen, started;
    int shots;
} LN;

static void lens_frame(double now, double dt)
{
    (void)now; (void)dt;
    bz_ui_keep_alive();
    if (LN.frozen) return;
    int w, h;
    const uint16_t *px = hal_camera_frame(&w, &h);
    if (!px) return;
    LN.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    LN.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    LN.dsc.header.w = (uint32_t)w;
    LN.dsc.header.h = (uint32_t)h;
    LN.dsc.header.stride = (uint32_t)w * 2;
    LN.dsc.data = (const uint8_t *)px;
    LN.dsc.data_size = (uint32_t)(w * h * 2);
    lv_image_cache_drop(&LN.dsc);
    lv_image_set_src(LN.img, &LN.dsc);
    /* fill the frame box: the P4's PPA scales on the tablet; here LVGL does */
    int bw = lv_obj_get_width(LN.frame_box), bh = lv_obj_get_height(LN.frame_box);
    float s = fminf((float)bw / w, (float)bh / h);
    lv_image_set_scale(LN.img, (uint32_t)(s * 256));
    lv_obj_center(LN.img);
    lv_obj_invalidate(LN.img);
}

static void ln_freeze(lv_obj_t *o, void *u)
{
    (void)u;
    LN.frozen = !LN.frozen;
    ui_chip_set(o, LN.frozen);
}

static void ln_snap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    const char *root = hal_sd_root();
    if (!root) {
        ui_island_say(BZ_I_SD_CARD, "no microSD card");
        return;
    }
    char dir[96], path[160];
    snprintf(dir, sizeof dir, "%s/lens", root);
    mkdir(dir, 0755);
    struct tm tm;
    hal_rtc_get(&tm);
    snprintf(path, sizeof path, "%s/%04d%02d%02d-%02d%02d%02d.jpg", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    bool ok = hal_camera_snapshot(path);
    char msg[80];
    snprintf(msg, sizeof msg, ok ? "saved %s" : "couldn't save", path + strlen(root) + 1);
    ui_island_say(BZ_I_PHOTO_CAMERA, msg);
    if (ok) {
        LN.shots++;
        ui_text(LN.info, "%d this session · %s/lens", LN.shots, root);
        hal_tone(2400, 30, S.volume * 0.5f);
    }
}

static void lens_open(void)
{
    LN.started = hal_camera_start();
    ui_text(LN.info, "%s", LN.started ? "sc2356 · 2 mp" : "the camera didn't start");
}

static void lens_close(void) { hal_camera_stop(); }

static void lens_build(lv_obj_t *b)
{
    LN.frame_box = bz_tile(b, 1000, APP_H);
    lv_obj_set_pos(LN.frame_box, PAD, APP_Y);
    lv_obj_set_style_pad_all(LN.frame_box, 0, 0);
    lv_obj_set_style_bg_color(LN.frame_box, lv_color_black(), 0);
    lv_obj_set_style_clip_corner(LN.frame_box, true, 0);
    LN.img = lv_image_create(LN.frame_box);
    lv_obj_add_flag(LN.img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_image_set_antialias(LN.img, false);
    lv_obj_t *side = bz_tile(b, W - 2 * PAD - 1000 - BZ_GAP, APP_H);
    lv_obj_set_pos(side, PAD + 1000 + BZ_GAP, APP_Y);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 12, 0);
    bz_label(side, "look inside", BZ_F_LABEL, BZ_C_DIM);
    LN.frz = ui_chip(side, "freeze", ln_freeze, NULL);
    ui_button(side, BZ_I_PHOTO_CAMERA, "save", ln_snap, NULL);
    LN.info = bz_label(side, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(LN.info, W - 2 * PAD - 1000 - BZ_GAP - 2 * BZ_PAD_TILE);
}

const ui_app_t APP_LENS = { .name = "lens", .icon = BZ_I_PHOTO_CAMERA, .build = lens_build, .open = lens_open,
                            .close = lens_close, .frame = lens_frame };

/* ================================================================== can tap */

static struct {
    cat_can_bus_t bus;
    bool on;
    reading_t load, fps;
    lv_obj_t *state, *hb, *hb_label, *list, *silent;
    uint32_t sig;
    lv_obj_t *marks[CAT_CAN_MAX_NODES], *rates[CAT_CAN_MAX_NODES];
    uint32_t frames_last;
    double t_last;
    float fps_v;
} CT;

/* Catalyst's device type names → FRC (device type, manufacturer) */
static bool catalyst_type(const char *t, int *type, int *mfr)
{
    static const struct { const char *n; int t, m; } K[] = {
        { "TalonFX", 2, 4 }, { "TalonFXS", 2, 4 }, { "CANcoder", 7, 4 }, { "Pigeon2", 4, 4 }, { "CANdle", 10, 4 },
        { "CANrange", 6, 4 }, { "SparkMax", 2, 5 }, { "SparkFlex", 2, 5 }, { "PDH", 8, 5 }, { "PH", 9, 5 },
        { "PDP", 8, 4 }, { "PCM", 9, 4 },
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (!strcmp(K[i].n, t)) { *type = K[i].t; *mfr = K[i].m; return true; }
    return false;
}

static void cantap_frame(double now, double dt)
{
    (void)dt;
    if (!CT.on) return;
    hal_can_frame_t f[64];
    int n;
    while ((n = hal_can_read(f, 64)) > 0)
        for (int i = 0; i < n; i++) cat_can_feed(&CT.bus, f[i].id, f[i].ext, f[i].len, now);
    cat_can_tick(&CT.bus, now);
    bz_ui_keep_alive();
}

static int node_cmp(const void *a, const void *b)
{
    const cat_can_node_t *x = a, *y = b;
    if (x->type != y->type) return x->type - y->type;
    return x->device - y->device;
}

static void cantap_refresh(void)
{
    hal_can_stats_t st;
    hal_can_stats(&st);
    static const char *const states[] = { "stopped", "listening · listen-only, never transmits", "bus errors · check termination and the bitrate", "no transceiver on port a" };
    ui_text(CT.state, "%s", states[st.state < 0 || st.state > 3 ? 0 : st.state]);
    double now = ui_now();
    if (now - CT.t_last >= 0.5) {
        CT.fps_v = (float)((CT.bus.frames - CT.frames_last) / (now - CT.t_last));
        CT.frames_last = CT.bus.frames;
        CT.t_last = now;
    }
    reading_set(&CT.load, "%.0f", CT.bus.load * 100, CT.on && CT.bus.frames > 0);
    reading_set(&CT.fps, "%.0f", CT.fps_v, CT.on && CT.bus.frames > 0);
    bz_mark_set(CT.hb, CT.bus.heartbeat ? BZ_OK : BZ_STALE);
    ui_text(CT.hb_label, "%s", CT.bus.heartbeat ? "controller heartbeat on the bus" : "no controller heartbeat");

    uint32_t sig = 2166136261u ^ (uint32_t)CT.bus.n;
    if (sig != CT.sig) {
        CT.sig = sig;
        qsort(CT.bus.nodes, (size_t)CT.bus.n, sizeof(cat_can_node_t), node_cmp);
        lv_obj_clean(CT.list);
        for (int i = 0; i < CT.bus.n; i++) {
            const cat_can_node_t *nd = &CT.bus.nodes[i];
            lv_obj_t *row = bz_tile(CT.list, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 18, 0);
            lv_obj_set_style_pad_ver(row, 12, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 14, 0);
            CT.marks[i] = bz_mark(row, BZ_OK, 10);
            char id[8];
            snprintf(id, sizeof id, "%2d", nd->device);
            lv_obj_t *l = bz_label(row, id, BZ_F_LABEL, BZ_C_INK);
            lv_obj_set_width(l, 28);
            l = bz_label(row, cat_can_device_name(nd->type, nd->mfr), BZ_F_BODY_S, BZ_C_INK);
            lv_obj_set_width(l, 200);
            l = bz_label(row, cat_can_mfr_name(nd->mfr), BZ_F_CAPTION, BZ_C_DIM);
            lv_obj_set_flex_grow(l, 1);
            CT.rates[i] = bz_label(row, "", BZ_F_LABEL, BZ_C_INK);
        }
    }
    for (int i = 0; i < CT.bus.n; i++) {
        const cat_can_node_t *nd = &CT.bus.nodes[i];
        bz_mark_set(CT.marks[i], nd->quiet ? BZ_FAULT : BZ_OK);
        if (nd->quiet) ui_text(CT.rates[i], "silent");
        else ui_text(CT.rates[i], "%.0f hz", nd->rate_hz);
    }
    /* what the robot declares that this bus isn't carrying */
    char silent[200] = "";
    size_t o = 0;
    for (int i = 0; i < R->ndevices; i++) {
        const cat_device_t *d = &R->devices[i];
        int type, mfr;
        if (!catalyst_type(d->type, &type, &mfr)) continue;
        bool heard = false;
        for (int k = 0; k < CT.bus.n && !heard; k++)
            heard = CT.bus.nodes[k].type == type && CT.bus.nodes[k].mfr == mfr && CT.bus.nodes[k].device == d->id && !CT.bus.nodes[k].quiet;
        if (!heard && o < sizeof silent - 32)
            o += (size_t)snprintf(silent + o, sizeof silent - o, "%s%s %d (%s)", o ? ", " : "", d->name[0] ? d->name : d->type, d->id, d->bus);
    }
    if (!R->connected) ui_text(CT.silent, "connect to the robot to compare with the devices it declares");
    else if (!CT.bus.frames) ui_text(CT.silent, " ");
    else ui_text(CT.silent, "%s%s", o ? "declared, not heard here: " : "every declared device is talking", silent);
}

static void cantap_open(void)
{
    cat_can_reset(&CT.bus, 1000000);
    CT.sig = 0;
    CT.frames_last = 0;
    CT.t_last = ui_now();
    CT.on = hal_can_start(1000000);
}

static void cantap_close(void)
{
    hal_can_stop();
    CT.on = false;
}

static void cantap_build(lv_obj_t *b)
{
    lv_obj_t *t = bz_tile(b, 420, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    reading(t, &CT.load, "bus load", BZ_F_DISPLAY, "%", 0);
    reading(t, &CT.fps, "frames", BZ_F_VALUE, "/s", 130);
    lv_obj_t *hr = bz_row(t, 10);
    lv_obj_set_pos(hr, 0, 232);
    CT.hb = bz_mark(hr, BZ_STALE, 10);
    CT.hb_label = bz_label(hr, "", BZ_F_BODY_S, BZ_C_INK);
    CT.state = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(CT.state, 420 - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(CT.state, 0, 270);
    CT.silent = bz_label(t, "", BZ_F_BODY_S, BZ_C_WARN);
    lv_obj_set_width(CT.silent, 420 - 2 * BZ_PAD_TILE);
    lv_obj_align(CT.silent, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD + 420 + BZ_GAP, APP_Y);
    CT.list = ui_scroller(wrap, W - 2 * PAD - 420 - BZ_GAP, APP_H);
    lv_obj_set_style_pad_row(CT.list, 8, 0);
}

const ui_app_t APP_CANTAP = { .name = "can tap", .icon = BZ_I_CABLE, .build = cantap_build, .open = cantap_open,
                              .close = cantap_close, .refresh = cantap_refresh, .frame = cantap_frame };

/* ================================================================== logs */

#define LG_MAX_FILES 32

static struct {
    lv_obj_t *files, *detail, *stats, *charts[4], *events, *title;
    char paths[LG_MAX_FILES][160];
    int nfiles;
    cat_log_t *log;
    int pending;
} LG;

static bool log_ext(const char *n)
{
    size_t l = strlen(n);
    return (l > 7 && !strcmp(n + l - 7, ".wpilog")) || (l > 6 && !strcmp(n + l - 6, ".dslog")) ||
           (l > 9 && !strcmp(n + l - 9, ".dsevents"));
}

static void lg_pick(lv_obj_t *o, void *u)
{
    (void)o;
    LG.pending = (int)(intptr_t)u;
    ui_text(LG.title, "reading %s…", strrchr(LG.paths[LG.pending], '/') + 1);
}

static void lg_back(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    lv_obj_add_flag(LG.detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lv_obj_get_parent(LG.files), LV_OBJ_FLAG_HIDDEN);
}

static void lg_scan(void)
{
    lv_obj_clean(LG.files);
    LG.nfiles = 0;
    const char *root = hal_sd_root();
    if (!root) {
        lv_obj_t *t = bz_tile(LG.files, W - 2 * PAD, 130);
        bz_label(t, "no microSD card", BZ_F_NAME, BZ_C_INK);
        lv_obj_t *l = bz_label(t, "Copy Driver Station logs (.wpilog, .dslog, .dsevents) to the card's root or a logs/ folder.",
                               BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        return;
    }
    const char *dirs[2] = { "", "/logs" };
    for (int d = 0; d < 2; d++) {
        char dir[128];
        snprintf(dir, sizeof dir, "%s%s", root, dirs[d]);
        DIR *dp = opendir(dir);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) && LG.nfiles < LG_MAX_FILES) {
            if (!log_ext(e->d_name)) continue;
            snprintf(LG.paths[LG.nfiles], sizeof LG.paths[0], "%s/%s", dir, e->d_name);
            struct stat st;
            long size = stat(LG.paths[LG.nfiles], &st) == 0 ? (long)st.st_size : 0;
            lv_obj_t *row = bz_tile(LG.files, W - 2 * PAD, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 20, 0);
            lv_obj_set_style_pad_ver(row, 16, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 16, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            bz_icon(row, BZ_I_RECEIPT_LONG, 24, BZ_C_DIM);
            lv_obj_t *n = bz_label(row, e->d_name, BZ_F_BODY, BZ_C_INK);
            lv_obj_set_flex_grow(n, 1);
            char sz[24];
            snprintf(sz, sizeof sz, size > 1 << 20 ? "%.1f mb" : "%.0f kb", size > 1 << 20 ? size / 1048576.0 : size / 1024.0);
            bz_label(row, sz, BZ_F_LABEL, BZ_C_DIM);
            bz_on_tap(row, lg_pick, (void *)(intptr_t)LG.nfiles);
            LG.nfiles++;
        }
        closedir(dp);
    }
    if (!LG.nfiles) {
        lv_obj_t *t = bz_tile(LG.files, W - 2 * PAD, 130);
        bz_label(t, "no logs on the card", BZ_F_NAME, BZ_C_INK);
        lv_obj_t *l = bz_label(t, "Looked in the root and logs/ for .wpilog, .dslog and .dsevents.", BZ_F_CAPTION, BZ_C_DIM);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void lg_show(void)
{
    cat_log_t *l = LG.log;
    lv_obj_add_flag(lv_obj_get_parent(LG.files), LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(LG.detail, LV_OBJ_FLAG_HIDDEN);
    char mb[16], tr[16];
    ui_text(LG.stats, "%s · %u records · %d:%02d\n\nlowest battery %s v\nbrownouts %d · dips under 6.8 v %d\nworst trip %s ms\npacket loss %.1f %%\nbusiest can %.0f %%\npeak cpu %.0f %%",
            l->kind, (unsigned)l->records, (int)l->duration_s / 60, (int)l->duration_s % 60,
            bz_fmt(mb, sizeof mb, l->min_battery == l->min_battery, "%.2f", l->min_battery), l->brownouts, l->dips,
            bz_fmt(tr, sizeof tr, l->have[CAT_LOG_TRIP], "%.1f", l->max_trip_ms), l->avg_loss * 100, l->max_can * 100, l->max_cpu * 100);
    static const cat_log_series_t series[4] = { CAT_LOG_BATTERY, CAT_LOG_TRIP, CAT_LOG_CAN, CAT_LOG_CPU };
    for (int c = 0; c < 4; c++) {
        bz_spark_clear(LG.charts[c]);
        if (series[c] == CAT_LOG_BATTERY) bz_spark_range(LG.charts[c], 6, 13.5f);
        for (int i = 0; i < CAT_LOG_POINTS; i++) {
            float v = l->series[series[c]][i];
            if (v == v) bz_spark_push(LG.charts[c], v);
        }
    }
    lv_obj_clean(LG.events);
    for (int i = 0; i < l->nevents; i++) {
        lv_obj_t *row = bz_row(LG.events, 10);
        bz_mark(row, l->events[i].level == 2 ? BZ_FAULT : l->events[i].level == 1 ? BZ_WARN : BZ_INFO, 8);
        char t[16];
        snprintf(t, sizeof t, "%6.1f s", l->events[i].t);
        bz_label(row, t, BZ_F_CAPTION, BZ_C_DIM);
        bz_label_line(row, l->events[i].text, BZ_F_BODY_S, BZ_C_INK, 560);
    }
    if (!l->nevents) bz_label(LG.events, "no events in this log", BZ_F_CAPTION, BZ_C_DIM);
}

static void logs_frame(double now, double dt)
{
    (void)now; (void)dt;
    if (LG.pending < 0) return;
    /* parse on the frame after the tap, so "reading…" is on screen while it works */
    static int wait;
    if (++wait < 2) return;
    wait = 0;
    int i = LG.pending;
    LG.pending = -1;
    if (!LG.log) LG.log = malloc(sizeof(cat_log_t));
    if (!LG.log) return;
    bool ok = cat_log_read(LG.paths[i], LG.log);
    ui_text(LG.title, "%s%s%s", strrchr(LG.paths[i], '/') + 1, ok ? "" : " · ", ok ? "" : LG.log->error);
    if (ok) lg_show();
}

static void logs_open(void)
{
    LG.pending = -1;
    lg_scan();
    lg_back(NULL, NULL);
    ui_text(LG.title, "%d log%s on the card", LG.nfiles, LG.nfiles == 1 ? "" : "s");
}

static void logs_build(lv_obj_t *b)
{
    lv_obj_t *r = head_right(b);
    LG.title = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    LG.files = ui_scroller(wrap, W - 2 * PAD, APP_H);
    lv_obj_set_style_pad_row(LG.files, 10, 0);

    LG.detail = bz_box(b);
    lv_obj_set_size(LG.detail, W - 2 * PAD, APP_H);
    lv_obj_set_pos(LG.detail, PAD, APP_Y);
    lv_obj_t *st = bz_tile(LG.detail, 330, APP_H);
    LG.stats = bz_label(st, "", BZ_F_BODY_S, BZ_C_INK);
    lv_obj_set_style_text_line_space(LG.stats, 6, 0);
    lv_obj_t *back = ui_button(st, BZ_I_ARROW_BACK, "files", lg_back, NULL);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    static const char *const names[4] = { "battery (lowest)", "trip time", "can", "cpu" };
    int cw = W - 2 * PAD - 330 - BZ_GAP, chw = (cw - BZ_GAP) / 2;
    for (int c = 0; c < 4; c++) {
        lv_obj_t *t = bz_tile(LG.detail, chw, 150);
        lv_obj_set_pos(t, 330 + BZ_GAP + (c % 2) * (chw + BZ_GAP), (c / 2) * (150 + BZ_GAP));
        bz_label(t, names[c], BZ_F_LABEL, BZ_C_DIM);
        LG.charts[c] = bz_spark(t, chw - 2 * BZ_PAD_TILE, 80, CAT_LOG_POINTS);
        lv_obj_align(LG.charts[c], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        bz_spark_color(LG.charts[c], c == 0 ? BZ_C_INK : BZ_C_ICE);
    }
    lv_obj_t *ev = bz_tile(LG.detail, cw, APP_H - 2 * (150 + BZ_GAP));
    lv_obj_set_pos(ev, 330 + BZ_GAP, 2 * (150 + BZ_GAP));
    bz_label(ev, "events", BZ_F_LABEL, BZ_C_DIM);
    LG.events = ui_scroller(ev, cw - 2 * BZ_PAD_TILE, APP_H - 2 * (150 + BZ_GAP) - 2 * BZ_PAD_TILE - 28);
    lv_obj_set_pos(lv_obj_get_parent(LG.events), 0, 28);
    lv_obj_set_style_pad_row(LG.events, 6, 0);
    lv_obj_add_flag(LG.detail, LV_OBJ_FLAG_HIDDEN);
    LG.pending = -1;
}

const ui_app_t APP_LOGS = { .name = "logs", .icon = BZ_I_RECEIPT_LONG, .build = logs_build, .open = logs_open,
                            .frame = logs_frame };

/* ================================================================== settings */

static struct {
    lv_obj_t *team, *addr_chips[5], *bright, *vol, *dark_chip, *light_chip, *calm_chip, *about, *wifi_list, *wifi_state;
    lv_obj_t *kb, *ta, *kb_title;
    char entry[8];
    char join_ssid[33];
} ST;

static const char *const ADDR_LABEL[5] = { "by team", "robot.local", "usb 172.22.11.2", "systemcore usb", "simulator" };
static const char *const ADDR_VALUE[5] = { "", "robot.local", "172.22.11.2", "172.26.0.1", "127.0.0.1" };

static void st_show_team(void)
{
    if (ST.entry[0]) ui_text(ST.team, "%s", ST.entry);
    else ui_text(ST.team, "%d", S.team);
}

static void st_key(lv_obj_t *o, void *u)
{
    (void)o;
    int k = (int)(intptr_t)u;
    size_t n = strlen(ST.entry);
    if (k >= 0 && k <= 9 && n < 4) {
        ST.entry[n] = (char)('0' + k);
        ST.entry[n + 1] = 0;
    } else if (k == 10 && n) {
        ST.entry[n - 1] = 0;
    } else if (k == 11 && n) {
        S.team = atoi(ST.entry);
        ST.entry[0] = 0;
        ui_settings_save();
        ui_apply_addresses();
        nt4_reconnect(cat_nt());
        char msg[48];
        snprintf(msg, sizeof msg, "team %d · reconnecting", S.team);
        ui_island_say(BZ_I_SYNC, msg);
    }
    hal_tone(1900, 8, S.volume * 0.4f);
    st_show_team();
}

static void st_addr(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    snprintf(S.address, sizeof S.address, "%s", ADDR_VALUE[i]);
    for (int k = 0; k < 5; k++) ui_chip_set(ST.addr_chips[k], k == i);
    ui_settings_save();
    ui_apply_addresses();
    nt4_reconnect(cat_nt());
}

static void st_level(lv_obj_t *lv, float v, bool final, void *u)
{
    (void)lv;
    if ((intptr_t)u == 0) {
        S.brightness = v;
        hal_set_brightness(v);
    } else {
        S.volume = v;
        hal_set_volume(v);
        if (final) hal_tone(1200, 60, v);
    }
    if (final) ui_settings_save();
}

static void st_tone(lv_obj_t *o, void *u)
{
    (void)o;
    S.dark = (intptr_t)u == 0;
    bz_ui_set_mode(S.dark, S.calm);
    ui_chip_set(ST.dark_chip, S.dark);
    ui_chip_set(ST.light_chip, !S.dark);
    ui_settings_save();
}

static void st_calm(lv_obj_t *o, void *u)
{
    (void)u;
    S.calm = !S.calm;
    bz_ui_set_mode(S.dark, S.calm);
    ui_chip_set(o, S.calm);
    ui_settings_save();
}

static void st_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        hal_wifi_join(ST.join_ssid, lv_textarea_get_text(ST.ta));
        snprintf(S.wifi_ssid, sizeof S.wifi_ssid, "%s", ST.join_ssid);
        char msg[64];
        snprintf(msg, sizeof msg, "joining %s", ST.join_ssid);
        ui_island_say(BZ_I_WIFI, msg);
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) lv_obj_add_flag(lv_obj_get_parent(ST.kb), LV_OBJ_FLAG_HIDDEN);
}

static void st_ap(lv_obj_t *o, void *u)
{
    (void)u;
    lv_obj_t *lbl = lv_obj_get_child(o, 0);
    snprintf(ST.join_ssid, sizeof ST.join_ssid, "%s", lv_label_get_text(lbl));
    ui_text(ST.kb_title, "password for %s", ST.join_ssid);
    lv_textarea_set_text(ST.ta, "");
    lv_obj_remove_flag(lv_obj_get_parent(ST.kb), LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lv_obj_get_parent(ST.kb));
}

static void st_scan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    hal_ap_t aps[8];
    int n = hal_wifi_scan(aps, 8);
    lv_obj_clean(ST.wifi_list);
    for (int i = 0; i < n; i++) ui_chip(ST.wifi_list, aps[i].ssid, st_ap, NULL);
    if (!n) bz_label(ST.wifi_list, "nothing in range", BZ_F_CAPTION, BZ_C_DIM);
}

static void settings_refresh(void)
{
    hal_net_t n;
    hal_net(&n);
    ui_text(ST.wifi_state, "%s%s%s · %d dbm · %s", n.up ? "on " : "off", n.up ? n.ssid : "", "", n.rssi, n.ip);
    hal_sys_t s;
    hal_sys(&s);
    hal_battery_t b;
    hal_battery(&b);
    ui_text(ST.about, "catalyst tab 0.1 · bezel %s\npanel %s · %s\npsram free %.1f mb · sram %u kb\nbattery %.2f v · %d %%%s\nmicroSD %s",
            "tab5", hal_panel_name(), s.chip, s.psram_free / 1048576.0, (unsigned)(s.sram_free / 1024), b.volts, b.percent,
            b.charging ? " · charging" : "", hal_sd_root() ? hal_sd_root() : "not mounted");
}

static void settings_open(void)
{
    ST.entry[0] = 0;
    st_show_team();
    for (int k = 0; k < 5; k++) ui_chip_set(ST.addr_chips[k], !strcmp(S.address, ADDR_VALUE[k]));
    bz_level_set(ST.bright, S.brightness, false);
    bz_level_set(ST.vol, S.volume, false);
    ui_chip_set(ST.dark_chip, S.dark);
    ui_chip_set(ST.light_chip, !S.dark);
    ui_chip_set(ST.calm_chip, S.calm);
}

static void settings_build(lv_obj_t *b)
{
    int c1 = 380, c2 = 440, c3 = W - 2 * PAD - c1 - c2 - 2 * BZ_GAP;
    /* team */
    lv_obj_t *t = bz_tile(b, c1, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    bz_label(t, "team", BZ_F_LABEL, BZ_C_DIM);
    ST.team = bz_label(t, "", BZ_F_DISPLAY, BZ_C_INK);
    lv_obj_set_pos(ST.team, -4, 20);
    lv_obj_t *pad = bz_row(t, 10);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(pad, c1 - 2 * BZ_PAD_TILE);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    static const int keys[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
    for (int i = 0; i < 12; i++) {
        int k = keys[i];
        char s[4];
        snprintf(s, sizeof s, "%d", k);
        lv_obj_t *btn = ui_button(pad, k == 10 ? BZ_I_ARROW_BACK : k == 11 ? BZ_I_CHECK_CIRCLE : NULL, k < 10 ? s : NULL, st_key,
                                  (void *)(intptr_t)k);
        lv_obj_set_size(btn, (c1 - 2 * BZ_PAD_TILE - 20) / 3, 60);
        lv_obj_set_style_radius(btn, 20, 0);
        if (k == 11) ui_chip_set(btn, true);
    }
    /* link */
    lv_obj_t *l = bz_tile(b, c2, APP_H);
    lv_obj_set_pos(l, PAD + c1 + BZ_GAP, APP_Y);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 12, 0);
    bz_label(l, "robot address", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *ar = bz_row(l, 8);
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(ar, c2 - 2 * BZ_PAD_TILE);
    for (int i = 0; i < 5; i++) ST.addr_chips[i] = ui_chip(ar, ADDR_LABEL[i], st_addr, (void *)(intptr_t)i);
    bz_label(l, "wi-fi", BZ_F_LABEL, BZ_C_DIM);
    ST.wifi_state = bz_label(l, "", BZ_F_CAPTION, BZ_C_DIM);
    ui_button(l, BZ_I_WIFI, "scan", st_scan, NULL);
    ST.wifi_list = bz_row(l, 8);
    lv_obj_set_flex_flow(ST.wifi_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(ST.wifi_list, c2 - 2 * BZ_PAD_TILE);
    lv_obj_t *note = bz_label(l, "At events, Wi-Fi to the robot isn't allowed: tether with a USB-Ethernet adapter on the USB-A port.",
                              BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(note, c2 - 2 * BZ_PAD_TILE);
    /* display */
    lv_obj_t *d = bz_tile(b, c3, APP_H);
    lv_obj_set_pos(d, PAD + c1 + c2 + 2 * BZ_GAP, APP_Y);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(d, 12, 0);
    bz_label(d, "brightness", BZ_F_LABEL, BZ_C_DIM);
    ST.bright = bz_level(d, c3 - 2 * BZ_PAD_TILE, 52, 0.05f, 1, 0.01f);
    bz_level_on_change(ST.bright, st_level, (void *)(intptr_t)0);
    bz_label(d, "volume", BZ_F_LABEL, BZ_C_DIM);
    ST.vol = bz_level(d, c3 - 2 * BZ_PAD_TILE, 52, 0, 1, 0.01f);
    bz_level_on_change(ST.vol, st_level, (void *)(intptr_t)1);
    lv_obj_t *tr = bz_row(d, 8);
    ST.dark_chip = ui_chip(tr, "dark", st_tone, (void *)(intptr_t)0);
    ST.light_chip = ui_chip(tr, "light", st_tone, (void *)(intptr_t)1);
    ST.calm_chip = ui_chip(tr, "calm", st_calm, NULL);
    ST.about = bz_label(d, "", BZ_F_CAPTION, BZ_C_DIM);

    /* the Wi-Fi password sheet: LVGL's keyboard in Bezel's tokens */
    lv_obj_t *sheet = bz_tile(b, W - 2 * PAD, 420);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -PAD);
    bz_tile_set_fill(sheet, BZ_C_SURFACE2);
    ST.kb_title = bz_label(sheet, "", BZ_F_LABEL, BZ_C_DIM);
    ST.ta = lv_textarea_create(sheet);
    lv_textarea_set_one_line(ST.ta, true);
    lv_textarea_set_password_mode(ST.ta, true);
    lv_obj_set_width(ST.ta, W - 2 * PAD - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(ST.ta, 0, 28);
    lv_obj_add_style(ST.ta, bz_style_font(BZ_F_BODY), 0);
    lv_obj_add_style(ST.ta, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_add_style(ST.ta, bz_style_color(BZ_C_INK), 0);
    lv_obj_set_style_radius(ST.ta, 16, 0);
    lv_obj_set_style_border_width(ST.ta, 0, 0);
    lv_obj_set_style_pad_all(ST.ta, 12, 0);
    ST.kb = lv_keyboard_create(sheet);
    lv_keyboard_set_textarea(ST.kb, ST.ta);
    lv_obj_set_size(ST.kb, W - 2 * PAD - 2 * BZ_PAD_TILE, 290);
    lv_obj_align(ST.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(ST.kb, bz_style_plain(), 0);
    lv_obj_add_style(ST.kb, bz_style_fill(BZ_C_SURFACE3), LV_PART_ITEMS);
    lv_obj_add_style(ST.kb, bz_style_color(BZ_C_INK), LV_PART_ITEMS);
    /* the keyboard's control keys are LVGL symbols, which only its own Montserrat carries */
    lv_obj_set_style_text_font(ST.kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(ST.kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ST.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(ST.kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(ST.kb, st_kb_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

const ui_app_t APP_SETTINGS = { .name = "settings", .icon = BZ_I_SETTINGS, .build = settings_build, .open = settings_open,
                                .refresh = settings_refresh };
