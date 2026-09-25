/* Tests for the battery fleet's pure half (cat_batt.h): the resistance fit, a robot log's battery numbers, the
 * JSON on the card, the ranking, attributing a log to its pick, and the watch rule. Called from test_main.c. */
#include "cat_batt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

#define R_TRUE 0.018 /* ohms */

/* the robot's load: rest, then a match of varied current */
static double load(double t) { return t < 5 ? 1.5 : 60 + 55 * sin(t * 1.7) + 25 * sin(t * 0.37); }
static double volts(double t, double i) { return 12.85 - 0.002 * t - i * R_TRUE; } /* V0 sags slowly over the match */

static void acc(void)
{
    cat_batt_acc_t *a = malloc(sizeof *a);
    cat_batt_acc_init(a);
    double wh = 0;
    for (int k = 0; k < 155 * 50; k++) {
        double t = k * 0.02, i = load(t), v = volts(t, i);
        cat_batt_acc_add(a, t, (float)v, (float)i, t >= 5);
        if (k) wh += v * i * 0.02 / 3600;
    }
    cat_batt_use_t u;
    cat_batt_use_init(&u);
    CHECK(cat_batt_acc_finish(a, &u));
    NEAR(u.r_mohm, R_TRUE * 1000, 0.6);
    NEAR(u.v_rest, 12.85 - 1.5 * R_TRUE, 0.02);
    NEAR(u.wh, wh, wh * 0.01);
    CHECK(u.brownouts == 0);
    CHECK(u.v_min < 11.0f);
    NEAR(u.dur_s, 155, 0.1);
    free(a);

    /* steady current says nothing about R */
    a = malloc(sizeof *a);
    cat_batt_acc_init(a);
    for (int k = 0; k < 60 * 50; k++) cat_batt_acc_add(a, k * 0.02, 12.0f, 40.0f, 1);
    cat_batt_use_init(&u);
    cat_batt_acc_finish(a, &u);
    CHECK(!(u.r_mohm == u.r_mohm));
    free(a);
}

/* ---- a WPILOG in memory ---- */

static size_t rec(uint8_t *p, uint32_t id, uint64_t ts, const void *pl, uint32_t n)
{
    p[0] = (uint8_t)(3 | 3 << 2 | 7 << 4); /* 4-byte id, 4-byte size, 8-byte time */
    memcpy(p + 1, &id, 4);
    memcpy(p + 5, &n, 4);
    memcpy(p + 9, &ts, 8);
    memcpy(p + 17, pl, n);
    return 17 + n;
}

static size_t start(uint8_t *p, uint32_t eid, const char *name, const char *type)
{
    uint8_t pl[256];
    uint32_t nl = (uint32_t)strlen(name), tl = (uint32_t)strlen(type), ml = 0;
    size_t k = 0;
    pl[k++] = 0;
    memcpy(pl + k, &eid, 4); k += 4;
    memcpy(pl + k, &nl, 4); k += 4;
    memcpy(pl + k, name, nl); k += nl;
    memcpy(pl + k, &tl, 4); k += 4;
    memcpy(pl + k, type, tl); k += tl;
    memcpy(pl + k, &ml, 4); k += 4;
    return rec(p, 0, 0, pl, (uint32_t)k);
}

static void wpilog(void)
{
    size_t cap = 2 << 20, n = 12;
    uint8_t *buf = malloc(cap);
    memcpy(buf, "WPILOG", 6);
    buf[6] = 0; buf[7] = 1;
    memset(buf + 8, 0, 4);
    n += start(buf + n, 1, "NT:/Catalyst/Brownout/MeasuredVoltage", "double");
    n += start(buf + n, 2, "NT:/Catalyst/Brownout/TotalCurrent", "double");
    n += start(buf + n, 3, "DS:enabled", "boolean");
    n += start(buf + n, 4, "systemTime", "int64");
    n += start(buf + n, 5, "DS:/Dscomm/Status/Battery", "double"); /* the DS's: a worse choice */
    n += start(buf + n, 6, "NT:/Catalyst/Systemcore/BrownedOut", "boolean");
    uint64_t base = 3000000;
    int64_t sys = 1773599662LL * 1000000; /* 2026-03-15 18:34:22 UTC */
    n += rec(buf + n, 4, base, &sys, 8);
    uint8_t f = 0;
    n += rec(buf + n, 6, base, &f, 1);
    for (int k = 0; k < 150 * 50; k++) {
        double t = k * 0.02, i = load(t), v = volts(t, i), junk = 7.0;
        uint64_t ts = base + (uint64_t)k * 20000;
        if (k == 250) {
            uint8_t en = 1;
            n += rec(buf + n, 3, ts, &en, 1);
        }
        if (k == 4000 || k == 4010) {
            uint8_t b = k == 4000;
            n += rec(buf + n, 6, ts, &b, 1);
        }
        n += rec(buf + n, 2, ts, &i, 8);
        n += rec(buf + n, 1, ts + 500, &v, 8);
        n += rec(buf + n, 5, ts + 700, &junk, 8);
    }
    cat_batt_log_t l;
    CHECK(cat_batt_log_parse("/sdcard/logs/FRC_20260315_183422_CASJ_Q34.wpilog", buf, n, &l));
    CHECK(l.have_current);
    NEAR(l.m.r_mohm, R_TRUE * 1000, 0.6);
    NEAR(l.m.v_rest, 12.85 - 1.5 * R_TRUE, 0.02);
    CHECK(l.m.brownouts == 1);
    CHECK(l.m.t == 1773599662LL);
    CHECK(l.match_type == 2 && l.match_number == 34);
    CHECK(!strcmp(l.event, "CASJ"));
    free(buf);
}

