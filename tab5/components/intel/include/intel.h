/* intel — the tablet's intelligence core: the event's live sources, and what the tablet makes of everything
 * it knows.
 *
 *   Nexus for FRC   the live queue: which match is queuing now, each match's estimated queue, on-deck,
 *                   on-field and start times, announcements and parts requests (frc.nexus/api/v1, the key
 *                   NEXUS_API_KEY from the card). "TBA is the past, Nexus is the next ten minutes": the match
 *                   alarms take their times from here first (ui_match.c).
 *   Statbotics      EPA and win probabilities (api.statbotics.io/v3, no key).
 *   the robot's net the Systemcore vision aggregator (:4810), each Limelight's own REST (:5807) and CTRE's
 *                   Phoenix diagnostics server (:1250), probed and dropped quietly when absent.
 *   insights        short, timely, actionable lines from cheap rules over all of it (intel_rules.c), kept,
 *                   deduplicated and rate-limited here; the ui decides where they show (ui_intel.c).
 *   the briefing    the day or the event in a few lines, on demand or at a set time; a model's summary of it
 *                   only when asked, on a budget.
 *   intents         the ask bar's typed words matched to things the tablet does at once ("timer 5 min",
 *                   "next match", "battery for Q34"); everything else goes to an AI.
 *
 * Threads. Every request runs on the home services' worker (tba.h's tba_ext_register): no thread of its own,
 * and the worker lives only while a poll is due. Results sit behind a mutex; readers copy them out and redraw
 * when a generation counter moves. The rules, the store and the briefing are the UI thread's (a pass every
 * couple of seconds, not per frame). Keys arrive from the UI thread (kv is its), and are never logged. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== Nexus */

#define NX_OURS 24      /* our matches kept */
#define NX_ANN 4        /* announcements kept, newest first */
#define NX_PARTS 4      /* parts requests kept, newest first */

typedef struct {
    char label[12];            /* TBA's way: "Q34", "SF5", "F2"; "P3" practice; "" when Nexus's name didn't parse */
    char name[32];             /* Nexus's own: "Qualification 34" */
    int level, set, number;    /* as tba_match_t: 0 qm, 3 sf, 4 f; -1 practice or unknown */
    char status[24];           /* "Queuing soon", "Now queuing", "On deck", "On field", "" */
    int red[3], blue[3];       /* team numbers, 0 empty */
    int ours;                  /* 0 not ours, 1 red, 2 blue */
    int index;                 /* its place in the event's list (for "2 matches away") */
    time_t queue_at, deck_at, field_at, start_at; /* Nexus's estimates; 0 unknown */
    time_t queued, started;    /* when it actually queued / went on the field; 0 not yet */
    bool replay;               /* a replay of an earlier match */
} nx_match_t;

typedef struct { char text[160]; time_t at; } nx_ann_t;
typedef struct { char text[96]; int team; time_t at; } nx_part_t;

typedef enum {
    NX_OFF,        /* no key: "add NEXUS_API_KEY to KEYS.ENV" */
    NX_NO_EVENT,   /* no event to ask about (TBA hasn't named one) */
    NX_LOADING,    /* asked, nothing yet */
    NX_READY,      /* data (fresh, or the last answer when `stale`) */
    NX_UNKNOWN,    /* 404: this event isn't on Nexus */
    NX_REFUSED,    /* 401/403: the key was refused */
    NX_ERROR,      /* anything else; err says what */
} nx_phase_t;

typedef struct {
    nx_phase_t phase;
    bool stale;                /* the last try failed: what's here is from before */
    char err[64];
    char event_key[20];
    time_t as_of;              /* Nexus's dataAsOfTime; 0 unknown */
    time_t fetched;            /* wall clock of the last good answer */
    char now_queuing[32];      /* Nexus's name, "" none */
    char now_label[12];        /* the same as a label ("Q32") */
    int now_index;             /* its index in the event's list; -1 unknown */
    int nmatches;              /* in the event's list */
    int nours;
    nx_match_t ours[NX_OURS];  /* in the event's order */
    int nann;
    nx_ann_t ann[NX_ANN];
    int nparts;
    nx_part_t parts[NX_PARTS];
    unsigned gen;              /* changes with anything above */
} nx_state_t;

