/* ui_kb — the keyboard sheet: one keyboard for the whole tablet, on the layer above everything.
 *
 * WHERE IT LIVES. There is exactly one sheet, built on bz_ui_glass() — the shell's overlay layer, the same
 * one the control centre, the lock screen, the island and the assistant's approval cards use. It used to be
 * a tile inside whichever app asked for it, which put it inside that app's stacking context: anything the
 * shell drew afterwards went over the top of it, and the keyboard is the one thing that must never be
 * covered. Each caller still gets its own small ui_kb_t handle (so ui_kb_open() answers for *its* field, and
 * a hide from a closing app cannot pull the sheet out from under another one), but they all raise the same
 * keys. One keyboard is also one buttonmatrix instead of six.
 *
 * THE UI MOVES. While the sheet is up, the app's body slides up by KB_LIFT, so the foot of what you were
 * reading is still on screen above the keys rather than buried under them. The app's head is on the glass
 * layer and stays where it is. The lift rides a bz_motion spring (BZ_LAYER): it springs in the simulator and
 * is instant on the tablet, because the lean renderer makes every spring instant — a moving full-width
 * surface is a redraw per frame the P4 cannot afford, and this is the same bargain the rest of Bezel makes.
 *
 * FEEDBACK. A pressed key fills with the accent and turns dark-on-light (LV_PART_ITEMS + LV_STATE_PRESSED),
 * and every character key carries a popover, so the letter appears above your fingertip. The popover earns
 * its place here: the panel is 5 inches at 294 ppi, a key is about 11 mm across but only 6 mm down, and a
 * fingertip covers it completely. The keys that are not characters — the layer switches, del, enter, done —
 * do not pop, because repeating a word you can already read is noise. (LVGL strips every popover flag
 * unless lv_keyboard_set_popovers() is on, whatever the control map says; that call is not optional.)
 *
 * SOUND. A short, soft, low tick per key, on the press rather than the release so it lands with the finger:
 * a click that trails the key reads as lag. Four pitches, a laptop key rather than a beep — the ordinary
 * keys high and quiet, space a little deeper, enter deeper again and del lowest, so the two keys you hit by
 * mistake are the two you can hear. hal_tone's envelope is a 3 ms raised cosine, so these are all attack and
 * decay with no steady tone in the middle. A held key ticks on every repeat. It follows the tablet's own
 * click setting and volume: silent means silent.
 *
 * THE LAYOUT. Five rows of ten. Digits get a row of their own — this is a pit tool and half of what anyone
 * types into it is an address — and the punctuation of one (. : / - _ @) is all on the first layer, so
 * 10.58.5.2:8765 needs no layer switch at all. The control keys are words, not LVGL's symbols: those live
 * in Montserrat's private-use area, which Bezel's faces do not carry, so the old sheet drew them as blank
 * keys. Words also mean this file dispatches every key itself (lv_keyboard_def_event_cb is removed), which
 * is simpler to read than matching magic strings.
 */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

/* The sheet is 452 of the screen's 720, which is a lot, and it is the honest trade. The panel is 5 inches
 * at 294 ppi, so the whole screen is only 62 mm tall: five rows of keys cannot each be the 10 mm a phone
 * gives them. At this height a key is 11 mm across and about 6 mm down — short, but a real target. What is
 * left above is the status band and a screenful of the app, lifted to meet it. */
#define KB_H 452
#define KB_FIELD_H 52
#define KB_PAD 12
#define KB_LIFT 170         /* how far the app's body rises to keep its foot in sight */
#define KB_GAP_MS 60        /* two presses of one key closer together than this: the second is a bounce */

/* the control keys, by the text on them */
#define K_DEL "del"
#define K_ENTER "enter"
#define K_DONE "done"
#define K_UPPER "ABC"
#define K_LOWER "abc"
#define K_SYMBOL "1#"

struct ui_kb {
    lv_obj_t *body;         /* the app content that moves out of the way while this field is up */
    ui_kb_done_fn done;
    void *user;
};

static struct {
    bool built;
    lv_obj_t *catcher;      /* over everything above the sheet: a tap there puts the keyboard away */
    lv_obj_t *sheet, *title, *ta, *kb;
    ui_kb_t *owner;         /* whose field is up (NULL: none) */
    bool one_line;
    bz_motion_t lift;       /* the body's offset, px up */
    /* The body being lifted, held separately from `owner`: the keyboard stops having an owner the
     * moment it is dismissed, but the lift still has all the way back to zero to travel, and
     * something has to be there to receive it. Cleared when the spring lands. */
    lv_obj_t *lifted;
    float lift_at;          /* what is applied, so a still body is not moved every frame */
    uint32_t last_ms;
    int last_btn;
} K;

