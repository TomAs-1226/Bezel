/* home mode's timed photo (home.h): the hal-touching half. home_capture_policy.c has the retention
 * decision itself, pure and unit-tested; this file is the glue around it — the camera, the clock, the card.
 *
 * home_capture_tick() is polled once a second from ui_home_mode.c's frame hook (whether or not home mode is
 * on screen right now). When a shot is due it fires a one-shot worker (hal_thread, the same pattern as the
 * lens app's own clip upload): camera on, wait for a frame, sweep the retention policy to make room, encode
 * and write the photo, camera off. One worker at a time (`shooting`); everything else is read and written
 * under `g_lock`, since the worker and the UI thread's tick both touch it. */
#include "home_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define RETRY_S 30.0         /* a failed attempt tries again sooner than a full period */
#define CAP_MAX_FILES 1024   /* a sweep never looks at more of its own photos than this at once */
#define CAP_MAX_DAYS 256     /* day folders it will try to tidy up after trimming */

typedef struct {
    bool enabled;
    int period_min, keep;
    bool shooting;
    double next_due;          /* hal_seconds(); a shot is due once now has passed this */
    home_capture_status_t st;
} cap_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cap_t G = { .keep = HOME_CAP_KEEP_DEFAULT };

static void set_reason(const char *s) { snprintf(G.st.reason, sizeof G.st.reason, "%s", s); }

