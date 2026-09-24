/* The music player: MP3 and WAV files in <sd>/CATOS/AUDIO, one after another, out through hal_play.h.
 *
 * WAV (PCM 8/16/24/32-bit, mono or stereo) is read here; MP3 goes through Espressif's esp_audio_codec
 * (its simple decoder, which finds the frames itself and skips ID3 tags) on the tablet. The simulator has
 * no codec: WAV only there. One thread ("play") decodes while something plays and ends with the queue;
 * the folder listing runs on the home worker. Big buffers are malloc'd (PSRAM on the tablet). */
#include "hal_play.h"
#include "home_priv.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#define HAVE_MP3 1
#else
#define HAVE_MP3 0
#endif

#define IN_SZ 4096
#define PCM_FRAMES 1152
#define PLAY_STACK 10240

typedef struct {
    char (*names)[64];       /* HOME_TRACKS */
    int count;               /* -1 while scanning */
    unsigned list_gen;
    bool scan_req;
    home_play_t st;
    int req;                 /* a track asked for (-1 none) */
    bool stop, paused, thread;
    char dir[96];
} pl_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pl_t *L;

static pl_t *pl(void)
{
    if (!L) {
        L = calloc(1, sizeof *L);
        if (L) {
            L->names = calloc(HOME_TRACKS, sizeof *L->names);
            L->req = -1;
            L->st.index = -1;
            L->st.duration = -1;
            L->st.mp3 = HAVE_MP3;
        }
    }
    return L && L->names ? L : NULL;
}

const char *home_player_dir(void)
{
    const char *root = hal_sd_root();
    if (!root || !pl()) return NULL;
    snprintf(L->dir, sizeof L->dir, "%s/CATOS/AUDIO", root);
    return L->dir;
}

static bool is_audio(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".mp3"));
}

static int by_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

void home_player_rescan(void)
{
    pthread_mutex_lock(&g_lock);
    if (pl()) {
        L->scan_req = true;
        L->count = -1;
    }
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

int home_player_tracks(char names[][64], int max, unsigned *gen)
{
    pthread_mutex_lock(&g_lock);
    int n = -1;
    if (pl()) {
        n = L->count;
        for (int i = 0; i < n && i < max; i++) memcpy(names[i], L->names[i], 64);
        if (n > max) n = max;
        if (gen) *gen = L->list_gen;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* on the home worker */
void home_player_work(double now)
{
    (void)now;
    pthread_mutex_lock(&g_lock);
    bool go = L && L->scan_req;
    if (go) L->scan_req = false;
    pthread_mutex_unlock(&g_lock);
    if (!go) return;
    char dir[96];
    const char *d = home_player_dir();
    snprintf(dir, sizeof dir, "%s", d ? d : "");
    char (*found)[64] = calloc(HOME_TRACKS, 64);
    int n = 0;
    if (found && dir[0]) {
        DIR *dp = opendir(dir);
        if (!dp) { /* make the folder, so the owner knows where to put music */
            char catos[96];
            snprintf(catos, sizeof catos, "%s/CATOS", hal_sd_root());
            mkdir(catos, 0755);
            mkdir(dir, 0755);
        } else {
            struct dirent *e;
            while ((e = readdir(dp)) && n < HOME_TRACKS) {
                if (e->d_name[0] == '.' || !is_audio(e->d_name) || strlen(e->d_name) >= 64) continue;
                snprintf(found[n++], 64, "%s", e->d_name);
            }
            closedir(dp);
            qsort(found, (size_t)n, 64, by_name);
        }
    }
    pthread_mutex_lock(&g_lock);
    if (found) memcpy(L->names, found, (size_t)HOME_TRACKS * 64);
    L->count = n;
    L->list_gen++;
    pthread_mutex_unlock(&g_lock);
    free(found);
}

void home_player_get(home_play_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (pl()) *out = L->st;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_lock);
}

static void set_state(home_play_state_t s, const char *err)
{
    pthread_mutex_lock(&g_lock);
    L->st.state = s;
    snprintf(L->st.err, sizeof L->st.err, "%s", err ? err : "");
    L->st.gen++;
    pthread_mutex_unlock(&g_lock);
}

/* ---- WAV ---- */

typedef struct {
    int rate, ch, bits;
    long data_at;
    uint32_t data_len;
} wav_t;

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool wav_open(FILE *f, wav_t *w, char *err, size_t en)
{
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        snprintf(err, en, "not a WAV file");
        return false;
    }
    bool fmt = false;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char b[40] = { 0 };
            size_t take = len < sizeof b ? len : sizeof b;
            if (fread(b, 1, take, f) != take) break;
            if (len > take) fseek(f, (long)(len - take), SEEK_CUR);
            int tag = le16(b);
            w->ch = le16(b + 2);
            w->rate = (int)le32(b + 4);
            w->bits = le16(b + 14);
            if (tag == 0xFFFE && len >= 26) tag = le16(b + 24); /* WAVE_FORMAT_EXTENSIBLE: the real tag */
            if (tag != 1) {
                snprintf(err, en, "only plain PCM WAV plays (this one is compressed)");
                return false;
            }
            fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt) break;
            w->data_at = ftell(f);
            w->data_len = len;
            if (w->ch < 1 || w->ch > 2 || w->rate < 8000 || w->rate > 48000 ||
                (w->bits != 8 && w->bits != 16 && w->bits != 24 && w->bits != 32)) {
                snprintf(err, en, "%d Hz, %d channels, %d-bit: not something this can play", w->rate, w->ch, w->bits);
                return false;
            }
            return true;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    snprintf(err, en, "a WAV file without its audio");
    return false;
}

