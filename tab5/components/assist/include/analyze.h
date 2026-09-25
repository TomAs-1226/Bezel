/* analyze — a robot log read by a GPT "pit engineer": the logs app's (and the recorder's) analyze button.
 *
 * The log never leaves the tablet whole. It is boiled down here to a bounded digest (cat_logs.h: headline
 * numbers, sliced series, counters and the events kept, or a run recording's per-column statistics), and the
 * digest goes to OpenAI's Chat Completions with the pit engineer's brief as the system prompt: what Catalyst
 * is and logs, how to reason about brownouts, CAN, the radio and the loop, and a fixed answer format (a
 * verdict, ranked causes with their evidence, next checks, what to look at, and what the log can't tell).
 *
 * It runs on the assistant's worker (assist_post_job): no thread of its own, after any conversation turn in
 * flight. The key and the model are the assistant's OpenAI settings ("oai_key", "oai_model"). The UI polls
 * analyze_gen() and copies the state out when it changes. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AN_IDLE,     /* nothing asked yet */
    AN_READING,  /* the log is being read and boiled down */
    AN_ASKING,   /* the digest is with the model */
    AN_DONE,     /* the answer is in */
    AN_FAILED,   /* the answer is why not */
} an_phase_t;

/* Starts an analysis of the log at `path` (.wpilog, .dslog, .dsevents, or a recorder .csv) for `team`.
 * false while one is under way, or when the assistant's worker isn't running. Any thread. */
bool analyze_start(const char *path, int team);
bool analyze_busy(void);
unsigned analyze_gen(void);              /* cheap, no lock: changes with anything below */
/* The state as it stands: the log's file name, the answer (or why it failed), what was sent, and the model
 * that answered, each malloc'd for the caller to free (NULL when there is none). Returns the phase. */
an_phase_t analyze_get(char **name, char **answer, char **digest, char *model, size_t mn);

#ifdef __cplusplus
}
#endif
