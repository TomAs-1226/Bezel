/* cat_batt — the battery fleet (cat_batt.h): the roster, the numbers of each use, the recommendation, and the
 * JSON on the card. */
#include "cat_batt.h"
#include "json_lite.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

const char *const CAT_BATT_STATUS[CB_NSTATUS] = { "good", "watch", "bad", "retired" };
const char *const CAT_BATT_CHARGE[CC_NCHARGE] = { "", "fresh", "rested", "used" };

#define KNOWN(x) ((x) == (x))
#define LABVIEW_EPOCH 2082844800LL

/* ------------------------------------------------------------------ the roster */

void cat_batt_use_init(cat_batt_use_t *u)
{
    memset(u, 0, sizeof *u);
    u->v_rest = u->v_min = u->r_mohm = u->wh = u->amps = u->peak_a = u->dur_s = NAN;
    u->brownouts = -1;
}

static void batt_init(cat_batt_t *b, int uid, const char *label)
{
    memset(b, 0, sizeof *b);
    b->uid = uid;
    snprintf(b->label, sizeof b->label, "%s", label);
    b->r_base = NAN;
}

void cat_fleet_default(cat_fleet_t *f, int n)
{
    memset(f, 0, sizeof *f);
    if (n > CAT_BATT_MAX) n = CAT_BATT_MAX;
    for (int i = 0; i < n; i++) {
        char l[8];
        snprintf(l, sizeof l, "%d", i + 1);
        batt_init(&f->b[i], i + 1, l);
    }
    f->n = n;
    f->next_uid = n + 1;
}

int cat_fleet_add(cat_fleet_t *f)
{
    if (f->n >= CAT_BATT_MAX) return -1;
    /* the lowest number no battery is called yet */
    int num = 1;
    for (bool clash = true; clash; ) {
        clash = false;
        for (int i = 0; i < f->n; i++)
            if (atoi(f->b[i].label) == num) {
                num++;
                clash = true;
                break;
            }
    }
    char l[8];
    snprintf(l, sizeof l, "%d", num);
    if (f->next_uid < 1) f->next_uid = 1;
    batt_init(&f->b[f->n], f->next_uid++, l);
    return f->n++;
}

void cat_fleet_remove(cat_fleet_t *f, int i)
{
    if (i < 0 || i >= f->n) return;
    memmove(&f->b[i], &f->b[i + 1], sizeof f->b[0] * (size_t)(f->n - i - 1));
    f->n--;
}

int cat_fleet_find_uid(const cat_fleet_t *f, int uid)
{
    for (int i = 0; i < f->n; i++)
        if (f->b[i].uid == uid) return i;
    return -1;
}

cat_batt_use_t *cat_batt_push_use(cat_batt_t *b)
{
    if (b->nuse >= CAT_BATT_USES) {
        memmove(&b->use[0], &b->use[1], sizeof b->use[0] * (CAT_BATT_USES - 1));
        b->nuse = CAT_BATT_USES - 1;
    }
    cat_batt_use_t *u = &b->use[b->nuse++];
    cat_batt_use_init(u);
    b->uses_total++;
    return u;
}

const cat_batt_use_t *cat_batt_last(const cat_batt_t *b) { return b->nuse ? &b->use[b->nuse - 1] : NULL; }

cat_batt_use_t *cat_fleet_last_pick(cat_fleet_t *f, int *bi)
{
    cat_batt_use_t *best = NULL;
    for (int i = 0; i < f->n; i++)
        for (int k = f->b[i].nuse - 1; k >= 0; k--) {
            cat_batt_use_t *u = &f->b[i].use[k];
            if (!(u->src & CU_PICK)) continue;
            if (!best || u->t > best->t) {
                best = u;
                if (bi) *bi = i;
            }
            break; /* the newest pick of this battery */
        }
    return best;
}