/* bytes of any supported depth → 16-bit, in place-safe order (out never overtakes in) */
static int to_s16(const unsigned char *in, int bytes, int bits, int16_t *out)
{
    int n = 0;
    if (bits == 16) {
        n = bytes / 2;
        memmove(out, in, (size_t)n * 2);
    } else if (bits == 8) {
        n = bytes;
        for (int i = n - 1; i >= 0; i--) out[i] = (int16_t)((in[i] - 128) << 8);
    } else if (bits == 24) {
        n = bytes / 3;
        for (int i = 0; i < n; i++) out[i] = (int16_t)(in[3 * i + 1] | in[3 * i + 2] << 8);
    } else if (bits == 32) {
        n = bytes / 4;
        for (int i = 0; i < n; i++) out[i] = (int16_t)(in[4 * i + 2] | in[4 * i + 3] << 8);
    }
    return n;
}

/* ---- the play loop ---- */

typedef enum { GO_ON, GO_STOP, GO_SWITCH } go_t;

/* Pauses (writing nothing) while paused; says whether to stop or switch track. */
static go_t checkpoint(void)
{
    for (;;) {
        pthread_mutex_lock(&g_lock);
        go_t g = L->stop ? GO_STOP : L->req >= 0 ? GO_SWITCH : GO_ON;
        bool paused = L->paused;
        pthread_mutex_unlock(&g_lock);
        if (g != GO_ON || !paused) return g;
        usleep(40 * 1000);
    }
}

static void progress(double pos, double dur)
{
    pthread_mutex_lock(&g_lock);
    L->st.position = pos;
    if (dur >= 0) L->st.duration = dur;
    pthread_mutex_unlock(&g_lock);
}

/* Writes it all (or until told to stop); false when the output broke. */
static bool out(const int16_t *pcm, int frames, int ch, bool *opened, int rate)
{
    if (!*opened) {
        if (!hal_play_open(rate, ch)) return false;
        *opened = true;
    }
    while (frames > 0) {
        if (checkpoint() != GO_ON) return true;
        int w = hal_play_write(pcm, frames, 200);
        if (w < 0) return false;
        pcm += (size_t)w * (size_t)ch;
        frames -= w;
    }
    return true;
}

