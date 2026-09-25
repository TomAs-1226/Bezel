/* analyze — a log's digest to OpenAI with the pit engineer's brief, on the assistant's worker (analyze.h).
 *
 * The state is a handful of pointers under one lock (this component's statics are internal RAM: nothing big
 * lives here); the digest and the answer are heap, which is PSRAM on the tablet. */
#include "analyze.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "as_json.h"
#include "as_oai.h"
#include "assist.h"
#include "cat_logs.h"
#include "hal.h"

#define DIGEST_MAX 14000        /* ~4k tokens: the whole of a kept event list and every series */
#define BODY_MAX (256 * 1024)
#define TIMEOUT_MS 120000

/* The custom agent: who it is, what Catalyst is and logs (from the library's docs and
 * docs/catalyst-integration.md), how to reason, and the answer's shape for a small screen. */
static const char PROMPT[] =
    "You are the pit engineer of an FRC team whose robots run on FrcCatalyst (\"Catalyst\"). You read one robot or "
    "Driver Station log at a time, already boiled down on the team's Catalyst Tab (a handheld pit tablet), and tell "
    "the technician at the cart what went wrong, how sure you are, and what to check before the next match. You know "
    "the roboRIO and Systemcore, CTRE Phoenix 6 and REV hardware, the Driver Station and the field network, "
    "batteries, breakers, CAN wiring and swerve drives, and you know Catalyst.\n"
    "\n"
    "What you are given\n"
    "- A digest, not the raw file: headline numbers, each series cut into slices (battery the lowest of each slice, "
    "the rest the highest), counters, and the events the tablet kept (when there are many, errors and warnings are "
    "kept before info). Times are seconds from the log's start. Anything absent was not measured or not kept: never "
    "read an absence as a zero or as \"fine\".\n"
    "- The kinds: .dslog (NI Driver Station at 50 Hz: trip time, packet loss, battery, CPU, CAN, the brownout bit); "
    ".dsevents (the Driver Station's messages); .wpilog (WPILib DataLog: the 2027 Driver Station's DS:/Dscomm/Status "
    "entries, or a robot log that recorded NetworkTables); and a Catalyst Tab run recording (.csv of chosen "
    "NetworkTables topics sampled at 50 Hz, with marks the technician set at moments that mattered).\n"
    "\n"
    "Catalyst in brief\n"
    "- An FRC robot library. 1.x runs on the roboRIO with WPILib 2026; 2.x on Systemcore with WPILib 2027 and "
    "Commands v3, CTRE Phoenix 6 first, Limelight for vision.\n"
    "- Mechanisms (Linear, Rotational, Flywheel, Roller, Winch, Claw, Turret, DifferentialWrist, Pneumatic, Servo) "
    "publish under /Catalyst/<Name>/: State, position or angle, velocity, setpoint, CurrentAmps, StatorCurrentAmps, "
    "TemperatureC, AtSetpoint or AtSpeed. State machines add Phase, Transitioning, Counters (Transitions, "
    "Rejections, Timeouts, Aborts), Faulted and FaultReason.\n"
    "- Health checks run on every mechanism, /Catalyst/Health/<subsystem>/<check>/firing: OverCurrent (warning, over "
    "90 % of the configured current limit for 0.5 s), HighTemp (warning), OverTemp (error: Catalyst stops that motor "
    "itself), Stall and NotZeroed (linear and rotational), NotSpinningUp (flywheel), LowPressure (pneumatics). "
    "RobotSafety can trip on several sustained errors; it is advisory and disables nothing.\n"
    "- Alerts: /Catalyst/Alerts/Errors and /Warnings, strings like \"[Elevator] not zeroed\".\n"
    "- Power, opt-in: /Catalyst/Brownout/{MeasuredVoltage,TotalCurrent,PredictedVoltage,AtRisk}; on Systemcore "
    "/Catalyst/Systemcore/{BatteryVolts,BrownedOut,CpuPercent,TempCelsius,CanUtilization,CanDown}. A robot with "
    "neither logs no battery voltage at all. Brownout floors: roboRIO 6.8 V, Systemcore 6.75 V; below them the "
    "controller cuts motor output.\n"
    "- CAN: /Catalyst/CAN/Health/<bus>/{OK,Utilization,BusOffCount,TxFullCount,REC,TEC}. Systemcore has five buses, "
    "can_s0..can_s4, on three controllers: s0+s1 and s3+s4 share one each, so a pair can saturate while each bus "
    "alone looks fine.\n"
    "- Loop: /Catalyst/Loop/Robot/{LastMs,AverageMs,MaxMs,OverBudget} against a 20 ms budget (opt-in). "
    "CatalystLog.enableLoggingInputs(false) is Catalyst's sanctioned way to cut logging load.\n"
    "- Tunables: /Catalyst/Tuning/<Mechanism>/{kP,kI,kD,kS,kV,kA,kG,MM/CruiseVelocity,MM/Acceleration,MM/Jerk}, "
    "live-editable unless the team turned tuning off for competition.\n"
    "- Swerve: CTRE swerve under Catalyst, /Catalyst/Swerve/{ModuleStates,ModuleTargets,Pose}. A module whose speed "
    "doesn't follow its target is hardware (a jammed drive, a loose wheel or encoder) before it is tuning.\n"
    "- Logging goes through CatalystLog to NetworkTables or a .wpilog; an unmeasured value is absent, never 0.\n"
    "\n"
    "How to reason\n"
    "- Tie every claim to a number or an event in the digest, with its time. Rank the causes by how well the "
    "evidence fits and give each a confidence (high, medium, low).\n"
    "- The usual chains: a battery sagging under ~7 V with brownouts points at the battery (charge, age, internal "
    "resistance), the main breaker, loose battery lugs or the Anderson connector, or current limits set too high "
    "(drive stator and supply limits, several motors stalling at once). Trip-time spikes and packet loss point at "
    "the radio, its power and cabling, or the field network, not the robot code. CAN faults and high utilisation "
    "point at wiring (a loose or damaged joint, missing termination), a device dropping off, or too many status "
    "frames. Loop overruns point at the code: blocking calls, heavy logging, vision or path work on the main loop. "
    "OverCurrent and Stall suggest a mechanism fighting a hard stop or a jam; HighTemp and OverTemp a motor worked "
    "too hard or a binding mechanism; NotZeroed a missed homing.\n"
    "- Use the mode changes (auto, teleop, disabled) to place problems in the match.\n"
    "- When the digest doesn't support a conclusion, say so plainly and name what would settle it (a log kind, "
    "a run recording preset such as power or drive, a topic to add). Never invent topics, values or events.\n"
    "\n"
    "Answer format: plain text for a small screen read minutes before a match. No markdown emphasis, no # "
    "headings, no tables. Exactly these lowercase sections, short lines, under about 300 words in all:\n"
    "verdict: one sentence.\n"
    "likely causes:\n"
    "1. the cause (confidence) - the evidence, with times\n"
    "next checks:\n"
    "- concrete physical or software checks, in the order to do them\n"
    "look at:\n"
    "- the tunables by their Catalyst path, current limits, wiring runs or devices involved\n"
    "can't tell from this log:\n"
    "- what is missing, and what to record next time\n"
    "If the log looks healthy, say that in the verdict and keep the rest short.";

