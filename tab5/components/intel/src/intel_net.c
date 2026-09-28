/* The intel component's requests: one GET helper, and the sources' turns on the home services' worker.
 *
 * No thread of its own. tba.h's tba_ext_register hands the worker intel_turn(), which gives each source a
 * chance in turn and lets at most one of them make a request per pass, so the PC's media and Home Assistant
 * keep theirs. The worker only lives while something wants it: the UI thread's pump (intel_pump, once a
 * second) calls tba_ext_want() when a source has a request due, and the worker ends ~15 s after the last. */
#include "intel.h"
#include "intel_net.h"
#include "intel_priv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hal.h"
#include "tba.h"

#define MIN_GAP_S 1.0

int ix_get_url(const char *url, const char *headers, const char *lm, char *lm_out, char **body, size_t *len,
               size_t max, int timeout_ms)
{
    static pthread_mutex_t gap_lock = PTHREAD_MUTEX_INITIALIZER;
    static double last;
    pthread_mutex_lock(&gap_lock);
    double since = hal_seconds() - last;
    if (last > 0 && since < MIN_GAP_S) usleep((useconds_t)((MIN_GAP_S - since) * 1e6));
    last = hal_seconds();
    pthread_mutex_unlock(&gap_lock);
    *body = NULL;
    *len = 0;
    if (lm_out) lm_out[0] = 0;
    char *hdr = malloc(1024); /* a key may be in it: heap (PSRAM), wiped after */
    if (!hdr) return -1;
    snprintf(hdr, 1024, "%s%s%s%s", headers ? headers : "", lm && lm[0] ? "If-Modified-Since: " : "",
             lm && lm[0] ? lm : "", lm && lm[0] ? "\r\n" : "");
    hal_http_req_t rq = { .url = url, .headers = hdr, .timeout_ms = timeout_ms > 0 ? timeout_ms : 8000 };
    int status = -1;
    char err[64];
    hal_http_t *h = hal_http_open(&rq, &status, err, sizeof err);
    memset(hdr, 0, 1024);
    free(hdr);
    if (!h) return -1;
    if (status == 200) {
        size_t cap = 8192, n = 0;
        char *b = malloc(cap);
        bool whole = false;
        while (b) {
            if (n + 2048 + 1 > cap) {
                if (cap >= max) break;
                size_t nc = cap * 2 > max ? max : cap * 2;
                char *nb = realloc(b, nc);
                if (!nb) break;
                b = nb;
                cap = nc;
            }
            int r = hal_http_read(h, b + n, (int)(cap - n - 1));
            if (r == 0) whole = true;
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (b && whole) {
            b[n] = 0;
            *body = b;
            *len = n;
            if (lm_out) snprintf(lm_out, 40, "%s", hal_http_last_modified(h));
        } else {
            free(b);
            status = -1;
        }
    }
    hal_http_close(h);
    return status;
}

bool ix_online(void)
{
    hal_net_t n;
    hal_net(&n);
    return n.up;
}

/* ---------------------------------------------------------------- the worker's turn */

static void intel_turn(double now)
{
    /* one request a pass, the queue first: it is the one that moves by the minute */
    if (nx_work(now)) return;
    if (sb_work(now)) return;
    rn_work(now);
}

void intel_net_init(void) { tba_ext_register(intel_turn); }

/* UI thread, once a second: the worker is started (or kept) only when a request is due. */
void intel_pump(double now)
{
    if (nx_due(now) || sb_due(now) || rn_due(now)) tba_ext_want();
}
