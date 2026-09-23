/* The assistant and the PC: assist, link. (Placeholders.) */
#include "ui_internal.h"

static void as_build(lv_obj_t *b)
{
    lv_obj_t *l = bz_label(b, "assist", BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_pos(l, PAD, 120);
}
static void ln_build(lv_obj_t *b)
{
    lv_obj_t *l = bz_label(b, "link", BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_pos(l, PAD, 120);
}

const ui_app_t APP_ASSIST = { .name = "assist", .icon = BZ_I_AUTO_AWESOME, .build = as_build };
const ui_app_t APP_LINK = { .name = "link", .icon = BZ_I_COMPUTER, .build = ln_build };
