/* hal_tab5_audio.c — the speaker, the microphones and the wake word on the Tab5.
 *
 * One I2S port serves both codecs (the BSP's shared bus, standard Philips framing, the P4 the master):
 * the ES8388 DAC and its NS4150B amp on TX, the ES7210 ADC on RX. Both directions run at 48 kHz, 16-bit
 * stereo, always: esp_codec_dev refuses two rates on one port, and a mono/stereo switch on RX would
 * re-allocate its DMA buffers in internal RAM at a moment when there may be none. So the RX side is
 * opened in its final format once at boot (the buffers are made while internal RAM is still free), then
 * closed until the companion wants to listen.
 *
 *   Out: one task (core 0, above the workers) owns the speaker. Each 10 ms it mixes the tone in progress
 *        (hal_tone: ticks, chimes) with the PCM stream (hal_play_*: speech, mono at 8/12/16/24/48 kHz,
 *        up-sampled by an integer factor), and writes 480 stereo frames; the codec write blocks on the
 *        DMA ring, which paces the loop. Nothing to play: it sleeps on the tone queue.
 *   In:  hal_mic_read() reads 48 kHz stereo straight from the RX DMA ring on the caller's thread, keeps
 *        one channel (the louder over the last seconds, unless told otherwise: which of the ES7210's inputs
 *        carry the Tab5's two microphones isn't documented, and a quiet one may be the echo reference),
 *        low-passes and decimates 3:1 to 16 kHz mono, and removes DC.
 *   Wake: ESP-SR's WakeNet9 "Hi, ESP" (wn9_hiesp) from the "model" partition, fed 16 kHz mono by the caller.
 *
 * Big buffers are in PSRAM; the only internal allocations are the I2S DMA rings (made at boot) and the out
 * task's 4 KB stack. ESP-SR's prebuilt dl_lib brings ~6 KB of initialised data into internal RAM at link time
 * (esp_idf_size --archives); WakeNet's own model state is allocated when the companion first listens.
 * UNVERIFIED on the unit: the microphone channel map, the ES7210 gain (30 dB), WakeNet's CPU and memory on
 * the P4, and the stream's under-run behaviour on Wi-Fi. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "hal.h"
#include "hal_tab5_priv.h"
#include "model_path.h"

static const char *TAG = "audio";

#define RATE 48000
#define FRAME 480                  /* 10 ms of output */
#define PCM_BYTES (24000 * 2 * 6)  /* six seconds of 24 kHz speech ahead of the speaker */
#define MIC_CHUNK 960              /* 48 kHz frames read at a time (20 ms) */
#define FIR_N 31

typedef struct { float hz; int ms; float vol; } tone_t;

static struct {
    esp_codec_dev_handle_t spk, mic;
    QueueHandle_t tones;
    TaskHandle_t task;
    /* the stream */
    StreamBufferHandle_t pcm;
    volatile int rate;             /* of the stream being played */
    volatile bool open, ended, abort;
    volatile float level;          /* what the speaker is saying now, 0..1 */
    /* the microphones */
    SemaphoreHandle_t mic_lock;
    bool mic_on;
    int16_t *raw;                  /* MIC_CHUNK stereo frames, PSRAM */
    float fir[FIR_N];
    float hist[FIR_N];             /* the chosen channel's last FIR_N samples */
    int hpos, phase;
    float dc_x, dc_y;
    float ms[2];                   /* each channel's mean square, slowly */
    int chan;                      /* -1 auto, 0 left, 1 right, 2 both */
    int use;                       /* the channel auto picked */
    volatile float mic_level;
    /* the wake word */
    const esp_wn_iface_t *wn;
    model_iface_data_t *wd;
    int wn_chunk, wn_fill;
    int16_t *wn_buf;
    bool wn_tried;
    char wn_name[24];
} AU = { .chan = -1 };

/* ------------------------------------------------------------------ out */

