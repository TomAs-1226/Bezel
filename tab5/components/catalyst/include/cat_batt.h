/* cat_batt — the team's battery fleet: the roster, each battery's uses, what the logs say about each use, and
 * which battery should go in next.
 *
 * The robot can't know which of the team's batteries is in it (RobotIdentity's .battery("MK ES17-12") is the
 * model, published as /Catalyst/Robot/Power/Battery, not the one on the cart), so the tablet is the source of
 * truth: the checklist records which battery went in, when, for which match and how charged. The logs then say
 * how it did: the voltage at rest and at its lowest, brownouts, and how far it sagged under load, V = V0 - I·R,
 * the same model as Catalyst's BatteryResistanceIdentifier, fitted from the voltage and total current the robot
 * logs (/Catalyst/Brownout/{MeasuredVoltage,TotalCurrent}, a PDH's TotalCurrent, SystemStats' BatteryVoltage,
 * the Driver Station's battery).
 *
 * Plain C, no LVGL, no threads: the parse (cat_batt_log) is a worker's job; everything else is small and is the
 * UI's. The fleet lives on the card as JSON (cat_batt_to_json / cat_batt_from_json). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_BATT_MAX 24      /* batteries in the roster */
#define CAT_BATT_USES 30     /* uses kept per battery, the newest last */
#define CAT_BATT_SEEN 256    /* logs already read, by a hash of name, size and time */
#define CAT_BATT_WATCH_MOHM 25.0f
#define CAT_BATT_WATCH_RISE 0.30f

typedef enum { CB_GOOD, CB_WATCH, CB_BAD, CB_RETIRED, CB_NSTATUS } cat_batt_status_t;
extern const char *const CAT_BATT_STATUS[CB_NSTATUS]; /* "good", "watch", "bad", "retired" */

/* how charged it was when it went in */
typedef enum { CC_UNKNOWN, CC_FRESH, CC_RESTED, CC_USED, CC_NCHARGE } cat_batt_charge_t;
extern const char *const CAT_BATT_CHARGE[CC_NCHARGE]; /* "", "fresh", "rested", "used" */

/* where a use's numbers came from */
enum { CU_PICK = 1, CU_LOG = 2, CU_LIVE = 4 };

typedef struct {
    int64_t t;               /* unix: when it went in (the pick), else the log's start */
    char match[24];          /* TBA match key, "2026casj_qm34"; "" unknown */
    char label[12];          /* the match as people say it, "Q34"; "" unknown */
    uint8_t charge;          /* cat_batt_charge_t */
    uint8_t src;             /* CU_* */
    int64_t charged;         /* unix: off the charger at, 0 unknown */
    /* measured (NAN: not measured, never a zero) */
    float v_rest, v_min;     /* V: at rest before the load, and the lowest */
    float r_mohm;            /* internal resistance, mΩ */
    float wh, amps, peak_a;  /* energy drawn, mean current under the robot, the peak */
    float dur_s;             /* how long the numbers cover */
    int16_t brownouts;       /* -1 unknown */
    char log[48];            /* the log the numbers came from */
} cat_batt_use_t;

typedef struct {
    int uid;                 /* stable: labels can be renamed */
    char label[16];          /* "1".."12" by default */
    uint8_t status;          /* cat_batt_status_t */
    bool auto_watch;         /* put on watch by the tablet (not by hand) */
    int year;                /* bought, 0 unknown */
    char notes[120];
    int64_t charged;         /* unix: last marked charged (off the charger), 0 never */
    float r_base;            /* mΩ: the median of its first three measured uses, NAN until then */
    int uses_total;          /* every use ever, not only the kept ones */
    int nuse;
    cat_batt_use_t use[CAT_BATT_USES];
} cat_batt_t;

typedef struct {
    int n, next_uid;
    int64_t updated;
    int nseen;
    uint32_t seen[CAT_BATT_SEEN];
    cat_batt_t b[CAT_BATT_MAX];
} cat_fleet_t;       /* ~110 KB: calloc it (PSRAM) */

void cat_batt_use_init(cat_batt_use_t *u);
/* The default roster: n batteries labelled "1".."n", good. */
void cat_fleet_default(cat_fleet_t *f, int n);
/* Adds a battery (the next free number as its label); its index, or -1 when the roster is full. */
int cat_fleet_add(cat_fleet_t *f);
void cat_fleet_remove(cat_fleet_t *f, int i);
int cat_fleet_find_uid(const cat_fleet_t *f, int uid);
/* A new use at the end of battery i's history (the oldest dropped when full); returns it. */
cat_batt_use_t *cat_batt_push_use(cat_batt_t *b);
/* The newest use of any battery that went in by a pick; its battery in *bi. NULL when none. */
cat_batt_use_t *cat_fleet_last_pick(cat_fleet_t *f, int *bi);
const cat_batt_use_t *cat_batt_last(const cat_batt_t *b);

