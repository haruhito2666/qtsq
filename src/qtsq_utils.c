/*
 * Quantum Tensor Sequence — Utilities
 * SHA-256 (NIST FIPS 180-4), CRC-32 (IEEE 802.3), entropy.
 * Author: Haruhito
 */

#include "qtsq.h"
#include "qtsq_sanitize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__APPLE__)
/* Phase 8: hardware-accelerated SHA-256 (Apple Silicon SHA2 extensions).
 * CommonCrypto is in the dyld shared cache, so no extra link flag is needed. */
#include <CommonCrypto/CommonDigest.h>
#endif

/* SHA-256 */
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2)  ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6)  ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7)  ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* ── Streaming SHA-256 (FIPS 180-4) ────────────────────────────────────
 * Phase 8: the old one-shot qtsq_sha256 calloc'd a padded copy of the whole
 * input — a full extra memcpy plus a large allocation on every hash, on both
 * the compress and decompress paths. The streaming form hashes in place with
 * a single 64-byte block buffer. The compression function is the identical
 * algorithm, so digests are byte-for-byte unchanged (gated against the known
 * FIPS vectors in tests/test_qtc_integration.c). */
typedef struct {
    uint32_t h[8];
    uint8_t  block[64];
    uint64_t bitlen;    /* bits in full blocks already compressed        */
    uint32_t blocklen;  /* bytes currently buffered in block[]           */
} qtsq_sha256_ctx;

static void sha256_transform(qtsq_sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e,f,g) + sha256_k[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(qtsq_sha256_ctx *ctx) {
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->blocklen = 0;
}

static void sha256_update(qtsq_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    while (len > 0) {
        uint32_t space = 64 - ctx->blocklen;
        uint32_t take = (len < space) ? (uint32_t)len : space;
        memcpy(ctx->block + ctx->blocklen, data, take);
        ctx->blocklen += take;
        data += take;
        len -= take;
        if (ctx->blocklen == 64) {
            sha256_transform(ctx, ctx->block);
            ctx->bitlen += 512;
            ctx->blocklen = 0;
        }
    }
}

static void sha256_final(qtsq_sha256_ctx *ctx, uint8_t out[32]) {
    uint64_t bitlen = ctx->bitlen + (uint64_t)ctx->blocklen * 8;

    ctx->block[ctx->blocklen++] = 0x80;
    if (ctx->blocklen > 56) {
        while (ctx->blocklen < 64) ctx->block[ctx->blocklen++] = 0;
        sha256_transform(ctx, ctx->block);
        ctx->blocklen = 0;
    }
    while (ctx->blocklen < 56) ctx->block[ctx->blocklen++] = 0;
    for (int i = 7; i >= 0; i--)
        ctx->block[ctx->blocklen++] = (uint8_t)(bitlen >> (i * 8));
    sha256_transform(ctx, ctx->block);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (ctx->h[i] >> 24) & 0xFF;
        out[i*4+1] = (ctx->h[i] >> 16) & 0xFF;
        out[i*4+2] = (ctx->h[i] >> 8) & 0xFF;
        out[i*4+3] = ctx->h[i] & 0xFF;
    }
}

/* Portable one-shot SHA-256 (streaming under the hood, no input copy).
 * Exposed so the test gate can verify it directly on every platform —
 * including Apple, where qtsq_sha256 dispatches to CommonCrypto. */
