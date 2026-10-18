/*
 * Quantum Tensor Sequence — Post-Quantum Security Vault
 * Author: Haruhito
 */

#include "qtsq_vault.h"
#include "qtsq_sanitize.h"
#include <stdlib.h>
#include <string.h>

/* Relative includes to the vendored PQClean code */
#include "../crypto/common/randombytes.h"
#include "qtsq_rng.h"
#include "../crypto/common/aes.h"

/* M-6: aes256_ctx_release() free()s the expanded key schedule WITHOUT
 * wiping it, and zeroing the context struct afterwards only clears a
 * dangling pointer. Wipe the schedule first, then release. */
static void aes256_release_wipe(aes256ctx *r) {
    if (r->sk_exp)
        qtsq_secure_zero(r->sk_exp, sizeof(uint64_t) * PQC_AES256_STATESIZE);
    aes256_ctx_release(r);
    qtsq_secure_zero(r, sizeof(*r));
}
#include "../crypto/common/fips202.h"
#include "../crypto/ml-kem-768/api.h"
#include "../crypto/ml-dsa-65/api.h"
#include "../crypto/argon2/argon2.h"

/* ═══ HMAC-SHA3-256 for KDF and MAC ═══ */

#define SHA3_256_BLOCK_SIZE 136

static void hmac_sha3_256(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[32]) {
    uint8_t k[SHA3_256_BLOCK_SIZE];
    uint8_t k_ipad[SHA3_256_BLOCK_SIZE];
    uint8_t k_opad[SHA3_256_BLOCK_SIZE];
    
    memset(k, 0, sizeof(k));
    if (key_len > SHA3_256_BLOCK_SIZE) {
        sha3_256(k, key, key_len);
    } else {
        memcpy(k, key, key_len);
    }
    
    for (int i = 0; i < SHA3_256_BLOCK_SIZE; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }
    
    /* Inner hash: H(K^ipad || data) */
    uint8_t inner_hash[32];
    sha3_256incctx inc_ctx;
    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, k_ipad, SHA3_256_BLOCK_SIZE);
    sha3_256_inc_absorb(&inc_ctx, data, data_len);
    sha3_256_inc_finalize(inner_hash, &inc_ctx);
    
    /* Outer hash: H(K^opad || inner_hash) */
    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, k_opad, SHA3_256_BLOCK_SIZE);
    sha3_256_inc_absorb(&inc_ctx, inner_hash, 32);
    sha3_256_inc_finalize(out, &inc_ctx);
}

/* Argon2id password KDF (Phase 2a: replaced homemade iterated SHA3-256).
 * Default params: t_cost=3, m_cost=64MB, parallelism=4 — tuned for security
 * vs. acceptable interactive latency (~100-200ms on modern hardware). */
#define QTSQ_ARGON2_T_COST 3

static int derive_password_key(const char *password, const uint8_t salt[32],
                                uint32_t iterations, uint8_t key[32]) {
    (void)iterations;  /* ignored; Argon2 cost is in t_cost/m_cost, not iteration count */
    
    uint32_t t_cost = QTSQ_ARGON2_T_COST;   /* time cost (iterations) */
    uint32_t m_cost = 65536;        /* memory cost in KiB (64 MB) */
    uint32_t parallelism = 4;       /* number of threads/lanes */
    
    int ret = argon2id_hash_raw(
        t_cost, m_cost, parallelism,
        password, strlen(password),
        salt, 32,
        key, 32
    );
    
    if (ret != ARGON2_OK) {
        /* Never fall back to an all-zero key: the caller would encrypt
         * under a key anyone can derive, and report success. m_cost is
         * 64 MB, so an allocation failure here is routine on memory-
         * capped targets rather than "should never happen". Fail, and
         * let the caller refuse to write the file. */
        memset(key, 0, 32);
        return QTSQ_ERR_ALLOC;
    }
    return QTSQ_OK;
}

/* ═══ Authenticated Encryption (AES-256-CTR + HMAC-SHA3-256) ═══ */

