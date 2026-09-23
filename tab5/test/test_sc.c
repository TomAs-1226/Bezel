/* Tests for cat_sc's pure half: catalyst-agent's /api/system, the motor-history file and rows, the
 * controls manifest, state lanes and the recorder's CSV. The JSON here is shaped exactly as
 * catalyst_agent.py and its test_agent.py fixtures write it. Called from test_main.c. */
#include "cat_sc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

static const char SYSTEM[] =
    "{\"identity\": {\"hostname\": \"robot\", \"os\": \"Systemcore OS 2027.0.0-beta14\", \"osVersion\": \"2027.0.0\","
    " \"kernel\": \"6.12.77-rt\", \"model\": \"Raspberry Pi Compute Module 5 Rev 1.0\", \"uptimeSeconds\": 12345.67,"
    " \"agentVersion\": \"2.0.2\"},"
    " \"cpu\": {\"cores\": [{\"core\": 0, \"percent\": 50.0, \"mhz\": 2400}, {\"core\": 1, \"percent\": null, \"mhz\": null},"
    " {\"core\": 2, \"percent\": 97.5, \"mhz\": 2400}], \"loadAverage\": [2.1, 1.84, 1.52], \"model\": \"Cortex-A76\","
    " \"throttling\": null},"
    " \"thermal\": [{\"zone\": \"cpu-thermal\", \"celsius\": 76.5}],"
    " \"memory\": {\"totalBytes\": 8318418944, \"availableBytes\": 6032623616, \"usedBytes\": 2285795328,"
    " \"cachedBytes\": 4608000000, \"swapTotalBytes\": 0, \"swapFreeBytes\": 0},"
    " \"storage\": {\"mounts\": [{\"mount\": \"/\", \"device\": \"/dev/mmcblk0p2\", \"filesystem\": \"ext4\","
    " \"totalBytes\": 4096000, \"usedBytes\": 2457600, \"freeBytes\": 1638400}],"
    " \"directories\": [{\"path\": \"/home/systemcore\", \"bytes\": 9400000000}, {\"path\": \"/var/log\", \"bytes\": null}]},"
    " \"processes\": {\"count\": 1, \"topByCpu\": [{\"pid\": 42, \"name\": \"Robot Main\", \"cpuPercent\": 400.0,"
    " \"rssBytes\": 20480000}], \"topByMemory\": []},"
    " \"can\": [{\"name\": \"can_s2\", \"up\": true, \"state\": \"ERROR-WARNING\", \"bitrate\": 1000000, \"restarts\": 1,"
    " \"rxPackets\": 1842300, \"txPackets\": 921100, \"rxErrors\": 0, \"txErrors\": 97, \"rxDropped\": 0, \"txDropped\": 2}],"
    " \"network\": [{\"name\": \"usb0\", \"up\": true, \"mac\": \"02:5c:0e:00:00:02\", \"speedMbps\": -1,"
    " \"addresses\": [\"172.26.0.1/24\", \"169.254.3.1/16\"], \"wireless\": null},"
    " {\"name\": \"wlan0\", \"up\": false, \"mac\": null, \"speedMbps\": null, \"addresses\": [],"
    " \"wireless\": {\"linkQuality\": 58.0, \"signalDbm\": -61.0}}],"
    " \"robotProgram\": {\"unit\": \"robot.service\", \"state\": \"active\", \"subState\": \"running\", \"restarts\": 3,"
    " \"runningForSeconds\": 212.4, \"memoryBytes\": null, \"pid\": 1873, \"log\": [\"first \\\"line\\\"\", \"second\"]},"
    " \"cameras\": {\"available\": true, \"cameras\": [{\"name\": \"limelight-left\", \"host\": \"limelight-left\","
    " \"ip\": \"10.58.5.12\", \"type\": \"limelight4\", \"ntConnected\": true, \"fps\": 56.6, \"temperatureC\": 71.6,"
    " \"cpuPercent\": 75.0, \"ramPercent\": 63.2, \"pipelineType\": \"pipe_fiducial\", \"statusReachable\": true},"
    " {\"name\": null, \"host\": \"limelight\", \"ip\": \"10.58.5.13\", \"ntConnected\": false, \"fps\": null,"
    " \"temperatureC\": null, \"statusReachable\": false}], \"sampledAt\": 1.0},"
    " \"motorHistory\": {\"present\": true, \"devices\": [{\"kind\": \"motor\"}, {\"kind\": \"encoder\"}, {\"kind\": \"motor\"}]},"
    " \"sampledAt\": 1789000000.5}";