/* The battery fleet's brief: the batteries app's ask gpt (analyze_fleet_start). */
static const char FLEET_PROMPT[] =
    "You are the battery lead of an FRC team, in the pit. The team's Catalyst Tab keeps its fleet of 12 V SLA robot "
    "batteries (typically 18 Ah, MK ES17-12 or similar): which battery went in for each match, how it was charged, "
    "and what the logs measured each time: the resting voltage before the match, the lowest voltage, brownouts, the "
    "energy drawn, and the internal resistance fitted from voltage sag against the robot's total current (V = V0 - "
    "I*R, the same model as Catalyst's BatteryResistanceIdentifier). You are given that summary and the tablet's own "
    "ranking, and you advise the technician.\n"
    "\n"
    "What you know\n"
    "- A healthy FRC battery measures about 11-20 mOhm under a robot's load; over ~25 mOhm it sags hard and browns "
    "out; a rise of 30 % or more over its own baseline means it is aging or sulfating, or has a bad lug, Anderson "
    "connector or cell. A single high reading can be a loose connector or a cold battery rather than the battery.\n"
    "- Resting voltage after charging and resting is ~12.8-13.0 V; under ~12.3 V at rest it wasn't charged, or can't "
    "hold charge. A battery straight off the charger reads high (surface charge) and runs warm; 30+ minutes of rest "
    "is better. A battery just used must be recharged before it goes in again.\n"
    "- Rotate so none is used much more than the others; a battery drawn deeply several times in a day heats and "
    "ages faster. Brownouts with a low-resistance battery point at the robot (current limits, a stall), not the "
    "battery.\n"
    "- Absent numbers were not measured: never read them as zero or as fine.\n"
    "\n"
    "Answer format: plain text for a small screen, no markdown emphasis, no # headings, no tables, under about 220 "
    "words, exactly these lowercase sections:\n"
    "verdict: one sentence: the battery for the next match and why.\n"
    "next matches:\n"
    "- the order to use them in, with the reason for each\n"
    "watch:\n"
    "- batteries to test, retire or check the lugs of, with the numbers that say so\n"
    "charging:\n"
    "- what to put on the charger now\n"
    "can't tell:\n"
    "- what is missing (a battery never measured, logs without current), and what to record";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    an_phase_t phase;
    bool busy;
    int team;
    bool fleet;                 /* the battery fleet's summary (G.text), not a log */
    char *path, *name, *answer, *digest, *text;
    char model[48];
} G;
static unsigned g_gen;

