/*
 * Quantum Tensor Sequence — Tensor Engine v2 (Hardened)
 * Compression: Float delta encoding + int32 quantization + ZLib deflate
 * Safety: NaN/Inf guards, checksum verification, chunked zlib, no size limits
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <zlib.h>
#include "qtsq_codec.h"
#include "qtsq.h"
#include "qtsq_keystore.h"
#include "qtc.h"

/* ══════════════════════════════════════════════════════════════
 *  Tensor Format (inside singularity):
 *
 *  [4B]  magic: "TNS2"
 *  [1B]  mode: 0=delta-quantized, 1=lossless-raw
 *  [1B]  num_dims
 *  [2B]  reserved
 *  [8B]  count (total float elements)
 *  [8B]  scale_factor (double, for delta dequantization)
 *  [8B]  first_value (double, anchor for delta reconstruction)
 *  [4B × num_dims]  dimensions  (embedded, survives container)
 *  [32B] payload_checksum (SHA-256 of uncompressed payload)
 *  [8B]  uncompressed_size (of the delta/raw payload)
 *  [... ]  ZLib-compressed payload:
 *     mode 0: int32_t deltas[count-1]
 *     mode 1: float raw[count]
 * ══════════════════════════════════════════════════════════════ */

#define TENSOR_MAGIC_0 'T'
#define TENSOR_MAGIC_1 'N'
#define TENSOR_MAGIC_2 'S'
#define TENSOR_MAGIC_3 '2'

/* Mode defines are below with f16/f8/f4 additions */


/* int32 range gives ~2 billion steps of precision.
 * For typical weight deltas in ±0.01, that's sub-1e-11 error. */
#define DELTA_MAX_RANGE 2147483647

/* Chunked zlib: process 256MB at a time to avoid 32-bit uLong overflow.
 * NOTE: chunked_compress was deleted — weight payloads are qtc-only
 * (user decision: no zlib on encode). chunked_decompress stays: old
 * zlib-written files must always read (magic sniff). */
#define ZLIB_CHUNK_SIZE (256ULL * 1024 * 1024)

static int chunked_decompress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) return -1;

    strm.next_in = (Bytef *)src;
    strm.next_out = dst;

    size_t in_remaining = src_len;
    size_t out_remaining = dst_len;

    while (in_remaining > 0 && out_remaining > 0) {
        strm.avail_in = (in_remaining > ZLIB_CHUNK_SIZE) 
                         ? (uInt)ZLIB_CHUNK_SIZE : (uInt)in_remaining;
        strm.avail_out = (out_remaining > ZLIB_CHUNK_SIZE)
                          ? (uInt)ZLIB_CHUNK_SIZE : (uInt)out_remaining;

        size_t in_before = strm.avail_in;
        size_t out_before = strm.avail_out;

        int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&strm);
            return -1;
        }

        in_remaining -= (in_before - strm.avail_in);
        out_remaining -= (out_before - strm.avail_out);

        /* Update pointers */
        strm.next_in = (Bytef *)(src + (src_len - in_remaining));
        strm.next_out = dst + (dst_len - out_remaining);
    }

    inflateEnd(&strm);
    return 0;
}

/* Phase 7: codec-aware payload coding, qtc default. qtc handles payloads of
 * any size as one self-describing stream (since qtc v0.16.0 the LZ layer's
 * 255 MB limit is gone). Weights are qtc-ONLY (no zlib branch, no zlib
 * fallback — a qtc failure is an error; oversize payloads report
 * TOO_LARGE so callers shard). Decode sniffs the magic (SGC\x01 vs 0x78),
 * so the TNS2 layout is unchanged and old chunked-zlib payloads still read. */

static int tensor_payload_compress(const qtsq_context_t *ctx,
                                   const uint8_t *src, size_t src_len,
                                   uint8_t **out, size_t *out_len) {
    (void)ctx; /* weights ignore the codec opt-out: qtc, always */
    if (qtsq_codec_compress(src, src_len, out, out_len) == QTSQ_OK)
        return 0;
    return -1;
}

static int tensor_payload_decompress(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_len) {
    if (qtsq_codec_is_qtc(src, src_len)) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (qtsq_codec_decompress(src, src_len, dst_len, &out, &out_len) != QTSQ_OK)
            return -1;
        if (out_len != dst_len) { free(out); return -1; }
        memcpy(dst, out, dst_len);
        free(out);
        return 0;
    }
    return chunked_decompress(src, src_len, dst, dst_len);
}

/* ══════════════════════════════════════════════════════════════
 *  NaN/Inf sanitize — replace poison values with 0.0
 * ══════════════════════════════════════════════════════════════ */

static size_t sanitize_floats(float *data, size_t count) {
    size_t sanitized = 0;
    for (size_t i = 0; i < count; i++) {
        if (isnan(data[i]) || isinf(data[i])) {
            data[i] = 0.0f;
            sanitized++;
        }
    }
    return sanitized;
}

/* Tensor modes stored in the binary header */
#define TENSOR_MODE_DELTA    0   /* f32 delta-quantized (int32 deltas) */
#define TENSOR_MODE_LOSSLESS 1   /* f32 raw */
#define TENSOR_MODE_F16      2   /* IEEE 754 half-precision */
#define TENSOR_MODE_F8       3   /* E4M3 (NVIDIA FP8) */
#define TENSOR_MODE_F4       4   /* 4-bit symmetric block quantization */
#define TENSOR_MODE_A8       5   /* bit-exact array, 1-byte elems (qtc transpose/delta) */
#define TENSOR_MODE_A16      6   /* bit-exact array, 2-byte elems (fp16/bf16 bits) */
#define TENSOR_MODE_A32      7   /* bit-exact array, 4-byte elems (int32/uint32) */
#define TENSOR_MODE_A64      8   /* bit-exact array, 8-byte elems (f64/int64) */
#define TENSOR_MODE_CHUNKED  9   /* chunked payload: any unit count (see below) */

static uint8_t tensor_mode_elem(uint8_t mode) {
    switch (mode) {
        case TENSOR_MODE_A8: return 1;
        case TENSOR_MODE_A16: return 2;
        case TENSOR_MODE_A32: return 4;
        case TENSOR_MODE_A64: return 8;
        default: return 0;
    }
}

/* Forward: chunked layout is defined below the entry points. */
static int tensor_chunk_encode(qtsq_context_t *ctx,
                               const uint8_t *data, size_t count,
                               uint8_t elem_eff, int is_f32,
                               const uint32_t *dimensions, uint8_t num_dims,
                               int packed_nibbles, int level);

/* Map the qtsq level (0 full, 1 fast, 2 store) onto qtc lens options. */
static void qtc_level_opts(qtc_options_t *o, int level) {
    qtc_options_default(o);
    if (level == 1) o->level = 1;
    else if (level == 2) o->level = 0;
}

/* Suffix for schema.encoding honesty. */
static const char *qtc_level_suffix(int level) {
    if (level == 1) return "_FAST";
    if (level == 2) return "_STORED";
    return "";
}

/* ══════════════════════════════════════════════════════════════
 *  IEEE 754 Half-Precision (f16) Converter
 *  1 sign + 5 exponent + 10 mantissa = 16 bits
 *  Range: ±65504, precision: ~3 decimal digits
 * ══════════════════════════════════════════════════════════════ */

