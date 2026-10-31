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
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/random.h>             /* getrandom(), 3.17+ */
#endif
#include "qtsq.h"
#include "qtsq_sanitize.h"
#include "../crypto/common/aes.h"   /* PQClean constant-time AES-256-CTR */

/* M-6: aes256_ctx_release() free()s the expanded key schedule WITHOUT
 * wiping it, and zeroing the context struct afterwards only clears a
 * dangling pointer. Wipe the schedule first, then release. */
static void aes256_release_wipe(aes256ctx *r) {
    if (r->sk_exp)
        qtsq_secure_zero(r->sk_exp, sizeof(uint64_t) * PQC_AES256_STATESIZE);
    aes256_ctx_release(r);
    qtsq_secure_zero(r, sizeof(*r));
}
#include "../crypto/argon2/argon2.h"

/* ── CTR mode (Counter) for streaming encryption ──
 *
 * FIX (#8): the previous implementation used a hand-rolled AES with a
 * data-dependent S-Box lookup table — vulnerable to cache-timing side
 * channels. We now delegate to the vendored PQClean aes256_ctr, which is
 * constant-time and table-free. The local function keeps the same
 * (in, out, size, key, iv) signature the callers already use. */
static int aes256_ctr_local(const uint8_t *in, uint8_t *out, size_t size,
                            const uint8_t key[32], const uint8_t iv[16]) {
    aes256ctx aes;
    aes256_ctr_keyexp(&aes, key);

    /* PQClean aes256_ctr emits the keystream; the caller XORs it with the
     * input. Generate the keystream and XOR in-place. */
    uint8_t *keystream = (uint8_t *)malloc(size);
    if (!keystream) {
        /* M-2 sibling: returning "success" with `out` untouched would make
         * encrypt() store uninitialised bytes (it copies from `out`) under
         * a QTSQ_OK. Propagate instead. */
        aes256_release_wipe(&aes);
        return QTSQ_ERR_ALLOC;
    }
    aes256_ctr(keystream, size, iv, &aes);
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i] ^ keystream[i];
    }

    qtsq_secure_zero(keystream, size);
    free(keystream);
    aes256_release_wipe(&aes);
    return QTSQ_OK;
}

/* ── Password key derivation ──
 *
 * New files use Argon2id (H-8 fix). The `iterations` field used to carry a
 * raw loop count for a home-made iterated-SHA256 KDF whose 256-byte buffer
 * silently truncated passwords at 240 bytes, so two distinct long
 * passwords derived the same key. It is now a KDF selector: files written
 * with the selector value below use Argon2id over the full password; any
 * other value is a legacy iteration count and decodes through the old KDF
 * so pre-fix files keep working. The selector is deliberately small: an
 * old reader that deserialises a new file runs the legacy loop only a
 * couple of times and then fails the auth tag cleanly, instead of
 * grinding through billions of SHA-256 rounds. */
#define QTSQ_CENSORSHIP_KDF_ARGON2 1

static void derive_key_legacy(const char *password, const uint8_t salt[16],
                              uint32_t iterations, uint8_t key[32]) {
    /* Iterative hash: key = SHA256(SHA256(...SHA256(password + salt))) */
    uint8_t buf[256];
    size_t pw_len = strlen(password);
    /* Clamp pw_len so the salt write stays in-bounds: we need pw_len + 16
     * ≤ sizeof(buf). Without this, a password ≥ 241 bytes overflows buf.
     * (The truncation is a defect in this legacy KDF itself; new files no
     * longer reach this code.) */
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

static int derive_key(const char *password, const uint8_t salt[16],
                      uint32_t iterations, uint8_t key[32]) {
    if (iterations == QTSQ_CENSORSHIP_KDF_ARGON2) {
        int ret = argon2id_hash_raw(3, 65536, 4,
                                    password, strlen(password),
                                    salt, 16,
                                    key, 32);
        if (ret != ARGON2_OK) {
            /* Never fall back to a predictable key on allocation failure:
             * the caller would seal data under a key anyone can derive. */
            memset(key, 0, 32);
            return QTSQ_ERR_ALLOC;
        }
        return QTSQ_OK;
    }
    derive_key_legacy(password, salt, iterations, key);
    return QTSQ_OK;
}

/* ── Cryptographically secure random bytes ── */

static void generate_random_bytes(uint8_t *out, size_t len) {
#if defined(__APPLE__)
    /* arc4random_buf is seeded by the OS kernel — CSPRNG, no setup needed */
    arc4random_buf(out, len);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__unix__) || defined(__EMSCRIPTEN__)
    /* Prefer getrandom() on Linux/Android: no file descriptor, no path.
     * Bionic only exports getrandom() from API 28 — below that the
     * declaration is hidden from <sys/random.h> and the symbol does not
     * exist in libc, so Android < 28 falls through to /dev/urandom, which
     * the code below already handles (still a CSPRNG). */
    #if defined(__linux__) && (!defined(__ANDROID__) || (defined(__ANDROID_API__) && __ANDROID_API__ >= 28))
    size_t got = 0;
    while (got < len) {
        ssize_t r = getrandom(out + got, len - got, 0);
        if (r <= 0) break;  /* EINTR or unavailable -> try /dev/urandom */
        got += (size_t)r;
    }
    if (got == len) return;
    #endif
    /* /dev/urandom is the correct POSIX source.
     * On Emscripten, the C runtime maps this to crypto.getRandomValues(). */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t rd = fread(out, 1, len, f);
        fclose(f);
        if (rd == len) return;
    }
    /* H-7: this used to be a void function that simply fell off the end on
     * Android/Emscripten, leaving the IV and salt as uninitialised memory
     * that then encrypted the payload under QTSQ_OK. An RNG that cannot
     * produce randomness must not produce output at all. */
    abort();