static void out_task(void *arg)
{
    (void)arg;
    int16_t *buf = heap_caps_malloc(FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *in = heap_caps_malloc(FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf || !in) {
        ESP_LOGE(TAG, "no memory for the speaker");
        vTaskDelete(NULL);
        return;
    }
    tone_t t = { 0 };
    int t_total = 0, t_done = 0;
    double ph = 0;
    bool primed = false;           /* the stream has enough queued to start (or to resume after a gap) */
    float prev = 0, lvl = 0;
    for (;;) {
        if (AU.abort) {
            xStreamBufferReset(AU.pcm);
            AU.abort = false;
            AU.open = false;
            AU.level = 0;
            primed = false;
            prev = 0;
        }
        if (t_done >= t_total && xQueueReceive(AU.tones, &t, 0) == pdTRUE) {
            t_total = 48 * t.ms;
            t_done = 0;
            ph = 0;
        }
        bool tone = t_done < t_total;
        bool speech = false;
        if (AU.open) {
            size_t have = xStreamBufferBytesAvailable(AU.pcm);
            if (!primed) primed = AU.ended || have >= (size_t)AU.rate * 2 / 6; /* ~170 ms queued */
            speech = primed;
            if (AU.ended && have == 0) { /* played out */
                AU.open = false;
                AU.level = 0;
                primed = false;
                prev = 0;
                speech = false;
            }
        }
        if (!tone && !speech) {
            AU.level = lvl = 0;
            /* nothing to do: a tone wakes us, and the stream is looked at every 10 ms */
            if (xQueueReceive(AU.tones, &t, pdMS_TO_TICKS(AU.open ? 10 : 200)) == pdTRUE) {
                t_total = 48 * t.ms;
                t_done = 0;
                ph = 0;
            }
            continue;
        }
        memset(buf, 0, FRAME * 2 * sizeof(int16_t));
        if (speech) {
            int up = RATE / AU.rate, need = FRAME / up;
            size_t got = xStreamBufferReceive(AU.pcm, in, (size_t)need * 2, 0) / 2;
            if ((int)got < need && !AU.ended) primed = false; /* ran dry: gather ~170 ms before going on */
            double acc = 0;
            for (int i = 0; i < need; i++) {
                float cur = i < (int)got ? in[i] : prev * 0.9f; /* a gap fades instead of clicking */
                for (int j = 0; j < up; j++) {
                    float s = prev + (cur - prev) * (float)(j + 1) / (float)up;
                    int k = i * up + j;
                    buf[2 * k] = buf[2 * k + 1] = (int16_t)s;
                }
                acc += (double)cur * cur;
                prev = cur;
            }
            float rms = sqrtf((float)(acc / need));
            float v = rms / 5000.0f;
            if (v > 1) v = 1;
            lvl += (v - lvl) * (v > lvl ? 0.6f : 0.25f);
            AU.level = lvl;
        }
        if (tone) {
            double step = 2 * M_PI * t.hz / RATE;
            int n = t_total - t_done < FRAME ? t_total - t_done : FRAME;
            for (int i = 0; i < n; i++) {
                /* a short raised-cosine envelope: a tick, not a click */
                int k = t_done + i, edge = 48 * 3;
                float env = k < edge ? 0.5f - 0.5f * cosf((float)M_PI * k / edge)
                          : k > t_total - edge ? 0.5f - 0.5f * cosf((float)M_PI * (t_total - k) / edge) : 1;
                int s = buf[2 * i] + (int)(sin(ph) * 12000 * env * t.vol);
                ph += step;
                s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
                buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
            }
            t_done += n;
        }
        esp_codec_dev_write(AU.spk, buf, FRAME * 2 * sizeof(int16_t));
    }
}

void hal_tone(float hz, int ms, float v)
{
    if (!AU.tones || v <= 0.01f) return;
    tone_t t = { hz, ms, v };
    xQueueSend(AU.tones, &t, 0);
}

void hal_audio_volume(float v)
{
    if (AU.spk) esp_codec_dev_set_out_vol(AU.spk, (int)(v * 100));
}

bool hal_play_start(int rate)
{
    if (!AU.pcm || rate <= 0 || RATE % rate || RATE / rate > FRAME || AU.open) return false;
    AU.rate = rate;
    AU.ended = false;
    AU.abort = false;
    AU.open = true;
    return true;
}

int hal_play_write(const int16_t *pcm, int n, int timeout_ms)
{
    if (!AU.open || AU.ended || n <= 0) return 0;
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = (size_t)n * 2;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (left && !AU.abort && AU.open) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0) break;
        size_t w = xStreamBufferSend(AU.pcm, p, left, until - now);
        p += w;
        left -= w;
    }
    return (int)(((size_t)n * 2 - left) / 2);
}

