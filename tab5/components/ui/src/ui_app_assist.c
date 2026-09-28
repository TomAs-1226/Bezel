/* The assistant and the PC: assist, link, and the orb that calls the assistant from anywhere.
 *
 * assist is a conversation with Claude about the robot on the cart (components/assist). The transcript
 * runs down the left: the technician's questions on ice, the answers on a tile, the assistant's
 * reasoning as a quiet line, every tool it uses as a row with its outcome. On the right, the core — rings
 * that turn with what it's doing, under a glass lens that bends them — says at a glance whether it's
 * thinking, reading, writing or waiting on you. Anything it wants to change arrives as a glass card:
 * nothing happens until the technician approves it.
 *
 * link is Catalyst Link, the PC beside the robot's code: finding and pairing it, how the assistant
 * reaches Claude, the work orders the PC's agent is working through and the patch branches proposed
 * from the tablet.
 *
 * The orb floats over every screen, bottom right: tap it to ask. It breathes while the assistant works
 * elsewhere, and turns amber when a card is waiting. */
#include "ui_internal.h"
#include "as_snap.h"
#include "assist.h"
#include "ccwatch.h"
#include "link.h"
#include "ui_companion.h"
#include "ui_md.h"
#include "ui_sys.h"
#include "voice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define APP_Y 104
#define APP_H (H - APP_Y - PAD)
#define TW 800                          /* transcript width */
#define SIDE_X (PAD + TW + BZ_GAP)
#define SIDE_W (W - PAD - SIDE_X)
#define BAR_H 76                        /* the prompt bar under the transcript */
#define TLIST_H (APP_H - BAR_H - BZ_GAP)
#define VIEW_MAX 60                     /* transcript rows kept as objects; older ones leave */
#define CORE_H 330
#define MEM_BTN 64                      /* the memory button in the prompt bar: an icon */
#define CHIPS_W (TW - 20 - 2 * 128 - 16 - MEM_BTN - 8) /* the suggestions' share of the prompt bar */
#define LENS 150

/* ================================================================== text */

/* Markdown with its marks taken off, for the places that are still one label: what the owner typed,
 * and an answer while it is still arriving. A finished answer goes through ui_md_render instead, which
 * draws the marks rather than deleting them — see view_fill. */
static void plain(char *out, size_t n, const char *in)
{
    size_t o = 0;
    bool line_start = true;
    for (const char *p = in; *p && o + 4 < n; p++) {
        if (p[0] == '*' && p[1] == '*') { p++; continue; }
        if (*p == '`') continue;
        if (line_start && (p[0] == '-' || p[0] == '*') && p[1] == ' ') {
            memcpy(out + o, "\xe2\x80\x94", 3); /* an em dash: the body face has no bullet or en dash */
            o += 3;
            line_start = false;
            continue;
        }
        if (line_start && p[0] == '#') {
            while (*p == '#') p++;
            if (*p == ' ') continue;
            p--;
            continue;
        }
        out[o++] = *p;
        line_start = *p == '\n';
    }
    out[o] = 0;
}

static const char *tool_icon(const char *t)
{
    if (!t) return BZ_I_BUILD;
    if (!strcmp(t, "remember") || !strcmp(t, "forget") || !strcmp(t, "recall") || strstr(t, "notebook") ||
        strstr(t, "persona"))
        return BZ_I_PSYCHOLOGY;
    if (!strcmp(t, "triage")) return BZ_I_HEALTH_AND_SAFETY;
    if (!strcmp(t, "swerve_check")) return BZ_I_EXPLORE;
    if (!strcmp(t, "timer")) return BZ_I_TIMER;
    if (!strcmp(t, "alarm") || !strcmp(t, "reminder")) return BZ_I_SCHEDULE;
    if (!strcmp(t, "notes")) return BZ_I_EDIT_NOTE;
    if (!strcmp(t, "documents")) return BZ_I_DESCRIPTION;
    if (!strcmp(t, "calendar")) return BZ_I_GRID_VIEW;
    if (!strcmp(t, "weather")) return BZ_I_LIGHT_MODE;
    if (!strcmp(t, "notifications")) return BZ_I_NOTIFICATIONS;
    if (!strcmp(t, "checklist")) return BZ_I_CHECKLIST_RTL;
    if (!strcmp(t, "tablet_status") || !strcmp(t, "set_tablet")) return BZ_I_DISPLAY_SETTINGS;
    if (!strcmp(t, "open_app")) return BZ_I_APPS;
    if (!strcmp(t, "music")) return BZ_I_GRAPHIC_EQ;
    if (!strcmp(t, "smart_home")) return BZ_I_LIGHTBULB;
    if (strstr(t, "patch")) return BZ_I_CALL_SPLIT;
    if (strstr(t, "work_order")) return BZ_I_OUTBOX;
    if (strstr(t, "snapshot")) return BZ_I_UNDO;
    if (strstr(t, "tunable")) return BZ_I_TUNE;
    if (strstr(t, "auto")) return BZ_I_FLAG;
    if (strstr(t, "code")) return BZ_I_CODE;
    if (strstr(t, "preflight")) return BZ_I_CHECKLIST;
    if (strstr(t, "can")) return BZ_I_CABLE;
    if (strstr(t, "power")) return BZ_I_BOLT;
    if (strstr(t, "systemcore")) return BZ_I_DEVELOPER_BOARD;
    if (strstr(t, "motor")) return BZ_I_HISTORY;
    if (strstr(t, "log")) return BZ_I_RECEIPT_LONG;
    if (strstr(t, "alert")) return BZ_I_WARNING;
    if (strstr(t, "vision")) return BZ_I_VISIBILITY;
    if (strstr(t, "mechanism")) return BZ_I_PRECISION_MANUFACTURING;
    if (strstr(t, "topic")) return BZ_I_HUB;
    return BZ_I_SMART_TOY;
}

static bz_status_t tool_status(as_tool_state_t s)
{
    switch (s) {
    case AS_TOOL_OK: return BZ_OK;
    case AS_TOOL_FAILED: return BZ_FAULT;
    case AS_TOOL_DENIED: return BZ_WARN;
    case AS_TOOL_WAITING: return BZ_WARN;
    default: return BZ_STALE;
    }
}

static const char *phase_word(as_phase_t p)
{
    switch (p) {
    case AS_PHASE_SENDING: return "listening";
    case AS_PHASE_THINKING: return "thinking";
    case AS_PHASE_WRITING: return "answering";
    case AS_PHASE_TOOL: return "working";
    case AS_PHASE_CONFIRM: return "needs your ok";
    case AS_PHASE_ERROR: return "stopped";
    default: return "ready";
    }
}

/* ================================================================== assist */

typedef struct {
    int id;
    uint32_t rev;
    as_entry_kind_t kind;
    lv_obj_t *row, *text, *mark;
    lv_obj_t *rich;      /* the rendered answer, once it has stopped arriving */
    uint32_t rich_rev;   /* what was rendered, so a settled answer is drawn once and not per frame */
} view_t;

static struct {
    lv_obj_t *body, *list, *empty;
    view_t v[VIEW_MAX];
    int nv;
    uint32_t rev;
    /* the core */
    lv_obj_t *core, *lens, *phase, *detail, *meta, *route;
    bz_glass_t *lens_glass;
    float spin[3], energy, hue_amt;
    bz_motion_t glow;
    as_phase_t last_phase;
    double core_at;   /* when the core was last drawn: it is capped, not drawn every frame */
    bool core_dirty;  /* the rings have turned, or the phase changed, since that drawing */
    /* the prompt bar */
    lv_obj_t *chips, *send, *send_icon, *send_label;
    const char *sugg[5];
    int nsugg;
    ui_kb_t *kb;
    /* the card */
    lv_obj_t *card, *card_icon, *card_title, *card_detail, *card_reason, *card_time, *card_yes, *card_no;
    bz_glass_t *card_glass;
    bool card_shown;
    double card_deadline;
    char card_kind[24];
    lv_obj_t *setup;
    double last_sync;
    uint32_t drev;         /* the transcript as the detail line last read it */
    bool net_lost;         /* a request under way and no network: the detail line says so */
} AS;

static void view_clear(void)
{
    for (int i = 0; i < AS.nv; i++) lv_obj_delete(AS.v[i].row);
    AS.nv = 0;
}

static void view_drop_front(void)
{
    lv_obj_delete(AS.v[0].row);
    memmove(&AS.v[0], &AS.v[1], sizeof(view_t) * (size_t)(AS.nv - 1));
    AS.nv--;
}

