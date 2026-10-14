/*
 * Quantum Tensor Sequence — Redshift Engine (Quality Scaling)
 * Feature: Multi-resolution decompression from a single compressed file
 * redshift=0.0 → tiny thumbnail, redshift=1.0 → full quality
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "qtsq.h"

/* ── Image downscale (nearest-neighbor, channel-aware) ── */

static void downscale_image(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint8_t ch,
                             uint8_t *dst, uint32_t dst_w, uint32_t dst_h) {
    for (uint32_t y = 0; y < dst_h; y++) {
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t sx = x * src_w / dst_w;
            uint32_t sy = y * src_h / dst_h;
            const uint8_t *s = src + (sy * src_w + sx) * ch;
            uint8_t *d = dst + (y * dst_w + x) * ch;
            for (uint8_t c = 0; c < ch; c++) d[c] = s[c];
        }
    }
}

/* ── Audio downsample (skip samples) ── */

static void downsample_audio(const int16_t *src, size_t src_count,
                              int16_t *dst, size_t dst_count) {
    for (size_t i = 0; i < dst_count; i++) {
        size_t si = i * src_count / dst_count;
        dst[i] = src[si];
    }
}

/* ── Text truncation ── */

static void truncate_text(const uint8_t *src, size_t src_size,
                           uint8_t *dst, size_t dst_size) {
    memcpy(dst, src, dst_size < src_size ? dst_size : src_size);
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_redshift
 * ══════════════════════════════════════════════════════════════
 *
 * Decompress with quality scaling:
 *   redshift = 0.0 → minimum quality (tiny preview / thumbnail)
 *   redshift = 0.5 → half quality / resolution
 *   redshift = 1.0 → full quality (identical to qtsq_decompress)
 *
 * For images:  scales output resolution
 * For audio:   reduces sample count
 * For text:    truncates output
 * For binary:  returns partial data
 */

int qtsq_decompress_redshift(const qtsq_context_t *ctx,
                             double redshift,
                             uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;

    /* Clamp redshift to [0.0, 1.0] */
    if (redshift < 0.0) redshift = 0.0;
    if (redshift > 1.0) redshift = 1.0;

    /* Full quality: just decompress normally */
    if (redshift >= 0.999) {
        return qtsq_decompress((qtsq_context_t *)ctx, out_data, out_size);
    }

    /* Minimum scale factor (never go below 1/32 of original) */
    double scale = 0.03125 + redshift * (1.0 - 0.03125);

    /* First: full decompress, then downscale/subsample */
    uint8_t *full_data = NULL;
    size_t full_size = 0;
    int result = qtsq_decompress((qtsq_context_t *)ctx, &full_data, &full_size);
    if (result != QTSQ_OK) return result;

    uint8_t data_type = ctx->header.data_type;

    /* ── Image redshift: downscale resolution ── */
    if (data_type >= QTSQ_TYPE_IMAGE_GRAY && data_type <= QTSQ_TYPE_IMAGE_RGBA) {
        uint32_t orig_w = ctx->schema.dimensions[0];
        uint32_t orig_h = ctx->schema.dimensions[1];

        if (orig_w == 0 || orig_h == 0) {
            /* Fallback: assume square image */
            orig_w = (uint32_t)sqrt((double)full_size);
            orig_h = orig_w;
        }

        uint32_t new_w = (uint32_t)(orig_w * scale);
        uint32_t new_h = (uint32_t)(orig_h * scale);
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;

        uint8_t ch = ctx->header.num_channels;
        if (ch == 0) ch = 1; /* grayscale or unknown → 1 byte/pixel */

        size_t new_size = (size_t)new_w * new_h * ch;
        uint8_t *scaled = (uint8_t *)malloc(new_size);
        if (!scaled) { free(full_data); return QTSQ_ERR_ALLOC; }

        downscale_image(full_data, orig_w, orig_h, ch, scaled, new_w, new_h);
        free(full_data);

        *out_data = scaled;
        *out_size = new_size;
        return QTSQ_OK;
    }

    /* ── Audio redshift: downsample ── */
    if (data_type >= QTSQ_TYPE_AUDIO_MONO && data_type <= QTSQ_TYPE_AUDIO_MULTI) {
        size_t orig_samples = full_size / sizeof(int16_t);
        size_t new_samples = (size_t)(orig_samples * scale);
        if (new_samples < 1) new_samples = 1;

        int16_t *downsampled = (int16_t *)malloc(new_samples * sizeof(int16_t));
        if (!downsampled) { free(full_data); return QTSQ_ERR_ALLOC; }

        downsample_audio((const int16_t *)full_data, orig_samples,
                          downsampled, new_samples);
        free(full_data);

        *out_data = (uint8_t *)downsampled;
        *out_size = new_samples * sizeof(int16_t);
        return QTSQ_OK;
    }

    /* ── Text redshift: truncate ── */
    if (data_type >= QTSQ_TYPE_TEXT_ASCII && data_type <= QTSQ_TYPE_CSV) {
        size_t new_size = (size_t)(full_size * scale);
        if (new_size < 1) new_size = 1;

        uint8_t *truncated = (uint8_t *)malloc(new_size + 1);
        if (!truncated) { free(full_data); return QTSQ_ERR_ALLOC; }

        truncate_text(full_data, full_size, truncated, new_size);
        truncated[new_size] = '\0';
        free(full_data);

        *out_data = truncated;
        *out_size = new_size;
        return QTSQ_OK;
    }

    /* ── Default: return scaled portion of raw data ── */
    {
        size_t new_size = (size_t)(full_size * scale);
        if (new_size < 1) new_size = 1;

        uint8_t *partial = (uint8_t *)malloc(new_size);
        if (!partial) { free(full_data); return QTSQ_ERR_ALLOC; }

        memcpy(partial, full_data, new_size);
        free(full_data);

        *out_data = partial;
        *out_size = new_size;
        return QTSQ_OK;
    }
}