static uint16_t f32_to_f16(float value) {
    uint32_t f;
    memcpy(&f, &value, 4);

    uint32_t sign = (f >> 16) & 0x8000;
    int32_t  exp  = ((f >> 23) & 0xFF) - 127;
    uint32_t mant = f & 0x007FFFFF;

    if (exp > 15) {
        /* Overflow → clamp to max f16 (65504) */
        return (uint16_t)(sign | 0x7BFF);
    } else if (exp < -14) {
        /* Underflow → denorm or zero */
        if (exp < -24) return (uint16_t)sign; /* too small → ±0 */
        mant |= 0x00800000; /* add implicit 1 */
        int shift = -exp - 1;
        /* Round to nearest even */
        uint32_t round_bit = 1U << (shift + 12);
        mant += (round_bit >> 1);
        if ((mant & round_bit) && (mant & (round_bit - 1)) == 0)
            mant &= ~(round_bit >> 1); /* tie to even */
        return (uint16_t)(sign | (mant >> (shift + 13)));
    } else {
        /* Normal range */
        uint32_t h_exp = (uint32_t)(exp + 15) << 10;
        uint32_t h_mant = (mant + 0x00001000) >> 13; /* round */
        if (h_mant > 0x3FF) { h_mant = 0; h_exp += 0x0400; } /* carry */
        return (uint16_t)(sign | h_exp | h_mant);
    }
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;

    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign; /* ±zero */
        } else {
            /* Denormalized → normalize */
            exp = 1;
            while (!(mant & 0x0400)) { mant <<= 1; exp--; }
            mant &= 0x03FF;
            f = sign | ((uint32_t)(127 - 15 + exp) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000 | (mant << 13); /* Inf/NaN */
    } else {
        f = sign | ((uint32_t)(exp - 15 + 127) << 23) | (mant << 13);
    }

    float result;
    memcpy(&result, &f, 4);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 *  E4M3 (NVIDIA FP8) Converter
 *  1 sign + 4 exponent + 3 mantissa = 8 bits
 *  Range: ±448, precision: ~1 decimal digit
 *  Bias = 7, no Inf representation (all exponent=15 are NaN)
 * ══════════════════════════════════════════════════════════════ */

static uint8_t f32_to_f8_e4m3(float value) {
    uint32_t f;
    memcpy(&f, &value, 4);

    uint8_t sign = (f >> 24) & 0x80;
    int32_t exp  = ((f >> 23) & 0xFF) - 127;
    uint32_t mant = f & 0x007FFFFF;

    /* Handle special values */
    if (isnan(value)) return sign | 0x7F; /* NaN = all 1s */
    if (value == 0.0f) return sign;

    float abs_val = fabsf(value);
    if (abs_val > 448.0f) return sign | 0x7E; /* clamp to max (±448) */

    if (exp > 8) {
        return sign | 0x7E; /* overflow → max */
    } else if (exp < -6) {
        /* Denorm: exp bits = 0, mantissa encodes value */
        if (exp < -9) return sign; /* too small → zero */
        mant |= 0x00800000;
        int shift = -exp + 6;
        uint32_t m = (mant >> (23 - 3 + shift));
        return sign | (uint8_t)(m & 0x07);
    } else {
        /* Normal */
        uint32_t e = (uint32_t)(exp + 7) & 0x0F;
        uint32_t m = (mant + (1 << 19)) >> 20; /* round to 3 bits */
        if (m > 7) { m = 0; e++; }
        if (e > 15) { e = 15; m = 6; } /* clamp */
        return sign | (uint8_t)((e << 3) | (m & 0x07));
    }
}

static float f8_e4m3_to_f32(uint8_t v) {
    uint32_t sign = (uint32_t)(v & 0x80) << 24;
    uint32_t exp  = (v >> 3) & 0x0F;
    uint32_t mant = v & 0x07;

    if (exp == 15 && mant == 7) {
        /* NaN */
        uint32_t f = sign | 0x7FC00000;
        float r; memcpy(&r, &f, 4); return r;
    }

    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            /* Denorm */
            float val = (float)mant / 8.0f;
            val *= (1.0f / 64.0f); /* 2^(-bias) = 2^(-6) for denorms */
            float result = (v & 0x80) ? -val : val;
            return result;
        }
    } else {
        /* Normal: value = (-1)^s × 2^(exp-7) × (1 + mant/8) */
        f = sign | ((uint32_t)(exp - 7 + 127) << 23) | (mant << 20);
    }

    float result;
    memcpy(&result, &f, 4);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 *  4-bit Symmetric Block Quantization (f4)
 *  Like GGUF Q4_0: each block of 32 values shares one f32 scale.
 *  4 bits → 16 levels → values mapped to [-8..7] × scale
 *  Two values packed per byte (high nibble first).
 *
 *  Binary layout per block:
 *    [4B]  scale (float32)
 *    [16B] 32 nibbles packed into 16 bytes
 *  Total: 20 bytes per block of 32 floats (vs 128 bytes for f32)
 *  Compression ratio: 6.4:1
 * ══════════════════════════════════════════════════════════════ */

#define F4_BLOCK_SIZE 32
#define F4_BLOCK_BYTES (4 + F4_BLOCK_SIZE / 2)  /* 20 bytes */

static size_t f4_packed_size(size_t count) {
    size_t num_blocks = (count + F4_BLOCK_SIZE - 1) / F4_BLOCK_SIZE;
    return num_blocks * F4_BLOCK_BYTES;
}

static void f32_to_f4_block(const float *src, size_t n, uint8_t *dst) {
    /* Find max absolute value in block */
    float amax = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float a = fabsf(src[i]);
        if (a > amax) amax = a;
    }

    /* Scale: maps max abs value to 8 (full range of signed 4-bit: [-8, 7]).
     * Using /8 (not /7) so the -8 level is reachable, matching GGUF Q4_0. */
    float scale = (amax > 0) ? amax / 8.0f : 0.0f;
    float inv_scale = (scale > 0) ? 1.0f / scale : 0.0f;

    /* Write scale */
    memcpy(dst, &scale, 4);
    dst += 4;

    /* Quantize and pack (2 values per byte, high nibble first) */
    for (size_t i = 0; i < F4_BLOCK_SIZE; i += 2) {
        int8_t q0 = 0, q1 = 0;
        if (i < n) {
            float v = src[i] * inv_scale;
            q0 = (int8_t)roundf(v);
            if (q0 > 7) q0 = 7;
            if (q0 < -8) q0 = -8;
        }
        if (i + 1 < n) {
            float v = src[i + 1] * inv_scale;
            q1 = (int8_t)roundf(v);
            if (q1 > 7) q1 = 7;
            if (q1 < -8) q1 = -8;
        }
        dst[i / 2] = (uint8_t)(((q0 + 8) << 4) | ((q1 + 8) & 0x0F));
    }
}

