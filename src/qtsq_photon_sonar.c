/*
 * Quantum Tensor Sequence — Photon Ultrasonic Sonar Engine
 *
 * FMCW (Frequency-Modulated Continuous Wave) sonar using ultrasonic
 * chirps emitted through the device speaker and captured by microphones.
 *
 * Pipeline:
 *   1. Generate linear FM chirp (18–22 kHz, 2ms)
 *   2. Emit through speaker
 *   3. Capture echo via mic
 *   4. Matched filter (cross-correlate with reference)
 *   5. Peak detection → time-of-flight → distance
 *   6. Subtract chirp from recording to preserve music
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
 *  Default Configuration
 * ═══════════════════════════════════════════════════════════════ */

void photon_config_default(photon_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate      = PHOTON_DEFAULT_SAMPLE_RATE;
    cfg->fft_size         = PHOTON_DEFAULT_FFT_SIZE;
    cfg->hop_size         = PHOTON_DEFAULT_HOP_SIZE;
    cfg->sonar_freq_lo    = PHOTON_DEFAULT_SONAR_LO;
    cfg->sonar_freq_hi    = PHOTON_DEFAULT_SONAR_HI;
    cfg->sonar_chirp_ms   = PHOTON_DEFAULT_CHIRP_MS;
    cfg->sonar_interval_ms = PHOTON_DEFAULT_SONAR_INTERVAL;
    cfg->speed_of_sound   = PHOTON_DEFAULT_SPEED_OF_SOUND;
    cfg->num_mics         = 2;

    /* Default stereo mic layout (MacBook-like ~6cm apart) */
    cfg->mic_positions[0][0] = -0.03f; /* Left mic */
    cfg->mic_positions[0][1] =  0.0f;
    cfg->mic_positions[0][2] =  0.0f;
    cfg->mic_positions[1][0] =  0.03f; /* Right mic */
    cfg->mic_positions[1][1] =  0.0f;
    cfg->mic_positions[1][2] =  0.0f;
}

/* ═══════════════════════════════════════════════════════════════
 *  Sonar Init / Free
 * ═══════════════════════════════════════════════════════════════ */

int photon_sonar_init(photon_sonar_state_t *state, const photon_config_t *cfg) {
    if (!state || !cfg) return -1;
    memset(state, 0, sizeof(*state));
    state->config = *cfg;

    /* Calculate chirp length in samples */
    state->chirp_len = (cfg->sample_rate * cfg->sonar_chirp_ms) / 1000;
    if (state->chirp_len < 16) state->chirp_len = 16;

    state->chirp_ref = (float *)calloc(state->chirp_len, sizeof(float));
    if (!state->chirp_ref) return -1;

    /* Correlation buffer (input + chirp - 1) needs space during detect */
    state->correlation_len = 0;
    state->correlation_buf = NULL;

    /* Generate the reference chirp */
    photon_sonar_generate_chirp(state);

    return 0;
}

void photon_sonar_free(photon_sonar_state_t *state) {
    if (!state) return;
    if (state->chirp_ref) { free(state->chirp_ref); state->chirp_ref = NULL; }
    if (state->correlation_buf) { free(state->correlation_buf); state->correlation_buf = NULL; }
}

/* ═══════════════════════════════════════════════════════════════
 *  Chirp Generation (Linear FM Sweep)
 *
 *  s(t) = A * sin(2π * (f0*t + (f1-f0)/(2T) * t²)) * window(t)
 *
 *  f0 = sonar_freq_lo (18 kHz)
 *  f1 = sonar_freq_hi (22 kHz)
 *  T  = chirp duration
 * ═══════════════════════════════════════════════════════════════ */

void photon_sonar_generate_chirp(photon_sonar_state_t *state) {
    if (!state || !state->chirp_ref) return;

    float f0 = (float)state->config.sonar_freq_lo;
    float f1 = (float)state->config.sonar_freq_hi;
    float sr = (float)state->config.sample_rate;
    uint32_t N = state->chirp_len;
    float T = (float)N / sr;  /* Duration in seconds */
    float sweep_rate = (f1 - f0) / (2.0f * T);

    for (uint32_t i = 0; i < N; i++) {
        float t = (float)i / sr;

        /* Linear FM sweep: instantaneous frequency = f0 + (f1-f0)/T * t */
        float phase = 2.0f * (float)M_PI * (f0 * t + sweep_rate * t * t);

        /* Hann window to smooth edges and reduce spectral leakage */
        float window = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1)));

        state->chirp_ref[i] = window * sinf(phase);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Matched Filter (Cross-Correlation)
 *
 *  For each lag τ, compute:
 *    R(τ) = Σ mic_data[n] * chirp_ref[n - τ]
 *
 *  Peaks in R(τ) correspond to echo arrival times.
 *  distance = speed_of_sound * τ / (2 * sample_rate)
 * ═══════════════════════════════════════════════════════════════ */

static float cross_correlate_at_lag(const float *signal, uint32_t sig_len,
                                     const float *ref, uint32_t ref_len,
                                     int lag) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < ref_len; i++) {
        int idx = (int)i + lag;
        if (idx >= 0 && idx < (int)sig_len) {
            sum += signal[idx] * ref[i];
        }
    }
    return sum;
}

