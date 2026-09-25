/* batteries — the team's battery fleet on the tablet (cat_batt.h): the roster and each battery's history, the
 * checklist's "which battery goes in", what the logs and the robot say about each use, and which one should go
 * in next.
 *
 * The data is <sd>/CATOS/DATA/batteries.json (kv "batteries", the roster without histories, when there is no
 * card), read at start-up and written from this thread after each change. The card has answered writes with EIO
 * for ~20 s after start-up, so a write that fails is tried again every SAVE_RETRY_S, SAVE_TRIES times.
 *
 * The logs on the card (the root and logs/, as the logs app lists them) are read on the assistant's worker
 * (assist_post_job), SCAN_PER_JOB a job, each once (a hash of its name, size and time is kept), and attributed to
 * the pick they belong to (cat_fleet_attribute). The robot's live battery voltage and total current are folded
 * into the current pick at 10 Hz while it is in and the robot runs. Nothing here has a thread of its own, and
 * nothing runs per frame: the screens are built again only when the fleet changes, and once a minute for the
 * "2 h ago"s. */
#include "ui_internal.h"
#include "ui_storage.h"
#include "analyze.h"
#include "assist.h"
#include "cat_batt.h"
#include "home.h"
#include "tba.h"

#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)
#define LW 800                        /* the roster */
#define RX (PAD + LW + BZ_GAP)
#define RW (W - PAD - RX)             /* the next match, a battery, or gpt */
#define FILE_NAME "batteries.json"
#define SAVE_RETRY_S 2.0
#define SAVE_TRIES 30
#define SCAN_PER_JOB 6
#define SCAN_MAX_BYTES (32L * 1024 * 1024)
#define SCAN_FIRST_S 40.0             /* after start-up: past the card's busy first seconds */
#define PICK_FIX_S 1200               /* a pick this recent with nothing measured yet is corrected by the next */
#define PICK_LIVE_S (4 * 3600)        /* the robot's live numbers belong to a pick this recent */
#define LIVE_END_S 15.0               /* disabled this long after running: the session is over */
#define KV_MAX 3900
#define KNOWN(x) ((x) == (x))

typedef struct {
    char name[48];
    uint32_t hash;
    bool ok;
    cat_batt_log_t log;
} scan_res_t;

typedef struct {
    int nseen;
    uint32_t seen[CAT_BATT_SEEN];
    int nres, more, found;
    scan_res_t res[SCAN_PER_JOB];
} scan_t;

static struct {
    bool booted;
    cat_fleet_t *f;
    cat_batt_rank_t *rank;
    int nrank;
    unsigned gen;                     /* moves with every change: the screens rebuild on it */
    bool dirty, save_failed, on_card;
    int save_tries;
    double next_save;
    /* the logs */
    scan_t *scan;
    int scan_phase;                   /* 0 idle, 1 with the worker, 2 done (the worker's last write) */
    bool scan_wanted, scanning;
    double next_scan;
    int logs_found, unmatched, read_total;
    /* the robot, live */
    struct {
        bool on, ran;
        int uid;
        int64_t use_t;
        double last_en;
        cat_batt_acc_t acc;
    } lv;
    /* gpt */
    unsigned an_seen;
} BM;

/* ------------------------------------------------------------------ words */

static void ago(int64_t s, char *out, size_t n)
{
    if (s < 0) s = 0;
    if (s < 90) snprintf(out, n, "just now");
    else if (s < 3600) snprintf(out, n, "%d min ago", (int)(s / 60));
    else if (s < 48 * 3600) snprintf(out, n, "%d h ago", (int)((s + 1800) / 3600));
    else snprintf(out, n, "%d d ago", (int)(s / 86400));
}