static void f4_block_to_f32(const uint8_t *src, size_t n, float *dst) {
    float scale;
    memcpy(&scale, src, 4);
    src += 4;

    for (size_t i = 0; i < F4_BLOCK_SIZE && i < n; i += 2) {
        uint8_t packed = src[i / 2];
        int8_t q0 = (int8_t)((packed >> 4) & 0x0F) - 8;
        int8_t q1 = (int8_t)(packed & 0x0F) - 8;
        dst[i] = (float)q0 * scale;
        if (i + 1 < n) dst[i + 1] = (float)q1 * scale;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_tensor (default f32 delta)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_tensor(qtsq_context_t *ctx,
                          const float *data, size_t count,
                          const uint32_t *dimensions, uint8_t num_dims,
                          int level) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (ctx->codec == QTSQ_CODEC_ZLIB) return QTSQ_ERR_STRATEGY; /* qtc-only */
    if (count == 0) return QTSQ_ERR_TOO_SMALL;
    if (num_dims > 8) return QTSQ_ERR_FORMAT;

    /* Make a mutable copy so we can sanitize NaN/Inf without modifying caller's data */
    float *safe_data = (float *)malloc(count * sizeof(float));
    if (!safe_data) return QTSQ_ERR_ALLOC;
    memcpy(safe_data, data, count * sizeof(float));
    sanitize_floats(safe_data, count);

    /* ── Step 1: Compute deltas and find max absolute delta ── */
    double *deltas = NULL;
    double max_abs_delta = 0.0;
    double first_value = (double)safe_data[0];
    int use_lossless = 0;

    if (count > 1) {
        deltas = (double *)malloc((count - 1) * sizeof(double));
        if (!deltas) { free(safe_data); return QTSQ_ERR_ALLOC; }

        for (size_t i = 0; i < count - 1; i++) {
            deltas[i] = (double)safe_data[i + 1] - (double)safe_data[i];
            double abs_d = fabs(deltas[i]);
            if (abs_d > max_abs_delta) max_abs_delta = abs_d;
        }

        if (max_abs_delta < 1e-30) {
            max_abs_delta = 1.0; /* avoid division by zero for constant tensor */
        }
    } else {
        use_lossless = 1;
    }

    /* ── Step 2: Determine mode ── */
    double scale = max_abs_delta / (double)DELTA_MAX_RANGE;

    /* Fallback to lossless if deltas are too wild */
    if (!use_lossless && max_abs_delta > 1e6) {
        use_lossless = 1;
    }

    /* ── Step 3: Encode payload ── */
    uint8_t *payload = NULL;
    size_t payload_size = 0;

    if (use_lossless || count <= 1) {
        payload_size = count * sizeof(float);
        payload = (uint8_t *)malloc(payload_size);
        if (!payload) { free(deltas); free(safe_data); return QTSQ_ERR_ALLOC; }
        memcpy(payload, safe_data, payload_size);
        use_lossless = 1;
    } else {
        payload_size = (count - 1) * sizeof(int32_t);
        payload = (uint8_t *)malloc(payload_size);
        if (!payload) { free(deltas); free(safe_data); return QTSQ_ERR_ALLOC; }

        int32_t *q = (int32_t *)payload;
        for (size_t i = 0; i < count - 1; i++) {
            double normalized = deltas[i] / scale;
            if (normalized > 2147483647.0) normalized = 2147483647.0;
            if (normalized < -2147483648.0) normalized = -2147483648.0;
            q[i] = (int32_t)round(normalized);
        }
    }
    free(deltas);

    /* ── Steps 3.5+4: qtc lens bytes stored verbatim ──
     * The typed lens (transpose/delta/per-plane contest) codes the
     * payload directly; the outer generic pass is skipped (it would
     * only add a STORED container around high-entropy bytes).
     * reserved[0]: 0 = legacy generic-outer file, 1 = lens bytes,
     * 2 = verbatim raw payload (incompressible — no LZ tax). */
    uint8_t r0 = 2;
    uint8_t *blob = payload; /* default: verbatim */
    size_t blob_len = payload_size;
    if (count > ((size_t)1 << 30) + 1) {
        free(payload);
        free(safe_data);
        return QTSQ_ERR_TOO_LARGE; /* RAW chunks instead; legacy stays capped */
    }
    {
        qtc_options_t lens_opts;
        qtc_level_opts(&lens_opts, level);
        if (use_lossless) {
            uint8_t *inner = NULL;
            size_t inner_len = 0;
            if (qtc_tensor_encode((const float *)payload, count,
                                  &inner, &inner_len, &lens_opts) == 0 && inner &&
                inner_len < payload_size) {
                free(payload);
                blob = inner;
                blob_len = inner_len;
                r0 = 1;
            } else {
                free(inner);
            }
        } else {
            uint8_t *inner = NULL;
            size_t inner_len = 0;
            if (qtc_compress_array(payload, (uint32_t)(count - 1), 4, 0,
                                   &inner, &inner_len, &lens_opts) == 0 && inner &&
                inner_len < payload_size) {
                free(payload);
                blob = inner;
                blob_len = inner_len;
                r0 = 1;
            } else {
                free(inner);
            }
        }
    }

    /* ── Step 5: checksum over the stored blob ── */
    uint8_t payload_hash[32];
    qtsq_sha256(blob, blob_len, payload_hash);
    free(safe_data);

    /* ── Step 6: Pack header + blob ── */
    /* magic(4) + mode(1) + num_dims(1) + reserved(2) + count(8) +
       scale(8) + first_value(8) + dims(num_dims*4) + hash(32) + uncomp_size(8) */
    size_t header_size = 4 + 1 + 1 + 2 + 8 + 8 + 8 + (num_dims * 4) + 32 + 8;

    size_t total = header_size + blob_len;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(blob); return QTSQ_ERR_ALLOC; }

    size_t p = 0;

    /* Magic */
    out[p++] = TENSOR_MAGIC_0;
    out[p++] = TENSOR_MAGIC_1;
    out[p++] = TENSOR_MAGIC_2;
    out[p++] = TENSOR_MAGIC_3;

    /* Mode */
    out[p++] = use_lossless ? TENSOR_MODE_LOSSLESS : TENSOR_MODE_DELTA;

    /* Num dims */
    out[p++] = num_dims;

    /* Reserved: outer framing selector (byte 0), zero (byte 1) */
    out[p++] = r0;
    out[p++] = 0;

    /* Count */
    uint64_t count64 = (uint64_t)count;
    memcpy(out + p, &count64, 8); p += 8;

    /* Scale factor */
    memcpy(out + p, &scale, 8); p += 8;

    /* First value */
    memcpy(out + p, &first_value, 8); p += 8;

    /* Dimensions — embedded so they survive container round-trip */
    if (dimensions && num_dims > 0) {
        memcpy(out + p, dimensions, num_dims * 4);
    }
    p += num_dims * 4;

    /* Payload checksum — for corruption detection on decompress */
    memcpy(out + p, payload_hash, 32); p += 32;

    /* Uncompressed size */
    uint64_t uncomp = (uint64_t)payload_size;
    memcpy(out + p, &uncomp, 8); p += 8;

    /* Stored blob: lens bytes (r0=1) or verbatim payload (r0=2);
     * legacy files (r0=0) carry a generic qtc/zlib stream here. */
    memcpy(out + p, blob, blob_len);
    p += blob_len;
    free(blob);

    /* ── Step 6: Store in context ── */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;

    ctx->header.strategy = QTSQ_STRATEGY_TENSOR;
    ctx->header.data_type = QTSQ_TYPE_TENSOR;
    ctx->header.original_size = count * sizeof(float);
    ctx->header.singularity_size = total;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    if (!use_lossless) ctx->header.flags |= QTSQ_FLAG_LOSSY;

    /* Schema */
    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "neural_tensor", 63);
    ctx->schema.data_type = QTSQ_TYPE_TENSOR;
    ctx->schema.num_dims = num_dims;
    if (dimensions) {
        for (int i = 0; i < num_dims && i < 8; i++)
            ctx->schema.dimensions[i] = dimensions[i];
    }
    snprintf(ctx->schema.encoding, sizeof(ctx->schema.encoding), "%s%s",
             use_lossless ? "TENSOR_F32_SANITIZED" : "TENSOR_DELTA_Q32_BITEXACT",
             qtc_level_suffix(level));

    qtsq_sha256((const uint8_t *)data, count * sizeof(float), ctx->header.checksum);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_tensor_quantized
 *  Supports f32 (delta), f16, f8, f4 precision levels.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_tensor_quantized(qtsq_context_t *ctx,
                                    const float *data, size_t count,
                                    const uint32_t *dimensions, uint8_t num_dims,
                                    qtsq_tensor_precision_t precision,
                                    int level) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (ctx->codec == QTSQ_CODEC_ZLIB) return QTSQ_ERR_STRATEGY; /* qtc-only */
    /* f32 keeps legacy delta path for backward compat (lossy ~1e-8).
     * RAW is now TRUE bit-exact: no sanitize, no delta, raw IEEE-754
     * bits preserved (NaN/Inf included). Use RAW for model weights
     * where retraining-quality exactness is required. */
    if (precision == QTSQ_TENSOR_F32) {
        return qtsq_compress_tensor(ctx, data, count, dimensions, num_dims, level);
    }
    if (precision == QTSQ_TENSOR_RAW) {
        if (!ctx || !data) return QTSQ_ERR_NULL;
        if (count == 0) return QTSQ_ERR_TOO_SMALL;
        if (num_dims > 8) return QTSQ_ERR_FORMAT;

        /* Beyond qtc's u32 unit cap the payload is chunked (mode 9) —
         * same bit-exact bytes, no unit-count limit. */
        if (count > ((size_t)1 << 30))
            return tensor_chunk_encode(ctx, (const uint8_t *)data, count,
                                         0, 1, dimensions, num_dims, 0, level);

        /* Bit-exact: typed qtc lens bytes stored verbatim (reserved[0]:
         * 1 = lens, 2 = verbatim raw). Preserves every NaN/Inf bit. */
        size_t payload_size = count * sizeof(float);
        uint8_t *inner = NULL;
        size_t inner_len = 0;
        uint8_t r0 = 2;
        qtc_options_t lens_opts;
        qtc_level_opts(&lens_opts, level);
        if (qtc_tensor_encode(data, count, &inner, &inner_len, &lens_opts) == 0 && inner &&
            inner_len < payload_size) {
            r0 = 1;
        } else {
            free(inner);
            inner = (uint8_t *)malloc(payload_size ? payload_size : 1);
            if (!inner) return QTSQ_ERR_ALLOC;
            memcpy(inner, data, payload_size);
            inner_len = payload_size;
        }

        uint8_t payload_hash[32];
        qtsq_sha256(inner, inner_len, payload_hash);

        size_t header_size = 4 + 1 + 1 + 2 + 8 + 8 + 8
                           + (num_dims * 4) + 32 + 8;
        size_t total = header_size + inner_len;
        uint8_t *out = (uint8_t *)malloc(total);
        if (!out) { free(inner); return QTSQ_ERR_ALLOC; }

        size_t p = 0;
        out[p++] = TENSOR_MAGIC_0;
        out[p++] = TENSOR_MAGIC_1;
        out[p++] = TENSOR_MAGIC_2;
        out[p++] = TENSOR_MAGIC_3;
        out[p++] = TENSOR_MODE_LOSSLESS;
        out[p++] = num_dims;
        out[p++] = r0;
        out[p++] = 0;
        uint64_t count64 = (uint64_t)count;
        memcpy(out + p, &count64, 8); p += 8;
        double zero = 0.0;
        memcpy(out + p, &zero, 8); p += 8;
        memcpy(out + p, &zero, 8); p += 8;
        if (dimensions && num_dims > 0)
            memcpy(out + p, dimensions, num_dims * 4);
        p += num_dims * 4;
        memcpy(out + p, payload_hash, 32); p += 32;
        uint64_t uncomp = (uint64_t)payload_size;
        memcpy(out + p, &uncomp, 8); p += 8;
        memcpy(out + p, inner, inner_len); p += inner_len;
        free(inner);

        if (ctx->singularity) free(ctx->singularity);
        ctx->singularity = out;
        ctx->singularity_size = total;

        ctx->header.strategy = QTSQ_STRATEGY_TENSOR;
        ctx->header.data_type = QTSQ_TYPE_TENSOR;
        ctx->header.original_size = payload_size;
        ctx->header.singularity_size = total;
        /* Exact: checksummed, NOT lossy. */
        ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
        ctx->header.flags &= ~QTSQ_FLAG_LOSSY;

        memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
        strncpy(ctx->schema.name, "neural_tensor", 63);
        ctx->schema.data_type = QTSQ_TYPE_TENSOR;
        ctx->schema.num_dims = num_dims;
        if (dimensions) {
            for (int i = 0; i < num_dims && i < 8; i++)
                ctx->schema.dimensions[i] = dimensions[i];
        }
        snprintf(ctx->schema.encoding, sizeof(ctx->schema.encoding), "%s%s",
                 "TENSOR_RAW_BITEXACT", qtc_level_suffix(level));

        qtsq_sha256((const uint8_t *)data, payload_size,
                    ctx->header.checksum);
        return QTSQ_OK;
    }

    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (count == 0) return QTSQ_ERR_TOO_SMALL;
    if (num_dims > 8) return QTSQ_ERR_FORMAT;

    /* Sanitize input */
    float *safe_data = (float *)malloc(count * sizeof(float));
    if (!safe_data) return QTSQ_ERR_ALLOC;
    memcpy(safe_data, data, count * sizeof(float));
    sanitize_floats(safe_data, count);

    /* ── Encode payload based on precision ── */
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    uint8_t mode;
    const char *encoding_name;

    switch (precision) {
        case QTSQ_TENSOR_F16: {
            mode = TENSOR_MODE_F16;
            payload_size = count * sizeof(uint16_t);
            payload = (uint8_t *)malloc(payload_size);
            if (!payload) { free(safe_data); return QTSQ_ERR_ALLOC; }
            uint16_t *dst = (uint16_t *)payload;
            for (size_t i = 0; i < count; i++) {
                dst[i] = f32_to_f16(safe_data[i]);
            }
            encoding_name = "TENSOR_F16_ZLIB";
            break;
        }
        case QTSQ_TENSOR_F8: {
            mode = TENSOR_MODE_F8;
            payload_size = count; /* 1 byte per value */
            payload = (uint8_t *)malloc(payload_size);
            if (!payload) { free(safe_data); return QTSQ_ERR_ALLOC; }
            for (size_t i = 0; i < count; i++) {
                payload[i] = f32_to_f8_e4m3(safe_data[i]);
            }
            encoding_name = "TENSOR_F8_E4M3_ZLIB";
            break;
        }
        case QTSQ_TENSOR_F4: {
            mode = TENSOR_MODE_F4;
            payload_size = f4_packed_size(count);
            payload = (uint8_t *)calloc(1, payload_size);
            if (!payload) { free(safe_data); return QTSQ_ERR_ALLOC; }
            size_t offset = 0;
            for (size_t i = 0; i < count; i += F4_BLOCK_SIZE) {
                size_t block_n = (count - i < F4_BLOCK_SIZE) ? (count - i) : F4_BLOCK_SIZE;
                f32_to_f4_block(safe_data + i, block_n, payload + offset);
                offset += F4_BLOCK_BYTES;
            }
            encoding_name = "TENSOR_F4_Q4_0_ZLIB";
            break;
        }
        default:
            free(safe_data);
            return QTSQ_ERR_FORMAT;
    }

    /* Payload checksum */
    uint8_t payload_hash[32];
    qtsq_sha256(payload, payload_size, payload_hash);

    /* ZLib compress */
    uint8_t *zbuf = NULL;
    size_t zsize = 0;
    if (tensor_payload_compress(ctx, payload, payload_size, &zbuf, &zsize) != 0) {
        free(payload); free(safe_data);
        return QTSQ_ERR_ALLOC;
    }
    free(payload);
    free(safe_data);

    /* Pack header + compressed data */
    /* Same header format: magic(4) + mode(1) + num_dims(1) + reserved(2) + count(8) +
       scale(8) + first_value(8) + dims(num_dims*4) + hash(32) + uncomp_size(8) */
    size_t header_size = 4 + 1 + 1 + 2 + 8 + 8 + 8 + (num_dims * 4) + 32 + 8;
    size_t total = header_size + zsize;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(zbuf); return QTSQ_ERR_ALLOC; }

    size_t p = 0;
    out[p++] = TENSOR_MAGIC_0;
    out[p++] = TENSOR_MAGIC_1;
    out[p++] = TENSOR_MAGIC_2;
    out[p++] = TENSOR_MAGIC_3;
    out[p++] = mode;
    out[p++] = num_dims;
    out[p++] = 0; /* reserved */
    out[p++] = 0;

    uint64_t count64 = (uint64_t)count;
    memcpy(out + p, &count64, 8); p += 8;

    /* Scale/first_value unused for f16/f8/f4 but kept for format compat */
    double zero_scale = 0.0;
    double zero_first = 0.0;
    memcpy(out + p, &zero_scale, 8); p += 8;
    memcpy(out + p, &zero_first, 8); p += 8;

    if (dimensions && num_dims > 0) memcpy(out + p, dimensions, num_dims * 4);
    p += num_dims * 4;

    memcpy(out + p, payload_hash, 32); p += 32;

    uint64_t uncomp = (uint64_t)payload_size;
    memcpy(out + p, &uncomp, 8); p += 8;

    memcpy(out + p, zbuf, zsize); p += zsize;
    free(zbuf);

    /* Store in context */
    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;

    ctx->header.strategy = QTSQ_STRATEGY_TENSOR;
    ctx->header.data_type = QTSQ_TYPE_TENSOR;
    ctx->header.original_size = count * sizeof(float);
    ctx->header.singularity_size = total;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED | QTSQ_FLAG_LOSSY;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "neural_tensor", 63);
    ctx->schema.data_type = QTSQ_TYPE_TENSOR;
    ctx->schema.num_dims = num_dims;
    if (dimensions) {
        for (int i = 0; i < num_dims && i < 8; i++)
            ctx->schema.dimensions[i] = dimensions[i];
    }
    strncpy(ctx->schema.encoding, encoding_name, 63);

    qtsq_sha256((const uint8_t *)data, count * sizeof(float), ctx->header.checksum);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_array / qtsq_decompress_array
 *
 *  Bit-exact storage for non-float32 weight dtypes (fp16/bf16 bits,
 *  int/uint 8..64, packed int4 nibbles, fp8 bits). Same TNS2 envelope
 *  as tensors with modes 5..8. The payload runs the qtc array lens
 *  (transpose + XOR-delta contest, STORED fallback — never worse than
 *  raw) and is stored verbatim as [u64 inner_len][inner]; the inner
 *  lens already did the entropy work, so no second pass is needed.
 *  count = element units (raw bytes = count*elem_size; packed
 *  nibbles: count = packed bytes).
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_array(qtsq_context_t *ctx,
                        const uint8_t *data, size_t count, uint8_t elem_size,
                        const uint32_t *dimensions, uint8_t num_dims,
                        int packed_nibbles, int level) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (ctx->codec == QTSQ_CODEC_ZLIB) return QTSQ_ERR_STRATEGY; /* qtc-only */
    if (count == 0) return QTSQ_ERR_TOO_SMALL;
    if (num_dims > 8) return QTSQ_ERR_FORMAT;
    if (elem_size != 1 && elem_size != 2 &&
        elem_size != 4 && elem_size != 8)
        return QTSQ_ERR_FORMAT;
    /* Beyond qtc's u32 unit cap the payload is chunked (mode 9). */
    if (count > ((size_t)1 << 30))
        return tensor_chunk_encode(ctx, data, count, elem_size, 0,
                                   dimensions, num_dims, packed_nibbles, level);
    if (count > (size_t)-1 / elem_size) return QTSQ_ERR_FORMAT;

    uint8_t mode;
    switch (elem_size) {
        case 1: mode = TENSOR_MODE_A8; break;
        case 2: mode = TENSOR_MODE_A16; break;
        case 4: mode = TENSOR_MODE_A32; break;
        default: mode = TENSOR_MODE_A64; break;
    }

    /* Inner: qtc array lens (never worse than raw — STORED fallback). */
    uint8_t *inner = NULL;
    size_t inner_len = 0;
    qtc_options_t lens_opts;
    qtc_level_opts(&lens_opts, level);
    int qrc = qtc_compress_array(data, (uint32_t)count, elem_size,
                                 packed_nibbles, &inner, &inner_len, &lens_opts);
    if (qrc != 0 || !inner) {
        free(inner);
        return QTSQ_ERR_ALLOC;
    }

    uint8_t inner_hash[32];
    qtsq_sha256(inner, inner_len, inner_hash);

    size_t payload_size = count * elem_size;
    size_t header_size = 4 + 1 + 1 + 2 + 8 + 8 + 8 + (num_dims * 4) + 32 + 8;
    size_t total = header_size + 8 + inner_len;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) { free(inner); return QTSQ_ERR_ALLOC; }

    size_t p = 0;
    out[p++] = TENSOR_MAGIC_0;
    out[p++] = TENSOR_MAGIC_1;
    out[p++] = TENSOR_MAGIC_2;
    out[p++] = TENSOR_MAGIC_3;
    out[p++] = mode;
    out[p++] = num_dims;
    out[p++] = (uint8_t)(packed_nibbles ? 1 : 0);
    out[p++] = 0;
    uint64_t count64 = (uint64_t)count;
    memcpy(out + p, &count64, 8); p += 8;
    double zero = 0.0;
    memcpy(out + p, &zero, 8); p += 8;
    memcpy(out + p, &zero, 8); p += 8;
    if (dimensions && num_dims > 0)
        memcpy(out + p, dimensions, num_dims * 4);
    p += num_dims * 4;
    memcpy(out + p, inner_hash, 32); p += 32;
    uint64_t uncomp = (uint64_t)payload_size;
    memcpy(out + p, &uncomp, 8); p += 8;
    uint64_t ilen64 = (uint64_t)inner_len;
    memcpy(out + p, &ilen64, 8); p += 8;
    memcpy(out + p, inner, inner_len); p += inner_len;
    free(inner);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;

    ctx->header.strategy = QTSQ_STRATEGY_TENSOR;
    ctx->header.data_type = QTSQ_TYPE_TENSOR;
    ctx->header.original_size = payload_size;
    ctx->header.singularity_size = total;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    ctx->header.flags &= ~QTSQ_FLAG_LOSSY;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, "weight_array", 63);
    ctx->schema.data_type = QTSQ_TYPE_TENSOR;
    ctx->schema.num_dims = num_dims;
    if (dimensions) {
        for (int i = 0; i < num_dims && i < 8; i++)
            ctx->schema.dimensions[i] = dimensions[i];
    }
    snprintf(ctx->schema.encoding, sizeof(ctx->schema.encoding), "%s%s",
             "TENSOR_ARRAY_BITEXACT", qtc_level_suffix(level));

    qtsq_sha256(data, payload_size, ctx->header.checksum);
    return QTSQ_OK;
}

