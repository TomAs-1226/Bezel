/* cat_sc — Systemcore and Catalyst depth: the data side of the systemcore, motors, states, controls and
 * recorder screens (docs/systemcore.md is the contract).
 *
 *   catalyst-agent  the controller's own account of itself, polled over HTTP from the robot's port 9010
 *   motor history   every motor by serial number: the agent's file, or /Catalyst/MotorHistory/Rows
 *   controls        /Catalyst/Controls/.manifest: which button does what, read and never written
 *   state lanes     every /Catalyst/…/State (and the robot's mode, and Autonomy's decisions) recorded from
 *                   boot, so a transition that happened before anyone opened a screen is still there
 *   recorder        chosen NT topics at 50 Hz into a CSV on microSD, with a mark column
 *
 * Nothing here draws or includes LVGL, and nothing includes the HAL: the few things borrowed from the
 * tablet (HTTP, threads, the card, the clock) are handed in through cat_sc_env_t, so the parsers are
 * unit-tested on a laptop (test/test_sc.c) and the workers run unchanged in the simulator. The pure half
 * is cat_sc.c; the worker threads are cat_sc_io.c.
 *
 * Workers never touch the UI's copy of anything. Each keeps its own result under a mutex and bumps a
 * revision; the UI thread copies out when the revision moves. Absent is never zero: every number that
 * may be missing is NAN until something says otherwise. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nt4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== environment */

typedef struct {
    /* One HTTP GET into `out` (NUL-terminated). Returns the status, or -1 when nothing answered. Blocking;
     * only the workers call it. */
    int (*http_get)(const char *url, int timeout_ms, char *out, int max, int *len);
    /* A worker thread (core 0 on the tablet). */
    bool (*spawn)(const char *name, void *(*fn)(void *), void *arg, int stack);
    const char *(*sd_root)(void);          /* NULL when there is no card */
    bool (*wall)(struct tm *out);          /* local wall-clock time, for file names */
} cat_sc_env_t;

/* Once at boot, after the NT client exists: starts the state recorder (so lanes cover the whole session).
 * The agent poller and the recorder start on first use. Safe to call again; later calls do nothing. */
void cat_sc_init(nt4_client_t *nt, const cat_sc_env_t *env);

/* The agent's port (catalyst_agent.py PORT) and how often Console polls it (app.js AGENT_POLL_MS). */
#define CAT_AGENT_PORT 9010
#define CAT_AGENT_POLL_S 3.0
/* Console stops asking, and says the agent isn't installed, after this many silent polls. */
#define CAT_AGENT_GIVE_UP 3

/* ================================================================== catalyst-agent /api/system */

#define CAT_AG_CORES 8
#define CAT_AG_ZONES 6
#define CAT_AG_MOUNTS 6
#define CAT_AG_DIRS 6
#define CAT_AG_PROCS 8
#define CAT_AG_CAN 6
#define CAT_AG_NICS 8
#define CAT_AG_LOG 40
#define CAT_AG_CAMS 8

typedef struct { int core; double percent, mhz; } cat_ag_core_t;
typedef struct { char zone[24]; double celsius; } cat_ag_zone_t;
typedef struct { char mount[32], device[32], fs[12]; double total, used, free; } cat_ag_mount_t;
typedef struct { char path[40]; double bytes; } cat_ag_dir_t;
typedef struct { int pid; char name[24]; double cpu, rss; } cat_ag_proc_t;
typedef struct {
    char name[12];
    bool up;
    char state[16];        /* "ERROR-ACTIVE", "ERROR-WARNING", "ERROR-PASSIVE", "BUS-OFF"; "" unknown */
    double bitrate, restarts, rx, tx, rx_err, tx_err, rx_drop, tx_drop;
} cat_ag_can_t;
typedef struct {
    char name[16];
    bool up;
    char mac[18];
    double mbps;           /* NAN unknown (and the kernel's -1) */
    char addrs[64];        /* "10.58.5.2/24, 172.26.0.1/24" */
    bool wireless;
    double quality, dbm;
} cat_ag_nic_t;
typedef struct {
    char name[32], ip[16], type[20], pipeline[20];
    bool nt, reachable;    /* holds an NT session with the robot program; answered its own /status */
    double fps, temp, cpu, ram;
} cat_ag_cam_t;