/* UI thread: the key (kv nexus_key; "" none), our team, and a base URL for tests ("" the real one). */
void nx_config(const char *key, int team, const char *base);
/* UI thread: the event to follow ("" none) and how often to ask (seconds; 0 stops polling). ui_match.c sets
 * it from TBA's event and how close our next match is. */
void nx_follow(const char *event_key, double every_s);
void nx_refresh(void);          /* ask now */
unsigned nx_gen(void);
void nx_get(nx_state_t *out);   /* ~6 KB: a PSRAM copy, not a stack */
/* Our match with that label ("Q34") as Nexus last had it; false when Nexus doesn't have it. Cheap. */
bool nx_find(const char *label, nx_match_t *out);
/* Seconds since Nexus last answered (wall clock); a large number when it never has. */
double nx_age_s(void);
/* Nexus's words for a match's status as a short word the island can hold: "queuing", "on deck", ... */
const char *nx_status_short(const char *status);
/* The dev console: a Nexus answer given as text, taken exactly as if it had come over the network. */
void nx_inject(const char *json);

/* Pure (host-tested). A Nexus event document into `out` (whose event_key, gen and fetch fields are kept);
 * `team` marks ours. false when it isn't one. */
bool nx_parse(const char *js, size_t len, int team, nx_state_t *out);
/* "Qualification 34" -> "Q34" (level 0, number 34); "Playoff 5", "Match 5 (R2)" -> "SF5"; "Final 2" -> "F2";
 * "Practice 3" -> "P3" (level -1). false when it can't tell. */
bool nx_label(const char *name, char *label, size_t n, int *level, int *set, int *number);

/* ================================================================== the alarms' timing (pure, host-tested)
 *
 * When each reminder for one match rings, given the sources. `start` is the match's time: Nexus's estimated
 * start when it has one, else TBA's predicted, else scheduled (src says which). The queue reminder rings at
 * start - queue_min, or earlier at Nexus's estimated queue time less a walk (NX_WALK_S) when that comes first,
 * or at once when Nexus says the match is queuing (or on deck, or on the field) and it hasn't rung. The match
 * reminder rings at start - match_min, or at once when Nexus puts the match on deck or the field. */
#define NX_WALK_S (5 * 60)
typedef enum { MT_SRC_NONE, MT_SRC_SCHEDULED, MT_SRC_PREDICTED, MT_SRC_NEXUS } mt_src_t;
typedef struct {
    time_t start;              /* the match's time; 0 unknown */
    mt_src_t src;
    time_t queue_at;           /* Nexus's estimated queue time; 0 none */
    int stage;                 /* Nexus's status: 0 none/soon, 1 now queuing, 2 on deck, 3 on field */
} mt_times_t;
/* The best times for a match from TBA's (predicted, scheduled) and Nexus's (may be NULL). */
void mt_times(time_t tba_predicted, time_t tba_scheduled, const nx_match_t *nx, mt_times_t *out);
/* 0 for "not yet", 1 the queue reminder is due now, 2 the match reminder is due now. `fired` bit 0 queue,
 * bit 1 match. The match one wins when both are due (a late start rings once). */
int mt_due(const mt_times_t *t, time_t now, int queue_min, int match_min, unsigned fired);
/* When the queue (kind 0) or match (1) reminder will ring; 0 never (off, or no time). */
time_t mt_ring_at(const mt_times_t *t, int kind, int lead_min);
const char *mt_src_name(mt_src_t s); /* "nexus", "tba predicted", "scheduled", "" */
int nx_stage(const char *status);   /* 0..3 as mt_times_t.stage */

/* ================================================================== Statbotics */

#define SB_MATCHES 24

typedef struct {
    char label[12];            /* "Q34" */
    time_t time;               /* scheduled (or predicted), 0 unknown */
    int ours;                  /* 1 red, 2 blue */
    double win;                /* our chance to win, 0..1; NAN unknown */
    double red_pred, blue_pred;/* predicted scores; NAN unknown */
    bool played;
    int red_score, blue_score; /* -1 not played */
} sb_match_t;

typedef enum { SB_IDLE, SB_LOADING, SB_READY, SB_ERROR } sb_phase_t;

