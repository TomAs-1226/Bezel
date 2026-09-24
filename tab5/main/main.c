/* Catalyst Tab on the M5Stack Tab5.
 *
 * Core 1 runs the UI: LVGL renders the page and the glass ink, the Bezel compositor frosts and lights
 * the glass, the PPA turns each finished frame onto the portrait panel, and the loop waits for vsync.
 * Core 0 runs everything that waits on the outside world: the NetworkTables client, and the HAL's
 * microphone, tone, camera and CAN workers. LVGL's software renderer spreads its own work over both
 * cores (two draw units, lv_conf.h). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_theme.h"
#include "bz_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal.h"
#include "nt4.h"
#include "ui.h"
#include "ui_boot.h"

static const char *TAG = "catalyst-tab";

static void *nt_thread(void *arg)
{
    nt4_run(arg);
    return NULL;
}

static SemaphoreHandle_t s_ui_go;
static hal_boot_t s_prev; /* how the last start ended, repeated in the heartbeat: USB comes up after boot */

static void ui_task(void *arg)
{
    (void)arg;
    /* created first thing, so its 16 KB stack comes from internal RAM before Wi-Fi and USB take it (the UI
     * saves settings to NVS, so its stack can't live in PSRAM); it waits here until start-up is done */
    xSemaphoreTake(s_ui_go, portMAX_DELAY);
    double t0 = hal_seconds();
    bool settled = false, ok = false;
    unsigned frames = 0;
    bool first = true;
    double beat = t0;
    for (;;) {
        bool busy = bz_ui_frame(hal_seconds());
        double up = hal_seconds() - t0;
        /* a heartbeat in the log: a UI that stops drawing shows as the beats stopping */
        if (first) {
            first = false;
            ESP_LOGI(TAG, "ui: first frame after %.2f s", up);
        }
        frames++;
        /* a heartbeat every 10 s: frame rate and cost, and internal RAM (the scarce memory here) */
        if (hal_seconds() - beat >= 10) {
            bz_ui_perf_t pf;
            bz_ui_perf(&pf);
            ESP_LOGI(TAG, "ui: %.0f loops/s, %.0f fps drawing; ms lvgl %.1f present %.1f; internal ram %u kb free",
                     frames / (hal_seconds() - beat), pf.fps, pf.lvgl_ms, pf.present_ms,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            if (s_prev.failed && up < 60)
                ESP_LOGW(TAG, "last start ended: %s at \"%s\" (%d in a row) %s", s_prev.reason, s_prev.stage,
                         s_prev.fails, s_prev.detail);
            frames = 0;
            beat = hal_seconds();
        }
        /* the USB-A port and the tether come last, once the UI has been drawing for a moment */
        if (!settled && up > 1.5) {
            settled = true;
            hal_settle();
        }
        /* twenty seconds of drawing: this start counts as good, and the next one starts normally */
        if (!ok && up > 20) {
            ok = true;
            hal_boot_ok();
        }
        /* animating: the next frame right away (present already waited for vsync); idle: ~30 Hz is
         * plenty for numbers that refresh at 10 Hz, and leaves the cores cool */
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(30));
    }
}

/* ---- the boot animation, on core 1 while core 0 starts everything else ---- */

static ui_boot_t *s_boot;
static SemaphoreHandle_t s_boot_done;

static void boot_task(void *arg)
{
    uint16_t *buf = arg;
    double t0 = hal_seconds();
    int frames = 0;
    double worst = 0, last = t0, draw = 0, pres = 0;
    uint32_t px = 0;
    for (;;) {
        bz_area_t a;
        double f0 = hal_seconds();
        bool more = ui_boot_frame(s_boot, hal_seconds() - t0, &a);
        double now = hal_seconds();
        draw += now - f0;
        if (now - last > worst) worst = now - last;
        last = now;
        frames++;
        /* the boxes that changed, not their span: the span is most of the screen and most of it is still */
        bz_area_t box[16];
        int nb = a.x2 >= a.x1 && a.y2 >= a.y1 ? ui_boot_damage(s_boot, box, 16) : 0;
        if (nb) {
            bz_present_t p[16];
            for (int i = 0; i < nb; i++) {
                p[i] = (bz_present_t){ box[i], buf + (size_t)box[i].y1 * HAL_W + box[i].x1, HAL_W };
                px += (uint32_t)(box[i].x2 - box[i].x1 + 1) * (uint32_t)(box[i].y2 - box[i].y1 + 1);
            }
            hal_present(p, nb, NULL);
            pres += hal_seconds() - now;
        }
        if (!more) break;
        /* a frame with nothing to present (the outro dims the backlight) waits about as long as one that has */
        vTaskDelay(nb ? 1 : pdMS_TO_TICKS(12));
    }
    ESP_LOGI(TAG, "boot: %d frames in %.2f s, %.1f fps, worst gap %.0f ms; per frame draw %.1f ms, present %.1f ms, %u px",
             frames, hal_seconds() - t0, frames / (hal_seconds() - t0), worst * 1000, draw * 1000 / frames,
             pres * 1000 / frames, (unsigned)(px / (frames ? frames : 1)));
    xSemaphoreGive(s_boot_done);
    vTaskDelete(NULL);
}

