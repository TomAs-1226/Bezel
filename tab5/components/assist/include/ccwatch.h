/* ccwatch — what Claude Code is doing on the owner's PC, as Catalyst Link reports it.
 *
 * Claude Code's hooks post each session's events to the Link (link/catalyst_link/sessions.py); the Link
 * keeps every session's state, current step and an estimate of when the turn will finish, from the
 * owner's own past turns (docs/link-api.md, "Claude Code sessions"). Link's poller calls ccw_poll() every
 * few seconds while the Link is reachable; the UI reads the cached list and drains transition events
 * (a session finished, needs input, failed) to react to them: eyes, a chime, the island, a reminder
 * that repeats until someone taps to acknowledge it.
 *
 * Thread-safe; nothing here blocks except ccw_poll(), which runs on Link's worker thread. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CCW_RUNNING, CCW_WAITING, CCW_DONE, CCW_ERROR } ccw_state_t;

#define CCW_MAX 16

typedef struct {
    char id[48];
    char title[96];        /* the latest prompt's first line */
    char project[48];      /* the working folder's name */
    char step[120];        /* "Edit Shooter.java", "waiting for permission to use Bash", "done" */
    ccw_state_t state;
    uint32_t seq;          /* the Link's per-session count of state changes */
    int tools;             /* tool calls this turn */
    double elapsed_s;      /* active time of this turn (time waiting on the owner excluded), as polled */
    double quiet_s;        /* since the last event, as polled */
    double since_s;        /* since the state last changed, as polled */
    bool have_eta;
    double eta_s, eta_lo_s, eta_hi_s; /* remaining, as polled; lo/hi: the quartiles */
    int eta_samples;
    char basis[120];       /* how the estimate was made, or why there isn't one */
    bool attention;        /* finished, needs input or failed, and nobody has tapped it yet */
    double attention_at;   /* hal_seconds() when it was raised */
    double polled_at;      /* hal_seconds() of the poll: add (now - polled_at) to elapsed, take it off eta */
} ccw_session_t;

/* A change worth reacting to. */
typedef struct {
    char id[48];
    char title[96];
    char project[48];
    ccw_state_t state;     /* what it changed to: CCW_DONE, CCW_WAITING or CCW_ERROR */
    double elapsed_s;
} ccw_event_t;

/* Link's worker: fetches GET /v1/claude/sessions (and sends pending acknowledgements). */
void ccw_poll(void);
/* The Link was unreachable: the list goes stale (kept, marked unavailable). */
void ccw_offline(void);

bool ccw_available(void);                 /* the Link answered the sessions endpoint recently */
uint32_t ccw_rev(void);                   /* bumps when the list changes */
int ccw_list(ccw_session_t *out, int max); /* newest activity first */
int ccw_attention(ccw_session_t *newest); /* how many need a look; the newest of them into *newest */
bool ccw_next_event(ccw_event_t *out);    /* one queued transition, oldest first */
void ccw_ack(const char *id);             /* NULL: every session */
const char *ccw_state_word(ccw_state_t s);

#ifdef __cplusplus
}
#endif
