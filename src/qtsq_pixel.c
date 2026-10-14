/*
 * Quantum Tensor Sequence — Pixel Encoding Engine
 * Primary safe-mode for exact pixel fidelity with zero metadata.
 * Author: Haruhito
 */

#include "qtsq_pixel.h"
#include "qtsq_codec.h"
#include "qtc.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* qtsq_pixel_header_t is defined in qtsq_pixel.h — do not redefine it here.
 * Duplicate anonymous-struct typedefs are incompatible types per C11 and
 * break newer clang (NDK r26+). */

/* Phase 7: per-frame pixel budget. Raised from 512 MB to the hard bound of
 * the u32 uncompressed_size framing field now that the qtc LZ layer handles
 * large streams. Decode still recomputes the size from the declared
 * dimensions and requires it to match uncompressed_size before allocating,
 * so a crafted header cannot force a larger allocation than w*h*ch(*2). */
#define QTSQ_PIXEL_MAX_BYTES 0xFFFFFFFFULL

/* Simple Paeth predictor for PNG-like row filtering */
static inline int paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Apply delta filtering to improve zlib compression */
static void filter_pixels(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint8_t channels) {
    uint32_t stride = width * channels;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            for (uint8_t c = 0; c < channels; c++) {
                uint32_t idx = y * stride + x * channels + c;
                int current = src[idx];
                int left = (x > 0) ? src[idx - channels] : 0;
                int up = (y > 0) ? src[idx - stride] : 0;
                int upleft = (x > 0 && y > 0) ? src[idx - stride - channels] : 0;
                
                int pred = paeth_predictor(left, up, upleft);
                dst[idx] = (uint8_t)((current - pred) & 0xFF);
            }
        }
    }
}

static void unfilter_pixels(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint8_t channels) {
    uint32_t stride = width * channels;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            for (uint8_t c = 0; c < channels; c++) {
                uint32_t idx = y * stride + x * channels + c;
                int delta = src[idx];
                int left = (x > 0) ? dst[idx - channels] : 0;
                int up = (y > 0) ? dst[idx - stride] : 0;
                int upleft = (x > 0 && y > 0) ? dst[idx - stride - channels] : 0;
                
                int pred = paeth_predictor(left, up, upleft);
                dst[idx] = (uint8_t)((delta + pred) & 0xFF);
            }
        }
    }
}

