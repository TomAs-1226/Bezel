/* home_pet_store — the one pet: kept on the card, advanced by the UI's tick, driven by the owner and the AI.
 *
 * CATOS/PET/state.json on the card, written through a temp file and a rename (FAT will not rename over a file,
 * so the old one goes first, and a temp found without its file is the write that was cut short). A change the
 * owner made is written within a couple of seconds, a slow drift once a minute at most. Without a card the same
 * text lives in the kv store, written only from the UI thread (NVS must not be written from a thread whose stack
 * may be in PSRAM). The card may not answer for the first ~20 s after power-up, so the file is read on the tick
 * and a failure is tried again. A state that failed to load is never written over. */
#include "home_pet.h"

#include "hal.h"
#include "home.h"
#include "json_lite.h"

#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KV_KEY "pet"
#define FILE_MAX 4096
#define CLOCK_OK 1609459200LL

static struct {
    pthread_mutex_t lock;
    home_pet_t p;
    bool loaded, dirty, urgent, new_pet;
    double last_write, next_load, last_adv, quiet;
    unsigned rev;
    int fails;
    int ev;            /* the last thing done to the pet, for the screen to react to */
    unsigned ev_seq;
} S = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return t > CLOCK_OK ? (int64_t)t : 0;
}

int home_pet_tz(void)
{
    time_t t = time(NULL);
    struct tm l, g;
    localtime_r(&t, &l);
    gmtime_r(&t, &g);
    int d = l.tm_yday - g.tm_yday;
    if (d > 1) d = -1;
    if (d < -1) d = 1;
    return d * 86400 + (l.tm_hour - g.tm_hour) * 3600 + (l.tm_min - g.tm_min) * 60;
}

static void pathf(char *out, size_t n, const char *name)
{
    const char *sd = hal_sd_root();
    snprintf(out, n, "%s/CATOS/PET/%s", sd ? sd : "", name);
}

static bool read_card(char *buf, size_t n, bool *missing)
{
    *missing = false;
    const char *sd = hal_sd_root();
    if (!sd) return false;
    char path[128];
    pathf(path, sizeof path, "state.json");
    FILE *f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        /* a write cut short leaves the temp file: it is the newer state */
        char tmp[128];
        pathf(tmp, sizeof tmp, "state.tmp");
        f = fopen(tmp, "rb");
    }
    if (!f) {
        /* the folder answers, so it is truly missing and not a card that has not woken yet */
        DIR *d = opendir(sd);
        if (d) {
            closedir(d);
            *missing = true;
        }
        return false;
    }
    size_t got = fread(buf, 1, n - 1, f);
    bool err = ferror(f);
    fclose(f);
    if (err) return false;
    buf[got] = 0;
    return got > 0;
}

static bool write_card(const char *js, size_t len)
{
    const char *sd = hal_sd_root();
    if (!sd) return false;
    char path[128], tmp[128];
    pathf(path, sizeof path, "state.json");
    pathf(tmp, sizeof tmp, "state.tmp");
    FILE *f = fopen(tmp, "wb");
    if (!f && errno == ENOENT) {
        char dir[128];
        snprintf(dir, sizeof dir, "%s/CATOS", sd);
        mkdir(dir, 0775);
        snprintf(dir, sizeof dir, "%s/CATOS/PET", sd);
        mkdir(dir, 0775);
        f = fopen(tmp, "wb");
    }
    if (!f) return false;
    bool ok = fwrite(js, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path);
    return rename(tmp, path) == 0;
}

static void save_locked(void)
{
    char *js = malloc(FILE_MAX);
    if (!js) return;
    size_t n = home_pet_to_json(&S.p, js, FILE_MAX);
    if (hal_sd_root()) {
        if (write_card(js, n)) {
            S.dirty = S.urgent = false;
            S.fails = 0;
        } else S.fails++;
    } else {
        hal_kv_set(KV_KEY, js); /* UI thread only: this is called from home_pet_tick */
        S.dirty = S.urgent = false;
    }
    S.last_write = hal_seconds();
    free(js);
}

