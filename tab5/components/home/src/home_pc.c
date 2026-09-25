/* The PC's media through Catalyst Link: GET /media/now every second while home mode shows it, the album
 * art when the song changes (RGB565, base64 in JSON: link_get() reads JSON bodies), and the transport
 * commands, sent the moment they're tapped and followed by a quick re-read. */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

#define POLL_S 1.0
#define IDLE_POLL_S 3.0      /* nothing playing, or the Link away: look less often */
#define NOW_MAX 4096
#define ART_BYTES (HOME_ART * HOME_ART * 2)
#define ART_JSON_MAX (ART_BYTES * 4 / 3 + 512)
#define CMDS 6

typedef struct {
    home_pc_t st;
    uint16_t *art;           /* ART_BYTES, the art on show */
    char art_id[24];         /* what the Link calls the art on show ("" none) */
    char want_art[24];       /* what /media/now says the song has */
    char cmd[CMDS][64];      /* queued POST bodies */
    int ncmd;
    bool enabled;
    double next_poll;
} pc_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pc_t *P; /* on the heap: internal RAM is scarce */

static pc_t *pc(void)
{
    if (!P) {
        P = calloc(1, sizeof *P);
        if (P) {
            P->enabled = true;
            P->st.volume = -1;
            P->st.position = P->st.duration = -1;
        }
    }
    return P;
}

