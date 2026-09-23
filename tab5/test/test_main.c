/* Tests for the portable pieces: Detent's springs and gesture math, msgpack, JSON, the FRC CAN id
 * decoder and the log parsers. Built by sim/CMakeLists.txt as catalyst_tab_tests. */
#include "bz_motion.h"
#include "cat_can.h"
#include "cat_logs.h"
#include "json_lite.h"
#include "mpack_lite.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
void test_assist(int *checks, int *fails); /* test_assist.c */
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

static void springs(void)
{
    /* role constants match motion.js (docs/bezel-port.md) */
    NEAR(BZ_HOLD.k, 1754.596, 0.05);
    NEAR(BZ_HOLD.c, 72.0472, 0.001);
    NEAR(BZ_RELEASE.k, 157.914, 0.01);
    NEAR(BZ_RELEASE.c, 21.3628, 0.001);
    NEAR(BZ_PAGE.k, 194.955, 0.01);
    NEAR(BZ_PAGE.c, 24.5742, 0.001);
    NEAR(BZ_SETTLE.c, 31.4159, 0.001);

    /* a release settles near its computed time (0.729 s at ε 0.001) and overshoots a little */
    bz_motion_clock(0);
    bz_motion_t m;
    bz_motion_init(&m, 0, 0.001f);
    bz_motion_to(&m, 1, BZ_RELEASE);
    double t = 0, settled = -1;
    float peak = 0;
    while (t < 2) {
        t += 1.0 / 240;
        bz_motion_clock(t);
        bool moving = bz_motion_tick(&m);
        if (m.value > peak) peak = m.value;
        if (!moving && settled < 0) settled = t;
    }
    CHECK(settled > 0.55 && settled < 0.85);
    CHECK(peak > 1.0f && peak < 1.08f);
    NEAR(m.value, 1, 1e-6);

    /* interruptible: retargeting mid-flight keeps position and velocity continuous */
    bz_motion_clock(0);
    bz_motion_init(&m, 0, 0.001f);
    bz_motion_to(&m, 100, BZ_SMOOTH);
    bz_motion_clock(0.1);
    bz_motion_tick(&m);
    float x = m.value, v = m.velocity;
    bz_motion_to(&m, -50, BZ_SMOOTH);
    bz_motion_clock(0.1 + 1e-4);
    bz_motion_tick(&m);
    NEAR(m.value, x + v * 1e-4, 0.05);

    /* calm critically damps: no overshoot at the same stiffness */
    bz_motion_set_calm(true);
    bz_motion_clock(0);
    bz_motion_init(&m, 0, 0.001f);
    bz_motion_to(&m, 1, BZ_WOBBLE);
    peak = 0;
    for (t = 0; t < 2; t += 1.0 / 240) {
        bz_motion_clock(t);
        bz_motion_tick(&m);
        if (m.value > peak) peak = m.value;
    }
    CHECK(peak <= 1.0001f);
    bz_motion_set_calm(false);

    /* gesture math */
    NEAR(bz_project(1000, 0.99f), 0.099 * 1000, 0.5);
    NEAR(bz_rubber_band(100, 400, 0.55f), 0.55 * 100 * 400 / (400 + 55.0), 1e-3);
    CHECK(bz_rubber_clamp(-50, 0, 1, 100) > -50 && bz_rubber_clamp(-50, 0, 1, 100) < 0);
    float targets[] = { 0, 100, 200 };
    NEAR(bz_snap_target(40, 1000, targets, 3), 100, 1e-6);
    NEAR(bz_snap_target(40, -100, targets, 3), 0, 1e-6);

    /* velocity: a finger moving 600 px/s */
    bz_velocity_t vt;
    bz_velocity_reset(&vt);
    for (int i = 0; i < 10; i++) bz_velocity_add(&vt, i * 0.008, i * 0.008f * 600);
    NEAR(bz_velocity_get(&vt, 9 * 0.008), 600, 1);
}

static void msgpack(void)
{
    uint8_t buf[128];
    mp_writer_t w;
    mp_writer_init(&w, buf, sizeof buf);
    mp_write_array(&w, 4);
    mp_write_int(&w, -1);
    mp_write_uint(&w, 1234567890123ull);
    mp_write_int(&w, 1);
    mp_write_f64(&w, 12.625);
    mp_write_str(&w, "hello", 5);
    mp_write_int(&w, -40000);
    CHECK(!w.overflow);
    mp_reader_t r;
    mp_reader_init(&r, buf, w.len);
    mp_item_t it;
    CHECK(mp_read(&r, &it) && it.kind == MP_ARRAY && it.count == 4);
    CHECK(mp_read(&r, &it) && mp_as_int(&it) == -1);
    CHECK(mp_read(&r, &it) && mp_as_int(&it) == 1234567890123ll);
    CHECK(mp_read(&r, &it) && mp_as_int(&it) == 1);
    CHECK(mp_read(&r, &it) && it.kind == MP_F64 && it.f64 == 12.625);
    CHECK(mp_read(&r, &it) && it.kind == MP_STR && it.bytes.n == 5 && !memcmp(it.bytes.p, "hello", 5));
    CHECK(mp_read(&r, &it) && mp_as_int(&it) == -40000);
    CHECK(mp_done(&r));
}

