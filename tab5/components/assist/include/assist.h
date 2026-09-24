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
    char oai_model[48];    /* "" → "gpt-4o-mini" */
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
/* "Claude through the PC", "Claude (key on tablet)", "OpenAI gpt-4o-mini" */
const char *assist_provider_name(as_route_t r);
/* false with a reason when it can't run: no Link and no key, Link unreachable… */
bool assist_ready(char *why, size_t n);
bool assist_send(const char *text);            /* false while a turn is in flight */
void assist_stop(void);                        /* ends the turn in flight; pending confirmation → declined */
void assist_reset(void);                       /* a new conversation */

as_phase_t assist_phase(void);
uint32_t assist_rev(void);                     /* bumps on any change; cheap, no lock */

/* Read the transcript: lock, read, unlock. Pointers are valid until unlock. Hold it briefly. */
void assist_lock(void);
int assist_count(void);
const as_entry_t *assist_entry(int i);
/* The id of entry 0: ids only grow, so a view can tell entries that dropped off the front (a long
 * conversation, a reset) from new ones. Under the lock. */
int assist_first(void);
void assist_unlock(void);

bool assist_confirm_pending(as_confirm_t *out);
void assist_confirm(bool approve);
void assist_usage(as_usage_t *out);

/* Suggested openers for the current robot state ("Why is Elevator hot?"), for chips under the prompt. */
int assist_suggestions(const char **out, int max);

/* Keys dropped on the microSD card: /sdcard/CATOS/KEYS/OPENAI.TXT and ANTHROPIC.TXT, read once at boot by
 * assist_init (whitespace trimmed), stored like a typed key, then overwritten and deleted so they don't
 * linger on the card. The key is never logged. assist_import_note() hands the UI thread a one-time
 * message for the island ("OpenAI key imported"), or NULL. */
const char *assist_import_note(void);

/* Points the direct route at another base URL instead of https://api.anthropic.com (tests use a local
 * fake; "" restores the default). assist_init also reads it from the kv key "ai_base". */
void assist_set_base_url(const char *url);

#ifdef __cplusplus
}
#endif