/* M5 fix: derive separate encryption and MAC subkeys from the master key
 * so the same key material isn't used for both AES-CTR and HMAC-SHA3. */
static void derive_subkeys(const uint8_t master[32],
                           uint8_t enc_key[32], uint8_t mac_key[32]) {
    sha3_256incctx hc;
    sha3_256_inc_init(&hc);
    sha3_256_inc_absorb(&hc, master, 32);
    sha3_256_inc_absorb(&hc, (const uint8_t *)"enc", 3);
    sha3_256_inc_finalize(enc_key, &hc);

    sha3_256_inc_init(&hc);
    sha3_256_inc_absorb(&hc, master, 32);
    sha3_256_inc_absorb(&hc, (const uint8_t *)"mac", 3);
    sha3_256_inc_finalize(mac_key, &hc);
}

/* Encrypt-then-MAC */
static int vault_sym_encrypt(qtsq_context_t *ctx, const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t enc_key[32], mac_key[32];
    derive_subkeys(key, enc_key, mac_key);

    uint8_t iv[16] = {0};
    memcpy(iv, nonce, 12);

    aes256ctx aes;
    aes256_ctr_keyexp(&aes, enc_key);

    /* aes256_ctr() in the vendored PQClean AES only emits the keystream;
     * the caller must XOR it with the plaintext. Generate the keystream
     * into a temporary buffer and XOR in-place. */
    uint8_t *keystream = (uint8_t *)malloc(ctx->singularity_size);
    if (!keystream) { aes256_release_wipe(&aes); qtsq_secure_zero(enc_key, 32); qtsq_secure_zero(mac_key, 32); return QTSQ_ERR_ALLOC; }
    aes256_ctr(keystream, ctx->singularity_size, iv, &aes);
    for (size_t i = 0; i < ctx->singularity_size; i++) {
        ctx->singularity[i] ^= keystream[i];
    }
    qtsq_secure_zero(keystream, ctx->singularity_size);
    free(keystream);
    aes256_release_wipe(&aes);
    qtsq_secure_zero(enc_key, 32);

    /* Compute MAC: tag = HMAC-SHA3-256(mac_key, ciphertext) */
    /* Only keeping first 16 bytes for auth_tag to match struct size */
    uint8_t full_mac[32];
    hmac_sha3_256(mac_key, 32, ctx->singularity, ctx->singularity_size, full_mac);
    memcpy(ctx->security.auth_tag, full_mac, 16);
    qtsq_secure_zero(full_mac, sizeof(full_mac));
    
    return QTSQ_OK;
}

static int vault_sym_decrypt(qtsq_context_t *ctx, const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t enc_key[32], mac_key[32];
    derive_subkeys(key, enc_key, mac_key);

    /* Verify MAC first — use constant-time comparison to prevent timing
     * oracle attacks (M1 fix: was memcmp which short-circuits). */
    uint8_t full_mac[32];
    hmac_sha3_256(mac_key, 32, ctx->singularity, ctx->singularity_size, full_mac);

    uint8_t mac_diff = 0;
    for (int i = 0; i < 16; i++) mac_diff |= ctx->security.auth_tag[i] ^ full_mac[i];
    if (mac_diff != 0) {
        qtsq_secure_zero(full_mac, sizeof(full_mac));
        qtsq_secure_zero(enc_key, 32);
        qtsq_secure_zero(mac_key, 32);
        return QTSQ_ERR_DECRYPT;
    }
    qtsq_secure_zero(full_mac, sizeof(full_mac));
    qtsq_secure_zero(mac_key, 32);

    uint8_t iv[16] = {0};
    memcpy(iv, nonce, 12);

    aes256ctx aes;
    aes256_ctr_keyexp(&aes, enc_key);
    
    /* aes256_ctr() only emits the keystream; XOR it with the ciphertext
     * in-place to recover the plaintext. */
    uint8_t *keystream = (uint8_t *)malloc(ctx->singularity_size);
    if (!keystream) { aes256_release_wipe(&aes); qtsq_secure_zero(enc_key, 32); return QTSQ_ERR_ALLOC; }
    aes256_ctr(keystream, ctx->singularity_size, iv, &aes);
    for (size_t i = 0; i < ctx->singularity_size; i++) {
        ctx->singularity[i] ^= keystream[i];
    }
    qtsq_secure_zero(keystream, ctx->singularity_size);
    free(keystream);
    aes256_release_wipe(&aes);
    qtsq_secure_zero(enc_key, 32);
    
    return QTSQ_OK;
}

