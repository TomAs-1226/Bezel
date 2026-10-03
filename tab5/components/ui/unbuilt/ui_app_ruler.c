/* NOT COMPILED: the ruler app, taken out of the build (components/ui/CMakeLists.txt) because a pit rule does the job
 * and its window and icon cost PSRAM for no reason. To bring it back: move this into ui_apps_shop.c (it needs that
 * file's pick(), bz_*, ui_* helpers), add &APP_RULER to the list in ui_pages2.c and "ruler" to ui_icon.c. */
/* ================================================================== ruler */

/* The panel's active area is 110.4 mm across its 1280 pixels (the 5" module's datasheet); a nudge
 * against a real rule corrects the rest, and is kept. */
#define RULER_PX_MM (1280 / 110.4f)
#define RULER_H 150

static struct {
    lv_obj_t *box, *read, *scale_note, *unit_chips[2];
    float px_mm;
    bool inches;
    int mark; /* px from the left edge, -1 none */
} RU = { .mark = -1 };

static void ru_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int base = a.y2; /* the glass's bottom edge */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.width = 2;
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = bz_font(BZ_F_LABEL);
    td.color = bz_lv(BZ_C_DIM);
    /* ticks: mm (1, 5, 10) or inches (1/16, 1/8, 1/4, 1/2, 1) */
    float step = RU.inches ? RU.px_mm * 25.4f / 16 : RU.px_mm;
    for (int i = 0;; i++) {
        float x = a.x1 + i * step;
        if (x > a.x2) break;
        int len;
        bool num = false;
        if (RU.inches) {
            len = i % 16 == 0 ? 70 : i % 8 == 0 ? 48 : i % 4 == 0 ? 34 : i % 2 == 0 ? 24 : 16;
            num = i % 16 == 0;
        } else {
            len = i % 10 == 0 ? 70 : i % 5 == 0 ? 44 : 24;
            num = i % 10 == 0;
        }
        ld.color = bz_lv(len >= 44 ? BZ_C_INK : BZ_C_DIM);
        ld.p1.x = x;
        ld.p1.y = base;
        ld.p2.x = x;
        ld.p2.y = base - len;
        lv_draw_line(layer, &ld);
        if (num && i > 0 && x + 16 <= a.x2) { /* a number the edge would cut in half is left off */
            char s[8];
            snprintf(s, sizeof s, "%d", RU.inches ? i / 16 : i / 10);
            td.text = s;
            td.text_local = 1;
            lv_area_t ta = { (int32_t)x - 30, base - 104, (int32_t)x + 30, base - 76 };
            td.align = LV_TEXT_ALIGN_CENTER;
            lv_draw_label(layer, &td, &ta);
        }
    }
    if (RU.mark >= 0) {
        ld.color = bz_lv(BZ_C_ICE);
        ld.width = 3;
        ld.p1.x = a.x1 + RU.mark;
        ld.p1.y = a.y1;
        ld.p2.x = a.x1 + RU.mark;
        ld.p2.y = base;
        lv_draw_line(layer, &ld);
    }
}

static void ru_show(void)
{
    pick(RU.unit_chips, 2, RU.inches);
    if (RU.mark >= 0) {
        float mm = RU.mark / RU.px_mm;
        ui_text(RU.read, "%.1f mm \xc2\xb7 %.3f in", mm, mm / 25.4f);
    } else {
        ui_text(RU.read, "tap the scale to measure from the left edge");
    }
    ui_text(RU.scale_note, "%.3f px per mm (%+.1f %% from the datasheet)", RU.px_mm, (RU.px_mm / RULER_PX_MM - 1) * 100);
    lv_obj_invalidate(RU.box);
}

static void ru_press(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    RU.mark = p.x - a.x1 < 0 ? 0 : p.x - a.x1;
    ru_show();
}

static void ru_unit(lv_obj_t *o, void *u) { (void)o; RU.inches = (intptr_t)u != 0; ru_show(); }

static void ru_nudge(lv_obj_t *o, void *u)
{
    (void)o;
    int d = (int)(intptr_t)u;
    RU.px_mm = d ? RU.px_mm * (1 + d * 0.002f) : RULER_PX_MM;
    char s[16];
    snprintf(s, sizeof s, "%.4f", RU.px_mm);
    hal_kv_set("ruler_pxmm", s);
    ru_show();
}

static void ruler_build(lv_obj_t *b)
{
    char s[16];
    RU.px_mm = hal_kv_get("ruler_pxmm", s, sizeof s) ? (float)atof(s) : RULER_PX_MM;
    if (RU.px_mm < RULER_PX_MM * 0.9f || RU.px_mm > RULER_PX_MM * 1.1f) RU.px_mm = RULER_PX_MM;

    lv_obj_t *t = bz_tile(b, W - 2 * PAD, H - APP_Y - RULER_H - 2 * BZ_GAP);
    lv_obj_set_pos(t, PAD, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0);
    RU.read = bz_label_line(t, "", BZ_F_NAME, BZ_C_INK, IN(W - 2 * PAD));
    lv_obj_t *r = bz_row(t, 10);
    RU.unit_chips[0] = ui_chip(r, "millimetres", ru_unit, (void *)0);
    RU.unit_chips[1] = ui_chip(r, "inches", ru_unit, (void *)1);
    bz_label(t, "calibrate: lay a real rule along the bottom edge and nudge until they agree", BZ_F_LABEL, BZ_C_DIM);
    r = bz_row(t, 10);
    ui_button(r, NULL, "shorter", ru_nudge, (void *)(intptr_t)1);
    ui_button(r, NULL, "longer", ru_nudge, (void *)(intptr_t)-1);
    ui_button(r, BZ_I_REFRESH, "datasheet", ru_nudge, (void *)0);
    RU.scale_note = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);

    /* the scale: the whole width, down to the glass's edge */
    RU.box = bz_box(b);
    lv_obj_set_size(RU.box, W, RULER_H);
    lv_obj_set_pos(RU.box, 0, H - RULER_H);
    lv_obj_add_flag(RU.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(RU.box, ru_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(RU.box, ru_press, LV_EVENT_PRESSING, NULL);
    ru_show();
}

const ui_app_t APP_RULER = { .name = "ruler", .icon = BZ_I_STRAIGHTEN, .build = ruler_build };