/* A character key fires on press and repeats while held, which is what a keyboard does, and pops its letter
 * above the finger; del is a character key in that sense, because holding it must keep deleting. The keys
 * that commit or change everything fire on release instead and never repeat, so a finger that lands on one
 * and slides off does nothing, and they are drawn in the quieter surface (CHECKED is how a buttonmatrix
 * asks for a second style). */
#define KB_W1 (LV_BUTTONMATRIX_CTRL_POPOVER | 1)
#define KB_REP 1
#define KB_CTRL (LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1)

static const char *const MAP_LOWER[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", K_DEL, "\n",
    K_UPPER, "z", "x", "c", "v", "b", "n", "m", ",", K_ENTER, "\n",
    K_SYMBOL, ".", ":", "/", " ", "-", "_", "@", K_DONE, ""
};
static const lv_buttonmatrix_ctrl_t CTRL_TEXT[] = {
    KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1,
    KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1,
    KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_REP,
    KB_CTRL, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_W1, KB_CTRL,
    KB_CTRL, KB_W1, KB_W1, KB_W1, 4, KB_W1, KB_W1, KB_W1, KB_CTRL,
};

static const char *const MAP_UPPER[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", K_DEL, "\n",
    K_LOWER, "Z", "X", "C", "V", "B", "N", "M", ",", K_ENTER, "\n",
    K_SYMBOL, ".", ":", "/", " ", "-", "_", "@", K_DONE, ""
};

/* The second layer: the rest of what a technician types, and the brackets and quotes a path or a bit of
 * JSON needs. Its own layer key comes back to the letters. */
static const char *const MAP_SPECIAL[] = {
    "!", "?", "#", "$", "%", "&", "*", "+", "=", "~", "\n",
    "(", ")", "[", "]", "{", "}", "<", ">", "|", K_DEL, "\n",
    "\"", "'", "`", "^", ";", "\\", "\xc2\xb0", "\xc2\xb7", "\xe2\x80\x94", K_ENTER, "\n",
    K_LOWER, "1", "2", "3", "4", "5", "6", "7", "8", "9", "\n",
    K_LOWER, ".", ":", "/", " ", "-", "_", "0", K_DONE, ""
};

static bool is(const char *a, const char *b) { return a && !strcmp(a, b); }

/* ---- the sound ---- */
static void key_tick(const char *txt)
{
    if (!S.clicks || S.volume <= 0.02f) return;
    float hz = 1900, gain = 0.16f;
    int ms = 7;
    if (is(txt, " ")) {
        hz = 1500;
        ms = 9;
    } else if (is(txt, K_DEL)) {
        hz = 1100;
        ms = 10;
        gain = 0.19f;
    } else if (is(txt, K_ENTER) || is(txt, K_DONE)) {
        hz = 1300;
        ms = 10;
        gain = 0.19f;
    } else if (is(txt, K_UPPER) || is(txt, K_LOWER) || is(txt, K_SYMBOL)) {
        hz = 2200;
        ms = 5;
        gain = 0.12f;
    }
    hal_tone(hz, ms, S.volume * gain);
}

#define NOT_APPLIED (-1.0f)   /* the lift is never negative, so this cannot match a real value */

static void lift_to(float px) { bz_motion_to(&K.lift, px, BZ_LAYER); }

