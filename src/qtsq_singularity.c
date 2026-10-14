/*
 * Quantum Tensor Sequence — Singularity Engine
 * Compression: xoshiro256** PRNG seed search + XOR delta + RLE
 * Best for: Raw binary, generic data
 * Author: Haruhito
 */

#include <string.h>
#include <stdlib.h>
#include "qtsq.h"
#include "qtsq_codec.h"

/* ── xoshiro256** PRNG (public domain, Blackman & Vigna 2018) ── */

typedef struct {
    uint64_t s[4];
} xoshiro256_state_t;

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t xoshiro256_next(xoshiro256_state_t *state) {
    const uint64_t result = rotl64(state->s[1] * 5, 7) * 9;
    const uint64_t t = state->s[1] << 17;

    state->s[2] ^= state->s[0];
    state->s[3] ^= state->s[1];
    state->s[1] ^= state->s[2];
    state->s[0] ^= state->s[3];
    state->s[2] ^= t;
    state->s[3] = rotl64(state->s[3], 45);

    return result;
}

static void xoshiro256_seed(xoshiro256_state_t *state, uint64_t seed) {
    /* SplitMix64 to expand single seed into full state */
    for (int i = 0; i < 4; i++) {
        seed += 0x9E3779B97F4A7C15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        state->s[i] = z;
    }
}

/* ── RLE (Run-Length Encoding) ── */

/*
 * Format: for each run:
 *   [count_byte] [value_byte]
 * count_byte: 1-255 repetitions of value_byte
 * A count of 0 is never emitted.
 */
static size_t rle_encode(const uint8_t *in, size_t in_size,
                         uint8_t *out, size_t out_capacity) {
    size_t out_pos = 0;
    size_t i = 0;

    while (i < in_size) {
        uint8_t val = in[i];
        uint8_t count = 1;

        while (i + count < in_size && in[i + count] == val && count < 255) {
            count++;
        }

        if (out_pos + 2 > out_capacity) return 0; /* overflow */
        out[out_pos++] = count;
        out[out_pos++] = val;
        i += count;
    }
    return out_pos;
}

static size_t rle_decode(const uint8_t *in, size_t in_size,
                         uint8_t *out, size_t out_capacity) {
    size_t out_pos = 0;
    size_t i = 0;

    while (i + 1 < in_size) {
        uint8_t count = in[i];
        uint8_t val   = in[i + 1];

        if (out_pos + count > out_capacity) return 0; /* overflow */
        memset(out + out_pos, val, count);
        out_pos += count;
        i += 2;
    }
    return out_pos;
}

/* ── XOR Delta Generation ── */

static void xor_delta(const uint8_t *data, size_t size,
                      uint64_t seed, uint8_t *delta) {
    xoshiro256_state_t prng;
    xoshiro256_seed(&prng, seed);

    size_t i = 0;
    /* Process 8 bytes at a time */
    for (; i + 8 <= size; i += 8) {
        uint64_t r = xoshiro256_next(&prng);
        uint64_t d;
        memcpy(&d, data + i, 8);
        d ^= r;
        memcpy(delta + i, &d, 8);
    }
    /* Remaining bytes */
    if (i < size) {
        uint64_t r = xoshiro256_next(&prng);
        uint8_t *rb = (uint8_t *)&r;
        for (size_t j = 0; i + j < size; j++) {
            delta[i + j] = data[i + j] ^ rb[j];
        }
    }
}

/* ── Count zero bytes (fast heuristic for seed quality) ── */

