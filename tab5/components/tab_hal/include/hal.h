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
    bool async_present;  /* hal_present returns before the panel has read the frame: compose alternately
                            into two buffers (bz_ui_config_t.async_present) */
    const bz_slide_ops_t *slide; /* page slides as row shifts in the portrait buffer */
} hal_display_t;

/* Start-up comes in three steps, so the boot animation is on screen for all but the first:
 *   hal_init()      power, I2C, the panel and the frame buffers: enough to show something;
 *   hal_start()     everything else — speaker, microSD, sensors, Wi-Fi — while the animation plays;
 *   hal_settle()    once the UI is up: the USB-A port's 5 V and the tether, last, so the tablet never
 *                   switches every rail on at the same moment. */
bool hal_init(void);
void hal_start(void);
void hal_settle(void);
void hal_display(hal_display_t *out);
void hal_present(const bz_present_t *areas, int n, void *user);
bool hal_touch(int *x, int *y, void *user);
void hal_set_brightness(float v01);
/* Which way up: false the default landscape, true turned 180°. The caller redraws the whole screen after. */
void hal_set_flip(bool flip);
void hal_loop_note(double work_s, bool busy); /* PROFILING: one UI loop's bz_ui_frame time */
void hal_loop_prof(double out[4]);
void hal_loop_hist(int out[5]); /* PROFILING: busy loops under 5, 10, 16.7, 25 ms, more */ /* PROFILING: busy loops, their mean and max work (s), loops idle */
void hal_frame_gaps(int out[4]); /* PROFILING: frame gaps of 1, 2, 3, more vsyncs since the last call */
void hal_present_prof(double out[6]); /* PROFILING: s per stage (pick, catch-up, scroll, rotate, hand-over), count */
bool hal_flip(void);
/* The development console on the USB-C port (tools/tab5_dev.py): screenshots and injected touches. */
void hal_dev_init(void);
const uint16_t *hal_front_fb(void); /* the panel's current picture, portrait 720x1280 */
bool hal_dev_touch(int *x, int *y); /* an injected finger, if one is down */
/* Commands the console doesn't know go to the UI ("open <app>", "page <n>", "close"): true if handled.
 * Called from the console's task; the handler must hand the work to the UI's own loop. */
void hal_dev_set_handler(bool (*fn)(const char *line));
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
/* A short tone on the speaker: detent ticks, chimes. Non-blocking. Mixed over any speech playing. */
void hal_tone(float hz, int ms, float volume01);
void hal_set_volume(float v01);

/* Speech out: 16-bit mono PCM streamed to the speaker, at a rate that divides 48 kHz (8, 12, 16, 24, 48
 * kHz). start → write… → end, and it plays out by itself (hal_play_busy() turns false); stop cuts it off
 * at once. Playback begins once ~170 ms is queued (or at end), so a stream that arrives in bursts doesn't
 * stutter. hal_play_write() blocks up to timeout_ms while the queue (~6 s) is full; returns samples taken.
 * One stream at a time, one writer. */
bool hal_play_start(int rate_hz);
int hal_play_write(const int16_t *pcm, int n, int timeout_ms);
void hal_play_end(void);
void hal_play_stop(void);
bool hal_play_busy(void);
float hal_play_level(void);               /* loudness of what the speaker is playing now, 0..1 */

/* The microphones: 16 kHz mono, for speech. Off until started; nothing listens unless asked. Blocking reads
 * (~20 ms of audio each): call from a worker. hal_mic_read returns samples (0 when off, < 0 on error). */
bool hal_mic_start(void);
int hal_mic_read(int16_t *out, int max);
void hal_mic_stop(void);
bool hal_mic_on(void);
float hal_mic_level(void);                /* loudness of the last read, 0..1 */
void hal_mic_channel(int ch);             /* -1 the louder (default), 0 left, 1 right, 2 both mixed */

/* The wake word, on the tablet (ESP-SR WakeNet on the Tab5). hal_wake_ready loads the model on first call
 * (slow: from a worker) and says which phrase it listens for; false when there is none (tap to talk only).
 * Feed it what hal_mic_read gives: true the moment the phrase is heard. */
bool hal_wake_ready(const char **word);
void hal_wake_reset(void);
bool hal_wake_feed(const int16_t *pcm, int n);

/* ---- camera ---- */
bool hal_camera_start(void);
/* The newest frame as RGB565, or NULL. Valid until the next call. */
const uint16_t *hal_camera_frame(int *w, int *h);
void hal_camera_stop(void);
/* Encodes the current frame as JPEG (hardware codec on the P4) to `path`. */
bool hal_camera_snapshot(const char *path);
/* The video plane: the camera's picture laid onto the panel by the HAL itself, centred in the landscape box
 * (bx, by, bw, bh), at the preview's size (the rect it takes comes back). LVGL draws nothing there that
 * shows: every present lays the newest frame over it. hal_camera_gen() counts frames, so the app knows when
 * to ask for a present; hal_camera_freeze() holds the picture. */
