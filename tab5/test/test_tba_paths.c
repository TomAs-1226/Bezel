/* Tests for the Blue Alliance path whitelist (home_tba_paths.c), the one thing between a path a
 * language model produced and a request carrying our API key. Called from test_main.c. */
#include "tba.h"

#include <stdio.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *GOOD[] = {
    "/team/frc5805/simple",
    "/team/frc254/events/2026/simple",
    "/team/frc5805/event/2026casj/status",
    "/team/frc5805/event/2026casj/matches/simple",
    "/team/frc1678/awards",
    "/event/2026casj/simple",
    "/event/2026casj/rankings",
    "/event/2026casj/oprs",
    "/event/2026casj/matches/simple",
    "/event/2026casj/teams/simple",
    "/events/2026/simple",
    "/match/2026casj_qm34/simple",
};

static const char *BAD[] = {
    "",
    "/",
    "team/frc5805/simple",                   /* no leading slash */
    "/team/frc5805/simple/",                 /* a trailing slash is a different path */
    "/team/frc5805",                         /* the full team form is not on the list */
    "/team/frc5805/matches",
    "/event/2026casj/matches",               /* the full form, not /simple */
    "/event/2026casj/rankings?x=1",          /* a query string */
    "/event/../../admin",                    /* traversal */
    "/event/2026casj/rankings#frag",
    "/event/2026 casj/rankings",             /* a space */
    "/team/frcABC/simple",                   /* not digits */
    "/team/frc5805/event/CASJ/status",       /* an event key starts with a year */
    "/event/26casj/simple",                  /* too short a year */
    "/event/2026c/simple",                   /* one character of event code */
    "/status",
    "/TEAM/frc5805/simple",                  /* case matters */
    "/team/frc12345678/simple",              /* no team number is that long */
};

void test_tba_paths(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    for (size_t i = 0; i < sizeof GOOD / sizeof *GOOD; i++) {
        if (!tba_read_allowed(GOOD[i])) printf("      refused: %s\n", GOOD[i]);
        CHECK(tba_read_allowed(GOOD[i]));
    }
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        if (tba_read_allowed(BAD[i])) printf("      allowed: %s\n", BAD[i]);
        CHECK(!tba_read_allowed(BAD[i]));
    }
    CHECK(!tba_read_allowed(NULL));
}