static void fleet(void)
{
    cat_fleet_t *f = calloc(1, sizeof *f), *g = calloc(1, sizeof *g);
    cat_fleet_default(f, 12);
    CHECK(f->n == 12 && !strcmp(f->b[11].label, "12"));
    int64_t now = 1773600000;
    /* #3: low resistance, charged an hour ago; #5: higher; #7: went in 30 min ago; #9: bad */
    static const float R3[3] = { 14, 15, 14 }, R5[3] = { 21, 22, 23 };
    for (int k = 0; k < 3; k++) {
        cat_batt_use_t *u = cat_batt_push_use(&f->b[2]);
        u->t = now - 86400 * 2 + k * 3600;
        u->src = CU_PICK | CU_LOG;
        u->r_mohm = R3[k];
        u->brownouts = 0;
        u = cat_batt_push_use(&f->b[4]);
        u->t = now - 86400 * 2 + k * 3600;
        u->src = CU_PICK | CU_LOG;
        u->r_mohm = R5[k];
    }
    f->b[2].charged = f->b[4].charged = now - 3600;
    cat_batt_use_t *u7 = cat_batt_push_use(&f->b[6]);
    u7->t = now - 1800;
    u7->src = CU_PICK;
    snprintf(u7->label, sizeof u7->label, "Q34");
    f->b[8].status = CB_BAD;
    snprintf(f->b[0].notes, sizeof f->b[0].notes, "dropped \"once\"\nlug replaced");
    cat_batt_update_base(&f->b[2]);
    NEAR(f->b[2].r_base, 14, 1e-4);

    cat_batt_rank_t rk[CAT_BATT_MAX];
    /* a fleet with nothing recorded: every score equal, the roster's order */
    cat_fleet_default(g, 12);
    int n = cat_fleet_rank(g, now, rk, CAT_BATT_MAX);
    CHECK(n == 12);
    for (int i = 0; i < n; i++) CHECK(rk[i].idx == i);

    n = cat_fleet_rank(f, now, rk, CAT_BATT_MAX);
    CHECK(n == 12);
    CHECK(rk[0].idx == 2);
    CHECK(strstr(rk[0].reason, "lowest resistance (14") != NULL);
    CHECK(strstr(rk[0].reason, "rested") != NULL);
    for (int i = 0; i < n; i++) {
        if (rk[i].idx == 6) CHECK(!rk[i].available && strstr(rk[i].reason, "not recharged"));
        if (rk[i].idx == 8) CHECK(!rk[i].available);
    }

    /* a log of Q34 finds #7's pick */
    cat_batt_log_t l;
    memset(&l, 0, sizeof l);
    cat_batt_use_init(&l.m);
    l.m.t = now - 600;
    l.match_type = 2;
    l.match_number = 34;
    l.have_current = true;
    l.m.r_mohm = 19;
    l.m.v_min = 8.2f;
    l.m.wh = 12;
    l.m.brownouts = 0;
    int bi = -1;
    cat_batt_use_t *u = cat_fleet_attribute(f, &l, &bi);
    CHECK(u == u7 && bi == 6);
    cat_batt_merge(u, &l, "FRC_x.wpilog");
    CHECK((u->src & CU_LOG) && u->r_mohm == 19 && !strcmp(u->log, "FRC_x.wpilog"));
    /* by time alone: the newest pick before it started */
    l.match_type = 0;
    l.m.t = now - 60;
    u = cat_fleet_attribute(f, &l, &bi);
    CHECK(u == u7);
    l.m.t = now - 10 * 3600;
    CHECK(cat_fleet_attribute(f, &l, &bi) == NULL);

    /* the card, and back */
    cat_fleet_mark_seen(f, 12345);
    size_t len = 0;
    char *js = cat_batt_to_json(f, &len);
    CHECK(js && len == strlen(js));
    CHECK(cat_batt_from_json(g, js, len));
    CHECK(g->n == 12 && g->nseen == 1 && g->seen[0] == 12345);
    CHECK(g->b[8].status == CB_BAD);
    CHECK(!strcmp(g->b[0].notes, f->b[0].notes));
    CHECK(g->b[2].nuse == 3 && g->b[2].use[1].r_mohm == 15);
    NEAR(g->b[2].r_base, 14, 1e-4);
    CHECK(g->b[6].use[0].src == (CU_PICK | CU_LOG) && !strcmp(g->b[6].use[0].label, "Q34"));
    CHECK(!(g->b[6].use[0].v_rest == g->b[6].use[0].v_rest)); /* not measured stays not measured */
    CHECK(g->next_uid == 13);
    free(js);

    /* the watch rule: over 25 mΩ on two uses, or up 30 % on its baseline */
    cat_batt_t *b = &f->b[4];
    char why[64];
    CHECK(!cat_batt_watch_due(b, why, sizeof why));
    for (int k = 0; k < 2; k++) {
        cat_batt_use_t *x = cat_batt_push_use(b);
        x->r_mohm = 27;
    }
    CHECK(cat_batt_watch_due(b, why, sizeof why));
    b = &f->b[2];
    cat_batt_use_t *x = cat_batt_push_use(b);
    x->r_mohm = 19.5f;
    x = cat_batt_push_use(b);
    x->r_mohm = 19.5f;
    CHECK(cat_batt_watch_due(b, why, sizeof why) && strstr(why, "up"));

    /* add fills the lowest free number; remove keeps the rest */
    cat_fleet_remove(f, 3);
    CHECK(f->n == 11);
    int i = cat_fleet_add(f);
    CHECK(i == 11 && !strcmp(f->b[i].label, "4"));
    free(f);
    free(g);
}

void test_batt(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    acc();
    wpilog();
    fleet();
}
