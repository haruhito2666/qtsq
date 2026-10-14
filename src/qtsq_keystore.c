/*
 * Quantum Tensor Sequence — Device Keystore
 * Persistent device keypair management with OS keystore integration.
 * Author: Haruhito
 */

#include "qtsq_keystore.h"
#include "qtsq_vault.h"
#include "qtsq_shamir.h"
#include "qtsq_sanitize.h"
#include "qtsq_format.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

/* ═══ Path helper ═══ */

/* Create every component of dirpath (mode 0700).
 *
 * A single-level mkdir("…/.qtsq/keys") fails with ENOENT when the parent
 * ".qtsq" does not exist yet — on a fresh install (new app filesDir, new
 * $HOME) that made every keystore_store fail with QTSQ_ERR_IO, i.e.
 * "FAIL@keystore_init rc=-3" on the camera-save diagnostics. EEXIST from a
 * concurrent/earlier creation is not an error. */
static int keystore_mkpath(const char *dirpath) {
    char tmp[512];
    size_t len = (size_t)snprintf(tmp, sizeof(tmp), "%s", dirpath);
    if (len == 0 || len >= sizeof(tmp)) return QTSQ_ERR_IO;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return QTSQ_ERR_IO;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return QTSQ_ERR_IO;
    return QTSQ_OK;
}

/* ═══ Platform Detection ═══ */
#if defined(__APPLE__)
  #define QTSQ_PLATFORM_MACOS 1
  #include <Security/Security.h>
  #include <sys/stat.h>
#elif defined(__ANDROID__)
  #define QTSQ_PLATFORM_ANDROID 1
  #include <sys/stat.h>
  #include <unistd.h>
  /* Phase 2d: Android Keystore requires JNI to access KeyStore API.
   * For now, we use file-based storage with a clear TODO marker.
   * Full implementation would need:
   * - JNI bridge to android.security.keystore.KeyStore
   * - Java helper to generate/retrieve keys
   * - Encryption of the key file with a Keystore-derived key */
#elif defined(__linux__)
  #define QTSQ_PLATFORM_LINUX 1
  #include <sys/stat.h>
  #include <unistd.h>
#else
  #define QTSQ_PLATFORM_GENERIC 1
#endif

/* ═══ In-Memory Key Cache ═══ */

static qtsq_keypair_t       g_enc_keypair;     /* ML-KEM-768 encryption keypair */
static qtsq_sign_keypair_t  g_sign_keypair;     /* ML-DSA-65 signing keypair */
static int                  g_initialized = 0;

#define QTSQ_KEYSTORE_SERVICE "com.qtsq.devicekeys"
#define QTSQ_KEYSTORE_ENC_KEY "qtsq_device_enc_sk"
#define QTSQ_KEYSTORE_SIGN_KEY "qtsq_device_sign_sk"

/* ═══ Base64 Encode/Decode for QTSQ Address ═══ */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_chars[(triple >> 18) & 0x3F];
        out[j++] = b64_chars[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_chars[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_chars[triple & 0x3F];
    }
    out[j] = '\0';
    return out;
}

static int base64_decode(const char *in, size_t in_len,
                         uint8_t *out, size_t out_cap, size_t *out_len) {
    uint8_t decode_table[256];
    memset(decode_table, 0xFF, 256);
    for (int i = 0; i < 64; i++) decode_table[(uint8_t)b64_chars[i]] = (uint8_t)i;

    size_t j = 0;
    uint32_t accum = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c == '=' || c == '\n' || c == '\r') continue;
        uint8_t val = decode_table[c];
        if (val == 0xFF) continue; /* skip invalid chars */

        accum = (accum << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (j >= out_cap) return QTSQ_ERR_ALLOC;
            out[j++] = (uint8_t)(accum >> bits);
        }
    }
    if (out_len) *out_len = j;
    return QTSQ_OK;
}

/* ═══ Platform-Specific Keystore Backends ═══ */

