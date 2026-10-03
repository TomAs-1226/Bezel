/* home_pet — the desk pet's life: hunger, energy, mood, affection, age, a name and a small diary. No UI.
 *
 * It advances by the clock, not by frames: a tablet that was off for a weekend catches up from the RTC at
 * boot, clamped so a week away leaves it hungry and groggy and never dead (a desk companion, not a guilt
 * machine). It sleeps at night and dozes when the room has been quiet for long. The state is one small JSON
 * file on the card (CATOS/PET/state.json), written through a temp file and a rename, with the kv store as the
 * fallback when no card is in (home_pet_store.c). The model here is pure and host-tested (test/test_pet.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_DIARY 12
#define PET_NAME_MAX 24

typedef struct {
    int64_t at;
    char text[72];
} pet_note_t;

typedef struct {
    char name[PET_NAME_MAX];
    float hunger;    /* 0 full .. 1 starving (never above 0.92) */
    float energy;    /* 1 rested .. 0 spent (never below 0.08) */
    float mood;      /* 0 low .. 1 happy */
    float affection; /* 0 distant .. 1 devoted */
    bool asleep;
    int64_t born, clock;                  /* unix seconds; clock: how far the state has been advanced */
    int64_t fed_at, played_at, petted_at; /* 0 never */
    int64_t up_until;                     /* woken by hand at night: stays up until then */
    int64_t mentioned_at;                 /* the companion last brought up its hunger */
    int64_t note_hunger_at;               /* the diary's last "got hungry" */
    int treats, treat_day;                /* treats given on this local day */
    int ndiary;                           /* newest last */
    pet_note_t diary[PET_DIARY];
} home_pet_t;

typedef enum { PET_FEEL_CALM, PET_FEEL_HAPPY, PET_FEEL_LOVE, PET_FEEL_HUNGRY, PET_FEEL_SLEEPY, PET_FEEL_SAD,
               PET_FEEL_ASLEEP } pet_feel_t;

typedef enum { PET_BERRY, PET_RICE, PET_FISH, PET_CAKE, PET_FOODS } pet_food_t;
extern const char *const PET_FOOD_NAME[PET_FOODS];

void home_pet_init(home_pet_t *p, int64_t now);
/* The state brought up to `now` (unix seconds). tz_s: the local zone, seconds east of UTC. quiet_s: how long
 * the room has been quiet (large when unknown). A clock that went backwards, or was never set (now before
 * 2021), changes nothing. */
void home_pet_advance(home_pet_t *p, int64_t now, int tz_s, double quiet_s);

/* The owner's actions. Each says in a few words what happened (or why not) and returns whether it did. */
bool home_pet_feed(home_pet_t *p, int food, int64_t now, int tz_s, char *say, size_t n);
bool home_pet_play(home_pet_t *p, int64_t now, char *say, size_t n);
void home_pet_stroke(home_pet_t *p, int64_t now);
/* sleep or wake by hand; waking at night keeps it up for 20 minutes */
bool home_pet_set_asleep(home_pet_t *p, bool asleep, int64_t now, int tz_s, char *say, size_t n);
bool home_pet_rename(home_pet_t *p, const char *name, int64_t now);
void home_pet_note(home_pet_t *p, int64_t now, const char *text);

int home_pet_age_days(const home_pet_t *p, int64_t now);
pet_feel_t home_pet_feel(const home_pet_t *p);
/* "hungry and a little sleepy" */
void home_pet_words(const home_pet_t *p, char *out, size_t n);
/* The companion's line: the state and the last few diary entries, compact. Sets *mention when the pet's hunger
 * may be brought up (once in a few hours), and counts that as the mention. */
void home_pet_brief(home_pet_t *p, int64_t now, char *out, size_t n);

/* The state file */
size_t home_pet_to_json(const home_pet_t *p, char *out, size_t n);
bool home_pet_from_json(home_pet_t *p, const char *js, size_t len);

/* ---- the one pet (home_pet_store.c): thread-safe, kept on the card ---- */
void home_pet_boot(void);                 /* UI thread, after the clock is known: loads and catches up */
void home_pet_tick(double now);           /* UI thread, ~10 Hz: advances, writes when it should */
void home_pet_get(home_pet_t *out);       /* a copy */
/* the actions on the one pet, thread-safe; `say` as above */
bool home_pet_do_feed(int food, char *say, size_t n);
bool home_pet_do_play(char *say, size_t n);
void home_pet_do_stroke(void);
bool home_pet_do_sleep(bool asleep, char *say, size_t n);
bool home_pet_do_rename(const char *name);
void home_pet_do_note(const char *text);
/* the AI's tools: pet_status, pet_feed {food}, pet_play, pet_rename {name}; false for another name */
bool home_pet_is_tool(const char *name);
bool home_pet_tool(const char *name, const char *args_json, char *out, size_t n);
/* the companion's context line (an empty string with no pet) */
void home_pet_context(char *out, size_t n);
unsigned home_pet_rev(void);              /* changes whenever the pet does */
int home_pet_tz(void);
void home_pet_set_quiet(double quiet_s);  /* the room's quiet, for dozing (the UI feeds it) */
void home_pet_dev(const char *args);      /* "pet set hunger 0.9" and friends: dev console */