typedef struct {
    bool valid;
    /* identity */
    char hostname[32], os[48], os_version[16], kernel[32], model[48], agent_version[12];
    double uptime_s;
    /* cpu */
    int ncores;
    cat_ag_core_t cores[CAT_AG_CORES];
    bool have_load;
    double load[3];
    char cpu_model[32];
    bool have_throttle;    /* vcgencmd answered: absent is not "all clear" */
    bool thr_under_now, thr_capped_now, thr_now, thr_soft_now, thr_since, thr_under_since;
    int nzones;
    cat_ag_zone_t zones[CAT_AG_ZONES];
    /* memory, bytes */
    double mem_total, mem_avail, mem_used, mem_cached;
    /* storage */
    int nmounts, ndirs;
    cat_ag_mount_t mounts[CAT_AG_MOUNTS];
    cat_ag_dir_t dirs[CAT_AG_DIRS];
    /* processes, by CPU */
    int proc_count, nprocs;
    cat_ag_proc_t procs[CAT_AG_PROCS];
    /* frame-level CAN counters per interface */
    int ncan;
    cat_ag_can_t can[CAT_AG_CAN];
    /* network interfaces (not lo, not can*) */
    int nnics;
    cat_ag_nic_t nics[CAT_AG_NICS];
    /* the robot program's unit */
    bool have_program;
    char unit[24], state[16], substate[16];
    double restarts, running_s, memory;
    int pid;
    int nlog;              /* the last CAT_AG_LOG lines, newest last */
    char log[CAT_AG_LOG][144];
    /* Limelights, as the OS's vision aggregator sees them */
    bool cams_available;
    char cams_reason[64];
    int ncams;
    cat_ag_cam_t cams[CAT_AG_CAMS];
    /* the motor-history summary the snapshot carries */
    int mh_devices, mh_motors;
    double sampled_at;     /* the agent's clock, epoch seconds */
} cat_agent_t;

/* /api/system → struct. Returns false (and out->valid false) when it isn't a snapshot. */
bool cat_agent_parse(const char *json, size_t len, cat_agent_t *out);

typedef enum {
    CAT_AG_IDLE,           /* nobody has asked yet, or no robot */
    CAT_AG_LOOKING,        /* asked, no answer yet */
    CAT_AG_OK,             /* answering */
    CAT_AG_ABSENT,         /* CAT_AGENT_GIVE_UP silent polls: not installed (or the robot is off) */
} cat_ag_state_t;

typedef struct {
    cat_ag_state_t state;
    char host[64];         /* the robot's address, from the NT connection */
    double age_s;          /* since the last good answer; NAN never */
    double fetch_ms;       /* how long the last answer took */
    int misses;
    char error[64];
    uint32_t rev;
} cat_ag_status_t;

/* Keeps the poller asking every CAT_AGENT_POLL_S for the next few seconds. Call it at 10 Hz while a
 * screen shows agent data; it stops asking by itself when nobody does (Console: only while the page is
 * open). */
void cat_agent_want(void);
void cat_agent_status(cat_ag_status_t *out);
/* Copies the newest snapshot when it is newer than *rev (and updates *rev). */
bool cat_agent_get(cat_agent_t *out, uint32_t *rev);
/* Builds "http://<host>:9010<path>" from the NT connection; false with no robot. */
bool cat_agent_url(char *out, size_t n, const char *path);

/* ================================================================== motor history */

#define CAT_MH_MAX 64
#define CAT_MH_IDS 6
#define CAT_MH_SESSIONS 24
/* MotorHistory.Config.hotCelsius: at or above this a second counts as hot (also Console's peak mark). */
#define CAT_MH_HOT_C 70.0