static void agent(void)
{
    static cat_agent_t a;
    CHECK(cat_agent_parse(SYSTEM, strlen(SYSTEM), &a));
    CHECK(a.valid);
    CHECK(!strcmp(a.os, "Systemcore OS 2027.0.0-beta14"));
    CHECK(!strcmp(a.kernel, "6.12.77-rt"));
    NEAR(a.uptime_s, 12345.67, 1e-6);
    CHECK(a.ncores == 3);
    NEAR(a.cores[0].percent, 50, 1e-9);
    CHECK(a.cores[1].percent != a.cores[1].percent); /* null is absent, not 0 */
    CHECK(a.cores[1].mhz != a.cores[1].mhz);
    CHECK(a.cores[2].core == 2);
    CHECK(a.have_load);
    NEAR(a.load[1], 1.84, 1e-9);
    CHECK(!a.have_throttle); /* no vcgencmd: absent is not all clear */
    CHECK(a.nzones == 1 && !strcmp(a.zones[0].zone, "cpu-thermal"));
    NEAR(a.mem_used, 2285795328.0, 0.5);
    CHECK(a.nmounts == 1);
    NEAR(a.mounts[0].used, 2457600, 0.5);
    CHECK(a.ndirs == 2 && a.dirs[1].bytes != a.dirs[1].bytes);
    CHECK(a.nprocs == 1 && !strcmp(a.procs[0].name, "Robot Main") && a.procs[0].pid == 42);
    CHECK(a.ncan == 1 && !strcmp(a.can[0].state, "ERROR-WARNING"));
    NEAR(a.can[0].tx_err, 97, 1e-9);
    CHECK(a.nnics == 2);
    CHECK(!strcmp(a.nics[0].addrs, "172.26.0.1/24, 169.254.3.1/16"));
    CHECK(a.nics[0].mbps != a.nics[0].mbps); /* the kernel's -1 */
    CHECK(!a.nics[0].wireless && a.nics[1].wireless);
    NEAR(a.nics[1].dbm, -61, 1e-9);
    CHECK(a.nics[1].mac[0] == 0);
    CHECK(a.have_program && a.pid == 1873);
    NEAR(a.restarts, 3, 1e-9);
    CHECK(a.memory != a.memory);
    CHECK(a.nlog == 2 && !strcmp(a.log[0], "first \"line\""));
    CHECK(a.cams_available && a.ncams == 2);
    CHECK(a.cams[0].nt && a.cams[0].reachable);
    NEAR(a.cams[0].temp, 71.6, 1e-9);
    CHECK(!strcmp(a.cams[1].name, "limelight")); /* no name of its own: the host */
    CHECK(!a.cams[1].reachable && a.cams[1].temp != a.cams[1].temp);
    CHECK(a.mh_devices == 3 && a.mh_motors == 2);

    /* not a snapshot */
    CHECK(!cat_agent_parse("{\"error\": \"not found\"}", 22, &a));
    CHECK(!a.valid);
    CHECK(!cat_agent_parse("<html>", 6, &a));
}

/* test_agent.py's MotorHistory.DOC */
static const char DOC[] =
    "{\"format\": \"catalyst-motor-history\", \"version\": 1, \"updatedMs\": 1, \"clockTrusted\": true,"
    " \"devices\": [{\"serial\": \"000E0B500C776800000A0001160000E3\", \"model\": \"Talon FX\", \"kind\": \"motor\","
    " \"boots\": 3, \"firstSeenMs\": 10, \"lastSeenMs\": 20,"
    " \"identities\": [{\"id\": 45, \"name\": \"Intake Roller\", \"bus\": \"can_s2\", \"firmware\": \"26.1.0.0\"},"
    " {\"id\": 45, \"name\": \"BL, \\\"Drive\\\"\", \"bus\": \"can_s2\", \"firmware\": \"26.1.1.1\"}],"
    " \"totals\": {\"poweredSeconds\": 120.5, \"runningSeconds\": 30, \"revolutions\": 1500.25,"
    " \"peakStatorAmps\": 80, \"peakTempC\": 61, \"hotSeconds\": 0, \"stickyFaults\": 4}}],"
    " \"path\": \"/home/systemcore/catalyst/motor-history.json\"}";

