/* Catalyst OS's hardware needs beyond the robot tools: pictures from the card through the P4's JPEG decoder.
 *
 * The decoder writes whole MCUs, so its output rows are the width rounded up to 8 or 16 pixels; the picture
 * handed back is scaled (box-averaged when shrinking by 2x or more) into a packed buffer of its own. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal.h"

static const char *TAG = "os";

#define JPEG_FILE_MAX (16u << 20) /* a 16 MB JPEG is already beyond what PSRAM could decode */
#define PSRAM_SPARE (1u << 20)    /* left free for everything else while a picture decodes */

static jpeg_decoder_handle_t s_dec;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool fail(char *err, size_t n, const char *fmt, ...)
{
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return false;
}

static inline uint16_t avg565(uint32_t r, uint32_t g, uint32_t b, uint32_t n)
{
    return (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
}

/* src (stride sw pixels, w×h used) into dst (ow×oh, packed): each output pixel averages the k×k block it
 * covers, k = the whole-number shrink (1: nearest). */
static void scale(const uint16_t *src, int sw, int w, int h, uint16_t *dst, int ow, int oh)
{
    int k = w / ow < h / oh ? w / ow : h / oh;
    if (k < 1) k = 1;
    if (k > 4) k = 4;
    for (int y = 0; y < oh; y++) {
        int sy = (int)((int64_t)y * h / oh);
        if (sy + k > h) sy = h - k;
        for (int x = 0; x < ow; x++) {
            int sx = (int)((int64_t)x * w / ow);
            if (sx + k > w) sx = w - k;
            if (k == 1) {
                dst[(size_t)y * ow + x] = src[(size_t)sy * sw + sx];
                continue;
            }
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < k; j++) {
                const uint16_t *row = src + (size_t)(sy + j) * sw + sx;
                for (int i = 0; i < k; i++) {
                    uint16_t p = row[i];
                    r += p >> 11;
                    g += (p >> 5) & 0x3f;
                    b += p & 0x1f;
                }
            }
            dst[(size_t)y * ow + x] = avg565(r, g, b, (uint32_t)(k * k));
        }
    }
}

bool hal_jpeg_load(const char *path, int max_w, int max_h, hal_picture_t *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    if (err && errn) err[0] = 0;
    if (!path || max_w <= 0 || max_h <= 0) return fail(err, errn, "nothing to decode");

    taskENTER_CRITICAL(&s_mux);
    bool make = !s_lock;
    taskEXIT_CRITICAL(&s_mux);
    if (make) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_mux);
        if (!s_lock) {
            s_lock = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (m) vSemaphoreDelete(m);
    }
    if (!s_lock) return fail(err, errn, "out of memory");

    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, errn, "can't open the file");
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return fail(err, errn, "the file is empty");
    }
    if ((unsigned long)size > JPEG_FILE_MAX) {
        fclose(f);
        return fail(err, errn, "too big: %ld MB", size >> 20);
    }

    size_t in_cap = 0, out_cap = 0;
    jpeg_decode_memory_alloc_cfg_t im = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    uint8_t *in = jpeg_alloc_decoder_mem((size_t)size, &im, &in_cap);
    if (!in) {
        fclose(f);
        return fail(err, errn, "out of memory reading the file");
    }
    size_t got = fread(in, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(in);
        return fail(err, errn, "read error");
    }

    jpeg_decode_picture_info_t info = { 0 };
    esp_err_t e = jpeg_decoder_get_info(in, (uint32_t)size, &info);
    if (e != ESP_OK || !info.width || !info.height) {
        free(in);
        return fail(err, errn, "not a JPEG the decoder reads (%s)", esp_err_to_name(e));
    }
    /* the decoder's rows: whole MCUs, 8 or 16 pixels each way by the sampling */
    int mx = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    int my = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    int w = (int)info.width, h = (int)info.height;
    int sw = (w + mx - 1) / mx * mx, sh = (h + my - 1) / my * my;
    size_t need = (size_t)sw * sh * 2;
    size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (need + PSRAM_SPARE > room) {
        free(in);
        return fail(err, errn, "%dx%d is too large to decode here", w, h);
    }
    jpeg_decode_memory_alloc_cfg_t om = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint16_t *dec = jpeg_alloc_decoder_mem(need, &om, &out_cap);
    if (!dec) {
        free(in);
        return fail(err, errn, "out of memory for %dx%d", w, h);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_dec) {
        jpeg_decode_engine_cfg_t ec = { .timeout_ms = 2000 };
        e = jpeg_new_decoder_engine(&ec, &s_dec);
        if (e != ESP_OK) s_dec = NULL;
    }
    uint32_t used = 0;
    if (s_dec) {
        jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                 .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, /* little-endian RGB565, as LVGL draws */
                                 .conv_std = JPEG_YUV_RGB_CONV_STD_BT601 };
        e = jpeg_decoder_process(s_dec, &dc, in, (uint32_t)size, (uint8_t *)dec, (uint32_t)out_cap, &used);
    }
    xSemaphoreGive(s_lock);
    free(in);
    if (!s_dec) {
        free(dec);
        return fail(err, errn, "no hardware decoder (%s)", esp_err_to_name(e));
    }
    if (e != ESP_OK) {
        free(dec);
        ESP_LOGW(TAG, "decode %s: %s", path, esp_err_to_name(e));
        return fail(err, errn, "the decoder refused it (%s): progressive JPEGs aren't supported", esp_err_to_name(e));
    }

    float s = (float)max_w / w < (float)max_h / h ? (float)max_w / w : (float)max_h / h;
    if (s > 1) s = 1;
    int ow = (int)(w * s), oh = (int)(h * s);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint16_t *px = heap_caps_malloc((size_t)ow * oh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) {
        free(dec);
        return fail(err, errn, "out of memory for the picture");
    }
    scale(dec, sw, w, h, px, ow, oh);
    free(dec);
    *out = (hal_picture_t){ .px = px, .w = ow, .h = oh, .src_w = w, .src_h = h };
    return true;
}

void hal_picture_free(hal_picture_t *p)
{
    if (!p) return;
    free(p->px);
    p->px = NULL;
    p->w = p->h = 0;
}
