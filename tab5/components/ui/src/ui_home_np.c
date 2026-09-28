/* Home mode's now-playing page, right of the desk: the cover big, the song, its progress and the transport, on a
 * surface tinted from the cover (Monet, as Bezel does it: the cover's seed at fixed OKLCH tones, so text keeps its
 * contrast in dark and in light).
 *
 * The cover is the one big picture home mode keeps: the "hero", the cover at HOME_ART_L set on the page's own
 * ground with its rounded corners and its shadow already in it (home_art_hero), painted on the covers' worker
 * whenever the song or the tone changes. It meets the flat ground LVGL paints around it at exactly the same 565
 * value, so there is no seam; the page takes the new tint only when the new picture is there, so the old picture
 * never shows on the new ground. Its memory (~390 KB) is given back when home mode is left.
 *
 * At rest: the progress and its time once a second while something plays, the small clock once a minute. */
#include "ui_home_priv.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CS HOME_ART_L                 /* the cover: 360 */
#define HERO_W (CS + 80)
#define HERO_H (CS + 84)
#define COVER_DX 28                   /* the cover inside the hero picture (the rest is shadow and ground) */
#define COVER_DY 24
#define COVER_X 108                   /* the cover on the page */
#define COVER_Y 172
#define RADIUS 26
#define TX (COVER_X + CS + 76)        /* the text column */
#define TW (W - TX - 96)
#define BIG_BTN 104
#define BTN 76

static struct {
    lv_obj_t *pg, *hero_img, *ph, *ph_icon, *clock;
    lv_obj_t *src, *title, *by, *meter, *t_pos, *t_dur, *ctl, *play, *songs;
    lv_obj_t *btn[5];
    uint16_t *hero;
    lv_image_dsc_t hero_dsc;
    unsigned song_gen, hero_shown;
    uint32_t asked_key;               /* the cover the hero was last asked for */
    uint16_t asked_ground;
    bool asked_dark, asked;
    home_tint_t asked_tint;
    bool asked_tinted;
    int last_sec, last_min;
    bool built, playing_shown, release;
    int have_shown;                   /* -1 not yet */
    int colors;                       /* -1 not yet, 0 Bezel's roles, 1 a tint */
    uint32_t tint_sig;
} NP = { .last_min = -1, .colors = -1, .have_shown = -1 };

static void open_music(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_MUSIC, o);
}

static void cmd(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cmd((const char *)u);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *icon, int d, const char *action)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, cmd, (void *)action);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_pad_hor(b, 0, 0);
    return b;
}

