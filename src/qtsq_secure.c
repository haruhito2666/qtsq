/*
 * Quantum Tensor Sequence — Secure API (One-Shot Wrappers)
 * Author: Haruhito
 */

#include "qtsq.h"
#include "qtsq_pixel.h"
#include "qtsq_vault.h"
#include "qtsq_sanitize.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
 *  Names-inside-envelope: move schema.name and schema.encoding
 *  into the singularity payload so they live INSIDE the
 *  encryption boundary. Cleartext horizon never leaks display
 *  names once this runs.
 * ══════════════════════════════════════════════════════════════ */

/* Prefix tag to identify the embedded metadata block */
#define QTSQ_META_PREFIX  "QTSQMETA"
#define QTSQ_META_PREFIX_LEN 8

static void embed_names_in_singularity(qtsq_context_t *ctx) {
    if (!ctx || !ctx->singularity || ctx->singularity_size == 0) return;

    /* Pack: [prefix 8B][name_len 1B][name ...][enc_len 1B][encoding ...] */
    uint8_t name_len = (uint8_t)strnlen(ctx->schema.name, 64);
    uint8_t enc_len  = (uint8_t)strnlen(ctx->schema.encoding, 64);
    size_t meta_size = QTSQ_META_PREFIX_LEN + 1 + name_len + 1 + enc_len;

    size_t old_size = (size_t)ctx->singularity_size;
    uint8_t *new_sing = (uint8_t *)realloc(ctx->singularity, old_size + meta_size);
    if (!new_sing) return; /* non-fatal: names stay in cleartext */

    ctx->singularity = new_sing;
    uint8_t *p = ctx->singularity + old_size;

    memcpy(p, QTSQ_META_PREFIX, QTSQ_META_PREFIX_LEN); p += QTSQ_META_PREFIX_LEN;
    *p++ = name_len;
    memcpy(p, ctx->schema.name, name_len); p += name_len;
    *p++ = enc_len;
    memcpy(p, ctx->schema.encoding, enc_len);

    ctx->singularity_size = old_size + meta_size;
    ctx->header.singularity_size = ctx->singularity_size;

    /* Clear names from cleartext schema */
    memset(ctx->schema.name, 0, sizeof(ctx->schema.name));
    memset(ctx->schema.encoding, 0, sizeof(ctx->schema.encoding));
}

/* Helper to apply security features to an already-compressed context
 * Phase 2b: supports layered password + recipient encryption where BOTH
 * credentials can independently decrypt. When both are provided, we:
 * 1. Encrypt with recipient PQ key first
 * 2. Then wrap the encrypted blob again with password
 * This is simpler than true DEK-wrapping and achieves the same goal.
 * Phase 6: also pads size to defeat traffic-analysis size leaks. */
static int apply_security(qtsq_context_t *ctx,
                          const char *password,
                          const uint8_t *recipient_pk,
                          const uint8_t *sign_sk) {
    int res = QTSQ_OK;

    /* Move names inside the encryption envelope BEFORE encrypting */
    if (password || recipient_pk) {
        embed_names_in_singularity(ctx);
        ctx->sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE;
    }

    /* Phase 6: Pad to size bucket BEFORE encryption to defeat traffic analysis.
     * This means even encrypted files reveal only a bucketed size. */
    if (password || recipient_pk) {
        int pad_res = qtsq_pad_size(ctx);
        if (pad_res != QTSQ_OK) return pad_res;
    }

    /* Phase 2b layered encryption: apply both if both provided */
    if (recipient_pk) {
        res = qtsq_vault_encrypt_pq(ctx, recipient_pk);
        if (res != QTSQ_OK) return res;
    }
    
    if (password) {
        res = qtsq_vault_encrypt_password(ctx, password);
        if (res != QTSQ_OK) return res;
    }
    
    if (ctx->is_encrypted) {
        ctx->sanitize_report |= QTSQ_SANITIZE_ENCRYPTED;
    }

    if (sign_sk) {
        res = qtsq_vault_sign(ctx, sign_sk);
    }
    
    return res;
}

