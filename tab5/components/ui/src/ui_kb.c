/* A keyboard sheet: LVGL's keyboard in Bezel's tokens, rising over the bottom of an app for one entry
 * (a Wi-Fi password, the Link's address, a question for the assistant). */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

struct ui_kb {
    lv_obj_t *sheet, *title, *ta, *kb;
    ui_kb_done_fn done;
    void *user;
};

static void kb_event(lv_event_t *e)
{
    ui_kb_t *k = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
    lv_obj_add_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
    if (code == LV_EVENT_READY && k->done) k->done(lv_textarea_get_text(k->ta), k->user);
}

ui_kb_t *ui_kb_create(lv_obj_t *body, int h)
{
    ui_kb_t *k = calloc(1, sizeof *k);
    k->sheet = bz_tile(body, W - 2 * PAD, h);
    lv_obj_align(k->sheet, LV_ALIGN_BOTTOM_MID, 0, -PAD);
    bz_tile_set_fill(k->sheet, BZ_C_SURFACE2);
    k->title = bz_label(k->sheet, "", BZ_F_LABEL, BZ_C_DIM);
    k->ta = lv_textarea_create(k->sheet);
    lv_obj_set_width(k->ta, W - 2 * PAD - 2 * BZ_PAD_TILE);
    lv_obj_set_pos(k->ta, 0, 28);
    lv_obj_add_style(k->ta, bz_style_font(BZ_F_BODY), 0);
    lv_obj_add_style(k->ta, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_add_style(k->ta, bz_style_color(BZ_C_INK), 0);
    lv_obj_set_style_radius(k->ta, 16, 0);
    lv_obj_set_style_border_width(k->ta, 0, 0);
    lv_obj_set_style_pad_all(k->ta, 12, 0);
    k->kb = lv_keyboard_create(k->sheet);
    lv_keyboard_set_textarea(k->kb, k->ta);
    lv_obj_set_size(k->kb, W - 2 * PAD - 2 * BZ_PAD_TILE, h - 130);
    lv_obj_align(k->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(k->kb, bz_style_plain(), 0);
    lv_obj_add_style(k->kb, bz_style_fill(BZ_C_SURFACE3), LV_PART_ITEMS);
    lv_obj_add_style(k->kb, bz_style_color(BZ_C_INK), LV_PART_ITEMS);
    /* the keyboard's control keys are LVGL symbols, which only its own Montserrat carries */
    lv_obj_set_style_text_font(k->kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(k->kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(k->kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(k->kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(k->kb, kb_event, LV_EVENT_ALL, k);
    lv_obj_add_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
    return k;
}

void ui_kb_show(ui_kb_t *k, const char *title, const char *text, bool secret, bool one_line, ui_kb_done_fn done,
                void *user)
{
    k->done = done;
    k->user = user;
    ui_text(k->title, "%s", title);
    lv_textarea_set_one_line(k->ta, one_line);
    lv_textarea_set_password_mode(k->ta, secret);
    lv_obj_set_height(k->ta, one_line ? LV_SIZE_CONTENT : 96);
    lv_textarea_set_text(k->ta, text ? text : "");
    lv_obj_remove_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(k->sheet);
}

bool ui_kb_open(const ui_kb_t *k) { return k && !lv_obj_has_flag(k->sheet, LV_OBJ_FLAG_HIDDEN); }

void ui_kb_hide(ui_kb_t *k)
{
    if (k) lv_obj_add_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
}