#elif defined(_WIN32)
    #include <bcrypt.h>
    NTSTATUS st = BCryptGenRandom(NULL, out, (ULONG)len,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) abort();
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

static int compute_auth_tag(const uint8_t *ciphertext, size_t size,
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
    if (!inner_buf) {
        /* M-2: an all-zero tag plus QTSQ_OK would seal a file nothing can
         * ever decrypt (the written tag is part of the file). Propagate. */
        qtsq_secure_zero(tag, 16);
        return QTSQ_ERR_ALLOC;
    }
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
    return QTSQ_OK;
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
    ctx->censorship.iterations = QTSQ_CENSORSHIP_KDF_ARGON2;

    /* Derive AES-256 key from password */
    uint8_t key[32];
    int kdf = derive_key(password, ctx->censorship.salt,
                         ctx->censorship.iterations, key);
    if (kdf != QTSQ_OK) {
        qtsq_secure_zero(key, 32);
        return kdf;
    }

    /* Encrypt singularity data in-place using CTR mode */
    uint8_t *ciphertext = (uint8_t *)malloc(ctx->singularity_size);
    if (!ciphertext) { qtsq_secure_zero(key, 32); return QTSQ_ERR_ALLOC; }

    int ctr_rc = aes256_ctr_local(ctx->singularity, ciphertext,
                                  ctx->singularity_size,
                                  key, ctx->censorship.iv);
    if (ctr_rc != QTSQ_OK) {
        free(ciphertext);
        qtsq_secure_zero(key, 32);
        return ctr_rc;
    }

    /* Compute authentication tag */
    int tag_rc = compute_auth_tag(ciphertext, ctx->singularity_size, key,
                                  ctx->censorship.auth_tag);
    if (tag_rc != QTSQ_OK) {
        free(ciphertext);
        qtsq_secure_zero(key, 32);
        return tag_rc;
    }

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

    /* Derive key from password (Argon2id, or the legacy iterated
     * SHA-256 for files written before the H-8 fix) */
    uint8_t key[32];
    int kdf = derive_key(password, ctx->censorship.salt,
                         ctx->censorship.iterations, key);
    if (kdf != QTSQ_OK) {
        qtsq_secure_zero(key, 32);
        return kdf;
    }

    /* Verify authentication tag before decrypting.
     * CRYPT-2 FIX: use constant-time comparison — see ct_memcmp() above. */
    uint8_t computed_tag[16];
    if (compute_auth_tag(ctx->singularity, ctx->singularity_size,
                         key, computed_tag) != QTSQ_OK) {
        qtsq_secure_zero(key, 32);
        return QTSQ_ERR_ALLOC;
    }

    if (ct_memcmp(computed_tag, ctx->censorship.auth_tag, 16) != 0) {
        qtsq_secure_zero(key, 32);
        return QTSQ_ERR_DECRYPT; /* wrong password or tampered data */
    }

    /* Decrypt in-place using CTR mode (CTR is symmetric) */
    uint8_t *plaintext = (uint8_t *)malloc(ctx->singularity_size);
    if (!plaintext) { qtsq_secure_zero(key, 32); return QTSQ_ERR_ALLOC; }

    int ctr_rc = aes256_ctr_local(ctx->singularity, plaintext,
                                  ctx->singularity_size,
                                  key, ctx->censorship.iv);
    if (ctr_rc != QTSQ_OK) {
        free(plaintext);
        qtsq_secure_zero(key, 32);
        return ctr_rc;
    }

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
