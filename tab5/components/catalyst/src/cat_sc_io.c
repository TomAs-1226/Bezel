/* cat_sc — the worker half: the catalyst-agent poller (and motor history), the state recorder, and the
 * run recorder. Each runs on its own thread (core 0 on the tablet), reads NetworkTables through nt4's
 * locked getters, and hands the UI thread its results under a mutex. The pure parsing is cat_sc.c. */
#include "cat_sc.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
    bool inited;
    nt4_client_t *nt;
    cat_sc_env_t env;
    int64_t t0_us;
} G;

static double s_now(void) { return (double)nt4_now_us() / 1e6; }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static bool nt_connected(char *host, size_t n)
{
    if (!G.nt) return false;
    nt4_status_t st;
    nt4_status(G.nt, &st);
    if (host) snprintf(host, n, "%s", st.address);
    return st.state == NT4_CONNECTED && st.address[0];
}

static bool spawn(const char *name, void *(*fn)(void *), void *arg)
{
    if (G.env.spawn) return G.env.spawn(name, fn, arg, 12288);
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg) != 0) return false;
    pthread_detach(th);
    return true;
}

static void *states_thread(void *arg);

void cat_sc_init(nt4_client_t *nt, const cat_sc_env_t *env)
{
    if (G.inited) return;
    G.inited = true;
    G.nt = nt;
    if (env) G.env = *env;
    G.t0_us = nt4_now_us();
    spawn("cat-states", states_thread, NULL);
}

bool cat_agent_url(char *out, size_t n, const char *path)
{
    char host[64];
    if (!nt_connected(host, sizeof host)) return false;
    /* a host carrying a port carries the NT port, not the agent's (Console app.js agentUrl) */
    char *colon = strchr(host, ':');
    if (colon && !strchr(colon + 1, ':')) *colon = 0;
    snprintf(out, n, "http://%s:%d%s", host, CAT_AGENT_PORT, path);
    return true;
}

/* ================================================================== agent + motor history */

#define AG_BUF (256 * 1024)
#define MH_BUF (768 * 1024)

static struct {
    pthread_mutex_t lock;
    bool started;
    double want_until, mh_want_until;
    cat_agent_t *snap;
    cat_ag_status_t st;
    double last_ok;
    char host[64];
    /* motor history */
    cat_mh_motor_t *mh;
    cat_mh_meta_t mh_meta;
    int mh_n;
    uint32_t mh_rev;
    double mh_fetched;
    uint32_t mh_nt_seq;
    bool mh_from_agent;
} A ={ .lock = PTHREAD_MUTEX_INITIALIZER };

static void *agent_thread(void *arg);

static void agent_start(void)
{
    pthread_mutex_lock(&A.lock);
    bool start = !A.started && G.inited;
    if (start) {
        A.started = true;
        A.snap = calloc(1, sizeof *A.snap);
        A.mh = calloc(CAT_MH_MAX, sizeof *A.mh);
        A.st.age_s = NAN;
    }
    pthread_mutex_unlock(&A.lock);
    if (start && !spawn("cat-agent", agent_thread, NULL)) A.started = false;
}

void cat_agent_want(void)
{
    agent_start();
    pthread_mutex_lock(&A.lock);
    A.want_until = s_now() + 1.0;
    pthread_mutex_unlock(&A.lock);
}

void cat_mh_want(void)
{
    agent_start();
    pthread_mutex_lock(&A.lock);
    A.mh_want_until = s_now() + 1.0;
    pthread_mutex_unlock(&A.lock);
}

void cat_agent_status(cat_ag_status_t *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.st;
    out->age_s = A.last_ok > 0 ? s_now() - A.last_ok : NAN;
    pthread_mutex_unlock(&A.lock);
}