static void motors(void)
{
    static cat_mh_motor_t m[8];
    cat_mh_meta_t meta;
    CHECK(cat_mh_parse_doc(DOC, strlen(DOC), m, 8, &meta) == 1);
    CHECK(meta.clock_trusted);
    NEAR(meta.updated_ms, 1, 1e-9);
    CHECK(!strcmp(m[0].serial, "000E0B500C776800000A0001160000E3"));
    CHECK(!strcmp(m[0].name, "BL, \"Drive\"")); /* the latest identity is the one in force */
    CHECK(!strcmp(m[0].firmware, "26.1.1.1") && m[0].id == 45 && !strcmp(m[0].bus, "can_s2"));
    CHECK(m[0].identities == 2 && m[0].nids == 2 && cat_mh_renumbered(&m[0]));
    NEAR(m[0].powered_s, 120.5, 1e-9);
    NEAR(m[0].revs, 1500.25, 1e-9);
    NEAR(m[0].loaded_s, 0, 1e-9);
    CHECK(m[0].sticky == 4 && m[0].boots == 3);
    CHECK(!cat_mh_hot(&m[0]));

    /* many boots: the newest CAT_MH_SESSIONS are kept, oldest first */
    static char big[16384];
    size_t o = (size_t)snprintf(big, sizeof big, "{\"devices\": [{\"serial\": \"S1\", \"kind\": \"motor\", \"sessions\": [");
    for (int i = 0; i < 40; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "%s{\"startMs\": %d, \"revolutions\": %d, \"hotSeconds\": %d}",
                              i ? ", " : "", i * 1000, i, i == 39 ? 12 : 0);
    o += (size_t)snprintf(big + o, sizeof big - o, "], \"totals\": {\"hotSeconds\": 12, \"peakTempC\": 72}},"
                                                   " {\"serial\": \"S2\", \"model\": \"CANcoder\"}], \"clockTrusted\": false}");
    CHECK(cat_mh_parse_doc(big, o, m, 8, &meta) == 2);
    CHECK(!meta.clock_trusted);
    CHECK(m[0].nsess == CAT_MH_SESSIONS);
    NEAR(m[0].sess[0].revs, 40 - CAT_MH_SESSIONS, 1e-9);
    NEAR(m[0].sess[CAT_MH_SESSIONS - 1].hot_s, 12, 1e-9);
    CHECK(cat_mh_hot(&m[0]) && m[0].id == -1);
    CHECK(!strcmp(m[1].kind, "device")); /* no kind: a device, not a motor */

    /* the agent's answer when the robot program hasn't written the file yet */
    const char *missing = "{\"error\": \"no motor history yet - the robot program writes it once it has seen a motor\","
                          " \"path\": \"/x\", \"devices\": []}";
    CHECK(cat_mh_parse_doc(missing, strlen(missing), m, 8, &meta) == 0);
    CHECK(!strncmp(meta.error, "no motor history yet", 20));
    CHECK(cat_mh_parse_doc("{not json", 9, m, 8, &meta) == -1);
    CHECK(cat_mh_parse_doc("[]", 2, m, 8, &meta) == -1);

    /* NT rows: MotorHistory.publish() */
    cat_mh_motor_t r;
    CHECK(cat_mh_parse_row("000E0B|Talon FX|motor|can_s2|25|Shooter L|26.1.1.1|35280|14760|12960|1410000|118|88.4|2710|"
                           "2656800|51|1700000000000|1790000000000|2|64", &r));
    CHECK(!strcmp(r.name, "Shooter L") && r.id == 25 && r.boots == 51 && r.identities == 2 && r.sticky == 64);
    NEAR(r.peak_c, 88.4, 1e-9);
    NEAR(r.hot_s, 2710, 1e-9);
    CHECK(cat_mh_hot(&r) && cat_mh_renumbered(&r));
    /* a name with a pipe in it keeps it */
    CHECK(cat_mh_parse_row("S|Talon FX|motor|can_s0|3|FR | drive|26|1|2|3|4|5|6|7|8|10|11|12|1|0", &r));
    CHECK(!strcmp(r.name, "FR | drive") && !strcmp(r.firmware, "26") && r.boots == 10);
    CHECK(!cat_mh_parse_row("S|Talon FX|motor", &r));
    CHECK(cat_mh_parse_row("S|CANcoder||can_s0||Gyro||0|0|0|0|0|0|0|0|0|0|0|1|0", &r) && r.id == -1 && !strcmp(r.kind, "device"));
}