static void day_hhmm(int64_t t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    static const char *const DAY[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    snprintf(out, n, "%s %d:%02d", DAY[tm.tm_wday], tm.tm_hour, tm.tm_min);
}

static const char *bname(const cat_batt_t *b, char *out, size_t n) { return cat_batt_name(b, out, n); }

static bool charged_now(const cat_batt_t *b)
{
    const cat_batt_use_t *l = cat_batt_last(b);
    return b->charged && (!l || b->charged >= l->t);
}

/* "12m ago": a tile's caption holds 15 characters of the caption face */
static void ago_short(int64_t s, char *out, size_t n)
{
    if (s < 0) s = 0;
    if (s < 90) snprintf(out, n, "now");
    else if (s < 3600) snprintf(out, n, "%dm ago", (int)(s / 60));
    else if (s < 48 * 3600) snprintf(out, n, "%dh ago", (int)((s + 1800) / 3600));
    else snprintf(out, n, "%dd ago", (int)(s / 86400));
}

/* a battery's short state: "charged 2 h ago", "went in 40 min ago", "used 5 h ago"; `tile`: the tile's
 * 15-character form ("charged 2h ago", "charge unknown"), which the long one overran ("charge not r...") */
static void short_state(const cat_batt_t *b, int64_t now, char *out, size_t n, bool tile)
{
    char a[24];
    const cat_batt_use_t *l = cat_batt_last(b);
    if (b->status == CB_BAD) snprintf(out, n, "marked bad");
    else if (b->status == CB_RETIRED) snprintf(out, n, "retired");
    else if (charged_now(b)) {
        if (tile) ago_short(now - b->charged, a, sizeof a);
        else ago(now - b->charged, a, sizeof a);
        snprintf(out, n, "charged %s", a);
    } else if (l) {
        if (tile) ago_short(now - l->t, a, sizeof a);
        else ago(now - l->t, a, sizeof a);
        snprintf(out, n, "%s %s", now - l->t < 2 * 3600 ? "went in" : "used", a);
    } else snprintf(out, n, "%s", tile ? "charge unknown" : "charge not recorded");
}

static bz_status_t status_mark(int s) { return s == CB_GOOD ? BZ_OK : s == CB_WATCH ? BZ_WARN : s == CB_BAD ? BZ_FAULT : BZ_STALE; }

/* the TBA key and label of our next match ("2026casj_qm34", "Q34"); false when TBA doesn't say */
static bool next_match(char *key, size_t kn, char *label, size_t ln, time_t *when)
{
    tba_match_t m;
    int n = 0;
    bool live = false, off = false;
    char ev[20] = "";
    key[0] = label[0] = 0;
    if (when) *when = 0;
    if (tba_upcoming(&m, 1, &n, &live, &off, ev, sizeof ev) != TBA_READY || n < 1) return false;
    static const char *const LV[5] = { "qm", "ef", "qf", "sf", "f" };
    const char *lv = m.level >= 0 && m.level < 5 ? LV[m.level] : "qm";
    if (ev[0]) {
        if (m.level == 0) snprintf(key, kn, "%s_qm%d", ev, m.number);
        else snprintf(key, kn, "%s_%s%dm%d", ev, lv, m.set, m.number);
    }
    snprintf(label, ln, "%s", m.label);
    if (when) *when = m.predicted ? m.predicted : m.time;
    return true;
}

/* ------------------------------------------------------------------ the card */

static void bm_rank(void)
{
    if (!BM.f || !BM.rank) return;
    BM.nrank = cat_fleet_rank(BM.f, time(NULL), BM.rank, CAT_BATT_MAX);
}

static void bm_changed(void)
{
    BM.gen++;
    BM.dirty = true;
    BM.save_tries = 0;
    BM.next_save = 0;
    if (BM.f) BM.f->updated = time(NULL);
    bm_rank();
}

static bool save_card(void)
{
    char path[128];
    if (!cstore_path(CS_DATA, FILE_NAME, path, sizeof path)) return false;
    size_t len = 0;
    char *js = cat_batt_to_json(BM.f, &len);
    bool ok = js && cstore_write(path, js, len);
    free(js);
    return ok;
}

/* no card: the roster alone in kv (NVS: this thread) */
static bool save_kv(void)
{
    cat_fleet_t *c = malloc(sizeof *c);
    if (!c) return false;
    memcpy(c, BM.f, sizeof *c);
    c->nseen = 0;
    for (int i = 0; i < c->n; i++) c->b[i].nuse = 0;
    size_t len = 0;
    char *js = cat_batt_to_json(c, &len);
    free(c);
    bool ok = js && len < KV_MAX;
    if (ok) hal_kv_set("batteries", js);
    free(js);
    return ok;
}

static void save_tick(double now)
{
    if (!BM.dirty || now < BM.next_save || !BM.f) return;
    if (!hal_sd_root()) {
        if (!save_kv()) printf("batteries: the roster is too long for kv: not kept without a card\n");
        BM.dirty = false;
        BM.on_card = false;
        return;
    }
    if (save_card()) {
        BM.dirty = false;
        BM.on_card = true;
        BM.save_failed = false;
        return;
    }
    if (++BM.save_tries >= SAVE_TRIES) {
        BM.dirty = false;
        BM.save_failed = true;
        ui_island_say(BZ_I_SD_CARD, "couldn't write batteries.json to the card");
        return;
    }
    BM.next_save = now + SAVE_RETRY_S;
}

static void bm_load(void)
{
    BM.f = calloc(1, sizeof *BM.f);
    BM.rank = calloc(CAT_BATT_MAX, sizeof *BM.rank);
    if (!BM.f || !BM.rank) return;
    bool ok = false;
    char path[128];
    if (cstore_path(CS_DATA, FILE_NAME, path, sizeof path)) {
        size_t len = 0;
        char *js = cstore_read(path, 2 << 20, &len, NULL);
        if (js) {
            ok = cat_batt_from_json(BM.f, js, len);
            if (!ok) {
                /* kept aside rather than written over */
                char bak[128];
                if (cstore_path(CS_DATA, "batteries.bad", bak, sizeof bak)) cstore_write(bak, js, len);
                ui_island_say(BZ_I_WARNING, "batteries.json couldn't be read: kept as batteries.bad");
            }
            free(js);
        }
        BM.on_card = ok;
    }
    if (!ok) {
        char *kv = malloc(4000);
        if (kv && hal_kv_get("batteries", kv, 4000)) ok = cat_batt_from_json(BM.f, kv, strlen(kv));
        free(kv);
        if (ok && hal_sd_root()) BM.dirty = true; /* onto the card */
    }
    if (!ok) {
        cat_fleet_default(BM.f, 12);
        BM.dirty = true;
    }
    BM.gen++;
    bm_rank();
}

/* ------------------------------------------------------------------ the intelligence */

static void check_watch(void)
{
    for (int i = 0; i < BM.f->n; i++) {
        cat_batt_t *b = &BM.f->b[i];
        char why[64], nm[20], msg[96];
        if (b->status != CB_GOOD || !cat_batt_watch_due(b, why, sizeof why)) continue;
        b->status = CB_WATCH;
        b->auto_watch = true;
        snprintf(msg, sizeof msg, "battery %s on watch: %s", bname(b, nm, sizeof nm), why);
        ui_island_say(BZ_I_BATTERY_ALERT, msg);
    }
}

/* Battery bi goes in now: a use recorded with the next match and how charged it is. A pick moments ago for the
 * same match with nothing measured yet was a mistake, and goes. */
static void bm_pick(int bi, int charge)
{
    if (!BM.f || bi < 0 || bi >= BM.f->n) return;
    int64_t now = time(NULL);
    char key[24], label[12];
    next_match(key, sizeof key, label, sizeof label, NULL);
    int lbi = -1;
    cat_batt_use_t *lp = cat_fleet_last_pick(BM.f, &lbi);
    if (lp && lbi >= 0 && now - lp->t < PICK_FIX_S && !(lp->src & (CU_LOG | CU_LIVE)) && !strcmp(lp->match, key)) {
        cat_batt_t *lb = &BM.f->b[lbi];
        if (lp == &lb->use[lb->nuse - 1]) {
            lb->nuse--;
            if (lb->uses_total > 0) lb->uses_total--;
        }
    }
    cat_batt_t *b = &BM.f->b[bi];
    bool was_charged = charged_now(b);
    cat_batt_use_t *u = cat_batt_push_use(b);
    u->t = now;
    u->src = CU_PICK;
    u->charge = (uint8_t)charge;
    snprintf(u->match, sizeof u->match, "%s", key);
    snprintf(u->label, sizeof u->label, "%s", label);
    if (charge == CC_FRESH) b->charged = now;
    if (charge == CC_FRESH || charge == CC_RESTED || was_charged) u->charged = b->charged;
    char nm[20], msg[80];
    snprintf(msg, sizeof msg, "battery %s in%s%s", bname(b, nm, sizeof nm), label[0] ? " for " : "", label);
    ui_island_say(BZ_I_BATTERY_CHARGING_FULL, msg);
    bm_changed();
}

/* ------------------------------------------------------------------ the logs */

static bool log_file(const char *n)
{
    size_t l = strlen(n);
    return (l > 7 && !strcasecmp(n + l - 7, ".wpilog")) || (l > 6 && !strcasecmp(n + l - 6, ".dslog"));
}

/* The worker's job: the logs on the card not read yet, SCAN_PER_JOB of them. */
static void scan_job(void *arg)
{
    scan_t *s = arg;
    const char *root = hal_sd_root();
    char *path = malloc(320);
    if (root && path) {
        static const char *const DIRS[2] = { "", "/logs" };
        for (int d = 0; d < 2; d++) {
            snprintf(path, 320, "%s%s", root, DIRS[d]);
            DIR *dp = opendir(path);
            if (!dp) continue;
            struct dirent *e;
            while ((e = readdir(dp))) {
                if (!log_file(e->d_name)) continue;
                s->found++;
                snprintf(path, 320, "%s%s/%s", root, DIRS[d], e->d_name);
                struct stat st;
                if (stat(path, &st) != 0) continue;
                uint32_t h = cat_batt_seen_hash(e->d_name, (long)st.st_size, (int64_t)st.st_mtime);
                bool seen = false;
                for (int k = 0; k < s->nseen && !seen; k++) seen = s->seen[k] == h;
                if (seen) continue;
                if (s->nres >= SCAN_PER_JOB) {
                    s->more++;
                    continue;
                }
                scan_res_t *r = &s->res[s->nres++];
                snprintf(r->name, sizeof r->name, "%s", e->d_name);
                r->hash = h;
                if (st.st_size > SCAN_MAX_BYTES) {
                    snprintf(r->log.error, sizeof r->log.error, "larger than 32 MB");
                    continue;
                }
                r->ok = cat_batt_log(path, &r->log);
            }
            closedir(dp);
        }
    }
    free(path);
    __atomic_store_n(&BM.scan_phase, 2, __ATOMIC_RELEASE);
}

static void scan_merge(void)
{
    scan_t *s = BM.scan;
    int read = 0, matched = 0;
    for (int i = 0; i < s->nres; i++) {
        scan_res_t *r = &s->res[i];
        cat_fleet_mark_seen(BM.f, r->hash);
        if (!r->ok) {
            printf("batteries: %s: %s\n", r->name, r->log.error);
            continue;
        }
        read++;
        int bi = -1;
        cat_batt_use_t *u = cat_fleet_attribute(BM.f, &r->log, &bi);
        if (!u || bi < 0) {
            BM.unmatched++;
            continue;
        }
        cat_batt_merge(u, &r->log, r->name);
        cat_batt_update_base(&BM.f->b[bi]);
        matched++;
    }
    BM.logs_found = s->found;
    BM.read_total += read;
    if (s->nres) {
        check_watch();
        bm_changed();
    }
    if (matched) {
        char msg[64];
        snprintf(msg, sizeof msg, "battery numbers from %d log%s", matched, matched == 1 ? "" : "s");
        ui_island_say(BZ_I_BATTERY_CHARGING_FULL, msg);
    }
    BM.scanning = s->more > 0;
    if (s->more) {
        BM.scan_wanted = true;
        BM.next_scan = hal_seconds() + 1;
    }
}

static void scan_tick(double now)
{
    int ph = __atomic_load_n(&BM.scan_phase, __ATOMIC_ACQUIRE);
    if (ph == 2) {
        scan_merge();
        __atomic_store_n(&BM.scan_phase, 0, __ATOMIC_RELEASE);
        return;
    }
    if (ph != 0 || !BM.scan_wanted || now < BM.next_scan || !BM.f || !hal_sd_root()) return;
    if (!BM.scan) BM.scan = calloc(1, sizeof *BM.scan);
    if (!BM.scan) return;
    memset(BM.scan, 0, sizeof *BM.scan);
    BM.scan->nseen = BM.f->nseen;
    memcpy(BM.scan->seen, BM.f->seen, sizeof BM.scan->seen[0] * (size_t)BM.f->nseen);
    __atomic_store_n(&BM.scan_phase, 1, __ATOMIC_RELEASE);
    if (assist_post_job(scan_job, BM.scan)) {
        BM.scan_wanted = false;
        BM.scanning = true;
    } else {
        /* the worker's slot is taken (an analysis, a scan): again shortly */
        __atomic_store_n(&BM.scan_phase, 0, __ATOMIC_RELEASE);
        BM.next_scan = now + 5;
    }
}

static void scan_soon(double delay)
{
    BM.scan_wanted = true;
    double at = hal_seconds() + delay;
    if (!BM.next_scan || at < BM.next_scan || BM.next_scan < hal_seconds()) BM.next_scan = at;
}

void ui_batt_logs_changed(void) { scan_soon(2); }

/* ------------------------------------------------------------------ the robot, live */

static void live_commit(void)
{
    int bi = cat_fleet_find_uid(BM.f, BM.lv.uid);
    if (bi < 0) return;
    cat_batt_t *b = &BM.f->b[bi];
    cat_batt_use_t *u = NULL;
    for (int k = 0; k < b->nuse; k++)
        if (b->use[k].t == BM.lv.use_t && (b->use[k].src & CU_PICK)) u = &b->use[k];
    if (!u || (u->src & CU_LOG)) return; /* a log's numbers are better */
    cat_batt_use_t m;
    cat_batt_use_init(&m);
    if (!cat_batt_acc_finish(&BM.lv.acc, &m)) return;
    /* a second run on the same pick (practice): the lowest, the sums, the latest fit */
    bool had = u->src & CU_LIVE;
    if (KNOWN(m.v_min)) u->v_min = had && KNOWN(u->v_min) ? fminf(u->v_min, m.v_min) : m.v_min;
    if (KNOWN(m.v_rest) && !(had && KNOWN(u->v_rest))) u->v_rest = m.v_rest;
    if (m.brownouts >= 0) u->brownouts = (int16_t)((had && u->brownouts > 0 ? u->brownouts : 0) + m.brownouts);
    if (KNOWN(m.r_mohm)) u->r_mohm = m.r_mohm;
    if (KNOWN(m.amps)) u->amps = m.amps;
    if (KNOWN(m.wh)) u->wh = (had && KNOWN(u->wh) ? u->wh : 0) + m.wh;
    if (KNOWN(m.peak_a)) u->peak_a = had && KNOWN(u->peak_a) ? fmaxf(u->peak_a, m.peak_a) : m.peak_a;
    if (KNOWN(m.dur_s)) u->dur_s = (had && KNOWN(u->dur_s) ? u->dur_s : 0) + m.dur_s;
    u->src |= CU_LIVE;
    cat_batt_update_base(b);
    check_watch();
    bm_changed();
}

static void live_end(void)
{
    if (BM.lv.on && BM.lv.ran) live_commit();
    BM.lv.on = BM.lv.ran = false;
}

static void live_tick(double now)
{
    int bi = -1;
    cat_batt_use_t *u = cat_fleet_last_pick(BM.f, &bi);
    bool pick = u && bi >= 0 && time(NULL) - u->t < PICK_LIVE_S && !(u->src & CU_LOG);
    bool link = R && R->connected && R->have_battery && R->battery_v > 3;
    if (!pick || !link) {
        live_end();
        return;
    }
    if (BM.lv.on && (BM.lv.uid != BM.f->b[bi].uid || BM.lv.use_t != u->t)) live_end();
    if (!BM.lv.on) {
        cat_batt_acc_init(&BM.lv.acc);
        BM.lv.on = true;
        BM.lv.uid = BM.f->b[bi].uid;
        BM.lv.use_t = u->t;
    }
    double amps = isfinite(R->total_current) ? R->total_current : isfinite(R->pd_total) ? R->pd_total : NAN;
    bool en = R->have_mode && R->enabled;
    cat_batt_acc_add(&BM.lv.acc, now, (float)R->battery_v, (float)amps, R->have_mode ? en : -1);
    if (R->have_sc) cat_batt_acc_brown(&BM.lv.acc, R->browned_out);
    if (en) {
        BM.lv.ran = true;
        BM.lv.last_en = now;
    } else if (BM.lv.ran && now - BM.lv.last_en > LIVE_END_S) {
        live_end();
    }
}

/* ------------------------------------------------------------------ gpt */

static void an_tick(void)
{
    unsigned g = analyze_gen();
    if (g == BM.an_seen) return;
    BM.an_seen = g;
    char *name = NULL, *answer = NULL;
    an_phase_t ph = analyze_get(&name, &answer, NULL, NULL, 0);
    if (name && !strcmp(name, ANALYZE_FLEET_NAME) && (ph == AN_DONE || ph == AN_FAILED)) {
        const char *a = answer ? answer : "";
        while (*a == ' ' || *a == '\n') a++;
        if (!strncmp(a, "verdict:", 8)) a += 8;
        while (*a == ' ') a++;
        char v[80], msg[96];
        snprintf(v, sizeof v, "%.*s", (int)strcspn(a, "\n"), a);
        home_fold_text(v);
        snprintf(msg, sizeof msg, "%s: %s", ph == AN_DONE ? "battery advice" : "no battery advice", v);
        ui_island_say(ph == AN_DONE ? BZ_I_AUTO_AWESOME : BZ_I_WARNING, msg);
    }
    free(name);
    free(answer);
}

static void ask_gpt(void)
{
    if (!BM.f) return;
    char *sum = malloc(12000);
    if (!sum) return;
    cat_batt_summary(BM.f, time(NULL), sum, 12000);
    bool ok = analyze_fleet_start(sum, S.team);
    free(sum);
    if (!ok) ui_island_say(BZ_I_AUTO_AWESOME, analyze_busy() ? "gpt is busy with an analysis" : "the assistant isn't running");
}

/* ------------------------------------------------------------------ the hook */

/* the fleet as text for the assistant's get_batteries tool: when it changes, and each minute as charges rest */
static void desk_post(double now)
{
    static unsigned gen = ~0u;
    static double next;
    if (BM.gen == gen && now < next) return;
    gen = BM.gen;
    next = now + 60;
    char *sum = malloc(7000);
    if (!sum) return;
    cat_batt_summary(BM.f, time(NULL), sum, 7000);
    assist_desk_post(AS_DESK_BATTERIES, sum);
    free(sum);
}

static void bm_tick(void *u)
{
    (void)u;
    if (!BM.f) return;
    double now = hal_seconds();
    scan_tick(now);
    live_tick(now);
    an_tick();
    save_tick(now);
    desk_post(now);
}

void ui_batt_boot(void)
{
    if (BM.booted) return;
    BM.booted = true;
    bm_load();
    BM.an_seen = analyze_gen();
    scan_soon(SCAN_FIRST_S);
    ui_on_refresh(bm_tick, NULL);
}

bool ui_batt_alarm_line(const char *match_label, char *out, size_t n)
{
    if (!BM.f || !BM.f->n) return false;
    int bi = -1;
    cat_batt_use_t *u = cat_fleet_last_pick(BM.f, &bi);
    char nm[20];
    if (u && bi >= 0 && match_label && match_label[0] && !strcmp(u->label, match_label) && time(NULL) - u->t < PICK_LIVE_S) {
        snprintf(out, n, "battery in: %s", bname(&BM.f->b[bi], nm, sizeof nm));
        return true;
    }
    bm_rank();
    if (BM.nrank < 1 || !BM.rank[0].available) return false;
    snprintf(out, n, "battery: %s \xc2\xb7 %s", bname(&BM.f->b[BM.rank[0].idx], nm, sizeof nm), BM.rank[0].reason);
    return true;
}

/* ------------------------------------------------------------------ a battery's tile */

typedef struct {
    int w, h;
    bool hl;           /* the recommended one: ice */
    bool dim;          /* bad or retired */
    bool top;          /* the recommendation's first */
} tile_opt_t;

static lv_obj_t *batt_tile(lv_obj_t *parent, const cat_batt_t *b, int64_t now, tile_opt_t o, bz_tap_fn fn)
{
    lv_obj_t *t = bz_tile(parent, o.w, o.h);
    lv_obj_set_style_pad_all(t, 16, 0);
    if (o.hl) bz_tile_set_fill(t, BZ_C_ICE);
    bz_color_role_t ink = o.hl ? BZ_C_ON_ICE : BZ_C_INK, dim = o.hl ? BZ_C_ON_ICE : BZ_C_DIM;
    char nm[20], s[48];
    lv_obj_t *l = bz_label_line(t, bname(b, nm, sizeof nm), BZ_F_TITLE, ink, o.w - 32 - 24);
    lv_obj_set_pos(l, 0, -6);
    lv_obj_t *mk = bz_mark(t, status_mark(b->status), 14);
    lv_obj_align(mk, LV_ALIGN_TOP_RIGHT, 0, 8);
    int m = 0;
    float r = cat_batt_r_now(b, &m);
    if (KNOWN(r)) snprintf(s, sizeof s, "%.0f mohm", r);
    else snprintf(s, sizeof s, "\xe2\x80\x94 mohm");
    l = bz_label_line(t, s, BZ_F_NAME, KNOWN(r) ? ink : dim, o.w - 32); /* not measured: dim, not a reading */
    lv_obj_set_pos(l, 0, 42);
    short_state(b, now, s, sizeof s, true);
    l = bz_label_line(t, s, BZ_F_CAPTION, dim, o.w - 32);
    lv_obj_set_pos(l, 0, o.h - 32 - 42);
    /* "(auto)" is the detail's to say: on a tile it pushed "watch" past the edge */
    if (o.top) snprintf(s, sizeof s, "recommended");
    else snprintf(s, sizeof s, "%d use%s%s", b->uses_total, b->uses_total == 1 ? "" : "s", b->status == CB_WATCH ? " \xc2\xb7 watch" : "");
    l = bz_label_line(t, s, BZ_F_CAPTION, o.top && !o.hl ? BZ_C_SIGNAL : dim, o.w - 32);
    lv_obj_set_pos(l, 0, o.h - 32 - 22);
    if (o.dim) lv_obj_set_style_opa(t, LV_OPA_40, 0);
    if (fn) {
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(t, fn, (void *)(intptr_t)b->uid);
    }
    return t;
}

static int top_idx(void) { return BM.nrank > 0 && BM.rank[0].available ? BM.rank[0].idx : -1; }

/* ------------------------------------------------------------------ the batteries app */

enum { V_NEXT, V_DETAIL, V_AI };

static struct {
    lv_obj_t *grid, *right, *title, *sub;
    ui_kb_t *kb;
    int view, sel_uid;
    unsigned shown_gen, an_gen;
    double next_min;
    double remove_armed;
    lv_obj_t *ai_state, *ai_text;
} BA;

static void ba_show(void);

static void ba_select(lv_obj_t *o, void *u)
{
    (void)o;
    BA.sel_uid = (int)(intptr_t)u;
    BA.view = V_DETAIL;
    BA.remove_armed = 0;
    ba_show();
}

static void ba_back(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    BA.view = V_NEXT;
    ba_show();
}

static cat_batt_t *sel(int *idx)
{
    int i = BM.f ? cat_fleet_find_uid(BM.f, BA.sel_uid) : -1;
    if (idx) *idx = i;
    return i >= 0 ? &BM.f->b[i] : NULL;
}

static void d_status(lv_obj_t *o, void *u)
{
    (void)o;
    cat_batt_t *b = sel(NULL);
    if (!b) return;
    b->status = (uint8_t)(intptr_t)u;
    b->auto_watch = false; /* by hand now */
    bm_changed();
}

static void d_charged(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    cat_batt_t *b = sel(NULL);
    if (!b) return;
    b->charged = time(NULL);
    char nm[20], msg[64];
    snprintf(msg, sizeof msg, "battery %s off the charger", bname(b, nm, sizeof nm));
    ui_island_say(BZ_I_BATTERY_CHARGING_FULL, msg);
    bm_changed();
}

static void d_put_in(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    int i;
    if (sel(&i)) bm_pick(i, CC_UNKNOWN);
}

static void kb_done(const char *text, void *u)
{
    cat_batt_t *b = sel(NULL);
    if (!b) return;
    int what = (int)(intptr_t)u;
    while (*text == ' ') text++;
    if (what == 0) {
        if (!text[0]) return;
        snprintf(b->label, sizeof b->label, "%s", text);
        size_t l = strlen(b->label);
        while (l && b->label[l - 1] == ' ') b->label[--l] = 0;
    } else if (what == 1) {
        snprintf(b->notes, sizeof b->notes, "%s", text);
    } else {
        int y = atoi(text);
        b->year = y >= 1990 && y <= 2100 ? y : 0;
    }
    bm_changed();
}

static void d_edit(lv_obj_t *o, void *u)
{
    (void)o;
    cat_batt_t *b = sel(NULL);
    if (!b) return;
    int what = (int)(intptr_t)u;
    char y[8] = "";
    if (b->year) snprintf(y, sizeof y, "%d", b->year);
    static const char *const TITLE[3] = { "the battery's name", "notes", "the year it was bought" };
    ui_kb_show(BA.kb, TITLE[what], what == 0 ? b->label : what == 1 ? b->notes : y, false, what != 1, kb_done,
               (void *)(intptr_t)what);
}

static void d_remove(lv_obj_t *o, void *u)
{
    (void)u;
    int i;
    cat_batt_t *b = sel(&i);
    if (!b) return;
    double now = hal_seconds();
    if (!BA.remove_armed || now - BA.remove_armed > 4) {
        BA.remove_armed = now;
        lv_obj_t *l = lv_obj_get_child(o, 1);
        if (l) ui_text(l, "tap again to remove");
        return;
    }
    char nm[20], msg[64];
    snprintf(msg, sizeof msg, "battery %s removed", bname(b, nm, sizeof nm));
    cat_fleet_remove(BM.f, i);
    ui_island_say(BZ_I_REMOVE, msg);
    BA.view = V_NEXT;
    bm_changed();
}

static void ba_add(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!BM.f) return;
    int i = cat_fleet_add(BM.f);
    if (i < 0) {
        ui_island_say(BZ_I_WARNING, "the roster is full (24)");
        return;
    }
    BA.sel_uid = BM.f->b[i].uid;
    BA.view = V_DETAIL;
    bm_changed();
}