typedef struct { int id; char name[40], bus[12], firmware[20]; double first_ms, last_ms; } cat_mh_ident_t;
typedef struct { double start_ms, seconds, running_s, revs, peak_a, peak_c, hot_s; } cat_mh_session_t;

typedef struct {
    char serial[36], model[20], kind[10];      /* kind: motor, encoder, imu, device */
    /* the identity in force (the last one) */
    char bus[12], name[40], firmware[20];
    int id;                                    /* -1 unknown */
    double powered_s, running_s, loaded_s, revs, peak_a, peak_c, hot_s, energy_j, first_ms, last_ms;
    int boots;
    int identities;                            /* how many it has carried, including this one */
    uint64_t sticky;
    char hw_rev[12], made[12];
    /* only from the agent's file: the last CAT_MH_IDS identities and CAT_MH_SESSIONS boots, oldest first */
    int nids, nsess;
    cat_mh_ident_t ids[CAT_MH_IDS];
    cat_mh_session_t sess[CAT_MH_SESSIONS];
} cat_mh_motor_t;

typedef struct {
    int n;
    char source[24];       /* "catalyst-agent", "networktables", "" */
    bool clock_trusted;    /* false while the robot's clock read 1970: dates are relative */
    double updated_ms;     /* NAN unknown */
    char summary[112];     /* /Catalyst/MotorHistory/Summary, when published */
    char error[96];
} cat_mh_meta_t;

/* One /Catalyst/MotorHistory/Rows string ("serial|model|kind|bus|id|name|firmware|poweredS|runningS|
 * loadedS|revolutions|peakA|peakC|hotS|energyJ|boots|firstSeenMs|lastSeenMs|identities|stickyFaults"). */
bool cat_mh_parse_row(const char *line, cat_mh_motor_t *out);
/* The agent's /api/motor-history (the file as it is on disk). Parsed one device at a time, so a big file
 * needs no big token buffer. Returns the devices read (≤ max), or -1 with `err` set. */
int cat_mh_parse_doc(const char *json, size_t len, cat_mh_motor_t *out, int max, cat_mh_meta_t *meta);

/* Keeps the motor-history fetch going (every 30 s while wanted; the file changes at most that often). */
void cat_mh_want(void);
/* Copies the newest table into `out` (≤ max) when it is newer than *rev. Returns the count, or -1 when
 * nothing changed. */
int cat_mh_get(cat_mh_motor_t *out, int max, cat_mh_meta_t *meta, uint32_t *rev);

/* Hot-worn: hot seconds on record (Console marks any). Renumbered: more than one identity. */
static inline bool cat_mh_hot(const cat_mh_motor_t *m) { return m->hot_s > 0 || m->peak_c >= CAT_MH_HOT_C; }
static inline bool cat_mh_renumbered(const cat_mh_motor_t *m) { return m->identities > 1; }

/* ================================================================== controls manifest */

#define CAT_CTL_TOPIC "/Catalyst/Controls/.manifest"
#define CAT_CTL_MAX 48

typedef struct {
    char control[32];      /* "Left stick", "R1", "D-pad up", "Left bumper + Y" */
    char action[128];
    char controller[24];   /* "Driver" when the robot says nothing (Console drivers.js) */
    bool combo;            /* declared, never inferred from a "+" */
} cat_ctl_t;

/* JSON array of {control, action, controller?, combo?}; entries without control and action are dropped.
 * Returns the bindings read, or -1 if it isn't a JSON array. */
int cat_controls_parse(const char *json, size_t len, cat_ctl_t *out, int max);

/* Where a control sits on a generic gamepad, for drawing: 0..1 across and down a pad outline, and the
 * kind of input. False for a control name it doesn't recognise (drawn in the list only). */
typedef enum { CAT_IN_BUTTON, CAT_IN_STICK, CAT_IN_TRIGGER, CAT_IN_BUMPER, CAT_IN_DPAD, CAT_IN_PAD } cat_input_kind_t;
bool cat_controls_place(const char *control, float *x, float *y, cat_input_kind_t *kind);

/* ================================================================== state lanes */