int qtsq_compress_pixel(qtsq_context_t *ctx,
                        const uint8_t *pixels,
                        uint32_t width, uint32_t height,
                        uint8_t channels, int compression_level) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width == 0 || height == 0 || channels == 0 || channels > 4) return QTSQ_ERR_FORMAT;

    /* FIX IMG-1: use 64-bit intermediate to detect overflow before truncating */
    uint64_t raw_size_64 = (uint64_t)width * height * channels;
    if (raw_size_64 > QTSQ_PIXEL_MAX_BYTES) return QTSQ_ERR_FORMAT;
    uint32_t raw_size = (uint32_t)raw_size_64;

    uint8_t *payload = NULL;
    size_t payload_size = 0;
    int stored_compression = compression_level;

    if (compression_level == QTSQ_PIXEL_COMPRESS_ZLIB && ctx->codec == QTSQ_CODEC_QTC) {
        /* Phase 6a/6b (opt-in): qtc image lens on RAW pixels. qtc's 2D MED
         * predictor subsumes the Paeth filter, so filtering is skipped.
         * quality > 0 switches to the lossy photo coder (DCT/mesh/accretion
         * contest); quality == 0 stays lossless. Decoders dispatch on the
         * stored codec byte. */
        qtc_options_t opts;
        qtc_options_default(&opts);
        int lossy = (ctx->quality > 0.0f);
        if (lossy) {
            opts.mode = QTC_MODE_LOSSY;
            opts.quality = ctx->quality;
        }
        int qrc = qtc_compress_image(pixels, width, height, channels,
                                     &payload, &payload_size, &opts);
        if (qrc != QTC_OK || !payload) {
            free(payload);
            return (qrc == QTC_ERR_ALLOC) ? QTSQ_ERR_ALLOC : QTSQ_ERR_FORMAT;
        }
        stored_compression = lossy ? QTSQ_PIXEL_COMPRESS_QTC_LOSSY
                                   : QTSQ_PIXEL_COMPRESS_QTC;
    } else {
        uint8_t *filtered = (uint8_t *)malloc(raw_size);
        if (!filtered) return QTSQ_ERR_ALLOC;

        filter_pixels(pixels, filtered, width, height, channels);

        if (compression_level == QTSQ_PIXEL_COMPRESS_ZLIB) {
            uLongf bound = compressBound((uLong)raw_size);
            payload = (uint8_t *)malloc(bound);
            if (!payload) {
                free(filtered);
                return QTSQ_ERR_ALLOC;
            }
            if (compress(payload, &bound, filtered, raw_size) != Z_OK) {
                free(filtered);
                free(payload);
                return QTSQ_ERR_FORMAT;
            }
            payload_size = bound;
        } else {
            payload = (uint8_t *)malloc(raw_size);
            if (!payload) {
                free(filtered);
                return QTSQ_ERR_ALLOC;
            }
            memcpy(payload, filtered, raw_size);
            payload_size = raw_size;
        }

        free(filtered);
    }

    size_t final_size = sizeof(qtsq_pixel_header_t) + payload_size;
    uint8_t *final_buf = (uint8_t *)malloc(final_size);
    if (!final_buf) {
        free(payload);
        return QTSQ_ERR_ALLOC;
    }

    qtsq_pixel_header_t *phdr = (qtsq_pixel_header_t *)final_buf;
    phdr->magic[0] = QTSQ_PIX_MAGIC_0;
    phdr->magic[1] = QTSQ_PIX_MAGIC_1;
    phdr->magic[2] = QTSQ_PIX_MAGIC_2;
    phdr->magic[3] = QTSQ_PIX_MAGIC_3;
    phdr->width = width;
    phdr->height = height;
    phdr->channels = channels;
    phdr->compression = stored_compression;
    phdr->uncompressed_size = raw_size;

    memcpy(final_buf + sizeof(qtsq_pixel_header_t), payload, payload_size);
    free(payload);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = final_buf;
    ctx->singularity_size = final_size;

    ctx->header.strategy = QTSQ_STRATEGY_PIXEL;
    ctx->header.data_type = (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            (channels == 3) ? QTSQ_TYPE_IMAGE_RGB : QTSQ_TYPE_IMAGE_GRAY;
    ctx->header.original_size = raw_size;
    ctx->header.singularity_size = final_size;
    ctx->header.num_channels = channels;

    /* Store image dimensions so the viewer can reconstruct the frame */
    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;

    /* V3 specific flag will be set by qtsq_secure_image if needed.
     * Lossy qtc output cannot be checksummed against the original pixels. */
    if (stored_compression == QTSQ_PIXEL_COMPRESS_QTC_LOSSY) {
        ctx->header.flags |= QTSQ_FLAG_LOSSY;
    } else {
        ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
        qtsq_sha256(pixels, raw_size, ctx->header.checksum);
    }

    return QTSQ_OK;
}

/* Phase 6b: 16-bit camera-raw pixels via the qtc IMAGE lens (depth=16).
 * Opt-in only: requires ctx->codec == QTSQ_CODEC_QTC — QTSQ has no other
 * 16-bit image coder. quality > 0 selects near-lossless mode (max error
 * <= 8 LSB per sample, guaranteed by qtc). */
