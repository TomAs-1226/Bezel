/* as_desk — what the assistant can read that lives above it, in the UI: the event's matches from The Blue
 * Alliance and the battery fleet. The UI posts each as text when it changes (assist_desk_post); the tools
 * get_matches and get_batteries, and the companion's context line, read the latest copy. One lock, copies
 * both ways: the worker never touches the UI's own state. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "as_json.h"
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
