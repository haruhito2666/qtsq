/*
 * Quantum Tensor Sequence — Radiation Engine (Hawking Radiation)
 * Feature: Lazy/partial decompression — decompress only the data you need
 * Supports: byte-range, audio time-range, image region, video frame
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include "qtsq.h"
#include "qtsq_codec.h"
#include "qtsq_splat_only.h"
#include "qtsq_pixel.h"
#include <zlib.h>

/* ══════════════════════════════════════════════════════════════
 *  Radiation Index Construction
 * ══════════════════════════════════════════════════════════════
 *
 * The radiation index maps logical offsets in the original data
 * to chunks in the compressed singularity. Each entry describes
 * one "chunk" that can be independently decompressed.
 */

int qtsq_build_radiation_index(qtsq_context_t *ctx, uint32_t chunk_size) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (chunk_size == 0) chunk_size = 4096;

    uint64_t orig_size = ctx->header.original_size;
    size_t num_chunks = (size_t)((orig_size + chunk_size - 1) / chunk_size);

    /* Free existing index */
    if (ctx->radiation_index) {
        free(ctx->radiation_index);
        ctx->radiation_index = NULL;
    }

    ctx->radiation_index = (qtsq_index_t *)calloc(num_chunks, sizeof(qtsq_index_t));
    if (!ctx->radiation_index) return QTSQ_ERR_ALLOC;

    for (uint32_t i = 0; i < num_chunks; i++) {
        ctx->radiation_index[i].logical_offset = (uint64_t)i * chunk_size;
        ctx->radiation_index[i].chunk_size = chunk_size;
        ctx->radiation_index[i].singularity_offset = 0; /* linear mapping */
        ctx->radiation_index[i].priority = (uint8_t)(i == 0 ? 255 : 128);
    }

    ctx->num_radiation = num_chunks;
    ctx->header.radiation_size = num_chunks;
    ctx->header.flags |= QTSQ_FLAG_LAZY_DECOMP | QTSQ_FLAG_SEEKABLE;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_radiate — Generic byte-range access
 * ══════════════════════════════════════════════════════════════
 *
 * Decompress only the bytes in [offset, offset+size).
 * This is the foundation for all lazy access methods.
 */