#define CAT_ST_LANES 16
#define CAT_ST_NAMES 24
#define CAT_ST_RING 512

typedef enum { CAT_LANE_MODE, CAT_LANE_MACHINE, CAT_LANE_MECH, CAT_LANE_AUTONOMY, CAT_LANE_OTHER } cat_lane_kind_t;

typedef struct {
    uint32_t ms;           /* when it entered the state, ms since cat_sc_init */
    int16_t s;             /* index into names, or -1: no value (disconnected, not published) */
} cat_st_edge_t;

typedef struct {
    char name[40];         /* "Arm", "Superstructure", "robot mode", "autonomy tasks" */
    char topic[96];
    cat_lane_kind_t kind;
    int nnames;
    char names[CAT_ST_NAMES][28];
    int head, count;       /* ring of edges, newest at head - 1 */
    uint32_t total;        /* transitions ever recorded (the ring keeps the last CAT_ST_RING) */
    cat_st_edge_t e[CAT_ST_RING];
} cat_st_lane_t;

void cat_st_lane_init(cat_st_lane_t *l, const char *name, const char *topic, cat_lane_kind_t kind);
/* Records `state` at `ms` if it differs from the current one (NULL: no value). True when it changed. A
 * lane with more than CAT_ST_NAMES distinct states files the rest under the last name, "…". */
bool cat_st_feed(cat_st_lane_t *l, uint32_t ms, const char *state);
/* The k-th newest edge (0 = the current state). False past the ring. */
bool cat_st_edge(const cat_st_lane_t *l, int k, cat_st_edge_t *out);
/* Changes into a real state (not into "no value") within [t0, t1). */
int cat_st_count(const cat_st_lane_t *l, uint32_t t0, uint32_t t1);
/* The current state's name, or NULL when there is no value. */
const char *cat_st_current(const cat_st_lane_t *l);
/* Milliseconds spent in `s` within [t0, t1), up to `now`. */
uint32_t cat_st_time_in(const cat_st_lane_t *l, int s, uint32_t t0, uint32_t t1, uint32_t now);

/* The recorder's lanes, read under its lock (it samples at 10 Hz on its own thread). */
void cat_states_lock(void);
void cat_states_unlock(void);
int cat_states_count(void);
const cat_st_lane_t *cat_states_lane(int i);
uint32_t cat_states_now(void);          /* ms since cat_sc_init, the lanes' clock */
uint32_t cat_states_rev(void);          /* bumps on every recorded change */

/* A CAT_LANE_MACHINE lane's own counters (CatalystStateMachineLog.java:192-196): all five, not the
 * tablet's earlier three-of-five subset (docs/catalyst-integration.md §14). `base` is the lane's topic
 * with its trailing "/State" removed, e.g. "/Catalyst/Arm" for a lane whose topic is
 * "/Catalyst/Arm/State" — the same base the states screen already builds for Counters/Transitions et al.
 * NAN for any counter the robot hasn't published. */
typedef struct {
    double transitions, rejections, timeouts, aborts, yields;
} cat_state_counters_t;
void cat_state_counters(const char *base, cat_state_counters_t *out);

/* ================================================================== autonomy, beyond the fixed lanes
 *
 * `AutonomyBoard` (frc.lib.catalyst.autonomy.AutonomyBoard, since 2.1.0) publishes more under
 * /Catalyst/Autonomy/ than the four lanes discover() tracks as state lanes above (Tasks/Running,
 * Chase/Target, Intent/Guess, Authority/Binding): free text and figures that don't fit the lane model,
 * because they aren't a small enumerated set of named states — they're reasoning strings and numbers
 * that can change every loop. docs/catalyst-integration.md §14 is the gap list this closes;
 * AutonomyBoard.java is the schema these read against. Fetched fresh from NT4 on every call (nothing
 * kept, nothing recorded) — cheap the way the states screen's own Counters topics read already are. Absent is
 * never zero, per this file's own rule: a number the robot hasn't published is NAN, a string is "". */
