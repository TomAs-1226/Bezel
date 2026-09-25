/* smart home — every Home Assistant entity picked in Settings > home, by room.
 *
 * A row of room chips ("all", then Home Assistant's areas, then "no room"), and under it a grid of cards, four
 * across: a light toggles on a tap and dims on its level (a drag, sent once on release); a thermostat shows the
 * room's temperature and steps its set point with − and +; switches, fans, covers, scenes, scripts, buttons and
 * media players act on a tap; sensors just show. Rooms need a token that may render templates (an admin's);
 * with another token every card is under "all".
 *
 * Built once per set of entities (and per room): the cards are rebuilt only when the picks or the room
 * change; otherwise each refresh sets only the labels, fills and levels whose value moved. */
#include "ui_home_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define ROOMS_H 56
#define GRID_Y (APP_Y + ROOMS_H + BZ_GAP)
#define GRID_H (H - GRID_Y)
#define COLS 4
#define CARD_W ((W - 2 * PAD - (COLS - 1) * BZ_GAP) / COLS)
#define CARD_H 190
#define CARD_PAD 20
#define CARD_IN (CARD_W - 2 * CARD_PAD)
#define MAX_ROOMS 7            /* chips on one row, with "all" and "no room" */

typedef struct {
    lv_obj_t *card, *icon, *name, *room, *state, *level, *minus, *plus, *target;
    char id[64];
    int kind, lit, bri;
    float tgt;
} sh_card_t;

static struct {
    lv_obj_t *status, *rooms, *grid, *hint;
    lv_obj_t *room_chips[MAX_ROOMS + 2];
    char room_names[MAX_ROOMS][32];
    int nrooms, room;            /* 0 all, 1..nrooms an area, nrooms+1 no room */
    bool has_unroomed;
    home_ha_entity_t ents[HOME_HA_PICKS]; /* the ui component's statics are in PSRAM */
    int n;
    sh_card_t cards[HOME_HA_PICKS];
    int ncards;
    uint32_t layout_sig;         /* the ids and rooms the cards were built for */
    unsigned gen;
} SH;

/* ---- actions ---- */

static const home_ha_entity_t *ent_of(const char *id)
{
    for (int i = 0; i < SH.n; i++)
        if (!strcmp(SH.ents[i].id, id)) return &SH.ents[i];
    return NULL;
}

static void card_tap(lv_obj_t *o, void *u)
{
    (void)o;
    sh_card_t *c = &SH.cards[(intptr_t)u];
    const home_ha_entity_t *e = ent_of(c->id);
    if (e && e->actionable && e->kind != HA_CLIMATE) home_ha_tap(c->id);
}

static void level_moved(lv_obj_t *lv, float v, bool final, void *u)
{
    (void)lv;
    if (!final) return; /* one call on release: Home Assistant isn't asked at every step of a drag */
    sh_card_t *c = &SH.cards[(intptr_t)u];
    c->bri = (int)lroundf(v);
    home_ha_brightness(c->id, c->bri);
}

static void step_temp(lv_obj_t *o, void *u)
{
    (void)o;
    intptr_t k = (intptr_t)u;
    sh_card_t *c = &SH.cards[k >> 1];
    const home_ha_entity_t *e = ent_of(c->id);
    if (!e || isnan(e->target)) return;
    float step = e->step > 0 ? e->step : 0.5f;
    home_ha_set_temp(c->id, e->target + ((k & 1) ? step : -step));
}

static void pick_room(lv_obj_t *o, void *u)
{
    (void)o;
    SH.room = (int)(intptr_t)u;
    SH.layout_sig = 0; /* rebuild the grid for the room */
    SH.gen = 0;
}

static void choose(lv_obj_t *o, void *u)
{
    (void)u;
    hm_open_settings(o);
}

/* ---- building ---- */

static bool in_room(const home_ha_entity_t *e)
{
    if (SH.room == 0) return true;
    if (SH.room == SH.nrooms + 1) return !e->area[0];
    return !strcmp(e->area, SH.room_names[SH.room - 1]);
}