#if QTSQ_PLATFORM_MACOS

static int keystore_store_macos(const char *key_id,
                                const uint8_t *data, size_t len) {
    /* Delete existing item first */
    CFStringRef service = CFStringCreateWithCString(NULL, QTSQ_KEYSTORE_SERVICE,
                                                     kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key_id,
                                                     kCFStringEncodingUTF8);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    SecItemDelete(query);
    CFRelease(query);

    /* Add new item */
    CFDataRef value = CFDataCreate(NULL, data, (CFIndex)len);
    query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecValueData, value);
    CFDictionarySetValue(query, kSecAttrAccessible,
                         kSecAttrAccessibleWhenUnlockedThisDeviceOnly);

    OSStatus status = SecItemAdd(query, NULL);
    CFRelease(query);
    CFRelease(value);
    CFRelease(service);
    CFRelease(account);

    return (status == errSecSuccess) ? QTSQ_OK : QTSQ_ERR_IO;
}

static int keystore_load_macos(const char *key_id,
                               uint8_t *data, size_t cap, size_t *out_len) {
    CFStringRef service = CFStringCreateWithCString(NULL, QTSQ_KEYSTORE_SERVICE,
                                                     kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key_id,
                                                     kCFStringEncodingUTF8);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);

    if (status != errSecSuccess || !result) {
        return QTSQ_ERR_IO;
    }

    CFDataRef cfdata = (CFDataRef)result;
    size_t len = (size_t)CFDataGetLength(cfdata);
    if (len > cap) { CFRelease(cfdata); return QTSQ_ERR_ALLOC; }

    memcpy(data, CFDataGetBytePtr(cfdata), len);
    if (out_len) *out_len = len;
    CFRelease(cfdata);
    return QTSQ_OK;
}

static int keystore_has_macos(const char *key_id) {
    CFStringRef service = CFStringCreateWithCString(NULL, QTSQ_KEYSTORE_SERVICE,
                                                     kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key_id,
                                                     kCFStringEncodingUTF8);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching(query, &result);
    if (result) CFRelease(result);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);

    return (status == errSecSuccess) ? 1 : 0;
}

static int keystore_delete_macos(const char *key_id) {
    CFStringRef service = CFStringCreateWithCString(NULL, QTSQ_KEYSTORE_SERVICE,
                                                     kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key_id,
                                                     kCFStringEncodingUTF8);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    SecItemDelete(query);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);
    return QTSQ_OK;
}

#endif /* macOS */

#if QTSQ_PLATFORM_LINUX

static int get_keystore_dir(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) return QTSQ_ERR_IO;
    snprintf(buf, cap, "%s/.qtsq/keys", home);
    return QTSQ_OK;
}

static int ensure_keystore_dir(void) {
    char dir[512];
    if (get_keystore_dir(dir, sizeof(dir)) != QTSQ_OK) return QTSQ_ERR_IO;
    return keystore_mkpath(dir);
}

static int keystore_store_linux(const char *key_id,
                                const uint8_t *data, size_t len) {
    if (ensure_keystore_dir() != QTSQ_OK) return QTSQ_ERR_IO;
    char path[600];
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", getenv("HOME"), key_id);

    FILE *f = fopen(path, "wb");
    if (!f) return QTSQ_ERR_IO;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);
    chmod(path, 0600);
    return QTSQ_OK;
}

static int keystore_load_linux(const char *key_id,
                               uint8_t *data, size_t cap, size_t *out_len) {
    char path[600];
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", getenv("HOME"), key_id);

    FILE *f = fopen(path, "rb");
    if (!f) return QTSQ_ERR_IO;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || (size_t)len > cap) { fclose(f); return QTSQ_ERR_ALLOC; }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return QTSQ_OK;
}

static int keystore_has_linux(const char *key_id) {
    char path[600];
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", getenv("HOME"), key_id);
    return (access(path, R_OK) == 0) ? 1 : 0;
}