void home_capture_config(bool enabled, int period_min, int keep)
{
    pthread_mutex_lock(&g_lock);
    bool was_on = G.enabled;
    G.enabled = enabled;
    G.period_min = period_min > 0 ? period_min : 5;
    G.keep = keep > 0 ? keep : HOME_CAP_KEEP_DEFAULT;
    G.st.enabled = enabled;
    if (enabled && !was_on) {
        /* the first shot waits a full period, not "as soon as the setting is on": a moment to step away
         * from the desk before the camera does anything, the same courtesy a real webcam light would buy */
        G.next_due = hal_seconds() + G.period_min * 60.0;
        G.st.reason[0] = 0;
    }
    if (!enabled) {
        G.st.reason[0] = 0;
        G.next_due = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void home_capture_status(home_capture_status_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = G.st;
    pthread_mutex_unlock(&g_lock);
}

/* ---- the card: <sd>/CATOS/PHOTOS/HOME/YYYY-MM-DD/HHMMSS.jpg ----
 * Its own subtree under the existing CATOS/PHOTOS folder — obviously photos, and clear of the lens app's
 * own <sd>/lens — a day at a time so a card browsed by hand isn't one folder with thousands of files. The
 * photos app's own listing only reads CATOS/PHOTOS itself (not subfolders), so these don't appear there;
 * that's deliberate, not an oversight — this tree is retention-swept automatically and unattended, which the
 * gallery's own photos (and the lens app's snapshots) should never be. */

static bool cap_mkdir(const char *p) { return mkdir(p, 0755) == 0 || errno == EEXIST; }

static bool cap_day_dir(const char *sd, const struct tm *tm, char *out, size_t n)
{
    char catos[64], photos[80], home[96];
    snprintf(catos, sizeof catos, "%s/CATOS", sd);
    snprintf(photos, sizeof photos, "%s/PHOTOS", catos);
    snprintf(home, sizeof home, "%s/HOME", photos);
    if (!cap_mkdir(catos) || !cap_mkdir(photos) || !cap_mkdir(home)) return false;
    snprintf(out, n, "%s/%04d-%02d-%02d", home, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return cap_mkdir(out);
}

/* Walks every day folder under CATOS/PHOTOS/HOME, applies home_capture_plan (the ring size, and the free-
 * space floor), deletes what it marks, and rmdir()s any day left empty. Updates G.st.kept. Bounded: it
 * only ever looks at (and only ever deletes within) this one subtree. */
static void cap_sweep(const char *sd)
{
    char home[96];
    snprintf(home, sizeof home, "%s/CATOS/PHOTOS/HOME", sd);
    DIR *dp = opendir(home);
    if (!dp) {
        pthread_mutex_lock(&g_lock);
        G.st.kept = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    home_cap_file_t *files = calloc(CAP_MAX_FILES, sizeof *files);
    char (*paths)[224] = calloc(CAP_MAX_FILES, sizeof *paths);
    char (*days)[192] = calloc(CAP_MAX_DAYS, sizeof *days);
    int n = 0, ndays = 0;
    if (files && paths && days) {
        struct dirent *d;
        while ((d = readdir(dp))) {
            if (d->d_name[0] == '.') continue;
            char day[192];
            snprintf(day, sizeof day, "%s/%s", home, d->d_name);
            struct stat st;
            if (stat(day, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (ndays < CAP_MAX_DAYS) snprintf(days[ndays++], sizeof days[0], "%s", day);
            DIR *dd = opendir(day);
            if (!dd) continue;
            struct dirent *f;
            while (n < CAP_MAX_FILES && (f = readdir(dd))) {
                if (f->d_name[0] == '.') continue;
                const char *dot = strrchr(f->d_name, '.');
                if (!dot || strcasecmp(dot, ".jpg")) continue;
                char full[224];
                snprintf(full, sizeof full, "%s/%s", day, f->d_name);
                struct stat fst;
                if (stat(full, &fst) != 0) continue;
                files[n].mtime = (double)fst.st_mtime;
                files[n].bytes = (uint64_t)fst.st_size;
                snprintf(paths[n], sizeof paths[0], "%s", full);
                n++;
            }
            closedir(dd);
        }
    }
    closedir(dp);

    int marked = 0;
    if (files && paths && n) {
        uint64_t total, free_b;
        if (!hal_sd_space(&total, &free_b)) free_b = UINT64_MAX; /* unknown: don't trim on the space rule alone */
        bool *del = calloc((size_t)n, sizeof *del);
        if (del) {
            marked = home_capture_plan(files, n, G.keep, free_b, (uint64_t)HOME_CAP_MARGIN_MB * 1024 * 1024, del);
            for (int i = 0; i < n; i++)
                if (del[i]) unlink(paths[i]);
            free(del);
        }
    }
    if (days) for (int i = 0; i < ndays; i++) rmdir(days[i]); /* harmlessly fails on anything not empty */

    pthread_mutex_lock(&g_lock);
    G.st.kept = n - marked;
    pthread_mutex_unlock(&g_lock);
    free(files);
    free(paths);
    free(days);
}

static void *cap_worker(void *arg)
{
    (void)arg;
    bool ok = false;
    char reason[64] = "";
    const char *sd = hal_sd_root();
    if (!sd) {
        snprintf(reason, sizeof reason, "no microSD card");
        goto done;
    }
    if (!hal_camera_start()) {
        snprintf(reason, sizeof reason, "the camera didn't start");
        goto done;
    }
    pthread_mutex_lock(&g_lock);
    G.st.live = true;
    pthread_mutex_unlock(&g_lock);
    {
        int w, h;
        const uint16_t *px = NULL;
        for (int i = 0; i < 50 && !px; i++) {
            px = hal_camera_frame(&w, &h);
            if (!px) usleep(20000);
        }
        if (!px) {
            snprintf(reason, sizeof reason, "no frame from the camera");
            goto stop;
        }
        cap_sweep(sd); /* room (and the ring) enforced before adding one more */
        struct tm tm;
        char dir[96], path[224];
        if (!hal_rtc_get(&tm) || !cap_day_dir(sd, &tm, dir, sizeof dir)) {
            snprintf(reason, sizeof reason, "couldn't make CATOS/PHOTOS/HOME");
            goto stop;
        }
        snprintf(path, sizeof path, "%s/%02d%02d%02d.jpg", dir, tm.tm_hour, tm.tm_min, tm.tm_sec);
        ok = hal_camera_snapshot(path);
        if (!ok) snprintf(reason, sizeof reason, "couldn't save the photo");
    }
stop:
    hal_camera_stop();
    pthread_mutex_lock(&g_lock);
    G.st.live = false;
    pthread_mutex_unlock(&g_lock);
done:
    pthread_mutex_lock(&g_lock);
    G.shooting = false;
    double now = hal_seconds();
    if (ok) {
        G.st.last_shot = now;
        G.st.kept++;
        G.st.reason[0] = 0;
        G.next_due = now + G.period_min * 60.0;
    } else {
        set_reason(reason[0] ? reason : "couldn't save the photo");
        G.next_due = now + RETRY_S; /* try again well before the next full period */
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void home_capture_tick(double now, bool home_active, bool camera_free)
{
    static double next_check;
    if (now < next_check) return;
    next_check = now + 1.0; /* once a second is plenty against a multi-minute period */

    pthread_mutex_lock(&g_lock);
    bool enabled = G.enabled, shooting = G.shooting;
    double due = G.next_due;
    pthread_mutex_unlock(&g_lock);
    if (!enabled || shooting) return;

    if (!home_active) return; /* the setting stays on; it just isn't home mode's turn */
    if (!hal_sd_root()) {
        pthread_mutex_lock(&g_lock);
        set_reason("no microSD card");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!camera_free) {
        /* the lens app has the camera, or someone's just using the tablet: neither is the moment for an
         * unattended photo, and trying anyway would fight the lens app for hal_camera_start/stop */
        pthread_mutex_lock(&g_lock);
        set_reason("the camera is busy");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (due != 0 && now < due) return; /* not due yet: leave whatever reason is already showing */

    pthread_mutex_lock(&g_lock);
    G.shooting = true;
    pthread_mutex_unlock(&g_lock);
    if (!hal_thread("home-cam", cap_worker, NULL, 8192)) {
        pthread_mutex_lock(&g_lock);
        G.shooting = false;
        set_reason("couldn't start the capture worker");
        G.next_due = now + RETRY_S;
        pthread_mutex_unlock(&g_lock);
    }
}
