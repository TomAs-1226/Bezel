/* Weak stand-ins for hal_play.h until tab_hal's audio provides the real ones: no output, said plainly. */
#include "hal_play.h"

__attribute__((weak)) bool hal_play_open(int sample_rate, int channels)
{
    (void)sample_rate;
    (void)channels;
    return false;
}

__attribute__((weak)) int hal_play_write(const int16_t *pcm, int frames, int timeout_ms)
{
    (void)pcm;
    (void)frames;
    (void)timeout_ms;
    return -1;
}

__attribute__((weak)) void hal_play_close(void) {}
