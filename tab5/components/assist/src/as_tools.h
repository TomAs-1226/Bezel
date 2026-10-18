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

typedef enum { AS_T_READ, AS_T_CHANGE, AS_T_SEND } as_tkind_t;

typedef struct {
    const char *name;
    const char *description;
    const char *schema;    /* JSON Schema for the input */
    as_tkind_t kind;
} as_tooldef_t;

extern const as_tooldef_t AS_TOOLS[];
extern const int AS_NTOOLS;

const as_tooldef_t *as_tool_find(const char *name);
/* The tools array for the request; every tool streams its input eagerly. */
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
} as_env_t;

/* Runs one validated call. `out` receives the result text (JSON, ≤ ~8 KB); `summary` a one-liner for
 * the transcript. */
as_tres_t as_tool_run(as_env_t *env, const char *name, const aj_t *in, ab_t *out, char *summary, size_t sn);

#define AS_RESULT_MAX 8000
