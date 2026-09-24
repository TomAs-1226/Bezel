/* hal_tab5 — Catalyst Tab's hardware layer on the M5Stack Tab5.
 *
 * Built on Espressif's Tab5 BSP (espressif/m5stack_tab5) for what it does well — panel detection and
 * bring-up for all three panel revisions, touch, the audio codecs, the camera's sensor pipeline, the
 * microSD slot — and on ESP-IDF directly for the rest. docs/tab5-hardware.md has the pin map and the
 * research behind every number here. The network half (Wi-Fi, the USB tether, mDNS, HTTP, threads) is
 * hal_tab5_net.c.
 *
 * Tasks: the UI (LVGL + compositor) runs on core 1; this file's workers — the frame hand-over, tones,
 * camera capture, the clip encoder, CAN — run on core 0 beside the NetworkTables client, so the renderer
 * never waits on a peripheral. The microphones (ES7210) are deliberately left unused.
 *
 * Verified by compiling only: this file has not yet run on a Tab5. Items marked UNVERIFIED are the
 * ones most likely to need a correction on first boot. */
#include "hal.h"
#include "bz_ui.h" /* BZ_LEAN */
#include "hal_tab5_priv.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "bmi270.h"
#include "esp_vfs_fat.h"
#include "esp_lcd_touch.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "driver/temperature_sensor.h"
#include "driver/twai.h"
#include "esp_cache.h"
#include "esp_async_fbcpy.h" /* esp_lcd's private DMA2D copier (priv_include added in CMakeLists) */
#include "hal/color_types.h"
#include "esp_codec_dev.h"
#include "esp_core_dump.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "hal";

/* Landscape on a portrait panel. The PPA rotates counter-clockwise; 90° puts the USB-C port on the
 * right-hand edge. UNVERIFIED which way a Tab5 is held most naturally — flip with ROTATE_270. */
/* Which way up is chosen at run time (hal_set_flip): false turns the picture 90° (USB-C on the right),
 * true 270°. CATALYST_ROTATE_270 only picks the default. */
#ifndef CATALYST_ROTATE_270
static bool s_flip = false;
#else
static bool s_flip = true;
#endif
#define ROT (s_flip ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90)

#define PANEL_W BSP_LCD_H_RES /* 720 */
#define PANEL_H BSP_LCD_V_RES /* 1280 */

/* Grove Port A carries the CAN transceiver's TX and RX. UNVERIFIED which wire a given Grove CAN unit
 * uses for which; swap if the tap hears nothing on a live bus. */
#define CAN_TX_GPIO 53
#define CAN_RX_GPIO 54

/* expander pins (docs/tab5-hardware.md); E2's are further down, with the code that drives it */
#define E1_ANTENNA IO_EXPANDER_PIN_NUM_0
#define E1_SPK_EN IO_EXPANDER_PIN_NUM_1
#define E1_EXT5V IO_EXPANDER_PIN_NUM_2

static struct {
    esp_io_expander_handle_t e1;
    i2c_master_bus_handle_t i2c;
    i2c_master_dev_handle_t ina, rtc, e2;
    bsp_lcd_handles_t lcd;
    void *fb[2];
    int back;
    SemaphoreHandle_t vsync;
    uint32_t lane_mbps;   /* MIPI lane rate: 1000, or 965 on the ST7121 */
    volatile uint32_t vsyncs; /* frames the panel has scanned out, for the measured refresh rate */
    double display_t0;
    bool lit;                 /* the backlight is on: only once a real frame is on the glass */
    /* one SRM client per task that uses it (a client queues one blocking transaction at a time): the
     * UI thread's copies and blocking rotations, and the camera task's scaling and clip conversion */
    ppa_client_handle_t ppa_srm, ppa_cam, ppa_blend;
    bool ppa_blend_ok;
    esp_lcd_touch_handle_t touch;
    uint16_t *content, *out;
    uint32_t *ink;
    bmi270_handle_t *imu;
    temperature_sensor_handle_t tsens;
    char panel[16];
    bool sd;
    float volume;
} T;

double hal_seconds(void) { return esp_timer_get_time() / 1e6; }

static void *psram_aligned(size_t n)
{
    /* 128 bytes: the P4's L2 cache line, which the PPA wants for any buffer it writes */
    return heap_caps_aligned_calloc(128, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ------------------------------------------------------------------ power and expanders */

/* Expander 0x44 (E2) holds the ESP32-C6's power, the USB-A port's 5 V and the charger. It is driven here
 * register by register and never through the esp_io_expander driver, whose start-up resets the chip:
 * that drops every output for a moment, which power-cycles the C6 under a running esp-hosted link (and
 * esp-hosted answers a lost link with esp_restart(), every start) and stops charging. Nothing else may
 * create a handle for it — the BSP only would for BSP_FEATURE_WIFI/USB, which this firmware never calls.
 * PI4IOE5V6408 registers: 0x03 direction (1 out), 0x05 output, 0x07 output high-Z (1 floating),
 * 0x0B pull-up/down enable. */
#define E2_ADDR 0x44
#define PI_DIR 0x03
#define PI_OUT 0x05
#define PI_HIZ 0x07

static uint8_t e2_read(uint8_t reg)
{
    uint8_t v = 0;
    if (T.e2) i2c_master_transmit_receive(T.e2, &reg, 1, &v, 1, 50);
    return v;
}

static void e2_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    if (T.e2) i2c_master_transmit(T.e2, b, 2, 50);
}

/* One E2 pin as a driven output at `level`, the others left exactly as they are: level first, then
 * direction, then out of high-Z, so the pin never passes through the opposite level. */
static void e2_set(int pin, bool level)
{
    uint8_t bit = (uint8_t)(1u << pin);
    uint8_t out = e2_read(PI_OUT);
    e2_write(PI_OUT, level ? (out | bit) : (out & ~bit));
    e2_write(PI_DIR, e2_read(PI_DIR) | bit);
    e2_write(PI_HIZ, e2_read(PI_HIZ) & ~bit);
}

#define E2_WLAN_PWR 0
#define E2_USB5V 3
#define E2_PWROFF 4
#define E2_NCHG_QC 5
#define E2_CHG_STAT 6
#define E2_CHG_EN 7

static void power_init(void)
{
    i2c_device_config_t c = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = E2_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(T.i2c, &c, &T.e2) != ESP_OK) T.e2 = NULL;
    /* the power-off line low first (it's the one pin whose wrong level turns the tablet off), then the
     * C6 on and kept on, then charging as M5Stack's own firmware does. The USB-A port's 5 V waits for
     * hal_settle(), and the Grove port's for the CAN tap. */
    e2_set(E2_PWROFF, 0);
    e2_set(E2_WLAN_PWR, 1);
    e2_set(E2_NCHG_QC, 0);
    e2_set(E2_CHG_EN, 1);
    /* E1 (0x43: antenna, speaker, Grove 5 V, the LCD/touch/camera resets) through the BSP, which the
     * display and touch bring-up use too; its reset at creation is harmless here, before the panel */
    T.e1 = bsp_io_expander_init();
    esp_io_expander_set_dir(T.e1, E1_ANTENNA | E1_EXT5V, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(T.e1, E1_ANTENNA, 0); /* the internal 3D antenna (M5Stack: RF path, low = internal) */
    esp_io_expander_set_level(T.e1, E1_EXT5V, 0);   /* Grove 5 V off until the CAN tap wants it */
    /* the driver's reset leaves every output high-Z: without this both pins float whatever their level,
     * and a floating RF switch connects neither antenna (every scan came back empty) */
    esp_io_expander_set_output_mode(T.e1, E1_ANTENNA | E1_EXT5V, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
}

void hal_antenna(bool external)
{
    if (T.e1) esp_io_expander_set_level(T.e1, E1_ANTENNA, external);
}

static void ext5v(bool on)
{
    if (T.e1) esp_io_expander_set_level(T.e1, E1_EXT5V, on);
}

void hal_power_off(void)
{
    /* the power MCU turns the tablet off on three pulses */
    for (int i = 0; i < 3; i++) {
        e2_set(E2_PWROFF, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        e2_set(E2_PWROFF, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ------------------------------------------------------------------ small I2C devices */

static i2c_master_dev_handle_t i2c_dev(uint8_t addr)
{
    i2c_device_config_t c = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_probe(T.i2c, addr, 50) != ESP_OK || i2c_master_bus_add_device(T.i2c, &c, &h) != ESP_OK) return NULL;
    return h;
}

static bool reg_write16(i2c_master_dev_handle_t d, uint8_t reg, uint16_t v)
{
    uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return d && i2c_master_transmit(d, b, 3, 50) == ESP_OK;
}

static bool reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    return d && i2c_master_transmit_receive(d, &reg, 1, buf, n, 50) == ESP_OK;
}

static void ina226_init(void)
{
    /* 5 mΩ shunt, 8.192 A full scale (M5's demo): current LSB 250 µA, calibration 0.00512/(LSB·R) = 4096 */
    T.ina = i2c_dev(0x41);
    reg_write16(T.ina, 0x00, 0x4527); /* 16-sample average, 1.1 ms conversions, shunt and bus continuous */
    reg_write16(T.ina, 0x05, 4096);
}

/* A 2S Li-ion pack by voltage per cell: there is no fuel gauge, so this is an estimate */
static int percent_from_cell(float v)
{
    static const float V[] = { 3.00f, 3.30f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f };
    static const float P[] = { 0, 5, 15, 35, 55, 70, 82, 92, 100 };
    if (v <= V[0]) return 0;
    for (int i = 1; i < 9; i++)
        if (v <= V[i]) return (int)(P[i - 1] + (P[i] - P[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]));
    return 100;
}

bool hal_battery(hal_battery_t *o)
{
    memset(o, 0, sizeof *o);
    uint8_t b[2];
    if (!reg_read(T.ina, 0x02, b, 2)) return false;
    o->volts = (uint16_t)(b[0] << 8 | b[1]) * 1.25e-3f;
    if (reg_read(T.ina, 0x04, b, 2)) o->amps = (int16_t)(b[0] << 8 | b[1]) * 250e-6f;
    o->charging = o->amps > 0.05f; /* positive current is charging (M5's demo) */
    o->external = o->charging;
    o->percent = percent_from_cell(o->volts / 2);
    o->ok = true;
    return true;
}

static uint8_t bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 15)); }
static uint8_t tobcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

bool hal_rtc_get(struct tm *out)
{
    uint8_t r[7];
    if (!reg_read(T.rtc, 0x10, r, 7)) {
        time_t now = time(NULL);
        localtime_r(&now, out);
        return false;
    }
    memset(out, 0, sizeof *out);
    out->tm_sec = bcd(r[0] & 0x7f);
    out->tm_min = bcd(r[1] & 0x7f);
    out->tm_hour = bcd(r[2] & 0x3f);
    out->tm_mday = bcd(r[4] & 0x3f);
    out->tm_mon = bcd(r[5] & 0x1f) - 1;
    out->tm_year = bcd(r[6]) + 100;
    return true;
}

void hal_rtc_set(const struct tm *t)
{
    uint8_t b[8] = { 0x10, tobcd(t->tm_sec), tobcd(t->tm_min), tobcd(t->tm_hour), (uint8_t)(1 << t->tm_wday),
                     tobcd(t->tm_mday), tobcd(t->tm_mon + 1), tobcd(t->tm_year % 100) };
    if (T.rtc) i2c_master_transmit(T.rtc, b, sizeof b, 50);
}

static void rtc_init(void)
{
    T.rtc = i2c_dev(0x32);
    struct tm tm;
    if (hal_rtc_get(&tm) && tm.tm_year > 120) {
        /* the supercap kept time: start the system clock from it */
        struct timeval tv = { .tv_sec = mktime(&tm) };
        settimeofday(&tv, NULL);
    }
}

/* ------------------------------------------------------------------ IMU */

static void imu_init(void)
{
    bmi270_driver_config_t cfg = { .addr = BMI270_I2C_ADDRESS_L, .interface = BMI270_USE_I2C, .i2c_bus = T.i2c };
    if (bmi270_create(&cfg, &T.imu) != ESP_OK) {
        T.imu = NULL;
        ESP_LOGW(TAG, "BMI270 not answering (after a hard power loss, wait 5 s before powering on)");
        return;
    }
    bmi270_config_t run = { .acce_odr = BMI270_ACC_ODR_100_HZ, .acce_range = BMI270_ACC_RANGE_2_G,
                            .gyro_odr = BMI270_GYR_ODR_100_HZ, .gyro_range = BMI270_GYR_RANGE_500_DPS };
    bmi270_start(T.imu, &run);
}

bool hal_imu(hal_imu_t *o)
{
    memset(o, 0, sizeof *o);
    float x, y, z, gx, gy, gz;
    if (!T.imu || bmi270_get_acce_data(T.imu, &x, &y, &z) != ESP_OK) return false;
    bmi270_get_gyro_data(T.imu, &gx, &gy, &gz);
    /* The sensor's axes in the panel's native portrait frame, then turned into the landscape frame the
     * UI uses (the same turn the PPA gives the picture), and negated: an accelerometer at rest reads the
     * reaction to gravity, the UI wants gravity's direction. UNVERIFIED against a unit: if the Level
     * tool reads mirrored, flip the signs here. */
    float px = x, py = -y;
    float lx = s_flip ? py : -py, ly = s_flip ? -px : px;
    /* Measured on a unit: the BMI270 sits turned about its y axis, so its x and z read opposite to the
     * panel's. Landscape y comes from raw x and z is raw z: those two take the other sign (auto-rotate
     * picked the wrong way up, and Level read ~180° lying flat); landscape x, from raw y, doesn't. */
    o->ax = -lx;
    o->ay = ly;
    o->az = z;
    o->gx = gx;
    o->gy = gy;
    o->gz = gz;
    o->ok = true;
    return true;
}

/* ------------------------------------------------------------------ display */

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *user)
{
    BaseType_t woken = pdFALSE;
    T.vsyncs++;
    xSemaphoreGiveFromISR(T.vsync, &woken);
    return woken == pdTRUE;
}

