/* hal_tab5 — Catalyst Tab's hardware layer on the M5Stack Tab5.
 *
 * Built on Espressif's Tab5 BSP (espressif/m5stack_tab5) for what it does well — panel detection and
 * bring-up for all three panel revisions, touch, the audio codecs, the camera's sensor pipeline, the
 * microSD slot — and on ESP-IDF directly for the rest. docs/tab5-hardware.md has the pin map and the
 * research behind every number here. The network half (Wi-Fi, the USB tether, mDNS, HTTP, threads) is
 * hal_tab5_net.c.
 *
 * Tasks: the UI (LVGL + compositor) runs on core 1; this file's workers — tones, camera capture, the clip
 * encoder, CAN — run on core 0 beside the NetworkTables client, so the renderer never waits on a
 * peripheral. The microphones (ES7210) are deliberately left unused.
 *
 * Verified by compiling only: this file has not yet run on a Tab5. Items marked UNVERIFIED are the
 * ones most likely to need a correction on first boot. */
#include "hal.h"
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
#include "esp_codec_dev.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
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
#ifndef CATALYST_ROTATE_270
#define ROT PPA_SRM_ROTATION_ANGLE_90
#else
#define ROT PPA_SRM_ROTATION_ANGLE_270
#endif

#define PANEL_W BSP_LCD_H_RES /* 720 */
#define PANEL_H BSP_LCD_V_RES /* 1280 */

/* Grove Port A carries the CAN transceiver's TX and RX. UNVERIFIED which wire a given Grove CAN unit
 * uses for which; swap if the tap hears nothing on a live bus. */
#define CAN_TX_GPIO 53
#define CAN_RX_GPIO 54

/* expander pins (docs/tab5-hardware.md) */
#define E1_ANTENNA IO_EXPANDER_PIN_NUM_0
#define E1_SPK_EN IO_EXPANDER_PIN_NUM_1
#define E1_EXT5V IO_EXPANDER_PIN_NUM_2
#define E2_WLAN_PWR IO_EXPANDER_PIN_NUM_0
#define E2_USB5V IO_EXPANDER_PIN_NUM_3
#define E2_PWROFF IO_EXPANDER_PIN_NUM_4
#define E2_NCHG_QC IO_EXPANDER_PIN_NUM_5
#define E2_CHG_STAT IO_EXPANDER_PIN_NUM_6
#define E2_CHG_EN IO_EXPANDER_PIN_NUM_7