/* ═══ Key Management ═══ */

int qtsq_keygen(qtsq_keypair_t *kp) {
    if (!kp) return QTSQ_ERR_NULL;
    return PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair(kp->public_key, kp->secret_key) == 0 ? QTSQ_OK : QTSQ_ERR_ALLOC;
}

int qtsq_sign_keygen(qtsq_sign_keypair_t *skp) {
    if (!skp) return QTSQ_ERR_NULL;
    return PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair(skp->sign_pk, skp->sign_sk) == 0 ? QTSQ_OK : QTSQ_ERR_ALLOC;
}

/* ═══ Encryption API ═══ */

static int init_security_block(qtsq_context_t *ctx) {
    /* Preserve any salt that qtsq_container_pack pre-generated so the
     * container name-encryption key (derived from the salt) stays consistent
     * before and after the vault encrypts. Without this, pack sees
     * encryption_type==0 (names cleartext) and the vault regenerates the
     * salt afterward — so the name-encryption Phase 5 code is dead. */
    uint8_t saved_salt[32];
    int has_salt = ctx->has_security;
    if (has_salt) memcpy(saved_salt, ctx->security.salt, 32);

    memset(&ctx->security, 0, sizeof(qtsq_security_block_t));
    ctx->security.security_version = 1;
    if (qtsq_random_bytes(ctx->security.nonce, 12) != 0) return QTSQ_ERR_RANDOM;

    if (has_salt) memcpy(ctx->security.salt, saved_salt, 32);
    else if (qtsq_random_bytes(ctx->security.salt, 32) != 0) return QTSQ_ERR_RANDOM;
    return QTSQ_OK;
}

/* True when the security block itself carries encryption state. Used to
 * fail closed when the header's ENCRYPTED flags have been cleared: the
 * flags are not covered by a MAC that can be checked before the key is
 * known, so a file edit that clears one bit must not turn a sealed file
 * into one that "decrypts" to itself. */
static int security_block_indicates_encryption(const qtsq_context_t *ctx) {
    return ctx->has_security &&
           ctx->security.encryption_type >= 1 && ctx->security.encryption_type <= 3;
}