/* Tells the panels apart exactly as the BSP's own bsp_get_board_version() does — the same 500 ms for the
 * touch controller to come out of reset, the same register — because the BSP picks the panel's init
 * sequence and timing from its answer, and the lane rate has to agree: 965 Mbps on the ST7121, the BSP's
 * 1000 on the others. The panel runs at the BSP's own timing: nothing here changes it. */
static uint32_t lane_rate(void)
{
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    T.lane_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
    if (i2c_master_probe(T.i2c, 0x55, 100) != ESP_OK) {
        snprintf(T.panel, sizeof T.panel, "ILI9881C");
        return T.lane_mbps;
    }
    i2c_master_dev_handle_t tp = i2c_dev(0x55);
    uint8_t reg[2] = { 0, 0 }, fw = 0;
    if (tp) i2c_master_transmit_receive(tp, reg, 2, &fw, 1, 50);
    if (tp) i2c_master_bus_rm_device(tp);
    snprintf(T.panel, sizeof T.panel, fw == 1 ? "ST7121" : "ST7123");
    if (fw == 1) T.lane_mbps = 965;
    return T.lane_mbps;
}

/* ---- presenting: the finished landscape areas, turned into the portrait back buffer ----
 *
 * Built with CATALYST_ASYNC_PRESENT, hal_present() is asynchronous, so the UI core composes frame N+1
 * while the PPA turns frame N (the default is the synchronous version further down):
 *
 *   hal_present(N+1)  waits for frame N's hand-over (at most one frame in flight), copies the area list,
 *                     queues the rotations — frame N's areas first (the back buffer is one frame old, and
 *                     their sources persist), then N+1's, so the newer pixels win where they overlap — as
 *                     non-blocking PPA transactions, and returns.
 *   present task      (core 0) waits for the PPA's completion callbacks, gives the finished buffer to the
 *                     DPI controller, waits for the vsync that makes it the front buffer, swaps, and
 *                     hands over.
 *
 * The rotations are submitted from the caller's core, the one that wrote the pixels: the PPA driver writes
 * each source's cache lines back as it queues them. UNVERIFIED on the unit: the pacing (no "overdue"
 * warnings) and that nothing stale survives the catch-up. */
#define PRESENT_MAX (2 * BZ_COMP_MAX_DIRTY) /* areas remembered for the catch-up */
#define PRESENT_ASYNC 64                    /* PPA transactions in flight; more fall back to blocking */

typedef struct {
    void *fb;
    int pending; /* non-blocking rotations to wait for */
} present_job_t;

static struct {
    bz_present_t prev[PRESENT_MAX];
    int nprev;
    ppa_client_handle_t ppa;
    SemaphoreHandle_t done;     /* counts finished rotations (given from the PPA's interrupt) */
    SemaphoreHandle_t handover; /* free: the previous frame is on the glass */
    QueueHandle_t jobs;
    uint32_t overflow, late;
} P;

#ifdef CATALYST_ASYNC_PRESENT
static bool IRAM_ATTR on_rotated(ppa_client_handle_t client, ppa_event_data_t *e, void *user)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(P.done, &woken);
    return woken == pdTRUE;
}
#endif

/* A PPA input block, given only its first pixel and row pitch. The PPA reads from any address; the block is
 * described from the largest alignment (64, else 4 bytes) at or before the first pixel that still fits the
 * driver's picture check, the same form as addressing a whole buffer with offsets. UNVERIFIED: the PPA
 * documents no input alignment and none is checked; if 2D-DMA wanted more, rectangles would come out
 * shifted or garbled. */
typedef struct {
    const void *buf;
    uint32_t pic_w, pic_h, off_x;
} in_rect_t;

static in_rect_t in_rect(const void *first, int stride, int bpp, int w, int h)
{
    static const uintptr_t aligns[] = { 64, 4 };
    for (int i = 0; i < 2; i++) {
        uintptr_t p = (uintptr_t)first, a = p & ~(aligns[i] - 1);
        uint32_t off = (uint32_t)((p - a) / (uintptr_t)bpp);
        if ((p - a) % (uintptr_t)bpp == 0 && off + (uint32_t)w <= (uint32_t)stride)
            return (in_rect_t){ (const void *)a, (uint32_t)stride, (uint32_t)h, off };
    }
    return (in_rect_t){ first, (uint32_t)stride, (uint32_t)h, 0 };
}

/* The PPA's blocking mode waits forever, and twice in ~25 starts a rotation never finished (during the
 * C6's bring-up): the boot card, then the whole interface, stopped on it. A present's rotations go through
 * their own client, each waited for with a timeout; a rotation that doesn't come back marks the PPA
 * wedged, and the CPU turns the picture (slower, but the tablet stays usable) until the late one lands. */
static struct {
    ppa_client_handle_t client;
    SemaphoreHandle_t done;
    bool wedged;
    unsigned wedges;
} ROTQ;

static bool rot_done_cb(ppa_client_handle_t c, ppa_event_data_t *e, void *u)
{
    (void)c; (void)e; (void)u;
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(ROTQ.done, &woke);
    return woke == pdTRUE;
}

static void rot_init(void)
{
    ROTQ.done = xSemaphoreCreateBinary();
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (!ROTQ.done || ppa_register_client(&pc, &ROTQ.client) != ESP_OK) {
        ROTQ.client = NULL;
        return;
    }
    ppa_event_callbacks_t cbs = { .on_trans_done = rot_done_cb };
    ppa_client_register_event_callbacks(ROTQ.client, &cbs);
}

