/*
 * Quantum Tensor Sequence — Cosmic Censorship Engine (Encryption)
 * Feature: Password-based encryption using custom AES-256 + GCM-like auth
 * Key derivation: PBKDF2-like iterative hashing
 * Author: Haruhito
 *
 * NOTE: This is a lightweight, self-contained implementation.
 * For production, link against OpenSSL/libsodium for validated AES-256-GCM.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "qtsq.h"
#include "qtsq_sanitize.h"
#include "../crypto/common/aes.h"   /* PQClean constant-time AES-256-CTR */

/* ── CTR mode (Counter) for streaming encryption ──
 *
 * FIX (#8): the previous implementation used a hand-rolled AES with a
 * data-dependent S-Box lookup table — vulnerable to cache-timing side
 * channels. We now delegate to the vendored PQClean aes256_ctr, which is
 * constant-time and table-free. The local function keeps the same
 * (in, out, size, key, iv) signature the callers already use. */
static void aes256_ctr_local(const uint8_t *in, uint8_t *out, size_t size,
                             const uint8_t key[32], const uint8_t iv[16]) {
    aes256ctx aes;
    aes256_ctr_keyexp(&aes, key);

    /* PQClean aes256_ctr emits the keystream; the caller XORs it with the
     * input. Generate the keystream and XOR in-place. */
    uint8_t *keystream = (uint8_t *)malloc(size);
    if (!keystream) {
        /* Allocation failure — caller will see unmodified output, but this
         * path is only reached under memory pressure. */
        aes256_ctx_release(&aes);
        return;
    }
    aes256_ctr(keystream, size, iv, &aes);
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i] ^ keystream[i];
    }

    qtsq_secure_zero(keystream, size);
    free(keystream);
    aes256_ctx_release(&aes);
    qtsq_secure_zero(&aes, sizeof(aes));
}

/* ── PBKDF2-like key derivation (SHA-256 based) ── */

static void derive_key(const char *password, const uint8_t salt[16],
                        uint32_t iterations, uint8_t key[32]) {
    /* Simple iterative hash: key = SHA256(SHA256(...SHA256(password + salt))) */
    uint8_t buf[256];
    size_t pw_len = strlen(password);
    /* Clamp pw_len so the salt write stays in-bounds: we need pw_len + 16
     * ≤ sizeof(buf). Without this, a password ≥ 241 bytes overflows buf. */
    if (pw_len > sizeof(buf) - 16) pw_len = sizeof(buf) - 16;
    size_t buf_len = pw_len + 16;

    memcpy(buf, password, pw_len);
    memcpy(buf + pw_len, salt, 16);

    qtsq_sha256(buf, buf_len, key);

    for (uint32_t i = 1; i < iterations; i++) {
        /* Hash the previous output concatenated with salt */
        memcpy(buf, key, 32);
        memcpy(buf + 32, salt, 16);
        qtsq_sha256(buf, 48, key);
    }

    qtsq_secure_zero(buf, sizeof(buf));
}

/* ── Cryptographically secure random bytes ── */

static void generate_random_bytes(uint8_t *out, size_t len) {
#if defined(__APPLE__)
    /* arc4random_buf is seeded by the OS kernel — CSPRNG, no setup needed */
    arc4random_buf(out, len);
#elif defined(__linux__) || defined(__unix__) || defined(__EMSCRIPTEN__)
    /* /dev/urandom is the correct POSIX source.
     * On Emscripten, the C runtime maps this to crypto.getRandomValues(). */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t rd = fread(out, 1, len, f);
        fclose(f);
        if (rd == len) return;
    }
    /* Fallback if /dev/urandom fails: use getrandom() syscall on Linux 3.17+ */
    #if defined(__linux__) && !defined(__ANDROID__)
    #include <sys/random.h>
    getrandom(out, len, 0);
    return;
    #endif
#elif defined(_WIN32)
    #include <bcrypt.h>
    BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return;
#else
    /* Hard abort — never ship encrypted files with a broken RNG */
    #error "No CSPRNG available for this platform. Add platform support before enabling encryption."
#endif
}

/* ── HMAC-SHA256 authentication tag (CRYPT-3 fix) ──
 *
 * Previous implementation: SHA256(key || ciphertext) — vulnerable to
 * SHA-256 length extension attacks. An attacker knowing the tag for
 * message M can compute the valid tag for M || padding || M' without
 * knowing the key.
 *
 * Proper HMAC construction:
 *   HMAC(key, msg) = SHA256((key XOR opad) || SHA256((key XOR ipad) || msg))
 * This is not vulnerable to length extension because the outer hash wraps
 * the inner hash result, which is opaque to the attacker.
 */

