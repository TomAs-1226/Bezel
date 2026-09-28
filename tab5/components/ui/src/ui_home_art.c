/* The covers home mode and the music app show, and the big one on home mode's now-playing page.
 *
 * A song's cover is its own art from the PC once it has arrived, else one generated from its artist and album
 * (home_art.c). Three small covers (HOME_ART_S, 63 KB each) are kept; asking for one that isn't there queues it
 * and returns NULL until it is ready, and the caller keeps showing what it had. The big cover exists once, as the
 * "hero": the cover at HOME_ART_L set on the page's ground with its shadow, painted again (it's deterministic)
 * whenever the song or the tone changes.
 *
 * Painting runs on a one-shot worker at a low priority on core 0 (core 1 draws) that ends itself when the queue
 * is empty, so a cover costs the UI thread a pointer swap. Its stack is in PSRAM (it never writes NVS), and its
 * working picture (HOME_ART_L² × 4, 518 KB) exists only while it runs. hm_art_trim() gives everything back when
 * home mode is left and the music app closed. */
#include "ui_home_priv.h"

#include "home_px.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define NART 3
#define REAL_BIT 0x80000000u
#define PENDING_S 2.5 /* a song whose own art hasn't arrived waits this long before it gets a generated one */

enum { A_FREE, A_QUEUED, A_PAINTING, A_READY };

struct hm_art {
    uint32_t key;             /* home_art_key(), or for the PC's own art its art_gen | REAL_BIT */
    int state;
    int style;                /* the dev console's forced style, -1 the key's */
    uint16_t *small;
    lv_image_dsc_t dsc;
    home_art_info_t info;
    uint32_t used;            /* the last query's stamp: the oldest is painted over next */
    unsigned serial;          /* new for every painting */
    float ms;
};

static struct {
    pthread_mutex_t lock;
    hm_art_t art[NART];
    uint32_t stamp;
    unsigned serial;
    bool running;
    struct {                  /* the hero, one at a time */
        int state;
        uint32_t key;
        int style;
        uint16_t *px;
        int w, h, cx, cy, radius;
        uint16_t ground;
        bool dark;
        uint32_t seed;
        unsigned serial;
        float ms;
    } hero;
    struct {                  /* other pictures (the clock faces'): a function run on the worker */
        void (*fn)(void *);
        void *arg;
        unsigned ticket;
        bool queued;
    } job[4];
    unsigned tickets, running_ticket;
    uint32_t pc_song;         /* the PC's song, for the pending wait */
    double pc_since;
    int force_style;          /* the dev console: every generated cover in this style (-1: the key's) */
    float bench[HOME_ART_STYLES + 2];
    int bench_want;           /* 0 none, 1 asked, 2 done */
} A = { .lock = PTHREAD_MUTEX_INITIALIZER, .force_style = -1 };

/* ---------------------------------------------------------------- the worker */

/* The big cover for key into work: painted, or the PC's own copied and adopted. */
static void big_cover(uint32_t key, int style, uint32_t *work, uint16_t *small, home_art_info_t *info)
{
    if (key & REAL_BIT) {
        uint16_t *tmp = malloc(sizeof(uint16_t) * HOME_ART_L * HOME_ART_L);
        if (tmp && home_pc_art(tmp, NULL)) {
            home_art_adopt(tmp, work, small, info);
            free(tmp);
            return;
        }
        free(tmp);
        /* it went before it was copied: a generated one, from nothing better than the key */
    }
    home_art_paint(key, style, work, small, info);
}

static void bench(uint32_t *work)
{
    uint16_t *small = malloc(sizeof(uint16_t) * HOME_ART_S * HOME_ART_S);
    uint16_t *hero = malloc(sizeof(uint16_t) * (HOME_ART_L + 80) * (HOME_ART_L + 80));
    if (small && hero) {
        home_art_info_t info;
        for (int s = 0; s < HOME_ART_STYLES; s++) {
            double t0 = hal_seconds();
            home_art_paint(home_art_key("bench", "mark", NULL) + (uint32_t)s, s, work, small, &info);
            A.bench[s] = (float)((hal_seconds() - t0) * 1000.0);
        }
        double t0 = hal_seconds();
        uint16_t *tmp = malloc(sizeof(uint16_t) * HOME_ART_L * HOME_ART_L);
        if (tmp) {
            for (int i = 0; i < HOME_ART_L * HOME_ART_L; i++) tmp[i] = (uint16_t)i;
            home_art_adopt(tmp, work, small, &info);
            free(tmp);
        }
        A.bench[HOME_ART_STYLES] = (float)((hal_seconds() - t0) * 1000.0);
        t0 = hal_seconds();
        home_art_hero(work, hero, HOME_ART_L + 80, HOME_ART_L + 80, 30, 24, 24, 0x18e3, true, info.seed);
        A.bench[HOME_ART_STYLES + 1] = (float)((hal_seconds() - t0) * 1000.0);
    }
    free(small);
    free(hero);
}