bool cat_agent_get(cat_agent_t *out, uint32_t *rev)
{
    bool got = false;
    pthread_mutex_lock(&A.lock);
    if (A.snap && A.st.rev != *rev) {
        *out = *A.snap;
        *rev = A.st.rev;
        got = true;
    }
    pthread_mutex_unlock(&A.lock);
    return got;
}

int cat_mh_get(cat_mh_motor_t *out, int max, cat_mh_meta_t *meta, uint32_t *rev)
{
    int n = -1;
    pthread_mutex_lock(&A.lock);
    if (A.mh && A.mh_rev != *rev) {
        n = A.mh_n < max ? A.mh_n : max;
        memcpy(out, A.mh, sizeof *out * (size_t)n);
        *meta = A.mh_meta;
        *rev = A.mh_rev;
    }
    pthread_mutex_unlock(&A.lock);
    return n;
}

static void poll_system(char *buf, cat_agent_t *tmp)
{
    char url[128];
    if (!cat_agent_url(url, sizeof url, "/api/system") || !G.env.http_get) return;
    double t = s_now();
    int len = 0;
    int status = G.env.http_get(url, 2500, buf, AG_BUF, &len);
    bool ok = status == 200 && cat_agent_parse(buf, (size_t)len, tmp);
    pthread_mutex_lock(&A.lock);
    if (ok) {
        *A.snap = *tmp;
        A.st.state = CAT_AG_OK;
        A.st.misses = 0;
        A.st.error[0] = 0;
        A.st.fetch_ms = (s_now() - t) * 1000;
        A.last_ok = s_now();
        A.st.rev++;
    } else {
        /* Silent, like Console: a robot without the package is the common case, not a fault. Three
         * misses rather than one, because a robot that just rebooted refuses connections for a while. */
        A.st.misses++;
        snprintf(A.st.error, sizeof A.st.error, status < 0 ? "no answer on port %d" : "HTTP %d", status < 0 ? CAT_AGENT_PORT : status);
        if (A.st.misses >= CAT_AGENT_GIVE_UP && A.st.state != CAT_AG_ABSENT) {
            A.st.state = CAT_AG_ABSENT;
            A.snap->valid = false;
            A.st.rev++;
        }
    }
    pthread_mutex_unlock(&A.lock);
}

static void publish_mh(cat_mh_motor_t *rows, int n, const cat_mh_meta_t *meta)
{
    pthread_mutex_lock(&A.lock);
    A.mh_n = n < CAT_MH_MAX ? n : CAT_MH_MAX;
    memcpy(A.mh, rows, sizeof *rows * (size_t)A.mh_n);
    A.mh_meta = *meta;
    A.mh_meta.n = A.mh_n;
    A.mh_rev++;
    pthread_mutex_unlock(&A.lock);
}

/* The agent's file: identities and recent boots as well as the totals. */
static bool fetch_mh_agent(cat_mh_motor_t *rows)
{
    char url[128];
    if (!cat_agent_url(url, sizeof url, "/api/motor-history") || !G.env.http_get) return false;
    char *buf = malloc(MH_BUF);
    if (!buf) return false;
    int len = 0;
    int status = G.env.http_get(url, 4000, buf, MH_BUF, &len);
    cat_mh_meta_t meta;
    int n = status > 0 ? cat_mh_parse_doc(buf, (size_t)len, rows, CAT_MH_MAX, &meta) : -1;
    free(buf);
    if (status != 200 || n < 0) return false;
    snprintf(meta.source, sizeof meta.source, "catalyst-agent");
    if (len >= MH_BUF - 1) snprintf(meta.error, sizeof meta.error, "the file is larger than the tablet reads; showing the first %d", n);
    if (G.nt) nt4_get_string(G.nt, "/Catalyst/MotorHistory/Summary", meta.summary, sizeof meta.summary);
    publish_mh(rows, n, &meta);
    pthread_mutex_lock(&A.lock);
    A.mh_from_agent = true;
    pthread_mutex_unlock(&A.lock);
    return true;
}