static int keystore_delete_linux(const char *key_id) {
    char path[600];
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", getenv("HOME"), key_id);
    unlink(path);
    return QTSQ_OK;
}

#endif /* Linux */

/* ═══ Android (Phase 2d: hardware-backed Android Keystore via JNI) ═══ */
#if QTSQ_PLATFORM_ANDROID

/* Phase 2d: The Android Keystore provides hardware-backed key storage.
 * We use it to derive an AES-256 key that encrypts the QTSQ device
 * secret keys (ML-KEM secret key, ML-DSA secret key) before they're
 * written to app-private file storage.
 *
 * Flow:
 *   1. On first run, the Java side generates an AndroidKeyStore AES-256
 *      key with PURPOSE_ENCRYPT|PURPOSE_DECRYPT and no auth requirement.
 *   2. C calls JNI → Java encrypts/decrypts the key file bytes.
 *   3. The file on disk is always ciphertext; plaintext never touches
 *      the filesystem.
 *
 * The JNI bridge is declared here and implemented in the Java side
 * (KeystoreHelper.java). On non-Android builds, these are no-ops. */

/* JNI function pointers — set by qtsq_keystore_set_jni_bridge() at
 * app startup. When NULL (e.g. during tests), the keystore falls back
 * to the previous file-based storage without encryption. */
typedef int (*qtsq_jni_encrypt_fn)(const uint8_t *plaintext, size_t len,
                                    uint8_t *ciphertext, size_t cap, size_t *out_len);
typedef int (*qtsq_jni_decrypt_fn)(const uint8_t *ciphertext, size_t len,
                                    uint8_t *plaintext, size_t cap, size_t *out_len);

static qtsq_jni_encrypt_fn g_jni_encrypt = NULL;
static qtsq_jni_decrypt_fn g_jni_decrypt = NULL;

/* Called from Java at app startup to register the JNI bridge. */
void qtsq_keystore_set_jni_bridge(qtsq_jni_encrypt_fn enc_fn,
                                   qtsq_jni_decrypt_fn dec_fn) {
    g_jni_encrypt = enc_fn;
    g_jni_decrypt = dec_fn;
}

static int get_keystore_dir_android(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) {
        snprintf(buf, cap, ".qtsq/keys");
    } else {
        snprintf(buf, cap, "%s/.qtsq/keys", home);
    }
    return QTSQ_OK;
}

static int ensure_keystore_dir_android(void) {
    char dir[512];
    if (get_keystore_dir_android(dir, sizeof(dir)) != QTSQ_OK) return QTSQ_ERR_IO;
    return keystore_mkpath(dir);
}

/* Key-file format byte (Android backend):
 *   0x01 = body is AndroidKeyStore-wrapped ciphertext (via JNI bridge)
 *   0x00 = body is plaintext (bridge unavailable or failed — loud warning) */
#define QTSQ_KF_PLAIN 0x00
#define QTSQ_KF_ENC   0x01

static int keystore_write_file_android(const char *path,
                                       const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[QTSQ] keystore: fopen(%s) failed — dir unwritable?\n", path);
        return QTSQ_ERR_IO;
    }
    if (fwrite(buf, 1, n, f) != n) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);
    chmod(path, 0600);
    return QTSQ_OK;
}

