/* Apps that use the tablet's own hardware: level (IMU), lens (camera), can tap (TWAI),
 * logs (microSD) and settings. */
#include "ui_internal.h"
#include "ui_home_mode.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: no longer in lvgl.h since 9.4 */
#include "cat_can.h"
#include "cat_logs.h"
#include "analyze.h"
#include "home.h"
#include "link.h"
#include "ui_storage.h"

#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    lv_obj_set_pos(r->value, -4, y + (f == BZ_F_CLOCK ? 34 : 20)); /* the clock face runs tall: clear of the label */
    r->unit = bz_label(tile, unit ? unit : "", BZ_F_NAME, BZ_C_DIM);
    lv_obj_align_to(r->unit, r->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -12);
}

static void reading_set(reading_t *r, const char *fmt, double v, bool have)
{
    char b[32];
    ui_text(r->value, "%s", bz_fmt(b, sizeof b, have, fmt, v));
    lv_obj_align_to(r->unit, r->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -12);
}

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

#define CLIP_MAX_S 60

static struct {
    lv_obj_t *img, *frame_box, *frz, *info, *rec, *clip_info;
    lv_image_dsc_t dsc;
    bool frozen, started, recording;
    bool plane;                 /* the HAL lays the camera on the panel itself (hal_camera_plane) */
    int px, py, pw, ph;         /* where */
    uint32_t gen;               /* the camera frame last asked for */
    int shots, shown_s;
    char clip_path[160], clip_name[48];
    volatile int upload;   /* 0 idle, 1 sending, 2 sent or queued, 3 failed */
    int upload_shown;
} LN;

/* A clip ended, by the button, by closing, or by reaching its length. */
static void clip_ended(double s)
{
    LN.recording = false;
    ui_chip_set(LN.rec, false);
    lv_label_set_text(lv_obj_get_child(LN.rec, 0), "record");
    ui_text(LN.clip_info, "saved · %.0f s in lens/", s);
    hal_tone(660, 80, S.volume);
}

static void clip_tick(void)
{
    if (LN.upload != LN.upload_shown) {
        LN.upload_shown = LN.upload;
        if (LN.upload == 2) ui_island_say(BZ_I_CLOUD_UPLOAD, "clip on its way to the pc");
        else if (LN.upload == 3) ui_island_say(BZ_I_CLOUD_OFF, "couldn't send the clip");
    }
    if (!LN.recording) return;
    double s = 0;
    if (!hal_clip_active(&s)) {
        clip_ended(CLIP_MAX_S);
        return;
    }
    if ((int)s != LN.shown_s) {
        LN.shown_s = (int)s;
        ui_text(LN.clip_info, "recording · %d of %d s", LN.shown_s, CLIP_MAX_S);
    }
}