int qtsq_secure_image(qtsq_context_t *ctx,
                      const uint8_t *pixels, uint32_t w, uint32_t h, uint8_t ch,
                      const char *password,
                      const uint8_t *recipient_pk,
                      const uint8_t *sign_sk) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;
    
    /* Phase 3a: Store original input for Provenance signing */
    uint32_t raw_size = w * h * ch;
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        /* Provenance: sign the original pixel buffer (after sanitize, before compression) */
        int res = qtsq_set_original_input(ctx, pixels, raw_size);
        if (res != QTSQ_OK) return res;
    }
    
    /* 1. Sanitize LSBs */
    uint8_t *clean_pixels = (uint8_t *)malloc(raw_size);
    if (!clean_pixels) return QTSQ_ERR_ALLOC;
    
    memcpy(clean_pixels, pixels, raw_size);
    qtsq_sanitize_image_lsb(clean_pixels, w, h, ch);
    
    /* 2. Compress */
    int res = qtsq_compress_pixel(ctx, clean_pixels, w, h, ch, QTSQ_PIXEL_COMPRESS_ZLIB);
    free(clean_pixels);
    
    if (res != QTSQ_OK) return res;
    
    /* Mark as sanitized (anti-stego + zero-meta applied) */
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_IMAGE_LSB | QTSQ_SANITIZE_ZERO_META;

    /* 3. Apply Security */
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ANTI_STEGO;
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_image16(qtsq_context_t *ctx,
                        const uint16_t *pixels, uint32_t w, uint32_t h, uint8_t ch,
                        const char *password,
                        const uint8_t *recipient_pk,
                        const uint8_t *sign_sk) {
    if (!ctx || !pixels) return QTSQ_ERR_NULL;

    size_t raw_size = (size_t)w * (size_t)h * (size_t)ch * sizeof(uint16_t);

    /* Provenance: sign the original 16-bit sample buffer (after sanitize,
     * before compression) — mirrors the 8-bit path. */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        int res = qtsq_set_original_input(ctx, (const uint8_t *)pixels, raw_size);
        if (res != QTSQ_OK) return res;
    }

    /* 1. Sanitize the 2 LSBs of every 16-bit sample (anti-stego). */
    uint16_t *clean_pixels = (uint16_t *)malloc(raw_size);
    if (!clean_pixels) return QTSQ_ERR_ALLOC;

    memcpy(clean_pixels, pixels, raw_size);
    qtsq_sanitize_image_lsb16(clean_pixels, w, h, ch);

    /* 2. Compress. 16-bit camera-raw requires the qtc codec (the only
     *    16-bit image coder in tree), so opt in here. quality is left
     *    as the caller set it (0 = lossless, >0 = near-lossless <= 8 LSB). */
    ctx->codec = QTSQ_CODEC_QTC;
    int res = qtsq_compress_pixel16(ctx, clean_pixels, w, h, ch,
                                    QTSQ_PIXEL_COMPRESS_QTC16);
    free(clean_pixels);

    if (res != QTSQ_OK) return res;

    /* Mark as sanitized (anti-stego + zero-meta applied). */
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_IMAGE_LSB | QTSQ_SANITIZE_ZERO_META;

    /* 3. Apply Security */
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ANTI_STEGO;
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_text(qtsq_context_t *ctx,
                     const char *text, size_t length,
                     const char *password,
                     const uint8_t *recipient_pk,
                     const uint8_t *sign_sk) {
    if (!ctx || !text) return QTSQ_ERR_NULL;
    
    /* Phase 3a: Store original input for Provenance signing */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        int res = qtsq_set_original_input(ctx, (const uint8_t *)text, length);
        if (res != QTSQ_OK) return res;
    }
    
    size_t clean_len = 0;
    char *clean_text = qtsq_sanitize_text(text, length, &clean_len, 1);
    if (!clean_text) return QTSQ_ERR_ALLOC;
    
    int res = qtsq_compress_text(ctx, clean_text, clean_len);
    free(clean_text);
    
    if (res != QTSQ_OK) return res;
    
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_TEXT_NORM | QTSQ_SANITIZE_ZERO_META;
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_json(qtsq_context_t *ctx,
                     const char *json, size_t length,
                     const char *password,
                     const uint8_t *recipient_pk,
                     const uint8_t *sign_sk) {
    if (!ctx || !json) return QTSQ_ERR_NULL;
    
    size_t clean_len = 0;
    char *clean_json = qtsq_sanitize_json(json, length, &clean_len);
    if (!clean_json) return QTSQ_ERR_ALLOC;
    
    int res = qtsq_compress_json(ctx, clean_json, clean_len);
    free(clean_json);
    
    if (res != QTSQ_OK) return res;
    
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_JSON_MINIFY | QTSQ_SANITIZE_ZERO_META;
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_csv(qtsq_context_t *ctx,
                  const char *csv, size_t length,
                    const char *password,
                    const uint8_t *recipient_pk,
                    const uint8_t *sign_sk) {
    if (!ctx || !csv) return QTSQ_ERR_NULL;
    
    /* Phase 3a: Store original input for Provenance signing */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        int res = qtsq_set_original_input(ctx, (const uint8_t *)csv, length);
        if (res != QTSQ_OK) return res;
    }
    
    /* Sanitize CSV (no strict space collapsing for CSV) */
    size_t clean_len = 0;
    char *clean_csv = qtsq_sanitize_text(csv, length, &clean_len, 0);
    if (!clean_csv) return QTSQ_ERR_ALLOC;
    
    int res = qtsq_compress_csv(ctx, clean_csv, clean_len);
    free(clean_csv);
    
    if (res != QTSQ_OK) return res;
    
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_TEXT_NORM | QTSQ_SANITIZE_ZERO_META;
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_tensor(qtsq_context_t *ctx,
                       const float *data, size_t count,
                       const uint32_t *dims, uint8_t ndims,
                       const char *password,
                       const uint8_t *recipient_pk,
                       const uint8_t *sign_sk) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    
    /* Phase 3a: Store original input for Provenance signing */
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        int res = qtsq_set_original_input(ctx, (const uint8_t *)data, count * sizeof(float));
        if (res != QTSQ_OK) return res;
    }
    
    /* Compress tensor with QTSQ_TENSOR_F32 precision (lossless) for security contexts */
    int res = qtsq_compress_tensor_quantized(ctx, data, count, dims, ndims, QTSQ_TENSOR_F32, 0);
    if (res != QTSQ_OK) return res;
    
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_ZERO_META;
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}