void qtsq_sha256_portable(const uint8_t *data, size_t len, uint8_t out[32]) {
    qtsq_sha256_ctx ctx;
    sha256_init(&ctx);
    if (data && len > 0) sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

int qtsq_sha256_init(qtsq_sha256_state_t *s) {
    _Static_assert(sizeof(qtsq_sha256_state_t) == sizeof(qtsq_sha256_ctx),
                   "public sha256 state must match internal layout");
    if (!s) return QTSQ_ERR_NULL;
#if defined(__APPLE__)
    /* Hardware path (SHA2 extensions): the opaque CC context fits in
     * the public state; digests match the portable path bit-for-bit. */
    _Static_assert(sizeof(CC_SHA256_CTX) <= sizeof(qtsq_sha256_state_t),
                   "CC context must fit");
    CC_SHA256_Init((CC_SHA256_CTX *)s);
    return QTSQ_OK;
#else
    sha256_init((qtsq_sha256_ctx *)s);
    return QTSQ_OK;
#endif
}

int qtsq_sha256_update(qtsq_sha256_state_t *s, const uint8_t *data, size_t len) {
    if (!s) return QTSQ_ERR_NULL;
    if (!data && len > 0) return QTSQ_ERR_NULL;
#if defined(__APPLE__)
    if (len > (size_t)0x7FFFFFFF) return QTSQ_ERR_TOO_LARGE;
    CC_SHA256_Update((CC_SHA256_CTX *)s, data ? data : (const void *)"", (CC_LONG)len);
    return QTSQ_OK;
#else
    sha256_update((qtsq_sha256_ctx *)s, data, len);
    return QTSQ_OK;
#endif
}

int qtsq_sha256_final(qtsq_sha256_state_t *s, uint8_t out[32]) {
    if (!s || !out) return QTSQ_ERR_NULL;
#if defined(__APPLE__)
    CC_SHA256_Final(out, (CC_SHA256_CTX *)s);
    return QTSQ_OK;
#else
    sha256_final((qtsq_sha256_ctx *)s, out);
    return QTSQ_OK;
#endif
}

void qtsq_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    if (!out) return;
    if (!data && len > 0) return;
#if defined(__APPLE__)
    /* Hardware-accelerated digest (Apple Silicon SHA2 extensions). Standard
     * FIPS 180-4 output — identical to the portable path, gated on the known
     * vectors. Fall back to portable for inputs beyond CC_LONG's 32-bit len. */
    if (len <= (size_t)0x7FFFFFFF) {
        CC_SHA256(data, (CC_LONG)len, out);
        return;
    }
#endif
    qtsq_sha256_portable(data, len, out);
}

/* CRC-32 */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = 0xEDB88320 ^ (c >> 1);
            else c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc32_initialized = 1;
}

uint32_t qtsq_crc32(const uint8_t *data, size_t len) {
    if (!crc32_initialized) crc32_init_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

/* Entropy */
double qtsq_entropy(const uint8_t *data, size_t len) {
    if (len == 0) return 0.0;
    uint64_t freq[256] = {0};
    for (size_t i = 0; i < len; i++) freq[data[i]]++;

    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] == 0) continue;
        double p = (double)freq[i] / (double)len;
        entropy -= p * log2(p);
    }
    return entropy;
}

/* Lifecycle */
int qtsq_init(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    memset(ctx, 0, sizeof(qtsq_context_t));

    /* Phase 7: qtc is the default codec. zlib remains available as an
     * explicit opt-out (set ctx->codec = QTSQ_CODEC_ZLIB after init) and
     * as the universal read path — decoders sniff the stream magic, so
     * files written by older (zlib) versions still decompress. */
    ctx->codec = QTSQ_CODEC_QTC;

    ctx->header.magic[0] = QTSQ_MAGIC_0;
    ctx->header.magic[1] = QTSQ_MAGIC_1;
    ctx->header.magic[2] = QTSQ_MAGIC_2;
    ctx->header.magic[3] = QTSQ_MAGIC_3;
    ctx->header.version  = QTSQ_VERSION;
    /* Phase 1: a fresh context is NOT sanitized. Only the qtsq_secure_* path,
     * which performs the real LSB scrub / zero-meta / names-in-envelope work,
     * sets is_sanitized=1. qtsq_write rejects writes until this is true, so a
     * raw qtsq_compress_* -> qtsq_write path can no longer slip past the
     * "unconditional on every file" privacy gate with a defaulted flag. */
    ctx->is_sanitized    = 0;
    ctx->sanitize_report = 0;
    ctx->intent          = QTSQ_INTENT_NONE; /* No intent set (legacy compat) */

    return QTSQ_OK;
}

int qtsq_create_creative(qtsq_context_t *ctx) {
    int result = qtsq_init(ctx);
    if (result != QTSQ_OK) return result;
    ctx->intent = QTSQ_INTENT_CREATIVE;
    ctx->header.intent = QTSQ_INTENT_CREATIVE;
    ctx->header.flags |= QTSQ_FLAG_CREATIVE;
    return QTSQ_OK;
}

