/* Tests for the intelligence core's pure parts (components/intel): Nexus's document and labels, when the match
 * alarms ring, Statbotics' and the robot network's documents, the rules, the store's "never nag", the ask bar's
 * intents and the briefing. */
#include "intel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void labels(void)
{
    char l[12];
    int lv, st, nb;
    CHECK(nx_label("Qualification 34", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "Q34") && lv == 0 && nb == 34);
    CHECK(nx_label("Qual 7", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "Q7"));
    CHECK(nx_label("Q12", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "Q12"));
    CHECK(nx_label("Playoff 5", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "SF5") && lv == 3 && st == 5 && nb == 1);
    CHECK(nx_label("Match 5 (R2)", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "SF5"));
    CHECK(nx_label("Semifinal 3", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "SF3"));
    CHECK(nx_label("Final 2", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "F2") && lv == 4 && nb == 2);
    CHECK(nx_label("Finals 3", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "F3"));
    CHECK(nx_label("Quarterfinal 2-3", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "QF2-3"));
    CHECK(nx_label("Practice 3", l, sizeof l, &lv, &st, &nb) && !strcmp(l, "P3") && lv == -1);
    CHECK(!nx_label("", l, sizeof l, &lv, &st, &nb));
    CHECK(!nx_label("Lunch", l, sizeof l, &lv, &st, &nb));
    CHECK(nx_stage("Now queuing") == 1 && nx_stage("Queuing soon") == 0 && nx_stage("On deck") == 2 &&
          nx_stage("On field") == 3 && nx_stage(NULL) == 0 && nx_stage("") == 0);
    CHECK(!strcmp(nx_status_short("Now queuing"), "queuing") && !strcmp(nx_status_short("Queuing soon"), "queuing soon"));
}

static const char NX_DOC[] =
    "{\"eventKey\":\"2026casj\",\"dataAsOfTime\":1790000000000,\"nowQueuing\":\"Qualification 32\","
    "\"matches\":["
    "{\"label\":\"Qualification 31\",\"status\":\"On field\",\"redTeams\":[\"1\",\"2\",\"3\"],\"blueTeams\":[\"4\",\"5\",\"6\"],"
    "\"times\":{\"estimatedStartTime\":1789999000000,\"actualOnFieldTime\":1789999100000}},"
    "{\"label\":\"Qualification 32\",\"status\":\"Now queuing\",\"redTeams\":[\"7\",\"8\",\"9\"],\"blueTeams\":[\"10\",\"11\",\"12\"],"
    "\"times\":{\"estimatedQueueTime\":1789999500000}},"
    "{\"label\":\"Qualification 33\",\"status\":null,\"redTeams\":[\"13\",null,\"15\"],\"blueTeams\":[\"16\",\"17\",\"18\"]},"
    "{\"label\":\"Qualification 34\",\"status\":\"Queuing soon\",\"redTeams\":[\"5805\",\"1234\",\"5678\"],"
    "\"blueTeams\":[\"111\",\"222\",\"333\"],\"times\":{\"estimatedQueueTime\":1790001200000,\"estimatedOnDeckTime\":"
    "1790001800000,\"estimatedOnFieldTime\":1790002100000,\"estimatedStartTime\":1790002200000},\"breakAfter\":\"Lunch\","
    "\"replayOf\":null},"
    "{\"label\":\"Qualification 40\",\"redTeams\":[254,1678,971],\"blueTeams\":[\"frc5805\",4414,1323],"
    "\"times\":{\"estimatedStartTime\":1790009000}},"
    "{\"label\":\"Playoff 3\",\"redTeams\":[{\"x\":1}],\"blueTeams\":{\"teams\":[\"5805\"]},\"replayOf\":\"Playoff 2\"}],"
    "\"announcements\":[{\"id\":\"a\",\"announcement\":\"Queue is open\",\"postedTime\":1789990000000},"
    "{\"id\":\"b\",\"announcement\":\"Lunch at 12:00 \\u2014 back 13:00\",\"postedTime\":1789995000000},"
    "{\"id\":\"c\",\"announcement\":\"Pit ends 18:00\",\"postedTime\":1789980000000},"
    "{\"id\":\"d\",\"announcement\":\"a\",\"postedTime\":1789970000000},"
    "{\"id\":\"e\",\"announcement\":\"newest\",\"postedTime\":1789999999000}],"
    "\"partsRequests\":[{\"id\":\"p\",\"parts\":\"1/2 in hex bearing\",\"requestedByTeam\":\"1234\",\"postedTime\":1789990000000}]}";

