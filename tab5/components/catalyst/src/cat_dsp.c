#include "cat_dsp.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = (float)(-2 * M_PI / len);
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

void cat_spectrum(const int16_t *s, int n, int rate, float min_hz, cat_spectrum_t *o)
{
    static float re[CAT_FFT_MAX], im[CAT_FFT_MAX];
    if (n > CAT_FFT_MAX) n = CAT_FFT_MAX;
    o->n = n;
    o->rate = rate;
    double energy = 0;
    for (int i = 0; i < n; i++) {
        float w = 0.5f - 0.5f * cosf((float)(2 * M_PI * i / (n - 1)));
        float x = s[i] / 32768.0f;
        energy += (double)x * x;
        re[i] = x * w;
        im[i] = 0;
    }
    o->rms_db = (float)(10 * log10(energy / n + 1e-12));
    fft(re, im, n);
    int half = n / 2, lo = (int)(min_hz * n / rate), best = lo > 1 ? lo : 1;
    for (int k = 0; k < half; k++) {
        /* ×4/n: Hann's coherent gain (0.5) and the one-sided spectrum, so a full-scale sine reads 0 dB */
        float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * 4.0f / n;
        o->db[k] = 20 * log10f(mag + 1e-9f);
        if (k >= lo && k > 0 && o->db[k] > o->db[best]) best = k;
    }
    float d = 0;
    if (best > 0 && best < half - 1) {
        float a = o->db[best - 1], b = o->db[best], c = o->db[best + 1];
        float den = a - 2 * b + c;
        if (fabsf(den) > 1e-6f) d = 0.5f * (a - c) / den;
    }
    o->peak_hz = (best + d) * rate / (float)n;
    o->peak_db = o->db[best];
}
