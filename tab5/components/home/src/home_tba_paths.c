/* The Blue Alliance paths the tablet will fetch on the assistant's behalf, and nothing else.
 *
 * Its own file, with no I/O in it, for one reason: this is the security boundary. `path` comes
 * from a language model, and what it selects is a request carrying our API key. A boundary that
 * cannot be unit-tested is a boundary nobody has checked -- so it lives here, beside the capture
 * and presence policies, and test/test_tba_paths.c exercises it. */
#include "tba.h"

#include <string.h>

/* A tiny matcher: %d a run of digits, %y exactly four, %e an event key (a year then 2..10 of
 * [a-z0-9]), %m a match key. Everything else is literal. */
static bool shape(const char *path, const char *pat)
{
    const char *p = path;
    for (const char *c = pat; *c; c++) {
        if (*c != '%') {
            if (*p++ != *c) return false;
            continue;
        }
        switch (*++c) {
        case 'd': {
            int k = 0;
            while (*p >= '0' && *p <= '9' && k < 6) { p++; k++; }
            if (!k) return false;
            break;
        }
        case 'y': {
            for (int k = 0; k < 4; k++) if (*p++ < '0' || p[-1] > '9') return false;
            break;
        }
        case 'e': {
            for (int k = 0; k < 4; k++) if (*p++ < '0' || p[-1] > '9') return false;
            int k = 0;
            while (((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) && k < 10) { p++; k++; }
            if (k < 2) return false;
            break;
        }
        case 'm': {
            int k = 0;
            while (((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
                   && k < 32) { p++; k++; }
            if (k < 3) return false;
            break;
        }
        default: return false;
        }
    }
    return *p == 0;
}

/* Read-only endpoints whose answers are small enough to be worth a screenful. Deliberately short:
 * a whitelist is only a whitelist while somebody has thought about each line on it. The /simple
 * variants are chosen wherever TBA offers one -- the full forms are several times the size for
 * fields nobody asked for. */
static const char *ALLOWED[] = {
    "/team/frc%d/simple",
    "/team/frc%d/events/%y/simple",
    "/team/frc%d/event/%e/status",
    "/team/frc%d/event/%e/matches/simple",
    "/team/frc%d/awards",
    "/event/%e/simple",
    "/event/%e/rankings",
    "/event/%e/oprs",
    "/event/%e/matches/simple",
    "/event/%e/teams/simple",
    "/events/%y/simple",
    "/match/%m/simple",
};

bool tba_read_allowed(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) > 120) return false;
    if (strstr(path, "..") || strchr(path, '?') || strchr(path, '#') || strchr(path, ' ')) return false;
    for (size_t i = 0; i < sizeof ALLOWED / sizeof *ALLOWED; i++)
        if (shape(path, ALLOWED[i])) return true;
    return false;
}

