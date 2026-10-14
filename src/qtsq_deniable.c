/*
 * Quantum Tensor Sequence — Deniable Dual-Image Encryption
 *
 * Construction:
 *   Let P = max(real_size, decoy_size) * 2 + 128 (padding).
 *   Derive key_A = KDF(real_password,  salt_A)
 *   Derive key_B = KDF(decoy_password, salt_B)
 *
 *   Hidden volume approach:
 *   Payload = [Noise]
 *   Decoy Envelope = [Size(4)][MAC(32)][Decoy Data]
 *   Real Envelope = [Size(4)][MAC(32)][Real Data]
 *
 *   We encrypt the Decoy Envelope and place it at offset 0.
 *   We encrypt the Real Envelope and place it at offset P/2.
 *   (Rest of the file is random noise).
 *
 *   When decrypting, we check both offsets (0 and P/2). If the MAC validates
 *   under the derived key, we know we found a valid envelope.
 *
 * Author: Haruhito
 */

#include "qtsq_deniable.h"
#include "qtsq.h"
#include "qtsq_sanitize.h"
#include "../crypto/common/aes.h"
#include "../crypto/common/fips202.h"
#include "../crypto/common/randombytes.h"
#include "../crypto/argon2/argon2.h"
#include <string.h>
#include <stdlib.h>

/* ── Key derivation (M3 fix: Argon2id, same params as the vault) ── */

static int deniable_kdf(const char *password, const uint8_t salt[32],
                          uint32_t iters, uint8_t key[32]) {
    (void)iters; /* Argon2 cost is in t_cost/m_cost, not iteration count */
    int ret = argon2id_hash_raw(
        3, 65536, 4,            /* t_cost=3, m_cost=64MB, parallelism=4 */
        password, strlen(password),
        salt, 32,
        key, 32);
    if (ret != ARGON2_OK) {
        /* Zero key on failure would encrypt under a publicly known key.
         * Propagate instead; see qtsq_vault.c derive_password_key. */
        memset(key, 0, 32);
        return QTSQ_ERR_ALLOC;
    }
    return QTSQ_OK;
}

/* ── AES-256-CTR stream ── */

static void aes_ctr_stream(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t iv[16] = {0};
    memcpy(iv, nonce, 12);
    aes256ctx aes;
    aes256_ctr_keyexp(&aes, key);
    aes256_ctr(out, len, iv, &aes);
    aes256_ctx_release(&aes);
    /* XOR with plaintext */
    for (size_t i = 0; i < len; i++) out[i] ^= in[i];
}

/* ── HMAC-SHA3-256 for authentication ── */

static void hmac_sha3(const uint8_t key[32], const uint8_t *data, size_t len,
                       uint8_t mac[32]) {
    sha3_256incctx ctx;
    sha3_256_inc_init(&ctx);
    sha3_256_inc_absorb(&ctx, key, 32);
    sha3_256_inc_absorb(&ctx, data, len);
    sha3_256_inc_finalize(mac, &ctx);
}

