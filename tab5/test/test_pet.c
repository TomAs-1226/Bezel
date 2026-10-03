/* Tests for the desk pet (home_pet.c: the model and its file) and the avatar's format reader and geometry
 * (ui_avatar_def.c). Built by sim/CMakeLists.txt into catalyst_tab_tests; with TEST_PET_STANDALONE it is a
 * program of its own, which is how it is run where there is no POSIX toolchain. */
#include "home_pet.h"
#include "../components/ui/src/ui_avatar_def.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef TEST_PET_STANDALONE
static int fails_, checks_;
#define CHECK(c) do { checks_++; if (!(c)) { fails_++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#else
#define CHECK(c) do { (*checks)++; if (!(c)) { (*fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#endif

#define T0 1790000000LL /* 2026-10-21 14:13 UTC */
#define H (3600LL)

static void pet_tests(int *checks, int *fails)
{
    (void)checks; (void)fails;
    home_pet_t p;
    home_pet_init(&p, T0);
    CHECK(!strcmp(p.name, "Tabi"));
    CHECK(p.ndiary == 1);

    /* a day awake in daylight makes it hungrier and more tired, and it is asleep by night */
    int tz = 0;
    home_pet_advance(&p, T0 + 6 * H, tz, 0); /* 20:13 */
    CHECK(p.hunger > 0.5f && p.hunger < 0.8f);
    CHECK(p.energy < 0.8f && !p.asleep);
    home_pet_advance(&p, T0 + 10 * H, tz, 0); /* 00:13: night */
    CHECK(p.asleep);
    home_pet_advance(&p, T0 + 21 * H, tz, 0); /* 11:13 next day */
    CHECK(!p.asleep);
    CHECK(p.energy > 0.7f);
    CHECK(p.hunger <= 0.92f);

    /* a week away: hungry, groggy, alive, and the diary says so */
    home_pet_t q = p;
    home_pet_advance(&q, T0 + 21 * H + 8 * 86400LL, tz, 0);
    CHECK(q.hunger > 0.3f && q.hunger <= 0.92f);
    CHECK(q.energy <= 0.45f + 1e-3f && q.energy >= 0.08f);
    CHECK(q.mood >= 0.2f);
    CHECK(q.ndiary > 0);
    /* ten times the absence is no worse than a week */
    home_pet_t r = p;
    home_pet_advance(&r, T0 + 21 * H + 80 * 86400LL, tz, 0);
    CHECK(r.hunger <= 0.92f && r.energy >= 0.08f);

    /* time going backwards or never set changes nothing */
    home_pet_t before = p;
    home_pet_advance(&p, 12345, tz, 0);
    CHECK(p.hunger == before.hunger);
    home_pet_advance(&p, T0, tz, 0);
    CHECK(p.hunger == before.hunger && p.clock == T0);

    /* feeding */
    home_pet_init(&p, T0);
    p.hunger = 0.8f;
    char say[80];
    CHECK(home_pet_feed(&p, PET_FISH, T0, tz, say, sizeof say));
    CHECK(p.hunger < 0.3f);
    CHECK(p.fed_at == T0);
    p.hunger = 0.05f;
    CHECK(!home_pet_feed(&p, PET_RICE, T0, tz, say, sizeof say)); /* not hungry */
    p.hunger = 0.9f;
    CHECK(home_pet_feed(&p, PET_CAKE, T0, tz, say, sizeof say));
    p.hunger = 0.9f;
    CHECK(home_pet_feed(&p, PET_CAKE, T0, tz, say, sizeof say));
    p.hunger = 0.9f;
    CHECK(!home_pet_feed(&p, PET_CAKE, T0, tz, say, sizeof say)); /* a third treat in a day */
    CHECK(home_pet_feed(&p, PET_RICE, T0, tz, say, sizeof say));
    p.asleep = true;
    p.hunger = 0.9f;
    CHECK(!home_pet_feed(&p, PET_RICE, T0, tz, say, sizeof say));

    /* play and strokes */
    p.asleep = false;
    p.energy = 0.9f;
    float m0 = p.mood;
    CHECK(home_pet_play(&p, T0, say, sizeof say));
    CHECK(p.mood >= m0 && p.energy < 0.9f);
    p.energy = 0.1f;
    CHECK(!home_pet_play(&p, T0, say, sizeof say));
    float a0 = p.affection;
    home_pet_stroke(&p, T0 + 5);
    CHECK(p.affection > a0);

    /* sleeping and waking by hand; waking at night keeps it up a while */
    home_pet_init(&p, T0 + 8 * H); /* 22:13 */
    CHECK(home_pet_set_asleep(&p, true, T0 + 8 * H, tz, say, sizeof say));
    CHECK(home_pet_set_asleep(&p, false, T0 + 8 * H, tz, say, sizeof say));
    home_pet_advance(&p, T0 + 8 * H + 600, tz, 0);
    CHECK(!p.asleep);
    home_pet_advance(&p, T0 + 8 * H + 2400, tz, 0);
    CHECK(p.asleep);

    /* a quiet room makes a tired pet doze, and company wakes it */
    home_pet_init(&p, T0);
    p.energy = 0.5f;
    home_pet_advance(&p, T0 + 60, tz, 1500);
    CHECK(p.asleep);
    home_pet_advance(&p, T0 + 120, tz, 5);
    CHECK(!p.asleep);

    /* names */
    CHECK(home_pet_rename(&p, "  Mochi \"x\" ", T0));
    CHECK(!strcmp(p.name, "Mochi x"));
    CHECK(!home_pet_rename(&p, "   ", T0));

    /* the words, the feel, the companion's line */
    home_pet_init(&p, T0);
    p.hunger = 0.85f;
    char w[64];
    home_pet_words(&p, w, sizeof w);
    CHECK(strstr(w, "hungry") != NULL);
    CHECK(home_pet_feel(&p) == PET_FEEL_HUNGRY);
    p.asleep = true;
    CHECK(home_pet_feel(&p) == PET_FEEL_ASLEEP);
    p.asleep = false;
    char b1[512], b2[512];
    home_pet_brief(&p, T0 + 100, b1, sizeof b1);
    CHECK(strstr(b1, "mention it once") != NULL);
    home_pet_brief(&p, T0 + 200, b2, sizeof b2);
    CHECK(strstr(b2, "mention it once") == NULL); /* once, not a nag */
    CHECK(strstr(b2, "Tabi") != NULL);
    home_pet_brief(&p, T0 + 4 * H, b2, sizeof b2);
    CHECK(strstr(b2, "mention it once") != NULL); /* hours later it may be again */

    /* the diary keeps the newest */
    for (int i = 0; i < 30; i++) {
        char s[24];
        snprintf(s, sizeof s, "event %d", i);
        home_pet_note(&p, T0 + 1000 + i, s);
    }
    CHECK(p.ndiary == PET_DIARY && !strcmp(p.diary[PET_DIARY - 1].text, "event 29"));

    /* the file round trips, and a damaged one is refused */
    char js[2048];
    p.up_until = T0 + 77;
    size_t n = home_pet_to_json(&p, js, sizeof js);
    CHECK(n > 50 && n < sizeof js);
    home_pet_t back;
    CHECK(home_pet_from_json(&back, js, n));
    CHECK(!strcmp(back.name, p.name) && back.ndiary == p.ndiary && back.born == p.born && back.up_until == p.up_until);
    CHECK(fabsf(back.hunger - p.hunger) < 0.001f);
    CHECK(!strcmp(back.diary[back.ndiary - 1].text, p.diary[p.ndiary - 1].text));
    CHECK(!home_pet_from_json(&back, "{\"name\":", 8));
    CHECK(!home_pet_from_json(&back, "[]", 2));
    CHECK(!home_pet_from_json(&back, "{\"name\":\"x\"}", 12)); /* no birth date: not a pet file */
}

