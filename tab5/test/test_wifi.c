/* Tests for the saved networks (components/tab_hal wifi_saved.c): saving, the least-recently-used one making
 * room, the stored form, and which network a scan's results lead to. The passwords here are made up. */
#include "wifi_saved.h"

#include <stdio.h>
#include <string.h>

static int *g_checks, *g_fails;
#define CHECK(c) do { (*g_checks)++; if (!(c)) { (*g_fails)++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

void test_wifi(int *checks, int *fails)
{
    g_checks = checks;
    g_fails = fails;
    static ws_set_t s;
    memset(&s, 0, sizeof s);
    bool ch;
    CHECK(ws_count(&s) == 0 && ws_find(&s, "home") == -1 && ws_find(&s, "") == -1);
    int home = ws_put(&s, "home", "secret-one", &ch);
    CHECK(home == 0 && ch && ws_count(&s) == 1 && ws_find(&s, "home") == 0);
    ws_put(&s, "home", "secret-one", &ch);
    CHECK(!ch);                                     /* the same again: nothing to write */
    int pit = ws_put(&s, "pit wifi", "", &ch);      /* an open network */
    CHECK(pit == 1 && ch && s.n[pit].used > s.n[home].used);
    CHECK(ws_touch(&s, home) && !ws_touch(&s, home) && s.n[home].used > s.n[pit].used);
    ws_put(&s, "home", "secret-two", &ch);          /* a new password replaces the old */
    CHECK(ch && !strcmp(s.n[home].pass, "secret-two") && ws_count(&s) == 2);
    CHECK(ws_put(&s, "", "x", &ch) == -1 && ws_put(&s, "a-name-of-thirty-three-characters", "x", &ch) == -1);

    /* the stored form, and back; a colon in the password and odd bytes in the name survive */
    char enc[WS_ENC_MAX];
    ws_net_t a, b;
    memset(&a, 0, sizeof a);
    snprintf(a.ssid, sizeof a.ssid, "caf\xc3\xa9: 5G");
    snprintf(a.pass, sizeof a.pass, "pa:ss:word");
    a.used = 41;
    CHECK(ws_encode(&a, enc, sizeof enc) > 0 && !strstr(enc, "caf"));
    CHECK(ws_decode(enc, &b) && !strcmp(b.ssid, a.ssid) && !strcmp(b.pass, a.pass) && b.used == 41);
    memset(&a, 0, sizeof a);
    snprintf(a.ssid, sizeof a.ssid, "12345678901234567890123456789012");
    snprintf(a.pass, sizeof a.pass, "0123456789012345678901234567890123456789012345678901234567890123");
    CHECK(ws_encode(&a, enc, sizeof enc) > 0 && ws_decode(enc, &b) && !strcmp(b.ssid, a.ssid) && !strcmp(b.pass, a.pass));
    char small[40];
    CHECK(ws_encode(&a, small, sizeof small) == 0 && !small[0]); /* no room: nothing, not half a password */
    memset(&a, 0, sizeof a);
    CHECK(ws_encode(&a, enc, sizeof enc) == 0);
    CHECK(!ws_decode("", &b) && !ws_decode("junk", &b) && !ws_decode("3:zz:pw", &b) && !ws_decode("3::pw", &b) &&
          !ws_decode("3:616", &b));
    CHECK(ws_decode("7:6f70656e:", &b) && !strcmp(b.ssid, "open") && !b.pass[0] && b.used == 7);

    /* which to join */
    ws_seen_t seen[4] = { { "home", -60 }, { "pit wifi", -55 }, { "someone else", -30 }, { "home", -48 } };
    CHECK(ws_pick(&s, seen, 0, 0) == -1);
    CHECK(ws_pick(&s, seen + 2, 1, 0) == -1);       /* only a stranger in range */
    CHECK(ws_pick(&s, seen, 1, 0) == home);
    CHECK(ws_pick(&s, seen + 1, 1, 0) == pit);
    CHECK(ws_pick(&s, seen, 3, 0) == home);         /* home was used last and is within 8 dB of the pit's */
    ws_touch(&s, pit);
    CHECK(ws_pick(&s, seen, 3, 0) == pit);          /* comparable: the last-used stays, either way round */
    seen[1].rssi = -80;
    CHECK(ws_pick(&s, seen, 3, 0) == home);         /* far louder beats last-used */
    seen[1].rssi = -55;
    seen[0].rssi = -90;
    CHECK(ws_pick(&s, seen, 3, 0) == pit);
    CHECK(ws_pick(&s, seen, 4, 0) == pit);          /* home's loudest AP (-48) is only comparable: pit stays */
    ws_touch(&s, home);
    CHECK(ws_pick(&s, seen, 4, 0) == home);
    /* one that just refused the password is passed over, unless it's all there is */
    CHECK(ws_pick(&s, seen, 4, 1u << home) == pit);
    CHECK(ws_pick(&s, seen, 1, 1u << home) == home);

    /* forgetting */
    CHECK(ws_remove(&s, pit) && !ws_remove(&s, pit) && ws_find(&s, "pit wifi") == -1 && ws_count(&s) == 1);
    CHECK(ws_pick(&s, seen + 1, 1, 0) == -1);

    /* full: the one used longest ago makes room, and only it */
    memset(&s, 0, sizeof s);
    char name[16];
    for (int i = 0; i < WS_MAX; i++) {
        snprintf(name, sizeof name, "net%d", i);
        CHECK(ws_put(&s, name, "pw", &ch) == i);
    }
    ws_touch(&s, 0);                                /* net0 used again: net1 is now the oldest */
    int k = ws_put(&s, "new", "pw", &ch);
    CHECK(k == 1 && ch && ws_count(&s) == WS_MAX && ws_find(&s, "net1") == -1 && ws_find(&s, "net0") == 0 &&
          ws_find(&s, "net15") == 15);
}