static void compute_auth_tag(const uint8_t *ciphertext, size_t size,
                              const uint8_t key[32], uint8_t tag[16]) {
    const size_t BLOCK_SIZE = 64; /* SHA-256 block size */
    uint8_t ipad[64], opad[64];
    uint8_t inner_hash[32];
    uint8_t outer_input[64 + 32]; /* opad block + inner hash */
    uint8_t outer_hash[32];

    /* Build ipad and opad from key (key is 32 bytes, zero-padded to 64) */
    for (int i = 0; i < 64; i++) {
        uint8_t k = (i < 32) ? key[i] : 0;
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5c;
    }

    /* Inner hash: SHA256(ipad || ciphertext)
     * We compute it in two passes: first hash ipad block, then extend with ciphertext.
     * Since qtsq_sha256 takes a single contiguous buffer, allocate one. */
    uint8_t *inner_buf = (uint8_t *)malloc(BLOCK_SIZE + size);
    if (!inner_buf) { qtsq_secure_zero(tag, 16); return; }
    memcpy(inner_buf, ipad, BLOCK_SIZE);
    memcpy(inner_buf + BLOCK_SIZE, ciphertext, size);
    qtsq_sha256(inner_buf, BLOCK_SIZE + size, inner_hash);
    free(inner_buf);

    /* Outer hash: SHA256(opad || inner_hash) */
    memcpy(outer_input, opad, BLOCK_SIZE);
    memcpy(outer_input + BLOCK_SIZE, inner_hash, 32);
    qtsq_sha256(outer_input, BLOCK_SIZE + 32, outer_hash);

    /* Use first 16 bytes of HMAC-SHA256 as the tag */
    memcpy(tag, outer_hash, 16);

    /* Wipe temporaries */
    qtsq_secure_zero(ipad, 64);
    qtsq_secure_zero(opad, 64);
    qtsq_secure_zero(inner_hash, 32);
    qtsq_secure_zero(outer_hash, 32);
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_encrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_encrypt(qtsq_context_t *ctx, const char *password) {
    if (!ctx || !password) return QTSQ_ERR_NULL;
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;
    if (ctx->is_encrypted) return QTSQ_OK; /* already encrypted */

    /* Generate random IV and salt */
    generate_random_bytes(ctx->censorship.iv, 16);
    generate_random_bytes(ctx->censorship.salt, 16);
    ctx->censorship.iterations = 100000;

    /* Derive AES-256 key from password */
    uint8_t key[32];
    derive_key(password, ctx->censorship.salt,
               ctx->censorship.iterations, key);

    /* Encrypt singularity data in-place using CTR mode */
    uint8_t *ciphertext = (uint8_t *)malloc(ctx->singularity_size);
    if (!ciphertext) { qtsq_secure_zero(key, 32); return QTSQ_ERR_ALLOC; }

    aes256_ctr_local(ctx->singularity, ciphertext, ctx->singularity_size,
                     key, ctx->censorship.iv);

    /* Compute authentication tag */
    compute_auth_tag(ciphertext, ctx->singularity_size, key,
                     ctx->censorship.auth_tag);

    /* Replace singularity with ciphertext */
    memcpy(ctx->singularity, ciphertext, ctx->singularity_size);
    free(ciphertext);

    /* Mark as encrypted */
    ctx->is_encrypted = 1;
    ctx->header.flags |= QTSQ_FLAG_ENCRYPTED;

    /* Wipe key */
    qtsq_secure_zero(key, 32);

    return QTSQ_OK;
}

/* ── Constant-time memory comparison (CRYPT-2) ──
 * Unlike memcmp(), this never short-circuits on mismatch, preventing
 * timing oracle attacks where attackers measure how many bytes matched. */
static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return (int)diff; /* 0 = equal, non-zero = different */
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_decrypt
 * ══════════════════════════════════════════════════════════════ */

int qtsq_decrypt(qtsq_context_t *ctx, const char *password) {
    if (!ctx || !password) return QTSQ_ERR_NULL;
    if (!ctx->is_encrypted) return QTSQ_OK; /* not encrypted */
    if (!ctx->singularity || ctx->singularity_size == 0) return QTSQ_ERR_FORMAT;

    /* Derive key from password */
    uint8_t key[32];
    derive_key(password, ctx->censorship.salt,
               ctx->censorship.iterations, key);

    /* Verify authentication tag before decrypting.
     * CRYPT-2 FIX: use constant-time comparison — see ct_memcmp() above. */
    uint8_t computed_tag[16];
    compute_auth_tag(ctx->singularity, ctx->singularity_size,
                     key, computed_tag);

    if (ct_memcmp(computed_tag, ctx->censorship.auth_tag, 16) != 0) {
        qtsq_secure_zero(key, 32);
        return QTSQ_ERR_DECRYPT; /* wrong password or tampered data */
    }

    /* Decrypt in-place using CTR mode (CTR is symmetric) */
    uint8_t *plaintext = (uint8_t *)malloc(ctx->singularity_size);
    if (!plaintext) { qtsq_secure_zero(key, 32); return QTSQ_ERR_ALLOC; }

    aes256_ctr_local(ctx->singularity, plaintext, ctx->singularity_size,
                     key, ctx->censorship.iv);

    memcpy(ctx->singularity, plaintext, ctx->singularity_size);
    free(plaintext);

    /* Mark as decrypted */
    ctx->is_encrypted = 0;
    ctx->header.flags &= ~QTSQ_FLAG_ENCRYPTED;

    /* Restore names that were hidden inside the encryption envelope */
    qtsq_extract_embedded_names(ctx);

    qtsq_secure_zero(key, 32);
    return QTSQ_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PUBLIC API: qtsq_is_encrypted
 * ══════════════════════════════════════════════════════════════ */

int qtsq_is_encrypted(const qtsq_context_t *ctx) {
    if (!ctx) return 0;
    return ctx->is_encrypted;
}
