/* NOT COMPILED: the storage app, taken out of the build (components/ui/CMakeLists.txt) because settings > storage and
 * the files app cover the card. To bring it back: move this into ui_apps_os.c (it uses that file's head_right() and
 * column_tile()), add &APP_STORAGE to the list in ui_pages2.c and "storage" to ui_icon.c. */
/* ================================================================== storage */

#define SG_ROWS 14

typedef struct {
    char name[40];
    cstore_usage_t u;
    bool os;             /* one of CATOS's own folders */
} sg_row_t;

static OS_BSS struct {
    sg_row_t *rows;      /* PSRAM */
    int n;
    uint64_t total, free_b, loose;
    int loose_files;
    bool have_space;
    volatile int state;  /* 0 idle, 1 counting, 2 counted, 3 no card */
    lv_obj_t *total_lbl, *free_lbl, *meter, *list, *note;
    int list_w;
} SG;

static void *sg_worker(void *u)
{
    (void)u;
    const char *sd = hal_sd_root();
    if (!sd) {
        SG.state = 3;
        return NULL;
    }
    cstore_layout();
    SG.have_space = hal_sd_space(&SG.total, &SG.free_b);
    int n = 0;
    char path[160];
    for (int i = 0; i < CS_NDIRS && n < SG_ROWS; i++) {
        sg_row_t *r = &SG.rows[n++];
        snprintf(r->name, sizeof r->name, "CATOS/%s", CS_DIR_NAME[i]);
        r->os = true;
        if (cstore_path((cs_dir_t)i, NULL, path, sizeof path)) cstore_usage(path, &r->u);
        else memset(&r->u, 0, sizeof r->u);
    }
    /* everything else at the card's top: its folders one by one, loose files together */
    SG.loose = 0;
    SG.loose_files = 0;
    cstore_entry_t *top = malloc(64 * sizeof *top);
    int k = top ? cstore_list(sd, NULL, false, top, 64) : -1;
    for (int i = 0; i < k; i++) {
        if (!top[i].dir) {
            SG.loose += top[i].size;
            SG.loose_files++;
            continue;
        }
        if (!strcasecmp(top[i].name, "CATOS") || n >= SG_ROWS) continue;
        sg_row_t *r = &SG.rows[n++];
        snprintf(r->name, sizeof r->name, "%s", top[i].name);
        r->os = false;
        snprintf(path, sizeof path, "%s/%s", sd, top[i].name);
        cstore_usage(path, &r->u);
    }
    free(top);
    SG.n = n;
    PUBLISH(SG.state, 2);
    return NULL;
}

static void sg_start(void)
{
    if (SG.state == 1) return;
    if (!SG.rows) SG.rows = lv_malloc_zeroed(SG_ROWS * sizeof *SG.rows);
    if (!SG.rows) return;
    SG.state = 1;
    ui_text(SG.note, "adding up the card…");
    if (!hal_thread("storage", sg_worker, NULL, 6144)) {
        SG.state = 0;
        ui_text(SG.note, "couldn't start");
    }
}

static void sg_rescan(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    sg_start();
}