void home_pc_enable(bool on)
{
    pthread_mutex_lock(&g_lock);
    if (pc()) {
        P->enabled = on;
        if (!on) {
            memset(&P->st, 0, sizeof P->st);
            P->st.volume = -1;
            P->st.gen++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void home_pc_get(home_pc_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (pc()) *out = P->st;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_lock);
}

bool home_pc_art(uint16_t *px)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    if (P && P->art && P->art_id[0] && P->st.art) {
        memcpy(px, P->art, ART_BYTES);
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static void queue(const char *body)
{
    pthread_mutex_lock(&g_lock);
    if (pc() && P->ncmd < CMDS) snprintf(P->cmd[P->ncmd++], sizeof P->cmd[0], "%s", body);
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

void home_pc_cmd(const char *action)
{
    char b[64];
    snprintf(b, sizeof b, "{\"action\":\"%s\"}", action);
    queue(b);
    /* optimistic: the button answers now, the next read confirms */
    pthread_mutex_lock(&g_lock);
    if (P && !strcmp(action, "toggle") && P->st.have) {
        P->st.playing = !P->st.playing;
        snprintf(P->st.state, sizeof P->st.state, "%s", P->st.playing ? "playing" : "paused");
        P->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
}

void home_pc_volume(float level01)
{
    char b[64];
    snprintf(b, sizeof b, "{\"action\":\"volume\",\"level\":%.2f}", level01 < 0 ? 0 : level01 > 1 ? 1 : level01);
    queue(b);
}

static void read_now(char *buf)
{
    int status = link_get("/media/now", buf, NOW_MAX);
    home_pc_t st;
    pthread_mutex_lock(&g_lock);
    st = P->st;
    pthread_mutex_unlock(&g_lock);
    st.link = status > 0;
    char want_art[24] = "";
    if (status == 404) {
        st.available = false;
        snprintf(st.reason, sizeof st.reason, "update catalyst link on the pc for its music");
        st.have = false;
    } else if (status == 401) {
        st.available = false;
        snprintf(st.reason, sizeof st.reason, "the pc refused the tablet: pair it again");
        st.have = false;
    } else if (status == 200) {
        home_json_t j;
        if (home_json_parse(&j, buf, strlen(buf))) {
            st.available = jl_bool(&j.d, jl_get(&j.d, 0, "available"), false);
            home_json_str(&j, 0, "reason", st.reason, sizeof st.reason);
            st.volume = (float)home_json_num(&j, 0, "volume", -1);
            st.muted = jl_bool(&j.d, jl_get(&j.d, 0, "muted"), false);
            int p = jl_get(&j.d, 0, "playing");
            st.have = p >= 0 && j.d.t[p].type == JL_OBJ;
            if (st.have) {
                home_json_str(&j, p, "title", st.title, sizeof st.title);
                home_json_str(&j, p, "artist", st.artist, sizeof st.artist);
                home_json_str(&j, p, "album", st.album, sizeof st.album);
                home_json_str(&j, p, "app", st.app, sizeof st.app);
                home_json_str(&j, p, "state", st.state, sizeof st.state);
                st.playing = !strcmp(st.state, "playing");
                st.position = home_json_num(&j, p, "position", -1);
                st.duration = home_json_num(&j, p, "duration", -1);
                st.at = hal_seconds();
                int can = jl_get(&j.d, p, "can");
                st.can_next = jl_bool(&j.d, can < 0 ? -1 : jl_get(&j.d, can, "next"), true);
                st.can_prev = jl_bool(&j.d, can < 0 ? -1 : jl_get(&j.d, can, "previous"), true);
                int a = jl_get(&j.d, p, "art");
                if (a >= 0 && j.d.t[a].type == JL_STR) jl_str(&j.d, a, want_art, sizeof want_art);
            }
            home_json_free(&j);
        }
    } else {
        st.have = false;
    }
    st.art = want_art[0] != 0;
    pthread_mutex_lock(&g_lock);
    bool changed = memcmp(&st, &P->st, sizeof st) != 0;
    /* the position alone moving isn't news: the UI runs it on from `at` */
    home_pc_t a = st, b = P->st;
    a.position = b.position = 0;
    a.at = b.at = 0;
    a.gen = b.gen;
    bool news = memcmp(&a, &b, sizeof a) != 0;
    if (changed) {
        unsigned g = P->st.gen, ag = P->st.art_gen;
        P->st = st;
        P->st.gen = news ? g + 1 : g;
        P->st.art_gen = ag;
    }
    snprintf(P->want_art, sizeof P->want_art, "%s", want_art);
    if (!want_art[0] && P->art_id[0]) {
        P->art_id[0] = 0;
        P->st.art_gen++;
        P->st.gen++;
    }
    pthread_mutex_unlock(&g_lock);
}

static void read_art(const char *id)
{
    char *js = malloc(ART_JSON_MAX);
    uint16_t *px = malloc(ART_BYTES);
    bool ok = false;
    if (js && px) {
        char path[96];
        snprintf(path, sizeof path, "/media/art?format=rgb565&size=%d&encoding=base64", HOME_ART);
        if (link_get(path, js, ART_JSON_MAX) == 200) {
            /* no tokenizer needed: find "data":"…" */
            char *d = strstr(js, "\"data\":\"");
            if (d) {
                d += 8;
                char *e = strchr(d, '"');
                if (e && home_b64_decode(d, (size_t)(e - d), (unsigned char *)px, ART_BYTES) == ART_BYTES) ok = true;
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    /* mark it tried either way, so a song without readable art isn't fetched every second */
    snprintf(P->art_id, sizeof P->art_id, "%s", id);
    if (ok) {
        uint16_t *old = P->art;
        P->art = px;
        px = old;
    } else {
        P->st.art = false;
    }
    P->st.art_gen++;
    P->st.gen++;
    pthread_mutex_unlock(&g_lock);
    free(px);
    free(js);
}

void home_pc_work(double now, bool want)
{
    static char *buf;
    pthread_mutex_lock(&g_lock);
    bool have = pc() != NULL;
    bool enabled = have && P->enabled;
    char cmd[64] = "";
    if (have && P->ncmd) {
        snprintf(cmd, sizeof cmd, "%s", P->cmd[0]);
        memmove(P->cmd[0], P->cmd[1], sizeof P->cmd[0] * (size_t)(P->ncmd - 1));
        P->ncmd--;
    }
    pthread_mutex_unlock(&g_lock);
    if (!have) return;
    if (!buf) buf = malloc(NOW_MAX);
    if (!buf) return;
    if (cmd[0] && enabled) {
        link_post("/media/control", cmd, buf, NOW_MAX);
        P->next_poll = now + 0.25; /* the app takes a moment to change */
        return;
    }
    if (!want || !enabled || now < P->next_poll) return;
    link_status_t ls;
    link_status(&ls);
    if (!ls.configured || !ls.reachable) {
        pthread_mutex_lock(&g_lock);
        if (P->st.link || P->st.have) {
            memset(&P->st.title, 0, sizeof P->st.title);
            P->st.link = P->st.have = P->st.playing = false;
            snprintf(P->st.reason, sizeof P->st.reason, "%s",
                     ls.configured ? "the pc isn't answering" : "no pc paired: settings, home");
            P->st.gen++;
        }
        pthread_mutex_unlock(&g_lock);
        P->next_poll = now + IDLE_POLL_S;
        return;
    }
    read_now(buf);
    pthread_mutex_lock(&g_lock);
    bool playing = P->st.playing;
    char want_art[24], art_id[24];
    snprintf(want_art, sizeof want_art, "%s", P->want_art);
    snprintf(art_id, sizeof art_id, "%s", P->art_id);
    pthread_mutex_unlock(&g_lock);
    if (want_art[0] && strcmp(want_art, art_id) != 0) read_art(want_art);
    P->next_poll = now + (playing ? POLL_S : IDLE_POLL_S * 0.5);
}
