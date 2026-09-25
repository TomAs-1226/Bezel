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
 * never waits on a peripheral. Audio (speaker, microphones, wake word) is in hal_tab5_audio.c.
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
#include <errno.h>
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
#include "hal/axi_icm_ll.h"
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
    void *fb[3];
    int back;  /* the asynchronous present's own (CATALYST_ASYNC_PRESENT) */
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

void hal_tab5_ext5v(bool on) { ext5v(on); }

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

/* A 2S Li-ion pack by voltage per cell: there is no fuel gauge, so this is an estimate. The curve is a
 * typical 18650/NP-F cell's resting (open-circuit) voltage at 25 °C. */
static float percent_from_cell(float v)
{
    static const float V[] = { 3.30f, 3.50f, 3.60f, 3.65f, 3.70f, 3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f };
    static const float P[] = { 0, 5, 12, 20, 30, 40, 50, 65, 78, 90, 100 };
    int n = sizeof V / sizeof V[0];
    if (v <= V[0]) return 0;
    for (int i = 1; i < n; i++)
        if (v <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]);
    return 100;
}

/* the pack's internal resistance (two cells, wiring and protection): a 1 A draw pulls the terminals
 * ~0.15 V under the resting voltage, which the curve would read as 10-15 % gone */
#define PACK_OHMS 0.15f

bool hal_battery(hal_battery_t *o)
{
    memset(o, 0, sizeof *o);
    uint8_t b[2];
    if (!reg_read(T.ina, 0x02, b, 2)) return false;
    o->volts = (uint16_t)(b[0] << 8 | b[1]) * 1.25e-3f;
    if (reg_read(T.ina, 0x04, b, 2)) o->amps = (int16_t)(b[0] << 8 | b[1]) * 250e-6f;
    o->charging = o->amps > 0.05f; /* positive current is charging (M5's demo) */
    /* on its own pack the tablet draws a few hundred mA; on USB power with the pack full nothing flows
     * either way: not discharging is external power */
    o->external = o->amps > -0.05f;
    /* On USB power with the pack idle, the monitor reads ~4.28 V for seconds at a time between ~8.39 V
     * readings (the charger probing the pack, as far as can be told from here), and at power-on it reads
     * low until the rails settle. A pack's voltage can't jump: a reading more than 0.3 V from the last
     * accepted one counts only once it has held for 20 s, and anything under 5.5 V (2.75 V a cell, under
     * the protection's cut-off) never does. Until a reading is accepted the last estimate stands. */
    static int last_pct = -1;
    static float last_v = -1;
    static double off_since;
    double now = hal_seconds();
    bool odd = o->volts < 5.5f;
    if (!odd && last_v > 0 && fabsf(o->volts - last_v) > 0.3f) {
        if (!off_since) off_since = now;
        odd = now - off_since < 20;
    }
    if (odd) {
        if (last_pct < 0) return false;
        o->volts = last_v;
        o->percent = last_pct;
        o->ok = true;
        return true;
    }
    off_since = 0;
    last_v = o->volts;
    /* the resting voltage: what the terminals would read with no current flowing either way */
    float rest = o->volts - o->amps * PACK_OHMS;
    float raw = percent_from_cell(rest / 2);
    /* the charge changes over minutes; the reading moves with every load step (the backlight, Wi-Fi, a
     * PPA burst). Filtered over ~2 minutes (the first minute after power-on settles fast), and while not
     * charging it climbs four times slower than it falls: a load easing off isn't charge coming back */
    static float soc = -1;
    static double at;
    if (soc < 0 || now - at > 600) soc = raw; /* first reading, or a long gap (asleep, off) */
    else {
        float k = (float)((now - at) / (now < 60 ? 3.0 : 120.0));
        if (!o->charging && raw > soc && now >= 60) k *= 0.25f;
        if (k > 1) k = 1;
        soc += (raw - soc) * k;
    }
    at = now;
    o->percent = (int)lroundf(soc);
    last_pct = o->percent;
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
    /* the chip keeps local wall time with no DST flag: -1 lets mktime work it out. Left 0 (standard time), a
     * start in PDT came up an hour ahead until SNTP set it right a minute later */
    out->tm_isdst = -1;
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

static struct {
    hal_imu_t v;
    volatile uint32_t seq; /* odd while being written */
} IMU;
static bool imu_read(hal_imu_t *o);

static void imu_task(void *arg)
{
    (void)arg;
    for (;;) {
        hal_imu_t m;
        bool ok = imu_read(&m);
        m.ok = ok;
        IMU.seq++;
        IMU.v = m;
        IMU.seq++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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
    /* read on core 0 at 50 Hz: an I2C read takes a few ms, and on the UI's core (auto-rotate, the level,
     * the companion's eyes) it came out of every frame's budget */
    xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 3, NULL, 0);
}

bool hal_imu(hal_imu_t *o)
{
    /* the last reading, a copy taken with the sequence number unchanged across it */
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s0 = IMU.seq;
        if (s0 & 1) continue;
        *o = IMU.v;
        if (IMU.seq == s0) return o->ok;
    }
    memset(o, 0, sizeof *o);
    return false;
}

