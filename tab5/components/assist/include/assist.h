/* assist — the tablet's AI technician.
 *
 * A Claude conversation (Messages API, streamed) whose tools are the robot, the tablet and the PC:
 * it reads what Catalyst Tab reads (the model, preflight, alerts, CAN, logs, Systemcore), and it can
 * change things only three ways, each behind an on-screen confirmation the technician taps:
 *
 *   - a declared tunable or the auto choice, with a snapshot taken first so it can be reverted;
 *   - a patch to the robot's code, which Catalyst Link applies on a new branch on the PC — never the
 *     branch you're on, never pushed, never deployed;
 *   - a work order dropped in Catalyst Link's inbox for the PC's own agent to pick up later.
 *
 * The conversation runs on its own worker thread (hal_thread); the UI only reads snapshots of it under
 * the lock, and answers confirmations. Nothing here touches LVGL; nothing here reads the UI's robot model
 * except through assist_feed(). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cat_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where requests go — the provider:
 *   AS_ROUTE_LINK    Claude through Catalyst Link on the PC, which holds the key or runs Claude Code on
 *                    the owner's subscription and forwards the stream (docs/link-api.md);
 *   AS_ROUTE_DIRECT  Claude straight from the tablet, with an Anthropic key stored on it;
 *   AS_ROUTE_OPENAI  OpenAI's Chat Completions straight from the tablet, with an OpenAI key stored on it
 *                    (as_oai.c translates; the tools, cards and transcript are the same).
 * Stored in the kv key "ai_route" as "link", "direct" or "openai". */
typedef enum { AS_ROUTE_LINK, AS_ROUTE_DIRECT, AS_ROUTE_OPENAI } as_route_t;

typedef struct {
    as_route_t route;
    char api_key[160];     /* the Anthropic key (AS_ROUTE_DIRECT) */
    char model[48];        /* Claude's model; "" → "claude-opus-5" */
    char oai_key[256];     /* the OpenAI key (AS_ROUTE_OPENAI) */
    char oai_model[48];    /* "" → "gpt-5" (AS_OAI_DEFAULT_MODEL) */
} assist_config_t;

typedef enum {
    AS_PHASE_IDLE,         /* waiting for the technician */
    AS_PHASE_SENDING,      /* request out, nothing back yet */
    AS_PHASE_THINKING,     /* a thinking block is streaming */
    AS_PHASE_WRITING,      /* a text block is streaming */
    AS_PHASE_TOOL,         /* a tool call is streaming in or running */
    AS_PHASE_CONFIRM,      /* waiting on the technician's yes or no */
    AS_PHASE_ERROR,        /* the last turn failed; the last entry says why */
} as_phase_t;

typedef enum {
    AS_E_USER,             /* what the technician asked */
    AS_E_TEXT,             /* the assistant's answer (markdown-light: **bold**, `code`, - lists) */
    AS_E_THINKING,         /* a summary of its reasoning ("display": "summarized") */
    AS_E_TOOL,             /* a tool call: `tool` is its name, `text` a one-line summary of what it did */
    AS_E_NOTE,             /* the tablet's own note: a snapshot taken, a patch branch, a fallback */
    AS_E_ERROR,
} as_entry_kind_t;

typedef enum { AS_TOOL_RUNNING, AS_TOOL_OK, AS_TOOL_FAILED, AS_TOOL_DENIED, AS_TOOL_WAITING } as_tool_state_t;

typedef struct {
    as_entry_kind_t kind;
    const char *text;
    const char *tool;              /* AS_E_TOOL */
    as_tool_state_t tool_state;    /* AS_E_TOOL */
    uint32_t rev;                  /* bumps whenever this entry changes (its text grows while streaming) */
} as_entry_t;

/* A change the assistant wants to make, shown as a card the technician approves or declines. */
typedef struct {
    bool pending;
    char kind[24];                 /* "set_tunable", "select_auto", "revert", "propose_patch" */
    char title[64];                /* "Change a tunable" */
    char detail[1024];             /* "Elevator/kP   0.80 → 1.10\nwhy: …" or the patch's summary and diffstat */
    char reason[256];              /* the assistant's stated reason */
    double deadline;               /* hal_seconds() after which it counts as declined */
} as_confirm_t;

typedef struct {
    int input_tokens, output_tokens;   /* the conversation so far */
    char model[48];                    /* the model that served the last turn */
    bool fell_back;                    /* a server-side fallback served some turn */
} as_usage_t;

void assist_init(void);                        /* once, at boot; starts the worker */
/* The UI thread hands over the robot as of each model update (10 Hz); assist copies it under its own
 * lock, so its worker never reads the UI's model while the UI writes it. */
void assist_feed(const cat_robot_t *r);
/* The robot as last fed (for the companion's own conversation, whose tools read it too). Any thread. */
void assist_robot(cat_robot_t *out);
void assist_configure(const assist_config_t *c);
/* The configuration as it stands (keys included: never log them, never show more than "saved"). */
void assist_config(assist_config_t *out);
/* Changes one part and stores it in the kv store (UI thread only: it writes NVS). A provider change starts a
 * new conversation, since one model's history (thinking signatures, tool ids) isn't another's. key/model
 * NULL leave that part as it is; "" clears a key or restores the default model. */
