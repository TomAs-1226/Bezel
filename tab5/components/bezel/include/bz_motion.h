/* bz_motion — Detent's springs and gesture math, in C.
 *
 * A port of Bezel's motion.js and Detent's spring.js / gesture.js / velocity.js. Springs are solved in
 * closed form at the exact frame time (never stepped), every retarget starts from the current value
 * and hands off the current velocity, and reduced motion critically damps each spring at its own
 * stiffness rather than swapping in a different curve. Role constants are motion.js's SPRINGS. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float k, c, m; /* stiffness, damping, mass */
} bz_spring_t;

/* The twelve roles, as declared in Bezel's motion.js (sources in the comments of bz_motion.c). */
extern const bz_spring_t BZ_HOLD, BZ_RELEASE, BZ_SMOOTH, BZ_SETTLE, BZ_DETENT, BZ_WOBBLE, BZ_EFFECT,
    BZ_LAYER, BZ_TICK, BZ_PAGE, BZ_EDGE, BZ_LIGHT;

/* Calm mode: every spring below critical damping is critically damped at the same stiffness. */
void bz_motion_set_calm(bool calm);
bool bz_motion_calm(void);
bz_spring_t bz_calmed(bz_spring_t s);

/* Seconds on the clock every motion reads; the UI loop advances it once per frame. */
void bz_motion_clock(double now_s);
double bz_motion_now(void);

typedef struct {
    float value, velocity, target;
    /* solve state */
    double t0;
    float x0, v0;
    bz_spring_t spring;
    float eps;          /* settle threshold, absolute: 0.001 default, 0.1 for pixels */
    bool running;
} bz_motion_t;

void bz_motion_init(bz_motion_t *m, float value, float eps);
/* Retarget from the current value, keeping the current velocity (or `vel` when has_vel). */
void bz_motion_to(bz_motion_t *m, float target, bz_spring_t spring);
void bz_motion_to_v(bz_motion_t *m, float target, bz_spring_t spring, float vel);
/* Hold under a finger: value is set, the velocity is remembered for the next hand-off. */
void bz_motion_set(bz_motion_t *m, float value, float velocity);
/* Samples at the motion clock. Returns true while still moving (the caller should keep drawing). */
bool bz_motion_tick(bz_motion_t *m);

/* Gesture math (Detent gesture.js). Rates: 0.998 normal, 0.99 fast. */
#define BZ_RATE_NORMAL 0.998f
#define BZ_RATE_FAST 0.99f
float bz_project(float velocity_px_s, float rate);
float bz_rubber_band(float overshoot, float dim, float c);
/* Clamps x into [lo, hi] with rubber banding past either edge (c = 0.55). */
float bz_rubber_clamp(float x, float lo, float hi, float dim);
float bz_nearest(float x, const float *targets, int n);
float bz_snap_target(float x, float v, const float *targets, int n);

/* Velocity from the last 100 ms of samples, least-squares quadratic (Detent velocity.js). */
typedef struct {
    double t[20];
    float x[20];
    int n, head;
} bz_velocity_t;
void bz_velocity_reset(bz_velocity_t *v);
void bz_velocity_add(bz_velocity_t *v, double t, float x);
float bz_velocity_get(const bz_velocity_t *v, double now);
