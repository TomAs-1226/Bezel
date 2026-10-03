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
/* HAL_W/HAL_H are the buffers': the panel is 720x1280 and the landscape picture is turned onto it, so
 * both frames are the same HAL_W*HAL_H pixels whichever way round the interface is laid out.
 *
 * The frame the interface is laid out IN is hal_frame_w/hal_frame_h: HAL_W x HAL_H normally, and the
 * panel's own 720 x 1280 in phone mode. It is chosen in hal_init() from the owner's setting and never
 * changes again, so layout code may read it as a constant. Phone mode is the owner's choice of frame;
 * which way up that frame is held is hal_set_flip()'s, and the two are independent. */
extern int hal_frame_w, hal_frame_h;
bool hal_phone(void);           /* the frame is the portrait one */
/* Phone mode for the NEXT start, saved; this start keeps the frame it was built with. The caller
 * restarts the tablet when it wants the change now. */
void hal_phone_next(bool on);

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
void hal_loop_note(double work_s, bool busy); /* perf: one UI loop's bz_ui_frame time */
void hal_scroll_dual(bool on); /* the tablet: a scroll's shift split between two DMA2D copiers (dev "dual") */
void hal_catchup_trace(int n); /* dev "cudbg N" */
void hal_cpu_rot_px(int n);     /* dev "cpurot N" */
void hal_slide_mask(bool on); /* the tablet: a slide holds chrome by its pixels (1) or its rectangles (dev "mask") */
/* dev "psram": [0] bytes in the big blocks taken once at boot, [1] PSRAM free, [2] its largest free
 * block, [3] of [0], the display's frame buffers. The program's own .text/.rodata are in none of these:
 * XIP from PSRAM puts them outside the heap, which is why PSRAM's total is not the 32 MB on the package. */
void hal_mem_psram(size_t out[4]);
/* Free PSRAM alone, cheaply: the heap keeps that total running. hal_mem_psram's largest-free-block walks
 * every free block with the heap's lock held, so anything on the frame path asks for this instead. */
size_t hal_psram_free(void);
/* Bytes the PSRAM heap has handed out, and in how many blocks (what "psram" subtracts its named pools from). */
size_t hal_psram_allocated(size_t *blocks);
/* Panel scan-out underruns since boot: the frames that went cyan because the DPI's FIFO could not be
 * refilled from PSRAM in time. Counted off the DSI driver's own log line, which is the only notice of one
 * that exists. Any number above zero is a PSRAM bandwidth problem, not a memory-capacity problem. */
uint32_t hal_underruns(void);
void hal_mem_psram_lines(void (*line)(const char *)); /* those blocks, one printable line each */
void hal_loop_prof(double out[4]); /* perf: busy loops, their mean and max work (s), loops idle */
void hal_loop_hist(int out[5]);    /* perf: busy loops under 5, 10, 16.7, 25 ms, more */
void hal_frame_gaps(int out[4]); /* perf: frame gaps of 1, 2, 3, more vsyncs since the last call */
void hal_present_prof(double out[6]); /* perf: s per stage (pick, catch-up, scroll, rotate, hand-over), count */
/* perf: frames a slide or a sheet composed by DMA (no rotation): count, total s, the worst one, and of
 * the total the part that was the chrome's rectangles */
void hal_slide_prof(double out[4]);
bool hal_flip(void);
/* The development console on the USB-C port (tools/tab5_dev.py): screenshots and injected touches. */
void hal_dev_init(void);
const uint16_t *hal_front_fb(void); /* the panel's current picture, portrait 720x1280 */
bool hal_front_fb_copy(uint16_t *dst); /* that picture copied whole, the presents held meanwhile */
/* The same picture read in place, with the presents held until it is released -- for a reader that would
 * rather freeze the screen for a moment than find 1.8 MB of contiguous PSRAM, which a tablet short of it
 * cannot. Every hold must be released. */
const uint16_t *hal_front_fb_hold(void);
void hal_front_fb_release(void);
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
float hal_volume(void);                     /* the speaker's level as last set, 0..1 */

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
/* A streaming MP3 decoder, for speech fetched as MP3 (a tenth of PCM's bytes over the Wi-Fi co-processor).
 * feed() takes the stream as it arrives, any number of bytes (eos on the last), and hands each decoded run to
 * sink as 16-bit mono (stereo mixed down) with its sample rate. false: undecodable, or the sink said stop.
 * open() returns NULL where there is no decoder (the simulator): fetch PCM instead. */
typedef struct hal_mp3 hal_mp3_t;
typedef bool (*hal_pcm_sink_t)(const int16_t *mono, int n, int rate, void *user);
hal_mp3_t *hal_mp3_open(void);
bool hal_mp3_feed(hal_mp3_t *d, const uint8_t *in, int n, bool eos, hal_pcm_sink_t sink, void *user);
void hal_mp3_close(hal_mp3_t *d);
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
    /* When `up` is false, why. The tablet spent a night saying "wi-fi off" while it was in fact looking
     * for a network that was not in the building -- 1397 join attempts, every one answered "no AP
     * found" -- and that reads to anyone holding it as Wi-Fi dropping over and over. It is the opposite:
     * nothing ever connected, and it never would have. `want` is the network it is trying to join, ""
     * when none is saved, and `absent` is true once a join has failed because that network could not be
     * found rather than because it was refused. */
    char want[33];
    bool absent;
} hal_net_t;
void hal_net(hal_net_t *out);
void hal_net_report(char *out, size_t n, const char *host); /* dev console: addresses, DNS, a lookup */
/* dev console: which slave the C6 runs, the station settings it actually holds, and the last drop's reason */
void hal_wifi_diag(char *out, size_t n);
void hal_wifi_join(const char *ssid, const char *pass);
void hal_wifi_rejoin(void);

