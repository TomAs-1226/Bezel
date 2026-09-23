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
    uint32_t lane_mbps;   /* MIPI lane rate: 1000, or 965 on the ST7121 */
    double refresh_hz;    /* from the DPI timing actually programmed */
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

/* ---- the panel at 60 Hz ----
 *
 * The BSP fixes each panel's DPI timing inside bsp_display_new_with_handles(), and the pixel clock it asks
 * for isn't the one it gets: the DPI clock is PLL_F240M over an integer divider that esp_lcd rounds down,
 * so the BSP's "60 MHz" is 60 and its "70 MHz" is 80. As shipped that makes the ILI9881C
 * 60e6 / (940 × 1324) = 48.2 Hz, the ST7123 80e6 / (802 × 1510) = 66.1 Hz and the ST7121
 * 80e6 / (802 × 1524) = 65.5 Hz.
 *
 * Catalyst Tab runs all three at ~60.5 Hz: an 80 MHz pixel clock (240 / 3, exact), the BSP's vertical
 * timing untouched, and the horizontal back porch widened until the frame is 60.5 Hz or just over. A
 * longer line than the BSP's is gentler on the panels' source drivers, never harsher, and 60.5 Hz keeps
 * the scan-out's PSRAM reads (~112 MB/s) under the ST712x's shipped 66 Hz. RGB565 at 80 MHz is
 * 1280 Mbit/s over two lanes: 64 % of 2 × 1000 Mbit/s, 66 % of the ST7121's 2 × 965.
 *
 * The timing is changed on its way into esp_lcd: tab_hal links with --wrap=esp_lcd_new_panel_dpi (its
 * CMakeLists.txt), so the BSP's panel drivers call __wrap_esp_lcd_new_panel_dpi() below, which adjusts a
 * copy of the config and hands it to the real function; the BSP's init sequences (private to it) stay the
 * BSP's. UNVERIFIED on all three panels: that each takes the wider line and runs at the logged rate. A
 * panel that doesn't shows a rolling or blank picture; build with CATALYST_BSP_PANEL_TIMING to go back. */
#define PANEL_TARGET_HZ 60.5
#define DPI_SRC_MHZ 240 /* PLL_F240M, esp_lcd's default DPI clock source on the P4 */
#define DPI_MHZ 80

esp_err_t __real_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dpi_panel_config_t *cfg,
                                       esp_lcd_panel_handle_t *ret);
esp_err_t __wrap_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dpi_panel_config_t *cfg,
                                       esp_lcd_panel_handle_t *ret);

static uint32_t dpi_htotal(const esp_lcd_video_timing_t *t)
{
    return t->h_size + t->hsync_back_porch + t->hsync_pulse_width + t->hsync_front_porch;
}

static uint32_t dpi_vtotal(const esp_lcd_video_timing_t *t)
{
    return t->v_size + t->vsync_back_porch + t->vsync_pulse_width + t->vsync_front_porch;
}

esp_err_t __wrap_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dpi_panel_config_t *cfg,
                                       esp_lcd_panel_handle_t *ret)
{
    esp_lcd_dpi_panel_config_t c = *cfg;
    esp_lcd_video_timing_t *t = &c.video_timing;
    uint32_t div = DPI_SRC_MHZ / (cfg->dpi_clock_freq_mhz ? cfg->dpi_clock_freq_mhz : DPI_SRC_MHZ);
    double bsp_mhz = (double)DPI_SRC_MHZ / (div ? div : 1);
    double bsp_hz = bsp_mhz * 1e6 / ((double)dpi_htotal(t) * dpi_vtotal(t));
    T.refresh_hz = bsp_hz;
#ifndef CATALYST_BSP_PANEL_TIMING
    uint32_t vtotal = dpi_vtotal(t);
    uint32_t htotal = (uint32_t)(DPI_MHZ * 1e6 / (PANEL_TARGET_HZ * vtotal)); /* rounded down: at or over 60.5 */
    uint32_t hfixed = t->h_size + t->hsync_pulse_width + t->hsync_front_porch;
    /* RGB565 over the two lanes, with a fifth kept for blanking packets and protocol overhead */
    bool lanes_ok = DPI_MHZ * 16 <= T.lane_mbps * 2 * 4 / 5;
    bool src_ok = cfg->dpi_clk_src == 0 || cfg->dpi_clk_src == MIPI_DSI_DPI_CLK_SRC_PLL_F240M;
    if (lanes_ok && src_ok && htotal >= hfixed + t->hsync_back_porch) {
        c.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_PLL_F240M;
        c.dpi_clock_freq_mhz = DPI_MHZ;
        t->hsync_back_porch = htotal - hfixed;
        T.refresh_hz = DPI_MHZ * 1e6 / ((double)dpi_htotal(t) * vtotal);
    }
#endif
    ESP_LOGI(TAG, "panel %s: BSP timing %.0f MHz (asked %u), %u x %u -> %.1f Hz; running %u MHz, %u x %u -> %.2f Hz",
             T.panel, bsp_mhz, (unsigned)cfg->dpi_clock_freq_mhz, (unsigned)dpi_htotal(&cfg->video_timing),
             (unsigned)dpi_vtotal(&cfg->video_timing), bsp_hz, (unsigned)(DPI_SRC_MHZ / (DPI_SRC_MHZ / c.dpi_clock_freq_mhz)),
             (unsigned)dpi_htotal(t), (unsigned)dpi_vtotal(t), T.refresh_hz);
    return __real_esp_lcd_new_panel_dpi(bus, &c, ret);
}

/* ---- presenting: the finished landscape areas, turned into the portrait back buffer ----
 *
 * hal_present() is asynchronous, so the UI core composes frame N+1 while the PPA turns frame N:
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
    uint32_t overflow;
} P;

static bool IRAM_ATTR on_rotated(ppa_client_handle_t client, ppa_event_data_t *e, void *user)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(P.done, &woken);
    return woken == pdTRUE;
}

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
#ifndef CATALYST_ROTATE_270
    /* 90° counter-clockwise: landscape (x, y) lands at portrait (y, W-1-x) */
    op.out.block_offset_x = (uint32_t)a->y1;
    op.out.block_offset_y = (uint32_t)(HAL_W - 1 - a->x2);
#else
    op.out.block_offset_x = (uint32_t)(HAL_H - 1 - a->y2);
    op.out.block_offset_y = (uint32_t)a->x1;
#endif
    if (async && ppa_do_scale_rotate_mirror(P.ppa, &op) == ESP_OK) return true;
    /* no room in the queue: blocking, which also waits out everything queued before it */
    op.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_scale_rotate_mirror(T.ppa_srm, &op);
    return false;
}

static bool covers(const bz_area_t *o, const bz_area_t *a)
{
    return o->x1 <= a->x1 && o->y1 <= a->y1 && o->x2 >= a->x2 && o->y2 >= a->y2;
}

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
    bsp_display_backlight_on();
    bsp_touch_new(NULL, &T.touch);
    ESP_LOGI(TAG, "panel %s at %.2f Hz, frame buffers %p %p", T.panel, T.refresh_hz, T.fb[0], T.fb[1]);
}

const char *hal_panel_name(void) { return T.panel; }

void hal_set_brightness(float v)
{
    bsp_display_brightness_set((int)(v * 100 + 0.5f));
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
    span_add(T.content, HAL_W * HAL_H * 2);
    span_add(T.ink, HAL_W * HAL_H * 4);
    span_add(T.out, HAL_W * HAL_H * 2);
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    ppa_register_client(&pc, &T.ppa_srm);
    ppa_register_client(&pc, &T.ppa_cam);
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
