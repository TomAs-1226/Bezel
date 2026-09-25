/* hal_sim — the Tab5's hardware, imitated on a laptop.
 *
 * Frames stay in memory (the script's `shot` saves them), touches come from the script, and every
 * sensor produces something plausible so each tool has data: the IMU is tilted by `tilt`, the camera sees
 * a test card, and the CAN tap hears a robot bus of Talon FXs, a Pigeon 2, CANcoders, a PDH and the
 * controller's heartbeat.
 *
 * The panel is imitated as the tablet drives it — two frame buffers flipped every frame, the back one
 * brought up to date with last frame's areas and then this frame's, each from its own source — so the
 * screenshots show exactly what the tablet's present path would put on the glass. The compositor's
 * two-core split runs on a real second thread. */
#include "hal.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static uint16_t *g_content, *g_out, *g_fb[2];
static int g_back;
static bz_present_t g_prev[BZ_COMP_MAX_PRESENT];
static int g_nprev;
static uint32_t *g_ink;
static bool g_pressed;
static int g_tx, g_ty;
static float g_tilt_x = 0.08f, g_tilt_y = -0.21f;
static float g_bright = 0.8f;
static double g_t0;

double hal_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9 - g_t0;
}

bool hal_init(void)
{
    g_t0 = 0;
    g_t0 = hal_seconds();
    /* 64-byte aligned, as LVGL's draw buffers (and the P4's PPA) require */
    g_content = aligned_alloc(64, HAL_W * HAL_H * 2);
    g_out = aligned_alloc(64, HAL_W * HAL_H * 2);
    g_ink = aligned_alloc(64, HAL_W * HAL_H * 4);
    g_fb[0] = calloc(HAL_W * HAL_H, 2);
    g_fb[1] = calloc(HAL_W * HAL_H, 2);
    mkdir("sim_sd", 0755);
    return g_content && g_out && g_ink && g_fb[0] && g_fb[1];
}

/* ---- the second core ---- */
static struct {
    pthread_mutex_t m;
    pthread_cond_t go, done;
    void (*job)(void *, int);
    void *arg;
    bool busy;
} P = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, false };

static void *worker(void *u)
{
    (void)u;
    pthread_mutex_lock(&P.m);
    for (;;) {
        while (!P.busy) pthread_cond_wait(&P.go, &P.m);
        pthread_mutex_unlock(&P.m);
        P.job(P.arg, 1);
        pthread_mutex_lock(&P.m);
        P.busy = false;
        pthread_cond_signal(&P.done);
    }
    return NULL;
}

static void parallel(void (*job)(void *, int), void *arg)
{
    pthread_mutex_lock(&P.m);
    P.job = job;
    P.arg = arg;
    P.busy = true;
    pthread_cond_signal(&P.go);
    pthread_mutex_unlock(&P.m);
    job(arg, 0);
    pthread_mutex_lock(&P.m);
    while (P.busy) pthread_cond_wait(&P.done, &P.m);
    pthread_mutex_unlock(&P.m);
}

void hal_display(hal_display_t *out)
{
    static bz_gfx_ops_t ops = { .parallel = parallel };
    if (getenv("SIM_ONE_CORE")) ops.parallel = NULL;
    static bool started;
    if (!started) {
        pthread_t th;
        pthread_create(&th, NULL, worker, NULL);
        pthread_detach(th);
        started = true;
    }
    out->content = g_content;
    out->ink = g_ink;
    out->out = g_out;
    out->ops = &ops;
}

/* What's on the glass: the front buffer. */
uint16_t *sim_frame(void) { return g_fb[g_back ^ 1]; }

static void put(uint16_t *fb, const bz_present_t *p)
{
    int w = p->a.x2 - p->a.x1 + 1;
    for (int y = p->a.y1; y <= p->a.y2; y++)
        memcpy(fb + (size_t)y * HAL_W + p->a.x1, p->src + (size_t)(y - p->a.y1) * p->stride, (size_t)w * 2);
}

void hal_present(const bz_present_t *areas, int n, void *user)
{
    (void)user;
    uint16_t *fb = g_fb[g_back];
    /* the back buffer is a frame old: last frame's areas first, then this frame's over them */
    for (int i = 0; i < g_nprev; i++) put(fb, &g_prev[i]);
    for (int i = 0; i < n; i++) put(fb, &areas[i]);
    memcpy(g_prev, areas, sizeof(bz_present_t) * (size_t)n);
    g_nprev = n;
    g_back ^= 1;
}