static bool imu_read(hal_imu_t *o)
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
    /* The DMA (a scrolled list, a sheet) writes these buffers behind the cache's back: lines cached from
     * before would be merged with the pixels written here and written back over what the DMA put there.
     * Dropped first (no line in a frame buffer is ever left dirty: every CPU write below is written back
     * straight after), so each is read fresh. */
    {
        int r0 = s_flip ? a->x1 : HAL_W - 1 - a->x2, r1 = s_flip ? a->x2 : HAL_W - 1 - a->x1;
        uintptr_t b0 = (uintptr_t)(fb + (size_t)r0 * PANEL_W) & ~(uintptr_t)127;
        uintptr_t b1 = ((uintptr_t)(fb + (size_t)(r1 + 1) * PANEL_W) + 127) & ~(uintptr_t)127;
        esp_cache_msync((void *)b0, b1 - b0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    }
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

/* A slide's picture turned in by the PPA while the CPU goes on (slide_patch): at most one in flight, waited
 * for (rot_wait) before anything reads that picture or uses the PPA's queue again */
static bool s_rot_nowait, s_rot_pending;
static volatile bool s_job_frame; /* the job last handed to core 0 is a composed frame, not a present */

static void rot_wait(void)
{
    if (!s_rot_pending) return;
    s_rot_pending = false;
    if (xSemaphoreTake(ROTQ.done, pdMS_TO_TICKS(300)) != pdTRUE) {
        ROTQ.wedged = true;
        ESP_LOGE(TAG, "present: a slide patch didn't finish in 300 ms: the CPU turns the picture until it does");
    }
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
                if (s_rot_nowait) {
                    s_rot_pending = true;
                    return false;
                }
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
/* The default: three frame buffers. The panel scans one; one may be handed over and waiting for the next
 * vsync; the third is always free, so a frame's work (DMA copies, rotations) starts at once and overlaps
 * the wait for the last one to reach the glass. With two buffers every moving frame had to wait for the
 * vsync before its work began, and so missed the next one: 30 fps at best.
 *
 * A buffer taken up again is brought up to date first: whatever changed in the frames since it last held
 * the picture is copied from the newest buffer by DMA2D (67 Mpx/s), not turned again by the PPA; a thin
 * sliver is turned from LVGL's buffer by the CPU instead (rotate_area). */
#define HIST 4
typedef struct {
    bz_present_t a[PRESENT_MAX + 4];
    int n;
    bool full; /* the whole screen: anything older is irrelevant */
} hist_t;
static hist_t s_hist[HIST];
static int s_nfb = 2, s_scan, s_pend = -1;  /* scanned now; handed over and not switched to yet (-1 none) */
static uint32_t s_frame, s_buf_frame[3];     /* frames handed over; the frame each buffer holds */

/* Lists scrolled since the last present (see scroll_apply): their pixels move in the panel's buffers by
 * DMA2D, from the newest picture; LVGL draws only the strip that came into view. */
#define SCROLL_MAX 4
static struct {
    bz_area_t a;
    int dy;
} s_scroll[SCROLL_MAX];
static int s_nscroll;
/* the scrolls the present being worked on applies: handed over from s_scroll with its frame */
static typeof(s_scroll[0]) s_jscroll[SCROLL_MAX];
static int s_njscroll;
static bool s_jstaged; /* its areas' pixels are a staging copy, gone by the next frame */
static bool scroll_apply(void *fb);
static void present_sync(void);
static void blk_copy(uint16_t *dst, int dx, int dy, const uint16_t *src, int sx, int sy, int w, int h);
static void portrait_rect(const bz_area_t *a, int *x, int *y, int *w, int *h);

static int fb_latest(void) { return s_pend >= 0 ? s_pend : s_scan; }

/* a vsync since the hand-over: the panel is on the handed buffer now */
static void fb_seen(bool wait)
{
    if (s_pend < 0) return;
    if (xSemaphoreTake(T.vsync, wait ? pdMS_TO_TICKS(100) : 0) == pdTRUE) {
        s_scan = s_pend;
        s_pend = -1;
    } else if (wait) {
        if (!P.late++) ESP_LOGW(TAG, "present: no vsync");
        s_scan = s_pend; /* carry on rather than stall */
        s_pend = -1;
    }
}

/* a buffer neither on the glass nor waiting to be */
static int fb_pick(void)
{
    fb_seen(false);
    for (int i = 0; i < s_nfb; i++)
        if (i != s_scan && i != s_pend) return i;
    fb_seen(true); /* two buffers and one pending: wait for it */
    for (int i = 0; i < s_nfb; i++)
        if (i != s_scan && i != s_pend) return i;
    return s_scan ^ 1;
}

static void fb_copy_rect(int dst, int src, const bz_area_t *a)
{
    int x, y, w, h;
    portrait_rect(a, &x, &y, &w, &h);
    blk_copy(T.fb[dst], x, y, T.fb[src], x, y, w, h);
}

static bool covered_by(const bz_area_t *r, const bz_present_t *areas, int n)
{
    for (int j = 0; j < n; j++)
        if (covers(&areas[j].a, r)) return true;
    for (int j = 0; j < s_njscroll; j++)
        if (covers(&s_jscroll[j].a, r)) return true;
    return false;
}

/* buffer b made the newest picture, except where this frame writes anyway */
static void fb_catch_up(int b, const bz_present_t *areas, int n, bool cover_full)
{
    uint32_t from = s_buf_frame[b];
    if (cover_full || from >= s_frame) return;
    int latest = fb_latest();
    bool full = s_frame - from >= HIST;
    for (uint32_t f = from + 1; f <= s_frame && !full; f++) full = s_hist[f % HIST].full;
    if (full) {
        blk_copy(T.fb[b], 0, 0, T.fb[latest], 0, 0, PANEL_W, PANEL_H);
        return;
    }
    for (uint32_t f = from + 1; f <= s_frame; f++) {
        const hist_t *h = &s_hist[f % HIST];
        for (int i = 0; i < h->n; i++) {
            const bz_present_t *e = &h->a[i];
            if (covered_by(&e->a, areas, n)) continue;
            int w = e->a.x2 - e->a.x1 + 1, hh = e->a.y2 - e->a.y1 + 1;
            /* a sliver the DMA would move as a block a few pixels wide: turned again from LVGL's buffer */
            if (e->src && (w < 16 || hh < 16)) rotate_area(e, T.fb[b], false);
            else fb_copy_rect(b, latest, &e->a);
        }
    }
}

/* dev console "perf": gaps between consecutive hand-overs while something moves (under 200 ms), bucketed by how
 * many vsyncs they span: 1, 2, 3, more */
static int s_gap[4];
static double s_lw, s_lmax;
static int s_ln, s_lidle, s_lh[5];
void hal_loop_hist(int out[5])
{
    for (int i = 0; i < 5; i++) {
        out[i] = s_lh[i];
        s_lh[i] = 0;
    }
}
void hal_loop_note(double work_s, bool busy)
{
    if (!busy) {
        s_lidle++;
        return;
    }
    s_lw += work_s;
    s_ln++;
    s_lh[work_s < 0.005 ? 0 : work_s < 0.010 ? 1 : work_s < 0.0167 ? 2 : work_s < 0.025 ? 3 : 4]++;
    if (work_s > s_lmax) s_lmax = work_s;
}
void hal_loop_prof(double out[4])
{
    out[0] = s_ln;
    out[1] = s_ln ? s_lw / s_ln : 0;
    out[2] = s_lmax;
    out[3] = s_lidle;
    s_lw = s_lmax = 0;
    s_ln = s_lidle = 0;
}
void hal_frame_gaps(int out[4])
{
    for (int i = 0; i < 4; i++) {
        out[i] = s_gap[i];
        s_gap[i] = 0;
    }
}

/* b holds this frame now: handed to the panel, which switches to it at the next vsync */
static void fb_handover(int b, const bz_present_t *areas, int n, bool full, int y0, int y1)
{
    fb_seen(true); /* one frame waiting at a time: none is ever dropped */
    xSemaphoreTake(T.vsync, 0);
    esp_lcd_panel_draw_bitmap(T.lcd.panel, 0, y0, PANEL_W, y1 + 1, T.fb[b]);
    {
        static double last;
        double now = hal_seconds(), g = now - last;
        if (last > 0 && g < 0.2) s_gap[g < 0.022 ? 0 : g < 0.039 ? 1 : g < 0.056 ? 2 : 3]++;
        last = now;
    }
    s_pend = b;
    s_frame++;
    s_buf_frame[b] = s_frame;
    hist_t *h = &s_hist[s_frame % HIST];
    h->full = full;
    h->n = 0;
    for (int i = 0; i < n && h->n < PRESENT_MAX + 4; i++) h->a[h->n++] = areas[i];
    if (!T.lit) {
        T.lit = true;
        bsp_display_brightness_set(70);
    }
}

/* dev console "perf": seconds per stage of hal_present since the last hal_present_prof() */
static double s_pp[5];
static int s_ppn;
void hal_present_prof(double out[6])
{
    for (int i = 0; i < 5; i++) {
        out[i] = s_pp[i];
        s_pp[i] = 0;
    }
    out[5] = s_ppn;
    s_ppn = 0;
}

static void plane_draw(uint16_t *fb, int *y0, int *y1);

static void present_do(const bz_present_t *areas, int n)
{
    double t0 = hal_seconds();
    int b = fb_pick();
    void *fb = T.fb[b];
    int y0 = PANEL_H, y1 = -1;
    /* areas that tile the whole screen leave nothing to catch up (a full redraw) */
    uint32_t cover = 0;
    for (int i = 0; i < n; i++)
        cover += (uint32_t)(areas[i].a.x2 - areas[i].a.x1 + 1) * (uint32_t)(areas[i].a.y2 - areas[i].a.y1 + 1);
    bool full = cover >= (uint32_t)HAL_W * HAL_H;
    double t1 = hal_seconds();
    fb_catch_up(b, areas, n, full);
    double t2 = hal_seconds();
    /* scrolled lists before anything drawn over them; remembered as copies from the newest picture */
    bz_present_t rec[PRESENT_MAX + SCROLL_MAX];
    int nrec = 0;
    for (int i = 0; i < s_njscroll; i++) {
        rec[nrec++] = (bz_present_t){ s_jscroll[i].a, NULL, 0 };
        rows_of(&s_jscroll[i].a, &y0, &y1);
    }
    scroll_apply(fb);
    double t3 = hal_seconds();
    for (int i = 0; i < n; i++) {
        rotate_area(&areas[i], fb, false);
        rows_of(&areas[i].a, &y0, &y1);
        /* a staged source is gone by the next frame: the catch-up copies from the newest buffer instead */
        if (nrec < PRESENT_MAX + SCROLL_MAX) rec[nrec++] = (bz_present_t){ areas[i].a, s_jstaged ? NULL : areas[i].src, areas[i].stride };
        else if (!P.overflow++) ESP_LOGW(TAG, "present: %d areas, the catch-up keeps %d", n, PRESENT_MAX);
    }
    plane_draw(fb, &y0, &y1); /* the camera's picture, over what LVGL drew under it */
    if (y1 < y0) { y0 = 0; y1 = 0; }
    /* only the rows the CPU may have touched: draw_bitmap writes back the cache over exactly these */
    double t4 = hal_seconds();
    fb_handover(b, rec, nrec, full, y0, y1);
    double t5 = hal_seconds();
    s_pp[0] += t1 - t0; /* pick (a wait only with two buffers) */
    s_pp[1] += t2 - t1; /* catch-up */
    s_pp[2] += t3 - t2; /* scroll copies */
    s_pp[3] += t4 - t3; /* rotations */
    s_pp[4] += t5 - t4; /* hand-over (the wait for the last frame's vsync) */
    s_ppn++;
}

/* The present runs on core 0 while core 1 goes on to the next frame: hooks and LVGL (~6 ms) overlap the
 * DMA copies and rotations (~14 ms), where one after the other they took ~21 ms, a frame every other
 * vsync. LVGL draws its next frame into the same buffer the areas point into, so their pixels are first
 * copied out (the areas of a moving frame are small: the strip a list scrolled into view, a label); a
 * frame too big to copy (a full redraw) is presented on the spot instead, as before. One job in flight:
 * a frame waits for the last one's present to finish, and anything else that touches the frame buffers
 * (a slide, a sheet, a flip, a shot) waits for it too (present_sync). */
#define STAGE_PX (HAL_W * 240)
static struct {
    SemaphoreHandle_t go, idle;
    bz_present_t a[BZ_COMP_MAX_PRESENT];
    int n;
    bool staged;
    uint16_t *stage;
    bool task;
    void (*fn)(void); /* a composed frame (a slide's, a sheet's) instead of areas */
} J;

/* A panel shot holds the presents while it copies the picture (hal_front_fb_copy): the UI waits here rather
 * than taking the lock back, which it would every frame ahead of the shot's low-priority task */
static volatile bool s_hold;
static void hold_wait(void)
{
    while (s_hold) vTaskDelay(1);
}

static void present_sync(void)
{
    if (!J.task) return;
    xSemaphoreTake(J.idle, portMAX_DELAY);
    xSemaphoreGive(J.idle);
}

static void present_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(J.go, portMAX_DELAY);
        if (J.fn) J.fn();
        else present_do(J.a, J.n);
        J.fn = NULL;
        xSemaphoreGive(J.idle);
    }
}