static void nexus(void)
{
    static nx_state_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.event_key, sizeof s.event_key, "keep");
    CHECK(nx_parse(NX_DOC, strlen(NX_DOC), 5805, &s));
    CHECK(!strcmp(s.event_key, "keep"));
    CHECK(s.as_of == 1790000000);
    CHECK(!strcmp(s.now_queuing, "Qualification 32") && !strcmp(s.now_label, "Q32") && s.now_index == 1);
    CHECK(s.nmatches == 6 && s.nours == 3);
    const nx_match_t *q = &s.ours[0];
    CHECK(!strcmp(q->label, "Q34") && q->ours == 1 && q->index == 3 && q->red[0] == 5805 && q->blue[2] == 333);
    CHECK(q->queue_at == 1790001200 && q->deck_at == 1790001800 && q->field_at == 1790002100 && q->start_at == 1790002200);
    CHECK(!strcmp(q->status, "Queuing soon") && !q->replay && !q->queued && !q->started);
    CHECK(!strcmp(s.ours[1].label, "Q40") && s.ours[1].ours == 2 && s.ours[1].start_at == 1790009000); /* seconds taken too */
    CHECK(s.ours[1].red[0] == 254 && s.ours[1].blue[1] == 4414);
    CHECK(!strcmp(s.ours[2].label, "SF3") && s.ours[2].ours == 2 && s.ours[2].replay);
    CHECK(s.nann == NX_ANN && !strcmp(s.ann[0].text, "newest") && s.ann[0].at == 1789999999);
    CHECK(!strcmp(s.ann[1].text, "Lunch at 12:00 \xe2\x80\x94 back 13:00")); /* the em dash kept, as the faces carry it */
    CHECK(s.ann[3].at == 1789980000); /* the oldest ("a") dropped */
    CHECK(s.nparts == 1 && s.parts[0].team == 1234 && !strcmp(s.parts[0].text, "1/2 in hex bearing"));
    /* not a document */
    nx_state_t t;
    memset(&t, 0, sizeof t);
    CHECK(!nx_parse("\"Missing API key.\"", 18, 5805, &t));
    CHECK(!nx_parse("{\"error\":\"nope\"}", 16, 5805, &t));
    CHECK(!nx_parse("not json", 8, 5805, &t));
    /* an empty event */
    CHECK(nx_parse("{\"nowQueuing\":null,\"matches\":[]}", 32, 5805, &t) && t.nours == 0 && t.now_index == -1 && !t.now_queuing[0]);
}

static void timing(void)
{
    const time_t T = 1790000000;
    mt_times_t t;
    /* TBA only: predicted wins over scheduled */
    mt_times(T + 3600, T + 3000, NULL, &t);
    CHECK(t.start == T + 3600 && t.src == MT_SRC_PREDICTED && !t.queue_at && !t.stage);
    CHECK(mt_ring_at(&t, 0, 25) == T + 3600 - 1500 && mt_ring_at(&t, 1, 5) == T + 3300 && mt_ring_at(&t, 0, 0) == 0);
    CHECK(mt_due(&t, T + 2099, 25, 5, 0) == 0);
    CHECK(mt_due(&t, T + 2100, 25, 5, 0) == 1);
    CHECK(mt_due(&t, T + 2100, 25, 5, 1) == 0);
    CHECK(mt_due(&t, T + 3300, 25, 5, 1) == 2);
    CHECK(mt_due(&t, T + 3300, 25, 5, 0) == 2); /* both due: the match one alone */
    CHECK(mt_due(&t, T + 3600, 25, 5, 0) == 0); /* its time has passed */
    CHECK(mt_due(&t, T + 2100, 0, 0, 0) == 0);  /* both off */
    mt_times(0, T + 3000, NULL, &t);
    CHECK(t.src == MT_SRC_SCHEDULED && t.start == T + 3000);
    /* Nexus: its start, and its queue time less the walk when that comes first */
    nx_match_t x;
    memset(&x, 0, sizeof x);
    x.start_at = T + 4200;
    x.queue_at = T + 1800;
    mt_times(T + 3600, T + 3000, &x, &t);
    CHECK(t.start == T + 4200 && t.src == MT_SRC_NEXUS && t.queue_at == T + 1800);
    CHECK(mt_ring_at(&t, 0, 25) == T + 1800 - NX_WALK_S); /* 25 min before the start would be T + 2700: later */
    CHECK(mt_due(&t, T + 1500, 25, 5, 0) == 1);
    x.queue_at = T + 4000; /* a queue time after start - 25: the lead wins */
    mt_times(0, 0, &x, &t);
    CHECK(mt_ring_at(&t, 0, 25) == T + 4200 - 1500);
    /* Nexus says it's queuing: now, even past its time; on deck: the match reminder; on the field: nothing */
    snprintf(x.status, sizeof x.status, "Now queuing");
    mt_times(0, 0, &x, &t);
    CHECK(t.stage == 1 && mt_due(&t, T, 25, 5, 0) == 1 && mt_due(&t, T + 5000, 25, 5, 0) == 1);
    CHECK(mt_due(&t, T, 25, 5, 1) == 0);
    snprintf(x.status, sizeof x.status, "On deck");
    mt_times(0, 0, &x, &t);
    CHECK(t.stage == 2 && mt_due(&t, T, 25, 5, 1) == 2 && mt_due(&t, T, 25, 0, 1) == 0);
    snprintf(x.status, sizeof x.status, "On field");
    mt_times(0, 0, &x, &t);
    CHECK(mt_due(&t, T + 4000, 25, 5, 0) == 0);
    /* queued by the actual time without a status word; started means done */
    memset(&x, 0, sizeof x);
    x.start_at = T + 4200;
    x.queued = T - 10;
    mt_times(0, 0, &x, &t);
    CHECK(t.stage == 1);
    x.started = T;
    mt_times(0, 0, &x, &t);
    CHECK(t.stage == 3);
    /* only on-field time: taken as the start */
    memset(&x, 0, sizeof x);
    x.field_at = T + 100;
    mt_times(T + 50, 0, &x, &t);
    CHECK(t.start == T + 100 && t.src == MT_SRC_NEXUS);
    CHECK(!strcmp(mt_src_name(MT_SRC_NEXUS), "nexus") && !strcmp(mt_src_name(MT_SRC_PREDICTED), "tba predicted"));
}