int qtsq_decompress_array(const qtsq_context_t *ctx,
                          uint8_t **out_data, size_t *out_count,
                          uint8_t *out_elem) {
    if (!ctx || !out_data || !out_count) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 40) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total = (size_t)ctx->singularity_size;
    size_t p = 0;

    if (buf[0] != TENSOR_MAGIC_0 || buf[1] != TENSOR_MAGIC_1 ||
        buf[2] != TENSOR_MAGIC_2 || buf[3] != TENSOR_MAGIC_3)
        return QTSQ_ERR_FORMAT;
    p += 4;

    uint8_t mode = buf[p++];
    uint8_t elem = tensor_mode_elem(mode);
    if (elem == 0) return QTSQ_ERR_FORMAT; /* not an array mode */

    uint8_t num_dims = buf[p++];
    if (num_dims > 8) return QTSQ_ERR_FORMAT;
    p += 2; /* reserved (byte 0 carries packed-nibbles flag, informational) */

    uint64_t count64;
    memcpy(&count64, buf + p, 8); p += 8;
    size_t count = (size_t)count64;

    double scale, first;
    memcpy(&scale, buf + p, 8); p += 8;
    memcpy(&first, buf + p, 8); p += 8;
    if (isnan(scale) || isinf(scale) || isnan(first) || isinf(first))
        return QTSQ_ERR_FORMAT;
    if (count == 0) return QTSQ_ERR_FORMAT;
    if (count > (size_t)-1 / elem) return QTSQ_ERR_FORMAT;

    uint32_t dims[8] = {0};
    if (num_dims > 0) {
        if (p + num_dims * 4 > total) return QTSQ_ERR_FORMAT;
        memcpy(dims, buf + p, num_dims * 4);
    }
    p += num_dims * 4;

    if (p + 32 > total) return QTSQ_ERR_FORMAT;
    uint8_t stored_hash[32];
    memcpy(stored_hash, buf + p, 32); p += 32;

    if (p + 8 > total) return QTSQ_ERR_FORMAT;
    uint64_t uncomp_size;
    memcpy(&uncomp_size, buf + p, 8); p += 8;
    if (uncomp_size != (uint64_t)count * elem) return QTSQ_ERR_FORMAT;

    if (p + 8 > total) return QTSQ_ERR_FORMAT;
    uint64_t inner_len64;
    memcpy(&inner_len64, buf + p, 8); p += 8;
    /* Inner is framed array data: at least 6 bytes, at most raw+6. */
    if (inner_len64 < 6 || inner_len64 > uncomp_size + 6) return QTSQ_ERR_FORMAT;
    size_t inner_len = (size_t)inner_len64;
    if (p + inner_len != total) return QTSQ_ERR_FORMAT;

    const uint8_t *inner = buf + p;

    uint8_t computed[32];
    qtsq_sha256(inner, inner_len, computed);
    if (memcmp(stored_hash, computed, 32) != 0) {
        return QTSQ_ERR_CHECKSUM;
    }

    uint8_t *arr = NULL;
    uint32_t acount = 0;
    uint8_t aelem = 0;
    if (qtc_decompress_array(inner, inner_len, &arr, &acount, &aelem) != 0) {
        return QTSQ_ERR_FORMAT;
    }

    if (aelem != elem || acount != (uint32_t)count ||
        (size_t)acount * aelem != (size_t)uncomp_size) {
        free(arr);
        return QTSQ_ERR_CHECKSUM;
    }

    if (num_dims > 0) {
        qtsq_context_t *m = (qtsq_context_t *)ctx;
        m->schema.num_dims = num_dims;
        for (int i = 0; i < num_dims && i < 8; i++)
            m->schema.dimensions[i] = dims[i];
    }
    *out_data = arr;
    *out_count = (size_t)acount * aelem; /* bytes */
    if (out_elem) *out_elem = aelem;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Chunked payloads (TNS2 mode 9): no unit-count limit.
 *
 *  qtc's lens framings count in u32 (cap 2^30 units ≈ 4 GiB of f32),
 *  but TNS2 counts in u64. Mode 9 bridges the gap: the payload is a
 *  table of independently-coded chunks, each within qtc limits:
 *
 *    inner = [u32 nchunks]([u64 chunk_len][chunk_bytes])*
 *
 *  f32 chunks hold qtc-tensor-lens bytes; array chunks hold
 *  qtc-array-lens bytes. reserved[1] carries elem_size (0 = float32).
 *  Chunks encode/decode streaming-style (peak RAM ~ one chunk), while
 *  the final concat is necessarily one buffer (documented).
 *  Old readers reject mode 9 loudly (unknown mode = corrupt).
 * ══════════════════════════════════════════════════════════════ */

