/* voice — the companion's conversation: wake word, recording, transcription, the answer, speech.
 * voice.h has the shape of it. Portable C over hal.h (the simulator has no microphones, so there it only
 * answers typed questions, and "speaks" into nothing). */
#include "voice.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "as_json.h"
#include "as_oai.h"
#include "as_tools.h"
#include "assist.h"
#include "ccwatch.h"
#include "hal.h"
#include "link.h"

#define SR 16000
#define CHUNK 320                  /* 20 ms at 16 kHz: what one hal_mic_read gives */
#define REC_MAX (SR * 15)          /* the longest utterance */
#define PREROLL (SR * 3 / 10)      /* kept from before the voice starts, so its onset isn't clipped */
#define HIST_MAX 16                /* messages remembered (8 exchanges) */
#define HIST_IDLE_S 600.0          /* quiet this long: the next question starts a new conversation */
#define TOOL_ROUNDS 4
#define TTS_RATE 24000             /* OpenAI's "pcm": 24 kHz, 16-bit, mono, little-endian */
/* connect, and each read: a spoken answer from gpt-4o-mini comes in a few seconds; a request that has heard
 * nothing for this long is lost (the HAL retries only a request that failed quickly) */
#define HTTP_TIMEOUT_MS 40000

static const char SYSTEM_PROMPT[] =
    "You are the companion that lives in Catalyst Tab, a small tablet standing on its owner's desk. The owner is on an "
    "FRC robotics team, and the tablet is also their pit diagnostics tool. You have a face: two expressive eyes on the "
    "screen. You are warm, curious and a little playful, and you are also a capable general assistant: answer anything "
    "well, from facts and maths to code, writing, planning and advice.\n"
    "\n"
    "Your words are spoken aloud by a text-to-speech voice and may also be shown on the screen, so:\n"
    "- Talk like a person in conversation: short, natural sentences. Usually one to three of them; go longer only when "
    "asked for detail or when the answer truly needs it.\n"
    "- Plain words only: no markdown, lists, headings, emoji, links or code blocks. Write numbers, units and symbols "
    "the way they are spoken.\n"
    "- The question was transcribed from speech. If it looks garbled or cut off, say so briefly and ask again rather "
    "than guessing.\n"
    "- Be honest about what you don't know.\n"
    "\n"
    "Tools: you can read the robot (connection, alerts, mechanisms, power, CAN, logs, preflight, Systemcore), the team's "
    "matches at its event (The Blue Alliance: times, alliance, partners, results, rank), the team's battery fleet and which "
    "battery should go in next, the robot's code through Catalyst Link on the PC, and the Claude Code sessions running on "
    "the owner's PC. Use them when the question "
    "is about those things; never guess a robot fact you could look up. You cannot change anything; for changes, send the "
    "owner to the Assist app on the tablet.\n"
    "\n"
    "A message may begin with a bracketed note from the tablet (time, robot and PC state). The tablet writes it, not "
    "the owner; use it only when it matters.\n"
    "\n"
    "Always reply with a JSON object: {\"say\": what you say, \"emotion\": how you honestly feel about this moment, "
    "\"intensity\": 0 to 1, \"look\": where your eyes glance}. Emotions: neutral, happy, excited, curious, thinking, "
    "sleepy, sad, worried, surprised, love, proud, confused. Choose from the content, honestly: good news is happy or "
    "excited, a problem is worried, a puzzle is curious or thinking, kind words are love, a success is proud, the "
    "unexpected is surprised, a sad topic is sad, not understanding is confused; plain facts are neutral. Intensity "
    "0.2 to 0.5 is mild, 0.6 to 1 strong. look is center unless a glance fits: up while recalling, down when shy or "
    "sad, left or right when pointing at something.";

static const char *EMO[VO_EMO_COUNT] = { "neutral", "happy", "excited", "curious", "thinking", "sleepy",
                                         "sad", "worried", "surprised", "love", "proud", "confused" };
static const char *LOOK[] = { "center", "left", "right", "up", "down" };

/* how each feeling should colour the voice (gpt-4o-mini-tts takes instructions) */
static const char *TONE[VO_EMO_COUNT] = {
    "calm and friendly", "cheerful and warm", "excited and upbeat", "curious and interested",
    "thoughtful, a little slower", "sleepy and soft", "gentle and a little sad", "concerned and careful",
    "surprised", "affectionate and warm", "proud and bright", "puzzled",
};

typedef struct {
    bool user;
    char *text;
} hmsg_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool started, enabled, ptt, cancel;
    bool working;                  /* the worker is in a turn (hearing, answering, speaking) */
    char *ask;                     /* a typed question waiting for the worker */
    vo_state_t state;
    voice_config_t cfg;
    char base[128];
    vo_reply_t *reply;             /* the last answer (on the heap: 1.5 KB of internal RAM otherwise) */
    char note[160];
    double note_at;
    bool note_err;                 /* the note says something went wrong (the face shows it) */
    const char *wake_word;
    bool wake_loaded;
    float floor;                   /* the room's noise, mean square, as the microphones hear it */
    double last_turn;
    bool clear_hist;               /* voice_reset: forget before the next question */
    bool stt_fallback, tts_fallback;
    uint32_t rev;
} V = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static hmsg_t g_hist[HIST_MAX];
static int g_nhist;

static void bump(void) { __atomic_add_fetch(&V.rev, 1, __ATOMIC_RELAXED); }
uint32_t voice_rev(void) { return __atomic_load_n(&V.rev, __ATOMIC_RELAXED); }

static void set_state(vo_state_t s)
{
    pthread_mutex_lock(&V.lock);
    /* A cancelled turn still waiting on the network shows nothing of itself (voice_cancel made the face idle);
     * a typed question queued behind it shows as thinking until its turn comes. */
    if (V.cancel && s != VO_IDLE && s != VO_OFF) s = V.state;
    if (V.ask && s == VO_IDLE) s = VO_THINKING;
    bool ch = V.state != s;
    V.state = s;
    pthread_mutex_unlock(&V.lock);
    if (ch) bump();
}

static void vnote(bool err, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&V.lock);
    vsnprintf(V.note, sizeof V.note, fmt, ap);
    V.note_at = hal_seconds();
    V.note_err = err;
    pthread_mutex_unlock(&V.lock);
    bump();
}