int qtsq_create_provenance(qtsq_context_t *ctx) {
    int result = qtsq_init(ctx);
    if (result != QTSQ_OK) return result;
    ctx->intent = QTSQ_INTENT_PROVENANCE;
    ctx->header.intent = QTSQ_INTENT_PROVENANCE;
    ctx->header.flags |= QTSQ_FLAG_PROVENANCE;
    return QTSQ_OK;
}

qtsq_intent_t qtsq_intent(const qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_INTENT_NONE;
    return ctx->intent;
}

int qtsq_is_resynthesis_allowed(const qtsq_context_t *ctx) {
    if (!ctx) return 0;
    return (ctx->intent == QTSQ_INTENT_CREATIVE || ctx->intent == QTSQ_INTENT_NONE);
}

int qtsq_require_creative(qtsq_context_t *ctx, const char *mode_name) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        fprintf(stderr, "QTSQ ERROR: '%s' mode is not allowed in Provenance intent. "
                "Use qtsq_create_creative() for re-synthesis modes.\n", mode_name);
        return QTSQ_ERR_TYPE;
    }
    return QTSQ_OK;
}

int qtsq_set_original_input(qtsq_context_t *ctx,
                            const uint8_t *data, size_t size) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (size == 0) return QTSQ_OK;

    free(ctx->original_input);
    ctx->original_input = (uint8_t *)malloc(size);
    if (!ctx->original_input) return QTSQ_ERR_ALLOC;
    memcpy(ctx->original_input, data, size);
    ctx->original_input_size = size;
    return QTSQ_OK;
}

void qtsq_free(qtsq_context_t *ctx) {
    if (!ctx) return;
    /* M-14: an unpacked container holds a builder behind the tagged
     * pointer in ctx->singularity; free it properly before the generic
     * frees below, or the builder and every slot leak. */
    qtsq_container_abandon(ctx);
    /* Securely zero sensitive buffers before freeing */
    if (ctx->singularity && ctx->singularity_size > 0)
        qtsq_secure_zero(ctx->singularity, (size_t)ctx->singularity_size);
    if (ctx->original_input && ctx->original_input_size > 0)
        qtsq_secure_zero(ctx->original_input, ctx->original_input_size);
    qtsq_secure_zero(ctx->geocrypt_key, sizeof(ctx->geocrypt_key));
    qtsq_secure_zero(&ctx->security, sizeof(ctx->security));
    if (ctx->transforms) {
        for (uint32_t i = 0; i < ctx->num_transforms; i++)
            free(ctx->transforms[i].params);
        free(ctx->transforms);
    }
    free(ctx->singularity);
    free(ctx->original_input);
    free(ctx->radiation_index);
    free(ctx->wormholes);
    free(ctx->recovery.chunk_checksums);
    free(ctx->recovery.reed_solomon_parity);
    memset(ctx, 0, sizeof(qtsq_context_t));
}

/* Total working-set budget shared by the video encoder and decoder.
 * Read once per process; the value cannot change mid-encode, which keeps the
 * accounting in the callers simple and race-free. */
size_t qtsq_memory_limit_bytes(void) {
    static size_t lim = 0;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        long mb = 5120;               /* 5 GB default */
        const char *e = getenv("QTSQ_MEMORY_LIMIT_MB");
        if (e && *e) {
            mb = strtol(e, NULL, 10);
            if (mb < 64) mb = 64;
            if (mb > 10240) mb = 10240;
        }
        lim = (size_t)mb * 1024u * 1024u;
    }
    return lim;
}

/* Error Strings */
const char* qtsq_error_string(int code) {
    switch (code) {
        case QTSQ_OK:              return "OK";
        case QTSQ_ERR_NULL:        return "Null pointer";
        case QTSQ_ERR_ALLOC:       return "Memory allocation failed";
        case QTSQ_ERR_IO:          return "File I/O error";
        case QTSQ_ERR_FORMAT:      return "Invalid .qtsq format";
        case QTSQ_ERR_CHECKSUM:    return "Checksum mismatch";
        case QTSQ_ERR_STRATEGY:    return "Unknown strategy";
        case QTSQ_ERR_TYPE:        return "Unknown data type";
        case QTSQ_ERR_ENCRYPTED:   return "Data encrypted, key required";
        case QTSQ_ERR_DECRYPT:     return "Decryption failed";
        case QTSQ_ERR_WORMHOLE:    return "Wormhole target not found";
        case QTSQ_ERR_WORMHOLE_HASH: return "Wormhole hash mismatch";
        case QTSQ_ERR_CORRUPT:     return "Data corruption detected";
        case QTSQ_ERR_RECOVERY_FAIL: return "Recovery failed";
        case QTSQ_ERR_TOO_SMALL:   return "Below Chandrasekhar limit";
        case QTSQ_ERR_NOT_ENCRYPTED: return "Refused: context not encrypted (no plaintext .qtsq)";
        case QTSQ_ERR_NOT_SANITIZED: return "Refused: context not sanitized (use a qtsq_secure_* API)";
        case QTSQ_ERR_TOO_LARGE:   return "Bitstream exceeds QTSQ_BITSTREAM_MB cap";
        default:                  return "Unknown error";
    }
}