static void lens_frame(double now, double dt)
{
    (void)now; (void)dt;
    bz_ui_keep_alive();
    clip_tick();
    /* the plane, once the window is up (the sheet that brings it in has no plane) */
    if (!LN.plane && LN.started && !bz_ui_sheeting()) {
        lv_area_t a;
        lv_obj_get_coords(LN.frame_box, &a);
        LN.plane = hal_camera_plane(a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a), &LN.px, &LN.py,
                                    &LN.pw, &LN.ph);
        if (LN.plane) lv_obj_add_flag(LN.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (LN.plane) {
        /* a new frame: one pixel under the plane redrawn, so a present lays the frame on */
        uint32_t g = hal_camera_gen();
        if (g != LN.gen && !LN.frozen) {
            LN.gen = g;
            lv_area_t a = { LN.px, LN.py, LN.px, LN.py }; /* screen coordinates */
            lv_obj_invalidate_area(LN.frame_box, &a);
        }
        return;
    }
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
    hal_camera_freeze(LN.frozen);
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

/* A clip: the camera through the P4's H.264 encoder into <sd>/lens/<time>.h264, up to a minute. */
static void ln_clip(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (LN.recording) {
        clip_ended(hal_clip_stop());
        return;
    }
    const char *root = hal_sd_root();
    if (!root) {
        ui_island_say(BZ_I_SD_CARD, "no microSD card");
        return;
    }
    if (!LN.started) {
        ui_island_say(BZ_I_VIDEOCAM_OFF, "the camera isn't running");
        return;
    }
    char dir[96];
    snprintf(dir, sizeof dir, "%s/lens", root);
    mkdir(dir, 0755);
    struct tm tm;
    hal_rtc_get(&tm);
    snprintf(LN.clip_name, sizeof LN.clip_name, "%04d%02d%02d-%02d%02d%02d.h264", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(LN.clip_path, sizeof LN.clip_path, "%s/%s", dir, LN.clip_name);
    if (!hal_clip_start(LN.clip_path, CLIP_MAX_S)) {
        ui_island_say(BZ_I_VIDEOCAM_OFF, "the encoder didn't start");
        LN.clip_path[0] = 0;
        return;
    }
    LN.recording = true;
    LN.shown_s = -1;
    ui_chip_set(LN.rec, true);
    lv_label_set_text(lv_obj_get_child(LN.rec, 0), "stop");
    hal_tone(990, 60, S.volume);
}

static void *ln_upload_thread(void *arg)
{
    (void)arg;
    /* link_upload queues on microSD when the Link is away; false means it couldn't even do that */
    LN.upload = link_upload(LN.clip_path, LN.clip_name) ? 2 : 3;
    return NULL;
}

static void ln_send(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (LN.recording || !LN.clip_path[0] || LN.upload == 1) {
        ui_island_say(BZ_I_MOVIE, LN.recording ? "stop the clip first" : "no clip yet");
        return;
    }
    LN.upload = 1;
    if (!hal_thread("clip-upload", ln_upload_thread, NULL, 8192)) LN.upload = 3;
    hal_tone(1800, 10, S.volume * 0.4f);
}

static void lens_open(void)
{
    LN.started = hal_camera_start();
    ui_text(LN.info, "%s", LN.started ? "sc2356 · 2 mp" : "the camera didn't start");
}

static void lens_close(void)
{
    hal_camera_plane_off();
    LN.plane = false;
    LN.frozen = false;
    hal_camera_freeze(false);
    if (LN.frz) ui_chip_set(LN.frz, false);
    lv_obj_remove_flag(LN.img, LV_OBJ_FLAG_HIDDEN);
    if (LN.recording) clip_ended(hal_clip_stop());
    hal_camera_stop();
}

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
    bz_label(side, "clip · h.264", BZ_F_LABEL, BZ_C_DIM);
    LN.rec = ui_chip(side, "record", ln_clip, NULL);
    ui_button(side, BZ_I_CLOUD_UPLOAD, "to pc", ln_send, NULL);
    LN.clip_info = bz_label(side, "up to a minute, to microSD", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(LN.clip_info, W - 2 * PAD - 1000 - BZ_GAP - 2 * BZ_PAD_TILE);
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

/* The card's Driver Station and robot logs (the root, logs/) and the recorder's runs (runs/): a log opens on its
 * numbers, charts and events; any of them can be analysed by GPT (analyze.h), the answer shown here, kept in the
 * notifications and saved to documents on request. */

#define LG_MAX_FILES 32

static struct {
    lv_obj_t *files, *detail, *stats, *charts[4], *events, *title;
    char paths[LG_MAX_FILES][160];
    int nfiles;
    cat_log_t *log;
    int pending;
    int cur;                 /* the log in the detail view, -1 none */
    /* the analysis panel */
    lv_obj_t *ai, *ai_title, *ai_state, *ai_text, *ai_go, *ai_save;
    char ai_path[160];
    bool ai_from_detail, ai_done;
    unsigned an_gen;
} LG;

static bool log_ext(const char *n)
{
    size_t l = strlen(n);
    return (l > 7 && !strcmp(n + l - 7, ".wpilog")) || (l > 6 && !strcmp(n + l - 6, ".dslog")) ||
           (l > 9 && !strcmp(n + l - 9, ".dsevents")) || (l > 4 && !strcmp(n + l - 4, ".csv"));
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
    const char *dirs[3] = { "", "/logs", "/runs" };
    for (int d = 0; d < 3; d++) {
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
        lv_obj_t *l = bz_label(t, "Looked in the root and logs/ for .wpilog, .dslog and .dsevents, and in runs/ for the recorder's .csv.",
                               BZ_F_CAPTION, BZ_C_DIM);
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

/* ---- the analysis: a log's digest to GPT (analyze.h), the answer here, in the notifications, and on the card ---- */

static bool is_csv(const char *p)
{
    size_t l = strlen(p);
    return l > 4 && !strcmp(p + l - 4, ".csv");
}

/* the answer's first line, "verdict: " dropped: what the notification says */
static void verdict(const char *a, char *out, size_t n)
{
    while (*a == ' ' || *a == '\n') a++;
    if (!strncmp(a, "verdict:", 8)) a += 8;
    while (*a == ' ') a++;
    size_t l = strcspn(a, "\n");
    snprintf(out, n, "%.*s", (int)l, a);
}

/* the answer for the faces: each line folded (home_fold_text), the line breaks kept */
static char *fold_lines(const char *s)
{
    char *o = malloc(strlen(s) + 1);
    if (!o) return NULL;
    size_t k = 0;
    while (*s) {
        size_t l = strcspn(s, "\n");
        memcpy(o + k, s, l);
        o[k + l] = 0;
        home_fold_text(o + k); /* in place: never longer */
        k += strlen(o + k);
        s += l;
        if (*s == '\n') {
            o[k++] = '\n';
            s++;
        }
    }
    o[k] = 0;
    return o;
}

/* Anywhere, not only with the app open: an analysis that finishes goes to the island and the notifications. */
static void an_watch(void *u)
{
    (void)u;
    static unsigned seen;
    unsigned g = analyze_gen();
    if (g == seen) return;
    seen = g;
    char *name = NULL, *answer = NULL;
    an_phase_t ph = analyze_get(&name, &answer, NULL, NULL, 0);
    if (ph == AN_DONE || ph == AN_FAILED) {
        char v[80], msg[96];
        verdict(answer ? answer : "", v, sizeof v);
        home_fold_text(v);
        snprintf(msg, sizeof msg, "%s %.24s: %s", ph == AN_DONE ? "analysis of" : "couldn't analyse", name ? name : "", v);
        ui_island_say(ph == AN_DONE ? BZ_I_AUTO_AWESOME : BZ_I_WARNING, msg);
    }
    free(name);
    free(answer);
}

static void ai_watch_start(void)
{
    static bool on;
    if (on) return;
    on = true;
    ui_on_refresh(an_watch, NULL);
}

/* The panel's words and buttons for LG.ai_path, from the analysis when it is about that file. */
static void ai_update(void)
{
    LG.an_gen = analyze_gen();
    char *name = NULL, *answer = NULL, model[48] = "";
    an_phase_t ph = analyze_get(&name, &answer, NULL, model, sizeof model);
    const char *base = strrchr(LG.ai_path, '/');
    base = base ? base + 1 : LG.ai_path;
    bool ours = name && !strcmp(name, base);
    if (!ours) ph = AN_IDLE;
    LG.ai_done = ph == AN_DONE;
    ui_text(LG.ai_title, "%s", base);
    const char *text = "";
    switch (ph) {
    case AN_IDLE:
        ui_text(LG.ai_state, "%s", is_csv(LG.ai_path) ? "a recorder run: analyze sends a summary of it to gpt"
                                                      : "analyze sends a summary of this log to gpt, not the file");
        break;
    case AN_READING: ui_text(LG.ai_state, "reading the log and boiling it down..."); break;
    case AN_ASKING: ui_text(LG.ai_state, "asking %s...", model[0] ? model : "gpt"); break;
    case AN_DONE: ui_text(LG.ai_state, "answered by %s \xc2\xb7 save keeps it in documents", model); break;
    case AN_FAILED: ui_text(LG.ai_state, "couldn't analyse it"); break;
    }
    if (ph == AN_DONE || ph == AN_FAILED) text = answer ? answer : "";
    char *t = fold_lines(text);
    const char *cur = lv_label_get_text(LG.ai_text);
    if (t && (!cur || strcmp(cur, t))) lv_label_set_text(LG.ai_text, t);
    free(t);
    bool busy = analyze_busy();
    lv_obj_set_style_opa(LG.ai_go, busy ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (LG.ai_done) lv_obj_remove_flag(LG.ai_save, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(LG.ai_save, LV_OBJ_FLAG_HIDDEN);
    free(name);
    free(answer);
}

static void ai_open(const char *path, bool from_detail)
{
    snprintf(LG.ai_path, sizeof LG.ai_path, "%s", path);
    LG.ai_from_detail = from_detail;
    lv_obj_add_flag(lv_obj_get_parent(LG.files), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(LG.detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(LG.ai, LV_OBJ_FLAG_HIDDEN);
    ai_update();
}

static void ai_go(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!LG.ai_path[0]) return;
    if (!analyze_start(LG.ai_path, S.team)) {
        ui_island_say(BZ_I_AUTO_AWESOME, analyze_busy() ? "an analysis is already under way" : "the assistant isn't running");
        return;
    }
    ai_watch_start();
    hal_tone(1800, 10, S.volume * 0.4f);
    ai_update();
}

static void ai_back(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    lv_obj_add_flag(LG.ai, LV_OBJ_FLAG_HIDDEN);
    if (LG.ai_from_detail) lv_obj_remove_flag(LG.detail, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lv_obj_get_parent(LG.files), LV_OBJ_FLAG_HIDDEN);
}

/* <sd>/CATOS/DOCS/MMDDHHMM.MD: the answer, then what was sent (a small write the user asked for: this thread) */
static void ai_save(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    char *name = NULL, *answer = NULL, *digest = NULL, model[48] = "";
    an_phase_t ph = analyze_get(&name, &answer, &digest, model, sizeof model);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char file[16], path[160];
    snprintf(file, sizeof file, "%02d%02d%02d%02d.MD", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    bool ok = false;
    if (ph == AN_DONE && answer && cstore_path(CS_DOCS, file, path, sizeof path)) {
        size_t cap = strlen(answer) + (digest ? strlen(digest) : 0) + 512;
        char *md = malloc(cap);
        if (md) {
            char when[32];
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
            int k = snprintf(md, cap, "# log analysis: %s\n\n%s, by %s, for team %d\n\n%s\n\n## what was sent\n\n```\n%s```\n",
                             name ? name : "", when, model, S.team, answer, digest ? digest : "");
            ok = k > 0 && cstore_write(path, md, (size_t)k < cap ? (size_t)k : cap - 1);
            free(md);
        }
    }
    char msg[64];
    snprintf(msg, sizeof msg, ok ? "saved to documents as %s" : "couldn't save %s: is the card in?", file);
    ui_island_say(ok ? BZ_I_SAVE : BZ_I_SD_CARD, msg);
    free(name);
    free(answer);
    free(digest);
}

static void lg_analyze(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (LG.cur >= 0) ai_open(LG.paths[LG.cur], true);
}

static char s_an_pending[160]; /* ui_logs_analyze's, for the next open */

void ui_logs_analyze(const char *path)
{
    snprintf(s_an_pending, sizeof s_an_pending, "%s", path ? path : "");
    ui_app_close();
    ui_app_open(&APP_LOGS, NULL);
}

static void ai_build(lv_obj_t *b)
{
    int w = W - 2 * PAD, iw = w - 2 * BZ_PAD_TILE;
    LG.ai = bz_tile(b, w, APP_H);
    lv_obj_set_pos(LG.ai, PAD, APP_Y);
    LG.ai_title = bz_label_line(LG.ai, "", BZ_F_NAME, BZ_C_INK, iw - 470);
    LG.ai_state = bz_label_line(LG.ai, "", BZ_F_LABEL, BZ_C_DIM, iw - 470);
    lv_obj_set_pos(LG.ai_state, 0, 38);
    lv_obj_t *r = bz_row(LG.ai, 10);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, 0);
    LG.ai_go = ui_button(r, BZ_I_AUTO_AWESOME, "analyze", ai_go, NULL);
    LG.ai_save = ui_button(r, BZ_I_SAVE, "save", ai_save, NULL);
    ui_button(r, BZ_I_ARROW_BACK, "back", ai_back, NULL);
    lv_obj_t *wrap = bz_box(LG.ai);
    int h = APP_H - 2 * BZ_PAD_TILE - 84;
    lv_obj_set_pos(wrap, 0, 84);
    lv_obj_set_size(wrap, iw, h);
    lv_obj_t *list = ui_scroller(wrap, iw, h);
    LG.ai_text = bz_label(list, "", BZ_F_BODY_S, BZ_C_INK);
    lv_label_set_long_mode(LG.ai_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(LG.ai_text, iw - 8);
    lv_obj_set_style_text_line_space(LG.ai_text, 4, 0);
    lv_obj_t *sp = bz_box(list); /* the last line clear of the bottom */
    lv_obj_set_height(sp, 40);
    lv_obj_add_flag(LG.ai, LV_OBJ_FLAG_HIDDEN);
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
    if (is_csv(LG.paths[i])) {
        /* a run has no DS numbers to chart: straight to its analysis */
        ui_text(LG.title, "%s", strrchr(LG.paths[i], '/') + 1);
        ai_open(LG.paths[i], false);
        return;
    }
    LG.cur = i;
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
    lv_obj_add_flag(LG.ai, LV_OBJ_FLAG_HIDDEN);
    ui_text(LG.title, "%d log%s on the card", LG.nfiles, LG.nfiles == 1 ? "" : "s");
    if (s_an_pending[0]) {
        /* from the recorder's analyze button: the run's analysis, asked for already */
        ai_open(s_an_pending, false);
        s_an_pending[0] = 0;
        ai_go(NULL, NULL);
    }
}

static void logs_refresh(void)
{
    if (!lv_obj_has_flag(LG.ai, LV_OBJ_FLAG_HIDDEN) && analyze_gen() != LG.an_gen) ai_update();
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
    lv_obj_t *an = ui_button(st, BZ_I_AUTO_AWESOME, "analyze", lg_analyze, NULL);
    lv_obj_align(an, LV_ALIGN_BOTTOM_LEFT, 0, -66);
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
    ai_build(b);
    LG.pending = -1;
    LG.cur = -1;
}

const ui_app_t APP_LOGS = { .name = "logs", .icon = BZ_I_RECEIPT_LONG, .build = logs_build, .open = logs_open,
                            .refresh = logs_refresh, .frame = logs_frame };

/* ================================================================== settings */

static struct {
    lv_obj_t *team, *addr_chips[5], *bright, *vol, *dark_chip, *light_chip, *calm_chip, *flip_chip, *perf_chip, *about, *wifi_list,
        *wifi_state, *usb_state;
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

static void st_autorot(lv_obj_t *o, void *u)
{
    (void)u;
    S.auto_rotate = !S.auto_rotate;
    ui_chip_set(o, S.auto_rotate);
    ui_settings_save();
}

static void st_flip(lv_obj_t *o, void *u)
{
    (void)u;
    S.auto_rotate = false; /* a hand-picked way up stays */
    ui_set_flip(!S.flip);
    ui_chip_set(o, S.flip);
    ui_chip_set(ST.calm_chip, false);
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

static void st_perf(lv_obj_t *o, void *u)
{
    (void)u;
    S.perf = !S.perf;
    ui_chip_set(o, S.perf);
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

/* A scan takes ~3 s on the C6: it runs on a worker, and the settings refresh lists what it heard. */
#define SCAN_MAX 12
static struct {
    volatile int state; /* 0 idle, 1 listening, 2 heard (for the refresh to show) */
    int n;
    hal_ap_t ap[SCAN_MAX];
} SCN;

static void *scan_worker(void *u)
{
    (void)u;
    hal_ap_t heard[24];
    int n = hal_wifi_scan(heard, 24), k = 0;
    /* strongest first (the driver's order); one entry per name, hidden networks left out */
    for (int i = 0; i < n && k < SCAN_MAX; i++) {
        if (!heard[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = !strcmp(SCN.ap[j].ssid, heard[i].ssid);
        if (!dup) SCN.ap[k++] = heard[i];
    }
    SCN.n = k;
    SCN.state = 2;
    return NULL;
}

static void st_ap(lv_obj_t *o, void *u)
{
    (void)o;
    const hal_ap_t *ap = &SCN.ap[(intptr_t)u];
    snprintf(ST.join_ssid, sizeof ST.join_ssid, "%s", ap->ssid);
    if (!ap->secure) {
        /* an open network needs no password */
        hal_wifi_join(ST.join_ssid, "");
        snprintf(S.wifi_ssid, sizeof S.wifi_ssid, "%s", ST.join_ssid);
        ui_island_say(BZ_I_WIFI, "joining the network");
        return;
    }
    ui_text(ST.kb_title, "password for %s", ST.join_ssid);
    lv_textarea_set_text(ST.ta, "");
    lv_obj_remove_flag(lv_obj_get_parent(ST.kb), LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lv_obj_get_parent(ST.kb));
}

static void st_scan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (SCN.state == 1) return;
    lv_obj_clean(ST.wifi_list);
    SCN.state = 1;
    if (!hal_thread("scan", scan_worker, NULL, 4096)) {
        SCN.state = 0;
        bz_label(ST.wifi_list, "couldn't start a scan", BZ_F_CAPTION, BZ_C_DIM);
        return;
    }
    bz_label(ST.wifi_list, "listening…", BZ_F_CAPTION, BZ_C_DIM);
}

static void st_scan_show(void)
{
    if (SCN.state != 2) return;
    SCN.state = 0;
    lv_obj_clean(ST.wifi_list);
    for (int i = 0; i < SCN.n; i++) {
        char t[48];
        /* four steps of signal, as a phone shows it */
        int r = SCN.ap[i].rssi, bars = r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1;
        snprintf(t, sizeof t, "%s  %.*s%.*s", SCN.ap[i].ssid, bars * 3, "\xe2\x97\x8f\xe2\x97\x8f\xe2\x97\x8f\xe2\x97\x8f",
                 (4 - bars) * 3, "\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b");
        ui_chip(ST.wifi_list, t, st_ap, (void *)(intptr_t)i);
    }
    if (!SCN.n) bz_label(ST.wifi_list, "nothing in range", BZ_F_CAPTION, BZ_C_DIM);
}

static void settings_refresh_more(void);

static void settings_refresh(void)
{
    settings_refresh_more();
    hal_net_t n;
    hal_net(&n);
    st_scan_show();
    if (n.up) ui_text(ST.wifi_state, "%s · %d dBm · %s", n.ssid, n.rssi, n.ip);
    else if (S.wifi_ssid[0]) ui_text(ST.wifi_state, "not connected · looking for %s", S.wifi_ssid);
    else ui_text(ST.wifi_state, "not connected · scan to pick a network");
    hal_tether_t t;
    hal_tether(&t);
    if (t.up) ui_text(ST.usb_state, "usb %s · %s via %s%s", t.kind, t.ip, t.gw, t.dhcp ? "" : " · fallback address");
    else if (t.present) ui_text(ST.usb_state, "usb %s attached · waiting for an address", t.kind[0] ? t.kind : "adapter");
    else ui_text(ST.usb_state, "usb: nothing on the usb-a port");
    hal_sys_t s;
    hal_sys(&s);
    hal_battery_t b;
    hal_battery(&b);
    ui_text(ST.about, "catalyst tab 0.1 · bezel %s\npanel %s · %s\npsram free %.1f mb · sram %u kb\nbattery %.2f v · %d %%%s\nmicroSD %s",
            "tab5", hal_panel_name(), s.chip, s.psram_free / 1048576.0, (unsigned)(s.sram_free / 1024), b.volts, b.percent,
            b.charging ? " · charging" : "", hal_sd_root() ? hal_sd_root() : "not mounted");
}

/* ---- the settings app: a list of sections on the left, the chosen one on the right ---- */

enum { SS_DISPLAY, SS_SOUND, SS_ROBOT, SS_NETWORK, SS_LINK, SS_ASSIST, SS_HOME, SS_TIME, SS_STORAGE, SS_POWER, SS_ABOUT,
       SS_COUNT };
static const char *const SS_NAME[SS_COUNT] = { "display", "sound", "robot", "network", "pc link",
                                               "assistant", "home", "date and time", "storage", "power", "about" };
static const char *SS_ICON[SS_COUNT];

static struct {
    lv_obj_t *nav[SS_COUNT], *pane[SS_COUNT];
    int cur;
    lv_obj_t *lock_chip, *sleep_chips[4], *dim_chips[4], *click_chip, *tz_chips[5], *clock, *sd_state, *batt, *off_btn, *link_state, *assist_state;
    double off_armed;
} SX;

static const int DIM_S[4] = { 30, 90, 300, 0 };
static const char *const DIM_L[4] = { "30 s", "90 s", "5 min", "never" };
static const int SLEEP_S[4] = { 60, 300, 900, 0 };
static const char *const SLEEP_L[4] = { "1 min", "5 min", "15 min", "never" };
static const char *const TZ_L[5] = { "pacific", "mountain", "central", "eastern", "utc" };
static const char *const TZ_V[5] = { "PST8PDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0",
                                     "EST5EDT,M3.2.0,M11.1.0", "UTC0" };

static void sx_show(int i)
{
    SX.cur = i;
    for (int k = 0; k < SS_COUNT; k++) {
        ui_chip_set(SX.nav[k], k == i);
        if (k == i) lv_obj_remove_flag(SX.pane[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(SX.pane[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void sx_nav(lv_obj_t *o, void *u)
{
    (void)o;
    sx_show((int)(intptr_t)u);
}

static void sx_dim(lv_obj_t *o, void *u)
{
    (void)o;
    S.dim_s = DIM_S[(int)(intptr_t)u];
    for (int i = 0; i < 4; i++) ui_chip_set(SX.dim_chips[i], S.dim_s == DIM_S[i]);
    ui_settings_save();
}

static void sx_lock(lv_obj_t *o, void *u)
{
    (void)u;
    S.lock = !S.lock;
    ui_chip_set(o, S.lock);
    ui_settings_save();
}

static void sx_sleep_after(lv_obj_t *o, void *u)
{
    (void)o;
    S.sleep_s = SLEEP_S[(int)(intptr_t)u];
    for (int i = 0; i < 4; i++) ui_chip_set(SX.sleep_chips[i], S.sleep_s == SLEEP_S[i]);
    ui_chip_set(SX.lock_chip, S.lock);
    ui_settings_save();
}

static void sx_clicks(lv_obj_t *o, void *u)
{
    (void)u;
    S.clicks = !S.clicks;
    ui_chip_set(o, S.clicks);
    ui_settings_save();
}

static void sx_chime(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    hal_tone(880, 120, S.volume);
}

static void sx_tz(lv_obj_t *o, void *u)
{
    (void)o;
    S.tz = (int)(intptr_t)u;
    setenv("TZ", TZ_V[S.tz], 1);
    tzset();
    hal_kv_set("tz", TZ_V[S.tz]);
    for (int i = 0; i < 5; i++) ui_chip_set(SX.tz_chips[i], i == S.tz);
    ui_settings_save();
}

static void sx_open_app(lv_obj_t *o, void *u)
{
    (void)o;
    ui_app_close();
    ui_app_open((const ui_app_t *)u, NULL);
}

static void sx_clear(lv_obj_t *o, void *u)
{
    (void)o;
    if ((intptr_t)u == 0) {
        hal_kv_set("notes", "");
        ui_island_say(BZ_I_EDIT_NOTE, "notes cleared");
    } else {
        hal_kv_set("checklist", "0");
        ui_island_say(BZ_I_CHECKLIST_RTL, "checklist reset");
    }
}

static void sx_sleep(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_sleep_now(); /* the next tap wakes it, and presses nothing */
}

static void sx_off(lv_obj_t *o, void *u)
{
    (void)u;
    /* two taps within three seconds: a power-off is never one stray touch */
    double now = hal_seconds();
    if (now - SX.off_armed < 3) {
        hal_power_off();
    } else {
        SX.off_armed = now;
        lv_label_set_text(lv_obj_get_child(o, 1), "tap again to turn off");
    }
}

static lv_obj_t *sx_pane(lv_obj_t *b, int i, int x, int w)
{
    lv_obj_t *t = bz_tile(b, w, APP_H);
    lv_obj_set_pos(t, x, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 14, 0);
    lv_obj_t *h = bz_row(t, 12);
    bz_icon(h, SS_ICON[i], 32, BZ_C_INK);
    bz_label(h, SS_NAME[i], BZ_F_NAME, BZ_C_INK);
    SX.pane[i] = t;
    return t;
}

static lv_obj_t *sx_wrap_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
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
#if BZ_LEAN
    ui_chip_set(ST.calm_chip, S.auto_rotate);
    ui_chip_set(ST.flip_chip, S.flip);
#else
    ui_chip_set(ST.calm_chip, S.calm);
#endif
    ui_chip_set(ST.perf_chip, S.perf);
    for (int i = 0; i < 4; i++) ui_chip_set(SX.dim_chips[i], S.dim_s == DIM_S[i]);
    for (int i = 0; i < 4; i++) ui_chip_set(SX.sleep_chips[i], S.sleep_s == SLEEP_S[i]);
    ui_chip_set(SX.click_chip, S.clicks);
    for (int i = 0; i < 5; i++) ui_chip_set(SX.tz_chips[i], i == S.tz);
    SX.off_armed = 0;
    lv_label_set_text(lv_obj_get_child(SX.off_btn, 1), "turn off");
    ui_assist_settings_open();
    ui_home_settings_open();
    if (ui_home_settings_wanted()) SX.cur = SS_HOME; /* "settings" from home mode */
    sx_show(SX.cur);
}

static void settings_refresh_more(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char d[64];
    if (tm.tm_year > 120) strftime(d, sizeof d, "%A %B %e · %H:%M:%S", &tm);
    else snprintf(d, sizeof d, "not set yet");
    ui_text(SX.clock, "%s", d);
    uint64_t total, fr;
    if (hal_sd_space(&total, &fr))
        ui_text(SX.sd_state, "microSD · %.1f gb free of %.1f gb", fr / 1073741824.0, total / 1073741824.0);
    else ui_text(SX.sd_state, "no microSD card");
    hal_battery_t b;
    if (hal_battery(&b) && b.ok)
        ui_text(SX.batt, "%d %% · %.2f v%s%s", b.percent, b.volts, b.charging ? " · charging" : "",
                b.external ? " · external power" : "");
    const cat_robot_t *r = R;
    ui_text(SX.link_state, "%s", r->connected ? "the robot is connected" : "the robot isn't connected");
}

static void settings_build(lv_obj_t *b)
{
    SS_ICON[SS_DISPLAY] = BZ_I_BRIGHTNESS_6;
    SS_ICON[SS_SOUND] = BZ_I_VOLUME_UP;
    SS_ICON[SS_ROBOT] = BZ_I_SMART_TOY;
    SS_ICON[SS_NETWORK] = BZ_I_WIFI;
    SS_ICON[SS_LINK] = BZ_I_COMPUTER;
    SS_ICON[SS_ASSIST] = BZ_I_AUTO_AWESOME;
    SS_ICON[SS_HOME] = BZ_I_HOME;
    SS_ICON[SS_TIME] = BZ_I_SCHEDULE;
    SS_ICON[SS_STORAGE] = BZ_I_SD_CARD;
    SS_ICON[SS_POWER] = BZ_I_POWER;
    SS_ICON[SS_ABOUT] = BZ_I_INFO;

    int nav_w = 300, px = PAD + nav_w + BZ_GAP, pw = W - 2 * PAD - nav_w - BZ_GAP, iw = pw - 2 * BZ_PAD_TILE;
    /* the sections */
    lv_obj_t *nav = bz_tile(b, nav_w, APP_H);
    lv_obj_set_pos(nav, PAD, APP_Y);
    lv_obj_set_style_pad_all(nav, 12, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, 4, 0);
    for (int i = 0; i < SS_COUNT; i++) {
        lv_obj_t *n = ui_button(nav, SS_ICON[i], SS_NAME[i], sx_nav, (void *)(intptr_t)i);
        /* eleven sections in the tile's height (APP_H less its padding) */
        lv_obj_set_size(n, nav_w - 24, (APP_H - 24 - (SS_COUNT - 1) * 4) / SS_COUNT);
        lv_obj_set_style_radius(n, 16, 0);
        lv_obj_set_flex_align(n, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        SX.nav[i] = n;
    }

    /* display */
    lv_obj_t *t = sx_pane(b, SS_DISPLAY, px, pw);
    bz_label(t, "brightness", BZ_F_LABEL, BZ_C_DIM);
    ST.bright = bz_level(t, iw, 52, 0.05f, 1, 0.01f);
    bz_level_on_change(ST.bright, st_level, (void *)(intptr_t)0);
    bz_label(t, "look", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = sx_wrap_row(t, iw);
    ST.dark_chip = ui_chip(r, "dark", st_tone, (void *)(intptr_t)0);
    ST.light_chip = ui_chip(r, "light", st_tone, (void *)(intptr_t)1);
    ST.perf_chip = ui_chip(r, "frame-rate overlay", st_perf, NULL);
    bz_label(t, "turn the picture", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
#if BZ_LEAN
    ST.calm_chip = ui_chip(r, "auto-rotate", st_autorot, NULL);
    ST.flip_chip = ui_chip(r, "upside down", st_flip, NULL);
#else
    ST.calm_chip = ui_chip(r, "calm", st_calm, NULL);
#endif
    bz_label(t, "dim when untouched for", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
    for (int i = 0; i < 4; i++) SX.dim_chips[i] = ui_chip(r, DIM_L[i], sx_dim, (void *)(intptr_t)i);

    /* sound */
    t = sx_pane(b, SS_SOUND, px, pw);
    bz_label(t, "volume", BZ_F_LABEL, BZ_C_DIM);
    ST.vol = bz_level(t, iw, 52, 0, 1, 0.01f);
    bz_level_on_change(ST.vol, st_level, (void *)(intptr_t)1);
    r = sx_wrap_row(t, iw);
    SX.click_chip = ui_chip(r, "tick on taps", sx_clicks, NULL);
    ui_button(r, BZ_I_VOLUME_UP, "play a chime", sx_chime, NULL);

    /* robot: the team and how to reach it */
    t = sx_pane(b, SS_ROBOT, px, pw);
    lv_obj_t *tr = bz_row(t, 24);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *tc = bz_col(tr, 6);
    bz_label(tc, "team", BZ_F_LABEL, BZ_C_DIM);
    ST.team = bz_label(tc, "", BZ_F_DISPLAY, BZ_C_INK);
    lv_obj_t *pad = bz_row(tr, 10);
    int kpw = 330;
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pad, 10, 0);
    lv_obj_set_width(pad, kpw);
    static const int keys[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
    for (int i = 0; i < 12; i++) {
        int k = keys[i];
        char s[4];
        snprintf(s, sizeof s, "%d", k);
        lv_obj_t *btn = ui_button(pad, k == 10 ? BZ_I_BACKSPACE : k == 11 ? BZ_I_CHECK_CIRCLE : NULL, k < 10 ? s : NULL, st_key,
                                  (void *)(intptr_t)k);
        lv_obj_set_size(btn, (kpw - 20) / 3, 54);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (k == 11) ui_chip_set(btn, true);
    }
    bz_label(t, "robot address", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
    for (int i = 0; i < 5; i++) ST.addr_chips[i] = ui_chip(r, ADDR_LABEL[i], st_addr, (void *)(intptr_t)i);

    /* network */
    t = sx_pane(b, SS_NETWORK, px, pw);
    bz_label(t, "wi-fi", BZ_F_LABEL, BZ_C_DIM);
    ST.wifi_state = bz_label_line(t, "", BZ_F_BODY, BZ_C_INK, iw);
    ui_button(t, BZ_I_WIFI, "scan for networks", st_scan, NULL);
    ST.wifi_list = sx_wrap_row(t, iw);
    bz_label(t, "usb tether", BZ_F_LABEL, BZ_C_DIM);
    ST.usb_state = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_width(ST.usb_state, iw);
    lv_obj_t *note = bz_label(t, "At events Wi-Fi to the robot isn't allowed: an A-to-C cable into Systemcore, or a USB-Ethernet "
                                 "adapter into the radio, on the USB-A port.",
                              BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(note, iw);

    /* pc link and the assistant: their own apps hold the detail */
    t = sx_pane(b, SS_LINK, px, pw);
    lv_obj_t *lt = bz_label(t, "Catalyst Link runs on the PC: code, patch branches, work orders for the PC's coding agent, "
                               "and the way the assistant reaches Claude.",
                            BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_width(lt, iw);
    ui_button(t, BZ_I_COMPUTER, "open link", sx_open_app, (void *)&APP_LINK);
    t = sx_pane(b, SS_ASSIST, px, pw);
    lv_obj_t *at = bz_label(t, "The assistant runs on your Claude subscription through Claude Code on the PC (Catalyst Link), "
                               "or on an API key. It reads everything the tablet sees and changes nothing without your ok.",
                            BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_width(at, iw);
    SX.link_state = bz_label(t, "", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
    ui_button(r, BZ_I_AUTO_AWESOME, "open assist", sx_open_app, (void *)&APP_ASSIST);
    ui_button(r, BZ_I_COMPUTER, "pair the pc", sx_open_app, (void *)&APP_LINK);
    ui_assist_settings(t, b, iw);

    /* home mode */
    t = sx_pane(b, SS_HOME, px, pw);
    ui_home_settings(t, b, iw);

    /* date and time */
    t = sx_pane(b, SS_TIME, px, pw);
    SX.clock = bz_label(t, "", BZ_F_NAME, BZ_C_INK);
    bz_label(t, "time zone", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
    for (int i = 0; i < 5; i++) SX.tz_chips[i] = ui_chip(r, TZ_L[i], sx_tz, (void *)(intptr_t)i);
    lv_obj_t *tn = bz_label(t, "The clock sets itself from the network whenever Wi-Fi reaches the internet, and from the PC "
                               "with tools/tab5_dev.py settime. The real-time clock keeps it through power-offs.",
                            BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(tn, iw);

    /* storage */
    t = sx_pane(b, SS_STORAGE, px, pw);
    SX.sd_state = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    r = sx_wrap_row(t, iw);
    ui_button(r, BZ_I_FOLDER, "browse the card", sx_open_app, (void *)&APP_FILES);
    ui_button(r, BZ_I_EDIT_NOTE, "clear notes", sx_clear, (void *)0);
    ui_button(r, BZ_I_CHECKLIST_RTL, "reset checklist", sx_clear, (void *)1);

    /* power */
    t = sx_pane(b, SS_POWER, px, pw);
    SX.batt = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
    r = sx_wrap_row(t, iw);
    ui_button(r, BZ_I_BEDTIME, "screen off", sx_sleep, NULL);
    SX.off_btn = ui_button(r, BZ_I_POWER, "turn off", sx_off, NULL);
    bz_label(t, "screen off when untouched for", BZ_F_LABEL, BZ_C_DIM);
    r = sx_wrap_row(t, iw);
    for (int i = 0; i < 4; i++) SX.sleep_chips[i] = ui_chip(r, SLEEP_L[i], sx_sleep_after, (void *)(intptr_t)i);
    r = sx_wrap_row(t, iw);
    SX.lock_chip = ui_chip(r, "wake to the lock screen", sx_lock, NULL);
    lv_obj_t *sn = bz_label(t, "Off, the robot link and recording keep running. A tap wakes the screen, and that tap "
                               "presses nothing; a push up opens the lock screen.", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(sn, iw);

    /* about */
    t = sx_pane(b, SS_ABOUT, px, pw);
    ST.about = bz_label(t, "", BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_width(ST.about, iw);
    lv_label_set_long_mode(ST.about, LV_LABEL_LONG_WRAP);
    ui_button(t, BZ_I_MONITORING, "system monitor", sx_open_app, (void *)&APP_SYSMON);

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
    sx_show(SS_DISPLAY);
}

const ui_app_t APP_SETTINGS = { .name = "settings", .icon = BZ_I_SETTINGS, .build = settings_build, .open = settings_open,
                                .refresh = settings_refresh };
