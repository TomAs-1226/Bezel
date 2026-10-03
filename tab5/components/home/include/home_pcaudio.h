/* home_pcaudio — the PC's sound out of the tablet's speaker.
 *
 * Catalyst Link captures what the PC plays (WASAPI loopback, tab5/link/catalyst_link/audio.py) and serves it
 * as 48 kHz mono PCM on GET /audio/stream. With this switched on, a thread here connects to it (the same
 * address and token as every other Link call), keeps a jitter buffer in front of the speaker, and plays the
 * stream through hal_play.h like the music player does. It is a citizen of the speaker:
 *   - the assistant (listening, thinking, speaking) has it: the stream is read and dropped meanwhile, so it
 *     comes back live, not stale, when the assistant is done; tones and alarms mix over it;
 *   - while the PC plays nothing the speaker is released, so the wake word works in the quiet;
 *   - the card's player and this are one speaker: starting either stops the other (home_player_*).
 * It stops cleanly when the PC or the Wi-Fi goes away and reconnects by itself. Switch state is the caller's
 * to keep (hal_kv, ui_app_music.c). */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PCA_OFF,         /* switched off here */
    PCA_WAITING,     /* on, but the PC isn't there (not paired, not answering, switched off over there) */
    PCA_CONNECTING,
    PCA_QUIET,       /* connected; the PC is playing nothing */
    PCA_LIVE,        /* playing the PC's sound */
    PCA_HELD,        /* connected and the PC is playing, but the assistant has the speaker */
} pca_state_t;

typedef struct {
    bool enabled;
    pca_state_t state;
    char note[80];          /* why not, when WAITING */
    int fill_ms;            /* queued ahead of the speaker */
    unsigned underruns;     /* since this connection began */
    unsigned packets;
    int rate_kbps;          /* what is arriving, a second's average */
    unsigned gen;           /* changes with state or note */
} pca_t;

void home_pcaudio_enable(bool on);   /* from any thread; starts or ends the receiver */
void home_pcaudio_get(pca_t *out);
/* The card's player is about to take the speaker: let go of it, and stay away for a few seconds (every track
 * asks again). */
void home_pcaudio_yield(void);
/* For testing against a second Link on another port: stream from this base URL (RAM only, "" clears it). The
 * token is still the paired Link's. */
void home_pcaudio_host(const char *url);

#ifdef __cplusplus
}
#endif