static void rotate_cpu(const bz_present_t *p, uint16_t *fb)
{
    const bz_area_t *a = &p->a;
    const uint16_t *src = p->src;
    for (int y = a->y1; y <= a->y2; y++, src += p->stride) {
        const uint16_t *s = src;
        if (!s_flip) {
            /* landscape (x, y) lands at portrait (y, W-1-x) */
            uint16_t *d = fb + (size_t)(HAL_W - 1 - a->x1) * PANEL_W + y;
            for (int x = a->x1; x <= a->x2; x++, d -= PANEL_W) *d = *s++;
        } else {
            uint16_t *d = fb + (size_t)a->x1 * PANEL_W + (HAL_H - 1 - y);
            for (int x = a->x1; x <= a->x2; x++, d += PANEL_W) *d = *s++;
        }
    }
    /* out of the cache now: a PPA rotation later in the frame invalidates its rows without writing them
     * back, which would drop these */
    int r0 = s_flip ? a->x1 : HAL_W - 1 - a->x2, r1 = s_flip ? a->x2 : HAL_W - 1 - a->x1;
    esp_cache_msync(fb + (size_t)r0 * PANEL_W, (size_t)(r1 - r0 + 1) * PANEL_W * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* One area from its own source into the portrait back buffer. true: queued non-blocking (wait for it). */
static bool rotate_area(const bz_present_t *p, void *fb, bool async)
{
    const bz_area_t *a = &p->a;
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    if (w <= 0 || h <= 0 || !p->src) return false;
    in_rect_t in = in_rect(p->src, p->stride, 2, w, h);
    ppa_srm_oper_config_t op = {
        .in = { .buffer = in.buf, .pic_w = in.pic_w, .pic_h = in.pic_h, .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                .block_offset_x = in.off_x, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = fb, .buffer_size = PANEL_W * PANEL_H * 2, .pic_w = PANEL_W, .pic_h = PANEL_H,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = ROT,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    if (!s_flip) {
        /* 90° counter-clockwise: landscape (x, y) lands at portrait (y, W-1-x) */
        op.out.block_offset_x = (uint32_t)a->y1;
        op.out.block_offset_y = (uint32_t)(HAL_W - 1 - a->x2);
    } else {
        op.out.block_offset_x = (uint32_t)(HAL_H - 1 - a->y2);
        op.out.block_offset_y = (uint32_t)a->x1;
    }
#ifdef CATALYST_ASYNC_PRESENT
    if (async && ppa_do_scale_rotate_mirror(P.ppa, &op) == ESP_OK) return true;
#else
    (void)async;
#endif
    /* A sliver never goes to the PPA: a 354×6 rotation hung it on nearly every start (its 2D-DMA doesn't
     * finish a turned block a few pixels across), and for a few thousand pixels the CPU is quicker than
     * the PPA's setup anyway. */
    if (w < 16 || h < 16 || w * h <= 4096) {
        rotate_cpu(p, fb);
        return false;
    }
    if (ROTQ.client) {
        /* a wedged PPA that has since finished its late rotation is back in service */
        if (ROTQ.wedged && xSemaphoreTake(ROTQ.done, 0) == pdTRUE) {
            ROTQ.wedged = false;
            ESP_LOGW(TAG, "present: the PPA came back");
        }
        if (!ROTQ.wedged) {
            op.mode = PPA_TRANS_MODE_NON_BLOCKING;
            if (ppa_do_scale_rotate_mirror(ROTQ.client, &op) == ESP_OK) {
                if (xSemaphoreTake(ROTQ.done, pdMS_TO_TICKS(300)) == pdTRUE) return false;
                ROTQ.wedged = true;
                ESP_LOGE(TAG, "present: a PPA rotation (%dx%d) didn't finish in 300 ms (%u so far): the CPU turns "
                              "the picture until it does", w, h, ++ROTQ.wedges);
            }
        }
        rotate_cpu(p, fb);
        return false;
    }
    /* no client of its own: blocking, which also waits out everything queued before it */
    op.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_scale_rotate_mirror(T.ppa_srm, &op);
    return false;
}

static void rows_of(const bz_area_t *a, int *y0, int *y1);

static bool covers(const bz_area_t *o, const bz_area_t *a)
{
    return o->x1 <= a->x1 && o->y1 <= a->y1 && o->x2 >= a->x2 && o->y2 >= a->y2;
}

#ifdef CATALYST_ASYNC_PRESENT
void hal_present(const bz_present_t *areas, int n, void *user)
{
    (void)user;
    /* at most one frame in flight: frame N must be on the glass before N+1 goes into the back buffer */
    if (xSemaphoreTake(P.handover, pdMS_TO_TICKS(500)) != pdTRUE) ESP_LOGW(TAG, "present: hand-over overdue");
    void *fb = T.fb[T.back];
    int pending = 0;
    for (int i = 0; i < P.nprev; i++) {
        bool covered = false;
        for (int j = 0; j < n && !covered; j++) covered = covers(&areas[j].a, &P.prev[i].a);
        if (!covered) pending += rotate_area(&P.prev[i], fb, pending < PRESENT_ASYNC);
    }
    for (int i = 0; i < n; i++) pending += rotate_area(&areas[i], fb, pending < PRESENT_ASYNC);
    /* this frame's list becomes the next one's catch-up: a copy, never the caller's array */
    int keep = n < PRESENT_MAX ? n : PRESENT_MAX;
    if (keep < n && !P.overflow++) ESP_LOGW(TAG, "present: %d areas, catch-up keeps %d", n, PRESENT_MAX);
    memcpy(P.prev, areas, sizeof *areas * (size_t)keep);
    P.nprev = keep;
    present_job_t job = { fb, pending };
    xQueueSend(P.jobs, &job, portMAX_DELAY);
}

static void present_task(void *arg)
{
    (void)arg;
    present_job_t job;
    for (;;) {
        if (xQueueReceive(P.jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        for (int i = 0; i < job.pending; i++)
            if (xSemaphoreTake(P.done, pdMS_TO_TICKS(200)) != pdTRUE) {
                ESP_LOGW(TAG, "present: PPA rotation overdue");
                break;
            }
        /* hand the DPI controller the finished buffer; it switches at the next frame, and the old front
         * buffer is free to draw into once that frame has gone out */
        xSemaphoreTake(T.vsync, 0);
        esp_lcd_panel_draw_bitmap(T.lcd.panel, 0, 0, PANEL_W, PANEL_H, job.fb);
        xSemaphoreTake(T.vsync, pdMS_TO_TICKS(40));
        T.back ^= 1;
        if (!T.lit) {
            T.lit = true;
            bsp_display_brightness_set(70);
        }
        xSemaphoreGive(P.handover);
    }
}

static void present_init(void)
{
    /* its own SRM client, so its non-blocking queue is deep; the PPA runs transactions in order */
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = PRESENT_ASYNC };
    ESP_ERROR_CHECK(ppa_register_client(&pc, &P.ppa));
    ppa_event_callbacks_t cbs = { .on_trans_done = on_rotated };
    ppa_client_register_event_callbacks(P.ppa, &cbs);
    P.done = xSemaphoreCreateCounting(PRESENT_ASYNC, 0);
    P.handover = xSemaphoreCreateBinary();
    xSemaphoreGive(P.handover);
    P.jobs = xQueueCreate(1, sizeof(present_job_t));
    /* above the NetworkTables client: a frame's hand-over is a few microseconds of work at the right time */
    xTaskCreatePinnedToCore(present_task, "present", 3072, NULL, 7, NULL, 0);
}

#else
/* The default. Each area (and whatever of the last frame's the back buffer is missing) is turned into
 * the back buffer with blocking PPA calls — so the source buffer is free again when this returns — and
 * the buffer is handed to the DPI controller. The switch to it happens at the panel's next vsync, and
 * this doesn't wait for it: the UI draws its next frame meanwhile, and only the next present waits (for
 * the other buffer to leave the glass) before writing into it. A frame no longer rounds up to the next
 * vsync, so work up to 16.5 ms per frame holds 60 Hz. */
static bool s_flip_pending; /* a buffer was handed over and the panel hasn't switched to it yet */

void hal_present(const bz_present_t *areas, int n, void *user)
{
    (void)user;
    if (s_flip_pending) {
        if (xSemaphoreTake(T.vsync, pdMS_TO_TICKS(100)) != pdTRUE && !P.late++) ESP_LOGW(TAG, "present: no vsync");
        s_flip_pending = false;
    }
    void *fb = T.fb[T.back];
    int y0 = PANEL_H, y1 = -1;
    /* areas that tile the whole screen leave nothing of the last frame to catch up (a slide, a full
     * redraw): skip re-turning it, which was half of every slide frame's work */
    uint32_t cover = 0;
    for (int i = 0; i < n; i++)
        cover += (uint32_t)(areas[i].a.x2 - areas[i].a.x1 + 1) * (uint32_t)(areas[i].a.y2 - areas[i].a.y1 + 1);
    if (cover >= (uint32_t)HAL_W * HAL_H) P.nprev = 0;
    for (int i = 0; i < P.nprev; i++) {
        bool covered = false;
        for (int j = 0; j < n && !covered; j++) covered = covers(&areas[j].a, &P.prev[i].a);
        if (!covered) {
            rotate_area(&P.prev[i], fb, false);
            rows_of(&P.prev[i].a, &y0, &y1);
        }
    }
    for (int i = 0; i < n; i++) {
        rotate_area(&areas[i], fb, false);
        rows_of(&areas[i].a, &y0, &y1);
    }
    int keep = n < PRESENT_MAX ? n : PRESENT_MAX;
    if (keep < n && !P.overflow++) ESP_LOGW(TAG, "present: %d areas, catch-up keeps %d", n, PRESENT_MAX);
    memcpy(P.prev, areas, sizeof *areas * (size_t)keep);
    P.nprev = keep;
    if (y1 < y0) { y0 = 0; y1 = PANEL_H - 1; }
    xSemaphoreTake(T.vsync, 0);
    /* only the rows that changed: draw_bitmap writes back the cache over exactly these, not 1.8 MB */
    esp_lcd_panel_draw_bitmap(T.lcd.panel, 0, y0, PANEL_W, y1 + 1, fb);
    s_flip_pending = true;
    T.back ^= 1;
    if (!T.lit) {
        T.lit = true;
        bsp_display_brightness_set(70);
    }
}

static void present_init(void) {}

/* ---- a page slide in the panel's own orientation ----
 *
 * A horizontal slide in the landscape picture is, in the portrait frame buffer, a shift of whole rows (a
 * landscape column is a portrait row), and a block of rows is contiguous memory. So a slide never
 * rotates anything per frame: the page as it is on the glass is copied once (it is already portrait),
 * the page beside it is turned in band by band while the finger moves (patch), and each frame is a
 * straight copy of two row ranges plus the fixed chrome — no 90° turn, which the PPA does at ~15 Mpx/s
 * (a full landscape frame: ~60 ms). */
static struct {
    uint16_t *snap, *nb, *chrome; /* portrait: the page, the page beside it, the glass as it was */
    uint16_t ground;              /* RGB565 of the gap a slide opens where no neighbour is drawn */
    bool active;
    ppa_client_handle_t fill;
} SLD;

static void present_wait(void)
{
    if (s_flip_pending) {
        if (xSemaphoreTake(T.vsync, pdMS_TO_TICKS(100)) != pdTRUE && !P.late++) ESP_LOGW(TAG, "present: no vsync");
        s_flip_pending = false;
    }
}

static void present_flip(void *fb, int y0, int y1)
{
    xSemaphoreTake(T.vsync, 0);
    esp_lcd_panel_draw_bitmap(T.lcd.panel, 0, y0, PANEL_W, y1 + 1, fb);
    s_flip_pending = true;
    T.back ^= 1;
}

/* Block copies between portrait buffers on the DMA2D, the P4's 2D copy engine (esp_lcd's frame-buffer
 * copier, the same the DPI driver uses). Not the PPA: its scale-rotate-mirror engine moves ~20 Mpx/s
 * whatever the angle (a full frame, 45 ms); nor the CPU: a memcpy through the cache from PSRAM, ~36 MB/s.
 * Nothing here is ever written by the CPU, so no dirty cache line can land on what the DMA wrote. */
static esp_async_fbcpy_handle_t s_fbcpy;
static SemaphoreHandle_t s_fbcpy_done;

static bool fbcpy_done(esp_async_fbcpy_handle_t h, esp_async_fbcpy_event_data_t *e, void *arg)
{
    (void)h; (void)e; (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_fbcpy_done, &woken);
    return woken == pdTRUE;
}

static bool s_fbcpy_pending;

static void fbcpy_wait(void)
{
    if (!s_fbcpy_pending) return;
    xSemaphoreTake(s_fbcpy_done, pdMS_TO_TICKS(100));
    s_fbcpy_pending = false;
}

/* a block copy started and left running (fbcpy_wait before touching dst, or before the next copy) */
static void blk_copy_async(uint16_t *dst, int dx, int dy, const uint16_t *src, int sx, int sy, int w, int h)
{
    fbcpy_wait();
    if (w <= 0 || h <= 0) return;
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = src, .dst_buffer = dst,
        .src_buffer_size_x = PANEL_W, .src_buffer_size_y = PANEL_H,
        .dst_buffer_size_x = PANEL_W, .dst_buffer_size_y = PANEL_H,
        .src_offset_x = (size_t)sx, .src_offset_y = (size_t)sy,
        .dst_offset_x = (size_t)dx, .dst_offset_y = (size_t)dy,
        .copy_size_x = (size_t)w, .copy_size_y = (size_t)h,
        .pixel_format_unique_id = { .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565) },
    };
    if (esp_async_fbcpy(s_fbcpy, &t, fbcpy_done, NULL) == ESP_OK) s_fbcpy_pending = true;
}

static void blk_copy(uint16_t *dst, int dx, int dy, const uint16_t *src, int sx, int sy, int w, int h)
{
    blk_copy_async(dst, dx, dy, src, sx, sy, w, h);
    fbcpy_wait();
}

static void rows_copy(uint16_t *dst, int dy, const uint16_t *src, int sy, int n)
{
    blk_copy(dst, 0, dy, src, 0, sy, PANEL_W, n);
}

static void rect_copy(uint16_t *dst, const uint16_t *src, int x, int y, int w, int h)
{
    blk_copy(dst, x, y, src, x, y, w, h);
}

/* the gap a slide opens where no neighbour is drawn: copied from a buffer of ground, also by DMA */
static uint16_t *s_ground;
static uint16_t s_ground_c;

static void rows_fill(uint16_t *dst, int dy, int n, uint16_t c)
{
    if (n <= 0 || !s_ground) return;
    if (c != s_ground_c) {
        for (size_t i = 0; i < (size_t)PANEL_W * PANEL_H; i++) s_ground[i] = c;
        esp_cache_msync(s_ground, (size_t)PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_ground_c = c;
    }
    rows_copy(dst, dy, s_ground, dy, n);
}

/* where a landscape rectangle lands in the portrait buffer */
static void portrait_rect(const bz_area_t *a, int *x, int *y, int *w, int *h)
{
    if (!s_flip) {
        *x = a->y1;
        *y = HAL_W - 1 - a->x2;
    } else {
        *x = HAL_H - 1 - a->y2;
        *y = a->x1;
    }
    *w = a->y2 - a->y1 + 1;
    *h = a->x2 - a->x1 + 1;
}

static bool slide_begin(uint32_t ground_rgb, const bz_area_t *chrome, int nchrome)
{
    if (!SLD.snap || !SLD.nb || !SLD.chrome || !s_fbcpy || !s_ground) return false;
    SLD.ground = (uint16_t)(((ground_rgb >> 19) & 31) << 11 | ((ground_rgb >> 10) & 63) << 5 | ((ground_rgb >> 3) & 31));
    /* the latest picture handed to the panel — final in memory even if the panel switches to it only at
     * the next vsync, so no waiting for it: the page with its chrome */
    const uint16_t *front = T.fb[T.back ^ 1];
    fbcpy_wait();
    /* the chrome's own pixels (small), then the page (big) left copying while the renderer draws the
     * chrome-free patches; the first patch waits for it */
    for (int i = 0; i < nchrome; i++) {
        int x, y, w, h;
        portrait_rect(&chrome[i], &x, &y, &w, &h);
        rect_copy(SLD.chrome, front, x, y, w, h);
    }
    blk_copy_async(SLD.snap, 0, 0, front, 0, 0, PANEL_W, PANEL_H);
    SLD.active = true;
    return true;
}

/* a landscape area turned into the snapshot (the page without its chrome) or the neighbour */
static void slide_patch(const bz_present_t *p, bool neighbour)
{
    if (!SLD.active) return;
    fbcpy_wait(); /* the capture may still be copying into the snapshot */
    rotate_area(p, neighbour ? SLD.nb : SLD.snap, false);
}

static void slide_frame(int dx, int side, const bz_area_t *chrome, int nchrome)
{
    if (!SLD.active) return;
    present_wait();
    double tf0 = hal_seconds();
    uint16_t *fb = T.fb[T.back];
    int W = HAL_W; /* portrait rows */
    if (dx > W) dx = W;
    if (dx < -W) dx = -W;
    /* back row r shows landscape x; the page's pixel came from x - dx, which is row r + s·dx */
    int s = s_flip ? -1 : 1, sd = s * dx;
    int r0 = sd < 0 ? -sd : 0, r1 = sd > 0 ? W - sd : W;
    rows_copy(fb, r0, SLD.snap, r0 + sd, r1 - r0);
    /* the gap: the neighbour where it's drawn and on that side, else ground. dx > 0 opens the left
     * (the previous page), dx < 0 the right (the next) */
    int need = dx > 0 ? -1 : 1;
    if (sd > 0) {
        if (side == need) rows_copy(fb, W - sd, SLD.nb, 0, sd);
        else rows_fill(fb, W - sd, sd, SLD.ground);
    } else if (sd < 0) {
        if (side == need) rows_copy(fb, 0, SLD.nb, W + sd, -sd);
        else rows_fill(fb, 0, -sd, SLD.ground);
    }
    /* the chrome stays where it is: straight from the glass as it was */
    for (int i = 0; i < nchrome; i++) {
        int x, y, w, h;
        portrait_rect(&chrome[i], &x, &y, &w, &h);
        rect_copy(fb, SLD.chrome, x, y, w, h);
    }
    P.nprev = 0; /* the whole buffer was written: nothing of the last frame to catch up */
    (void)tf0;
    present_flip(fb, 0, PANEL_H - 1);
}

/* At start-up, while internal RAM has room: the DMA2D's descriptors must be in internal, DMA-capable
 * memory, and by the first swipe there is none left (the install failed, the slide never started). */
static void slide_init(void)
{
    esp_async_fbcpy_config_t cfg = {};
    if (esp_async_fbcpy_install(&cfg, &s_fbcpy) != ESP_OK) {
        s_fbcpy = NULL;
        ESP_LOGW(TAG, "no DMA2D copier: page slides fall back to the landscape path");
    }
    s_fbcpy_done = xSemaphoreCreateBinary();
    /* the slide's portrait pictures, up front and not zeroed (nothing reads them before they're written;
     * never written by the CPU, so no dirty cache line can land on the DMA's data) */
    size_t n = (size_t)PANEL_W * PANEL_H * 2;
    SLD.snap = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
    SLD.nb = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
    SLD.chrome = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
    s_ground = heap_caps_aligned_alloc(128, n, MALLOC_CAP_SPIRAM);
    s_ground_c = 1; /* differs from any real ground: filled on first use */
    if (!SLD.snap || !SLD.nb || !SLD.chrome || !s_ground) ESP_LOGW(TAG, "no memory for the page slide's pictures");
}

static void slide_end(void)
{
    SLD.active = false;
    P.nprev = 0;
}

/* A sheet pulled down over the page (the control center). A landscape row is a portrait column, so the
 * sheet's visible rows and the page's are each one rectangle the panel's full height: two DMA2D copies a
 * frame, nothing turned. The sheet's picture (drawn at rest, full screen) shows by its bottom h rows. */
static void sheet_frame(int h, int sh, bool swapped)
{
    if (!SLD.active) return;
    const int H = HAL_H; /* landscape rows = portrait columns = PANEL_W */
    if (sh > H) sh = H;
    if (h < 8) h = 0;    /* a sliver would be a DMA block a few pixels wide: none at all */
    if (h > sh - 8) h = sh;
    /* the sheet's picture; the page's rows it has uncovered, [h, sh); below the sheet the glass as it was */
    const uint16_t *sheet = swapped ? SLD.snap : SLD.nb, *under = swapped ? SLD.nb : SLD.snap;
    present_wait();
    uint16_t *fb = T.fb[T.back];
    fbcpy_wait();
    if (!s_flip) {
        /* portrait column c shows landscape row c: in the sheet, row c + (sh - h) */
        if (h) blk_copy_async(fb, 0, 0, sheet, sh - h, 0, h, PANEL_H);
        if (sh - h) blk_copy_async(fb, h, 0, under, h, 0, sh - h, PANEL_H);
        if (H - sh) blk_copy_async(fb, sh, 0, SLD.snap, sh, 0, H - sh, PANEL_H);
    } else {
        /* column c shows landscape row H - 1 - c */
        if (h) blk_copy_async(fb, H - h, 0, sheet, H - sh, 0, h, PANEL_H);
        if (sh - h) blk_copy_async(fb, H - sh, 0, under, H - sh, 0, sh - h, PANEL_H);
        if (H - sh) blk_copy_async(fb, 0, 0, SLD.snap, 0, 0, H - sh, PANEL_H);
    }
    fbcpy_wait();
    P.nprev = 0;
    present_flip(fb, 0, PANEL_H - 1);
}

static void slide_settle(void)
{
    /* the glass's buffer copied into the other one: the next present, a few areas, lands on the same picture */
    present_wait();
    blk_copy(T.fb[T.back], 0, 0, T.fb[T.back ^ 1], 0, 0, PANEL_W, PANEL_H);
    P.nprev = 0;
}

static const bz_slide_ops_t SLIDE_OPS = { .begin = slide_begin, .patch = slide_patch, .frame = slide_frame, .end = slide_end,
                                          .sheet = sheet_frame, .settle = slide_settle };
#endif

static void display_init(void)
{
    bsp_display_config_t cfg = { .dsi_bus = { .phy_clk_src = 0, .lane_bit_rate_mbps = lane_rate() } };
    ESP_ERROR_CHECK(bsp_display_new_with_handles(&cfg, &T.lcd));
    esp_lcd_panel_disp_on_off(T.lcd.panel, true);
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(T.lcd.panel, 2, &T.fb[0], &T.fb[1]));
    T.vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(T.lcd.panel, &cbs, NULL);
    T.back = 1;
    present_init();
#ifndef CATALYST_ASYNC_PRESENT
    slide_init();
#endif
    T.display_t0 = hal_seconds();
    /* the backlight stays off until hal_present() has put a real frame up: never the panel's power-on
     * noise, and one less load switching on with everything else */
    bsp_touch_new(NULL, &T.touch);
    ESP_LOGI(TAG, "panel %s at the BSP's timing, lanes %u Mbps, frame buffers %p %p", T.panel, (unsigned)T.lane_mbps,
             T.fb[0], T.fb[1]);
}

const char *hal_panel_name(void) { return T.panel; }

void hal_set_brightness(float v)
{
    if (!T.lit) return; /* the first frame lights it */
    bsp_display_brightness_set((int)(v * 100 + 0.5f));
}

bool hal_touch(int *x, int *y, void *user)
{
    (void)user;
    if (hal_dev_touch(x, y)) return true;
    if (!T.touch) return false;
    esp_lcd_touch_point_data_t p[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(T.touch);
    if (esp_lcd_touch_get_data(T.touch, p, &cnt, 1) != ESP_OK || !cnt) return false;
    /* the inverse of the picture's turn in rotate_area() */
    if (!s_flip) {
        *x = HAL_W - 1 - p[0].y;
        *y = p[0].x;
    } else {
        *x = p[0].y;
        *y = HAL_H - 1 - p[0].x;
    }
    return true;
}

/* The buffer on the glass now (the one handed over last), portrait 720x1280: what the panel shows. */
const uint16_t *hal_front_fb(void) { return T.fb[T.back ^ 1]; }

void hal_set_flip(bool flip)
{
    if (flip == s_flip) return;
    s_flip = flip;
    P.nprev = 0; /* last frame's areas are in the old orientation: the caller redraws the whole screen */
}

bool hal_flip(void) { return s_flip; }

/* The portrait rows a landscape area lands on: all the panel needs written back and shown. */
static void rows_of(const bz_area_t *a, int *y0, int *y1)
{
    int r0 = s_flip ? a->x1 : HAL_W - 1 - a->x2, r1 = s_flip ? a->x2 : HAL_W - 1 - a->x1;
    if (r0 < *y0) *y0 = r0;
    if (r1 > *y1) *y1 = r1;
}

/* ---- the compositor's accelerated primitives ----
 *
 * copy565 and blend take any buffers and strides. What the PPA needs from a destination is care: before a
 * transaction the driver invalidates the output's cache lines over whole rows (from the block's first row
 * to its last, rounded out to 128 bytes), which would discard anything the CPU had written nearby and not
 * yet written back — or, past the end of the buffer, someone else's data. So a destination is only given to
 * the PPA within the heap block that holds it (found once with heap_caps_walk(), then remembered; these are
 * long-lived frame buffers), its window is written back first, and whatever the window can't cover (the
 * last rows of a block at the buffer's end, the columns past a row's end) is done on the CPU. */
#define PPA_MIN_PX 4096 /* smaller: the CPU is quicker than setting up a transaction */
#define LINE 128        /* the P4's L2 cache line, the PPA's output alignment */

typedef struct { uintptr_t lo, hi; } span_t;
static span_t s_spans[8];
static int s_nspans, s_next_span;

static void span_add(const void *p, size_t n)
{
    int i = s_nspans < 8 ? s_nspans++ : (s_next_span++ % 8);
    s_spans[i] = (span_t){ (uintptr_t)p, (uintptr_t)p + n };
}

static bool span_walk(walker_heap_into_t heap, walker_block_info_t b, void *user)
{
    (void)heap;
    uintptr_t *q = user, p = (uintptr_t)b.ptr;
    if (b.used && q[0] >= p && q[0] < p + b.size) {
        q[1] = p;
        q[2] = p + b.size;
        return false;
    }
    return true;
}

static const span_t *span_of(const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    for (int i = 0; i < s_nspans; i++)
        if (p >= s_spans[i].lo && p < s_spans[i].hi) return &s_spans[i];
    if (!esp_ptr_external_ram(ptr)) return NULL;
    uintptr_t q[3] = { p, 0, 0 };
    heap_caps_walk(MALLOC_CAP_SPIRAM, span_walk, q);
    if (!q[1]) return NULL;
    span_add((const void *)q[1], q[2] - q[1]);
    return span_of(ptr);
}

/* How much of a w×h block at dst the PPA may write: out window [buf, buf + size) inside dst's buffer,
 * rows 0..h-1 and columns 0..w-1 of the block (the rest is the CPU's). false: all CPU. */
typedef struct {
    void *buf;
    uint32_t size, pic_w, off_x;
    int w, h;
} out_plan_t;

static bool plan_out(void *dst, int stride, int bpp, int w, int h, out_plan_t *o)
{
    const span_t *s = span_of(dst);
    if (!s) return false;
    uintptr_t d = (uintptr_t)dst, a = d & ~(uintptr_t)(LINE - 1), top = s->hi & ~(uintptr_t)(LINE - 1);
    size_t row = (size_t)stride * (size_t)bpp;
    if (a < s->lo || top <= a || (d - a) % (uintptr_t)bpp) return false;
    o->off_x = (uint32_t)((d - a) / (uintptr_t)bpp);
    o->w = (int)o->off_x + w <= stride ? w : stride - (int)o->off_x;
    size_t rows = (top - a) / row;
    o->h = (size_t)h < rows ? h : (int)rows;
    if (o->w <= 0 || o->h <= 0 || o->w * o->h < PPA_MIN_PX) return false;
    o->buf = (void *)a;
    o->pic_w = (uint32_t)stride;
    o->size = (uint32_t)(((size_t)o->h * row + LINE - 1) & ~(size_t)(LINE - 1));
    /* the CPU's writes near the block reach memory before the driver invalidates these lines */
    esp_cache_msync(o->buf, o->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return true;
}

static void cpu_copy565(uint16_t *dst, int ds, const uint16_t *src, int ss, int w, int h)
{
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * ds, src + (size_t)y * ss, (size_t)w * 2);
}

static void ppa_copy565(uint16_t *dst, int ds, const uint16_t *src, int ss, int w, int h)
{
    out_plan_t o;
    if (w * h < PPA_MIN_PX || !plan_out(dst, ds, 2, w, h, &o)) {
        cpu_copy565(dst, ds, src, ss, w, h);
        return;
    }
    in_rect_t in = in_rect(src, ss, 2, o.w, o.h);
    ppa_srm_oper_config_t op = {
        .in = { .buffer = in.buf, .pic_w = in.pic_w, .pic_h = in.pic_h, .block_w = (uint32_t)o.w,
                .block_h = (uint32_t)o.h, .block_offset_x = in.off_x, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = o.buf, .buffer_size = o.size, .pic_w = o.pic_w, .pic_h = (uint32_t)o.h,
                 .block_offset_x = o.off_x, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(T.ppa_srm, &op) != ESP_OK) {
        cpu_copy565(dst, ds, src, ss, w, h);
        return;
    }
    /* what the window couldn't cover, after the transaction has finished with those lines */
    if (o.w < w) cpu_copy565(dst + o.w, ds, src + o.w, ss, w - o.w, o.h);
    if (o.h < h) cpu_copy565(dst + (size_t)o.h * ds, ds, src + (size_t)o.h * ss, ss, w, h - o.h);
}

static void cpu_blend(uint16_t *d, int ds, const uint16_t *u, int us, const uint32_t *o, int os, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = o[(size_t)y * os + x], a = p >> 24;
            uint16_t q = u[(size_t)y * us + x];
            if (a == 0) { d[(size_t)y * ds + x] = q; continue; }
            int r = (q >> 11) << 3, g = ((q >> 5) & 63) << 2, b = (q & 31) << 3;
            r += (int)((((p >> 16) & 255) - r) * a) / 255;
            g += (int)((((p >> 8) & 255) - g) * a) / 255;
            b += (int)(((p & 255) - b) * a) / 255;
            d[(size_t)y * ds + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
}

static void ppa_blend(uint16_t *d, int ds, const uint16_t *u, int us, const uint32_t *o, int os, int w, int h)
{
    out_plan_t p;
    if (!T.ppa_blend_ok || w * h < PPA_MIN_PX || !plan_out(d, ds, 2, w, h, &p)) {
        cpu_blend(d, ds, u, us, o, os, w, h);
        return;
    }
    in_rect_t bg = in_rect(u, us, 2, p.w, p.h), fg = in_rect(o, os, 4, p.w, p.h);
    ppa_blend_oper_config_t op = {
        .in_bg = { .buffer = bg.buf, .pic_w = bg.pic_w, .pic_h = bg.pic_h, .block_w = (uint32_t)p.w,
                   .block_h = (uint32_t)p.h, .block_offset_x = bg.off_x, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = fg.buf, .pic_w = fg.pic_w, .pic_h = fg.pic_h, .block_w = (uint32_t)p.w,
                   .block_h = (uint32_t)p.h, .block_offset_x = fg.off_x, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
        .out = { .buffer = p.buf, .buffer_size = p.size, .pic_w = p.pic_w, .pic_h = (uint32_t)p.h,
                 .block_offset_x = p.off_x, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_blend(T.ppa_blend, &op) != ESP_OK) {
        cpu_blend(d, ds, u, us, o, os, w, h);
        return;
    }
    if (p.w < w) cpu_blend(d + p.w, ds, u + p.w, us, o + p.w, os, w - p.w, p.h);
    if (p.h < h)
        cpu_blend(d + (size_t)p.h * ds, ds, u + (size_t)p.h * us, us, o + (size_t)p.h * os, os, w, h - p.h);
}

/* In-place blending isn't documented either way for the PPA; check it once against the CPU on a
 * corner of the real buffers and fall back to the CPU if they disagree. */
static void ppa_blend_selftest(void)
{
    const int w = 64, h = 64;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            T.out[y * HAL_W + x] = (uint16_t)(x * 997 + y * 131);
            T.ink[y * HAL_W + x] = (uint32_t)((x * 4) << 24 | (y * 4) << 16 | 0x8040);
        }
    static uint16_t expect[64 * 64];
    cpu_blend(expect, 64, T.out, HAL_W, T.ink, HAL_W, w, h);
    T.ppa_blend_ok = true;
    ppa_blend(T.out, HAL_W, T.out, HAL_W, T.ink, HAL_W, w, h);
    int bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t a = T.out[y * HAL_W + x], b = expect[y * 64 + x];
            int dr = abs((a >> 11) - (b >> 11)), dg = abs(((a >> 5) & 63) - ((b >> 5) & 63)), db = abs((a & 31) - (b & 31));
            if (dr > 1 || dg > 2 || db > 1) bad++;
        }
    T.ppa_blend_ok = bad < 8;
    memset(T.out, 0, HAL_W * HAL_H * 2);
    memset(T.ink, 0, HAL_W * HAL_H * 4);
    ESP_LOGI(TAG, "PPA in-place blend %s (%d of 4096 pixels off)", T.ppa_blend_ok ? "used" : "not trusted: CPU blend", bad);
}

/* The compositor's rows on both cores: part 1 on a worker pinned to core 0, part 0 on the caller (the UI
 * task, core 1). Glass, fills and blinds are CPU work per row, so this is close to twice as fast. */
static struct {
    SemaphoreHandle_t go, done;
    void (*job)(void *arg, int part);
    void *arg;
} W;

static void par_worker(void *unused)
{
    (void)unused;
    for (;;) {
        xSemaphoreTake(W.go, portMAX_DELAY);
        W.job(W.arg, 1);
        xSemaphoreGive(W.done);
    }
}

static void par_run(void (*job)(void *arg, int part), void *arg)
{
    if (!W.go) {
        job(arg, 0);
        job(arg, 1);
        return;
    }
    W.job = job;
    W.arg = arg;
    xSemaphoreGive(W.go);
    job(arg, 0);
    xSemaphoreTake(W.done, portMAX_DELAY);
}

/* Core 1 is the renderer's. esp-hosted and lwIP create their tasks unpinned at priorities 18–23, and
 * the scheduler put them on core 1 as often as not: the boot card's frames went from 10 ms to 100 while
 * the C6 came up. Every unpinned task above 9 goes to core 0 (see CMakeLists.txt for the wrap). */
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack, void *const arg,
                                          UBaseType_t prio, TaskHandle_t *const handle, const BaseType_t core);
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t stack, void *const arg,
                                          UBaseType_t prio, TaskHandle_t *const handle, const BaseType_t core)
{
    return __real_xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle,
                                          core == tskNO_AFFINITY && prio > 9 ? 0 : core);
}

static void par_init(void)
{
    W.go = xSemaphoreCreateBinary();
    W.done = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(par_worker, "comp1", 4096, NULL, 7, NULL, 0) != pdPASS) {
        vSemaphoreDelete(W.go);
        vSemaphoreDelete(W.done);
        W.go = W.done = NULL;
        ESP_LOGW(TAG, "compositor: no second-core worker, one core only");
    }
}

static const bz_gfx_ops_t OPS = { .copy565 = ppa_copy565, .blend = ppa_blend, .parallel = par_run };

void hal_display(hal_display_t *o)
{
    o->content = T.content;
    o->ink = T.ink;
    o->out = T.out;
    o->ops = &OPS;
#ifdef CATALYST_ASYNC_PRESENT
    o->async_present = true;
#else
    o->async_present = false;
    o->slide = s_fbcpy ? &SLIDE_OPS : NULL; /* without the DMA2D copier the renderer slides by itself */
#endif
}

/* ------------------------------------------------------------------ audio */

/* The ES8388 and the speaker amp only. The ES7210 and its microphones are never configured: nothing in
 * Catalyst Tab listens. (The BSP's shared I2S bus still enables its receive channel; with no codec behind
 * it that costs a few DMA interrupts and captures nothing.) */
static struct {
    esp_codec_dev_handle_t spk;
    QueueHandle_t tones;
} A;

typedef struct { float hz; int ms; float vol; } tone_t;

static void tone_task(void *arg)
{
    static int16_t buf[48 * 2 * 20]; /* 20 ms stereo at 48 kHz */
    tone_t t;
    for (;;) {
        if (xQueueReceive(A.tones, &t, portMAX_DELAY) != pdTRUE || !A.spk) continue;
        int total = 48 * t.ms, done = 0;
        double ph = 0, step = 2 * M_PI * t.hz / 48000.0;
        while (done < total) {
            int n = total - done > 960 ? 960 : total - done;
            for (int i = 0; i < n; i++) {
                /* a short raised-cosine envelope: a tick, not a click */
                int k = done + i, edge = 48 * 3;
                float env = k < edge ? 0.5f - 0.5f * cosf((float)M_PI * k / edge)
                          : k > total - edge ? 0.5f - 0.5f * cosf((float)M_PI * (total - k) / edge) : 1;
                int16_t s = (int16_t)(sin(ph) * 12000 * env * t.vol);
                ph += step;
                buf[2 * i] = buf[2 * i + 1] = s;
            }
            esp_codec_dev_write(A.spk, buf, n * 4);
            done += n;
        }
    }
}

static void audio_init(void)
{
    A.spk = bsp_audio_codec_speaker_init();
    esp_codec_dev_sample_info_t fs = { .sample_rate = 48000, .channel = 2, .bits_per_sample = 16 };
    if (A.spk) {
        esp_codec_dev_open(A.spk, &fs);
        esp_codec_dev_set_out_vol(A.spk, 70);
    }
    A.tones = xQueueCreate(8, sizeof(tone_t));
    xTaskCreatePinnedToCore(tone_task, "tone", 4096, NULL, 4, NULL, 0);
}

void hal_tone(float hz, int ms, float v)
{
    if (!A.tones || v <= 0.01f) return;
    tone_t t = { hz, ms, v };
    xQueueSend(A.tones, &t, 0);
}

void hal_set_volume(float v)
{
    T.volume = v;
    if (A.spk) esp_codec_dev_set_out_vol(A.spk, (int)(v * 100));
}

/* ------------------------------------------------------------------ camera */

#define CAM_OUT_W 960
#define CAM_OUT_H 540
static struct {
    int fd;
    bool on, started;
    void *bufs[2];
    size_t lens[2];
    int src_w, src_h;
    uint16_t *frames[2];
    volatile int ready;         /* index of the newest complete frame, -1 none */
    TaskHandle_t task;
    jpeg_encoder_handle_t jpeg;
} C = { .fd = -1, .ready = -1 };

static void clip_take(const void *src);
static bool clip_taking_frames(void);
static volatile bool s_clip_taking;

static void cam_task(void *arg)
{
    int w = 0;
    for (;;) {
        if (!C.on) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(C.fd, VIDIOC_DQBUF, &b) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* a clip gets the full-resolution frame first; s_clip_taking lets the encoder worker wait out a
         * take in flight when the clip ends */
        if (clip_taking_frames()) {
            s_clip_taking = true;
            if (clip_taking_frames()) clip_take(C.bufs[b.index]);
            s_clip_taking = false;
        }
        /* scale 1280×720 to 960×540 on the PPA straight into the frame the UI will show */
        ppa_srm_oper_config_t op = {
            .in = { .buffer = C.bufs[b.index], .pic_w = (uint32_t)C.src_w, .pic_h = (uint32_t)C.src_h,
                    .block_w = (uint32_t)C.src_w, .block_h = (uint32_t)C.src_h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = C.frames[w], .buffer_size = CAM_OUT_W * CAM_OUT_H * 2, .pic_w = CAM_OUT_W,
                     .pic_h = CAM_OUT_H, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)CAM_OUT_W / C.src_w,
            .scale_y = (float)CAM_OUT_H / C.src_h,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        ppa_do_scale_rotate_mirror(T.ppa_cam, &op);
        C.ready = w;
        w ^= 1;
        ioctl(C.fd, VIDIOC_QBUF, &b);
    }
}

bool hal_camera_start(void)
{
    if (C.on) return true;
    if (!C.started) {
        bsp_camera_cfg_t cfg = { 0 };
        if (bsp_camera_start(&cfg) != ESP_OK) return false;
        C.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
        if (C.fd < 0) return false;
        struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        ioctl(C.fd, VIDIOC_G_FMT, &f);
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(C.fd, VIDIOC_S_FMT, &f) != 0) return false;
        C.src_w = (int)f.fmt.pix.width;
        C.src_h = (int)f.fmt.pix.height;
        struct v4l2_requestbuffers req = { .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(C.fd, VIDIOC_REQBUFS, &req) != 0) return false;
        for (int i = 0; i < 2; i++) {
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)i };
            ioctl(C.fd, VIDIOC_QUERYBUF, &b);
            C.bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, C.fd, b.m.offset);
            C.lens[i] = b.length;
            ioctl(C.fd, VIDIOC_QBUF, &b);
        }
        C.frames[0] = psram_aligned(CAM_OUT_W * CAM_OUT_H * 2);
        C.frames[1] = psram_aligned(CAM_OUT_W * CAM_OUT_H * 2);
        xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 5, &C.task, 0);
        C.started = true;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(C.fd, VIDIOC_STREAMON, &type) != 0) return false;
    C.on = true;
    return true;
}