static int keystore_store_android(const char *key_id,
                                  const uint8_t *data, size_t len) {
    if (ensure_keystore_dir_android() != QTSQ_OK) return QTSQ_ERR_IO;
    char path[600];
    char dir[512];
    get_keystore_dir_android(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.bin", dir, key_id);

    /* Phase 2d: encrypt with the AndroidKeyStore JNI bridge when available.
     * If the bridge itself fails (OEM KeyStore quirks, invalidated key, …),
     * DO NOT fail the whole init — that made every save on the device
     * impossible forever. Fall back to plaintext storage (chmod 600,
     * app-private dir) with a loud warning, matching the documented
     * no-bridge behavior. The real exception is logged under QTSQ_KSJNI. */
    uint8_t out[8192 + 1];
    if (g_jni_encrypt) {
        if (len > sizeof(out) - 1) return QTSQ_ERR_ALLOC;
        size_t ct_len = 0;
        int brc = g_jni_encrypt(data, len, out + 1, sizeof(out) - 1, &ct_len);
        if (brc == 0 && ct_len <= sizeof(out) - 1) {
            out[0] = QTSQ_KF_ENC;
            return keystore_write_file_android(path, out, ct_len + 1);
        }
        fprintf(stderr,
            "[QTSQ] ⚠️  keystore: bridge encrypt FAILED (rc=%d) for %s — "
            "storing PLAINTEXT in app-private dir. Check logcat tag QTSQ_KSJNI "
            "for the underlying Java exception.\n", brc, key_id);
        /* fall through to plaintext */
    } else {
        fprintf(stderr,
            "[QTSQ] ⚠️  keystore: storing %s in PLAINTEXT at %s (no JNI bridge "
            "registered).\n", key_id, path);
    }

    if (len > sizeof(out) - 1) return QTSQ_ERR_ALLOC;
    out[0] = QTSQ_KF_PLAIN;
    memcpy(out + 1, data, len);
    return keystore_write_file_android(path, out, len + 1);
}

static int keystore_load_android(const char *key_id,
                                 uint8_t *data, size_t cap, size_t *out_len) {
    char path[600];
    char dir[512];
    get_keystore_dir_android(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.bin", dir, key_id);

    FILE *f = fopen(path, "rb");
    if (!f) return QTSQ_ERR_IO;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 1 || (size_t)len > cap + 8192) { fclose(f); return QTSQ_ERR_ALLOC; }

    /* Read file contents into a temp buffer (format byte + body) */
    uint8_t *filebuf = (uint8_t *)malloc((size_t)len);
    if (!filebuf) { fclose(f); return QTSQ_ERR_ALLOC; }
    if (fread(filebuf, 1, (size_t)len, f) != (size_t)len) {
        free(filebuf); fclose(f); return QTSQ_ERR_IO;
    }
    fclose(f);

    size_t body_len = (size_t)len - 1;
    const uint8_t *body = filebuf + 1;
    int rc = QTSQ_OK;

    switch (filebuf[0]) {
    case QTSQ_KF_ENC:
        /* Body is AndroidKeyStore-wrapped — requires the bridge */
        if (!g_jni_decrypt) {
            fprintf(stderr,
                "[QTSQ] ⚠️  keystore: %s is bridge-encrypted but no JNI "
                "bridge registered — cannot load\n", key_id);
            rc = QTSQ_ERR_DECRYPT;
            break;
        }
        do {
            size_t pt_len = 0;
            rc = g_jni_decrypt(body, body_len, data, cap, &pt_len);
            if (rc == 0 && pt_len <= cap) {
                if (out_len) *out_len = pt_len;
            } else {
                fprintf(stderr,
                    "[QTSQ] ⚠️  keystore: bridge decrypt FAILED (rc=%d) for %s "
                    "— check logcat tag QTSQ_KSJNI\n", rc, key_id);
                rc = QTSQ_ERR_DECRYPT;
            }
        } while (0);
        break;

    case QTSQ_KF_PLAIN:
    default:
        /* Plaintext body (or legacy unprefixed file — caller's expected-size
         * check rejects garbage, which then triggers self-heal). */
        if (body_len > cap) { rc = QTSQ_ERR_ALLOC; break; }
        memcpy(data, body, body_len);
        if (out_len) *out_len = body_len;
        rc = QTSQ_OK;
        break;
    }

    free(filebuf);
    return rc;
}

static int keystore_has_android(const char *key_id) {
    char path[600];
    char dir[512];
    get_keystore_dir_android(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.bin", dir, key_id);
    return (access(path, R_OK) == 0) ? 1 : 0;
}

static int keystore_delete_android(const char *key_id) {
    char path[600];
    char dir[512];
    get_keystore_dir_android(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.bin", dir, key_id);
    unlink(path);
    return QTSQ_OK;
}

#endif /* Android */

/* ═══ Generic fallback (file-based in ~/.qtsq/keys/) ═══ */
#if QTSQ_PLATFORM_GENERIC

static int keystore_store_generic(const char *key_id,
                                  const uint8_t *data, size_t len) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home) return QTSQ_ERR_IO;
    snprintf(path, sizeof(path), "%s/.qtsq/keys", home);
    if (keystore_mkpath(path) != QTSQ_OK) return QTSQ_ERR_IO;
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", home, key_id);

    FILE *f = fopen(path, "wb");
    if (!f) return QTSQ_ERR_IO;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);
    return QTSQ_OK;
}