static void controls(void)
{
    const char *m = "[{\"control\":\"Left stick\",\"action\":\"Drive, field-centric\",\"controller\":\"Driver\",\"combo\":false},"
                    "{\"control\":\"R1\",\"action\":\"Turret mode while held\"},"
                    "{\"control\":\"Left bumper + Y\",\"action\":\"Climb\",\"controller\":\"Operator\",\"combo\":true},"
                    "{\"control\":\"B\"},{\"action\":\"orphan\"},7,"
                    "{\"control\":\"D-pad up\",\"action\":\"Line up on the tag\",\"controller\":\"\"}]";
    cat_ctl_t c[8];
    int n = cat_controls_parse(m, strlen(m), c, 8);
    CHECK(n == 4); /* the three malformed entries dropped */
    CHECK(!strcmp(c[0].control, "Left stick") && !strcmp(c[0].action, "Drive, field-centric") && !c[0].combo);
    CHECK(!strcmp(c[1].controller, "Driver")); /* unnamed: the driver's */
    CHECK(!strcmp(c[2].controller, "Operator") && c[2].combo);
    CHECK(!strcmp(c[3].controller, "Driver"));
    CHECK(cat_controls_parse("{\"a\":1}", 7, c, 8) == -1);
    CHECK(cat_controls_parse("[]", 2, c, 8) == 0);

    float x, y, yx, yy;
    cat_input_kind_t k;
    CHECK(cat_controls_place("D-pad up", &x, &y, &k) && k == CAT_IN_DPAD);
    CHECK(cat_controls_place("Triangle", &yx, &yy, &k) && k == CAT_IN_BUTTON);
    CHECK(cat_controls_place("Left bumper + Y", &x, &y, &k) && x == yx && y == yy); /* drawn at what completes it */
    CHECK(cat_controls_place("L1", &x, &y, &k) && k == CAT_IN_BUMPER && x < 0.5f);
    CHECK(cat_controls_place("right trigger", &x, &y, &k) && k == CAT_IN_TRIGGER && x > 0.5f);
    CHECK(!cat_controls_place("Paddle 3", &x, &y, &k));
}