void hal_camera_stop(void)
{
    if (!C.on) return;
    hal_clip_stop();
    C.on = false;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(C.fd, VIDIOC_STREAMOFF, &type);
    C.ready = -1;
}

const uint16_t *hal_camera_frame(int *w, int *h)
{
    if (!C.on || C.ready < 0) return NULL;
    *w = CAM_OUT_W;
    *h = CAM_OUT_H;
    return C.frames[C.ready];
}

bool hal_camera_snapshot(const char *path)
{
    int w, h;
    const uint16_t *f = hal_camera_frame(&w, &h);
    if (!f) return false;
    if (!C.jpeg) {
        jpeg_encode_engine_cfg_t ec = { .timeout_ms = 100 };
        if (jpeg_new_encoder_engine(&ec, &C.jpeg) != ESP_OK) return false;
    }
    size_t in_cap = 0, out_cap = 0;
    jpeg_encode_memory_alloc_cfg_t in_m = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_m = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    uint8_t *in = jpeg_alloc_encoder_mem((size_t)w * h * 2, &in_m, &in_cap);
    uint8_t *out = jpeg_alloc_encoder_mem((size_t)w * h, &out_m, &out_cap);
    bool ok = false;
    if (in && out) {
        memcpy(in, f, (size_t)w * h * 2);
        jpeg_encode_cfg_t cfg = { .width = (uint32_t)w, .height = (uint32_t)h, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                                  .sub_sample = JPEG_DOWN_SAMPLING_YUV422, .image_quality = 88 };
        uint32_t size = 0;
        if (jpeg_encoder_process(C.jpeg, &cfg, in, (uint32_t)w * h * 2, out, (uint32_t)out_cap, &size) == ESP_OK) {
            FILE *fp = fopen(path, "wb");
            if (fp) {
                ok = fwrite(out, 1, size, fp) == size;
                fclose(fp);
            }
        }
    }
    free(in);
    free(out);
    return ok;
}

