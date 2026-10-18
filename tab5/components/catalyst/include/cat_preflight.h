/* cat_preflight — Catalyst X1's tools/preflight.py, generalised and running on the tablet.
 *
 * PASS / WARN / FAIL / INFO for everything a pit tech checks before a robot goes on the field: the link,
 * the program, e-stop, the Driver Station, battery and brownouts, CAN, the device roster, alerts and
 * health checks, the gyro (a frozen heading means stale Pigeon frames), the pose, cameras, tunables,
 * the wheel-radius calibration, the auto choice and the robot's own Preflight / SystemCheck results.
 * It listens for three seconds (as preflight.py does) so the gyro test sees real motion; any FAIL is a
 * NO-GO. Thresholds are preflight.py's and Console's, so the tablet, the script and the dashboard agree. */
#pragma once
#include "cat_model.h"

typedef enum { CAT_PASS, CAT_WARN, CAT_FAIL, CAT_INFO } cat_verdict_t;

typedef struct {
    cat_verdict_t v;
    char what[28];
    char detail[112];
} cat_check_t;

#define CAT_MAX_CHECKS 48

typedef struct {
    bool running, done;
    double started;
    int n;
    cat_check_t c[CAT_MAX_CHECKS];
    int fails, warns, passes;
    /* gyro: distinct headings seen while listening */
    double headings[16];
    int nheadings;
    int expect_team;
} cat_preflight_t;

void cat_preflight_start(cat_preflight_t *p, double now, int expect_team);
/* Feeds one model update. Returns true once the listen window is over and the checklist is final. */
bool cat_preflight_step(cat_preflight_t *p, const cat_robot_t *r, double now);
double cat_preflight_progress(const cat_preflight_t *p, double now);