void hal_present(const bz_present_t *areas, int n, void *user)
{
    (void)user;
    if (n > BZ_COMP_MAX_PRESENT) n = BZ_COMP_MAX_PRESENT;
    uint32_t px = 0;
    for (int i = 0; i < n; i++)
        if (areas[i].src)
            px += (uint32_t)(areas[i].a.x2 - areas[i].a.x1 + 1) * (uint32_t)(areas[i].a.y2 - areas[i].a.y1 + 1);
    rot_wait(); /* the PPA's queue is the present's again */
    hold_wait();
    if (J.task) xSemaphoreTake(J.idle, portMAX_DELAY);
    s_job_frame = false;
    /* this frame's scrolls go with it */
    memcpy(s_jscroll, s_scroll, sizeof s_scroll[0] * (size_t)s_nscroll);
    s_njscroll = s_nscroll;
    s_nscroll = 0;
    J.n = n;
    J.staged = J.task && J.stage && px <= STAGE_PX;
    s_jstaged = J.staged;
    if (!J.staged) {
        memcpy(J.a, areas, sizeof *areas * (size_t)n);
        present_do(J.a, J.n);
        if (J.task) xSemaphoreGive(J.idle);
        return;
    }
    uint16_t *st = J.stage;
    for (int i = 0; i < n; i++) {
        J.a[i] = areas[i];
        if (!areas[i].src) continue;
        int w = areas[i].a.x2 - areas[i].a.x1 + 1, h = areas[i].a.y2 - areas[i].a.y1 + 1;
        for (int y = 0; y < h; y++) memcpy(st + (size_t)y * w, areas[i].src + (size_t)y * areas[i].stride, (size_t)w * 2);
        J.a[i].src = st;
        J.a[i].stride = w;
        st += (size_t)w * h;
    }
    /* out of the cache: the PPA reads the staging copy from memory */
    if (st > J.stage)
        esp_cache_msync(J.stage, (size_t)(st - J.stage) * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    xSemaphoreGive(J.go);
}

static void present_init(void)
{
    J.go = xSemaphoreCreateBinary();
    J.idle = xSemaphoreCreateBinary();
    xSemaphoreGive(J.idle);
    J.stage = heap_caps_aligned_alloc(128, STAGE_PX * 2 + 256, MALLOC_CAP_SPIRAM);
    /* above everything on core 0 but the Wi-Fi link's own tasks: its work is mostly waiting on DMA */
    J.task = J.go && J.idle && J.stage &&
             xTaskCreatePinnedToCore(present_task, "present", 4096, NULL, 8, NULL, 0) == pdPASS;
    if (!J.task) ESP_LOGW(TAG, "present: on the UI's core (no memory for the core-0 task)");
}

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

/* a whole frame composed by DMA (a slide, a sheet), handed over as the full screen */
static void present_end(int b) { fb_handover(b, NULL, 0, true, 0, 0); }

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
    present_sync();
    if (!SLD.snap || !SLD.nb || !SLD.chrome || !s_fbcpy || !s_ground) return false;
    SLD.ground = (uint16_t)(((ground_rgb >> 19) & 31) << 11 | ((ground_rgb >> 10) & 63) << 5 | ((ground_rgb >> 3) & 31));
    /* the latest picture handed to the panel — final in memory even if the panel switches to it only at
     * the next vsync, so no waiting for it: the page with its chrome */
    const uint16_t *front = T.fb[fb_latest()];
    fbcpy_wait();
    /* the chrome's own pixels (small), then the page (big) left copying while the renderer draws the
     * chrome-free patches; the first patch waits for it */
    for (int i = 0; i < nchrome; i++) {
        int x, y, w, h;
        portrait_rect(&chrome[i], &x, &y, &w, &h);
        rect_copy(SLD.chrome, front, x, y, w, h);
    }
    blk_copy_async(SLD.snap, 0, 0, front, 0, 0, PANEL_W, PANEL_H);
    /* done here, once, at touch-down: the frames that follow run their DMA2D copies on core 0, and a
     * wait for this one from the UI's core could take their completion instead */
    fbcpy_wait();
    SLD.active = true;
    return true;
}

