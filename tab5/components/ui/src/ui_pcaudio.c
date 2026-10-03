/* The PC's sound on the tablet's speaker (home_pcaudio.c): the switch, kept in kv ("pcaudio"), its row for the
 * music app and settings > pc link, and the island activity while it plays.
 *
 * The receiver does all the work on its own thread; here is only what the UI shows. The row's labels are set
 * on change only, from the owning app's refresh hook (a hidden page must not refresh), and the activity is
 * driven by a tick that reads one small struct under a mutex. */
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "home_pcaudio.h"

#define SLOTS 2
#define END_AFTER_S 3.0   /* a pause between songs doesn't take the island away */

static struct {
    lv_obj_t *chip[SLOTS], *state[SLOTS];
    bool on_shown[SLOTS];
    unsigned gen_shown[SLOTS];
    bool on;                 /* the switch, as kept */
    int act;                 /* the island activity (0 none) */
    double not_live_since;
    bool loaded;
} P;

bool ui_pcaudio_enabled(void)
{
    return P.on;
}

void ui_pcaudio_set(bool on)
{
    P.on = on;
    hal_kv_set("pcaudio", on ? "1" : "0");
    home_pcaudio_enable(on);
    for (int i = 0; i < SLOTS; i++) P.gen_shown[i] = 0; /* rows redraw at their next refresh */
}

static void toggle(lv_obj_t *o, void *u)
{
    (void)o;
    (void)u;
    ui_pcaudio_set(!P.on);
}

lv_obj_t *ui_pcaudio_row(lv_obj_t *parent, int w, int slot)
{
    lv_obj_t *r = bz_row(parent, 14);
    lv_obj_set_width(r, w);
    P.chip[slot] = ui_chip(r, "play the pc's sound here", toggle, NULL);
    P.state[slot] = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, w - 330);
    P.on_shown[slot] = !P.on;
    P.gen_shown[slot] = 0;
    return r;
}

/* from the owning app's refresh: the chip and its words, on change only */
void ui_pcaudio_row_refresh(int slot)
{
    if (!P.chip[slot]) return;
    pca_t s;
    home_pcaudio_get(&s);
    if (s.enabled != P.on_shown[slot]) {
        P.on_shown[slot] = s.enabled;
        ui_chip_set(P.chip[slot], s.enabled);
    }
    if (s.state == PCA_LIVE) {
        ui_text(P.state[slot], "playing \xc2\xb7 %d ms buffered \xc2\xb7 %u dropouts \xc2\xb7 volume is the tablet's own", s.fill_ms, s.underruns);
        return;
    }
    if (s.gen == P.gen_shown[slot]) return;
    P.gen_shown[slot] = s.gen;
    switch (s.state) {
    case PCA_OFF: ui_text(P.state[slot], "%s", "everything the pc plays comes out of this speaker"); break;
    case PCA_WAITING: ui_text(P.state[slot], "%s", s.note[0] ? s.note : "waiting for the pc"); break;
    case PCA_CONNECTING: ui_text(P.state[slot], "%s", "connecting to the pc"); break;
    case PCA_QUIET: ui_text(P.state[slot], "%s", "connected \xc2\xb7 the pc is quiet"); break;
    case PCA_HELD: ui_text(P.state[slot], "%s", s.note[0] ? s.note : "paused"); break;
    default: break;
    }
}

/* every refresh: the island while the PC's sound is playing */
static void tick(void *u)
{
    (void)u;
    pca_t s;
    home_pcaudio_get(&s);
    double now = hal_seconds();
    bool live = s.enabled && (s.state == PCA_LIVE || s.state == PCA_HELD);
    if (live) {
        P.not_live_since = 0;
        if (!P.act || !ui_activity_live(P.act)) P.act = ui_activity_begin(BZ_I_GRAPHIC_EQ, "pc sound", 0);
        ui_activity_update(P.act, s.state == PCA_HELD ? "pc sound \xc2\xb7 paused" : "pc sound");
    } else if (P.act) {
        if (P.not_live_since == 0) P.not_live_since = now;
        if (now - P.not_live_since > END_AFTER_S) {
            ui_activity_end(P.act, NULL);
            P.act = 0;
        }
    }
}

void ui_pcaudio_init(void)
{
    char v[8] = "";
    P.loaded = true;
    P.on = hal_kv_get("pcaudio", v, sizeof v) && v[0] == '1';
    if (P.on) home_pcaudio_enable(true);
    ui_on_refresh(tick, NULL);
}

void ui_pcaudio_dev(const char *cmd)
{
    if (!strncmp(cmd, "pca on", 6)) ui_pcaudio_set(true);
    else if (!strncmp(cmd, "pca off", 7)) ui_pcaudio_set(false);
    else if (!strncmp(cmd, "pca host ", 9)) home_pcaudio_host(cmd + 9); /* a test Link: pca host http://192.168.0.5:8766 */
    pca_t s;
    home_pcaudio_get(&s);
    static const char *const N[] = { "off", "waiting", "connecting", "quiet", "live", "held" };
    printf("MEM pca enabled %d state %s fill %d ms underruns %u reconnects %u packets %u rate %d kbps note '%s'\n", s.enabled,
           N[s.state], s.fill_ms, s.underruns, s.reconnects, s.packets, s.rate_kbps, s.note);
}
