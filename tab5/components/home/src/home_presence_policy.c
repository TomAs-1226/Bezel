/* The room-occupied hysteresis (home.h): pure, no I/O, no hal — the part worth testing without a
 * microphone. See home_presence.c for the half that actually duty-cycles the mic. */
#include "home.h"

void home_presence_step(home_presence_track_t *t, double now, float level, float threshold, double confirm_s,
                        double quiet_s)
{
    bool above = level >= threshold;
    if (above) {
        if (t->above_since == 0) t->above_since = now;
        t->below_since = 0;
        t->last_seen = now;
    } else {
        if (t->below_since == 0) t->below_since = now;
        t->above_since = 0;
    }

    if (above) {
        /* only the EMPTY -> OCCUPIED transition is considered here: a run of loud samples has to last
         * confirm_s before it counts, so one loud moment doesn't flip it */
        if (t->state != HOME_PRESENCE_OCCUPIED && now - t->above_since >= confirm_s)
            t->state = HOME_PRESENCE_OCCUPIED;
    } else {
        /* a fresh track (never decided anything yet) calls its first quiet sample EMPTY outright — absence
         * is the default that needs no confirming; once OCCUPIED, a quiet_s run of quiet is required before
         * it lets go, so one pause in conversation doesn't either */
        if (t->state == HOME_PRESENCE_UNKNOWN || now - t->below_since >= quiet_s)
            t->state = HOME_PRESENCE_EMPTY;
    }
}