static size_t count_zeros(const uint8_t *delta, size_t size) {
    size_t zeros = 0;
    for (size_t i = 0; i < size; i++) {
        if (delta[i] == 0) zeros++;
    }
    return zeros;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_compress_raw
 * ══════════════════════════════════════════════════════════════ */

int qtsq_compress_raw(qtsq_context_t *ctx,
                      const uint8_t *data, size_t size, uint64_t user_seed) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (size == 0) return QTSQ_ERR_TOO_SMALL;

    /* Phase 6a (opt-in): qtc generic lens. Skips the XOR-delta seed search —
     * qtc's LZ + rANS core subsumes it. Falls back to STORE if qtc cannot
     * shrink the input, matching the existing fallback contract. */
    if (ctx->codec == QTSQ_CODEC_QTC) {
        uint8_t *qtc_stream = NULL;
        size_t qtc_len = 0;
        int qrc = qtsq_codec_compress(data, size, &qtc_stream, &qtc_len);
        int shrunk = (qrc == QTSQ_OK) && qtc_stream && (qtc_len < size);

        uint8_t *out_buf;
        size_t out_len;
        if (shrunk) {
            out_buf = qtc_stream;
            out_len = qtc_len;
            ctx->header.strategy = QTSQ_STRATEGY_SINGULARITY;
        } else {
            free(qtc_stream);
            out_buf = (uint8_t *)malloc(size);
            if (!out_buf) return QTSQ_ERR_ALLOC;
            memcpy(out_buf, data, size);
            out_len = size;
            ctx->header.strategy = QTSQ_STRATEGY_NONE;
        }

        if (ctx->singularity) free(ctx->singularity);
        ctx->singularity = out_buf;
        ctx->singularity_size = out_len;
        ctx->header.data_type = QTSQ_TYPE_RAW;
        ctx->header.original_size = size;
        ctx->header.singularity_size = out_len;
        ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;
        qtsq_sha256(data, size, ctx->header.checksum);
        return QTSQ_OK;
    }

    uint8_t *delta = (uint8_t *)malloc(size);
    if (!delta) return QTSQ_ERR_ALLOC;

    uint8_t *best_delta = (uint8_t *)malloc(size);
    if (!best_delta) { free(delta); return QTSQ_ERR_ALLOC; }

    /*
     * Seed search: try candidate seeds and pick the one that
     * produces the most compressible XOR delta (most zeros).
     */
    uint64_t best_seed = user_seed;
    size_t   best_zeros = 0;

    /* If user provided a specific seed, just use it */
    if (user_seed != 0) {
        xor_delta(data, size, user_seed, best_delta);
        best_zeros = count_zeros(best_delta, size);
        best_seed = user_seed;
    } else {
        /* Brute-force search: try 256 candidate seeds */
        uint64_t candidates[] = {
            0, 1, 42, 137, 255, 256, 1024, 4096,
            0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0xFEDCBA98,
            0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL,
            0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL
        };
        int num_fixed = 16;

        /* Try fixed candidates */
        for (int i = 0; i < num_fixed; i++) {
            xor_delta(data, size, candidates[i], delta);
            size_t z = count_zeros(delta, size);
            if (z > best_zeros) {
                best_zeros = z;
                best_seed = candidates[i];
                memcpy(best_delta, delta, size);
            }
        }

        /* Try seeds derived from the data itself */
        for (int i = 0; i < 64 && (size_t)i * 8 + 8 <= size; i++) {
            uint64_t data_seed;
            memcpy(&data_seed, data + i * 8, 8);
            xor_delta(data, size, data_seed, delta);
            size_t z = count_zeros(delta, size);
            if (z > best_zeros) {
                best_zeros = z;
                best_seed = data_seed;
                memcpy(best_delta, delta, size);
            }
        }

        /* Try random-ish seeds based on data hash */
        xoshiro256_state_t searcher;
        xoshiro256_seed(&searcher, size ^ 0xB1AC4B01EULL);
        for (int i = 0; i < 176; i++) {
            uint64_t trial_seed = xoshiro256_next(&searcher);
            xor_delta(data, size, trial_seed, delta);
            size_t z = count_zeros(delta, size);
            if (z > best_zeros) {
                best_zeros = z;
                best_seed = trial_seed;
                memcpy(best_delta, delta, size);
            }
        }
    }

    /* If we didn't find a great seed, just generate the delta for best_seed */
    if (best_zeros == 0) {
        xor_delta(data, size, best_seed, best_delta);
    }

    /*
     * RLE-encode the delta.
     * Worst case: RLE doubles the size (no runs). 
     * Allocate 2x + 16 for the header.
     */
    size_t rle_capacity = size * 2 + 16;
    uint8_t *rle_buf = (uint8_t *)malloc(rle_capacity);
    if (!rle_buf) {
        free(delta);
        free(best_delta);
        return QTSQ_ERR_ALLOC;
    }

    /* Pack: [seed 8B] [original_size 8B] [RLE data...] */
    memcpy(rle_buf, &best_seed, 8);
    memcpy(rle_buf + 8, &size, 8);

    size_t rle_size = rle_encode(best_delta, size,
                                 rle_buf + 16, rle_capacity - 16);

    free(delta);
    free(best_delta);

    uint32_t total_compressed;

    if (rle_size == 0 || (16 + rle_size) >= size) {
        /*
         * RLE failed or expanded the data.
         * Fall back to STORE mode: save raw bytes as-is.
         * This guarantees output ≤ input size.
         */
        free(rle_buf);

        uint8_t *store_buf = (uint8_t *)malloc(size);
        if (!store_buf) return QTSQ_ERR_ALLOC;
        memcpy(store_buf, data, size);

        if (ctx->singularity) free(ctx->singularity);
        ctx->singularity = store_buf;
        ctx->singularity_size = size;
        total_compressed = size;

        ctx->header.strategy = QTSQ_STRATEGY_NONE;
    } else {
        /* Delta+RLE compressed successfully (smaller than original) */
        if (ctx->singularity) free(ctx->singularity);
        ctx->singularity = rle_buf;
        ctx->singularity_size = 16 + rle_size;
        total_compressed = ctx->singularity_size;

        ctx->header.strategy = QTSQ_STRATEGY_DELTA_SEED;
    }

    /* Set header fields */
    ctx->header.data_type = QTSQ_TYPE_RAW;
    ctx->header.original_size = size;
    ctx->header.singularity_size = total_compressed;
    ctx->header.flags |= QTSQ_FLAG_CHECKSUMMED;

    /* Compute SHA-256 checksum */
    qtsq_sha256(data, size, ctx->header.checksum);

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decompress_singularity
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decompress_singularity(const qtsq_context_t *ctx,
                                uint8_t **out_data, size_t *out_size) {
    if (!ctx || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* STRATEGY_NONE: data is stored as-is (no compression) */
    if (ctx->header.strategy == QTSQ_STRATEGY_NONE) {
        size_t sz = ctx->header.original_size;
        if (sz == 0) sz = ctx->singularity_size;
        uint8_t *output = (uint8_t *)malloc(sz);
        if (!output) return QTSQ_ERR_ALLOC;
        memcpy(output, ctx->singularity, sz);
        *out_data = output;
        *out_size = sz;
        return QTSQ_OK;
    }

    /* QTSQ_STRATEGY_SINGULARITY (Phase 6a): qtc generic stream. */
    if (ctx->header.strategy == QTSQ_STRATEGY_SINGULARITY) {
        const uint8_t *p = ctx->singularity;
        size_t n = ctx->singularity_size;
        if (!qtsq_codec_is_qtc(p, n)) return QTSQ_ERR_FORMAT;
        uint64_t declared = qtsq_codec_declared_size(p, n);
        if (declared == 0) return QTSQ_ERR_FORMAT;
        if (declared != ctx->header.original_size) return QTSQ_ERR_FORMAT;
        /* DoS bound: generous ratio cap (highly compressible data like
         * all-zero buffers is legitimate) before the bounded decode. */
        if (declared > (uint64_t)n * 4096ULL + (1ULL << 20)) return QTSQ_ERR_FORMAT;
        return qtsq_codec_decompress(p, n, declared, out_data, out_size);
    }

    /* DELTA_SEED strategy needs at least 16 bytes header */
    if (ctx->singularity_size < 16) return QTSQ_ERR_FORMAT;

    /* Unpack seed and original size */
    uint64_t seed;
    uint64_t orig_size;
    memcpy(&seed, ctx->singularity, 8);
    memcpy(&orig_size, ctx->singularity + 8, 8);

    /* Validate orig_size against the singularity — a crafted file can set
     * orig_size to a huge value (up to 2^64), driving a giant malloc and a
     * raw-copy that reads past the buffer. The decoded/output size can
     * never exceed the RLE source bytes by more than the RLE expansion
     * factor (worst case ~2× for a 0x00 run), so bound against the source. */
    if (orig_size == 0 || orig_size > (uint64_t)ctx->singularity_size * 4 + (1 << 20))
        return QTSQ_ERR_FORMAT;

    /* Allocate output */
    uint8_t *output = (uint8_t *)malloc((size_t)orig_size);
    if (!output) return QTSQ_ERR_ALLOC;

    /* RLE-decode the delta */
    uint8_t *delta = (uint8_t *)malloc((size_t)orig_size);
    if (!delta) { free(output); return QTSQ_ERR_ALLOC; }

    size_t rle_data_size = ctx->singularity_size - 16;
    size_t decoded = rle_decode(ctx->singularity + 16, rle_data_size,
                                delta, (size_t)orig_size);

    if (decoded == 0 && rle_data_size == orig_size) {
        /* RLE decode returned 0 — data was stored raw (no RLE).
         * Bound the memcpy against the actual source bytes. */
        memcpy(delta, ctx->singularity + 16, (size_t)orig_size);
    } else if (decoded != (size_t)orig_size) {
        free(delta);
        free(output);
        return QTSQ_ERR_FORMAT;
    }

    /* XOR with PRNG stream to reconstruct original */
    xor_delta(delta, (size_t)orig_size, seed, output);

    free(delta);
    *out_data = output;
    *out_size = (size_t)orig_size;

    return QTSQ_OK;
}
