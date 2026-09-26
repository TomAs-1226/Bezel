/* The timed photo's retention decision (home.h): pure, no I/O, no hal — the part worth testing without a
 * card or a camera. See home_capture.c for the half that actually touches the filesystem and the camera. */
#include "home.h"

#include <stdlib.h>

typedef struct {
    int idx;
    double mtime;
} cap_order_t;

static int by_mtime(const void *a, const void *b)
{
    const cap_order_t *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0;
}

int home_capture_plan(const home_cap_file_t *files, int n, int keep, uint64_t free_bytes, uint64_t margin_bytes,
                      bool *del)
{
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) del[i] = false;
    if (keep < 0) keep = 0;

    /* oldest first: a scratch index sorted by mtime, so the loop below only ever deletes the oldest thing
     * still standing, whichever rule (count or space) is still unmet */
    cap_order_t *order = malloc(sizeof *order * (size_t)n);
    if (!order) return 0; /* no scratch space to plan with; the next sweep tries again */
    for (int i = 0; i < n; i++) {
        order[i].idx = i;
        order[i].mtime = files[i].mtime;
    }
    qsort(order, (size_t)n, sizeof *order, by_mtime);

    int remaining = n, marked = 0;
    uint64_t free_after = free_bytes;
    for (int k = 0; k < n; k++) {
        bool over_count = remaining > keep;
        bool under_space = free_after < margin_bytes;
        if (!over_count && !under_space) break;
        int i = order[k].idx;
        del[i] = true;
        marked++;
        remaining--;
        free_after += files[i].bytes;
    }
    free(order);
    return marked;
}
