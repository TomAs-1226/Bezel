/* as_memio — the two memory pools, kept: loaded from and saved to the card, or kv without one.
 *
 *   <sd>/CATOS/DATA/MEM-ASSIST.JSON      the technician's (Assist)
 *   <sd>/CATOS/DATA/MEM-COMPANION.JSON   the companion's
 *
 * A pool loads the first time something asks for it (a conversation, the settings list): by then the
 * card has long stopped answering EIO the way it does for ~20 s after start-up, and a load that fails
 * anyway is tried again two seconds later. A pool that failed to load is never written, so a card that
 * hiccups can't have its memories replaced by an empty pool. Writes go through a .TMP beside the file
 * (a pulled card leaves the old one or the new one), from the thread that made the change; one that
 * fails is retried from the UI's 10 Hz tick. Without a card a pool lives in kv (mem_tech, mem_comp: the
 * self sections and as many of the most valuable memories as fit in 3.8 KB), written only from the UI
 * thread, since NVS must not be written from a thread whose stack may be in PSRAM.
 *
 * One lock for both pools; the tools, the conversation and the UI all take it through here. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "as_mem.h"

/* UI thread, once, at start-up (assist_init): reads the kv copies (NVS: this thread), and without a card
 * loads the pools from them. */
void as_memio_boot(void);

/* The pool, loaded if it wasn't, with the lock held: give it back with as_pool_put. NULL, and the lock
 * not held, when it can't be had right now (why says so, in words for a model). */
as_mem_t *as_pool_get(int pool, char *why, size_t wn);
/* Gives the pool back. `changed`: it is saved (now, from this thread, when it lives on the card). */
void as_pool_put(int pool, bool changed);

/* What the model is shown at the start of a conversation: its memory, framed, inside the pool's budget,
 * the memories most relevant to `query` (the first question) first. malloc'd; never NULL unless out of
 * memory (an unavailable pool says so in words). */
char *as_memio_digest(int pool, const char *query);

/* unix seconds now, or 0 while the clock isn't set */
long long as_mem_now(void);
