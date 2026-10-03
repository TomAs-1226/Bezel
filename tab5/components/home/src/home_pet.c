/* home_pet — the model. See home_pet.h. */
#include "home_pet.h"

#include "json_lite.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const PET_FOOD_NAME[PET_FOODS] = { "berry", "rice ball", "fish", "cake" };

#define DAY 86400
#define STEP_S 600            /* the catch-up works in ten-minute steps */
#define AWAY_MAX_S (7 * DAY)  /* a week away counts as a week */
#define CLOCK_OK 1609459200LL /* 2021: before it, the clock was never set */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void home_pet_init(home_pet_t *p, int64_t now)
{
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "Tabi");
    p->hunger = 0.25f;
    p->energy = 0.9f;
    p->mood = 0.7f;
    p->affection = 0.3f;
    p->born = p->clock = now;
    home_pet_note(p, now, "hatched");
}

void home_pet_note(home_pet_t *p, int64_t now, const char *text)
{
    if (!text || !text[0]) return;
    if (p->ndiary && !strcmp(p->diary[p->ndiary - 1].text, text) && now - p->diary[p->ndiary - 1].at < 600) return;
    if (p->ndiary == PET_DIARY) {
        memmove(p->diary, p->diary + 1, sizeof p->diary[0] * (PET_DIARY - 1));
        p->ndiary--;
    }
    pet_note_t *n = &p->diary[p->ndiary++];
    n->at = now;
    snprintf(n->text, sizeof n->text, "%s", text);
    for (char *c = n->text; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 32) *c = '\'';
}

static int local_hour(int64_t t, int tz_s)
{
    int64_t l = t + tz_s;
    return (int)(((l % DAY) + DAY) % DAY / 3600);
}

static int local_day(int64_t t, int tz_s) { return (int)((t + tz_s) / DAY); }

static bool is_night(int64_t t, int tz_s)
{
    int h = local_hour(t, tz_s);
    return h >= 22 || h < 7;
}

static void hungry_note(home_pet_t *p, int64_t t)
{
    if (p->hunger > 0.7f && t - p->note_hunger_at > 6 * 3600) {
        p->note_hunger_at = t;
        home_pet_note(p, t, "got hungry");
    }
}

/* one step of dt seconds ending at t */
static void step(home_pet_t *p, int64_t t, double dt, int tz_s)
{
    float h = (float)(dt / 3600.0);
    bool night = is_night(t, tz_s) && t >= p->up_until;
    bool was = p->asleep;
    if (p->asleep) {
        p->hunger += 0.02f * h;
        p->energy += 0.14f * h;
        if (!night && p->energy > 0.8f && !(p->up_until > t)) p->asleep = false;
    } else {
        p->hunger += 0.065f * h;
        p->energy -= 0.045f * h;
        if (night || p->energy < 0.12f) p->asleep = true;
    }
    p->hunger = clampf(p->hunger, 0, 0.92f);
    p->energy = clampf(p->energy, 0.08f, 1);
    p->affection = clampf(p->affection - 0.006f * h, 0.1f, 1);
    float target = 0.75f - 0.55f * clampf(p->hunger - 0.4f, 0, 1) - 0.35f * clampf(0.35f - p->energy, 0, 1) +
                   0.25f * (p->affection - 0.3f);
    target = clampf(target, 0.05f, 0.95f);
    float k = clampf(h / 3.0f, 0, 1);
    p->mood += (target - p->mood) * k;
    if (p->asleep != was) home_pet_note(p, t, p->asleep ? "went to sleep" : "woke up");
    hungry_note(p, t);
}