static void *worker(void *arg)
{
    (void)arg;
    uint32_t *work = malloc(sizeof(uint32_t) * HOME_ART_L * HOME_ART_L);
    for (;;) {
        pthread_mutex_lock(&A.lock);
        hm_art_t *e = NULL;
        for (int i = 0; i < NART && !e; i++)
            if (A.art[i].state == A_QUEUED) e = &A.art[i];
        bool hero = !e && A.hero.state == A_QUEUED;
        int jb = -1;
        for (int i = 0; i < 4 && !e && !hero && jb < 0; i++)
            if (A.job[i].queued) jb = i;
        if (jb >= 0) {
            void (*fn)(void *) = A.job[jb].fn;
            void *arg = A.job[jb].arg;
            A.running_ticket = A.job[jb].ticket;
            A.job[jb].queued = false;
            pthread_mutex_unlock(&A.lock);
            fn(arg);
            pthread_mutex_lock(&A.lock);
            A.running_ticket = 0;
            pthread_mutex_unlock(&A.lock);
            continue;
        }
        bool ben = !e && !hero && A.bench_want == 1;
        if (!work || (!e && !hero && !ben)) {
            /* nothing left (or no memory for the working picture: the jobs are dropped, and asked for again) */
            for (int i = 0; i < NART; i++)
                if (A.art[i].state == A_QUEUED) A.art[i].state = A_FREE;
            if (A.hero.state == A_QUEUED) A.hero.state = A_FREE;
            if (A.bench_want == 1) A.bench_want = 0;
            A.running = false;
            pthread_mutex_unlock(&A.lock);
            break;
        }
        uint32_t key = 0;
        int style = -1;
        if (e) {
            e->state = A_PAINTING;
            key = e->key;
            style = e->style;
        } else if (hero) {
            A.hero.state = A_PAINTING;
            key = A.hero.key;
            style = A.hero.style;
        }
        pthread_mutex_unlock(&A.lock);
        double t0 = hal_seconds();
        if (e) {
            big_cover(key, style, work, e->small, &e->info);
            pthread_mutex_lock(&A.lock);
            e->ms = (float)((hal_seconds() - t0) * 1000.0);
            e->state = A_READY;
            e->serial = ++A.serial;
            pthread_mutex_unlock(&A.lock);
        } else if (hero) {
            home_art_info_t info;
            big_cover(key, style, work, NULL, &info);
            home_art_hero(work, A.hero.px, A.hero.w, A.hero.h, A.hero.cx, A.hero.cy, A.hero.radius, A.hero.ground,
                          A.hero.dark, A.hero.seed);
            pthread_mutex_lock(&A.lock);
            A.hero.ms = (float)((hal_seconds() - t0) * 1000.0);
            /* asked for again while it painted (a new song, a new tone): once more */
            if (A.hero.state != A_QUEUED) A.hero.state = A_READY;
            A.hero.serial = ++A.serial;
            pthread_mutex_unlock(&A.lock);
        } else {
            bench(work);
            pthread_mutex_lock(&A.lock);
            A.bench_want = 2;
            pthread_mutex_unlock(&A.lock);
        }
    }
    free(work);
    return NULL;
}

#ifdef ESP_PLATFORM
static void task_main(void *arg)
{
    worker(arg);
    vTaskDeleteWithCaps(NULL);
}
#endif