static void statbotics(void)
{
    static sb_state_t s;
    memset(&s, 0, sizeof s);
    const char *ty = "{\"team\":5805,\"year\":2026,\"epa\":{\"total_points\":{\"mean\":31.24,\"sd\":6.1},\"unitless\":1612,"
                     "\"ranks\":{\"total\":{\"rank\":312,\"percentile\":0.912,\"team_count\":3700},"
                     "\"state\":{\"rank\":40,\"percentile\":0.8,\"team_count\":260},\"district\":null}},"
                     "\"record\":{\"wins\":12,\"losses\":9,\"ties\":1,\"count\":22,\"winrate\":0.56}}";
    CHECK(sb_parse_team_year(ty, strlen(ty), &s));
    CHECK(s.have_year && fabs(s.epa - 31.24) < 1e-9 && fabs(s.epa_sd - 6.1) < 1e-9);
    CHECK(s.rank == 312 && s.team_count == 3700 && fabs(s.percentile - 0.912) < 1e-9 && s.state_rank == 40 && s.state_count == 260);
    CHECK(s.wins == 12 && s.losses == 9 && s.ties == 1);
    /* the older flat shape */
    sb_state_t f;
    memset(&f, 0, sizeof f);
    const char *flat = "{\"team\":5805,\"epa_end\":28.5,\"epa_rank\":400,\"wins\":3,\"losses\":4,\"ties\":0}";
    CHECK(sb_parse_team_year(flat, strlen(flat), &f) && fabs(f.epa - 28.5) < 1e-9 && f.rank == 400 && f.wins == 3);
    const char *te = "{\"team\":5805,\"event\":\"2026casj\",\"epa\":{\"total_points\":{\"mean\":33.0}},"
                     "\"record\":{\"qual\":{\"wins\":5,\"losses\":7,\"ties\":0,\"rank\":45,\"num_teams\":60}}}";
    CHECK(sb_parse_team_event(te, strlen(te), &s) && s.have_event && fabs(s.event_epa - 33.0) < 1e-9);
    CHECK(s.event_rank == 45 && s.event_teams == 60 && s.ev_wins == 5 && !strcmp(s.event_key, "2026casj"));
    const char *ms = "[{\"key\":\"2026casj_qm34\",\"comp_level\":\"qm\",\"set_number\":1,\"match_number\":34,\"time\":1790002000,"
                     "\"alliances\":{\"red\":{\"team_keys\":[5805,1234,5678]},\"blue\":{\"team_keys\":[111,222,333]}},"
                     "\"pred\":{\"winner\":\"red\",\"red_win_prob\":0.62,\"red_score\":85.3,\"blue_score\":70.1},"
                     "\"result\":{\"red_score\":null,\"blue_score\":null},\"status\":\"Upcoming\"},"
                     "{\"key\":\"2026casj_qm12\",\"alliances\":{\"red\":{\"team_keys\":[1,2,3]},\"blue\":{\"team_keys\":[\"frc5805\",5,6]}},"
                     "\"pred\":{\"red_win_prob\":0.3},\"result\":{\"red_score\":40,\"blue_score\":55},\"status\":\"Completed\"},"
                     "{\"key\":\"2026casj_sf5m1\",\"alliances\":{\"red\":{\"team_keys\":[5805]},\"blue\":{\"team_keys\":[9]}}},"
                     "{\"key\":\"2026casj_qm50\",\"alliances\":{\"red\":{\"team_keys\":[7,8,9]},\"blue\":{\"team_keys\":[10,11,12]}}}]";
    CHECK(sb_parse_matches(ms, strlen(ms), 5805, &s));
    CHECK(s.nmatches == 3);
    CHECK(!strcmp(s.matches[0].label, "Q34") && s.matches[0].ours == 1 && fabs(s.matches[0].win - 0.62) < 1e-9 && !s.matches[0].played);
    CHECK(fabs(s.matches[0].red_pred - 85.3) < 1e-9 && s.matches[0].time == 1790002000);
    CHECK(!strcmp(s.matches[1].label, "Q12") && s.matches[1].ours == 2 && fabs(s.matches[1].win - 0.7) < 1e-9);
    CHECK(s.matches[1].played && s.matches[1].red_score == 40 && s.matches[1].blue_score == 55);
    CHECK(!strcmp(s.matches[2].label, "SF5") && isnan(s.matches[2].win));
    sb_match_t m;
    (void)m;
    CHECK(!sb_parse_matches("{}", 2, 5805, &s));
}