/* ---- clips: the camera's frames through the P4's hardware H.264 encoder ----
 *
 * The camera task converts each full-resolution RGB565 frame on the PPA into one of two YUV420 buffers
 * (the encoder's only input format, "O_UYY_E_VYY": the P4's packed 4:2:0, taken to be what the PPA's
 * YUV420 output writes — UNVERIFIED on the unit; wrong would show as scrambled colour, not a crash) and
 * hands it to a core-0 worker, which runs it through esp_h264's hardware encoder and appends the NAL units
 * to the file. The Lens preview keeps its own path; when the worker falls behind, frames are dropped
 * rather than queued. esp_video's V4L2 H.264 device wraps the same encoder; calling esp_h264 directly
 * needs no second V4L2 pipeline. */
#define CLIP_FPS 30          /* the SC202CS mode: 1280×720 at 30 fps */
#define CLIP_BITRATE 4000000 /* ~30 MB a minute: detail enough to read a label inside a gearbox */
#define CLIP_SLOTS 2

static struct {
    TaskHandle_t task;
    volatile bool active;            /* taking frames */
    FILE *fp;
    esp_h264_enc_handle_t enc;
    uint8_t *yuv[CLIP_SLOTS], *bits;
    uint32_t yuv_len, bits_len;
    QueueHandle_t full, empty;       /* slot indices: camera → encoder, encoder → camera */
    SemaphoreHandle_t done;
    int w, h;
    double t_start, max_s;
    volatile double t_first, t_last; /* when the first and the newest frame were taken */
    volatile uint32_t taken, frames, dropped;
    double seconds;                  /* the finished clip's length */
    bool write_error;
} V;

