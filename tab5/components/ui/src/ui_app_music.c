/* music — the songs on the microSD card (CATOS/AUDIO, MP3 and WAV) played on the tablet's speaker, and the
 * PC's player (Spotify, YouTube Music, whatever Windows' media controls see) through Catalyst Link.
 *
 * Left: the card's songs, a tap plays one (then the next, to the end of the list). Right, top: the tablet's
 * own player. Right, bottom: the PC's. The list is rebuilt only when the card's listing changes; the players'
 * labels are set only when their text changes, and their progress once a second. */
#include "ui_home_priv.h"

#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define LIST_W 520
#define RX (PAD + LIST_W + BZ_GAP)
#define RW (W - PAD - RX)
#define TOP_H 292
#define BOT_H (APP_H - TOP_H - BZ_GAP)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)

static struct {
    lv_obj_t *list, *list_state, *rows[HOME_TRACKS];
    lv_obj_t *title, *state, *meter, *time, *play;
    lv_obj_t *pc_src, *pc_title, *pc_artist, *pc_time, *pc_meter, *pc_play;
    char (*names)[64];
    int n, current;
    unsigned list_gen, pl_gen, pc_gen;
    int last_sec;
    bool pl_playing, pc_playing;
} MU = { .current = -2 };

static void row_tap(lv_obj_t *o, void *u)
{
    (void)o;
    home_player_play((int)(intptr_t)u);
}

static void local_cmd(lv_obj_t *o, void *u)
{
    (void)o;
    switch ((intptr_t)u) {
    case 0: home_player_skip(-1); break;
    case 1: home_player_toggle(); break;
    case 2: home_player_skip(1); break;
    case 3: home_player_stop(); break;
    case 4: home_player_rescan(); break;
    }
}

static void pc_cmd(lv_obj_t *o, void *u)
{
    (void)o;
    home_pc_cmd((const char *)u);
}

static void pair_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_PAIR, o);
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon, bz_tap_fn fn, void *u)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, fn, u);
    lv_obj_set_size(b, 68, 56);
    lv_obj_set_style_pad_hor(b, 0, 0);
    return b;
}

static void set_play_icon(lv_obj_t *b, bool playing, bool *shown)
{
    if (*shown == playing) return;
    *shown = playing;
    bz_icon_set(lv_obj_get_child(b, 0), playing ? BZ_I_PAUSE : BZ_I_PLAY_ARROW, 24, false);
}