/* a small avatar of our own, to read */
static const char AV[] =
    "{\"schema\":\"bible-strong/avatar-definition\",\"schemaVersion\":1,\"name\":\"T\","
    "\"body\":{\"primary\":{\"type\":\"sphere\",\"width\":240,\"height\":240,\"depth\":240,\"roundness\":1},"
    "\"nodes\":[{\"surface\":{\"type\":\"sphere\",\"width\":50,\"height\":50,\"depth\":40,\"roundness\":1},"
    "\"position\":[-70,-90,-10],\"rotation\":[0,0,0]}]},"
    "\"colors\":{\"body\":\"#5b7fe5\",\"eyes\":\"#111316\"},"
    "\"expressions\":{"
    "\"neutral\":{\"head\":{\"x\":0,\"y\":0,\"z\":0},\"eyes\":{\"left\":{\"width\":20,\"height\":50,\"x\":0,\"y\":-7,"
    "\"angle\":0},\"right\":{\"width\":20,\"height\":50,\"x\":0,\"y\":-7,\"angle\":0},\"spacing\":35},"
    "\"perspective\":1,\"motion\":{\"eyes\":\"none\",\"body\":\"none\"}},"
    "\"turn\":{\"head\":{\"x\":0,\"y\":35,\"z\":-10},\"eyes\":{\"left\":{\"width\":22,\"height\":40,\"x\":0,\"y\":0,"
    "\"angle\":0},\"right\":{\"width\":22,\"height\":40,\"x\":0,\"y\":0,\"angle\":0},\"spacing\":54},"
    "\"perspective\":1,\"motion\":{\"eyes\":\"microSaccades\",\"body\":\"slowDrift\"},"
    "\"colors\":{\"body\":\"#ba3636\"}}},"
    "\"expressionOrder\":[\"neutral\",\"turn\"],"
    "\"animations\":{\"idle\":{\"playbackMode\":\"loop\",\"steps\":[{\"expression\":\"neutral\",\"holdMs\":3000,"
    "\"transitionMs\":500,\"transition\":\"smooth\"},{\"expression\":\"turn\",\"holdMs\":2000,\"transitionMs\":400,"
    "\"transition\":\"spring\"},{\"expression\":\"nonesuch\",\"holdMs\":2000,\"transitionMs\":400,"
    "\"transition\":\"spring\"}],\"blink\":{\"enabled\":true,\"initialDelayMs\":2000,\"minIntervalMs\":3000,"
    "\"maxIntervalMs\":6000,\"durationMs\":280}}},"
    "\"animationOrder\":[\"idle\"]}";