/* a landscape area turned into the snapshot (the page without its chrome) or the neighbour */
static void slide_patch(const bz_present_t *p, bool neighbour)
{
    /* a slide's or a sheet's frame in flight on core 0 reads rows of the pictures this doesn't write
     * (the ones not shown yet), and uses the DMA2D, not the PPA: no need to wait for it. A present
     * uses the PPA's queue: that one is waited for. */
    if (!s_job_frame) present_sync();
    rot_wait();
    if (!SLD.active) return;
    /* not waited for: the renderer draws the next band (or the frame goes on) while the PPA turns this */
    s_rot_nowait = true;
    rotate_area(p, neighbour ? SLD.nb : SLD.snap, false);
    s_rot_nowait = false;
}

static void slide_frame_do(int dx, int side, const bz_area_t *chrome, int nchrome)
{
    int b = fb_pick();
    double tf0 = hal_seconds();
    uint16_t *fb = T.fb[b];
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
    (void)tf0;
    present_end(b);
}

/* A composed frame goes to core 0 like any other present: the UI's core draws the next band meanwhile */
static struct {
    int dx, side, n, h, sh;
    bool swapped, bottom;
    bz_area_t chrome[BZ_SLIDE_CHROME];
} SJ;

static void slide_job(void) { slide_frame_do(SJ.dx, SJ.side, SJ.chrome, SJ.n); }

static void job_run(void (*fn)(void))
{
    if (!J.task) {
        fn();
        return;
    }
    hold_wait();
    xSemaphoreTake(J.idle, portMAX_DELAY);
    s_job_frame = true;
    J.fn = fn;
    xSemaphoreGive(J.go);
}

