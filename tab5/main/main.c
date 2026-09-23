/* Catalyst Tab on the M5Stack Tab5.
 *
 * Core 1 runs the UI: LVGL renders the page and the glass ink, the Bezel compositor frosts and lights
 * the glass, the PPA turns each finished frame onto the portrait panel, and the loop waits for vsync.
 * Core 0 runs everything that waits on the outside world: the NetworkTables client, and the HAL's
 * microphone, tone, camera and CAN workers. LVGL's software renderer spreads its own work over both
 * cores (two draw units, lv_conf.h). */
#include <pthread.h>
#include <stdio.h>

#include "bz_theme.h"
#include "bz_ui.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"
#include "nt4.h"
#include "ui.h"

static const char *TAG = "catalyst-tab";

static void *nt_thread(void *arg)
{
    nt4_run(arg);
    return NULL;
}

static void ui_task(void *arg)
{
    for (;;) {
        bool busy = bz_ui_frame(hal_seconds());
        /* animating: the next frame right away (present already waited for vsync); idle: ~30 Hz is
         * plenty for numbers that refresh at 10 Hz, and leaves the cores cool */
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    if (!hal_init()) {
        ESP_LOGE(TAG, "hardware init failed");
        return;
    }
    lv_init();
    hal_display_t d;
    hal_display(&d);
    bz_ui_config_t cfg = {
        .w = HAL_W, .h = HAL_H, .content = d.content, .ink = d.ink, .out = d.out,
        .present = hal_present, .read_touch = hal_touch, .ops = d.ops,
    };
    bz_ui_init(&cfg);
    bz_theme_init();

    static const char *const prefixes[] = { "/Catalyst/", "/FMSInfo/", "/Auto Selector/", "/SmartDashboard/",
                                             "/limelight", "/PathPlanner/", NULL };
    nt4_config_t ncfg = { .client_name = "catalyst-tab", .period_s = 0.05, .prefixes = prefixes };
    nt4_client_t *nt = nt4_create(&ncfg);
    ui_config_t ucfg = { .nt = nt, .team = 0, .sim_address = NULL };
    ui_init(&ucfg);

    esp_pthread_cfg_t pc = esp_pthread_get_default_config();
    pc.pin_to_core = 0;
    pc.stack_size = 8192;
    pc.prio = 5;
    pc.thread_name = "nt4";
    esp_pthread_set_cfg(&pc);
    pthread_t th;
    pthread_create(&th, NULL, nt_thread, nt);

    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 6, NULL, 1);
}
