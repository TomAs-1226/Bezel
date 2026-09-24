#include "bz_motion.h"
#include <math.h>

#define PI_F 3.14159265358979f

/* Detent's three spellings of a spring, all reduced to k, c, m (spring.js:43-84). */
#define DUR_BOUNCE(d, b) { (2 * PI_F / (d)) * (2 * PI_F / (d)), 4 * PI_F * (1 - (b)) / (d), 1 }
#define RESPONSE(r, f) { (2 * PI_F / (r)) * (2 * PI_F / (r)), 4 * PI_F * (f) / (r), 1 }

/* hold — the press answer: response 0.15, dampingFraction 0.86 */
const bz_spring_t BZ_HOLD = RESPONSE(0.15f, 0.86f);
/* release — a finger lets go and hands off its velocity: duration 0.5, bounce 0.15 */
const bz_spring_t BZ_RELEASE = DUR_BOUNCE(0.5f, 0.15f);
/* smooth — no overshoot, for chrome tucking away: duration 0.5, bounce 0 */
const bz_spring_t BZ_SMOOTH = DUR_BOUNCE(0.5f, 0.0f);
/* settle — land somewhere and stay: response 0.4, dampingFraction 1 */
const bz_spring_t BZ_SETTLE = RESPONSE(0.4f, 1.0f);
/* detent — click into a notch: response 0.4, dampingFraction 0.8 */
const bz_spring_t BZ_DETENT = RESPONSE(0.4f, 0.8f);
/* wobble — the droplet melting back: duration 0.5, bounce 0.3 */
const bz_spring_t BZ_WOBBLE = DUR_BOUNCE(0.5f, 0.3f);
/* effect — colour and strength changes: stiffness 1600, critically damped */
const bz_spring_t BZ_EFFECT = { 1600.0f, 80.0f, 1 };
/* layer — state layers: stiffness 3800, critically damped */
const bz_spring_t BZ_LAYER = { 3800.0f, 123.2883f, 1 };
/* tick — a beat that rings a little: stiffness 800, damping ratio 0.6 */
const bz_spring_t BZ_TICK = { 800.0f, 33.9411f, 1 };
/* page — one page at a time: duration 0.45, bounce 0.12 */
const bz_spring_t BZ_PAGE = DUR_BOUNCE(0.45f, 0.12f);
/* edge — the spring back past a scroll bound: mass 0.5, k 100, ratio 1.1 */
const bz_spring_t BZ_EDGE = { 100.0f, 15.5563f, 0.5f };
/* light — the glass light direction leaning: response 0.4, dampingFraction 1 */
const bz_spring_t BZ_LIGHT = RESPONSE(0.4f, 1.0f);

static bool g_calm;
static double g_now;

void bz_motion_set_calm(bool calm) { g_calm = calm; }
static bool g_instant;
void bz_motion_set_instant(bool instant) { g_instant = instant; }
bool bz_motion_instant(void) { return g_instant; }
bool bz_motion_calm(void) { return g_calm; }
void bz_motion_clock(double now_s) { g_now = now_s; }
double bz_motion_now(void) { return g_now; }

bz_spring_t bz_calmed(bz_spring_t s)
{
    if (!g_calm) return s;
    float crit = 2 * sqrtf(s.k * s.m);
    if (s.c < crit) s.c = crit;
    return s;
}

/* Closed-form damped harmonic oscillator (spring.js:260-294). x0 is the offset from the target. */
static void solve(const bz_spring_t *s, float x0, float v0, float t, float *x, float *v)
{
    float w0 = sqrtf(s->k / s->m);
    float zeta = s->c / (2 * sqrtf(s->k * s->m));
    if (fabsf(zeta - 1) < 1e-4f) {
        float e = expf(-w0 * t);
        float b = v0 + w0 * x0;
        *x = e * (x0 + b * t);
        *v = e * (v0 - w0 * b * t);
    } else if (zeta < 1) {
        float wd = w0 * sqrtf(1 - zeta * zeta);
        float B = (v0 + zeta * w0 * x0) / wd;
        float e = expf(-zeta * w0 * t), cs = cosf(wd * t), sn = sinf(wd * t);
        *x = e * (x0 * cs + B * sn);
        *v = e * ((B * wd - zeta * w0 * x0) * cs - (x0 * wd + zeta * w0 * B) * sn);
    } else {
        float root = w0 * sqrtf(zeta * zeta - 1);
        float r1 = -zeta * w0 + root, r2 = -zeta * w0 - root;
        float c2 = (v0 - r1 * x0) / (r2 - r1), c1 = x0 - c2;
        float e1 = expf(r1 * t), e2 = expf(r2 * t);
        *x = c1 * e1 + c2 * e2;
        *v = c1 * r1 * e1 + c2 * r2 * e2;
    }
}