#define QTSQ_CHUNK_UNITS ((size_t)1 << 28)  /* 256M units per chunk */

static int chunked_append(uint8_t **buf, size_t *len, size_t *cap,
                          const uint8_t *data, size_t n) {
    if (*len + n < *len) return -1;
    if (*len + n > *cap) {
        size_t ncap = (*len + n) * 2;
        if (ncap < *len + n) return -1;
        uint8_t *nb = (uint8_t *)realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 0;
}

static int tensor_chunk_encode(qtsq_context_t *ctx,
                                 const uint8_t *data, size_t count,
                                 uint8_t elem_eff, int is_f32,
                                 const uint32_t *dimensions, uint8_t num_dims,
                                 int packed_nibbles, int level) {
    size_t elem = is_f32 ? 4 : elem_eff;
    size_t nchunks = (count + QTSQ_CHUNK_UNITS - 1) / QTSQ_CHUNK_UNITS;
    if (nchunks == 0 || nchunks > 0xFFFFFFFFu) return QTSQ_ERR_TOO_LARGE;
    if (count > (size_t)-1 / elem) return QTSQ_ERR_FORMAT;

    uint8_t *inner = NULL;
    size_t ilen = 0, icap = 0;
    uint32_t nc32 = (uint32_t)nchunks;
    if (chunked_append(&inner, &ilen, &icap, (uint8_t *)&nc32, 4) != 0)
        return QTSQ_ERR_ALLOC;

    for (size_t c = 0; c < nchunks; c++) {
        size_t off = c * QTSQ_CHUNK_UNITS;
        size_t cn = count - off < QTSQ_CHUNK_UNITS ? count - off : QTSQ_CHUNK_UNITS;
        uint8_t *chunk = NULL;
        size_t chunk_len = 0;
        int qrc;
        qtc_options_t lens_opts;
        qtc_level_opts(&lens_opts, level);
        if (is_f32) {
            qrc = qtc_tensor_encode((const float *)(data + off * 4), cn,
                                    &chunk, &chunk_len, &lens_opts);
        } else {
            qrc = qtc_compress_array(data + off * elem, (uint32_t)cn,
                                     (uint8_t)elem, packed_nibbles,
                                     &chunk, &chunk_len, &lens_opts);
        }
        if (qrc != 0 || !chunk) {
            free(chunk);
            free(inner);
            return QTSQ_ERR_ALLOC;
        }
        uint64_t cl64 = (uint64_t)chunk_len;
        int ar = chunked_append(&inner, &ilen, &icap, (uint8_t *)&cl64, 8);
        if (ar == 0) ar = chunked_append(&inner, &ilen, &icap, chunk, chunk_len);
        free(chunk);
        if (ar != 0) {
            free(inner);
            return QTSQ_ERR_ALLOC;
        }
    }

    uint8_t inner_hash[32];
    qtsq_sha256(inner, ilen, inner_hash);

    size_t payload_size = count * elem;
    size_t header_size = 4 + 1 + 1 + 2 + 8 + 8 + 8 + (num_dims * 4) + 32 + 8;
    size_t total = header_size + ilen;
    uint8_t *out = (uint8_t *)malloc(total ? total : 1);
    if (!out) { free(inner); return QTSQ_ERR_ALLOC; }

    size_t p = 0;
    out[p++] = TENSOR_MAGIC_0;
    out[p++] = TENSOR_MAGIC_1;
    out[p++] = TENSOR_MAGIC_2;
    out[p++] = TENSOR_MAGIC_3;
    out[p++] = TENSOR_MODE_CHUNKED;
    out[p++] = num_dims;
    out[p++] = (uint8_t)(packed_nibbles ? 1 : 0);
    out[p++] = is_f32 ? 0 : (uint8_t)elem;
    uint64_t count64 = (uint64_t)count;
    memcpy(out + p, &count64, 8); p += 8;
    double zero = 0.0;
    memcpy(out + p, &zero, 8); p += 8;
    memcpy(out + p, &zero, 8); p += 8;
    if (dimensions && num_dims > 0)
        memcpy(out + p, dimensions, num_dims * 4);
    p += num_dims * 4;
    memcpy(out + p, inner_hash, 32); p += 32;
    uint64_t uncomp = (uint64_t)payload_size;
    memcpy(out + p, &uncomp, 8); p += 8;
    memcpy(out + p, inner, ilen); p += ilen;
    free(inner);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = out;
    ctx->singularity_size = total;

    ctx->header.strategy = QTSQ_STRATEGY_TENSOR;
    ctx->header.data_type = QTSQ_TYPE_TENSOR;
    ctx->header.original_size = payload_size;
    ctx->header.singularity_size = total;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
    ctx->header.flags &= ~QTSQ_FLAG_LOSSY;

    memset(&ctx->schema, 0, sizeof(qtsq_schema_t));
    strncpy(ctx->schema.name, is_f32 ? "neural_tensor" : "weight_array", 63);
    ctx->schema.data_type = QTSQ_TYPE_TENSOR;
    ctx->schema.num_dims = num_dims;
    if (dimensions) {
        for (int i = 0; i < num_dims && i < 8; i++)
            ctx->schema.dimensions[i] = dimensions[i];
    }
    strncpy(ctx->schema.encoding,
            is_f32 ? "TENSOR_RAW_BITEXACT" : "TENSOR_ARRAY_BITEXACT", 63);

    qtsq_sha256(data, payload_size, ctx->header.checksum);
    return QTSQ_OK;
}

/* Returns raw bytes; out_elem 0 = float32, else element size. */
static int tensor_chunk_decode(const qtsq_context_t *ctx,
                                   uint8_t **out_data, size_t *out_count,
                                   uint8_t *out_elem) {
    if (!ctx || !out_data || !out_count) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 40) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total = (size_t)ctx->singularity_size;
    size_t p = 0;

    if (buf[0] != TENSOR_MAGIC_0 || buf[1] != TENSOR_MAGIC_1 ||
        buf[2] != TENSOR_MAGIC_2 || buf[3] != TENSOR_MAGIC_3)
        return QTSQ_ERR_FORMAT;
    p += 4;
    if (buf[p++] != TENSOR_MODE_CHUNKED) return QTSQ_ERR_FORMAT;

    uint8_t num_dims = buf[p++];
    if (num_dims > 8) return QTSQ_ERR_FORMAT;
    p += 1; /* packed-nibbles flag (informational) */
    uint8_t elem_mark = buf[p++];
    int is_f32 = (elem_mark == 0);
    size_t elem = is_f32 ? 4 : elem_mark;
    if (!is_f32 && elem != 1 && elem != 2 && elem != 4 && elem != 8)
        return QTSQ_ERR_FORMAT;

    uint64_t count64;
    memcpy(&count64, buf + p, 8); p += 8;
    size_t count = (size_t)count64;

    double scale, first;
    memcpy(&scale, buf + p, 8); p += 8;
    memcpy(&first, buf + p, 8); p += 8;
    if (isnan(scale) || isinf(scale) || isnan(first) || isinf(first))
        return QTSQ_ERR_FORMAT;
    if (count == 0 || count > (size_t)-1 / elem) return QTSQ_ERR_FORMAT;

    uint32_t dims[8] = {0};
    if (num_dims > 0) {
        if (p + num_dims * 4 > total) return QTSQ_ERR_FORMAT;
        memcpy(dims, buf + p, num_dims * 4);
    }
    p += num_dims * 4;

    if (p + 32 > total) return QTSQ_ERR_FORMAT;
    uint8_t stored_hash[32];
    memcpy(stored_hash, buf + p, 32); p += 32;

    if (p + 8 > total) return QTSQ_ERR_FORMAT;
    uint64_t uncomp_size;
    memcpy(&uncomp_size, buf + p, 8); p += 8;
    if (uncomp_size != (uint64_t)count * elem) return QTSQ_ERR_FORMAT;

    size_t inner_off = p;
    size_t inner_end = total;
    uint8_t computed[32];
    qtsq_sha256(buf + inner_off, inner_end - inner_off, computed);
    if (memcmp(stored_hash, computed, 32) != 0)
        return QTSQ_ERR_CHECKSUM;

    if (inner_end - inner_off < 4) return QTSQ_ERR_FORMAT;
    uint32_t nchunks;
    memcpy(&nchunks, buf + inner_off, 4);
    p = inner_off + 4;
    size_t expect_chunks =
        (count + QTSQ_CHUNK_UNITS - 1) / QTSQ_CHUNK_UNITS;
    if (nchunks == 0 || (uint64_t)nchunks != expect_chunks)
        return QTSQ_ERR_CHECKSUM;
    /* Sanity cap: chunk table entries alone must fit the payload. */
    if ((uint64_t)nchunks * 8 + 4 > inner_end - inner_off)
        return QTSQ_ERR_FORMAT;

    size_t payload_size = count * elem;
    uint8_t *out = (uint8_t *)malloc(payload_size ? payload_size : 1);
    if (!out) return QTSQ_ERR_ALLOC;

    size_t got = 0;
    for (uint32_t c = 0; c < nchunks; c++) {
        if (p + 8 > inner_end) { free(out); return QTSQ_ERR_FORMAT; }
        uint64_t chunk_len;
        memcpy(&chunk_len, buf + p, 8); p += 8;
        if (chunk_len == 0 || chunk_len > inner_end || p + chunk_len > inner_end) {
            free(out);
            return QTSQ_ERR_FORMAT;
        }
        size_t want = count - got < QTSQ_CHUNK_UNITS ? count - got : QTSQ_CHUNK_UNITS;
        if (is_f32) {
            extern int qtc_tensor_decode(const uint8_t *, size_t, float **, size_t *);
            float *f = NULL;
            size_t fn = 0;
            if (qtc_tensor_decode(buf + p, (size_t)chunk_len, &f, &fn) != 0 ||
                fn != want) {
                free(f);
                free(out);
                return QTSQ_ERR_FORMAT;
            }
            memcpy(out + got * 4, f, want * 4);
            free(f);
        } else {
            uint8_t *arr = NULL;
            uint32_t ac = 0;
            uint8_t ae = 0;
            if (qtc_decompress_array(buf + p, (size_t)chunk_len, &arr, &ac,
                                     &ae) != 0 ||
                ac != want || ae != elem) {
                free(arr);
                free(out);
                return QTSQ_ERR_FORMAT;
            }
            memcpy(out + got * elem, arr, want * elem);
            free(arr);
        }
        p += (size_t)chunk_len;
        got += want;
    }
    if (got != count || p != inner_end) {
        free(out);
        return QTSQ_ERR_CHECKSUM;
    }

    if (num_dims > 0) {
        qtsq_context_t *m = (qtsq_context_t *)ctx;
        m->schema.num_dims = num_dims;
        for (int i = 0; i < num_dims && i < 8; i++)
            m->schema.dimensions[i] = dims[i];
    }
    *out_data = out;
    *out_count = payload_size; /* bytes */
    if (out_elem) *out_elem = (uint8_t)(is_f32 ? 0 : elem);
    return QTSQ_OK;
}