typedef struct {
    sb_phase_t phase;
    bool stale;
    char err[64];
    int team, year;
    time_t fetched;
    /* the season (team_year) */
    bool have_year;
    double epa, epa_sd;        /* total points; NAN unknown */
    int rank, team_count;      /* worldwide by EPA; 0 unknown */
    double percentile;         /* 0..1; NAN unknown */
    int state_rank, state_count;
    int wins, losses, ties;
    /* the event (team_event) */
    bool have_event;
    char event_key[20];
    double event_epa;          /* NAN unknown */
    int event_rank, event_teams;
    int ev_wins, ev_losses, ev_ties;
    /* our matches there */
    int nmatches;
    sb_match_t matches[SB_MATCHES];
    unsigned gen;
} sb_state_t;

void sb_config(int team);                          /* UI thread; <= 0 stops */
void sb_follow(const char *event_key, bool live);  /* UI thread: the event, and whether it runs today */
void sb_refresh(void);
unsigned sb_gen(void);
void sb_get(sb_state_t *out);                      /* ~3 KB */
bool sb_find(const char *label, sb_match_t *out);  /* our match's prediction, cheap */

/* Pure (host-tested). Each into `out`, leaving what it doesn't carry. false when it isn't one. */
bool sb_parse_team_year(const char *js, size_t len, sb_state_t *out);
bool sb_parse_team_event(const char *js, size_t len, sb_state_t *out);
bool sb_parse_matches(const char *js, size_t len, int team, sb_state_t *out);

/* ================================================================== the robot's network */

#define RN_CAMS 8
#define RN_DEVS 48

typedef struct {
    char name[32], ip[16];
    bool nt;                   /* holds an NT session with the robot (the aggregator's ntConnected) */
    bool answered;             /* its :5807/status answered the last time it was asked */
    double fps, temp, cpu, ram;/* NAN unknown */
    int pipeline;              /* -1 unknown */
    char pipe_type[20];
    double age_s;              /* since its last answer; NAN never */
} rn_cam_t;

typedef struct {
    char model[20], name[40], firmware[20], bus[16];
    int id;
} rn_dev_t;

typedef struct {
    bool host_known;           /* a robot is connected: its address is known */
    char host[48];
    /* the aggregator */
    int agg;                   /* -1 not asked, 0 absent, 1 answering */
    int ncams;
    rn_cam_t cams[RN_CAMS];
    /* Phoenix's diagnostics server */
    int phoenix;               /* -1 not asked, 0 absent, 1 answering */
    char phoenix_version[24];
    int ndevs;
    rn_dev_t devs[RN_DEVS];
    time_t checked;            /* wall clock of the last pass */
    unsigned gen;
} rn_state_t;

/* UI thread: the robot's address ("" none: not connected), and the cameras NetworkTables names (for a robot
 * without the aggregator), a comma list. */
void rn_config(const char *host, const char *nt_cams);
void rn_refresh(void);
unsigned rn_gen(void);
void rn_get(rn_state_t *out);                      /* ~5 KB */

/* Pure (host-tested). */
int rn_parse_cameras(const char *js, size_t len, rn_cam_t *out, int max);  /* the aggregator's list */
bool rn_parse_ll_status(const char *js, size_t len, rn_cam_t *cam);        /* a Limelight's /status */
int rn_parse_phoenix(const char *js, size_t len, rn_dev_t *out, int max);  /* getdevices; -1 not one */

/* ================================================================== insights */

typedef enum { IC_MATCH, IC_ROBOT, IC_POWER, IC_EVENT, IC_DAY, IC_SERVICE, IC_N } intel_cat_t;
#define IC_ALL ((1u << IC_N) - 1)

typedef struct {
    char key[40];              /* what makes two the same ("queue:Q34"): never said twice */
    int cat;                   /* intel_cat_t */
    int prio;                  /* 1 good to know, 2 worth a look, 3 act now */
    char text[160];            /* the line: "Q34 queues in ~20 min: battery #7 · FL lagged 30% last match" */
    char why[200];             /* where it came from, for the list and the AIs */
    char app[24];              /* the app that acts on it ("batteries"), "" none */
    double first, last;        /* hal_seconds() first raised, last confirmed by the rules */
    time_t wall;               /* wall clock when first raised */
    double until;              /* kept after its rule stops raising it, until then (0: dropped at once) */
    bool said;                 /* the island said it */
    bool dismissed;
} intel_insight_t;

