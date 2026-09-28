/* as_desk — what the assistant can read that lives above it, in the UI: the event's matches from The Blue
 * Alliance and the battery fleet. The UI posts each as text when it changes (assist_desk_post); the tools
 * get_matches and get_batteries, and the companion's context line, read the latest copy. One lock, copies
 * both ways: the worker never touches the UI's own state. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "as_json.h"
#include "as_tools.h"
#include "assist.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_text[AS_DESK_N];

void assist_desk_post(as_desk_t which, const char *text)
{
    if (which < 0 || which >= AS_DESK_N) return;
    char *c = text && text[0] ? as_strdup(text) : NULL;
    pthread_mutex_lock(&g_lock);
    bool same = (!c && !g_text[which]) || (c && g_text[which] && !strcmp(c, g_text[which]));
    if (!same) {
        free(g_text[which]);
        g_text[which] = c;
        c = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    free(c);
}

char *assist_desk_get(as_desk_t which)
{
    if (which < 0 || which >= AS_DESK_N) return NULL;
    pthread_mutex_lock(&g_lock);
    char *c = g_text[which] ? as_strdup(g_text[which]) : NULL;
    pthread_mutex_unlock(&g_lock);
    return c;
}

/* ---- The Blue Alliance reader, registered by whoever can see both components ---- */

static as_tba_read_fn g_tba;

void assist_set_tba_reader(as_tba_read_fn fn) { g_tba = fn; }
as_tba_read_fn assist_tba_reader(void) { return g_tba; }

void assist_fold_text(char *s) { as_fold_text(s); }

/* ---- the island's live activities, registered by the ui (as_tools.h has the calls) ---- */

static as_activity_hooks_t g_act;

void assist_set_activity_hooks(const as_activity_hooks_t *h)
{
    if (h) g_act = *h;
    else memset(&g_act, 0, sizeof g_act);
}

void as_act_turn(int pool, const char *doing)
{
    if (g_act.turn) g_act.turn(pool, doing);
}

int as_act_begin(const char *kind, const char *text)
{
    return g_act.begin ? g_act.begin(kind, text) : 0;
}

void as_act_update(int id, const char *text)
{
    if (id > 0 && g_act.update) g_act.update(id, text);
}

void as_act_end(int id, const char *said)
{
    if (id > 0 && g_act.end) g_act.end(id, said);
}

/* ---- the tablet's and the house's actions, registered by whoever can see the ui ---- */

static as_action_fn g_action;

void assist_set_action_handler(as_action_fn fn) { g_action = fn; }
as_action_fn assist_action_handler(void) { return g_action; }