const char *cat_batt_name(const cat_batt_t *b, char *out, size_t n)
{
    bool num = b->label[0] != 0;
    for (const char *p = b->label; *p; p++) num &= isdigit((unsigned char)*p) != 0;
    snprintf(out, n, num ? "#%s" : "%s", b->label);
    return out;
}

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float median(float *v, int n)
{
    if (n <= 0) return NAN;
    qsort(v, (size_t)n, sizeof *v, cmp_f);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

float cat_batt_r_now(const cat_batt_t *b, int *n)
{
    float v[3];
    int k = 0, total = 0;
    for (int i = b->nuse - 1; i >= 0; i--)
        if (KNOWN(b->use[i].r_mohm)) {
            total++;
            if (k < 3) v[k++] = b->use[i].r_mohm;
        }
    if (n) *n = total;
    return median(v, k);
}

void cat_batt_update_base(cat_batt_t *b)
{
    if (KNOWN(b->r_base)) return;
    float v[3];
    int k = 0;
    for (int i = 0; i < b->nuse && k < 3; i++)
        if (KNOWN(b->use[i].r_mohm)) v[k++] = b->use[i].r_mohm;
    if (k == 3) b->r_base = median(v, 3);
}

bool cat_batt_watch_due(const cat_batt_t *b, char *why, size_t n)
{
    int m = 0;
    float r = cat_batt_r_now(b, &m);
    if (m < 2 || !KNOWN(r)) return false;
    if (r > CAT_BATT_WATCH_MOHM) {
        snprintf(why, n, "%.0f mohm, over %.0f", r, CAT_BATT_WATCH_MOHM);
        return true;
    }
    if (KNOWN(b->r_base) && m >= 4 && r > b->r_base * (1 + CAT_BATT_WATCH_RISE)) {
        snprintf(why, n, "%.0f mohm, up %.0f %% from %.0f", r, (r / b->r_base - 1) * 100, b->r_base);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ the recommendation */

static void ago(int64_t s, char *out, size_t n)
{
    if (s < 0) s = 0;
    if (s < 3600) snprintf(out, n, "%d min", (int)(s / 60));
    else if (s < 48 * 3600) snprintf(out, n, "%d h", (int)((s + 1800) / 3600));
    else snprintf(out, n, "%d d", (int)(s / 86400));
}

typedef struct {
    float w;
    char text[48];
} part_t;

static int by_weight(const void *a, const void *b)
{
    const part_t *x = a, *y = b;
    /* the good first, strongest first; then the bad, worst first */
    if ((x->w >= 0) != (y->w >= 0)) return x->w >= 0 ? -1 : 1;
    float ax = fabsf(x->w), ay = fabsf(y->w);
    return ax > ay ? -1 : ax < ay;
}

static int by_rank(const void *a, const void *b)
{
    const cat_batt_rank_t *x = a, *y = b;
    if (x->available != y->available) return x->available ? -1 : 1;
    return x->score > y->score ? -1 : x->score < y->score;
}

#define CHARGE_OUT_S (2 * 3600) /* used this recently and not recharged: out */
#define REST_S (30 * 60)        /* off the charger this long: rested, cool */

/* is the battery charged: marked charged after its last use (or never used and marked) */
static bool charged_since(const cat_batt_t *b)
{
    const cat_batt_use_t *l = cat_batt_last(b);
    return b->charged && (!l || b->charged >= l->t);
}

int cat_fleet_rank(const cat_fleet_t *f, int64_t now, cat_batt_rank_t *out, int max)
{
    int k = 0;
    /* the lowest resistance among the ones that could go in: named as such */
    float lowest = INFINITY;
    int uses24 = 0, avail = 0;
    for (int i = 0; i < f->n; i++) {
        const cat_batt_t *b = &f->b[i];
        if (b->status >= CB_BAD) continue;
        float r = cat_batt_r_now(b, NULL);
        if (KNOWN(r) && r < lowest) lowest = r;
        for (int u = 0; u < b->nuse; u++) uses24 += b->use[u].t > now - 86400;
        avail++;
    }
    float mean24 = avail ? (float)uses24 / avail : 0;
    for (int i = 0; i < f->n && k < max; i++) {
        const cat_batt_t *b = &f->b[i];
        cat_batt_rank_t *rk = &out[k++];
        memset(rk, 0, sizeof *rk);
        rk->idx = i;
        rk->available = true;
        if (b->status == CB_BAD || b->status == CB_RETIRED) {
            rk->available = false;
            rk->score = -1000;
            snprintf(rk->reason, sizeof rk->reason, "%s", b->status == CB_BAD ? "marked bad" : "retired");
            continue;
        }
        part_t p[8];
        int np = 0;
        char t[24];
        const cat_batt_use_t *last = cat_batt_last(b);
        if (charged_since(b)) {
            p[np].w = 20;
            snprintf(p[np++].text, sizeof p[0].text, "charged");
            int64_t rest = now - b->charged;
            ago(rest, t, sizeof t);
            if (rest >= REST_S) {
                p[np].w = 10;
                snprintf(p[np++].text, sizeof p[0].text, "rested %s", t);
            } else {
                p[np].w = -3;
                snprintf(p[np++].text, sizeof p[0].text, "off the charger %s, still warm", t);
            }
        } else if (last) {
            int64_t since = now - last->t;
            ago(since, t, sizeof t);
            if (since < CHARGE_OUT_S) {
                rk->available = false;
                rk->score = -500;
                snprintf(rk->reason, sizeof rk->reason, "went in %s ago, not recharged since", t);
                continue;
            }
            p[np].w = -8;
            snprintf(p[np++].text, sizeof p[0].text, "no charge recorded since its last use %s ago", t);
        } else {
            p[np].w = 0;
            snprintf(p[np++].text, sizeof p[0].text, "charge not recorded");
        }
        int m = 0;
        float r = cat_batt_r_now(b, &m);
        if (KNOWN(r)) {
            p[np].w = (22 - r) * 1.5f;
            if (r <= lowest + 0.05f && avail > 1) snprintf(p[np++].text, sizeof p[0].text, "lowest resistance (%.0f mohm)", r);
            else snprintf(p[np++].text, sizeof p[0].text, "%.0f mohm", r);
            if (KNOWN(b->r_base) && m >= 4 && r > b->r_base * 1.15f) {
                p[np].w = -8;
                snprintf(p[np++].text, sizeof p[0].text, "resistance rising (%.0f to %.0f)", b->r_base, r);
            }
        } else {
            p[np].w = 0;
            snprintf(p[np++].text, sizeof p[0].text, "resistance not measured yet");
        }
        int brown = 0, seen = 0;
        for (int u = b->nuse - 1; u >= 0 && seen < 3; u--) {
            if (b->use[u].brownouts < 0) continue;
            brown += b->use[u].brownouts;
            seen++;
        }
        if (brown > 0) {
            p[np].w = -10.0f * (brown > 3 ? 3 : brown);
            snprintf(p[np++].text, sizeof p[0].text, "%d brownout%s in its last %d uses", brown, brown == 1 ? "" : "s", seen);
        } else if (last && KNOWN(last->v_min) && last->v_min < 7.5f) {
            p[np].w = -4;
            snprintf(p[np++].text, sizeof p[0].text, "sagged to %.1f v last time", last->v_min);
        }
        if (b->status == CB_WATCH) {
            p[np].w = -15;
            snprintf(p[np++].text, sizeof p[0].text, "on watch");
        }
        int c = 0;
        for (int u = 0; u < b->nuse; u++) c += b->use[u].t > now - 86400;
        float bal = -5.0f * (c - mean24);
        if (np < 8 && (c == 0 && mean24 >= 1)) {
            p[np].w = bal;
            snprintf(p[np++].text, sizeof p[0].text, "not used today");
        } else if (np < 8 && c >= mean24 + 2) {
            p[np].w = bal;
            snprintf(p[np++].text, sizeof p[0].text, "used %d times today", c);
        } else {
            rk->score += bal; /* a small nudge, not worth words */
        }
        for (int j = 0; j < np; j++) rk->score += p[j].w;
        qsort(p, (size_t)np, sizeof p[0], by_weight);
        size_t o = 0;
        for (int j = 0, said = 0; j < np && said < 4; j++) {
            if (!p[j].text[0]) continue;
            int w = snprintf(rk->reason + o, sizeof rk->reason - o, "%s%s", said ? ", " : "", p[j].text);
            if (w < 0 || o + (size_t)w >= sizeof rk->reason) break;
            o += (size_t)w;
            said++;
        }
    }
    qsort(out, (size_t)k, sizeof out[0], by_rank);
    return k;
}

/* ------------------------------------------------------------------ a use's numbers */

#define WIN_S 8.0          /* a window of the fit: the open-circuit voltage drifts little inside one */
#define WIN_MIN_N 15
#define WIN_SPREAD_A 15.0f /* the load must vary this much in a window, or V and I say nothing about R */
#define LOAD_A 8.0f        /* more than this: the robot is working, no longer at rest */
#define REST_ONLY_S 5.0    /* with nothing to say when the load began: the first seconds are the rest */

void cat_batt_acc_init(cat_batt_acc_t *a)
{
    memset(a, 0, sizeof *a);
    a->v_min = NAN;
    a->peak_a = NAN;
    a->last_ti = -1;
    a->w_imin = INFINITY;
    a->w_imax = -INFINITY;
}

static float win_slope(const cat_batt_acc_t *a)
{
    if (a->w_n < WIN_MIN_N || a->w_imax - a->w_imin < WIN_SPREAD_A) return NAN;
    double den = a->w_n * a->w_ii - a->w_i * a->w_i;
    if (den <= 0) return NAN;
    double r = -(a->w_n * a->w_iv - a->w_i * a->w_v) / den; /* V = V0 - I·R */
    return r >= 0.003 && r <= 0.15 ? (float)r : NAN;
}

static void win_reset(cat_batt_acc_t *a, double t)
{
    a->w_t0 = t;
    a->w_n = a->w_i = a->w_v = a->w_ii = a->w_iv = 0;
    a->w_imin = INFINITY;
    a->w_imax = -INFINITY;
}

void cat_batt_acc_add(cat_batt_acc_t *a, double t, float v, float amps, int enabled)
{
    if (!(v >= 3 && v <= 16)) return;
    bool hi = amps == amps && amps > -5 && amps < 1000;
    if (!hi) amps = NAN;
    if (!a->any) {
        a->any = true;
        a->t0 = t;
        a->v_min = v;
        win_reset(a, t);
    }
    a->t_last = t;
    if (v < a->v_min) a->v_min = v;
    bool below = v < 6.8f;
    if (below && !a->below) a->dips++;
    a->below = below;
    if (!a->loaded && (enabled == 1 || (hi && amps > LOAD_A))) {
        a->loaded = true;
        a->t_first_load = t;
    }
    /* at rest before the load: all of it when the load's start is known, else only the first seconds */
    bool knows = enabled >= 0 || hi;
    if (!a->loaded && (knows || t - a->t0 <= REST_ONLY_S)) {
        a->rest_sum += v;
        a->rest_n++;
    }
    if (!hi) return;
    a->have_i = true;
    if (!KNOWN(a->peak_a) || amps > a->peak_a) a->peak_a = amps;
    if (a->last_ti >= 0) {
        double dt = t - a->last_ti;
        if (dt > 0 && dt < 0.25) {
            a->wh += v * amps * dt / 3600.0;
            if (a->loaded) {
                a->amp_s += amps * dt;
                a->load_s += dt;
            }
        }
    }
    a->last_ti = t;
    if (t - a->w_t0 >= WIN_S) {
        float r = win_slope(a);
        if (KNOWN(r)) a->win[a->nwin++ % CAT_BATT_WIN_MAX] = r;
        win_reset(a, t);
    }
    a->w_n++;
    a->w_i += amps;
    a->w_v += v;
    a->w_ii += (double)amps * amps;
    a->w_iv += (double)amps * v;
    if (amps < a->w_imin) a->w_imin = amps;
    if (amps > a->w_imax) a->w_imax = amps;
}

void cat_batt_acc_brown(cat_batt_acc_t *a, bool on)
{
    a->brown_seen = true;
    if (on && !a->brown_last) a->brown_flags++;
    a->brown_last = on;
}

bool cat_batt_acc_finish(const cat_batt_acc_t *a, cat_batt_use_t *u)
{
    if (!a->any) return false;
    u->v_min = a->v_min;
    u->v_rest = a->rest_n >= 3 ? (float)(a->rest_sum / a->rest_n) : NAN;
    u->dur_s = (float)(a->t_last - a->t0);
    u->brownouts = (int16_t)(a->brown_seen ? a->brown_flags : a->dips);
    if (a->have_i) {
        u->wh = (float)a->wh;
        u->peak_a = a->peak_a;
        u->amps = a->load_s > 1 ? (float)(a->amp_s / a->load_s) : NAN;
        float w[CAT_BATT_WIN_MAX + 1];
        int n = a->nwin < CAT_BATT_WIN_MAX ? a->nwin : CAT_BATT_WIN_MAX;
        memcpy(w, a->win, sizeof w[0] * (size_t)n);
        float last = win_slope(a); /* the window still open */
        if (KNOWN(last)) w[n++] = last;
        u->r_mohm = n >= 2 ? median(w, n) * 1000 : NAN;
    }
    return true;
}

/* ------------------------------------------------------------------ a log's numbers */

static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

static bool ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* WPILib's DataLogManager names: FRC_20260315_183422[_CASJ_Q34].wpilog, in UTC; the event and match once the
 * FMS was seen (Q qualification, E elimination, P practice) */
static void name_facts(const char *path, cat_batt_log_t *o)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int y, mo, d, h, mi, s, n = 0;
    if (sscanf(base, "FRC_%4d%2d%2d_%2d%2d%2d%n", &y, &mo, &d, &h, &mi, &s, &n) == 6 && n > 0 && y > 2000) {
        if (!o->m.t) o->m.t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
        const char *p = base + n;
        char ev[16], mt;
        int num;
        if (sscanf(p, "_%15[A-Za-z0-9]_%c%d", ev, &mt, &num) == 3) {
            if (!o->event[0]) snprintf(o->event, sizeof o->event, "%s", ev);
            if (o->match_type <= 0) {
                o->match_type = mt == 'Q' ? 2 : mt == 'E' ? 3 : mt == 'P' ? 1 : 0;
                o->match_number = num;
            }
        }
    }
}

enum { E_NONE, E_V, E_I, E_BROWN, E_EN, E_SYS, E_MNUM, E_MTYPE, E_EVENT };

typedef struct {
    uint32_t id;
    uint8_t kind, prio;
    uint8_t type; /* 0 double, 1 float, 2 int64, 3 boolean, 4 string */
} bent_t;

static int v_prio(const char *n)
{
    if (ends(n, "Brownout/MeasuredVoltage") || ends(n, "Status/BatteryVolts") || ends(n, "Systemcore/BatteryVolts")) return 3;
    if (ends(n, "BatteryVoltage")) return 2; /* SystemStats, a PDH's */
    if (ends(n, "/Battery") || ends(n, "BatteryVolts")) return 1; /* the Driver Station's */
    return 0;
}

static float num_of(const bent_t *e, const uint8_t *pl, uint32_t size)
{
    uint64_t v;
    if (e->type == 0 && size == 8) {
        v = le(pl, 8);
        double d;
        memcpy(&d, &v, 8);
        return (float)d;
    }
    if (e->type == 1 && size == 4) {
        uint32_t w = (uint32_t)le(pl, 4);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }
    if (e->type == 2 && size == 8) return (float)(int64_t)le(pl, 8);
    return NAN;
}

static bool wpilog(const uint8_t *d, size_t len, cat_batt_log_t *o)
{
    if (len < 12 || memcmp(d, "WPILOG", 6)) {
        snprintf(o->error, sizeof o->error, "not a WPILOG file");
        return false;
    }
    size_t start = 12 + (size_t)le(d + 8, 4);
    if (start > len) {
        snprintf(o->error, sizeof o->error, "truncated header");
        return false;
    }
    bent_t *ents = NULL;
    int nent = 0, cap = 0;
    uint32_t v_id = UINT32_MAX, i_id = UINT32_MAX, b_id = UINT32_MAX;
    int v_best = 0, i_best = 0, b_best = 0;
    double t0 = -1;
    cat_batt_acc_t *acc = malloc(sizeof *acc);
    if (!acc) {
        snprintf(o->error, sizeof o->error, "out of memory");
        return false;
    }
    cat_batt_acc_init(acc);
    int enabled = -1;
    float cur_i = NAN;
    double t_i = -10;
    bool have_sys = false;
    for (int pass = 0; pass < 2; pass++) {
        size_t p = start;
        while (p < len) {
            uint8_t bf = d[p];
            int idl = (bf & 3) + 1, szl = ((bf >> 2) & 3) + 1, tsl = ((bf >> 4) & 7) + 1;
            if (p + 1 + idl + szl + tsl > len) break;
            uint32_t id = (uint32_t)le(d + p + 1, idl);
            uint32_t size = (uint32_t)le(d + p + 1 + idl, szl);
            double ts = (double)le(d + p + 1 + idl + szl, tsl) / 1e6;
            const uint8_t *pl = d + p + 1 + idl + szl + tsl;
            p += 1 + (size_t)idl + szl + tsl + size;
            if (p > len) break;
            if (id == 0) {
                if (pass == 1 || size < 5 || pl[0] != 0) continue;
                uint32_t eid = (uint32_t)le(pl + 1, 4), nl = (uint32_t)le(pl + 5, 4);
                if (9 + nl + 4 > size) continue;
                uint32_t tl = (uint32_t)le(pl + 9 + nl, 4);
                if (13 + nl + tl > size) continue;
                char name[128], type[16];
                snprintf(name, sizeof name, "%.*s", (int)(nl < 127 ? nl : 127), (const char *)pl + 9);
                snprintf(type, sizeof type, "%.*s", (int)(tl < 15 ? tl : 15), (const char *)pl + 13 + nl);
                bent_t e = { .id = eid };
                e.type = !strcmp(type, "double") ? 0 : !strcmp(type, "float") ? 1 : !strcmp(type, "int64") ? 2
                       : !strcmp(type, "boolean") ? 3 : !strcmp(type, "string") ? 4 : 9;
                bool num = e.type <= 2;
                int vp = num ? v_prio(name) : 0;
                if (vp) {
                    e.kind = E_V;
                    if (vp > v_best) v_best = vp, v_id = eid;
                } else if (num && ends(name, "TotalCurrent")) {
                    e.kind = E_I;
                    int ip = ends(name, "Brownout/TotalCurrent") ? 2 : 1;
                    if (ip > i_best) i_best = ip, i_id = eid;
                } else if (e.type == 3 && (ends(name, "BrownedOut") || ends(name, "Brownout"))) {
                    e.kind = E_BROWN;
                    int bp = ends(name, "Systemcore/BrownedOut") ? 2 : 1;
                    if (bp > b_best) b_best = bp, b_id = eid;
                } else if (e.type == 3 && ((!strncmp(name, "DS:", 3) && ends(name, "enabled")) || ends(name, "DriverStation/Enabled")))
                    e.kind = E_EN;
                else if (e.type == 2 && !strcmp(name, "systemTime")) e.kind = E_SYS;
                else if (num && ends(name, "FMSInfo/MatchNumber")) e.kind = E_MNUM;
                else if (num && ends(name, "FMSInfo/MatchType")) e.kind = E_MTYPE;
                else if (e.type == 4 && ends(name, "FMSInfo/EventName")) e.kind = E_EVENT;
                if (!e.kind) continue;
                if (nent == cap) {
                    cap = cap ? cap * 2 : 16;
                    bent_t *n = realloc(ents, sizeof *ents * (size_t)cap);
                    if (!n) break;
                    ents = n;
                }
                ents[nent++] = e;
                continue;
            }
            if (pass == 0) {
                if (t0 < 0 || ts < t0) t0 = ts;
                continue;
            }
            const bent_t *e = NULL;
            for (int i = 0; i < nent; i++)
                if (ents[i].id == id) {
                    e = &ents[i];
                    break;
                }
            if (!e) continue;
            double t = ts - t0;
            switch (e->kind) {
            case E_V:
                if (id == v_id) cat_batt_acc_add(acc, t, num_of(e, pl, size), fabs(t - t_i) <= 0.1 ? cur_i : NAN, enabled);
                break;
            case E_I:
                if (id == i_id) {
                    cur_i = num_of(e, pl, size);
                    t_i = t;
                }
                break;
            case E_BROWN:
                if (id == b_id && size >= 1) cat_batt_acc_brown(acc, pl[0] != 0);
                break;
            case E_EN:
                if (size >= 1) enabled = pl[0] != 0;
                break;
            case E_SYS:
                if (!have_sys && size == 8) {
                    int64_t us = (int64_t)le(pl, 8);
                    if (us > 1600000000LL * 1000000) {
                        o->m.t = us / 1000000 - (int64_t)t;
                        have_sys = true;
                    }
                }
                break;
            case E_MNUM: {
                float v = num_of(e, pl, size);
                if (v >= 1) o->match_number = (int)v;
                break;
            }
            case E_MTYPE: {
                float v = num_of(e, pl, size);
                if (v >= 1 && v <= 3) o->match_type = (int)v;
                break;
            }
            case E_EVENT:
                if (size > 0 && size < sizeof o->event) snprintf(o->event, sizeof o->event, "%.*s", (int)size, (const char *)pl);
                break;
            }
        }
    }
    free(ents);
    o->have_current = acc->have_i;
    bool ok = cat_batt_acc_finish(acc, &o->m);
    free(acc);
    if (!ok) snprintf(o->error, sizeof o->error, v_best ? "no battery samples" : "no battery voltage in this log");
    return ok;
}

/* the NI dslog's record stride, as cat_logs.c picks it: the one whose battery reads like a battery */
static double plausibility(const uint8_t *d, size_t len, size_t stride)
{
    int good = 0, total = 0;
    double last = -1;
    for (size_t p = 20; p + stride <= len && total < 300; p += stride, total++) {
        double v = d[p + 2] + d[p + 3] / 256.0;
        if (v >= 4 && v <= 16 && (last < 0 || fabs(v - last) < 1.5)) good++;
        last = v;
    }
    return total ? (double)good / total : 0;
}

static bool dslog(const uint8_t *d, size_t len, cat_batt_log_t *o)
{
    if (len < 20 || (be(d, 4) != 3 && be(d, 4) != 4)) {
        snprintf(o->error, sizeof o->error, "not a dslog this reads");
        return false;
    }
    o->m.t = (int64_t)be(d + 4, 8) - LABVIEW_EPOCH;
    static const size_t strides[] = { 35, 10 + 4 + 33, 10 + 4, 10 };
    size_t stride = 35;
    double best = -1;
    for (size_t i = 0; i < sizeof strides / sizeof strides[0]; i++) {
        double q = plausibility(d, len, strides[i]);
        if (q > best + 0.02) best = q, stride = strides[i];
    }
    cat_batt_acc_t *acc = malloc(sizeof *acc);
    if (!acc) {
        snprintf(o->error, sizeof o->error, "out of memory");
        return false;
    }
    cat_batt_acc_init(acc);
    /* the status byte is inverted: bit 7 brownout, bit 0 the robot disabled (AdvantageScope's reading). A log
     * whose enabled bit never moves is read as not saying. */
    bool moved = false, first = true, en0 = false;
    for (size_t p = 20; p + stride <= len; p += stride) {
        bool en = (d[p + 5] & 0x01) != 0;
        if (first) en0 = en, first = false;
        else if (en != en0) {
            moved = true;
            break;
        }
    }
    uint32_t i = 0;
    for (size_t p = 20; p + stride <= len; p += stride, i++) {
        const uint8_t *r = d + p;
        cat_batt_acc_brown(acc, !(r[5] & 0x80));
        cat_batt_acc_add(acc, i * 0.02, r[2] + r[3] / 256.0f, NAN, moved ? (r[5] & 0x01) != 0 : -1);
    }
    bool ok = cat_batt_acc_finish(acc, &o->m);
    free(acc);
    if (!ok) snprintf(o->error, sizeof o->error, "no battery samples");
    return ok;
}

bool cat_batt_log_parse(const char *name, const uint8_t *d, size_t len, cat_batt_log_t *o)
{
    memset(o, 0, sizeof *o);
    cat_batt_use_init(&o->m);
    o->match_type = -1;
    bool ok;
    if (len >= 6 && !memcmp(d, "WPILOG", 6)) ok = wpilog(d, len, o);
    else if (ends(name, ".dslog")) ok = dslog(d, len, o);
    else {
        snprintf(o->error, sizeof o->error, "not a log with a battery in it");
        return false;
    }
    name_facts(name, o);
    return ok;
}

bool cat_batt_log(const char *path, cat_batt_log_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "can't open");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64L * 1024 * 1024) {
        fclose(f);
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, n <= 0 ? "empty" : "larger than 64 MB");
        return false;
    }
    uint8_t *buf = malloc((size_t)n); /* PSRAM on the tablet */
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) ok = cat_batt_log_parse(path, buf, (size_t)n, out);
    else {
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "read failed");
    }
    free(buf);
    return ok;
}

