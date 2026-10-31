/*
 * Quantum Tensor Sequence — Singularity Audio Codec v5
 * Dictionary-Based Spectral Compression with Phase Preservation
 *
 * Architecture:
 *   1. STFT → magnitude + phase frames (using sqrt-Hann window)
 *   2. Build dictionary: greedy clustering via cosine similarity
 *      - Each unique spectral shape stored once
 *      - Per-band gains (8 bands) for adaptive matching
 *   3. Phase: store deviation from expected phase advance (8-bit)
 *   4. Timeline: [dictionary_index, band_gains[], phase_deviations[]] per frame
 *   5. Decode: lookup × band_gains → combine with stored phase → iSTFT
 *
 * v5 improvements over v4:
 *   - Phase preservation (no more metallic/robotic artifacts)
 *   - Per-band gains (8 bands) for better spectral matching
 *   - Log-scale magnitude quantization (perceptual)
 *   - Proper sqrt-Hann COLA overlap-add
 *   - Reduced dictionary quantization noise
 *
 * Author: Haruhito
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <zlib.h>
#include "qtsq.h"
#include "qtsq_codec.h"
#include "qtc.h"

#ifdef QTSQ_HAVE_OPUS
#include "qtsq_opus.h"
#endif

#define FFT_SIZE     2048
#define HOP_SIZE     1024      /* 50% overlap — good balance of quality vs size */
#define FREQ_BINS    (FFT_SIZE / 2 + 1)  /* 1025 */
#define PHASE_BINS   256       /* Only store phase for bins 0-255 (~5.5 kHz, perceptually critical) */
#define SIM_THRESH   0.92f    /* Slightly more permissive → fewer entries, bigger gains */
#define MAX_DICT     4096     /* More entries allowed for quality */
#define NUM_BANDS    8        /* Per-band gain resolution */
#define PHASE_BITS   4        /* 4-bit phase deviation → 2 values per byte */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Band boundaries (approximate critical bands simplified to 8) */
static const int BAND_EDGES[NUM_BANDS + 1] = {
    0, 16, 48, 128, 256, 384, 512, 768, FREQ_BINS
};

/* ═══════════════════════════════════════════
 *  FFT — Radix-2 Cooley-Tukey
 * ═══════════════════════════════════════════ */
static void fft(float *re, float *im, int N, int inverse) {
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

/* ═══════════════════════════════════════════
 *  Window — sqrt-Hann for COLA compliance
 *  Analysis × Synthesis both use sqrt-Hann
 *  so (sqrt-Hann)² = Hann, which is COLA.
 * ═══════════════════════════════════════════ */
static float ana_win[FFT_SIZE];
static int win_ready = 0;
static void ensure_window(void) {
    if (win_ready) return;
    for (int i = 0; i < FFT_SIZE; i++) {
        float hann = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(FFT_SIZE - 1)));
        ana_win[i] = sqrtf(hann + 1e-10f);  /* sqrt-Hann for analysis+synthesis */
    }
    win_ready = 1;
}

/* ═══════════════════════════════════════════
 *  STFT — Now stores both magnitude AND phase
 * ═══════════════════════════════════════════ */
static void stft_forward_full(const float *audio, uint32_t nsamp,
                               float *mag, float *phase, uint32_t *nf_out) {
    ensure_window();
    uint32_t nf = 0;
    for (uint32_t start = 0; start + FFT_SIZE <= nsamp; start += HOP_SIZE) {
        float re[FFT_SIZE], im[FFT_SIZE];
        for (int i = 0; i < FFT_SIZE; i++) {
            re[i] = audio[start + i] * ana_win[i];
            im[i] = 0.0f;
        }
        fft(re, im, FFT_SIZE, 0);
        for (int k = 0; k < FREQ_BINS; k++) {
            mag[nf * FREQ_BINS + k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
            phase[nf * FREQ_BINS + k] = atan2f(im[k], re[k]);
        }
        nf++;
    }
    *nf_out = nf;
}

/* ═══════════════════════════════════════════
 *  iSTFT — Uses stored phase (no vocoder needed)
 * ═══════════════════════════════════════════ */
static void istft_from_phase(const float *mag, const float *phase,
                              uint32_t nf, float *audio, uint32_t nsamp) {
    ensure_window();
    float *wsum = (float *)calloc(nsamp, sizeof(float));
    memset(audio, 0, nsamp * sizeof(float));

    for (uint32_t f = 0; f < nf; f++) {
        float re[FFT_SIZE], im[FFT_SIZE];
        for (int k = 0; k < FREQ_BINS; k++) {
            float m = mag[f * FREQ_BINS + k];
            float p = phase[f * FREQ_BINS + k];
            re[k] = m * cosf(p);
            im[k] = m * sinf(p);
        }
        /* Mirror for real-valued iFFT */
        for (int k = FREQ_BINS; k < FFT_SIZE; k++) {
            re[k] =  re[FFT_SIZE - k];
            im[k] = -im[FFT_SIZE - k];
        }
        fft(re, im, FFT_SIZE, 1);

        uint32_t start = f * HOP_SIZE;
        for (int i = 0; i < FFT_SIZE && start + i < nsamp; i++) {
            audio[start + i] += re[i] * ana_win[i];  /* synthesis window */
            wsum[start + i]  += ana_win[i] * ana_win[i];
        }
    }
    /* Normalize by window sum (COLA) with safe floor */
    for (uint32_t i = 0; i < nsamp; i++) {
        if (wsum[i] > 0.01f) audio[i] /= wsum[i];
    }
    free(wsum);
}

/* ═══════════════════════════════════════════
 *  Phase Deviation Encoding
 *
 *  expected_phase[k] = prev_phase[k] + 2π × k × HOP / FFT
 *  deviation = actual_phase - expected_phase (wrapped to [-π, π])
 *  Quantize deviation to 8-bit: [0, 255] → [-π, π]
 * ═══════════════════════════════════════════ */
static float wrap_phase(float p) {
    while (p > (float)M_PI) p -= 2.0f * (float)M_PI;
    while (p < -(float)M_PI) p += 2.0f * (float)M_PI;
    return p;
}

static uint8_t encode_phase_dev(float dev) {
    /* dev ∈ [-π, π] → [0, 15] (4-bit) */
    float normalized = (dev + (float)M_PI) / (2.0f * (float)M_PI);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;
    return (uint8_t)(normalized * 15.0f + 0.5f);
}

static float decode_phase_dev(uint8_t v) {
    /* [0, 15] → [-π, π] */
    return ((float)(v & 0x0F) / 15.0f) * 2.0f * (float)M_PI - (float)M_PI;
}

/* ═══════════════════════════════════════════
 *  Dictionary Builder (unchanged conceptually)
 * ═══════════════════════════════════════════ */
static float vec_norm(const float *v, int n) {
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)v[i] * v[i];
    return (float)sqrt(sum);
}

static float cosine_sim(const float *a, const float *b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += (double)a[i] * b[i];
        na  += (double)a[i] * a[i];
        nb  += (double)b[i] * b[i];
    }
    double denom = sqrt(na) * sqrt(nb);
    if (denom < 1e-12) return 0.0f;
    return (float)(dot / denom);
}

typedef struct {
    float *entries;     /* dict_size × FREQ_BINS */
    float *norms;       /* dict_size — precomputed L2 norms */
    int size;
    int capacity;
} dict_t;

static void dict_init(dict_t *d, int cap) {
    d->entries = (float *)malloc(cap * FREQ_BINS * sizeof(float));
    d->norms = (float *)malloc(cap * sizeof(float));
    d->size = 0;
    d->capacity = cap;
}