/* One of our matches to come, as the rules see it. */
typedef struct {
    char label[12];
    time_t start;              /* the best time (mt_times) */
    mt_src_t src;
    time_t queue_at;           /* Nexus's, 0 none */
    char status[24];           /* Nexus's, "" none */
    int ours;                  /* 1 red, 2 blue */
    int partners[2], opponents[3];
    double win;                /* Statbotics, NAN */
    int away;                  /* matches until it, from Nexus's now-queuing; -1 unknown */
} intel_up_t;

/* Everything the rules look at: the UI thread fills it (the robot, the battery pick, the schedule, the day)
 * from what it already holds and the sources' copies. Absent is never zero: NAN, -1, "" or a false have_. */
typedef struct {
    time_t now;                /* wall clock; the rules say nothing time-bound while it's unset */
    double mono;               /* hal_seconds() */
    int team;
    /* the event (TBA) */
    bool event_live;
    char event_key[20], event_name[72];
    int rank, num_teams, wins, losses, ties;
    int nup;
    intel_up_t up[6];          /* our next matches, soonest first */
    char batt_line[100];       /* ui_batt's recommendation for the next match, "" none */
    char batt_pick[24];        /* the battery picked for the next match, "" none */
    /* Nexus */
    nx_phase_t nexus;
    char now_queuing[12];
    int behind_min;            /* the field vs TBA's schedule, from Nexus (+ late); 0 unknown or on time */
    char ann[160];             /* the newest announcement, "" none */
    time_t ann_at;
    char part[96];             /* the newest parts request from another team, "" none */
    int part_team;
    time_t part_at;
    /* Statbotics */
    bool have_epa;
    double epa, percentile;
    int epa_rank, epa_count;
    /* the robot */
    bool connected, enabled;
    int session;               /* counts enabled sessions that have ended (0 none yet): the "last run" */
    double session_end;        /* hal_seconds() the last one ended; 0 none */
    double battery_v;          /* NAN */
    int brownouts_last;        /* in the last enabled session; -1 no session yet */
    double vmin_last;          /* its lowest voltage; NAN */
    int errors, warnings;
    char worst_alert[112];
    bool can_down;
    int devices_missing, devices_total;
    bool loop_over;
    double loop_max_ms;
    int mods;                  /* swerve modules judged in the last session (0 none) */
    double follow_pct[4];      /* each module's following, % of its command; NAN not judged */
    char code_sha[16];         /* the robot's build commit ("" unknown) */
    bool code_dirty;
    /* Systemcore's agent */
    bool have_agent;
    bool throttled;
    double core_temp;          /* NAN */
    double disk_free_pct;      /* NAN */
    /* cameras (the robot's own vision state, and the aggregator's) */
    int cams_bad;
    char cam_bad[48];          /* "limelight-front: disconnected" */
    double cam_hot;            /* the hottest camera, NAN */
    char cam_hot_name[32];
    /* Phoenix */
    int fw_mixed;              /* device models with more than one firmware version */
    char fw_line[96];
    /* the day */
    bool have_weather;
    double temp, hi, lo;
    int rain_pct, rain_hour;   /* the first hour ahead with rain >= 40 %, -1 none */
    char weather_word[20];
    char next_event[80];       /* the calendar's next event today, "14:00 team meeting"; "" none */
    time_t next_event_at;
    char next_alarm[24];       /* "tomorrow 6:30"; "" none */
    int tablet_pct;            /* -1 unknown */
    bool tablet_charging;
    /* services */
    bool nexus_key;
} intel_in_t;

/* Pure (host-tested): every insight the rules raise for this snapshot, at most max. */
int intel_rules(const intel_in_t *in, intel_insight_t *out, int max);

#define INTEL_MAX 24

typedef struct {
    unsigned cats;             /* IC_* bits the island may say */
    int quiet_from, quiet_to;  /* hours; equal: no quiet hours */
    int brief_hhmm;            /* the daily briefing's time, -1 off */
    bool ai;                   /* a model's summary with the briefing (on a budget) */
} intel_cfg_t;

void intel_cfg_default(intel_cfg_t *c);
/* "cats,quiet_from,quiet_to,brief,ai" (kv "intel"). */
void intel_cfg_format(const intel_cfg_t *c, char *out, size_t n);
bool intel_cfg_parse(const char *s, intel_cfg_t *c);

