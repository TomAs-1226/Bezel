/* hal_play — PCM out through the speaker, for the music player (home_player.c).
 *
 * Declared here, implemented by tab_hal's audio (the ES8388 codec and its I2S), which the voice work owns.
 * Until that lands, components/home/src/hal_play_weak.c provides weak stubs: hal_play_open() answers false
 * and the player says "no audio output in this build" instead of playing. A strong definition anywhere in
 * the firmware replaces them (put it in a source file the link already pulls in, e.g. hal_tab5.c: an
 * archive member that only these symbols would pull is never pulled, because the weak ones satisfy them).
 *
 * The contract:
 *   - One stream at a time. The caller is a worker thread (never the UI thread); every call may block.
 *   - PCM is signed 16-bit little-endian, interleaved; channels 1 or 2; any rate the files carry
 *     (8000..48000 Hz: the implementation resamples, or reclocks the codec, as it prefers).
 *   - hal_play_write() blocks until the frames are queued (the codec's DMA is the clock), or timeout_ms.
 *   - Not writing is silence: the output must not repeat its last buffer when fed nothing (a paused
 *     player simply stops writing; I2S auto_clear, or equivalent).
 *   - The level is the tablet's volume (hal_set_volume), the same as tones.
 *   - hal_play_close() stops at once (queued audio is dropped) and gives the speaker back to tones and
 *     the assistant. hal_play_open() while the speaker is taken by something that can't share it (the
 *     assistant speaking) answers false. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the speaker for a stream. false: no output (no driver in this build, or busy). */
bool hal_play_open(int sample_rate, int channels);
/* Queues `frames` frames (frames × channels samples). Returns the frames taken (fewer on timeout), or < 0
 * when the stream has broken (the codec went away): the caller then closes. */
int hal_play_write(const int16_t *pcm, int frames, int timeout_ms);
/* Stops and releases the speaker. Safe to call when nothing is open. */
void hal_play_close(void);

#ifdef __cplusplus
}
#endif
