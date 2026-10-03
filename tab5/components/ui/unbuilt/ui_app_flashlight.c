/* NOT COMPILED: the flashlight app and its control-center toggle, taken out of the build (components/ui/CMakeLists.txt):
 * the panel at full white is a tablet's job on any phone, and it was a window, an icon and a tile. To bring it back:
 * move this into ui_apps_util.c, add &APP_LIGHT to ui_pages2.c, "flashlight" to ui_icon.c, ui_light_cc_add() to
 * ui_cc.c, the deferred DF_LIGHT and the "flashlight" argument of set_tablet to ui_actions.c, and the torch test
 * (full brightness, no dimming or sleep while it is open) to the sleep logic in ui_shell.c. */
/* ================================================================== flashlight */

static struct {
    lv_obj_t *field, *label;
    bool red;
} FL;

static void fl_paint(void)
{
    lv_obj_set_style_bg_color(FL.field, FL.red ? lv_color_hex(0xB0000A) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(FL.label, FL.red ? lv_color_hex(0x40000A) : lv_color_hex(0x9AA1A8), 0);
}

static void fl_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    FL.red = !FL.red; /* red keeps night vision, and doesn't blind the next pit */
    fl_paint();
}

static void light_build(lv_obj_t *b)
{
    FL.field = lv_obj_create(b);
    lv_obj_remove_style_all(FL.field);
    lv_obj_set_size(FL.field, W, H);
    lv_obj_set_style_bg_opa(FL.field, LV_OPA_COVER, 0);
    lv_obj_add_flag(FL.field, LV_OBJ_FLAG_CLICKABLE);
    FL.label = bz_label(FL.field, "tap for red · back to close", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_align(FL.label, LV_ALIGN_BOTTOM_MID, 0, -24);
    bz_on_tap(FL.field, fl_tap, NULL);
    fl_paint();
}

static void light_open(void) { hal_set_brightness(1.0f); }
static void light_close(void) { hal_set_brightness(S.brightness); }

const ui_app_t APP_LIGHT = { .name = "flashlight", .icon = BZ_I_FLASHLIGHT_ON, .build = light_build,
                             .open = light_open, .close = light_close };

/* The flashlight, on the control center: a panel at full brightness is what a pit needs at 9 pm, and it
 * is two taps away otherwise. The registry is how it gets there — the control center knows nothing about
 * this app, and nothing here knows how the sheet is laid out. */
static bool torch_on(void) { return ui_app_is_open(&APP_LIGHT); }
static void torch_open(void) { ui_app_open(&APP_LIGHT, NULL); }
static void torch_tap(void)
{
    if (ui_app_is_open(&APP_LIGHT)) ui_app_close();
    else ui_cc_after_close(torch_open);
}

void ui_light_cc_add(void)
{
    static const ui_cc_control_t CTL = {
        .name = "torch", .icon = BZ_I_FLASHLIGHT_ON, .kind = UI_CC_TOGGLE, .cols = 1, .rows = 1,
        .on = torch_on, .tap = torch_tap,
    };
    ui_cc_add(&CTL);
}