/* Inspection */
double qtsq_compression_ratio(const qtsq_context_t *ctx) {
    if (!ctx || ctx->header.singularity_size == 0) return 0.0;
    return (double)ctx->header.original_size / (double)ctx->header.singularity_size;
}

void qtsq_print_info(const qtsq_context_t *ctx) {
    if (!ctx) return;
    printf("=== Quantum Tensor Sequence (.qtsq) ===\n");
    printf("  Version:     %u\n", ctx->header.version);
    printf("  Intent:      %s\n",
           ctx->intent == QTSQ_INTENT_CREATIVE ? "CREATIVE" :
           ctx->intent == QTSQ_INTENT_PROVENANCE ? "PROVENANCE" : "NONE");
    printf("  Data Type:   %s (0x%02X)\n",
           qtsq_type_name(ctx->header.data_type), ctx->header.data_type);
    printf("  Strategy:    %s (0x%02X)\n",
           qtsq_strategy_name(ctx->header.strategy), ctx->header.strategy);
    printf("  Channels:    %u\n", ctx->header.num_channels);
    printf("  Bit Depth:   %u\n", ctx->header.sample_depth);
    printf("  Original:    %llu bytes\n",
           (unsigned long long)ctx->header.original_size);
    printf("  Singularity: %llu bytes\n", (unsigned long long)ctx->header.singularity_size);
    printf("  Ratio:       %.1f:1\n", qtsq_compression_ratio(ctx));
    printf("  Flags:       0x%08X", ctx->header.flags);
    if (ctx->header.flags & QTSQ_FLAG_ENCRYPTED)     printf(" [ENCRYPTED]");
    if (ctx->header.flags & QTSQ_FLAG_HAS_WORMHOLES) printf(" [WORMHOLES]");
    if (ctx->header.flags & QTSQ_FLAG_HAS_ECC)       printf(" [ECC]");
    if (ctx->header.flags & QTSQ_FLAG_LOSSY)         printf(" [LOSSY]");
    if (ctx->header.flags & QTSQ_FLAG_REDSHIFTABLE)  printf(" [REDSHIFT]");
    if (ctx->header.flags & QTSQ_FLAG_PQ_ENCRYPTED)  printf(" [PQ_ENCRYPTED]");
    if (ctx->header.flags & QTSQ_FLAG_PQ_SIGNED)     printf(" [PQ_SIGNED]");
    if (ctx->header.flags & QTSQ_FLAG_ZERO_META)     printf(" [ZERO_META]");
    if (ctx->header.flags & QTSQ_FLAG_ANTI_STEGO)    printf(" [ANTI_STEGO]");
    if (ctx->header.flags & QTSQ_FLAG_LOCAL_ONLY)    printf(" [LOCAL_ONLY]");
    if (ctx->header.flags & QTSQ_FLAG_CREATIVE)      printf(" [CREATIVE]");
    if (ctx->header.flags & QTSQ_FLAG_PROVENANCE)    printf(" [PROVENANCE]");
    if (ctx->header.flags & QTSQ_FLAG_DO_NOT_TRAIN)  printf(" [DO_NOT_TRAIN]");
    if (ctx->header.flags & QTSQ_FLAG_NO_AI)         printf(" [NO_AI]");
    printf("\n");
    printf("  Wormholes:   %u\n", ctx->num_wormholes);
    if (ctx->is_sanitized) {
        printf("  Sanitized:   YES (0x%02X)", ctx->sanitize_report);
        if (ctx->sanitize_report & QTSQ_SANITIZE_IMAGE_LSB)   printf(" [IMG_LSB]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_AUDIO_LSB)   printf(" [AUD_LSB]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_TEXT_NORM)   printf(" [TEXT_NORM]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_JSON_MINIFY) printf(" [JSON_MIN]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_NAMES_HIDE)  printf(" [NAMES_HIDE]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_ENCRYPTED)   printf(" [ENCRYPTED]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_ZERO_META)   printf(" [ZERO_META]");
        if (ctx->sanitize_report & QTSQ_SANITIZE_SIZE_PADDED) printf(" [SIZE_PADDED]");
        printf("\n");
    } else {
        printf("  Sanitized:   NO\n");
    }
    printf("==============================\n");
}