static void sg_show(void)
{
    char a[24], b[24], c[24];
    lv_obj_clean(SG.list);
    if (SG.state == 3) {
        ui_text(SG.total_lbl, "no card");
        ui_text(SG.free_lbl, " ");
        bz_meter_set(SG.meter, 0, BZ_C_ICE);
        ui_text(SG.note, "Put a microSD card in the slot on the tablet's edge.");
        return;
    }
    uint64_t used = SG.have_space && SG.total > SG.free_b ? SG.total - SG.free_b : 0;
    if (SG.have_space) {
        ui_text(SG.total_lbl, "%s", cstore_size(used, a, sizeof a));
        ui_text(SG.free_lbl, "%s free of %s", cstore_size(SG.free_b, c, sizeof c), cstore_size(SG.total, b, sizeof b));
        float f = SG.total ? (float)((double)used / (double)SG.total) : 0;
        bz_meter_set(SG.meter, f, f > 0.9f ? BZ_C_WARN : BZ_C_ICE);
    } else {
        ui_text(SG.total_lbl, "—");
        ui_text(SG.free_lbl, "the card didn't say its size");
    }
    uint64_t biggest = 1;
    for (int i = 0; i < SG.n; i++) if (SG.rows[i].u.bytes > biggest) biggest = SG.rows[i].u.bytes;
    int lw = SG.list_w;
    for (int i = 0; i < SG.n; i++) {
        const sg_row_t *r = &SG.rows[i];
        lv_obj_t *t = bz_tile(SG.list, lw, 84);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(t, 12, 0);
        lv_obj_set_style_pad_hor(t, 20, 0);
        bz_icon(t, r->os ? BZ_I_FOLDER_OPEN : BZ_I_FOLDER, 24, r->os ? BZ_C_ICE : BZ_C_DIM);
        lv_obj_t *nm = bz_label_line(t, r->name, BZ_F_BODY, BZ_C_INK, 360);
        lv_obj_set_pos(nm, 40, 0);
        lv_obj_t *sz = bz_label(t, "", BZ_F_BODY, BZ_C_INK);
        ui_text(sz, "%s", cstore_size(r->u.bytes, a, sizeof a));
        lv_obj_align(sz, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *meta = bz_label_line(t, "", BZ_F_CAPTION, BZ_C_DIM, 300);
        ui_text(meta, "%d file%s%s", r->u.files, r->u.files == 1 ? "" : "s", r->u.truncated ? " · partial count" : "");
        lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 40, 0);
        lv_obj_t *bar = bz_meter(t, lw - 40 - 400, 8);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
        bz_meter_set(bar, (float)((double)r->u.bytes / (double)biggest), r->os ? BZ_C_ICE : BZ_C_DIM);
    }
    if (SG.loose_files) {
        lv_obj_t *l = bz_label(SG.list, "", BZ_F_CAPTION, BZ_C_DIM);
        ui_text(l, "%d loose file%s at the top of the card · %s", SG.loose_files, SG.loose_files == 1 ? "" : "s",
                cstore_size(SG.loose, a, sizeof a));
    }
    ui_text(SG.note, "CATOS holds Catalyst OS's documents, photos, audio, app data and logs. The other folders "
                     "belong to the robot tools: lens clips, recordings, logs.");
}

static void storage_build(lv_obj_t *b)
{
    lv_obj_t *hr = head_right(b);
    ui_button(hr, BZ_I_REFRESH, "recount", sg_rescan, NULL);
    int lw = 400, rw = W - 2 * PAD - lw - BZ_GAP;
    lv_obj_t *t = column_tile(b, PAD, lw);
    bz_icon(t, BZ_I_SD_CARD, 40, BZ_C_ICE);
    lv_obj_t *cap = bz_label(t, "microSD", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(cap, 56, 12);
    /* the value size, not display: "320.0 kb" at 76 px ran past the tile's 352 */
    SG.total_lbl = bz_label(t, "—", BZ_F_VALUE, BZ_C_INK);
    lv_obj_set_pos(SG.total_lbl, 0, 84);
    SG.free_lbl = bz_label_line(t, "", BZ_F_BODY, BZ_C_DIM, IN(lw));
    lv_obj_set_pos(SG.free_lbl, 0, 164);
    SG.meter = bz_meter(t, IN(lw), 14);
    lv_obj_set_pos(SG.meter, 0, 206);
    SG.note = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_label_set_long_mode(SG.note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(SG.note, IN(lw));
    lv_obj_set_pos(SG.note, 0, 250);
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD + lw + BZ_GAP, APP_Y);
    SG.list_w = rw;
    SG.list = ui_scroller(wrap, rw, APP_H);
}

static void storage_open(void) { sg_start(); }

static void storage_refresh(void)
{
    if (SG.state >= 2) {
        sg_show();
        SG.state = 0;
    }
}

const ui_app_t APP_STORAGE = { .name = "storage", .icon = BZ_I_SD_CARD, .build = storage_build, .open = storage_open,
                               .refresh = storage_refresh };
