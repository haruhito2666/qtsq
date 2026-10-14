/*
 * Quantum Tensor Sequence — Photon Audio HQ Decoder (v4)
 * Hybrid Lossless PCM + Per-Atom Spatial Metadata
 *
 * Architecture:
 *   1. Lossless Layer: zlib -> 2nd-order delta int32 -> float32 -> L/R stereo decode
 *   2. Spatial Layer: extract spatial atom metadata for renderer
 *
 * Portable: no platform-specific code, no debug logging, no dependencies beyond zlib.
 *
 * Author: Haruhito
 */
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include "qtsq_codec.h"
#include "qtsq.h"

/* ═══════════ 2nd-Order Delta Decoding ═══════════ */
static void delta_decode_int32_to_float32(const int32_t *input, float *output, uint32_t count) {
    if (count == 0) return;

    const float inv_scale = 1.0f / 8388607.0f;

    int32_t dec0 = input[0];
    output[0] = (float)dec0 * inv_scale;
    if (count == 1) return;

    int32_t dec1 = dec0 + input[1];
    output[1] = (float)dec1 * inv_scale;

    int32_t p0 = dec0;
    int32_t p1 = dec1;

    for (uint32_t i = 2; i < count; i++) {
        int32_t predicted = 2 * p1 - p0;
        int32_t val = predicted + input[i];
        output[i] = (float)val * inv_scale;
        p0 = p1;
        p1 = val;
    }
}