/* with the lock held */
static void kick(void)
{
    if (A.running) return;
    A.running = true;
#ifdef ESP_PLATFORM
    /* priority 2 on core 0: under the network's workers (4) and the presents (8); the stack in PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(task_main, "home-art", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS)
        A.running = false;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
    else A.running = false;
#endif
}

/* ---------------------------------------------------------------- the small covers */

static bool alloc_entry(hm_art_t *e)
{
    if (e->small) return true;
    e->small = malloc(sizeof(uint16_t) * HOME_ART_S * HOME_ART_S);
    if (!e->small) return false;
    lv_image_dsc_t *d = &e->dsc;
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = HOME_ART_S;
    d->header.h = HOME_ART_S;
    d->header.stride = HOME_ART_S * 2;
    d->data = (const uint8_t *)e->small;
    d->data_size = HOME_ART_S * HOME_ART_S * 2;
    return true;
}

/* The entry for key when it's ready; else NULL, and it is being painted (queued now if it wasn't). */
static const hm_art_t *lookup(uint32_t key)
{
    pthread_mutex_lock(&A.lock);
    A.stamp++;
    hm_art_t *hit = NULL, *victim = NULL;
    for (int i = 0; i < NART; i++) {
        hm_art_t *e = &A.art[i];
        if (e->state != A_FREE && e->key == key && ((key & REAL_BIT) || e->style == A.force_style)) hit = e;
    }
    if (hit) {
        hit->used = A.stamp;
        const hm_art_t *r = hit->state == A_READY ? hit : NULL;
        pthread_mutex_unlock(&A.lock);
        return r;
    }
    for (int i = 0; i < NART; i++) {
        hm_art_t *e = &A.art[i];
        if (e->state == A_QUEUED || e->state == A_PAINTING) continue;
        if (!victim || e->state == A_FREE || (victim->state != A_FREE && e->used < victim->used)) victim = e;
        if (victim->state == A_FREE) break;
    }
    if (victim && alloc_entry(victim)) {
        lv_image_cache_drop(&victim->dsc); /* its pixels are about to change under anyone who still shows them */
        victim->key = key;
        victim->style = (key & REAL_BIT) ? -1 : A.force_style;
        victim->state = A_QUEUED;
        victim->used = A.stamp;
        kick();
    }
    pthread_mutex_unlock(&A.lock);
    return NULL;
}

const hm_art_t *hm_art_song(const char *artist, const char *album, const char *title)
{
    return lookup(home_art_key(artist, album, title) & ~REAL_BIT);
}

uint32_t hm_art_pc_key(const home_pc_t *pc)
{
    if (!pc->have) return 0;
    uint32_t song = home_art_key(pc->artist, pc->album, pc->title);
    if (song != A.pc_song) {
        A.pc_song = song;
        A.pc_since = hal_seconds();
    }
    if (pc->art && pc->art_ready) return (pc->art_gen & 0x7fffffffu) | REAL_BIT;
    if (pc->art && hal_seconds() - A.pc_since < PENDING_S) return 0; /* its own is on its way */
    return song & ~REAL_BIT;
}

const hm_art_t *hm_art_pc(const home_pc_t *pc)
{
    uint32_t k = hm_art_pc_key(pc);
    return k ? lookup(k) : NULL;
}

const home_art_info_t *hm_art_info(const hm_art_t *a) { return a ? &a->info : NULL; }
unsigned hm_art_serial(const hm_art_t *a) { return a ? a->serial : 0; }
uint32_t hm_art_key(const hm_art_t *a) { return a ? a->key : 0; }

bool hm_art_set(lv_obj_t *img, const hm_art_t *a, unsigned *shown)
{
    unsigned s = hm_art_serial(a);
    if (!a || s == *shown) return false;
    *shown = s;
    lv_image_cache_drop(&a->dsc);
    lv_image_set_src(img, &a->dsc);
    lv_obj_invalidate(img);
    return true;
}

/* ---------------------------------------------------------------- the hero */

bool hm_art_hero(const hm_art_t *a, uint16_t *px, int w, int h, int cx, int cy, int radius, uint16_t ground, bool dark)
{
    if (!a) return false;
    pthread_mutex_lock(&A.lock);
    A.hero.key = a->key;
    A.hero.style = a->style;
    A.hero.seed = a->info.seed;
    A.hero.px = px;
    A.hero.w = w;
    A.hero.h = h;
    A.hero.cx = cx;
    A.hero.cy = cy;
    A.hero.radius = radius;
    A.hero.ground = ground;
    A.hero.dark = dark;
    /* queued (again, if it's painting now: the worker paints it once more when done) */
    A.hero.state = A_QUEUED;
    kick();
    pthread_mutex_unlock(&A.lock);
    return true;
}

unsigned hm_art_hero_done(void)
{
    pthread_mutex_lock(&A.lock);
    unsigned s = A.hero.state == A_READY ? A.hero.serial : 0;
    pthread_mutex_unlock(&A.lock);
    return s;
}

bool hm_art_busy(void)
{
    pthread_mutex_lock(&A.lock);
    bool b = A.running;
    pthread_mutex_unlock(&A.lock);
    return b;
}

/* ---------------------------------------------------------------- other pictures */

unsigned hm_art_job(void (*fn)(void *), void *arg)
{
    pthread_mutex_lock(&A.lock);
    unsigned t = 0;
    for (int i = 0; i < 4; i++) {
        /* the same function asked again before it ran: one run does for both */
        if (A.job[i].queued && A.job[i].fn == fn && A.job[i].arg == arg) {
            t = A.job[i].ticket;
            break;
        }
    }
    for (int i = 0; i < 4 && !t; i++) {
        if (A.job[i].queued) continue;
        A.job[i].fn = fn;
        A.job[i].arg = arg;
        A.job[i].ticket = t = ++A.tickets ? A.tickets : ++A.tickets;
        A.job[i].queued = true;
        kick();
    }
    pthread_mutex_unlock(&A.lock);
    return t;
}

bool hm_art_job_done(unsigned t)
{
    if (!t) return true;
    pthread_mutex_lock(&A.lock);
    bool done = A.running_ticket != t;
    for (int i = 0; i < 4 && done; i++)
        if (A.job[i].queued && A.job[i].ticket == t) done = false;
    pthread_mutex_unlock(&A.lock);
    return done;
}

/* ---------------------------------------------------------------- memory */

void hm_art_trim(void)
{
    pthread_mutex_lock(&A.lock);
    for (int i = 0; i < NART; i++) {
        hm_art_t *e = &A.art[i];
        if (e->state == A_QUEUED || e->state == A_PAINTING) continue;
        lv_image_cache_drop(&e->dsc);
        free(e->small);
        e->small = NULL;
        e->state = A_FREE;
    }
    pthread_mutex_unlock(&A.lock);
}

/* ---------------------------------------------------------------- the dev console */

void hm_art_dev(const char *a)
{
    while (*a == ' ') a++;
    if (!strncmp(a, "style", 5)) {
        /* "hm art style bauhaus" (or its number, or "auto"): every generated cover in that style */
        const char *s = a + 5;
        while (*s == ' ') s++;
        int k = -1;
        for (int i = 0; i < HOME_ART_STYLES; i++)
            if (!strcmp(s, home_art_style_name(i)) || (s[0] == '0' + i && !s[1])) k = i;
        pthread_mutex_lock(&A.lock);
        A.force_style = k;
        pthread_mutex_unlock(&A.lock);
        printf("art: style %s\n", k < 0 ? "from the key" : home_art_style_name(k));
        return;
    }
    if (!strncmp(a, "bench", 5)) {
        /* "hm art bench": a cover of each style, real art's adoption and a hero, timed on the worker */
        pthread_mutex_lock(&A.lock);
        int st = A.bench_want;
        if (st == 0) {
            A.bench_want = 1;
            kick();
        } else if (st == 2) {
            A.bench_want = 0;
        }
        pthread_mutex_unlock(&A.lock);
        if (st != 2) {
            printf("art: timing on the worker; ask again in a few seconds\n");
            return;
        }
        for (int s = 0; s < HOME_ART_STYLES; s++)
            printf("art: %-8s %6.1f ms (%d px, and its %d px copy)\n", home_art_style_name(s), A.bench[s], HOME_ART_L,
                   HOME_ART_S);
        printf("art: real art adopted in %.1f ms; a hero set in %.1f ms\n", A.bench[HOME_ART_STYLES],
               A.bench[HOME_ART_STYLES + 1]);
        return;
    }
    pthread_mutex_lock(&A.lock);
    static const char *const ST[] = { "free", "queued", "painting", "ready" };
    for (int i = 0; i < NART; i++) {
        hm_art_t *e = &A.art[i];
        printf("art %d: %s %08x %s %s, %.1f ms, seed %06x\n", i, ST[e->state], (unsigned)e->key,
               (e->key & REAL_BIT) ? "real" : "generated", home_art_style_name(e->info.style), e->ms,
               (unsigned)e->info.seed);
    }
    printf("art: hero %s, %.1f ms; worker %s\n", ST[A.hero.state], A.hero.ms, A.running ? "running" : "idle");
    pthread_mutex_unlock(&A.lock);
}