/* The battery's resistance now: the median of its last three measured uses (NAN: none). *n: how many measured
 * uses it has. */
float cat_batt_r_now(const cat_batt_t *b, int *n);
/* Sets r_base once three uses are measured. */
void cat_batt_update_base(cat_batt_t *b);
/* Should it be on watch by its numbers: resistance over CAT_BATT_WATCH_MOHM, or up CAT_BATT_WATCH_RISE over its
 * baseline (two measured uses at least). The reason in why. */
bool cat_batt_watch_due(const cat_batt_t *b, char *why, size_t n);
/* "#7" for a numeric label, the label itself otherwise */
const char *cat_batt_name(const cat_batt_t *b, char *out, size_t n);

/* ---- the recommendation ---- */
typedef struct {
    int idx;                 /* into f->b */
    float score;
    bool available;          /* false: bad, retired, or just used and not recharged */
    char reason[120];        /* "lowest resistance (18 mΩ), rested 2 h, charged" */
} cat_batt_rank_t;
/* Every battery, best first; the unavailable ones last with why. Returns the count. */
int cat_fleet_rank(const cat_fleet_t *f, int64_t now, cat_batt_rank_t *out, int max);

/* ---- a use's numbers, from samples (a log, or the robot live) ---- */
#define CAT_BATT_WIN_MAX 96
typedef struct {
    double t0, t_last, t_first_load;
    bool any, loaded, have_i;
    double rest_sum;
    int rest_n;
    float v_min, peak_a;
    double wh, amp_s, load_s;
    int brown_flags, dips;
    bool below, brown_seen, brown_last;
    /* the resistance: a least-squares slope of V on I in windows of CAT_BATT_WIN_S, their median */
    double w_t0, w_n, w_i, w_v, w_ii, w_iv;
    float w_imin, w_imax;
    int nwin;
    float win[CAT_BATT_WIN_MAX];
    double last_ti;
} cat_batt_acc_t;
void cat_batt_acc_init(cat_batt_acc_t *a);
/* One sample: seconds (any origin, increasing), bus volts, total amps (NAN when not logged); enabled -1 unknown. */
void cat_batt_acc_add(cat_batt_acc_t *a, double t, float v, float amps, int enabled);
/* A brownout flag's value (the controller's own). */
void cat_batt_acc_brown(cat_batt_acc_t *a, bool on);
/* The use's measured fields from what was added (the rest of *u untouched). false when there was nothing. */
bool cat_batt_acc_finish(const cat_batt_acc_t *a, cat_batt_use_t *u);

/* ---- a log's numbers ---- */
typedef struct {
    cat_batt_use_t m;        /* the numbers (t: the log's start, unix, 0 unknown) */
    int match_type;          /* FMS: 0 none, 1 practice, 2 qualification, 3 elimination; -1 unknown */
    int match_number;
    char event[16];
    bool have_current;
    char error[48];
} cat_batt_log_t;
/* Reads a .wpilog or .dslog off the card (a worker's job: it reads the whole file). */
bool cat_batt_log(const char *path, cat_batt_log_t *out);
bool cat_batt_log_parse(const char *name, const uint8_t *d, size_t len, cat_batt_log_t *out);
uint32_t cat_batt_seen_hash(const char *name, long size, int64_t mtime);
bool cat_fleet_seen(const cat_fleet_t *f, uint32_t h);
void cat_fleet_mark_seen(cat_fleet_t *f, uint32_t h);
/* Which use a log belongs to: the pick for its match (by number) when there is one, else the newest pick at most
 * 10 min after the log started and no more than 4 h before it. Returns the use (and *bi), NULL when none. */
cat_batt_use_t *cat_fleet_attribute(cat_fleet_t *f, const cat_batt_log_t *l, int *bi);
/* Folds a log's numbers into a use (the richer numbers win: a log with current over one without, a log over live). */
void cat_batt_merge(cat_batt_use_t *u, const cat_batt_log_t *l, const char *log_name);

/* ---- the card ---- */
/* The fleet as JSON (malloc'd, NUL-terminated; free it), NULL when out of memory. */
char *cat_batt_to_json(const cat_fleet_t *f, size_t *len);
bool cat_batt_from_json(cat_fleet_t *f, const char *js, size_t len);
/* A bounded plain-text summary of the fleet for a language model: each battery, its recent uses, the ranking. */
size_t cat_batt_summary(const cat_fleet_t *f, int64_t now, char *out, size_t n);

#ifdef __cplusplus
}
#endif