uint32_t photon_sonar_detect_echoes(photon_sonar_state_t *state,
                                     const float *mic_data, uint32_t num_samples,
                                     photon_source_t *sources, uint32_t max_sources) {
    if (!state || !mic_data || !sources || !state->chirp_ref) return 0;

    uint32_t chirp_len = state->chirp_len;
    float sr = (float)state->config.sample_rate;
    float c = state->config.speed_of_sound;

    /* Cross-correlation over all valid lags */
    int max_lag = (int)num_samples - (int)chirp_len;
    if (max_lag <= 0) return 0;

    /* Allocate correlation buffer if needed */
    if (state->correlation_len < (uint32_t)max_lag) {
        if (state->correlation_buf) free(state->correlation_buf);
        state->correlation_buf = (float *)malloc(max_lag * sizeof(float));
        state->correlation_len = max_lag;
    }
    if (!state->correlation_buf) return 0;

    /* Compute correlation at each lag */
    float max_corr = 0.0f;
    for (int lag = 0; lag < max_lag; lag++) {
        state->correlation_buf[lag] = cross_correlate_at_lag(
            mic_data, num_samples,
            state->chirp_ref, chirp_len, lag
        );
        float abs_corr = fabsf(state->correlation_buf[lag]);
        if (abs_corr > max_corr) max_corr = abs_corr;
    }

    if (max_corr < 1e-10f) return 0;

    /* Normalize correlation */
    float inv_max = 1.0f / max_corr;
    for (int i = 0; i < max_lag; i++) {
        state->correlation_buf[i] *= inv_max;
    }

    /* Peak detection: find local maxima above threshold */
    float threshold = 0.3f;  /* 30% of peak correlation */
    uint32_t count = 0;

    /* Skip the direct path (first ~2ms = chirp_len samples) */
    int min_lag = (int)chirp_len + 10;

    for (int lag = min_lag; lag < max_lag - 1 && count < max_sources; lag++) {
        float v = state->correlation_buf[lag];
        if (v > threshold &&
            v > state->correlation_buf[lag - 1] &&
            v > state->correlation_buf[lag + 1]) {

            /* Parabolic interpolation for sub-sample precision */
            float alpha = state->correlation_buf[lag - 1];
            float beta  = v;
            float gamma = state->correlation_buf[lag + 1];
            float denom = alpha - 2.0f * beta + gamma;
            float delta = (fabsf(denom) > 1e-10f) ? 0.5f * (alpha - gamma) / denom : 0.0f;
            float precise_lag = (float)lag + delta;

            /* Time of flight → distance (round trip, so divide by 2) */
            float tof = precise_lag / sr;
            float distance = c * tof / 2.0f;

            /* Only accept reasonable distances (0.1m – 15m) */
            if (distance >= 0.1f && distance <= 15.0f) {
                sources[count].x = distance;  /* Will be refined by angle later */
                sources[count].y = 0.0f;
                sources[count].z = 0.0f;
                sources[count].confidence = v;
                sources[count].freq_lo = (float)state->config.sonar_freq_lo;
                sources[count].freq_hi = (float)state->config.sonar_freq_hi;
                sources[count].source_id = count;
                sources[count].last_seen_us = 0;
                count++;
            }

            /* Skip past this peak to avoid double-counting */
            lag += (int)chirp_len / 2;
        }
    }

    return count;
}

/* ═══════════════════════════════════════════════════════════════
 *  Chirp Subtraction
 *
 *  Removes the emitted chirp from the audio recording.
 *  Uses least-squares scaling to find the best-fit amplitude
 *  of the chirp in the recording, then subtracts it.
 * ═══════════════════════════════════════════════════════════════ */

void photon_sonar_subtract_chirp(const photon_sonar_state_t *state,
                                  float *audio, uint32_t num_samples) {
    if (!state || !audio || !state->chirp_ref) return;

    uint32_t chirp_len = state->chirp_len;
    if (chirp_len > num_samples) return;

    /* Find best alignment via cross-correlation peak */
    float best_corr = 0.0f;
    int best_offset = 0;

    int search_range = (int)(num_samples - chirp_len);
    /* Limit search to first 10% of the buffer (chirp should be near start) */
    int search_limit = search_range < (int)(num_samples / 10) ? search_range : (int)(num_samples / 10);

    for (int off = 0; off < search_limit; off++) {
        float corr = 0.0f;
        for (uint32_t i = 0; i < chirp_len; i++) {
            corr += audio[off + i] * state->chirp_ref[i];
        }
        if (fabsf(corr) > fabsf(best_corr)) {
            best_corr = corr;
            best_offset = off;
        }
    }

    if (fabsf(best_corr) < 1e-10f) return;

    /* Compute optimal scaling factor: α = (audio · chirp) / (chirp · chirp) */
    float chirp_energy = 0.0f;
    for (uint32_t i = 0; i < chirp_len; i++) {
        chirp_energy += state->chirp_ref[i] * state->chirp_ref[i];
    }
    if (chirp_energy < 1e-10f) return;

    float alpha = best_corr / chirp_energy;

    /* Subtract scaled chirp */
    for (uint32_t i = 0; i < chirp_len && (best_offset + (int)i) < (int)num_samples; i++) {
        audio[best_offset + i] -= alpha * state->chirp_ref[i];
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Chirp Accessor
 * ═══════════════════════════════════════════════════════════════ */

const float* photon_sonar_get_chirp(const photon_sonar_state_t *state,
                                     uint32_t *out_len) {
    if (!state || !out_len) return NULL;
    *out_len = state->chirp_len;
    return state->chirp_ref;
}

/* ═══════════════════════════════════════════════════════════════
 *  Utility: Print Sources
 * ═══════════════════════════════════════════════════════════════ */

void photon_print_sources(const photon_source_t *sources, uint32_t count) {
    printf("┌─── Photon Sources (%u) ───┐\n", count);
    for (uint32_t i = 0; i < count; i++) {
        printf("│ [%u] pos=(%.2f, %.2f, %.2f) conf=%.2f freq=%.0f–%.0f Hz\n",
               sources[i].source_id,
               sources[i].x, sources[i].y, sources[i].z,
               sources[i].confidence,
               sources[i].freq_lo, sources[i].freq_hi);
    }
    printf("└──────────────────────────┘\n");
}
