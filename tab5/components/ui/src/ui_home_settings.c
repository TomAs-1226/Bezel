/* Settings > home: how the tablet starts, the desk stand and the NFC tag, the photos screensaver, the weather's
 * place, Home Assistant (its address, a long-lived token, and which entities the smart home app shows — the first
 * four also on home mode), and the PC's media through Catalyst Link (with pairing).
 *
 * The token is typed on the keyboard sheet in password mode, kept with hal_kv, never logged and never shown
 * again: the pane says only whether one is set. */
#include "ui_home_priv.h"

#include "ui_companion.h"
#include "link.h"

#include <stdlib.h>
#include <string.h>

#define LIST_SHOWN 48      /* entity chips at once: the kind filter narrows a big house */

enum { ED_PLACE, ED_URL, ED_TOKEN };
static const char *const KINDS[] = { "lights", "switches", "scenes", "sensors", "media", "other" };
#define NKINDS 6

static const int SAVER_MIN[] = { 0, 5, 15, 30 };
#define NSAVER 4

static struct {
    lv_obj_t *start_catalyst, *start_home, *stand, *far, *cel, *pc;
    lv_obj_t *saver[NSAVER], *tag_state, *tag_btn, *tag_forget, *pc_state;
    lv_obj_t *net_state, *net_btn, *net_any;
    int tag_shown;             /* what tag_state says: -2 not yet, else hm_tag_pair_state()'s idea */
    bool tag_waiting;
    lv_obj_t *place, *url, *token, *ha_state, *list_state, *list, *kind[NKINDS];
    lv_obj_t *find_btn;
    ui_kb_t *kb;
    int editing;
    int kind_on;
    bool listing, finding;
    home_ha_entity_t *items;   /* HOME_HA_LIST, while the picker is open */
    int nitems;
    unsigned ha_gen;
} HS;

static void show(void)
{
    hm_cfg_t *c = hm_cfg();
    ui_chip_set(HS.start_catalyst, !c->start_home);
    ui_chip_set(HS.start_home, c->start_home);
    ui_chip_set(HS.stand, c->stand);
    ui_chip_set(HS.cel, !c->fahrenheit);
    ui_chip_set(HS.far, c->fahrenheit);
    ui_chip_set(HS.pc, c->pc);
    for (int i = 0; i < NSAVER; i++) ui_chip_set(HS.saver[i], c->saver_min == SAVER_MIN[i]);
    ui_text(lv_obj_get_child(HS.place, 1), "%s", c->place[0] ? c->place : "set the place");
    ui_text(lv_obj_get_child(HS.url, 1), "%s", c->ha_url[0] ? c->ha_url : "home assistant's address");
    ui_text(lv_obj_get_child(HS.token, 1), "%s", c->ha_token[0] ? "token set \xc2\xb7 change it" : "add a token");
}

static void pick_start(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cfg()->start_home = (intptr_t)u != 0;
    hm_cfg_save();
    show();
}

static void toggle(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cfg_t *c = hm_cfg();
    switch ((intptr_t)u) {
    case 0:
        c->stand = !c->stand;
        if (c->stand) { /* one desk mode decides: the companion's own stops opening itself */
            ui_companion_cfg_t *cc = ui_companion_cfg();
            if (cc->desk_auto) {
                cc->desk_auto = false;
                ui_companion_cfg_save();
            }
        }
        break;
    case 1: c->fahrenheit = false; break;
    case 2: c->fahrenheit = true; break;
    case 3: c->pc = !c->pc; break;
    }
    hm_cfg_save();
    show();
}

/* the home network: what the automatic triggers ask for, so the shop's Wi-Fi never brings home mode */
static void net_show(void)
{
    hm_cfg_t *c = hm_cfg();
    if (c->net[0]) ui_text(HS.net_state, "Only on %s: anywhere else (the shop) the stand and the tag leave it in catalyst mode.", c->net);
    else ui_text(HS.net_state, "%s", "Not set: the stand and the tag bring home mode on any network. Tap \"this is home\" "
                                   "on your home Wi-Fi.");
    if (c->net[0]) lv_obj_remove_flag(HS.net_any, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(HS.net_any, LV_OBJ_FLAG_HIDDEN);
}

static void net_here(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    hal_net_t n;
    hal_net(&n);
    if (!n.up || !n.ssid[0]) {
        ui_island_say(BZ_I_WIFI_OFF, "join your home wi-fi first");
        return;
    }
    snprintf(hm_cfg()->net, sizeof hm_cfg()->net, "%s", n.ssid);
    hm_cfg_save();
    net_show();
}

static void net_anywhere(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    hm_cfg()->net[0] = 0;
    hm_cfg_save();
    net_show();
}

static void pick_saver(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cfg()->saver_min = SAVER_MIN[(intptr_t)u];
    hm_cfg_save();
    show();
}

static void tag_pair(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!hal_nfc_present()) return;
    if (HS.tag_waiting) {
        hm_tag_pair_cancel();
        HS.tag_waiting = false;
        HS.tag_shown = -2;
        return;
    }
    hm_tag_pair_begin();
    HS.tag_waiting = true;
    HS.tag_shown = -2;
}