static bool clip_taking_frames(void) { return V.active; }

static void clip_take(const void *src)
{
    int slot;
    if (xQueueReceive(V.empty, &slot, 0) != pdTRUE) {
        V.dropped++;
        return;
    }
    ppa_srm_oper_config_t op = {
        .in = { .buffer = src, .pic_w = (uint32_t)C.src_w, .pic_h = (uint32_t)C.src_h, .block_w = (uint32_t)V.w,
                .block_h = (uint32_t)V.h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        /* limited range, BT.709: what players assume for an HD stream that doesn't say otherwise */
        .out = { .buffer = V.yuv[slot], .buffer_size = V.yuv_len, .pic_w = (uint32_t)V.w, .pic_h = (uint32_t)V.h,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_LIMIT,
                 .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT709 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(T.ppa_cam, &op) != ESP_OK) {
        xQueueSend(V.empty, &slot, 0);
        V.dropped++;
        return;
    }
    double t = hal_seconds();
    if (!V.taken++) V.t_first = t;
    V.t_last = t;
    xQueueSend(V.full, &slot, 0);
}

static void clip_free(void)
{
    if (V.enc) {
        esp_h264_enc_close(V.enc);
        esp_h264_enc_del(V.enc);
    }
    for (int i = 0; i < CLIP_SLOTS; i++) free(V.yuv[i]);
    free(V.bits);
    if (V.full) vQueueDelete(V.full);
    if (V.empty) vQueueDelete(V.empty);
    if (V.fp) fclose(V.fp);
    V.enc = NULL;
    V.fp = NULL;
    V.full = V.empty = NULL;
    V.bits = NULL;
    memset(V.yuv, 0, sizeof V.yuv);
}

static void clip_task(void *arg)
{
    (void)arg;
    uint32_t pts = 0;
    for (;;) {
        int slot;
        if (xQueueReceive(V.full, &slot, pdMS_TO_TICKS(50)) == pdTRUE) {
            esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = V.yuv[slot], .len = V.yuv_len }, .pts = pts };
            esp_h264_enc_out_frame_t out = { .raw_data = { .buffer = V.bits, .len = V.bits_len } };
            pts += 1000 / CLIP_FPS;
            /* the encoder emits Annex-B NAL units with SPS and PPS ahead of every IDR (once a second at
             * gop = fps), so the file plays from its start or from any cut. UNVERIFIED on the unit. */
            if (esp_h264_enc_process(V.enc, &in, &out) == ESP_H264_ERR_OK && out.length) {
                if (fwrite(V.bits, 1, out.length, V.fp) != out.length && !V.write_error) {
                    V.write_error = true;
                    V.active = false; /* the card is full or gone: the clip ends here */
                    ESP_LOGE(TAG, "clip: write to microSD failed");
                }
                V.frames++;
            }
            xQueueSend(V.empty, &slot, 0);
        }
        if (V.active && hal_seconds() - V.t_start >= V.max_s) V.active = false;
        if (!V.active) {
            while (s_clip_taking) vTaskDelay(1); /* a take in flight lands in `full` before the drain */
            if (!uxQueueMessagesWaiting(V.full)) break;
        }
    }
    double s = V.taken ? V.t_last - V.t_first + 1.0 / CLIP_FPS : 0;
    V.seconds = s > V.max_s ? V.max_s : s;
    ESP_LOGI(TAG, "clip: %.1f s, %u frames encoded, %u dropped", V.seconds, (unsigned)V.frames, (unsigned)V.dropped);
    clip_free();
    V.task = NULL;
    xSemaphoreGive(V.done);
    vTaskDelete(NULL);
}