static void ba_scan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!hal_sd_root()) {
        ui_island_say(BZ_I_SD_CARD, "no card: the logs are read off the card");
        return;
    }
    scan_soon(0);
    ui_island_say(BZ_I_SYNC, "reading the card's logs for battery numbers");
}

static void ba_gpt(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    BA.view = V_AI;
    ask_gpt();
    ba_show();
}

static lv_obj_t *wrap_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *para(lv_obj_t *parent, const char *text, bz_font_role_t f, bz_color_role_t c, int w)
{
    lv_obj_t *l = bz_label(parent, text, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    return l;
}

static void build_grid(int64_t now)
{
    lv_obj_clean(BA.grid);
    int tw = (LW - 3 * BZ_GAP) / 4, top = top_idx();
    lv_obj_t *row = bz_row(BA.grid, BZ_GAP);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, BZ_GAP, 0);
    lv_obj_set_width(row, LW);
    for (int i = 0; i < BM.f->n; i++) {
        const cat_batt_t *b = &BM.f->b[i];
        tile_opt_t o = { .w = tw, .h = 150, .dim = b->status >= CB_BAD, .top = i == top };
        lv_obj_t *t = batt_tile(row, b, now, o, ba_select);
        if (BA.view == V_DETAIL && b->uid == BA.sel_uid) {
            lv_obj_set_style_border_width(t, 3, 0);
            lv_obj_set_style_border_color(t, bz_lv(BZ_C_SIGNAL), 0);
        }
    }
    lv_obj_t *sp = bz_box(BA.grid);
    lv_obj_set_height(sp, 24);
}