static lv_obj_t *row_box(void)
{
    lv_obj_t *r = bz_box(AS.list);
    lv_obj_set_size(r, TW, LV_SIZE_CONTENT);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

static void view_make(view_t *v, const as_entry_t *e)
{
    v->kind = e->kind;
    v->mark = NULL;
    v->rich = NULL;      /* the row is new: whatever was rendered into the old one went with it */
    v->rich_rev = 0;
    switch (e->kind) {
    case AS_E_USER: {
        /* the question: on ice, on the right */
        v->row = row_box();
        lv_obj_t *b = bz_tile(v->row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        bz_tile_set_fill(b, BZ_C_ICE);
        lv_obj_set_style_max_width(b, 560, 0);
        lv_obj_set_style_pad_hor(b, 22, 0);
        lv_obj_set_style_pad_ver(b, 14, 0);
        lv_obj_set_style_radius(b, 26, 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
        v->text = bz_label(b, "", BZ_F_BODY, BZ_C_ON_ICE);
        lv_obj_set_style_max_width(v->text, 560 - 44, 0);
        lv_label_set_long_mode(v->text, LV_LABEL_LONG_WRAP);
        break;
    }
    case AS_E_TEXT: {
        /* the answer: a tile the transcript's width */
        v->row = bz_tile(AS.list, TW, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(v->row, 18, 0);
        v->text = bz_label(v->row, "", BZ_F_BODY, BZ_C_INK);
        lv_obj_set_width(v->text, TW - 2 * BZ_PAD_TILE);
        lv_label_set_long_mode(v->text, LV_LABEL_LONG_WRAP);
        break;
    }
    case AS_E_THINKING: {
        /* its reasoning, summarized: a quiet line with the neurology glyph */
        v->row = row_box();
        lv_obj_set_style_pad_hor(v->row, 8, 0);
        lv_obj_t *ic = bz_icon(v->row, BZ_I_NEUROLOGY, 24, BZ_C_FAINT);
        lv_obj_set_pos(ic, 0, 0);
        v->text = bz_label(v->row, "", BZ_F_LABEL, BZ_C_DIM);
        lv_obj_set_pos(v->text, 36, 2);
        lv_obj_set_width(v->text, TW - 52);
        lv_label_set_long_mode(v->text, LV_LABEL_LONG_WRAP);
        break;
    }
    case AS_E_TOOL: {
        /* a tool: its glyph, its name, what it found, how it went */
        v->row = bz_tile(AS.list, TW, 60);
        bz_tile_set_fill(v->row, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(v->row, 0, 0);
        lv_obj_set_style_pad_hor(v->row, 18, 0);
        lv_obj_set_style_radius(v->row, 20, 0);
        lv_obj_t *ic = bz_icon(v->row, tool_icon(e->tool), 24, BZ_C_DIM);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
        char name[40];
        snprintf(name, sizeof name, "%s", e->tool ? e->tool : "tool");
        for (char *p = name; *p; p++) if (*p == '_') *p = ' ';
        lv_obj_t *nm = bz_label_line(v->row, name, BZ_F_LABEL, BZ_C_DIM, 170);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 38, 0);
        v->text = bz_label_line(v->row, "", BZ_F_BODY_S, BZ_C_INK, TW - 36 - 38 - 180 - 40);
        lv_obj_align(v->text, LV_ALIGN_LEFT_MID, 38 + 180, 0);
        v->mark = bz_mark(v->row, tool_status(e->tool_state), 12);
        lv_obj_align(v->mark, LV_ALIGN_RIGHT_MID, 0, 0);
        break;
    }
    case AS_E_NOTE:
    case AS_E_ERROR: {
        v->row = row_box();
        lv_obj_set_style_pad_hor(v->row, 8, 0);
        bool err = e->kind == AS_E_ERROR;
        lv_obj_t *ic = bz_icon(v->row, err ? BZ_I_ERROR : BZ_I_INFO, 24, err ? BZ_C_FAULT : BZ_C_FAINT);
        lv_obj_set_pos(ic, 0, 0);
        v->text = bz_label(v->row, "", BZ_F_CAPTION, err ? BZ_C_FAULT : BZ_C_DIM);
        lv_obj_set_pos(v->text, 36, 4);
        lv_obj_set_width(v->text, TW - 52);
        lv_label_set_long_mode(v->text, LV_LABEL_LONG_WRAP);
        break;
    }
    }
    v->rev = e->rev - 1; /* forces the first fill */
}

/* An answer is rendered properly — headings, bold, code, charts, drawings — once it has stopped
 * arriving. While it streams it stays one plain label: re-laying out a column of objects on every
 * delta would cost more than the answer, and a fence half-written is not a chart yet. */
static void view_rich(view_t *v, const as_entry_t *e)
{
    const char *t = e->text ? e->text : "";
    if (v->rich_rev == e->rev) return;
    v->rich_rev = e->rev;
    if (!v->rich) {
        v->rich = bz_box(v->row);
        lv_obj_set_pos(v->rich, 0, 0);
    }
    char *f = strdup(t); /* the model's curly quotes folded: the faces don't carry them */
    if (f) assist_fold_text(f);
    ui_md_render(v->rich, f ? f : t, TW - 2 * BZ_PAD_TILE);
    free(f);
    if (v->text) lv_obj_add_flag(v->text, LV_OBJ_FLAG_HIDDEN);
}

/* `ph` is the phase read by the caller under the same lock this runs inside: an answer is only settled
 * once the conversation has stopped writing. It may have nothing to do — the caller hands it every row,
 * not only the ones whose text changed, because a settled answer waiting for the renderer looks exactly
 * like a row that has not moved. */
static void view_fill(view_t *v, const as_entry_t *e, as_phase_t ph)
{
    bool fresh = v->rev != e->rev;
    if (!fresh && (v->kind != AS_E_TEXT || v->rich_rev == e->rev)) return;
    v->rev = e->rev;
    const char *t = e->text ? e->text : "";
    if (v->kind == AS_E_TEXT && ph != AS_PHASE_WRITING) {
        /* it has stopped arriving: draw it as what it says it is. rich_rev records the verdict either
         * way, so a plain answer is read for marks once and not again on every later bump. */
        if (ui_md_is_rich(t)) {
            view_rich(v, e);
            return;
        }
        v->rich_rev = e->rev;
    }
    if (!fresh) return;
    if (v->kind == AS_E_TEXT && v->rich) {
        /* rendered once and grown since, still arriving: the column the reader is looking at stays, and
         * the settle above redraws it with what was added. A half-written table is not a table yet. */
        return;
    }
    if (v->kind == AS_E_TEXT || v->kind == AS_E_USER) {
        size_t n = strlen(t) + 8;
        char *buf = malloc(n);
        if (!buf) return;
        plain(buf, n, t);
        assist_fold_text(buf);
        lv_label_set_text(v->text, buf);
        free(buf);
    } else if (v->kind == AS_E_THINKING) {
        /* the newest few lines of a long summary */
        size_t n = strlen(t);
        const char *s = n > 360 ? t + n - 360 : t;
        if (s != t) {
            const char *sp = strchr(s, ' ');
            if (sp) s = sp + 1;
        }
        char line[400];
        snprintf(line, sizeof line, "%s%s", s != t ? "\xe2\x80\xa6" : "", s);
        assist_fold_text(line);
        ui_text(v->text, "%s", line);
    } else {
        char line[256];
        snprintf(line, sizeof line, "%s", t);
        assist_fold_text(line);
        ui_text(v->text, "%s", line);
    }
    if (v->mark) bz_mark_set(v->mark, tool_status(e->tool_state));
}

/* Brings the transcript's objects up to the conversation: new entries get rows, growing ones their
 * new text, and the oldest leave once there are more than the view keeps. */
static void sync_transcript(void)
{
    uint32_t rev = assist_rev();
    if (rev == AS.rev) return;
    AS.rev = rev;
    bool grew = false;
    assist_lock();
    as_phase_t ph = assist_phase_locked();
    int first = assist_first(), n = assist_count();
    int start = first + n - VIEW_MAX;
    if (start < first) start = first;
    if (AS.nv && AS.v[0].id < first) view_clear(); /* a new conversation, or rows gone past the core's cap */
    while (AS.nv && AS.v[0].id < start) view_drop_front();
    int next = AS.nv ? AS.v[AS.nv - 1].id + 1 : start;
    for (int i = 0; i < AS.nv; i++) {
        const as_entry_t *e = assist_entry(AS.v[i].id - first);
        if (!e) continue;
        /* Every row, not only the grown ones: an answer that stopped arriving is still one plain label
         * and this is where it becomes what it says it is. That is not growth — the reader's place in a
         * finished answer is theirs, so promoting it must not pull the list to the bottom. */
        bool changed = e->rev != AS.v[i].rev;
        view_fill(&AS.v[i], e, ph);
        if (changed) grew = true;
    }
    if (next < first + n) {
        /* new rows are made at the top of the list and laid out at the bottom; LVGL would redraw both
         * places, the whole list: they're made quietly, and only where they land is drawn */
        lv_display_t *d = lv_obj_get_display(AS.list);
        lv_obj_update_layout(AS.list); /* what's already changed redraws as usual */
        int made = AS.nv;
        lv_display_enable_invalidation(d, false);
        for (int id = next; id < first + n; id++) {
            if (AS.nv == VIEW_MAX) {
                view_drop_front();
                made = 0; /* everything moved up */
            }
            view_t *v = &AS.v[AS.nv++];
            const as_entry_t *e = assist_entry(id - first);
            v->id = id;
            view_make(v, e);
            view_fill(v, e, ph);
        }
        lv_obj_update_layout(AS.list);
        lv_display_enable_invalidation(d, true);
        if (made == 0) lv_obj_invalidate(lv_obj_get_parent(AS.list));
        else for (int i = made; i < AS.nv; i++) lv_obj_invalidate(AS.v[i].row);
        grew = true;
    }
    assist_unlock();
    if (AS.empty) {
        if (AS.nv) lv_obj_add_flag(AS.empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(AS.empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (grew) ui_scroller_follow(AS.list);
}

/* ---- the core: three rings and a crown of ticks, turning with what it's doing, under a lens ---- */

static bz_color_role_t phase_color(as_phase_t p)
{
    switch (p) {
    case AS_PHASE_THINKING: return BZ_C_ICE;
    case AS_PHASE_TOOL: return BZ_C_AMBER;
    case AS_PHASE_WRITING: return BZ_C_LEAF;
    case AS_PHASE_CONFIRM: return BZ_C_AMBER;
    case AS_PHASE_ERROR: return BZ_C_FAULT;
    case AS_PHASE_SENDING: return BZ_C_ICE;
    default: return BZ_C_DIM;
    }
}

static void core_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    as_phase_t ph = assist_phase();
    bz_color_role_t col = phase_color(ph);
    float en = AS.energy;
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = (int32_t)cx;
    ad.center.y = (int32_t)cy;
    ad.rounded = 1;
    /* the crown: 72 ticks, a wave of light running round while it works */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.round_start = ld.round_end = 1;
    ld.width = 3;
    float r_out = 150;
    for (int i = 0; i < 72; i++) {
        float an = i * (6.2831853f / 72);
        float wave = 0.5f + 0.5f * cosf(an * 3 - AS.spin[0] * 2.3f);
        bool lit = en > 0.05f && wave > 0.82f;
        float len = lit ? 12 + 8 * en : 7;
        ld.color = bz_lv(lit ? col : BZ_C_FAINT);
        ld.p1.x = cx + cosf(an) * (r_out - len); ld.p1.y = cy + sinf(an) * (r_out - len);
        ld.p2.x = cx + cosf(an) * r_out; ld.p2.y = cy + sinf(an) * r_out;
        lv_draw_line(layer, &ld);
    }
    /* three rings with gaps, each turning at its own speed */
    static const float R[3] = { 122, 100, 80 }, WID[3] = { 6, 10, 4 }, SPAN[3] = { 250, 120, 300 };
    for (int k = 0; k < 3; k++) {
        float start = fmodf(AS.spin[k] * 57.29578f, 360.0f);
        if (start < 0) start += 360;
        ad.radius = (uint16_t)R[k];
        ad.width = (uint16_t)WID[k];
        ad.color = bz_lv(BZ_C_FAINT);
        ad.start_angle = 0;
        ad.end_angle = 360;
        ad.opa = LV_OPA_40;
        lv_draw_arc(layer, &ad);
        ad.opa = (lv_opa_t)(120 + 135 * (en > 1 ? 1 : en));
        ad.color = bz_lv(en > 0.05f ? col : BZ_C_DIM);
        ad.start_angle = (int32_t)start;
        ad.end_angle = (int32_t)(start + SPAN[k] * (0.35f + 0.65f * (en > 1 ? 1 : en)));
        lv_draw_arc(layer, &ad);
    }
    /* the heart, under the lens: brighter as it works */
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.radius = (uint16_t)(26 + 10 * en);
    ad.width = ad.radius;
    ad.opa = (lv_opa_t)(90 + 150 * (en > 1 ? 1 : en));
    ad.color = bz_lv(en > 0.05f ? col : BZ_C_FAINT);
    lv_draw_arc(layer, &ad);
}

/* ---- the card ---- */

static void card_answer(lv_obj_t *o, void *u)
{
    (void)o;
    bool yes = (intptr_t)u != 0;
    assist_confirm(yes);
    hal_tone(yes ? 1500 : 700, 30, S.volume * 0.5f);
    ui_island_say(yes ? BZ_I_TASK_ALT : BZ_I_CANCEL, yes ? "approved" : "declined");
}

static void card_show(bool show)
{
    if (show == AS.card_shown) return;
    AS.card_shown = show;
    bz_glass_show(AS.card_glass, show);
    if (show) {
        lv_obj_remove_flag(AS.card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(AS.card);
        hal_tone(1100, 40, S.volume * 0.5f);
    }
}

static const char *kind_icon(const char *k)
{
    if (!strcmp(k, "set_tunable")) return BZ_I_TUNE;
    if (!strcmp(k, "select_auto")) return BZ_I_FLAG;
    if (!strcmp(k, "revert")) return BZ_I_UNDO;
    if (!strcmp(k, "propose_patch")) return BZ_I_CALL_SPLIT;
    return BZ_I_WARNING;
}

static void card_frame(double now)
{
    as_confirm_t c;
    bool pending = assist_confirm_pending(&c) && c.pending && ui_app_is_open(&APP_ASSIST);
    if (pending && (!AS.card_shown || strcmp(c.kind, AS.card_kind) != 0 || c.deadline != AS.card_deadline)) {
        snprintf(AS.card_kind, sizeof AS.card_kind, "%s", c.kind);
        AS.card_deadline = c.deadline;
        lv_label_set_text(AS.card_icon, kind_icon(c.kind));
        ui_text(AS.card_title, "%s", c.title);
        ui_text(AS.card_detail, "%s", c.detail);
        /* the detail usually carries the reason already ("why: …"); only say it again if it doesn't */
        ui_text(AS.card_reason, "%s", c.reason[0] && !strstr(c.detail, c.reason) ? c.reason : "");
    }
    card_show(pending);
    if (AS.card_shown) {
        int left = (int)ceil(AS.card_deadline - hal_seconds());
        ui_text(AS.card_time, "declines itself in %d s", left < 0 ? 0 : left);
    }
    (void)now;
}

/* Runs every frame whatever is open (from the orb's hook): the card and the lens live on the glass
 * layer, outside the app's window, so they must leave with it. */
static void assist_glass_housekeeping(void)
{
    if (!AS.card) return;
    if (!ui_app_is_open(&APP_ASSIST)) card_show(false);
    if (!AS.card_shown && bz_glass_strength(AS.card_glass) < 0.02f && !lv_obj_has_flag(AS.card, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(AS.card, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the prompt bar ---- */

static void ask(const char *q)
{
    char why[96];
    if (!assist_ready(why, sizeof why)) {
        ui_island_say(BZ_I_LINK_OFF, why);
        return;
    }
    if (assist_send(q)) hal_tone(1300, 12, S.volume * 0.4f);
    else ui_island_say(BZ_I_HOURGLASS_TOP, "still working on the last one");
}

static void chip_tap(lv_obj_t *o, void *u)
{
    (void)o;
    int i = (int)(intptr_t)u;
    if (i < AS.nsugg) ask(AS.sugg[i]);
}

static void typed(const char *text, void *u)
{
    (void)u;
    while (*text == ' ' || *text == '\n') text++;
    if (*text) ask(text);
}

static void send_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    as_phase_t p = assist_phase();
    if (p != AS_PHASE_IDLE && p != AS_PHASE_ERROR) {
        assist_stop();
        ui_island_say(BZ_I_STOP_CIRCLE, "stopped");
        return;
    }
    ui_kb_show(AS.kb, "ask about the robot", "", false, false, typed, NULL);
}

static void new_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    assist_reset();
    view_clear();
    AS.rev = 0;
    ui_island_say(BZ_I_REFRESH, "new conversation");
}

static void setup_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_LINK, o);
}

static void memory_tap(lv_obj_t *o, void *u)
{
    (void)u;
    ui_memory_open(AS_POOL_ASSIST, o);
}

static void refresh_chips(void)
{
    const char *s[5];
    int n = assist_suggestions(s, 2); /* two fit beside ask and new */
    bool same = n == AS.nsugg;
    for (int i = 0; i < n && same; i++) same = !strcmp(s[i], AS.sugg[i]);
    if (same) return;
    static char keep[5][80];
    lv_obj_clean(AS.chips);
    AS.nsugg = n;
    for (int i = 0; i < n; i++) {
        snprintf(keep[i], sizeof keep[i], "%s", s[i]);
        AS.sugg[i] = keep[i];
        lv_obj_t *c = ui_chip(AS.chips, keep[i], chip_tap, (void *)(intptr_t)i);
        /* each its share of the row, and a long question ends in "…" instead of scrolling off the left */
        int share = (CHIPS_W - 8 * (n - 1)) / n; /* the row's width, as the build sets it */
        lv_obj_set_style_max_width(c, share, 0);
        lv_obj_t *l = lv_obj_get_child(c, -1);
        if (l && lv_obj_check_type(l, &lv_label_class)) {
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_max_width(l, share - 48, 0);
        }
    }
}

static void assist_refresh(void)
{
    char why[96];
    bool ready = assist_ready(why, sizeof why);
    if (ready) lv_obj_add_flag(AS.setup, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(AS.setup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *l = lv_obj_get_child(AS.setup, 1);
        ui_text(l, "%s", why);
    }
    refresh_chips();
    /* a request under way with no network under it: say so plainly, since it can take its timeout to give up */
    as_phase_t ph = assist_phase();
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR && ph != AS_PHASE_CONFIRM;
    hal_net_t net;
    hal_net(&net);
    hal_tether_t te;
    hal_tether(&te);
    bool lost = busy && !net.up && !te.up;
    if (lost) ui_text(AS.detail, "the network dropped: tap stop, and ask again once it's back");
    else if (AS.net_lost) AS.drev = AS.rev - 1; /* the detail line goes back to what it's doing */
    AS.net_lost = lost;
    as_usage_t u;
    assist_usage(&u);
    link_status_t ls;
    link_status(&ls);
    assist_config_t *cfg = malloc(sizeof *cfg); /* it carries the keys: off the stack, wiped after */
    as_route_t route = AS_ROUTE_LINK;
    if (cfg) {
        assist_config(cfg);
        route = cfg->route;
        memset(cfg, 0, sizeof *cfg);
        free(cfg);
    }
    ui_text(AS.meta, "%s\n%s%s%d in · %d out tokens%s", assist_provider_name(route),
            u.model[0] ? u.model : route == AS_ROUTE_OPENAI ? "" : "claude-opus-5", u.model[0] || route != AS_ROUTE_OPENAI ? "\n" : "",
            u.input_tokens, u.output_tokens, u.fell_back ? " · fell back" : "");
    if (ls.configured)
        ui_text(AS.route, "pc %s · %s%s", ls.name[0] ? ls.name : "link", ls.reachable ? (ls.auth ? "paired" : "wrong token") : "away",
                ls.outbox ? " · outbox waiting" : "");
    else ui_text(AS.route, "no pc paired");
}

static void assist_frame(double now, double dt)
{
    sync_transcript();
    card_frame(now);
    as_phase_t ph = assist_phase();
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    /* The energy is the core's whole state: its colour, how far round the arcs reach, how big the heart
     * is. It rides Bezel's effects spring, which never bounces, and it keeps that spring through the lean
     * renderer's instant motion — without `keep` the core simply stepped between dim and lit, which is
     * most of what made it look wrong. */
    float want = busy ? 1.0f : 0.0f;
    if (AS.glow.target != want) bz_motion_to(&AS.glow, want, BZ_EFFECT);
    bool m = bz_motion_tick(&AS.glow);
    AS.energy = AS.glow.value < 0 ? 0 : AS.glow.value;
    /* The rings turn while it works. This was gated on bz_ui_calm(), which on the tablet is held true by
     * the renderer (no glass pass, solid surfaces) and not because stillness was asked for: the core
     * never turned there at all, and was redrawn every frame anyway for a picture that never changed. */
    float speed = 0.25f + 2.2f * AS.energy;
    if (AS.energy > 0.005f || m) {
        AS.spin[0] += (float)dt * speed;
        AS.spin[1] -= (float)dt * speed * 1.6f;
        AS.spin[2] += (float)dt * speed * 0.7f;
        AS.core_dirty = true;
        bz_ui_keep_alive();
    }
    /* 358 x 330 of arcs and seventy-two ticks is most of a frame's drawing, for a turn of a few degrees.
     * Drawn at 24 Hz, which is as fast as rings read as turning, and whatever the frame rate is doing. */
    if (AS.core_dirty) {
        bz_ui_keep_alive();
        if (now - AS.core_at >= 1.0 / 24) {
            AS.core_dirty = false;
            AS.core_at = now;
            lv_obj_invalidate(AS.core);
        }
    }
    if (ph != AS.last_phase) {
        AS.last_phase = ph;
        AS.core_dirty = true;
        ui_text(AS.phase, "%s", phase_word(ph));
        bz_icon_set(AS.send_icon, busy ? BZ_I_STOP_CIRCLE : BZ_I_KEYBOARD, 24, false);
        ui_text(AS.send_label, "%s", busy ? "stop" : "ask");
    }
    /* what it's doing right now: the last tool's line, or its last words (the network's loss says itself) */
    if (AS.drev != AS.rev && !AS.net_lost) {
        AS.drev = AS.rev;
        char line[120] = "";
        assist_lock();
        int n = assist_count();
        for (int i = n - 1; i >= 0 && i >= n - 3; i--) {
            const as_entry_t *e = assist_entry(i);
            if (e->kind == AS_E_TOOL) {
                snprintf(line, sizeof line, "%s", e->text ? e->text : "");
                break;
            }
        }
        assist_unlock();
        ui_text(AS.detail, "%s", line);
    }
}

static void assist_open(void)
{
    AS.rev = 0; /* catch up with anything that happened while it was closed */
    assist_refresh();
    bz_glass_show(AS.lens_glass, true);
}

static void assist_close(void)
{
    ui_kb_hide(AS.kb);
    bz_glass_show(AS.lens_glass, false);
    card_show(false);
}

static void assist_build(lv_obj_t *b)
{
    AS.body = b;
    lv_obj_t *head = bz_col(b, 2);
    lv_obj_align(head, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 26);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    bz_label(head, "ai technician", BZ_F_LABEL, BZ_C_DIM);
    bz_label(head, "Assist", BZ_F_TITLE, BZ_C_INK);

    /* the transcript */
    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, PAD, APP_Y);
    AS.list = ui_scroller(wrap, TW, TLIST_H);
    lv_obj_set_style_pad_row(AS.list, 12, 0);
    lv_obj_set_style_pad_bottom(AS.list, 12, 0);
    AS.empty = bz_label(wrap, "Ask about the robot on the cart: what's wrong, why it browned out, whether it's ready. "
                              "It reads everything the tablet can see, and changes nothing without your ok.",
                        BZ_F_BODY, BZ_C_DIM);
    lv_obj_set_width(AS.empty, TW - 120);
    lv_obj_set_pos(AS.empty, 12, 40);

    /* the prompt bar: suggestions, then ask / stop and new */
    lv_obj_t *bar = bz_tile(b, TW, BAR_H);
    lv_obj_set_pos(bar, PAD, APP_Y + TLIST_H + BZ_GAP);
    lv_obj_set_style_pad_all(bar, 10, 0);
    lv_obj_set_style_radius(bar, 38, 0);
    AS.chips = bz_row(bar, 8);
    lv_obj_set_width(AS.chips, CHIPS_W);
    lv_obj_set_height(AS.chips, BAR_H - 20);
    lv_obj_set_style_clip_corner(AS.chips, false, 0);
    lv_obj_align(AS.chips, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *nb = ui_button(bar, BZ_I_REFRESH, "new", new_tap, NULL);
    lv_obj_set_width(nb, 120);
    lv_obj_align(nb, LV_ALIGN_RIGHT_MID, -128 - 8, 0);
    /* what the technician remembers, and the way to take it back */
    lv_obj_t *mb = ui_button(bar, BZ_I_PSYCHOLOGY, NULL, memory_tap, NULL);
    lv_obj_set_style_pad_hor(mb, 0, 0);
    lv_obj_set_width(mb, MEM_BTN);
    lv_obj_align(mb, LV_ALIGN_RIGHT_MID, -128 - 8 - 120 - 8, 0);
    AS.send = ui_button(bar, BZ_I_KEYBOARD, "ask", send_tap, NULL);
    ui_chip_set(AS.send, true);
    lv_obj_set_width(AS.send, 128);
    lv_obj_align(AS.send, LV_ALIGN_RIGHT_MID, 0, 0);
    AS.send_icon = lv_obj_get_child(AS.send, 0);
    AS.send_label = lv_obj_get_child(AS.send, 1);

    /* the side: the core under its lens, what it's doing, what it's using */
    lv_obj_t *side = bz_tile(b, SIDE_W, APP_H);
    lv_obj_set_pos(side, SIDE_X, APP_Y);
    AS.core = bz_box(side);
    lv_obj_set_size(AS.core, SIDE_W - 2 * BZ_PAD_TILE, CORE_H);
    lv_obj_set_pos(AS.core, 0, 0);
    lv_obj_add_event_cb(AS.core, core_draw, LV_EVENT_DRAW_MAIN, NULL);
    AS.phase = bz_label(side, "ready", BZ_F_NAME, BZ_C_INK);
    lv_obj_set_pos(AS.phase, 0, CORE_H + 6);
    /* up to two lines: a tool's outcome, or why nothing is coming */
    AS.detail = bz_label(side, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(AS.detail, SIDE_W - 2 * BZ_PAD_TILE);
    lv_label_set_long_mode(AS.detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(AS.detail, 2 * lv_font_get_line_height(bz_font(BZ_F_BODY_S)));
    lv_obj_set_pos(AS.detail, 0, CORE_H + 40);
    AS.meta = bz_label(side, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(AS.meta, LV_ALIGN_BOTTOM_LEFT, 0, -26);
    AS.route = bz_label_line(side, "", BZ_F_CAPTION, BZ_C_DIM, SIDE_W - 2 * BZ_PAD_TILE);
    lv_obj_align(AS.route, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* not set up yet: how to be */
    AS.setup = bz_tile(b, TW - 80, 150);
    bz_tile_set_fill(AS.setup, BZ_C_SURFACE2);
    lv_obj_set_pos(AS.setup, PAD + 40, APP_Y + 150);
    lv_obj_set_flex_flow(AS.setup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(AS.setup, 8, 0);
    bz_label(AS.setup, "the assistant can't answer yet", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *why = bz_label(AS.setup, "", BZ_F_BODY, BZ_C_INK);
    lv_obj_set_width(why, TW - 80 - 2 * BZ_PAD_TILE);
    ui_button(AS.setup, BZ_I_COMPUTER, "pair with the pc", setup_tap, NULL);
    lv_obj_add_flag(AS.setup, LV_OBJ_FLAG_HIDDEN);

    AS.kb = ui_kb_create(b, 440);

    /* the lens: glass over the core's heart, on the glass layer, so it bends the rings under it */
    lv_obj_t *g = bz_ui_glass();
    AS.lens = lv_obj_create(g);
    lv_obj_remove_style_all(AS.lens);
    lv_obj_set_size(AS.lens, LENS, LENS);
    lv_obj_set_pos(AS.lens, SIDE_X + SIDE_W / 2 - LENS / 2, APP_Y + BZ_PAD_TILE + CORE_H / 2 - LENS / 2);
    lv_obj_remove_flag(AS.lens, LV_OBJ_FLAG_CLICKABLE);
    AS.lens_glass = bz_glass_attach(AS.lens, 5, LENS / 2);
    bz_glass_set_solo(AS.lens_glass, true);
    bz_glass_show(AS.lens_glass, false);

    /* the card: glass over the transcript, its words on the glass */
    AS.card = lv_obj_create(g);
    lv_obj_remove_style_all(AS.card);
    lv_obj_set_size(AS.card, TW - 60, 380);
    lv_obj_set_pos(AS.card, PAD + 30, APP_Y + 60);
    lv_obj_add_flag(AS.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(AS.card, 28, 0);
    AS.card_glass = bz_glass_attach(AS.card, 5, 40);
    bz_glass_set_solo(AS.card_glass, true);
    bz_glass_show(AS.card_glass, false);
    AS.card_icon = bz_icon(AS.card, BZ_I_TUNE, 32, BZ_C_INK);
    lv_obj_set_pos(AS.card_icon, 0, 0);
    AS.card_title = bz_label(AS.card, "", BZ_F_NAME, BZ_C_INK);
    lv_obj_set_pos(AS.card_title, 46, 2);
    AS.card_time = bz_label(AS.card, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_align(AS.card_time, LV_ALIGN_TOP_RIGHT, 0, 10);
    AS.card_detail = bz_label(AS.card, "", BZ_F_LABEL, BZ_C_INK);
    lv_obj_set_width(AS.card_detail, TW - 60 - 56);
    lv_label_set_long_mode(AS.card_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(AS.card_detail, 170, 0);
    lv_obj_set_pos(AS.card_detail, 0, 52);
    AS.card_reason = bz_label(AS.card, "", BZ_F_BODY_S, BZ_C_DIM);
    lv_obj_set_width(AS.card_reason, TW - 60 - 56);
    lv_label_set_long_mode(AS.card_reason, LV_LABEL_LONG_DOT);
    lv_obj_set_height(AS.card_reason, 44);
    lv_obj_align(AS.card_reason, LV_ALIGN_BOTTOM_LEFT, 0, -76);
    AS.card_no = ui_button(AS.card, BZ_I_CLOSE, "decline", card_answer, (void *)0);
    lv_obj_set_width(AS.card_no, 200);
    lv_obj_align(AS.card_no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    AS.card_yes = ui_button(AS.card, BZ_I_CHECK_CIRCLE, "approve", card_answer, (void *)1);
    ui_chip_set(AS.card_yes, true);
    lv_obj_set_width(AS.card_yes, 240);
    lv_obj_align(AS.card_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(AS.card, LV_OBJ_FLAG_HIDDEN);

    bz_motion_init(&AS.glow, 0, 0.002f);
    AS.glow.keep = true; /* the core's one transition: it eases even where other motion is instant */
    AS.last_phase = (as_phase_t)-1;
}

const ui_app_t APP_ASSIST = { .name = "assist", .icon = BZ_I_AUTO_AWESOME, .build = assist_build, .open = assist_open,
                              .close = assist_close, .refresh = assist_refresh, .frame = assist_frame };

/* ================================================================== the orb */

static struct {
    lv_obj_t *obj, *icon;
    bz_glass_t *glass;
    bz_motion_t breath;
    as_phase_t last;
    bool asked;
} ORB;

static void orb_tap(lv_obj_t *o, void *u)
{
    (void)u;
    bz_ui_wake();
    /* Claude Code finished or wants something: the companion's panel says what; else as settings say */
    if (ccw_attention(NULL)) ui_companion_open(true, o);
    else if (ui_companion_cfg()->orb_companion) ui_companion_open(false, o);
    else ui_app_open(&APP_ASSIST, o);
}

static void orb_frame(double now, double dt, void *u)
{
    (void)u;
    ui_companion_tick(now, dt); /* Claude Code's reminders, chimes and desk mode, whatever is on screen */
    assist_glass_housekeeping();
    as_phase_t ph = assist_phase();
    bool busy = ph != AS_PHASE_IDLE && ph != AS_PHASE_ERROR;
    bool here = ui_app_is_open(&APP_ASSIST);
    /* over the pages only: an open app (this one included) takes the corner */
    bool show = !here && !ui_app_any_open() && !ui_overlay_up();
    bool hidden = lv_obj_has_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN);
    if (show == hidden) {
        if (show) lv_obj_remove_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN);
        bz_glass_show(ORB.glass, show);
    }
    if (!show && bz_glass_strength(ORB.glass) < 0.02f && !hidden) lv_obj_add_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN);
    /* breathing while it works away from its own screen; amber and steady while a card waits */
    if (ph == AS_PHASE_CONFIRM) {
        bz_glass_set_tint(ORB.glass, 1, 0.55f);
        if (ORB.last != ph && !here) ui_island_say(BZ_I_AUTO_AWESOME, "assist needs your ok");
    } else if (busy && show) {
        /* Breathing while it works away from its own screen. This was gated on bz_ui_calm(), which the
         * tablet holds true for the renderer's sake, so on the tablet the orb sat still and said nothing
         * while the assistant worked. The tint is quantized where it is applied, so the breath restyles
         * a few times a second, not every frame — and it only breathes while it is on screen, because a
         * breath asks for a frame and the hidden orb was asking for them all. */
        float k = 0.18f + 0.22f * (0.5f + 0.5f * sinf((float)now * 3.4f));
        bz_glass_set_tint(ORB.glass, 3, k);
        bz_ui_keep_alive();
    } else if (ph == AS_PHASE_ERROR) {
        bz_glass_set_tint(ORB.glass, 4, 0.3f);
    } else if (ccw_attention(NULL)) {
        bz_glass_set_tint(ORB.glass, 1, 0.35f); /* Claude Code on the PC wants a look */
    } else {
        bz_glass_set_tint(ORB.glass, 3, busy ? 0.3f : 0);
    }
    ORB.last = ph;
}

void ui_orb_show(bool show)
{
    if (!ORB.obj || show != lv_obj_has_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (show) lv_obj_remove_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ORB.obj, LV_OBJ_FLAG_HIDDEN);
}

void ui_orb_init(void)
{
    lv_obj_t *g = bz_ui_glass();
    ORB.obj = lv_obj_create(g);
    lv_obj_remove_style_all(ORB.obj);
    lv_obj_set_size(ORB.obj, 92, 92);
    /* Beside the dock in landscape, above it in portrait.
     *
     * The dock is a fixed 660 px centred in the frame. At 1280 that leaves 300 px of corner to its
     * right and the orb sits in it. At 720 the dock spans 30..690 and that corner IS the last dock
     * item, so the orb covers "apps" and the dock reads as incomplete rather than as obscured. A
     * phone floats this kind of button above the bar; so does this one now. */
    if (PORTRAIT) lv_obj_set_pos(ORB.obj, W - PAD - 92, H - DOCK_BOTTOM - DOCK_H - 12 - 92);
    else lv_obj_set_pos(ORB.obj, W - PAD - 92, H - DOCK_BOTTOM - 92);
    lv_obj_add_flag(ORB.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ORB.obj, LV_OBJ_FLAG_SCROLLABLE);
    ORB.glass = bz_glass_attach(ORB.obj, 7, 46);
    bz_glass_set_solo(ORB.glass, true);
    ORB.icon = bz_icon(ORB.obj, BZ_I_AUTO_AWESOME, 32, BZ_C_INK);
    lv_obj_center(ORB.icon);
    bz_on_tap(ORB.obj, orb_tap, NULL);
    ORB.last = AS_PHASE_IDLE;
    bz_ui_on_frame(orb_frame, NULL);
}

/* ================================================================== link */

static struct {
    lv_obj_t *state, *where, *pc_mark, *route_chips[3], *key_state, *inbox, *patches;
    ui_kb_t *kb;
    char url[96], token[64];
    int editing; /* 0 url, 1 token, 2 anthropic key, 3 openai key, 4 openai model */
    uint32_t sig;
} LK;

/* ---- keys and providers, shared by the link app and settings > assistant ---- */

static const char *const KEY_TITLE[5] = { "the pc's address, e.g. http://192.168.1.20:8765 (empty: find it)",
                                          "the token catalyst-link printed", "anthropic api key (sk-ant-...)",
                                          "openai api key (sk-...)", "openai model, e.g. gpt-5 (empty: gpt-5)" };

/* A key or model typed on either screen: stored, applied, and never shown again. */
static void key_typed(int what, const char *text)
{
    if (what == 2) {
        assist_set_anthropic(text, NULL);
        if (text[0]) assist_use(AS_ROUTE_DIRECT);
        ui_island_say(BZ_I_AUTO_AWESOME, text[0] ? "claude key saved on the tablet" : "claude key cleared");
    } else if (what == 3) {
        assist_set_openai(text, NULL);
        if (text[0]) assist_use(AS_ROUTE_OPENAI);
        ui_island_say(BZ_I_AUTO_AWESOME, text[0] ? "openai key saved on the tablet" : "openai key cleared");
    } else if (what == 4) {
        assist_set_openai(NULL, text);
        ui_island_say(BZ_I_AUTO_AWESOME, text[0] ? "openai model set" : "openai model: the default");
    }
}

static void keys_state(char *out, size_t n)
{
    assist_config_t *c = malloc(sizeof *c);
    if (!c) {
        if (n) out[0] = 0;
        return;
    }
    assist_config(c);
    snprintf(out, n, "claude key: %s · openai key: %s · openai model: %s", c->api_key[0] ? "saved" : "none",
             c->oai_key[0] ? "saved" : "none", c->oai_model[0] ? c->oai_model : "gpt-5 (default)");
    memset(c, 0, sizeof *c);
    free(c);
}

static as_route_t current_route(void)
{
    assist_config_t *c = malloc(sizeof *c);
    as_route_t r = AS_ROUTE_LINK;
    if (c) {
        assist_config(c);
        r = c->route;
        memset(c, 0, sizeof *c);
        free(c);
    }
    return r;
}

static void lk_save_and_apply(void)
{
    hal_kv_set("link_url", LK.url);
    hal_kv_set("link_token", LK.token);
    link_configure(LK.url, LK.token);
}

static void lk_typed(const char *text, void *u)
{
    (void)u;
    if (LK.editing == 0) snprintf(LK.url, sizeof LK.url, "%s", text);
    else if (LK.editing == 1) snprintf(LK.token, sizeof LK.token, "%s", text);
    if (LK.editing <= 1) {
        lk_save_and_apply();
        ui_island_say(BZ_I_COMPUTER, "pairing with the pc");
        return;
    }
    /* a key, for going straight to Claude or OpenAI without the PC */
    key_typed(LK.editing, text);
    for (int i = 0; i < 3; i++) ui_chip_set(LK.route_chips[i], (int)current_route() == i);
}

static void lk_edit(lv_obj_t *o, void *u)
{
    (void)o;
    LK.editing = (int)(intptr_t)u;
    ui_kb_show(LK.kb, KEY_TITLE[LK.editing], LK.editing == 0 ? LK.url : LK.editing == 1 ? LK.token : "",
               LK.editing > 0 && LK.editing < 4, true, lk_typed, NULL);
}

static void lk_find(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    LK.url[0] = 0;
    lk_save_and_apply(); /* an empty address: the poller browses mDNS for _catalyst-link */
    ui_island_say(BZ_I_RADAR, "looking for catalyst link");
}

static void lk_route(lv_obj_t *o, void *u)
{
    (void)o;
    as_route_t r = (as_route_t)(intptr_t)u;
    if (r != current_route()) ui_island_say(BZ_I_REFRESH, "new conversation on the new model");
    assist_use(r);
    for (int i = 0; i < 3; i++) ui_chip_set(LK.route_chips[i], (int)r == i);
}

static void lk_list(lv_obj_t *list, const link_item_t *it, int n, bool patches)
{
    lv_obj_clean(list);
    if (!n) {
        lv_obj_t *e = bz_label(list, patches ? "no patch branches yet" : "the inbox is empty", BZ_F_BODY_S, BZ_C_DIM);
        lv_obj_set_style_pad_left(e, 18, 0); /* on the heading's edge, as a row's text is */
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = bz_tile(list, lv_obj_get_width(list), LV_SIZE_CONTENT);
        bz_tile_set_fill(r, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(r, 14, 0);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_set_style_radius(r, 20, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 4, 0);
        lv_obj_t *top = bz_row(r, 10);
        bool done = !strcmp(it[i].status, "done") || !strcmp(it[i].status, "merged");
        bool bad = !strcmp(it[i].status, "rejected") || !strcmp(it[i].status, "dropped") ||
                   !strcmp(it[i].check, "failed") || !strcmp(it[i].check, "timeout");
        bz_mark(top, done ? BZ_OK : bad ? BZ_FAULT : !strcmp(it[i].status, "claimed") ? BZ_WARN : BZ_STALE, 12);
        bz_label_line(top, it[i].title, BZ_F_BODY_S, BZ_C_INK, lv_obj_get_width(list) - 90);
        char sub[160];
        if (patches)
            snprintf(sub, sizeof sub, "%s · %s%s%s", it[i].branch, it[i].status, it[i].check[0] ? " · check " : "",
                     it[i].check);
        else snprintf(sub, sizeof sub, "%s · %s", it[i].status, it[i].when);
        bz_label_line(r, sub, BZ_F_CAPTION, BZ_C_DIM, lv_obj_get_width(list) - 40);
    }
}

static void link_refresh(void)
{
    link_status_t s;
    link_status(&s);
    const char *st = !s.configured ? "looking for a pc" : !s.reachable ? "not answering" : !s.auth ? "wrong token" : "paired";
    bz_mark_set(LK.pc_mark, s.reachable && s.auth ? BZ_OK : s.reachable ? BZ_WARN : BZ_STALE);
    ui_text(LK.state, "%s", s.reachable ? (s.name[0] ? s.name : "catalyst link") : st);
    /* the repo's line only when there is one (an empty line read as a gap in the card); the outbox on the last */
    char repo[120] = "";
    if (s.repo[0] || s.branch[0])
        snprintf(repo, sizeof repo, "%s%s%s\n", s.repo, s.repo[0] && s.branch[0] ? " on " : "", s.branch);
    char out[48] = "";
    if (s.outbox) snprintf(out, sizeof out, " · %d waiting for the pc", s.outbox);
    ui_text(LK.where, "%s\n%sclaude through the pc: %s%s", s.url[0] ? s.url : "no address yet", repo, s.claude ? "yes" : "no", out);
    char keys[160];
    keys_state(keys, sizeof keys);
    ui_text(LK.key_state, "%s", keys);
    link_item_t in[8], pa[8];
    int ni = link_inbox(in, 8), np = link_patches(pa, 8);
    uint32_t sig = (uint32_t)(ni * 31 + np) ^ (uint32_t)s.inbox_open * 131u ^ (uint32_t)s.patches * 977u;
    for (int i = 0; i < ni; i++) sig = sig * 33 + (uint8_t)in[i].status[0] + (uint8_t)in[i].id[strlen(in[i].id) / 2];
    for (int i = 0; i < np; i++) sig = sig * 33 + (uint8_t)pa[i].status[0] + (uint8_t)pa[i].check[0];
    if (sig == LK.sig) return;
    LK.sig = sig;
    lk_list(LK.inbox, in, ni, false);
    lk_list(LK.patches, pa, np, true);
}

static void lk_pair(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_open(&APP_PAIR, o);
}

static void link_open(void)
{
    /* the pair app may have paired it since this was built */
    hal_kv_get("link_url", LK.url, sizeof LK.url);
    hal_kv_get("link_token", LK.token, sizeof LK.token);
    as_route_t r = current_route();
    for (int i = 0; i < 3; i++) ui_chip_set(LK.route_chips[i], (int)r == i);
    LK.sig = 0xFFFFFFFFu; /* no signature matches: the lists draw, empty ones included */
}

static void link_build(lv_obj_t *b)
{
    lv_obj_t *head = bz_col(b, 2);
    lv_obj_align(head, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 26);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    bz_label(head, "catalyst link", BZ_F_LABEL, BZ_C_DIM);
    bz_label(head, "The PC", BZ_F_TITLE, BZ_C_INK);
    hal_kv_get("link_url", LK.url, sizeof LK.url);
    hal_kv_get("link_token", LK.token, sizeof LK.token);

    /* wide enough that the buttons and the route chips each take one row at the tablet's type size */
    int c1 = 480, c2 = (W - 2 * PAD - c1 - 2 * BZ_GAP) / 2;
    lv_obj_t *t = bz_tile(b, c1, APP_H);
    lv_obj_set_pos(t, PAD, APP_Y);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 10, 0); /* everything down to the key buttons fits the card */
    bz_label(t, "pc", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = bz_row(t, 12);
    LK.pc_mark = bz_mark(r, BZ_STALE, 14);
    LK.state = bz_label_line(r, "", BZ_F_NAME, BZ_C_INK, c1 - 2 * BZ_PAD_TILE - 30);
    LK.where = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(LK.where, c1 - 2 * BZ_PAD_TILE); /* a long address wraps inside the card */
    lv_label_set_long_mode(LK.where, LV_LABEL_LONG_WRAP);
    lv_obj_t *br = bz_row(t, 8);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(br, c1 - 2 * BZ_PAD_TILE);
    ui_button(br, BZ_I_LINK, "pair", lk_pair, NULL);
    ui_button(br, BZ_I_RADAR, "find", lk_find, NULL);
    ui_button(br, BZ_I_LINK, "address", lk_edit, (void *)0);
    ui_button(br, BZ_I_KEYBOARD, "token", lk_edit, (void *)1);
    bz_label(t, "the assistant's model", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *rr = bz_row(t, 8);
    lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(rr, c1 - 2 * BZ_PAD_TILE);
    lv_obj_set_style_pad_row(rr, 8, 0);
    LK.route_chips[AS_ROUTE_LINK] = ui_chip(rr, "claude via pc", lk_route, (void *)(intptr_t)AS_ROUTE_LINK);
    LK.route_chips[AS_ROUTE_DIRECT] = ui_chip(rr, "claude key", lk_route, (void *)(intptr_t)AS_ROUTE_DIRECT);
    LK.route_chips[AS_ROUTE_OPENAI] = ui_chip(rr, "openai key", lk_route, (void *)(intptr_t)AS_ROUTE_OPENAI);
    LK.key_state = bz_label(t, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(LK.key_state, c1 - 2 * BZ_PAD_TILE);
    lv_label_set_long_mode(LK.key_state, LV_LABEL_LONG_WRAP);
    lv_obj_t *kr = bz_row(t, 8);
    lv_obj_set_flex_flow(kr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(kr, c1 - 2 * BZ_PAD_TILE);
    lv_obj_set_style_pad_row(kr, 8, 0);
    ui_button(kr, BZ_I_KEYBOARD, "claude key", lk_edit, (void *)2);
    ui_button(kr, BZ_I_KEYBOARD, "openai key", lk_edit, (void *)3);

    /* the two lists: their headings and their first words on one left edge (the rows' own text is inset) */
    lv_obj_t *w1 = bz_box(b);
    lv_obj_set_pos(w1, PAD + c1 + BZ_GAP, APP_Y);
    lv_obj_t *l1 = bz_label(w1, "work orders", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(l1, 18, 0);
    lv_obj_t *s1 = bz_box(w1);
    lv_obj_set_pos(s1, 0, 28);
    LK.inbox = ui_scroller(s1, c2, APP_H - 28);
    lv_obj_t *w2 = bz_box(b);
    lv_obj_set_pos(w2, PAD + c1 + c2 + 2 * BZ_GAP, APP_Y);
    lv_obj_t *l2 = bz_label(w2, "patch branches", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_set_pos(l2, 18, 0);
    lv_obj_t *s2 = bz_box(w2);
    lv_obj_set_pos(s2, 0, 28);
    LK.patches = ui_scroller(s2, c2, APP_H - 28);

    LK.kb = ui_kb_create(b, 420);
}

/* ================================================================== settings > assistant */

#define NVOICES 7 /* the companion's voices on offer (VOICES, below) */
static const int REMIND_S[4] = { 0, 60, 120, 300 };
static const char *const REMIND_L[4] = { "once", "every minute", "every 2 min", "every 5 min" };

static struct {
    lv_obj_t *route[3], *keys, *orb[2], *desk, *remind[4];
    lv_obj_t *vout[3], *vwake, *vfollow, *vvoice[NVOICES], *vstate, *mem;
    ui_kb_t *kb;
    int editing;           /* 0-4 the keys (key_typed); 10-13 the companion's voice (VOICE_TITLE) */
} SA;

/* the companion's voice */
static const char *const VOUT_L[3] = { "speak and show", "speak only", "show only" };
static const vo_out_t VOUT[3] = { VO_OUT_BOTH, VO_OUT_SPEAK, VO_OUT_SHOW };
/* hers is a young woman's voice: the female voices first, marin (the default) leading; cedar is the one
 * male voice kept for choice. tts-1 has neither marin nor cedar: voice.c picks the nearest it does have. */
static const char *const VOICES[NVOICES] = { "marin", "coral", "nova", "shimmer", "sage", "alloy", "cedar" };
static const char *const VOICE_TITLE[4] = { "the companion's chat model (empty: the openai model above)",
                                            "speech to text model (empty: " VO_STT_DEFAULT ")",
                                            "speech model (empty: " VO_TTS_DEFAULT ")",
                                            "language you speak, e.g. en (empty: detect)" };

static void voice_state_text(char *out, size_t n)
{
    voice_config_t c;
    voice_config(&c);
    const char *w = voice_wake_word();
    snprintf(out, n, "chat %s · hears with %s · speaks with %s, voice %s · wake word: %s",
             c.chat_model[0] ? c.chat_model : "(the openai model)", c.stt_model[0] ? c.stt_model : VO_STT_DEFAULT,
             c.tts_model[0] ? c.tts_model : VO_TTS_DEFAULT, c.voice[0] ? c.voice : VO_VOICE_DEFAULT,
             !c.wake ? "off" : w ? w : "loads when the companion opens (none if the model partition wasn't flashed)");
}

static void sa_show(void)
{
    if (!SA.keys) return;
    as_route_t r = current_route();
    for (int i = 0; i < 3; i++) ui_chip_set(SA.route[i], (int)r == i);
    char k[160];
    keys_state(k, sizeof k);
    ui_text(SA.keys, "%s", k);
    ui_companion_cfg_t *c = ui_companion_cfg();
    ui_chip_set(SA.orb[0], !c->orb_companion);
    ui_chip_set(SA.orb[1], c->orb_companion);
    ui_chip_set(SA.desk, c->desk_auto);
    for (int i = 0; i < 4; i++) ui_chip_set(SA.remind[i], c->remind_s == REMIND_S[i]);
    if (SA.mem) {
        char a[160], b[160];
        assist_mem_status(AS_POOL_ASSIST, a, sizeof a);
        assist_mem_status(AS_POOL_COMPANION, b, sizeof b);
        ui_text(SA.mem, "the technician: %s\nthe companion: %s", a, b);
    }
    if (SA.vstate) {
        voice_config_t v;
        voice_config(&v);
        for (int i = 0; i < 3; i++) ui_chip_set(SA.vout[i], v.out == VOUT[i]);
        ui_chip_set(SA.vwake, v.wake);
        ui_chip_set(SA.vfollow, v.follow);
        for (int i = 0; i < NVOICES; i++) ui_chip_set(SA.vvoice[i], !strcmp(v.voice[0] ? v.voice : VO_VOICE_DEFAULT, VOICES[i]));
        char t[256];
        voice_state_text(t, sizeof t);
        ui_text(SA.vstate, "%s", t);
    }
}

static void sa_route(lv_obj_t *o, void *u)
{
    (void)o;
    as_route_t r = (as_route_t)(intptr_t)u;
    if (r != current_route()) ui_island_say(BZ_I_REFRESH, "new conversation on the new model");
    assist_use(r);
    sa_show();
}

static void sa_typed(const char *text, void *u)
{
    (void)u;
    if (SA.editing >= 10) {
        voice_config_t v;
        voice_config(&v);
        char t[48];
        snprintf(t, sizeof t, "%s", text);
        char *p = t;
        while (*p == ' ') p++;
        for (size_t l = strlen(p); l && (p[l - 1] == ' ' || p[l - 1] == '\n'); l--) p[l - 1] = 0;
        switch (SA.editing) {
        case 10: snprintf(v.chat_model, sizeof v.chat_model, "%s", p); break;
        case 11: snprintf(v.stt_model, sizeof v.stt_model, "%s", p); break;
        case 12: snprintf(v.tts_model, sizeof v.tts_model, "%s", p); break;
        case 13: snprintf(v.lang, sizeof v.lang, "%s", p); break;
        }
        voice_set_config(&v);
    } else {
        key_typed(SA.editing, text);
    }
    sa_show();
}

static void sa_vedit(lv_obj_t *o, void *u)
{
    (void)o;
    SA.editing = (int)(intptr_t)u;
    voice_config_t v;
    voice_config(&v);
    const char *cur = SA.editing == 10 ? v.chat_model : SA.editing == 11 ? v.stt_model : SA.editing == 12 ? v.tts_model : v.lang;
    ui_kb_show(SA.kb, VOICE_TITLE[SA.editing - 10], cur, false, true, sa_typed, NULL);
}

static void sa_vout(lv_obj_t *o, void *u)
{
    (void)o;
    voice_config_t v;
    voice_config(&v);
    v.out = VOUT[(int)(intptr_t)u];
    voice_set_config(&v);
    sa_show();
}

static void sa_vwake(lv_obj_t *o, void *u)
{
    (void)o;
    voice_config_t v;
    voice_config(&v);
    if ((intptr_t)u) v.follow = !v.follow;
    else v.wake = !v.wake;
    voice_set_config(&v);
    sa_show();
}

static void sa_vvoice(lv_obj_t *o, void *u)
{
    (void)o;
    voice_config_t v;
    voice_config(&v);
    snprintf(v.voice, sizeof v.voice, "%s", VOICES[(int)(intptr_t)u]);
    voice_set_config(&v);
    sa_show();
}

static void sa_edit(lv_obj_t *o, void *u)
{
    (void)o;
    SA.editing = (int)(intptr_t)u;
    char model[48] = "";
    if (SA.editing == 4) {
        assist_config_t *c = malloc(sizeof *c);
        if (c) {
            assist_config(c);
            snprintf(model, sizeof model, "%s", c->oai_model);
            memset(c, 0, sizeof *c);
            free(c);
        }
    }
    ui_kb_show(SA.kb, KEY_TITLE[SA.editing], model, SA.editing < 4, true, sa_typed, NULL);
}

static void sa_orb(lv_obj_t *o, void *u)
{
    (void)o;
    ui_companion_cfg()->orb_companion = (intptr_t)u != 0;
    ui_companion_cfg_save();
    sa_show();
}

static void sa_desk(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    ui_companion_cfg_t *c = ui_companion_cfg();
    c->desk_auto = !c->desk_auto;
    ui_companion_cfg_save();
    sa_show();
}

static void sa_remind(lv_obj_t *o, void *u)
{
    (void)o;
    ui_companion_cfg()->remind_s = REMIND_S[(int)(intptr_t)u];
    ui_companion_cfg_save();
    sa_show();
}

static void sa_open_companion(lv_obj_t *o, void *u)
{
    (void)u;
    ui_app_close();
    ui_companion_open(false, o);
}

static void sa_memory(lv_obj_t *o, void *u) { ui_memory_open((int)(intptr_t)u, o); }

static lv_obj_t *sa_row(lv_obj_t *parent, int w)
{
    lv_obj_t *r = bz_row(parent, 10);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_set_width(r, w);
    return r;
}

void ui_assist_settings(lv_obj_t *pane, lv_obj_t *body, int w)
{
    /* everything below the pane's own lines, in a scroller that takes what's left of the pane */
    lv_obj_t *col = ui_scroller(pane, w, 200);
    lv_obj_set_flex_grow(lv_obj_get_parent(col), 1);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_style_pad_bottom(col, 16, 0);

    bz_label(col, "the model", BZ_F_LABEL, BZ_C_DIM);
    lv_obj_t *r = sa_row(col, w);
    SA.route[AS_ROUTE_LINK] = ui_chip(r, "claude through the pc", sa_route, (void *)(intptr_t)AS_ROUTE_LINK);
    SA.route[AS_ROUTE_DIRECT] = ui_chip(r, "claude, key on tablet", sa_route, (void *)(intptr_t)AS_ROUTE_DIRECT);
    SA.route[AS_ROUTE_OPENAI] = ui_chip(r, "openai, key on tablet", sa_route, (void *)(intptr_t)AS_ROUTE_OPENAI);
    SA.keys = bz_label(col, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SA.keys, w);
    lv_label_set_long_mode(SA.keys, LV_LABEL_LONG_WRAP);
    r = sa_row(col, w);
    ui_button(r, BZ_I_KEYBOARD, "claude key", sa_edit, (void *)2);
    ui_button(r, BZ_I_KEYBOARD, "openai key", sa_edit, (void *)3);
    ui_button(r, BZ_I_EDIT_NOTE, "openai model", sa_edit, (void *)4);
    lv_obj_t *n = bz_label(col, "OpenAI runs straight from the tablet over Wi-Fi, no PC needed; the key stays on the tablet. "
                                "The default model is gpt-5: check that your OpenAI account offers it, or set another.",
                           BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    bz_label(col, "memory", BZ_F_LABEL, BZ_C_DIM);
    SA.mem = bz_label(col, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SA.mem, w);
    lv_label_set_long_mode(SA.mem, LV_LABEL_LONG_WRAP);
    r = sa_row(col, w);
    ui_button(r, BZ_I_PSYCHOLOGY, "the technician's", sa_memory, (void *)(intptr_t)AS_POOL_ASSIST);
    ui_button(r, BZ_I_PSYCHOLOGY, "the companion's", sa_memory, (void *)(intptr_t)AS_POOL_COMPANION);
    n = bz_label(col, "Each keeps its own long-term memory, and neither can read the other's: the technician its season "
                      "notebook and what it learns about the robot and the team, the companion her persona and what she "
                      "learns about you. Both on the card, in CATOS/DATA. See, delete, or forget everything there.",
                 BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    bz_label(col, "the orb opens", BZ_F_LABEL, BZ_C_DIM);
    r = sa_row(col, w);
    SA.orb[0] = ui_chip(r, "the assistant", sa_orb, (void *)0);
    SA.orb[1] = ui_chip(r, "the companion", sa_orb, (void *)1);

    bz_label(col, "desk mode", BZ_F_LABEL, BZ_C_DIM);
    r = sa_row(col, w);
    SA.desk = ui_chip(r, "open by itself on a stand while charging", sa_desk, NULL);
    ui_button(r, BZ_I_VISIBILITY, "open the companion", sa_open_companion, NULL);

    bz_label(col, "the companion's voice (openai, with the key above)", BZ_F_LABEL, BZ_C_DIM);
    r = sa_row(col, w);
    for (int i = 0; i < 3; i++) SA.vout[i] = ui_chip(r, VOUT_L[i], sa_vout, (void *)(intptr_t)i);
    r = sa_row(col, w);
    SA.vwake = ui_chip(r, "wake word", sa_vwake, (void *)0);
    SA.vfollow = ui_chip(r, "listen for a follow-up", sa_vwake, (void *)1);
    r = sa_row(col, w);
    for (int i = 0; i < NVOICES; i++) SA.vvoice[i] = ui_chip(r, VOICES[i], sa_vvoice, (void *)(intptr_t)i);
    r = sa_row(col, w);
    ui_button(r, BZ_I_EDIT_NOTE, "chat model", sa_vedit, (void *)10);
    ui_button(r, BZ_I_EDIT_NOTE, "hearing model", sa_vedit, (void *)11);
    ui_button(r, BZ_I_EDIT_NOTE, "speech model", sa_vedit, (void *)12);
    ui_button(r, BZ_I_EDIT_NOTE, "language", sa_vedit, (void *)13);
    SA.vstate = bz_label(col, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(SA.vstate, w);
    lv_label_set_long_mode(SA.vstate, LV_LABEL_LONG_WRAP);
    n = bz_label(col, "The microphones are on only while the companion is on screen. The wake word is heard on the tablet "
                      "itself; what you say after it, or after a tap on the face, goes to OpenAI to be transcribed and answered, "
                      "and the answer is spoken back. The companion keeps its own conversation, apart from assist's.",
                 BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    bz_label(col, "when claude code on the pc finishes or needs you, remind", BZ_F_LABEL, BZ_C_DIM);
    r = sa_row(col, w);
    for (int i = 0; i < 4; i++) SA.remind[i] = ui_chip(r, REMIND_L[i], sa_remind, (void *)(intptr_t)i);
    n = bz_label(col, "Needs the Claude Code hooks on the PC: tab5/link/README.md, \"Claude Code on the tablet\". A tap on the "
                      "companion's face, or on the session, stops the reminder.",
                 BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    SA.kb = ui_kb_create(body, 420);
    sa_show();
}

void ui_assist_settings_open(void)
{
    if (SA.kb) ui_kb_hide(SA.kb);
    sa_show();
}

const ui_app_t APP_LINK = { .name = "link", .icon = BZ_I_COMPUTER, .build = link_build, .open = link_open,
                            .refresh = link_refresh };

/* ================================================================== memory
 *
 * What each AI remembers, and the owner's way to take any of it back. Two pools, one screen: the chips at
 * the top pick whose. On the left the self it carries into every conversation -- the technician's season
 * notebook, the companion's persona -- each section with its own ×; on the right its memories, the most
 * valuable first, each with an × (tap it twice: the second tap forgets); at the bottom of the left, forget
 * everything (also twice). Built again only when a pool changes (assist_mem_gen), never per frame. */

#define MEM_ROWS 120

static struct {
    lv_obj_t *chips[2], *status, *self_head, *self_list, *list, *all_btn, *all_label, *empty;
    int pool;
    uint32_t gen;
    bool built_once;
    int armed_id;          /* a memory whose × was tapped once; -1 none; 1000+i a self section */
    double armed_at;
    bool armed_all;
    as_memory_t *rows;
    int lw, rw, lh;
} MM = { .armed_id = -1 };

static const char *const POOL_WHO[2] = { "the technician", "the companion" };

static void mm_rebuild(void);

static void mm_disarm(void)
{
    MM.armed_id = -1;
    if (MM.armed_all) {
        MM.armed_all = false;
        ui_text(MM.all_label, "forget everything");
    }
}

static void mm_row_x(lv_obj_t *o, void *u)
{
    (void)o;
    int id = (int)(intptr_t)u;
    double now = hal_seconds();
    if (MM.armed_id != id || now - MM.armed_at > 4) {
        mm_disarm();
        MM.armed_id = id;
        MM.armed_at = now;
        char key[24] = "that section";
        if (id >= 1000) assist_mem_self((as_pool_t)MM.pool, id - 1000, key, sizeof key, NULL, 0);
        for (char *p = key; *p; p++) if (*p == '_') *p = ' ';
        if (id >= 1000) ui_text(MM.status, "tap \xc3\x97 again to empty \"%s\"", key);
        else ui_text(MM.status, "tap \xc3\x97 again to forget #%d", id);
        return;
    }
    MM.armed_id = -1;
    bool ok = id >= 1000 ? assist_mem_clear_self((as_pool_t)MM.pool, id - 1000) : assist_mem_forget((as_pool_t)MM.pool, id);
    hal_tone(ok ? 900 : 500, 30, S.volume * 0.5f);
    if (!ok) ui_island_say(BZ_I_PSYCHOLOGY, "that memory couldn't be changed right now");
    mm_rebuild();
}

static void mm_all(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    double now = hal_seconds();
    if (!MM.armed_all || now - MM.armed_at > 4) {
        mm_disarm();
        MM.armed_all = true;
        MM.armed_at = now;
        ui_text(MM.all_label, "tap again to forget it all");
        return;
    }
    MM.armed_all = false;
    ui_text(MM.all_label, "forget everything");
    assist_mem_forget_all((as_pool_t)MM.pool);
    char msg[64];
    snprintf(msg, sizeof msg, "%s's memory is empty now", POOL_WHO[MM.pool]);
    ui_island_say(BZ_I_PSYCHOLOGY, msg);
    mm_rebuild();
}

static void mm_pool(lv_obj_t *o, void *u)
{
    (void)o;
    int p = (int)(intptr_t)u;
    if (p == MM.pool) return;
    MM.pool = p;
    mm_disarm();
    mm_rebuild();
}

/* a small round × on a row's top right */
static lv_obj_t *mm_x(lv_obj_t *parent, int id)
{
    lv_obj_t *x = ui_button(parent, BZ_I_CLOSE, NULL, mm_row_x, (void *)(intptr_t)id);
    lv_obj_set_style_pad_hor(x, 14, 0);
    lv_obj_set_size(x, 52, 52);
    lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    return x;
}

static void mm_rebuild(void)
{
    MM.gen = assist_mem_gen();
    for (int i = 0; i < 2; i++) ui_chip_set(MM.chips[i], MM.pool == i);
    char st[160];
    assist_mem_status((as_pool_t)MM.pool, st, sizeof st);
    ui_text(MM.status, "%s", st);
    ui_text(MM.self_head, "%s", MM.pool == AS_POOL_ASSIST ? "season notebook" : "persona");

    /* the self sections */
    lv_obj_clean(MM.self_list);
    int iw = MM.lw - 2 * BZ_PAD_TILE;
    char *text = malloc(1024);
    for (int i = 0; text && i < 4; i++) {
        char key[24];
        if (!assist_mem_self((as_pool_t)MM.pool, i, key, sizeof key, text, 1024)) break;
        lv_obj_t *r = bz_box(MM.self_list);
        lv_obj_set_width(r, iw);
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_t *k = bz_label(r, "", BZ_F_LABEL, BZ_C_DIM);
        for (char *p = key; *p; p++) if (*p == '_') *p = ' ';
        ui_text(k, "%s", key);
        lv_obj_t *t = bz_label(r, "", BZ_F_BODY_S, text[0] ? BZ_C_INK : BZ_C_FAINT);
        lv_label_set_text(t, text[0] ? text : "nothing yet");
        lv_obj_set_width(t, iw - (text[0] ? 60 : 0));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(t, 0, 30);
        if (text[0]) mm_x(r, 1000 + i);
    }
    free(text);

    /* the memories */
    lv_obj_clean(MM.list);
    int n = MM.rows ? assist_mem_list((as_pool_t)MM.pool, MM.rows, MEM_ROWS) : -1;
    if (n <= 0) {
        MM.empty = ui_empty(MM.list, MM.rw,
                            n < 0 ? "memory can't be read right now" : "nothing remembered yet",
                            n < 0 ? "The card didn't answer: it is tried again in a moment."
                                  : MM.pool == AS_POOL_ASSIST
                                        ? "The technician remembers the robot's quirks, faults and fixes, the team's "
                                          "procedures and the season as you work with it in Assist."
                                        : "The companion remembers you: names, plans, what you like, the jokes you "
                                          "share, as you talk with her.");
        return;
    }
    for (int i = 0; i < n; i++) {
        const as_memory_t *m = &MM.rows[i];
        lv_obj_t *t = bz_tile(MM.list, MM.rw, LV_SIZE_CONTENT);
        bz_tile_set_fill(t, BZ_C_SURFACE2);
        lv_obj_set_style_pad_ver(t, 14, 0);
        lv_obj_set_style_pad_hor(t, 20, 0);
        lv_obj_set_style_radius(t, 20, 0);
        char d[16] = "";
        if (m->updated > 0) {
            time_t tt = (time_t)m->updated;
            struct tm tm;
            localtime_r(&tt, &tm);
            strftime(d, sizeof d, "%b %d", &tm);
        }
        lv_obj_t *meta = bz_label(t, "", BZ_F_CAPTION, m->imp >= 3 ? BZ_C_ICE : BZ_C_DIM);
        ui_text(meta, "#%d · %s · %s%s%s", m->id, m->kind, m->imp >= 3 ? "important" : m->imp == 2 ? "useful" : "minor",
                d[0] ? " · " : "", d);
        lv_obj_t *tx = bz_label(t, "", BZ_F_BODY_S, BZ_C_INK);
        lv_label_set_text(tx, m->text);
        lv_obj_set_width(tx, MM.rw - 40 - 64);
        lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(tx, 0, 26);
        mm_x(t, m->id);
    }
}

static void memory_open(void)
{
    if (!MM.rows) MM.rows = malloc(sizeof *MM.rows * MEM_ROWS); /* 60 KB: PSRAM */
    mm_disarm();
    mm_rebuild();
}

static void memory_refresh(void)
{
    double now = hal_seconds();
    if ((MM.armed_id >= 0 || MM.armed_all) && now - MM.armed_at > 4) {
        mm_disarm();
        char st[160];
        assist_mem_status((as_pool_t)MM.pool, st, sizeof st);
        ui_text(MM.status, "%s", st);
    }
    if (assist_mem_gen() != MM.gen) mm_rebuild(); /* a conversation remembered something meanwhile */
}

static void memory_build(lv_obj_t *b)
{
    lv_obj_t *head = bz_col(b, 2);
    lv_obj_align(head, LV_ALIGN_TOP_RIGHT, HEAD_RIGHT_X, 26);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    bz_label(head, "what the ais remember", BZ_F_LABEL, BZ_C_DIM);
    bz_label(head, "Memory", BZ_F_TITLE, BZ_C_INK);

    /* landscape: the self on the left, the memories on the right; portrait: one above the other */
    int lx = PAD, ly = APP_Y, rx, ry;
    if (PORTRAIT) {
        MM.lw = W - 2 * PAD;
        MM.lh = 470;
        MM.rw = W - 2 * PAD;
        rx = PAD;
        ry = APP_Y + MM.lh + BZ_GAP;
    } else {
        MM.lw = 470;
        MM.lh = APP_H;
        MM.rw = W - 2 * PAD - MM.lw - BZ_GAP;
        rx = PAD + MM.lw + BZ_GAP;
        ry = APP_Y;
    }
    lv_obj_t *l = bz_tile(b, MM.lw, MM.lh);
    lv_obj_set_pos(l, lx, ly);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_t *cr = bz_row(l, 8);
    MM.chips[0] = ui_chip(cr, "the technician", mm_pool, (void *)0);
    MM.chips[1] = ui_chip(cr, "the companion", mm_pool, (void *)1);
    MM.status = bz_label(l, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_width(MM.status, MM.lw - 2 * BZ_PAD_TILE);
    lv_label_set_long_mode(MM.status, LV_LABEL_LONG_WRAP);
    MM.self_head = bz_label(l, "", BZ_F_NAME, BZ_C_INK);
    MM.self_list = ui_scroller(l, MM.lw - 2 * BZ_PAD_TILE, 200);
    lv_obj_set_flex_grow(lv_obj_get_parent(MM.self_list), 1); /* what the rest of the tile leaves */
    lv_obj_set_style_pad_row(MM.self_list, 16, 0);
    MM.all_btn = ui_button(l, BZ_I_CLOSE, "forget everything", mm_all, NULL);
    MM.all_label = lv_obj_get_child(MM.all_btn, 1);

    lv_obj_t *wrap = bz_box(b);
    lv_obj_set_pos(wrap, rx, ry);
    MM.list = ui_scroller(wrap, MM.rw, PORTRAIT ? H - ry - PAD : APP_H);
    lv_obj_set_style_pad_row(MM.list, 10, 0);
    lv_obj_set_style_pad_bottom(MM.list, 12, 0);
}

const ui_app_t APP_MEMORY = { .name = "memory", .icon = BZ_I_PSYCHOLOGY, .build = memory_build, .open = memory_open,
                              .refresh = memory_refresh };

/* ================================================================== the dev console
 *
 *   assist ask <text>     a typed question to the technician, as its keyboard would send it (the app opens)
 *   assist new            a new conversation
 *   assist dump           the transcript on the console, one "ASSIST" line an entry
 *   memo [a|c]            a pool's status, self sections and memories on the console ("MEMO" lines)
 *   memo digest a|c [q]   exactly what that model is shown at the start of a conversation
 *   memo forget a|c <id>, memo clear a|c, memo open a|c
 * The console's replies are read with tab5-shots/brain/tabcmd.py (tab5_dev.py prints only its own lines). */

/* one console line for a long text: newlines as " / ", cut at `max` */
static void dev_print(const char *tag, const char *head, const char *text, size_t max)
{
    size_t n = strlen(text ? text : "");
    char *b = malloc(max + 8);
    if (!b) return;
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        char c = text[i];
        if (c == '\n') {
            if (k + 3 >= max) break;
            memcpy(b + k, " / ", 3);
            k += 3;
        } else if ((unsigned char)c >= 0x20 || c == '\t') b[k++] = c;
    }
    b[k] = 0;
    printf("%s %s%s%s\n", tag, head, b, n > k ? " …" : "");
    free(b);
}

void ui_assist_dev(const char *cmd)
{
    if (!strncmp(cmd, "assist ask ", 11)) {
        if (!ui_app_is_open(&APP_ASSIST)) ui_app_open(&APP_ASSIST, NULL);
        ask(cmd + 11);
        printf("ASSIST asked: %s\n", cmd + 11);
    } else if (!strcmp(cmd, "assist new")) {
        new_tap(NULL, NULL);
        printf("ASSIST a new conversation\n");
    } else if (!strncmp(cmd, "assist robot", 12)) {
        /* "assist robot 192.168.0.5": the robot looked for at that address first (tools/fake_robot.py on a
         * PC, for testing the technician against a robot); "assist robot" alone goes back to the team's */
        const char *a = cmd + 12;
        while (*a == ' ') a++;
        snprintf(S.address, sizeof S.address, "%s", a);
        ui_settings_save();
        ui_apply_addresses();
        printf("ASSIST robot address: %s\n", S.address[0] ? S.address : "the team's (no override)");
    } else if (!strncmp(cmd, "assist dump", 11)) {
        static const char *const KIND[] = { "you", "answer", "thinking", "tool", "note", "error" };
        assist_lock();
        int n = assist_count();
        printf("ASSIST %d entries, phase %s\n", n, phase_word(assist_phase_locked()));
        for (int i = 0; i < n; i++) {
            const as_entry_t *e = assist_entry(i);
            char head[64];
            snprintf(head, sizeof head, "%d %s%s%s: ", i, KIND[e->kind < 6 ? e->kind : 4], e->tool && e->tool[0] ? " " : "",
                     e->tool ? e->tool : "");
            dev_print("ASSIST", head, e->text, e->kind == AS_E_TEXT ? 1400 : 300);
        }
        assist_unlock();
    } else if (!strncmp(cmd, "memo", 4)) {
        const char *a = cmd + 4;
        while (*a == ' ') a++;
        char verb[12] = "";
        int k = 0;
        while (*a && *a != ' ' && k < 11) verb[k++] = *a++;
        verb[k] = 0;
        while (*a == ' ') a++;
        /* "memo a", "memo c": the verb is the pool */
        bool list = !verb[0] || !strcmp(verb, "a") || !strcmp(verb, "c");
        const char *pp = list ? verb : a;
        as_pool_t pool = pp[0] == 'c' ? AS_POOL_COMPANION : AS_POOL_ASSIST;
        if (!list) {
            while (*a && *a != ' ') a++;
            while (*a == ' ') a++;
        }
        if (list) {
            for (int p = 0; p < 2; p++) {
                if (verb[0] && p != (int)pool) continue;
                char st[160];
                assist_mem_status((as_pool_t)p, st, sizeof st);
                printf("MEMO %s: %s\n", POOL_WHO[p], st);
                char key[24], *text = malloc(1024);
                for (int i = 0; text && assist_mem_self((as_pool_t)p, i, key, sizeof key, text, 1024); i++) {
                    char head[40];
                    snprintf(head, sizeof head, "  %s: ", key);
                    dev_print("MEMO", head, text[0] ? text : "(empty)", 700);
                }
                free(text);
                as_memory_t *rows = malloc(sizeof *rows * MEM_ROWS);
                int n = rows ? assist_mem_list((as_pool_t)p, rows, MEM_ROWS) : -1;
                for (int i = 0; i < n; i++) {
                    char head[64];
                    snprintf(head, sizeof head, "  #%d %s %d: ", rows[i].id, rows[i].kind, rows[i].imp);
                    dev_print("MEMO", head, rows[i].text, 500);
                }
                free(rows);
            }
        } else if (!strcmp(verb, "digest")) {
            char *d = assist_mem_digest(pool, a);
            if (d) {
                /* line by line: it is what the model reads */
                for (char *s = d, *nl; s && *s; s = nl ? nl + 1 : NULL) {
                    nl = strchr(s, '\n');
                    if (nl) *nl = 0;
                    printf("MEMO | %s\n", s);
                }
            }
            free(d);
        } else if (!strcmp(verb, "forget")) {
            int id = atoi(a);
            printf("MEMO forget %s #%d: %s\n", POOL_WHO[pool], id, assist_mem_forget(pool, id) ? "done" : "no such memory");
        } else if (!strcmp(verb, "clear")) {
            assist_mem_forget_all(pool);
            printf("MEMO %s's memory cleared\n", POOL_WHO[pool]);
        } else if (!strcmp(verb, "open")) {
            ui_memory_open((int)pool, NULL);
        } else {
            printf("MEMO usage: memo [a|c] | memo digest a|c [question] | memo forget a|c ID | memo clear a|c | memo open a|c\n");
        }
    }
}

void ui_memory_open(int pool, lv_obj_t *from)
{
    MM.pool = pool == AS_POOL_COMPANION ? AS_POOL_COMPANION : AS_POOL_ASSIST;
    if (ui_app_is_open(&APP_MEMORY) && MM.list) {
        memory_open();
        return;
    }
    ui_app_open(&APP_MEMORY, from);
}
