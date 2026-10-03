/* The ask bar's words to what the tablet can do at once (intel.h). Pure: host-tested.
 *
 * Short and literal on purpose: a few dozen phrasings people actually type ("timer 5 min", "5 min timer tea",
 * "brightness 40", "next match", "battery for q34", "open weather"). Anything with more to it than that is a
 * question, and a question goes to an AI with the tools to answer it; guessing here would do the wrong
 * thing quickly, which is worse than the right thing a second later. */
#include "intel.h"
#include "intel_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* lower case, punctuation to spaces (':' kept for times), single spaces */
static void norm(const char *in, char *out, size_t n)
{
    size_t o = 0;
    bool sp = true;
    for (const char *p = in; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == ':' || c >= 0x80) {
            out[o++] = (char)tolower(c);
            sp = false;
        } else if (c == '\'' ) {
            continue; /* "what's" -> "whats" */
        } else if (!sp) {
            out[o++] = ' ';
            sp = true;
        }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static bool is(const char *s, const char *w) { return !strcmp(s, w); }
/* s begins with the word(s) w ("open " with its space: with more after it) */
static bool starts(const char *s, const char *w)
{
    size_t l = strlen(w);
    if (strncmp(s, w, l)) return false;
    return (l && w[l - 1] == ' ') || s[l] == 0 || s[l] == ' ';
}
/* the word w anywhere in s, whole */
static bool word(const char *s, const char *w)
{
    size_t l = strlen(w);
    for (const char *p = s; (p = strstr(p, w)); p++)
        if ((p == s || p[-1] == ' ') && (p[l] == 0 || p[l] == ' ')) return true;
    return false;
}

static int small_number(const char *w)
{
    static const char *const W[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten" };
    for (int i = 0; i <= 10; i++)
        if (!strcmp(w, W[i])) return i;
    if (!strcmp(w, "fifteen")) return 15;
    if (!strcmp(w, "twenty")) return 20;
    if (!strcmp(w, "thirty")) return 30;
    if (!strcmp(w, "forty")) return 40;
    if (!strcmp(w, "sixty")) return 60;
    if (!strcmp(w, "a") || !strcmp(w, "an")) return 1;
    return -1;
}

/* A duration anywhere in s: "5 min", "90s", "1 hour 30 minutes", "1h30", "2:30" (minutes:seconds), "half an
 * hour". Returns seconds (0 none); the span it took in *a..*b. */
static double duration(const char *s, const char **a, const char **b)
{
    double total = 0;
    const char *first = NULL, *last = NULL;
    const char *p = s;
    char nxt[16];
    if (strstr(s, "half an hour") || strstr(s, "half hour")) {
        const char *h = strstr(s, "half");
        if (a) *a = h;
        if (b) *b = h + (strstr(s, "half an hour") ? 12 : 9);
        return 1800;
    }
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ws = p;
        char tok[24];
        size_t k = 0;
        while (*p && *p != ' ' && k < sizeof tok - 1) tok[k++] = *p++;
        tok[k] = 0;
        while (*p && *p != ' ') p++;
        double v = -1;
        const char *unit = NULL;
        int mm, ss;
        char *e;
        if (sscanf(tok, "%d:%d", &mm, &ss) == 2 && ss < 60 && !strchr(tok + strcspn(tok, ":") + 1, ':')) {
            /* "2:30" as a length only after a timer word or before one; a clock time is the alarm's */
            total += mm * 60 + ss;
            if (!first) first = ws;
            last = p;
            continue;
        }
        if (isdigit((unsigned char)tok[0])) {
            v = strtod(tok, &e);
            unit = *e ? e : NULL;
        } else {
            int w = small_number(tok);
            if (w >= 0) v = w;
        }
        if (v < 0) continue;
        if (!unit) {
            /* the unit is the next word */
            const char *q = p;
            while (*q == ' ') q++;
            size_t j = 0;
            while (q[j] && q[j] != ' ' && j < sizeof nxt - 1) nxt[j] = q[j], j++;
            nxt[j] = 0;
            if (j) {
                unit = nxt;
                p = q + j;
            }
        }
        if (!unit) continue;
        double mul = 0;
        if (!strncmp(unit, "h", 1)) mul = 3600;
        else if (!strncmp(unit, "m", 1) && strncmp(unit, "ma", 2)) mul = 60; /* "min", "mins", "m"; not "match" */
        else if (!strncmp(unit, "s", 1) && strncmp(unit, "sf", 2)) mul = 1;
        if (!mul) continue;
        /* "1h30": the rest after an h */
        if (mul == 3600 && isdigit((unsigned char)unit[1])) total += atof(unit + 1) * 60;
        total += v * mul;
        if (!first) first = ws;
        last = p;
    }
    if (a) *a = first;
    if (b) *b = last;
    return total;
}

/* "14:30", "6:30", "7am", "7 pm", "at 7" -> hhmm; -1 none */
static int clock_time(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > s && isdigit((unsigned char)p[-1]))) continue;
        int h, m = 0, used = 0;
        if (sscanf(p, "%d:%d%n", &h, &m, &used) == 2 || (sscanf(p, "%d%n", &h, &used) == 1 && (p > s + 2 && !strncmp(p - 3, "at ", 3)))) {
            const char *after = p + used;
            while (*after == ' ') after++;
            if (!strncmp(after, "pm", 2) && h < 12) h += 12;
            if (!strncmp(after, "am", 2) && h == 12) h = 0;
            if (h >= 0 && h < 24 && m >= 0 && m < 60) return h * 100 + m;
        }
        int hh;
        if (sscanf(p, "%dam", &hh) == 1 && strstr(p, "am") == p + (hh >= 10 ? 2 : 1)) return (hh % 12) * 100;
        if (sscanf(p, "%dpm", &hh) == 1 && strstr(p, "pm") == p + (hh >= 10 ? 2 : 1)) return (hh % 12 + 12) * 100;
    }
    return -1;
}