static void tag_forget(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    hm_cfg()->tag[0] = 0;
    hm_cfg_save();
    HS.tag_shown = -2;
}

static void pair_pc(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_PAIR, o);
}

/* the tag's line, and its button's words */
static void tag_show(void)
{
    int st = hm_tag_pair_state();
    if (st == 2 || (HS.tag_waiting && st == 0)) HS.tag_waiting = false; /* paired, or it gave up waiting */
    int shown = st == -1 ? -1 : HS.tag_waiting ? 1 : hm_cfg()->tag[0] ? 3 : 0;
    if (shown == HS.tag_shown) return;
    HS.tag_shown = shown;
    if (shown == -1)
        ui_text(HS.tag_state, "%s", "No NFC reader: plug M5Stack's Unit RFID 2 into port A (the tablet has none of its own).");
    else if (shown == 1) ui_text(HS.tag_state, "%s", "Hold the tag to the reader...");
    else if (shown == 3) ui_text(HS.tag_state, "Tag %s brings home mode, and leaves it when taken away.", hm_cfg()->tag);
    else ui_text(HS.tag_state, "%s", "No tag yet: pair one, then put it where the tablet stands.");
    ui_text(lv_obj_get_child(HS.tag_btn, 1), "%s", shown == 1 ? "stop" : shown == 3 ? "pair another" : "pair a tag");
    if (shown == 3) lv_obj_remove_flag(HS.tag_forget, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(HS.tag_forget, LV_OBJ_FLAG_HIDDEN);
    if (shown == -1) lv_obj_add_flag(HS.tag_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(HS.tag_btn, LV_OBJ_FLAG_HIDDEN);
}

static void enter_now(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_home_mode_enter(); /* closes the settings first */
}

static void typed(const char *text, void *u)
{
    (void)u;
    hm_cfg_t *c = hm_cfg();
    while (*text == ' ') text++;
    if (HS.editing == ED_PLACE) {
        snprintf(c->place, sizeof c->place, "%s", text);
    } else if (HS.editing == ED_URL) {
        char u2[128];
        /* "192.168.1.30" → "http://192.168.1.30:8123", as Home Assistant answers by default */
        bool scheme = !strncmp(text, "http://", 7) || !strncmp(text, "https://", 8);
        const char *host = scheme ? strstr(text, "://") + 3 : text;
        snprintf(u2, sizeof u2, "%s%s%s", !text[0] || scheme ? "" : "http://", text,
                 !text[0] || strchr(host, ':') ? "" : ":8123");
        snprintf(c->ha_url, sizeof c->ha_url, "%s", u2);
    } else if (text[0]) {
        snprintf(c->ha_token, sizeof c->ha_token, "%s", text);
    }
    hm_cfg_save();
    show();
}

static void edit(lv_obj_t *o, void *u)
{
    (void)o;
    HS.editing = (int)(intptr_t)u;
    hm_cfg_t *c = hm_cfg();
    static const char *const TITLE[3] = {
        "the weather's place: a city (Seattle) or latitude, longitude (47.61, -122.33)",
        "home assistant's address, e.g. http://192.168.1.30:8123",
        "a long-lived access token: home assistant, your profile, security, create token",
    };
    ui_kb_show(HS.kb, TITLE[HS.editing], HS.editing == ED_PLACE ? c->place : HS.editing == ED_URL ? c->ha_url : "",
               HS.editing == ED_TOKEN, true, typed, NULL);
}

static void find(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    HS.finding = true;
    home_ha_discover();
    ui_text(HS.ha_state, "%s", "looking for home assistant on this network...");
}

/* ---- the entity picker ---- */

static bool picked(const char *id)
{
    const char *p = hm_cfg()->picks;
    size_t n = strlen(id);
    while ((p = strstr(p, id))) {
        if ((p == hm_cfg()->picks || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return true;
        p += n;
    }
    return false;
}

static int npicked(void)
{
    const char *p = hm_cfg()->picks;
    if (!p[0]) return 0;
    int n = 1;
    for (; *p; p++) n += *p == ',';
    return n;
}

static void unpick(const char *id)
{
    static char out[sizeof hm_cfg()->picks]; /* ~1.5 KB: PSRAM (this component's statics), not the UI stack */
    out[0] = 0;
    char *copy = strdup(hm_cfg()->picks);
    if (!copy) return;
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ","))
        if (strcmp(t, id) != 0) snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? "," : "", t);
    free(copy);
    snprintf(hm_cfg()->picks, sizeof hm_cfg()->picks, "%s", out);
}

static int kind_group(home_ha_kind_t k)
{
    switch (k) {
    case HA_LIGHT: return 0;
    case HA_SWITCH: case HA_FAN: case HA_COVER: return 1;
    case HA_SCENE: case HA_SCRIPT: case HA_BUTTON: return 2;
    case HA_SENSOR: case HA_BINARY: case HA_CLIMATE: return 3;
    case HA_MEDIA: return 4;
    default: return 5;
    }
}

static void list_show(void);

static void chip_tap(lv_obj_t *o, void *u)
{
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= HS.nitems) return;
    const char *id = HS.items[i].id;
    hm_cfg_t *c = hm_cfg();
    if (picked(id)) {
        unpick(id);
        ui_chip_set(o, false);
    } else if (npicked() >= HOME_HA_PICKS) {
        char m[48];
        snprintf(m, sizeof m, "%d at most: unpick one first", HOME_HA_PICKS);
        ui_island_say(BZ_I_INFO, m);
        return;
    } else {
        size_t l = strlen(c->picks);
        snprintf(c->picks + l, sizeof c->picks - l, "%s%s", l ? "," : "", id);
        ui_chip_set(o, true);
    }
    hm_cfg_save();
    ui_text(HS.list_state, "%d of %d picked \xc2\xb7 the first %d show on home mode", npicked(), HOME_HA_PICKS,
            HM_HOME_TILES);
}