void sim_touch(bool pressed, int x, int y)
{
    g_pressed = pressed;
    g_tx = x;
    g_ty = y;
}

bool hal_touch(int *x, int *y, void *user)
{
    (void)user;
    if (!g_pressed) return false;
    *x = g_tx;
    *y = g_ty;
    return true;
}

void hal_set_brightness(float v) { g_bright = v; }
static bool g_flip;
void hal_set_flip(bool flip) { g_flip = flip; }
bool hal_flip(void) { return g_flip; }
void hal_dev_set_handler(bool (*fn)(const char *line)) { (void)fn; }
const char *hal_panel_name(void) { return "simulator"; }

void sim_tilt(float x, float y) { g_tilt_x = x; g_tilt_y = y; }

bool hal_imu(hal_imu_t *o)
{
    double t = hal_seconds();
    /* held in a hand: the tilt the script set, plus a little tremor */
    float jx = 0.004f * sinf((float)t * 7.1f), jy = 0.004f * sinf((float)t * 5.3f + 1);
    o->ax = sinf(g_tilt_x) + jx;
    o->ay = sinf(g_tilt_y) + jy;
    o->az = -sqrtf(fmaxf(0, 1 - o->ax * o->ax - o->ay * o->ay)); /* face up: gravity into the glass */
    o->gx = 0.3f * sinf((float)t * 3);
    o->gy = 0.2f * cosf((float)t * 2);
    o->gz = 0;
    o->ok = true;
    return true;
}

bool hal_battery(hal_battery_t *o)
{
    o->volts = 7.92f;
    o->amps = -0.41f;
    o->percent = 71;
    o->charging = false;
    o->external = false;
    o->ok = true;
    return true;
}

/* A gearbox: a whine whose frequency follows a motor ramping between two speeds, its second and third
 * harmonics, a tooth-mesh sideband and broadband noise. */
void hal_tone(float hz, int ms, float v) { (void)hz; (void)ms; (void)v; }
void hal_set_volume(float v) { (void)v; }
/* no sound card: speech is swallowed at once, and nothing listens */
bool hal_play_start(int rate) { return rate > 0 && 48000 % rate == 0; }
int hal_play_write(const int16_t *pcm, int n, int timeout_ms) { (void)pcm; (void)timeout_ms; return n; }
void hal_play_end(void) {}
void hal_play_stop(void) {}
bool hal_play_busy(void) { return false; }
float hal_play_level(void) { return 0; }
bool hal_mic_start(void) { return false; }
int hal_mic_read(int16_t *out, int max) { (void)out; (void)max; return 0; }
void hal_mic_stop(void) {}
bool hal_mic_on(void) { return false; }
float hal_mic_level(void) { return 0; }
void hal_mic_channel(int ch) { (void)ch; }
bool hal_wake_ready(const char **word) { if (word) *word = NULL; return false; }
void hal_wake_reset(void) {}
bool hal_wake_feed(const int16_t *pcm, int n) { (void)pcm; (void)n; return false; }

#define CAM_W 640
#define CAM_H 360
static uint16_t g_cam[CAM_W * CAM_H];
static bool g_cam_on;

bool hal_camera_start(void) { g_cam_on = true; return true; }
void hal_camera_stop(void) { g_cam_on = false; }

static uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

const uint16_t *hal_camera_frame(int *w, int *h)
{
    if (!g_cam_on) return NULL;
    double t = hal_seconds();
    /* the inside of a gearbox, more or less: a dark housing, a turning gear, warm light from the left */
    float cx = CAM_W * 0.55f, cy = CAM_H * 0.5f, ang = (float)t * 1.3f;
    for (int y = 0; y < CAM_H; y++) {
        for (int x = 0; x < CAM_W; x++) {
            float dx = x - cx, dy = y - cy, r = sqrtf(dx * dx + dy * dy);
            float a = atan2f(dy, dx) + ang;
            float tooth = 110 + 9 * (fmodf(a * 18 / (2 * (float)M_PI) + 100, 1.0f) < 0.5f ? 1 : -1);
            int base = 26 + (int)(40 * (1 - x / (float)CAM_W));
            uint16_t p;
            if (r < 28) p = rgb(40, 42, 46);
            else if (r < tooth) {
                int shade = 120 + (int)(60 * cosf(a * 3)) - (int)(r * 0.3f);
                p = rgb(shade + 20, shade + 10, shade);
            } else p = rgb(base + 12, base + 8, base + 4);
            g_cam[y * CAM_W + x] = p;
        }
    }
    *w = CAM_W;
    *h = CAM_H;
    return g_cam;
}