static void on_stage(const char *stage)
{
    static const struct { const char *stage, *say; float p; } MAP[] = {
        { "sensors", "sensors", 0.2f }, { "speaker", "speaker", 0.28f }, { "microsd", "microSD", 0.34f },
        { "wi-fi", "waking the wi-fi co-processor", 0.45f }, { "ui", "building the interface", 0.62f },
    };
    for (size_t i = 0; i < sizeof MAP / sizeof *MAP; i++)
        if (!strcmp(MAP[i].stage, stage)) ui_boot_set(s_boot, MAP[i].p, MAP[i].say);
}

void app_main(void)
{
    if (!hal_init()) {
        ESP_LOGE(TAG, "hardware init failed");
        return;
    }
    s_ui_go = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 6, NULL, 1) != pdPASS)
        ESP_LOGE(TAG, "no internal RAM for the UI task's stack (largest block %u bytes)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    hal_boot_t prev;
    hal_boot_prev(&prev);
    s_prev = prev;
    bool safe = prev.fails >= 1;
    if (prev.failed)
        ESP_LOGW(TAG, "last start: %s at %s (%d in a row) %s%s", prev.reason, prev.stage, prev.fails, prev.detail,
                 safe ? " — safe mode" : "");

    lv_init();
    hal_display_t d;
    hal_display(&d);
    /* the animation draws in the composite buffer, which nothing else touches until the UI's first frame */
    s_boot = ui_boot_create(d.out, HAL_W, HAL_H, &prev, safe);
    ui_boot_set(s_boot, 0.1f, "starting");
    s_boot_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(boot_task, "boot", 8192, d.out, 6, NULL, 1);

    hal_boot_watch(on_stage);
    hal_start();

    bz_ui_config_t cfg = {
        .w = HAL_W, .h = HAL_H, .content = d.content, .ink = d.ink, .out = d.out,
        .present = hal_present, .read_touch = hal_touch, .ops = d.ops, .async_present = d.async_present,
        .slide = d.slide,
    };
    bz_ui_init(&cfg);
    bz_theme_init();

    static const char *const prefixes[] = { "/Catalyst/", "/FMSInfo/", "/Auto Selector/", "/SmartDashboard/",
                                             "/limelight", "/PathPlanner/", NULL };
    nt4_config_t ncfg = { .client_name = "catalyst-tab", .period_s = 0.05, .prefixes = prefixes };
    nt4_client_t *nt = nt4_create(&ncfg);
    ui_config_t ucfg = { .nt = nt, .team = 0, .sim_address = NULL };
    ui_init(&ucfg);
    /* safe mode after a failed start: solid glass (no frost or refraction), the cheapest the UI draws */
    if (safe) bz_ui_set_mode(bz_ui_dark(), true);
    ui_boot_set(s_boot, 0.9f, "looking for the robot");

    esp_pthread_cfg_t pc = esp_pthread_get_default_config();
    pc.pin_to_core = 0;
    pc.stack_size = 8192;
    pc.prio = 5;
    pc.thread_name = "nt4";
    esp_pthread_set_cfg(&pc);
    pthread_t th;
    pthread_create(&th, NULL, nt_thread, nt);

    hal_boot_stage("first frame");
    ui_boot_finish(s_boot);
    xSemaphoreTake(s_boot_done, portMAX_DELAY);
    ui_boot_destroy(s_boot);
    s_boot = NULL;
    bz_comp_damage_all(bz_ui_comp());
    ESP_LOGI(TAG, "internal RAM free %u, largest block %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    xSemaphoreGive(s_ui_go);
}