/* a passing word for the status line ("looking: get_alerts") */
static void set_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vnote(false, fmt, ap);
    va_end(ap);
}

/* something went wrong: the face looks worried, a low chime, and the words stay a while */
static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vnote(true, fmt, ap);
    va_end(ap);
}

/* the network as the reason a request failed, in plain words */
static bool net_down(void)
{
    hal_net_t net;
    hal_net(&net);
    return !net.up;
}

/* the turn in flight should end: cancelled, or desk mode left */
static bool stopped(void)
{
    pthread_mutex_lock(&V.lock);
    bool s = V.cancel || !V.enabled;
    pthread_mutex_unlock(&V.lock);
    return s;
}

/* like stopped(), but a typed question may run with desk mode closed */
static bool cancelled(void)
{
    pthread_mutex_lock(&V.lock);
    bool s = V.cancel;
    pthread_mutex_unlock(&V.lock);
    return s;
}

const char *voice_emotion_name(vo_emotion_t e) { return e < VO_EMO_COUNT ? EMO[e] : "neutral"; }

/* ------------------------------------------------------------------ settings */

static void load_config(void)
{
    char v[64];
    voice_config_t c = { .out = VO_OUT_BOTH, .wake = true, .follow = true };
    if (hal_kv_get("v_out", v, sizeof v)) c.out = !strcmp(v, "speak") ? VO_OUT_SPEAK : !strcmp(v, "show") ? VO_OUT_SHOW : VO_OUT_BOTH;
    if (hal_kv_get("v_wake", v, sizeof v)) c.wake = strcmp(v, "0") != 0;
    if (hal_kv_get("v_follow", v, sizeof v)) c.follow = strcmp(v, "0") != 0;
    if (hal_kv_get("v_chat", v, sizeof v)) snprintf(c.chat_model, sizeof c.chat_model, "%s", v);
    if (hal_kv_get("v_stt", v, sizeof v)) snprintf(c.stt_model, sizeof c.stt_model, "%s", v);
    if (hal_kv_get("v_tts", v, sizeof v)) snprintf(c.tts_model, sizeof c.tts_model, "%s", v);
    if (hal_kv_get("v_voice", v, sizeof v)) snprintf(c.voice, sizeof c.voice, "%s", v);
    if (hal_kv_get("v_lang", v, sizeof v)) snprintf(c.lang, sizeof c.lang, "%s", v);
    if (hal_kv_get("v_mic", v, sizeof v) && v[0]) hal_mic_channel(atoi(v));
    char base[128] = "";
    hal_kv_get("oai_base", base, sizeof base);
    pthread_mutex_lock(&V.lock);
    V.cfg = c;
    snprintf(V.base, sizeof V.base, "%s", base[0] ? base : AS_OAI_DEFAULT_BASE);
    size_t l = strlen(V.base);
    while (l && V.base[l - 1] == '/') V.base[--l] = 0;
    pthread_mutex_unlock(&V.lock);
}

void voice_config(voice_config_t *out)
{
    pthread_mutex_lock(&V.lock);
    *out = V.cfg;
    pthread_mutex_unlock(&V.lock);
}

void voice_set_config(const voice_config_t *c)
{
    pthread_mutex_lock(&V.lock);
    V.cfg = *c;
    pthread_mutex_unlock(&V.lock);
    hal_kv_set("v_out", c->out == VO_OUT_SPEAK ? "speak" : c->out == VO_OUT_SHOW ? "show" : "both");
    hal_kv_set("v_wake", c->wake ? "1" : "0");
    hal_kv_set("v_follow", c->follow ? "1" : "0");
    hal_kv_set("v_chat", c->chat_model);
    hal_kv_set("v_stt", c->stt_model);
    hal_kv_set("v_tts", c->tts_model);
    hal_kv_set("v_voice", c->voice);
    hal_kv_set("v_lang", c->lang);
    pthread_cond_signal(&V.wake);
    bump();
}

/* the key, copied off the assistant's configuration; the caller wipes and frees it */
static char *oai_key(void)
{
    assist_config_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    assist_config(c);
    char *k = c->oai_key[0] ? as_strdup(c->oai_key) : NULL;
    memset(c, 0, sizeof *c);
    free(c);
    return k;
}

static void drop_key(char *k)
{
    if (!k) return;
    memset(k, 0, strlen(k));
    free(k);
}

bool voice_ready(char *why, size_t n)
{
    char *k = oai_key();
    bool key = k != NULL;
    drop_key(k);
    if (!key) {
        snprintf(why, n, "no OpenAI key: add one in settings, assistant");
        return false;
    }
    if (net_down()) {
        snprintf(why, n, "Wi-Fi isn't connected: I need the internet to talk");
        return false;
    }
    if (n) why[0] = 0;
    return true;
}

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    int status;
    char *body;                    /* NUL-terminated, malloc'd (PSRAM when large) */
    int len;
    char err[96];
} resp_t;

static void resp_free(resp_t *r)
{
    free(r->body);
    r->body = NULL;
}

/* POSTs to base + path with the key, the given content type and body; the whole answer into r. */
static bool post(const char *path, const char *ctype, const char *body, size_t len, int max, resp_t *r)
{
    memset(r, 0, sizeof *r);
    r->status = -1;
    char url[200], *hdr = malloc(512);
    char *key = oai_key();
    if (!hdr || !key) {
        snprintf(r->err, sizeof r->err, "%s", key ? "no memory" : "no OpenAI key");
        free(hdr);
        drop_key(key);
        return false;
    }
    pthread_mutex_lock(&V.lock);
    snprintf(url, sizeof url, "%s%s", V.base, path);
    pthread_mutex_unlock(&V.lock);
    snprintf(hdr, 512, "Authorization: Bearer %s\r\nContent-Type: %s\r\n", key, ctype);
    drop_key(key);
    hal_http_req_t rq = { .method = "POST", .url = url, .headers = hdr, .body = body, .body_len = len,
                          .timeout_ms = HTTP_TIMEOUT_MS };
    hal_http_t *h = hal_http_open(&rq, &r->status, r->err, sizeof r->err);
    memset(hdr, 0, 512);
    free(hdr);
    if (!h) return false;
    int cap = 4096;
    r->body = malloc((size_t)cap + 1);
    int rd = 0;
    while (r->body) {
        if (r->len == cap) {
            if (cap >= max) break;
            cap *= 2;
            char *p = realloc(r->body, (size_t)cap + 1);
            if (!p) break;
            r->body = p;
        }
        rd = hal_http_read(h, r->body + r->len, cap - r->len);
        if (rd <= 0 || cancelled()) break;
        r->len += rd;
    }
    hal_http_close(h);
    if (r->body) r->body[r->len] = 0;
    if (rd < 0 && r->status == 200) {
        snprintf(r->err, sizeof r->err, "the connection dropped");
        return false;
    }
    return r->body != NULL;
}

