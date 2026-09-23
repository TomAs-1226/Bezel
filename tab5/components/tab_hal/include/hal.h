/* hal — everything Catalyst Tab needs from the hardware, as one small C interface.
 *
 * hal_tab5.c implements it on the M5Stack Tab5 (ESP32-P4 + ESP32-C6); sim/hal_sim.c implements it on a
 * laptop with synthetic sensors, so the UI and every tool can be exercised without the tablet. The UI
 * never includes an ESP-IDF header: if a tool needs the hardware, the need is spelled here. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "bz_comp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_W 1280
#define HAL_H 720

/* ---- display and touch: the buffers bz_ui renders into, and how a frame reaches the glass ---- */
typedef struct {
    uint16_t *content;   /* HAL_W*HAL_H RGB565 */
    uint32_t *ink;       /* HAL_W*HAL_H ARGB8888 */
    uint16_t *out;       /* HAL_W*HAL_H RGB565 */
    const bz_gfx_ops_t *ops;
} hal_display_t;

bool hal_init(void);
void hal_display(hal_display_t *out);
void hal_present(const bz_area_t *areas, int n, void *user);
bool hal_touch(int *x, int *y, void *user);
void hal_set_brightness(float v01);
const char *hal_panel_name(void);        /* "ILI9881C", "ST7123", "ST7121", "simulator" */

/* ---- sensors ---- */
typedef struct {
    float ax, ay, az;    /* gravity's direction in g, in the landscape screen frame: x right, y down,
                            z out of the glass. Held upright: (0, 1, 0); lying face up: (0, 0, -1) */
    float gx, gy, gz;    /* deg/s */
    bool ok;
} hal_imu_t;
bool hal_imu(hal_imu_t *out);

typedef struct {
    float volts, amps;   /* amps > 0 charging */
    int percent;         /* from voltage: there is no fuel gauge */
    bool charging, external; /* external: powered from USB or the robot's 12 V */
    bool ok;
} hal_battery_t;
bool hal_battery(hal_battery_t *out);

/* ---- audio ---- */
/* Mono microphone samples at *rate Hz, whatever is buffered (up to max). Starts the mics on first use. */
int hal_mic_read(int16_t *buf, int max, int *rate);
void hal_mic_stop(void);
/* A short tone on the speaker: detent ticks, chimes. Non-blocking. */
void hal_tone(float hz, int ms, float volume01);
void hal_set_volume(float v01);

/* ---- camera ---- */
bool hal_camera_start(void);
/* The newest frame as RGB565, or NULL. Valid until the next call. */
const uint16_t *hal_camera_frame(int *w, int *h);
void hal_camera_stop(void);
/* Encodes the current frame as JPEG (hardware codec on the P4) to `path`. */
bool hal_camera_snapshot(const char *path);

/* ---- time ---- */
bool hal_rtc_get(struct tm *out);
void hal_rtc_set(const struct tm *t);
double hal_seconds(void);                 /* monotonic */

/* ---- storage ---- */
const char *hal_sd_root(void);            /* "/sdcard" when mounted, else NULL */
bool hal_kv_get(const char *key, char *buf, size_t n);
void hal_kv_set(const char *key, const char *value);

/* ---- network ---- */
typedef enum { HAL_LINK_NONE, HAL_LINK_WIFI, HAL_LINK_USB, HAL_LINK_SIM } hal_link_t;
typedef struct {
    hal_link_t link;
    bool up;
    char ssid[33];
    int rssi;
    char ip[16];
} hal_net_t;
void hal_net(hal_net_t *out);
void hal_wifi_join(const char *ssid, const char *pass);
typedef struct { char ssid[33]; int rssi; bool secure; } hal_ap_t;
int hal_wifi_scan(hal_ap_t *out, int max);  /* blocking, a couple of seconds */

/* ---- CAN tap (TWAI, listen-only, through a Grove CAN transceiver on Port A) ---- */
typedef struct {
    uint32_t id;
    uint8_t len;
    bool ext, rtr;
    uint8_t data[8];
    int64_t t_us;
} hal_can_frame_t;
typedef struct {
    uint32_t rx, errors, missed;
    int state;            /* 0 stopped, 1 listening, 2 bus error / passive, 3 no transceiver */
} hal_can_stats_t;
bool hal_can_start(int bitrate);
int hal_can_read(hal_can_frame_t *out, int max);
void hal_can_stats(hal_can_stats_t *out);
void hal_can_stop(void);

/* ---- system ---- */
typedef struct {
    uint32_t psram_free, sram_free;
    float cpu[2];         /* HP core load 0..1 */
    float temp_c;         /* the P4's own sensor */
    char chip[32];
} hal_sys_t;
void hal_sys(hal_sys_t *out);
void hal_power_off(void);

#ifdef __cplusplus
}
#endif