bool hal_camera_plane(int bx, int by, int bw, int bh, int *x, int *y, int *w, int *h);
void hal_camera_plane_off(void);
void hal_camera_freeze(bool freeze);
uint32_t hal_camera_gen(void);
/* A clip: the camera's frames through the P4's hardware H.264 encoder into `path` as an Annex-B
 * elementary stream (plays in VLC/ffplay; `ffmpeg -i x.h264 -c copy x.mp4` boxes it). The camera must
 * be running. Stops by itself at `max_s`. */
bool hal_clip_start(const char *path, double max_s);
double hal_clip_stop(void);               /* seconds recorded */
bool hal_clip_active(double *seconds);

/* ---- time ---- */
bool hal_rtc_get(struct tm *out);
void hal_rtc_set(const struct tm *t);
double hal_seconds(void);                 /* monotonic */

/* ---- storage ---- */
const char *hal_sd_root(void);            /* "/sdcard" when mounted, else NULL */
bool hal_sd_space(uint64_t *total, uint64_t *free_bytes);
bool hal_kv_get(const char *key, char *buf, size_t n);
void hal_kv_set(const char *key, const char *value);

/* ---- pictures (hal_tab5_os.c) ---- */
/* A decoded picture: RGB565 little-endian, w×h, rows packed (stride w*2), in PSRAM. src_w/src_h are the
 * file's own size before scaling. */
typedef struct {
    uint16_t *px;
    int w, h, src_w, src_h;
} hal_picture_t;
/* Decodes a baseline JPEG file with the P4's hardware decoder and scales it down to fit max_w×max_h (never
 * up). Blocking and heavy: call it from a worker. false, with err saying why, when it can't (progressive
 * JPEGs, too large for free PSRAM, unreadable). */
bool hal_jpeg_load(const char *path, int max_w, int max_h, hal_picture_t *out, char *err, size_t errn);
void hal_picture_free(hal_picture_t *p);

/* ---- network ---- */
typedef enum { HAL_LINK_NONE, HAL_LINK_WIFI, HAL_LINK_USB, HAL_LINK_SIM } hal_link_t;
typedef struct {
    hal_link_t link;     /* the way to the robot: USB while the tether is up, else Wi-Fi */
    bool up;             /* Wi-Fi associated with an address */
    char ssid[33];
    int rssi;
    char ip[16];         /* Wi-Fi's */
} hal_net_t;
void hal_net(hal_net_t *out);
void hal_net_report(char *out, size_t n, const char *host); /* dev console: addresses, DNS, a lookup */
void hal_wifi_join(const char *ssid, const char *pass);
void hal_wifi_rejoin(void);
/* The Wi-Fi watchdog restarts the tablet when the C6 stops answering (its reset needs a host restart). Before
 * it does, fn runs (the UI notes where it is with hal_resume_note, from the watchdog's task: only a note);
 * after, hal_resume_take gives it back once. */
/* a restart the firmware asks for: not counted as a failed start (no safe mode for it) */
void hal_restart_planned(const char *why);
void hal_restart_hook(void (*fn)(void));
void hal_c6_restart_test(void); /* dev console: the watchdog's restart, now */
void hal_resume_note(int page, const char *app);
bool hal_resume_take(int *page, char *app, size_t n);
/* the C6 co-processor's firmware from a file on the card; restart after a success */
bool hal_c6_ota(const char *path, void (*progress)(size_t done, size_t total), char *err, size_t errn);
typedef struct { char ssid[33]; int rssi; bool secure; } hal_ap_t;
int hal_wifi_scan(hal_ap_t *out, int max);  /* blocking, a couple of seconds */

/* USB tether on the USB-A host port (the USB-C port only takes power): Systemcore's own USB-C
 * gadget through an A-to-C cable (CDC-NCM/ECM or RNDIS; Systemcore answers at 172.26.0.1), or any
 * USB-Ethernet dongle (CDC-ECM, or the ASIX/Realtek chips esp-iot-solution drives) into the robot's
 * radio or switch. DHCP first; with no answer in a few seconds, the fallback address. Routes by
 * subnet: the robot over the tether, everything else (the PC, the internet) over Wi-Fi. */
typedef struct {
    bool present;        /* a USB network adapter is enumerated */
    bool up;             /* link up with an IPv4 address */
    bool dhcp;           /* the address came from DHCP (else the fallback) */
    char kind[16];       /* "ncm", "ecm", "rndis", "ax88179", …; "" with nothing attached */
    char ip[16], gw[16], mask[16];
    uint8_t mac[6];
    uint64_t rx_bytes, tx_bytes;
    int mbps;            /* 0 when the adapter doesn't say */
} hal_tether_t;
void hal_tether(hal_tether_t *out);
/* The static address used when DHCP doesn't answer, e.g. "10.58.5.60" / "255.255.255.0" for team 5805. */
void hal_tether_fallback(const char *ip, const char *mask);