/* ═══════════ DECODER ═══════════ */
int qtsq_decompress_audio_hq(const qtsq_context_t *ctx,
                              float **out_pcm, size_t *out_total_samples,
                              photon_atom_t **out_atoms, size_t *out_atom_count)
{
    if (!ctx || !out_pcm || !out_total_samples) {
        return QTSQ_ERR_NULL;
    }
    *out_pcm = NULL;
    *out_total_samples = 0;
    if (out_atoms) *out_atoms = NULL;
    if (out_atom_count) *out_atom_count = 0;

    if (!ctx->singularity || ctx->singularity_size < 8) {
        return QTSQ_ERR_FORMAT;
    }

    /* Decompress outer zlib payload */
    uint32_t raw_size;
    memcpy(&raw_size, ctx->singularity, 4);

    /* SEC: raw_size comes from the file — bound it before malloc.
     * The compressed payload is (singularity_size - 4) bytes; a decompressed
     * buffer more than 64x the compressed size is never legitimate for this
     * codec (delta PCM + sparse atoms). Also reject absurd absolute sizes. */
    if (raw_size > (ctx->singularity_size - 4) * 64ULL ||
        raw_size > 1ULL << 31) {
        return QTSQ_ERR_FORMAT;
    }

    uint8_t *raw = NULL;
    uLongf dl = 0;
    if (qtsq_codec_is_qtc(ctx->singularity + 4, ctx->singularity_size - 4)) {
        /* qtc stream (opt-in): declared size must match the framed size. */
        if (qtsq_codec_declared_size(ctx->singularity + 4,
                                     ctx->singularity_size - 4) != raw_size) {
            return QTSQ_ERR_FORMAT;
        }
        size_t dl_sz = 0;
        int qrc = qtsq_codec_decompress(ctx->singularity + 4,
                                        ctx->singularity_size - 4,
                                        raw_size, &raw, &dl_sz);
        if (qrc != QTSQ_OK) return qrc;
        dl = (uLongf)dl_sz;
    } else {
        raw = (uint8_t *)malloc(raw_size);
        if (!raw) return QTSQ_ERR_ALLOC;
        dl = (uLongf)raw_size;
        int zerr = uncompress(raw, &dl, ctx->singularity + 4,
                       (uLong)(ctx->singularity_size - 4));
        if (zerr != Z_OK) {
            free(raw);
            return QTSQ_ERR_FORMAT;
        }
    }

    /* Parse header — PH04 format */
    if (dl < 28) {
        free(raw); return QTSQ_ERR_FORMAT;
    }
    if (raw[0] != 'P' || raw[1] != 'H' || raw[2] != '0' || raw[3] != '4') {
        free(raw); return QTSQ_ERR_FORMAT;
    }

    size_t p = 4;
    uint32_t spc, sample_rate, pcm_bytes_per_ch, total_atoms, atom_size;
    uint8_t channels, bit_depth, flags;

    memcpy(&spc, raw + p, 4); p += 4;
    memcpy(&sample_rate, raw + p, 4); p += 4;
    channels = raw[p++];
    bit_depth = raw[p++]; (void)bit_depth;
    flags = raw[p++];
    p++; /* reserved */

    memcpy(&pcm_bytes_per_ch, raw + p, 4); p += 4;
    memcpy(&total_atoms, raw + p, 4); p += 4;
    memcpy(&atom_size, raw + p, 4); p += 4;

    /* SEC: spc and pcm_bytes_per_ch both come from the file. The delta
     * decode loop reads `spc` int32 values per channel, so the per-channel
     * PCM region must hold exactly spc * 4 bytes — otherwise a crafted
     * file causes a heap over-read. Validate the relationship. */
    if (channels < 1 || channels > 16) {
        free(raw);
        return QTSQ_ERR_FORMAT;
    }
    if (pcm_bytes_per_ch != (uint32_t)spc * 4u) {
        free(raw);
        return QTSQ_ERR_FORMAT;
    }
    if (atom_size != sizeof(photon_atom_t)) {
        free(raw);
        return QTSQ_ERR_FORMAT;
    }

    /* Validate sizes */
    size_t required_pcm_bytes = (size_t)channels * pcm_bytes_per_ch;
    size_t required_atom_bytes = (size_t)total_atoms * atom_size;
    if (required_atom_bytes > (size_t)dl) {
        free(raw);
        return QTSQ_ERR_FORMAT;
    }
    if (p + required_pcm_bytes + required_atom_bytes > dl) {
        free(raw);
        return QTSQ_ERR_FORMAT;
    }

    /* Read and Delta Decode PCM Layers */
    float **decoded_ch = (float **)malloc(channels * sizeof(float *));
    if (!decoded_ch) { free(raw); return QTSQ_ERR_ALLOC; }

    for (int c = 0; c < channels; c++) {
        decoded_ch[c] = (float *)malloc(spc * sizeof(float));
        if (!decoded_ch[c]) {
            for (int i = 0; i < c; i++) free(decoded_ch[i]);
            free(decoded_ch); free(raw);
            return QTSQ_ERR_ALLOC;
        }

        int32_t *delta_data = (int32_t *)(raw + p);
        p += pcm_bytes_per_ch;

        delta_decode_int32_to_float32(delta_data, decoded_ch[c], spc);
    }

    /* Mid/Side -> Left/Right reconstruction */
    float *pcm = (float *)malloc((size_t)channels * spc * sizeof(float));
    if (!pcm) {
        for (int c = 0; c < channels; c++) free(decoded_ch[c]);
        free(decoded_ch); free(raw);
        return QTSQ_ERR_ALLOC;
    }

    for (int pair = 0; pair < channels / 2; pair++) {
        int L_ch = pair * 2;
        int R_ch = pair * 2 + 1;
        for (uint32_t i = 0; i < spc; i++) {
            float Mid = decoded_ch[L_ch][i];
            float Side = decoded_ch[R_ch][i];

            float L = Mid + Side;
            float R = Mid - Side;

            if (L > 1.0f) L = 1.0f; else if (L < -1.0f) L = -1.0f;
            if (R > 1.0f) R = 1.0f; else if (R < -1.0f) R = -1.0f;

            pcm[i * channels + L_ch] = L;
            pcm[i * channels + R_ch] = R;
        }
    }
    if (channels % 2 == 1) {
        int c = channels - 1;
        for (uint32_t i = 0; i < spc; i++) {
            float val = decoded_ch[c][i];
            if (val > 1.0f) val = 1.0f; else if (val < -1.0f) val = -1.0f;
            pcm[i * channels + c] = val;
        }
    }

    for (int c = 0; c < channels; c++) free(decoded_ch[c]);
    free(decoded_ch);

    /* Read Atoms (if requested) */
    if (out_atoms && out_atom_count && total_atoms > 0 && (flags & 1)) {
        photon_atom_t *atoms = (photon_atom_t *)malloc(total_atoms * sizeof(photon_atom_t));
        if (atoms) {
            if (atom_size == sizeof(photon_atom_t)) {
                memcpy(atoms, raw + p, total_atoms * sizeof(photon_atom_t));
            } else {
                for (uint32_t i = 0; i < total_atoms; i++) {
                    size_t copy_size = atom_size < sizeof(photon_atom_t) ? atom_size : sizeof(photon_atom_t);
                    memcpy(&atoms[i], raw + p + i * atom_size, copy_size);
                }
            }
            *out_atoms = atoms;
            *out_atom_count = total_atoms;
        }
    }

    free(raw);
    *out_pcm = pcm;
    *out_total_samples = (size_t)channels * spc;
    return QTSQ_OK;
}
