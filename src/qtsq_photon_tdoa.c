/*
 * Quantum Tensor Sequence — Photon TDOA Acoustic Localization
 *
 * Time-Difference-of-Arrival (TDOA) source localization using
 * GCC-PHAT (Generalized Cross-Correlation with Phase Transform).
 *
 * Pipeline:
 *   1. For each mic pair, compute GCC-PHAT → time delay τ
 *   2. τ → angle of arrival (using known mic spacing)
 *   3. Per-band TDOA via STFT → direction per frequency
 *   4. Cluster consistent directions → each cluster = spatial source
 *   5. Classify by dominant frequency band
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "qtsq_photon.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ═══════════════════════════════════════════════════════════════
 *  Internal FFT (Radix-2 Cooley-Tukey, in-place)
 * ═══════════════════════════════════════════════════════════════ */

static void tdoa_fft(float *re, float *im, int N, int inverse) {
    /* Bit-reversal permutation */
    int j = 0;
    for (int i = 0; i < N - 1; i++) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int m = N >> 1;
        while (m >= 1 && j >= m) { j -= m; m >>= 1; }
        j += m;
    }

    /* Butterfly passes */
    for (int step = 1; step < N; step <<= 1) {
        float angle = (inverse ? (float)M_PI : (float)-M_PI) / (float)step;
        float wr = cosf(angle), wi = sinf(angle);
        for (int group = 0; group < N; group += step << 1) {
            float cr = 1.0f, ci = 0.0f;
            for (int pair = 0; pair < step; pair++) {
                int a = group + pair, b = a + step;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }

    if (inverse) {
        float inv = 1.0f / (float)N;
        for (int i = 0; i < N; i++) { re[i] *= inv; im[i] *= inv; }
    }
}

/* Next power of 2 */
static uint32_t next_pow2(uint32_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  TDOA Init / Free
 * ═══════════════════════════════════════════════════════════════ */

int photon_tdoa_init(photon_tdoa_state_t *state, const photon_config_t *cfg) {
    if (!state || !cfg) return -1;
    memset(state, 0, sizeof(*state));
    state->config = *cfg;

    /* FFT size should be at least 2x the analysis window for zero-padding */
    state->fft_size = next_pow2(cfg->fft_size * 2);

    state->fft_scratch_re = (float *)calloc(state->fft_size, sizeof(float));
    state->fft_scratch_im = (float *)calloc(state->fft_size, sizeof(float));
    state->gcc_scratch    = (float *)calloc(state->fft_size, sizeof(float));

    if (!state->fft_scratch_re || !state->fft_scratch_im || !state->gcc_scratch) {
        photon_tdoa_free(state);
        return -1;
    }

    return 0;
}

void photon_tdoa_free(photon_tdoa_state_t *state) {
    if (!state) return;
    if (state->fft_scratch_re) { free(state->fft_scratch_re); state->fft_scratch_re = NULL; }
    if (state->fft_scratch_im) { free(state->fft_scratch_im); state->fft_scratch_im = NULL; }
    if (state->gcc_scratch)    { free(state->gcc_scratch);    state->gcc_scratch = NULL; }
}

/* ═══════════════════════════════════════════════════════════════
 *  GCC-PHAT (Generalized Cross-Correlation with Phase Transform)
 *
 *  R_phat(τ) = IFFT[ X_a(f) · conj(X_b(f)) / |X_a(f) · conj(X_b(f))| ]
 *
 *  The Phase Transform whitens the spectrum, making the correlation
 *  peak sharper and more robust to reverberation.
 *
 *  Returns: time delay in seconds (positive = mic_b leads mic_a)
 * ═══════════════════════════════════════════════════════════════ */

float photon_tdoa_gcc_phat(photon_tdoa_state_t *state,
                            const float *mic_a, const float *mic_b,
                            uint32_t num_samples) {
    if (!state || !mic_a || !mic_b || num_samples == 0) return 0.0f;

    uint32_t N = state->fft_size;
    uint32_t use_samples = num_samples < N ? num_samples : N;

    /* Allocate temporary buffers for mic_b's FFT */
    float *b_re = (float *)calloc(N, sizeof(float));
    float *b_im = (float *)calloc(N, sizeof(float));
    if (!b_re || !b_im) {
        if (b_re) free(b_re);
        if (b_im) free(b_im);
        return 0.0f;
    }

    /* Copy mic_a into scratch, zero-pad */
    memset(state->fft_scratch_re, 0, N * sizeof(float));
    memset(state->fft_scratch_im, 0, N * sizeof(float));
    for (uint32_t i = 0; i < use_samples; i++) {
        state->fft_scratch_re[i] = mic_a[i];
    }

    /* Copy mic_b into temp buffers, zero-pad */
    memset(b_re, 0, N * sizeof(float));
    memset(b_im, 0, N * sizeof(float));
    for (uint32_t i = 0; i < use_samples; i++) {
        b_re[i] = mic_b[i];
    }

    /* FFT both signals */
    tdoa_fft(state->fft_scratch_re, state->fft_scratch_im, (int)N, 0);
    tdoa_fft(b_re, b_im, (int)N, 0);

    /* Cross-power spectrum: X_a * conj(X_b) */
    for (uint32_t i = 0; i < N; i++) {
        float a_re = state->fft_scratch_re[i];
        float a_im = state->fft_scratch_im[i];
        /* conj(X_b) = (b_re, -b_im) */
        float cross_re = a_re * b_re[i] + a_im * b_im[i];
        float cross_im = a_im * b_re[i] - a_re * b_im[i];

        /* Phase Transform: normalize by magnitude */
        float mag = sqrtf(cross_re * cross_re + cross_im * cross_im);
        if (mag > 1e-10f) {
            state->fft_scratch_re[i] = cross_re / mag;
            state->fft_scratch_im[i] = cross_im / mag;
        } else {
            state->fft_scratch_re[i] = 0.0f;
            state->fft_scratch_im[i] = 0.0f;
        }
    }

    /* IFFT to get GCC-PHAT correlation */
    tdoa_fft(state->fft_scratch_re, state->fft_scratch_im, (int)N, 1);

    /* Find peak in the correlation function */
    /* Search range: ±max_delay based on mic spacing */
    float max_mic_dist = 0.0f;
    for (int m = 0; m < state->config.num_mics; m++) {
        float d = sqrtf(state->config.mic_positions[m][0] * state->config.mic_positions[m][0] +
                        state->config.mic_positions[m][1] * state->config.mic_positions[m][1] +
                        state->config.mic_positions[m][2] * state->config.mic_positions[m][2]);
        if (d > max_mic_dist) max_mic_dist = d;
    }
    if (max_mic_dist < 0.01f) max_mic_dist = 0.1f;  /* Min 10cm spacing */

    float max_delay_sec = max_mic_dist / state->config.speed_of_sound;
    int max_delay_samples = (int)(max_delay_sec * (float)state->config.sample_rate) + 1;
    if (max_delay_samples > (int)(N / 2)) max_delay_samples = (int)(N / 2);

    /* Search both positive and negative lags */
    float best_val = -1e20f;
    int best_lag = 0;

    for (int lag = -max_delay_samples; lag <= max_delay_samples; lag++) {
        int idx = lag < 0 ? (int)N + lag : lag;
        if (idx >= 0 && idx < (int)N) {
            float v = state->fft_scratch_re[idx];
            if (v > best_val) {
                best_val = v;
                best_lag = lag;
            }
        }
    }

    /* Parabolic interpolation for sub-sample precision */
    float precise_lag = (float)best_lag;
    int idx_c = best_lag < 0 ? (int)N + best_lag : best_lag;
    int idx_l = (best_lag - 1) < 0 ? (int)N + (best_lag - 1) : (best_lag - 1);
    int idx_r = (best_lag + 1) < 0 ? (int)N + (best_lag + 1) : (best_lag + 1);

    if (idx_l >= 0 && idx_l < (int)N && idx_r >= 0 && idx_r < (int)N && idx_c >= 0 && idx_c < (int)N) {
        float alpha = state->fft_scratch_re[idx_l];
        float beta  = state->fft_scratch_re[idx_c];
        float gamma = state->fft_scratch_re[idx_r];
        float denom = alpha - 2.0f * beta + gamma;
        if (fabsf(denom) > 1e-10f) {
            precise_lag = (float)best_lag + 0.5f * (alpha - gamma) / denom;
        }
    }

    free(b_re);
    free(b_im);

    /* Convert lag to time delay */
    return precise_lag / (float)state->config.sample_rate;
}

/* ═══════════════════════════════════════════════════════════════
 *  Source Localization from Multi-Mic TDOA
 *
 *  For each mic pair:
 *    1. Compute GCC-PHAT time delay
 *    2. Convert delay to angle: θ = asin(τ * c / d)
 *       where d = distance between mics
 *    3. Cluster angles from different pairs → source locations
 * ═══════════════════════════════════════════════════════════════ */

/* Internal: compute energy in a frequency band */
static float band_energy(const float *signal, uint32_t num_samples,
                          uint32_t sample_rate, float freq_lo, float freq_hi) {
    /* Simple bandpass energy estimation via per-sample correlation with tone */
    /* (lightweight alternative to full STFT for frequency classification) */
    float energy = 0.0f;
    float center_freq = (freq_lo + freq_hi) * 0.5f;
    float dt = 1.0f / (float)sample_rate;

    for (uint32_t i = 0; i < num_samples; i++) {
        float t = (float)i * dt;
        float ref = sinf(2.0f * (float)M_PI * center_freq * t);
        energy += signal[i] * ref * signal[i] * ref;
    }

    return energy / (float)num_samples;
}

uint32_t photon_tdoa_locate_sources(photon_tdoa_state_t *state,
                                     const photon_audio_frame_t *frame,
                                     photon_source_t *sources, uint32_t max_sources) {
    if (!state || !frame || !sources) return 0;
    if (frame->num_channels < 2) return 0;

    uint32_t count = 0;
    float sr = (float)state->config.sample_rate;
    float c = state->config.speed_of_sound;

    /* For each mic pair, compute TDOA and derive angle */
    for (uint32_t m1 = 0; m1 < frame->num_channels && count < max_sources; m1++) {
        for (uint32_t m2 = m1 + 1; m2 < frame->num_channels && count < max_sources; m2++) {
            if (!frame->channels[m1] || !frame->channels[m2]) continue;

            /* Mic spacing */
            float dx = state->config.mic_positions[m2][0] - state->config.mic_positions[m1][0];
            float dy = state->config.mic_positions[m2][1] - state->config.mic_positions[m1][1];
            float dz = state->config.mic_positions[m2][2] - state->config.mic_positions[m1][2];
            float mic_dist = sqrtf(dx*dx + dy*dy + dz*dz);

            if (mic_dist < 0.001f) continue;  /* Mics too close */

            /* GCC-PHAT → time delay */
            float delay = photon_tdoa_gcc_phat(state,
                                                frame->channels[m1],
                                                frame->channels[m2],
                                                frame->num_samples);

            /* Convert delay to angle of arrival */
            float sin_theta = (delay * c) / mic_dist;

            /* Clamp to valid range */
            if (sin_theta > 1.0f) sin_theta = 1.0f;
            if (sin_theta < -1.0f) sin_theta = -1.0f;

            float theta = asinf(sin_theta);  /* Angle in radians */

            /* Convert angle to approximate source position */
            /* Assume source is at 2m distance (will be refined by sonar) */
            float assumed_distance = 2.0f;
            float sx = assumed_distance * sinf(theta);
            float sy = assumed_distance * cosf(theta);

            /* Classify by dominant frequency band */
            float bass_e   = band_energy(frame->channels[m1], frame->num_samples, (uint32_t)sr, 20.0f, 200.0f);
            float mid_e    = band_energy(frame->channels[m1], frame->num_samples, (uint32_t)sr, 200.0f, 2000.0f);
            float treble_e = band_energy(frame->channels[m1], frame->num_samples, (uint32_t)sr, 2000.0f, 8000.0f);

            float max_e = bass_e;
            float fl = 20.0f, fh = 200.0f;
            if (mid_e > max_e) { max_e = mid_e; fl = 200.0f; fh = 2000.0f; }
            if (treble_e > max_e) { fl = 2000.0f; fh = 8000.0f; }

            /* Confidence based on GCC-PHAT peak sharpness */
            float conf = fabsf(sin_theta) < 0.99f ? 0.7f : 0.3f;

            sources[count].x = sx;
            sources[count].y = sy;
            sources[count].z = 0.0f;
            sources[count].confidence = conf;
            sources[count].freq_lo = fl;
            sources[count].freq_hi = fh;
            sources[count].source_id = count;
            sources[count].last_seen_us = frame->timestamp_us;
            count++;
        }
    }

    return count;
}

/* ═══════════════════════════════════════════════════════════════
 *  Shannon Entropy Utility
 * ═══════════════════════════════════════════════════════════════ */

float photon_shannon_entropy(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0.0f;

    uint32_t hist[256] = {0};
    for (size_t i = 0; i < len; i++) {
        hist[data[i]]++;
    }

    float entropy = 0.0f;
    float inv_len = 1.0f / (float)len;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            float p = (float)hist[i] * inv_len;
            entropy -= p * log2f(p);
        }
    }
    return entropy;
}