static void robot_net(void)
{
    rn_cam_t c[RN_CAMS];
    const char *agg = "[{\"name\":\"limelight-front\",\"host\":\"limelight-front\",\"ip\":\"10.58.5.11\",\"ntConnected\":true},"
                      "{\"name\":\"limelight-left\",\"ip\":\"10.58.5.12/24\",\"ntConnected\":false,\"fps\":88.9,"
                      "\"temperatureC\":71.5,\"cpuPercent\":40,\"ramPercent\":30,\"pipelineIndex\":2,\"pipelineType\":\"pipe_fiducial\"}]";
    int n = rn_parse_cameras(agg, strlen(agg), c, RN_CAMS);
    CHECK(n == 2 && !strcmp(c[0].name, "limelight-front") && !strcmp(c[0].ip, "10.58.5.11") && c[0].nt && isnan(c[0].fps));
    CHECK(!strcmp(c[1].ip, "10.58.5.12") && !c[1].nt && fabs(c[1].temp - 71.5) < 1e-9 && c[1].pipeline == 2);
    const char *wrapped = "{\"available\":true,\"cameras\":[{\"ntName\":\"limelight-shooter\",\"ip\":\"10.58.5.13\"}]}";
    CHECK(rn_parse_cameras(wrapped, strlen(wrapped), c, RN_CAMS) == 1 && !strcmp(c[0].name, "limelight-shooter"));
    CHECK(rn_parse_cameras("{\"x\":1}", 7, c, RN_CAMS) == -1);
    rn_cam_t cam;
    memset(&cam, 0, sizeof cam);
    snprintf(cam.name, sizeof cam.name, "front");
    const char *st = "{\"name\":\"limelight\",\"fps\":90.1,\"cpu\":22.5,\"ram\":41,\"temp\":63.2,\"pipelineIndex\":0,"
                     "\"pipelineType\":\"pipe_fiducial\",\"hwType\":5}";
    CHECK(rn_parse_ll_status(st, strlen(st), &cam) && cam.answered && !strcmp(cam.name, "front"));
    CHECK(fabs(cam.fps - 90.1) < 1e-9 && fabs(cam.temp - 63.2) < 1e-9 && cam.pipeline == 0);
    CHECK(!rn_parse_ll_status("{\"hello\":1}", 11, &cam));
    rn_dev_t d[8];
    const char *ph = "{\"DeviceArray\":[{\"Model\":\"Talon FX\",\"ID\":1,\"Name\":\"FL drive\",\"CurrentVers\":\"26.1.0.0\"},"
                     "{\"model\":\"CANcoder\",\"id\":11,\"firmwareVersion\":\"26.70.0\",\"canbus\":\"can_s0\"}],"
                     "\"GeneralReturn\":{\"Error\":0}}";
    n = rn_parse_phoenix(ph, strlen(ph), d, 8);
    CHECK(n == 2 && !strcmp(d[0].model, "Talon FX") && d[0].id == 1 && !strcmp(d[0].firmware, "26.1.0.0"));
    CHECK(!strcmp(d[1].model, "CANcoder") && d[1].id == 11 && !strcmp(d[1].bus, "can_s0"));
    CHECK(rn_parse_phoenix("{\"GeneralReturn\":{}}", 20, d, 8) == -1);
}

