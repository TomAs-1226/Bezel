/* Catalyst OS storage: a fixed layout on the microSD card, and the file helpers the apps share.
 *
 *   <sd>/CATOS/DOCS     documents: notes, .TXT and .MD to read
 *   <sd>/CATOS/PHOTOS   pictures (the lens app's snapshots stay in <sd>/lens; photos shows both)
 *   <sd>/CATOS/AUDIO    recordings and sounds
 *   <sd>/CATOS/DATA     app data: the calendar's EVENTS.TXT
 *   <sd>/CATOS/LOGS     the OS's own logs
 *
 * Names are upper-case 8.3, so the layout reads the same on a card formatted without long names.
 * Everything here touches the card and blocks: call it from a worker (hal_thread), except a small write
 * the user asked for. Plain C and malloc (PSRAM on the tablet), so a worker may call any of it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum { CS_DOCS, CS_PHOTOS, CS_AUDIO, CS_DATA, CS_LOGS, CS_NDIRS } cs_dir_t;
extern const char *const CS_DIR_NAME[CS_NDIRS]; /* "DOCS", "PHOTOS", … */

/* "<sd>/CATOS/<dir>[/<file>]" into out; false with no card (or too long). */
bool cstore_path(cs_dir_t d, const char *file, char *out, size_t n);
/* Makes <sd>/CATOS and its folders where missing. false with no card, or when the card won't take them. */
bool cstore_layout(void);

typedef struct {
    char name[64];
    uint64_t size;
    time_t mtime;
    bool dir;
} cstore_entry_t;
/* A folder's entries, skipping "." and "..", into out (at most max). exts: NULL for all, else a list like
 * ".TXT.MD" matched against the name's extension, case-insensitively, with folders left out. Sorted by
 * name, or newest first. Returns the count, or -1 when the folder can't be opened. */
int cstore_list(const char *dir, const char *exts, bool newest_first, cstore_entry_t *out, int max);

/* The whole file (up to cap bytes) NUL-terminated in a malloc'd buffer: free() it. NULL when it can't be
 * read. *truncated (may be NULL) says the file was longer than cap. */
char *cstore_read(const char *path, size_t cap, size_t *len, bool *truncated);
/* Writes the file through a .TMP beside it, so a pulled card leaves the old one or the new one. Makes the
 * layout first when the folder is missing. */
bool cstore_write(const char *path, const void *data, size_t len);

typedef struct {
    uint64_t bytes;
    int files, dirs;
    bool truncated; /* stopped early: too deep or too many entries */
} cstore_usage_t;
/* Adds up everything under path (a file counts itself). */
void cstore_usage(const char *path, cstore_usage_t *u);

/* "12 b", "3.4 kb", "5.6 mb", "1.2 gb" */
const char *cstore_size(uint64_t bytes, char *buf, size_t n);
