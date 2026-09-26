/* Room-occupied signal from the microphones' ambient level (home.h): the hal-touching half.
 * home_presence_policy.c has the hysteresis itself, pure and unit-tested.
 *
 * This is a passive reader, nothing more: it never calls hal_mic_start() or hal_mic_stop(). Home mode's
 * own wake word (ui_home_mode_showing() driving voice_enable(), in ui_home_mode.c / assist's voice.c)
 * already keeps the microphones open the whole time home mode is on screen, listening for "Hi ESP" — so
 * presence has no reason to duty-cycle them itself, and an earlier version that did was a real bug, not a
 * tidiness point: it opened and closed the ES7210 every four seconds regardless of the wake word, which
 * shows up in the log as `audio: microphones on` / `off` a few thousand times an hour, and it was a second
 * owner of a codec the wake word already held.
 *
 * home_presence_tick(), called from ui_home_mode.c's frame hook (UI thread, not a worker: hal_mic_on() and
 * hal_mic_level() are cheap reads, never blocking, so this needs no thread of its own): once a poll, if the
 * microphones are already open for any reason, it samples the level they're already producing; if they are
 * not open, it takes no sample and waits for the next poll. When nothing is listening (the wake word off,
 * or home mode not showing), presence simply gets no fresh signal and its idle time grows, which falls back
 * safely to the ordinary touch-based dim/sleep timer (home_presence_idle_s, blended in by ui_shell.c). */
#include "home_priv.h"

#define POLL_S_DEFAULT 4.0        /* how often the room's level is sampled, when something already holds the mic */
#define CONFIRM_S_DEFAULT 2.5     /* > one poll: a single burst doesn't flip it to occupied */
#define QUIET_S_DEFAULT 120.0     /* two minutes of quiet before "the room's empty" */
#define THRESHOLD_DEFAULT 0.08f   /* hal_mic_level() is 0..1; a starting point, not a measured one */

typedef struct {
    bool active;               /* home mode is up (home_presence_set_active); gates whether we bother polling */
    float threshold;
    double poll_s, confirm_s, quiet_s;
    double next_poll;
    home_presence_track_t track;
} pres_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pres_t G = {
    .threshold = THRESHOLD_DEFAULT,
    .poll_s = POLL_S_DEFAULT,
    .confirm_s = CONFIRM_S_DEFAULT,
    .quiet_s = QUIET_S_DEFAULT,
};

void home_presence_config(float threshold01, double poll_s, double confirm_s, double quiet_s)
{
    pthread_mutex_lock(&g_lock);
    if (threshold01 > 0) G.threshold = threshold01;
    if (poll_s > 0) G.poll_s = poll_s;
    if (confirm_s > 0) G.confirm_s = confirm_s;
    if (quiet_s > 0) G.quiet_s = quiet_s;
    pthread_mutex_unlock(&g_lock);
}

home_presence_state_t home_presence_state(void)
{
    pthread_mutex_lock(&g_lock);
    home_presence_state_t s = G.track.state;
    pthread_mutex_unlock(&g_lock);
    return s;
}

double home_presence_last_seen(void)
{
    pthread_mutex_lock(&g_lock);
    double t = G.track.last_seen;
    pthread_mutex_unlock(&g_lock);
    return t;
}

double home_presence_idle_s(double now)
{
    pthread_mutex_lock(&g_lock);
    double seen = G.track.last_seen;
    pthread_mutex_unlock(&g_lock);
    return seen > 0 ? now - seen : 1e9; /* no signal yet: leave the touch-based idle timer alone */
}

/* home mode entering/leaving screen (ui_home_mode.c's go()): while inactive we don't even bother polling. */
void home_presence_set_active(bool home_active)
{
    pthread_mutex_lock(&g_lock);
    G.active = home_active;
    pthread_mutex_unlock(&g_lock);
}

/* UI thread, once a frame; internally throttled to poll_s. Never touches hal_mic_start/stop. */
void home_presence_tick(double now)
{
    pthread_mutex_lock(&g_lock);
    bool active = G.active;
    bool due = now >= G.next_poll;
    float threshold = G.threshold;
    double confirm_s = G.confirm_s, quiet_s = G.quiet_s;
    if (active && due) G.next_poll = now + G.poll_s;
    pthread_mutex_unlock(&g_lock);
    if (!active || !due) return;

    if (!hal_mic_on()) return; /* nothing already listening: no sample this round, and nothing to open */
    float level = hal_mic_level();

    pthread_mutex_lock(&g_lock);
    home_presence_step(&G.track, now, level, threshold, confirm_s, quiet_s);
    pthread_mutex_unlock(&g_lock);
}