uint32_t cat_batt_seen_hash(const char *name, long size, int64_t mtime)
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)((uint64_t)size >> (8 * i))) * 16777619u;
    for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)((uint64_t)mtime >> (8 * i))) * 16777619u;
    return h ? h : 1;
}

bool cat_fleet_seen(const cat_fleet_t *f, uint32_t h)
{
    for (int i = 0; i < f->nseen; i++)
        if (f->seen[i] == h) return true;
    return false;
}

void cat_fleet_mark_seen(cat_fleet_t *f, uint32_t h)
{
    if (cat_fleet_seen(f, h)) return;
    if (f->nseen >= CAT_BATT_SEEN) {
        memmove(&f->seen[0], &f->seen[1], sizeof f->seen[0] * (CAT_BATT_SEEN - 1));
        f->nseen = CAT_BATT_SEEN - 1;
    }
    f->seen[f->nseen++] = h;
}

#define ATTR_BEFORE_S (4 * 3600) /* a pick this long before a log's start still owns it */
#define ATTR_AFTER_S 600         /* the robot was on before the battery was recorded */

cat_batt_use_t *cat_fleet_attribute(cat_fleet_t *f, const cat_batt_log_t *l, int *bi)
{
    cat_batt_use_t *best = NULL;
    /* its qualification match, by number, when both say */
    if (l->match_type == 2 && l->match_number > 0) {
        char want[12];
        snprintf(want, sizeof want, "Q%d", l->match_number);
        for (int i = 0; i < f->n; i++)
            for (int k = 0; k < f->b[i].nuse; k++) {
                cat_batt_use_t *u = &f->b[i].use[k];
                if (!(u->src & CU_PICK) || strcmp(u->label, want)) continue;
                if (l->m.t && llabs((long long)(u->t - l->m.t)) > 12 * 3600) continue;
                if (!best || u->t > best->t) best = u, *bi = i;
            }
        if (best) return best;
    }
    if (!l->m.t) return NULL;
    /* the newest pick before it started; else the first one just after (recorded late) */
    cat_batt_use_t *after = NULL;
    int after_i = -1;
    for (int i = 0; i < f->n; i++)
        for (int k = 0; k < f->b[i].nuse; k++) {
            cat_batt_use_t *u = &f->b[i].use[k];
            if (!(u->src & CU_PICK)) continue;
            if (u->t <= l->m.t && l->m.t - u->t <= ATTR_BEFORE_S) {
                if (!best || u->t > best->t) best = u, *bi = i;
            } else if (u->t > l->m.t && u->t - l->m.t <= ATTR_AFTER_S) {
                if (!after || u->t < after->t) after = u, after_i = i;
            }
        }
    if (best) return best;
    if (after) *bi = after_i;
    return after;
}

