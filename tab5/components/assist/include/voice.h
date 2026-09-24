/* voice — the companion's own conversation, spoken and heard.
 *
 * Separate from the Assist app's technician conversation (assist.h): its own history, its own system
 * prompt (a friendly desk companion that is also a capable general assistant), its own worker. It still
 * reads the robot, the code and Claude Code's sessions through the assistant's read-only tools when asked.
 *
 *   wake word ("Hi ESP", on the tablet) or a tap → record until a pause (energy VAD) → OpenAI transcription
 *   (multipart upload; gpt-4o-mini-transcribe, whisper-1 when refused) → Chat Completions with a JSON
 *   schema: {"say","emotion","intensity","look"} → OpenAI speech (gpt-4o-mini-tts, 24 kHz PCM) streamed to
 *   the speaker as it arrives.
 *
 * The microphones are on only while voice_enable(true) (desk mode on screen) — and, without a wake word,
 * only while it records after a tap. Everything that blocks runs on the worker; the UI reads state and the
 * last reply. Needs an OpenAI key (the assistant's, from settings) and the internet. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VO_OFF,        /* not on screen: the microphones are off */
    VO_IDLE,       /* on screen, waiting: for the wake word when there is one, else for a tap */
    VO_LISTEN,     /* recording what's said, until a pause */
    VO_HEARING,    /* the recording is being transcribed */
    VO_THINKING,   /* the model is answering (tool calls included) */
    VO_SPEAKING,   /* the answer is playing */
} vo_state_t;

typedef enum {
    VO_EMO_NEUTRAL, VO_EMO_HAPPY, VO_EMO_EXCITED, VO_EMO_CURIOUS, VO_EMO_THINKING, VO_EMO_SLEEPY,
    VO_EMO_SAD, VO_EMO_WORRIED, VO_EMO_SURPRISED, VO_EMO_LOVE, VO_EMO_PROUD, VO_EMO_CONFUSED,
    VO_EMO_COUNT
} vo_emotion_t;

typedef enum { VO_LOOK_CENTER, VO_LOOK_LEFT, VO_LOOK_RIGHT, VO_LOOK_UP, VO_LOOK_DOWN } vo_look_t;

typedef enum { VO_OUT_BOTH, VO_OUT_SPEAK, VO_OUT_SHOW } vo_out_t;

typedef struct {
    uint32_t seq;          /* bumps with each answer (0: none yet) */
    char heard[256];       /* what was asked, transcribed or typed */
    char say[1200];        /* the answer */
    vo_emotion_t emotion;
    float intensity;       /* 0..1 */
    vo_look_t look;
    bool spoken;           /* it goes to the speaker */
} vo_reply_t;

typedef struct {
    vo_out_t out;          /* speak, show, or both ("v_out": "both", "speak", "show") */
    bool wake;             /* listen for the wake word while on screen ("v_wake") */
    bool follow;           /* after an answer, listen a few seconds for a follow-up, no wake word ("v_follow") */
    char chat_model[48];   /* "" → the assistant's OpenAI model, else gpt-4o-mini ("v_chat") */
    char stt_model[48];    /* "" → gpt-4o-mini-transcribe ("v_stt") */
    char tts_model[48];    /* "" → gpt-4o-mini-tts ("v_tts") */
    char voice[24];        /* "" → coral ("v_voice") */
    char lang[8];          /* ISO 639-1 hint for transcription, "" → detect ("v_lang") */
} voice_config_t;

#define VO_STT_DEFAULT "gpt-4o-mini-transcribe"
#define VO_STT_FALLBACK "whisper-1"
#define VO_TTS_DEFAULT "gpt-4o-mini-tts"
#define VO_TTS_FALLBACK "tts-1"
#define VO_VOICE_DEFAULT "coral"

void voice_init(void);                         /* once, after assist_init: starts the worker */
void voice_config(voice_config_t *out);
void voice_set_config(const voice_config_t *c); /* UI thread only: writes the kv store */

/* Desk mode came on screen (true) or left it (false: anything under way stops, the microphones go off). */
void voice_enable(bool on);
/* An OpenAI key and the internet: false with a reason. */
bool voice_ready(char *why, size_t n);
/* The wake phrase ("Hi ESP") once the model has loaded; NULL while loading or when there is none. */
const char *voice_wake_word(void);

void voice_listen(void);                       /* tap to talk: record now */
bool voice_ask(const char *text);              /* a typed question into the same conversation */
void voice_cancel(void);                       /* stops whatever is under way (recording, answer, speech) */
void voice_reset(void);                        /* a new conversation */

vo_state_t voice_state(void);
uint32_t voice_rev(void);                      /* bumps on any change the UI shows */
float voice_mic_level(void);                   /* 0..1 while listening */
float voice_speak_level(void);                 /* 0..1: the speaker's loudness now, for the face */
bool voice_reply(vo_reply_t *out);             /* the last answer; false when there is none */
/* A short note for the status line ("didn't catch that", an error) and when it was set (hal_seconds). */
double voice_note(char *out, size_t n);

const char *voice_emotion_name(vo_emotion_t e);

#ifdef __cplusplus
}
#endif