/* /Catalyst/MotorHistory/Rows: the totals only, re-read when the robot republishes. */
static void fetch_mh_nt(cat_mh_motor_t *rows)
{
    if (!G.nt) return;
    char type[24];
    uint32_t seq = 0;
    int64_t age;
    bool have = nt4_info(G.nt, "/Catalyst/MotorHistory/Rows", type, sizeof type, &seq, &age) && seq;
    if (have && seq == A.mh_nt_seq) return;
    A.mh_nt_seq = seq;
    cat_mh_meta_t meta;
    memset(&meta, 0, sizeof meta);
    meta.updated_ms = NAN;
    meta.clock_trusted = true;
    int n = 0;
    if (have) {
        enum { BUF = 64 * 1024, MAXR = 96 };
        char *buf = malloc(BUF);
        const char **items = malloc(sizeof(char *) * MAXR);
        int k = buf && items ? nt4_get_strings(G.nt, "/Catalyst/MotorHistory/Rows", buf, BUF, items, MAXR) : -1;
        for (int i = 0; i < k && n < CAT_MH_MAX; i++)
            if (cat_mh_parse_row(items[i], &rows[n])) n++;
        free(buf);
        free(items);
        snprintf(meta.source, sizeof meta.source, "networktables");
        bool trusted;
        if (nt4_get_bool(G.nt, "/Catalyst/MotorHistory/ClockTrusted", &trusted)) meta.clock_trusted = trusted;
        double u;
        if (nt4_get_number(G.nt, "/Catalyst/MotorHistory/UpdatedMs", &u)) meta.updated_ms = u;
        nt4_get_string(G.nt, "/Catalyst/MotorHistory/Summary", meta.summary, sizeof meta.summary);
    }
    publish_mh(rows, n, &meta);
}

static void *agent_thread(void *arg)
{
    (void)arg;
    char *buf = malloc(AG_BUF);
    cat_agent_t *tmp = malloc(sizeof *tmp);
    cat_mh_motor_t *rows = calloc(CAT_MH_MAX, sizeof *rows);
    if (!buf || !tmp || !rows) {
        free(buf); free(tmp); free(rows);
        pthread_mutex_lock(&A.lock);
        A.started = false;
        pthread_mutex_unlock(&A.lock);
        return NULL;
    }
    double last_poll = -1e9;
    for (;;) {
        double now = s_now();
        pthread_mutex_lock(&A.lock);
        bool want = now < A.want_until, mhw = now < A.mh_want_until;
        pthread_mutex_unlock(&A.lock);
        if (!want && !mhw) {
            sleep_ms(200);
            continue;
        }
        char host[64];
        bool up = nt_connected(host, sizeof host);
        pthread_mutex_lock(&A.lock);
        if (!up || strcmp(host, A.host)) {
            /* a different robot (or none): start over, including the three chances */
            snprintf(A.host, sizeof A.host, "%s", up ? host : "");
            snprintf(A.st.host, sizeof A.st.host, "%s", A.host);
            A.st.state = up ? CAT_AG_LOOKING : CAT_AG_IDLE;
            A.st.misses = 0;
            A.last_ok = 0;
            if (A.snap) A.snap->valid = false;
            A.st.rev++;
            A.mh_fetched = -1e9;
            A.mh_nt_seq = 0;
            A.mh_from_agent = false;
            A.mh_n = 0;
            memset(&A.mh_meta, 0, sizeof A.mh_meta);
            A.mh_rev++;
            last_poll = -1e9;
        }
        if (up && A.st.state == CAT_AG_IDLE) A.st.state = CAT_AG_LOOKING;
        cat_ag_state_t state = A.st.state;
        double mh_fetched = A.mh_fetched;
        pthread_mutex_unlock(&A.lock);
        if (!up) {
            sleep_ms(300);
            continue;
        }
        if (want && now - last_poll >= CAT_AGENT_POLL_S) {
            last_poll = now;
            poll_system(buf, tmp);
            pthread_mutex_lock(&A.lock);
            state = A.st.state;
            pthread_mutex_unlock(&A.lock);
        }
        if (mhw) {
            /* the file changes at most every 30 s on the robot (MotorHistory flushSeconds) */
            if (now - mh_fetched >= 30) {
                if (state != CAT_AG_OK) poll_system(buf, tmp); /* is it there at all? */
                pthread_mutex_lock(&A.lock);
                state = A.st.state;
                pthread_mutex_unlock(&A.lock);
                bool ok = state == CAT_AG_OK && fetch_mh_agent(rows);
                pthread_mutex_lock(&A.lock);
                A.mh_fetched = ok ? now : now - 20; /* without it, ask again in 10 s */
                pthread_mutex_unlock(&A.lock);
            }
            /* without the agent's file, the robot's own rows: the totals, no identities or boots */
            pthread_mutex_lock(&A.lock);
            bool have_file = A.mh_from_agent;
            pthread_mutex_unlock(&A.lock);
            if (!have_file) fetch_mh_nt(rows);
        }
        sleep_ms(100);
    }
    return NULL;
}