static void bump(void) { __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELAXED); }
unsigned analyze_gen(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }

static void set_phase(an_phase_t p)
{
    pthread_mutex_lock(&g_lock);
    G.phase = p;
    pthread_mutex_unlock(&g_lock);
    bump();
}

/* the end of the job: the answer (or why not), taken over */
static void finish(an_phase_t p, char *text, char *digest)
{
    pthread_mutex_lock(&g_lock);
    free(G.answer);
    free(G.digest);
    G.answer = text;
    G.digest = digest;
    G.phase = p;
    G.busy = false;
    pthread_mutex_unlock(&g_lock);
    bump();
}

static void fail(const char *why, char *digest) { finish(AN_FAILED, as_strdup(why), digest); }

static bool ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

/* The digest: a run recording's per column, a log's from its parse. NULL with the reason in why. */
static char *make_digest(const char *path, const char *name, char *why, size_t wn)
{
    char *d = malloc(DIGEST_MAX);
    if (!d) {
        snprintf(why, wn, "out of memory");
        return NULL;
    }
    if (ends(path, ".csv")) {
        if (!cat_csv_digest(path, d, DIGEST_MAX)) {
            snprintf(why, wn, "couldn't read the run: %s", d);
            free(d);
            return NULL;
        }
        return d;
    }
    cat_log_t *l = malloc(sizeof *l); /* ~12 KB: PSRAM */
    if (!l || !cat_log_read(path, l)) {
        snprintf(why, wn, "couldn't read the log: %s", l ? l->error : "out of memory");
        free(l);
        free(d);
        return NULL;
    }
    cat_log_digest(l, name, d, DIGEST_MAX);
    free(l);
    return d;
}

