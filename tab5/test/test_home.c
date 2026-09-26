/* Tests for home mode's two additions that don't need a card, a camera or a microphone to get right:
 * the timed photo's retention decision (home_capture_plan, home_capture_policy.c) and the room-occupied
 * hysteresis (home_presence_step, home_presence_policy.c). Called from test_main.c. */
#include "home.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the retention ring ---- */

static void capture_under_cap(void)
{
    /* five files, a ring of ten, plenty of free space: nothing to delete */
    home_cap_file_t f[5];
    for (int i = 0; i < 5; i++) { f[i].mtime = i; f[i].bytes = 100000; }
    bool del[5];
    int n = home_capture_plan(f, 5, 10, 1000000000ull, 1000000ull, del);
    CHECK(n == 0);
    for (int i = 0; i < 5; i++) CHECK(!del[i]);
}

static void capture_over_cap(void)
{
    /* ten files (mtime 0..9, arrival order shuffled), a ring of four: the six oldest go, the four newest
     * (mtime 6..9) stay, regardless of the order they were handed in */
    home_cap_file_t f[10];
    int order[10] = { 3, 7, 0, 9, 4, 1, 8, 5, 2, 6 };
    for (int i = 0; i < 10; i++) { f[i].mtime = order[i]; f[i].bytes = 1000; }
    bool del[10];
    int n = home_capture_plan(f, 10, 4, 1000000000ull, 0, del);
    CHECK(n == 6);
    int kept = 0;
    for (int i = 0; i < 10; i++) {
        bool should_keep = f[i].mtime >= 6;
        CHECK(del[i] == !should_keep);
        if (!del[i]) kept++;
    }
    CHECK(kept == 4);
}

static void capture_low_space(void)
{
    /* under the count cap, but free space is short: the oldest go until the margin is met, even though
     * the ring alone wouldn't have touched any of them */
    home_cap_file_t f[5];
    for (int i = 0; i < 5; i++) { f[i].mtime = i; f[i].bytes = 10 * 1024 * 1024; } /* 10 MB each */
    bool del[5];
    /* 5 MB free, wants 32 MB clear: needs at least 3 of the 10 MB files back (5 + 30 >= 32) */
    int n = home_capture_plan(f, 5, 10, 5ull * 1024 * 1024, 32ull * 1024 * 1024, del);
    CHECK(n == 3);
    CHECK(del[0] && del[1] && del[2] && !del[3] && !del[4]); /* oldest three, by mtime */
}

static void capture_cant_win(void)
{
    /* the margin still isn't met even after everything this feature owns is gone: it deletes all of it
     * (never anything it wasn't given) and stops there, rather than looping or crashing */
    home_cap_file_t f[3];
    for (int i = 0; i < 3; i++) { f[i].mtime = i; f[i].bytes = 1024; }
    bool del[3];
    int n = home_capture_plan(f, 3, 10, 0, 1000000000ull, del);
    CHECK(n == 3);
    for (int i = 0; i < 3; i++) CHECK(del[i]);
}

static void capture_empty(void)
{
    bool del[1];
    CHECK(home_capture_plan(NULL, 0, 10, 0, 0, del) == 0);
}

/* ---- the room-occupied hysteresis ---- */

#define QUIET_S 60.0
#define CONFIRM_S 3.0
#define THRESHOLD 0.1f

static void presence_starts_empty(void)
{
    home_presence_track_t t = { 0 };
    home_presence_step(&t, 0, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S);
    CHECK(t.state == HOME_PRESENCE_EMPTY); /* one quiet sample is enough: absence needs no confirming */
    CHECK(t.last_seen == 0);
}

static void presence_brief_noise_ignored(void)
{
    /* a burst shorter than CONFIRM_S (a door, a dropped book): never counts as occupied */
    home_presence_track_t t = { 0 };
    double now = 0;
    home_presence_step(&t, now, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S);
    now += 1.0;
    home_presence_step(&t, now, 0.9f, THRESHOLD, CONFIRM_S, QUIET_S); /* loud, but only for a moment */
    CHECK(t.state != HOME_PRESENCE_OCCUPIED);
    now += 1.5; /* < CONFIRM_S since the loud sample */
    home_presence_step(&t, now, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S);
    CHECK(t.state != HOME_PRESENCE_OCCUPIED);
}

static void presence_sustained_noise_confirms(void)
{
    home_presence_track_t t = { 0 };
    double now = 0;
    home_presence_step(&t, now, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S);
    CHECK(t.state == HOME_PRESENCE_EMPTY);
    for (now = 1.0; now <= 5.0; now += 1.0) home_presence_step(&t, now, 0.9f, THRESHOLD, CONFIRM_S, QUIET_S);
    CHECK(t.state == HOME_PRESENCE_OCCUPIED); /* 4 s of a loud room, past CONFIRM_S */
    CHECK(fabs(t.last_seen - now + 1.0) < 1e-6);
}

static void presence_holds_through_a_quiet_pause(void)
{
    /* occupied, then a pause in conversation shorter than QUIET_S: still occupied */
    home_presence_track_t t = { .state = HOME_PRESENCE_OCCUPIED, .last_seen = 10 };
    double now = 10;
    for (int i = 0; i < 5; i++) { now += 5; home_presence_step(&t, now, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S); }
    CHECK(t.state == HOME_PRESENCE_OCCUPIED); /* 25 s of quiet, well under QUIET_S = 60 */
}

static void presence_empties_after_sustained_quiet(void)
{
    home_presence_track_t t = { .state = HOME_PRESENCE_OCCUPIED, .last_seen = 0 };
    double now = 0;
    for (int i = 0; i < 13; i++) { now += 5; home_presence_step(&t, now, 0.01f, THRESHOLD, CONFIRM_S, QUIET_S); }
    CHECK(now - 0 >= QUIET_S); /* the test itself ran long enough to mean something */
    CHECK(t.state == HOME_PRESENCE_EMPTY);
}

void test_home(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    capture_under_cap();
    capture_over_cap();
    capture_low_space();
    capture_cant_win();
    capture_empty();
    presence_starts_empty();
    presence_brief_noise_ignored();
    presence_sustained_noise_confirms();
    presence_holds_through_a_quiet_pause();
    presence_empties_after_sustained_quiet();
}