void home_pet_advance(home_pet_t *p, int64_t now, int tz_s, double quiet_s)
{
    if (now < CLOCK_OK) return;
    if (p->clock < CLOCK_OK || p->clock > now) {
        p->clock = now; /* never ticked, or the clock moved back: begin again from here */
        return;
    }
    int64_t dt = now - p->clock;
    if (dt <= 0) return;
    int64_t away = dt;
    if (dt > AWAY_MAX_S) {
        p->clock = now - AWAY_MAX_S;
        dt = AWAY_MAX_S;
    }
    while (p->clock < now) {
        int64_t d = now - p->clock;
        if (d > STEP_S) d = STEP_S;
        p->clock += d;
        step(p, p->clock, (double)d, tz_s);
    }
    if (away > 12 * 3600) {
        /* a long time alone: groggy on coming back, and it says so in the diary */
        char s[48];
        int days = (int)(away / DAY);
        if (days >= 1) snprintf(s, sizeof s, "was alone for %d day%s", days, days == 1 ? "" : "s");
        else snprintf(s, sizeof s, "was alone for %d hours", (int)(away / 3600));
        home_pet_note(p, now, s);
        if (p->energy > 0.45f) p->energy = 0.45f;
        p->mood = clampf(p->mood, 0.2f, 0.6f);
    }
    /* the room has been quiet a long while and it is tired: a doze; someone back wakes it */
    bool night = is_night(now, tz_s) && now >= p->up_until;
    if (!p->asleep && quiet_s > 1200 && p->energy < 0.7f) {
        p->asleep = true;
        home_pet_note(p, now, "dozed off");
    } else if (p->asleep && !night && quiet_s < 20 && p->energy > 0.25f && now >= p->up_until - 1200) {
        p->asleep = false;
    }
    int days = home_pet_age_days(p, now);
    static int last_birthday; /* a note per tens of days, once per boot is enough */
    if (days > 0 && days % 10 == 0 && days != last_birthday) {
        char s[32];
        snprintf(s, sizeof s, "%d days old", days);
        home_pet_note(p, now, s);
        last_birthday = days;
    }
}

bool home_pet_feed(home_pet_t *p, int food, int64_t now, int tz_s, char *say, size_t n)
{
    static const float FEED[PET_FOODS] = { 0.20f, 0.40f, 0.55f, 0.15f };
    static const float MOOD[PET_FOODS] = { 0.04f, 0.05f, 0.07f, 0.18f };
    if (food < 0 || food >= PET_FOODS) food = PET_RICE;
    if (p->asleep) {
        snprintf(say, n, "%s is asleep", p->name);
        return false;
    }
    if (p->hunger < 0.10f) {
        snprintf(say, n, "%s isn't hungry", p->name);
        return false;
    }
    if (food == PET_CAKE) {
        int day = local_day(now, tz_s);
        if (p->treat_day != day) {
            p->treat_day = day;
            p->treats = 0;
        }
        if (p->treats >= 2) {
            snprintf(say, n, "%s has had enough treats today", p->name);
            return false;
        }
        p->treats++;
    }
    p->hunger = clampf(p->hunger - FEED[food], 0, 1);
    p->mood = clampf(p->mood + MOOD[food], 0, 1);
    p->affection = clampf(p->affection + 0.01f, 0, 1);
    p->fed_at = now;
    char s[48];
    if (food == PET_CAKE) snprintf(s, sizeof s, "had some cake");
    else if (food == PET_FISH) snprintf(s, sizeof s, "ate a fish");
    else snprintf(s, sizeof s, "ate a %s", PET_FOOD_NAME[food]);
    home_pet_note(p, now, s);
    snprintf(say, n, "%s ate the %s", p->name, PET_FOOD_NAME[food]);
    return true;
}

bool home_pet_play(home_pet_t *p, int64_t now, char *say, size_t n)
{
    if (p->asleep) {
        snprintf(say, n, "%s is asleep", p->name);
        return false;
    }
    if (p->energy < 0.18f) {
        snprintf(say, n, "%s is too sleepy to play", p->name);
        return false;
    }
    p->energy = clampf(p->energy - 0.10f, 0, 1);
    p->mood = clampf(p->mood + 0.20f, 0, 1);
    p->affection = clampf(p->affection + 0.05f, 0, 1);
    p->hunger = clampf(p->hunger + 0.03f, 0, 0.92f);
    p->played_at = now;
    home_pet_note(p, now, "played");
    snprintf(say, n, "%s had a good play", p->name);
    return true;
}

void home_pet_stroke(home_pet_t *p, int64_t now)
{
    p->affection = clampf(p->affection + 0.012f, 0, 1);
    p->mood = clampf(p->mood + 0.015f, 0, 1);
    if (now - p->petted_at > 3600) home_pet_note(p, now, "was petted");
    p->petted_at = now;
}

bool home_pet_set_asleep(home_pet_t *p, bool asleep, int64_t now, int tz_s, char *say, size_t n)
{
    if (asleep == p->asleep) {
        snprintf(say, n, "%s is already %s", p->name, asleep ? "asleep" : "awake");
        return false;
    }
    p->asleep = asleep;
    if (!asleep && is_night(now, tz_s)) p->up_until = now + 1200;
    if (asleep) p->up_until = 0;
    home_pet_note(p, now, asleep ? "was put to bed" : "was woken");
    snprintf(say, n, "%s is %s", p->name, asleep ? "asleep" : "awake");
    return true;
}