/* A failure in words for the status line. Never the body of a 401: OpenAI echoes part of the key. */
static void describe(const resp_t *r, const char *what)
{
    if (r->status < 0 || (r->status == 200 && r->err[0])) {
        /* the reason in the log; on screen, what it means for the owner */
        printf("voice: %s: %s (status %d)\n", what, r->err[0] ? r->err : "no answer", r->status);
        if (net_down()) set_err("the Wi-Fi dropped: ask me again once it's back");
        else if (r->status == 200) set_err("the connection dropped in the middle of %s: ask me again", what);
        else set_err("couldn't reach OpenAI: the network didn't answer. Try again in a moment");
        return;
    }
    if (r->status == 401) {
        set_err("OpenAI refused the key (401): check it in settings, assistant");
        return;
    }
    char msg[160] = "", code[48] = "";
    if (r->body) as_oai_error(r->body, msg, sizeof msg, code, sizeof code);
    if (r->status == 429 && !strcmp(code, "insufficient_quota")) set_err("OpenAI: this key's account is out of credit");
    else if (r->status == 429) set_err("OpenAI is rate-limiting this key: try again in a moment");
    else if (r->status >= 500) set_err("OpenAI is having trouble (HTTP %d): try again in a moment", r->status);
    else set_err("%s failed (HTTP %d)%s%.100s", what, r->status, msg[0] ? ": " : "", msg);
}

/* ------------------------------------------------------------------ listening */

static float mean_square(const int16_t *s, int n)
{
    double a = 0;
    for (int i = 0; i < n; i++) a += (double)s[i] * s[i];
    return n ? (float)(a / n) : 0;
}

/* the room's noise, followed slowly upward and quickly downward, while nobody speaks */
static void floor_track(float e)
{
    float f = V.floor;
    if (f <= 0) f = e;
    else if (e < f) f += (e - f) * 0.2f;
    else f += (e - f) * 0.01f;
    if (f < 40.0f * 40.0f) f = 40.0f * 40.0f;
    V.floor = f;
}

/* Records one utterance into rec: waits up to wait_s for a voice to start, then until ~0.8 s of quiet. The
 * first skip_ms are ignored (the listening chime, still in the air). Returns the samples kept, 0 when nothing
 * was said, -1 when stopped or the microphones failed. */
static int record(int16_t *rec, int16_t *chunk, double wait_s, int skip_ms)
{
    set_state(VO_LISTEN);
    int n = 0, pre = 0, run = 0, quiet_ms = 0, voiced = 0;
    bool started = false;
    double t0 = hal_seconds(), heard_at = t0;
    for (;;) {
        if (stopped()) return -1;
        int r = hal_mic_read(chunk, CHUNK);
        if (r < 0) {
            set_err("the microphones stopped: tap me to try again");
            return -1;
        }
        if (r == 0) {
            /* nothing from the microphones at all: they've stalled, and "listening..." would never end */
            if (hal_seconds() - heard_at > 2.0) {
                set_err("the microphones went quiet: tap me to try again");
                return -1;
            }
            usleep(10000);
            continue;
        }
        heard_at = hal_seconds();
        if (skip_ms > 0) {
            skip_ms -= r * 1000 / SR;
            continue;
        }
        float e = mean_square(chunk, r);
        float on = V.floor * 6.0f, off = V.floor * 3.0f;
        if (on < 280.0f * 280.0f) on = 280.0f * 280.0f;
        if (off < 180.0f * 180.0f) off = 180.0f * 180.0f;
        if (!started) {
            /* keep the last PREROLL samples at the front of rec, as a sliding window */
            if (pre + r > PREROLL) {
                int drop = pre + r - PREROLL;
                memmove(rec, rec + drop, (size_t)(pre - drop) * sizeof *rec);
                pre -= drop;
            }
            memcpy(rec + pre, chunk, (size_t)r * sizeof *rec);
            pre += r;
            run = e > on ? run + 1 : 0;
            if (run >= 3) { /* 60 ms of voice */
                started = true;
                n = pre;
                voiced = run;
                continue;
            }
            floor_track(e);
            if (hal_seconds() - t0 > wait_s) return 0;
            continue;
        }
        if (n + r > REC_MAX) break;
        memcpy(rec + n, chunk, (size_t)r * sizeof *rec);
        n += r;
        if (e > on) voiced++;
        quiet_ms = e < off ? quiet_ms + r * 1000 / SR : 0;
        if (quiet_ms >= 800) break;
    }
    if (voiced < 12) return 0; /* under a quarter second of voice: a cough, a knock */
    int tail = quiet_ms * SR / 1000 - SR / 5; /* keep 0.2 s of the pause */
    if (tail > 0 && tail < n) n -= tail;
    return n;
}

/* ------------------------------------------------------------------ transcription */

static void put_le(char *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) p[i] = (char)((v >> (8 * i)) & 0xff);
}