static void build_rooms(void)
{
    lv_obj_clean(SH.rooms);
    memset(SH.room_chips, 0, sizeof SH.room_chips);
    SH.nrooms = 0;
    SH.has_unroomed = false;
    for (int i = 0; i < SH.n; i++) {
        const char *a = SH.ents[i].area;
        if (!a[0]) {
            SH.has_unroomed = true;
            continue;
        }
        bool known = false;
        for (int k = 0; k < SH.nrooms; k++) known = known || !strcmp(SH.room_names[k], a);
        if (!known && SH.nrooms < MAX_ROOMS) snprintf(SH.room_names[SH.nrooms++], sizeof SH.room_names[0], "%s", a);
    }
    if (SH.room > SH.nrooms + 1) SH.room = 0;
    if (SH.nrooms == 0) return; /* no rooms known: no chips, every card shows */
    SH.room_chips[0] = ui_chip(SH.rooms, "all", pick_room, (void *)(intptr_t)0);
    for (int k = 0; k < SH.nrooms; k++) {
        char lower[32];
        snprintf(lower, sizeof lower, "%.16s", SH.room_names[k]); /* a chip never cuts its text: keep it short */
        SH.room_chips[k + 1] = ui_chip(SH.rooms, lower, pick_room, (void *)(intptr_t)(k + 1));
    }
    if (SH.has_unroomed) SH.room_chips[SH.nrooms + 1] = ui_chip(SH.rooms, "no room", pick_room, (void *)(intptr_t)(SH.nrooms + 1));
    for (int k = 0; k <= SH.nrooms + 1; k++)
        if (SH.room_chips[k]) ui_chip_set(SH.room_chips[k], k == SH.room);
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *icon, bz_tap_fn fn, void *u)
{
    lv_obj_t *b = ui_button(parent, icon, NULL, fn, u);
    lv_obj_set_size(b, 64, 52);
    lv_obj_set_style_pad_hor(b, 0, 0);
    return b;
}