/* the words left once the ones listed are taken out, for a timer's label ("tea") */
static void leftover(const char *s, const char *a, const char *b, char *out, size_t n)
{
    static const char *const STOP[] = { "timer", "set", "a", "an", "the", "for", "start", "countdown", "count",
                                        "down", "me", "please", "of", "on", "minute", "minutes", "min", "mins", NULL };
    out[0] = 0;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", s);
    if (a && b && a >= s && b >= a) memset(buf + (a - s), ' ', (size_t)(b - a));
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
        bool stop = false;
        for (int i = 0; STOP[i]; i++)
            if (!strcmp(t, STOP[i])) stop = true;
        if (stop) continue;
        ix_addf(out, n, "%s%s", out[0] ? " " : "", t);
    }
}

/* a match label in the words: "q34", "qual 34", "sf5", "f2" -> "Q34" */
static bool match_label(const char *s, char *out, size_t n)
{
    for (const char *p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        int v;
        if (sscanf(p, "q%d", &v) == 1 || sscanf(p, "qm%d", &v) == 1 || sscanf(p, "qual %d", &v) == 1 ||
            sscanf(p, "quals %d", &v) == 1 || sscanf(p, "match %d", &v) == 1) {
            snprintf(out, n, "Q%d", v);
            return true;
        }
        if (sscanf(p, "sf%d", &v) == 1) { snprintf(out, n, "SF%d", v); return true; }
        if (sscanf(p, "f%d", &v) == 1 && p[1] >= '0' && p[1] <= '9') { snprintf(out, n, "F%d", v); return true; }
    }
    return false;
}