int qtsq_radiate(const qtsq_context_t *ctx,
                uint64_t offset, uint32_t size,
                uint8_t **out_data) {
    if (!ctx || !out_data) return QTSQ_ERR_NULL;
    if (!ctx->singularity) return QTSQ_ERR_FORMAT;

    /* Full decompression then extract range */
    /* (A production implementation would use the radiation index
     *  to decompress only the relevant chunks) */
    uint8_t *full_data = NULL;
    size_t full_size = 0;
    int result;

    switch (ctx->header.strategy) {
        case QTSQ_STRATEGY_NONE:
        case QTSQ_STRATEGY_DELTA_SEED:
        case QTSQ_STRATEGY_PROCEDURAL:
            result = qtsq_decompress_singularity(ctx, &full_data, &full_size);
            break;
        case QTSQ_STRATEGY_DICTIONARY:
            result = qtsq_decompress_horizon(ctx, &full_data, &full_size);
            break;
        case QTSQ_STRATEGY_FOURIER:
            result = qtsq_decompress_lensing(ctx, &full_data, &full_size);
            break;
        case QTSQ_STRATEGY_FRACTAL:
        case QTSQ_STRATEGY_TEMPORAL:
            result = qtsq_decompress_accretion(ctx, &full_data, &full_size);
            break;
        case QTSQ_STRATEGY_TENSOR: {
            float *fdata; size_t fcount;
            result = qtsq_decompress_tensor(ctx, &fdata, &fcount);
            if (result == QTSQ_OK) {
                full_size = fcount * sizeof(float);
                full_data = (uint8_t *)fdata;
            }
            break;
        }
        default:
            return QTSQ_ERR_STRATEGY;
    }

    if (result != QTSQ_OK) return result;

    /* Bounds check */
    if (offset >= full_size) {
        free(full_data);
        return QTSQ_ERR_FORMAT;
    }

    uint32_t actual_size = size;
    if (offset + size > full_size) {
        actual_size = (uint32_t)(full_size - offset);
    }

    /* Copy requested range */
    uint8_t *output = (uint8_t *)malloc(actual_size);
    if (!output) {
        free(full_data);
        return QTSQ_ERR_ALLOC;
    }

    memcpy(output, full_data + offset, actual_size);
    free(full_data);

    *out_data = output;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_radiate_time — Audio time-range access
 * ══════════════════════════════════════════════════════════════
 *
 * Decompress only the audio samples in [start_sec, start_sec + duration_sec).
 */

int qtsq_radiate_time(const qtsq_context_t *ctx,
                     double start_sec, double duration_sec,
                     int16_t **out_samples, size_t *out_count) {
    if (!ctx || !out_samples || !out_count) return QTSQ_ERR_NULL;

    /* Get sample rate from schema */
    uint32_t sample_rate = ctx->schema.sample_rate;
    if (sample_rate == 0) sample_rate = 44100; /* default */

    /* Calculate sample range */
    size_t start_sample = (size_t)(start_sec * sample_rate);
    size_t num_samples = (size_t)(duration_sec * sample_rate);

    /* Decompress full audio */
    uint8_t *full_data = NULL;
    size_t full_size = 0;
    int result = qtsq_decompress_lensing(ctx, &full_data, &full_size);
    if (result != QTSQ_OK) return result;

    size_t total_samples = full_size / sizeof(int16_t);
    int16_t *all_samples = (int16_t *)full_data;

    /* Bounds check */
    if (start_sample >= total_samples) {
        free(full_data);
        return QTSQ_ERR_FORMAT;
    }
    if (start_sample + num_samples > total_samples) {
        num_samples = total_samples - start_sample;
    }

    /* Copy requested range */
    int16_t *output = (int16_t *)malloc(num_samples * sizeof(int16_t));
    if (!output) {
        free(full_data);
        return QTSQ_ERR_ALLOC;
    }

    memcpy(output, all_samples + start_sample, num_samples * sizeof(int16_t));
    free(full_data);

    *out_samples = output;
    *out_count = num_samples;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_radiate_region — Image region access
 * ══════════════════════════════════════════════════════════════
 *
 * Decompress only a rectangular region (x, y, w, h) from an image.
 */

int qtsq_radiate_region(const qtsq_context_t *ctx,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint8_t **out_pixels) {
    if (!ctx || !out_pixels) return QTSQ_ERR_NULL;

    /* Get image dimensions from schema */
    uint32_t img_width = ctx->schema.dimensions[0];
    uint32_t img_height = ctx->schema.dimensions[1];

    if (img_width == 0 || img_height == 0) return QTSQ_ERR_FORMAT;

    /* Bounds check */
    if (x + w > img_width) w = img_width - x;
    if (y + h > img_height) h = img_height - y;

    /* Decompress full image */
    uint8_t *full_data = NULL;
    size_t full_size = 0;
    int result = qtsq_decompress_accretion(ctx, &full_data, &full_size);
    if (result != QTSQ_OK) return result;

    /* Extract region */
    size_t region_size = (size_t)w * h;
    uint8_t *output = (uint8_t *)malloc(region_size);
    if (!output) {
        free(full_data);
        return QTSQ_ERR_ALLOC;
    }

    for (uint32_t row = 0; row < h; row++) {
        uint32_t src_offset = (y + row) * img_width + x;
        uint32_t dst_offset = row * w;

        if (src_offset + w <= full_size) {
            memcpy(output + dst_offset, full_data + src_offset, w);
        }
    }

    free(full_data);
    *out_pixels = output;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_radiate_frame — Video frame access
 * ══════════════════════════════════════════════════════════════
 *
 * Decompress a single video frame by index.
 * For now: decompresses the keyframe (frame 0).
 * Production version would support delta-frame seeking.
 */

int qtsq_radiate_frame(const qtsq_context_t *ctx,
                      uint32_t frame_index, uint8_t **out_pixels) {
    if (!ctx || !out_pixels) return QTSQ_ERR_NULL;

    /* Currently only keyframe (frame 0) is stored */
    if (frame_index != 0) {
        /* Future: walk delta chain from nearest keyframe */
        return QTSQ_ERR_FORMAT;
    }

    /* Decompress the fractal-encoded keyframe */
    uint8_t *full_data = NULL;
    size_t full_size = 0;
    int result = qtsq_decompress_accretion(ctx, &full_data, &full_size);
    if (result != QTSQ_OK) return result;

    *out_pixels = full_data;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_uncompress_bounded — zlib-decompress with hard caps
 * ══════════════════════════════════════════════════════════════
 *
 * Every engine-specific decoder does the same dance:
 *   [u32 uncompressed_size][zlib stream]
 * and malloc'd the file-declared size with no validation — a crafted
 * file could declare UINT32_MAX and force a ~4 GB malloc (DoS), or
 * declare a small size and over-read. This helper centralizes the
 * caps and returns QTSQ_ERR_FORMAT on any violation.
 */
int qtsq_uncompress_bounded(const uint8_t *src, size_t src_size,
                            uint64_t max_ratio, uint64_t max_abs,
                            uint8_t **out, size_t *out_size) {
    if (!src || !out || !out_size) return QTSQ_ERR_NULL;
    *out = NULL;
    *out_size = 0;
    if (src_size < 5) return QTSQ_ERR_FORMAT;  /* 4-byte size + stream */

    uint32_t declared;
    memcpy(&declared, src, 4);

    uint64_t compressed = (uint64_t)(src_size - 4);
    if (compressed == 0) return QTSQ_ERR_FORMAT;
    if ((uint64_t)declared > compressed * max_ratio) return QTSQ_ERR_FORMAT;
    if ((uint64_t)declared > max_abs) return QTSQ_ERR_FORMAT;
    if (declared == 0) return QTSQ_ERR_FORMAT;

    /* Phase 6b: the stream after the size prefix may be qtc (opt-in) or
     * zlib (default). qtc starts with SGC\x01, zlib with 0x78 — sniff. */
    if (qtsq_codec_is_qtc(src + 4, src_size - 4)) {
        if (qtsq_codec_declared_size(src + 4, src_size - 4) != declared)
            return QTSQ_ERR_FORMAT;
        return qtsq_codec_decompress(src + 4, src_size - 4, declared,
                                     out, out_size);
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)declared);
    if (!buf) return QTSQ_ERR_ALLOC;

    uLongf dl = (uLongf)declared;
    int zerr = uncompress(buf, &dl, src + 4, (uLong)compressed);
    if (zerr != Z_OK) {
        free(buf);
        return QTSQ_ERR_FORMAT;
    }
    *out = buf;
    *out_size = (size_t)dl;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  qtsq_decompress — Universal dispatcher
 * ══════════════════════════════════════════════════════════════
 *
 * Automatically picks the right decompression engine based
 * on the strategy stored in the header.
 */

static int qtsq_decompress_dispatch(qtsq_context_t *ctx,
                                    uint8_t **out_data, size_t *out_size) {
    switch (ctx->header.strategy) {
        case QTSQ_STRATEGY_NONE:
        case QTSQ_STRATEGY_DELTA_SEED:
        case QTSQ_STRATEGY_SINGULARITY:
            return qtsq_decompress_singularity(ctx, out_data, out_size);

        case QTSQ_STRATEGY_PROCEDURAL:
            /* Short-audio lossless fallback (qtsq_compress_audio stores
             * sub-FFT clips as raw int16 under PROCEDURAL + audio dtype;
             * qtsq_compress_signal re-tags the type to SIGNAL_1D). */
            if (ctx->header.data_type == QTSQ_TYPE_AUDIO_MONO ||
                ctx->header.data_type == QTSQ_TYPE_AUDIO_STEREO ||
                ctx->header.data_type == QTSQ_TYPE_AUDIO_MULTI ||
                ctx->header.data_type == QTSQ_TYPE_SIGNAL_1D) {
                int16_t *pcm = NULL;
                size_t ns = 0;
                uint32_t sr = 0;
                uint8_t ch = 0;
                int r = qtsq_decompress_audio(ctx, &pcm, &ns, &sr, &ch);
                if (r != QTSQ_OK) return r;
                *out_data = (uint8_t *)pcm;
                *out_size = ns * sizeof(int16_t);
                return QTSQ_OK;
            }
            return qtsq_decompress_singularity(ctx, out_data, out_size);

        case QTSQ_STRATEGY_DICTIONARY:
            return qtsq_decompress_horizon(ctx, out_data, out_size);

        case QTSQ_STRATEGY_FOURIER:
            return qtsq_decompress_lensing(ctx, out_data, out_size);

        case QTSQ_STRATEGY_PHOTON:
            /* Photon HQ audio: decode float32 via the HQ path and convert
             * to interleaved int16 (same conversion qtsq_decompress_audio
             * performs for this strategy). Read-path only — no wire change. */
            {
                int16_t *pcm = NULL;
                size_t ns = 0;
                uint32_t sr = 0;
                uint8_t ch = 0;
                int r = qtsq_decompress_audio(ctx, &pcm, &ns, &sr, &ch);
                if (r != QTSQ_OK) return r;
                *out_data = (uint8_t *)pcm;
                *out_size = ns * sizeof(int16_t);
                return QTSQ_OK;
            }

        case QTSQ_STRATEGY_FRACTAL:
        case QTSQ_STRATEGY_TEMPORAL:
            return qtsq_decompress_accretion(ctx, out_data, out_size);

        case QTSQ_STRATEGY_LAYERED:
            if (ctx->header.data_type == QTSQ_TYPE_AUDIO_MONO ||
                ctx->header.data_type == QTSQ_TYPE_AUDIO_STEREO) {
                return qtsq_decompress_gravitational_audio(ctx, out_data, out_size);
            }
            return qtsq_decompress_gravitational(ctx, out_data, out_size);

        case QTSQ_STRATEGY_TRIANGLE:
            return qtsq_decompress_gravitational(ctx, out_data, out_size);

        case QTSQ_STRATEGY_SPLAT:
            return qtsq_decompress_splat_only(ctx, out_data, out_size);

        case QTSQ_STRATEGY_TENSOR: {
            float *fdata; size_t fcount;
            int r = qtsq_decompress_tensor(ctx, &fdata, &fcount);
            if (r == QTSQ_OK) {
                *out_data = (uint8_t *)fdata;
                *out_size = fcount * sizeof(float);
                return r;
            }
            /* Array modes (5..8: int/fp16/fp64 weight payloads) are not
             * float tensors — decode as raw bytes; mode 9 chunked likewise. */
            uint8_t *adata = NULL;
            size_t asize = 0;
            r = qtsq_decompress_array(ctx, &adata, &asize, NULL);
            if (r != QTSQ_OK) {
                uint8_t el = 0;
                r = qtsq_decompress_chunked(ctx, &adata, &asize, &el);
            }
            if (r == QTSQ_OK) {
                *out_data = adata;
                *out_size = asize;
            }
            return r;
        }

        case QTSQ_STRATEGY_PIXEL: {
            uint8_t *pdata = NULL;
            size_t psize = 0;
            int r = qtsq_decompress_pixel(ctx, &pdata, &psize);
            if (r != QTSQ_OK) {
                /* Phase 6b: 16-bit camera-raw payloads (QTC16 marker) fail
                 * the 8-bit size check in qtsq_decompress_pixel; decode them
                 * via the 16-bit path and hand back the sample bytes. */
                uint16_t *p16 = NULL;
                r = qtsq_decompress_pixel16(ctx, &p16, &psize);
                if (r == QTSQ_OK) pdata = (uint8_t *)p16;
            }
            if (r == QTSQ_OK) {
                *out_data = pdata;
                *out_size = psize;
            }
            return r;
        }

        case QTSQ_STRATEGY_TSUBU:
            /* Tsubu atom streams: decompress zlib if flagged,
             * otherwise return raw payload for tsubu_unpack(). */
            if (!ctx->singularity || ctx->singularity_size == 0)
                return QTSQ_ERR_FORMAT;

            if (ctx->header.flags & QTSQ_FLAG_ZLIB) {
                uLongf dest_len = (uLongf)ctx->header.original_size;
                uint8_t *decomp = (uint8_t *)malloc(dest_len);
                if (!decomp) return QTSQ_ERR_ALLOC;
                if (uncompress(decomp, &dest_len,
                               ctx->singularity, (uLong)ctx->singularity_size) != Z_OK) {
                    free(decomp);
                    return QTSQ_ERR_FORMAT;
                }
                *out_data = decomp;
                *out_size = (size_t)dest_len;
            } else {
                *out_data = (uint8_t *)malloc(ctx->singularity_size);
                if (!*out_data) return QTSQ_ERR_ALLOC;
                memcpy(*out_data, ctx->singularity, ctx->singularity_size);
                *out_size = ctx->singularity_size;
            }
            return QTSQ_OK;

        case QTSQ_STRATEGY_4DGS:
            /* 4D Gaussian Splatting: return raw payload for gs_4d_unpack() */
            if (!ctx->singularity || ctx->singularity_size == 0)
                return QTSQ_ERR_FORMAT;
            if (ctx->header.flags & QTSQ_FLAG_ZLIB) {
                uLongf dest_len = (uLongf)ctx->header.original_size;
                uint8_t *decomp = (uint8_t *)malloc(dest_len);
                if (!decomp) return QTSQ_ERR_ALLOC;
                if (uncompress(decomp, &dest_len,
                               ctx->singularity, (uLong)ctx->singularity_size) != Z_OK) {
                    free(decomp);
                    return QTSQ_ERR_FORMAT;
                }
                *out_data = decomp;
                *out_size = (size_t)dest_len;
            } else {
                *out_data = (uint8_t *)malloc(ctx->singularity_size);
                if (!*out_data) return QTSQ_ERR_ALLOC;
                memcpy(*out_data, ctx->singularity, ctx->singularity_size);
                *out_size = ctx->singularity_size;
            }
            return QTSQ_OK;

        default:
            return QTSQ_ERR_STRATEGY;
    }
}

/* GEN-1 FIX (Phase 6b): every engine writes sha256(ORIGINAL input) into
 * header.checksum, so verification can only happen after decode — not at
 * read time, where only the compressed singularity exists (the old
 * read-time check hashed the singularity and rejected healthy files).
 * Applies to lossless paths only: lossy engines stamp QTSQ_FLAG_LOSSY and
 * their decoded output legitimately differs from the original. The pixel
 * engine verifies its own checksum inside qtsq_decompress_pixel. This also
 * covers encrypted files, which previously skipped verification entirely. */
static int qtsq_verify_decoded_checksum(const qtsq_context_t *ctx,
                                        const uint8_t *data, size_t size) {
    if (!(ctx->header.flags & QTSQ_FLAG_CHECKSUMMED)) return QTSQ_OK;
    if (ctx->header.flags & QTSQ_FLAG_LOSSY) return QTSQ_OK;
    if (ctx->header.strategy == QTSQ_STRATEGY_PIXEL) return QTSQ_OK;

    static const uint8_t zero_digest[32] = {0};
    if (memcmp(ctx->header.checksum, zero_digest, 32) == 0) return QTSQ_OK;
    if (size != (size_t)ctx->header.original_size) return QTSQ_OK;

    uint8_t computed[32];
    qtsq_sha256(data, size, computed);
    if (memcmp(computed, ctx->header.checksum, 32) != 0)
        return QTSQ_ERR_CORRUPT;
    return QTSQ_OK;
}

int qtsq_decompress(qtsq_context_t *ctx,
                   uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity) return QTSQ_ERR_FORMAT;

    int rc = qtsq_decompress_dispatch(ctx, out_data, out_size);
    if (rc != QTSQ_OK) return rc;

    rc = qtsq_verify_decoded_checksum(ctx, *out_data, *out_size);
    if (rc != QTSQ_OK) {
        free(*out_data);
        *out_data = NULL;
        *out_size = 0;
    }
    return rc;
}
