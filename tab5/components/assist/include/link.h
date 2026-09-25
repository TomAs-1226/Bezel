/* link — Catalyst Link, the PC side of the tablet (tab5/link/, docs/link-api.md).
 *
 * The Link runs on the team's laptop next to the robot's code. Through it the tablet reads the code,
 * proposes patches on their own branches, drops work orders in an inbox for the PC's agent, uploads
 * recordings, and (optionally) reaches Claude without keeping an API key on the tablet.
 *
 * Everything the tablet sends while the Link is unreachable waits in an outbox on microSD and goes
 * when it comes back (store and forward). Calls here block: use them from a worker thread, except
 * link_status() and link_outbox_count(), which read a cache the worker keeps fresh. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;       /* an address is set (or found by mDNS) */
    bool reachable;        /* answered /link/status in the last 10 s */
    bool claude;           /* it serves the Messages API (with a key, or through Claude Code) */
    char url[96];          /* "http://192.168.1.20:8765" */
    char name[48];         /* the PC's name */
    char repo[64];         /* the robot project's folder name */
    char branch[48];       /* the branch checked out there (never written to) */
    int inbox_open;        /* work orders not yet done */
    int patches;           /* patch branches proposed from the tablet */
    int outbox;            /* items waiting on microSD */
    double last_ok;        /* hal_seconds() of the last good answer */
    bool auth;             /* the Link accepted the token (reachable with auth false: the token is wrong) */
    /* How the Link reaches Claude: "api" (its ANTHROPIC_API_KEY) or "claude-code" (the owner's Claude
     * subscription through Claude Code on the PC; the model is Claude Code's, not the one the tablet
     * asks for). "" when the Link is older or has Claude off. */
    char claude_via[16];
} link_status_t;

typedef struct {
    char id[64];
    char title[96];
    char status[16];       /* "open", "claimed", "done", "rejected" / patches: "proposed", "merged", "dropped" */
    char branch[80];       /* patches */
    char when[24];         /* "2026-09-23 14:12" */
    char check[12];        /* patches: the compile check, "none", "passed", "failed" or "timeout" */
} link_item_t;

void link_init(void);                                  /* starts the poller */
void link_configure(const char *url, const char *token); /* url "" → find it by mDNS */
void link_status(link_status_t *out);
int link_outbox_count(void);

/* Lists, from the poller's cache (refreshed every few seconds while something reads them). */
int link_inbox(link_item_t *out, int max);
int link_patches(link_item_t *out, int max);

/* Blocking calls, JSON in and out (docs/link-api.md). Return the HTTP status, or -1 unreachable.
 * `out` receives the response body, NUL-terminated. */
int link_get(const char *path, char *out, int max);
int link_post(const char *path, const char *json, char *out, int max);
/* Queue a POST for store-and-forward: sent now if possible, else from the outbox later. */
bool link_post_queued(const char *path, const char *json);
/* Upload a file from microSD (a recording, a clip, a log) to the Link's files/; queued if offline. */
bool link_upload(const char *sd_path, const char *name);

/* ---- pairing (docs/link-api.md, "Pairing") ----
 * The tablet finds Links on the LAN (mDNS _catalyst-link._tcp), asks one to pair, and the Link shows a six-digit
 * code on the PC; the code typed here gets the Link's token back. Each call only hands the work to the poller's
 * thread and returns: poll link_pair_status() (and link_peers()) from the UI. */
typedef struct {
    char name[48];         /* the PC's name, as its mDNS record says */
    char url[96];          /* "http://192.168.1.20:8765" */
} link_peer_t;
void link_discover(void);                           /* browse mDNS for Links (~2 s) */
/* What the last browse found; *busy while it's still looking. */
int link_peers(link_peer_t *out, int max, bool *busy);

typedef enum {
    LINK_PAIR_IDLE,
    LINK_PAIR_ASKING,      /* asking the Link to show a code */
    LINK_PAIR_CODE,        /* the code is on the PC: waiting for it here */
    LINK_PAIR_CHECKING,    /* the code is on its way */
    LINK_PAIR_DONE,        /* paired: link_pair_take() has the result */
    LINK_PAIR_FAILED,      /* `msg` says why; start again */
} link_pair_state_t;
typedef struct {
    link_pair_state_t state;
    char msg[96];          /* what to say: "that code isn't it: 4 tries left" */
    char name[48], url[96];
    int tries_left;        /* after a wrong code; 0 unknown */
    unsigned gen;          /* changes with anything above */
} link_pair_t;
void link_pair_start(const char *url, const char *device);
void link_pair_code(const char *code);              /* the six digits typed */
void link_pair_cancel(void);
void link_pair_status(link_pair_t *out);
/* Once LINK_PAIR_DONE: the address and token, handed over once (the UI saves them with hal_kv and calls
 * link_configure). Never log the token. */
bool link_pair_take(char *url, size_t un, char *token, size_t tn, char *name, size_t nn);

/* The URL and headers for the Messages API through the Link: "http://…/v1/messages" and the token. */
bool link_messages_endpoint(char *url, size_t n, char *headers, size_t hn);

#ifdef __cplusplus
}
#endif