int qtsq_compress_pixel16(qtsq_context_t *ctx,
                          const uint16_t *pixels,
                          uint32_t width, uint32_t height,
                          uint8_t channels, int compression_level) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    if (width == 0 || height == 0 || channels == 0 || channels > 4) return QTSQ_ERR_FORMAT;
    if (ctx->codec != QTSQ_CODEC_QTC) return QTSQ_ERR_FORMAT;
    (void)compression_level; /* single coder; level reserved for the future */

    uint64_t raw_size_64 = (uint64_t)width * height * channels * 2;
    if (raw_size_64 > QTSQ_PIXEL_MAX_BYTES) return QTSQ_ERR_FORMAT;
    uint32_t raw_size = (uint32_t)raw_size_64;

    qtc_options_t opts;
    qtc_options_default(&opts);
    if (ctx->quality > 0.0f) {
        opts.mode = QTC_MODE_LOSSY;
        opts.quality = ctx->quality;
    }

    uint8_t *payload = NULL;
    size_t payload_size = 0;
    int qrc = qtc_compress_image16(pixels, width, height, channels,
                                   &payload, &payload_size, &opts);
    if (qrc != QTC_OK || !payload) {
        free(payload);
        return (qrc == QTC_ERR_ALLOC) ? QTSQ_ERR_ALLOC : QTSQ_ERR_FORMAT;
    }

    size_t final_size = sizeof(qtsq_pixel_header_t) + payload_size;
    uint8_t *final_buf = (uint8_t *)malloc(final_size);
    if (!final_buf) {
        free(payload);
        return QTSQ_ERR_ALLOC;
    }

    qtsq_pixel_header_t *phdr = (qtsq_pixel_header_t *)final_buf;
    phdr->magic[0] = QTSQ_PIX_MAGIC_0;
    phdr->magic[1] = QTSQ_PIX_MAGIC_1;
    phdr->magic[2] = QTSQ_PIX_MAGIC_2;
    phdr->magic[3] = QTSQ_PIX_MAGIC_3;
    phdr->width = width;
    phdr->height = height;
    phdr->channels = channels;
    phdr->compression = QTSQ_PIXEL_COMPRESS_QTC16;
    phdr->uncompressed_size = raw_size;

    memcpy(final_buf + sizeof(qtsq_pixel_header_t), payload, payload_size);
    free(payload);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = final_buf;
    ctx->singularity_size = final_size;

    ctx->header.strategy = QTSQ_STRATEGY_PIXEL;
    ctx->header.data_type = (channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                            (channels == 3) ? QTSQ_TYPE_IMAGE_RGB : QTSQ_TYPE_IMAGE_GRAY;
    ctx->header.original_size = raw_size;
    ctx->header.singularity_size = final_size;
    ctx->header.num_channels = channels;
    ctx->header.sample_depth = 16;

    ctx->schema.dimensions[0] = width;
    ctx->schema.dimensions[1] = height;

    if (ctx->quality > 0.0f) {
        ctx->header.flags |= QTSQ_FLAG_LOSSY;
    } else {
        ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
        qtsq_sha256((const uint8_t *)pixels, raw_size, ctx->header.checksum);
    }

    return QTSQ_OK;
}

int qtsq_compress_pixel_passthrough(qtsq_context_t *ctx, 
                                    const uint8_t *file_data, 
                                    size_t file_size) {
    if (!ctx || !file_data) return QTSQ_ERR_NULL;
    if (file_size == 0) return QTSQ_ERR_TOO_SMALL;

    ctx->singularity = (uint8_t *)malloc(file_size);
    if (!ctx->singularity) return QTSQ_ERR_ALLOC;
    memcpy(ctx->singularity, file_data, file_size);

    ctx->singularity_size = file_size;

    ctx->header.strategy = QTSQ_STRATEGY_PIXEL;
    ctx->header.data_type = QTSQ_TYPE_IMAGE_RGB;
    ctx->header.original_size = file_size;
    ctx->header.singularity_size = file_size;
    
    /* Set arbitrary fallback dimensions, viewer will use NSImage which reads actual dimensions from the JPEG/PNG headers */
    ctx->schema.dimensions[0] = 0;
    ctx->schema.dimensions[1] = 0;
    ctx->header.num_channels = 3;

    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    qtsq_sha256(file_data, file_size, ctx->header.checksum);

    return QTSQ_OK;
}

/* FIX IMG-3: removed const qualifier — decompressor legitimately writes back
 * dimensions into ctx->schema, which is not UB when the original object is mutable */