static void build_next(int64_t now)
{
    lv_obj_t *c = BA.right;
    int w = IN(RW);
    char key[24], label[12], at[24];
    time_t when = 0;
    bool m = next_match(key, sizeof key, label, sizeof label, &when);
    if (m && when) day_hhmm(when, at, sizeof at);
    char h[64];
    if (m) snprintf(h, sizeof h, "for %s%s%s", label, when ? " \xc2\xb7 " : "", when ? at : "");
    else snprintf(h, sizeof h, "for the next match");
    bz_label_line(c, h, BZ_F_NAME, BZ_C_INK, w);
    int shown = 0;
    for (int i = 0; i < BM.nrank && shown < 8; i++) {
        const cat_batt_rank_t *r = &BM.rank[i];
        const cat_batt_t *b = &BM.f->b[r->idx];
        if (!r->available && b->status == CB_RETIRED) continue;
        char nm[20], line[160];
        lv_obj_t *row = bz_col(c, 2);
        lv_obj_set_width(row, w);
        snprintf(line, sizeof line, "%s%s", bname(b, nm, sizeof nm), r->available ? "" : " \xc2\xb7 out");
        bz_label_line(row, line, BZ_F_NAME, !r->available ? BZ_C_DIM : shown == 0 ? BZ_C_SIGNAL : BZ_C_INK, w);
        para(row, r->reason, BZ_F_BODY_S, r->available ? BZ_C_INK : BZ_C_DIM, w);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(row, ba_select, (void *)(intptr_t)b->uid);
        shown++;
    }
    char foot[200];
    int o = snprintf(foot, sizeof foot, "%d batteries \xc2\xb7 %d log%s read", BM.f->n, BM.read_total, BM.read_total == 1 ? "" : "s");
    if (BM.unmatched && o > 0 && (size_t)o < sizeof foot)
        o += snprintf(foot + o, sizeof foot - (size_t)o, ", %d with no pick to match", BM.unmatched);
    if (BM.scanning && o > 0 && (size_t)o < sizeof foot) o += snprintf(foot + o, sizeof foot - (size_t)o, " \xc2\xb7 reading more");
    if (R && R->battery_model[0] && o > 0 && (size_t)o < sizeof foot)
        o += snprintf(foot + o, sizeof foot - (size_t)o, " \xc2\xb7 the robot's type: %s", R->battery_model);
    if (!hal_sd_root() && o > 0 && (size_t)o < sizeof foot)
        o += snprintf(foot + o, sizeof foot - (size_t)o, " \xc2\xb7 no card: histories aren't kept");
    else if (BM.save_failed && o > 0 && (size_t)o < sizeof foot)
        o += snprintf(foot + o, sizeof foot - (size_t)o, " \xc2\xb7 the last write to the card failed");
    para(c, foot, BZ_F_CAPTION, BZ_C_DIM, w);
    lv_obj_t *r = wrap_row(c, w);
    ui_button(r, BZ_I_ADD, "add a battery", ba_add, NULL);
    (void)now;
}