int qtsq_vault_encrypt_password(qtsq_context_t *ctx, const char *password) {
    if (!ctx || !password) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Refuse a second password layer. The nonce is per-file, so
     * re-encrypting in place would reuse it under the same derived key:
     * identical keystream, and encrypting the ciphertext again XORs that
     * keystream straight back off -- the context would still claim to be
     * encrypted while holding plaintext. Layering (PQ then password) is
     * the one legal re-entry and arrives with encryption_type == 2. */
    if (ctx->is_encrypted && ctx->security.encryption_type != 2)
        return QTSQ_ERR_ENCRYPTED;

    /* Guarantee #1 (zero metadata): if the caller did not run a qtsq_secure_*
     * path (which sets is_sanitized after real LSB scrub + names-in-envelope),
     * apply the data-type-agnostic floor now — move display/encoding names
     * inside the encryption envelope. LSB scrubbing needs the original
     * pixels/samples and is the caller's responsibility, but names-in-envelope
     * is unconditional. This closes the is_sanitized=1 flag-bypass that the
     * CLI/Python/tests/Swift write paths used to skip sanitization. */
    if (!ctx->is_sanitized) {
        int san = qtsq_sanitize_context(ctx);
        if (san != QTSQ_OK) return san;
    }

    /* Phase 2b: if already encrypted with PQ, this becomes layered encryption */
    int was_pq_encrypted = (ctx->is_encrypted && ctx->security.encryption_type == 2);
    
    if (!ctx->is_encrypted) {
        int rng = init_security_block(ctx);
        if (rng != QTSQ_OK) return rng;
    }
    
    /* If layered, backup the PQ layer's auth_tag before password layer overwrites it */
    if (was_pq_encrypted) {
        /* Store PQ auth_tag in signature space at offset 3261 (before salt backup at 3277) */
        memcpy(ctx->security.signature + 3261, ctx->security.auth_tag, 16);
    }
    
    /* Set encryption_type: 1=password-only, 3=layered(PQ+password) */
    ctx->security.encryption_type = was_pq_encrypted ? 3 : 1;
    /* M-8: this field used to claim 100000 while the actual KDF was
     * Argon2id t=3, m=64 MiB, p=4 -- an on-disk lie. It now records the
     * real Argon2 time cost. Readers still ignore it (the cost is fixed
     * in the code, and honouring a file-controlled cost would be a DoS
     * lever), so files written before this change keep working. */
    ctx->security.kdf_iterations = QTSQ_ARGON2_T_COST;

    /* Generate new salt for password layer — unless qtsq_container_pack
     * already pre-generated one (preserving it keeps the name-encryption
     * key consistent with what pack used). */
    if (was_pq_encrypted) {
        /* Store password salt at offset 3277 in signature space */
        if (qtsq_random_bytes(ctx->security.signature + 3277, 32) != 0)
            return QTSQ_ERR_RANDOM;
    } else if (!ctx->has_security) {
        if (qtsq_random_bytes(ctx->security.salt, 32) != 0)
            return QTSQ_ERR_RANDOM;
    }
    ctx->has_security = 1;
    
    uint8_t key[32];
    uint8_t *salt = was_pq_encrypted ? (ctx->security.signature + 3277) : ctx->security.salt;
    if (derive_password_key(password, salt, ctx->security.kdf_iterations, key) != QTSQ_OK)
        return QTSQ_ERR_ALLOC;

    int res = vault_sym_encrypt(ctx, key, ctx->security.nonce);
    qtsq_secure_zero(key, 32);
    
    if (res == QTSQ_OK) {
        ctx->is_encrypted = 1;
        ctx->has_security = 1;
        ctx->header.flags |= QTSQ_FLAG_ENCRYPTED;
        ctx->header.flags |= QTSQ_FLAG_PQ_ENCRYPTED;
    }
    return res;
}

int qtsq_vault_decrypt_password(qtsq_context_t *ctx, const char *password) {
    if (!ctx || !password) return QTSQ_ERR_NULL;
    if (!ctx->is_encrypted) {
        /* Fail closed: a cleared flag is not a decryption. */
        if (security_block_indicates_encryption(ctx)) return QTSQ_ERR_DECRYPT;
        return QTSQ_OK;
    }
    
    /* Phase 2b: handle both password-only (type=1) and layered (type=3) */
    if (ctx->security.encryption_type != 1 && ctx->security.encryption_type != 3) {
        return QTSQ_ERR_DECRYPT;
    }
    
    int is_layered = (ctx->security.encryption_type == 3);
    uint8_t *salt = is_layered ? (ctx->security.signature + 3277) : ctx->security.salt;

    uint8_t key[32];
    if (derive_password_key(password, salt, ctx->security.kdf_iterations, key) != QTSQ_OK)
        return QTSQ_ERR_ALLOC;

    int res = vault_sym_decrypt(ctx, key, ctx->security.nonce);
    qtsq_secure_zero(key, 32);
    
    if (res == QTSQ_OK) {
        if (is_layered) {
            /* After password decrypt, PQ layer remains - change type back to 2 */
            memcpy(ctx->security.auth_tag, ctx->security.signature + 3261, 16);
            ctx->security.encryption_type = 2;
            /* Don't clear is_encrypted - still has PQ layer */
        } else {
            /* Password-only: fully decrypted */
            ctx->is_encrypted = 0;
            ctx->header.flags &= ~QTSQ_FLAG_ENCRYPTED;
            ctx->header.flags &= ~QTSQ_FLAG_PQ_ENCRYPTED;
            ctx->security.encryption_type = 0;
            /* Unwrap in reverse embed order (apply_security embeds names,
             * THEN pads): strip size padding first so the embedded names
             * block sits at the singularity end where extract expects it. */
            qtsq_unpad_size(ctx);
            qtsq_extract_embedded_names(ctx);
        }
    }
    return res;
}