/* Public chunked API (custom engines + dispatcher). Array payloads;
 * float32-huge routes internally with is_f32=1. */
int qtsq_compress_chunked(qtsq_context_t *ctx,
                          const uint8_t *data, size_t count, uint8_t elem_size,
                          const uint32_t *dimensions, uint8_t num_dims,
                          int level) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (ctx->codec == QTSQ_CODEC_ZLIB) return QTSQ_ERR_STRATEGY; /* qtc-only */
    if (count == 0) return QTSQ_ERR_TOO_SMALL;
    if (num_dims > 8) return QTSQ_ERR_FORMAT;
    if (elem_size != 1 && elem_size != 2 &&
        elem_size != 4 && elem_size != 8)
        return QTSQ_ERR_FORMAT;
    return tensor_chunk_encode(ctx, data, count, elem_size, 0,
                               dimensions, num_dims, 0, level);
}

int qtsq_decompress_chunked(const qtsq_context_t *ctx,
                            uint8_t **out_data, size_t *out_count,
                            uint8_t *out_elem) {
    return tensor_chunk_decode(ctx, out_data, out_count, out_elem);
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_tensor
 *  Handles modes 0..4 plus chunked f32 (mode 9, elem_mark 0)
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_tensor(const qtsq_context_t *ctx,
                            float **out_data, size_t *out_count) {
    if (!ctx || !out_data || !out_count) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size < 40) return QTSQ_ERR_FORMAT;

    const uint8_t *buf = ctx->singularity;
    size_t total = (size_t)ctx->singularity_size;
    size_t p = 0;

    /* ── Verify magic ── */
    if (buf[0] != TENSOR_MAGIC_0 || buf[1] != TENSOR_MAGIC_1 ||
        buf[2] != TENSOR_MAGIC_2 || buf[3] != TENSOR_MAGIC_3)
        return QTSQ_ERR_FORMAT;
    p += 4;

    /* ── Read header ── */
    uint8_t mode = buf[p++];
    if (mode == TENSOR_MODE_CHUNKED) {
        /* Huge payloads: chunk table decodes to raw bytes (f32 only
         * for the float API; array-chunked goes via decompress_array
         * fallback in the dispatcher). */
        uint8_t *raw = NULL;
        size_t raw_bytes = 0;
        uint8_t ce = 0;
        int cr = tensor_chunk_decode(ctx, &raw, &raw_bytes, &ce);
        if (cr != QTSQ_OK || !raw || ce != 0) {
            free(raw);
            return (cr != QTSQ_OK) ? cr : QTSQ_ERR_FORMAT;
        }
        if (raw_bytes % 4 != 0) {
            free(raw);
            return QTSQ_ERR_FORMAT;
        }
        *out_data = (float *)raw;
        *out_count = raw_bytes / 4;
        return QTSQ_OK;
    }
    if (mode > 4) return QTSQ_ERR_FORMAT; /* unknown mode = corrupt */

    uint8_t num_dims = buf[p++];
    if (num_dims > 8) return QTSQ_ERR_FORMAT; /* corrupt dims */
    p += 2; /* reserved */

    uint64_t count64;
    memcpy(&count64, buf + p, 8); p += 8;
    size_t count = (size_t)count64;

    double scale;
    memcpy(&scale, buf + p, 8); p += 8;

    double first_value;
    memcpy(&first_value, buf + p, 8); p += 8;

    /* ── Sanity checks ── */
    if (count == 0) return QTSQ_ERR_FORMAT;
    if (isnan(scale) || isinf(scale)) return QTSQ_ERR_FORMAT;
    if (isnan(first_value) || isinf(first_value)) return QTSQ_ERR_FORMAT;

    /* Read embedded dimensions */
    uint32_t dims[8] = {0};
    if (num_dims > 0) {
        if (p + num_dims * 4 > total) return QTSQ_ERR_FORMAT;
        memcpy(dims, buf + p, num_dims * 4);
    }
    p += num_dims * 4;

    /* Read payload checksum */
    if (p + 32 > total) return QTSQ_ERR_FORMAT;
    uint8_t stored_hash[32];
    memcpy(stored_hash, buf + p, 32); p += 32;

    /* Uncompressed payload size */
    if (p + 8 > total) return QTSQ_ERR_FORMAT;
    uint64_t uncomp_size;
    memcpy(&uncomp_size, buf + p, 8); p += 8;

    /* ── Validate expected sizes ── */
    if (mode == TENSOR_MODE_LOSSLESS) {
        if (uncomp_size != count * sizeof(float)) return QTSQ_ERR_FORMAT;
    } else if (mode == TENSOR_MODE_DELTA) {
        /* Bind uncomp_size for count == 1 too. The other modes pin it
         * unconditionally; here a single-element tensor left it free,
         * and it flowed straight into malloc() -- a 40-byte TNS2 header
         * could request an arbitrary allocation. */
        if (count == 0 || uncomp_size != (count - 1) * sizeof(int32_t))
            return QTSQ_ERR_FORMAT;
    } else if (mode == TENSOR_MODE_F16) {
        if (uncomp_size != count * sizeof(uint16_t)) return QTSQ_ERR_FORMAT;
    } else if (mode == TENSOR_MODE_F8) {
        if (uncomp_size != count) return QTSQ_ERR_FORMAT;
    } else if (mode == TENSOR_MODE_F4) {
        if (uncomp_size != f4_packed_size(count)) return QTSQ_ERR_FORMAT;
    }

    /* Overflow check: count * sizeof(float) must not overflow */
    if (count > (size_t)-1 / sizeof(float)) return QTSQ_ERR_FORMAT;

    /* ── Stored blob → payload ──
     * reserved[0]: 0 = legacy generic-outer stream (pre-2.1 files),
     * 1 = qtc-lens bytes verbatim, 2 = raw payload bytes verbatim.
     * Only modes 0/1 use the new framing; the hash always covers the
     * stored blob for new files (legacy files hash the inner payload). */
    uint8_t r0 = buf[6];
    size_t blob_len = total - p;
    if (blob_len == 0) return QTSQ_ERR_FORMAT;

    uint8_t *payload = NULL;
    if ((mode == TENSOR_MODE_LOSSLESS || mode == TENSOR_MODE_DELTA) &&
        (r0 == 1 || r0 == 2)) {
        uint8_t computed_hash[32];
        qtsq_sha256(buf + p, blob_len, computed_hash);
        if (memcmp(stored_hash, computed_hash, 32) != 0) {
            return QTSQ_ERR_CHECKSUM;
        }
        if (r0 == 2) {
            if (blob_len != (size_t)uncomp_size) return QTSQ_ERR_FORMAT;
            payload = (uint8_t *)malloc(blob_len ? blob_len : 1);
            if (!payload) return QTSQ_ERR_ALLOC;
            memcpy(payload, buf + p, blob_len);
        } else if (mode == TENSOR_MODE_LOSSLESS) {
            float *f = NULL;
            size_t fn = 0;
            if (qtc_tensor_decode(buf + p, blob_len, &f, &fn) != 0 || !f) {
                free(f);
                return QTSQ_ERR_FORMAT;
            }
            if (fn != count || fn * 4 != (size_t)uncomp_size) {
                free(f);
                return QTSQ_ERR_CHECKSUM;
            }
            payload = (uint8_t *)f;
        } else {
            uint8_t *arr = NULL;
            uint32_t ac = 0;
            uint8_t ae = 0;
            if (count <= 1 ||
                qtc_decompress_array(buf + p, blob_len, &arr, &ac, &ae) != 0 ||
                !arr || ae != 4 || ac != (uint32_t)(count - 1) ||
                (size_t)ac * ae != (size_t)uncomp_size) {
                free(arr);
                return QTSQ_ERR_FORMAT;
            }
            payload = arr;
        }
    } else {
        /* ── Legacy outer stream ── */
        size_t zdata_len = blob_len;

        payload = (uint8_t *)malloc((size_t)uncomp_size);
        if (!payload) return QTSQ_ERR_ALLOC;

        if (tensor_payload_decompress(buf + p, zdata_len, payload, (size_t)uncomp_size) != 0) {
            free(payload);
            return QTSQ_ERR_FORMAT;
        }

        /* ── Verify payload checksum ── */
        uint8_t computed_hash[32];
        qtsq_sha256(payload, (size_t)uncomp_size, computed_hash);
        if (memcmp(stored_hash, computed_hash, 32) != 0) {
            free(payload);
            return QTSQ_ERR_CHECKSUM;
        }
    }

    /* ── Reconstruct float array ── */
    float *output = (float *)malloc(count * sizeof(float));
    if (!output) { free(payload); return QTSQ_ERR_ALLOC; }

    if (mode == TENSOR_MODE_LOSSLESS) {
        memcpy(output, payload, count * sizeof(float));
    } else if (mode == TENSOR_MODE_DELTA) {
        /* Delta reconstruction: prefix sum with int32 deltas */
        output[0] = (float)first_value;
        if (count > 1) {
            const int32_t *q = (const int32_t *)payload;
            double running = first_value;
            for (size_t i = 0; i < count - 1; i++) {
                running += (double)q[i] * scale;
                output[i + 1] = (float)running;
            }
        }
    } else if (mode == TENSOR_MODE_F16) {
        /* f16 → f32 reconstruction */
        const uint16_t *h = (const uint16_t *)payload;
        for (size_t i = 0; i < count; i++) {
            output[i] = f16_to_f32(h[i]);
        }
    } else if (mode == TENSOR_MODE_F8) {
        /* f8 E4M3 → f32 reconstruction */
        for (size_t i = 0; i < count; i++) {
            output[i] = f8_e4m3_to_f32(payload[i]);
        }
    } else if (mode == TENSOR_MODE_F4) {
        /* f4 block → f32 reconstruction */
        size_t offset = 0;
        for (size_t i = 0; i < count; i += F4_BLOCK_SIZE) {
            size_t block_n = (count - i < F4_BLOCK_SIZE) ? (count - i) : F4_BLOCK_SIZE;
            f4_block_to_f32(payload + offset, block_n, output + i);
            offset += F4_BLOCK_BYTES;
        }
    }

    free(payload);

    /* Patch schema dimensions back from embedded binary payload */
    if (num_dims > 0) {
        qtsq_context_t *mutable_ctx = (qtsq_context_t *)ctx;
        mutable_ctx->schema.num_dims = num_dims;
        for (int i = 0; i < num_dims && i < 8; i++) {
            mutable_ctx->schema.dimensions[i] = dims[i];
        }
    }

    *out_data = output;
    *out_count = count;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  HIGH-LEVEL API: qtsq_brain_save
 *
 *  Packs weights, biases, memory vectors, and metadata JSON
 *  into a single QTSQ container file.
 *  No size limits — uses uint64_t throughout.
 * ══════════════════════════════════════════════════════════════ */

int qtsq_brain_save(const char *filename,
                     const float *weights, size_t weight_count,
                     const float *biases, size_t bias_count,
                     const float *memory_vectors, size_t num_memories,
                     size_t vector_dim,
                     const char *metadata_json) {
    if (!filename) return QTSQ_ERR_NULL;

    qtsq_context_t container;
    int r = qtsq_container_create(&container);
    if (r != QTSQ_OK) return r;

    /* ── Stream 1: Network Weights ── */
    if (weights && weight_count > 0) {
        qtsq_context_t sub;
        qtsq_init(&sub);
        uint32_t dims[1] = { (uint32_t)weight_count };
        /* For counts > UINT32_MAX, store as 2D: [high, low] */
        uint8_t ndims = 1;
        if (weight_count > 0xFFFFFFFFULL) {
            dims[0] = 0; /* signals "use count field" */
            ndims = 0;
        }
        r = qtsq_compress_tensor(&sub, weights, weight_count, dims, ndims, 0);
        if (r != QTSQ_OK) { qtsq_free(&sub); qtsq_free(&container); return r; }
        r = qtsq_container_add_stream_take(&container, &sub, "network_weights");
        qtsq_free(&sub);
        if (r != QTSQ_OK) { qtsq_free(&container); return r; }
    }

    /* ── Stream 2: Neuron Biases ── */
    if (biases && bias_count > 0) {
        qtsq_context_t sub;
        qtsq_init(&sub);
        uint32_t dims[1] = { (uint32_t)bias_count };
        uint8_t ndims = (bias_count <= 0xFFFFFFFFULL) ? 1 : 0;
        r = qtsq_compress_tensor(&sub, biases, bias_count, dims, ndims, 0);
        if (r != QTSQ_OK) { qtsq_free(&sub); qtsq_free(&container); return r; }
        r = qtsq_container_add_stream_take(&container, &sub, "neuron_biases");
        qtsq_free(&sub);
        if (r != QTSQ_OK) { qtsq_free(&container); return r; }
    }

    /* ── Stream 3: Memory Vectors (flat table) ── */
    if (memory_vectors && num_memories > 0 && vector_dim > 0) {
        qtsq_context_t sub;
        qtsq_init(&sub);
        size_t total_floats = num_memories * vector_dim;
        uint32_t dims[2] = { (uint32_t)num_memories, (uint32_t)vector_dim };
        r = qtsq_compress_tensor(&sub, memory_vectors, total_floats, dims, 2, 0);
        if (r != QTSQ_OK) { qtsq_free(&sub); qtsq_free(&container); return r; }
        r = qtsq_container_add_stream_take(&container, &sub, "memories");
        qtsq_free(&sub);
        if (r != QTSQ_OK) { qtsq_free(&container); return r; }
    }

    /* ── Stream 4: Metadata (JSON) ── */
    if (metadata_json && strlen(metadata_json) > 0) {
        qtsq_context_t sub;
        qtsq_init(&sub);
        r = qtsq_compress_json(&sub, metadata_json, strlen(metadata_json));
        if (r != QTSQ_OK) { qtsq_free(&sub); qtsq_free(&container); return r; }
        r = qtsq_container_add_stream_take(&container, &sub, "metadata");
        qtsq_free(&sub);
        if (r != QTSQ_OK) { qtsq_free(&container); return r; }
    }

    /* ── Pack and write ── */
    r = qtsq_container_pack(&container);
    if (r != QTSQ_OK) { qtsq_free(&container); return r; }

    /* Phase 1: fail-closed — encrypt before writing. qtsq_container_pack
     * now pre-generates the security salt and encrypts stream names (Phase 5),
     * which is the names-in-envelope guarantee for containers. Mark sanitized
     * so qtsq_write's fail-closed gate accepts the context; the vault will
     * preserve the salt pack generated. */
    container.is_sanitized = 1;
    container.sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ENCRYPTED;
    if (qtsq_keystore_is_initialized()) {
        r = qtsq_keystore_encrypt(&container);
    } else {
        r = QTSQ_ERR_NOT_ENCRYPTED;
    }
    if (r != QTSQ_OK) { qtsq_free(&container); return r; }

    r = qtsq_write(&container, filename);
    qtsq_free(&container);
    return r;
}

/* ══════════════════════════════════════════════════════════════
 *  HIGH-LEVEL API: qtsq_brain_load
 * ══════════════════════════════════════════════════════════════ */

int qtsq_brain_load(const char *filename,
                     float **out_weights, size_t *out_weight_count,
                     float **out_biases, size_t *out_bias_count,
                     float **out_memory_vectors, size_t *out_num_memories,
                     size_t *out_vector_dim,
                     char **out_metadata_json) {
    if (!filename) return QTSQ_ERR_NULL;

    /* Initialize all outputs to NULL/0 */
    if (out_weights) *out_weights = NULL;
    if (out_weight_count) *out_weight_count = 0;
    if (out_biases) *out_biases = NULL;
    if (out_bias_count) *out_bias_count = 0;
    if (out_memory_vectors) *out_memory_vectors = NULL;
    if (out_num_memories) *out_num_memories = 0;
    if (out_vector_dim) *out_vector_dim = 0;
    if (out_metadata_json) *out_metadata_json = NULL;

    qtsq_context_t container;
    qtsq_init(&container);
    int r = qtsq_read(&container, filename);
    if (r != QTSQ_OK) { qtsq_free(&container); return r; }

    if (container.header.data_type != QTSQ_TYPE_CONTAINER) {
        qtsq_free(&container);
        return QTSQ_ERR_TYPE;
    }

    uint32_t num_streams = 0;
    r = qtsq_container_get_count(&container, &num_streams);
    if (r != QTSQ_OK) { qtsq_free(&container); return r; }

    for (uint32_t i = 0; i < num_streams; i++) {
        qtsq_context_t sub;
        char name[256];
        r = qtsq_container_get_stream(&container, i, &sub, name, sizeof(name));
        if (r != QTSQ_OK) continue;

        if (strcmp(name, "network_weights") == 0 && out_weights) {
            float *data; size_t count;
            if (qtsq_decompress_tensor(&sub, &data, &count) == QTSQ_OK) {
                *out_weights = data;
                if (out_weight_count) *out_weight_count = count;
            }
        }
        else if (strcmp(name, "neuron_biases") == 0 && out_biases) {
            float *data; size_t count;
            if (qtsq_decompress_tensor(&sub, &data, &count) == QTSQ_OK) {
                *out_biases = data;
                if (out_bias_count) *out_bias_count = count;
            }
        }
        else if (strcmp(name, "memories") == 0 && out_memory_vectors) {
            float *data; size_t count;
            if (qtsq_decompress_tensor(&sub, &data, &count) == QTSQ_OK) {
                *out_memory_vectors = data;
                if (sub.schema.num_dims >= 2) {
                    if (out_num_memories) *out_num_memories = sub.schema.dimensions[0];
                    if (out_vector_dim) *out_vector_dim = sub.schema.dimensions[1];
                } else {
                    if (out_num_memories) *out_num_memories = count;
                    if (out_vector_dim) *out_vector_dim = 1;
                }
            }
        }
        else if (strcmp(name, "metadata") == 0 && out_metadata_json) {
            uint8_t *raw; size_t raw_size;
            if (qtsq_decompress_horizon(&sub, &raw, &raw_size) == QTSQ_OK) {
                char *json = (char *)malloc(raw_size + 1);
                if (json) {
                    memcpy(json, raw, raw_size);
                    json[raw_size] = '\0';
                    *out_metadata_json = json;
                }
                free(raw);
            }
        }

        qtsq_free(&sub);
    }

    qtsq_free(&container);
    return QTSQ_OK;
}