static int dict_add(dict_t *d, const float *frame) {
    if (d->size >= d->capacity) return d->size - 1;
    int idx = d->size;
    memcpy(d->entries + idx * FREQ_BINS, frame, FREQ_BINS * sizeof(float));
    d->norms[idx] = vec_norm(frame, FREQ_BINS);
    d->size++;
    return idx;
}

/* Per-band gains: compute gain for each of NUM_BANDS frequency bands */
static void compute_band_gains(const float *frame, const float *dict_entry,
                                float *band_gains) {
    for (int b = 0; b < NUM_BANDS; b++) {
        int lo = BAND_EDGES[b], hi = BAND_EDGES[b + 1];
        float frame_energy = 0, dict_energy = 0;
        for (int k = lo; k < hi; k++) {
            frame_energy += frame[k] * frame[k];
            dict_energy += dict_entry[k] * dict_entry[k];
        }
        frame_energy = sqrtf(frame_energy);
        dict_energy = sqrtf(dict_energy);
        band_gains[b] = (dict_energy > 1e-6f) ? (frame_energy / dict_energy) : 0.0f;
    }
}

static int dict_find_best(const dict_t *d, const float *frame, float *band_gains) {
    float frame_norm = vec_norm(frame, FREQ_BINS);
    if (frame_norm < 1e-6f) {
        for (int b = 0; b < NUM_BANDS; b++) band_gains[b] = 0.0f;
        return 0;
    }

    int best_idx = -1;
    float best_sim = -1.0f;

    for (int i = 0; i < d->size; i++) {
        float sim = cosine_sim(frame, d->entries + i * FREQ_BINS, FREQ_BINS);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }

    if (best_sim >= SIM_THRESH && best_idx >= 0) {
        compute_band_gains(frame, d->entries + best_idx * FREQ_BINS, band_gains);
        return best_idx;
    }

    /* No match: add new entry */
    for (int b = 0; b < NUM_BANDS; b++) band_gains[b] = 1.0f;
    return dict_add((dict_t *)d, frame);
}

static void dict_free(dict_t *d) {
    free(d->entries); free(d->norms);
    d->entries = NULL; d->norms = NULL;
    d->size = 0;
}

/* ═══════════════════════════════════════════
 *  Quantization helpers — Log-scale for magnitudes
 * ═══════════════════════════════════════════ */
/* Log-scale: better for perceptual audio (more bits for quiet parts) */
static uint16_t mag_to_u16_log(float v, float mx) {
    if (mx < 1e-10f || v <= 0) return 0;
    /* log(1 + v/mx * 1023) / log(1024) → [0, 1] → × 65535 */
    float ratio = v / mx;
    if (ratio > 1.0f) ratio = 1.0f;
    float logval = log2f(1.0f + ratio * 1023.0f) / 10.0f;  /* log2(1024) = 10 */
    return (uint16_t)(logval * 65535.0f);
}

static float u16_to_mag_log(uint16_t v, float mx) {
    float logval = (float)v / 65535.0f;
    float ratio = (powf(2.0f, logval * 10.0f) - 1.0f) / 1023.0f;
    return ratio * mx;
}

/* Log-scale gain encoding: 16-bit */
static uint16_t encode_gain(float g) {
    if (g <= 0) return 0;
    float lg = log2f(g + 1e-6f) + 10.0f;
    if (lg < 0) lg = 0;
    if (lg > 17.0f) lg = 17.0f;
    return (uint16_t)(lg / 17.0f * 65535.0f);
}

static float decode_gain(uint16_t v) {
    float lg = ((float)v / 65535.0f) * 17.0f;
    return powf(2.0f, lg - 10.0f);
}

/* ═══════════════════════════════════════════
 *  ENCODER v5
 * ═══════════════════════════════════════════ */

/* Phase 6a/7: lossless raw-int16 payload shared by the Provenance path and
 * the sub-FFT short-clip fallback. qtc is the default codec: the typed AUDIO
 * lens (mid/side + linear prediction) for whole-frame data, generic qtc for
 * ragged data; zlib only via explicit opt-out. Framing is [u32 raw_bytes]
 * [stream] either way — the decoder sniffs the stream magic, so old zlib
 * files keep their read path. */
static int store_raw_int16(qtsq_context_t *ctx, const int16_t *samples,
                           size_t num_samples, uint32_t sample_rate,
                           uint8_t channels) {
    size_t raw_bytes = num_samples * sizeof(int16_t);
    uint8_t *body = NULL;
    size_t body_len = 0;

    /* Phase 7: qtc is the default; there is no silent zlib fallback. The
     * typed AUDIO lens (mid/side + linear prediction) needs whole frames,
     * so a ragged sample count (not a multiple of the channel count) is
     * handed to the generic qtc compressor over the raw int16 bytes. zlib
     * is used only when explicitly selected via the codec opt-out. */
    if (ctx->codec == QTSQ_CODEC_QTC) {
        if (channels != 0 && num_samples % channels == 0) {
            uint32_t nframes = (uint32_t)(num_samples / channels);
            int qrc = qtc_compress_audio((const uint8_t *)samples, nframes,
                                         channels, 16, &body, &body_len, NULL);
            if (qrc != QTC_OK || !body) {
                free(body);
                return (qrc == QTC_ERR_ALLOC) ? QTSQ_ERR_ALLOC : QTSQ_ERR_FORMAT;
            }
        } else {
            if (qtsq_codec_compress((const uint8_t *)samples, raw_bytes,
                                    &body, &body_len) != QTSQ_OK) {
                free(body);
                return QTSQ_ERR_FORMAT;
            }
        }
    } else {
        uLongf zbound = compressBound((uLong)raw_bytes);
        body = (uint8_t *)malloc(zbound);
        if (!body) return QTSQ_ERR_ALLOC;
        uLongf zlen = zbound;
        if (compress2(body, &zlen, (const uint8_t *)samples,
                      (uLong)raw_bytes, 9) != Z_OK) {
            free(body);
            return QTSQ_ERR_ALLOC;
        }
        body_len = (size_t)zlen;
    }

    uint8_t *out = (uint8_t *)malloc(4 + body_len);
    if (!out) { free(body); return QTSQ_ERR_ALLOC; }
    uint32_t raw32 = (uint32_t)raw_bytes;
    memcpy(out, &raw32, 4);
    memcpy(out + 4, body, body_len);
    free(body);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = 4 + body_len;
    ctx->header.strategy = QTSQ_STRATEGY_PROCEDURAL;
    ctx->header.data_type = (channels >= 2) ? QTSQ_TYPE_AUDIO_STEREO : QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = num_samples * sizeof(int16_t);
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 16;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "audio_raw", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.num_dims = 1;
    ctx->schema.dimensions[0] = (uint32_t)num_samples;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "RAW_INT16", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256((const uint8_t *)samples, raw_bytes, ctx->header.checksum);
    return QTSQ_OK;
}

/* Phase 6b: lossy creative audio via qtc MDCT2 (perceptual coder).
 * Framing mirrors store_raw_int16: [u32 raw_bytes][qtc stream]; the
 * decoder sniffs the qtc magic. quality <= 0 picks the middle of the
 * MDCT2 quality range as a sensible default. */