/* a quiet afternoon: nothing time-bound, nothing wrong */
static void base_in(intel_in_t *in, time_t now)
{
    memset(in, 0, sizeof *in);
    in->now = now;
    in->mono = 1000;
    in->team = 5805;
    in->battery_v = in->vmin_last = in->epa = in->percentile = in->loop_max_ms = NAN;
    in->core_temp = in->disk_free_pct = in->cam_hot = NAN;
    for (int i = 0; i < 4; i++) in->follow_pct[i] = NAN;
    in->brownouts_last = -1;
    in->rain_hour = -1;
    in->tablet_pct = 80;
    in->nexus_key = true;
    in->nexus = NX_READY;
}

static const intel_insight_t *find_key(const intel_insight_t *l, int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
        if (!strncmp(l[i].key, prefix, strlen(prefix))) return &l[i];
    return NULL;
}

static void rules(void)
{
    const time_t T = 1790000000;
    static intel_in_t in;
    static intel_insight_t out[INTEL_MAX];
    base_in(&in, T);
    CHECK(intel_rules(&in, out, INTEL_MAX) == 0);
    /* a match 40 minutes out, Nexus's queue in 25: a battery to pick makes the prep line worth saying */
    in.event_live = true;
    in.nup = 1;
    snprintf(in.up[0].label, sizeof in.up[0].label, "Q34");
    in.up[0].start = T + 40 * 60;
    in.up[0].src = MT_SRC_NEXUS;
    in.up[0].queue_at = T + 25 * 60;
    in.up[0].ours = 1;
    in.up[0].win = 0.62;
    in.up[0].away = 3;
    snprintf(in.now_queuing, sizeof in.now_queuing, "Q31");
    snprintf(in.batt_line, sizeof in.batt_line, "battery: #7 \xc2\xb7 lowest resistance");
    int n = intel_rules(&in, out, INTEL_MAX);
    const intel_insight_t *p = find_key(out, n, "prep:Q34");
    CHECK(p && p->prio == 2 && p->cat == IC_MATCH);
    CHECK(p && strstr(p->text, "Q34 queues ~") && strstr(p->text, "put in #7") && strstr(p->text, "62% to win"));
    CHECK(p && strstr(p->why, "nexus") && strstr(p->why, "3 away"));
    /* picked already and nothing wrong: listed, not said */
    snprintf(in.batt_pick, sizeof in.batt_pick, "#7");
    n = intel_rules(&in, out, INTEL_MAX);
    p = find_key(out, n, "prep:Q34");
    CHECK(p && p->prio == 1 && strstr(p->text, "battery #7 in"));
    /* the last run: FL following 70 % while the rest follow; two brownouts */
    in.session = 3;
    in.session_end = 900;
    in.mods = 4;
    in.follow_pct[0] = 70;
    in.follow_pct[1] = in.follow_pct[2] = in.follow_pct[3] = 97;
    in.brownouts_last = 2;
    in.vmin_last = 6.4;
    n = intel_rules(&in, out, INTEL_MAX);
    const intel_insight_t *lag = find_key(out, n, "lag:3:FL"), *br = find_key(out, n, "brown:3");
    CHECK(lag && lag->prio == 2 && strstr(lag->text, "FL module followed 70%"));
    CHECK(br && br->prio == 2 && strstr(br->text, "2 brownouts") && strstr(br->text, "6.4 V") && br->until > 900);
    p = find_key(out, n, "prep:Q34");
    CHECK(p && p->prio == 2 && strstr(p->text, "FL lagged 30% last run")); /* the worry rides the prep line */
    /* queuing now: the alarm has it; a robot fault still makes it urgent */
    snprintf(in.up[0].status, sizeof in.up[0].status, "Now queuing");
    n = intel_rules(&in, out, INTEL_MAX);
    p = find_key(out, n, "prep:Q34");
    CHECK(p && p->prio == 3 && strstr(p->text, "Q34 is queuing"));
    in.mods = 0;
    in.brownouts_last = 0;
    n = intel_rules(&in, out, INTEL_MAX);
    p = find_key(out, n, "prep:Q34");
    CHECK(p && p->prio == 1);
    /* every module under-following alike: global, listed */
    in.mods = 4;
    for (int i = 0; i < 4; i++) in.follow_pct[i] = 72;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "lag:3:") && find_key(out, n, "lagall:3") && find_key(out, n, "lagall:3")->prio == 1);
    /* a low battery in the robot with the match close */
    in.connected = true;
    in.battery_v = 12.1;
    in.up[0].status[0] = 0;
    in.up[0].start = T + 20 * 60;
    n = intel_rules(&in, out, INTEL_MAX);
    p = find_key(out, n, "batt-low:Q34");
    CHECK(p && p->prio == 3 && strstr(p->text, "12.10 V"));
    in.enabled = true;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "batt-low")); /* under load it reads low anyway */
    in.enabled = false;
    /* CAN, errors, cameras, firmware */
    in.can_down = true;
    in.errors = 2;
    snprintf(in.worst_alert, sizeof in.worst_alert, "[Arm] encoder disconnected");
    in.cams_bad = 1;
    snprintf(in.cam_bad, sizeof in.cam_bad, "limelight-front: no NT session");
    in.fw_mixed = 1;
    snprintf(in.fw_line, sizeof in.fw_line, "Talon FX on two firmwares");
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(find_key(out, n, "can-down") && find_key(out, n, "can-down")->prio == 3);
    CHECK(find_key(out, n, "err:") && strstr(find_key(out, n, "err:")->text, "(+1 more)"));
    CHECK(find_key(out, n, "cam:") && find_key(out, n, "fw:") && find_key(out, n, "fw:")->prio == 2);
    /* a disconnected robot says nothing about its CAN */
    in.connected = false;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "can-down") && !find_key(out, n, "err:"));
    /* the event without Nexus: listed; an announcement: said */
    base_in(&in, T);
    in.event_live = true;
    in.nexus_key = false;
    in.nexus = NX_OFF;
    snprintf(in.ann, sizeof in.ann, "Lunch until 13:00");
    in.ann_at = T - 60;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(find_key(out, n, "svc:nexus") && find_key(out, n, "svc:nexus")->prio == 1);
    CHECK(find_key(out, n, "ann:") && find_key(out, n, "ann:")->prio == 2);
    in.ann_at = T - 3600; /* an hour old: no longer news */
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "ann:"));
    /* another team's parts request: listed; our own isn't news to us */
    snprintf(in.part, sizeof in.part, "a 1/2 in hex bearing");
    in.part_team = 1234;
    in.part_at = T - 60;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(find_key(out, n, "part:") && find_key(out, n, "part:")->prio == 1 &&
          !strcmp(find_key(out, n, "part:")->text, "team 1234 needs a 1/2 in hex bearing"));
    in.part_team = 5805;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "part:"));
    in.part[0] = 0;
    /* the tablet low on an event day */
    in.tablet_pct = 30;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(find_key(out, n, "tablet-low"));
    in.tablet_charging = true;
    n = intel_rules(&in, out, INTEL_MAX);
    CHECK(!find_key(out, n, "tablet-low"));
    /* no clock: nothing time-bound */
    base_in(&in, 0);
    in.nup = 1;
    snprintf(in.up[0].label, sizeof in.up[0].label, "Q1");
    in.up[0].start = 100;
    CHECK(!find_key(out, intel_rules(&in, out, INTEL_MAX), "prep"));
}