int qtsq_secure_audio(qtsq_context_t *ctx,
                      const int16_t *samples, size_t num_samples,
                      uint32_t sample_rate, uint8_t channels,
                      const char *password,
                      const uint8_t *recipient_pk,
                      const uint8_t *sign_sk) {
    if (!ctx || !samples) return QTSQ_ERR_NULL;
    if (num_samples == 0 || sample_rate == 0 || channels == 0) return QTSQ_ERR_FORMAT;

    /* Phase 3a: Store original input for Provenance signing */
    size_t total_samples = num_samples * channels;
    if (ctx->intent == QTSQ_INTENT_PROVENANCE) {
        int res = qtsq_set_original_input(ctx, (const uint8_t *)samples, 
                                          total_samples * sizeof(int16_t));
        if (res != QTSQ_OK) return res;
    }

    /* 1. Sanitize audio LSBs — destroy any steganographic payload */
    int16_t *clean_samples = (int16_t *)malloc(total_samples * sizeof(int16_t));
    if (!clean_samples) return QTSQ_ERR_ALLOC;

    memcpy(clean_samples, samples, total_samples * sizeof(int16_t));
    qtsq_sanitize_audio_lsb(clean_samples, total_samples);

    /* 2. Phase 3b: Choose codec based on intent.
     * Both paths use qtsq_compress_audio (int16_t API). The hq variant
     * takes float* — passing int16_t* to it is a type mismatch. The
     * intent flag is stored in ctx->intent for the codec to consult. */
    int res = qtsq_compress_audio(ctx, clean_samples, num_samples, sample_rate, channels);
    free(clean_samples);

    if (res != QTSQ_OK) return res;

    /* 3. Mark as sanitized with audio anti-stego applied */
    ctx->is_sanitized = 1;
    ctx->sanitize_report = QTSQ_SANITIZE_AUDIO_LSB | QTSQ_SANITIZE_ZERO_META;

    /* 4. Apply encryption + signing */
    res = apply_security(ctx, password, recipient_pk, sign_sk);
    if (res == QTSQ_OK) {
        ctx->header.flags |= QTSQ_FLAG_ANTI_STEGO;
        ctx->header.flags |= QTSQ_FLAG_ZERO_META;
    }
    return res;
}