static go_t play_wav(FILE *f, char *err, size_t en, bool *opened)
{
    wav_t w = { 0 };
    if (!wav_open(f, &w, err, en)) return GO_STOP;
    int fb = w.ch * (w.bits / 8);
    double dur = (double)w.data_len / fb / w.rate;
    progress(0, dur);
    unsigned char *buf = malloc(IN_SZ * 2);
    if (!buf) return GO_STOP;
    uint64_t done = 0;
    bool ok = true;
    uint32_t left = w.data_len;
    go_t g = GO_ON;
    while (left > 0) {
        if ((g = checkpoint()) != GO_ON) break;
        size_t want = left < (uint32_t)(IN_SZ / fb * fb) ? left : (uint32_t)(IN_SZ / fb * fb);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        left -= (uint32_t)got;
        int samples = to_s16(buf, (int)got, w.bits, (int16_t *)buf);
        int frames = samples / w.ch;
        if (!(ok = out((int16_t *)buf, frames, w.ch, opened, w.rate))) break;
        done += (uint64_t)frames;
        progress((double)done / w.rate, -1);
    }
    free(buf);
    if (!ok) snprintf(err, en, *opened ? "the speaker stopped taking audio" : "no audio output in this build");
    return !ok ? GO_STOP : g == GO_ON ? GO_ON : g;
}

#if HAVE_MP3
static go_t play_mp3(FILE *f, char *err, size_t en, bool *opened)
{
    static bool registered;
    if (!registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registered = true;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    esp_audio_simple_dec_cfg_t cfg = { .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        snprintf(err, en, "the MP3 decoder wouldn't start");
        return GO_STOP;
    }
    uint32_t out_max = PCM_FRAMES * 2 * 2 * 2;
    uint8_t *in = malloc(IN_SZ), *pcm = malloc(out_max);
    go_t g = GO_ON;
    bool ok = true, eof = false;
    uint64_t frames_done = 0;
    esp_audio_simple_dec_info_t info = { 0 };
    esp_audio_simple_dec_raw_t raw = { 0 };
    while (in && pcm && ok && (g = checkpoint()) == GO_ON) {
        if (raw.len == 0) {
            if (eof) break;
            size_t got = fread(in, 1, IN_SZ, f);
            eof = got < IN_SZ;
            raw.buffer = in;
            raw.len = (uint32_t)got;
            raw.eos = eof;
            if (got == 0) break;
        }
        esp_audio_simple_dec_out_t o = { .buffer = pcm, .len = out_max };
        esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &o);
        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *np = realloc(pcm, o.needed_size);
            if (!np) break;
            pcm = np;
            out_max = o.needed_size;
            continue;
        }
        if (r != ESP_AUDIO_ERR_OK) {
            if (frames_done == 0) snprintf(err, en, "this MP3 couldn't be decoded (%d)", (int)r);
            break; /* a broken tail ends the track, it doesn't stop the player */
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (o.decoded_size == 0) continue;
        if (info.sample_rate == 0) {
            esp_audio_simple_dec_get_info(dec, &info);
            double dur = info.bitrate > 0 ? (double)size * 8 / info.bitrate : -1;
            progress(0, dur);
        }
        if (info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2) {
            snprintf(err, en, "an MP3 this can't play (%d-bit, %d channels)", info.bits_per_sample, info.channel);
            break;
        }
        int frames = (int)(o.decoded_size / (2u * info.channel));
        ok = out((const int16_t *)pcm, frames, info.channel, opened, (int)info.sample_rate);
        frames_done += (uint64_t)frames;
        progress((double)frames_done / info.sample_rate, -1);
    }
    esp_audio_simple_dec_close(dec);
    free(in);
    free(pcm);
    if (!ok) snprintf(err, en, *opened ? "the speaker stopped taking audio" : "no audio output in this build");
    if (!ok || err[0]) return GO_STOP;
    return g;
}
#endif