static void build_detail(int64_t now)
{
    lv_obj_t *c = BA.right;
    int w = IN(RW);
    cat_batt_t *b = sel(NULL);
    if (!b) {
        BA.view = V_NEXT;
        build_next(now);
        return;
    }
    char nm[20], s[200];
    lv_obj_t *r = wrap_row(c, w);
    ui_button(r, BZ_I_ARROW_BACK, "back", ba_back, NULL);
    ui_button(r, BZ_I_BATTERY_CHARGING_FULL, "put in now", d_put_in, NULL);
    snprintf(s, sizeof s, "battery %s", bname(b, nm, sizeof nm));
    bz_label_line(c, s, BZ_F_TITLE, BZ_C_INK, w);
    r = wrap_row(c, w);
    for (int i = 0; i < CB_NSTATUS; i++) ui_chip_set(ui_chip(r, CAT_BATT_STATUS[i], d_status, (void *)(intptr_t)i), b->status == i);
    int m = 0;
    float rn = cat_batt_r_now(b, &m);
    char rs[32] = "not measured", bs[32] = "", yr[16] = "year unknown", ch[48];
    if (KNOWN(rn)) snprintf(rs, sizeof rs, "%.1f mohm", rn);
    if (KNOWN(b->r_base)) snprintf(bs, sizeof bs, " (baseline %.1f)", b->r_base);
    if (b->year) snprintf(yr, sizeof yr, "bought %d", b->year);
    short_state(b, now, ch, sizeof ch, false);
    snprintf(s, sizeof s, "resistance %s%s \xc2\xb7 %d measured\n%s \xc2\xb7 %d uses \xc2\xb7 %s%s", rs, bs, m, yr,
             b->uses_total, ch, b->auto_watch && b->status == CB_WATCH ? "\non watch by its numbers" : "");
    para(c, s, BZ_F_BODY_S, BZ_C_INK, w);
    if (b->notes[0]) para(c, b->notes, BZ_F_BODY_S, BZ_C_DIM, w);
    r = wrap_row(c, w);
    ui_button(r, BZ_I_BATTERY_FULL, "off the charger now", d_charged, NULL);
    ui_button(r, BZ_I_EDIT_NOTE, "rename", d_edit, (void *)0);
    ui_button(r, BZ_I_EDIT_NOTE, "notes", d_edit, (void *)1);
    ui_button(r, BZ_I_SCHEDULE, "year", d_edit, (void *)2);
    ui_button(r, BZ_I_REMOVE, "remove", d_remove, NULL);
    bz_label(c, b->nuse ? "its uses, newest first" : "no uses yet: pick it in the checklist", BZ_F_LABEL, BZ_C_DIM);
    for (int k = b->nuse - 1; k >= 0; k--) {
        const cat_batt_use_t *u = &b->use[k];
        char when[24], v0[16] = "-", vm[16] = "-", rr[16] = "-", wh[16] = "-", br[16] = "-", a[24];
        day_hhmm(u->t, when, sizeof when);
        if (KNOWN(u->v_rest)) snprintf(v0, sizeof v0, "%.2f v", u->v_rest);
        if (KNOWN(u->v_min)) snprintf(vm, sizeof vm, "%.1f v", u->v_min);
        if (KNOWN(u->r_mohm)) snprintf(rr, sizeof rr, "%.0f mohm", u->r_mohm);
        if (KNOWN(u->wh)) snprintf(wh, sizeof wh, "%.1f wh", u->wh);
        if (u->brownouts >= 0) snprintf(br, sizeof br, "%d", u->brownouts);
        static const char *const CH[CC_NCHARGE] = { "", " \xc2\xb7 fresh off charger", " \xc2\xb7 rested", " \xc2\xb7 not charged" };
        ago(now - u->t, a, sizeof a);
        lv_obj_t *row = bz_col(c, 2);
        lv_obj_set_width(row, w);
        snprintf(s, sizeof s, "%s \xc2\xb7 %s%s", u->label[0] ? u->label : "no match", when, CH[u->charge < CC_NCHARGE ? u->charge : 0]);
        bz_label_line(row, s, BZ_F_BODY_S, BZ_C_INK, w);
        if (u->src & (CU_LOG | CU_LIVE))
            snprintf(s, sizeof s, "rest %s, low %s, %s, brownouts %s, %s \xc2\xb7 %s", v0, vm, rr, br, wh,
                     u->src & CU_LOG ? "from the log" : "live");
        else snprintf(s, sizeof s, "nothing measured yet \xc2\xb7 %s", a);
        para(row, s, BZ_F_CAPTION, BZ_C_DIM, w);
    }
}