static void list_show(void)
{
    lv_obj_clean(HS.list);
    for (int k = 0; k < NKINDS; k++) ui_chip_set(HS.kind[k], k == HS.kind_on);
    int shown = 0, total = 0;
    for (int i = 0; i < HS.nitems; i++) {
        if (kind_group(HS.items[i].kind) != HS.kind_on) continue;
        total++;
        if (shown >= LIST_SHOWN) continue;
        char label[64];
        const char *nm = HS.items[i].name;
        if (strlen(nm) > 30) snprintf(label, sizeof label, "%.27s...", nm); /* a chip never cuts its text */
        else snprintf(label, sizeof label, "%s", nm);
        lv_obj_t *ch = ui_chip(HS.list, label, chip_tap, (void *)(intptr_t)i);
        ui_chip_set(ch, picked(HS.items[i].id));
        shown++;
    }
    if (total > shown) ui_text(HS.list_state, "%d of %d %s shown \xc2\xb7 %d of %d picked", shown, total,
                               KINDS[HS.kind_on], npicked(), HOME_HA_PICKS);
    else if (!total) ui_text(HS.list_state, "no %s in home assistant", KINDS[HS.kind_on]);
    else ui_text(HS.list_state, "%d of %d picked \xc2\xb7 tap to add or remove", npicked(), HOME_HA_PICKS);
}

static void kind_tap(lv_obj_t *o, void *u)
{
    (void)o;
    HS.kind_on = (int)(intptr_t)u;
    list_show();
}

static void load_list(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    if (!hm_cfg()->ha_url[0] || !hm_cfg()->ha_token[0]) {
        ui_text(HS.list_state, "%s", "set the address and a token first");
        return;
    }
    HS.listing = true;
    home_ha_list_request();
    ui_text(HS.list_state, "%s", "reading home assistant's entities...");
}