static void slide_frame(int dx, int side, const bz_area_t *chrome, int nchrome)
{
    if (!SLD.active) return;
    if (side) rot_wait(); /* the neighbour is shown once all of it is drawn: its last band may be turning */
    present_sync();
    SJ.dx = dx;
    SJ.side = side;
    SJ.n = nchrome < BZ_SLIDE_CHROME ? nchrome : BZ_SLIDE_CHROME;
    for (int i = 0; i < SJ.n; i++) SJ.chrome[i] = chrome[i];
    job_run(slide_job);
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
    present_sync();
    rot_wait();
    SLD.active = false;
    P.nprev = 0;
}

/* A sheet pulled down over the page (the control center). A landscape row is a portrait column, so the
 * sheet's visible rows and the page's are each one rectangle the panel's full height: two DMA2D copies a
 * frame, nothing turned. The sheet's picture (drawn at rest, full screen) shows by its bottom h rows. */
/* landscape rows [sy, sy + n) of src onto rows [dy, dy + n) of dst: a landscape row is a portrait column
 * (column H - 1 - y when the picture is turned), so any run of rows is one rectangle */
static void lrows_copy(uint16_t *dst, int dy, const uint16_t *src, int sy, int n)
{
    if (n <= 0) return;
    if (!s_flip) blk_copy_async(dst, dy, 0, src, sy, 0, n, PANEL_H);
    else blk_copy_async(dst, HAL_H - dy - n, 0, src, HAL_H - sy - n, 0, n, PANEL_H);
}

static void sheet_frame_do(int h, int sh, bool swapped, bool bottom)
{
    const int H = HAL_H; /* landscape rows = portrait columns = PANEL_W */
    if (sh > H || bottom) sh = H;
    if (h < 8) h = 0;    /* a sliver would be a DMA block a few pixels wide: none at all */
    if (h > sh - 8) h = sh;
    /* the sheet's picture, and the page's rows it has uncovered; past the sheet, the glass as it was */
    const uint16_t *sheet = swapped ? SLD.snap : SLD.nb, *under = swapped ? SLD.nb : SLD.snap;
    int b = fb_pick();
    uint16_t *fb = T.fb[b];
    fbcpy_wait();
    if (!bottom) {
        lrows_copy(fb, 0, sheet, sh - h, h);
        lrows_copy(fb, h, under, h, sh - h);
        lrows_copy(fb, sh, SLD.snap, sh, H - sh);
    } else {
        lrows_copy(fb, 0, under, 0, H - h);
        lrows_copy(fb, H - h, sheet, 0, h);
    }
    fbcpy_wait();
    present_end(b);
}

static void sheet_job(void) { sheet_frame_do(SJ.h, SJ.sh, SJ.swapped, SJ.bottom); }

static void sheet_frame(int h, int sh, bool swapped, bool bottom)
{
    if (!SLD.active) return;
    rot_wait(); /* the rows just patched are shown now */
    present_sync();
    SJ.h = h;
    SJ.sh = sh;
    SJ.swapped = swapped;
    SJ.bottom = bottom;
    job_run(sheet_job);
}

static void slide_scroll(const bz_area_t *a, int dy)
{
    if (!dy) return;
    for (int i = 0; i < s_nscroll; i++)
        if (!memcmp(&s_scroll[i].a, a, sizeof *a)) {
            s_scroll[i].dy += dy; /* twice in a frame: one move */
            return;
        }
    if (s_nscroll < SCROLL_MAX) s_scroll[s_nscroll++] = (typeof(s_scroll[0])){ *a, dy };
}

/* Each scrolled rect: the rows that stay in view, from the glass, dy rows further on. In the portrait
 * buffer a landscape row is a column, so it's one DMA2D rectangle. */
static bool scroll_apply(void *fb)
{
    if (!s_njscroll) return false;
    const uint16_t *front = T.fb[fb_latest()];
    for (int i = 0; i < s_njscroll; i++) {
        const bz_area_t *a = &s_jscroll[i].a;
        int dy = s_jscroll[i].dy, d0 = a->y1 > a->y1 + dy ? a->y1 : a->y1 + dy, d1 = a->y2 < a->y2 + dy ? a->y2 : a->y2 + dy;
        if (d1 < d0) continue;
        bz_area_t dst = { a->x1, (int16_t)d0, a->x2, (int16_t)d1 }, src = { a->x1, (int16_t)(d0 - dy), a->x2, (int16_t)(d1 - dy) };
        int dx, dyy, sx, sy, w, h;
        portrait_rect(&dst, &dx, &dyy, &w, &h);
        portrait_rect(&src, &sx, &sy, &w, &h);
        blk_copy_async(fb, dx, dyy, front, sx, sy, w, h);
    }
    fbcpy_wait(); /* before anything is turned over it */
    s_njscroll = 0;
    return true;
}

static void slide_settle(void)
{
    present_sync();
    /* the glass's buffer copied into the other one: the next present, a few areas, lands on the same picture */
    /* nothing to do: a buffer taken up again is brought up to date from the newest (fb_catch_up) */
}

static const bz_slide_ops_t SLIDE_OPS = { .begin = slide_begin, .patch = slide_patch, .frame = slide_frame, .end = slide_end,
                                          .sheet = sheet_frame, .settle = slide_settle, .scroll = slide_scroll };
#endif