/* The request's body: the brief as the system message, the digest as the user's. */
static char *request(const char *model, int team, const char *digest, bool fleet)
{
    ab_t b;
    ab_init(&b);
    ab_puts(&b, "{\"model\":");
    ab_str(&b, model);
    ab_puts(&b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    ab_str(&b, fleet ? FLEET_PROMPT : PROMPT);
    ab_puts(&b, "},{\"role\":\"user\",\"content\":");
    ab_t u;
    ab_init(&u);
    if (team > 0) ab_fmt(&u, "Team %d. ", team);
    ab_puts(&u, fleet ? "Which battery goes in next, and what needs attention? The fleet follows.\n\n"
                      : "What does this log say about the robot? The digest follows.\n\n");
    ab_puts(&u, digest);
    ab_strn(&b, u.p ? u.p : "", u.n);
    ab_free(&u);
    /* no token cap: a reasoning model spends part of it thinking, and a small one comes back empty */
    ab_puts(&b, "}]}");
    if (b.oom) {
        ab_free(&b);
        return NULL;
    }
    return ab_take(&b);
}

/* the whole body of an answer, NUL-terminated (heap), or NULL */
static char *read_all(hal_http_t *h, size_t *len)
{
    size_t cap = 8192, n = 0;
    char *b = malloc(cap);
    while (b) {
        if (n + 2048 + 1 > cap) {
            if (cap >= BODY_MAX) break;
            char *nb = realloc(b, cap * 2);
            if (!nb) break;
            b = nb;
            cap *= 2;
        }
        int r = hal_http_read(h, b + n, (int)(cap - n - 1));
        if (r <= 0) break;
        n += (size_t)r;
    }
    if (b) b[n] = 0;
    *len = n;
    return b;
}

static void job(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    char *path = G.path, *name = as_strdup(G.name ? G.name : ""), *text = G.text;
    int team = G.team;
    bool fleet = G.fleet;
    G.path = G.text = NULL;
    pthread_mutex_unlock(&g_lock);
    char why[160] = "out of memory";
    char *digest = fleet ? text : path && name ? make_digest(path, name, why, sizeof why) : NULL;
    if (!fleet) free(text);
    free(path);
    free(name);
    if (!digest) {
        fail(why, NULL);
        return;
    }
    set_phase(AN_ASKING);

    char url[192], hdr[400], model[48];
    if (!assist_oai_endpoint(url, sizeof url, hdr, sizeof hdr, model, sizeof model)) {
        memset(hdr, 0, sizeof hdr);
        fail("no OpenAI key: add one in settings (assistant), or OPENAI_API_KEY in CATOS/KEYS.ENV on the card", digest);
        return;
    }
    pthread_mutex_lock(&g_lock);
    snprintf(G.model, sizeof G.model, "%s", model);
    pthread_mutex_unlock(&g_lock);
    char *body = request(model, team, digest, fleet);
    if (!body) {
        memset(hdr, 0, sizeof hdr);
        fail("out of memory", digest);
        return;
    }
    hal_http_req_t rq = { .method = "POST", .url = url, .headers = hdr, .body = body, .body_len = strlen(body),
                          .timeout_ms = TIMEOUT_MS };
    int status = -1;
    char err[96] = "";
    hal_http_t *h = hal_http_open(&rq, &status, err, sizeof err);
    memset(hdr, 0, sizeof hdr); /* the key */
    free(body);
    if (!h) {
        snprintf(why, sizeof why, "couldn't reach OpenAI (%s): is the Wi-Fi on the internet?", err);
        fail(why, digest);
        return;
    }
    size_t len = 0;
    char *resp = read_all(h, &len);
    hal_http_close(h);
    if (!resp) {
        fail("out of memory", digest);
        return;
    }
    if (status != 200) {
        char em[240], code[48];
        as_oai_error(resp, em, sizeof em, code, sizeof code);
        /* never the body on a 401: OpenAI's echoes part of the key */
        if (status == 401) snprintf(why, sizeof why, "OpenAI refused the key (401): check the OpenAI key in settings");
        else if (status == 429 && !strcmp(code, "insufficient_quota"))
            snprintf(why, sizeof why, "the OpenAI account is out of credit (429)");
        else if (status == 404 || !strcmp(code, "model_not_found"))
            snprintf(why, sizeof why, "OpenAI doesn't offer %s to this key: check the model in settings", model);
        else snprintf(why, sizeof why, "OpenAI HTTP %d: %.100s", status, em);
        free(resp);
        fail(why, digest);
        return;
    }
    aj_t *d = aj_parse(resp, len, NULL, 0);
    free(resp);
    const aj_t *msg = aj_get(aj_at(aj_get(d, "choices"), 0), "message");
    const char *content = aj_gets(msg, "content"), *refusal = aj_gets(msg, "refusal");
    const char *fin = aj_gets(aj_at(aj_get(d, "choices"), 0), "finish_reason");
    char *answer = NULL;
    if (content && content[0]) {
        ab_t a;
        ab_init(&a);
        ab_puts(&a, content);
        if (fin && !strcmp(fin, "length")) ab_puts(&a, "\n\n(cut off: the model ran out of room)");
        answer = ab_take(&a);
    }
    char *model_used = as_strdup(aj_gets(d, "model") ? aj_gets(d, "model") : model);
    if (!answer) snprintf(why, sizeof why, "%s", refusal ? "the model declined to answer" : "the model's answer was empty");
    aj_free(d);
    if (model_used) {
        pthread_mutex_lock(&g_lock);
        snprintf(G.model, sizeof G.model, "%s", model_used);
        pthread_mutex_unlock(&g_lock);
        free(model_used);
    }
    if (answer) finish(AN_DONE, answer, digest);
    else fail(why, digest);
}

/* a log (p) or the fleet's summary (text): taken over either way */
static bool start(char *p, char *n, char *text, int team)
{
    pthread_mutex_lock(&g_lock);
    bool ok = !G.busy && n && (p || text);
    if (ok) {
        free(G.path);
        free(G.name);
        free(G.answer);
        free(G.digest);
        free(G.text);
        G.path = p;
        G.name = n;
        G.text = text;
        G.fleet = text != NULL;
        G.answer = G.digest = NULL;
        G.model[0] = 0;
        G.team = team;
        G.phase = AN_READING;
        G.busy = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        free(p);
        free(n);
        free(text);
        return false;
    }
    if (!assist_post_job(job, NULL)) {
        pthread_mutex_lock(&g_lock);
        free(G.path);
        free(G.text);
        G.path = G.text = NULL;
        G.busy = false;
        G.phase = AN_FAILED;
        G.answer = as_strdup("the assistant is busy with another job: try again in a moment");
        pthread_mutex_unlock(&g_lock);
        bump();
        return false;
    }
    bump();
    return true;
}

bool analyze_start(const char *path, int team)
{
    if (!path || !path[0]) return false;
    const char *base = strrchr(path, '/');
    return start(as_strdup(path), as_strdup(base ? base + 1 : path), NULL, team);
}

bool analyze_fleet_start(const char *summary, int team)
{
    if (!summary || !summary[0]) return false;
    return start(NULL, as_strdup(ANALYZE_FLEET_NAME), as_strdup(summary), team);
}

bool analyze_busy(void)
{
    pthread_mutex_lock(&g_lock);
    bool b = G.busy;
    pthread_mutex_unlock(&g_lock);
    return b;
}

an_phase_t analyze_get(char **name, char **answer, char **digest, char *model, size_t mn)
{
    pthread_mutex_lock(&g_lock);
    an_phase_t p = G.phase;
    if (name) *name = G.name ? as_strdup(G.name) : NULL;
    if (answer) *answer = G.answer ? as_strdup(G.answer) : NULL;
    if (digest) *digest = G.digest ? as_strdup(G.digest) : NULL;
    if (model && mn) snprintf(model, mn, "%s", G.model);
    pthread_mutex_unlock(&g_lock);
    return p;
}