static void load_locked(double now)
{
    char *buf = malloc(FILE_MAX);
    if (!buf) return;
    bool missing = false, ok = false;
    if (hal_sd_root()) {
        if (read_card(buf, FILE_MAX, &missing) && home_pet_from_json(&S.p, buf, strlen(buf))) ok = true;
        else if (missing || S.fails > 10) {
            /* no file: the kv copy from before a card was in, else a new pet */
            if (hal_kv_get(KV_KEY, buf, FILE_MAX) && home_pet_from_json(&S.p, buf, strlen(buf))) {
                ok = true;
                S.dirty = S.urgent = true;
            } else if (now_unix()) {
                home_pet_init(&S.p, now_unix());
                ok = true;
                S.dirty = S.urgent = true;
                S.new_pet = true;
            }
        }
    } else if (now > 20.0) {
        if (hal_kv_get(KV_KEY, buf, FILE_MAX) && home_pet_from_json(&S.p, buf, strlen(buf))) ok = true;
        else if (now_unix()) {
            home_pet_init(&S.p, now_unix());
            ok = true;
            S.dirty = S.urgent = true;
        }
    }
    free(buf);
    if (!ok) {
        S.fails++;
        S.next_load = now + 2.0;
        return;
    }
    S.loaded = true;
    S.rev++;
    /* the time the tablet was off, caught up at once */
    home_pet_advance(&S.p, now_unix(), home_pet_tz(), 0);
    S.dirty = true;
    printf("pet: %s, %d days old, %s\n", S.p.name, home_pet_age_days(&S.p, now_unix()), hal_sd_root() ? "on the card" : "in kv");
}

void home_pet_boot(void) { S.next_load = 0; }

void home_pet_set_quiet(double q) { S.quiet = q; }

void home_pet_tick(double now)
{
    pthread_mutex_lock(&S.lock);
    if (!S.loaded) {
        if (now >= S.next_load && now_unix() && now > 2.0) load_locked(now);
        pthread_mutex_unlock(&S.lock);
        return;
    }
    if (now - S.last_adv >= 5.0) {
        S.last_adv = now;
        home_pet_t was = S.p;
        home_pet_advance(&S.p, now_unix(), home_pet_tz(), S.quiet);
        if (S.p.asleep != was.asleep || fabsf(S.p.hunger - was.hunger) > 0.004f || S.p.ndiary != was.ndiary) {
            S.dirty = true;
            if (S.p.asleep != was.asleep || S.p.ndiary != was.ndiary) S.rev++;
        }
    }
    if ((S.urgent && now - S.last_write > 2.0) || (S.dirty && now - S.last_write > 60.0)) save_locked();
    pthread_mutex_unlock(&S.lock);
}

void home_pet_get(home_pet_t *out)
{
    pthread_mutex_lock(&S.lock);
    *out = S.p;
    pthread_mutex_unlock(&S.lock);
}

unsigned home_pet_rev(void) { return S.rev; }

/* ---- the owner's and the AI's actions ---- */

#define ACT(body)                                                                    \
    do {                                                                             \
        pthread_mutex_lock(&S.lock);                                                 \
        if (!S.loaded) {                                                             \
            snprintf(say, n, "the pet is not awake yet: the card is still starting"); \
            pthread_mutex_unlock(&S.lock);                                           \
            return false;                                                            \
        }                                                                            \
        home_pet_advance(&S.p, now_unix(), home_pet_tz(), S.quiet);                  \
        bool ok = (body);                                                            \
        if (ok) {                                                                    \
            S.dirty = S.urgent = true;                                               \
            S.rev++;                                                                 \
        }                                                                            \
        pthread_mutex_unlock(&S.lock);                                               \
        return ok;                                                                   \
    } while (0)

static void event(int e)
{
    S.ev = e;
    S.ev_seq++;
}

int home_pet_event(unsigned *seen)
{
    if (*seen == S.ev_seq) return 0;
    *seen = S.ev_seq;
    return S.ev;
}