static void music_build(lv_obj_t *b)
{
    MU.names = calloc(HOME_TRACKS, 64);

    lv_obj_t *l = bz_tile(b, LIST_W, APP_H);
    lv_obj_set_pos(l, PAD, APP_Y);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_t *hr = bz_row(l, 10);
    lv_obj_set_width(hr, IN(LIST_W));
    lv_obj_set_flex_align(hr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bz_label(hr, "on this tablet", BZ_F_NAME, BZ_C_INK);
    ui_button(hr, BZ_I_REFRESH, "look again", local_cmd, (void *)(intptr_t)4);
    MU.list_state = bz_label_line(l, "", BZ_F_CAPTION, BZ_C_DIM, IN(LIST_W));
    MU.list = ui_scroller(l, IN(LIST_W), APP_H - 2 * BZ_PAD_TILE - 56 - 20 - 20);
    lv_obj_set_style_pad_row(MU.list, 8, 0);

    /* the tablet's player */
    lv_obj_t *t = bz_tile(b, RW, TOP_H);
    lv_obj_set_pos(t, RX, APP_Y);
    bz_label(t, "playing here", BZ_F_LABEL, BZ_C_DIM);
    MU.title = bz_label_line(t, "", BZ_F_TITLE, BZ_C_INK, IN(RW));
    lv_obj_set_pos(MU.title, 0, 28);
    MU.state = bz_label_line(t, "", BZ_F_BODY_S, BZ_C_DIM, IN(RW));
    lv_obj_set_pos(MU.state, 0, 78);
    MU.meter = bz_meter(t, IN(RW) - 130, 6);
    lv_obj_set_pos(MU.meter, 0, 124);
    MU.time = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, 120);
    lv_obj_set_style_text_align(MU.time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(MU.time, IN(RW) - 120, 116);
    lv_obj_t *c = bz_row(t, 10);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    icon_button(c, BZ_I_ARROW_BACK, local_cmd, (void *)(intptr_t)0);
    MU.play = icon_button(c, BZ_I_PLAY_ARROW, local_cmd, (void *)(intptr_t)1);
    ui_chip_set(MU.play, true);
    icon_button(c, BZ_I_ARROW_FORWARD, local_cmd, (void *)(intptr_t)2);
    icon_button(c, BZ_I_STOP, local_cmd, (void *)(intptr_t)3);

    /* the PC's */
    t = bz_tile(b, RW, BOT_H);
    lv_obj_set_pos(t, RX, APP_Y + TOP_H + BZ_GAP);
    MU.pc_src = bz_label_line(t, "", BZ_F_LABEL, BZ_C_DIM, IN(RW));
    MU.pc_title = bz_label_line(t, "", BZ_F_NAME, BZ_C_INK, IN(RW));
    lv_obj_set_pos(MU.pc_title, 0, 26);
    MU.pc_artist = bz_label_line(t, "", BZ_F_BODY_S, BZ_C_DIM, IN(RW));
    lv_obj_set_pos(MU.pc_artist, 0, 60);
    MU.pc_meter = bz_meter(t, IN(RW) - 130, 6);
    lv_obj_set_pos(MU.pc_meter, 0, 100);
    MU.pc_time = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, 120);
    lv_obj_set_style_text_align(MU.pc_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(MU.pc_time, IN(RW) - 120, 92);
    c = bz_row(t, 10);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    icon_button(c, BZ_I_ARROW_BACK, pc_cmd, (void *)"previous");
    MU.pc_play = icon_button(c, BZ_I_PLAY_ARROW, pc_cmd, (void *)"toggle");
    ui_chip_set(MU.pc_play, true);
    icon_button(c, BZ_I_ARROW_FORWARD, pc_cmd, (void *)"next");
    lv_obj_t *gap = bz_box(c);
    lv_obj_set_size(gap, 18, 1);
    icon_button(c, BZ_I_VOLUME_OFF, pc_cmd, (void *)"mute");
    icon_button(c, BZ_I_REMOVE, pc_cmd, (void *)"volume_down");
    icon_button(c, BZ_I_ADD, pc_cmd, (void *)"volume_up");
    lv_obj_t *gap2 = bz_box(c);
    lv_obj_set_size(gap2, 18, 1);
    icon_button(c, BZ_I_LINK, pair_tap, NULL); /* pair (or pair again) with the pc */
}

static void rebuild_list(void)
{
    lv_obj_clean(MU.list);
    memset(MU.rows, 0, sizeof MU.rows);
    MU.current = -2;
    for (int i = 0; i < MU.n; i++) {
        char label[64];
        snprintf(label, sizeof label, "%s", MU.names[i]);
        char *dot = strrchr(label, '.');
        if (dot) *dot = 0;
        home_fold_text(label);
        lv_obj_t *r = ui_chip(MU.list, NULL, row_tap, (void *)(intptr_t)i);
        lv_obj_set_width(r, IN(LIST_W));
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        bz_label_line(r, label, BZ_F_BODY_S, BZ_C_INK, IN(LIST_W) - 48);
        MU.rows[i] = r;
    }
}

static void music_refresh(void)
{
    home_want(HOME_WANT_PC);
    /* the card's songs */
    unsigned gen = 0;
    int n = MU.names ? home_player_tracks(MU.names, HOME_TRACKS, &gen) : 0;
    if (n >= 0 && gen != MU.list_gen) {
        MU.list_gen = gen;
        MU.n = n;
        rebuild_list();
        const char *dir = home_player_dir();
        home_play_t pl;
        home_player_get(&pl);
        if (!dir) ui_text(MU.list_state, "%s", "no microSD card");
        else if (!n) ui_text(MU.list_state, "put %s files in CATOS/AUDIO on the card", pl.mp3 ? "MP3 or WAV" : "WAV");
        else ui_text(MU.list_state, "%d song%s in CATOS/AUDIO%s", n, n == 1 ? "" : "s", pl.mp3 ? "" : " \xc2\xb7 WAV plays here, MP3 on the tablet");
    } else if (n < 0) {
        ui_text(MU.list_state, "%s", "reading the card...");
    }

    double now = hal_seconds();
    int sec = (int)now;
    bool tick = sec != MU.last_sec;
    MU.last_sec = sec;

    home_play_t pl;
    home_player_get(&pl);
    if (pl.gen != MU.pl_gen || MU.current == -2) {
        MU.pl_gen = pl.gen;
        bool active = pl.state == HP_PLAYING || pl.state == HP_PAUSED || pl.state == HP_LOADING;
        int cur = active ? pl.index : -1;
        if (cur != MU.current) {
            if (MU.current >= 0 && MU.current < MU.n && MU.rows[MU.current]) ui_chip_set(MU.rows[MU.current], false);
            if (cur >= 0 && cur < MU.n && MU.rows[cur]) ui_chip_set(MU.rows[cur], true);
            MU.current = cur;
        }
        ui_text(MU.title, "%s", active || pl.state == HP_ERROR ? pl.title : "Nothing playing");
        ui_text(MU.state, "%s", pl.state == HP_ERROR ? pl.err : pl.state == HP_LOADING ? "starting" : pl.state == HP_PAUSED ? "paused"
                                : pl.state == HP_PLAYING ? "playing" : "tap a song");
        set_play_icon(MU.play, pl.state == HP_PLAYING || pl.state == HP_LOADING, &MU.pl_playing);
    }
    if (tick) {
        char a[16], b[16];
        bool active = pl.state == HP_PLAYING || pl.state == HP_PAUSED;
        hm_fmt_time(active ? pl.position : -1, a, sizeof a);
        hm_fmt_time(active ? pl.duration : -1, b, sizeof b);
        if (active) ui_text(MU.time, "%s / %s", a, b);
        else ui_text(MU.time, "%s", "");
        bz_meter_set(MU.meter, active && pl.duration > 0 ? (float)(pl.position / pl.duration) : 0, BZ_C_ICE);
    }

    /* the PC's */
    home_pc_t pc;
    home_pc_get(&pc);
    if (pc.gen != MU.pc_gen || !MU.pc_gen) {
        MU.pc_gen = pc.gen ? pc.gen : 1;
        if (!hm_cfg()->pc) {
            ui_text(MU.pc_src, "%s", "the pc");
            ui_text(MU.pc_title, "%s", "Off");
            ui_text(MU.pc_artist, "%s", "turn it on in settings, home");
        } else if (pc.have) {
            ui_text(MU.pc_src, "the pc \xc2\xb7 %s", pc.app[0] ? pc.app : "media");
            ui_text(MU.pc_title, "%s", pc.title[0] ? pc.title : "untitled");
            ui_text(MU.pc_artist, "%s", pc.artist[0] ? pc.artist : pc.album);
        } else {
            ui_text(MU.pc_src, "%s", "the pc");
            ui_text(MU.pc_title, "%s", "Nothing playing");
            ui_text(MU.pc_artist, "%s", pc.reason[0] ? pc.reason : pc.link ? "play something on the pc" : "no pc paired: tap the link button to pair it");
        }
        set_play_icon(MU.pc_play, pc.have && pc.playing, &MU.pc_playing);
    }
    if (tick) {
        double pos = pc.position;
        if (pc.have && pc.playing && pos >= 0) pos += now - pc.at;
        if (pc.duration > 0 && pos > pc.duration) pos = pc.duration;
        char a[16], b[16];
        hm_fmt_time(pos, a, sizeof a);
        hm_fmt_time(pc.duration, b, sizeof b);
        if (pc.have && pc.duration > 0) ui_text(MU.pc_time, "%s / %s", a, b);
        else ui_text(MU.pc_time, "%s", "");
        bz_meter_set(MU.pc_meter, pc.have && pc.duration > 0 ? (float)(pos / pc.duration) : 0, BZ_C_ICE);
    }
}

static void music_open(void)
{
    MU.pl_gen = MU.pc_gen = 0;
    MU.current = -2;
    hm_cfg_apply();
    home_player_rescan();
}

const ui_app_t APP_MUSIC = { .name = "music", .icon = BZ_I_GRAPHIC_EQ, .build = music_build, .open = music_open,
                             .refresh = music_refresh };
