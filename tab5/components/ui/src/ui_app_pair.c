/* pair pc — the tablet and the PC's Catalyst Link, paired by a code the PC shows (docs/link-api.md, "Pairing").
 *
 * Left: the Links on this network (mDNS _catalyst-link._tcp, looked for as the app opens), or an address typed
 * by hand. Right: the code. Picking a PC asks it for a code; the PC shows six digits in its console (and as a
 * Windows notification); they're typed on the keypad here, and the Link answers with its token, which is saved
 * like one typed in the link app ("link_url", "link_token") and used by everything that talks to the PC — the
 * assistant, the inbox, and home mode's music card.
 *
 * All the network work is on the Link's own poller thread (link.c); this only reads its state at 10 Hz and
 * redraws what changed. */
#include "ui_home_priv.h"

#include "link.h"

#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define IN(w) ((w) - 2 * BZ_PAD_TILE)
#define LW 560
#define RX (PAD + LW + BZ_GAP)
#define RW (W - PAD - RX)
#define PEERS 4
#define DIGITS 6
/* the boxes and the keypad take the card's width (a keypad a third as wide left an empty half beside it) */
#define BOX_W ((IN(RW) - 5 * 10 - 20) / 6)
#define BOX_H 88
#define KEY_W ((IN(RW) - 2 * 12) / 3)
#define KEY_H 64

static struct {
    lv_obj_t *head_state;
    lv_obj_t *peer_rows[PEERS], *peer_name[PEERS], *peer_url[PEERS], *look_state;
    lv_obj_t *title, *boxes[DIGITS], *digits[DIGITS], *msg, *pad, *done;
    ui_kb_t *kb;
    link_peer_t peers[PEERS];
    int npeers;
    uint32_t peers_sig;
    bool looking;
    char code[DIGITS + 1];
    unsigned gen;
    int shown_state;
    char chosen[96];
} PR = { .shown_state = -1 };

static void save_result(void)
{
    char url[96], token[80], name[48];
    if (!link_pair_take(url, sizeof url, token, sizeof token, name, sizeof name)) return;
    hal_kv_set("link_url", url);
    hal_kv_set("link_token", token);
    link_configure(url, token);
    memset(token, 0, sizeof token);
    /* home mode's music card reads the PC through this pairing */
    if (!hm_cfg()->pc) {
        hm_cfg()->pc = true;
        hm_cfg_save();
    }
    char m[80];
    snprintf(m, sizeof m, "paired with %s", name[0] ? name : "the pc");
    ui_island_say(BZ_I_COMPUTER, m);
}

static void show_code(void)
{
    size_t n = strlen(PR.code);
    for (int i = 0; i < DIGITS; i++) {
        char d[2] = { (size_t)i < n ? PR.code[i] : 0, 0 };
        ui_text(PR.digits[i], "%s", d);
        bz_tile_set_fill(PR.boxes[i], (size_t)i == n ? BZ_C_SURFACE3 : BZ_C_SURFACE2);
    }
}

static void start(const char *url)
{
    snprintf(PR.chosen, sizeof PR.chosen, "%s", url);
    PR.code[0] = 0;
    show_code();
    link_pair_start(url, "catalyst tab");
}

static void peer_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= PR.npeers) return;
    start(PR.peers[i].url);
}

static void look(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    PR.looking = true;
    PR.peers_sig = 0;
    link_discover();
    ui_text(PR.look_state, "%s", "looking on this network...");
}

static void typed(const char *text, void *u)
{
    (void)u;
    while (*text == ' ') text++;
    if (text[0]) start(text);
}

static void type_address(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_kb_show(PR.kb, "the pc's address, e.g. 192.168.1.20 (catalyst link prints it)", "", false, true, typed, NULL);
}

static void submit(void)
{
    if (strlen(PR.code) == DIGITS) link_pair_code(PR.code);
}

static void key(lv_obj_t *o, void *u)
{
    (void)o;
    int k = (int)(intptr_t)u;
    link_pair_t st;
    link_pair_status(&st);
    if (st.state != LINK_PAIR_CODE) return;
    size_t n = strlen(PR.code);
    if (k >= 0 && k <= 9 && n < DIGITS) {
        PR.code[n] = (char)('0' + k);
        PR.code[n + 1] = 0;
    } else if (k == 10 && n) {
        PR.code[n - 1] = 0;
    } else if (k == 11) {
        submit();
        return;
    }
    show_code();
    if (strlen(PR.code) == DIGITS) submit();
}