/* 10 Hz while the settings app is open (through its hook below) */
static void hs_tick(void *u)
{
    (void)u;
    if (!HS.list || !ui_app_is_open(&APP_SETTINGS)) return;
    home_want(HOME_WANT_HA);
    tag_show();
    link_status_t ls;
    link_status(&ls);
    ui_text(HS.pc_state, "%s", !ls.configured ? "No PC paired yet."
                               : ls.reachable && ls.auth ? "Paired with the PC, and it's answering."
                               : ls.reachable ? "The PC refused the tablet's token: pair again."
                               : "Paired, but the PC isn't answering right now.");
    if (HS.finding) {
        char url[128];
        int r = home_ha_found(url, sizeof url);
        if (r != 0) {
            HS.finding = false;
            if (r > 0) {
                snprintf(hm_cfg()->ha_url, sizeof hm_cfg()->ha_url, "%s", url);
                hm_cfg_save();
                show();
                ui_text(HS.ha_state, "found it at %s", url);
            } else {
                ui_text(HS.ha_state, "%s", "nothing answered: type the address");
            }
        }
    }
    if (HS.listing) {
        if (!HS.items) HS.items = malloc(sizeof *HS.items * HOME_HA_LIST);
        char err[80];
        int n = HS.items ? home_ha_list(HS.items, HOME_HA_LIST, err, sizeof err) : 0;
        if (n >= 0) {
            HS.listing = false;
            HS.nitems = n;
            list_show();
            if (err[0]) ui_text(HS.list_state, "%s", err);
        }
    }
    home_ha_status_t st;
    home_ha_status(&st);
    if (!HS.finding && st.gen != HS.ha_gen) {
        HS.ha_gen = st.gen;
        if (!st.configured) ui_text(HS.ha_state, "%s", "not set up: the address, then a token");
        else if (st.ok) ui_text(HS.ha_state, "%s", "connected");
        else if (st.err[0]) ui_text(HS.ha_state, "%s", st.err);
        else ui_text(HS.ha_state, "%s", "connecting...");
    }
}

