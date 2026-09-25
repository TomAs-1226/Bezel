/* cat_logs — Driver Station and robot logs, read off a microSD card on the tablet.
 *
 *  .wpilog    WPILib DataLog: the 2027 Driver Station's logs (DS:/Dscomm/Status/...) and robot logs
 *             (battery, CPU, CAN, and the "messages" console entry)
 *  .dslog     NI Driver Station, v3/v4: 50 Hz trip time, packet loss, battery, CPU, status (brownout)
 *  .dsevents  NI Driver Station events: timestamped text
 *
 * The result is a summary sized for a screen: each series bucketed into CAT_LOG_POINTS points (battery
 * keeps each bucket's minimum, since the dips are what matter; the rest keep the maximum), headline
 * numbers, and the first events. The formats follow Catalyst Console's dslog.rs and wpilog.rs. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAT_LOG_POINTS 240
#define CAT_LOG_EVENTS 64

typedef enum { CAT_LOG_BATTERY, CAT_LOG_CPU, CAT_LOG_CAN, CAT_LOG_TRIP, CAT_LOG_LOSS, CAT_LOG_SERIES } cat_log_series_t;

typedef struct {
    double t;              /* seconds from the log's start */
    uint8_t level;         /* 0 info, 1 warn, 2 error */
    char text[100];
} cat_log_event_t;

typedef struct {
    char kind[12];
    double duration_s;
    int64_t start_unix;    /* 0 if the format doesn't say */
    uint32_t records;
    bool have[CAT_LOG_SERIES];
    float series[CAT_LOG_SERIES][CAT_LOG_POINTS]; /* NAN where a bucket had no sample */
    float min_battery, max_trip_ms, avg_loss, max_can, max_cpu;
    int brownouts;          /* the log's own brownout flag (dslog status bit, a BrownedOut entry) */
    int dips;               /* battery samples crossing below 6.8 V: brownouts by voltage */
    int nevents;
    cat_log_event_t events[CAT_LOG_EVENTS];
    /* for the digest (cat_log_digest): counted over the whole log, not only the events kept */
    int events_seen;        /* events of any level; once the list is full, a warning or error replaces an info */
    int errors, warnings;   /* events of those levels */
    int overruns;           /* loop overruns: "overrun" in a message, a Loop/.../OverBudget rising edge */
    int can_faults;         /* CAN trouble in a message, a CanDown rising edge */
    int mode_changes;       /* DS:enabled / autonomous / test / estop changes (wpilog) */
    int state_changes;      /* mechanism and state machine State strings changing (wpilog) */
    int health_firings;     /* Catalyst health checks starting to fire (wpilog .../firing) */
    float max_loop_ms;      /* the worst Loop/.../LastMs or MaxMs, 0 none */
    double batt_sum;        /* battery samples' sum and count: the mean */
    uint32_t batt_n;
    char error[64];
    bool below;             /* parser state: the last battery sample was under the floor */
} cat_log_t;

bool cat_log_read(const char *path, cat_log_t *out);
/* Parses from memory (tests; small files). */
bool cat_log_parse(const char *name, const uint8_t *data, size_t len, cat_log_t *out);

/* A parsed log as a bounded plain-text digest for a language model: the headline numbers, each series in
 * `slices` slices, the counters and every kept event. Returns the length written (truncated to n - 1). */
size_t cat_log_digest(const cat_log_t *l, const char *name, char *out, size_t n);
/* The same for a run recorder CSV (t,mark,columns...; cat_sc.h): per column its count, range with the times
 * of the extremes, mean, last value and a sliced trend; the marks. Reads the file (a worker's job). false,
 * with the reason in out, when it can't be read. */
bool cat_csv_digest(const char *path, char *out, size_t n);