static intel_insight_t mk(const char *key, int prio, int cat, const char *text)
{
    intel_insight_t x;
    memset(&x, 0, sizeof x);
    snprintf(x.key, sizeof x.key, "%s", key);
    snprintf(x.text, sizeof x.text, "%s", text);
    x.prio = prio;
    x.cat = cat;
    return x;
}

static void store(void)
{
    intel_clear();
    intel_cfg_t cfg;
    intel_cfg_default(&cfg);
    char f[48];
    intel_cfg_format(&cfg, f, sizeof f);
    intel_cfg_t back;
    CHECK(intel_cfg_parse(f, &back) && back.cats == cfg.cats && back.quiet_from == 22 && back.quiet_to == 7 && back.brief_hhmm == -1);
    CHECK(!intel_cfg_parse("1,2,3", &back) && !intel_cfg_parse("63,25,7,-1,0", &back) && !intel_cfg_parse("63,22,7,1261,0", &back));
    intel_insight_t a[3] = { mk("a", 2, IC_ROBOT, "fl lags"), mk("b", 2, IC_MATCH, "q34 soon"), mk("c", 1, IC_EVENT, "epa") };
    double t = 10000;
    CHECK(intel_merge(a, 3, t) == 3);
    intel_insight_t said;
    /* one prio 2 now, the next not within ten minutes; prio 1 never */
    CHECK(intel_next_say(&cfg, 12, t, &said) && said.prio == 2);
    CHECK(!intel_next_say(&cfg, 12, t + 60, &said));
    CHECK(intel_next_say(&cfg, 12, t + 601, &said) && said.prio == 2);
    CHECK(!intel_next_say(&cfg, 12, t + 2000, &said)); /* both said; "c" is prio 1 */
    /* a prio 3 isn't held by the gap */
    intel_insight_t u = mk("urgent", 3, IC_POWER, "swap the battery");
    intel_insight_t all[4] = { a[0], a[1], a[2], u };
    intel_merge(all, 4, t + 700);
    CHECK(intel_next_say(&cfg, 12, t + 701, &said) && !strcmp(said.key, "urgent"));
    /* the list: most important first */
    intel_insight_t l[INTEL_MAX];
    int n = intel_list(l, INTEL_MAX);
    CHECK(n == 4 && !strcmp(l[0].key, "urgent") && l[3].prio == 1);
    intel_insight_t top;
    CHECK(intel_top(&top) && !strcmp(top.key, "urgent"));
    intel_dismiss("urgent");
    CHECK(intel_top(&top) && strcmp(top.key, "urgent"));
    n = intel_list(l, INTEL_MAX);
    CHECK(!strcmp(l[n - 1].key, "urgent") && l[n - 1].dismissed);
    /* dropped by the rules: gone; back within the hour: not said again */
    intel_merge(a, 2, t + 800);
    n = intel_list(l, INTEL_MAX);
    CHECK(n == 2);
    intel_merge(all, 4, t + 900);
    n = intel_list(l, INTEL_MAX);
    bool urgent_dismissed = false;
    for (int i = 0; i < n; i++)
        if (!strcmp(l[i].key, "urgent")) urgent_dismissed = l[i].dismissed && l[i].said;
    CHECK(urgent_dismissed);
    CHECK(!intel_next_say(&cfg, 12, t + 5000, &said));
    /* a rise to prio 3 is said again */
    intel_insight_t up = mk("a", 3, IC_ROBOT, "fl lags badly");
    intel_insight_t all2[4] = { up, a[1], a[2], u };
    intel_merge(all2, 4, t + 5100);
    CHECK(intel_next_say(&cfg, 12, t + 5101, &said) && !strcmp(said.key, "a") && said.prio == 3);
    /* kept past its rule while `until` holds */
    intel_clear();
    intel_insight_t k = mk("brown:1", 2, IC_POWER, "brownouts");
    k.until = t + 100;
    intel_merge(&k, 1, t);
    CHECK(intel_merge(NULL, 0, t + 50) == 1 && intel_merge(NULL, 0, t + 150) == 0);
    /* quiet hours, across midnight; a category turned off */
    intel_clear();
    intel_insight_t q = mk("q", 2, IC_DAY, "rain");
    intel_merge(&q, 1, t);
    CHECK(!intel_next_say(&cfg, 23, t, &said) && !intel_next_say(&cfg, 3, t, &said));
    cfg.cats &= ~(1u << IC_DAY);
    CHECK(!intel_next_say(&cfg, 12, t, &said));
    cfg.cats = IC_ALL;
    CHECK(intel_next_say(&cfg, 12, t, &said));
    /* at most six an hour, even urgent ones */
    intel_clear();
    int count = 0;
    for (int i = 0; i < 9; i++) {
        char key[8];
        snprintf(key, sizeof key, "u%d", i);
        intel_insight_t x = mk(key, 3, IC_ROBOT, "x");
        intel_merge(&x, 1, t + i); /* each replaces the last (the rules raise only it) */
        if (intel_next_say(&cfg, 12, t + i, &said)) count++;
    }
    CHECK(count == 6);
    intel_clear();
}

