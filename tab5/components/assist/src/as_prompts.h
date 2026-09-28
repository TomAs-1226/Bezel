/* as_prompts — what the two AIs are told, and the companion's voice (pure: host-tested).
 *
 * Two system prompts, written to work on Claude and on GPT-5-class models alike (plain sections, rules as
 * short bullets, no model-specific syntax): the technician's for the Assist app (assist.c), the
 * companion's for the voice (voice.c). And the rules for which OpenAI speech voice to ask for: the
 * companion speaks with a young woman's voice, whatever model will take the request. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

extern const char AS_PROMPT_TECH[];       /* the Assist app's technician */
extern const char AS_PROMPT_COMPANION[];  /* the companion; its reply is the JSON object voice.c asks for */

/* The companion's default voice (OpenAI's newest female voice; gpt-4o-mini-tts takes it, tts-1 does not). */
#define AS_TTS_VOICE_DEFAULT "marin"
/* When a speech model refuses the voice asked for: a female voice every OpenAI speech model has. */
#define AS_TTS_VOICE_SAFE "coral"

/* The voice to ask `model` for, given the one wanted ("" or NULL: the default): the wanted voice when the
 * model has it, else the nearest female one it does have (tts-1 and tts-1-hd have neither marin nor cedar). */
const char *as_tts_voice(const char *model, const char *wanted);
/* The model accepts this voice at all (as OpenAI lists them). */
bool as_tts_voice_known(const char *model, const char *voice);
/* The speech `instructions` for gpt-4o-mini-tts: who is speaking, and the feeling of this line. `tone` is
 * a few words ("cheerful and warm"); `strong` says the feeling clearly. */
void as_tts_instructions(char *out, size_t n, const char *tone, bool strong);