static void display_init(void)
{
    bsp_display_config_t cfg = { .dsi_bus = { .phy_clk_src = 0, .lane_bit_rate_mbps = lane_rate() } };
    ESP_ERROR_CHECK(bsp_display_new_with_handles(&cfg, &T.lcd));
    esp_lcd_panel_disp_on_off(T.lcd.panel, true);
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS >= 3
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(T.lcd.panel, 3, &T.fb[0], &T.fb[1], &T.fb[2]));
    s_nfb = 3;
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(T.lcd.panel, 2, &T.fb[0], &T.fb[1]));
#endif
    s_scan = 0; /* the panel starts on the first */
    /* PSRAM is shared by the panel's scan-out (DW-GDMA, ~110 MB/s, a hard deadline), DMA2D (a scrolled list
     * moves ~3 MB a frame), the PPA and the CPU's cache. Every master starts at QoS 0, equal: a long DMA2D
     * copy could hold off the scan-out until its FIFO ran dry, and an underrun shows as a cyan screen. The
     * scan-out's reads go first, always. The copy engines come next, then the cache: below the cache, DMA2D
     * took 14.4 ms to shift a full-page list and scrolled at 30 fps; above it, 11.4 ms (its own ~60 Mpx/s,
     * where any higher priority stops helping) and a third of the frames at 60. */
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 8, 15);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 8, 15);
    axi_icm_ll_set_cache_qos_arbiter_prio(6, 6);
    axi_icm_ll_set_dma2d_qos_arbiter_prio(7, 7);
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
const uint16_t *hal_front_fb(void) { return T.fb[fb_latest()]; }

/* The same picture copied out whole, for the dev console's panel shot. Read in place over USB (a second or
 * more) it was overwritten by the frames after it whenever something moved: stripes of a later frame down
 * the right of the shot. The presents wait (s_hold) while the copy is made, a few tens of ms. */