static void intents(void)
{
    static const char *const APPS[] = { "weather", "batteries", "blue alliance", "notes", "timer", NULL };
    intel_intent_t r = intel_intent("timer 5 min", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 300 && !r.arg[0]);
    r = intel_intent("5 min timer tea", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 300 && !strcmp(r.arg, "tea"));
    r = intel_intent("Set a timer for 1 hour 30 minutes", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 5400);
    r = intel_intent("90s", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 90);
    r = intel_intent("tea timer 4 min", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 240 && !strcmp(r.arg, "tea"));
    r = intel_intent("timer two minutes", APPS);
    CHECK(r.kind == IN_TIMER && r.seconds == 120);
    CHECK(intel_intent("stopwatch", APPS).kind == IN_STOPWATCH);
    r = intel_intent("brightness 40", APPS);
    CHECK(r.kind == IN_BRIGHT && r.value == 40 && !r.delta);
    r = intel_intent("brighter", APPS);
    CHECK(r.kind == IN_BRIGHT && r.value == 20 && r.delta);
    r = intel_intent("brightness", APPS);
    CHECK(r.kind == IN_NONE);
    r = intel_intent("volume up", APPS);
    CHECK(r.kind == IN_VOLUME && r.delta && r.value == 20);
    CHECK(intel_intent("mute", APPS).kind == IN_MUTE);
    r = intel_intent("dark mode", APPS);
    CHECK(r.kind == IN_TONE && !strcmp(r.arg, "dark"));
    CHECK(intel_intent("Next match", APPS).kind == IN_NEXT);
    CHECK(intel_intent("when do we play", APPS).kind == IN_NEXT);
    r = intel_intent("battery for Q34", APPS);
    CHECK(r.kind == IN_BATTERY && !strcmp(r.arg, "Q34"));
    r = intel_intent("which battery", APPS);
    CHECK(r.kind == IN_BATTERY && !r.arg[0]);
    CHECK(intel_intent("what's queuing", APPS).kind == IN_QUEUE);
    CHECK(intel_intent("EPA", APPS).kind == IN_EPA);
    CHECK(intel_intent("brief me", APPS).kind == IN_BRIEF);
    CHECK(intel_intent("what's up", APPS).kind == IN_INSIGHTS);
    CHECK(intel_intent("weather", APPS).kind == IN_WEATHER);
    CHECK(intel_intent("weather tomorrow", APPS).kind == IN_NONE);
    r = intel_intent("open weather", APPS);
    CHECK(r.kind == IN_OPEN && !strcmp(r.arg, "weather"));
    r = intel_intent("show the batteries", APPS);
    CHECK(r.kind == IN_OPEN && !strcmp(r.arg, "batteries"));
    r = intel_intent("blue alliance", APPS);
    CHECK(r.kind == IN_OPEN && !strcmp(r.arg, "blue alliance"));
    r = intel_intent("remind me in 10 min to call the inspector", APPS);
    CHECK(r.kind == IN_REMIND && r.seconds == 600 && !strcmp(r.arg, "call the inspector"));
    r = intel_intent("remind me at 14:30 to charge #7", APPS);
    CHECK(r.kind == IN_REMIND && r.seconds == 0 && r.value == 1430 && !strcmp(r.arg, "charge #7"));
    r = intel_intent("alarm 6:30", APPS);
    CHECK(r.kind == IN_ALARM && r.value == 630);
    r = intel_intent("wake me at 7am", APPS);
    CHECK(r.kind == IN_ALARM && r.value == 700);
    r = intel_intent("note swapped the FL module", APPS);
    CHECK(r.kind == IN_NOTE && !strcmp(r.arg, "swapped the FL module"));
    CHECK(intel_intent("why is the elevator hot?", APPS).kind == IN_NONE);
    CHECK(intel_intent("what's wrong", APPS).kind == IN_ROBOT);
    CHECK(intel_intent("", APPS).kind == IN_NONE && intel_intent(NULL, APPS).kind == IN_NONE);
    CHECK(intel_intent("how many matches until we queue for the next one if the field is late", APPS).kind == IN_NONE);
}