/* ================================================================== state recorder */

static struct {
    pthread_mutex_t lock;
    cat_st_lane_t *lanes;
    int n;
    uint32_t seq[CAT_ST_LANES];
    uint32_t rev;
} ST = { .lock = PTHREAD_MUTEX_INITIALIZER };

void cat_states_lock(void) { pthread_mutex_lock(&ST.lock); }
void cat_states_unlock(void) { pthread_mutex_unlock(&ST.lock); }
int cat_states_count(void) { return ST.n; }
const cat_st_lane_t *cat_states_lane(int i) { return i >= 0 && i < ST.n ? &ST.lanes[i] : NULL; }
uint32_t cat_states_now(void) { return G.inited ? (uint32_t)((nt4_now_us() - G.t0_us) / 1000) : 0; }
uint32_t cat_states_rev(void) { return ST.rev; }

/* What Autonomy says it is doing (AutonomyBoard's schema), one lane per decision. */
static const struct { const char *topic, *name; } AUTONOMY[] = {
    { "/Catalyst/Autonomy/Tasks/Running", "autonomy tasks" },
    { "/Catalyst/Autonomy/Chase/Target", "autonomy chase" },
    { "/Catalyst/Autonomy/Intent/Guess", "autonomy intent" },
    { "/Catalyst/Autonomy/Authority/Binding", "autonomy authority" },
};
#define MODE_TOPIC "(mode)"

typedef struct { char names[40][96]; int n; } found_t;

static void found_state(const char *name, const char *type, void *user)
{
    found_t *f = user;
    size_t l = strlen(name);
    if (f->n >= 40 || l < 7 || strcmp(name + l - 6, "/State") || strcmp(type, "string")) return;
    snprintf(f->names[f->n++], sizeof f->names[0], "%s", name);
}

static int lane_find(const char *topic)
{
    for (int i = 0; i < ST.n; i++)
        if (!strcmp(ST.lanes[i].topic, topic)) return i;
    return -1;
}

static void lane_add(const char *name, const char *topic, cat_lane_kind_t kind)
{
    if (ST.n >= CAT_ST_LANES || lane_find(topic) >= 0) return;
    pthread_mutex_lock(&ST.lock);
    cat_st_lane_init(&ST.lanes[ST.n], name, topic, kind);
    ST.seq[ST.n] = 0;
    ST.n++;
    ST.rev++;
    pthread_mutex_unlock(&ST.lock);
}