static int store_qtc_mdct2(qtsq_context_t *ctx, const int16_t *samples,
                           size_t num_samples, uint32_t sample_rate,
                           uint8_t channels) {
    /* The [u32 raw_bytes] framing and schema dimension are 32-bit. Check
     * before multiplying so a huge size_t cannot wrap into a small value
     * on 32-bit builds. */
    if (num_samples > (size_t)UINT32_MAX / sizeof(int16_t))
        return QTSQ_ERR_TOO_LARGE;
    size_t raw_bytes = num_samples * sizeof(int16_t);
    if (channels == 0 || num_samples % channels != 0) return QTSQ_ERR_FORMAT;
    uint32_t nframes = (uint32_t)(num_samples / channels);

    qtc_options_t opts;
    qtc_options_default(&opts);
    opts.mode = QTC_MODE_LOSSY;
    opts.quality = (ctx->quality > 0.0f) ? ctx->quality : 0.5f;
    /* Rate control divides the kbps target by the stream's actual rate.
     * Leaving this 0 made MDCT2 assume 44.1 kHz, so 48/96 kHz streams
     * produced 9%/118% more bytes than the selected tier. */
    opts.sample_rate = (int)sample_rate;

    uint8_t *body = NULL;
    size_t body_len = 0;
    int qrc = qtc_compress_audio((const uint8_t *)samples, nframes,
                                 channels, 16, &body, &body_len, &opts);
    if (qrc != QTC_OK || !body) {
        free(body);
        return (qrc == QTC_ERR_ALLOC) ? QTSQ_ERR_ALLOC : QTSQ_ERR_FORMAT;
    }

    uint8_t *out = (uint8_t *)malloc(4 + body_len);
    if (!out) { free(body); return QTSQ_ERR_ALLOC; }
    uint32_t raw32 = (uint32_t)raw_bytes;
    memcpy(out, &raw32, 4);
    memcpy(out + 4, body, body_len);
    free(body);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = 4 + body_len;
    ctx->header.strategy = QTSQ_STRATEGY_PROCEDURAL;
    ctx->header.data_type = (channels >= 2) ? QTSQ_TYPE_AUDIO_STEREO : QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = raw_bytes;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 16;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "audio_qtc_mdct2", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.num_dims = 1;
    ctx->schema.dimensions[0] = (uint32_t)num_samples;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "QTC_AUDIO_MDCT2", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256((const uint8_t *)samples, raw_bytes, ctx->header.checksum);
    return QTSQ_OK;
}

