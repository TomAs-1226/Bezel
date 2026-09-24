/* Catalyst OS storage on the microSD card (ui_storage.h). */
#include "ui_storage.h"
#include "hal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const CS_DIR_NAME[CS_NDIRS] = { "DOCS", "PHOTOS", "AUDIO", "DATA", "LOGS" };

#define USAGE_DEPTH 8
#define USAGE_MAX 20000 /* entries walked before the sum is called partial */

bool cstore_path(cs_dir_t d, const char *file, char *out, size_t n)
{
    const char *sd = hal_sd_root();
    if (!sd || d < 0 || d >= CS_NDIRS) return false;
    int k = file && file[0] ? snprintf(out, n, "%s/CATOS/%s/%s", sd, CS_DIR_NAME[d], file)
                            : snprintf(out, n, "%s/CATOS/%s", sd, CS_DIR_NAME[d]);
    return k > 0 && (size_t)k < n;
}

bool cstore_layout(void)
{
    const char *sd = hal_sd_root();
    if (!sd) return false;
    char p[96];
    snprintf(p, sizeof p, "%s/CATOS", sd);
    if (mkdir(p, 0755) != 0 && errno != EEXIST) return false;
    bool ok = true;
    for (int i = 0; i < CS_NDIRS; i++) {
        snprintf(p, sizeof p, "%s/CATOS/%s", sd, CS_DIR_NAME[i]);
        if (mkdir(p, 0755) != 0 && errno != EEXIST) ok = false;
    }
    return ok;
}

static bool ext_match(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    size_t n = strlen(dot);
    for (const char *p = exts; (p = strchr(p, '.')); p++) {
        size_t m = strcspn(p + 1, ".") + 1;
        if (m == n && !strncasecmp(p, dot, n)) return true;
    }
    return false;
}

static int by_name(const void *a, const void *b)
{
    const cstore_entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1; /* folders first */
    return strcasecmp(x->name, y->name);
}

static int by_time(const void *a, const void *b)
{
    const cstore_entry_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
    return strcasecmp(y->name, x->name); /* same second: the later name (a timestamped one) first */
}

int cstore_list(const char *dir, const char *exts, bool newest_first, cstore_entry_t *out, int max)
{
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d) return -1;
    char *full = malloc(320);
    if (!full) {
        closedir(d);
        return -1;
    }
    int k = 0;
    struct dirent *e;
    while (k < max && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, 320, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool isdir = S_ISDIR(st.st_mode);
        if (exts && (isdir || !ext_match(e->d_name, exts))) continue;
        cstore_entry_t *o = &out[k++];
        snprintf(o->name, sizeof o->name, "%s", e->d_name);
        o->size = isdir ? 0 : (uint64_t)st.st_size;
        o->mtime = st.st_mtime;
        o->dir = isdir;
    }
    closedir(d);
    free(full);
    qsort(out, (size_t)k, sizeof *out, newest_first ? by_time : by_name);
    return k;
}

char *cstore_read(const char *path, size_t cap, size_t *len, bool *truncated)
{
    if (len) *len = 0;
    if (truncated) *truncated = false;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    size_t want = (size_t)size > cap ? cap : (size_t)size;
    char *buf = malloc(want + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    if (truncated) *truncated = (size_t)size > cap;
    return buf;
}

bool cstore_write(const char *path, const void *data, size_t len)
{
    if (!path || !hal_sd_root()) return false;
    char tmp[200];
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - path) : strlen(path);
    if (stem + 5 >= sizeof tmp) return false;
    memcpy(tmp, path, stem);
    memcpy(tmp + stem, ".TMP", 5);
    FILE *f = fopen(tmp, "wb");
    if (!f && errno == ENOENT && cstore_layout()) f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    /* FAT won't rename over a file: the old one goes first */
    unlink(path);
    return rename(tmp, path) == 0;
}

static void usage_walk(char *path, size_t cap, int depth, cstore_usage_t *u)
{
    DIR *d = opendir(path);
    if (!d) return;
    size_t base = strlen(path);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (u->files + u->dirs >= USAGE_MAX) {
            u->truncated = true;
            break;
        }
        if (base + strlen(e->d_name) + 2 >= cap) continue;
        path[base] = '/';
        strcpy(path + base + 1, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                u->dirs++;
                if (depth < USAGE_DEPTH) usage_walk(path, cap, depth + 1, u);
                else u->truncated = true;
            } else {
                u->files++;
                u->bytes += (uint64_t)st.st_size;
            }
        }
        path[base] = 0;
    }
    closedir(d);
}

void cstore_usage(const char *path, cstore_usage_t *u)
{
    memset(u, 0, sizeof *u);
    struct stat st;
    if (!path || stat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        u->files = 1;
        u->bytes = (uint64_t)st.st_size;
        return;
    }
    char *buf = malloc(512);
    if (!buf) return;
    snprintf(buf, 512, "%s", path);
    usage_walk(buf, 512, 0, u);
    free(buf);
}

const char *cstore_size(uint64_t b, char *buf, size_t n)
{
    if (b < 1024) snprintf(buf, n, "%u b", (unsigned)b);
    else if (b < 1048576) snprintf(buf, n, "%.1f kb", b / 1024.0);
    else if (b < 1073741824ull) snprintf(buf, n, "%.1f mb", b / 1048576.0);
    else snprintf(buf, n, "%.2f gb", b / 1073741824.0);
    return buf;
}