int qtsq_vault_encrypt_pq(qtsq_context_t *ctx, const uint8_t recipient_pk[1184]) {
    if (!ctx || !recipient_pk) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* PQ is always the inner layer: the layered flow encrypts PQ first and
     * then adds a password on top. A second PQ pass would re-init the
     * security block -- overwriting the kem_ciphertext that is the only
     * way back to the first layer -- and re-encrypt ciphertext with a
     * fresh keystream, so it is refused rather than half-supported. */
    if (ctx->is_encrypted) return QTSQ_ERR_ENCRYPTED;

    /* Guarantee #1 (zero metadata): same floor as the password path —
     * move names inside the envelope before encryption. */
    if (!ctx->is_sanitized) {
        int san = qtsq_sanitize_context(ctx);
        if (san != QTSQ_OK) return san;
    }

    {
        int rng = init_security_block(ctx);
        if (rng != QTSQ_OK) return rng;
    }
    ctx->security.encryption_type = 2; /* ML-KEM-768 */
    ctx->security.kem_ct_size = PQCLEAN_MLKEM768_CLEAN_CRYPTO_CIPHERTEXTBYTES;

    uint8_t shared_secret[32];
    if (PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc(ctx->security.kem_ciphertext, shared_secret, recipient_pk) != 0) {
        return QTSQ_ERR_ALLOC;
    }

    int res = vault_sym_encrypt(ctx, shared_secret, ctx->security.nonce);
    qtsq_secure_zero(shared_secret, 32);

    if (res == QTSQ_OK) {
        ctx->is_encrypted = 1;
        ctx->has_security = 1;
        ctx->header.flags |= QTSQ_FLAG_PQ_ENCRYPTED;
    }
    return res;
}

int qtsq_vault_decrypt_pq(qtsq_context_t *ctx, const uint8_t my_sk[2400]) {
    if (!ctx || !my_sk) return QTSQ_ERR_NULL;
    if (!ctx->is_encrypted) {
        if (security_block_indicates_encryption(ctx)) return QTSQ_ERR_DECRYPT;
        return QTSQ_OK;
    }
    
    /* Phase 2b: handle both PQ-only (type=2) and layered (type=3) */
    if (ctx->security.encryption_type != 2 && ctx->security.encryption_type != 3) {
        return QTSQ_ERR_DECRYPT;
    }
    
    int is_layered = (ctx->security.encryption_type == 3);

    uint8_t shared_secret[32];
    if (PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(shared_secret, ctx->security.kem_ciphertext, my_sk) != 0) {
        return QTSQ_ERR_DECRYPT;
    }

    int res = vault_sym_decrypt(ctx, shared_secret, ctx->security.nonce);
    qtsq_secure_zero(shared_secret, 32);

    if (res == QTSQ_OK) {
        if (is_layered) {
            /* After PQ decrypt, password layer remains - change type back to 1 */
            ctx->security.encryption_type = 1;
            /* Copy password salt from backup location to main salt field */
            memcpy(ctx->security.salt, ctx->security.signature + 3277, 32);
            /* Don't clear is_encrypted - still has password layer */
        } else {
            /* PQ-only: fully decrypted */
            ctx->is_encrypted = 0;
            ctx->header.flags &= ~QTSQ_FLAG_PQ_ENCRYPTED;
            ctx->security.encryption_type = 0;
            /* Unwrap in reverse embed order: strip size padding first so the
             * embedded names block sits at the singularity end. */
            qtsq_unpad_size(ctx);
            qtsq_extract_embedded_names(ctx);
        }
    }
    return res;
}