int qtsq_compress_audio(qtsq_context_t *ctx, const int16_t *samples,
                        size_t num_samples, uint32_t sample_rate,
                        uint8_t channels) {
    if (!ctx || !samples) return QTSQ_ERR_NULL;
    if (num_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (num_samples > (size_t)UINT32_MAX / sizeof(int16_t))
        return QTSQ_ERR_TOO_LARGE;
    if (channels == 0) channels = 1;
    /* Standard audio is stereo. Mono is accepted only when the caller has
     * a mono source; silently reinterpreting a 3+ channel interleaved
     * buffer as stereo would scramble the channel order. Downmix to
     * stereo (or encode with another codec) before calling. */
    if (channels > 2) return QTSQ_ERR_FORMAT;

    /* Phase 7: Creative intent uses Opus regardless of the codec selection
     * (measured ~34% smaller than qtc MDCT2 on clean tonal content). Now
     * that qtc is the default codec, MDCT2 is kept only as the creative
     * fallback for builds without libopus. */
#ifdef QTSQ_HAVE_OPUS
    if (ctx->intent == QTSQ_INTENT_CREATIVE) {
        return qtsq_compress_audio_opus(ctx, samples, num_samples,
                                        sample_rate, channels);
    }
#endif
    if (ctx->intent == QTSQ_INTENT_CREATIVE && ctx->codec == QTSQ_CODEC_QTC) {
        return store_qtc_mdct2(ctx, samples, num_samples, sample_rate, channels);
    }

    /* Phase 3b: Provenance intent is bit-exact by definition (PRODUCT §2).
     * Route it onto the LOSSLESS raw-int16 path for ALL lengths, not just
     * the sub-FFT fallback below. The spectral encoder down the default
     * branch is lossy and must never run for Provenance. */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        return store_raw_int16(ctx, samples, num_samples, sample_rate, channels);
    }

    /* Standard intent (None): the standard lossy type is MDCT2, not the
     * legacy spectral dictionary below. The spectral coder is lossless-
     * ish and expands already-compressed media; it stays reachable only
     * through the explicit zlib opt-out (and old files still decode).
     * This is what gives the int16 callers -- Android, iOS, the CLI --
     * the same MP3-comparable size as the float capture path. */
    if (ctx->codec == QTSQ_CODEC_QTC) {
        return store_qtc_mdct2(ctx, samples, num_samples, sample_rate, channels);
    }

    size_t spc = num_samples / channels;
    if (spc == 0) return QTSQ_ERR_TOO_SMALL;

    /* Short-audio fallback: the STFT needs at least one full FFT frame.
     * Shorter inputs previously encoded "successfully" but decoded to pure
     * silence (nf=0 frames) — silent data loss. Store them LOSSLESSLY as a
     * zlib-compressed raw int16 payload instead, so small clips round-trip
     * exactly. Marker: strategy PROCEDURAL + audio data type; the decoder
     * recognizes this and skips the spectral path. */
    if (spc < FFT_SIZE) {
        return store_raw_int16(ctx, samples, num_samples, sample_rate, channels);
    }

    /* Mid/Side split */
    float *mid  = (float *)malloc(spc * sizeof(float));
    float *side = (float *)malloc(spc * sizeof(float));
    if (!mid || !side) { free(mid); free(side); return QTSQ_ERR_ALLOC; }

    if (channels >= 2) {
        for (size_t i = 0; i < spc; i++) {
            float L = (float)samples[i * 2], R = (float)samples[i * 2 + 1];
            mid[i]  = (L + R) * 0.5f;
            side[i] = (L - R) * 0.5f;
        }
    } else {
        for (size_t i = 0; i < spc; i++) mid[i] = (float)samples[i];
        memset(side, 0, spc * sizeof(float));
    }

    /* STFT → magnitude + phase frames */
    uint32_t max_fr = (spc / HOP_SIZE) + 2;
    float *mid_mag   = (float *)calloc(max_fr * FREQ_BINS, sizeof(float));
    float *mid_phase  = (float *)calloc(max_fr * FREQ_BINS, sizeof(float));
    float *side_mag  = (float *)calloc(max_fr * FREQ_BINS, sizeof(float));
    float *side_phase = (float *)calloc(max_fr * FREQ_BINS, sizeof(float));
    if (!mid_mag || !mid_phase || !side_mag || !side_phase) {
        free(mid); free(side);
        free(mid_mag); free(mid_phase); free(side_mag); free(side_phase);
        return QTSQ_ERR_ALLOC;
    }

    uint32_t nf, snf;
    stft_forward_full(mid, spc, mid_mag, mid_phase, &nf);
    stft_forward_full(side, spc, side_mag, side_phase, &snf);
    free(mid); free(side);

    /* ─── Compute phase deviations (only for low-freq bins 0..PHASE_BINS-1) ─── */
    float *phase_inc = (float *)malloc(PHASE_BINS * sizeof(float));
    for (int k = 0; k < PHASE_BINS; k++)
        phase_inc[k] = 2.0f * (float)M_PI * (float)k * (float)HOP_SIZE / (float)FFT_SIZE;

    /* Pack 2 × 4-bit values per byte → PHASE_BINS/2 bytes per frame */
    uint32_t phase_bytes_per_frame = (PHASE_BINS + 1) / 2;
    uint8_t *mid_phase_packed  = (uint8_t *)calloc(nf * phase_bytes_per_frame, 1);
    uint8_t *side_phase_packed = (uint8_t *)calloc(nf * phase_bytes_per_frame, 1);
    float *prev_mid_phase  = (float *)calloc(PHASE_BINS, sizeof(float));
    float *prev_side_phase = (float *)calloc(PHASE_BINS, sizeof(float));

    for (uint32_t f = 0; f < nf; f++) {
        for (int k = 0; k < PHASE_BINS; k++) {
            float expected_mid  = prev_mid_phase[k] + phase_inc[k];
            float dev_mid = wrap_phase(mid_phase[f * FREQ_BINS + k] - expected_mid);
            uint8_t qm = encode_phase_dev(dev_mid);
            prev_mid_phase[k] = mid_phase[f * FREQ_BINS + k];

            float expected_side = prev_side_phase[k] + phase_inc[k];
            float dev_side = wrap_phase(side_phase[f * FREQ_BINS + k] - expected_side);
            uint8_t qs = encode_phase_dev(dev_side);
            prev_side_phase[k] = side_phase[f * FREQ_BINS + k];

            /* Pack 2 nibbles into 1 byte */
            uint32_t byte_idx = f * phase_bytes_per_frame + (k / 2);
            if (k % 2 == 0) {
                mid_phase_packed[byte_idx]  = (qm << 4);
                side_phase_packed[byte_idx] = (qs << 4);
            } else {
                mid_phase_packed[byte_idx]  |= (qm & 0x0F);
                side_phase_packed[byte_idx] |= (qs & 0x0F);
            }
        }
    }
    free(phase_inc); free(prev_mid_phase); free(prev_side_phase);
    free(mid_phase); free(side_phase);

    /* ─── Build dictionaries with per-band gains ─── */
    dict_t mid_dict, side_dict;
    dict_init(&mid_dict, MAX_DICT);
    dict_init(&side_dict, MAX_DICT);

    uint16_t *mid_indices  = (uint16_t *)malloc(nf * sizeof(uint16_t));
    uint16_t *mid_bgains   = (uint16_t *)malloc(nf * NUM_BANDS * sizeof(uint16_t));
    uint16_t *side_indices = (uint16_t *)malloc(nf * sizeof(uint16_t));
    uint16_t *side_bgains  = (uint16_t *)malloc(nf * NUM_BANDS * sizeof(uint16_t));

    dict_add(&mid_dict, mid_mag);  /* seed */
    dict_add(&side_dict, side_mag);

    for (uint32_t f = 0; f < nf; f++) {
        float mg[NUM_BANDS], sg[NUM_BANDS];
        int mi = dict_find_best(&mid_dict, mid_mag + f * FREQ_BINS, mg);
        int si = dict_find_best(&side_dict, side_mag + f * FREQ_BINS, sg);
        mid_indices[f]  = (uint16_t)mi;
        side_indices[f] = (uint16_t)si;
        for (int b = 0; b < NUM_BANDS; b++) {
            mid_bgains[f * NUM_BANDS + b]  = encode_gain(mg[b]);
            side_bgains[f * NUM_BANDS + b] = encode_gain(sg[b]);
        }
    }
    free(mid_mag); free(side_mag);

    /* ─── Pack v5 format ─── */
    /* Find max values for log quantization */
    float mid_dict_max = 0, side_dict_max = 0;
    for (int i = 0; i < mid_dict.size * FREQ_BINS; i++)
        if (mid_dict.entries[i] > mid_dict_max) mid_dict_max = mid_dict.entries[i];
    for (int i = 0; i < side_dict.size * FREQ_BINS; i++)
        if (side_dict.entries[i] > side_dict_max) side_dict_max = side_dict.entries[i];

    /* v5 header: magic(4) + spc(4) + sr(4) + nf(4) + md_sz(2) + sd_sz(2)
     *          + ch(1) + num_bands(1) + md_max(4) + sd_max(4) = 30 bytes */
    size_t phase_total = nf * phase_bytes_per_frame;  /* per channel */
    size_t packed_sz = 30 +
                       mid_dict.size * FREQ_BINS * 2 +    /* mid dict (16-bit log) */
                       side_dict.size * FREQ_BINS * 2 +   /* side dict (16-bit log) */
                       nf * 2 +                            /* mid indices */
                       nf * NUM_BANDS * 2 +                /* mid band gains */
                       nf * 2 +                            /* side indices */
                       nf * NUM_BANDS * 2 +                /* side band gains */
                       phase_total +                        /* mid phase (4-bit packed) */
                       phase_total;                         /* side phase (4-bit packed) */

    uint8_t *packed = (uint8_t *)malloc(packed_sz);
    if (!packed) {
        dict_free(&mid_dict); dict_free(&side_dict);
        free(mid_indices); free(mid_bgains); free(side_indices); free(side_bgains);
        free(mid_phase_packed); free(side_phase_packed);
        return QTSQ_ERR_ALLOC;
    }

    size_t p = 0;
    /* v5 magic to distinguish from v4 */
    packed[p++] = 'S'; packed[p++] = 'D'; packed[p++] = 'v'; packed[p++] = '5';
    memcpy(packed + p, &spc, 4); p += 4;
    memcpy(packed + p, &sample_rate, 4); p += 4;
    memcpy(packed + p, &nf, 4); p += 4;
    uint16_t md_sz = (uint16_t)mid_dict.size, sd_sz = (uint16_t)side_dict.size;
    memcpy(packed + p, &md_sz, 2); p += 2;
    memcpy(packed + p, &sd_sz, 2); p += 2;
    packed[p++] = channels;
    packed[p++] = (uint8_t)NUM_BANDS;
    memcpy(packed + p, &mid_dict_max, 4); p += 4;
    memcpy(packed + p, &side_dict_max, 4); p += 4;

    /* Mid dictionary: 16-bit log-quantized */
    for (int i = 0; i < mid_dict.size * FREQ_BINS; i++) {
        uint16_t v = mag_to_u16_log(mid_dict.entries[i], mid_dict_max);
        packed[p++] = v >> 8; packed[p++] = v & 0xFF;
    }
    /* Side dictionary */
    for (int i = 0; i < side_dict.size * FREQ_BINS; i++) {
        uint16_t v = mag_to_u16_log(side_dict.entries[i], side_dict_max);
        packed[p++] = v >> 8; packed[p++] = v & 0xFF;
    }

    /* Mid timeline: indices + per-band gains */
    for (uint32_t f = 0; f < nf; f++) {
        packed[p++] = mid_indices[f] >> 8; packed[p++] = mid_indices[f] & 0xFF;
    }
    for (uint32_t f = 0; f < nf; f++) {
        for (int b = 0; b < NUM_BANDS; b++) {
            uint16_t g = mid_bgains[f * NUM_BANDS + b];
            packed[p++] = g >> 8; packed[p++] = g & 0xFF;
        }
    }
    /* Side timeline */
    for (uint32_t f = 0; f < nf; f++) {
        packed[p++] = side_indices[f] >> 8; packed[p++] = side_indices[f] & 0xFF;
    }
    for (uint32_t f = 0; f < nf; f++) {
        for (int b = 0; b < NUM_BANDS; b++) {
            uint16_t g = side_bgains[f * NUM_BANDS + b];
            packed[p++] = g >> 8; packed[p++] = g & 0xFF;
        }
    }

    /* Phase deviations (4-bit packed, PHASE_BINS bins only) */
    memcpy(packed + p, mid_phase_packed, phase_total); p += phase_total;
    memcpy(packed + p, side_phase_packed, phase_total); p += phase_total;

    dict_free(&mid_dict); dict_free(&side_dict);
    free(mid_indices); free(mid_bgains); free(side_indices); free(side_bgains);
    free(mid_phase_packed); free(side_phase_packed);

    /* zlib compress */
    uLongf zb = compressBound((uLong)p);
    uint8_t *zd = (uint8_t *)malloc(zb);
    if (!zd) { free(packed); return QTSQ_ERR_ALLOC; }
    uLongf zs = zb;
    if (compress2(zd, &zs, packed, (uLong)p, 9) != Z_OK) {
        free(packed); free(zd); return QTSQ_ERR_ALLOC;
    }
    free(packed);

    size_t total = 4 + (size_t)zs;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(zd); return QTSQ_ERR_ALLOC; }
    uint32_t ps32 = (uint32_t)p;
    memcpy(out, &ps32, 4);
    memcpy(out + 4, zd, zs);
    free(zd);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;
    ctx->header.strategy = QTSQ_STRATEGY_FOURIER;
    ctx->header.data_type = (channels >= 2) ? QTSQ_TYPE_AUDIO_STEREO : QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = num_samples * sizeof(int16_t);
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 16;
    ctx->header.singularity_size = ctx->singularity_size;
    ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "audio_dictionary", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = spc;
    ctx->schema.num_dims = 1;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "SPEC_DICTIONARY", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    qtsq_sha256((const uint8_t *)samples, num_samples * sizeof(int16_t), ctx->header.checksum);
    return QTSQ_OK;
}