static void ai_fill(void)
{
    BA.an_gen = analyze_gen();
    char *name = NULL, *answer = NULL, model[48] = "";
    an_phase_t ph = analyze_get(&name, &answer, NULL, model, sizeof model);
    bool ours = name && !strcmp(name, ANALYZE_FLEET_NAME);
    if (!ours) ph = AN_IDLE;
    switch (ph) {
    case AN_IDLE: ui_text(BA.ai_state, "%s", analyze_busy() ? "gpt is busy with a log's analysis: ask again after" : "ask gpt sends the roster and the histories, not the logs"); break;
    case AN_READING:
    case AN_ASKING: ui_text(BA.ai_state, "asking %s...", model[0] ? model : "gpt"); break;
    case AN_DONE: ui_text(BA.ai_state, "answered by %s", model); break;
    case AN_FAILED: ui_text(BA.ai_state, "no answer"); break;
    }
    const char *t = (ph == AN_DONE || ph == AN_FAILED) && answer ? answer : "";
    char *f = malloc(strlen(t) + 1);
    if (f) {
        /* each line folded for the fonts, the breaks kept */
        size_t k = 0;
        while (*t) {
            size_t l = strcspn(t, "\n");
            memcpy(f + k, t, l);
            f[k + l] = 0;
            home_fold_text(f + k);
            k += strlen(f + k);
            t += l;
            if (*t == '\n') f[k++] = '\n', t++;
        }
        f[k] = 0;
        const char *cur = lv_label_get_text(BA.ai_text);
        if (!cur || strcmp(cur, f)) lv_label_set_text(BA.ai_text, f);
        free(f);
    }
    free(name);
    free(answer);
}

static void build_ai(void)
{
    lv_obj_t *c = BA.right;
    int w = IN(RW);
    lv_obj_t *r = wrap_row(c, w);
    ui_button(r, BZ_I_ARROW_BACK, "back", ba_back, NULL);
    ui_button(r, BZ_I_AUTO_AWESOME, "ask again", ba_gpt, NULL);
    bz_label_line(c, "gpt on the fleet", BZ_F_NAME, BZ_C_INK, w);
    BA.ai_state = para(c, "", BZ_F_LABEL, BZ_C_DIM, w);
    BA.ai_text = para(c, "", BZ_F_BODY_S, BZ_C_INK, w);
    lv_obj_set_style_text_line_space(BA.ai_text, 4, 0);
    ai_fill();
}

static void ba_show(void)
{
    if (!BA.grid || !BM.f) return;
    int64_t now = time(NULL);
    bm_rank();
    build_grid(now);
    lv_obj_clean(BA.right);
    BA.ai_state = BA.ai_text = NULL;
    if (BA.view == V_DETAIL) build_detail(now);
    else if (BA.view == V_AI) build_ai();
    else build_next(now);
    lv_obj_t *sp = bz_box(BA.right); /* the last line clear of the bottom */
    lv_obj_set_height(sp, 30);
    BA.shown_gen = BM.gen;
    BA.next_min = hal_seconds() + 60;
    ui_text(BA.sub, "%d in the roster%s", BM.f->n, BM.scanning ? " \xc2\xb7 reading logs" : "");
}

