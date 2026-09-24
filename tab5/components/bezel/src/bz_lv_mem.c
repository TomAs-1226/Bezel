/* LVGL's allocator on the tablet: everything LVGL owns — objects, styles, label text, layers — in PSRAM.
 * With the C library's malloc, anything under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL went to internal RAM, and
 * a UI of a few thousand small objects used up the 768 KB that DMA, Wi-Fi and the task stacks need. */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "src/stdlib/lv_mem.h"
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : malloc(size);
#else
    return malloc(size);
#endif
}

void *lv_realloc_core(void *p, size_t new_size)
{
#ifdef ESP_PLATFORM
    void *q = heap_caps_realloc(p, new_size, CAPS);
    return q ? q : realloc(p, new_size);
#else
    return realloc(p, new_size);
#endif
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