/* mDNS browse (e.g. "_catalyst-link", "_tcp"): blocking up to timeout_ms. */
typedef struct { char name[48]; char host[64]; char ip[16]; int port; } hal_service_t;
int hal_mdns_browse(const char *service, const char *proto, hal_service_t *out, int max, int timeout_ms);

/* HTTP/1.1 client, http:// and https:// (TLS verified against the platform's CA bundle), with the
 * body streamed as it arrives — Server-Sent Events for the assistant, JSON for everything else.
 * Blocking: call it from a worker thread (hal_thread), never the UI thread. */
typedef struct hal_http hal_http_t;
typedef struct {
    const char *method;          /* "GET" when NULL */
    const char *url;
    const char *headers;         /* extra request headers, each "Name: value\r\n"; may be NULL */
    const char *body;            /* may be NULL */
    size_t body_len;
    int timeout_ms;              /* connect, and each read; 0: 10 s */
} hal_http_req_t;
/* Sends the request and reads the response head. NULL when it never got a response (err says why). */
hal_http_t *hal_http_open(const hal_http_req_t *req, int *status, char *err, size_t errn);
/* The next bytes of the body, chunked encoding already removed: > 0 bytes, 0 at the end, < 0 error. */
int hal_http_read(hal_http_t *h, char *buf, int max);
void hal_http_close(hal_http_t *h);
/* The response's Last-Modified header ("" when it sent none), to send back as If-Modified-Since. */
const char *hal_http_last_modified(const hal_http_t *h);

/* One whole response into `out` (NUL-terminated, truncated to max-1). Returns the status, or -1. */
static inline int hal_http_fetch(const hal_http_req_t *req, char *out, int max, int *len)
{
    int status = -1, n = 0;
    char err[64];
    hal_http_t *h = hal_http_open(req, &status, err, sizeof err);
    if (!h) {
        if (len) *len = 0;
        if (max > 0) out[0] = 0;
        return -1;
    }
    for (;;) {
        if (n >= max - 1) break;
        int r = hal_http_read(h, out + n, max - 1 - n);
        if (r <= 0) {
            if (r < 0) status = -1;
            break;
        }
        n += r;
    }
    hal_http_close(h);
    out[n] = 0;
    if (len) *len = n;
    return status;
}

/* A worker thread (core 0 on the tablet: core 1 renders). stack in bytes; TLS wants >= 12 KB. */
bool hal_thread(const char *name, void *(*fn)(void *), void *arg, int stack);
/* hal_thread, but the stack only ever in internal RAM (a thread that reads flash or writes NVS): false when it
 * doesn't fit */
bool hal_thread_internal(const char *name, void *(*fn)(void *), void *arg, int stack);

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

/* ---- NFC: M5Stack's Unit RFID 2 (WS1850S, MFRC522 registers, I2C 0x28) on Grove Port A ----
 * The Tab5 has no NFC reader of its own. The unit is probed once, a few seconds after start-up; absent,
 * nothing runs. Present, a low-priority task on core 0 looks for an ISO 14443A card every ~300 ms. Port A is
 * also the CAN tap's: hal_can_start() takes the port from the reader until the next start-up. */
bool hal_nfc_present(void);
/* The card in the field now: its UID as upper-case hex ("04A1B2C3D4E5F6") into `uid`, true; false with none
 * (or no reader). Any thread; no debouncing (a card at the edge of the field comes and goes). */
bool hal_nfc_card(char *uid, size_t n);

/* ---- system ---- */
typedef struct {
    uint32_t psram_free, sram_free;
    float cpu[2];         /* HP core load 0..1 */
    float temp_c;         /* the P4's own sensor */
    char chip[32];
} hal_sys_t;
void hal_sys(hal_sys_t *out);
void hal_power_off(void);

/* ---- the start-up record ----
 * Where each start got to survives a reset, so a start that dies can be told apart from a clean one and
 * the next one can say what happened (on the boot screen, in the log, in <sd>/catalyst-boot.txt) and
 * come up in safe mode. */
typedef struct {
    bool failed;          /* the previous start never settled */
    int fails;            /* consecutive starts that didn't */
    char reason[24];      /* how it ended: "crash", "watchdog", "brownout", "restart" */
    char stage[24];       /* the last stage it reached */
    char detail[72];      /* from the core dump, when there is one: task and address */
} hal_boot_t;
void hal_boot_stage(const char *stage);   /* a breadcrumb */
void hal_boot_prev(hal_boot_t *out);      /* how the previous start went */
void hal_boot_ok(void);                   /* this start has run long enough to count as good */
void hal_boot_watch(void (*fn)(const char *stage)); /* called with each stage as it's reached */

#ifdef __cplusplus
}
#endif