static void build_card(int slot, const home_ha_entity_t *e)
{
    sh_card_t *c = &SH.cards[slot];
    memset(c, 0, sizeof *c);
    snprintf(c->id, sizeof c->id, "%s", e->id);
    c->kind = (int)e->kind;
    c->lit = -1;
    c->bri = -2;
    c->tgt = NAN;
    c->card = bz_tile(SH.grid, CARD_W, CARD_H);
    lv_obj_set_style_pad_all(c->card, CARD_PAD, 0);
    if (e->actionable && e->kind != HA_CLIMATE) {
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
        bz_on_tap(c->card, card_tap, (void *)(intptr_t)slot);
    }
    c->icon = bz_icon(c->card, hm_ha_icon(e->kind, e->unit), 32, BZ_C_INK);
    c->state = bz_label_line(c->card, "", BZ_F_LABEL, BZ_C_DIM, CARD_IN - 56);
    lv_obj_set_style_text_align(c->state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(c->state, LV_ALIGN_TOP_RIGHT, 0, 8);
    c->name = bz_label_line(c->card, "", BZ_F_BODY_S, BZ_C_INK, CARD_IN);
    lv_obj_set_pos(c->name, 0, 48);
    c->room = bz_label_line(c->card, "", BZ_F_CAPTION, BZ_C_DIM, CARD_IN);
    lv_obj_set_pos(c->room, 0, 74);
    if (e->kind == HA_LIGHT && e->dimmable) {
        c->level = bz_level(c->card, CARD_IN, 40, 0, 100, 1);
        lv_obj_align(c->level, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        bz_level_on_change(c->level, level_moved, (void *)(intptr_t)slot);
    } else if (e->kind == HA_CLIMATE) {
        lv_obj_t *r = bz_row(c->card, 8);
        lv_obj_set_width(r, CARD_IN);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(r, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        c->minus = small_button(r, BZ_I_REMOVE, step_temp, (void *)(intptr_t)(slot << 1));
        c->target = bz_label_line(r, "", BZ_F_NAME, BZ_C_INK, CARD_IN - 2 * 64 - 16);
        lv_obj_set_style_text_align(c->target, LV_TEXT_ALIGN_CENTER, 0);
        c->plus = small_button(r, BZ_I_ADD, step_temp, (void *)(intptr_t)(slot << 1 | 1));
    }
}

static uint32_t layout_sig(void)
{
    uint32_t h = 2166136261u ^ (uint32_t)SH.room;
    for (int i = 0; i < SH.n; i++) {
        const home_ha_entity_t *e = &SH.ents[i];
        for (const char *s = e->id; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        for (const char *s = e->area; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ (uint32_t)e->kind ^ (e->dimmable ? 64u : 0u) ^ (e->actionable ? 128u : 0u)) * 16777619u;
    }
    return h | 1u;
}

static void rebuild(void)
{
    build_rooms();
    lv_obj_clean(SH.grid);
    SH.ncards = 0;
    for (int i = 0; i < SH.n && SH.ncards < HOME_HA_PICKS; i++)
        if (in_room(&SH.ents[i])) build_card(SH.ncards++, &SH.ents[i]);
    lv_obj_t *sp = bz_box(SH.grid); /* room to scroll the last row clear of the bottom */
    lv_obj_set_size(sp, W - 2 * PAD, 24);
}

/* ---- what changes ---- */

static void update_card(sh_card_t *c, const home_ha_entity_t *e)
{
    ui_text(c->name, "%s", e->name);
    ui_text(c->room, "%s", e->area[0] ? e->area : "");
    char s[40];
    hm_ha_state_text(e, s, sizeof s);
    if (e->kind == HA_CLIMATE && !isnan(e->current)) snprintf(s, sizeof s, "%.1f\xc2\xb0 %s", e->current, e->state);
    ui_text(c->state, "%s", s);
    int lit = e->on && e->actionable && e->kind != HA_CLIMATE;
    if (lit != c->lit) {
        c->lit = lit;
        bz_tile_set_fill(c->card, lit ? BZ_C_ICE : BZ_C_SURFACE1);
        bz_set_color(c->icon, lit ? BZ_C_ON_ICE : BZ_C_INK);
        bz_set_color(c->name, lit ? BZ_C_ON_ICE : BZ_C_INK);
        bz_set_color(c->room, lit ? BZ_C_ON_ICE : BZ_C_DIM);
        bz_set_color(c->state, lit ? BZ_C_ON_ICE : BZ_C_DIM);
    }
    if (c->level) {
        int bri = e->on ? (e->brightness >= 0 ? e->brightness : 100) : 0;
        if (bri != c->bri && (int)lroundf(bz_level_get(c->level)) != bri) {
            c->bri = bri;
            bz_level_set(c->level, (float)bri, true); /* ignored while a finger holds it */
        }
    }
    if (c->target) {
        bool same = (isnan(e->target) && isnan(c->tgt)) || e->target == c->tgt;
        if (!same) {
            c->tgt = e->target;
            if (isnan(e->target)) ui_text(c->target, "%s", "\xe2\x80\x94");
            else ui_text(c->target, "%.1f\xc2\xb0", e->target);
        }
    }
}

static void smarthome_refresh(void)
{
    home_want(HOME_WANT_HA);
    home_ha_status_t st;
    home_ha_status(&st);
    if (st.gen == SH.gen && SH.gen) return;
    SH.gen = st.gen ? st.gen : 1;
    SH.n = st.configured ? home_ha_tiles(SH.ents, HOME_HA_PICKS) : 0;

    if (!st.configured) ui_text(SH.status, "%s", "not set up");
    else if (!st.ok && st.err[0]) ui_text(SH.status, "%s", st.err);
    else if (st.ok) ui_text(SH.status, "%d picked \xc2\xb7 connected", SH.n);
    else ui_text(SH.status, "%s", "connecting...");

    bool empty = SH.n == 0;
    if (empty) {
        ui_text(SH.hint, "%s", !st.configured
                                   ? "Home Assistant isn't set up yet. In settings, home: its address (or find it), then a "
                                     "long-lived token from your Home Assistant profile, then pick what shows here."
                                   : "Nothing picked yet. In settings, home: choose from home assistant, and pick up to 24 "
                                     "lights, switches, thermostats, scenes and sensors.");
    }
    if (empty == lv_obj_has_flag(SH.hint, LV_OBJ_FLAG_HIDDEN)) {
        if (empty) lv_obj_remove_flag(SH.hint, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(SH.hint, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t sig = layout_sig();
    if (sig != SH.layout_sig) {
        SH.layout_sig = sig;
        rebuild();
    }
    for (int i = 0; i < SH.ncards; i++) {
        const home_ha_entity_t *e = ent_of(SH.cards[i].id);
        if (e) update_card(&SH.cards[i], e);
    }
}

static void smarthome_build(lv_obj_t *b)
{
    lv_obj_t *hr = bz_row(b, 14);
    lv_obj_align(hr, LV_ALIGN_TOP_RIGHT, -PAD, 18);
    lv_obj_set_height(hr, 60);
    SH.status = bz_label_line(hr, "", BZ_F_LABEL, BZ_C_DIM, 420);
    lv_obj_set_style_text_align(SH.status, LV_TEXT_ALIGN_RIGHT, 0);
    ui_button(hr, BZ_I_TUNE, "choose", choose, NULL);

    SH.rooms = bz_row(b, 10);
    lv_obj_set_pos(SH.rooms, PAD, APP_Y);
    lv_obj_set_size(SH.rooms, W - 2 * PAD, ROOMS_H);

    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, GRID_Y);
    lv_obj_t *col = ui_scroller(wrap, W - 2 * PAD, GRID_H);
    SH.grid = bz_row(col, BZ_GAP);
    lv_obj_set_flex_flow(SH.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(SH.grid, BZ_GAP, 0);
    lv_obj_set_width(SH.grid, W - 2 * PAD);

    SH.hint = bz_label(b, "", BZ_F_BODY, BZ_C_DIM);
    lv_label_set_long_mode(SH.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(SH.hint, 760);
    lv_obj_set_style_text_align(SH.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(SH.hint, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(SH.hint, LV_OBJ_FLAG_HIDDEN);
}

static void smarthome_open(void)
{
    hm_cfg_apply();
    SH.gen = 0;
    SH.layout_sig = 0;
}

const ui_app_t APP_SMARTHOME = { .name = "smart home", .icon = BZ_I_LIGHTBULB, .build = smarthome_build,
                                 .open = smarthome_open, .refresh = smarthome_refresh };
