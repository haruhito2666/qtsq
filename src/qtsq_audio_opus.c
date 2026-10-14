/*
 * Quantum Tensor Sequence — Opus Audio Codec (Phase 9)
 *
 * Creative-intent, lossy audio codec backed by the standard Opus codec
 * (RFC 6716).  Open, royalty-free, beats MP3.  Default Creative path.
 *
 * CONDITIONALLY compiled (guarded by QTSQ_HAVE_OPUS, defined by the Makefile
 * when libopus is present).  When libopus is absent the body compiles to an
 * empty translation unit, so glob-based platform builds (iOS/WASM/Android)
 * stay green, and qtsq_compress_audio falls back to the in-house spectral
 * codec.
 *
 * Singularity framing (self-contained, no external parse deps):
 *   [4B magic "OPS1"][4B sample_rate][1B channels][4B num_frames]
 *   [4B num_packets][ each packet: 4B len + bytes ]
 * where num_frames = total non-interleaved source frames (per channel).
 *
 * Author: Haruhito
 */
#include <string.h>
#include <stdlib.h>

/* Whole body compiles only when libopus is available. Glob-based platform
 * builds (iOS/WASM/Android) include this file unconditionally but do not
 * define QTSQ_HAVE_OPUS, so it becomes an empty translation unit there. */
#ifdef QTSQ_HAVE_OPUS

#include <opus/opus.h>
#include "qtsq.h"
#include "qtsq_opus.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OPUS_FRAME_MS      20          /* 20 ms frames (Opus default) */
#define OPUS_MAX_PACKET    4000        /* large enough for any Opus frame */
#define OPUS_MAGIC0       'O'
#define OPUS_MAGIC1       'P'
#define OPUS_MAGIC2       'S'
#define OPUS_MAGIC3       '1'

/* Nearest supported Opus sample rate (Opus supports only these). */
static uint32_t opus_map_rate(uint32_t sr) {
    const uint32_t rates[] = {48000, 24000, 16000, 12000, 8000};
    uint32_t best = 48000;
    uint32_t best_delta = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(rates)/sizeof(rates[0]); i++) {
        uint32_t d = sr > rates[i] ? sr - rates[i] : rates[i] - sr;
        if (d < best_delta) { best_delta = d; best = rates[i]; }
    }
    return best;
}

int qtsq_compress_audio_opus(qtsq_context_t *ctx,
                             const int16_t *samples,
                             size_t num_samples,
                             uint32_t sample_rate,
                             uint8_t channels) {
    if (!ctx || !samples) return QTSQ_ERR_NULL;
    if (num_samples == 0) return QTSQ_ERR_TOO_SMALL;
    if (channels > 2) channels = 2;   /* down-mix to stereo max */
    if (channels == 0) channels = 1;

    uint32_t rate = opus_map_rate(sample_rate);
    int frame_size = (int)(rate * OPUS_FRAME_MS / 1000);   /* 20 ms */
    if (frame_size <= 0) frame_size = 960;

    int err = 0;
    OpusEncoder *enc = opus_encoder_create((opus_int32)rate, channels,
                                            OPUS_APPLICATION_AUDIO, &err);
    if (!enc) return QTSQ_ERR_ALLOC;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    size_t frames = num_samples / channels;              /* per-channel */
    size_t packets = (frames + (size_t)frame_size - 1) / (size_t)frame_size;
    if (packets == 0) packets = 1;

    /* Worst-case output: header + per-packet (4B len + OPUS_MAX_PACKET). */
    size_t cap = 4 + 4 + 1 + 4 + 4 + packets * (4 + OPUS_MAX_PACKET);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint8_t *pkt_buf = (uint8_t *)malloc(OPUS_MAX_PACKET);
    if (!out || !pkt_buf) {
        if (out) free(out);
        if (pkt_buf) free(pkt_buf);
        opus_encoder_destroy(enc);
        return QTSQ_ERR_ALLOC;
    }

    size_t p = 0;
    out[p++] = OPUS_MAGIC0; out[p++] = OPUS_MAGIC1;
    out[p++] = OPUS_MAGIC2; out[p++] = OPUS_MAGIC3;
    memcpy(out + p, &rate, 4); p += 4;
    out[p++] = (uint8_t)channels;
    uint32_t frames32 = (uint32_t)frames;
    memcpy(out + p, &frames32, 4); p += 4;
    uint32_t packets32 = (uint32_t)packets;
    memcpy(out + p, &packets32, 4); p += 4;

    /* Encode frame by frame.  The last partial frame is zero-padded (Opus
     * accepts it; decoder returns the real frame count). */
    for (size_t k = 0; k < packets; k++) {
        int16_t frame_buf[2 * 2880];    /* 20ms@48k * 2ch = 1920 max; pad */
        memset(frame_buf, 0, sizeof(frame_buf));

        size_t src_off = k * (size_t)frame_size * channels;
        size_t avail = frames - k * (size_t)frame_size;   /* per-channel remain */
        size_t this_frames = avail < (size_t)frame_size ? avail : (size_t)frame_size;
        size_t copy_samps = this_frames * channels;
        if (src_off < num_samples)
            memcpy(frame_buf, samples + src_off, copy_samps * sizeof(int16_t));

        opus_int32 nb = opus_encode(enc, frame_buf, frame_size,
                                    pkt_buf, OPUS_MAX_PACKET);
        if (nb < 0) {
            free(out); free(pkt_buf); opus_encoder_destroy(enc);
            return QTSQ_ERR_FORMAT;
        }
        uint32_t len32 = (uint32_t)nb;
        memcpy(out + p, &len32, 4); p += 4;
        memcpy(out + p, pkt_buf, (size_t)nb); p += (size_t)nb;
    }

    free(pkt_buf);
    opus_encoder_destroy(enc);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = p;
    ctx->header.singularity_size = p;
    ctx->header.strategy = QTSQ_STRATEGY_OPUS;
    ctx->header.data_type = (channels >= 2) ? QTSQ_TYPE_AUDIO_STEREO : QTSQ_TYPE_AUDIO_MONO;
    ctx->header.original_size = num_samples * sizeof(int16_t);
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 16;
    ctx->header.flags |= (QTSQ_FLAG_LOSSY | QTSQ_FLAG_CHECKSUMMED);

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "opus_audio", 63);
    ctx->schema.data_type = ctx->header.data_type;
    ctx->schema.dimensions[0] = (uint32_t)frames;
    ctx->schema.num_dims = 1;
    ctx->schema.sample_rate = rate;
    strncpy(ctx->schema.encoding, "OPUS", 63);
    ctx->header.horizon_size = sizeof(qtsq_schema_t);

    qtsq_sha256((const uint8_t *)samples, num_samples * sizeof(int16_t),
                ctx->header.checksum);
    return QTSQ_OK;
}