/* ══════════════════════════════════════════════════════════════
 *  Size Padding — defeat traffic analysis by padding to buckets
 *
 *  Pads the singularity to the next fixed size bucket with random
 *  bytes. An adversary can only determine which bucket the file
 *  falls in, not the exact size.
 *
 *  Buckets: 1K, 4K, 16K, 64K, 256K, 1M, 4M, 16M, 64M, 256M
 * ══════════════════════════════════════════════════════════════ */

#include "../crypto/common/randombytes.h"
#include "qtsq_rng.h"

/* Magic marking the size-padding trailer. Sits inside the encryption
 * envelope so it is only visible after decrypt; it lets qtsq_unpad_size
 * verify the marker after a file round-trip, where the in-memory
 * SIZE_PADDED report flag is not restored by qtsq_read. */
#define QTSQ_PAD_MAGIC "QTSQPAD1"

int qtsq_pad_size(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Already padded */
    if (ctx->sanitize_report & QTSQ_SANITIZE_SIZE_PADDED) return QTSQ_OK;

    /* Size buckets (bytes) */
    static const uint64_t buckets[] = {
        1024, 4096, 16384, 65536, 262144, 1048576, 4194304,
        16777216, 67108864, 268435456
    };
    const int num_buckets = sizeof(buckets) / sizeof(buckets[0]);

    uint64_t current = ctx->singularity_size;

    /* Find the next bucket that's larger than current size */
    uint64_t target = 0;
    for (int i = 0; i < num_buckets; i++) {
        if (buckets[i] > current) {
            target = buckets[i];
            break;
        }
    }

    /* If already larger than the biggest bucket, don't pad */
    if (target == 0) return QTSQ_OK;

    /* Need at least 17 bytes of padding: 16 for the trailer
     * (8B magic + 8B original size) + 1 minimum random pad. */
    size_t pad_bytes = (size_t)(target - current);
    if (pad_bytes < 17) {
        /* Move to the next bucket to ensure enough padding */
        for (int i = 0; i < num_buckets; i++) {
            if (buckets[i] > current + 17) {
                target = buckets[i];
                break;
            }
        }
        pad_bytes = (size_t)(target - current);
        if (pad_bytes < 17) return QTSQ_OK; /* still too small, skip padding */
    }

    /* Reallocate and fill with random bytes */
    uint8_t *new_buf = (uint8_t *)realloc(ctx->singularity, (size_t)target);
    if (!new_buf) return QTSQ_ERR_ALLOC;

    if (qtsq_random_bytes(new_buf + current, pad_bytes) != 0) {
        /* realloc already replaced the old block; keep that consistent. */
        ctx->singularity = new_buf;
        return QTSQ_ERR_RANDOM;
    }

    ctx->singularity = new_buf;
    ctx->singularity_size = (size_t)target;
    ctx->header.singularity_size = (size_t)target;

    /* Trailer: [8B magic][8B original size], both inside the envelope the
     * vault encrypts. qtsq_unpad_size verifies the magic before trusting
     * the size. */
    uint64_t orig = current;
    memcpy(new_buf + target - 16, QTSQ_PAD_MAGIC, 8);
    memcpy(new_buf + target - 8, &orig, 8);

    ctx->sanitize_report |= QTSQ_SANITIZE_SIZE_PADDED;

    /* Hide the exact original size from the cleartext horizon. The true
     * size is preserved only in the 8-byte marker inside the singularity
     * (which the vault encrypts). qtsq_unpad_size() restores it after
     * decrypt. Without this, header.original_size in the plaintext header
     * leaks the exact pre-compression size — defeating the traffic-analysis
     * defense this function provides. */
    ctx->header.original_size = (size_t)target;
    return QTSQ_OK;
}