static void *player_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int idx = L->req;
        L->req = -1;
        bool stop = L->stop;
        L->stop = false;
        char name[64] = "";
        if (!stop && idx >= 0 && idx < L->count) memcpy(name, L->names[idx], 64);
        if (stop || !name[0]) {
            L->thread = false;
            L->paused = false;
            if (stop || idx < 0) {
                L->st.state = HP_IDLE;
                L->st.position = 0;
            }
            L->st.gen++;
            pthread_mutex_unlock(&g_lock);
            break;
        }
        L->paused = false;
        L->st.index = idx;
        L->st.state = HP_PLAYING;
        L->st.position = 0;
        L->st.duration = -1;
        L->st.err[0] = 0;
        snprintf(L->st.title, sizeof L->st.title, "%s", name);
        char *dot = strrchr(L->st.title, '.');
        if (dot) *dot = 0;
        home_fold_text(L->st.title);
        L->st.gen++;
        char path[180];
        snprintf(path, sizeof path, "%s/%s", L->dir, name);
        pthread_mutex_unlock(&g_lock);

        char err[80] = "";
        bool opened = false;
        go_t g = GO_STOP;
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(err, sizeof err, "couldn't open it (was the card taken out?)");
        } else {
            const char *ext = strrchr(name, '.');
            if (ext && !strcasecmp(ext, ".wav")) g = play_wav(f, err, sizeof err, &opened);
#if HAVE_MP3
            else g = play_mp3(f, err, sizeof err, &opened);
#else
            else snprintf(err, sizeof err, "MP3 plays on the tablet only (the simulator has no decoder)");
#endif
            fclose(f);
        }
        if (opened) hal_play_close();
        if (err[0]) {
            set_state(HP_ERROR, err);
            pthread_mutex_lock(&g_lock);
            L->thread = false;
            L->req = -1;
            L->stop = false;
            pthread_mutex_unlock(&g_lock);
            break;
        }
        pthread_mutex_lock(&g_lock);
        if (g == GO_ON && L->req < 0 && !L->stop) L->req = idx + 1 < L->count ? idx + 1 : -1; /* the next one */
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void home_player_play(int index)
{
    bool start = false;
    pthread_mutex_lock(&g_lock);
    if (pl() && index >= 0 && index < L->count) {
        home_player_dir();
        L->req = index;
        L->paused = false;
        L->st.state = HP_LOADING;
        L->st.err[0] = 0;
        L->st.gen++;
        if (!L->thread) start = L->thread = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (start && !hal_thread("play", player_main, NULL, PLAY_STACK)) {
        pthread_mutex_lock(&g_lock);
        L->thread = false;
        L->st.state = HP_ERROR;
        snprintf(L->st.err, sizeof L->st.err, "no memory to start playing");
        L->st.gen++;
        pthread_mutex_unlock(&g_lock);
    }
}

void home_player_toggle(void)
{
    pthread_mutex_lock(&g_lock);
    if (!pl()) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    bool running = L->thread;
    int idx = L->st.index;
    if (running) {
        L->paused = !L->paused;
        L->st.state = L->paused ? HP_PAUSED : HP_PLAYING;
        L->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!running) home_player_play(idx >= 0 ? idx : 0);
}

void home_player_skip(int dir)
{
    pthread_mutex_lock(&g_lock);
    int n = L ? L->count : 0, idx = L ? L->st.index : -1;
    pthread_mutex_unlock(&g_lock);
    if (n <= 0) return;
    int next = idx < 0 ? 0 : (idx + dir + n) % n;
    home_player_play(next);
}

void home_player_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (pl() && L->thread) L->stop = true;
    else if (L) {
        L->st.state = HP_IDLE;
        L->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
}