int qtsq_compress_signal(qtsq_context_t *ctx, const double *sig, size_t n) {
    if (!ctx || !sig) return QTSQ_ERR_NULL;
    if (n == 0) return QTSQ_ERR_TOO_SMALL;
    int16_t *s = (int16_t *)malloc(n * sizeof(int16_t));
    if (!s) return QTSQ_ERR_ALLOC;
    double mx = 0;
    for (size_t i = 0; i < n; i++) { double a = fabs(sig[i]); if (a > mx) mx = a; }
    if (mx == 0) mx = 1;
    for (size_t i = 0; i < n; i++) s[i] = (int16_t)(sig[i] / mx * 32767.0);
    int r = qtsq_compress_audio(ctx, s, n, 1, 1);
    free(s);
    if (r == QTSQ_OK) ctx->header.data_type = QTSQ_TYPE_SIGNAL_1D;
    return r;
}

/* ═══════════════════════════════════════════
 *  DECODER v5 — Dictionary + Phase Reconstruction
 * ═══════════════════════════════════════════ */
int qtsq_decompress_lensing(const qtsq_context_t *ctx,
                            uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 8) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;

    /* SEC: bound file-declared decompressed size (DoS guard). */
    uint8_t *packed = NULL;
    size_t dl = 0;
    int rc = qtsq_uncompress_bounded(buf, ctx->singularity_size, 256, 1ULL << 30,
                                     &packed, &dl);
    if (rc != QTSQ_OK) return rc;

    size_t p = 0;

    /* Check for v5 magic */
    int is_v5 = (packed[0] == 'S' && packed[1] == 'D' &&
                 packed[2] == 'v' && packed[3] == '5');

    uint32_t spc, sr, nf;
    uint16_t md_sz, sd_sz;
    uint8_t ch;
    uint8_t num_bands = 1;  /* v4 default: 1 global gain */
    float mid_dict_max, side_dict_max;

    if (is_v5) {
        p = 4; /* skip magic */
        memcpy(&spc, packed + p, 4); p += 4;
        memcpy(&sr, packed + p, 4); p += 4;
        memcpy(&nf, packed + p, 4); p += 4;
        memcpy(&md_sz, packed + p, 2); p += 2;
        memcpy(&sd_sz, packed + p, 2); p += 2;
        ch = packed[p++]; if (ch < 1) ch = 1; if (ch > 2) ch = 2;
        num_bands = packed[p++]; if (num_bands < 1) num_bands = 1; if (num_bands > 16) num_bands = 16;
        memcpy(&mid_dict_max, packed + p, 4); p += 4;
        memcpy(&side_dict_max, packed + p, 4); p += 4;
    } else {
        /* v4 backward compat */
        memcpy(&spc, packed + p, 4); p += 4;
        memcpy(&sr, packed + p, 4); p += 4;
        memcpy(&nf, packed + p, 4); p += 4;
        memcpy(&md_sz, packed + p, 2); p += 2;
        memcpy(&sd_sz, packed + p, 2); p += 2;
        ch = packed[p++]; if (ch < 1) ch = 1; if (ch > 2) ch = 2;
        memcpy(&mid_dict_max, packed + p, 4); p += 4;
        memcpy(&side_dict_max, packed + p, 4); p += 4;
    }

    int fb = FREQ_BINS;

    /* Read dictionaries */
    float *mid_dict = (float *)malloc(md_sz * fb * sizeof(float));
    float *side_dict = (float *)malloc(sd_sz * fb * sizeof(float));
    if (!mid_dict || !side_dict) {
        free(packed); free(mid_dict); free(side_dict);
        return QTSQ_ERR_ALLOC;
    }

    for (int i = 0; i < md_sz * fb; i++) {
        uint16_t v = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
        mid_dict[i] = is_v5 ? u16_to_mag_log(v, mid_dict_max)
                            : ((float)v / 65535.0f) * mid_dict_max; /* v4 linear */
    }
    for (int i = 0; i < sd_sz * fb; i++) {
        uint16_t v = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
        side_dict[i] = is_v5 ? u16_to_mag_log(v, side_dict_max)
                             : ((float)v / 65535.0f) * side_dict_max;
    }

    /* Read timelines */
    uint16_t *m_idx = (uint16_t *)malloc(nf * sizeof(uint16_t));
    uint16_t *s_idx = (uint16_t *)malloc(nf * sizeof(uint16_t));
    uint16_t *m_bgains = (uint16_t *)calloc(nf * num_bands, sizeof(uint16_t));
    uint16_t *s_bgains = (uint16_t *)calloc(nf * num_bands, sizeof(uint16_t));

    for (uint32_t f = 0; f < nf; f++) {
        m_idx[f] = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
    }
    if (is_v5) {
        /* v5: per-band gains */
        for (uint32_t f = 0; f < nf; f++) {
            for (int b = 0; b < num_bands; b++) {
                m_bgains[f * num_bands + b] = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
            }
        }
    } else {
        /* v4: single gain per frame */
        for (uint32_t f = 0; f < nf; f++) {
            uint16_t g = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
            for (int b = 0; b < num_bands; b++) m_bgains[f * num_bands + b] = g;
        }
    }

    for (uint32_t f = 0; f < nf; f++) {
        s_idx[f] = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
    }
    if (is_v5) {
        for (uint32_t f = 0; f < nf; f++) {
            for (int b = 0; b < num_bands; b++) {
                s_bgains[f * num_bands + b] = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
            }
        }
    } else {
        for (uint32_t f = 0; f < nf; f++) {
            uint16_t g = ((uint16_t)packed[p] << 8) | packed[p + 1]; p += 2;
            for (int b = 0; b < num_bands; b++) s_bgains[f * num_bands + b] = g;
        }
    }

    /* Phase deviations (v5 only — 4-bit packed, PHASE_BINS bins) */
    uint32_t phase_bytes_per_frame_dec = (PHASE_BINS + 1) / 2;
    uint32_t phase_total_dec = nf * phase_bytes_per_frame_dec;
    uint8_t *mid_phase_packed = NULL, *side_phase_packed = NULL;
    if (is_v5) {
        mid_phase_packed = (uint8_t *)malloc(phase_total_dec);
        side_phase_packed = (uint8_t *)malloc(phase_total_dec);
        memcpy(mid_phase_packed, packed + p, phase_total_dec); p += phase_total_dec;
        memcpy(side_phase_packed, packed + p, phase_total_dec); p += phase_total_dec;
    }
    free(packed);

    /* Resolve band edges for this num_bands */
    int band_edges_dec[17]; /* max 16 bands + 1 edge */
    if (num_bands == NUM_BANDS) {
        for (int i = 0; i <= NUM_BANDS; i++) band_edges_dec[i] = BAND_EDGES[i];
    } else {
        /* Uniform split fallback */
        for (int i = 0; i <= num_bands; i++)
            band_edges_dec[i] = (int)((float)i / num_bands * fb);
    }

    /* Reconstruct magnitude spectrograms with per-band gains */
    float *mid_mag  = (float *)calloc(nf * fb, sizeof(float));
    float *side_mag = (float *)calloc(nf * fb, sizeof(float));
    if (!mid_mag || !side_mag) {
        free(mid_dict); free(side_dict);
        free(m_idx); free(s_idx); free(m_bgains); free(s_bgains);
        free(mid_phase_packed); free(side_phase_packed);
        free(mid_mag); free(side_mag);
        return QTSQ_ERR_ALLOC;
    }

    for (uint32_t f = 0; f < nf; f++) {
        int mi = m_idx[f];
        if (mi < md_sz) {
            for (int b = 0; b < num_bands; b++) {
                float mg = decode_gain(m_bgains[f * num_bands + b]);
                int lo = band_edges_dec[b], hi = band_edges_dec[b + 1];
                for (int k = lo; k < hi && k < fb; k++)
                    mid_mag[f * fb + k] = mid_dict[mi * fb + k] * mg;
            }
        }

        int si = s_idx[f];
        if (si < sd_sz) {
            for (int b = 0; b < num_bands; b++) {
                float sg = decode_gain(s_bgains[f * num_bands + b]);
                int lo = band_edges_dec[b], hi = band_edges_dec[b + 1];
                for (int k = lo; k < hi && k < fb; k++)
                    side_mag[f * fb + k] = side_dict[si * fb + k] * sg;
            }
        }
    }
    free(mid_dict); free(side_dict);
    free(m_idx); free(s_idx); free(m_bgains); free(s_bgains);

    /* Reconstruct phase */
    float *mid_phase_out  = (float *)calloc(nf * fb, sizeof(float));
    float *side_phase_out = (float *)calloc(nf * fb, sizeof(float));

    if (is_v5 && mid_phase_packed && side_phase_packed) {
        /* v5: reconstruct low-freq phase from stored deviations,
         * synthesize high-freq phase via standard advance */
        float *phase_inc = (float *)malloc(fb * sizeof(float));
        for (int k = 0; k < fb; k++)
            phase_inc[k] = 2.0f * (float)M_PI * (float)k * (float)HOP_SIZE / (float)FFT_SIZE;

        float *prev_mid  = (float *)calloc(fb, sizeof(float));
        float *prev_side = (float *)calloc(fb, sizeof(float));

        for (uint32_t f = 0; f < nf; f++) {
            /* Low-frequency bins: use stored phase deviations */
            for (int k = 0; k < PHASE_BINS && k < fb; k++) {
                uint32_t byte_idx = f * phase_bytes_per_frame_dec + (k / 2);
                uint8_t nibble_m, nibble_s;
                if (k % 2 == 0) {
                    nibble_m = (mid_phase_packed[byte_idx] >> 4) & 0x0F;
                    nibble_s = (side_phase_packed[byte_idx] >> 4) & 0x0F;
                } else {
                    nibble_m = mid_phase_packed[byte_idx] & 0x0F;
                    nibble_s = side_phase_packed[byte_idx] & 0x0F;
                }

                float expected_mid = prev_mid[k] + phase_inc[k];
                float dev_mid = decode_phase_dev(nibble_m);
                mid_phase_out[f * fb + k] = expected_mid + dev_mid;
                prev_mid[k] = mid_phase_out[f * fb + k];

                float expected_side = prev_side[k] + phase_inc[k];
                float dev_side = decode_phase_dev(nibble_s);
                side_phase_out[f * fb + k] = expected_side + dev_side;
                prev_side[k] = side_phase_out[f * fb + k];
            }
            /* High-frequency bins: standard phase advance (vocoder) */
            for (int k = PHASE_BINS; k < fb; k++) {
                mid_phase_out[f * fb + k] = prev_mid[k] + phase_inc[k];
                prev_mid[k] = mid_phase_out[f * fb + k];
                side_phase_out[f * fb + k] = prev_side[k] + phase_inc[k];
                prev_side[k] = side_phase_out[f * fb + k];
            }
        }
        free(phase_inc); free(prev_mid); free(prev_side);
    } else {
        /* v4 fallback: phase vocoder (synthesized phase) */
        float *phase_inc = (float *)malloc(fb * sizeof(float));
        for (int k = 0; k < fb; k++)
            phase_inc[k] = 2.0f * (float)M_PI * (float)k * (float)HOP_SIZE / (float)FFT_SIZE;

        float *cur_phase = (float *)calloc(fb, sizeof(float));
        for (uint32_t f = 0; f < nf; f++) {
            for (int k = 0; k < fb; k++) {
                mid_phase_out[f * fb + k] = cur_phase[k];
                side_phase_out[f * fb + k] = cur_phase[k];
                cur_phase[k] += phase_inc[k];
            }
        }
        free(phase_inc); free(cur_phase);
    }
    free(mid_phase_packed); free(side_phase_packed);

    /* iSTFT synthesis using stored/reconstructed phase */
    float *mid_audio  = (float *)calloc(spc, sizeof(float));
    float *side_audio = (float *)calloc(spc, sizeof(float));
    if (!mid_audio || !side_audio) {
        free(mid_mag); free(side_mag);
        free(mid_phase_out); free(side_phase_out);
        free(mid_audio); free(side_audio);
        return QTSQ_ERR_ALLOC;
    }

    istft_from_phase(mid_mag, mid_phase_out, nf, mid_audio, spc);
    istft_from_phase(side_mag, side_phase_out, nf, side_audio, spc);
    free(mid_mag); free(side_mag);
    free(mid_phase_out); free(side_phase_out);

    /* Build stereo WAV */
    uint16_t out_ch = (ch >= 2) ? 2 : 1;
    uint32_t pcm_bytes = spc * out_ch * 2;
    uint32_t wav_size = 44 + pcm_bytes;
    uint8_t *wav = (uint8_t *)malloc(wav_size);
    if (!wav) { free(mid_audio); free(side_audio); return QTSQ_ERR_ALLOC; }

    uint32_t fsz = wav_size - 8, brate = sr * out_ch * 2, fc = 16;
    uint16_t af = 1, ba = out_ch * 2, bits = 16;
    memcpy(wav, "RIFF", 4);       memcpy(wav + 4, &fsz, 4);
    memcpy(wav + 8, "WAVE", 4);   memcpy(wav + 12, "fmt ", 4);
    memcpy(wav + 16, &fc, 4);     memcpy(wav + 20, &af, 2);
    memcpy(wav + 22, &out_ch, 2); memcpy(wav + 24, &sr, 4);
    memcpy(wav + 28, &brate, 4);  memcpy(wav + 32, &ba, 2);
    memcpy(wav + 34, &bits, 2);   memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &pcm_bytes, 4);

    int16_t *pcm = (int16_t *)(wav + 44);
    for (uint32_t i = 0; i < spc; i++) {
        float m = mid_audio[i], s = side_audio[i];
        float L = m + s, R = m - s;
        if (L > 32767) L = 32767; if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; if (R < -32768) R = -32768;
        if (out_ch == 2) {
            pcm[i * 2] = (int16_t)L; pcm[i * 2 + 1] = (int16_t)R;
        } else pcm[i] = (int16_t)m;
    }
    free(mid_audio); free(side_audio);

    *out_data = wav;
    *out_size = wav_size;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Generic Audio Decompression — auto-detects codec
 *
 *  Dispatches to the correct decoder based on strategy:
 *    QTSQ_STRATEGY_FOURIER → v5 Dictionary/Spectral decoder
 *    QTSQ_STRATEGY_PHOTON  → Photon HQ decoder (outputs float32)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_audio(const qtsq_context_t *ctx,
                          int16_t **out_pcm, size_t *out_samples,
                          uint32_t *out_sample_rate, uint8_t *out_channels) {
    if (!ctx || !out_pcm || !out_samples) return QTSQ_ERR_NULL;
    *out_pcm = NULL;
    *out_samples = 0;
    if (out_sample_rate) *out_sample_rate = 0;
    if (out_channels) *out_channels = 0;

#ifdef QTSQ_HAVE_OPUS
    if (ctx->header.strategy == QTSQ_STRATEGY_OPUS) {
        return qtsq_decompress_audio_opus(ctx, out_pcm, out_samples,
                                           out_sample_rate, out_channels);
    }
#endif

    if (ctx->header.strategy == QTSQ_STRATEGY_PHOTON) {
        /* Photon HQ: decode float32, then convert to int16 */
        float *float_pcm = NULL;
        size_t total_samples = 0;
        int result = qtsq_decompress_audio_hq(ctx, &float_pcm, &total_samples, NULL, NULL);
        if (result != QTSQ_OK) return result;

        int16_t *int16_pcm = (int16_t *)malloc(total_samples * sizeof(int16_t));
        if (!int16_pcm) { free(float_pcm); return QTSQ_ERR_ALLOC; }

        for (size_t i = 0; i < total_samples; i++) {
            float s = float_pcm[i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            int16_pcm[i] = (int16_t)(s * 32767.0f);
        }
        free(float_pcm);

        *out_pcm = int16_pcm;
        *out_samples = total_samples;
        if (out_sample_rate) *out_sample_rate = ctx->schema.sample_rate;
        if (out_channels) *out_channels = (uint8_t)ctx->header.num_channels;
        return QTSQ_OK;
    }

    /* Short-audio lossless fallback (see qtsq_compress_audio): raw int16
     * payload, zlib-compressed with a [u32 raw_bytes] prefix. Also covers
     * qtsq_compress_signal, which re-tags the type to SIGNAL_1D. */
    if (ctx->header.strategy == QTSQ_STRATEGY_PROCEDURAL &&
        (ctx->header.data_type == QTSQ_TYPE_AUDIO_MONO ||
         ctx->header.data_type == QTSQ_TYPE_AUDIO_STEREO ||
         ctx->header.data_type == QTSQ_TYPE_AUDIO_MULTI ||
         ctx->header.data_type == QTSQ_TYPE_SIGNAL_1D)) {
        /* Phase 6b: float32 payloads use qtsq_decompress_audio_float. */
        if (ctx->header.sample_depth == 32) return QTSQ_ERR_FORMAT;
        if (!ctx->singularity || ctx->singularity_size < 5) return QTSQ_ERR_FORMAT;
        uint32_t raw_bytes;
        memcpy(&raw_bytes, ctx->singularity, 4);
        if ((uint64_t)raw_bytes > (1ULL << 30)) return QTSQ_ERR_FORMAT;
        /* All-zero/silent clips legitimately compress at extremely high
         * ratios, so use a generous cap (raw_bytes is bounded by 2^30
         * above, and the compressed input must be at least 1 byte). */
        if ((uint64_t)raw_bytes > (ctx->singularity_size - 4) * 4096ULL)
            return QTSQ_ERR_FORMAT;
        const uint8_t *body = ctx->singularity + 4;
        size_t body_len = ctx->singularity_size - 4;

        if (qtsq_codec_is_qtc(body, body_len)) {
            /* The declared original size is raw_bytes for both the typed
             * AUDIO lens and the generic stream, so matching it against
             * raw_bytes also bounds the decode allocation. */
            uint64_t declared = qtsq_codec_declared_size(body, body_len);
            if (declared != (uint64_t)raw_bytes) return QTSQ_ERR_FORMAT;

            /* Phase 7: ragged clips are stored as a generic qtc stream;
             * whole-frame clips use the typed AUDIO lens. Route on the
             * container lens byte. */
            if (body_len >= QTSQ_QTC_HEADER_SIZE &&
                body[4] != QTC_LENS_AUDIO) {
                uint8_t *pcm_bytes = NULL;
                size_t out_len = 0;
                if (qtsq_codec_decompress(body, body_len, raw_bytes,
                                          &pcm_bytes, &out_len) != QTSQ_OK ||
                    out_len != raw_bytes) {
                    free(pcm_bytes);
                    return QTSQ_ERR_FORMAT;
                }
                *out_pcm = (int16_t *)pcm_bytes;
                *out_samples = raw_bytes / sizeof(int16_t);
                if (out_sample_rate) *out_sample_rate = ctx->schema.sample_rate;
                if (out_channels) *out_channels = (uint8_t)ctx->header.num_channels;
                return QTSQ_OK;
            }

            /* Phase 6a qtc AUDIO lens. The declared original size is
             * nframes*ch*(bps/8) for the audio lens, so matching it against
             * raw_bytes also bounds the decode allocation. */
            uint8_t *pcm_bytes = NULL;
            uint32_t nframes = 0;
            uint8_t nch = 0, bps = 0;
            int qrc = qtc_decompress_audio(body, body_len, &pcm_bytes,
                                           &nframes, &nch, &bps);
            if (qrc != QTC_OK || !pcm_bytes) {
                free(pcm_bytes);
                return QTSQ_ERR_FORMAT;
            }
            if (bps != 16 ||
                (uint64_t)nframes * nch * 2 != (uint64_t)raw_bytes) {
                free(pcm_bytes);
                return QTSQ_ERR_FORMAT;
            }
            *out_pcm = (int16_t *)pcm_bytes;
            *out_samples = raw_bytes / sizeof(int16_t);
            if (out_sample_rate) *out_sample_rate = ctx->schema.sample_rate;
            if (out_channels) *out_channels = (uint8_t)ctx->header.num_channels;
            return QTSQ_OK;
        }

        int16_t *pcm = (int16_t *)malloc(raw_bytes);
        if (!pcm) return QTSQ_ERR_ALLOC;
        uLongf dl = (uLongf)raw_bytes;
        if (uncompress((uint8_t *)pcm, &dl, body, (uLong)body_len) != Z_OK ||
            dl != raw_bytes) {
            free(pcm);
            return QTSQ_ERR_FORMAT;
        }
        *out_pcm = pcm;
        *out_samples = dl / sizeof(int16_t);
        if (out_sample_rate) *out_sample_rate = ctx->schema.sample_rate;
        if (out_channels) *out_channels = (uint8_t)ctx->header.num_channels;
        return QTSQ_OK;
    }

    /* Default: v5 Dictionary/Spectral — outputs WAV, extract PCM */
    uint8_t *wav_data = NULL;
    size_t wav_size = 0;
    int result = qtsq_decompress_lensing(ctx, &wav_data, &wav_size);
    if (result != QTSQ_OK) return result;

    /* Parse WAV header to extract PCM */
    if (wav_size < 44 || wav_data[0] != 'R' || wav_data[1] != 'I' ||
        wav_data[2] != 'F' || wav_data[3] != 'F') {
        free(wav_data);
        return QTSQ_ERR_FORMAT;
    }

    uint16_t bits_per_sample = *(uint16_t *)(wav_data + 34);
    uint16_t num_channels = *(uint16_t *)(wav_data + 22);
    uint32_t sample_rate_val = *(uint32_t *)(wav_data + 24);
    uint32_t data_size = *(uint32_t *)(wav_data + 40);
    size_t pcm_bytes = wav_size - 44;

    if (pcm_bytes > data_size) pcm_bytes = data_size;

    if (bits_per_sample == 16) {
        size_t num_samples = pcm_bytes / sizeof(int16_t);
        int16_t *pcm = (int16_t *)malloc(pcm_bytes);
        if (!pcm) { free(wav_data); return QTSQ_ERR_ALLOC; }
        memcpy(pcm, wav_data + 44, pcm_bytes);
        *out_pcm = pcm;
        *out_samples = num_samples;
    } else {
        free(wav_data);
        return QTSQ_ERR_FORMAT;
    }

    if (out_sample_rate) *out_sample_rate = sample_rate_val;
    if (out_channels) *out_channels = (uint8_t)num_channels;

    free(wav_data);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 6b: float32 audio capture path (microphone / DSP)
 * ══════════════════════════════════════════════════════════════
 * Framing mirrors store_raw_int16: [u32 raw_bytes][stream]. With the qtc
 * codec the AUDIO_FLOAT lens stores every IEEE-754 bit pattern exactly
 * (or routes through the lossy int16-MDCT2 adapter when quality > 0);
 * the default codec keeps raw floats under zlib. Decode sniffs the qtc
 * magic, so both streams share one layout. */

int qtsq_compress_audio_float(qtsq_context_t *ctx, const float *samples,
                              size_t num_samples, uint32_t sample_rate,
                              uint8_t channels) {
    if (!ctx || !samples) return QTSQ_ERR_NULL;
    if (num_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (channels == 0) channels = 1;
    /* Standard audio is stereo; mono is a fallback for mono-only sources
     * (microphones, mono files), never a second-class copy of a stereo
     * signal. Rejecting >2 keeps the interleaving interpretable. */
    if (channels > 2) return QTSQ_ERR_FORMAT;
    if (num_samples % channels != 0) return QTSQ_ERR_FORMAT;
    uint32_t nframes = (uint32_t)(num_samples / channels);

    /* Check in 64-bit before multiplying: on a 32-bit build the size_t
     * product can wrap to a small value that then passes the u32 test. */
    if ((uint64_t)num_samples * sizeof(float) > UINT32_MAX)
        return QTSQ_ERR_TOO_LARGE;
    size_t raw_bytes = num_samples * sizeof(float);
    /* Phase 7: raised from 1 GB toward qtc's ceiling now that the LZ layer
     * handles large streams. The [u32 raw_bytes] framing bounds the payload
     * below 4 GB; qtc's audio-float lens itself caps at 2^30 samples. */
    if ((uint64_t)raw_bytes >= (1ULL << 32)) return QTSQ_ERR_TOO_LARGE;

    uint8_t *body = NULL;
    size_t body_len = 0;
    int lossy = 0;

    if (ctx->codec == QTSQ_CODEC_QTC) {
        qtc_options_t opts;
        qtc_options_default(&opts);
        /* The MDCT2 rate controller needs the real stream rate. 0 means
         * "assume 44.1 kHz", which made every 48/96 kHz capture larger
         * than the selected tier. */
        opts.sample_rate = (int)sample_rate;
        if (ctx->quality > 0.0f) {
            opts.mode = QTC_MODE_LOSSY;
            opts.quality = ctx->quality;
            lossy = 1;
        }
        int qrc = qtc_compress_audio_float(samples, nframes, channels,
                                           &body, &body_len, &opts);
        if (qrc != QTC_OK || !body) {
            free(body);
            return (qrc == QTC_ERR_ALLOC) ? QTSQ_ERR_ALLOC : QTSQ_ERR_FORMAT;
        }
    } else {
        uLongf zbound = compressBound((uLong)raw_bytes);
        body = (uint8_t *)malloc(zbound);
        if (!body) return QTSQ_ERR_ALLOC;
        uLongf zlen = zbound;
        if (compress2(body, &zlen, (const uint8_t *)samples,
                      (uLong)raw_bytes, 9) != Z_OK) {
            free(body);
            return QTSQ_ERR_ALLOC;
        }
        body_len = (size_t)zlen;
    }

    uint8_t *out = (uint8_t *)malloc(4 + body_len);
    if (!out) { free(body); return QTSQ_ERR_ALLOC; }
    uint32_t raw32 = (uint32_t)raw_bytes;
    memcpy(out, &raw32, 4);
    memcpy(out + 4, body, body_len);
    free(body);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = 4 + body_len;
    ctx->header.strategy = QTSQ_STRATEGY_PROCEDURAL;
    ctx->header.data_type = (channels == 2) ? QTSQ_TYPE_AUDIO_STEREO
                                             : QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = raw_bytes;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 32;
    ctx->header.singularity_size = ctx->singularity_size;
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "audio_float", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.num_dims = 1;
    ctx->schema.dimensions[0] = (uint32_t)num_samples;
    ctx->schema.sample_rate = sample_rate;
    strncpy(ctx->schema.encoding, "RAW_FLOAT32", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);
    if (lossy) {
        ctx->header.flags |= QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED;
    } else {
        ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    }
    qtsq_sha256((const uint8_t *)samples, raw_bytes, ctx->header.checksum);
    return QTSQ_OK;
}

int qtsq_decompress_audio_float(const qtsq_context_t *ctx,
                                float **out_pcm, size_t *out_samples,
                                uint32_t *out_sample_rate, uint8_t *out_channels) {
    if (!ctx || !out_pcm || !out_samples) return QTSQ_ERR_NULL;
    *out_pcm = NULL;
    *out_samples = 0;
    if (ctx->header.sample_depth != 32 ||
        ctx->header.strategy != QTSQ_STRATEGY_PROCEDURAL)
        return QTSQ_ERR_FORMAT;
    if (!ctx->singularity || ctx->singularity_size < 5) return QTSQ_ERR_FORMAT;

    uint32_t raw_bytes;
    memcpy(&raw_bytes, ctx->singularity, 4);
    /* Phase 7: the 1 GB cap is gone; raw_bytes is u32-bounded by the framing
     * and the ratio cap below keeps the decode allocation in check. */
    if ((uint64_t)raw_bytes > (ctx->singularity_size - 4) * 4096ULL)
        return QTSQ_ERR_FORMAT;
    if (raw_bytes % sizeof(float) != 0) return QTSQ_ERR_FORMAT;

    const uint8_t *body = ctx->singularity + 4;
    size_t body_len = ctx->singularity_size - 4;
    float *pcm = NULL;
    size_t pcm_bytes = 0;

    if (qtsq_codec_is_qtc(body, body_len)) {
        uint64_t declared = qtsq_codec_declared_size(body, body_len);
        if (declared != (uint64_t)raw_bytes) return QTSQ_ERR_FORMAT;
        uint32_t nframes = 0;
        uint8_t nch = 0;
        int qrc = qtc_decompress_audio_float(body, body_len, &pcm,
                                             &nframes, &nch);
        if (qrc != QTC_OK || !pcm) {
            free(pcm);
            return QTSQ_ERR_FORMAT;
        }
        pcm_bytes = (size_t)nframes * nch * sizeof(float);
        if (pcm_bytes != raw_bytes) {
            free(pcm);
            return QTSQ_ERR_FORMAT;
        }
    } else {
        pcm = (float *)malloc(raw_bytes);
        if (!pcm) return QTSQ_ERR_ALLOC;
        uLongf dl = (uLongf)raw_bytes;
        if (uncompress((uint8_t *)pcm, &dl, body, (uLong)body_len) != Z_OK ||
            dl != raw_bytes) {
            free(pcm);
            return QTSQ_ERR_FORMAT;
        }
        pcm_bytes = raw_bytes;
    }

    /* Lossless streams are checksummed over the original floats; lossy
     * ones (LOSSY flag) cannot match and skip verification. */
    if ((ctx->header.flags & QTSQ_FLAG_CHECKSUMMED) &&
        !(ctx->header.flags & QTSQ_FLAG_LOSSY)) {
        uint8_t computed[32];
        qtsq_sha256((const uint8_t *)pcm, pcm_bytes, computed);
        if (memcmp(computed, ctx->header.checksum, 32) != 0) {
            free(pcm);
            return QTSQ_ERR_CORRUPT;
        }
    }

    *out_pcm = pcm;
    *out_samples = pcm_bytes / sizeof(float);
    if (out_sample_rate) *out_sample_rate = ctx->schema.sample_rate;
    if (out_channels) *out_channels = (uint8_t)ctx->header.num_channels;
    return QTSQ_OK;
}