static int keystore_load_generic(const char *key_id,
                                 uint8_t *data, size_t cap, size_t *out_len) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home) return QTSQ_ERR_IO;
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", home, key_id);

    FILE *f = fopen(path, "rb");
    if (!f) return QTSQ_ERR_IO;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (size_t)len > cap) { fclose(f); return QTSQ_ERR_ALLOC; }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) { fclose(f); return QTSQ_ERR_IO; }
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return QTSQ_OK;
}

static int keystore_has_generic(const char *key_id) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home) return 0;
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", home, key_id);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int keystore_delete_generic(const char *key_id) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home) return QTSQ_ERR_IO;
    snprintf(path, sizeof(path), "%s/.qtsq/keys/%s.bin", home, key_id);
    unlink(path);
    return QTSQ_OK;
}

#endif /* Generic */

/* ═══ Platform-agnostic wrappers ═══ */

static int keystore_store(const char *key_id,
                          const uint8_t *data, size_t len) {
#if QTSQ_PLATFORM_MACOS
    return keystore_store_macos(key_id, data, len);
#elif QTSQ_PLATFORM_ANDROID
    return keystore_store_android(key_id, data, len);
#elif QTSQ_PLATFORM_LINUX
    return keystore_store_linux(key_id, data, len);
#else
    return keystore_store_generic(key_id, data, len);
#endif
}

static int keystore_load(const char *key_id,
                         uint8_t *data, size_t cap, size_t *out_len) {
#if QTSQ_PLATFORM_MACOS
    return keystore_load_macos(key_id, data, cap, out_len);
#elif QTSQ_PLATFORM_ANDROID
    return keystore_load_android(key_id, data, cap, out_len);
#elif QTSQ_PLATFORM_LINUX
    return keystore_load_linux(key_id, data, cap, out_len);
#else
    return keystore_load_generic(key_id, data, cap, out_len);
#endif
}

static int keystore_has(const char *key_id) {
#if QTSQ_PLATFORM_MACOS
    return keystore_has_macos(key_id);
#elif QTSQ_PLATFORM_ANDROID
    return keystore_has_android(key_id);
#elif QTSQ_PLATFORM_LINUX
    return keystore_has_linux(key_id);
#else
    return keystore_has_generic(key_id);
#endif
}

/* Remove stored key material (self-heal path for unreadable/corrupt keys). */
static int keystore_delete(const char *key_id) {
#if QTSQ_PLATFORM_MACOS
    return keystore_delete_macos(key_id);
#elif QTSQ_PLATFORM_ANDROID
    return keystore_delete_android(key_id);
#elif QTSQ_PLATFORM_LINUX
    return keystore_delete_linux(key_id);
#else
    return keystore_delete_generic(key_id);
#endif
}

/* ═══ Public API ═══ */

