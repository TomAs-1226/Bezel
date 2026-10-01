/* The intel component's network side (not pure): one GET into a PSRAM body, and each source's turn on the
 * home worker. Private to components/intel. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* GET `url` with extra `headers` ("Name: value\r\n"..., may be NULL) and If-Modified-Since `lm` when given.
 * On 200 *body is a malloc'd (PSRAM), NUL-terminated copy of at most `max` bytes and *len its length, and
 * lm_out (when given, 40 bytes) the response's Last-Modified; a body that didn't arrive whole is -1. Returns
 * the status, -1 when nothing answered. At least MIN_GAP_S between two requests (the C6 has stopped answering
 * after bursts). Blocking: the worker's call. */
int ix_get_url(const char *url, const char *headers, const char *lm, char *lm_out, char **body, size_t *len,
               size_t max, int timeout_ms);

/* Wi-Fi is up with an address (the internet sources ask only then). Any thread; cheap. */
bool ix_online(void);

/* Each source's turn on the worker: at most one request per call; true when it made one. */
bool nx_work(double now);
bool sb_work(double now);
bool rn_work(double now);
/* Whether a source has a request due now (the UI's pump starts the worker for it). */
bool nx_due(double now);
bool sb_due(double now);
bool rn_due(double now);