static void avatar_tests(int *checks, int *fails)
{
    (void)checks; (void)fails;
    char why[96];
    av_def_t *d = av_def_parse(AV, sizeof AV - 1, why, sizeof why);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(d->nx == 2 && d->na == 1 && d->nnodes == 1);
    CHECK(d->a[0].n == 2); /* the step naming an unknown expression is dropped */
    CHECK(d->a[0].blink && d->a[0].b_dur == 280 && d->a[0].mode == AV_LOOP);
    CHECK(d->st[1].kind == AV_SPRING && d->st[0].kind == AV_SMOOTH);
    CHECK(d->body == 0x5b7fe5 && d->eyes == 0x111316);
    CHECK(d->x[1].e.meye == 1 && d->x[1].e.mbody == 1 && d->x[1].e.cbody == (0x1000000u | 0xba3636));
    CHECK(fabsf(d->sc - 240.0f / 196.2f) < 1e-4f);

    /* the neutral face: two upright bars, mirrored, about where the format's own render puts them */
    float P[AV_NP];
    av_eye_t e[2];
    av_ear_t ear[AV_MAX_NODES];
    av_pose_of(d, 0, 0, P);
    av_geom(d, P, e, ear);
    CHECK(fabsf(e[0].cx + e[1].cx) < 0.01f && e[1].cx > 15 && e[1].cx < 24);
    CHECK(fabsf(e[0].len - 61.2f) < 3.0f && fabsf(e[0].wid - 24.5f) < 2.0f);
    CHECK(fabsf(e[0].dx) < 0.12f && fabsf(e[0].dy) > 0.99f); /* upright, to within the surface's own lean */
    CHECK(fabsf(ear[0].x + 70) < 0.01f && fabsf(ear[0].y + 90) < 0.01f);

    /* a head turned right moves the face right and foreshortens what leans away */
    av_pose_of(d, 1, 0, P);
    av_geom(d, P, e, ear);
    CHECK(e[0].cx > 30 && e[1].cx > e[0].cx + 30);
    CHECK(e[1].wid < e[0].wid + 1);      /* the far eye is no wider */
    CHECK(e[1].cy < e[0].cy);            /* a roll of -10: the right eye is higher */
    CHECK(ear[0].z > 20);                /* the left ear swings round to the front */
    /* a pose that carries a colour override wears it; the others wear the file's, or the accent */
    CHECK(P[P_BR] == 0xba && P[P_BB] == 0x36);
    av_pose_of(d, 0, 0, P);
    CHECK(P[P_BR] == 0x5b);
    av_pose_of(d, 0, 0x102030, P);
    CHECK(P[P_BR] == 0x10 && P[P_BB] == 0x30);

    /* refusals */
    char bad[96] = "";
    CHECK(av_def_parse("{}", 2, bad, sizeof bad) == NULL && bad[0]);
    CHECK(av_def_parse("nope", 4, bad, sizeof bad) == NULL);
    CHECK(av_def_parse(AV, 40, bad, sizeof bad) == NULL); /* cut short */
    av_def_free(d);

    /* the built-in character is complete enough to stand in for any state */
    const av_def_t *b = av_builtin();
    static const char *const NEED[] = { "neutral", "happy", "sleepy", "asleep", "curious-left", "curious-right",
                                        "surprised", "sad", "hungry", "eating", "listening", "thinking",
                                        "speaking", "love" };
    for (size_t i = 0; i < sizeof NEED / sizeof NEED[0]; i++) CHECK(av_def_expr(b, NEED[i]) >= 0);
    static const char *const ANIM[] = { "idle", "idle-quiet", "listening", "thinking", "working", "happy", "confused",
                                        "surprised", "sleeping", "waking", "drowsy", "sad", "hungry", "eating",
                                        "petted", "love", "fed", "curious" };
    for (size_t i = 0; i < sizeof ANIM / sizeof ANIM[0]; i++) CHECK(av_def_anim(b, ANIM[i]) >= 0);
    for (int i = 0; i < b->na; i++)
        for (int k = 0; k < b->a[i].n; k++) CHECK(b->st[b->a[i].first + k].expr < b->nx);
}

#ifdef TEST_PET_STANDALONE
int main(void)
{
    int c = 0, f = 0;
    pet_tests(&c, &f);
    avatar_tests(&c, &f);
    printf("pet+avatar tests: %d checks, %d failed\n", checks_, fails_);
    return fails_ ? 1 : 0;
}
#else
void test_pet(int *checks, int *fails)
{
    pet_tests(checks, fails);
    avatar_tests(checks, fails);
}
#endif