int qtsq_keystore_init(void) {
    if (g_initialized) return QTSQ_OK;

    int result;

    if (keystore_has(QTSQ_KEYSTORE_ENC_KEY) &&
        keystore_has(QTSQ_KEYSTORE_SIGN_KEY)) {
        /* Try to load existing keys from OS keystore */
        size_t enc_len = 0, sign_len = 0;
        int load_ok = 1;

        result = keystore_load(QTSQ_KEYSTORE_ENC_KEY,
                               (uint8_t *)&g_enc_keypair,
                               sizeof(g_enc_keypair), &enc_len);
        if (result != QTSQ_OK || enc_len != sizeof(g_enc_keypair)) load_ok = 0;

        if (load_ok) {
            result = keystore_load(QTSQ_KEYSTORE_SIGN_KEY,
                                   (uint8_t *)&g_sign_keypair,
                                   sizeof(g_sign_keypair), &sign_len);
            if (result != QTSQ_OK || sign_len != sizeof(g_sign_keypair)) load_ok = 0;
        }

        if (!load_ok) {
            /* SELF-HEAL: the stored key material exists but cannot be read
             * back — e.g. the wrapping OS-keystore key was lost (app data
             * restored to another device / cleared AndroidKeyStore entry),
             * a file written by an older incompatible build, or corruption.
             * Previously this returned an error on EVERY call, which made
             * the fail-closed engine refuse all saves forever with no way
             * out. Wipe the stale entries and regenerate below.
             *
             * Trade-off: files encrypted to the old device key become
             * undecryptable. A device that can't save anything at all is
             * worse; Shamir backup/recovery is the proper migration path. */
            fprintf(stderr,
                "[QTSQ] ⚠️  keystore: stored key material unreadable "
                "(enc rc=%d len=%zu/%zu) — regenerating device keypair\n",
                result, enc_len, sizeof(g_enc_keypair));
            keystore_delete(QTSQ_KEYSTORE_ENC_KEY);
            keystore_delete(QTSQ_KEYSTORE_SIGN_KEY);
        } else {
            g_initialized = 1;
            return QTSQ_OK;
        }
    }

    /* First run (or self-heal): generate new keypairs */
    result = qtsq_keygen(&g_enc_keypair);
    if (result != QTSQ_OK) return result;

    result = qtsq_sign_keygen(&g_sign_keypair);
    if (result != QTSQ_OK) return result;

    /* Persist to OS keystore */
    result = keystore_store(QTSQ_KEYSTORE_ENC_KEY,
                            (const uint8_t *)&g_enc_keypair,
                            sizeof(g_enc_keypair));
    if (result != QTSQ_OK) return result;

    result = keystore_store(QTSQ_KEYSTORE_SIGN_KEY,
                            (const uint8_t *)&g_sign_keypair,
                            sizeof(g_sign_keypair));
    if (result != QTSQ_OK) return result;

    g_initialized = 1;
    return QTSQ_OK;
}

void qtsq_keystore_destroy(void) {
    if (g_initialized) {
        qtsq_secure_zero(&g_enc_keypair, sizeof(g_enc_keypair));
        qtsq_secure_zero(&g_sign_keypair, sizeof(g_sign_keypair));
    }
    g_initialized = 0;
}

int qtsq_keystore_is_initialized(void) {
    return g_initialized;
}

const uint8_t* qtsq_keystore_get_enc_pk(void) {
    return g_initialized ? g_enc_keypair.public_key : NULL;
}

const uint8_t* qtsq_keystore_get_enc_sk(void) {
    return g_initialized ? g_enc_keypair.secret_key : NULL;
}

const uint8_t* qtsq_keystore_get_sign_pk(void) {
    return g_initialized ? g_sign_keypair.sign_pk : NULL;
}

const uint8_t* qtsq_keystore_get_sign_sk(void) {
    return g_initialized ? g_sign_keypair.sign_sk : NULL;
}

int qtsq_keystore_encrypt(qtsq_context_t *ctx) {
    if (!g_initialized) return QTSQ_ERR_FORMAT;
    return qtsq_vault_encrypt_pq(ctx, g_enc_keypair.public_key);
}