static void briefing(void)
{
    const time_t T = 1790000000;
    static intel_in_t in;
    base_in(&in, T);
    snprintf(in.event_key, sizeof in.event_key, "2026casj");
    snprintf(in.event_name, sizeof in.event_name, "Silicon Valley Regional");
    in.event_live = true;
    in.rank = 12;
    in.num_teams = 40;
    in.wins = 7;
    in.losses = 3;
    in.have_epa = true;
    in.epa = 31.2;
    in.percentile = 0.91;
    in.epa_rank = 312;
    in.epa_count = 3700;
    in.nup = 1;
    snprintf(in.up[0].label, sizeof in.up[0].label, "Q34");
    in.up[0].start = T + 1800;
    in.up[0].src = MT_SRC_NEXUS;
    in.up[0].ours = 1;
    in.up[0].partners[0] = 1234;
    in.up[0].opponents[0] = 111;
    in.up[0].win = 0.62;
    in.up[0].away = 3;
    snprintf(in.now_queuing, sizeof in.now_queuing, "Q31");
    in.behind_min = 8;
    snprintf(in.batt_line, sizeof in.batt_line, "battery: #7");
    static char out[3072];
    size_t l = intel_briefing(&in, out, sizeof out);
    CHECK(l > 0 && l == strlen(out));
    CHECK(strstr(out, "event: Silicon Valley Regional (today) \xc2\xb7 rank 12 of 40 (7-3-0)"));
    CHECK(strstr(out, "statbotics: EPA 31.2, #312 of 3700 (top 9%)"));
    CHECK(strstr(out, "next: Q34 ~") && strstr(out, "(nexus) \xc2\xb7 red with 1234 against 111 \xc2\xb7 62% to win"));
    CHECK(strstr(out, "queue: now queuing Q31 \xc2\xb7 Q34 is 3 matches after it \xc2\xb7 the field runs ~8 min behind (nexus)"));
    CHECK(strstr(out, "battery: #7") && strstr(out, "robot: not connected") && out[l - 1] != '\n');
    /* a tiny buffer never overflows */
    char tiny[16];
    intel_briefing(&in, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny);
}

void test_intel(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    labels();
    nexus();
    timing();
    statbotics();
    robot_net();
    rules();
    store();
    intents();
    briefing();
}