void bz_motion_init(bz_motion_t *m, float value, float eps)
{
    *m = (bz_motion_t){ .value = value, .target = value, .eps = eps > 0 ? eps : 0.001f, .spring = BZ_SMOOTH };
}

static void sample(bz_motion_t *m)
{
    float x, v;
    solve(&m->spring, m->x0, m->v0, (float)(g_now - m->t0), &x, &v);
    m->value = m->target + x;
    m->velocity = v;
}

void bz_motion_to_v(bz_motion_t *m, float target, bz_spring_t spring, float vel)
{
    if (g_instant && !m->keep) {
        bz_motion_set(m, target, 0);
        return;
    }
    if (m->running) sample(m);
    m->t0 = g_now;
    m->target = target;
    m->spring = bz_calmed(spring);
    m->x0 = m->value - target;
    m->v0 = vel;
    m->velocity = vel;
    m->running = true;
}

void bz_motion_to(bz_motion_t *m, float target, bz_spring_t spring)
{
    if (m->running) sample(m);
    bz_motion_to_v(m, target, spring, m->velocity);
}

void bz_motion_set(bz_motion_t *m, float value, float velocity)
{
    m->value = m->target = value;
    m->velocity = velocity;
    m->running = false;
}

bool bz_motion_tick(bz_motion_t *m)
{
    if (!m->running) return false;
    sample(m);
    float w0 = sqrtf(m->spring.k / m->spring.m);
    if (hypotf(m->value - m->target, m->velocity / w0) <= m->eps) {
        m->value = m->target;
        m->velocity = 0;
        m->running = false;
        return false;
    }
    return true;
}

float bz_project(float v, float rate)
{
    return (v / 1000.0f) * rate / (1.0f - rate);
}

float bz_rubber_band(float o, float dim, float c)
{
    float a = fabsf(o);
    float r = c * a * dim / (dim + c * a);
    return o < 0 ? -r : r;
}

float bz_rubber_clamp(float x, float lo, float hi, float dim)
{
    if (x < lo) return lo - bz_rubber_band(lo - x, dim, 0.55f);
    if (x > hi) return hi + bz_rubber_band(x - hi, dim, 0.55f);
    return x;
}

float bz_nearest(float x, const float *t, int n)
{
    float best = t[0];
    for (int i = 1; i < n; i++) if (fabsf(t[i] - x) < fabsf(best - x)) best = t[i];
    return best;
}

float bz_snap_target(float x, float v, const float *t, int n)
{
    return bz_nearest(x + bz_project(v, BZ_RATE_FAST), t, n);
}

void bz_velocity_reset(bz_velocity_t *v) { v->n = v->head = 0; }

void bz_velocity_add(bz_velocity_t *v, double t, float x)
{
    /* A gap longer than 40 ms breaks the window: the finger paused, old motion is not this motion. */
    if (v->n) {
        int last = (v->head + 19) % 20;
        if (t - v->t[last] > 0.040) v->n = 0;
    }
    v->t[v->head] = t;
    v->x[v->head] = x;
    v->head = (v->head + 1) % 20;
    if (v->n < 20) v->n++;
}

float bz_velocity_get(const bz_velocity_t *v, double now)
{
    if (v->n < 2) return 0;
    int last = (v->head + 19) % 20;
    double tn = v->t[last];
    if (now - tn > 0.040) return 0;
    /* Least-squares quadratic x = a + b·dt + c·dt² over the last 100 ms, dt relative to the newest
     * sample, so the slope at the newest sample is simply b. */
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, y0 = 0, y1 = 0, y2 = 0;
    int used = 0;
    for (int i = 0; i < v->n; i++) {
        int idx = (last - i + 20) % 20;
        double dt = v->t[idx] - tn;
        if (dt < -0.100) break;
        double y = v->x[idx];
        s0 += 1; s1 += dt; s2 += dt * dt; s3 += dt * dt * dt; s4 += dt * dt * dt * dt;
        y0 += y; y1 += y * dt; y2 += y * dt * dt;
        used++;
    }
    if (used < 2) return 0;
    if (used == 2) return (float)(s1 != 0 ? (y1 - y0 * s1 / s0) / (s2 - s1 * s1 / s0) : 0);
    /* Solve the 3x3 normal equations by Cramer's rule. */
    double det = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s3 * s2) + s2 * (s1 * s3 - s2 * s2);
    if (fabs(det) < 1e-18) return 0;
    double db = s0 * (y1 * s4 - s3 * y2) - y0 * (s1 * s4 - s3 * s2) + s2 * (s1 * y2 - y1 * s2);
    return (float)(db / det);
}
