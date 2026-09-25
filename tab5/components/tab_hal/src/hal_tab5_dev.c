/* The development console on the USB-C port (USB-Serial/JTAG), for driving the tablet from a PC without
 * touching it: screenshots of what LVGL drew, and taps and swipes injected as if from the touch panel.
 * tools/tab5_dev.py is the other end.
 *
 *   shot                     → "SHOT w h n\n", then n bytes: the landscape picture, RGB565 little-endian,
 *                              run-length coded as (uint16 count, uint16 pixel) pairs
 *   tap x y                  → a press and release at (x, y) in the landscape frame
 *   swipe x0 y0 x1 y1 ms     → a press moving from one point to the other over ms, then a release
 *   flip                     → turns the picture 180°
 *
 * Every reply ends in "OK\n" or "ERR ...\n". Logging is silenced while a picture goes out so no log line
 * lands in the middle of it. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bz_ui.h"
#include "hal.h"

static const char *TAG = "dev";

/* the injected finger, read by hal_touch() before the panel */
static volatile struct {
    bool down;
    int x, y;
} s_touch;

bool hal_dev_touch(int *x, int *y)
{
    if (!s_touch.down) return false;
    *x = s_touch.x;
    *y = s_touch.y;
    return true;
}

static void put(const void *p, size_t n)
{
    const uint8_t *b = p;
    int stalls = 0;
    while (n) {
        int w = usb_serial_jtag_write_bytes(b, n > 4096 ? 4096 : n, pdMS_TO_TICKS(1000));
        /* a big shot (the camera's noise barely compresses) outlasts the host's reads now and then: a few
         * stalls are waited out, not taken as the host gone */
        if (w <= 0) {
            if (++stalls > 8) return;
            continue;
        }
        b += w;
        n -= (size_t)w;
    }
}

static void say(const char *s) { put(s, strlen(s)); }

static void shot(bool panel)
{
    hal_display_t d;
    hal_display(&d);
    /* LVGL's landscape picture, or the portrait frame buffer the panel is scanning out */
    const uint16_t *px = bz_ui_content_buf(); /* LVGL's current buffer (they swap) */
    int w = panel ? HAL_H : HAL_W, h = panel ? HAL_W : HAL_H;
    size_t n = (size_t)HAL_W * HAL_H;
    uint16_t *copy = NULL; /* the panel's picture, copied out before the frames after it can overwrite it */
    if (panel) {
        copy = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
        if (!copy) {
            say("ERR no memory\n");
            return;
        }
        hal_front_fb_copy(copy);
        px = copy;
    }
    /* run-length coded in two passes, so nothing the size of the picture is allocated: the first counts
     * the runs (the header carries the length), the second sends them 32 KB at a time */
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        uint16_t v = px[i];
        size_t run = 1;
        while (i + run < n && run < 65535 && px[i + run] == v) run++;
        k += 2;
        i += run;
    }
    uint16_t *rle = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    if (!rle) {
        free(copy);
        say("ERR no memory\n");
        return;
    }
    esp_log_level_t was = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    char head[48];
    /* a panel picture also says whether the picture is turned, so the tool shows it the way up it reads */
    snprintf(head, sizeof head, "SHOT %d %d %u %d\n", w, h, (unsigned)(k * 2), panel && hal_flip() ? 1 : 0);
    say(head);
    size_t m = 0;
    for (size_t i = 0; i < n;) {
        uint16_t v = px[i];
        size_t run = 1;
        while (i + run < n && run < 65535 && px[i + run] == v) run++;
        rle[m++] = (uint16_t)run;
        rle[m++] = v;
        i += run;
        if (m == 16384) {
            put(rle, m * 2);
            m = 0;
        }
    }
    if (m) put(rle, m * 2);
    say("OK\n");
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
    esp_log_level_set("*", was);
    free(rle);
    free(copy);
}

#define KEYS_MAX 8192
static char *s_keys; /* a key file on its way in (keybegin..keyend) */
static size_t s_nkeys;

static FILE *s_put; /* a file on its way in (putbegin..putend) */
static char s_put_path[96];
static unsigned long s_put_size;
static size_t s_put_n;
static uint32_t s_put_crc;

static void c6_progress(size_t done, size_t total)
{
    char m[64];
    snprintf(m, sizeof m, "MEM c6 update: %u of %u\n", (unsigned)done, (unsigned)total);
    say(m);
}