int qtsq_unpad_size(qtsq_context_t *ctx) {
    if (!ctx || !ctx->singularity) return QTSQ_ERR_NULL;

    size_t current = (size_t)ctx->singularity_size;

    if (!(ctx->sanitize_report & QTSQ_SANITIZE_SIZE_PADDED)) {
        /* After a file round-trip the SIZE_PADDED report flag is not
         * restored by qtsq_read, so verify the in-envelope trailer
         * directly: [8B "QTSQPAD1"][8B original size]. The magic makes a
         * false match on an unpadded singularity ~2^-64. */
        if (current < 17) return QTSQ_OK;
        uint64_t orig;
        memcpy(&orig, ctx->singularity + current - 8, 8);
        if (memcmp(ctx->singularity + current - 16, QTSQ_PAD_MAGIC, 8) != 0 ||
            orig == 0 || orig + 16 > current) {
            return QTSQ_OK; /* not padded — leave untouched */
        }
        ctx->singularity_size = (size_t)orig;
        ctx->header.singularity_size = (size_t)orig;
        ctx->header.original_size = (size_t)orig;
        return QTSQ_OK;
    }

    if (current < 17) return QTSQ_ERR_FORMAT;

    /* Read the original (unpadded) size from the trailer's last 8 bytes.
     * This lived inside the encryption envelope, so it was not visible to
     * an adversary observing the ciphertext. */
    uint64_t orig;
    memcpy(&orig, ctx->singularity + current - 8, 8);
    if (orig == 0 || orig + 16 > current) return QTSQ_ERR_FORMAT;

    /* Shrink the singularity back to the original size */
    ctx->singularity_size = (size_t)orig;
    ctx->header.singularity_size = (size_t)orig;
    ctx->header.original_size = (size_t)orig;
    ctx->sanitize_report &= ~QTSQ_SANITIZE_SIZE_PADDED;
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Sanitize Report — human-readable summary
 * ══════════════════════════════════════════════════════════════ */

const char* qtsq_sanitize_report(const qtsq_context_t *ctx) {
    if (!ctx) return "No context";
    if (!ctx->is_sanitized) return "NOT SANITIZED — privacy guarantees not applied";

    /* M20 fix: use snprintf with remaining-length tracking instead of
     * unbounded strcat (could overflow the 512-byte static buffer). */
    static char report[512];
    size_t pos = 0;

    uint32_t r = ctx->sanitize_report;

    #define APPEND(flag, msg) do { \
        if (r & (flag)) { \
            int _n = snprintf(report + pos, sizeof(report) - pos, \
                              "%s%s", (pos > 0) ? ", " : "", msg); \
            if (_n > 0 && (size_t)_n < sizeof(report) - pos) pos += (size_t)_n; \
        } \
    } while(0)

    APPEND(QTSQ_SANITIZE_ZERO_META, "zero-metadata");
    APPEND(QTSQ_SANITIZE_ENCRYPTED, "encrypted");
    APPEND(QTSQ_SANITIZE_IMAGE_LSB, "image-LSB-scrubbed");
    APPEND(QTSQ_SANITIZE_AUDIO_LSB, "audio-LSB-scrubbed");
    APPEND(QTSQ_SANITIZE_TEXT_NORM, "text-normalized");
    APPEND(QTSQ_SANITIZE_JSON_MINIFY, "JSON-minified");
    APPEND(QTSQ_SANITIZE_NAMES_HIDE, "names-hidden");
    APPEND(QTSQ_SANITIZE_SIZE_PADDED, "size-padded");

    #undef APPEND

    if (pos == 0) {
        snprintf(report, sizeof(report), "(no specific actions recorded)");
    }

    return report;
}

/* ══════════════════════════════════════════════════════════════
 *  Consent Flags — DO_NOT_TRAIN / NO_AI
 * ══════════════════════════════════════════════════════════════ */

int qtsq_set_consent(qtsq_context_t *ctx, uint32_t consent_flags) {
    if (!ctx) return QTSQ_ERR_NULL;

    if (consent_flags & 0x01) ctx->header.flags |= QTSQ_FLAG_DO_NOT_TRAIN;
    if (consent_flags & 0x02) ctx->header.flags |= QTSQ_FLAG_NO_AI;

    return QTSQ_OK;
}