void cat_batt_merge(cat_batt_use_t *u, const cat_batt_log_t *l, const char *log_name)
{
    const cat_batt_use_t *n = &l->m;
    bool had_log = u->src & CU_LOG;
    /* the voltage: a log over the live 10 Hz; two logs of one match, the lowest and the most brownouts */
    if (KNOWN(n->v_min)) u->v_min = had_log && KNOWN(u->v_min) ? fminf(u->v_min, n->v_min) : n->v_min;
    if (KNOWN(n->v_rest) && (!had_log || !KNOWN(u->v_rest))) u->v_rest = n->v_rest;
    if (n->brownouts >= 0) u->brownouts = had_log && u->brownouts > n->brownouts ? u->brownouts : n->brownouts;
    /* the current's numbers: from the log that has them (the longer of two), over live */
    bool had_log_i = had_log && u->log[0] && KNOWN(u->wh);
    if (l->have_current && (!had_log_i || !KNOWN(u->dur_s) || n->dur_s > u->dur_s)) {
        u->r_mohm = n->r_mohm;
        u->wh = n->wh;
        u->amps = n->amps;
        u->peak_a = n->peak_a;
    }
    if (KNOWN(n->dur_s) && (!KNOWN(u->dur_s) || !had_log || n->dur_s > u->dur_s)) u->dur_s = n->dur_s;
    if (log_name && (l->have_current || !u->log[0])) snprintf(u->log, sizeof u->log, "%s", log_name);
    if (!u->t) u->t = n->t;
    u->src |= CU_LOG;
}