bool home_pet_rename(home_pet_t *p, const char *name, int64_t now)
{
    char s[PET_NAME_MAX];
    size_t k = 0;
    while (name && *name == ' ') name++;
    for (; name && *name && k < sizeof s - 1; name++) {
        unsigned char c = (unsigned char)*name;
        if (c < 32 || c == '"' || c == '\\') continue;
        s[k++] = (char)c;
    }
    while (k && s[k - 1] == ' ') k--;
    if (!k) return false;
    /* never cut a character in two */
    while (k && ((unsigned char)s[k - 1] & 0xC0) == 0x80 && k == sizeof s - 1) k--;
    s[k] = 0;
    if (!strcmp(s, p->name)) return true;
    char old[PET_NAME_MAX];
    snprintf(old, sizeof old, "%s", p->name);
    snprintf(p->name, sizeof p->name, "%s", s);
    char d[72];
    snprintf(d, sizeof d, "%s is now called %s", old, s);
    home_pet_note(p, now, d);
    return true;
}

int home_pet_age_days(const home_pet_t *p, int64_t now)
{
    return now > p->born ? (int)((now - p->born) / DAY) : 0;
}

pet_feel_t home_pet_feel(const home_pet_t *p)
{
    if (p->asleep) return PET_FEEL_ASLEEP;
    if (p->hunger > 0.62f) return PET_FEEL_HUNGRY;
    if (p->energy < 0.22f) return PET_FEEL_SLEEPY;
    if (p->mood < 0.3f) return PET_FEEL_SAD;
    if (p->mood > 0.85f && p->affection > 0.6f) return PET_FEEL_LOVE;
    if (p->mood > 0.7f) return PET_FEEL_HAPPY;
    return PET_FEEL_CALM;
}

void home_pet_words(const home_pet_t *p, char *out, size_t n)
{
    if (p->asleep) {
        snprintf(out, n, "asleep");
        return;
    }
    const char *a = NULL, *b = NULL;
    if (p->hunger > 0.78f) a = "very hungry";
    else if (p->hunger > 0.55f) a = "hungry";
    else if (p->hunger > 0.4f) a = "a little peckish";
    if (p->energy < 0.2f) b = a ? "sleepy" : "worn out";
    else if (p->energy < 0.4f) b = a ? "a little sleepy" : "getting sleepy";
    if (a && b) snprintf(out, n, "%s and %s", a, b);
    else if (a) snprintf(out, n, "%s", a);
    else if (b) snprintf(out, n, "%s", b);
    else if (p->mood > 0.85f) snprintf(out, n, "delighted");
    else if (p->mood > 0.65f) snprintf(out, n, "happy");
    else if (p->mood > 0.4f) snprintf(out, n, "content");
    else if (p->mood > 0.25f) snprintf(out, n, "a bit flat");
    else snprintf(out, n, "down");
}

static void ago(int64_t now, int64_t then, char *out, size_t n)
{
    if (!then) {
        snprintf(out, n, "never");
        return;
    }
    int64_t d = now - then;
    if (d < 90) snprintf(out, n, "just now");
    else if (d < 3600) snprintf(out, n, "%d min ago", (int)(d / 60));
    else if (d < 2 * DAY) snprintf(out, n, "%d h ago", (int)(d / 3600));
    else snprintf(out, n, "%d days ago", (int)(d / DAY));
}

void home_pet_brief(home_pet_t *p, int64_t now, char *out, size_t n)
{
    char w[40], f[24], line[260];
    home_pet_words(p, w, sizeof w);
    ago(now, p->fed_at, f, sizeof f);
    int len = snprintf(line, sizeof line, "[the pet on this desk: %s, %d days old, %s; hunger %d%%, energy %d%%; fed %s",
                       p->name, home_pet_age_days(p, now), w, (int)(p->hunger * 100 + 0.5f),
                       (int)(p->energy * 100 + 0.5f), f);
    int k = p->ndiary;
    if (k > 3) k = 3;
    if (k) {
        len += snprintf(line + len, sizeof line - (size_t)len, "; lately:");
        for (int i = p->ndiary - k; i < p->ndiary && len < (int)sizeof line - 40; i++) {
            char a[24];
            ago(now, p->diary[i].at, a, sizeof a);
            len += snprintf(line + len, sizeof line - (size_t)len, " %s (%s)%s", p->diary[i].text, a,
                            i + 1 < p->ndiary ? "," : "");
        }
    }
    bool mention = !p->asleep && p->hunger > 0.6f && now - p->mentioned_at > 3 * 3600;
    if (mention) {
        p->mentioned_at = now;
        snprintf(line + len, sizeof line - (size_t)len, "; it is hungry: mention it once, lightly, only if it fits]");
    } else {
        snprintf(line + len, sizeof line - (size_t)len, "]");
    }
    snprintf(out, n, "%s", line);
}

