/* LVGL 9.3 configuration for Catalyst Tab — shared by the ESP32-P4 firmware and the host simulator.
 * Only what differs from LVGL's defaults is set here; lv_conf_internal.h fills in the rest.
 * The build points LVGL at this file with -DLV_CONF_PATH. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* malloc everywhere: on the P4, CONFIG_SPIRAM_USE_MALLOC routes large blocks to the 32 MB PSRAM */
#ifdef ESP_PLATFORM
/* the tablet: LVGL's own allocator, all in PSRAM (components/bezel/src/bz_lv_mem.c) */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* below the panel's 16.5 ms frame: LVGL renders on every frame the loop runs, never skips one to jitter */
#define LV_DEF_REFR_PERIOD 8
#define LV_DPI_DEF 294

#ifdef ESP_PLATFORM
/* Two software draw units, one per HP core: LVGL's renderer uses both. */
#define LV_USE_OS LV_OS_FREERTOS
#define LV_DRAW_SW_DRAW_UNIT_CNT 2
/* shadows, big glyphs and transforms go deep: LVGL's 8 KB default is too tight for its draw threads */
#define LV_DRAW_THREAD_STACK_SIZE (16 * 1024)
/* above the NetworkTables client (5), the HAL workers (4) and lwIP, level with the UI task: at LVGL's
 * default (3 over idle) a draw thread on core 0 waits behind the network and a frame waits for it */
#define LV_DRAW_THREAD_PRIO 6
/* LVGL's hot loops run from PSRAM like the rest of the code (XIP, through the 256 KB L2 cache). In IRAM
 * they took ~67 KB of the 768 KB internal SRAM, which the DMA reserve, esp-hosted and the task stacks need. */
#define LV_ATTRIBUTE_FAST_MEM
/* The P4's PPA as an LVGL draw unit (LVGL 9.4+): square, opaque fills — every page's ground — go to the
 * 2D engine instead of the CPU. Rounded or translucent fills stay on the CPU draw units. */
#define LV_USE_PPA 0
#define LV_USE_PPA_IMG 0
#define LV_PPA_BURST_LENGTH 128
#else
#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#endif

/* 64-byte alignment: the P4's PPA and cache lines want it for any buffer it touches */
#define LV_DRAW_BUF_ALIGN 128
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (64 * 1024)
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_USE_FLOAT 1
#define LV_USE_MATRIX 1

/* Bezel's faces are baked by tools/make_fonts.py; Montserrat stays as LVGL's fallback */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 0

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0

#define LV_USE_CANVAS 1
#define LV_USE_SNAPSHOT 0
#define LV_USE_OBSERVER 0
#define LV_USE_SYSMON 0

#endif