/* The networks the tablet remembers (wifi_saved.h): every one it has joined, with its password, 16 at most
 * (the one used longest ago makes room), in the tablet's own kv so they survive a reflash and whatever the
 * C6 keeps. A network is saved when a join to it gets an address. With the link down the tablet scans and
 * joins the loudest saved network in range, keeping the last-used when the two are comparable; it never
 * leaves a network that works for a louder one. Passwords are never returned, shown or logged. */
#define HAL_WIFI_SAVED 16
/* The saved names, the latest used first; the count. */
int hal_wifi_saved(char names[][33], int max);
bool hal_wifi_is_saved(const char *ssid);
unsigned hal_wifi_saved_gen(void);            /* changes with the set: a list redraws on it */
/* Joins a saved network with the password it was saved with; false when it isn't saved. Returns at once:
 * the radio is asked on the network task. */
bool hal_wifi_join_saved(const char *ssid);
/* Adds a network without joining it (KEYS.ENV's WIFI_SSID/WIFI_PASS). Writes kv: UI thread or a worker with
 * an internal stack. */
void hal_wifi_save(const char *ssid, const char *pass);
/* Forgets one (kv: as above). The network in use is left, and another saved one in range joined. */
void hal_wifi_forget(const char *ssid);

/* Something on this network answered for a host we asked for and its certificate did not match: a
 * school filter, a captive portal, a proxy. The link is fine; the internet is not what it claims to
 * be. Cleared when the tablet gets a new address. */
bool hal_net_filtered(void);

/* The DNS servers to use instead of the ones DHCP hands out. Both "" (or NULL) goes back to DHCP's.
 * Kept across boots and re-applied on every lease, because DHCP overwrites them each time.
 *
 * It is worth knowing what this can and cannot do: it changes which resolver is asked, so it moves
 * past filtering done AT the resolver. A network that redirects outgoing port 53 to its own server,
 * or that blocks by address or by SNI at the firewall, is unaffected -- the tablet will simply get
 * the same answer from somewhere else, or none. Try it and look; do not assume it worked. */
void hal_wifi_dns(const char *primary, const char *secondary);
/* What is set, "" for DHCP's. Returns true when the owner has chosen. */
bool hal_wifi_dns_get(char *primary, size_t pn, char *secondary, size_t sn);
void hal_antenna(bool external); /* the RF switch on E1.P0: false internal, true the MMCX socket */
int hal_antenna_get(void);       /* which one, read back off the expander; -1 it didn't answer */
/* The Wi-Fi watchdog restarts the tablet when the C6 stops answering (its reset needs a host restart). Before
 * it does, fn runs (the UI notes where it is with hal_resume_note, from the watchdog's task: only a note);
 * after, hal_resume_take gives it back once. */
/* a restart the firmware asks for: not counted as a failed start (no safe mode for it) */
void hal_restart_planned(const char *why);
void hal_restart_hook(void (*fn)(void));
void hal_c6_restart_test(void); /* dev console: the watchdog's restart, now */
void hal_c6_hold(bool on);      /* dev console "wdhold 1": the watchdog notes a hang but doesn't reset it */
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

/* ---- BLE presence watch ----
 * A doorbell, never a wire. The only thing this carries is "something advertising this 4-byte id is
 * within radio range", which link.c turns into "the PC is switched on and its Catalyst Link is
 * running". No connection is ever made, no GATT is read, and no data crosses the radio: once the
 * beacon says the PC is there, the tablet talks to it over HTTP on the LAN exactly as it does now.
 *
 * Two reasons it is worth having at all. One, a failed HTTP poll cannot tell "the PC is asleep" from
 * "a packet went missing", and those want different words on the screen and different patience. Two,
 * a PC that has just woken up is reachable a second later, but the poller only looks every POLL_S, so
 * the beacon appearing is the cheapest way to look *now*.
 *
 * The radio belongs to the C6 co-processor, which reaches the P4 as HCI frames multiplexed over the
 * same SDIO link that carries Wi-Fi. It works only when three things line up: the C6's esp-hosted
 * image was built with CONFIG_BT_ENABLED (M5Stack's factory image and this repository's firmware/c6
 * up to now were not), this firmware was built with NimBLE as its host stack, and the controller then
 * answers. When any of them is missing hal_ble_watch() answers false and the tablet behaves exactly
 * as it did before BLE existed -- that is the point of it being a separate question. */
typedef struct {
    bool built;          /* this firmware has a BLE host stack compiled in at all */
    bool ready;          /* the host stack synced with the C6's controller: a radio really answered */
    bool watching;
    bool present;        /* the beacon was seen inside the watch's hold window */
    int rssi;            /* of the last sighting; 0 when there has been none */
    double last;         /* hal_seconds() of the last sighting, 0 never */
    unsigned seen;       /* sightings since boot -- proof the radio path works, for the dev console */
} hal_ble_t;

/* Watch for undirected advertisements carrying manufacturer-specific data whose first `n` bytes match
 * `id`. `hold_s` is how long a sighting counts for: advertisers skip intervals and a scan window
 * misses packets, so a single gap must not read as absence. `on_change` runs on the BLE host's task
 * when present flips either way -- do nothing slow or blocking in it, just set a flag.
 *
 * Returns false when this firmware has no BLE host stack at all, or the stack refused to start; then
 * nothing else here does anything. It cannot tell a working radio from a missing one, because a C6
 * without a controller answers nothing rather than refusing: watch `ready` for that. Calling it again
 * replaces the id being watched for. */
bool hal_ble_watch(const uint8_t *id, size_t n, double hold_s, void (*on_change)(bool present, int rssi));
void hal_ble_stop(void);
void hal_ble(hal_ble_t *out);

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