intel_intent_t intel_intent(const char *text, const char *const *apps)
{
    intel_intent_t r;
    memset(&r, 0, sizeof r);
    char s[160];
    norm(text ? text : "", s, sizeof s);
    if (!s[0]) return r;
    const char *a = NULL, *b = NULL;

    /* reminders first: "remind me in 10 min to call" has a duration and words that belong to it */
    if (starts(s, "remind me") || starts(s, "reminder")) {
        const char *to = strstr(s, " to ");
        int at = strstr(s, " at ") ? clock_time(strstr(s, " at ")) : -1;
        double secs = at >= 0 ? 0 : duration(s, &a, &b); /* "at 14:30" is a time of day, not 14 min 30 s */
        if ((secs > 0 || at >= 0) && to) {
            r.kind = IN_REMIND;
            r.seconds = secs;
            r.value = secs > 0 ? -1 : at;
            /* the words as typed ("#7" stays), after the first " to " */
            const char *w = text;
            for (const char *p = text; *p; p++)
                if (!strncasecmp(p, " to ", 4)) {
                    w = p + 4;
                    break;
                }
            snprintf(r.arg, sizeof r.arg, "%s", w != text ? w : to + 4);
            return r;
        }
    }
    /* alarms: "alarm 6:30", "wake me at 7am", "set an alarm for 6:45" */
    if (word(s, "alarm") || starts(s, "wake me")) {
        int t = clock_time(s);
        if (t >= 0) {
            r.kind = IN_ALARM;
            r.value = t;
            return r;
        }
    }
    /* timers */
    if (word(s, "stopwatch")) {
        r.kind = IN_STOPWATCH;
        return r;
    }
    if (word(s, "timer") || starts(s, "countdown") || starts(s, "count down")) {
        double secs = duration(s, &a, &b);
        if (secs > 0 && secs <= 86400) {
            r.kind = IN_TIMER;
            r.seconds = secs;
            leftover(s, a, b, r.arg, sizeof r.arg);
            if (strlen(r.arg) > 24) r.arg[24] = 0;
            return r;
        }
    }
    /* "5 min" alone is a timer too */
    {
        double secs = duration(s, &a, &b);
        if (secs > 0 && a == s && b && !*b) {
            r.kind = IN_TIMER;
            r.seconds = secs;
            return r;
        }
    }
    /* the screen and the sound */
    if (starts(s, "brightness") || starts(s, "screen brightness") || is(s, "brighter") || is(s, "dimmer") ||
        is(s, "dim")) {
        r.kind = IN_BRIGHT;
        int v;
        const char *p = s + strcspn(s, "0123456789");
        if (*p && sscanf(p, "%d", &v) == 1) {
            r.value = v > 100 ? 100 : v;
        } else if (word(s, "max") || word(s, "full")) {
            r.value = 100;
        } else if (word(s, "up") || is(s, "brighter")) {
            r.value = 20;
            r.delta = true;
        } else if (word(s, "down") || is(s, "dimmer") || is(s, "dim")) {
            r.value = -20;
            r.delta = true;
        } else {
            r.kind = IN_NONE;
        }
        return r;
    }
    if (starts(s, "volume") || is(s, "louder") || is(s, "quieter") || is(s, "softer")) {
        r.kind = IN_VOLUME;
        int v;
        const char *p = s + strcspn(s, "0123456789");
        if (*p && sscanf(p, "%d", &v) == 1) r.value = v > 100 ? 100 : v;
        else if (word(s, "up") || is(s, "louder")) r.value = 20, r.delta = true;
        else if (word(s, "down") || is(s, "quieter") || is(s, "softer")) r.value = -20, r.delta = true;
        else if (word(s, "off")) r.value = 0;
        else r.kind = IN_NONE;
        return r;
    }
    if (is(s, "mute") || is(s, "silence") || is(s, "unmute")) {
        r.kind = IN_MUTE;
        r.value = is(s, "unmute") ? 0 : 1;
        return r;
    }
    if (is(s, "dark") || is(s, "dark mode") || is(s, "light mode") || is(s, "light tone") || is(s, "dark tone")) {
        r.kind = IN_TONE;
        snprintf(r.arg, sizeof r.arg, "%s", starts(s, "dark") ? "dark" : "light");
        return r;
    }
    if (is(s, "home mode") || is(s, "desk") || is(s, "desk mode")) {
        r.kind = IN_HOME;
        return r;
    }
    if (is(s, "sleep") || is(s, "screen off") || is(s, "lock") || is(s, "lock screen")) {
        r.kind = IN_SLEEP;
        snprintf(r.arg, sizeof r.arg, "%s", starts(s, "lock") ? "lock" : "sleep");
        return r;
    }
    /* the event */
    if (is(s, "next") || is(s, "next match") || starts(s, "when do we play") || starts(s, "whens our next") ||
        starts(s, "when is our next") || is(s, "our next match") || is(s, "when do we queue")) {
        r.kind = IN_NEXT;
        return r;
    }
    if (starts(s, "battery") || starts(s, "which battery") || starts(s, "what battery")) {
        if (word(s, "battery") && (word(s, "for") || is(s, "battery") || starts(s, "which battery") ||
                                   starts(s, "what battery") || word(s, "next"))) {
            r.kind = IN_BATTERY;
            match_label(s, r.arg, sizeof r.arg);
            return r;
        }
    }
    if (is(s, "queue") || is(s, "queuing") || is(s, "nexus") || starts(s, "whats queuing") ||
        starts(s, "what is queuing") || is(s, "now queuing") || is(s, "the queue")) {
        r.kind = IN_QUEUE;
        return r;
    }
    if (is(s, "epa") || is(s, "statbotics") || is(s, "rank") || is(s, "our rank") || is(s, "ranking") ||
        starts(s, "how are we doing")) {
        r.kind = IN_EPA;
        return r;
    }
    if (starts(s, "brief") || is(s, "briefing") || is(s, "morning briefing") || is(s, "daily briefing") ||
        is(s, "the briefing")) {
        r.kind = IN_BRIEF;
        return r;
    }
    if (is(s, "insights") || is(s, "whats up") || is(s, "anything new") || starts(s, "what did i miss") ||
        is(s, "what should i know") || is(s, "noticed")) {
        r.kind = IN_INSIGHTS;
        return r;
    }
    if (is(s, "weather") || is(s, "weather today") || is(s, "forecast")) {
        r.kind = IN_WEATHER;
        return r;
    }
    if (is(s, "triage") || is(s, "robot status") || is(s, "whats wrong") || is(s, "whats wrong with the robot") ||
        is(s, "robot health")) {
        r.kind = IN_ROBOT;
        return r;
    }
    if (starts(s, "note ") || starts(s, "add note ") || starts(s, "add a note ") || starts(s, "jot ")) {
        const char *t = strchr(text, ' ');
        if (starts(s, "add note ") || starts(s, "add a note ")) t = strstr(text, "note") ? strstr(text, "note") + 4 : t;
        while (t && *t == ' ') t++;
        if (t && *t) {
            r.kind = IN_NOTE;
            snprintf(r.arg, sizeof r.arg, "%s", t);
            return r;
        }
    }
    /* an app: "open weather", "show the batteries", or its name alone */
    const char *name = s;
    if (starts(s, "open ")) name = s + 5;
    else if (starts(s, "show ")) name = s + 5;
    else if (starts(s, "go to ")) name = s + 6;
    else if (starts(s, "launch ")) name = s + 7;
    if (starts(name, "the ")) name += 4;
    if (starts(name, "my ")) name += 3;
    for (int i = 0; apps && apps[i]; i++) {
        char an[40];
        norm(apps[i], an, sizeof an);
        if (!strcmp(name, an)) {
            r.kind = IN_OPEN;
            snprintf(r.arg, sizeof r.arg, "%s", apps[i]);
            return r;
        }
    }
    if (name != s && name[0]) {
        /* "open settings look": let the tool look for it */
        r.kind = IN_OPEN;
        snprintf(r.arg, sizeof r.arg, "%s", name);
        return r;
    }
    return r;
}