static void discover(void)
{
    static found_t f;
    f.n = 0;
    nt4_list(G.nt, "/Catalyst/", found_state, &f);
    for (int i = 0; i < f.n; i++) {
        const char *t = f.names[i];
        /* "/Catalyst/Vision/Health/limelight-ground/State" → "limelight-ground"; "/Catalyst/Arm/State" → "Arm" */
        char rel[96];
        snprintf(rel, sizeof rel, "%s", t + 10);
        rel[strlen(rel) - 6] = 0;
        const char *leaf = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
        /* a state machine (CatalystStateMachineLog) publishes its Phase beside its State */
        char phase[112], type[16];
        snprintf(phase, sizeof phase, "/Catalyst/%s/Phase", rel);
        uint32_t seq;
        int64_t age;
        bool machine = nt4_info(G.nt, phase, type, sizeof type, &seq, &age);
        cat_lane_kind_t kind = machine ? CAT_LANE_MACHINE : strchr(rel, '/') ? CAT_LANE_OTHER : CAT_LANE_MECH;
        lane_add(leaf, t, kind);
    }
    for (size_t i = 0; i < sizeof AUTONOMY / sizeof AUTONOMY[0]; i++) {
        char type[16];
        uint32_t seq;
        int64_t age;
        if (nt4_info(G.nt, AUTONOMY[i].topic, type, sizeof type, &seq, &age)) lane_add(AUTONOMY[i].name, AUTONOMY[i].topic, CAT_LANE_AUTONOMY);
    }
}

static const char *mode_now(void)
{
    double w;
    if (!nt4_get_number(G.nt, "/FMSInfo/ControlWord", &w) && !nt4_get_number(G.nt, "/FMSInfo/FMSControlData", &w))
        return NULL;
    int bits = (int)w;
    /* FMSControlData bits (nt4 maps 2027's ControlWord onto them): enabled 1, auto 2, test 4, e-stop 8 */
    if (bits & 8) return "e-stop";
    if (!(bits & 1)) return "disabled";
    return bits & 2 ? "auto" : bits & 4 ? "test" : "teleop";
}

static void *states_thread(void *arg)
{
    (void)arg;
    ST.lanes = calloc(CAT_ST_LANES, sizeof *ST.lanes);
    if (!ST.lanes) return NULL;
    lane_add("robot mode", MODE_TOPIC, CAT_LANE_MODE);
    double last_discover = -1e9;
    char buf[64];
    for (;;) {
        double now = s_now();
        bool up = nt_connected(NULL, 0);
        if (up && now - last_discover >= 1.0) {
            last_discover = now;
            discover();
        }
        uint32_t ms = cat_states_now();
        for (int i = 0; i < ST.n; i++) {
            cat_st_lane_t *l = &ST.lanes[i];
            const char *v = NULL;
            if (up) {
                if (!strcmp(l->topic, MODE_TOPIC)) {
                    v = mode_now();
                } else {
                    char type[24];
                    uint32_t seq = 0;
                    int64_t age;
                    if (!nt4_info(G.nt, l->topic, type, sizeof type, &seq, &age) || !seq) {
                        v = NULL;
                    } else if (seq == ST.seq[i]) {
                        continue; /* unchanged: nothing to read */
                    } else {
                        ST.seq[i] = seq;
                        v = nt4_get_string(G.nt, l->topic, buf, sizeof buf) ? buf : NULL;
                    }
                }
            } else {
                ST.seq[i] = 0; /* read it afresh on reconnect */
            }
            pthread_mutex_lock(&ST.lock);
            if (cat_st_feed(l, ms, v)) ST.rev++;
            pthread_mutex_unlock(&ST.lock);
        }
        sleep_ms(100);
    }
    return NULL;
}

/* ================================================================== run recorder */