/* ================================================================== the file */

size_t home_pet_to_json(const home_pet_t *p, char *out, size_t n)
{
    size_t k = 0;
#define A(...) do { if (k < n) { int r = snprintf(out + k, n - k, __VA_ARGS__); if (r > 0) k += (size_t)r; } } while (0)
    A("{\"version\":1,\"name\":\"%s\",\"hunger\":%.3f,\"energy\":%.3f,\"mood\":%.3f,\"affection\":%.3f,"
      "\"asleep\":%d,\"born\":%lld,\"clock\":%lld,\"fed\":%lld,\"played\":%lld,\"petted\":%lld,\"up\":%lld,"
      "\"mentioned\":%lld,\"hungry_note\":%lld,\"treats\":%d,\"treat_day\":%d,\"diary\":[",
      p->name, p->hunger, p->energy, p->mood, p->affection, p->asleep ? 1 : 0, (long long)p->born,
      (long long)p->clock, (long long)p->fed_at, (long long)p->played_at, (long long)p->petted_at,
      (long long)p->up_until, (long long)p->mentioned_at, (long long)p->note_hunger_at, p->treats, p->treat_day);
    for (int i = 0; i < p->ndiary; i++)
        A("%s{\"at\":%lld,\"text\":\"%s\"}", i ? "," : "", (long long)p->diary[i].at, p->diary[i].text);
    A("]}\n");
#undef A
    return k < n ? k : n - 1;
}

bool home_pet_from_json(home_pet_t *p, const char *js, size_t len)
{
    jl_tok_t *tk = malloc(sizeof *tk * 200);
    if (!tk) return false;
    int nt = jl_parse(js, len, tk, 200);
    bool ok = nt > 0 && tk[0].type == JL_OBJ;
    if (ok) {
        jl_doc_t D = { js, tk, nt };
        home_pet_t q;
        memset(&q, 0, sizeof q);
        if (!jl_str(&D, jl_get(&D, 0, "name"), q.name, sizeof q.name) || !q.name[0]) snprintf(q.name, sizeof q.name, "Tabi");
        q.hunger = clampf((float)jl_num(&D, jl_get(&D, 0, "hunger"), 0.3), 0, 0.92f);
        q.energy = clampf((float)jl_num(&D, jl_get(&D, 0, "energy"), 0.8), 0.08f, 1);
        q.mood = clampf((float)jl_num(&D, jl_get(&D, 0, "mood"), 0.6), 0, 1);
        q.affection = clampf((float)jl_num(&D, jl_get(&D, 0, "affection"), 0.3), 0, 1);
        q.asleep = jl_num(&D, jl_get(&D, 0, "asleep"), 0) != 0;
        q.born = (int64_t)jl_num(&D, jl_get(&D, 0, "born"), 0);
        q.clock = (int64_t)jl_num(&D, jl_get(&D, 0, "clock"), 0);
        q.fed_at = (int64_t)jl_num(&D, jl_get(&D, 0, "fed"), 0);
        q.played_at = (int64_t)jl_num(&D, jl_get(&D, 0, "played"), 0);
        q.petted_at = (int64_t)jl_num(&D, jl_get(&D, 0, "petted"), 0);
        q.up_until = (int64_t)jl_num(&D, jl_get(&D, 0, "up"), 0);
        q.mentioned_at = (int64_t)jl_num(&D, jl_get(&D, 0, "mentioned"), 0);
        q.note_hunger_at = (int64_t)jl_num(&D, jl_get(&D, 0, "hungry_note"), 0);
        q.treats = (int)jl_num(&D, jl_get(&D, 0, "treats"), 0);
        q.treat_day = (int)jl_num(&D, jl_get(&D, 0, "treat_day"), 0);
        int d = jl_get(&D, 0, "diary");
        for (int i = 0; d >= 0 && i < PET_DIARY; i++) {
            int e = jl_at(&D, d, i);
            if (e < 0) break;
            pet_note_t *nn = &q.diary[q.ndiary];
            nn->at = (int64_t)jl_num(&D, jl_get(&D, e, "at"), 0);
            if (jl_str(&D, jl_get(&D, e, "text"), nn->text, sizeof nn->text)) q.ndiary++;
        }
        if (q.born <= 0) ok = false;
        else *p = q;
    }
    free(tk);
    return ok;
}
