/* cat_model — a Catalyst robot, as the tablet sees it over NetworkTables.
 *
 * cat_model_update() reads the NT topic table (docs/catalyst-contract.md) into one plain struct the
 * screens draw from: identity, mode, battery, loop, CAN, the device roster, alerts and health checks,
 * mechanisms (discovered, not configured), pose and modules, vision, Systemcore, power channels,
 * tunables, the auto chooser and the robot's own preflight / system-check results. Every field has a
 * `have_` flag or a count: a value the robot isn't publishing is absent, never 0. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "nt4.h"

#define CAT_MAX_DEVICES 96
#define CAT_MAX_ALERTS 48
#define CAT_MAX_MECHS 16
#define CAT_MAX_TUNABLES 48
#define CAT_MAX_AUTOS 24
#define CAT_MAX_CHANNELS 24
#define CAT_MAX_CAMERAS 8
#define CAT_MAX_FINDINGS 32

typedef enum { CAT_SEV_INFO, CAT_SEV_WARN, CAT_SEV_ERROR } cat_sev_t;

typedef struct {
    char bus[12];          /* normalised: "" "0" "can_s0" → "can_s0"; "rio"; a CANivore's name */
    int id;
    char type[24];         /* "TalonFX", "CANcoder", "Pigeon2", ... */
    char name[40];
    int connected;         /* 1 yes, 0 no, -1 unknown (1.x publishes no per-device state) */
    double temp_c, current_a; /* NAN when unknown */
} cat_device_t;

typedef struct {
    cat_sev_t sev;
    char source[24];       /* "[Subsystem]" or the health check's subsystem */
    char text[112];
    bool health;           /* from HealthMonitor rather than AlertManager */
} cat_alert_t;

typedef enum { CAT_MECH_ROTATIONAL, CAT_MECH_LINEAR, CAT_MECH_FLYWHEEL, CAT_MECH_OTHER } cat_mech_kind_t;

typedef struct {
    char name[32];
    cat_mech_kind_t kind;
    char state[32];
    double pos, setpoint;  /* degrees, meters or RPS by kind; NAN absent */
    double vel;
    double current_a, temp_c;
    int at_goal;           /* -1 unknown */
    char unit[8];
} cat_mech_t;

typedef struct {
    char key[96];          /* full NT path */
    char name[40];
    char group[24];
    char unit[12];
    double min, max, step; /* NAN when the manifest leaves them out */
    double value;
    bool have;
    bool is_bool;
} cat_tunable_t;

typedef struct {
    char name[32];
    char state[16];        /* OK, NO_TARGETS, DISCONNECTED, STALE, HOT, LOW_FPS, REJECTING */
    char detail[48];
    double fps, temp_c;
    bool connected;
} cat_camera_t;

typedef struct {
    int channel;
    char what[32];
    double amps;           /* NAN unless the PDH itself is published */
} cat_channel_t;

typedef struct {
    cat_sev_t sev;         /* ERROR = blocker */
    char text[112];
} cat_finding_t;

typedef struct {
    /* link */
    bool connected;
    char address[64];
    double rtt_ms;
    uint32_t topics;
    double connected_s;

    /* identity */
    bool have_identity;
    char name[40];
    int team;
    char catalyst_version[24], code_version[40], wpilib_version[24], git_sha[16];
    char controller[24];
    bool line2;            /* 2.x (Systemcore) line: ControlWord / Systemcore topics seen */

    /* mode */
    bool have_mode;
    bool enabled, autonomous, test, estop, fms, ds;
    bool have_alliance, red;
    int station;
    char opmode[32];
    double match_time;     /* NAN: no clock */

    /* power */
    bool have_battery;
    double battery_v, brownout_v, predicted_v, total_current;
    bool browned_out, brownout_risk;
    int nchannels;
    char pd_module[16];
    cat_channel_t channels[CAT_MAX_CHANNELS];
    bool have_pd_live;
    double pd_voltage, pd_total;
    double pd_amps[24];     /* NAN unless the PDH is published */

    /* loop */
    bool have_loop;
    double loop_last_ms, loop_avg_ms, loop_max_ms;
    bool loop_over;

    /* CAN */
    int ncan_util;
    double can_util[5];    /* fractions, per bus can_s0..s4 (2.x) or one bus (1.x) */
    bool can_down;
    int ndevices;
    cat_device_t devices[CAT_MAX_DEVICES];
    bool have_roster;
    int motors_expected, motors_connected, cams_expected, cams_connected;
    bool have_controller;
    bool controller_connected;
    char controller_kind[24];

    /* alerts + health */
    int nalerts, n_errors, n_warnings, n_infos;
    cat_alert_t alerts[CAT_MAX_ALERTS];

    /* mechanisms */
    int nmechs;
    cat_mech_t mechs[CAT_MAX_MECHS];

    /* pose */
    bool have_pose;
    double pose_x, pose_y, pose_rad;
    int nmodules;
    double module_speed[4], module_angle[4], target_speed[4], target_angle[4];
    bool have_targets;
    double heading_deg;
    int npath;
    double path[64 * 3];

    /* vision */
    bool have_vision;
    int vision_level;      /* 0 OK, 1 DEGRADED, 2 BLIND */
    char vision_summary[64];
    int ncameras;
    cat_camera_t cameras[CAT_MAX_CAMERAS];
    int tag_seen;          /* a Limelight's tid, -1 none */

    /* Systemcore (2.x) */
    bool have_sc;
    double sc_cpu, sc_temp, sc_ram, sc_storage;
    int sc_emmc;

    /* tuning */
    bool have_manifest;
    int ntunables;
    cat_tunable_t tunables[CAT_MAX_TUNABLES];

    /* auto chooser */
    bool have_autos;
    char auto_base[40];
    int nautos;
    char autos[CAT_MAX_AUTOS][40];
    char auto_selected[40], auto_active[40];

    /* the robot's own checks */
    bool have_preflight;
    bool preflight_ready;
    char preflight_summary[80];
    int nfindings;
    cat_finding_t findings[CAT_MAX_FINDINGS];
    bool have_systemcheck;
    bool systemcheck_ready;
    char systemcheck_report[96];
    bool have_wheel_radius;
    char wheel_radius_status[24];
    double wheel_radius_in, wheel_radius_change;
    bool have_start_check;
    bool start_ready;
    double start_dist_m, start_heading_deg;
    char build_commit[16];
    bool build_dirty;
} cat_robot_t;

void cat_model_init(nt4_client_t *nt);
nt4_client_t *cat_nt(void);
/* Refreshes `r` from the topic table. Cheap enough at 10 Hz. */
void cat_model_update(cat_robot_t *r);

/* The address list Console tries for a team (main.rs:46-73), plus an explicit override first. */
int cat_addresses(int team, const char *override_addr, char out[][64], int max);

/* Writes, exactly the ones Console makes. Returns false if refused (not declared / protected). */
bool cat_set_tunable(const cat_robot_t *r, int index, double value);
bool cat_select_auto(const cat_robot_t *r, const char *name);
void cat_blink_limelight(const char *name, bool on);

/* Battery bands (Console app.js:4489): 2 charged ≥ 12.5, 1 swap before a match ≥ 12.2, 0 low. */
int cat_battery_band(double v);
const char *cat_mode_name(const cat_robot_t *r);
