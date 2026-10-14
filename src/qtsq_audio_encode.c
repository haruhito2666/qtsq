/*
 * Quantum Tensor Sequence — Photon Audio HQ Codec (v4)
 * Hybrid Lossless PCM + Per-Atom Spatial Metadata
 *
 * Architecture:
 *   1. Lossless Layer: Multi-channel float32 -> int32 scaled, 2nd-order delta, zlib
 *   2. Spatial Layer: STFT peak extraction -> spatial mapping (x, y, z)
 *      x = pan (L/R energy ratio)
 *      y = height (mapped from frequency)
 *      z = depth (mapped from amplitude)
 *
 * Author: Haruhito
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <zlib.h>
#include "qtsq_codec.h"
#include "qtsq.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ═══════════ Configuration ═══════════ */
#define PHOTON_FFT_SIZE   2048
#define PHOTON_HOP_SIZE   512
#define PHOTON_FREQ_BINS  (PHOTON_FFT_SIZE / 2 + 1)
#define PHOTON_MAX_PEAKS  128  /* Increased atoms per frame for spatial richness */

/* ═══════════ FFT ═══════════ */
static void photon_fft(float *re, float *im, int N, int inverse) {
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
                re[a] += tr; im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
    if (inverse) {
        float inv = 1.0f / (float)N;
        for (int i = 0; i < N; i++) { re[i] *= inv; im[i] *= inv; }
    }
}

/* ═══════════ Window ═══════════ */
static float photon_window[PHOTON_FFT_SIZE];
static int photon_win_ready = 0;
static void ensure_photon_window(void) {
    if (photon_win_ready) return;
    for (int i = 0; i < PHOTON_FFT_SIZE; i++) {
        float hann = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (PHOTON_FFT_SIZE - 1)));
        photon_window[i] = sqrtf(hann + 1e-10f);
    }
    photon_win_ready = 1;
}

/* ═══════════ 2nd-Order Delta Encoding for int32 ═══════════ */
static void delta_encode_float32(const float *input, int32_t *output, uint32_t count) {
    if (count == 0) return;
    
    /* Scale float to 24-bit integer range to perfectly capture up to 24-bit depth */
    const float scale = 8388607.0f;
    
    int32_t val0 = (int32_t)(input[0] * scale);
    output[0] = val0;
    if (count == 1) return;
    
    int32_t val1 = (int32_t)(input[1] * scale);
    output[1] = val1 - val0;
    
    int32_t p0 = val0;
    int32_t p1 = val1;
    
    for (uint32_t i = 2; i < count; i++) {
        int32_t val = (int32_t)(input[i] * scale);
        int32_t predicted = 2 * p1 - p0;
        output[i] = val - predicted;
        p0 = p1;
        p1 = val;
    }
}

/* ═══════════ Spatial Peak Extraction ═══════════ */
static uint32_t extract_spatial_atoms(const float *L_re, const float *L_im,
                                      const float *R_re, const float *R_im,
                                      uint32_t frame_index, uint32_t sample_rate,
                                      photon_atom_t *atoms, uint32_t max_atoms)
{
    int freq_bins = PHOTON_FREQ_BINS;
    float freq_res = (float)sample_rate / PHOTON_FFT_SIZE;
    uint32_t count = 0;
    
    float *mag = (float *)malloc(freq_bins * sizeof(float));
    for (int k = 0; k < freq_bins; k++) {
        float ml = sqrtf(L_re[k] * L_re[k] + L_im[k] * L_im[k]);
        float mr = sqrtf(R_re[k] * R_re[k] + R_im[k] * R_im[k]);
        mag[k] = ml + mr;
    }
    
    for (int k = 2; k < freq_bins - 2 && count < max_atoms; k++) {
        if (mag[k] > mag[k-1] && mag[k] > mag[k+1] &&
            mag[k] > mag[k-2] && mag[k] > mag[k+2] &&
            mag[k] > 0.0001f * PHOTON_FFT_SIZE)
        {
            float alpha = mag[k-1], beta = mag[k], gamma = mag[k+1];
            float denom = alpha - 2.0f * beta + gamma;
            float delta = (fabsf(denom) > 1e-10f) ? 0.5f * (alpha - gamma) / denom : 0.0f;
            
            float freq = ((float)k + delta) * freq_res;
            float amplitude = (beta - 0.25f * (alpha - gamma) * delta) * 2.0f / PHOTON_FFT_SIZE;
            
            /* Spatial mapping */
            float ml = sqrtf(L_re[k] * L_re[k] + L_im[k] * L_im[k]);
            float mr = sqrtf(R_re[k] * R_re[k] + R_im[k] * R_im[k]);
            
            /* Pan (X): -1 (left) to 1 (right) */
            float pan = (ml + mr > 1e-6f) ? (mr - ml) / (ml + mr) : 0.0f;
            
            /* Height (Y): map frequency to -1 to 1 (log scale) */
            float norm_f = log2f(freq / 20.0f + 1.0f) / log2f(20000.0f / 20.0f + 1.0f);
            float height = (norm_f * 2.0f) - 1.0f;
            if (height < -1.0f) height = -1.0f;
            if (height > 1.0f) height = 1.0f;
            
            /* Depth (Z): loud = near(1.0), quiet = far(0.0) */
            float depth = amplitude * 5.0f; 
            if (depth > 1.0f) depth = 1.0f;
            
            /* Phase average */
            float pl = atan2f(L_im[k], L_re[k]);
            float pr = atan2f(R_im[k], R_re[k]);
            float phase = (pl + pr) * 0.5f;
            if (phase < 0) phase += 2.0f * (float)M_PI;
            
            atoms[count].freq_hz = freq;
            atoms[count].magnitude = amplitude;
            atoms[count].phase = phase;
            atoms[count].x = pan;
            atoms[count].y = height;
            atoms[count].z = depth;
            atoms[count].spread = 1.0f - depth; /* distant sounds are more diffuse */
            atoms[count].frame_index = frame_index;
            count++;
        }
    }
    
    free(mag);
    return count;
}