static bool (*s_handler)(const char *line);
void hal_dev_set_handler(bool (*fn)(const char *line)) { s_handler = fn; }

static void swipe(int x0, int y0, int x1, int y1, int ms)
{
    int steps = ms / 16 > 1 ? ms / 16 : 1;
    s_touch.x = x0;
    s_touch.y = y0;
    s_touch.down = true;
    vTaskDelay(pdMS_TO_TICKS(32));
    for (int i = 1; i <= steps; i++) {
        s_touch.x = x0 + (x1 - x0) * i / steps;
        s_touch.y = y0 + (y1 - y0) * i / steps;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    vTaskDelay(pdMS_TO_TICKS(32));
    s_touch.down = false;
}

static void run(char *line)
{
    int a, b, c, e, f;
    if (!strcmp(line, "shot")) {
        shot(false);
    } else if (!strcmp(line, "pshot")) {
        shot(true);
    } else if (sscanf(line, "tap %d %d", &a, &b) == 2) {
        swipe(a, b, a, b, 60);
        say("OK\n");
    } else if (sscanf(line, "swipe %d %d %d %d %d", &a, &b, &c, &e, &f) == 5) {
        swipe(a, b, c, e, f);
        say("OK\n");
    } else if (sscanf(line, "down %d %d", &a, &b) == 2 || sscanf(line, "move %d %d", &a, &b) == 2) {
        /* a finger held down: move it with more "move"s, lift it with "up" */
        s_touch.x = a;
        s_touch.y = b;
        s_touch.down = true;
        say("OK\n");
    } else if (!strcmp(line, "up")) {
        s_touch.down = false;
        say("OK\n");
    } else if (!strncmp(line, "settime ", 8)) {
        /* the PC's clock (tools/tab5_dev.py settime sends it): seconds since the epoch, UTC, and the zone */
        long long t = atoll(line + 8);
        const char *tz = strchr(line + 8, ' ');
        struct timeval tv = { .tv_sec = (time_t)t };
        settimeofday(&tv, NULL);
        if (tz) {
            setenv("TZ", tz + 1, 1);
            tzset();
            hal_kv_set("tz", tz + 1);
        }
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        hal_rtc_set(&tm);
        say("OK\n");
    } else if (!strcmp(line, "bat")) {
        hal_battery_t bt;
        char buf[120];
        if (hal_battery(&bt)) snprintf(buf, sizeof buf, "MEM battery %.3f V %.3f A, %d %%%s\n", bt.volts, bt.amps, bt.percent, bt.charging ? ", charging" : "");
        else snprintf(buf, sizeof buf, "MEM battery: no reading\n");
        say(buf);
        say("OK\n");
    } else if (!strncmp(line, "net", 3) && (line[3] == 0 || line[3] == ' ')) {
        char buf[256];
        hal_net_report(buf, sizeof buf, line[3] ? line + 4 : "api.openai.com");
        say("MEM ");
        say(buf);
        say("\n");
        say("OK\n");
    } else if (!strncmp(line, "putbegin ", 9)) {
        /* a file from the PC onto the card: putbegin <path> <size>, puthex <hex>..., putend <crc32 hex> */
        if (s_put) fclose(s_put);
        unsigned long size = 0;
        char *sp = strrchr(line + 9, ' ');
        if (sp) {
            size = strtoul(sp + 1, NULL, 10);
            *sp = 0;
        }
        snprintf(s_put_path, sizeof s_put_path, "%s", line + 9);
        s_put = NULL;
        for (int t = 0; t < 20 && !s_put; t++) {
            if (t) vTaskDelay(pdMS_TO_TICKS(250));
            s_put = fopen(s_put_path, "wb");
        }
        s_put_size = size;
        s_put_n = 0;
        s_put_crc = 0;
        say(s_put ? "OK\n" : "ERR can't open\n");
    } else if (!strncmp(line, "puthex ", 7)) {
        /* puthex <offset> <hex>: a chunk the PC sends again after a lost reply is one already here: taken as
         * done, not written twice */
        char *hx = strchr(line + 7, ' ');
        unsigned long off = strtoul(line + 7, NULL, 10);
        if (!hx || off != s_put_n) {
            say(hx && off < s_put_n ? "OK\n" : "ERR offset\n");
            return;
        }
        uint8_t b[600];
        size_t n = 0;
        for (const char *h = hx + 1; h[0] && h[1] && n < sizeof b; h += 2) {
            unsigned v;
            if (sscanf(h, "%2x", &v) != 1) break;
            b[n++] = (uint8_t)v;
        }
        bool ok = s_put && fwrite(b, 1, n, s_put) == n;
        if (ok) {
            s_put_crc = esp_rom_crc32_le(s_put_crc, b, n);
            s_put_n += n;
        }
        say(ok ? "OK\n" : "ERR write\n");
    } else if (!strncmp(line, "putend ", 7)) {
        unsigned long want = strtoul(line + 7, NULL, 16);
        bool ok = s_put && fclose(s_put) == 0;
        s_put = NULL;
        char m[120];
        snprintf(m, sizeof m, "MEM put %s: %u of %lu bytes, crc %08lx (want %08lx)\n", s_put_path, (unsigned)s_put_n,
                 s_put_size, (unsigned long)s_put_crc, want);
        say(m);
        say(ok && s_put_n == s_put_size && s_put_crc == want ? "OK\n" : "ERR mismatch\n");
    } else if (!strncmp(line, "c6ota ", 6)) {
        /* the C6's firmware from a file on the card, then a restart (the C6 restarts into the new image) */
        char err[96] = "";
        say("MEM c6 update: starting\n");
        bool ok = hal_c6_ota(line + 6, c6_progress, err, sizeof err);
        char m[140];
        snprintf(m, sizeof m, "MEM c6 update: %s%s\n", ok ? "done, restarting" : "failed: ", ok ? "" : err);
        say(m);
        say(ok ? "OK\n" : "ERR c6 update\n");
        if (ok) {
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
            vTaskDelay(pdMS_TO_TICKS(5000)); /* the C6 finishing its switch */
            hal_restart_planned("the C6 updated");
        }
    } else if (!strncmp(line, "wdhold", 6)) {
        hal_c6_hold(line[6] != ' ' || line[7] != '0');
        say(line[6] == ' ' && line[7] == '0' ? "MEM watchdog restarts back on\n" : "MEM watchdog restarts held\n");
        say("OK\n");
    } else if (!strncmp(line, "c6kick", 6)) {
        extern void esp_hosted_sdio_kick(int n);
        esp_hosted_sdio_kick(line[6] == ' ' ? atoi(line + 7) : 4);
        say("OK\n");
    } else if (!strcmp(line, "c6dbg")) {
        extern int esp_hosted_sdio_debug(char *out, size_t n);
        char m[256] = "MEM ";
        esp_hosted_sdio_debug(m + 4, sizeof m - 6);
        strcat(m, "\n");
        say(m);
        extern int esp_hosted_sdio_debug2(char *out, size_t n);
        esp_hosted_sdio_debug2(m + 4, sizeof m - 6);
        strcat(m, "\n");
        say(m);
        say("OK\n");
    } else if (!strcmp(line, "wdtest")) {
        say("OK\n");
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(300));
        hal_c6_restart_test();
    } else if (!strcmp(line, "rejoin")) {
        hal_wifi_rejoin();
        say("OK\n");
    } else if (!strcmp(line, "mem")) {
        /* internal RAM (the scarce kind) and PSRAM: free, the lowest it has been, the largest block; then
         * each task's stack headroom */
        char buf[160];
        snprintf(buf, sizeof buf, "MEM internal free %u min %u largest %u; psram free %u largest %u\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        say(buf);
        /* and each task's state and its share of the CPU since the last "mem" */
        static TaskStatus_t st[48];
        static struct { TaskHandle_t h; uint32_t run; } prev[48];
        static uint32_t prev_total;
        uint32_t total = 0;
        int n = (int)uxTaskGetSystemState(st, 48, &total);
        uint32_t dt = total - prev_total;
        static const char STATE[] = "RrBSDI";
        for (int i = 0; i < n; i++) {
            uint32_t was = 0;
            for (int k = 0; k < 48; k++)
                if (prev[k].h == st[i].xHandle) was = prev[k].run;
            unsigned pct = dt ? (unsigned)((uint64_t)(st[i].ulRunTimeCounter - was) * 1000 / dt) : 0;
            snprintf(buf, sizeof buf, "TASK %-16s core %d prio %u headroom %u state %c cpu %u.%u%%\n", st[i].pcTaskName,
                     (int)xTaskGetCoreID(st[i].xHandle), (unsigned)st[i].uxCurrentPriority,
                     (unsigned)st[i].usStackHighWaterMark, STATE[st[i].eCurrentState < 6 ? st[i].eCurrentState : 5],
                     pct / 10, pct % 10);
            say(buf);
        }
        for (int i = 0; i < 48; i++) prev[i].h = NULL;
        for (int i = 0; i < n && i < 48; i++) prev[i] = (typeof(prev[0])){ st[i].xHandle, st[i].ulRunTimeCounter };
        prev_total = total;
        say("OK\n");
    } else if (!strcmp(line, "scan")) {
        /* a Wi-Fi scan, one "AP rssi ssid" line per network heard */
        static hal_ap_t ap[20];
        int n = hal_wifi_scan(ap, 20);
        char l[64];
        for (int i = 0; i < n; i++) {
            snprintf(l, sizeof l, "AP %d %s\n", ap[i].rssi, ap[i].ssid);
            say(l);
        }
        say("OK\n");
    } else if (!strcmp(line, "keybegin")) {
        /* A key file sent from the PC (tools/tab5_dev.py keys FILE): hex chunks into PSRAM, written to the
         * card as CATOS/KEYS.ENV at keyend, then a restart, whose start-up imports it and burns the file
         * (assist_import_card). Never echoed, never logged. */
        free(s_keys);
        s_keys = heap_caps_calloc(1, KEYS_MAX, MALLOC_CAP_SPIRAM);
        s_nkeys = 0;
        say(s_keys ? "OK\n" : "ERR no memory\n");
    } else if (!strncmp(line, "keyhex ", 7)) {
        for (const char *h = line + 7; s_keys && h[0] && h[1] && s_nkeys < KEYS_MAX - 1; h += 2) {
            unsigned v;
            if (sscanf(h, "%2x", &v) != 1) break;
            s_keys[s_nkeys++] = (char)v;
        }
        say("OK\n");
    } else if (!strcmp(line, "keyend")) {
        const char *sd = hal_sd_root();
        bool ok = false;
        if (s_keys && sd) {
            char path[64];
            snprintf(path, sizeof path, "%s/CATOS/KEYS.ENV", sd);
            /* in its first ~20 s after start-up the card answers writes with EIO (start-up is still reading it): tried
             * for a few seconds */
            for (int t = 0; t < 20 && !ok; t++) {
                if (t) vTaskDelay(pdMS_TO_TICKS(250));
                FILE *f = fopen(path, "wb");
                if (!f) continue;
                ok = fwrite(s_keys, 1, s_nkeys, f) == s_nkeys;
                ok = fclose(f) == 0 && ok;
            }
        }
        if (s_keys) memset(s_keys, 0, KEYS_MAX);
        free(s_keys);
        s_keys = NULL;
        s_nkeys = 0;
        if (!ok) {
            char e[64];
            snprintf(e, sizeof e, sd ? "ERR couldn't write the card (errno %d)\n" : "ERR no microSD card\n", errno);
            say(e);
        } else {
            say("OK\n");
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
            vTaskDelay(pdMS_TO_TICKS(300));
            hal_restart_planned("a key file arrived"); /* the start-up imports it */
        }
    } else if (sscanf(line, "ant %d", &a) == 1) {
        void hal_antenna(bool external);
        hal_antenna(a != 0);
        say("OK\n");
    } else if (!strcmp(line, "flip")) {
        hal_set_flip(!hal_flip());
        say("OK\n");
    } else if (s_handler && s_handler(line)) {
        vTaskDelay(pdMS_TO_TICKS(200)); /* the UI's loop picks it up next frame */
        say("OK\n");
    } else if (line[0]) {
        say("ERR unknown\n");
    }
}

static void dev_task(void *arg)
{
    (void)arg;
    static char line[1200]; /* puthex carries 512 bytes */
    size_t len = 0;
    for (;;) {
        char ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[len] = 0;
            run(line);
            len = 0;
        } else if (len < sizeof line - 1) {
            line[len++] = ch;
        }
    }
}

void hal_dev_init(void)
{
    /* 4 KB in: a puthex line is ~1 KB, and bytes past a full buffer are dropped, not waited for */
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 16384, .rx_buffer_size = 4096 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no USB-Serial/JTAG driver: the dev console is off");
        return;
    }
    usb_serial_jtag_vfs_use_driver(); /* the log goes through the driver too, so the two don't collide */
    xTaskCreatePinnedToCore(dev_task, "dev", 4096, NULL, 2, NULL, 0);
}