static void lanes(void)
{
    static cat_st_lane_t l;
    cat_st_lane_init(&l, "Arm", "/Catalyst/Arm/State", CAT_LANE_MECH);
    CHECK(!cat_st_feed(&l, 0, NULL)); /* nothing yet, and still nothing */
    CHECK(cat_st_feed(&l, 100, "STOW"));
    CHECK(!cat_st_feed(&l, 200, "STOW"));
    CHECK(cat_st_feed(&l, 1000, "INTAKE"));
    CHECK(cat_st_feed(&l, 3000, "STOW"));
    CHECK(cat_st_feed(&l, 4000, NULL));     /* link dropped */
    CHECK(cat_st_feed(&l, 5000, "STOW"));   /* back: not a transition */
    CHECK(cat_st_feed(&l, 6000, "AMP"));
    CHECK(!strcmp(cat_st_current(&l), "AMP"));
    CHECK(l.nnames == 3 && l.count == 6);
    CHECK(cat_st_count(&l, 0, 10000) == 3);   /* STOW→INTAKE, INTAKE→STOW, STOW→AMP */
    CHECK(cat_st_count(&l, 2000, 10000) == 2);
    CHECK(cat_st_count(&l, 0, 1000) == 0);
    /* time in STOW over [0, 7000): 100..1000, 3000..4000, 5000..6000 */
    CHECK(cat_st_time_in(&l, 0, 0, 7000, 7000) == 900 + 1000 + 1000);
    CHECK(cat_st_time_in(&l, 0, 3500, 7000, 7000) == 500 + 1000);
    CHECK(cat_st_time_in(&l, 2, 0, 7000, 6500) == 500); /* AMP until now */
    cat_st_edge_t e;
    CHECK(cat_st_edge(&l, 0, &e) && e.ms == 6000 && e.s == 2);
    CHECK(cat_st_edge(&l, 2, &e) && e.s == -1);
    CHECK(!cat_st_edge(&l, 6, &e));

    /* the ring keeps the newest CAT_ST_RING edges; the total keeps counting */
    cat_st_lane_init(&l, "flip", "/x", CAT_LANE_OTHER);
    for (int i = 0; i < CAT_ST_RING + 10; i++) cat_st_feed(&l, (uint32_t)i * 10, i % 2 ? "A" : "B");
    CHECK(l.count == CAT_ST_RING && l.total == CAT_ST_RING + 10);
    CHECK(cat_st_edge(&l, CAT_ST_RING - 1, &e) && e.ms == 100);

    /* more distinct states than the table: the rest share "…" */
    cat_st_lane_init(&l, "many", "/y", CAT_LANE_OTHER);
    char s[16];
    for (int i = 0; i < CAT_ST_NAMES + 5; i++) {
        snprintf(s, sizeof s, "S%d", i);
        cat_st_feed(&l, (uint32_t)i, s);
    }
    CHECK(l.nnames == CAT_ST_NAMES && !strcmp(cat_st_current(&l), "\xe2\x80\xa6"));
}

static void csv(void)
{
    char b[256];
    CHECK(cat_csv_field(b, sizeof b, "plain") == 5 && !strcmp(b, "plain"));
    cat_csv_field(b, sizeof b, "a,b \"c\"");
    CHECK(!strcmp(b, "\"a,b \"\"c\"\"\""));
    cat_rec_col_t cols[3] = {
        { "/Catalyst/Systemcore/BatteryVolts", -1, "/Catalyst/Systemcore/BatteryVolts" },
        { "/Catalyst/Physics/PoseArray", 0, "/Catalyst/Physics/PoseArray[0]" },
        { "/SmartDashboard/Odd, name", -1, "/SmartDashboard/Odd, name" },
    };
    size_t n = cat_csv_header(b, sizeof b, cols, 3);
    CHECK(n > 0 && !strcmp(b, "t,mark,/Catalyst/Systemcore/BatteryVolts,/Catalyst/Physics/PoseArray[0],\"/SmartDashboard/Odd, name\"\n"));
    CHECK(cat_csv_count_cols(b) == 5);
    CHECK(cat_csv_header(b, 20, cols, 3) == 0); /* doesn't fit: nothing half-written */

    double v[3] = { 12.5, 3.25, 0 };
    bool have[3] = { true, true, false };
    n = cat_csv_row(b, sizeof b, 1.02, 0, v, have, 3);
    CHECK(!strcmp(b, "1.020,,12.5,3.25,\n")); /* absent is empty, never 0 */
    n = cat_csv_row(b, sizeof b, 2.5, 3, v, have, 3);
    CHECK(!strcmp(b, "2.500,3,12.5,3.25,\n") && n == strlen(b));
    v[0] = NAN;
    cat_csv_row(b, sizeof b, 3, 0, v, have, 3);
    CHECK(!strcmp(b, "3.000,,,3.25,\n"));
    CHECK(cat_csv_row(b, 8, 3, 0, v, have, 3) == 0);

    double t;
    const char *file = "t,mark,a\n0.000,,1\n0.020,,2\n0.040,1,3\n0.06";
    CHECK(cat_csv_last_t(file, strlen(file), &t));
    NEAR(t, 0.04, 1e-9); /* the half-written row doesn't count */
    CHECK(!cat_csv_last_t("t,mark,a\n", 9, &t));
    CHECK(cat_csv_last_t("12.5,,1\r\n", 9, &t) && t == 12.5);
}

void test_sc(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    agent();
    motors();
    controls();
    lanes();
    csv();
}