static struct {
    pthread_mutex_t lock;
    bool stop;
    int mark_pending;
    cat_rec_col_t cols[CAT_REC_COLS];
    double latest[CAT_REC_COLS];
    bool have[CAT_REC_COLS];
    cat_rec_status_t st;
    FILE *f;
} RC = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *rec_thread(void *arg)
{
    (void)arg;
    FILE *f = RC.f;
    static char line[CAT_REC_COLS * 24 + 64];
    double v[CAT_REC_COLS];
    bool have[CAT_REC_COLS];
    double arr[32];
    int ncols = RC.st.ncols;
    double t0 = s_now(), next = t0, last_flush = t0;
    for (;;) {
        pthread_mutex_lock(&RC.lock);
        bool stop = RC.stop;
        int mark = RC.mark_pending;
        RC.mark_pending = 0;
        pthread_mutex_unlock(&RC.lock);
        if (stop) break;
        bool up = nt_connected(NULL, 0);
        int narr = -1;
        const char *arr_topic = NULL;
        for (int i = 0; i < ncols; i++) {
            const cat_rec_col_t *c = &RC.cols[i];
            have[i] = false;
            v[i] = NAN;
            if (!up) continue;
            if (c->index < 0) {
                have[i] = nt4_get_number(G.nt, c->topic, &v[i]);
            } else {
                /* the elements of one array are adjacent columns: read the array once per row */
                if (!arr_topic || strcmp(arr_topic, c->topic)) {
                    arr_topic = c->topic;
                    narr = nt4_get_numbers(G.nt, c->topic, arr, 32);
                }
                if (c->index < narr) {
                    v[i] = arr[c->index];
                    have[i] = true;
                }
            }
        }
        double now = s_now();
        size_t len = cat_csv_row(line, sizeof line, now - t0, mark, v, have, ncols);
        bool ok = len && fwrite(line, 1, len, f) == len;
        if (now - last_flush >= 1.0) {
            last_flush = now;
            ok = ok && fflush(f) == 0;
        }
        pthread_mutex_lock(&RC.lock);
        RC.st.elapsed = now - t0;
        RC.st.rows++;
        RC.st.bytes += len;
        RC.st.seq++;
        memcpy(RC.latest, v, sizeof v[0] * (size_t)ncols);
        memcpy(RC.have, have, sizeof have[0] * (size_t)ncols);
        if (!ok) {
            snprintf(RC.st.error, sizeof RC.st.error, "the card stopped taking writes (%s)", strerror(errno));
            RC.stop = true;
        }
        pthread_mutex_unlock(&RC.lock);
        /* 50 Hz on the clock, not on the loop: a slow write is caught up, a long stall is not replayed */
        next += 1.0 / CAT_REC_HZ;
        double wait = next - s_now();
        if (wait < -0.25) next = s_now();
        else if (wait > 0) sleep_ms((int)(wait * 1000 + 0.5));
    }
    fclose(f);
    pthread_mutex_lock(&RC.lock);
    RC.f = NULL;
    RC.st.active = false;
    RC.st.seq++;
    pthread_mutex_unlock(&RC.lock);
    return NULL;
}