void assist_use(as_route_t route);
void assist_set_anthropic(const char *key, const char *model);
void assist_set_openai(const char *key, const char *model);
/* "Claude through the PC", "Claude (key on tablet)", "OpenAI gpt-5" */
const char *assist_provider_name(as_route_t r);
/* false with a reason when it can't run: no Link and no key, Link unreachable… */
bool assist_ready(char *why, size_t n);
bool assist_send(const char *text);            /* false while a turn is in flight */
void assist_stop(void);                        /* ends the turn in flight; pending confirmation → declined */
void assist_reset(void);                       /* a new conversation */
/* A question and an answer put straight into the transcript, no model and no network: the dev console's
 * "assist fake". Scrolling a long answer is the app's most expensive frame, and it cannot be measured
 * twice the same way when every measurement needs a different answer from a model. */
void assist_fake(const char *question, const char *answer);

as_phase_t assist_phase(void);
uint32_t assist_rev(void);                     /* bumps on any change; cheap, no lock */

/* Read the transcript: lock, read, unlock. Pointers are valid until unlock. Hold it briefly. */
void assist_lock(void);
int assist_count(void);
const as_entry_t *assist_entry(int i);
/* The id of entry 0: ids only grow, so a view can tell entries that dropped off the front (a long
 * conversation, a reset) from new ones. Under the lock. */
int assist_first(void);
/* The phase for a reader that already holds the lock. assist_phase() takes that same lock and it is
 * not recursive, so asking it from inside assist_lock() would wait on itself forever. */
as_phase_t assist_phase_locked(void);
void assist_unlock(void);

bool assist_confirm_pending(as_confirm_t *out);
void assist_confirm(bool approve);
void assist_usage(as_usage_t *out);

/* Suggested openers for the current robot state ("Why is Elevator hot?"), for chips under the prompt. */
int assist_suggestions(const char **out, int max);

/* Keys dropped on the microSD card, read once at boot: /sdcard/CATOS/KEYS/OPENAI.TXT and ANTHROPIC.TXT (one
 * key each), and KEYS.ENV in CATOS/ or at the card's root (dotenv: OPENAI_API_KEY, ANTHROPIC_API_KEY,
 * TBA_API_KEY, HA_URL, HA_TOKEN, NEXUS_API_KEY, FRC_EVENTS_USER, FRC_EVENTS_TOKEN, TEAM, WIFI_SSID, WIFI_PASS;
 * docs/keys.md). Each value goes to the kv key its settings read, the Wi-Fi pair to the Wi-Fi driver; then
 * the file is overwritten and deleted so nothing lingers on the card. Values are never logged.
 * assist_import_card() runs on the UI thread (it writes NVS) before the settings are loaded, so everything
 * that reads them at start-up sees the new values. assist_import_note() hands the UI thread a one-time
 * message for the island naming what came in (never a value), or NULL; valid until the next call. */
void assist_import_card(void);
const char *assist_import_note(void);

/* The desk (as_desk.c): what the assistant reads that lives in the UI, posted as plain text whenever it changes
 * (NULL or "" clears it). The tools get_matches and get_batteries return MATCHES and BATTERIES; NOW is one line
 * for every question's context ("next match Q34 at 14:05 on red · battery #7 goes in next"). Any thread;
 * assist_desk_get's copy is the caller's to free. */
typedef enum { AS_DESK_MATCHES, AS_DESK_BATTERIES, AS_DESK_NOW, AS_DESK_INTEL, AS_DESK_N } as_desk_t;
/* AS_DESK_INTEL: the intelligence core's most important insight, one line ("noticed: Q34 queues ~14:32 ..."),
 * for every question's context after NOW (ui_intel.c posts it; "" none). */
/* The Blue Alliance, handed in rather than called: `home` requires `assist`, so the assistant
 * cannot reach tba.h. Whoever requires both (the ui) registers tba_read here at start-up; without
 * it the tba_query tool says so rather than failing obscurely. Called from the assistant's worker,
 * so the reader must be safe to block in. */
/* The tablet's and the house's own actions, handed in for the same reason as the reader below:
 * ui requires assist, so assist cannot call the ui. `action` is the tool's name, `args` its
 * validated input; `out` takes a sentence for the model either way. Called on the assistant's
 * worker -- the handler is what moves the work to the UI thread.
 *
 * `args` is the validated input re-serialised as JSON, not a parsed object: as_json.h is private to
 * this component, and a public header that leaks it would drag the parser into everyone who talks
 * to the assistant. The handler parses it with whatever it already has. `pool` says which conversation
 * is asking (AS_POOL_ASSIST or AS_POOL_COMPANION): the companion is on screen and talking, so what
 * would take the screen from her waits until she has finished. */
typedef bool (*as_action_fn)(const char *action, const char *args_json, int pool, char *out, size_t n);
void assist_set_action_handler(as_action_fn fn);
as_action_fn assist_action_handler(void);