typedef struct {
    char why[80];             /* Chase/Why: why the chaser picked what it picked */
    char rejected[96];        /* Chase/Rejected: comma-separated, or the library's own "—" for none */
    double intent_hit_rate;   /* Intent/HitRate, 0..1 */
    double intent_samples;    /* Intent/Samples */
    char intent_explain[96];
    double authority_scale;   /* Authority/Scale, 0..1 (published every loop when present) */
    char authority_explain[96];
    double power_deficit_a;   /* Power/Deficit, amps asked for beyond the budget */
    double power_shed_a;      /* Power/Shed, amps actually found by shedding */
    double power_short_a;     /* Power/Short, amps still missing after shedding (0: the budget was met) */
    char power_explain[96];
} cat_autonomy_extra_t;
/* True when any field came back; every field is independently NAN/"" when the robot hasn't published it. */
bool cat_autonomy_extra(cat_autonomy_extra_t *out);

typedef struct {
    bool have_valid;
    bool valid;                /* Situation/Valid: false when the snapshot is blindfolded */
    double confidence;         /* Situation/Confidence, 0..1; only when localization is valid */
    double speed_mps;          /* Situation/Speed; only when the motion reading is valid */
    double slip;               /* Situation/Slip; only when the traction reading is valid */
    double bus_volts;          /* Situation/BusVolts */
    double headroom_a;         /* Situation/Headroom, amps; only when the power reading is valid */
    char binding[32];          /* Situation/Binding: which limit is currently binding, "" if none yet */
} cat_autonomy_situation_t;
bool cat_autonomy_situation(cat_autonomy_situation_t *out);

/* ================================================================== recorder */

#define CAT_REC_COLS 48
#define CAT_REC_HZ 50

typedef struct {
    char topic[96];
    int index;             /* -1 a scalar; else the element of a numeric array (Pose2d x = 0, …) */
    char label[40];        /* the CSV column: the topic, with "[i]" for an element */
} cat_rec_col_t;

/* CSV, RFC 4180: a field is quoted when it holds a comma, a quote or a line break. Returns the length. */
size_t cat_csv_field(char *out, size_t n, const char *s);
/* "t,mark,<label>,…" and a line break. Returns the length (0 if it didn't fit). */
size_t cat_csv_header(char *out, size_t n, const cat_rec_col_t *cols, int ncols);
/* One row: t in seconds (3 decimals), the mark number or an empty field, then each value (%.6g) or an
 * empty field when absent — never 0. Returns the length (0 if it didn't fit). */
size_t cat_csv_row(char *out, size_t n, double t, int mark, const double *v, const bool *have, int ncols);
/* The elapsed time of a recording, from its file's last complete row (its tail is enough). */
bool cat_csv_last_t(const char *tail, size_t n, double *t);
/* Columns in a header line. */
int cat_csv_count_cols(const char *line);

typedef struct {
    bool active;
    double elapsed;        /* s */
    uint32_t rows, marks;
    uint64_t bytes;
    int ncols;
    char path[160];        /* the file being (or last) written */
    char error[80];
    uint32_t seq;          /* bumps with every row */
} cat_rec_status_t;

/* Starts recording `cols` at CAT_REC_HZ into <sd>/runs/run-YYYYMMDD-HHMMSS.csv. False (and `err`) with
 * no card, no columns, or a file that won't open. */
bool cat_rec_start(const cat_rec_col_t *cols, int ncols, char *err, size_t errn);
void cat_rec_stop(void);
/* The next row carries the next mark number. */
void cat_rec_mark(void);
void cat_rec_status(cat_rec_status_t *out);
/* The newest row's values (for sparklines). Returns the column count. */
int cat_rec_latest(double *v, bool *have, int max);

typedef struct {
    char name[48];         /* "run-20270314-102144.csv" */
    char path[160];
    long bytes;
    double duration_s;     /* NAN unreadable */
    int channels;
} cat_run_t;

/* The recordings on the card, newest first. */
int cat_runs_list(cat_run_t *out, int max);

#ifdef __cplusplus
}
#endif
