/* Catalyst Tab on the M5Stack Tab5.
 *
 * Core 1 runs the UI: LVGL renders the page and the glass ink, the Bezel compositor frosts and lights
 * the glass, the PPA turns each finished frame onto the portrait panel, and the loop waits for vsync.
 * Core 0 runs everything that waits on the outside world: the NetworkTables client, and the HAL's
 * microphone, tone, camera and CAN workers. LVGL's software renderer spreads its own work over both
 * cores (two draw units, lv_conf.h). */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "bz_theme.h"
#include "bz_ui.h"
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

static void ui_task(void *arg)
{
    (void)arg;
    double t0 = hal_seconds();
    bool settled = false, ok = false;
    for (;;) {
        bool busy = bz_ui_frame(hal_seconds());
        double up = hal_seconds() - t0;
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
    for (;;) {
        bz_area_t a;
        bool more = ui_boot_frame(s_boot, hal_seconds() - t0, &a);
        if (a.x2 >= a.x1 && a.y2 >= a.y1) {
            bz_present_t p = { a, buf + (size_t)a.y1 * HAL_W + a.x1, HAL_W };
            hal_present(&p, 1, NULL);
        }
        if (!more) break;
        vTaskDelay(1);
    }
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
    hal_boot_t prev;
    hal_boot_prev(&prev);
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
    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 6, NULL, 1);
}
