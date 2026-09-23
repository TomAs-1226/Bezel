/* Systemcore and Catalyst depth: systemcore, motors, states, controls, recorder. (Placeholders.) */
#include "ui_internal.h"

static void placeholder(lv_obj_t *b, const char *what)
{
    lv_obj_t *l = bz_label(b, what, BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_pos(l, PAD, 120);
}
static void sc_build(lv_obj_t *b) { placeholder(b, "systemcore"); }
static void mh_build(lv_obj_t *b) { placeholder(b, "motors"); }
static void st_build(lv_obj_t *b) { placeholder(b, "states"); }
static void ct_build(lv_obj_t *b) { placeholder(b, "controls"); }
static void rc_build(lv_obj_t *b) { placeholder(b, "recorder"); }

const ui_app_t APP_SYSTEMCORE = { .name = "systemcore", .icon = BZ_I_DEVELOPER_BOARD, .build = sc_build };
const ui_app_t APP_MOTORS = { .name = "motors", .icon = BZ_I_HISTORY, .build = mh_build };
const ui_app_t APP_STATES = { .name = "states", .icon = BZ_I_ACCOUNT_TREE, .build = st_build };
const ui_app_t APP_CONTROLS = { .name = "controls", .icon = BZ_I_SPORTS_ESPORTS, .build = ct_build };
const ui_app_t APP_RECORDER = { .name = "recorder", .icon = BZ_I_FIBER_MANUAL_RECORD, .build = rc_build };