int qtsq_decompress_audio_opus(const qtsq_context_t *ctx,
                                int16_t **out_pcm,
                                size_t *out_num_samples,
                                uint32_t *out_sample_rate,
                                uint8_t *out_channels) {
    if (!ctx || !out_pcm || !out_num_samples) return QTSQ_ERR_NULL;
    *out_pcm = NULL;
    *out_num_samples = 0;
    if (out_sample_rate) *out_sample_rate = 0;
    if (out_channels) *out_channels = 0;

    if (!ctx->singularity || ctx->singularity_size < 17) return QTSQ_ERR_FORMAT;
    const uint8_t *s = ctx->singularity;
    if (s[0] != OPUS_MAGIC0 || s[1] != OPUS_MAGIC1 ||
        s[2] != OPUS_MAGIC2 || s[3] != OPUS_MAGIC3) return QTSQ_ERR_FORMAT;

    size_t p = 4;
    uint32_t rate; memcpy(&rate, s + p, 4); p += 4;
    uint8_t channels = s[p++];
    uint32_t frames; memcpy(&frames, s + p, 4); p += 4;
    uint32_t packets; memcpy(&packets, s + p, 4); p += 4;
    if (channels < 1 || channels > 2) return QTSQ_ERR_FORMAT;

    int err = 0;
    OpusDecoder *dec = opus_decoder_create((opus_int32)rate, channels, &err);
    if (!dec) return QTSQ_ERR_ALLOC;

    int frame_size = (int)(rate * OPUS_FRAME_MS / 1000);
    if (frame_size <= 0) frame_size = 960;

    /* Upper bound output = frames * channels. */
    size_t out_cap = (size_t)frames * channels;
    int16_t *pcm = (int16_t *)calloc(out_cap ? out_cap : 1, sizeof(int16_t));
    if (!pcm) { opus_decoder_destroy(dec); return QTSQ_ERR_ALLOC; }

    size_t written_frames = 0;
    for (uint32_t k = 0; k < packets; k++) {
        if (p + 4 > ctx->singularity_size) { free(pcm); opus_decoder_destroy(dec); return QTSQ_ERR_FORMAT; }
        uint32_t len; memcpy(&len, s + p, 4); p += 4;
        if (p + len > ctx->singularity_size) { free(pcm); opus_decoder_destroy(dec); return QTSQ_ERR_FORMAT; }

        opus_int32 got = (len > 0)
            ? opus_decode(dec, s + p, (opus_int32)len, pcm + written_frames * channels, frame_size, 0)
            : opus_decode(dec, NULL, 0, pcm + written_frames * channels, frame_size, 0);
        p += len;
        if (got < 0) { free(pcm); opus_decoder_destroy(dec); return QTSQ_ERR_FORMAT; }
        written_frames += (size_t)got;
        if (written_frames > frames) written_frames = frames; /* clamp */
    }

    opus_decoder_destroy(dec);
    *out_pcm = pcm;
    *out_num_samples = written_frames * channels;
    if (out_sample_rate) *out_sample_rate = rate;
    if (out_channels) *out_channels = channels;
    return QTSQ_OK;
}

#endif /* QTSQ_HAVE_OPUS */