void hal_play_end(void) { AU.ended = true; }

void hal_play_stop(void)
{
    if (!AU.open) return;
    AU.abort = true;
    for (int i = 0; i < 50 && AU.abort; i++) vTaskDelay(pdMS_TO_TICKS(4));
}

bool hal_play_busy(void) { return AU.open; }
float hal_play_level(void) { return AU.level; }

/* ------------------------------------------------------------------ in */

static void fir_design(void)
{
    /* windowed sinc (Hamming) at 7 kHz: flat through speech, well down by 8 kHz where 16 kHz folds */
    float fc = 7000.0f / RATE, sum = 0;
    for (int i = 0; i < FIR_N; i++) {
        float m = i - (FIR_N - 1) / 2.0f;
        float s = m == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m);
        float w = 0.54f - 0.46f * cosf(2 * (float)M_PI * i / (FIR_N - 1));
        AU.fir[i] = s * w;
        sum += AU.fir[i];
    }
    for (int i = 0; i < FIR_N; i++) AU.fir[i] /= sum;
}

bool hal_mic_start(void)
{
    if (!AU.mic || !AU.mic_lock) return false;
    xSemaphoreTake(AU.mic_lock, portMAX_DELAY);
    if (!AU.mic_on) {
        esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
        if (esp_codec_dev_open(AU.mic, &fs) == ESP_CODEC_DEV_OK) {
            esp_codec_dev_set_in_gain(AU.mic, 30.0f);
            AU.mic_on = true;
            memset(AU.hist, 0, sizeof AU.hist);
            AU.hpos = AU.phase = 0;
            AU.dc_x = AU.dc_y = 0;
            ESP_LOGI(TAG, "microphones on");
        } else {
            ESP_LOGE(TAG, "microphones: open failed");
        }
    }
    bool on = AU.mic_on;
    xSemaphoreGive(AU.mic_lock);
    return on;
}

void hal_mic_stop(void)
{
    if (!AU.mic_lock) return;
    xSemaphoreTake(AU.mic_lock, portMAX_DELAY);
    if (AU.mic_on) {
        esp_codec_dev_close(AU.mic); /* the ES7210 powered down, the RX channel stopped */
        AU.mic_on = false;
        AU.mic_level = 0;
        ESP_LOGI(TAG, "microphones off");
    }
    xSemaphoreGive(AU.mic_lock);
}

bool hal_mic_on(void) { return AU.mic_on; }
float hal_mic_level(void) { return AU.mic_level; }
void hal_mic_channel(int ch) { AU.chan = ch < -1 || ch > 2 ? -1 : ch; }

int hal_mic_read(int16_t *out, int max)
{
    if (!AU.mic_on || !AU.raw || max < MIC_CHUNK / 3) return 0;
    xSemaphoreTake(AU.mic_lock, portMAX_DELAY);
    if (!AU.mic_on) {
        xSemaphoreGive(AU.mic_lock);
        return 0;
    }
    int frames = MIC_CHUNK;
    if (frames > max * 3) frames = max * 3;
    int r = esp_codec_dev_read(AU.mic, AU.raw, frames * 4);
    xSemaphoreGive(AU.mic_lock);
    if (r != ESP_CODEC_DEV_OK) return -1;
    /* which channel: each one's energy, slowly; auto keeps the louder with some hysteresis */
    double e0 = 0, e1 = 0;
    for (int i = 0; i < frames; i++) {
        float l = AU.raw[2 * i], rr = AU.raw[2 * i + 1];
        e0 += l * l;
        e1 += rr * rr;
    }
    AU.ms[0] += ((float)(e0 / frames) - AU.ms[0]) * 0.01f;
    AU.ms[1] += ((float)(e1 / frames) - AU.ms[1]) * 0.01f;
    if (AU.chan >= 0) AU.use = AU.chan;
    else if (AU.ms[AU.use ^ 1] > AU.ms[AU.use] * 1.6f) AU.use ^= 1;
    int n = 0;
    double acc = 0;
    for (int i = 0; i < frames; i++) {
        float x = AU.use == 2 ? 0.5f * (AU.raw[2 * i] + AU.raw[2 * i + 1]) : AU.raw[2 * i + (AU.use & 1)];
        AU.hist[AU.hpos] = x;
        AU.hpos = (AU.hpos + 1) % FIR_N;
        if (++AU.phase < 3) continue;
        AU.phase = 0;
        float y = 0;
        for (int k = 0, p = AU.hpos; k < FIR_N; k++, p = (p + 1) % FIR_N) y += AU.fir[k] * AU.hist[p];
        /* DC out: the ADC's offset would read as constant "sound" to the voice detector */
        float hp = y - AU.dc_x + 0.995f * AU.dc_y;
        AU.dc_x = y;
        AU.dc_y = hp;
        int s = (int)lrintf(hp);
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        out[n++] = (int16_t)s;
        acc += (double)hp * hp;
    }
    float v = n ? sqrtf((float)(acc / n)) / 3000.0f : 0;
    AU.mic_level = v > 1 ? 1 : v;
    return n;
}

