/* as_tools — what the assistant can do: the tool definitions sent to the API (as_tooldefs.c, pure) and
 * their execution on the tablet (as_tools.c).
 *
 * Reading is free. Changing anything goes through the technician: set_tunable, select_auto,
 * revert_snapshot and propose_patch each block on an on-screen confirmation, and a tunable write is
 * preceded by a snapshot of every tunable. create_work_order sends without asking (it changes nothing
 * on the robot or in the code) and waits in the outbox if the PC is away. Nothing commands the robot. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "as_json.h"
#include "cat_model.h"

/* AS_T_DEVICE changes the TABLET or the HOUSE -- a light, the volume, which app is on screen --
 * and asks nobody first. AS_T_CHANGE changes the ROBOT or its code and always shows a card. The
 * line between them is the robot: a lamp turned on by voice that first demanded a tap on a screen
 * would be a worse assistant, and a tunable written without one would be a dangerous robot. */
/* AS_T_MEMORY reads and writes the calling conversation's OWN memory pool (as_memio.h), and nothing
 * else: which pool is decided by the conversation (as_env_t.pool), never by anything the model says.
 * AS_T_SELF_TECH and AS_T_SELF_COMP are the same, for one conversation only: the technician's season
 * notebook, the companion's persona. AS_T_CHANGE and AS_T_SEND are the technician's alone too. */
/* AS_T_PET is the companion's alone: the desk pet (home_pet.h) lives with her, and the technician never sees it. */
typedef enum { AS_T_READ, AS_T_CHANGE, AS_T_SEND, AS_T_DEVICE, AS_T_MEMORY, AS_T_SELF_TECH, AS_T_SELF_COMP, AS_T_PET } as_tkind_t;

typedef struct {
    const char *name;
    const char *description;
    const char *schema;    /* JSON Schema for the input */
    as_tkind_t kind;
} as_tooldef_t;

extern const as_tooldef_t AS_TOOLS[];
extern const int AS_NTOOLS;

const as_tooldef_t *as_tool_find(const char *name);
/* The tool is offered to (and may be run by) the conversation with this memory pool (AS_MEM_*). */
bool as_tool_for(const as_tooldef_t *t, int pool);
/* The technician's tools array for the Messages API; every tool streams its input eagerly. */
void as_tools_json(ab_t *out);
/* Checks a parsed input against the tool's schema. */
bool as_tool_check(const as_tooldef_t *t, const aj_t *input, char *err, size_t n);

/* ---- execution (as_tools.c) ---- */

typedef enum { AS_TR_OK, AS_TR_ERROR, AS_TR_DENIED } as_tres_t;

typedef struct {
    cat_robot_t *robot;                    /* the worker's own copy */
    void (*refresh)(cat_robot_t *r);       /* copies the robot as last fed */
    /* Shows a confirmation card and blocks until the technician answers (false: declined or timed out). */
    bool (*confirm)(void *user, const char *kind, const char *title, const char *detail, const char *reason);
    void (*note)(void *user, const char *text);
    bool (*stopped)(void *user);           /* the turn was stopped: give up early */
    void *user;
    int team;                              /* settings' team number, for preflight */
    int pool;                              /* whose memory the memory tools touch (AS_MEM_*; 0 the technician) */
    int act;                               /* an island activity already begun for this call (0 none): the
                                              technician's writes start one while their input streams in */
} as_env_t;

/* ---- the island (as_desk.c; no-ops until the ui registers its hooks, assist.h) ---- */
void as_act_turn(int pool, const char *doing);
int as_act_begin(const char *kind, const char *text);
void as_act_update(int id, const char *text);
void as_act_end(int id, const char *said);
/* What a call is doing, in a few words for the island ("reading the alerts"). Never NULL. */
const char *as_tool_label(const char *name);
/* A call that writes something the owner will want to know is done (a document, the notes, a patch, a work
 * order): its activity's kind ("edit", "patch", "send"), or NULL. `in` may be NULL or partial. */
const char *as_tool_work(const char *name, const aj_t *in);
/* The words for a write's activity while it runs ("editing pit-notes"), from its (possibly partial) input. */
void as_tool_work_text(const char *name, const aj_t *in, char *out, size_t n);

/* Runs one validated call. `out` receives the result text (JSON, ≤ ~8 KB); `summary` a one-liner for
 * the transcript. */
as_tres_t as_tool_run(as_env_t *env, const char *name, const aj_t *in, ab_t *out, char *summary, size_t sn);

#define AS_RESULT_MAX 8000