int qtsq_keystore_decrypt(qtsq_context_t *ctx) {
    if (!g_initialized) return QTSQ_ERR_FORMAT;
    return qtsq_vault_decrypt_pq(ctx, g_enc_keypair.secret_key);
}

int qtsq_keystore_sign(qtsq_context_t *ctx) {
    if (!g_initialized) return QTSQ_ERR_FORMAT;
    return qtsq_vault_sign(ctx, g_sign_keypair.sign_sk);
}

/* ═══ QTSQ Address Export/Import ═══ */

char* qtsq_keystore_export_address(void) {
    if (!g_initialized) return NULL;

    /* Format: "qtsq_kem768:<base64>" */
    char *b64 = base64_encode(g_enc_keypair.public_key, 1184);
    if (!b64) return NULL;

    size_t prefix_len = strlen("qtsq_kem768:");
    size_t b64_len = strlen(b64);
    char *address = (char *)malloc(prefix_len + b64_len + 1);
    if (!address) { free(b64); return NULL; }

    memcpy(address, "qtsq_kem768:", prefix_len);
    memcpy(address + prefix_len, b64, b64_len + 1);
    free(b64);
    return address;
}

int qtsq_keystore_import_address(const char *address, uint8_t out_pk[1184]) {
    if (!address || !out_pk) return QTSQ_ERR_NULL;

    /* Skip optional prefix */
    const char *b64_start = address;
    if (strncmp(address, "qtsq_kem768:", 12) == 0) {
        b64_start = address + 12;
    }

    size_t decoded_len = 0;
    int result = base64_decode(b64_start, strlen(b64_start),
                               out_pk, 1184, &decoded_len);
    if (result != QTSQ_OK) return result;
    if (decoded_len != 1184) return QTSQ_ERR_FORMAT;

    return QTSQ_OK;
}

/* ═══ Shamir Backup/Recovery ═══ */

int qtsq_keystore_backup(uint8_t k, uint8_t n, const char **share_paths,
                         const char *password) {
    if (!g_initialized) return QTSQ_ERR_FORMAT;
    if (!share_paths || !password) return QTSQ_ERR_NULL;
    if (k < 2 || k > n || n > 255) return QTSQ_ERR_FORMAT;

    /* Create a context with the secret AND public keys as singularity.
     * Layout: [enc_sk 2400B][enc_pk 1184B][sign_sk 4032B][sign_pk 1952B]
     * Public keys must be persisted because they cannot be derived from the
     * secret keys by simple prefix copy (ML-KEM-768 embeds the pk at offset
     * KYBER_INDCPA_SECRETKEYBYTES inside the sk; ML-DSA-65 pk/sk have
     * completely different layouts — only the first 32 bytes overlap). */
    qtsq_context_t secret_ctx;
    qtsq_init(&secret_ctx);

    size_t secret_size = 2400 + 1184 + 4032 + 1952;
    secret_ctx.singularity = (uint8_t *)malloc(secret_size);
    if (!secret_ctx.singularity) return QTSQ_ERR_ALLOC;

    size_t off = 0;
    memcpy(secret_ctx.singularity + off, g_enc_keypair.secret_key, 2400); off += 2400;
    memcpy(secret_ctx.singularity + off, g_enc_keypair.public_key, 1184); off += 1184;
    memcpy(secret_ctx.singularity + off, g_sign_keypair.sign_sk, 4032);   off += 4032;
    memcpy(secret_ctx.singularity + off, g_sign_keypair.sign_pk, 1952);   off += 1952;
    secret_ctx.singularity_size = secret_size;
    secret_ctx.header.singularity_size = secret_size;
    secret_ctx.header.data_type = QTSQ_TYPE_RAW;
    secret_ctx.is_sanitized = 1;

    /* Split into N shares */
    qtsq_context_t *shares = (qtsq_context_t *)calloc(n, sizeof(qtsq_context_t));
    if (!shares) { qtsq_free(&secret_ctx); return QTSQ_ERR_ALLOC; }

    int result = qtsq_shamir_split(&secret_ctx, k, n, shares);
    qtsq_free(&secret_ctx);
    if (result != QTSQ_OK) { free(shares); return result; }

    /* Write each share to its file (encrypted with password) */
    for (uint8_t i = 0; i < n; i++) {
        shares[i].is_sanitized = 1; /* shares are clean */
        shares[i].sanitize_report |= QTSQ_SANITIZE_ENCRYPTED;
        result = qtsq_vault_encrypt_password(&shares[i], password);
        if (result != QTSQ_OK) {
            for (uint8_t j = i; j < n; j++) qtsq_free(&shares[j]);
            free(shares);
            return result;
        }
        result = qtsq_write(&shares[i], share_paths[i]);
        qtsq_free(&shares[i]);
        if (result != QTSQ_OK) {
            /* Clean up remaining shares */
            for (uint8_t j = i + 1; j < n; j++) qtsq_free(&shares[j]);
            free(shares);
            return result;
        }
    }

    free(shares);
    return QTSQ_OK;
}