static struct {
    esp_io_expander_handle_t e1, e2;
    i2c_master_bus_handle_t i2c;
    i2c_master_dev_handle_t ina, rtc;
    bsp_lcd_handles_t lcd;
    void *fb[2];
    int back;
    SemaphoreHandle_t vsync;
    bz_area_t prev[BZ_COMP_MAX_DIRTY];
    int nprev;
    ppa_client_handle_t ppa_srm, ppa_blend;
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

static void power_init(void)
{
    /* Create both expander handles first: the driver's init resets its outputs, so every level set
     * below must come after, and nothing may create them again later (the upstream Wi-Fi feature
     * enable is reported to re-initialise 0x44 and cut charging). */
    T.e1 = bsp_io_expander_init();
    T.e2 = bsp_io_expander1_init();
    esp_io_expander_set_dir(T.e1, E1_ANTENNA | E1_SPK_EN | E1_EXT5V, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(T.e1, E1_ANTENNA, 0); /* the internal 3D antenna */
    esp_io_expander_set_level(T.e1, E1_EXT5V, 1);   /* 5 V to Grove: the CAN transceiver */
    esp_io_expander_set_dir(T.e2, E2_WLAN_PWR | E2_USB5V | E2_PWROFF | E2_NCHG_QC | E2_CHG_EN, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(T.e2, E2_CHG_STAT, IO_EXPANDER_INPUT);
    esp_io_expander_set_level(T.e2, E2_PWROFF, 0);
    esp_io_expander_set_level(T.e2, E2_WLAN_PWR, 1);
    esp_io_expander_set_level(T.e2, E2_USB5V, 1);
    /* the Tab5 charges only while firmware asks it to (M5's demo: quick charge, then charge enable) */
    esp_io_expander_set_level(T.e2, E2_NCHG_QC, 0);
    esp_io_expander_set_level(T.e2, E2_CHG_EN, 1);
}

void hal_power_off(void)
{
    /* the power MCU turns the tablet off on three pulses */
    for (int i = 0; i < 3; i++) {
        esp_io_expander_set_level(T.e2, E2_PWROFF, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(T.e2, E2_PWROFF, 0);
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
#ifndef CATALYST_ROTATE_270
    float lx = -py, ly = px;
#else
    float lx = py, ly = -px;
#endif
    o->ax = -lx;
    o->ay = -ly;
    o->az = -z;
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
    xSemaphoreGiveFromISR(T.vsync, &woken);
    return woken == pdTRUE;
}

/* Tells the ST7121 panel apart: its lanes run at 965 Mbps, the others at the BSP's 1000. */
static uint32_t lane_rate(void)
{
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (i2c_master_probe(T.i2c, 0x55, 100) != ESP_OK) {
        snprintf(T.panel, sizeof T.panel, "ILI9881C");
        return BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
    }
    i2c_master_dev_handle_t tp = i2c_dev(0x55);
    uint8_t reg[2] = { 0, 0 }, fw = 0;
    if (tp) i2c_master_transmit_receive(tp, reg, 2, &fw, 1, 50);
    if (tp) i2c_master_bus_rm_device(tp);
    snprintf(T.panel, sizeof T.panel, fw == 1 ? "ST7121" : "ST7123");
    return fw == 1 ? 965 : BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
}

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
    bsp_display_backlight_on();
    bsp_touch_new(NULL, &T.touch);
    ESP_LOGI(TAG, "panel %s, frame buffers %p %p", T.panel, T.fb[0], T.fb[1]);
}

const char *hal_panel_name(void) { return T.panel; }

void hal_set_brightness(float v)
{
    bsp_display_brightness_set((int)(v * 100 + 0.5f));
}

/* One landscape rectangle of the composite into a portrait frame buffer, turned by the PPA. */
static void rotate_area(const bz_area_t *a, void *fb)
{
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    ppa_srm_oper_config_t op = {
        .in = { .buffer = T.out, .pic_w = HAL_W, .pic_h = HAL_H, .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                .block_offset_x = (uint32_t)a->x1, .block_offset_y = (uint32_t)a->y1, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = fb, .buffer_size = PANEL_W * PANEL_H * 2, .pic_w = PANEL_W, .pic_h = PANEL_H,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = ROT,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
#ifndef CATALYST_ROTATE_270
    /* 90° counter-clockwise: landscape (x, y) lands at portrait (y, W-1-x) */
    op.out.block_offset_x = (uint32_t)a->y1;
    op.out.block_offset_y = (uint32_t)(HAL_W - 1 - a->x2);
#else
    op.out.block_offset_x = (uint32_t)(HAL_H - 1 - a->y2);
    op.out.block_offset_y = (uint32_t)a->x1;
#endif
    ppa_do_scale_rotate_mirror(T.ppa_srm, &op);
}

void hal_present(const bz_area_t *areas, int n, void *user)
{
    (void)user;
    /* The back buffer is one frame old: it needs this frame's areas and last frame's. */
    void *fb = T.fb[T.back];
    for (int i = 0; i < n; i++) rotate_area(&areas[i], fb);
    for (int i = 0; i < T.nprev; i++) rotate_area(&T.prev[i], fb);
    memcpy(T.prev, areas, sizeof(bz_area_t) * (size_t)n);
    T.nprev = n;
    /* hand the DPI controller the finished buffer; it switches at the next frame, and the old front
     * buffer is free to draw into once that frame has gone out */
    xSemaphoreTake(T.vsync, 0);
    esp_lcd_panel_draw_bitmap(T.lcd.panel, 0, 0, PANEL_W, PANEL_H, fb);
    xSemaphoreTake(T.vsync, pdMS_TO_TICKS(40));
    T.back ^= 1;
}

bool hal_touch(int *x, int *y, void *user)
{
    (void)user;
    if (!T.touch) return false;
    esp_lcd_touch_point_data_t p[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(T.touch);
    if (esp_lcd_touch_get_data(T.touch, p, &cnt, 1) != ESP_OK || !cnt) return false;
    /* the inverse of the picture's turn in rotate_area() */
#ifndef CATALYST_ROTATE_270
    *x = HAL_W - 1 - p[0].y;
    *y = p[0].x;
#else
    *x = p[0].y;
    *y = HAL_H - 1 - p[0].x;
#endif
    return true;
}

/* ---- the compositor's accelerated primitives ---- */

static void ppa_copy565(uint16_t *dst, int ds, const uint16_t *src, int ss, int w, int h)
{
    if (w * h < 4096 || ds != HAL_W || ss != HAL_W) {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * ds, src + (size_t)y * ss, (size_t)w * 2);
        return;
    }
    /* the rectangle's offset inside the full-frame buffers */
    size_t off_s = (size_t)(src - T.content), off_d = (size_t)(dst - T.out);
    ppa_srm_oper_config_t op = {
        .in = { .buffer = T.content, .pic_w = HAL_W, .pic_h = HAL_H, .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                .block_offset_x = (uint32_t)(off_s % HAL_W), .block_offset_y = (uint32_t)(off_s / HAL_W),
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = T.out, .buffer_size = HAL_W * HAL_H * 2, .pic_w = HAL_W, .pic_h = HAL_H,
                 .block_offset_x = (uint32_t)(off_d % HAL_W), .block_offset_y = (uint32_t)(off_d / HAL_W),
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(T.ppa_srm, &op);
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
    if (!T.ppa_blend_ok || w * h < 4096) {
        cpu_blend(d, ds, u, us, o, os, w, h);
        return;
    }
    size_t off = (size_t)(d - T.out);
    uint32_t bx = (uint32_t)(off % HAL_W), by = (uint32_t)(off / HAL_W);
    ppa_blend_oper_config_t op = {
        .in_bg = { .buffer = T.out, .pic_w = HAL_W, .pic_h = HAL_H, .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                   .block_offset_x = bx, .block_offset_y = by, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = T.ink, .pic_w = HAL_W, .pic_h = HAL_H, .block_w = (uint32_t)w, .block_h = (uint32_t)h,
                   .block_offset_x = bx, .block_offset_y = by, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888 },
        .out = { .buffer = T.out, .buffer_size = HAL_W * HAL_H * 2, .pic_w = HAL_W, .pic_h = HAL_H,
                 .block_offset_x = bx, .block_offset_y = by, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    (void)u; (void)us; (void)os; (void)ds;
    ppa_do_blend(T.ppa_blend, &op);
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

static const bz_gfx_ops_t OPS = { .copy565 = ppa_copy565, .blend = ppa_blend };

void hal_display(hal_display_t *o)
{
    o->content = T.content;
    o->ink = T.ink;
    o->out = T.out;
    o->ops = &OPS;
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
        ppa_do_scale_rotate_mirror(T.ppa_srm, &op);
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
    if (ppa_do_scale_rotate_mirror(T.ppa_srm, &op) != ESP_OK) {
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
    /* listen-only: the controller never acknowledges or sends, so a tap can't disturb the robot */
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT_V2(0, CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 512;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
    if (bitrate == 500000) t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install_v2(&g, &t, &f, &K.h) != ESP_OK) return false;
    if (twai_start_v2(K.h) != ESP_OK) {
        twai_driver_uninstall_v2(K.h);
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

/* ------------------------------------------------------------------ init */

bool hal_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(bsp_i2c_init());
    T.i2c = bsp_i2c_get_handle();
    power_init();
    ina226_init();
    rtc_init();
    imu_init();

    /* the three full-frame buffers the renderer works in, in PSRAM */
    T.content = psram_aligned(HAL_W * HAL_H * 2);
    T.ink = psram_aligned(HAL_W * HAL_H * 4);
    T.out = psram_aligned(HAL_W * HAL_H * 2);
    if (!T.content || !T.ink || !T.out) return false;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    ppa_register_client(&pc, &T.ppa_srm);
    pc.oper_type = PPA_OPERATION_BLEND;
    ppa_register_client(&pc, &T.ppa_blend);
    ppa_blend_selftest();

    display_init();
    audio_init();
    T.sd = bsp_sdcard_mount() == ESP_OK;
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &T.tsens) == ESP_OK) temperature_sensor_enable(T.tsens);
    hal_net_init();
    ESP_LOGI(TAG, "ready: panel %s, sd %s, imu %s", T.panel, T.sd ? "mounted" : "none", T.imu ? "ok" : "missing");
    return true;
}