/* ------------------------------------------------------------------ wake word */

bool hal_wake_ready(const char **word)
{
    if (!AU.wn_tried) {
        AU.wn_tried = true;
        srmodel_list_t *models = esp_srmodel_init("model");
        char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp") : NULL;
        if (!name) name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
        if (name) {
            AU.wn = esp_wn_handle_from_name(name);
            AU.wd = AU.wn ? AU.wn->create(name, DET_MODE_90) : NULL;
        }
        if (AU.wd) {
            AU.wn_chunk = AU.wn->get_samp_chunksize(AU.wd);
            AU.wn_buf = heap_caps_malloc((size_t)AU.wn_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            char *w = esp_wn_wakeword_from_name(name);
            snprintf(AU.wn_name, sizeof AU.wn_name, "%s", w && w[0] ? w : "Hi ESP");
            ESP_LOGI(TAG, "wake word %s (%s), %d samples at %d Hz a chunk", AU.wn_name, name, AU.wn_chunk,
                     AU.wn->get_samp_rate(AU.wd));
        } else {
            ESP_LOGW(TAG, "no wake-word model (is the model partition flashed?): tap to talk only");
        }
        if (!AU.wn_buf) AU.wd = NULL;
    }
    if (word) *word = AU.wd ? AU.wn_name : NULL;
    return AU.wd != NULL;
}

void hal_wake_reset(void)
{
    AU.wn_fill = 0;
    if (AU.wd && AU.wn->clean) AU.wn->clean(AU.wd);
}

bool hal_wake_feed(const int16_t *pcm, int n)
{
    if (!AU.wd) return false;
    bool hit = false;
    while (n > 0) {
        int k = AU.wn_chunk - AU.wn_fill;
        if (k > n) k = n;
        memcpy(AU.wn_buf + AU.wn_fill, pcm, (size_t)k * sizeof(int16_t));
        AU.wn_fill += k;
        pcm += k;
        n -= k;
        if (AU.wn_fill == AU.wn_chunk) {
            AU.wn_fill = 0;
            if (AU.wn->detect(AU.wd, AU.wn_buf) == WAKENET_DETECTED) hit = true;
        }
    }
    return hit;
}

/* ------------------------------------------------------------------ init */

void hal_audio_init(void)
{
    AU.spk = bsp_audio_codec_speaker_init();
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
    if (AU.spk) {
        esp_codec_dev_open(AU.spk, &fs);
        esp_codec_dev_set_out_vol(AU.spk, 70);
    }
    /* the microphones, opened once in their final format so the RX DMA ring is sized now, then shut */
    AU.mic_lock = xSemaphoreCreateMutex();
    AU.mic = bsp_audio_codec_microphone_init();
    if (AU.mic) {
        if (esp_codec_dev_open(AU.mic, &fs) == ESP_CODEC_DEV_OK) esp_codec_dev_close(AU.mic);
        else ESP_LOGW(TAG, "microphones: first open failed");
    } else {
        ESP_LOGW(TAG, "microphones: no ES7210");
    }
    AU.raw = heap_caps_malloc(MIC_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    fir_design();
    AU.pcm = xStreamBufferCreateWithCaps(PCM_BYTES, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    AU.tones = xQueueCreate(8, sizeof(tone_t));
    /* above the workers (4) and NetworkTables (5), below the SDIO and lwIP tasks: the speaker is a
     * deadline, 10 ms at a time */
    if (AU.spk) xTaskCreatePinnedToCore(out_task, "audio", 4096, NULL, 6, &AU.task, 0);
}
