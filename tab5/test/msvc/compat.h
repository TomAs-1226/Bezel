/* MSVC shim for the GCC/POSIX-isms in tab5's portable code (host tests only) */
#pragma once
#define __attribute__(x)
#define _USE_MATH_DEFINES
#include <stddef.h>
#include <string.h>
#include <time.h>

static inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }

static inline void *memmem(const void *h, size_t hn, const void *n, size_t nn)
{
    const unsigned char *a = h;
    if (!nn) return (void *)h;
    for (size_t i = 0; i + nn <= hn; i++)
        if (a[i] == *(const unsigned char *)n && !memcmp(a + i, n, nn)) return (void *)(a + i);
    return NULL;
}