#define DEN_HDR_SIZE 64
#define DEN_ITERS    5000

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct {
    uint8_t salt_A[32];
    uint8_t salt_B[32];
}
#if defined(_MSC_VER)
deniable_hdr_t;
#pragma pack(pop)
#else
__attribute__((packed)) deniable_hdr_t;
#endif

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Encrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_deniable_encrypt(qtsq_context_t *ctx,
                           const uint8_t *real_data,  size_t real_size,
                           const uint8_t *decoy_data, size_t decoy_size,
                           const char *real_password,
                           const char *decoy_password) {
    if (!ctx || !real_data || !decoy_data || !real_password || !decoy_password) return QTSQ_ERR_NULL;
    if (real_size == 0 || decoy_size == 0) return QTSQ_ERR_TOO_SMALL;

    size_t P = (real_size > decoy_size ? real_size : decoy_size) * 2 + 128;

    uint8_t *payload = (uint8_t *)malloc(P);
    if (!payload) return QTSQ_ERR_ALLOC;
    randombytes(payload, P); 

    deniable_hdr_t hdr;
    randombytes(hdr.salt_A, 32);
    randombytes(hdr.salt_B, 32);

    uint8_t key_A[32], key_B[32];
    deniable_kdf(real_password, hdr.salt_A, DEN_ITERS, key_A);
    deniable_kdf(decoy_password, hdr.salt_B, DEN_ITERS, key_B);

    uint8_t nonce_A[12], nonce_B[12];
    memcpy(nonce_A, hdr.salt_A, 12);
    memcpy(nonce_B, hdr.salt_B, 12);

    /* Construct Decoy Envelope at offset 0: [Size(4)] [MAC(32)] [Data] */
    uint32_t ds32 = (uint32_t)decoy_size;
    uint8_t *decoy_env = (uint8_t *)malloc(36 + decoy_size);
    memcpy(decoy_env, &ds32, 4);
    hmac_sha3(key_B, decoy_data, decoy_size, decoy_env + 4); 
    memcpy(decoy_env + 36, decoy_data, decoy_size);
    aes_ctr_stream(key_B, nonce_B, decoy_env, payload, 36 + decoy_size);
    free(decoy_env);

    /* Construct Real Envelope at offset P/2: [Size(4)] [MAC(32)] [Data] */
    uint32_t rs32 = (uint32_t)real_size;
    uint8_t *real_env = (uint8_t *)malloc(36 + real_size);
    memcpy(real_env, &rs32, 4);
    hmac_sha3(key_A, real_data, real_size, real_env + 4);
    memcpy(real_env + 36, real_data, real_size);
    aes_ctr_stream(key_A, nonce_A, real_env, payload + (P / 2), 36 + real_size);
    free(real_env);

    size_t total = DEN_HDR_SIZE + P;
    uint8_t *singularity = (uint8_t *)malloc(total);
    memcpy(singularity, &hdr, DEN_HDR_SIZE);
    memcpy(singularity + DEN_HDR_SIZE, payload, P);

    memset(key_A, 0, 32); memset(key_B, 0, 32);
    free(payload);

    if (ctx->singularity) free(ctx->singularity);
    ctx->singularity = singularity;
    ctx->singularity_size = (uint32_t)total;
    ctx->header.singularity_size = (uint32_t)total;
    ctx->header.flags |= QTSQ_FLAG_DENIABLE | QTSQ_FLAG_ENCRYPTED;
    ctx->is_encrypted = 1;
    ctx->has_security = 1;
    ctx->security.encryption_type = 3;

    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: Decrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_deniable_decrypt(qtsq_context_t *ctx,
                           const char *password,
                           uint8_t **out_data, size_t *out_size) {
    if (!ctx || !password || !out_data || !out_size) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size <= DEN_HDR_SIZE) return QTSQ_ERR_FORMAT;

    deniable_hdr_t hdr;
    memcpy(&hdr, ctx->singularity, DEN_HDR_SIZE);
    size_t P = (size_t)ctx->singularity_size - DEN_HDR_SIZE;
    const uint8_t *payload = ctx->singularity + DEN_HDR_SIZE;

    /* Each envelope needs at least 4 (size) + 32 (MAC) = 36 bytes, and the
     * real envelope lives at P/2 so P must be ≥ 72. A crafted small file
     * would otherwise underflow the (P - 36) / ((P/2) - 36) size checks
     * (unsigned wrap → huge) and cause a heap over-read before any MAC
     * check. */
    if (P < 72) return QTSQ_ERR_FORMAT;

    uint8_t key[32], nonce[12];
    uint8_t *decrypted = (uint8_t *)malloc(P);
    if (!decrypted) return QTSQ_ERR_ALLOC;

    /* Try Decoy (Offset 0) */
    deniable_kdf(password, hdr.salt_B, DEN_ITERS, key);
    memcpy(nonce, hdr.salt_B, 12);
    aes_ctr_stream(key, nonce, payload, decrypted, P);

    uint32_t decoy_sz;
    memcpy(&decoy_sz, decrypted, 4);
    if (decoy_sz > 0 && decoy_sz <= P - 36) {
        uint8_t computed_mac[32];
        hmac_sha3(key, decrypted + 36, decoy_sz, computed_mac);
        /* M1 fix: constant-time MAC comparison (was memcmp — timing oracle). */
        uint8_t mac_diff = 0;
        for (int i = 0; i < 32; i++) mac_diff |= computed_mac[i] ^ decrypted[4 + i];
        if (mac_diff == 0) {
            *out_data = (uint8_t *)malloc(decoy_sz);
            memcpy(*out_data, decrypted + 36, decoy_sz);
            *out_size = decoy_sz;
            qtsq_secure_zero(key, 32);
            qtsq_secure_zero(decrypted, P);
            free(decrypted);
            ctx->is_encrypted = 0;
            return QTSQ_OK;
        }
    }

    /* Try Real (Offset P/2) */
    qtsq_secure_zero(key, 32);
    deniable_kdf(password, hdr.salt_A, DEN_ITERS, key);
    memcpy(nonce, hdr.salt_A, 12);
    aes_ctr_stream(key, nonce, payload + (P / 2), decrypted, P / 2);

    uint32_t real_sz;
    memcpy(&real_sz, decrypted, 4);
    if (real_sz > 0 && real_sz <= (P / 2) - 36) {
        uint8_t computed_mac[32];
        hmac_sha3(key, decrypted + 36, real_sz, computed_mac);
        /* M1 fix: constant-time MAC comparison. */
        uint8_t mac_diff = 0;
        for (int i = 0; i < 32; i++) mac_diff |= computed_mac[i] ^ decrypted[4 + i];
        if (mac_diff == 0) {
            *out_data = (uint8_t *)malloc(real_sz);
            memcpy(*out_data, decrypted + 36, real_sz);
            *out_size = real_sz;
            qtsq_secure_zero(key, 32);
            qtsq_secure_zero(decrypted, P);
            free(decrypted);
            ctx->is_encrypted = 0;
            return QTSQ_OK;
        }
    }

    qtsq_secure_zero(key, 32);
    qtsq_secure_zero(decrypted, P);
    free(decrypted);
    return QTSQ_ERR_DECRYPT;
}