int qtsq_decompress_pixel(qtsq_context_t *ctx,
                          uint8_t **out_pixels, size_t *out_size) {
    if (!ctx || !out_pixels || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity) return QTSQ_ERR_FORMAT;

    /* Check for raw JPEG/PNG passthrough */
    if (ctx->singularity_size > 4) {
        if ((ctx->singularity[0] == 0xFF && ctx->singularity[1] == 0xD8) ||
            (ctx->singularity[0] == 0x89 && ctx->singularity[1] == 0x50 && ctx->singularity[2] == 0x4E && ctx->singularity[3] == 0x47)) {
            *out_pixels = (uint8_t *)malloc(ctx->singularity_size);
            if (!*out_pixels) return QTSQ_ERR_ALLOC;
            memcpy(*out_pixels, ctx->singularity, ctx->singularity_size);
            *out_size = ctx->singularity_size;
            return QTSQ_OK;
        }
    }

    if (ctx->singularity_size < sizeof(qtsq_pixel_header_t)) return QTSQ_ERR_FORMAT;

    qtsq_pixel_header_t *phdr = (qtsq_pixel_header_t *)ctx->singularity;
    if (phdr->magic[0] != QTSQ_PIX_MAGIC_0 ||
        phdr->magic[1] != QTSQ_PIX_MAGIC_1 ||
        phdr->magic[2] != QTSQ_PIX_MAGIC_2 ||
        phdr->magic[3] != QTSQ_PIX_MAGIC_3) {
        return QTSQ_ERR_FORMAT;
    }

    /* FIX IMG-2: validate uncompressed_size against declared dimensions before malloc
     * Prevents DoS via crafted 4 GB uncompressed_size in file header */
    if (phdr->width == 0 || phdr->height == 0 || phdr->channels == 0 || phdr->channels > 4)
        return QTSQ_ERR_FORMAT;
    uint64_t expected_size = (uint64_t)phdr->width * phdr->height * phdr->channels;
    if (expected_size > QTSQ_PIXEL_MAX_BYTES) return QTSQ_ERR_FORMAT;
    if (phdr->uncompressed_size != (uint32_t)expected_size) return QTSQ_ERR_FORMAT;

    uint8_t *filtered = NULL;
    uint8_t *pixels = NULL;

    const uint8_t *payload = ctx->singularity + sizeof(qtsq_pixel_header_t);
    size_t payload_size = ctx->singularity_size - sizeof(qtsq_pixel_header_t);

    if (phdr->compression == QTSQ_PIXEL_COMPRESS_QTC ||
        phdr->compression == QTSQ_PIXEL_COMPRESS_QTC_LOSSY) {
        /* Phase 6a/6b: qtc image stream decodes straight to raw pixels (no
         * Paeth unfilter). qtc_decompress_image accepts the lossless IMAGE
         * lens and all lossy lenses (DCT/mesh/accretion). Pre-validate the
         * declared size — for image lenses the container's original_size is
         * w*h*ch, so this also bounds the decode allocation — then confirm
         * the stream's own geometry matches. */
        uint64_t declared = qtsq_codec_declared_size(payload, payload_size);
        if (declared != (uint64_t)phdr->uncompressed_size) return QTSQ_ERR_FORMAT;
        uint32_t ow = 0, oh = 0;
        uint8_t och = 0;
        int qrc = qtc_decompress_image(payload, payload_size, &pixels, &ow, &oh, &och);
        if (qrc != QTC_OK || !pixels) {
            free(pixels);
            return QTSQ_ERR_FORMAT;
        }
        if (ow != phdr->width || oh != phdr->height || och != phdr->channels) {
            free(pixels);
            return QTSQ_ERR_FORMAT;
        }
    } else {
        filtered = (uint8_t *)malloc(phdr->uncompressed_size);
        if (!filtered) return QTSQ_ERR_ALLOC;

        if (phdr->compression == QTSQ_PIXEL_COMPRESS_ZLIB) {
            uLongf dst_len = phdr->uncompressed_size;
            if (uncompress(filtered, &dst_len, payload, payload_size) != Z_OK || dst_len != phdr->uncompressed_size) {
                free(filtered);
                return QTSQ_ERR_FORMAT;
            }
        } else {
            if (payload_size < phdr->uncompressed_size) {
                free(filtered);
                return QTSQ_ERR_FORMAT;
            }
            memcpy(filtered, payload, phdr->uncompressed_size);
        }

        pixels = (uint8_t *)malloc(phdr->uncompressed_size);
        if (!pixels) {
            free(filtered);
            return QTSQ_ERR_ALLOC;
        }

        unfilter_pixels(filtered, pixels, phdr->width, phdr->height, phdr->channels);
        free(filtered);
    }

    /* Verify checksum if available — critical for Provenance intent
     * where bit-exact fidelity is required. */
    if (ctx->header.flags & QTSQ_FLAG_CHECKSUMMED) {
        uint8_t computed[32];
        qtsq_sha256(pixels, phdr->uncompressed_size, computed);
        if (memcmp(computed, ctx->header.checksum, 32) != 0) {
            free(pixels);
            return QTSQ_ERR_CORRUPT;
        }
    }

    /* Propagate dimensions back so the viewer can render at correct size.
     * This fixes rendering for BOTH old files (no schema.dimensions)
     * and new files — the pixel header is the single source of truth. */
    /* FIX IMG-3: ctx is now non-const, write back dimensions cleanly (no UB) */
    ctx->schema.dimensions[0] = phdr->width;
    ctx->schema.dimensions[1] = phdr->height;
    ctx->header.num_channels  = phdr->channels;
    ctx->header.data_type     = (phdr->channels == 4) ? QTSQ_TYPE_IMAGE_RGBA :
                                (phdr->channels == 3) ? QTSQ_TYPE_IMAGE_RGB :
                                                        QTSQ_TYPE_IMAGE_GRAY;

    *out_pixels = pixels;
    *out_size = phdr->uncompressed_size;

    return QTSQ_OK;
}