/* The store: the rules' output merged in (UI thread, a pass every couple of seconds). New keys are added;
 * known ones keep when they were first raised and whether they were said; ones the rules stop raising stay
 * until their `until`. Returns how many are kept. */
int intel_merge(const intel_insight_t *fresh, int n, double now);
/* The next one the island should say now, by the config (its category, quiet hours at `hour`), the rate
 * limits (at most one a SAY_GAP_S unless it's prio 3, at most SAY_HOUR an hour) and never twice; marks it
 * said. false: nothing. */
bool intel_next_say(const intel_cfg_t *cfg, int hour, double now, intel_insight_t *out);
/* The kept ones, most important first (dismissed last), at most max. */
int intel_list(intel_insight_t *out, int max);
bool intel_top(intel_insight_t *out);     /* the most important one not dismissed */
void intel_dismiss(const char *key);
void intel_clear(void);
unsigned intel_gen(void);                 /* changes with the store */

/* The briefing: the day, the event, the next matches, the robot, the queue, as plain lines. Pure. */
size_t intel_briefing(const intel_in_t *in, char *out, size_t n);

/* ================================================================== the ask bar's intents (pure) */

typedef enum {
    IN_NONE,       /* not the tablet's: an AI's */
    IN_OPEN,       /* open an app: arg the name */
    IN_TIMER,      /* a countdown: seconds, arg a label */
    IN_STOPWATCH,
    IN_BRIGHT,     /* value 0..100, or delta +/-20 */
    IN_VOLUME,
    IN_MUTE,
    IN_TONE,       /* arg "dark" / "light" */
    IN_HOME,       /* home mode */
    IN_SLEEP,
    IN_NEXT,       /* "next match" */
    IN_BATTERY,    /* "battery for Q34": arg the match, "" the next */
    IN_QUEUE,      /* "queue", "nexus", "what's queuing" */
    IN_EPA,        /* "epa", "rank", "statbotics" */
    IN_BRIEF,      /* "briefing", "brief me" */
    IN_WEATHER,
    IN_NOTE,       /* "note <text>": arg */
    IN_REMIND,     /* "remind me in 10 min to X": seconds, arg */
    IN_ALARM,      /* "alarm 6:30": value hhmm */
    IN_INSIGHTS,   /* "what's up", "insights" */
    IN_ROBOT,      /* "robot", "status", "triage" -> triage by the technician */
} intel_intent_kind_t;

typedef struct {
    intel_intent_kind_t kind;
    char arg[96];
    double seconds;
    int value;
    bool delta;
} intel_intent_t;

/* The typed words to an intent; IN_NONE for a question. `apps` is a list of app names the tablet has
 * (NULL-terminated) so "weather" opens the app while "weather tomorrow" asks. */
intel_intent_t intel_intent(const char *text, const char *const *apps);

/* ================================================================== the AIs' tools
 *
 * Answers for the insights, event_live and robot_network tools, from what the core holds (on the calling
 * AI's worker; a source older than a minute or two is asked again and waited on a few seconds). `args` is the
 * tool's JSON input. The insights' text comes from the UI's last pass (intel_post_text). */
bool intel_tool(const char *name, const char *args, char *out, size_t n);
bool intel_is_tool(const char *name);
/* The UI thread's words for the tools and the AIs' context: the insights listed and the briefing. */
void intel_post_text(const char *insights, const char *briefing);

/* The briefing summarized by a model (the assistant's OpenAI key), on the assistant's worker, at most a few
 * times a day (yday: today's day of the year, for the budget). false with `why` when it can't start. */
bool intel_summarize(const char *briefing, int team, int yday, char *why, size_t wn);
/* 0 none, 1 asking, 2 done (out: the summary), 3 failed (out: why). */
int intel_summary(char *out, size_t n);
unsigned intel_summary_gen(void);

/* ================================================================== start-up */

/* Once, from the UI thread at start-up: registers the worker's turn. */
void intel_init(void);
/* UI thread, once a second: starts the home worker when a source has a request due (and only then). */
void intel_pump(double now);
/* The sources' state for settings and the services list: a line each, "nexus: add NEXUS_API_KEY to
 * KEYS.ENV", "statbotics: EPA 31.2 · rank 312 of 3700". */
int intel_services(char lines[][96], int max);

#ifdef __cplusplus
}
#endif