static void batt_build(lv_obj_t *b)
{
    lv_obj_t *hr = bz_row(b, 14);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18); /* clear of the orb */
    lv_obj_set_height(hr, 60);
    BA.sub = bz_label(hr, "", BZ_F_LABEL, BZ_C_DIM);
    ui_button(hr, BZ_I_SYNC, "read logs", ba_scan, NULL);
    ui_button(hr, BZ_I_AUTO_AWESOME, "ask gpt", ba_gpt, NULL);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    BA.grid = ui_scroller(wrap, LW, APP_H);
    lv_obj_t *rt = bz_tile(b, RW, APP_H);
    lv_obj_set_pos(rt, RX, APP_Y);
    lv_obj_t *rw = bz_box(rt);
    lv_obj_set_size(rw, IN(RW), APP_H - 2 * BZ_PAD_TILE);
    BA.right = ui_scroller(rw, IN(RW), APP_H - 2 * BZ_PAD_TILE);
    lv_obj_set_style_pad_row(BA.right, 12, 0);
    BA.kb = ui_kb_create(b, 420);
}

static void batt_open(void)
{
    if (!BM.booted) ui_batt_boot();
    if (BA.view == V_DETAIL && !sel(NULL)) BA.view = V_NEXT;
    ba_show();
    scan_soon(1); /* new logs since: read them */
}

static void batt_refresh(void)
{
    if (!BM.f) return;
    if (BA.shown_gen != BM.gen || hal_seconds() >= BA.next_min) {
        if (!ui_kb_open(BA.kb)) ba_show();
    } else if (BA.view == V_AI && BA.ai_state && analyze_gen() != BA.an_gen) {
        ai_fill();
    }
}

const ui_app_t APP_BATT = { .name = "batteries", .icon = BZ_I_BATTERY_CHARGING_FULL, .build = batt_build, .open = batt_open,
                            .refresh = batt_refresh };

/* ------------------------------------------------------------------ the checklist's battery row and picker */

static struct {
    lv_obj_t *row_title, *row_sub, *sheet, *title, *grid, *chips[3], *bad_lbl;
    int charge;          /* CC_*: how charged the one going in is, if said */
    bool mark_bad;
    unsigned shown_gen;
    double next_min;
} PK;

static void pk_build_grid(void);

static void pk_close(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (PK.sheet) lv_obj_add_flag(PK.sheet, LV_OBJ_FLAG_HIDDEN);
    PK.mark_bad = false;
    PK.shown_gen = BM.gen - 1; /* the row says what was picked */
}

static void pk_chips(void)
{
    static const int C[3] = { CC_FRESH, CC_RESTED, CC_USED };
    for (int i = 0; i < 3; i++) ui_chip_set(PK.chips[i], PK.charge == C[i]);
    if (PK.bad_lbl) ui_text(PK.bad_lbl, "%s", PK.mark_bad ? "tap the bad one" : "mark bad");
}

static void pk_charge(lv_obj_t *o, void *u)
{
    (void)o;
    int c = (int)(intptr_t)u;
    PK.charge = PK.charge == c ? CC_UNKNOWN : c;
    pk_chips();
}

static void pk_bad(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    PK.mark_bad = !PK.mark_bad;
    pk_chips();
}

static void pk_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = BM.f ? cat_fleet_find_uid(BM.f, (int)(intptr_t)u) : -1;
    if (i < 0) return;
    cat_batt_t *b = &BM.f->b[i];
    if (PK.mark_bad) {
        PK.mark_bad = false;
        b->status = CB_BAD;
        b->auto_watch = false;
        char nm[20], msg[64];
        snprintf(msg, sizeof msg, "battery %s marked bad", bname(b, nm, sizeof nm));
        ui_island_say(BZ_I_BATTERY_ALERT, msg);
        bm_changed();
        pk_chips();
        pk_build_grid();
        return;
    }
    if (b->status >= CB_BAD) return;
    hal_tone(1800, 12, S.volume * 0.4f);
    bm_pick(i, PK.charge);
    PK.charge = CC_UNKNOWN;
    pk_close(NULL, NULL);
}

static void pk_build_grid(void)
{
    if (!PK.grid || !BM.f) return;
    lv_obj_clean(PK.grid);
    bm_rank();
    int64_t now = time(NULL);
    int w = W - 2 * PAD - 2 * BZ_PAD_TILE, tw = (w - 5 * BZ_GAP) / 6, top = top_idx();
    lv_obj_t *row = bz_row(PK.grid, BZ_GAP);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, BZ_GAP, 0);
    lv_obj_set_width(row, w);
    for (int i = 0; i < BM.f->n; i++) {
        const cat_batt_t *b = &BM.f->b[i];
        bool out = b->status >= CB_BAD;
        tile_opt_t o = { .w = tw, .h = 150, .hl = i == top, .dim = out, .top = i == top };
        batt_tile(row, b, now, o, pk_tap);
    }
    char key[24], label[12];
    next_match(key, sizeof key, label, sizeof label, NULL);
    ui_text(PK.title, "which battery goes in%s%s?", label[0] ? " for " : "", label);
    PK.shown_gen = BM.gen;
    PK.next_min = hal_seconds() + 60;
}

static void pk_open(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!BM.booted) ui_batt_boot();
    if (!PK.sheet || !BM.f) return;
    PK.charge = CC_UNKNOWN;
    PK.mark_bad = false;
    pk_chips();
    pk_build_grid();
    lv_obj_move_foreground(PK.sheet);
    lv_obj_remove_flag(PK.sheet, LV_OBJ_FLAG_HIDDEN);
}

static void open_batteries(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_app_close();
    ui_app_open(&APP_BATT, NULL);
}