bool hal_clip_start(const char *path, double max_s)
{
    if (V.task || !C.on || !path || max_s <= 0) return false;
    if (!V.done && !(V.done = xSemaphoreCreateBinary())) return false;
    xSemaphoreTake(V.done, 0);
    /* the encoder works in 16-pixel macroblocks: 1280×720 already is */
    V.w = C.src_w & ~15;
    V.h = C.src_h & ~15;
    V.yuv_len = (uint32_t)(V.w * V.h * 3 / 2);
    V.taken = V.frames = V.dropped = 0;
    V.write_error = false;
    V.seconds = 0;
    V.max_s = max_s;
    uint32_t got = 0;
    bool ok = true;
    for (int i = 0; i < CLIP_SLOTS; i++) {
        /* 128-byte aligned whole cache lines: the PPA writes these */
        V.yuv[i] = esp_h264_aligned_calloc(128, 1, V.yuv_len, &got, ESP_H264_MEM_SPIRAM);
        ok = ok && V.yuv[i];
    }
    /* esp_h264 asks for an output buffer as large as the input, so no frame can overflow it */
    V.bits_len = V.yuv_len;
    V.bits = esp_h264_aligned_calloc(128, 1, V.bits_len, &V.bits_len, ESP_H264_MEM_SPIRAM);
    V.full = xQueueCreate(CLIP_SLOTS, sizeof(int));
    V.empty = xQueueCreate(CLIP_SLOTS, sizeof(int));
    ok = ok && V.bits && V.full && V.empty;
    if (ok) {
        esp_h264_enc_cfg_hw_t cfg = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop = CLIP_FPS, /* an IDR a second */
            .fps = CLIP_FPS,
            .res = { .width = (uint16_t)V.w, .height = (uint16_t)V.h },
            .rc = { .bitrate = CLIP_BITRATE, .qp_min = 20, .qp_max = 40 },
        };
        ok = esp_h264_enc_hw_new(&cfg, &V.enc) == ESP_H264_ERR_OK && esp_h264_enc_open(V.enc) == ESP_H264_ERR_OK;
    }
    if (ok) ok = (V.fp = fopen(path, "wb")) != NULL;
    if (!ok) {
        ESP_LOGE(TAG, "clip: couldn't start (memory, the encoder, or %s)", path);
        clip_free();
        return false;
    }
    for (int i = 0; i < CLIP_SLOTS; i++) xQueueSend(V.empty, &i, 0);
    V.t_start = hal_seconds();
    V.active = true;
    /* internal-RAM stack: it writes the microSD through FATFS */
    if (xTaskCreatePinnedToCore(clip_task, "clip", 6144, NULL, 4, &V.task, 0) != pdPASS) {
        V.active = false;
        V.task = NULL;
        clip_free();
        return false;
    }
    return true;
}

double hal_clip_stop(void)
{
    if (V.task) {
        V.active = false;
        xSemaphoreTake(V.done, pdMS_TO_TICKS(3000));
    }
    /* also a clip that already ended by itself at max_s: its length, reported once */
    double s = V.seconds;
    V.seconds = 0;
    return s;
}

bool hal_clip_active(double *seconds)
{
    if (!V.task || !V.active) return false;
    if (seconds) *seconds = hal_seconds() - V.t_start;
    return true;
}

/* ------------------------------------------------------------------ storage */

const char *hal_sd_root(void) { return T.sd ? BSP_SD_MOUNT_POINT : NULL; }

bool hal_sd_space(uint64_t *total, uint64_t *free_b)
{
    if (!T.sd) return false;
    return esp_vfs_fat_info(BSP_SD_MOUNT_POINT, total, free_b) == ESP_OK;
}