bool hal_front_fb_copy(uint16_t *dst)
{
    s_hold = true;
    if (J.task) xSemaphoreTake(J.idle, portMAX_DELAY);
    const uint16_t *src = T.fb[fb_latest()];
    size_t n = (size_t)PANEL_W * PANEL_H * 2;
    esp_cache_msync((void *)src, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    memcpy(dst, src, n);
    if (J.task) xSemaphoreGive(J.idle);
    s_hold = false;
    return true;
}

void hal_set_flip(bool flip)
{
    present_sync();
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

/* The speaker, the microphones and the wake word live in hal_tab5_audio.c. */
void hal_set_volume(float v)
{
    T.volume = v;
    hal_audio_volume(v);
}

float hal_volume(void) { return T.volume; }

/* ------------------------------------------------------------------ camera */

/* The preview: 960×540 (the PPA scales in sixteenths: 3/4 of the sensor's 1280×720) when two of them fit in
 * PSRAM beside the camera's own two 1.8 MB capture buffers, else 800×450 (5/8) */
#define CAM_OUT_W C.out_w
#define CAM_OUT_H C.out_h
static struct {
    int fd;
    bool on, started;
    void *bufs[2];
    size_t lens[2];
    int src_w, src_h;
    uint16_t *frames[2];
    volatile int ready;         /* index of the newest complete frame, -1 none */
    volatile bool busy;         /* the camera task is between taking a frame and giving it back */
    int out_w, out_h;           /* the preview's size */
    /* The video plane: the preview turned into the panel's orientation by the PPA on core 0, then copied onto
     * the panel by DMA2D with each present (present_do), over whatever LVGL drew there. LVGL scaling and
     * the PPA turning 0.6 Mpx a frame had the lens app at ~14 fps with every button lagging. */
    volatile bool plane, freeze;
    int px, py;                 /* the plane's landscape top-left; its size is the preview's */
    volatile int reading;       /* the frame a present is copying (-1 none): not written meanwhile */
    volatile bool want_upright; /* a snapshot wants the next frame upright, for the JPEG */
    volatile int upright;       /* that frame (-1 none) */
    bool frame_flip[2];         /* the picture's turn each frame was made for */
    volatile uint32_t gen;      /* frames made */
    TaskHandle_t task;
    jpeg_encoder_handle_t jpeg;
} C = { .fd = -1, .ready = -1, .reading = -1, .upright = -1 };

static void clip_take(const void *src);
static bool clip_taking_frames(void);
static volatile bool s_clip_taking;

/* The plane's frame from the capture: scaled (nearest) and turned into the panel's orientation by the CPU, in
 * 32-row by 64-column tiles so both the capture's rows and the plane's stay in the cache. The PPA doing the
 * same took ~47 ms a frame (0.5 Mpx turned and scaled) and held up every present's rotations meanwhile. */
#define PT_ROWS 64 /* plane rows a tile */
#define PT_COLS 16 /* plane columns a tile: capture rows, each a cache line apart at least */
static struct {
    const uint16_t *rowp[720];
    int16_t sxt[1280];
    uint16_t *dst;
    int ow, oh, split;
    bool flip;
    TaskHandle_t helper, caller;
} PT;

/* the plane's rows [y0, y1) */
static void plane_turn_rows(int y0, int y1)
{
    int pw = PT.oh, ow = PT.ow, oh = PT.oh;
    for (int by = y0; by < y1; by += PT_ROWS) {
        int ey = by + PT_ROWS < y1 ? by + PT_ROWS : y1;
        for (int bx = 0; bx < pw; bx += PT_COLS) {
            int ex = bx + PT_COLS < pw ? bx + PT_COLS : pw;
            for (int py = by; py < ey; py++) {
                uint16_t *d = PT.dst + (size_t)py * pw;
                /* landscape (x, y) lands at portrait (y, W-1-x); turned the other way, at (H-1-y, x) */
                int sx = PT.sxt[PT.flip ? py : ow - 1 - py];
                if (!PT.flip)
                    for (int px = bx; px < ex; px++) d[px] = PT.rowp[px][sx];
                else
                    for (int px = bx; px < ex; px++) d[px] = PT.rowp[oh - 1 - px][sx];
            }
        }
    }
    esp_cache_msync(PT.dst + (size_t)y0 * pw, (size_t)(y1 - y0) * pw * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* The second half, on the UI's core under everything there: in the lens app that core is mostly idle */
static void plane_helper(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        plane_turn_rows(PT.split, PT.ow);
        xTaskNotifyGive(PT.caller);
    }
}

static void plane_turn(const uint16_t *src, int sw, int sh, uint16_t *dst, int ow, int oh, bool flip)
{
    if (ow > 1280 || oh > 720) return;
    for (int x = 0; x < ow; x++) PT.sxt[x] = (int16_t)(x * sw / ow);
    for (int y = 0; y < oh; y++) PT.rowp[y] = src + (size_t)(y * sh / oh) * sw;
    PT.dst = dst;
    PT.ow = ow;
    PT.oh = oh;
    PT.flip = flip;
    if (!PT.helper) xTaskCreatePinnedToCore(plane_helper, "plane", 3072, NULL, 3, &PT.helper, 1);
    if (!PT.helper) {
        plane_turn_rows(0, ow);
        return;
    }
    /* halves split on a multiple of 32 rows: a whole number of cache lines for either width, so neither core
     * writes back a line the other is writing */
    PT.split = ow / 2 / 32 * 32;
    PT.caller = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(PT.helper);
    plane_turn_rows(0, PT.split);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
}

static void cam_task(void *arg)
{
    int w = 0;
    for (;;) {
        C.busy = C.on;
        if (!C.busy) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(C.fd, VIDIOC_DQBUF, &b) != 0) {
            C.busy = false;
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
        /* the frame to write: not the one shown, not one a present is copying, not a snapshot's */
        bool upright = C.want_upright || !C.plane;
        w = C.ready < 0 ? 0 : C.ready ^ 1;
        bool skip = (C.plane && C.freeze && !C.want_upright) || w == C.reading || w == C.upright;
        if (!skip && !upright) {
            bool flip = s_flip;
            /* the capture came in by DMA: what the cache holds of this buffer is last time's */
            esp_cache_msync(C.bufs[b.index], (C.lens[b.index] + 127) & ~(size_t)127, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            /* The camera is fixed to the body, as the panel is: when the picture turns 180° (the tablet held
             * the other way up), the scene has turned with the body too, and the two cancel. So the frame is
             * laid out as the unturned panel's, whatever the picture's turn; only where it goes follows it. */
            (void)flip;
            plane_turn(C.bufs[b.index], C.src_w, C.src_h, C.frames[w], CAM_OUT_W, CAM_OUT_H, false);
            C.frame_flip[w] = flip;
            C.ready = w;
            C.gen++;
        } else if (!skip) {
            /* scale 1280×720 to the preview's size on the PPA, upright (the LVGL preview, a snapshot) */
            bool flip = s_flip;
            ppa_srm_oper_config_t op = {
                .in = { .buffer = C.bufs[b.index], .pic_w = (uint32_t)C.src_w, .pic_h = (uint32_t)C.src_h,
                        .block_w = (uint32_t)C.src_w, .block_h = (uint32_t)C.src_h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                .out = { .buffer = C.frames[w], .buffer_size = ((uint32_t)CAM_OUT_W * CAM_OUT_H * 2 + 127) & ~127u,
                         .pic_w = (uint32_t)(upright ? CAM_OUT_W : CAM_OUT_H),
                         .pic_h = (uint32_t)(upright ? CAM_OUT_H : CAM_OUT_W), .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
                /* upright for the person holding it: turned half round when the picture is (see plane_turn) */
                .rotation_angle = flip ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_0,
                .scale_x = (float)CAM_OUT_W / C.src_w,
                .scale_y = (float)CAM_OUT_H / C.src_h,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            static bool said;
            if (ppa_do_scale_rotate_mirror(T.ppa_cam, &op) != ESP_OK && !said) {
                said = true;
                ESP_LOGE(TAG, "camera: scale %p (%ux%u) -> %p failed", C.bufs[b.index], (unsigned)C.src_w,
                         (unsigned)C.src_h, C.frames[w]);
            }
            if (C.plane && C.want_upright) {
                C.upright = w;
                C.want_upright = false;
            } else {
                C.frame_flip[w] = flip;
                C.ready = w;
                C.gen++;
            }
        }
        ioctl(C.fd, VIDIOC_QBUF, &b);
        C.busy = false;
    }
}

bool hal_camera_start(void)
{
    if (C.on) return true;
    if (!C.started) {
        /* the video devices register once: a second bsp_camera_start() fails on the ISP already there */
        static bool bsp_up;
        if (!bsp_up) {
            bsp_camera_cfg_t cfg = { 0 };
            esp_err_t e = bsp_camera_start(&cfg);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "camera: bsp start: %s", esp_err_to_name(e));
                return false;
            }
            bsp_up = true;
        }
        if (C.fd < 0) C.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
        if (C.fd < 0) {
            ESP_LOGE(TAG, "camera: open %s: errno %d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
            return false;
        }
        struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        ioctl(C.fd, VIDIOC_G_FMT, &f);
        /* G_FMT copies the stream's stored format over all of it, type included, and before a first
         * S_FMT that type is 0: S_FMT would then find no stream and fail with EINVAL, silently */
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        for (int i = 0; i < 12; i++) {
            struct v4l2_fmtdesc d = { .index = (uint32_t)i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
            if (ioctl(C.fd, VIDIOC_ENUM_FMT, &d) != 0) break;
            ESP_LOGI(TAG, "camera: offers %.4s (%s)", (const char *)&d.pixelformat, (const char *)d.description);
        }
        ESP_LOGI(TAG, "camera: now %.4s %ux%u", (const char *)&f.fmt.pix.pixelformat, (unsigned)f.fmt.pix.width,
                 (unsigned)f.fmt.pix.height);
        if (ioctl(C.fd, VIDIOC_S_FMT, &f) != 0) {
            ESP_LOGE(TAG, "camera: RGB565 at %ux%u refused: errno %d", (unsigned)f.fmt.pix.width, (unsigned)f.fmt.pix.height, errno);
            return false;
        }
        C.src_w = (int)f.fmt.pix.width;
        C.src_h = (int)f.fmt.pix.height;
        struct v4l2_requestbuffers req = { .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(C.fd, VIDIOC_REQBUFS, &req) != 0) {
            ESP_LOGE(TAG, "camera: buffers refused: errno %d", errno);
            return false;
        }
        for (int i = 0; i < 2; i++) {
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)i };
            ioctl(C.fd, VIDIOC_QUERYBUF, &b);
            C.bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, C.fd, b.m.offset);
            C.lens[i] = b.length;
            ioctl(C.fd, VIDIOC_QBUF, &b);
        }
        static const int SZ[2][2] = { { 960, 540 }, { 800, 450 } };
        for (int k = 0; k < 2 && !C.frames[1]; k++) {
            free(C.frames[0]);
            C.frames[0] = NULL;
            C.out_w = SZ[k][0];
            C.out_h = SZ[k][1];
            size_t n = ((size_t)C.out_w * C.out_h * 2 + 127) & ~(size_t)127;
            C.frames[0] = psram_aligned(n);
            C.frames[1] = C.frames[0] ? psram_aligned(n) : NULL;
        }
        C.ready = C.reading = C.upright = -1;
        if (!C.frames[0] || !C.frames[1]) {
            ESP_LOGE(TAG, "camera: no PSRAM for the preview (%u free, %u in one piece)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            free(C.frames[0]);
            free(C.frames[1]);
            C.frames[0] = C.frames[1] = NULL;
            return false;
        }
        if (!C.task) xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 5, &C.task, 0);
        C.started = true;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(C.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "camera: stream on: errno %d", errno);
        return false;
    }
    C.on = true;
    return true;
}

/* Everything the camera holds goes back: its two 1.8 MB capture buffers (freed when the device closes)
 * and the two previews, 5 MB of PSRAM in all. Kept open, the lens app left the rest of the system with
 * ~0.5 MB, and every allocation after that failed (garbage on the panel, then a hang). */
void hal_camera_stop(void)
{
    if (!C.started) return;
    hal_clip_stop();
    C.on = false;
    C.plane = false;
    present_sync(); /* no present copying the plane from buffers about to be freed */
    for (int i = 0; i < 50 && C.busy; i++) vTaskDelay(pdMS_TO_TICKS(4));
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(C.fd, VIDIOC_STREAMOFF, &type);
    C.ready = -1;
    close(C.fd);
    C.fd = -1;
    free(C.frames[0]);
    free(C.frames[1]);
    C.frames[0] = C.frames[1] = NULL;
    C.bufs[0] = C.bufs[1] = NULL;
    C.started = false;
}

const uint16_t *hal_camera_frame(int *w, int *h)
{
    if (!C.on || C.ready < 0 || C.plane) return NULL;
    *w = CAM_OUT_W;
    *h = CAM_OUT_H;
    return C.frames[C.ready];
}

/* The plane's picture into a panel buffer: one DMA2D block from a buffer the preview's size */
static void plane_draw(uint16_t *fb, int *y0, int *y1)
{
    int i = C.ready;
    if (!C.plane || i < 0 || !s_fbcpy) return;
    C.reading = i;
    bz_area_t a = { (int16_t)C.px, (int16_t)C.py, (int16_t)(C.px + C.out_w - 1), (int16_t)(C.py + C.out_h - 1) };
    int x, y, w, h;
    portrait_rect(&a, &x, &y, &w, &h);
    fbcpy_wait();
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = C.frames[i], .dst_buffer = fb,
        .src_buffer_size_x = (size_t)w, .src_buffer_size_y = (size_t)h,
        .dst_buffer_size_x = PANEL_W, .dst_buffer_size_y = PANEL_H,
        .src_offset_x = 0, .src_offset_y = 0,
        .dst_offset_x = (size_t)x, .dst_offset_y = (size_t)y,
        .copy_size_x = (size_t)w, .copy_size_y = (size_t)h,
        .pixel_format_unique_id = { .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565) },
    };
    if (esp_async_fbcpy(s_fbcpy, &t, fbcpy_done, NULL) == ESP_OK) s_fbcpy_pending = true;
    fbcpy_wait();
    C.reading = -1;
    rows_of(&a, y0, y1);
}

bool hal_camera_plane(int bx, int by, int bw, int bh, int *x, int *y, int *w, int *h)
{
    if (!C.on || !C.frames[0] || !s_fbcpy) return false;
    C.px = bx + (bw - C.out_w) / 2;
    C.py = by + (bh - C.out_h) / 2;
    *x = C.px;
    *y = C.py;
    *w = C.out_w;
    *h = C.out_h;
    C.ready = -1; /* the frames so far are upright: none shown until a turned one is made */
    C.plane = true;
    return true;
}

void hal_camera_plane_off(void)
{
    if (!C.plane) return;
    C.plane = false;
    present_sync();
    C.ready = -1;
}

void hal_camera_freeze(bool freeze) { C.freeze = freeze; }
uint32_t hal_camera_gen(void) { return C.gen; }

bool hal_camera_snapshot(const char *path)
{
    int w = CAM_OUT_W, h = CAM_OUT_H;
    const uint16_t *f;
    if (C.plane) {
        /* the plane's frames are turned: the next one is made upright for this */
        C.upright = -1;
        C.want_upright = true;
        for (int i = 0; i < 60 && C.upright < 0; i++) vTaskDelay(pdMS_TO_TICKS(5));
        if (C.upright < 0) {
            C.want_upright = false;
            return false;
        }
        f = C.frames[C.upright];
    } else {
        f = hal_camera_frame(&w, &h);
    }
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
        C.upright = -1; /* copied out: the camera may write it again */
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
    hal_nfc_release(); /* the same two pins: an NFC reader there gives them up */
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
    uint32_t planned;  /* the last restart was asked for (hal_restart_planned): not a failed start */
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
    /* a restart asked for (the Wi-Fi watchdog, a key file, the C6's update) is no failed start: counted as
     * one, three of them put the tablet in safe mode, which leaves Wi-Fi off */
    if (valid && B.planned == BOOT_MAGIC) {
        abnormal = false;
        snprintf(s_prev.reason, sizeof s_prev.reason, "planned");
    }
    B.planned = 0;
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

bool hal_boot_settled(void) { return B.settled != 0; }
void hal_restart_mark_planned(void) { B.planned = BOOT_MAGIC; }

void hal_restart_planned(const char *why)
{
    ESP_LOGW(TAG, "restart: %s", why);
    B.planned = BOOT_MAGIC;
    esp_restart();
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
    hal_audio_init();
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
    hal_nfc_init(); /* probes Port A for an NFC unit a few seconds from now, on its own task */
    hal_boot_stage("settling");
}
