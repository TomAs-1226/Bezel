/* as_diag — the technician's judgement, as code the tools run on the tablet (pure: host-tested).
 *
 *  - Triage: one pass over the robot as last fed, every anomaly the tablet can see ranked by how
 *    likely it is to lose a match: the link and the program, power, CAN, the loop, the robot's own
 *    checks, alerts and health checks, mechanisms, the drivetrain, vision, Systemcore. Each finding
 *    says what it is, the evidence, and the next check. Thresholds are Catalyst Console's
 *    (docs/catalyst-contract.md, docs/systemcore.md), so the tablet and Console never disagree.
 *  - Swerve tracking: each module's commanded state against its actual one, accumulated over samples
 *    (live, or a recording's rows), then judged: one module that doesn't follow while the others do
 *    is a mechanical fault or its motor, not tuning. That comparison is the first thing to look at on
 *    a bad drivetrain run; a jammed drive or a loose wheel otherwise passes for a tuning problem. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cat_model.h"

typedef enum { AS_DG_INFO, AS_DG_WARN, AS_DG_FAULT } as_dg_sev_t;

typedef struct {
    as_dg_sev_t sev;
    int order;              /* within a severity: lower first (the area's place in the triage order) */
    char area[12];          /* link, program, power, can, loop, checks, alerts, mech, swerve, vision, core */
    char what[160];         /* the finding, with its numbers */
    char next[160];         /* the next check that would confirm or clear it */
} as_finding_t;

/* Every finding for the robot `r`, worst first. Returns how many (at most max). */
int as_diag_triage(const cat_robot_t *r, as_finding_t *out, int max);
const char *as_dg_sev_name(as_dg_sev_t s); /* "fault", "warn", "info" */

/* ---- swerve tracking ---- */

#define AS_TRACK_MIN_MPS 0.3   /* commanded slower than this, a module's following can't be judged */

typedef struct {
    int nmod;
    int samples;                /* rows offered */
    int used[4];                /* rows where this module was commanded fast enough to judge */
    double sum_cmd[4], sum_along[4], sum_err2[4], sum_angle[4], max_angle[4];
    double worst_ratio[4];      /* the lowest along/commanded seen (a stall shows here first) */
} as_track_t;

void as_track_init(as_track_t *t, int nmod);
/* One instant: each module's commanded speed (m/s) and angle (rad), and its actual ones. Either state may
 * be the other's 180-degree equivalent (a module optimises its turn): compared as velocity vectors. */
void as_track_add(as_track_t *t, const double cmd_speed[4], const double cmd_angle[4], const double act_speed[4],
                  const double act_angle[4]);

typedef struct {
    int used;
    double follow_pct;          /* mean actual speed along the commanded direction, % of commanded */
    double rms_err;             /* m/s */
    double angle_err_deg;       /* mean |steering error| */
    double worst_angle_deg;
    double worst_follow_pct;
    const char *verdict;        /* "follows", "lags", "steering", "can't judge" */
} as_track_mod_t;

/* The judgement: per module, and overall words saying what the pattern means and what to check. */
typedef struct {
    as_track_mod_t mod[4];
    int nmod;
    char summary[400];
} as_track_verdict_t;

void as_track_judge(const as_track_t *t, as_track_verdict_t *v);

/* A run recorder CSV's header: which columns hold Swerve/ModuleStates and ModuleTargets ("<topic>.fl_speed"
 * ...). cols[0..7] states fl..br speed/angle, cols[8..15] targets; -1 where absent. true when both are
 * there. */
bool as_track_csv_header(const char *line, int cols[16]);
/* One row into the tracker with the columns found above; false for a row it can't use. */
bool as_track_csv_row(as_track_t *t, const char *line, const int cols[16]);

extern const char *const AS_MOD_NAME[4]; /* "fl", "fr", "bl", "br" */