/* What the two conversations are doing, for the island's live activities, handed in by the ui for the same
 * reason as the action handler. Any thread; each may be NULL.
 *   turn(pool, doing)   what that conversation is doing now ("thinking", "reading the alerts"); NULL when its
 *                       turn is over. The ui shows it only while that conversation's own screen is not.
 *   begin(kind, text)   a piece of work worth showing wherever the owner is, from the moment it starts:
 *                       kind "edit" (a document or the notes being written), "patch", "send" (a work order);
 *                       returns an id (> 0), or 0 when it can't be shown;
 *   update(id, text), end(id, said)   said: what to leave on the island ("saved notes.md"), or NULL. */
typedef struct {
    void (*turn)(int pool, const char *doing);
    int (*begin)(const char *kind, const char *text);
    void (*update)(int id, const char *text);
    void (*end)(int id, const char *said);
} as_activity_hooks_t;
void assist_set_activity_hooks(const as_activity_hooks_t *h);

typedef bool (*as_tba_read_fn)(const char *path, char *out, size_t n);
void assist_set_tba_reader(as_tba_read_fn fn);
/* The desk pet's line for the companion's context (home_pet_context; `home` requires `assist`, so it is handed in
 * like the reader above). Called on the companion's worker for every question; fills `out` or leaves it empty. */
typedef void (*as_pet_ctx_fn)(char *out, size_t n);
void assist_set_pet_context(as_pet_ctx_fn fn);
void assist_pet_context(char *out, size_t n);
as_tba_read_fn assist_tba_reader(void);

void assist_desk_post(as_desk_t which, const char *text);
char *assist_desk_get(as_desk_t which);

/* A model's typography folded, in place, into what the tablet's faces carry: curly quotes to straight, the
 * en dash to '-', odd spaces to ' ', a bullet to '·' (they came out as nothing at all: "wheels" for
 * "wheel’s"). The em dash and the ellipsis stay. Never lengthens the text. Any thread. */
void assist_fold_text(char *s);

/* ---- memory (as_memio.c) ----
 * Each AI keeps its own long-term memory, and neither can read the other's: the technician's (this app's
 * conversation) and the companion's (voice.h). A pool is a few "self" sections the model rewrites whole
 * (the technician's season notebook: robot, team, season, watchlist; the companion's persona: name,
 * personality, relationship, running_jokes) and up to 120 memories. The models keep them with their
 * remember / forget / recall tools; these calls are for the screens that show them and let the owner
 * delete them. On the card at CATOS/DATA/MEM-ASSIST.JSON and MEM-COMPANION.JSON; without one, the most
 * valuable part in kv. Any thread, except assist_mem_tick. */
typedef enum { AS_POOL_ASSIST, AS_POOL_COMPANION } as_pool_t;
typedef struct {
    int id;
    char kind[16];                 /* fact, preference, decision, fix, fault, procedure, event, person, plan */
    char text[480];
    int imp;                       /* 1 minor, 2 useful, 3 important */
    long long updated;             /* unix seconds, 0 unknown */
} as_memory_t;
/* The pool's memories, most valuable first (at most max); -1 while the pool can't be read. */
int assist_mem_list(as_pool_t pool, as_memory_t *out, int max);
/* Self section i (0..3): its name and its text (""). false past the last, or while the pool can't be read. */
bool assist_mem_self(as_pool_t pool, int i, char *key, size_t kn, char *text, size_t tn);
bool assist_mem_forget(as_pool_t pool, int id);
bool assist_mem_clear_self(as_pool_t pool, int i);
void assist_mem_forget_all(as_pool_t pool);          /* every memory, and the self sections */
void assist_mem_status(as_pool_t pool, char *out, size_t n); /* "37 memories of 120 · on the card, ..." or why not */
uint32_t assist_mem_gen(void);                       /* changes whenever either pool does */
void assist_mem_tick(void);                          /* UI thread, 10 Hz: finishes saves that failed, kv without a card */
/* Exactly what that pool's model is shown at the start of a conversation whose first question is `query`
 * (the dev console's "memo digest"). malloc'd; the caller frees. */
char *assist_mem_digest(as_pool_t pool, const char *query);

/* Runs fn(arg) once on the assistant's worker, between conversation turns (a turn in flight finishes first):
 * for one-shot jobs like a log analysis (analyze.h), which then need no thread of their own. false while
 * another job is queued or running. Any thread. */
bool assist_post_job(void (*fn)(void *), void *arg);
/* OpenAI's Chat Completions URL, the headers with the stored key ("oai_key") for a plain JSON answer, and the
 * model from settings ("oai_model", default gpt-5). false when no key is stored. The caller wipes hdr
 * after use. Any thread. */
bool assist_oai_endpoint(char *url, size_t un, char *hdr, size_t hn, char *model, size_t mn);

/* Points the direct route at another base URL instead of https://api.anthropic.com (tests use a local
 * fake; "" restores the default). assist_init also reads it from the kv key "ai_base". */
void assist_set_base_url(const char *url);

#ifdef __cplusplus
}
#endif