static lv_obj_t *wrap(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int w)
{
    lv_obj_t *l = bz_label(parent, text, BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(l, w);
    return l;
}

void ui_home_settings(lv_obj_t *pane, lv_obj_t *body, int w)
{
    lv_obj_update_layout(pane);
    int h = lv_obj_get_content_height(pane) - 60; /* under the pane's head */
    lv_obj_t *col = ui_scroller(pane, w, h > 200 ? h : 200);
    lv_obj_set_style_pad_row(col, 12, 0);

    bz_label(col, "start in", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = wrap(col, w);
    HS.start_catalyst = ui_chip(r, "catalyst mode", pick_start, (void *)(intptr_t)0);
    HS.start_home = ui_chip(r, "home mode", pick_start, (void *)(intptr_t)1);
    ui_button(r, BZ_I_HOME, "home mode now", enter_now, NULL);

    bz_label(col, "desk stand", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.stand = ui_chip(r, "home mode on the stand", toggle, (void *)(intptr_t)0);
    caption(col, "Upright, still and charging for a minute: home mode comes up by itself, and goes when the tablet is "
                 "lifted or unplugged. It replaces the companion's own desk mode.", w);

    bz_label(col, "home network", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.net_btn = ui_button(r, BZ_I_HOME, "this is home", net_here, NULL);
    HS.net_any = ui_button(r, BZ_I_CLOSE, "any network", net_anywhere, NULL);
    HS.net_state = caption(col, "", w);
    net_show();

    bz_label(col, "nfc tag", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.tag_btn = ui_button(r, BZ_I_RADAR, "pair a tag", tag_pair, NULL);
    HS.tag_forget = ui_button(r, BZ_I_CLOSE, "forget it", tag_forget, NULL);
    HS.tag_state = caption(col, "", w);
    HS.tag_shown = -2;

    bz_label(col, "photos when idle", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    for (int i = 0; i < NSAVER; i++) {
        char t[16];
        if (SAVER_MIN[i]) snprintf(t, sizeof t, "after %d min", SAVER_MIN[i]);
        else snprintf(t, sizeof t, "off");
        HS.saver[i] = ui_chip(r, t, pick_saver, (void *)(intptr_t)i);
    }
    caption(col, "On home mode, untouched: the card's photos (CATOS/PHOTOS) one after another. A tap ends it.", w);

    bz_label(col, "weather", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.place = ui_button(r, BZ_I_PIN_DROP, "set the place", edit, (void *)(intptr_t)ED_PLACE);
    HS.cel = ui_chip(r, "\xc2\xb0" "C", toggle, (void *)(intptr_t)1);
    HS.far = ui_chip(r, "\xc2\xb0" "F", toggle, (void *)(intptr_t)2);

    bz_label(col, "home assistant", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.url = ui_button(r, BZ_I_LINK, "home assistant's address", edit, (void *)(intptr_t)ED_URL);
    HS.find_btn = ui_button(r, BZ_I_RADAR, "find it", find, NULL);
    HS.token = ui_button(r, BZ_I_LOCK, "add a token", edit, (void *)(intptr_t)ED_TOKEN);
    HS.ha_state = bz_label_line(col, "", BZ_F_BODY_S, BZ_C_INK, w);

    bz_label(col, "on home mode", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    ui_button(r, BZ_I_REFRESH, "choose from home assistant", load_list, NULL);
    r = wrap(col, w);
    for (int k = 0; k < NKINDS; k++) HS.kind[k] = ui_chip(r, KINDS[k], kind_tap, (void *)(intptr_t)k);
    HS.list_state = bz_label_line(col, "", BZ_F_CAPTION, BZ_C_DIM, w);
    HS.list = wrap(col, w);

    bz_label(col, "music", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    HS.pc = ui_chip(r, "the pc's music", toggle, (void *)(intptr_t)3);
    ui_button(r, BZ_I_LINK, "pair the pc", pair_pc, NULL);
    HS.pc_state = bz_label_line(col, "", BZ_F_BODY_S, BZ_C_INK, w);
    caption(col, "The PC runs Catalyst Link with the media extras (pip install catalyst-link[media]): Spotify, YouTube "
                 "Music, anything Windows shows in its media controls. Pairing shows a code on the PC to type here. "
                 "Songs on the card go in CATOS/AUDIO.", w);
    lv_obj_t *sp = bz_box(col);
    lv_obj_set_height(sp, 40);

    HS.kb = ui_kb_create(body, 420);
    ui_on_refresh(hs_tick, NULL);
}

/* ---- settings > look: home mode's face ---- */

static const char *const CARD_L[5] = { "music", "companion", "smart home", "weather", "next match" };
static const int CARD_BIT[5] = { HM_CARD_MUSIC, HM_CARD_COMPANION, HM_CARD_HOME, HM_CARD_WEATHER, HM_CARD_MATCH };

static struct {
    lv_obj_t *card[5], *big, *small, *secs, *plain, *tint;
} LK;

static void look_show(void)
{
    if (!LK.big) return;
    hm_cfg_t *c = hm_cfg();
    for (int i = 0; i < 5; i++) ui_chip_set(LK.card[i], c->cards & CARD_BIT[i]);
    ui_chip_set(LK.big, !c->small_clock);
    ui_chip_set(LK.small, c->small_clock);
    ui_chip_set(LK.secs, c->seconds);
    ui_chip_set(LK.plain, !c->tint);
    ui_chip_set(LK.tint, c->tint);
}

static void look_tap(lv_obj_t *o, void *u)
{
    (void)o;
    hm_cfg_t *c = hm_cfg();
    int k = (int)(intptr_t)u;
    if (k < 5) c->cards ^= CARD_BIT[k];
    else if (k == 5 || k == 6) c->small_clock = k == 6;
    else if (k == 7) c->seconds = !c->seconds;
    else c->tint = k == 9;
    hm_cfg_save();
    hm_look_changed(); /* laid out now, under the settings: it shows as it is when they close */
    look_show();
}

void ui_home_look_settings(lv_obj_t *col, int w)
{
    bz_label(col, "home mode shows", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = wrap(col, w);
    for (int i = 0; i < 5; i++) LK.card[i] = ui_chip(r, CARD_L[i], look_tap, (void *)(intptr_t)i);
    bz_label(col, "home mode clock", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    LK.big = ui_chip(r, "big", look_tap, (void *)(intptr_t)5);
    LK.small = ui_chip(r, "small", look_tap, (void *)(intptr_t)6);
    LK.secs = ui_chip(r, "seconds", look_tap, (void *)(intptr_t)7);
    bz_label(col, "home mode ground", BZ_F_LABEL, BZ_C_DIM);
    r = wrap(col, w);
    LK.plain = ui_chip(r, "plain", look_tap, (void *)(intptr_t)8);
    LK.tint = ui_chip(r, "tinted with the accent", look_tap, (void *)(intptr_t)9);
    caption(col, "Seconds redraw one small label a second; everything else on home mode stays still until it changes.", w);
    ui_button(col, BZ_I_HOME, "see home mode", enter_now, NULL);
    look_show();
}

void ui_home_look_settings_open(void) { look_show(); }

void ui_home_settings_open(void)
{
    if (!HS.list) return;
    ui_kb_hide(HS.kb);
    HS.ha_gen = 0;
    HS.tag_shown = -2;
    show();
    ui_text(HS.list_state, "%d of %d picked", npicked(), HOME_HA_PICKS);
}