lv_obj_t *hm_np_build(lv_obj_t *parent)
{
    lv_obj_t *pg = bz_box(parent);
    NP.pg = pg;
    lv_obj_set_size(pg, W, H);
    lv_obj_add_style(pg, bz_style_fill(BZ_C_GROUND), 0);

    NP.clock = bz_label_line(pg, "", BZ_F_LABEL, BZ_C_DIM, 400);
    lv_obj_set_pos(NP.clock, 58, 42);

    /* the cover: the hero picture once painted; before (and with nothing playing) a quiet square */
    NP.ph = bz_tile(pg, CS, CS);
    bz_tile_set_fill(NP.ph, BZ_C_SURFACE2);
    lv_obj_set_style_radius(NP.ph, RADIUS, 0);
    lv_obj_set_pos(NP.ph, COVER_X, COVER_Y);
    lv_obj_add_flag(NP.ph, LV_OBJ_FLAG_CLICKABLE);
    bz_on_tap(NP.ph, open_music, NULL);
    NP.ph_icon = bz_icon(NP.ph, BZ_I_GRAPHIC_EQ, 48, BZ_C_DIM);
    lv_obj_center(NP.ph_icon);
    NP.hero_img = lv_image_create(pg);
    lv_obj_set_pos(NP.hero_img, COVER_X - COVER_DX, COVER_Y - COVER_DY);
    lv_obj_add_flag(NP.hero_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(NP.hero_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(NP.hero_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    bz_on_tap(NP.hero_img, open_music, NULL);

    /* the words: where it plays, the title (two lines), who and from what */
    NP.src = bz_label_line(pg, "", BZ_F_LABEL, BZ_C_DIM, TW);
    lv_obj_set_pos(NP.src, TX, COVER_Y + 14);
    NP.title = bz_label(pg, "", BZ_F_TITLE, BZ_C_INK);
    lv_label_set_long_mode(NP.title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(NP.title, TW, 2 * lv_font_get_line_height(bz_font(BZ_F_TITLE)));
    lv_obj_set_pos(NP.title, TX, COVER_Y + 44);
    NP.by = bz_label(pg, "", BZ_F_BODY, BZ_C_DIM);
    lv_label_set_long_mode(NP.by, LV_LABEL_LONG_DOT);
    lv_obj_set_size(NP.by, TW, 2 * lv_font_get_line_height(bz_font(BZ_F_BODY)));
    lv_obj_set_pos(NP.by, TX, COVER_Y + 44 + 2 * lv_font_get_line_height(bz_font(BZ_F_TITLE)) + 8);

    /* the progress: a capsule, and the time either side under it */
    int my = COVER_Y + 232;
    NP.meter = bz_meter(pg, TW, 8);
    lv_obj_set_pos(NP.meter, TX, my);
    NP.t_pos = bz_label_line(pg, "", BZ_F_LABEL, BZ_C_DIM, 160);
    lv_obj_set_pos(NP.t_pos, TX, my + 18);
    NP.t_dur = bz_label_line(pg, "", BZ_F_LABEL, BZ_C_DIM, 160);
    lv_obj_set_style_text_align(NP.t_dur, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(NP.t_dur, TX + TW - 160, my + 18);

    /* the transport: back, the big play, forward; the volume apart on the right */
    NP.ctl = bz_row(pg, 18);
    lv_obj_set_pos(NP.ctl, TX - 4, COVER_Y + CS - BIG_BTN + 4);
    NP.btn[0] = round_btn(NP.ctl, BZ_I_ARROW_BACK, BTN, "previous");
    NP.btn[1] = NP.play = round_btn(NP.ctl, BZ_I_PLAY_ARROW, BIG_BTN, "toggle");
    ui_chip_set(NP.play, true);
    bz_icon_set(lv_obj_get_child(NP.play, 0), BZ_I_PLAY_ARROW, 40, false);
    NP.btn[2] = round_btn(NP.ctl, BZ_I_ARROW_FORWARD, BTN, "next");
    lv_obj_t *gap = bz_box(NP.ctl);
    lv_obj_set_size(gap, TW - 2 * BTN - BIG_BTN - 2 * 64 - 5 * 18 + 8, 1);
    NP.btn[3] = round_btn(NP.ctl, BZ_I_REMOVE, 64, "volume_down");
    NP.btn[4] = round_btn(NP.ctl, BZ_I_ADD, 64, "volume_up");

    /* nothing playing: the way to the card's songs */
    NP.songs = ui_button(pg, BZ_I_GRAPHIC_EQ, "songs on the card", open_music, NULL);
    lv_obj_set_pos(NP.songs, TX, COVER_Y + CS - 56);
    lv_obj_add_flag(NP.songs, LV_OBJ_FLAG_HIDDEN);

    NP.hero_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    NP.hero_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    NP.hero_dsc.header.w = HERO_W;
    NP.hero_dsc.header.h = HERO_H;
    NP.hero_dsc.header.stride = HERO_W * 2;
    NP.hero_dsc.data_size = HERO_W * HERO_H * 2;
    NP.built = true;
    return pg;
}

/* Bezel's roles, or the tint's colours, on everything the page draws */
static void colors(const home_tint_t *t)
{
    int want = t ? 1 : 0;
    uint32_t sig = t ? t->ground ^ t->accent * 3u ^ t->dim * 7u : 0;
    if (want == NP.colors && sig == NP.tint_sig) return;
    NP.colors = want;
    NP.tint_sig = sig;
    uint32_t R = HM_ROLE;
    hm_fill(NP.pg, t ? t->ground : R);
    hm_ink(NP.title, t ? t->ink : R);
    lv_obj_t *dims[] = { NP.src, NP.by, NP.t_pos, NP.t_dur, NP.clock };
    for (size_t i = 0; i < sizeof dims / sizeof *dims; i++) hm_ink(dims[i], t ? t->dim : R);
    hm_fill(NP.meter, t ? t->track : R);
    hm_fill(lv_obj_get_child(NP.meter, 0), t ? t->accent : R);
    for (int i = 0; i < 5; i++) {
        bool play = NP.btn[i] == NP.play;
        hm_fill(NP.btn[i], t ? (play ? t->accent : t->surface2) : R);
        hm_ink(lv_obj_get_child(NP.btn[i], 0), t ? (play ? t->on_accent : t->ink) : R);
    }
    hm_fill(NP.songs, t ? t->surface2 : R);
    for (uint32_t i = 0; i < lv_obj_get_child_count(NP.songs); i++) hm_ink(lv_obj_get_child(NP.songs, (int32_t)i), t ? t->ink : R);
}

static bool hero_alloc(void)
{
    if (NP.hero) return true;
    NP.hero = malloc(sizeof(uint16_t) * HERO_W * HERO_H);
    NP.hero_dsc.data = (const uint8_t *)NP.hero;
    return NP.hero != NULL;
}

static void show_cover(bool hero)
{
    hm_show(NP.hero_img, hero);
    hm_show(NP.ph, !hero);
}

void hm_np_refresh(bool shown)
{
    if (!NP.built) return;
    if (NP.release) {
        /* home mode was left while the worker painted into the hero: give it back once it has finished */
        hm_np_release();
        return;
    }
    hm_song_t s;
    hm_song(&s);
    if (s.gen != NP.song_gen || !NP.song_gen) {
        NP.song_gen = s.gen ? s.gen : 1;
        ui_text(NP.src, "%s", s.src);
        ui_text(NP.title, "%s", s.have ? s.title : "Nothing playing");
        if (!s.have) ui_text(NP.by, "%s", s.hint);
        else if (s.artist[0] && s.album[0]) ui_text(NP.by, "%s \xc2\xb7 %s", s.artist, s.album);
        else ui_text(NP.by, "%s", s.artist[0] ? s.artist : s.album);
        if (s.playing != NP.playing_shown) {
            NP.playing_shown = s.playing;
            bz_icon_set(lv_obj_get_child(NP.play, 0), s.playing ? BZ_I_PAUSE : BZ_I_PLAY_ARROW, 40, false);
        }
        if ((int)s.have != NP.have_shown) {
            NP.have_shown = s.have;
            hm_show(NP.ctl, s.have);
            hm_show(NP.meter, s.have && s.dur > 0);
            hm_show(NP.t_pos, s.have && s.dur > 0);
            hm_show(NP.t_dur, s.have && s.dur > 0);
            hm_show(NP.songs, !s.have);
        }
        if (s.have) {
            hm_show(NP.meter, s.dur > 0);
            hm_show(NP.t_pos, s.dur > 0);
            hm_show(NP.t_dur, s.dur > 0);
        }
    }

    /* the cover: asked for when the song, the tone or the tint setting changes; taken, with its colours, when
     * it's painted */
    const hm_art_t *a = s.have ? hm_song_art(&s) : NULL;
    bool dark = bz_ui_dark();
    if (!s.have) {
        if (NP.hero_shown || NP.asked) {
            NP.hero_shown = 0;
            NP.asked = false;
            NP.asked_key = 0;
        }
        show_cover(false);
        colors(NULL);
    } else if (a) {
        home_tint_t t;
        home_art_tint(hm_art_info(a)->seed, dark, &t);
        bool tinted = hm_cfg()->art_tint;
        uint16_t ground = hm_565(tinted ? t.ground : bz_color(BZ_C_GROUND));
        if (!NP.asked || hm_art_key(a) != NP.asked_key || ground != NP.asked_ground || dark != NP.asked_dark) {
            if (hero_alloc() &&
                hm_art_hero(a, NP.hero, HERO_W, HERO_H, COVER_DX, COVER_DY, RADIUS, ground, dark)) {
                NP.asked = true;
                NP.asked_key = hm_art_key(a);
                NP.asked_ground = ground;
                NP.asked_dark = dark;
                NP.asked_tint = t;
                NP.asked_tinted = tinted;
            }
        }
    }
    unsigned done = NP.asked ? hm_art_hero_done() : 0;
    if (done && done != NP.hero_shown) {
        NP.hero_shown = done;
        lv_image_cache_drop(&NP.hero_dsc);
        lv_image_set_src(NP.hero_img, &NP.hero_dsc);
        lv_obj_invalidate(NP.hero_img);
        show_cover(true);
        colors(NP.asked_tinted ? &NP.asked_tint : NULL);
    }

    if (!shown) return;
    double now = hm_now();
    int sec = (int)now;
    if (sec != NP.last_sec) {
        NP.last_sec = sec;
        if (s.have && s.dur > 0) {
            char a1[16], b1[16];
            hm_fmt_time(s.pos, a1, sizeof a1);
            hm_fmt_time(s.dur, b1, sizeof b1);
            ui_text(NP.t_pos, "%s", a1);
            ui_text(NP.t_dur, "%s", b1);
            bz_meter_set(NP.meter, (float)(s.pos / s.dur), BZ_C_ICE);
        }
        time_t tt = time(NULL);
        struct tm tm;
        localtime_r(&tt, &tm);
        if (tm.tm_min != NP.last_min) {
            NP.last_min = tm.tm_min;
            if (tm.tm_year > 120) {
                char d[48];
                strftime(d, sizeof d, "%H:%M \xc2\xb7 %A", &tm);
                for (char *c = d; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
                ui_text(NP.clock, "%s", d);
            } else {
                ui_text(NP.clock, "%s", "");
            }
        }
    }
}

void hm_np_release(void)
{
    if (!NP.built) return;
    if (hm_art_busy()) {
        NP.release = true; /* the worker may be painting into it */
        return;
    }
    NP.release = false;
    if (NP.hero) {
        lv_image_cache_drop(&NP.hero_dsc);
        show_cover(false);
        free(NP.hero);
        NP.hero = NULL;
        NP.hero_dsc.data = NULL;
    }
    NP.asked = false;
    NP.asked_key = 0;
    NP.hero_shown = 0;
    NP.colors = -1;
    NP.song_gen = 0;
    NP.have_shown = -1;
    colors(NULL);
}

bool hm_np_released(void) { return !NP.hero && !NP.release; }