void ui_batt_check_row(lv_obj_t *col, lv_obj_t *body, int w)
{
    lv_obj_t *t = bz_tile(col, w, 104);
    lv_obj_set_style_pad_ver(t, 0, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t, 16, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    bz_icon(t, BZ_I_BATTERY_CHARGING_FULL, 32, BZ_C_ICE);
    lv_obj_t *c = bz_col(t, 4);
    lv_obj_set_flex_grow(c, 1);
    PK.row_title = bz_label_line(c, "", BZ_F_NAME, BZ_C_INK, w - 2 * BZ_PAD_TILE - 48 - 200);
    PK.row_sub = bz_label_line(c, "", BZ_F_CAPTION, BZ_C_DIM, w - 2 * BZ_PAD_TILE - 48 - 200);
    lv_obj_t *btn = ui_button(t, BZ_I_BATTERY_FULL, "batteries", open_batteries, NULL);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE); /* its own tap, not the row's picker too */
    bz_on_tap(t, pk_open, NULL);

    /* the picker: a sheet over the checklist */
    int sw = W - 2 * PAD, iw = sw - 2 * BZ_PAD_TILE;
    PK.sheet = bz_tile(body, sw, APP_H);
    lv_obj_set_pos(PK.sheet, PAD, APP_Y);
    bz_tile_set_fill(PK.sheet, BZ_C_SURFACE1);
    lv_obj_add_flag(PK.sheet, LV_OBJ_FLAG_CLICKABLE); /* nothing under it takes a tap */
    PK.title = bz_label_line(PK.sheet, "", BZ_F_NAME, BZ_C_INK, iw - 420);
    lv_obj_t *r = bz_row(PK.sheet, 10);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, -8);
    lv_obj_t *bb = ui_button(r, BZ_I_BATTERY_ALERT, "mark bad", pk_bad, NULL);
    PK.bad_lbl = lv_obj_get_child(bb, 1);
    ui_button(r, BZ_I_CLOSE, "cancel", pk_close, NULL);
    lv_obj_t *cr = bz_row(PK.sheet, 10);
    lv_obj_set_pos(cr, 0, 48);
    bz_label(cr, "charge, if known:", BZ_F_LABEL, BZ_C_DIM);
    PK.chips[0] = ui_chip(cr, "fresh off charger", pk_charge, (void *)(intptr_t)CC_FRESH);
    PK.chips[1] = ui_chip(cr, "rested", pk_charge, (void *)(intptr_t)CC_RESTED);
    PK.chips[2] = ui_chip(cr, "not charged", pk_charge, (void *)(intptr_t)CC_USED);
    lv_obj_t *gw = bz_box(PK.sheet);
    int gh = APP_H - 2 * BZ_PAD_TILE - 122;
    lv_obj_set_pos(gw, 0, 122);
    lv_obj_set_size(gw, iw, gh);
    PK.grid = ui_scroller(gw, iw, gh);
    lv_obj_add_flag(PK.sheet, LV_OBJ_FLAG_HIDDEN);
    if (!BM.booted) ui_batt_boot();
    PK.shown_gen = BM.gen - 1;
    ui_batt_check_refresh();
}

void ui_batt_check_refresh(void)
{
    if (!PK.row_title || !BM.f) return;
    double now = hal_seconds();
    if (PK.shown_gen == BM.gen && now < PK.next_min) return;
    if (PK.sheet && !lv_obj_has_flag(PK.sheet, LV_OBJ_FLAG_HIDDEN)) {
        pk_build_grid();
        return;
    }
    PK.shown_gen = BM.gen;
    PK.next_min = now + 60;
    bm_rank();
    int64_t wall = time(NULL);
    char key[24], label[12], nm[20], when[24];
    next_match(key, sizeof key, label, sizeof label, NULL);
    int bi = -1;
    cat_batt_use_t *u = cat_fleet_last_pick(BM.f, &bi);
    bool cur = u && bi >= 0 && wall - u->t < 3 * 3600 && (!label[0] || !u->label[0] || !strcmp(u->label, label));
    int top = top_idx();
    if (cur) {
        day_hhmm(u->t, when, sizeof when);
        static const char *const CH[CC_NCHARGE] = { "", " \xc2\xb7 fresh off charger", " \xc2\xb7 rested", " \xc2\xb7 not charged" };
        ui_text(PK.row_title, "battery %s in%s%s", bname(&BM.f->b[bi], nm, sizeof nm), u->label[0] ? " for " : "", u->label);
        ui_text(PK.row_sub, "at %s%s \xc2\xb7 tap to change", when, CH[u->charge < CC_NCHARGE ? u->charge : 0]);
    } else {
        ui_text(PK.row_title, "battery%s%s: tap to pick the one going in", label[0] ? " for " : "", label);
        if (top >= 0) ui_text(PK.row_sub, "recommended %s: %s", bname(&BM.f->b[top], nm, sizeof nm), BM.rank[0].reason);
        else ui_text(PK.row_sub, "no battery is ready: every one is out, bad or just used");
    }
}

/* ------------------------------------------------------------------ the dev console */

static void dev_demo(void)
{
    /* made-up history on the first six: a spread of resistances, one aging, one just used */
    int64_t now = time(NULL);
    static const float R0[6] = { 16, 18, 21, 24, 15, 19 };
    for (int i = 0; i < 6 && i < BM.f->n; i++) {
        cat_batt_t *b = &BM.f->b[i];
        for (int k = 0; k < 5; k++) {
            cat_batt_use_t *u = cat_batt_push_use(b);
            u->t = now - (int64_t)(6 - k) * 5400 - i * 600;
            u->src = CU_PICK | CU_LOG;
            snprintf(u->label, sizeof u->label, "Q%d", 10 + k * 6 + i);
            float r = R0[i] + (i == 3 ? k * 2.5f : 0);
            u->r_mohm = r;
            u->v_rest = 12.9f - r * 0.004f;
            u->v_min = 12.6f - r * 0.2f;
            u->wh = 13 + i;
            u->amps = 35 + i;
            u->peak_a = 180 + 10 * i;
            u->dur_s = 150;
            u->brownouts = (int16_t)(i == 3 && k > 2);
            snprintf(u->log, sizeof u->log, "demo");
        }
        b->charged = i == 5 ? 0 : now - 1800 * (i + 1);
        cat_batt_update_base(b);
    }
    if (BM.f->n > 5) BM.f->b[5].use[BM.f->b[5].nuse - 1].t = now - 1200; /* just used */
    check_watch();
    bm_changed();
}

void ui_batt_dev(const char *args)
{
    if (!BM.booted) ui_batt_boot();
    if (!BM.f) {
        printf("bms: no memory for the fleet\n");
        return;
    }
    while (*args == ' ') args++;
    if (!strcmp(args, "scan")) {
        scan_soon(0);
        printf("bms: reading the card's logs\n");
    } else if (!strncmp(args, "pick ", 5)) {
        for (int i = 0; i < BM.f->n; i++)
            if (!strcmp(BM.f->b[i].label, args + 5)) {
                bm_pick(i, CC_FRESH);
                printf("bms: %s in\n", BM.f->b[i].label);
                return;
            }
        printf("bms: no battery called %s\n", args + 5);
        return;
    } else if (!strcmp(args, "demo")) {
        dev_demo();
        printf("bms: demo history on the first six (bms reset puts the roster back)\n");
    } else if (!strcmp(args, "reset")) {
        cat_fleet_default(BM.f, 12);
        BM.unmatched = BM.read_total = 0;
        bm_changed();
        printf("bms: the default roster of 12, no history\n");
    } else if (!strcmp(args, "gpt")) {
        ask_gpt();
    } else if (!strcmp(args, "json")) {
        size_t len = 0;
        char *js = cat_batt_to_json(BM.f, &len);
        if (js) {
            fwrite(js, 1, len, stdout);
            free(js);
        }
        return;
    } else if (args[0]) {
        printf("bms: scan | pick <label> | demo | reset | gpt | json\n");
        return;
    }
    bm_rank();
    printf("bms: %d batteries, %s, %d logs read, %d unmatched%s\n", BM.f->n, BM.on_card ? "on the card" : "not on the card",
           BM.read_total, BM.unmatched, BM.scanning ? ", reading" : "");
    for (int i = 0; i < BM.nrank; i++) {
        char nm[20];
        printf("  %d. %s %s%.1f  %s\n", i + 1, bname(&BM.f->b[BM.rank[i].idx], nm, sizeof nm), BM.rank[i].available ? "" : "(out) ",
               (double)BM.rank[i].score, BM.rank[i].reason);
    }
    char line[160];
    if (ui_batt_alarm_line("", line, sizeof line)) printf("  alarm: %s\n", line);
}