bool home_pet_do_feed(int food, char *say, size_t n)
{
    ACT(home_pet_feed(&S.p, food, now_unix(), home_pet_tz(), say, n) && (event(PET_EV_FED), true));
}

bool home_pet_do_play(char *say, size_t n) { ACT(home_pet_play(&S.p, now_unix(), say, n) && (event(PET_EV_PLAYED), true)); }
bool home_pet_do_sleep(bool asleep, char *say, size_t n)
{
    ACT(home_pet_set_asleep(&S.p, asleep, now_unix(), home_pet_tz(), say, n));
}

void home_pet_do_stroke(void)
{
    pthread_mutex_lock(&S.lock);
    if (S.loaded) {
        home_pet_stroke(&S.p, now_unix());
        S.dirty = true;
        event(PET_EV_PETTED);
    }
    pthread_mutex_unlock(&S.lock);
}

bool home_pet_do_rename(const char *name)
{
    pthread_mutex_lock(&S.lock);
    bool ok = S.loaded && home_pet_rename(&S.p, name, now_unix());
    if (ok) {
        S.dirty = S.urgent = true;
        S.rev++;
    }
    pthread_mutex_unlock(&S.lock);
    return ok;
}

void home_pet_do_note(const char *text)
{
    pthread_mutex_lock(&S.lock);
    if (S.loaded) {
        home_pet_note(&S.p, now_unix(), text);
        S.dirty = true;
    }
    pthread_mutex_unlock(&S.lock);
}

void home_pet_context(char *out, size_t n)
{
    out[0] = 0;
    pthread_mutex_lock(&S.lock);
    if (S.loaded) {
        home_pet_advance(&S.p, now_unix(), home_pet_tz(), S.quiet);
        home_pet_brief(&S.p, now_unix(), out, n);
        S.dirty = true; /* the brief may have counted a mention */
    }
    pthread_mutex_unlock(&S.lock);
}

/* ---- the AI's tools ---- */

bool home_pet_is_tool(const char *name)
{
    return name && (!strcmp(name, "pet_status") || !strcmp(name, "pet_feed") || !strcmp(name, "pet_play") ||
                    !strcmp(name, "pet_rename"));
}

static int food_of(const char *s)
{
    if (!s || !s[0]) return PET_RICE;
    if (strstr(s, "berr") || strstr(s, "fruit") || strstr(s, "snack") || strstr(s, "apple")) return PET_BERRY;
    if (strstr(s, "fish") || strstr(s, "tuna") || strstr(s, "salmon")) return PET_FISH;
    if (strstr(s, "cake") || strstr(s, "treat") || strstr(s, "cookie") || strstr(s, "sweet")) return PET_CAKE;
    return PET_RICE;
}

bool home_pet_tool(const char *name, const char *args, char *out, size_t n)
{
    jl_tok_t tk[24];
    int nt = args ? jl_parse(args, strlen(args), tk, 24) : -1;
    jl_doc_t D = { args, tk, nt };
    char val[64] = "";
    if (nt > 0 && tk[0].type == JL_OBJ) {
        jl_str(&D, jl_get(&D, 0, "food"), val, sizeof val);
        if (!val[0]) jl_str(&D, jl_get(&D, 0, "name"), val, sizeof val);
    }
    if (!strcmp(name, "pet_status")) {
        home_pet_t p;
        home_pet_get(&p);
        if (!S.loaded) {
            snprintf(out, n, "the pet is not awake yet: the card is still starting");
            return false;
        }
        char w[48], b[600];
        home_pet_words(&p, w, sizeof w);
        int64_t t = now_unix();
        home_pet_t q = p;
        home_pet_brief(&q, t, b, sizeof b); /* on a copy: a status question is not a mention */
        snprintf(out, n, "%s is %s. hunger %d%%, energy %d%%, mood %d%%, affection %d%%, %d days old, %s. %s", p.name, w,
                 (int)(p.hunger * 100 + 0.5f), (int)(p.energy * 100 + 0.5f), (int)(p.mood * 100 + 0.5f),
                 (int)(p.affection * 100 + 0.5f), home_pet_age_days(&p, t), p.asleep ? "asleep" : "awake", b);
        return true;
    }
    if (!strcmp(name, "pet_feed")) return home_pet_do_feed(food_of(val), out, n);
    if (!strcmp(name, "pet_play")) return home_pet_do_play(out, n);
    if (!strcmp(name, "pet_rename")) {
        if (!val[0]) {
            snprintf(out, n, "what should the pet be called?");
            return false;
        }
        if (!home_pet_do_rename(val)) {
            snprintf(out, n, "that name didn't take");
            return false;
        }
        home_pet_t p;
        home_pet_get(&p);
        snprintf(out, n, "the pet is now called %s", p.name);
        return true;
    }
    snprintf(out, n, "no such pet tool");
    return false;
}