static void part(ab_t *b, const char *bound, const char *name, const char *value)
{
    ab_fmt(b, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", bound, name, value);
}

/* The recording as text, or NULL (with a note saying why). Caller frees. */
static char *transcribe(const int16_t *pcm, int n)
{
    voice_config_t c;
    voice_config(&c);
    const char *bound = "----catalyst-tab-voice-7d1f";
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&V.lock);
        bool fb = V.stt_fallback;
        pthread_mutex_unlock(&V.lock);
        const char *model = c.stt_model[0] ? c.stt_model : fb ? VO_STT_FALLBACK : VO_STT_DEFAULT;
        ab_t b;
        ab_init(&b);
        part(&b, bound, "model", model);
        part(&b, bound, "response_format", "json");
        if (c.lang[0]) part(&b, bound, "language", c.lang);
        ab_fmt(&b, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                   "Content-Type: audio/wav\r\n\r\n", bound);
        char h[44];
        uint32_t data = (uint32_t)n * 2;
        memcpy(h, "RIFF", 4); put_le(h + 4, 36 + data, 4); memcpy(h + 8, "WAVEfmt ", 8);
        put_le(h + 16, 16, 4); put_le(h + 20, 1, 2); put_le(h + 22, 1, 2); put_le(h + 24, SR, 4);
        put_le(h + 28, SR * 2, 4); put_le(h + 32, 2, 2); put_le(h + 34, 16, 2);
        memcpy(h + 36, "data", 4); put_le(h + 40, data, 4);
        ab_raw(&b, h, sizeof h);
        ab_raw(&b, (const char *)pcm, data); /* little-endian on both the tablet and a PC */
        ab_fmt(&b, "\r\n--%s--\r\n", bound);
        if (b.oom) {
            ab_free(&b);
            set_err("out of memory for the recording");
            return NULL;
        }
        char ctype[96];
        snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", bound);
        resp_t r;
        bool ok = post("/v1/audio/transcriptions", ctype, b.p, b.n, 64 * 1024, &r);
        ab_free(&b);
        if (cancelled()) {
            resp_free(&r);
            return NULL;
        }
        if (ok && r.status == 200) {
            aj_t *d = aj_parse(r.body, (size_t)r.len, NULL, 0);
            const char *t = aj_gets(d, "text");
            char *out = t ? as_strdup(t) : NULL;
            aj_free(d);
            resp_free(&r);
            return out;
        }
        /* the default model refused (not offered to this key): once more on whisper-1 */
        if (ok && !c.stt_model[0] && !fb && (r.status == 400 || r.status == 403 || r.status == 404)) {
            pthread_mutex_lock(&V.lock);
            V.stt_fallback = true;
            pthread_mutex_unlock(&V.lock);
            resp_free(&r);
            continue;
        }
        describe(&r, "hearing you");
        resp_free(&r);
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ the answer */

static void hist_add(bool user, const char *text)
{
    if (g_nhist == HIST_MAX) {
        /* the oldest exchange goes whole, so the history still starts with the owner */
        int drop = g_nhist > 1 && !g_hist[1].user ? 2 : 1;
        for (int i = 0; i < drop; i++) free(g_hist[i].text);
        memmove(g_hist, g_hist + drop, (size_t)(g_nhist - drop) * sizeof *g_hist);
        g_nhist -= drop;
    }
    g_hist[g_nhist].user = user;
    g_hist[g_nhist].text = as_strdup(text);
    if (g_hist[g_nhist].text) g_nhist++;
}

static void hist_clear(void)
{
    for (int i = 0; i < g_nhist; i++) free(g_hist[i].text);
    g_nhist = 0;
}

static void context_note(char *out, size_t n)
{
    char when[32] = "";
    struct tm tm;
    if (hal_rtc_get(&tm)) strftime(when, sizeof when, "%a %Y-%m-%d %H:%M", &tm);
    cat_robot_t *r = malloc(sizeof *r);
    char robot[120] = "robot not connected";
    if (r) {
        assist_robot(r);
        if (r->connected)
            snprintf(robot, sizeof robot, "robot %s connected, %s, %d errors, %d warnings", r->name, cat_mode_name(r),
                     r->n_errors, r->n_warnings);
        free(r);
    }
    ccw_session_t *s = malloc(sizeof *s * CCW_MAX);
    int ns = s ? ccw_list(s, CCW_MAX) : 0, running = 0, att = 0;
    for (int i = 0; i < ns; i++) {
        running += s[i].state == CCW_RUNNING;
        att += s[i].attention;
    }
    free(s);
    char cc[80] = "";
    if (ccw_available()) snprintf(cc, sizeof cc, " · Claude Code on the PC: %d running, %d need a look", running, att);
    /* the next match and the battery to use, as the UI last posted them (get_matches / get_batteries say more) */
    char *now = assist_desk_get(AS_DESK_NOW);
    snprintf(out, n, "[tablet: %s · %s%s%s%s]", when[0] ? when : "time unknown", robot, cc, now ? " · " : "",
             now ? now : "");
    free(now);
}

/* the read-only tools, as Chat Completions functions */
static char *tools_json(void)
{
    ab_t b;
    ab_init(&b);
    ab_puts(&b, "[");
    bool first = true;
    for (int i = 0; i < AS_NTOOLS; i++) {
        if (AS_TOOLS[i].kind != AS_T_READ) continue;
        ab_puts(&b, first ? "" : ",");
        first = false;
        ab_puts(&b, "{\"type\":\"function\",\"function\":{\"name\":");
        ab_str(&b, AS_TOOLS[i].name);
        ab_puts(&b, ",\"description\":");
        ab_str(&b, AS_TOOLS[i].description);
        ab_puts(&b, ",\"parameters\":");
        ab_puts(&b, AS_TOOLS[i].schema);
        ab_puts(&b, "}}");
    }
    ab_puts(&b, "]");
    return ab_take(&b);
}

static const char REPLY_SCHEMA[] =
    "{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"companion_reply\",\"strict\":true,\"schema\":{"
    "\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"say\",\"emotion\",\"intensity\",\"look\"],"
    "\"properties\":{"
    "\"say\":{\"type\":\"string\",\"description\":\"what the companion says aloud\"},"
    "\"emotion\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\",\"excited\",\"curious\",\"thinking\",\"sleepy\","
    "\"sad\",\"worried\",\"surprised\",\"love\",\"proud\",\"confused\"]},"
    "\"intensity\":{\"type\":\"number\",\"description\":\"0 to 1\"},"
    "\"look\":{\"type\":\"string\",\"enum\":[\"center\",\"left\",\"right\",\"up\",\"down\"]}}}}}";

/* The request: system, the remembered exchanges, this question, and this turn's tool rounds. fmt: 0 the
 * JSON schema, 1 plain JSON mode, 2 nothing (a model that takes neither). */
static void chat_body(ab_t *b, const char *model, const char *question, const aj_t *turn, const char *tools, int fmt)
{
    ab_reset(b);
    ab_puts(b, "{\"model\":");
    ab_str(b, model);
    ab_puts(b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    ab_str(b, SYSTEM_PROMPT);
    ab_puts(b, "}");
    for (int i = 0; i < g_nhist; i++) {
        ab_puts(b, g_hist[i].user ? ",{\"role\":\"user\",\"content\":" : ",{\"role\":\"assistant\",\"content\":");
        ab_str(b, g_hist[i].text);
        ab_puts(b, "}");
    }
    ab_puts(b, ",{\"role\":\"user\",\"content\":");
    ab_str(b, question);
    ab_puts(b, "}");
    for (int i = 0; turn && i < turn->n; i++) {
        ab_puts(b, ",");
        aj_write(b, turn->kid[i]);
    }
    ab_puts(b, "]");
    if (tools && tools[0]) {
        ab_puts(b, ",\"tools\":");
        ab_puts(b, tools);
    }
    if (fmt == 0) {
        ab_puts(b, ",\"response_format\":");
        ab_puts(b, REPLY_SCHEMA);
    } else if (fmt == 1) {
        ab_puts(b, ",\"response_format\":{\"type\":\"json_object\"}");
    }
    ab_puts(b, "}");
}

static int word_index(const char *s, const char *const *words, int n, int fallback)
{
    for (int i = 0; s && i < n; i++)
        if (!strcasecmp(s, words[i])) return i;
    return fallback;
}

/* The model's content into the reply: the JSON object, or whatever JSON-like object is in the text, or the
 * text itself as a neutral answer. */
static void parse_reply(const char *content, vo_reply_t *r)
{
    r->emotion = VO_EMO_NEUTRAL;
    r->intensity = 0.4f;
    r->look = VO_LOOK_CENTER;
    r->say[0] = 0;
    if (!content) return;
    aj_t *d = aj_parse(content, strlen(content), NULL, 0);
    if (!d) {
        const char *a = strchr(content, '{'), *z = strrchr(content, '}');
        if (a && z && z > a) d = aj_parse(a, (size_t)(z - a + 1), NULL, 0);
    }
    const char *say = d ? aj_gets(d, "say") : NULL;
    if (!say && d) say = aj_gets(d, "text");
    if (say) {
        snprintf(r->say, sizeof r->say, "%s", say);
        r->emotion = (vo_emotion_t)word_index(aj_gets(d, "emotion"), EMO, VO_EMO_COUNT, VO_EMO_NEUTRAL);
        double k = aj_getn(d, "intensity", 0.5);
        r->intensity = (float)(k < 0 ? 0 : k > 1 ? 1 : k);
        r->look = (vo_look_t)word_index(aj_gets(d, "look"), LOOK, 5, VO_LOOK_CENTER);
    } else {
        snprintf(r->say, sizeof r->say, "%s", content);
    }
    aj_free(d);
}

static void tool_refresh(cat_robot_t *r) { assist_robot(r); }
static bool tool_confirm(void *u, const char *k, const char *t, const char *d, const char *why)
{
    (void)u; (void)k; (void)t; (void)d; (void)why;
    return false; /* read-only here: nothing to confirm */
}
static void tool_note(void *u, const char *t) { (void)u; (void)t; }
static bool tool_stopped(void *u)
{
    (void)u;
    return cancelled();
}

/* One call, run and appended to the turn as a "tool" message. */
static void run_call(const aj_t *call, aj_t *turn, cat_robot_t *robot)
{
    const char *id = aj_gets(call, "id");
    const aj_t *fn = aj_get(call, "function");
    const char *name = aj_gets(fn, "name"), *args = aj_gets(fn, "arguments");
    ab_t out;
    ab_init(&out);
    const as_tooldef_t *def = name ? as_tool_find(name) : NULL;
    char err[200] = "", summary[200];
    aj_t *in = args ? aj_parse(args, strlen(args), err, sizeof err) : aj_new(AJ_OBJ);
    if (!def || def->kind != AS_T_READ) ab_puts(&out, "{\"error\":\"no such read-only tool\"}");
    else if (!in) ab_fmt(&out, "{\"error\":\"arguments aren't valid JSON\"}");
    else if (!as_tool_check(def, in, err, sizeof err)) {
        ab_puts(&out, "{\"error\":");
        ab_str(&out, err);
        ab_puts(&out, "}");
    } else {
        set_note("looking: %s", name);
        char tstr[16] = "";
        int team = 0;
        if (hal_kv_get("team", tstr, sizeof tstr)) team = atoi(tstr);
        as_env_t env = { .robot = robot, .refresh = tool_refresh, .confirm = tool_confirm, .note = tool_note,
                         .stopped = tool_stopped, .user = NULL, .team = team };
        assist_robot(robot);
        as_tool_run(&env, name, in, &out, summary, sizeof summary);
    }
    aj_free(in);
    aj_t *m = aj_new(AJ_OBJ);
    aj_set(m, "role", aj_new_str("tool"));
    aj_set(m, "tool_call_id", aj_new_str(id ? id : ""));
    char *text = ab_take(&out);
    aj_set(m, "content", aj_new_str(text ? text : "{}"));
    free(text);
    aj_push(turn, m);
}

/* A question to an answer, in the reply. false (with a note) when it failed. */
static bool answer(const char *question, vo_reply_t *rep)
{
    voice_config_t c;
    voice_config(&c);
    char model[48];
    if (c.chat_model[0]) snprintf(model, sizeof model, "%s", c.chat_model);
    else {
        assist_config_t *a = malloc(sizeof *a);
        model[0] = 0;
        if (a) {
            assist_config(a);
            snprintf(model, sizeof model, "%s", a->oai_model);
            memset(a, 0, sizeof *a);
            free(a);
        }
        if (!model[0]) snprintf(model, sizeof model, "%s", AS_OAI_DEFAULT_MODEL);
    }
    pthread_mutex_lock(&V.lock);
    bool fresh = V.clear_hist || hal_seconds() - V.last_turn > HIST_IDLE_S;
    V.clear_hist = false;
    pthread_mutex_unlock(&V.lock);
    if (fresh) hist_clear();
    char note[400];
    context_note(note, sizeof note);
    size_t ql = strlen(note) + strlen(question) + 4;
    char *q = malloc(ql);
    if (!q) return false;
    snprintf(q, ql, "%s %s", note, question);

    char *tools = tools_json();
    cat_robot_t *robot = malloc(sizeof *robot);
    aj_t *turn = aj_new(AJ_ARR);
    ab_t body;
    ab_init(&body);
    bool ok = false;
    static int fmt; /* what this model took last time: the schema first */
    static char fmt_model[48];
    if (strcmp(fmt_model, model) != 0) {
        snprintf(fmt_model, sizeof fmt_model, "%s", model);
        fmt = 0;
    }
    for (int round = 0; round <= TOOL_ROUNDS && !cancelled(); round++) {
        /* the last round offers no tools: it must answer */
        chat_body(&body, model, q, turn, round < TOOL_ROUNDS ? tools : NULL, fmt);
        if (body.oom) {
            set_err("out of memory building the request");
            break;
        }
        resp_t r;
        bool sent = post("/v1/chat/completions", "application/json", body.p, body.n, 256 * 1024, &r);
        if (cancelled()) {
            resp_free(&r);
            break;
        }
        if (sent && r.status == 400 && fmt < 2 && r.body &&
            (strstr(r.body, "response_format") || strstr(r.body, "json_schema") || strstr(r.body, "json_object"))) {
            fmt++; /* this model can't do structured output: a looser format, same question */
            resp_free(&r);
            round--;
            continue;
        }
        if (!sent || r.status != 200) {
            describe(&r, "the answer");
            resp_free(&r);
            break;
        }
        aj_t *d = aj_parse(r.body, (size_t)r.len, NULL, 0);
        resp_free(&r);
        const aj_t *msg = aj_get(aj_at(aj_get(d, "choices"), 0), "message");
        const aj_t *calls = aj_get(msg, "tool_calls");
        const char *refusal = aj_gets(msg, "refusal");
        if (calls && calls->type == AJ_ARR && calls->n > 0 && round < TOOL_ROUNDS && robot) {
            aj_t *m = aj_new(AJ_OBJ);
            aj_set(m, "role", aj_new_str("assistant"));
            aj_set(m, "content", aj_new(AJ_NULL));
            aj_set(m, "tool_calls", aj_clone(calls));
            aj_push(turn, m);
            for (int i = 0; i < calls->n && !cancelled(); i++) run_call(calls->kid[i], turn, robot);
            aj_free(d);
            continue;
        }
        const char *content = aj_gets(msg, "content");
        if (refusal && !content) {
            snprintf(rep->say, sizeof rep->say, "%s", refusal);
            rep->emotion = VO_EMO_WORRIED;
            rep->intensity = 0.4f;
            rep->look = VO_LOOK_DOWN;
        } else {
            parse_reply(content, rep);
        }
        ok = rep->say[0] != 0;
        if (!ok) set_err("the answer came back empty: ask me again");
        if (ok) {
            hist_add(true, question);
            hist_add(false, content ? content : rep->say);
            V.last_turn = hal_seconds();
        }
        aj_free(d);
        break;
    }
    ab_free(&body);
    aj_free(turn);
    free(robot);
    free(tools);
    free(q);
    return ok;
}

/* ------------------------------------------------------------------ speech */

/* Speaks `text` in the reply's mood, streaming the audio to the speaker as it arrives. */
static bool speak(const vo_reply_t *rep)
{
    voice_config_t c;
    voice_config(&c);
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&V.lock);
        bool fb = V.tts_fallback;
        pthread_mutex_unlock(&V.lock);
        const char *model = c.tts_model[0] ? c.tts_model : fb ? VO_TTS_FALLBACK : VO_TTS_DEFAULT;
        ab_t b;
        ab_init(&b);
        ab_puts(&b, "{\"model\":");
        ab_str(&b, model);
        ab_puts(&b, ",\"voice\":");
        ab_str(&b, c.voice[0] ? c.voice : VO_VOICE_DEFAULT);
        ab_puts(&b, ",\"input\":");
        ab_str(&b, rep->say);
        ab_puts(&b, ",\"response_format\":\"pcm\"");
        if (!strncmp(model, "gpt-", 4)) {
            ab_fmt(&b, ",\"instructions\":\"A friendly little desk companion talking with its owner. Natural, "
                       "conversational pace. Tone: %s%s.\"",
                   TONE[rep->emotion < VO_EMO_COUNT ? rep->emotion : 0], rep->intensity > 0.7f ? ", clearly" : "");
        }
        ab_puts(&b, "}");
        char url[200], *hdr = malloc(512), *key = oai_key();
        if (!hdr || !key || b.oom) {
            free(hdr);
            drop_key(key);
            ab_free(&b);
            return false;
        }
        pthread_mutex_lock(&V.lock);
        snprintf(url, sizeof url, "%s/v1/audio/speech", V.base);
        pthread_mutex_unlock(&V.lock);
        snprintf(hdr, 512, "Authorization: Bearer %s\r\nContent-Type: application/json\r\n", key);
        drop_key(key);
        hal_http_req_t rq = { .method = "POST", .url = url, .headers = hdr, .body = b.p, .body_len = b.n,
                              .timeout_ms = 20000 };
        resp_t r = { .status = -1 };
        hal_http_t *h = hal_http_open(&rq, &r.status, r.err, sizeof r.err);
        memset(hdr, 0, 512);
        free(hdr);
        ab_free(&b);
        if (!h) {
            describe(&r, "speaking");
            return false;
        }
        if (r.status != 200) {
            char e[1024];
            int n = 0, k;
            while (n < (int)sizeof e - 1 && (k = hal_http_read(h, e + n, (int)sizeof e - 1 - n)) > 0) n += k;
            e[n] = 0;
            hal_http_close(h);
            if (!c.tts_model[0] && !fb && (r.status == 400 || r.status == 403 || r.status == 404)) {
                pthread_mutex_lock(&V.lock);
                V.tts_fallback = true;
                pthread_mutex_unlock(&V.lock);
                continue;
            }
            r.body = e;
            r.len = n;
            describe(&r, "speaking");
            return false;
        }
        if (!hal_play_start(TTS_RATE)) {
            hal_play_stop();
            if (!hal_play_start(TTS_RATE)) {
                hal_http_close(h);
                set_err("the speaker wouldn't start: the answer is on screen");
                return false;
            }
        }
        set_state(VO_SPEAKING);
        char *buf = malloc(4096 + 2);
        int carry = 0, rd = 0;
        while (buf && !stopped()) {
            rd = hal_http_read(h, buf + carry, 4096);
            if (rd <= 0) break;
            int have = carry + rd, samples = have / 2;
            hal_play_write((const int16_t *)(void *)buf, samples, 5000);
            carry = have & 1;
            if (carry) buf[0] = buf[have - 1];
        }
        free(buf);
        hal_http_close(h);
        if (stopped()) {
            hal_play_stop();
            return false;
        }
        hal_play_end();
        /* what's still queued plays out (writes wait on the speaker, so a few seconds at most); a speaker that
         * never says it's done can't hold the face "speaking" for good */
        double until = hal_seconds() + 8.0;
        while (hal_play_busy() && !stopped() && hal_seconds() < until) usleep(20000);
        if (stopped() || hal_play_busy()) hal_play_stop();
        if (rd < 0) {
            set_err(net_down() ? "the Wi-Fi dropped while I was talking" : "my voice broke off: the network dropped");
            return false; /* the rest of it goes on screen */
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ a turn */

/* The reply the UI reads, replaced whole under the lock, with a new sequence number. */
static void publish(const vo_reply_t *rep)
{
    pthread_mutex_lock(&V.lock);
    uint32_t seq = V.reply->seq + 1;
    *V.reply = *rep;
    V.reply->seq = seq;
    pthread_mutex_unlock(&V.lock);
    bump();
}

/* One question to its answer, shown and spoken. true when it was answered. */
static bool turn(const char *question, bool heard)
{
    vo_reply_t *rep = calloc(1, sizeof *rep);
    if (!rep) return false;
    voice_config_t c;
    voice_config(&c);
    set_state(VO_THINKING);
    snprintf(rep->heard, sizeof rep->heard, "%s", question);
    /* a typed question is answered on screen too, whatever the setting: the owner is looking */
    rep->shown = !heard || c.out != VO_OUT_SPEAK;
    publish(rep); /* the question, with no answer yet */
    double t0 = hal_seconds();
    bool ok = answer(question, rep);
    if (cancelled()) {
        /* stopped: the face went idle when it was; nothing more to show */
    } else if (!ok) {
        /* why, where the answer would have been (the status line alone is gone in a few seconds) */
        pthread_mutex_lock(&V.lock);
        bool why = V.note_err && V.note_at >= t0 && V.note[0];
        snprintf(rep->say, sizeof rep->say, "%s", why ? V.note : "I couldn't answer that: ask me again");
        pthread_mutex_unlock(&V.lock);
        rep->failed = true;
        rep->shown = true;
        rep->spoken = false;
        rep->emotion = VO_EMO_WORRIED;
        rep->intensity = 0.6f;
        rep->look = VO_LOOK_DOWN;
        publish(rep);
    } else {
        /* the sound off, or the face closed (a typed question from elsewhere): the answer is read, not heard */
        pthread_mutex_lock(&V.lock);
        bool can_speak = V.enabled && hal_volume() > 0.02f;
        pthread_mutex_unlock(&V.lock);
        rep->spoken = c.out != VO_OUT_SHOW && can_speak;
        if (!rep->spoken) rep->shown = true;
        pthread_mutex_lock(&V.lock);
        V.note[0] = 0;
        pthread_mutex_unlock(&V.lock);
        publish(rep);
        if (rep->spoken && !speak(rep) && !cancelled() && !rep->shown) {
            /* it couldn't be said: show it instead */
            rep->spoken = false;
            rep->shown = true;
            publish(rep);
        }
    }
    free(rep);
    return ok;
}

/* ------------------------------------------------------------------ the worker */

static void *worker(void *arg)
{
    (void)arg;
    int16_t *rec = malloc(REC_MAX * sizeof *rec), *chunk = malloc(CHUNK * sizeof *chunk);
    if (!rec || !chunk) {
        set_err("no memory for listening");
        return NULL;
    }
    bool follow = false;           /* just answered: a follow-up needs no wake word */
    for (;;) {
        pthread_mutex_lock(&V.lock);
        char *text = V.ask;
        V.ask = NULL;
        bool ptt = V.ptt, on = V.enabled;
        V.ptt = false;
        if (!text) V.cancel = false;
        voice_config_t c = V.cfg;
        pthread_mutex_unlock(&V.lock);

        if (text) {
            pthread_mutex_lock(&V.lock);
            V.cancel = false;
            V.working = true;
            pthread_mutex_unlock(&V.lock);
            turn(text, false);
            free(text);
            follow = false;
            pthread_mutex_lock(&V.lock);
            V.working = false;
            pthread_mutex_unlock(&V.lock);
            set_state(VO_IDLE); /* (OFF, below, when the face is closed) */
            continue;
        }
        if (!on) {
            hal_mic_stop();
            if (hal_play_busy()) hal_play_stop();
            follow = false;
            set_state(VO_OFF);
            pthread_mutex_lock(&V.lock);
            while (!V.enabled && !V.ask) pthread_cond_wait(&V.wake, &V.lock);
            pthread_mutex_unlock(&V.lock);
            continue;
        }
        /* a key and the network, looked at once a second (this loop turns every 20 ms while it listens) */
        static double ready_at;
        static bool ready;
        static char why[80];
        if (ptt || hal_seconds() - ready_at > 1.0) {
            ready = voice_ready(why, sizeof why);
            ready_at = hal_seconds();
        }
        const char *word = NULL;
        bool wake = c.wake && ready && hal_wake_ready(&word);
        pthread_mutex_lock(&V.lock);
        if (V.wake_word != word || !V.wake_loaded) {
            V.wake_word = word;
            V.wake_loaded = true;
            pthread_mutex_unlock(&V.lock);
            bump();
        } else {
            pthread_mutex_unlock(&V.lock);
        }
        if ((ptt || follow) && ready) {
            if (!hal_mic_start()) {
                set_err("the microphones didn't start");
                follow = false;
                continue;
            }
            if (ptt) hal_tone(1320, 40, 0.25f);
            int n = record(rec, chunk, ptt ? 6.0 : 4.0, ptt ? 140 : 0);
            bool was_follow = follow;
            follow = false;
            if (n == 0 && !was_follow) set_note("didn't hear anything");
            if (n <= 0) {
                set_state(VO_IDLE);
                continue;
            }
            set_state(VO_HEARING);
            pthread_mutex_lock(&V.lock);
            V.working = true;
            pthread_mutex_unlock(&V.lock);
            char *heard = transcribe(rec, n);
            size_t l = heard ? strlen(heard) : 0;
            while (l && (heard[l - 1] == ' ' || heard[l - 1] == '\n')) heard[--l] = 0;
            if (!heard || !l) {
                if (heard && !cancelled()) set_note("didn't catch that");
                free(heard);
                pthread_mutex_lock(&V.lock);
                V.working = false;
                pthread_mutex_unlock(&V.lock);
                set_state(VO_IDLE);
                continue;
            }
            bool answered = turn(heard, true);
            free(heard);
            pthread_mutex_lock(&V.lock);
            V.working = false;
            pthread_mutex_unlock(&V.lock);
            /* a follow-up only after an answer: after a failure, the owner reads why first */
            follow = answered && c.follow && !stopped();
            if (follow) usleep(250000); /* the room's echo of the last word dies away first */
            hal_wake_reset();
            set_state(VO_IDLE);
            continue;
        }
        if (ptt && !ready) set_err("%s", why);
        if (wake) {
            if (!hal_mic_on()) {
                if (!hal_mic_start()) {
                    set_err("the microphones didn't start");
                    usleep(1000000);
                    continue;
                }
                hal_wake_reset();
            }
            set_state(VO_IDLE);
            int r = hal_mic_read(chunk, CHUNK);
            if (r <= 0) {
                usleep(20000);
                continue;
            }
            floor_track(mean_square(chunk, r));
            if (hal_wake_feed(chunk, r)) {
                pthread_mutex_lock(&V.lock);
                V.ptt = true; /* the next pass records */
                pthread_mutex_unlock(&V.lock);
            }
            continue;
        }
        /* no wake word: the microphones stay off until a tap */
        hal_mic_stop();
        set_state(VO_IDLE);
        pthread_mutex_lock(&V.lock);
        if (V.enabled && !V.ptt && !V.ask) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; /* now and then: the key or the network may have come */
            pthread_cond_timedwait(&V.wake, &V.lock, &ts);
        }
        pthread_mutex_unlock(&V.lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ the UI's side */

void voice_init(void)
{
    pthread_mutex_lock(&V.lock);
    bool start = !V.started;
    V.started = true;
    pthread_mutex_unlock(&V.lock);
    if (!start) return;
    load_config();
    V.reply = calloc(1, sizeof *V.reply);
    if (!V.reply) return;
    /* TLS, the tools' buffers and WakeNet: likely in PSRAM, which is fine: this thread never writes NVS */
    /* internal RAM, never PSRAM: the wake word's model is read from its flash partition on this thread, and
     * with the cache off for that a PSRAM stack crashed the tablet whenever the companion opened. ~5 KB is
     * used; 16 KB leaves room for TLS. */
    if (!hal_thread_internal("voice", worker, NULL, 16 * 1024)) printf("voice: no internal RAM for its thread: no voice\n");
}

void voice_enable(bool on)
{
    pthread_mutex_lock(&V.lock);
    V.enabled = on;
    if (!on) {
        V.cancel = true;
        /* off at once: reopened while a stopped request still waits on the network, the face mustn't come
         * back "thinking" about it */
        if (!V.ask) V.state = VO_OFF;
    }
    pthread_cond_signal(&V.wake);
    pthread_mutex_unlock(&V.lock);
    bump();
}

const char *voice_wake_word(void)
{
    pthread_mutex_lock(&V.lock);
    const char *w = V.wake_word;
    pthread_mutex_unlock(&V.lock);
    return w;
}

void voice_listen(void)
{
    pthread_mutex_lock(&V.lock);
    V.ptt = true;
    /* a stopped request is still waiting on the network: the recording starts once it gives up */
    bool held = V.working && V.cancel;
    pthread_cond_signal(&V.wake);
    pthread_mutex_unlock(&V.lock);
    if (held) set_note("one moment: the last request is still letting go of the network");
}

bool voice_ask(const char *text)
{
    if (!text || !text[0]) return false;
    pthread_mutex_lock(&V.lock);
    bool busy = V.ask || V.state == VO_THINKING || V.state == VO_HEARING || V.state == VO_SPEAKING;
    if (!busy) {
        V.ask = as_strdup(text);
        V.cancel = true; /* a recording or a wait under way gives way to it */
        if (V.ask) V.state = VO_THINKING; /* at once: the face shows it was heard */
        pthread_cond_signal(&V.wake);
    }
    pthread_mutex_unlock(&V.lock);
    bump();
    return !busy;
}

void voice_cancel(void)
{
    pthread_mutex_lock(&V.lock);
    V.cancel = true;
    V.ptt = false;
    /* idle at once, whatever the worker is still blocked in (a request can take its timeout to give up) */
    if (V.state != VO_OFF && V.state != VO_IDLE) V.state = V.enabled ? VO_IDLE : VO_OFF;
    pthread_mutex_unlock(&V.lock);
    hal_play_stop();
    bump();
}

void voice_reset(void)
{
    voice_cancel();
    pthread_mutex_lock(&V.lock);
    V.clear_hist = true; /* the worker forgets before its next question */
    if (V.reply) memset(V.reply, 0, sizeof *V.reply);
    pthread_mutex_unlock(&V.lock);
    bump();
}

vo_state_t voice_state(void)
{
    pthread_mutex_lock(&V.lock);
    vo_state_t s = V.state;
    pthread_mutex_unlock(&V.lock);
    return s;
}

float voice_mic_level(void) { return hal_mic_level(); }
float voice_speak_level(void) { return hal_play_level(); }

bool voice_reply(vo_reply_t *out)
{
    pthread_mutex_lock(&V.lock);
    if (V.reply) *out = *V.reply;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&V.lock);
    return out->seq != 0;
}

double voice_note(char *out, size_t n, bool *err)
{
    pthread_mutex_lock(&V.lock);
    snprintf(out, n, "%s", V.note);
    double at = V.note_at;
    if (err) *err = V.note_err;
    pthread_mutex_unlock(&V.lock);
    return at;
}