/* ------------------------------------------------------------------ JSON */

typedef struct {
    char *p;
    size_t n, cap;
    bool oom;
} jb_t;

static void jb_put(jb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_put(jb_t *b, const char *fmt, ...)
{
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        size_t cap = b->cap * 2 + (size_t)k;
        char *np = realloc(b->p, cap);
        if (!np) {
            b->oom = true;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
}

static void jb_str(jb_t *b, const char *s)
{
    jb_put(b, "\"");
    for (; *s && !b->oom; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') jb_put(b, "\\%c", c);
        else if (c == '\n') jb_put(b, "\\n");
        else if (c < 32) jb_put(b, "\\u%04x", c);
        else jb_put(b, "%c", c);
    }
    jb_put(b, "\"");
}

static void jb_num(jb_t *b, const char *key, float v, const char *fmt)
{
    if (!KNOWN(v)) return;
    jb_put(b, ",\"%s\":", key);
    jb_put(b, fmt, (double)v);
}

char *cat_batt_to_json(const cat_fleet_t *f, size_t *len)
{
    jb_t b = { .cap = 16384 };
    b.p = malloc(b.cap);
    if (!b.p) return NULL;
    b.p[0] = 0;
    jb_put(&b, "{\"version\":1,\"updated\":%lld,\"next_uid\":%d,\n\"seen\":[", (long long)f->updated, f->next_uid);
    for (int i = 0; i < f->nseen; i++) jb_put(&b, "%s%u", i ? "," : "", (unsigned)f->seen[i]);
    jb_put(&b, "],\n\"batteries\":[");
    for (int i = 0; i < f->n; i++) {
        const cat_batt_t *t = &f->b[i];
        jb_put(&b, "%s\n{\"uid\":%d,\"label\":", i ? "," : "", t->uid);
        jb_str(&b, t->label);
        jb_put(&b, ",\"status\":\"%s\"", CAT_BATT_STATUS[t->status < CB_NSTATUS ? t->status : 0]);
        if (t->auto_watch) jb_put(&b, ",\"auto_watch\":true");
        if (t->year) jb_put(&b, ",\"year\":%d", t->year);
        jb_put(&b, ",\"notes\":");
        jb_str(&b, t->notes);
        if (t->charged) jb_put(&b, ",\"charged\":%lld", (long long)t->charged);
        jb_num(&b, "base_mohm", t->r_base, "%.1f");
        jb_put(&b, ",\"uses_total\":%d,\"uses\":[", t->uses_total);
        for (int k = 0; k < t->nuse; k++) {
            const cat_batt_use_t *u = &t->use[k];
            jb_put(&b, "%s\n {\"t\":%lld", k ? "," : "", (long long)u->t);
            if (u->match[0]) {
                jb_put(&b, ",\"match\":");
                jb_str(&b, u->match);
            }
            if (u->label[0]) {
                jb_put(&b, ",\"label\":");
                jb_str(&b, u->label);
            }
            if (u->charge) jb_put(&b, ",\"charge\":\"%s\"", CAT_BATT_CHARGE[u->charge < CC_NCHARGE ? u->charge : 0]);
            if (u->charged) jb_put(&b, ",\"charged\":%lld", (long long)u->charged);
            jb_put(&b, ",\"src\":\"%s%s%s\"", u->src & CU_PICK ? "p" : "", u->src & CU_LOG ? "l" : "", u->src & CU_LIVE ? "n" : "");
            jb_num(&b, "v_rest", u->v_rest, "%.2f");
            jb_num(&b, "v_min", u->v_min, "%.2f");
            jb_num(&b, "mohm", u->r_mohm, "%.1f");
            jb_num(&b, "wh", u->wh, "%.2f");
            jb_num(&b, "amps", u->amps, "%.1f");
            jb_num(&b, "peak_a", u->peak_a, "%.0f");
            jb_num(&b, "dur_s", u->dur_s, "%.0f");
            if (u->brownouts >= 0) jb_put(&b, ",\"brownouts\":%d", u->brownouts);
            if (u->log[0]) {
                jb_put(&b, ",\"log\":");
                jb_str(&b, u->log);
            }
            jb_put(&b, "}");
        }
        jb_put(&b, "]}");
    }
    jb_put(&b, "\n]}\n");
    if (b.oom) {
        free(b.p);
        return NULL;
    }
    if (len) *len = b.n;
    return b.p;
}

static int status_of(const char *s)
{
    for (int i = 0; i < CB_NSTATUS; i++)
        if (!strcmp(s, CAT_BATT_STATUS[i])) return i;
    return CB_GOOD;
}

bool cat_batt_from_json(cat_fleet_t *f, const char *js, size_t len)
{
    /* every token but the first follows one of , : [ { */
    int max = 2;
    for (size_t i = 0; i < len; i++) max += js[i] == ',' || js[i] == ':' || js[i] == '[' || js[i] == '{';
    jl_tok_t *t = malloc(sizeof *t * (size_t)max);
    if (!t) return false;
    int n = jl_parse(js, len, t, max);
    if (n <= 0 || t[0].type != JL_OBJ) {
        free(t);
        return false;
    }
    jl_doc_t d = { js, t, n };
    memset(f, 0, sizeof *f);
    f->updated = (int64_t)jl_num(&d, jl_get(&d, 0, "updated"), 0);
    f->next_uid = (int)jl_num(&d, jl_get(&d, 0, "next_uid"), 1);
    int seen = jl_get(&d, 0, "seen");
    for (int i = 0; seen >= 0 && f->nseen < CAT_BATT_SEEN; i++) {
        int e = jl_at(&d, seen, i);
        if (e < 0) break;
        f->seen[f->nseen++] = (uint32_t)jl_num(&d, e, 0);
    }
    int arr = jl_get(&d, 0, "batteries");
    int max_uid = 0;
    char s[128];
    for (int i = 0; arr >= 0 && f->n < CAT_BATT_MAX; i++) {
        int o = jl_at(&d, arr, i);
        if (o < 0) break;
        cat_batt_t *b = &f->b[f->n];
        batt_init(b, (int)jl_num(&d, jl_get(&d, o, "uid"), 0), "");
        if (b->uid <= 0) b->uid = 1000 + f->n;
        if (b->uid > max_uid) max_uid = b->uid;
        if (!jl_str(&d, jl_get(&d, o, "label"), b->label, sizeof b->label)) snprintf(b->label, sizeof b->label, "%d", f->n + 1);
        if (jl_str(&d, jl_get(&d, o, "status"), s, sizeof s)) b->status = (uint8_t)status_of(s);
        b->auto_watch = jl_bool(&d, jl_get(&d, o, "auto_watch"), false);
        b->year = (int)jl_num(&d, jl_get(&d, o, "year"), 0);
        jl_str(&d, jl_get(&d, o, "notes"), b->notes, sizeof b->notes);
        b->charged = (int64_t)jl_num(&d, jl_get(&d, o, "charged"), 0);
        b->r_base = (float)jl_num(&d, jl_get(&d, o, "base_mohm"), NAN);
        int uses = jl_get(&d, o, "uses");
        for (int k = 0; uses >= 0 && b->nuse < CAT_BATT_USES; k++) {
            int e = jl_at(&d, uses, k);
            if (e < 0) break;
            cat_batt_use_t *u = &b->use[b->nuse++];
            cat_batt_use_init(u);
            u->t = (int64_t)jl_num(&d, jl_get(&d, e, "t"), 0);
            jl_str(&d, jl_get(&d, e, "match"), u->match, sizeof u->match);
            jl_str(&d, jl_get(&d, e, "label"), u->label, sizeof u->label);
            if (jl_str(&d, jl_get(&d, e, "charge"), s, sizeof s))
                for (int c = 1; c < CC_NCHARGE; c++)
                    if (!strcmp(s, CAT_BATT_CHARGE[c])) u->charge = (uint8_t)c;
            u->charged = (int64_t)jl_num(&d, jl_get(&d, e, "charged"), 0);
            if (jl_str(&d, jl_get(&d, e, "src"), s, sizeof s))
                u->src = (uint8_t)((strchr(s, 'p') ? CU_PICK : 0) | (strchr(s, 'l') ? CU_LOG : 0) | (strchr(s, 'n') ? CU_LIVE : 0));
            u->v_rest = (float)jl_num(&d, jl_get(&d, e, "v_rest"), NAN);
            u->v_min = (float)jl_num(&d, jl_get(&d, e, "v_min"), NAN);
            u->r_mohm = (float)jl_num(&d, jl_get(&d, e, "mohm"), NAN);
            u->wh = (float)jl_num(&d, jl_get(&d, e, "wh"), NAN);
            u->amps = (float)jl_num(&d, jl_get(&d, e, "amps"), NAN);
            u->peak_a = (float)jl_num(&d, jl_get(&d, e, "peak_a"), NAN);
            u->dur_s = (float)jl_num(&d, jl_get(&d, e, "dur_s"), NAN);
            u->brownouts = (int16_t)jl_num(&d, jl_get(&d, e, "brownouts"), -1);
            jl_str(&d, jl_get(&d, e, "log"), u->log, sizeof u->log);
        }
        b->uses_total = (int)jl_num(&d, jl_get(&d, o, "uses_total"), b->nuse);
        if (b->uses_total < b->nuse) b->uses_total = b->nuse;
        f->n++;
    }
    if (f->next_uid <= max_uid) f->next_uid = max_uid + 1;
    free(t);
    return true;
}

/* ------------------------------------------------------------------ the summary for a language model */

typedef struct {
    char *p;
    size_t n, o;
} out_t;

static void outf(out_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(out_t *w, const char *fmt, ...)
{
    if (w->o + 1 >= w->n) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(w->p + w->o, w->n - w->o, fmt, ap);
    va_end(ap);
    if (k > 0) w->o += (size_t)k;
    if (w->o >= w->n) w->o = w->n - 1;
}

static void fnum(char *b, size_t n, float v, const char *fmt)
{
    if (KNOWN(v)) snprintf(b, n, fmt, (double)v);
    else snprintf(b, n, "-");
}

size_t cat_batt_summary(const cat_fleet_t *f, int64_t now, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    out_t w = { out, n, 0 };
    outf(&w, "%d batteries. Per battery: status, year bought, uses (all time / last 24 h), charge state, internal "
             "resistance now (median of the last 3 measured uses) and its baseline (first 3), notes; then its last "
             "uses, newest first: when, match, how charged when it went in, resting V, lowest V, resistance mOhm, "
             "brownouts, Wh drawn, mean A under load. '-' is not measured, never zero.\n",
         f->n);
    for (int i = 0; i < f->n; i++) {
        const cat_batt_t *b = &f->b[i];
        char nm[20], rn[16], rb[16], ch[64], t[24];
        int m = 0, c24 = 0;
        for (int u = 0; u < b->nuse; u++) c24 += b->use[u].t > now - 86400;
        fnum(rn, sizeof rn, cat_batt_r_now(b, &m), "%.1f");
        fnum(rb, sizeof rb, b->r_base, "%.1f");
        const cat_batt_use_t *last = cat_batt_last(b);
        if (b->charged && (!last || b->charged >= last->t)) {
            ago(now - b->charged, t, sizeof t);
            snprintf(ch, sizeof ch, "charged, off the charger %s ago", t);
        } else if (last) {
            ago(now - last->t, t, sizeof t);
            snprintf(ch, sizeof ch, "not marked charged since its last use %s ago", t);
        } else {
            snprintf(ch, sizeof ch, "charge not recorded");
        }
        char yr[12];
        if (b->year) snprintf(yr, sizeof yr, "%d", b->year);
        else snprintf(yr, sizeof yr, "unknown");
        outf(&w, "\nbattery %s: %s%s, bought %s, uses %d / %d, %s, R %s (baseline %s, %d measured)%s%s\n",
             cat_batt_name(b, nm, sizeof nm), CAT_BATT_STATUS[b->status < CB_NSTATUS ? b->status : 0],
             b->auto_watch ? " (by the tablet)" : "", yr, b->uses_total, c24, ch, rn, rb, m, b->notes[0] ? "; notes: " : "",
             b->notes);
        for (int u = b->nuse - 1, k = 0; u >= 0 && k < 6; u--, k++) {
            const cat_batt_use_t *x = &b->use[u];
            char v0[12], vm[12], r[12], wh[12], a[12];
            fnum(v0, sizeof v0, x->v_rest, "%.2f");
            fnum(vm, sizeof vm, x->v_min, "%.2f");
            fnum(r, sizeof r, x->r_mohm, "%.1f");
            fnum(wh, sizeof wh, x->wh, "%.1f");
            fnum(a, sizeof a, x->amps, "%.0f");
            ago(now - x->t, t, sizeof t);
            char br[8];
            if (x->brownouts >= 0) snprintf(br, sizeof br, "%d", x->brownouts);
            else snprintf(br, sizeof br, "-");
            outf(&w, "  %s ago, %s, %s, rest %s, low %s, R %s, brownouts %s, %s Wh, %s A\n", t,
                 x->label[0] ? x->label : "no match", x->charge ? CAT_BATT_CHARGE[x->charge] : "charge unknown", v0,
                 vm, r, br, wh, a);
        }
    }
    cat_batt_rank_t *rk = malloc(sizeof *rk * CAT_BATT_MAX);
    if (rk) {
        int k = cat_fleet_rank(f, now, rk, CAT_BATT_MAX);
        outf(&w, "\nthe tablet's own ranking for the next match:\n");
        for (int i = 0; i < k; i++) {
            char nm[20];
            outf(&w, "%d. %s: %s%s\n", i + 1, cat_batt_name(&f->b[rk[i].idx], nm, sizeof nm), rk[i].available ? "" : "out: ",
                 rk[i].reason);
        }
        free(rk);
    }
    return w.o;
}
