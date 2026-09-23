/* cat_dsp — the spectrum behind the Listen tool.
 *
 * A mechanism's sound says how fast it turns: a gear mesh sings at (shaft rev/s × teeth), a belt at its
 * tooth pass rate, a failing bearing adds sidebands. This takes a block of microphone samples, windows
 * it (Hann), runs a radix-2 FFT and reports the magnitude spectrum in dB and the dominant peak, refined
 * between bins by parabolic interpolation. N is a power of two up to 2048. */
#pragma once
#include <stdint.h>

#define CAT_FFT_MAX 2048

typedef struct {
    int n, rate;
    float db[CAT_FFT_MAX / 2];   /* magnitude per bin, dBFS */
    float peak_hz, peak_db;
    float rms_db;
} cat_spectrum_t;

/* `samples` holds n values; `min_hz` ignores rumble below it when looking for the peak. */
void cat_spectrum(const int16_t *samples, int n, int rate, float min_hz, cat_spectrum_t *out);