int png_write_rgb565(const char *path, const uint16_t *px, int w, int h);
/* no video plane in the simulator: the lens app draws the preview through LVGL */
bool hal_camera_plane(int bx, int by, int bw, int bh, int *x, int *y, int *w, int *h)
{
    (void)bx; (void)by; (void)bw; (void)bh; (void)x; (void)y; (void)w; (void)h;
    return false;
}
void hal_camera_plane_off(void) {}
void hal_camera_freeze(bool freeze) { (void)freeze; }
uint32_t hal_camera_gen(void) { return 0; }

bool hal_camera_snapshot(const char *path)
{
    int w, h;
    const uint16_t *f = hal_camera_frame(&w, &h);
    return f && png_write_rgb565(path, f, w, h) == 0;
}

bool hal_rtc_get(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

void hal_rtc_set(const struct tm *t) { (void)t; }

const char *hal_sd_root(void) { return "sim_sd"; }
bool hal_sd_space(uint64_t *total, uint64_t *free_bytes) { *total = 32ull << 30; *free_bytes = 30ull << 30; return true; }

/* no JPEG decoder on the host: the photos app shows the error path */
bool hal_jpeg_load(const char *path, int max_w, int max_h, hal_picture_t *out, char *err, size_t errn)
{
    (void)path; (void)max_w; (void)max_h;
    memset(out, 0, sizeof *out);
    if (err && errn) snprintf(err, errn, "no JPEG decoder in the simulator");
    return false;
}

void hal_picture_free(hal_picture_t *p)
{
    if (!p) return;
    free(p->px);
    p->px = NULL;
}

bool hal_kv_get(const char *key, char *buf, size_t n)
{
    char path[160];
    snprintf(path, sizeof path, "sim_sd/.kv_%s", key);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t k = fread(buf, 1, n - 1, f);
    buf[k] = 0;
    fclose(f);
    return true;
}

void hal_kv_set(const char *key, const char *value)
{
    char path[160];
    snprintf(path, sizeof path, "sim_sd/.kv_%s", key);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(value, f);
    fclose(f);
}

void hal_wifi_rejoin(void) {}

void hal_net_report(char *out, size_t n, const char *host)
{
    (void)host;
    snprintf(out, n, "simulator");
}

void hal_net(hal_net_t *o)
{
    memset(o, 0, sizeof *o);
    o->link = HAL_LINK_SIM;
    o->up = true;
    snprintf(o->ssid, sizeof o->ssid, "5805_pit");
    o->rssi = -52;
    snprintf(o->ip, sizeof o->ip, "127.0.0.1");
}

void hal_wifi_join(const char *ssid, const char *pass) { (void)ssid; (void)pass; }

int hal_wifi_scan(hal_ap_t *out, int max)
{
    static const hal_ap_t aps[] = { { "5805_pit", -48, true }, { "5805", -61, true }, { "FRC-Pit-Guest", -70, false } };
    int n = 0;
    for (; n < max && n < 3; n++) out[n] = aps[n];
    return n;
}

/* ---- a robot's CAN bus, frame by frame ---- */
static bool g_can_on;
static double g_can_last;
static hal_can_stats_t g_can_stats;

static uint32_t frc_id(int type, int mfr, int api_class, int api_index, int dev)
{
    return (uint32_t)(type & 31) << 24 | (uint32_t)(mfr & 255) << 16 | (uint32_t)(api_class & 63) << 10 |
           (uint32_t)(api_index & 15) << 6 | (uint32_t)(dev & 63);
}

bool hal_can_start(int bitrate)
{
    (void)bitrate;
    g_can_on = true;
    g_can_last = hal_seconds();
    g_can_stats.state = 1;
    return true;
}

int hal_can_read(hal_can_frame_t *out, int max)
{
    if (!g_can_on) return 0;
    double now = hal_seconds();
    int ticks = (int)((now - g_can_last) * 1000); /* one "millisecond" of bus per tick */
    if (ticks <= 0) return 0;
    g_can_last += ticks / 1000.0;
    int n = 0;
    for (int ms = 0; ms < ticks && n < max; ms++) {
        int t = (int)(g_can_last * 1000) - ticks + ms;
        /* eight Talon FXs (1–8) at 250 Hz, device 27 has dropped off the bus */
        if (t % 4 == 0) for (int d = 1; d <= 8 && n < max; d++) {
            hal_can_frame_t *f = &out[n++];
            f->id = frc_id(2, 4, 0x06, 1 + (t / 4 + d) % 4, d);
            f->len = 8;
            f->ext = true;
            for (int k = 0; k < 8; k++) f->data[k] = (uint8_t)(t * 7 + d * 13 + k);
            f->t_us = (int64_t)(now * 1e6);
        }
        /* Pigeon 2 (20) and four CANcoders (21–24) at 100 Hz */
        if (t % 10 == 0) for (int d = 20; d <= 24 && n < max; d++) {
            hal_can_frame_t *f = &out[n++];
            f->id = frc_id(d == 20 ? 4 : 7, 4, 0x06, 2, d);
            f->len = 8;
            f->ext = true;
            memset(f->data, d, 8);
            f->t_us = (int64_t)(now * 1e6);
        }
        /* REV PDH (1) at 50 Hz, the controller's heartbeat at 50 Hz */
        if (t % 20 == 0 && n + 1 < max) {
            hal_can_frame_t *f = &out[n++];
            f->id = frc_id(8, 5, 0x19, 0, 1);
            f->len = 8;
            f->ext = true;
            memset(f->data, 0x21, 8);
            f->t_us = (int64_t)(now * 1e6);
            f = &out[n++];
            f->id = 0x01011840;
            f->len = 8;
            f->ext = true;
            memset(f->data, 0, 8);
            f->data[5] = 0x14;
            f->t_us = (int64_t)(now * 1e6);
        }
    }
    g_can_stats.rx += (uint32_t)n;
    return n;
}

void hal_can_stats(hal_can_stats_t *o) { *o = g_can_stats; }
void hal_can_stop(void) { g_can_on = false; g_can_stats.state = 0; }

/* ---- NFC: no reader on a laptop. SIM_NFC_TAG=<hex uid> in the environment puts that tag on it. ---- */
bool hal_nfc_present(void) { return getenv("SIM_NFC_TAG") != NULL; }

bool hal_nfc_card(char *uid, size_t n)
{
    const char *t = getenv("SIM_NFC_TAG");
    if (!t || !t[0]) return false;
    snprintf(uid, n, "%s", t);
    return true;
}

void hal_sys(hal_sys_t *o)
{
    o->psram_free = 24u << 20;
    o->sram_free = 380u << 10;
    o->cpu[0] = 0.21f;
    o->cpu[1] = 0.46f;
    o->temp_c = 41.5f;
    snprintf(o->chip, sizeof o->chip, "simulator");
}

void hal_power_off(void) {}

void hal_start(void) {}
void hal_settle(void) {}

/* The start-up record: SIM_PREV_BOOT="reason:stage:fails[:detail]" pretends the last start failed. */
void hal_boot_stage(const char *stage) { (void)stage; }
void hal_boot_ok(void) {}
void hal_boot_watch(void (*fn)(const char *stage)) { (void)fn; }
void hal_boot_prev(hal_boot_t *o)
{
    memset(o, 0, sizeof *o);
    const char *e = getenv("SIM_PREV_BOOT");
    if (!e || !*e) return;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL, *reason = strtok_r(buf, ":", &save), *stage = strtok_r(NULL, ":", &save);
    char *fails = strtok_r(NULL, ":", &save), *detail = strtok_r(NULL, "", &save);
    o->failed = true;
    snprintf(o->reason, sizeof o->reason, "%s", reason ? reason : "crash");
    snprintf(o->stage, sizeof o->stage, "%s", stage ? stage : "start");
    o->fails = fails ? atoi(fails) : 1;
    if (detail) snprintf(o->detail, sizeof o->detail, "%s", detail);
}