static void put_away(void)
{
    if (!K.owner) return;
    K.owner = NULL;
    lv_obj_add_flag(K.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(K.catcher, LV_OBJ_FLAG_HIDDEN);
    lift_to(0);   /* K.lifted deliberately stays: the body has to be put back down first */
}

/* Hand the text back and put the sheet away. */
static void commit(bool ready)
{
    ui_kb_t *k = K.owner;
    const char *text = lv_textarea_get_text(K.ta);
    put_away();
    if (ready && k && k->done) k->done(text, k->user);
}

/* Every frame: carry the lift, and put the keyboard away if the app it belongs to has gone. */
static void kb_frame(double now, double dt, void *u)
{
    (void)now; (void)dt; (void)u;
    if (K.owner && K.owner->body && !lv_obj_is_visible(K.owner->body)) put_away();
    bool moving = bz_motion_tick(&K.lift);
    /* K.lifted, not K.owner->body: after a dismiss there is no owner and the body still has to come
     * down. An app can also be closed while lifted, so the object is checked before it is written. */
    if (K.lifted && !lv_obj_is_valid(K.lifted)) {
        K.lifted = NULL;
        K.lift_at = NOT_APPLIED;
    }
    if (K.lifted) {
        float v = K.lift.value;
        if (v != K.lift_at) {
            K.lift_at = v;
            lv_obj_set_style_translate_y(K.lifted, -(int)(v + 0.5f), 0);
        }
        if (!moving && !K.owner && v < 0.5f) {
            /* Landed, and nobody is using it: put it back exactly, then let go. Rounding the spring's
             * tail could otherwise leave the app a pixel high for as long as it is open. */
            lv_obj_set_style_translate_y(K.lifted, 0, 0);
            K.lifted = NULL;
            K.lift_at = NOT_APPLIED;
        }
    }
    if (moving) bz_ui_keep_alive();
}

/* Every key, dispatched here: LVGL's own handler was removed, because it matches the symbol strings this
 * keyboard no longer uses and because one place that decides what a key does is easier to read. */
static void kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        /* the sound goes out with the touch, not the release: a click that trails the key reads as lag */
        uint32_t id = lv_buttonmatrix_get_selected_button(K.kb);
        uint32_t ms = lv_tick_get();
        if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
        /* a panel that reports one press twice must not make two clicks; a repeat is not a bounce */
        if (code == LV_EVENT_LONG_PRESSED_REPEAT || (int)id != K.last_btn || ms - K.last_ms >= KB_GAP_MS) {
            key_tick(lv_buttonmatrix_get_button_text(K.kb, id));
            K.last_btn = (int)id;
            K.last_ms = ms;
        }
        return;
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        commit(code == LV_EVENT_READY);
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED) return;
    uint32_t id = lv_buttonmatrix_get_selected_button(K.kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *t = lv_buttonmatrix_get_button_text(K.kb, id);
    if (!t) return;
    if (is(t, K_UPPER)) lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    else if (is(t, K_LOWER)) lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    else if (is(t, K_SYMBOL)) lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_SPECIAL);
    else if (is(t, K_DEL)) lv_textarea_delete_char(K.ta);
    else if (is(t, K_DONE)) commit(true);
    else if (is(t, K_ENTER)) {
        /* in a one-line field there is nowhere for a newline to go, so enter is done */
        if (K.one_line) commit(true);
        else lv_textarea_add_char(K.ta, '\n');
    } else {
        lv_textarea_add_text(K.ta, t);
        /* a capital was asked for once, not for the rest of the sentence */
        if (lv_keyboard_get_mode(K.kb) == LV_KEYBOARD_MODE_TEXT_UPPER)
            lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

static void catcher_tap(lv_obj_t *o, void *u)
{
    (void)o; (void)u;
    put_away(); /* a tap anywhere above the sheet puts it away, committing nothing */
}

static void build(void)
{
    if (K.built) return;
    K.built = true;
    lv_obj_t *g = bz_ui_glass();

    /* everything above the sheet catches a tap; it draws nothing */
    K.catcher = lv_obj_create(g);
    lv_obj_remove_style_all(K.catcher);
    lv_obj_set_size(K.catcher, W, H - KB_H);
    lv_obj_set_pos(K.catcher, 0, 0);
    lv_obj_add_flag(K.catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(K.catcher, LV_OBJ_FLAG_HIDDEN);
    bz_on_tap(K.catcher, catcher_tap, NULL);

    K.sheet = lv_obj_create(g);
    lv_obj_remove_style_all(K.sheet);
    lv_obj_set_size(K.sheet, W, KB_H);
    lv_obj_set_pos(K.sheet, 0, H - KB_H);
    lv_obj_add_style(K.sheet, bz_style_fill(BZ_C_SURFACE2), 0);
    lv_obj_set_style_radius(K.sheet, 28, 0);
    lv_obj_set_style_pad_all(K.sheet, KB_PAD, 0);
    /* a flex column, not three absolute positions: lv_keyboard leaves its constructor with a percentage
     * height and an alignment of its own, and laying the sheet out by hand fought both — the last row
     * ended up off the bottom of the screen. The keys take whatever the title and the field leave. */
    lv_obj_set_layout(K.sheet, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(K.sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(K.sheet, 6, 0);
    lv_obj_remove_flag(K.sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(K.sheet, LV_OBJ_FLAG_CLICKABLE); /* the gaps between keys are not the app behind */
    lv_obj_add_flag(K.sheet, LV_OBJ_FLAG_HIDDEN);

    K.title = bz_label(K.sheet, "", BZ_F_CAPTION, BZ_C_DIM);
    lv_obj_set_style_pad_left(K.title, 6, 0);

    K.ta = lv_textarea_create(K.sheet);
    lv_obj_set_width(K.ta, W - 2 * KB_PAD);
    lv_obj_set_height(K.ta, KB_FIELD_H);
    lv_obj_add_style(K.ta, bz_style_font(BZ_F_BODY), 0);
    lv_obj_add_style(K.ta, bz_style_fill(BZ_C_SURFACE1), 0);
    lv_obj_add_style(K.ta, bz_style_color(BZ_C_INK), 0);
    lv_obj_set_style_radius(K.ta, 14, 0);
    lv_obj_set_style_border_width(K.ta, 0, 0);
    lv_obj_set_style_pad_all(K.ta, 10, 0);
    lv_obj_set_style_bg_color(K.ta, bz_lv(BZ_C_ICE), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(K.ta, LV_OPA_COVER, LV_PART_CURSOR);

    K.kb = lv_keyboard_create(K.sheet);
    lv_obj_remove_event_cb(K.kb, lv_keyboard_def_event_cb); /* kb_event does all of it */
    lv_keyboard_set_textarea(K.kb, K.ta);
    lv_keyboard_set_map(K.kb, LV_KEYBOARD_MODE_TEXT_LOWER, MAP_LOWER, CTRL_TEXT);
    lv_keyboard_set_map(K.kb, LV_KEYBOARD_MODE_TEXT_UPPER, MAP_UPPER, CTRL_TEXT);
    lv_keyboard_set_map(K.kb, LV_KEYBOARD_MODE_SPECIAL, MAP_SPECIAL, CTRL_TEXT);
    lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(K.kb, true); /* without this LVGL strips the popover flags out of the map */
    lv_obj_set_width(K.kb, W - 2 * KB_PAD);
    lv_obj_set_flex_grow(K.kb, 1);
    lv_obj_set_align(K.kb, LV_ALIGN_DEFAULT); /* its constructor aligns it bottom-mid; flex places it */
    lv_obj_add_style(K.kb, bz_style_plain(), 0);
    lv_obj_set_style_pad_row(K.kb, 6, 0);
    lv_obj_set_style_pad_column(K.kb, 6, 0);
    lv_obj_set_style_pad_all(K.kb, 0, 0);

    /* a key at rest, and a key under a finger: the accent, so a press is unmistakable at arm's length */
    lv_obj_add_style(K.kb, bz_style_fill(BZ_C_SURFACE3), LV_PART_ITEMS);
    lv_obj_add_style(K.kb, bz_style_color(BZ_C_INK), LV_PART_ITEMS);
    lv_obj_set_style_text_font(K.kb, bz_font(BZ_F_BODY), LV_PART_ITEMS);
    lv_obj_set_style_radius(K.kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(K.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(K.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(K.kb, bz_lv(BZ_C_ICE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(K.kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(K.kb, bz_lv(BZ_C_ON_ICE), LV_PART_ITEMS | LV_STATE_PRESSED);
    /* the layer switches, del, enter and done: the quieter surface, so the letters read first */
    lv_obj_set_style_bg_color(K.kb, bz_lv(BZ_C_SURFACE1), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(K.kb, bz_font(BZ_F_LABEL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(K.kb, kb_event, LV_EVENT_ALL, NULL);

    bz_motion_init(&K.lift, 0, 0.1f);
    bz_ui_on_frame(kb_frame, NULL);
}

ui_kb_t *ui_kb_create(lv_obj_t *body, int h)
{
    (void)h; /* one sheet, one height: the callers' old guesses are no longer theirs to make */
    build();
    ui_kb_t *k = lv_malloc_zeroed(sizeof *k);
    if (k) k->body = body;
    return k;
}

void ui_kb_show(ui_kb_t *k, const char *title, const char *text, bool secret, bool one_line, ui_kb_done_fn done,
                void *user)
{
    if (!k) return;
    build();
    if (K.owner && K.owner != k) put_away(); /* another field was up: it gets no callback */
    k->done = done;
    k->user = user;
    K.owner = k;
    K.one_line = one_line;
    ui_text(K.title, "%s", title ? title : "");
    lv_textarea_set_one_line(K.ta, one_line); /* this sets the height to the content: put it back */
    lv_textarea_set_password_mode(K.ta, secret);
    lv_obj_set_height(K.ta, KB_FIELD_H);
    lv_textarea_set_text(K.ta, text ? text : "");
    lv_keyboard_set_mode(K.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(K.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(K.catcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(K.catcher);
    lv_obj_move_foreground(K.sheet); /* above every other overlay: the keyboard is the top layer */
    K.last_btn = -1;
    /* A different body than last time, or the same one after it was let go: either way the next
     * frame must write, so lift_at starts at a value no spring produces. */
    if (K.lifted != k->body) {
        if (K.lifted && lv_obj_is_valid(K.lifted)) lv_obj_set_style_translate_y(K.lifted, 0, 0);
        K.lifted = k->body;
        K.lift_at = NOT_APPLIED;
    }
    lift_to(KB_LIFT);
}

bool ui_kb_open(const ui_kb_t *k) { return k && K.owner == k; }

void ui_kb_hide(ui_kb_t *k)
{
    if (k && K.owner == k) put_away();
}

/* The shell, when something takes the screen out from under whoever was typing. */
void ui_kb_dismiss(void) { put_away(); }