/* ---- the dev console: pet | pet set hunger 0.9 | pet asleep 1 | pet feed fish | pet play | pet stroke | pet reset ---- */
void home_pet_dev(const char *a)
{
    while (*a == ' ') a++;
    char say[96] = "";
    pthread_mutex_lock(&S.lock);
    bool loaded = S.loaded;
    pthread_mutex_unlock(&S.lock);
    if (!loaded) {
        printf("pet: not loaded yet\n");
        return;
    }
    if (!strncmp(a, "set ", 4)) {
        char k[24];
        float v = 0;
        if (sscanf(a + 4, "%23s %f", k, &v) == 2) {
            pthread_mutex_lock(&S.lock);
            if (!strcmp(k, "hunger")) S.p.hunger = v < 0 ? 0 : v > 0.92f ? 0.92f : v;
            else if (!strcmp(k, "energy")) S.p.energy = v < 0.08f ? 0.08f : v > 1 ? 1 : v;
            else if (!strcmp(k, "mood")) S.p.mood = v < 0 ? 0 : v > 1 ? 1 : v;
            else if (!strcmp(k, "affection")) S.p.affection = v < 0 ? 0 : v > 1 ? 1 : v;
            else if (!strcmp(k, "age")) S.p.born = now_unix() - (int64_t)(v * 86400);
            S.dirty = true;
            S.rev++;
            pthread_mutex_unlock(&S.lock);
        }
    } else if (!strncmp(a, "asleep", 6)) {
        pthread_mutex_lock(&S.lock);
        S.p.asleep = atoi(a + 6) != 0;
        S.p.up_until = S.p.asleep ? 0 : now_unix() + 1800;
        S.rev++;
        pthread_mutex_unlock(&S.lock);
    } else if (!strncmp(a, "feed", 4)) {
        bool ok = home_pet_do_feed(food_of(a + 4), say, sizeof say);
        printf("pet: %s%s\n", ok ? "" : "no: ", say);
    } else if (!strncmp(a, "play", 4)) {
        bool ok = home_pet_do_play(say, sizeof say);
        printf("pet: %s%s\n", ok ? "" : "no: ", say);
    } else if (!strncmp(a, "stroke", 6)) {
        home_pet_do_stroke();
    } else if (!strncmp(a, "reset", 5)) {
        pthread_mutex_lock(&S.lock);
        home_pet_init(&S.p, now_unix());
        S.dirty = S.urgent = true;
        S.rev++;
        pthread_mutex_unlock(&S.lock);
    }
    home_pet_t p;
    home_pet_get(&p);
    char w[48];
    home_pet_words(&p, w, sizeof w);
    printf("pet: %s, %d days, %s%s; hunger %.2f energy %.2f mood %.2f affection %.2f; diary %d\n", p.name,
           home_pet_age_days(&p, now_unix()), w, p.asleep ? " (asleep)" : "", p.hunger, p.energy, p.mood, p.affection,
           p.ndiary);
    for (int i = 0; i < p.ndiary; i++) printf("pet:   %lld %s\n", (long long)p.diary[i].at, p.diary[i].text);
}
