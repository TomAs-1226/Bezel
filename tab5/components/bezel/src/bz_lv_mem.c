/* LVGL's allocator on the tablet: everything LVGL owns — objects, styles, label text, layers — in PSRAM.
 * With the C library's malloc, anything under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL went to internal RAM, and
 * a UI of a few thousand small objects used up the 768 KB that DMA, Wi-Fi and the task stacks need. */
#include "lvgl.h"
#include <stdatomic.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "src/stdlib/lv_mem.h"
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

/* What LVGL holds, so "psram" can name it. Every object, style, label string and layer LVGL owns comes
 * through here, which makes this counter the only place the UI's own share of PSRAM is visible: the heap
 * can say how much is gone but not who has it. heap_caps_get_allocated_size gives the block's real size,
 * header and rounding included, so the total matches what the heap lost rather than what was asked for. */
static atomic_size_t s_held, s_peak;
static atomic_int s_blocks, s_internal; /* s_internal: blocks that fell back to internal RAM (PSRAM full) */

static void held_add(long d)
{
    size_t h = atomic_fetch_add(&s_held, (size_t)d) + (size_t)d;
    size_t pk = atomic_load(&s_peak);
    while (h > pk && !atomic_compare_exchange_weak(&s_peak, &pk, h)) {}
}

void bz_lv_mem_stat(size_t *held, size_t *peak, int *blocks, int *internal)
{
    if (held) *held = atomic_load(&s_held);
    if (peak) *peak = atomic_load(&s_peak);
    if (blocks) *blocks = atomic_load(&s_blocks);
    if (internal) *internal = atomic_load(&s_internal);
}

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
    /* PSRAM full. A small block may come out of internal RAM, counted, because losing a style or a label
     * string is a visible fault for the sake of a few hundred bytes. A large one may not: internal RAM is
     * the scarce kind -- the DMA reserve, the Wi-Fi link and every task stack come out of its 768 KB, with
     * ~48 KB free and no block over 30 KB in normal running -- so handing a draw layer or a long string
     * internal memory does not save the frame, it takes down the network or a task stack a moment later.
     * Failing here is the better answer: LVGL's callers all handle it, and "psram" counts what spilled. */
    if (!p && size <= 2048 && (p = malloc(size)) != NULL) atomic_fetch_add(&s_internal, 1);
    if (p) {
        held_add((long)heap_caps_get_allocated_size(p));
        atomic_fetch_add(&s_blocks, 1);
    }
    return p;
#else
    return malloc(size);
#endif
}

void *lv_realloc_core(void *p, size_t new_size)
{
#ifdef ESP_PLATFORM
    size_t was = p ? heap_caps_get_allocated_size(p) : 0;
    void *q = heap_caps_realloc(p, new_size, CAPS);
    if (!q && new_size <= 2048 && (q = realloc(p, new_size)) != NULL) atomic_fetch_add(&s_internal, 1);
    if (q) {
        held_add((long)heap_caps_get_allocated_size(q) - (long)was);
        if (!p) atomic_fetch_add(&s_blocks, 1);
    }
    return q;
#else
    return realloc(p, new_size);
#endif
}

void lv_free_core(void *p)
{
#ifdef ESP_PLATFORM
    if (p) {
        held_add(-(long)heap_caps_get_allocated_size(p));
        atomic_fetch_sub(&s_blocks, 1);
    }
#endif
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