static void json(void)
{
    const char *s = "[{\"method\":\"announce\",\"params\":{\"name\":\"/Catalyst/Robot/Identity/Name\",\"id\":7,"
                    "\"type\":\"string\",\"properties\":{\"retained\":true}}},{\"method\":\"unannounce\","
                    "\"params\":{\"name\":\"/a\\\"b\",\"id\":8}}]";
    jl_tok_t t[64];
    int n = jl_parse(s, strlen(s), t, 64);
    CHECK(n > 0);
    jl_doc_t d = { s, t, n };
    int m0 = jl_at(&d, 0, 0), m1 = jl_at(&d, 0, 1);
    CHECK(jl_eq(&d, jl_get(&d, m0, "method"), "announce"));
    NEAR(jl_num(&d, jl_path(&d, m0, "params.id"), -1), 7, 0);
    char buf[64];
    jl_str(&d, jl_path(&d, m1, "params.name"), buf, sizeof buf);
    CHECK(!strcmp(buf, "/a\"b"));
    CHECK(jl_bool(&d, jl_path(&d, m0, "params.properties.retained"), false));
    CHECK(jl_parse("[1,2", 4, t, 64) < 0);
}

static void can(void)
{
    /* a Talon FX (motor controller 2, CTRE 4) device 13 */
    uint32_t id = 2u << 24 | 4u << 16 | 0x06u << 10 | 1u << 6 | 13u;
    cat_frc_id_t f = cat_can_decode(id);
    CHECK(f.type == 2 && f.mfr == 4 && f.api_class == 6 && f.api_index == 1 && f.device == 13);
    CHECK(!strcmp(cat_can_device_name(2, 4), "Talon FX"));
    CHECK(!strcmp(cat_can_device_name(8, 5), "PDH"));
    cat_can_bus_t b;
    cat_can_reset(&b, 1000000);
    for (int i = 0; i < 250; i++) cat_can_feed(&b, id, true, 8, i * 0.004);
    cat_can_tick(&b, 1.0);
    CHECK(b.n == 1);
    NEAR(b.nodes[0].rate_hz, 250, 5);
    NEAR(b.load, 250 * 158 / 1e6, 0.01);
    cat_can_tick(&b, 2.5);
    CHECK(b.nodes[0].quiet);
}

static size_t wp_record(uint8_t *p, uint32_t id, uint64_t ts, const void *payload, uint32_t n)
{
    /* 4-byte id, 4-byte size, 8-byte timestamp */
    p[0] = 3 | 3 << 2 | 7 << 4;
    memcpy(p + 1, &id, 4);
    memcpy(p + 5, &n, 4);
    memcpy(p + 9, &ts, 8);
    memcpy(p + 17, payload, n);
    return 17 + n;
}

static size_t wp_start(uint8_t *p, uint32_t eid, const char *name, const char *type)
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
    return wp_record(p, 0, 0, pl, (uint32_t)k);
}

static void logs(void)
{
    static uint8_t buf[65536];
    size_t n = 0;
    memcpy(buf, "WPILOG", 6);
    buf[6] = 0; buf[7] = 1;
    memset(buf + 8, 0, 4);
    n = 12;
    n += wp_start(buf + n, 1, "DS:/Dscomm/Status/Battery", "double");
    n += wp_start(buf + n, 2, "messages", "string");
    n += wp_start(buf + n, 3, "DS:/Dscomm/Status/PacketTime", "int64");
    for (int i = 0; i < 100; i++) {
        double v = 12.5 - (i == 50 ? 3.0 : 0.01 * i);
        n += wp_record(buf + n, 1, 1000000 + (uint64_t)i * 20000, &v, 8);
        int64_t us = 4000 + i * 10;
        n += wp_record(buf + n, 3, 1000000 + (uint64_t)i * 20000, &us, 8);
    }
    n += wp_record(buf + n, 2, 1500000, "Brownout detected", 17);
    cat_log_t log;
    CHECK(cat_log_parse("x.wpilog", buf, n, &log));
    CHECK(log.have[CAT_LOG_BATTERY]);
    NEAR(log.min_battery, 9.5, 1e-3);
    NEAR(log.duration_s, 1.98, 0.01);
    NEAR(log.max_trip_ms, 4.99, 0.01);
    CHECK(log.nevents == 1 && log.events[0].level == 2);

    /* a v4 dslog, CTRE-sized 35-byte records */
    memset(buf, 0, sizeof buf);
    buf[3] = 4;
    n = 20;
    for (int i = 0; i < 200; i++) {
        uint8_t *r = buf + n;
        r[0] = 10;          /* 5 ms trip */
        r[1] = 1;           /* 4 % loss */
        r[2] = 12; r[3] = 128; /* 12.5 V */
        r[4] = 60;          /* 30 % cpu */
        r[5] = i == 100 ? 0x7f : 0xff; /* one brownout (inverted bit 7) */
        r[6] = 80;          /* 40 % CAN */
        n += 35;
    }
    CHECK(cat_log_parse("x.dslog", buf, n, &log));
    NEAR(log.min_battery, 12.5, 1e-3);
    NEAR(log.max_trip_ms, 5, 1e-3);
    NEAR(log.max_can, 0.40, 1e-3);
    CHECK(log.brownouts == 1);
    NEAR(log.duration_s, 4.0, 1e-6);
}

int main(void)
{
    springs();
    msgpack();
    json();
    can();
    logs();
    test_assist(&checks, &fails);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