/* Phase 6b: decode a 16-bit pixel payload (QTSQ_PIXEL_COMPRESS_QTC16). */
int qtsq_decompress_pixel16(qtsq_context_t *ctx,
                            uint16_t **out_pixels, size_t *out_size) {
    if (!ctx || !out_pixels || !out_size) return QTSQ_ERR_NULL;
    *out_pixels = NULL;
    *out_size = 0;
    if (!ctx->singularity) return QTSQ_ERR_FORMAT;
    if (ctx->singularity_size < sizeof(qtsq_pixel_header_t)) return QTSQ_ERR_FORMAT;

    qtsq_pixel_header_t *phdr = (qtsq_pixel_header_t *)ctx->singularity;
    if (phdr->magic[0] != QTSQ_PIX_MAGIC_0 ||
        phdr->magic[1] != QTSQ_PIX_MAGIC_1 ||
        phdr->magic[2] != QTSQ_PIX_MAGIC_2 ||
        phdr->magic[3] != QTSQ_PIX_MAGIC_3) {
        return QTSQ_ERR_FORMAT;
    }
    if (phdr->compression != QTSQ_PIXEL_COMPRESS_QTC16) return QTSQ_ERR_FORMAT;

    if (phdr->width == 0 || phdr->height == 0 || phdr->channels == 0 || phdr->channels > 4)
        return QTSQ_ERR_FORMAT;
    uint64_t expected_size = (uint64_t)phdr->width * phdr->height * phdr->channels * 2;
    if (expected_size > QTSQ_PIXEL_MAX_BYTES) return QTSQ_ERR_FORMAT;
    if (phdr->uncompressed_size != (uint32_t)expected_size) return QTSQ_ERR_FORMAT;

    const uint8_t *payload = ctx->singularity + sizeof(qtsq_pixel_header_t);
    size_t payload_size = ctx->singularity_size - sizeof(qtsq_pixel_header_t);

    uint64_t declared = qtsq_codec_declared_size(payload, payload_size);
    if (declared != (uint64_t)phdr->uncompressed_size) return QTSQ_ERR_FORMAT;

    uint16_t *samples = NULL;
    uint32_t ow = 0, oh = 0;
    uint8_t och = 0;
    int qrc = qtc_decompress_image16(payload, payload_size, &samples, &ow, &oh, &och);
    if (qrc != QTC_OK || !samples) {
        free(samples);
        return QTSQ_ERR_FORMAT;
    }
    if (ow != phdr->width || oh != phdr->height || och != phdr->channels) {
        free(samples);
        return QTSQ_ERR_FORMAT;
    }

    if (ctx->header.flags & QTSQ_FLAG_CHECKSUMMED) {
        uint8_t computed[32];
        qtsq_sha256((const uint8_t *)samples, phdr->uncompressed_size, computed);
        if (memcmp(computed, ctx->header.checksum, 32) != 0) {
            free(samples);
            return QTSQ_ERR_CORRUPT;
        }
    }

    ctx->schema.dimensions[0] = phdr->width;
    ctx->schema.dimensions[1] = phdr->height;
    ctx->header.num_channels  = phdr->channels;
    ctx->header.sample_depth  = 16;

    *out_pixels = samples;
    *out_size = phdr->uncompressed_size;
    return QTSQ_OK;
}
