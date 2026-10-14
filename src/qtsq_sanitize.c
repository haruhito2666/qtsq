/*
 * Quantum Tensor Sequence — Sanitizer & Anti-Steganography
 * Author: Haruhito
 */

#include "qtsq_sanitize.h"
#include "qtsq_format.h"
#include "../crypto/common/randombytes.h"
#include <stdlib.h>
#include <string.h>

/* Prefix tag to identify the embedded metadata block inside singularity */
#define QTSQ_META_PREFIX  "QTSQMETA"
#define QTSQ_META_PREFIX_LEN 8

/* Secure zero — volatile pointer prevents compiler from optimizing away the memset */
void qtsq_secure_zero(void *ptr, size_t len) {
    if (!ptr || len == 0) return;
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}

void qtsq_extract_embedded_names(qtsq_context_t *ctx) {
    if (!ctx || !ctx->singularity || ctx->singularity_size < QTSQ_META_PREFIX_LEN + 2) return;

    /* Only accept metadata embedded at the very end of the singularity.
     * Scanning for the prefix anywhere in the payload caused false-positive
     * matches on compressed/encrypted bytes, corrupting the data on decrypt.
     */
    size_t size = (size_t)ctx->singularity_size;
    size_t min_meta = QTSQ_META_PREFIX_LEN + 2; /* prefix + name_len byte + enc_len byte */
    if (size < min_meta) return;

    size_t off = size - min_meta;
    /* Walk backwards only through positions where the prefix could end exactly
     * at the singularity boundary. We allow up to 63 bytes of name + encoding
     * combined, so search a tiny window near the end. */
    for (int attempt = 0; attempt < 128 && off > 0; attempt++, off--) {
        if (memcmp(ctx->singularity + off, QTSQ_META_PREFIX, QTSQ_META_PREFIX_LEN) != 0)
            continue;

        uint8_t *p = ctx->singularity + off + QTSQ_META_PREFIX_LEN;
        uint8_t name_len = *p++;
        if (name_len > 63) break;
        uint8_t *name_end = p + name_len;
        if (name_end >= ctx->singularity + size) break;
        uint8_t enc_len = *name_end;
        if (enc_len > 63) break;

        size_t meta_total = QTSQ_META_PREFIX_LEN + 1 + name_len + 1 + enc_len;
        if (off + meta_total != size) {
            /* Prefix found, but it doesn't end exactly at singularity boundary.
             * Keep searching backwards for a real embedded block. */
            continue;
        }

        memcpy(ctx->schema.name, p, name_len);
        ctx->schema.name[name_len] = '\0';
        p = name_end + 1;
        memcpy(ctx->schema.encoding, p, enc_len);
        ctx->schema.encoding[enc_len] = '\0';

        /* Trim the metadata from singularity */
        ctx->singularity_size = off;
        ctx->header.singularity_size = off;
        return;
    }
}

int qtsq_sanitize_image_lsb(uint8_t *pixels, uint32_t width, uint32_t height, uint8_t channels) {
    if (!pixels || width == 0 || height == 0 || channels == 0) return QTSQ_ERR_NULL;

    /* M11 fix: use size_t to prevent uint32 overflow on large images
     * (e.g. 65536×65536×4 overflows uint32). */
    size_t total_bytes = (size_t)width * (size_t)height * (size_t)channels;
    
    /* We need a CSPRNG stream to XOR with the LSBs.
     * To avoid calling randombytes() per byte (too slow), we generate a block 
     * of random bytes, XOR 4 pixels per random byte (2 bits per pixel). */
    
    size_t random_len = (total_bytes / 4) + 1;
    uint8_t *rand_stream = (uint8_t *)malloc(random_len);
    if (!rand_stream) return QTSQ_ERR_ALLOC;

    randombytes(rand_stream, random_len);

    for (size_t i = 0; i < total_bytes; i++) {
        uint8_t r_byte = rand_stream[i / 4];
        uint8_t shift = (i % 4) * 2;
        uint8_t r_bits = (r_byte >> shift) & 0x03; /* get 2 bits */
        
        /* XOR the 2 least significant bits */
        pixels[i] ^= r_bits;
    }

    qtsq_secure_zero(rand_stream, random_len);
    free(rand_stream);
    return QTSQ_OK;
}

int qtsq_sanitize_image_lsb16(uint16_t *pixels, uint32_t width, uint32_t height, uint8_t channels) {
    if (!pixels || width == 0 || height == 0 || channels == 0) return QTSQ_ERR_NULL;

    size_t total_samples = (size_t)width * (size_t)height * (size_t)channels;

    /* 2 CSPRNG bits per 16-bit sample; one random byte feeds 4 samples. */
    size_t random_len = (total_samples / 4) + 1;
    uint8_t *rand_stream = (uint8_t *)malloc(random_len);
    if (!rand_stream) return QTSQ_ERR_ALLOC;

    randombytes(rand_stream, random_len);

    for (size_t i = 0; i < total_samples; i++) {
        uint8_t r_byte = rand_stream[i / 4];
        uint8_t shift = (i % 4) * 2;
        uint16_t r_bits = (uint16_t)((r_byte >> shift) & 0x03);

        /* XOR the 2 least significant bits of the 16-bit sample */
        pixels[i] ^= r_bits;
    }

    qtsq_secure_zero(rand_stream, random_len);
    free(rand_stream);
    return QTSQ_OK;
}

