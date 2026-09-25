/* tba — The Blue Alliance (read API v3) for the team's current event: its matches, its rank and the event's
 * rankings.
 *
 * It runs on the home services' worker (home.h): no thread of its own. The UI calls tba_want() from the
 * app's 10 Hz refresh while the app is on screen; the worker then polls every 60 s (20 s on a day the event
 * is running), with If-Modified-Since so an unchanged answer is a 304 and no body. Nothing polls while
 * nothing wants it. The match alerts (ui_match.c) also call tba_want() once per background poll, every
 * ~2 min on an event day with matches to come and every 30 min otherwise. The last answer per endpoint is kept in PSRAM and on the card under
 * <sd>/CATOS/DATA/tba/, so the app has something to show before the network answers, or without it.
 *
 * The key (X-TBA-Auth-Key, from thebluealliance.com/account) is the UI's to read from kv "tba_key" (NVS:
 * UI thread only) and hand over with tba_config(); it is never logged or shown. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TBA_MATCHES 48
#define TBA_RANKS 100

typedef struct {
    char label[12];            /* "Q34", "SF3", "F2", "QF1-2" */
    int level;                 /* 0 qm, 1 ef, 2 qf, 3 sf, 4 f: the sort order with set and number */
    int set, number;
    int red[3], blue[3];       /* team numbers, 0 empty */
    int red_score, blue_score; /* -1 not played */
    int ours;                  /* our alliance: 0 not in it, 1 red, 2 blue */
    int result;                /* ours: 1 won, -1 lost, 0 tie or not played */
    bool played;
    time_t time;               /* scheduled, 0 unknown */
    time_t predicted;          /* TBA's prediction, 0 none */
} tba_match_t;

typedef struct {
    int rank, team;
    int wins, losses, ties, played;
    double sort1;              /* the first sort order (ranking score); NAN when absent */
    bool dq;
} tba_rank_t;

typedef enum {
    TBA_IDLE,        /* not asked yet */
    TBA_NO_KEY,      /* no key configured */
    TBA_NO_TEAM,     /* no team number */
    TBA_NO_CLOCK,    /* the clock isn't set: which season, which day? */
    TBA_LOADING,     /* asking, nothing to show yet */
    TBA_READY,       /* data (fresh, or from the cache when `offline`) */
    TBA_NO_EVENT,    /* the team has no events this season */
    TBA_REFUSED,     /* 401: the key was refused */
} tba_phase_t;

typedef struct {
    tba_phase_t phase;
    bool offline;              /* the last try didn't reach TBA: what's here is from before */
    char err[64];              /* why, in words, when offline or refused */
    int team, year;
    time_t as_of;              /* wall clock of the newest data shown (0 unknown) */
    unsigned gen;              /* changes with anything here */
    /* the event */
    bool have_event, live;     /* live: today is one of its days */
    char event_key[20], event_name[72], event_where[64];
    char start[11], end[11];   /* "YYYY-MM-DD" */
    /* our matches there, sorted */
    int nmatches;
    tba_match_t matches[TBA_MATCHES];
    /* our status there */
    bool have_status;
    int rank, num_teams, wins, losses, ties;
    char alliance[32];         /* "alliance 3 captain", "" none yet */
    char playoff[48];          /* "playoffs: playing in the semifinals", "" none */
    char status_line[200];     /* overall_status_str, tags stripped */
    /* the event's rankings */
    int nranks;
    char sort_name[32];        /* "ranking score" */
    int sort_precision;
    tba_rank_t ranks[TBA_RANKS];
} tba_state_t;

/* UI thread. key "" clears it; team <= 0 none. Nothing restarts when both are unchanged. */
void tba_config(const char *key, int team);
void tba_want(void);           /* from the app's refresh: keeps polling for a few seconds more */
void tba_refresh(void);        /* poll now (the refresh button) */
unsigned tba_gen(void);        /* cheap: whether tba_get() has anything new */
void tba_get(tba_state_t *out); /* ~9 KB: give it a PSRAM copy, not a stack */
/* Our next unplayed match at the current event, from the last data (no network): false when there is
 * none. For a line like "your next match is Q34 in 12 min" (when = predicted, else scheduled, else 0). */
bool tba_next_match(tba_match_t *out, time_t *when);
/* Our unplayed matches at the current event, in order (at most max into out, the count in *n), from the last
 * data (no network): the match alerts' view, without tba_get()'s ~9 KB copy. Returns the phase; *live and
 * *offline as in tba_state_t, event_key ("" none) may be NULL. */
tba_phase_t tba_upcoming(tba_match_t *out, int max, int *n, bool *live, bool *offline, char *event_key, size_t kn);

#ifdef __cplusplus
}
#endif