bool hal_kv_get(const char *key, char *buf, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("catalyst", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    bool ok = nvs_get_str(h, key, buf, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

void hal_kv_set(const char *key, const char *value)
{
    /* writing flash turns the cache off, and PSRAM with it: a caller whose stack lives there would fault
     * mid-write (hal_thread() only puts a stack there when internal RAM runs out) */
    int probe = 0;
    if (esp_ptr_external_ram(&probe)) {
        ESP_LOGE(TAG, "hal_kv_set(%s) from a task with a PSRAM stack: not written", key);
        return;
    }
    nvs_handle_t h;
    if (nvs_open("catalyst", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ CAN tap (TWAI, listen-only) */

static struct {
    twai_handle_t h;
    bool on;
    double started;
    hal_can_stats_t st;
} K;

bool hal_can_start(int bitrate)
{
    if (K.on) return true;
    ext5v(true); /* the Grove port's 5 V powers the CAN transceiver unit: on only while tapping */
    vTaskDelay(pdMS_TO_TICKS(20));
    /* listen-only: the controller never acknowledges or sends, so a tap can't disturb the robot */
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT_V2(0, CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 512;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
    if (bitrate == 500000) t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install_v2(&g, &t, &f, &K.h) != ESP_OK) {
        ext5v(false);
        return false;
    }
    if (twai_start_v2(K.h) != ESP_OK) {
        twai_driver_uninstall_v2(K.h);
        ext5v(false);
        return false;
    }
    memset(&K.st, 0, sizeof K.st);
    K.st.state = 1;
    K.started = hal_seconds();
    K.on = true;
    return true;
}

int hal_can_read(hal_can_frame_t *out, int max)
{
    if (!K.on) return 0;
    int n = 0;
    twai_message_t m;
    while (n < max && twai_receive_v2(K.h, &m, 0) == ESP_OK) {
        hal_can_frame_t *f = &out[n++];
        f->id = m.identifier;
        f->ext = m.extd;
        f->rtr = m.rtr;
        f->len = m.data_length_code > 8 ? 8 : m.data_length_code;
        memcpy(f->data, m.data, f->len);
        f->t_us = esp_timer_get_time();
    }
    K.st.rx += (uint32_t)n;
    return n;
}

void hal_can_stats(hal_can_stats_t *o)
{
    if (K.on) {
        twai_status_info_t s;
        if (twai_get_status_info_v2(K.h, &s) == ESP_OK) {
            K.st.errors = s.bus_error_count;
            K.st.missed = s.rx_missed_count + s.rx_overrun_count;
            if (s.state == TWAI_STATE_BUS_OFF || s.bus_error_count > 100) K.st.state = 2;
            else if (!K.st.rx && hal_seconds() - K.started > 3) K.st.state = 3; /* silence: no transceiver, or no bus */
            else K.st.state = 1;
        }
    }
    *o = K.st;
}

void hal_can_stop(void)
{
    if (!K.on) return;
    twai_stop_v2(K.h);
    twai_driver_uninstall_v2(K.h);
    ext5v(false);
    K.on = false;
    K.st.state = 0;
}

/* ------------------------------------------------------------------ system */

void hal_sys(hal_sys_t *o)
{
    memset(o, 0, sizeof *o);
    o->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    o->sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    float c = 0;
    if (T.tsens && temperature_sensor_get_celsius(T.tsens, &c) == ESP_OK) o->temp_c = c;
    /* core load from the idle tasks' run time since the last call */
    /* run-time counters tick in esp_timer microseconds */
    static configRUN_TIME_COUNTER_TYPE last_idle[2];
    static int64_t last_total;
    int64_t total = esp_timer_get_time();
    for (int core = 0; core < 2; core++) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
        double dt = (double)(total - last_total);
        o->cpu[core] = dt > 0 ? 1.0f - (float)((idle - last_idle[core]) / dt) : 0;
        if (o->cpu[core] < 0) o->cpu[core] = 0;
        last_idle[core] = idle;
    }
    last_total = total;
    snprintf(o->chip, sizeof o->chip, "esp32-p4 · 360 mhz");
}

/* ------------------------------------------------------------------ the start-up record */

#define BOOT_MAGIC 0xCA7A1257u

/* In RTC memory that a reset leaves alone (a power-on clears it: magic no longer matches). */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t fails;    /* consecutive starts that never reached hal_boot_ok() */
    uint32_t settled;  /* this start did */
    char stage[24];
} B;

static hal_boot_t s_prev;
static bool s_minimal; /* three failed starts running: no Wi-Fi, no USB host, until one settles */

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SW: return "restart";
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_DEEPSLEEP: return "wake";
    case ESP_RST_CPU_LOCKUP: return "cpu lockup";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_USB: return "usb reset";
    case ESP_RST_JTAG: return "jtag reset";
    default: return "reset";
    }
}

static void boot_on_restart(void);

static void boot_record_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool valid = B.magic == BOOT_MAGIC;
    /* a crash, a watchdog, a brownout or a restart nobody here asked for (esp-hosted restarts the chip
     * when it loses the C6) — anything but a power-on or the reset button */
    bool abnormal = r != ESP_RST_POWERON && r != ESP_RST_EXT && r != ESP_RST_DEEPSLEEP && r != ESP_RST_USB &&
                    r != ESP_RST_JTAG;
    memset(&s_prev, 0, sizeof s_prev);
    snprintf(s_prev.reason, sizeof s_prev.reason, "%s", reset_name(r));
    if (valid) snprintf(s_prev.stage, sizeof s_prev.stage, "%.*s", (int)sizeof B.stage - 1, B.stage);
    if (!valid) B.fails = 0;
    if (valid && abnormal) {
        s_prev.failed = true;
        if (!B.settled) B.fails++;
    } else {
        B.fails = 0;
    }
    s_prev.fails = (int)B.fails;
    s_minimal = B.fails >= 3;
    /* the core dump the crash left, if any: which task, where, and the caller */
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t *sum = malloc(sizeof *sum);
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK)
            snprintf(s_prev.detail, sizeof s_prev.detail, "%.15s pc %08lx ra %08lx cause %lu", sum->exc_task,
                     (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.ra, (unsigned long)sum->ex_info.mcause);
        free(sum);
        esp_core_dump_image_erase();
    }
    B.magic = BOOT_MAGIC;
    B.settled = 0;
    snprintf(B.stage, sizeof B.stage, "start");
    esp_register_shutdown_handler(boot_on_restart);
    ESP_LOGI(TAG, "reset reason %d (%s), record %s, last stage \"%s\"", (int)r, reset_name(r), valid ? "kept" : "new",
             s_prev.stage);
    if (s_prev.failed)
        ESP_LOGW(TAG, "last start ended: %s at \"%s\" (%d in a row)%s%s", s_prev.reason, s_prev.stage, s_prev.fails,
                 s_prev.detail[0] ? ", " : "", s_prev.detail);
}

/* esp_restart() from anywhere (esp-hosted's lost-link restart among them): the stage says so next start */
static void boot_on_restart(void)
{
    char was[sizeof B.stage];
    snprintf(was, sizeof was, "%s", B.stage);
    snprintf(B.stage, sizeof B.stage, "restart@%.14s", was);
}

static void (*s_watch)(const char *stage);

void hal_boot_watch(void (*fn)(const char *stage)) { s_watch = fn; }

void hal_boot_stage(const char *stage)
{
    snprintf(B.stage, sizeof B.stage, "%s", stage);
    ESP_LOGI(TAG, "start: %s", stage);
    if (s_watch) s_watch(stage);
}

void hal_boot_prev(hal_boot_t *o) { *o = s_prev; }

void hal_boot_ok(void)
{
    B.settled = 1;
    B.fails = 0;
    snprintf(B.stage, sizeof B.stage, "running");
}

/* The last start's ending, where someone can read it without a serial cable: <sd>/catalyst-boot.txt. */
static void boot_report_sd(void)
{
    if (!T.sd || !s_prev.failed) return;
    FILE *f = fopen(BSP_SD_MOUNT_POINT "/catalyst-boot.txt", "a");
    if (!f) return;
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d  last start: %s at \"%s\", %d in a row%s%s\n", tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, s_prev.reason, s_prev.stage, s_prev.fails,
            s_prev.detail[0] ? " · " : "", s_prev.detail);
    fclose(f);
}

/* ------------------------------------------------------------------ init */

bool hal_init(void)
{
    boot_record_init();
    hal_dev_init(); /* first: the console carries the log from here on */
    hal_boot_stage("nvs");
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    /* the time zone the PC or settings chose (the RTC keeps UTC-agnostic local time; TZ turns it back) */
    char tz[48];
    if (hal_kv_get("tz", tz, sizeof tz)) {
        setenv("TZ", tz, 1);
        tzset();
    }
    hal_boot_stage("power");
    ESP_ERROR_CHECK(bsp_i2c_init());
    T.i2c = bsp_i2c_get_handle();
    power_init();

    /* the three full-frame buffers the renderer works in, in PSRAM */
    hal_boot_stage("buffers");
    T.content = psram_aligned(HAL_W * HAL_H * 2);
#if !BZ_LEAN
    T.ink = psram_aligned(HAL_W * HAL_H * 4); /* the glass layer: the lean renderer has none (3.7 MB saved) */
#endif
    T.out = psram_aligned(HAL_W * HAL_H * 2);
    if (!T.content || (!BZ_LEAN && !T.ink) || !T.out) return false;
    span_add(T.content, HAL_W * HAL_H * 2);
    if (T.ink) span_add(T.ink, HAL_W * HAL_H * 4);
    span_add(T.out, HAL_W * HAL_H * 2);
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    ppa_register_client(&pc, &T.ppa_srm);
    ppa_register_client(&pc, &T.ppa_cam);
    pc.oper_type = PPA_OPERATION_BLEND;
    ppa_register_client(&pc, &T.ppa_blend);
    rot_init();
    if (T.ink) ppa_blend_selftest();
    par_init();

    hal_boot_stage("display");
    display_init();
    return true;
}

void hal_start(void)
{
    hal_boot_stage("sensors");
    ina226_init();
    rtc_init();
    imu_init();
    hal_boot_stage("speaker");
    audio_init();
    hal_boot_stage("microsd");
    T.sd = bsp_sdcard_mount() == ESP_OK;
    boot_report_sd();
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &T.tsens) == ESP_OK) temperature_sensor_enable(T.tsens);
    if (s_minimal) {
        ESP_LOGW(TAG, "three failed starts: no Wi-Fi or USB tether until one settles");
    } else {
        hal_boot_stage("wi-fi");
        hal_net_init();
    }
    hal_boot_stage("ui");
    ESP_LOGI(TAG, "ready: panel %s, sd %s, imu %s", T.panel, T.sd ? "mounted" : "none", T.imu ? "ok" : "missing");
}

void hal_settle(void)
{
    ESP_LOGI(TAG, "panel refresh measured %.1f Hz", T.vsyncs / (hal_seconds() - T.display_t0 + 1e-6));
    if (s_minimal) return;
    hal_boot_stage("usb");
    e2_set(E2_USB5V, 1); /* the USB-A port's 5 V, for a Systemcore cable or a dongle */
    vTaskDelay(pdMS_TO_TICKS(50));
    hal_net_tether_init();
    hal_boot_stage("settling");
}