/* Prefix tag to identify the embedded metadata block inside singularity.
 * Must match the prefix used by qtsq_extract_embedded_names above. */
#define QTSQ_META_PREFIX_EMBED  "QTSQMETA"
#define QTSQ_META_PREFIX_EMBED_LEN 8

int qtsq_sanitize_context(qtsq_context_t *ctx) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Move display/encoding names inside the singularity envelope so the
     * cleartext horizon never leaks them. This is the floor of Guarantee #1
     * and is the #1 real-world metadata leak per PRODUCT.md. */
    uint8_t name_len = (uint8_t)strnlen(ctx->schema.name, 64);
    uint8_t enc_len  = (uint8_t)strnlen(ctx->schema.encoding, 64);
    size_t meta_size = QTSQ_META_PREFIX_EMBED_LEN + 1 + name_len + 1 + enc_len;

    size_t old_size = (size_t)ctx->singularity_size;
    uint8_t *new_sing = (uint8_t *)realloc(ctx->singularity, old_size + meta_size);
    if (!new_sing) return QTSQ_ERR_ALLOC; /* non-fatal: caller will fail-closed */

    ctx->singularity = new_sing;
    uint8_t *p = ctx->singularity + old_size;

    memcpy(p, QTSQ_META_PREFIX_EMBED, QTSQ_META_PREFIX_EMBED_LEN);
    p += QTSQ_META_PREFIX_EMBED_LEN;
    *p++ = name_len;
    memcpy(p, ctx->schema.name, name_len); p += name_len;
    *p++ = enc_len;
    memcpy(p, ctx->schema.encoding, enc_len);

    ctx->singularity_size = old_size + meta_size;
    ctx->header.singularity_size = ctx->singularity_size;

    /* Clear names from cleartext schema */
    memset(ctx->schema.name, 0, sizeof(ctx->schema.name));
    memset(ctx->schema.encoding, 0, sizeof(ctx->schema.encoding));

    ctx->is_sanitized = 1;
    ctx->sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ZERO_META;
    ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    return QTSQ_OK;
}

char *qtsq_sanitize_text(const char *text, size_t in_len, size_t *out_len, int strict) {
    if (!text || !out_len) return NULL;
    
    char *out = (char *)malloc(in_len + 1);
    if (!out) return NULL;
    
    size_t i = 0, j = 0;
    
    /* Strip UTF-8 BOM if present: EF BB BF */
    if (in_len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
        i = 3;
    }
    
    int last_was_space = 0;
    
    for (; i < in_len; i++) {
        char c = text[i];
        
        /* Normalize CR LF to just LF */
        if (c == '\r') {
            if (i + 1 < in_len && text[i+1] == '\n') continue;
            c = '\n';
        }
        
        if (strict) {
            if (c == ' ' || c == '\t') {
                if (last_was_space) continue;
                last_was_space = 1;
                c = ' ';
            } else {
                last_was_space = 0;
            }
        }
        
        out[j++] = c;
    }
    
    out[j] = '\0';
    *out_len = j;
    return out;
}

char *qtsq_sanitize_json(const char *json, size_t in_len, size_t *out_len) {
    if (!json || !out_len) return NULL;
    
    char *out = (char *)malloc(in_len + 1);
    if (!out) return NULL;
    
    size_t j = 0;
    int in_string = 0;
    int escape = 0;
    
    for (size_t i = 0; i < in_len; i++) {
        char c = json[i];
        
        if (escape) {
            out[j++] = c;
            escape = 0;
            continue;
        }
        
        if (c == '\\') {
            out[j++] = c;
            escape = 1;
            continue;
        }
        
        if (c == '"') {
            in_string = !in_string;
            out[j++] = c;
            continue;
        }
        
        if (!in_string) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                continue; /* Minify structural whitespace */
            }
        }
        
        out[j++] = c;
    }
    
    out[j] = '\0';
    *out_len = j;
    return out;
}

int qtsq_sanitize_audio_lsb(int16_t *samples, size_t num_samples) {
    if (!samples || num_samples == 0) return QTSQ_ERR_NULL;

    /* Generate CSPRNG stream for LSB scrubbing.
     * We XOR the 2 least significant bits of each 16-bit sample.
     * At 16-bit depth, this is below -90 dBFS — completely inaudible. */
    size_t random_len = (num_samples / 4) + 1;
    uint8_t *rand_stream = (uint8_t *)malloc(random_len);
    if (!rand_stream) return QTSQ_ERR_ALLOC;

    randombytes(rand_stream, random_len);

    for (size_t i = 0; i < num_samples; i++) {
        uint8_t r_byte = rand_stream[i / 4];
        uint8_t shift = (i % 4) * 2;
        int16_t r_bits = (int16_t)((r_byte >> shift) & 0x03);

        /* XOR the 2 LSBs — imperceptible noise that destroys stego payloads */
        samples[i] ^= r_bits;
    }

    qtsq_secure_zero(rand_stream, random_len);
    free(rand_stream);
    return QTSQ_OK;
}