/* ═══ Digital Signature API ═══ */

int qtsq_vault_sign(qtsq_context_t *ctx, const uint8_t sign_sk[4032]) {
    if (!ctx || !sign_sk) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;
    
    ctx->security.signature_type = 1; /* ML-DSA-65 */
    ctx->security.sig_size = PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES;
    
    /* Intent-dependent signature target:
     *   Creative   → sign header + singularity (the re-synthesized output)
     *   Provenance → sign original input bytes (the untouched source) */
    sha3_256incctx inc_ctx;
    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, (const uint8_t*)&ctx->header, sizeof(qtsq_header_t));
    
    if (ctx->intent == QTSQ_INTENT_PROVENANCE && 
        ctx->original_input && ctx->original_input_size > 0) {
        /* Provenance: sign the original input bytes */
        sha3_256_inc_absorb(&inc_ctx, ctx->original_input, ctx->original_input_size);
    } else {
        /* Creative (or legacy): sign the output singularity */
        sha3_256_inc_absorb(&inc_ctx, ctx->singularity, ctx->singularity_size);
    }
    
    uint8_t message_hash[32];
    sha3_256_inc_finalize(message_hash, &inc_ctx);
    
    size_t siglen;
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature(ctx->security.signature, &siglen, message_hash, 32, sign_sk) != 0) {
        return QTSQ_ERR_ALLOC;
    }
    
    /* We could also store a fingerprint of the public key to help receivers */
    /* uint8_t pk_hash[32]; sha3_256(pk_hash, corresponding_pk, 1952); memcpy(...) */
    
    ctx->has_security = 1;
    ctx->header.flags |= QTSQ_FLAG_PQ_SIGNED;
    
    return QTSQ_OK;
}

int qtsq_vault_verify(const qtsq_context_t *ctx, const uint8_t sign_pk[1952]) {
    if (!ctx || !sign_pk) return QTSQ_ERR_NULL;
    if (ctx->security.signature_type != 1) return QTSQ_ERR_FORMAT;
    
    /* Intent-dependent verification target (must match sign logic) */
    sha3_256incctx inc_ctx;
    sha3_256_inc_init(&inc_ctx);
    sha3_256_inc_absorb(&inc_ctx, (const uint8_t*)&ctx->header, sizeof(qtsq_header_t));
    
    if (ctx->intent == QTSQ_INTENT_PROVENANCE &&
        ctx->original_input && ctx->original_input_size > 0) {
        sha3_256_inc_absorb(&inc_ctx, ctx->original_input, ctx->original_input_size);
    } else {
        sha3_256_inc_absorb(&inc_ctx, ctx->singularity, ctx->singularity_size);
    }
    
    uint8_t message_hash[32];
    sha3_256_inc_finalize(message_hash, &inc_ctx);
    
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify(ctx->security.signature, ctx->security.sig_size, message_hash, 32, sign_pk) != 0) {
        return QTSQ_ERR_CHECKSUM; /* Signature verification failed */
    }
    
    return QTSQ_OK;
}

/* ═══ Query API ═══ */

int qtsq_vault_is_encrypted(const qtsq_context_t *ctx) {
    return ctx && ctx->is_encrypted;
}

int qtsq_vault_is_signed(const qtsq_context_t *ctx) {
    return ctx && ctx->has_security && ctx->security.signature_type != 0;
}

int qtsq_vault_get_security_level(const qtsq_context_t *ctx) {
    if (!ctx) return 0;
    int enc = 0;
    if (ctx->is_encrypted) {
        enc = (ctx->security.encryption_type == 2) ? 2 : 1;
    }
    int sig = (ctx->has_security && ctx->security.signature_type != 0) ? 1 : 0;
    
    if (enc == 2 && sig == 1) return 3;
    if (enc == 2) return 2;
    if (enc == 1) return 1;
    if (sig == 1) return 1; // Unencrypted but signed
    return 0;
}