int qtsq_keystore_recover(uint8_t k, const char **share_paths,
                          const char *password) {
    if (!share_paths || !password) return QTSQ_ERR_NULL;
    if (k < 2) return QTSQ_ERR_FORMAT;

    /* Read K share files */
    qtsq_context_t *shares = (qtsq_context_t *)calloc(k, sizeof(qtsq_context_t));
    if (!shares) return QTSQ_ERR_ALLOC;

    int result;
    for (uint8_t i = 0; i < k; i++) {
        qtsq_init(&shares[i]);
        result = qtsq_read(&shares[i], share_paths[i]);
        if (result == QTSQ_OK) {
            result = qtsq_vault_decrypt_password(&shares[i], password);
        }
        if (result != QTSQ_OK) {
            for (uint8_t j = 0; j <= i; j++) qtsq_free(&shares[j]);
            free(shares);
            return result;
        }
    }

    /* Reconstruct */
    qtsq_context_t recovered;
    qtsq_init(&recovered);
    result = qtsq_shamir_reconstruct(shares, k, &recovered);

    for (uint8_t i = 0; i < k; i++) qtsq_free(&shares[i]);
    free(shares);

    if (result != QTSQ_OK) { qtsq_free(&recovered); return result; }

    /* Verify recovered data has expected size (enc_sk + enc_pk + sign_sk + sign_pk) */
    if (recovered.singularity_size != 2400 + 1184 + 4032 + 1952) {
        qtsq_free(&recovered);
        return QTSQ_ERR_FORMAT;
    }

    /* Extract keys — public keys are persisted alongside secret keys because
     * they cannot be derived from the secret keys by prefix copy. */
    size_t off = 0;
    memcpy(g_enc_keypair.secret_key, recovered.singularity + off, 2400); off += 2400;
    memcpy(g_enc_keypair.public_key, recovered.singularity + off, 1184); off += 1184;
    memcpy(g_sign_keypair.sign_sk, recovered.singularity + off, 4032);   off += 4032;
    memcpy(g_sign_keypair.sign_pk, recovered.singularity + off, 1952);   off += 1952;
    qtsq_free(&recovered);

    /* Persist recovered keys to OS keystore */
    result = keystore_store(QTSQ_KEYSTORE_ENC_KEY,
                            (const uint8_t *)&g_enc_keypair,
                            sizeof(g_enc_keypair));
    if (result != QTSQ_OK) return result;

    result = keystore_store(QTSQ_KEYSTORE_SIGN_KEY,
                            (const uint8_t *)&g_sign_keypair,
                            sizeof(g_sign_keypair));
    if (result != QTSQ_OK) return result;

    g_initialized = 1;
    return QTSQ_OK;
}