bool cat_rec_start(const cat_rec_col_t *cols, int ncols, char *err, size_t errn)
{
    pthread_mutex_lock(&RC.lock);
    bool busy = RC.st.active || RC.f;
    pthread_mutex_unlock(&RC.lock);
    if (busy) {
        snprintf(err, errn, "already recording");
        return false;
    }
    const char *root = G.env.sd_root ? G.env.sd_root() : NULL;
    if (!root) {
        snprintf(err, errn, "no microSD card");
        return false;
    }
    if (ncols <= 0 || !G.nt) {
        snprintf(err, errn, "nothing to record");
        return false;
    }
    if (ncols > CAT_REC_COLS) ncols = CAT_REC_COLS;
    char dir[128], path[160];
    snprintf(dir, sizeof dir, "%s/runs", root);
    mkdir(dir, 0755);
    struct tm tm;
    if (G.env.wall && G.env.wall(&tm))
        snprintf(path, sizeof path, "%s/run-%04d%02d%02d-%02d%02d%02d.csv", dir, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    else snprintf(path, sizeof path, "%s/run-%010u.csv", dir, (unsigned)cat_states_now());
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errn, "couldn't create %s", path + strlen(root) + 1);
        return false;
    }
    /* a 16 KB buffer: the card sees one write every ~80 rows rather than one per row */
    setvbuf(f, NULL, _IOFBF, 16384);
    static char head[CAT_REC_COLS * 110 + 16];
    size_t hl = cat_csv_header(head, sizeof head, cols, ncols);
    if (!hl || fwrite(head, 1, hl, f) != hl) {
        fclose(f);
        snprintf(err, errn, "couldn't write to the card");
        return false;
    }
    pthread_mutex_lock(&RC.lock);
    memcpy(RC.cols, cols, sizeof *cols * (size_t)ncols);
    memset(&RC.st, 0, sizeof RC.st);
    RC.st.active = true;
    RC.st.ncols = ncols;
    RC.st.bytes = hl;
    snprintf(RC.st.path, sizeof RC.st.path, "%s", path);
    RC.stop = false;
    RC.mark_pending = 0;
    RC.f = f;
    pthread_mutex_unlock(&RC.lock);
    if (!spawn("cat-rec", rec_thread, NULL)) {
        fclose(f);
        pthread_mutex_lock(&RC.lock);
        RC.f = NULL;
        RC.st.active = false;
        pthread_mutex_unlock(&RC.lock);
        snprintf(err, errn, "couldn't start the recorder");
        return false;
    }
    return true;
}

void cat_rec_stop(void)
{
    pthread_mutex_lock(&RC.lock);
    RC.stop = true;
    pthread_mutex_unlock(&RC.lock);
}

void cat_rec_mark(void)
{
    pthread_mutex_lock(&RC.lock);
    if (RC.st.active) RC.mark_pending = (int)++RC.st.marks;
    pthread_mutex_unlock(&RC.lock);
}

void cat_rec_status(cat_rec_status_t *out)
{
    pthread_mutex_lock(&RC.lock);
    *out = RC.st;
    pthread_mutex_unlock(&RC.lock);
}

int cat_rec_latest(double *v, bool *have, int max)
{
    pthread_mutex_lock(&RC.lock);
    int n = RC.st.ncols < max ? RC.st.ncols : max;
    memcpy(v, RC.latest, sizeof *v * (size_t)n);
    memcpy(have, RC.have, sizeof *have * (size_t)n);
    pthread_mutex_unlock(&RC.lock);
    return n;
}

static int run_cmp(const void *a, const void *b) { return strcmp(((const cat_run_t *)b)->name, ((const cat_run_t *)a)->name); }

int cat_runs_list(cat_run_t *out, int max)
{
    const char *root = G.env.sd_root ? G.env.sd_root() : NULL;
    if (!root) return -1;
    char dir[128];
    snprintf(dir, sizeof dir, "%s/runs", root);
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".csv") || e->d_name[0] == '.') continue;
        cat_run_t *r = &out[n];
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        snprintf(r->path, sizeof r->path, "%s/%s", dir, e->d_name);
        struct stat st;
        r->bytes = stat(r->path, &st) == 0 ? (long)st.st_size : 0;
        r->duration_s = NAN;
        r->channels = 0;
        FILE *f = fopen(r->path, "r");
        if (f) {
            /* the header is a line of column names: long, but one line */
            static char head[CAT_REC_COLS * 110 + 16];
            if (fgets(head, sizeof head, f)) r->channels = cat_csv_count_cols(head) - 2;
            char buf[512];
            /* the elapsed time is the last complete row's first field */
            long from = r->bytes > (long)sizeof buf - 1 ? r->bytes - (long)(sizeof buf - 1) : 0;
            if (fseek(f, from, SEEK_SET) == 0) {
                size_t k = fread(buf, 1, sizeof buf - 1, f);
                double t;
                if (cat_csv_last_t(buf, k, &t)) r->duration_s = t;
            }
            fclose(f);
        }
        n++;
    }
    closedir(dp);
    qsort(out, (size_t)n, sizeof *out, run_cmp);
    return n;
}