static void done(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    link_pair_cancel();
    ui_app_close();
}

static void pair_build(lv_obj_t *b)
{
    lv_obj_t *hr = bz_row(b, 14);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 18);
    lv_obj_set_height(hr, 60);
    PR.head_state = bz_label_line(hr, "", BZ_F_LABEL, BZ_C_DIM, 560);
    lv_obj_set_style_text_align(PR.head_state, LV_TEXT_ALIGN_RIGHT, 0);

    /* the PCs */
    lv_obj_t *l = bz_tile(b, LW, APP_H);
    lv_obj_set_pos(l, PAD, APP_Y);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 12, 0);
    bz_label(l, "catalyst link on this network", BZ_F_LABEL, BZ_C_DIM);
    for (int i = 0; i < PEERS; i++) {
        lv_obj_t *r = bz_tile(l, IN(LW), 64);
        bz_tile_set_fill(r, BZ_C_SURFACE2);
        lv_obj_set_style_radius(r, 22, 0);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_set_style_pad_ver(r, 0, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(r, peer_tap, (void *)(intptr_t)i);
        lv_obj_t *ic = bz_icon(r, BZ_I_COMPUTER, 32, BZ_C_INK);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
        PR.peer_name[i] = bz_label_line(r, "", BZ_F_BODY, BZ_C_INK, IN(LW) - 36 - 60);
        lv_obj_set_pos(PR.peer_name[i], 56, 6);
        PR.peer_url[i] = bz_label_line(r, "", BZ_F_CAPTION, BZ_C_DIM, IN(LW) - 36 - 60);
        lv_obj_set_pos(PR.peer_url[i], 56, 36);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        PR.peer_rows[i] = r;
    }
    PR.look_state = bz_label(l, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(PR.look_state, IN(LW));
    lv_label_set_long_mode(PR.look_state, LV_LABEL_LONG_WRAP);
    lv_obj_t *br = bz_row(l, 10);
    ui_button(br, BZ_I_RADAR, "look again", look, NULL);
    ui_button(br, BZ_I_KEYBOARD, "type an address", type_address, NULL);

    /* the code */
    lv_obj_t *r = bz_tile(b, RW, APP_H);
    lv_obj_set_pos(r, RX, APP_Y);
    bz_label(r, "the code", BZ_F_LABEL, BZ_C_DIM);
    PR.title = bz_label_line(r, "", BZ_F_NAME, BZ_C_INK, IN(RW));
    lv_obj_set_pos(PR.title, 0, 30);
    for (int i = 0; i < DIGITS; i++) {
        lv_obj_t *bx = bz_tile(r, BOX_W, BOX_H);
        bz_tile_set_fill(bx, BZ_C_SURFACE2);
        lv_obj_set_style_radius(bx, 18, 0);
        lv_obj_set_style_pad_all(bx, 0, 0);
        /* three and three, like the PC shows it */
        lv_obj_set_pos(bx, i * (BOX_W + 10) + (i >= 3 ? 20 : 0), 80);
        PR.digits[i] = bz_label(bx, "", BZ_F_TITLE, BZ_C_INK);
        lv_obj_center(PR.digits[i]);
        PR.boxes[i] = bx;
    }
    PR.msg = bz_label_line(r, "", BZ_F_BODY_S, BZ_C_DIM, IN(RW));
    lv_obj_set_pos(PR.msg, 0, 184);
    PR.pad = bz_box(r);
    lv_obj_set_pos(PR.pad, 0, 226);
    lv_obj_set_size(PR.pad, 3 * KEY_W + 2 * 12, 4 * KEY_H + 3 * 12);
    static const char *const LBL[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", NULL, "0", NULL };
    static const int KEY[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
    for (int i = 0; i < 12; i++) {
        lv_obj_t *k = ui_button(PR.pad, i == 9 ? BZ_I_BACKSPACE : i == 11 ? BZ_I_CHECK_CIRCLE : NULL,
                                LBL[i], key, (void *)(intptr_t)KEY[i]);
        lv_obj_set_size(k, KEY_W, KEY_H);
        lv_obj_set_style_pad_hor(k, 0, 0);
        lv_obj_set_pos(k, (i % 3) * (KEY_W + 12), (i / 3) * (KEY_H + 12));
        if (LBL[i]) bz_set_font(lv_obj_get_child(k, 0), BZ_F_NAME);
    }
    PR.done = ui_button(r, BZ_I_CHECK_CIRCLE, "done", done, NULL);
    lv_obj_set_pos(PR.done, 0, 226);
    lv_obj_add_flag(PR.done, LV_OBJ_FLAG_HIDDEN);

    PR.kb = ui_kb_create(b, 420);
}

static void show_peers(void)
{
    bool busy;
    link_peer_t found[PEERS];
    int n = link_peers(found, PEERS, &busy);
    uint32_t sig = (uint32_t)n * 2654435761u + (busy ? 1u : 0u) + 7;
    for (int i = 0; i < n; i++)
        for (const char *c = found[i].url; *c; c++) sig = sig * 31 + (uint8_t)*c;
    if (sig == PR.peers_sig) return;
    PR.peers_sig = sig;
    PR.npeers = n;
    memcpy(PR.peers, found, sizeof found);
    for (int i = 0; i < PEERS; i++) {
        if (i < n) {
            ui_text(PR.peer_name[i], "%s", found[i].name);
            ui_text(PR.peer_url[i], "%s", found[i].url);
            lv_obj_remove_flag(PR.peer_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(PR.peer_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (busy) ui_text(PR.look_state, "%s", "looking on this network...");
    else if (!n) ui_text(PR.look_state, "%s", "No Catalyst Link answered. On the PC: catalyst-link serve --repo <robot repo> "
                                              "(with zeroconf installed), on the same network as the tablet.");
    else ui_text(PR.look_state, "%s", "Tap the PC to pair with.");
}

static void pair_refresh(void)
{
    show_peers();
    link_status_t ls;
    link_status(&ls);
    ui_text(PR.head_state, "%s%s", ls.reachable && ls.auth ? "paired with " : !ls.configured ? "no pc paired" : "the pc isn't answering",
            ls.reachable && ls.auth ? (ls.name[0] ? ls.name : "the pc") : "");
    link_pair_t st;
    link_pair_status(&st);
    if (st.state == LINK_PAIR_DONE) save_result();
    if (st.gen == PR.gen && PR.shown_state >= 0) return;
    PR.gen = st.gen;
    if ((int)st.state != PR.shown_state && (st.state == LINK_PAIR_CODE || st.state == LINK_PAIR_FAILED)) {
        PR.code[0] = 0; /* a new code, or a start again: the boxes empty */
        show_code();
    }
    PR.shown_state = (int)st.state;
    /* the keys work only while a code is being typed: faint otherwise, so they don't look dead */
    lv_obj_set_style_opa(PR.pad, st.state == LINK_PAIR_CODE ? LV_OPA_COVER : LV_OPA_40, 0);
    const char *who = st.name[0] ? st.name : "the pc";
    switch (st.state) {
    case LINK_PAIR_IDLE: ui_text(PR.title, "%s", "pick a pc on the left"); break;
    case LINK_PAIR_ASKING: ui_text(PR.title, "asking %s for a code", who); break;
    case LINK_PAIR_CODE:
    case LINK_PAIR_CHECKING: ui_text(PR.title, "type the code %s shows", who); break;
    case LINK_PAIR_DONE: ui_text(PR.title, "paired with %s", who); break;
    case LINK_PAIR_FAILED: ui_text(PR.title, "%s", "not paired"); break;
    }
    ui_text(PR.msg, "%s", st.state == LINK_PAIR_IDLE ? "the pc then shows six digits to type here"
                          : st.state == LINK_PAIR_DONE ? "home mode's music and the assistant now use this pc"
                          : st.msg);
    bool finished = st.state == LINK_PAIR_DONE;
    if (finished != !lv_obj_has_flag(PR.done, LV_OBJ_FLAG_HIDDEN)) {
        if (finished) {
            lv_obj_remove_flag(PR.done, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(PR.pad, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(PR.done, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(PR.pad, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void pair_open(void)
{
    ui_kb_hide(PR.kb);
    link_pair_t st;
    link_pair_status(&st);
    if (st.state == LINK_PAIR_DONE || st.state == LINK_PAIR_FAILED) link_pair_cancel(); /* a fresh start */
    PR.code[0] = 0;
    show_code();
    PR.shown_state = -1;
    look(NULL, NULL);
}

const ui_app_t APP_PAIR = { .name = "pair pc", .icon = BZ_I_LINK, .build = pair_build, .open = pair_open,
                            .refresh = pair_refresh };