/* ═══════════ ENCODER ═══════════ */
int qtsq_compress_audio_hq(qtsq_context_t *ctx,
                            const float *samples,     /* 32-bit float Native */
                            size_t num_samples,
                            uint32_t sample_rate,
                            uint8_t channels)
{
    if (!ctx || !samples) return QTSQ_ERR_NULL;
    if (num_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (channels < 2) channels = 2; /* Force stereo min for spatial */
    if (channels > 16) channels = 16;
    
    ensure_photon_window();
    
    /* SEC: bound per-channel sample count so the uint32 arithmetic below
     * (max_frames * PHOTON_MAX_PEAKS, spc * sizeof(int32_t)) cannot
     * overflow. 1 GiS (2^30) per channel is far beyond any real clip. */
    size_t spc_size = num_samples / channels;
    if (spc_size > (1u << 30)) return QTSQ_ERR_TOO_LARGE;
    uint32_t spc = (uint32_t)spc_size;
    
    /* ─── Layer 1: Lossless Delta Encode with Mid/Side for stereo pairs ─── */
    int32_t **delta_ch = (int32_t **)malloc(channels * sizeof(int32_t *));
    float *ch_buf = (float *)malloc(spc * sizeof(float));
    if (!delta_ch || !ch_buf) {
        if(delta_ch) free(delta_ch);
        if(ch_buf) free(ch_buf);
        return QTSQ_ERR_ALLOC;
    }

    for (int c = 0; c < channels; c++) {
        delta_ch[c] = (int32_t *)malloc(spc * sizeof(int32_t));
        if (!delta_ch[c]) {
            /* Free previous channels + ch_buf on failure */
            for (int j = 0; j < c; j++) free(delta_ch[j]);
            free(delta_ch); free(ch_buf);
            return QTSQ_ERR_ALLOC;
        }
    }

    /* Mid/Side explicit pass for stereo pairs (replaces the old dead
     * backpatch loop that was overwritten by this pass). Uses memcpy
     * for float→int32 storage (no strict-aliasing UB). */
    for (int pair = 0; pair < channels / 2; pair++) {
        int L_ch = pair * 2;
        int R_ch = pair * 2 + 1;
        for (uint32_t i = 0; i < spc; i++) {
            float L = samples[i * channels + L_ch];
            float R = samples[i * channels + R_ch];
            float Mid = (L + R) * 0.5f;
            float Side = (L - R) * 0.5f;
            memcpy(&delta_ch[L_ch][i], &Mid, sizeof(float));
            memcpy(&delta_ch[R_ch][i], &Side, sizeof(float));
        }
        /* Copy mid to ch_buf via memcpy (was (float*) cast — strict-aliasing UB),
         * then delta-encode. Repeat for side. */
        memcpy(ch_buf, delta_ch[L_ch], spc * sizeof(float));
        delta_encode_float32(ch_buf, delta_ch[L_ch], spc);

        memcpy(ch_buf, delta_ch[R_ch], spc * sizeof(float));
        delta_encode_float32(ch_buf, delta_ch[R_ch], spc);
    }
    // Handle odd channel if exists
    if (channels % 2 == 1) {
        int c = channels - 1;
        for(uint32_t i=0; i<spc; i++) ch_buf[i] = samples[i * channels + c];
        delta_encode_float32(ch_buf, delta_ch[c], spc);
    }
    free(ch_buf);
    
    /* ─── Layer 2: Spatial Atom Extraction (always uses ch 0 and 1) ─── */
    int fft_size = PHOTON_FFT_SIZE;
    int hop_size = PHOTON_HOP_SIZE;
    uint32_t max_frames = (spc / hop_size) + 1;
    
    /* Pre-allocate atoms array */
    uint32_t max_total_atoms = max_frames * PHOTON_MAX_PEAKS;
    photon_atom_t *atoms = (photon_atom_t *)malloc(max_total_atoms * sizeof(photon_atom_t));
    if (!atoms) {
        for (int c = 0; c < channels; c++) free(delta_ch[c]);
        free(delta_ch);
        return QTSQ_ERR_ALLOC;
    }
    uint32_t total_atoms = 0;
    
    float *L_re = (float *)calloc(fft_size, sizeof(float));
    float *L_im = (float *)calloc(fft_size, sizeof(float));
    float *R_re = (float *)calloc(fft_size, sizeof(float));
    float *R_im = (float *)calloc(fft_size, sizeof(float));
    
    uint32_t frame_index = 0;
    for (uint32_t start = 0; start + fft_size <= spc; start += hop_size) {
        for (int i = 0; i < fft_size; i++) {
            L_re[i] = samples[(start + i) * channels + 0] * photon_window[i];
            R_re[i] = samples[(start + i) * channels + 1] * photon_window[i];
            L_im[i] = 0.0f; R_im[i] = 0.0f;
        }
        photon_fft(L_re, L_im, fft_size, 0);
        photon_fft(R_re, R_im, fft_size, 0);
        
        uint32_t extracted = extract_spatial_atoms(L_re, L_im, R_re, R_im,
                                                  frame_index, sample_rate,
                                                  &atoms[total_atoms], PHOTON_MAX_PEAKS);
        total_atoms += extracted;
        frame_index++;
    }
    free(L_re); free(L_im); free(R_re); free(R_im);
    
    /* ─── Pack binary payload (PH04) ─── */
    size_t header_size = 32;
    uint32_t atom_size = sizeof(photon_atom_t); /* 32 bytes */
    uint32_t pcm_bytes_per_ch = spc * sizeof(int32_t);
    size_t raw_size = header_size + (channels * pcm_bytes_per_ch) + (total_atoms * atom_size);
    
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) {
        for (int c = 0; c < channels; c++) free(delta_ch[c]);
        free(delta_ch); free(atoms);
        return QTSQ_ERR_ALLOC;
    }
    
    size_t p = 0;
    raw[p++] = 'P'; raw[p++] = 'H'; raw[p++] = '0'; raw[p++] = '4';
    memcpy(raw + p, &spc, 4); p += 4;
    memcpy(raw + p, &sample_rate, 4); p += 4;
    raw[p++] = channels;
    raw[p++] = 32; /* bit depth float */
    raw[p++] = 1;  /* flags: has_spatial */
    raw[p++] = 0;  /* reserved */
    
    memcpy(raw + p, &pcm_bytes_per_ch, 4); p += 4;
    memcpy(raw + p, &total_atoms, 4); p += 4;
    memcpy(raw + p, &atom_size, 4); p += 4;
    
    /* Write PCM Data */
    for (int c = 0; c < channels; c++) {
        memcpy(raw + p, delta_ch[c], pcm_bytes_per_ch);
        p += pcm_bytes_per_ch;
        free(delta_ch[c]);
    }
    free(delta_ch);
    
    /* Write Atom Data */
    memcpy(raw + p, atoms, total_atoms * atom_size);
    p += (total_atoms * atom_size);
    free(atoms);
    
    /* ─── compress: qtc (opt-in) or zlib (default) ─── */
    uint8_t *zdata = NULL;
    size_t zlen = 0;
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (qtsq_codec_compress(raw, p, &zdata, &zlen) != QTSQ_OK) {
            free(raw); return QTSQ_ERR_ALLOC;
        }
    } else {
        uLongf zbound = compressBound((uLong)p);
        zdata = (uint8_t *)malloc(zbound);
        if (!zdata) { free(raw); return QTSQ_ERR_ALLOC; }
        uLongf zl = zbound;
        if (compress2(zdata, &zl, raw, (uLong)p, 9) != Z_OK) {
            free(raw); free(zdata); return QTSQ_ERR_ALLOC;
        }
        zlen = (size_t)zl;
    }
    free(raw);

    /* Final: [4B raw_size][stream] */
    size_t total = 4 + zlen;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(zdata); return QTSQ_ERR_ALLOC; }
    uint32_t rs32 = (uint32_t)p;
    memcpy(out, &rs32, 4);
    memcpy(out + 4, zdata, zlen);
    free(zdata);
    
    /* Store in context */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;
    ctx->header.singularity_size = total;
    ctx->header.strategy = QTSQ_STRATEGY_PHOTON;
    ctx->header.data_type = (channels > 2) ? QTSQ_TYPE_AUDIO_MULTI : QTSQ_TYPE_AUDIO_STEREO;
    ctx->header.original_size = num_samples * sizeof(float);
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 32;
    ctx->header.flags |= (QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_SPATIAL);
    
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "photon_audio", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = spc;
    ctx->schema.num_dims = 1;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "SPATIAL_ATOMS", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256((const uint8_t *)samples, num_samples * sizeof(float), ctx->header.checksum);
    
    return QTSQ_OK;
}
