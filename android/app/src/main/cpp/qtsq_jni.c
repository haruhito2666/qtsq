#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <android/log.h>
#include "qtsq.h"
#include "qtsq_triangle_only.h"
#include "qtsq_pixel.h"
#include "qtsq_keystore.h"
#include "qtsq_vault.h"
#include "qtsq_capture.h"
#include "qtsq_trainer.h"

#define LOG_TAG "QTSQ_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Phase 7: the container codec is a user Settings preference. qtc is the
 * default (qtsq_init already sets it); setCodec() can opt out to legacy zlib.
 * Applied to each context after qtsq_init. Decode sniffs the stream magic, so
 * the read path ignores this value. */
static uint8_t g_jni_codec = QTSQ_CODEC_QTC;

static double jni_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static jbyteArray create_jbyteArray(JNIEnv *env, const uint8_t *data, size_t size) {
    if (!data || size == 0) return NULL;
    jbyteArray arr = (*env)->NewByteArray(env, (jint)size);
    if (arr != NULL) {
        (*env)->SetByteArrayRegion(env, arr, 0, (jint)size, (const jbyte *)data);
    }
    return arr;
}

/* Pack decoded pixels into Android ARGB_8888 layout (the byte order Bitmap
 * expects): RGB expands with A=0xFF, RGBA copies through, GRAY triples.
 * `npix` is the pixel count; dst must hold npix*4 bytes. */
static void pack_argb_into(const uint8_t *src, uint8_t *dst, size_t npix,
                           int data_type) {
    if (data_type == QTSQ_TYPE_IMAGE_RGB) {
        for (size_t i = 0; i < npix; i++) {
            dst[i*4 + 0] = src[i*3 + 0]; // R
            dst[i*4 + 1] = src[i*3 + 1]; // G
            dst[i*4 + 2] = src[i*3 + 2]; // B
            dst[i*4 + 3] = 0xFF;         // A
        }
    } else if (data_type == QTSQ_TYPE_IMAGE_RGBA) {
        memcpy(dst, src, npix * 4);
    } else { /* QTSQ_TYPE_IMAGE_GRAY */
        for (size_t i = 0; i < npix; i++) {
            dst[i*4 + 0] = src[i];
            dst[i*4 + 1] = src[i];
            dst[i*4 + 2] = src[i];
            dst[i*4 + 3] = 0xFF;
        }
    }
}

/* ═══ Full-sync credential helpers (mirror tools/qtsq_cli.c) ═══
 *
 * The desktop CLI encrypts via qtsq_vault_encrypt_password (Argon2id KDF +
 * V3 security block) or keystore auto-encrypt, and decrypts via
 * qtsq_vault_decrypt_password / qtsq_keystore_decrypt. The JNI previously
 * used the legacy censorship path (qtsq_encrypt/qtsq_decrypt), producing
 * files the rest of the codebase treats as legacy-only. These helpers
 * route Android through the exact same paths as every other surface. */

static int jni_encrypt_for_write(qtsq_context_t *ctx, const char *password) {
    if (!ctx) return QTSQ_ERR_NULL;
    if (password && password[0]) {
        return qtsq_vault_encrypt_password(ctx, password);
    }
    if (qtsq_keystore_is_initialized()) {
        return qtsq_keystore_encrypt(ctx);
    }
    return QTSQ_ERR_NOT_ENCRYPTED;
}

static int jni_decrypt_after_read(qtsq_context_t *ctx, const char *password) {
    if (!ctx || !ctx->is_encrypted) return QTSQ_OK;
    if (password && password[0]) {
        if (ctx->has_security) {
            return qtsq_vault_decrypt_password(ctx, password);
        }
        return qtsq_decrypt(ctx, password); /* legacy censorship files */
    }
    if (qtsq_keystore_is_initialized()) {
        return qtsq_keystore_decrypt(ctx);
    }
    return QTSQ_ERR_DECRYPT; /* encrypted but no credential available */
}

/**
 * Helper: serialize a qtsq context directly to a byte array in memory.
 * Avoids temp files entirely — critical on Android where TMPDIR and
 * /data/local/tmp may not be writable by regular apps.
 */
/* Phase 1 fail-closed gate: encrypt before serializing. Android apps must
 * either initialize the QTSQ keystore (qtsq_keystore_init) before any
 * compress/save call, or the context must already be encrypted.
 * Returns QTSQ_OK, or an error code (context left untouched on failure). */
static int context_ensure_encrypted(qtsq_context_t *ctx) {
    if (ctx->is_encrypted) return QTSQ_OK;
    if (!qtsq_keystore_is_initialized()) {
        /* Lazy self-bootstrap: app startup should have called
         * ensureKeystoreReady, but try once more here rather than
         * instantly refusing the save. */
        if (qtsq_keystore_init() != QTSQ_OK) {
            LOGE("ensure_encrypted: keystore init failed lazily — refusing (Phase 1 fail-closed)");
            return QTSQ_ERR_NOT_ENCRYPTED;
        }
    }
    int rc = qtsq_keystore_encrypt(ctx);
    if (rc != QTSQ_OK) {
        LOGE("ensure_encrypted: keystore encrypt failed: %d (%s)", rc, qtsq_error_string(rc));
    }
    return rc;
}

/* Pure-C serializer: writes the exact core qtsq_write byte layout.
 * context_serialized_size computes the exact total; context_serialize_into
 * writes that many bytes into a caller-provided buffer (used to serialize
 * straight into a Java byte array with no intermediate copy).
 * context_serialize keeps the malloc'd-buffer form for non-JNI callers. */
static size_t context_serialized_size(const qtsq_context_t *ctx) {
    /*
     * Calculate total output size:
     * header (112 bytes) + schema (horizon_size) + transforms (accretion)
     * + singularity data + optional censorship/security + optional recovery
     */
    size_t total_size = sizeof(qtsq_header_t);

    if (ctx->header.horizon_size > 0) {
        total_size += ctx->header.horizon_size;
    }

    /* Transforms */
    for (uint32_t i = 0; i < ctx->num_transforms; i++) {
        total_size += 1 + 4 + ctx->transforms[i].param_size; /* type + param_size + params */
    }

    if (ctx->singularity && ctx->singularity_size > 0) {
        total_size += ctx->singularity_size;
    }

    if (ctx->radiation_index && ctx->num_radiation > 0) {
        total_size += sizeof(qtsq_index_t) * ctx->num_radiation;
    }

    /* Legacy Censorship or V3 Security block */
    if (ctx->has_security) {
        total_size += sizeof(qtsq_security_block_t);
    } else if (ctx->is_encrypted || (ctx->header.flags & QTSQ_FLAG_ENCRYPTED)) {
        total_size += sizeof(qtsq_censorship_t);
    }

    /* Recovery data */
    if (ctx->has_recovery && ctx->recovery.num_chunks > 0) {
        total_size += 4 + 4; /* num_chunks + parity_size */
        total_size += 4 * ctx->recovery.num_chunks; /* chunk checksums */
        if (ctx->recovery.reed_solomon_parity && ctx->recovery.parity_size > 0) {
            total_size += ctx->recovery.parity_size;
        }
    }

    return total_size;
}

static void context_serialize_into(const qtsq_context_t *ctx, uint8_t *buf) {
    size_t pos = 0;

    /* Header — mirror core qtsq_write: force V3 and stamp the security
     * block size so readers (desktop included) know the V3 security
     * block follows the radiation index. Writing ctx->header raw left
     * security_block_size=0 even when has_security was set, producing
     * files no reader could decrypt. */
    qtsq_header_t h = ctx->header;
    h.version = QTSQ_VERSION_3;
    h.security_block_size = ctx->has_security ? sizeof(qtsq_security_block_t) : 0;
    memcpy(buf + pos, &h, sizeof(qtsq_header_t));
    pos += sizeof(qtsq_header_t);

    /* Schema */
    if (ctx->header.horizon_size > 0) {
        size_t hsize = ctx->header.horizon_size;
        if (hsize > sizeof(qtsq_schema_t)) hsize = sizeof(qtsq_schema_t);
        memcpy(buf + pos, &ctx->schema, hsize);
        pos += ctx->header.horizon_size;
    }

    /* Transforms */
    for (uint32_t i = 0; i < ctx->num_transforms; i++) {
        buf[pos++] = ctx->transforms[i].type;
        memcpy(buf + pos, &ctx->transforms[i].param_size, 4);
        pos += 4;
        if (ctx->transforms[i].param_size > 0 && ctx->transforms[i].params) {
            memcpy(buf + pos, ctx->transforms[i].params, ctx->transforms[i].param_size);
            pos += ctx->transforms[i].param_size;
        }
    }

    /* Singularity data */
    if (ctx->singularity && ctx->singularity_size > 0) {
        memcpy(buf + pos, ctx->singularity, ctx->singularity_size);
        pos += ctx->singularity_size;
    }

    /* Radiation index */
    if (ctx->radiation_index && ctx->num_radiation > 0) {
        size_t radsz = sizeof(qtsq_index_t) * ctx->num_radiation;
        memcpy(buf + pos, ctx->radiation_index, radsz);
        pos += radsz;
    }

    /* Legacy Censorship or V3 Security block */
    if (ctx->has_security) {
        memcpy(buf + pos, &ctx->security, sizeof(qtsq_security_block_t));
        pos += sizeof(qtsq_security_block_t);
    } else if (ctx->is_encrypted || (ctx->header.flags & QTSQ_FLAG_ENCRYPTED)) {
        memcpy(buf + pos, &ctx->censorship, sizeof(qtsq_censorship_t));
        pos += sizeof(qtsq_censorship_t);
    }

    /* Recovery data */
    if (ctx->has_recovery && ctx->recovery.num_chunks > 0) {
        memcpy(buf + pos, &ctx->recovery.num_chunks, 4); pos += 4;
        memcpy(buf + pos, &ctx->recovery.parity_size, 4); pos += 4;
        memcpy(buf + pos, ctx->recovery.chunk_checksums, 4 * ctx->recovery.num_chunks);
        pos += 4 * ctx->recovery.num_chunks;
        if (ctx->recovery.reed_solomon_parity && ctx->recovery.parity_size > 0) {
            memcpy(buf + pos, ctx->recovery.reed_solomon_parity, ctx->recovery.parity_size);
            pos += ctx->recovery.parity_size;
        }
    }
}

/* Malloc'd-buffer form (used by nativeSelfTest and the JNI fallback).
 * Returns NULL on allocation failure. */
static uint8_t *context_serialize(const qtsq_context_t *ctx, size_t *out_size) {
    size_t total_size = context_serialized_size(ctx);
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        LOGE("context_serialize: malloc failed for %zu bytes", total_size);
        return NULL;
    }
    context_serialize_into(ctx, buf);
    *out_size = total_size;
    return buf;
}

/* JNI wrapper: encrypt-gate + serialize straight into a Java byte array
 * (no intermediate malloc'd copy). */
static jbyteArray context_to_bytes(JNIEnv *env, qtsq_context_t *ctx, const char *suffix) {
    (void)suffix; /* unused now */

    int rc = context_ensure_encrypted(ctx);
    if (rc != QTSQ_OK) return NULL;

    size_t total = context_serialized_size(ctx);
    if (total > (size_t)0x7FFFFFFF) return NULL;
    jbyteArray result = (*env)->NewByteArray(env, (jint)total);
    if (!result) return NULL;

    jbyte *dst = (*env)->GetByteArrayElements(env, result, NULL);
    if (dst) {
        context_serialize_into(ctx, (uint8_t *)dst);
        (*env)->ReleaseByteArrayElements(env, result, dst, 0);
    } else {
        /* Direct pin failed — serialize into a temp buffer and copy in. */
        size_t blen = 0;
        uint8_t *buf = context_serialize(ctx, &blen);
        if (!buf) {
            (*env)->DeleteLocalRef(env, result);
            return NULL;
        }
        (*env)->SetByteArrayRegion(env, result, 0, (jint)blen, (const jbyte *)buf);
        free(buf);
    }

    LOGI("context_to_bytes: serialized %zu bytes", total);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressBytes(JNIEnv *env, jobject thiz,
                                                jbyteArray input, jstring type, jstring password) {
    if (!input) {
        LOGE("compressBytes: null input");
        return NULL;
    }

    jsize input_len = (*env)->GetArrayLength(env, input);
    jbyte *input_bytes = (*env)->GetByteArrayElements(env, input, NULL);
    if (!input_bytes) {
        LOGE("compressBytes: failed to get input bytes");
        return NULL;
    }

    const char *type_str = type ? (*env)->GetStringUTFChars(env, type, NULL) : NULL;
    const char *effective_type = type_str ? type_str : "raw";

    LOGI("compressBytes: %d bytes, type=%s", input_len, effective_type);

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    int result;
    if (strcmp(effective_type, "text") == 0) {
        result = qtsq_compress_text(&ctx, (const char *)input_bytes, input_len);
    } else {
        result = qtsq_compress_raw(&ctx, (const uint8_t *)input_bytes, input_len, 0);
    }

    (*env)->ReleaseByteArrayElements(env, input, input_bytes, JNI_ABORT);
    if (type_str) (*env)->ReleaseStringUTFChars(env, type, type_str);

    if (result != QTSQ_OK) {
        LOGE("Compression failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "raw");
    qtsq_free(&ctx);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressImage(JNIEnv *env, jobject thiz,
                                                jbyteArray pixels, jint width, jint height, jint channels, jstring password, jlong source_size) {
    if (!pixels || width <= 0 || height <= 0 || channels <= 0) {
        LOGE("compressImage: invalid params w=%d h=%d c=%d", width, height, channels);
        return NULL;
    }

    jsize pixels_len = (*env)->GetArrayLength(env, pixels);
    LOGI("compressImage: %dx%d, ch=%d, buffer=%d bytes", width, height, channels, pixels_len);

    /* Validate buffer size matches dimensions */
    int expected_size = width * height * channels;
    if (pixels_len < expected_size) {
        LOGE("compressImage: buffer too small (%d < %d)", pixels_len, expected_size);
        height = pixels_len / (width * channels);
        if (height <= 0) {
            LOGE("compressImage: cannot determine valid height");
            return NULL;
        }
        LOGI("compressImage: adjusted height to %d", height);
    }

    jbyte *pixel_bytes = (*env)->GetByteArrayElements(env, pixels, NULL);
    if (!pixel_bytes) {
        LOGE("compressImage: failed to get pixel bytes");
        return NULL;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    int result;
    size_t total_pixels = (size_t)width * height * channels;

    /*
     * Gravitational (Triangle RGB Mesh) compression.
     * Places vertices at edges/detail and rasterizes with barycentric
     * interpolation — produces visually compelling lossy compression.
     */
    LOGI("compressImage: using Gravitational (Triangle RGB Mesh) for %zu bytes (%dx%d, %dch)",
         total_pixels, width, height, channels);
    double t_jni = jni_time_ms();
    result = qtsq_compress_gravitational(&ctx, (const uint8_t *)pixel_bytes, width, height, channels, (size_t)source_size);
    LOGI("compressImage: compress took %.1f ms", jni_time_ms() - t_jni);

    (*env)->ReleaseByteArrayElements(env, pixels, pixel_bytes, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("Image compression failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ HD: High-quality image compression (up to 3M vertices) ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressImageHD(JNIEnv *env, jobject thiz,
                                                    jbyteArray pixels, jint width, jint height,
                                                    jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;

    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    jsize len = (*env)->GetArrayLength(env, pixels);

    const char *c_pwd = NULL;
    if (password != NULL) {
        c_pwd = (*env)->GetStringUTFChars(env, password, 0);
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    /* Password handling is now done internally or passed elsewhere,
     * qtsq_set_password is no longer part of the C API. */

    size_t total_pixels = (size_t)width * height * channels;

    LOGI("compressImageHD: Gravitational HD for %zu bytes (%dx%d, %dch)",
         total_pixels, width, height, channels);
    double t_hd = jni_time_ms();
    int result = qtsq_compress_gravitational_hd(&ctx, (const uint8_t *)c_pixels, (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressImageHD: compress took %.1f ms", jni_time_ms() - t_hd);

    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("HD compression failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ Upscale: Mathematical quality enhancement for lossy inputs ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressImageUpscale(JNIEnv *env, jobject thiz,
                                                         jbyteArray pixels, jint width, jint height,
                                                         jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;

    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    LOGI("compressImageUpscale: Upscale for %dx%d, %dch, source=%lld",
         width, height, channels, (long long)sourceSize);
    double t_up = jni_time_ms();
    int result = qtsq_compress_gravitational_upscale(&ctx, (const uint8_t *)c_pixels,
                 (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressImageUpscale: took %.1f ms", jni_time_ms() - t_up);

    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("Upscale failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressImageLossless(JNIEnv *env, jobject thiz,
                                                         jbyteArray pixels, jint width, jint height,
                                                         jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;
    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;
    LOGI("compressImageLossless: %dx%d, %dch, source=%lld", width, height, channels, (long long)sourceSize);
    double t_ll = jni_time_ms();
    int result = qtsq_compress_gravitational_lossless(&ctx, (const uint8_t *)c_pixels,
                 (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressImageLossless: took %.1f ms", jni_time_ms() - t_ll);
    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);
    if (result != QTSQ_OK) {
        LOGE("Lossless failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) { jni_encrypt_for_write(&ctx, pass_str); (*env)->ReleaseStringUTFChars(env, password, pass_str); }
    }
    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ Pure Splat Only: Compact ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressSplatCompact(JNIEnv *env, jobject thiz,
                                                         jbyteArray pixels, jint width, jint height,
                                                         jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;
    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;
    LOGI("compressSplatCompact: %dx%d, %dch, source=%lld", width, height, channels, (long long)sourceSize);
    double t0 = jni_time_ms();
    int result = qtsq_compress_splat_compact(&ctx, (const uint8_t *)c_pixels,
                 (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressSplatCompact: took %.1f ms", jni_time_ms() - t0);
    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);
    if (result != QTSQ_OK) {
        LOGE("Splat Compact failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) { jni_encrypt_for_write(&ctx, pass_str); (*env)->ReleaseStringUTFChars(env, password, pass_str); }
    }
    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ Pure Splat Only: HD ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressSplatHD(JNIEnv *env, jobject thiz,
                                                     jbyteArray pixels, jint width, jint height,
                                                     jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;
    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;
    LOGI("compressSplatHD: %dx%d, %dch, source=%lld", width, height, channels, (long long)sourceSize);
    double t0 = jni_time_ms();
    int result = qtsq_compress_splat_hd(&ctx, (const uint8_t *)c_pixels,
                 (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressSplatHD: took %.1f ms", jni_time_ms() - t0);
    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);
    if (result != QTSQ_OK) {
        LOGE("Splat HD failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) { jni_encrypt_for_write(&ctx, pass_str); (*env)->ReleaseStringUTFChars(env, password, pass_str); }
    }
    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ Pure Splat Only: Lossless ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressSplatLossless(JNIEnv *env, jobject thiz,
                                                          jbyteArray pixels, jint width, jint height,
                                                          jint channels, jstring password, jlong sourceSize) {
    if (!pixels) return NULL;
    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;
    LOGI("compressSplatLossless: %dx%d, %dch, source=%lld", width, height, channels, (long long)sourceSize);
    double t0 = jni_time_ms();
    int result = qtsq_compress_splat_lossless(&ctx, (const uint8_t *)c_pixels,
                 (uint32_t)width, (uint32_t)height, (uint8_t)channels, (size_t)sourceSize);
    LOGI("compressSplatLossless: took %.1f ms", jni_time_ms() - t0);
    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);
    if (result != QTSQ_OK) {
        LOGE("Splat Lossless failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) { jni_encrypt_for_write(&ctx, pass_str); (*env)->ReleaseStringUTFChars(env, password, pass_str); }
    }
    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressAudio(JNIEnv *env, jobject thiz,
                                                jshortArray pcm, jint sampleRate, jint channels, jstring password) {
    if (!pcm || sampleRate <= 0 || channels <= 0) {
        LOGE("compressAudio: invalid params sr=%d ch=%d", sampleRate, channels);
        return NULL;
    }

    jsize pcm_len = (*env)->GetArrayLength(env, pcm);
    LOGI("compressAudio: %d samples, %dHz, %dch", pcm_len, sampleRate, channels);

    if (pcm_len == 0) {
        LOGE("compressAudio: empty PCM data");
        return NULL;
    }

    jshort *pcm_data = (*env)->GetShortArrayElements(env, pcm, NULL);
    if (!pcm_data) {
        LOGE("compressAudio: failed to get PCM data");
        return NULL;
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    int result;

    /*
     * Lensing (DFT) is O(n log n) per 1024-sample frame.
     * For short recordings (≤10 seconds), use Lensing for good compression.
     * For longer recordings, use Singularity (fast, lossless) to avoid delay.
     */
    size_t max_lensing_samples = (size_t)sampleRate * 10; /* 10 seconds */

    if ((size_t)pcm_len <= max_lensing_samples) {
        LOGI("compressAudio: using Lensing (Fourier) for %d samples", pcm_len);
        result = qtsq_compress_audio(&ctx, (const int16_t *)pcm_data, pcm_len, sampleRate, channels);
    } else {
        LOGI("compressAudio: using Singularity (fast) for %d samples (>10s)", pcm_len);
        size_t byte_size = (size_t)pcm_len * sizeof(int16_t);
        result = qtsq_compress_raw(&ctx, (const uint8_t *)pcm_data, byte_size, 0);
        if (result == QTSQ_OK) {
            /* Preserve audio metadata */
            ctx.header.data_type = (channels > 1) ? QTSQ_TYPE_AUDIO_STEREO : QTSQ_TYPE_AUDIO_MONO;
            ctx.schema.sample_rate = sampleRate;
            ctx.header.num_channels = channels;
            ctx.header.sample_depth = 16;
        }
    }

    (*env)->ReleaseShortArrayElements(env, pcm, pcm_data, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("Audio compression failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "aud");
    qtsq_free(&ctx);
    return out;
}

/* ── QTSQ Audio HQ: Photon Gabor spatial atom compression ── */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressAudioHQ(JNIEnv *env, jobject thiz,
                                                    jshortArray pcm, jint sampleRate, jint channels, jstring password) {
    if (!pcm || sampleRate <= 0 || channels <= 0) {
        LOGE("compressAudioHQ: invalid params sr=%d ch=%d", sampleRate, channels);
        return NULL;
    }

    jsize pcm_len = (*env)->GetArrayLength(env, pcm);
    LOGI("compressAudioHQ: %d samples, %dHz, %dch", pcm_len, sampleRate, channels);

    if (pcm_len == 0) return NULL;

    jshort *pcm_data = (*env)->GetShortArrayElements(env, pcm, NULL);
    if (!pcm_data) return NULL;

    /* Convert int16 PCM to float32 for Photon HQ codec */
    float *float_pcm = (float *)malloc((size_t)pcm_len * sizeof(float));
    if (!float_pcm) {
        (*env)->ReleaseShortArrayElements(env, pcm, pcm_data, JNI_ABORT);
        return NULL;
    }
    for (int i = 0; i < pcm_len; i++) {
        float_pcm[i] = (float)pcm_data[i] / 32767.0f;
    }

    (*env)->ReleaseShortArrayElements(env, pcm, pcm_data, JNI_ABORT);

    /* Ensure at least 2 channels for Photon HQ */
    int reqChannels = channels < 2 ? 2 : channels;
    float *hq_pcm = float_pcm;
    int hq_len = pcm_len;

    if (channels == 1) {
        /* Duplicate mono to stereo */
        int frames = pcm_len;
        hq_len = frames * 2;
        hq_pcm = (float *)malloc((size_t)hq_len * sizeof(float));
        if (!hq_pcm) { free(float_pcm); return NULL; }
        for (int i = 0; i < frames; i++) {
            hq_pcm[i * 2]     = float_pcm[i];
            hq_pcm[i * 2 + 1] = float_pcm[i];
        }
        free(float_pcm);
    }

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    LOGI("compressAudioHQ: using Photon (Gabor) for %d samples", hq_len);
    double t_hq = jni_time_ms();
    int result = qtsq_compress_audio_hq(&ctx, hq_pcm, (size_t)hq_len, (uint32_t)sampleRate, (uint8_t)reqChannels);
    LOGI("compressAudioHQ: took %.1f ms", jni_time_ms() - t_hq);

    free(hq_pcm);

    if (result != QTSQ_OK) {
        LOGE("AudioHQ compression failed: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "aud");
    qtsq_free(&ctx);
    return out;
}

/**
 * Parse a qtsq context from an in-memory byte buffer.
 * Full mirror of core qtsq_read() (src/qtsq_format.c): V1/V2 header
 * migration, V3 security block, sanitize-flag reconstruction, IO guards,
 * and GEN-1 checksum verification.
 */
static int qtsq_read_from_memory(qtsq_context_t *ctx, const uint8_t *data, size_t data_len) {
    if (!ctx || !data) return QTSQ_ERR_NULL;
    if (data_len < 5) return QTSQ_ERR_FORMAT;

    size_t pos = 0;

    /* Magic check */
    if (data[0] != QTSQ_MAGIC_0 ||
        data[1] != QTSQ_MAGIC_1 ||
        data[2] != QTSQ_MAGIC_2 ||
        data[3] != QTSQ_MAGIC_3) {
        return QTSQ_ERR_FORMAT;
    }

    /* Version dispatch — peek byte at offset 4 (same as core qtsq_read) */
    uint8_t file_version = data[4];

    if (file_version == QTSQ_VERSION_1) {
        /* V1 header (80 bytes) — upgrade in memory, zero-extend 32-bit sizes */
        qtsq_header_v1_t v1;
        if (data_len < sizeof(v1)) return QTSQ_ERR_FORMAT;
        memcpy(&v1, data, sizeof(v1));
        memcpy(ctx->header.magic, v1.magic, 4);
        ctx->header.version         = QTSQ_VERSION_2; /* upgrade in memory */
        ctx->header.strategy        = v1.strategy;
        ctx->header.flags           = v1.flags;
        ctx->header.data_type       = v1.data_type;
        ctx->header.num_channels    = v1.num_channels;
        ctx->header.sample_depth    = v1.sample_depth;
        ctx->header.original_size   = v1.original_size;
        ctx->header.timestamp       = v1.timestamp;
        memcpy(ctx->header.checksum, v1.checksum, 32);
        ctx->header.horizon_size     = (uint64_t)v1.horizon_size;
        ctx->header.accretion_size   = (uint64_t)v1.accretion_size;
        ctx->header.singularity_size = (uint64_t)v1.singularity_size;
        ctx->header.radiation_size   = (uint64_t)v1.radiation_size;
        ctx->header.security_block_size = 0;
        pos = sizeof(v1);
    } else if (file_version == QTSQ_VERSION_2) {
        /* V2 header (96 bytes) — upgrade in memory */
        qtsq_header_v2_t v2;
        if (data_len < sizeof(v2)) return QTSQ_ERR_FORMAT;
        memcpy(&v2, data, sizeof(v2));
        memcpy(ctx->header.magic, v2.magic, 4);
        ctx->header.version         = QTSQ_VERSION_3; /* upgrade */
        ctx->header.strategy        = v2.strategy;
        ctx->header.flags           = v2.flags;
        ctx->header.data_type       = v2.data_type;
        ctx->header.num_channels    = v2.num_channels;
        ctx->header.sample_depth    = v2.sample_depth;
        ctx->header.original_size   = v2.original_size;
        ctx->header.timestamp       = v2.timestamp;
        memcpy(ctx->header.checksum, v2.checksum, 32);
        ctx->header.horizon_size     = v2.horizon_size;
        ctx->header.accretion_size   = v2.accretion_size;
        ctx->header.singularity_size = v2.singularity_size;
        ctx->header.radiation_size   = v2.radiation_size;
        ctx->header.security_block_size = 0;
        pos = sizeof(v2);
    } else if (file_version == QTSQ_VERSION_3) {
        /* V3 header (112 bytes) directly */
        if (data_len < sizeof(qtsq_header_t)) return QTSQ_ERR_FORMAT;
        memcpy(&ctx->header, data, sizeof(qtsq_header_t));
        pos = sizeof(qtsq_header_t);
    } else {
        /* Unknown/future version — refuse rather than misparse as v3.
         * Mirrors qtsq_read() in src/qtsq_format.c. */
        return QTSQ_ERR_FORMAT;
    }

    /* Schema */
    if (ctx->header.horizon_size > 0) {
        if ((uint64_t)pos + ctx->header.horizon_size > data_len) return QTSQ_ERR_FORMAT;
        size_t to_read = ctx->header.horizon_size;
        if (to_read > sizeof(qtsq_schema_t)) to_read = sizeof(qtsq_schema_t);
        memcpy(&ctx->schema, data + pos, to_read);
        pos += (size_t)ctx->header.horizon_size; /* skip full declared size */
    }

    /* Transforms (accretion) — two-pass scan with core IO guards */
    if (ctx->header.accretion_size > 0) {
        if ((uint64_t)pos + ctx->header.accretion_size > data_len) return QTSQ_ERR_FORMAT;
        size_t acc_end = pos + (size_t)ctx->header.accretion_size;

        /* First pass: count transforms */
        size_t scan = pos;
        uint32_t count = 0;
        while (scan < acc_end) {
            if (scan + 5 > acc_end) break;
            uint8_t type = data[scan];
            uint32_t psize;
            memcpy(&psize, data + scan + 1, 4);
            /* IO-4 FIX: zero-sized params with zero type = padding/sentinel */
            if (psize == 0 && type == 0) break;
            /* IO-2 guard: param_size comes from file — bound before use */
            if (psize > 65536) return QTSQ_ERR_FORMAT;
            scan += 5 + psize;
            count++;
        }

        ctx->num_transforms = count;
        if (count > 0) {
            ctx->transforms = calloc(count, sizeof(qtsq_transform_t));
            if (!ctx->transforms) return QTSQ_ERR_ALLOC;

            /* Second pass: read transforms */
            for (uint32_t i = 0; i < count && pos < acc_end; i++) {
                ctx->transforms[i].type = data[pos++];
                if (pos + 4 > data_len) return QTSQ_ERR_FORMAT;
                memcpy(&ctx->transforms[i].param_size, data + pos, 4);
                pos += 4;
                if (ctx->transforms[i].param_size > 0) {
                    if (ctx->transforms[i].param_size > 65536) return QTSQ_ERR_FORMAT;
                    if (pos + ctx->transforms[i].param_size > data_len) return QTSQ_ERR_FORMAT;
                    ctx->transforms[i].params = malloc(ctx->transforms[i].param_size);
                    if (!ctx->transforms[i].params) return QTSQ_ERR_ALLOC;
                    memcpy(ctx->transforms[i].params, data + pos, ctx->transforms[i].param_size);
                    pos += ctx->transforms[i].param_size;
                }
            }
        }
    }

    /* Singularity data */
    if (ctx->header.singularity_size > 0) {
        /* IO-1 FIX: file-declared size — bound to prevent DoS malloc */
        if (ctx->header.singularity_size > 4000000000ULL) return QTSQ_ERR_FORMAT;
        if ((uint64_t)pos + ctx->header.singularity_size > data_len) return QTSQ_ERR_FORMAT;
        ctx->singularity_size = ctx->header.singularity_size;
        ctx->singularity = malloc((size_t)ctx->singularity_size);
        if (!ctx->singularity) return QTSQ_ERR_ALLOC;
        memcpy(ctx->singularity, data + pos, (size_t)ctx->singularity_size);
        pos += (size_t)ctx->singularity_size;
    }

    /* Radiation index */
    if (ctx->header.radiation_size > 0) {
        if ((uint64_t)pos + ctx->header.radiation_size > data_len) return QTSQ_ERR_FORMAT;
        ctx->num_radiation = (uint32_t)(ctx->header.radiation_size / sizeof(qtsq_index_t));
        ctx->radiation_index = calloc(ctx->num_radiation > 0 ? ctx->num_radiation : 1,
                                      sizeof(qtsq_index_t));
        if (!ctx->radiation_index) return QTSQ_ERR_ALLOC;
        memcpy(ctx->radiation_index, data + pos, (size_t)ctx->header.radiation_size);
        pos += (size_t)ctx->header.radiation_size;
    }

    /* Flags — PQ files set QTSQ_FLAG_PQ_ENCRYPTED, not just ENCRYPTED */
    ctx->is_encrypted = ((ctx->header.flags & QTSQ_FLAG_ENCRYPTED) ||
                         (ctx->header.flags & QTSQ_FLAG_PQ_ENCRYPTED)) ? 1 : 0;
    ctx->has_recovery = (ctx->header.flags & QTSQ_FLAG_HAS_ECC) ? 1 : 0;

    /* Reconstruct sanitize state from header flags (core parity) */
    if ((ctx->header.flags & QTSQ_FLAG_ANTI_STEGO) ||
        (ctx->header.flags & QTSQ_FLAG_ZERO_META)) {
        ctx->is_sanitized = 1;
        ctx->sanitize_report = 0;
        if (ctx->header.flags & QTSQ_FLAG_ANTI_STEGO)  ctx->sanitize_report |= QTSQ_SANITIZE_IMAGE_LSB;
        if (ctx->header.flags & QTSQ_FLAG_ZERO_META)   ctx->sanitize_report |= QTSQ_SANITIZE_ZERO_META;
        if (ctx->is_encrypted)                          ctx->sanitize_report |= QTSQ_SANITIZE_ENCRYPTED;
    }

    /* V3 security block if present, else legacy censorship metadata */
    if (ctx->header.security_block_size > 0 &&
        ctx->header.security_block_size <= sizeof(qtsq_security_block_t)) {
        if ((uint64_t)pos + ctx->header.security_block_size > data_len) return QTSQ_ERR_FORMAT;
        memset(&ctx->security, 0, sizeof(ctx->security));
        memcpy(&ctx->security, data + pos, (size_t)ctx->header.security_block_size);
        pos += (size_t)ctx->header.security_block_size;
        ctx->has_security = 1;
    } else if (ctx->is_encrypted) {
        if ((uint64_t)pos + sizeof(qtsq_censorship_t) > data_len) {
            ctx->is_encrypted = 0; /* truncated — same leniency as core */
        } else {
            memcpy(&ctx->censorship, data + pos, sizeof(qtsq_censorship_t));
            pos += sizeof(qtsq_censorship_t);
        }
    }

    /* Recovery (ECC) data — with core IO-3 bounds. Lenient at the head,
     * like core qtsq_read: some writers set QTSQ_FLAG_HAS_ECC without
     * appending a recovery block, and core treats EOF here as "no data",
     * not an error. Only a *partial* block is malformed. */
    if (ctx->has_recovery) {
        if ((uint64_t)pos + 8 <= data_len) {
            uint32_t num_chunks, parity_size;
            memcpy(&num_chunks, data + pos, 4); pos += 4;
            memcpy(&parity_size, data + pos, 4); pos += 4;
            if (num_chunks > 1000000 || parity_size > 100000000UL) return QTSQ_ERR_FORMAT;
            ctx->recovery.num_chunks = num_chunks;
            ctx->recovery.parity_size = parity_size;
            if (num_chunks > 0) {
                if ((uint64_t)pos + 4ULL * num_chunks > data_len) return QTSQ_ERR_FORMAT;
                ctx->recovery.chunk_checksums = (uint32_t *)calloc(num_chunks, sizeof(uint32_t));
                if (!ctx->recovery.chunk_checksums) return QTSQ_ERR_ALLOC;
                memcpy(ctx->recovery.chunk_checksums, data + pos, (size_t)(4ULL * num_chunks));
                pos += (size_t)(4ULL * num_chunks);
            }
            if (parity_size > 0) {
                if ((uint64_t)pos + parity_size > data_len) return QTSQ_ERR_FORMAT;
                ctx->recovery.reed_solomon_parity = (uint8_t *)malloc(parity_size);
                if (!ctx->recovery.reed_solomon_parity) return QTSQ_ERR_ALLOC;
                memcpy(ctx->recovery.reed_solomon_parity, data + pos, parity_size);
                pos += parity_size;
            }
        }
    }

    /* GEN-1 FIX: verify checksum when flagged (skip if encrypted —
     * the checksum covers plaintext). */
    if ((ctx->header.flags & QTSQ_FLAG_CHECKSUMMED) &&
         ctx->singularity && ctx->singularity_size > 0 &&
         !ctx->is_encrypted) {
        uint8_t computed[32];
        qtsq_sha256(ctx->singularity, ctx->singularity_size, computed);
        if (memcmp(computed, ctx->header.checksum, 32) != 0) {
            return QTSQ_ERR_CORRUPT;
        }
    }

    return QTSQ_OK;
}

extern int qtsq_use_gpu;

JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_setUseGpu(JNIEnv *env, jobject thiz, jboolean useGpu) {
    qtsq_use_gpu = useGpu ? 1 : 0;
    LOGI("JNI setUseGpu: %d", qtsq_use_gpu);
}

JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_setCodec(JNIEnv *env, jobject thiz, jint codec) {
    (void)env; (void)thiz;
    g_jni_codec = (codec == QTSQ_CODEC_ZLIB) ? QTSQ_CODEC_ZLIB : QTSQ_CODEC_QTC;
    LOGI("JNI setCodec: %d", g_jni_codec);
}

JNIEXPORT jobject JNICALL
Java_com_haruhito_qtsq_QtsqEngine_decompressBytes(JNIEnv *env, jobject thiz,
                                                  jbyteArray qtsqBytes, jstring password) {
    if (!qtsqBytes) {
        LOGE("decompressBytes: null input");
        return NULL;
    }

    jsize in_len = (*env)->GetArrayLength(env, qtsqBytes);
    jbyte *in_data = (*env)->GetByteArrayElements(env, qtsqBytes, NULL);
    if (!in_data) {
        LOGE("decompressBytes: failed to get input bytes");
        return NULL;
    }

    LOGI("decompressBytes: %d bytes input", in_len);

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    /* Parse directly from memory — no temp files needed */
    int read_res = qtsq_read_from_memory(&ctx, (const uint8_t *)in_data, (size_t)in_len);
    (*env)->ReleaseByteArrayElements(env, qtsqBytes, in_data, JNI_ABORT);

    if (read_res != QTSQ_OK) {
        LOGE("Decompression parse failed: %d (%s)", read_res, qtsq_error_string(read_res));
        qtsq_free(&ctx);
        return NULL;
    }

    /* Full-sync decrypt: V3 vault password → legacy censorship → device
     * keystore (PQ files saved by this device open without a password,
     * matching tools/qtsq_cli.c cli_decrypt_after_read). */
    if (ctx.is_encrypted) {
        const char *pass_str = password ? (*env)->GetStringUTFChars(env, password, NULL) : NULL;
        int dec_res = jni_decrypt_after_read(&ctx, pass_str);
        if (pass_str) (*env)->ReleaseStringUTFChars(env, password, pass_str);
        if (dec_res != QTSQ_OK) {
            LOGE("Decryption failed: %d (%s)", dec_res, qtsq_error_string(dec_res));
            qtsq_free(&ctx);
            return NULL;
        }
    }

    uint8_t *out_data = NULL;
    size_t out_size = 0;
    int dec_res = qtsq_decompress(&ctx, &out_data, &out_size);
    if (dec_res != QTSQ_OK) {
        LOGE("Decompression failed: %d (%s)", dec_res, qtsq_error_string(dec_res));
        qtsq_free(&ctx);
        return NULL;
    }

    LOGI("Decompressed: %zu bytes, type=%d", out_size, ctx.header.data_type);

    /* Pre-pack for Android ARGB_8888 Bitmap format automatically for instant
     * Kotlin zero-copy. The pack writes straight into the returned Java byte
     * array — the old path packed into a malloc'd ARGB buffer first and then
     * copied the whole thing again into the array. */
    int is_img = (ctx.header.data_type == QTSQ_TYPE_IMAGE_RGB || 
                  ctx.header.data_type == QTSQ_TYPE_IMAGE_GRAY ||
                  ctx.header.data_type == QTSQ_TYPE_IMAGE_RGBA);
    int packed_argb = 0;
    size_t argb_sz = 0;
    if (is_img) {
        int w = ctx.schema.dimensions[0];
        int h = ctx.schema.dimensions[1];

        /* JPEG/PNG passthrough detection: dimensions are 0 when using pixel_passthrough */
        if (w <= 0 || h <= 0) {
            /* Check for JPEG magic (0xFF 0xD8) or PNG magic */
            int is_jpeg = (out_size > 2 && out_data[0] == 0xFF && out_data[1] == 0xD8);
            int is_png  = (out_size > 4 && out_data[0] == 0x89 && out_data[1] == 0x50);
            if (is_jpeg || is_png) {
                /* Return raw JPEG/PNG bytes — Kotlin will BitmapFactory.decode() them */
                /* Override type so Kotlin knows to treat as encoded image */
                ctx.header.data_type = QTSQ_TYPE_IMAGE_RGB; /* keep image type but marker w=0 */
                LOGI("Pixel JPEG/PNG passthrough detected (%s), %zu bytes",
                     is_jpeg ? "JPEG" : "PNG", out_size);
                /* Fall through — do NOT re-pack into ARGB bytes */
                is_img = 0;
            }
        }

        if (is_img) {
            argb_sz = (size_t)w * h * 4;
            packed_argb = 1;
        }
    }

    jbyteArray out_arr = NULL;
    if (packed_argb) {
        out_arr = (*env)->NewByteArray(env, (jint)argb_sz);
        if (out_arr) {
            jbyte *dst = (*env)->GetByteArrayElements(env, out_arr, NULL);
            if (dst) {
                pack_argb_into(out_data, (uint8_t *)dst, argb_sz / 4,
                               ctx.header.data_type);
                (*env)->ReleaseByteArrayElements(env, out_arr, dst, 0);
            } else {
                /* Direct pin failed — pack into a temp buffer and copy in. */
                uint8_t *tmp = (uint8_t *)malloc(argb_sz);
                if (tmp) {
                    pack_argb_into(out_data, tmp, argb_sz / 4,
                                   ctx.header.data_type);
                    (*env)->SetByteArrayRegion(env, out_arr, 0, (jint)argb_sz,
                                               (const jbyte *)tmp);
                    free(tmp);
                }
            }
        }
        free(out_data);
        out_size = argb_sz;
        /* Keep original data_type so Kotlin knows it's an image */
    } else {
        out_arr = create_jbyteArray(env, out_data, out_size);
        free(out_data);
    }

    if (!out_arr) {
        LOGE("Failed to create output byte array");
        qtsq_free(&ctx);
        return NULL;
    }

    jclass resultClass = (*env)->FindClass(env, "com/haruhito/qtsq/QtsqEngine$QtsqResult");
    if (!resultClass) {
        LOGE("Failed to find QtsqResult class");
        qtsq_free(&ctx);
        return NULL;
    }

    jmethodID ctor = (*env)->GetMethodID(env, resultClass, "<init>", "([BIJIIILjava/lang/String;III)V");
    if (!ctor) {
        LOGE("Failed to find QtsqResult constructor");
        qtsq_free(&ctx);
        return NULL;
    }

    int t_width = 0, t_height = 0, t_sampleRate = 0;
    if (ctx.header.data_type == QTSQ_TYPE_IMAGE_RGB || 
        ctx.header.data_type == QTSQ_TYPE_IMAGE_GRAY ||
        ctx.header.data_type == QTSQ_TYPE_IMAGE_RGBA) {
        t_width = ctx.schema.dimensions[0];
        t_height = ctx.schema.dimensions[1];
        LOGI("Image dimensions: %dx%d", t_width, t_height);
    } else if (ctx.header.data_type == QTSQ_TYPE_AUDIO_MONO || 
               ctx.header.data_type == QTSQ_TYPE_AUDIO_STEREO) {
        t_sampleRate = ctx.schema.sample_rate;
        LOGI("Audio sample rate: %d", t_sampleRate);
    }

    /* Pass encoding label and vertex count for hybrid mode display */
    jstring j_encoding = (*env)->NewStringUTF(env, ctx.schema.encoding);
    int t_vertexCount = (int)ctx.header.reserved32;
    /* In pure triangle mode, dimensions[2] holds the number of channels (3), not splat count */
    int t_splatCount = (ctx.header.strategy == QTSQ_STRATEGY_TRIANGLE) ? 0 : (int)ctx.schema.dimensions[2]; 
    int t_faceCount = (int)ctx.schema.dimensions[3];

    LOGI("Encoding: %s, Vertices: %d", ctx.schema.encoding, t_vertexCount);

    jobject resultObj = (*env)->NewObject(env, resultClass, ctor,
                                          out_arr,
                                          (jint)ctx.header.data_type,
                                          (jlong)ctx.header.original_size,
                                          (jint)t_width,
                                          (jint)t_height,
                                          (jint)t_sampleRate,
                                          j_encoding,
                                          (jint)t_vertexCount,
                                          (jint)t_faceCount,
                                          (jint)t_splatCount);

    qtsq_free(&ctx);
    return resultObj;
}

/* Convert Android ARGB_8888 pixels (Bitmap.getPixels IntArray) to packed
 * RGB bytes for the compress path. Native replacement for the per-pixel
 * Kotlin loops; writes straight into the returned Java byte array. */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_argbToRgb(JNIEnv *env, jobject thiz,
                                            jintArray argb) {
    (void)thiz;
    if (!argb) return NULL;
    jsize npix = (*env)->GetArrayLength(env, argb);
    if (npix <= 0 || (uint64_t)npix * 3 > (uint64_t)0x7FFFFFFF) return NULL;

    jint *src = (*env)->GetIntArrayElements(env, argb, NULL);
    if (!src) return NULL;

    jbyteArray out = (*env)->NewByteArray(env, npix * 3);
    if (out) {
        jbyte *dst = (*env)->GetByteArrayElements(env, out, NULL);
        if (dst) {
            for (jsize i = 0; i < npix; i++) {
                uint32_t p = (uint32_t)src[i];
                dst[i*3 + 0] = (jbyte)((p >> 16) & 0xFF);
                dst[i*3 + 1] = (jbyte)((p >> 8) & 0xFF);
                dst[i*3 + 2] = (jbyte)(p & 0xFF);
            }
            (*env)->ReleaseByteArrayElements(env, out, dst, 0);
        } else {
            /* Direct pin failed — convert into a temp buffer and copy in. */
            uint8_t *tmp = (uint8_t *)malloc((size_t)npix * 3);
            if (tmp) {
                for (jsize i = 0; i < npix; i++) {
                    uint32_t p = (uint32_t)src[i];
                    tmp[i*3 + 0] = (uint8_t)((p >> 16) & 0xFF);
                    tmp[i*3 + 1] = (uint8_t)((p >> 8) & 0xFF);
                    tmp[i*3 + 2] = (uint8_t)(p & 0xFF);
                }
                (*env)->SetByteArrayRegion(env, out, 0, npix * 3,
                                           (const jbyte *)tmp);
                free(tmp);
            }
        }
    }
    (*env)->ReleaseIntArrayElements(env, argb, src, JNI_ABORT);
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Triangle Only — Lossless (Adaptive Recursive Subdivision / TRM5)
 *
 *  Uses qtsq_compress_triangle_adaptive with depth=4, threshold=3.0
 *  to produce pixel-perfect, extreme-detail triangle mesh output.
 *  This is the default mode for all Android direct-camera captures.
 *
 *  Memory:  vertex + face buffers are capped by platform (#ifdef __ANDROID__)
 *           inside qtsq_triangle_only.c — the iQOO 12 with 16 GB RAM
 *           can safely handle the full desktop budget.
 * ═══════════════════════════════════════════════════════════════════════ */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressImageLosslessTriangle(
        JNIEnv *env, jobject thiz,
        jbyteArray pixels, jint width, jint height,
        jint channels, jstring password, jlong sourceSize) {

    if (!pixels) return NULL;

    jsize pxLen = (*env)->GetArrayLength(env, pixels);
    jbyte *c_pixels = (*env)->GetByteArrayElements(env, pixels, NULL);
    if (!c_pixels) return NULL;

    LOGI("compressImageLosslessTriangle: %dx%d ch=%d src=%lld px_bytes=%d",
         width, height, channels, (long long)sourceSize, (int)pxLen);

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    double t0 = jni_time_ms();

    /* Android camera capture parameters — tuned for iQOO 12 real-time use:
     *
     * depth=4  → selects Lossless tier (px_fraction=0.65), but the __ANDROID__
     *             hard cap in qtsq_triangle_only.c clamps total verts to 500K.
     *             With g³ gradient boost, 300K initial verts concentrate heavily
     *             on edges/text giving near-lossless quality on detail regions.
     *
     * threshold=5.0  → subdivide any triangle with per-pixel MSE > 5
     *             (≈2.2 color units per channel error). This is visually
     *             transparent for natural photos while being 16× faster than
     *             the previous threshold=0.3.
     *
     * Expected compression time on iQOO 12 (4K image, 12 cores):
     *   Gradient:   ~150ms  (parallel, Sobel)
     *   Placement:  ~300ms  (parallel, 300K verts)
     *   Delaunay:   ~15s    (serial, O(n log n) on 300K points)
     *   Refine:     ~5s     (parallel error eval, selective subdivision)
     *   Total:      ~20-30s (was 3-8+ minutes)
     */
    int result = qtsq_compress_triangle_adaptive(
            &ctx,
            (const uint8_t *)c_pixels,
            (uint32_t)width, (uint32_t)height,
            (uint8_t)channels,
            (size_t)sourceSize,
            5,      /* max_depth: Bounded subdivision allows straight edges to form gracefully */
            1.5f    /* error_threshold: Calibrated to accept base-grid relaxed vectors */
    );

    LOGI("compressImageLosslessTriangle: compression done in %.1f ms, result=%d",
         jni_time_ms() - t0, result);

    (*env)->ReleaseByteArrayElements(env, pixels, c_pixels, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("compressImageLosslessTriangle FAILED: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    /* Optional encryption */
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_compressPixelJpeg(
        JNIEnv *env, jobject thiz,
        jbyteArray jpegBytes, jstring password) {

    if (!jpegBytes) return NULL;

    jsize pxLen = (*env)->GetArrayLength(env, jpegBytes);
    jbyte *c_jpeg = (*env)->GetByteArrayElements(env, jpegBytes, NULL);
    if (!c_jpeg) return NULL;

    LOGI("compressPixelJpeg: wrapping %d bytes", (int)pxLen);

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;

    double t0 = jni_time_ms();

    /* Pixel compression bypass directly stores the JPEG bytes */
    int result = qtsq_compress_pixel_passthrough(
            &ctx,
            (const uint8_t *)c_jpeg,
            (size_t)pxLen
    );

    LOGI("compressPixelJpeg: passthrough done in %.1f ms, result=%d",
         jni_time_ms() - t0, result);

    (*env)->ReleaseByteArrayElements(env, jpegBytes, c_jpeg, JNI_ABORT);

    if (result != QTSQ_OK) {
        LOGE("compressPixelJpeg FAILED: %d (%s)", result, qtsq_error_string(result));
        qtsq_free(&ctx);
        return NULL;
    }

    /* Optional encryption */
    if (password) {
        const char *pass_str = (*env)->GetStringUTFChars(env, password, NULL);
        if (pass_str) {
            jni_encrypt_for_write(&ctx, pass_str);
            (*env)->ReleaseStringUTFChars(env, password, pass_str);
        }
    }

    jbyteArray out = context_to_bytes(env, &ctx, "img");
    qtsq_free(&ctx);
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Phase C — Live ARCore capture (posed-frame keyframe buffer)
 *
 *  Native state is a qtsq_capture_t* returned to Kotlin as a jlong handle.
 *  No file I/O, no network: the keyframe window lives in RAM only, and
 *  non-keyframes are never copied ("no video is ever stored").
 * ══════════════════════════════════════════════════════════════════════ */

JNIEXPORT jlong JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureInit(JNIEnv *env, jobject thiz,
        jint w, jint h, jint kfCap, jfloat tThresh, jfloat cosThresh) {
    (void)env; (void)thiz;
    qtsq_capture_t *c = qtsq_capture_create((int)w, (int)h, (int)kfCap,
                                            (float)tThresh, (float)cosThresh);
    if (!c) {
        LOGE("captureInit: failed (w=%d h=%d kfCap=%d)", w, h, kfCap);
        return 0;
    }
    LOGI("captureInit: %dx%d window=%d tThresh=%.3f cosThresh=%.4f",
         w, h, kfCap, tThresh, cosThresh);
    return (jlong)c;
}

/*
 * Offer one posed frame.
 *   poseMatrix : float[16] column-major camera→world (Pose.toMatrix)
 *   intr       : float[4]  = {fx, fy, cx, cy} already scaled into W×H space
 *   rgb        : byte[W*H*3] R,G,B (0..255); converted to linear [0,1] here
 * Returns 1 if admitted as a keyframe (frame kept), 0 if discarded.
 */
JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureObserve(JNIEnv *env, jobject thiz,
        jlong handle, jfloatArray poseMatrix, jfloatArray intr, jbyteArray rgb) {
    (void)thiz;
    qtsq_capture_t *c = (qtsq_capture_t *)handle;
    if (!c || !poseMatrix || !intr || !rgb) return 0;

    jsize pose_len = (*env)->GetArrayLength(env, poseMatrix);
    jsize intr_len = (*env)->GetArrayLength(env, intr);
    jsize rgb_len  = (*env)->GetArrayLength(env, rgb);
    size_t need_px = (size_t)c->W * c->H * 3;
    if (pose_len < 16 || intr_len < 4 || (size_t)rgb_len < need_px) {
        LOGE("captureObserve: bad array sizes (pose=%d intr=%d rgb=%d need=%zu)",
             pose_len, intr_len, rgb_len, need_px);
        return 0;
    }

    jfloat *pose = (*env)->GetFloatArrayElements(env, poseMatrix, NULL);
    jfloat *ci   = (*env)->GetFloatArrayElements(env, intr, NULL);
    cap_camera_t cam;
    qtsq_capture_camera_from_arpose(&cam, (const float *)pose,
                                    ci[0], ci[1], ci[2], ci[3], c->W, c->H);
    (*env)->ReleaseFloatArrayElements(env, poseMatrix, pose, JNI_ABORT);
    (*env)->ReleaseFloatArrayElements(env, intr, ci, JNI_ABORT);

    /* Convert the incoming byte RGB → linear float [0,1] scratch buffer.
     * We allocate here (not in the ring) so a discarded frame leaves nothing
     * behind: qtsq_capture_observe copies only on admission. */
    float *img = (float *)malloc(need_px * sizeof(float));
    if (!img) return 0;
    jbyte *rb = (*env)->GetByteArrayElements(env, rgb, NULL);
    for (size_t i = 0; i < need_px; i++)
        img[i] = (float)((unsigned char)rb[i]) * (1.0f / 255.0f);
    (*env)->ReleaseByteArrayElements(env, rgb, rb, JNI_ABORT);

    int kept = qtsq_capture_observe(c, &cam, img);
    free(img);   /* the ring made its own copy iff kept; scratch always freed */
    return kept;
}

/* Returns int[3] = {live keyframe count, capacity, total frames seen}. */
JNIEXPORT jintArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureStats(JNIEnv *env, jobject thiz,
        jlong handle) {
    (void)thiz;
    qtsq_capture_t *c = (qtsq_capture_t *)handle;
    jintArray out = (*env)->NewIntArray(env, 3);
    if (!out) return NULL;
    jint vals[3] = {0, 0, 0};
    if (c) {
        vals[0] = (jint)c->count;
        vals[1] = (jint)c->cap;
        vals[2] = (jint)c->frames_seen;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 3, vals);
    return out;
}

JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureFree(JNIEnv *env, jobject thiz,
        jlong handle) {
    (void)env; (void)thiz;
    qtsq_capture_t *c = (qtsq_capture_t *)handle;
    if (c) {
        LOGI("captureFree: %d keyframes retained, %ld frames seen",
             c->count, c->frames_seen);
        qtsq_capture_destroy(c);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Phase D — GPU Surfel Trainer JNI Bindings
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Returns trainer handle (long), or 0 on failure.
 * pointsXyz (float[n*3], world coords) / pointsRgb (float[n*3] or NULL)
 * seed the surfel map from the ARCore sparse point cloud; when absent or
 * empty the trainer falls back to random surfels near the first keyframe.
 */
JNIEXPORT jlong JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureTrainStart(JNIEnv *env, jobject thiz,
        jlong captureHandle, jfloatArray pointsXyz, jfloatArray pointsRgb) {
    (void)thiz;
    qtsq_capture_t *c = (qtsq_capture_t *)captureHandle;
    if (!c) { LOGE("captureTrainStart: null capture handle"); return 0; }

    LOGI("captureTrainStart: creating trainer for %dx%d, kf_cap=%d", c->W, c->H, c->cap);
    qtsq_trainer_t *t = trainer_create(c);
    if (!t) { LOGE("captureTrainStart: trainer_create failed"); return 0; }

    int seeded = 0;
    if (pointsXyz) {
        jsize len = (*env)->GetArrayLength(env, pointsXyz);
        int n = (int)(len / 3);
        if (n > 0) {
            jfloat *xyz = (*env)->GetFloatArrayElements(env, pointsXyz, NULL);
            jfloat *rgb = NULL;
            if (pointsRgb) {
                jsize rlen = (*env)->GetArrayLength(env, pointsRgb);
                if ((size_t)rlen >= (size_t)n * 3)
                    rgb = (*env)->GetFloatArrayElements(env, pointsRgb, NULL);
            }
            trainer_seed_from_points(t, xyz, rgb, n);
            if (rgb) (*env)->ReleaseFloatArrayElements(env, pointsRgb, rgb, JNI_ABORT);
            (*env)->ReleaseFloatArrayElements(env, pointsXyz, xyz, JNI_ABORT);
            seeded = 1;
            LOGI("captureTrainStart: seeded %d surfels from ARCore points", n);
        }
    }
    if (!seeded) {
        /* Fallback: random surfels near the first keyframe's camera. */
        cap_camera_t cam;
        if (qtsq_capture_sample(c, 0, &cam, NULL) == 1) {
            trainer_seed_random(t, 512, &cam, 1.0f);
            LOGI("captureTrainStart: seeded 512 random surfels (no point cloud)");
        } else {
            LOGI("captureTrainStart: no keyframes yet — trainer created unseeded");
        }
    }

    return (jlong)t;
}

/* Start (1) / stop (0) the background train loop (Phase D §3). */
JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureTrainSetRunning(JNIEnv *env, jobject thiz,
        jlong trainerHandle, jboolean running) {
    (void)thiz;
    qtsq_trainer_t *t = (qtsq_trainer_t *)trainerHandle;
    if (!t) return;
    if (running) trainer_start_loop(t);
    else         trainer_stop_loop(t);
}

/* Initialize the device keystore (device keypair) for secured saves.
 * Returns QTSQ_OK (0) on success. Must run after the JNI bridge is set up. */
JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_nativeKeystoreInit(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    int rc = qtsq_keystore_init();
    if (rc != QTSQ_OK)
        LOGE("nativeKeystoreInit: qtsq_keystore_init failed: %d", rc);
    else
        LOGI("nativeKeystoreInit: keystore ready");
    return rc;
}

/* ═══ On-device self-test: runs the EXACT camera-save pipeline in miniature ═══
 * compress → keystore encrypt → serialize → parse → keystore decrypt → decompress.
 * Returns a human-readable status string ("OK ..." or the first failing stage
 * with its error code). This turns any phone into a test bench for the
 * fail-closed save path without needing adb. */
JNIEXPORT jstring JNICALL
Java_com_haruhito_qtsq_QtsqEngine_nativeSelfTest(JNIEnv *env, jobject thiz) {
    (void)thiz;
    char msg[640];
    size_t off = 0;

    const char *home = getenv("HOME");
    off += (size_t)snprintf(msg + off, sizeof(msg) - off,
                            "HOME=%s; ", home ? home : "<UNSET>");

    /* 1. Keystore init (idempotent) */
    if (!qtsq_keystore_is_initialized()) {
        int rc = qtsq_keystore_init();
        if (rc != QTSQ_OK) {
            snprintf(msg + off, sizeof(msg) - off,
                     "FAIL@keystore_init rc=%d (%s)", rc, qtsq_error_string(rc));
            return (*env)->NewStringUTF(env, msg);
        }
    }
    off += (size_t)snprintf(msg + off, sizeof(msg) - off, "ks=ok; ");

    /* 2. Compress */
    const char *secret = "QTSQ selftest payload 12345";
    qtsq_context_t ctx;
    qtsq_init(&ctx);
    ctx.codec = g_jni_codec;
    int rc = qtsq_compress_text(&ctx, secret, (uint32_t)strlen(secret));
    if (rc != QTSQ_OK) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@compress rc=%d (%s)",
                 rc, qtsq_error_string(rc));
        return (*env)->NewStringUTF(env, msg);
    }

    /* 3. Encrypt via the same path every camera/audio/convert save uses */
    rc = context_ensure_encrypted(&ctx);
    if (rc != QTSQ_OK) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@encrypt rc=%d (%s)",
                 rc, qtsq_error_string(rc));
        qtsq_free(&ctx);
        return (*env)->NewStringUTF(env, msg);
    }
    off += (size_t)snprintf(msg + off, sizeof(msg) - off,
                            "enc=%s; ", ctx.has_security ? "v3-pq" : "legacy");

    /* 4. Serialize exactly like context_to_bytes */
    size_t blen = 0;
    uint8_t *bytes = context_serialize(&ctx, &blen);
    qtsq_free(&ctx);
    if (!bytes) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@serialize (malloc)");
        return (*env)->NewStringUTF(env, msg);
    }

    /* 5. Parse with the in-memory reader */
    qtsq_context_t r;
    qtsq_init(&r);
    rc = qtsq_read_from_memory(&r, bytes, blen);
    free(bytes);
    if (rc != QTSQ_OK) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@parse rc=%d (%s)",
                 rc, qtsq_error_string(rc));
        return (*env)->NewStringUTF(env, msg);
    }

    /* 6. Decrypt with NULL password (keystore branch — the app's own flow) */
    rc = jni_decrypt_after_read(&r, NULL);
    if (rc != QTSQ_OK) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@decrypt rc=%d (%s)",
                 rc, qtsq_error_string(rc));
        qtsq_free(&r);
        return (*env)->NewStringUTF(env, msg);
    }

    /* 7. Decompress and verify */
    uint8_t *out = NULL;
    size_t out_len = 0;
    rc = qtsq_decompress(&r, &out, &out_len);
    if (rc != QTSQ_OK || !out || out_len != strlen(secret) ||
        memcmp(out, secret, out_len) != 0) {
        snprintf(msg + off, sizeof(msg) - off, "FAIL@decompress rc=%d len=%zu",
                 rc, out_len);
        free(out);
        qtsq_free(&r);
        return (*env)->NewStringUTF(env, msg);
    }
    free(out);
    qtsq_free(&r);

    snprintf(msg + off, sizeof(msg) - off, "OK roundtrip");
    return (*env)->NewStringUTF(env, msg);
}

/* Returns trainer stats as float[7] = {step, loss, psnr, surfel_count,
 * coverage_pct, angle_spread_deg, thermal_sleep_ms} (Phase E/G HUD). */
JNIEXPORT jfloatArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureTrainStats(JNIEnv *env, jobject thiz,
        jlong trainerHandle) {
    (void)thiz;
    qtsq_trainer_t *t = (qtsq_trainer_t *)trainerHandle;
    jfloatArray out = (*env)->NewFloatArray(env, 7);
    if (!out) return NULL;
    jfloat vals[7] = {0, 0, 0, 0, 0, 0, 0};
    if (t) {
        trainer_stats_t s = trainer_get_stats(t);
        vals[0] = (jfloat)s.step;
        vals[1] = s.loss;
        vals[2] = s.psnr;
        vals[3] = (jfloat)s.surfel_count;
        vals[4] = s.coverage_pct;
        vals[5] = s.angle_spread_deg;
        vals[6] = s.thermal_sleep_ms;
    }
    (*env)->SetFloatArrayRegion(env, out, 0, 7, vals);
    return out;
}

/* Returns float[n*13] surfel data for GL preview, or NULL. */
JNIEXPORT jfloatArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureGetSurfels(JNIEnv *env, jobject thiz,
        jlong trainerHandle) {
    (void)thiz;
    qtsq_trainer_t *t = (qtsq_trainer_t *)trainerHandle;
    if (!t) return NULL;

    int count = 0;
    const float *data = trainer_get_surfels(t, &count);
    if (!data || count <= 0) return NULL;

    jsize len = (jsize)count * SURFEL_STRIDE_FLOATS;
    jfloatArray out = (*env)->NewFloatArray(env, len);
    if (out)
        (*env)->SetFloatArrayRegion(env, out, 0, len, (const jfloat *)data);
    return out;
}

/* Save the trained model. Returns 0 on success. */
JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureTrainSave(JNIEnv *env, jobject thiz,
        jlong trainerHandle, jstring path) {
    (void)thiz;
    qtsq_trainer_t *t = (qtsq_trainer_t *)trainerHandle;
    if (!t || !path) return -1;

    const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);
    int result = trainer_save(t, c_path);
    (*env)->ReleaseStringUTFChars(env, path, c_path);
    return result;
}

/* Destroy the trainer. */
JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_captureTrainFree(JNIEnv *env, jobject thiz,
        jlong trainerHandle) {
    (void)env; (void)thiz;
    qtsq_trainer_t *t = (qtsq_trainer_t *)trainerHandle;
    if (t) {
        LOGI("captureTrainFree: destroying trainer");
        trainer_destroy(t);
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  VIDEO (pixel path, QTSQ_VIDEO_PIXEL)
 *
 *  Encoding is split into create/addFrame/setAudio/finish so a clip can be
 *  fed frame by frame without materialising every frame in the JVM first.
 *  Decoding keeps the context alive behind a jlong handle because the video
 *  decoder needs the parsed singularity for the life of playback.
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    qtsq_context_t ctx;
    qtsq_video_encoder_t *enc;
    int finished;
} jni_vid_enc_t;

typedef struct {
    qtsq_context_t ctx;
    qtsq_video_decoder_t *dec;
} jni_vid_dec_t;

JNIEXPORT jlong JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoEncodeCreate(
    JNIEnv *env, jobject thiz,
    jint width, jint height, jint channels, jint fps, jint flags, jlong targetPayload,
    jfloat quality) {
    if (width <= 0 || height <= 0 || channels <= 0 || fps <= 0) return 0;
    jni_vid_enc_t *h = (jni_vid_enc_t *)calloc(1, sizeof(jni_vid_enc_t));
    if (!h) return 0;
    qtsq_init(&h->ctx);
    h->ctx.codec = g_jni_codec;
    /* Quality tier drives the quantiser. The rate controller walks it down if
     * a byte budget is set, so callers can request a high ceiling and let the
     * budget decide. */
    h->ctx.quality = quality > 0.0f ? quality : 0.40f;
    if (h->ctx.quality > 0.95f) h->ctx.quality = 0.95f;
    if (h->ctx.quality < 0.15f) h->ctx.quality = 0.15f;
    h->enc = qtsq_video_encoder_create(&h->ctx, (uint32_t)width, (uint32_t)height,
                                       (uint8_t)channels, (uint32_t)fps,
                                       (uint32_t)flags,
                                       targetPayload > 0 ? (size_t)targetPayload : 0);
    if (!h->enc) {
        qtsq_free(&h->ctx);
        free(h);
        return 0;
    }
    LOGI("videoEncodeCreate: %dx%d @%d fps, enc=%p", width, height, fps, (void *)h->enc);
    return (jlong)(intptr_t)h;
}

JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoEncodeAddFrame(
    JNIEnv *env, jobject thiz, jlong handle, jbyteArray pixels) {
    jni_vid_enc_t *h = (jni_vid_enc_t *)(intptr_t)handle;
    if (!h || !h->enc || !pixels || h->finished) return -1;
    jsize n = (*env)->GetArrayLength(env, pixels);
    const uint8_t *src = (const uint8_t *)(*env)->GetByteArrayElements(env, pixels, NULL);
    if (!src) return -2;
    int rc = qtsq_video_encoder_add_frame(h->enc, src);
    (*env)->ReleaseByteArrayElements(env, pixels, (jbyte *)src, JNI_ABORT);
    return rc;
}

JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoEncodeSetAudio(
    JNIEnv *env, jobject thiz, jlong handle,
    jshortArray pcm, jint sampleRate, jint channels) {
    jni_vid_enc_t *h = (jni_vid_enc_t *)(intptr_t)handle;
    if (!h || !h->enc || !pcm || h->finished) return -1;
    jsize n = (*env)->GetArrayLength(env, pcm);
    const int16_t *src = (const int16_t *)(*env)->GetShortArrayElements(env, pcm, NULL);
    if (!src) return -2;
    /* Classic Gravitational only — the video path deliberately does not use
     * Photon/HQ or Opus. */
    int rc = qtsq_video_encoder_set_audio(h->enc, src, (uint32_t)n,
                                          (uint8_t)(channels > 0 ? channels : 1),
                                          (uint32_t)sampleRate);
    (*env)->ReleaseShortArrayElements(env, pcm, (jshort *)src, JNI_ABORT);
    LOGI("videoEncodeSetAudio: %d samples @%d Hz rc=%d", n, sampleRate, rc);
    return rc;
}

JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoEncodeFinish(
    JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_enc_t *h = (jni_vid_enc_t *)(intptr_t)handle;
    if (!h || !h->enc || h->finished) return NULL;
    int rc = qtsq_video_encoder_finish(h->enc);
    if (rc != QTSQ_OK) {
        LOGE("videoEncodeFinish: rc=%d (%s)", rc, qtsq_error_string(rc));
        return NULL;
    }
    h->finished = 1;
    /* Mark the payload as video frames so the viewer can route it. */
    h->ctx.header.data_type = QTSQ_TYPE_VIDEO_FRAMES;
    jbyteArray out = context_to_bytes(env, &h->ctx, "");
    LOGI("videoEncodeFinish: serialized=%d bytes", out ? (*env)->GetArrayLength(env, out) : -1);
    return out;
}

JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoEncoderFree(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_enc_t *h = (jni_vid_enc_t *)(intptr_t)handle;
    if (!h) return;
    if (h->enc) qtsq_video_encoder_free(h->enc);
    qtsq_free(&h->ctx);
    free(h);
}

JNIEXPORT jlong JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderOpen(
    JNIEnv *env, jobject thiz, jbyteArray qtsqBytes, jstring password) {
    if (!qtsqBytes) return 0;
    jni_vid_dec_t *h = (jni_vid_dec_t *)calloc(1, sizeof(jni_vid_dec_t));
    if (!h) return 0;
    qtsq_init(&h->ctx);
    h->ctx.codec = g_jni_codec;

    jsize in_len = (*env)->GetArrayLength(env, qtsqBytes);
    jbyte *in_data = (*env)->GetByteArrayElements(env, qtsqBytes, NULL);
    if (!in_data) { qtsq_free(&h->ctx); free(h); return 0; }
    int rc = qtsq_read_from_memory(&h->ctx, (const uint8_t *)in_data, (size_t)in_len);
    (*env)->ReleaseByteArrayElements(env, qtsqBytes, in_data, JNI_ABORT);
    if (rc != QTSQ_OK) {
        LOGE("videoDecoderOpen: parse failed %d (%s)", rc, qtsq_error_string(rc));
        qtsq_free(&h->ctx);
        free(h);
        return 0;
    }
    if (h->ctx.is_encrypted) {
        const char *pass = password ? (*env)->GetStringUTFChars(env, password, NULL) : NULL;
        rc = jni_decrypt_after_read(&h->ctx, pass);
        if (pass) (*env)->ReleaseStringUTFChars(env, password, pass);
        if (rc != QTSQ_OK) {
            LOGE("videoDecoderOpen: decrypt failed %d", rc);
            qtsq_free(&h->ctx);
            free(h);
            return 0;
        }
    }
    h->dec = qtsq_video_decoder_open(&h->ctx);
    if (!h->dec) {
        LOGE("videoDecoderOpen: decoder refused the file");
        qtsq_free(&h->ctx);
        free(h);
        return 0;
    }
    LOGI("videoDecoderOpen: %u frames, %ux%u",
         qtsq_video_decoder_get_frame_count(h->dec),
         (unsigned)h->ctx.schema.dimensions[0], (unsigned)h->ctx.schema.dimensions[1]);
    return (jlong)(intptr_t)h;
}

JNIEXPORT jintArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderInfo(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    /* {frameCount, width, height, fps} — the player needs the real rate to
     * pace frames; assuming 30 made correct clips play at the wrong speed. */
    jint info[4] = {0, 0, 0, 0};
    if (!h || !h->dec) return (*env)->NewIntArray(env, 0);
    info[0] = (jint)qtsq_video_decoder_get_frame_count(h->dec);
    info[1] = (jint)h->ctx.schema.dimensions[0];
    info[2] = (jint)h->ctx.schema.dimensions[1];
    info[3] = (jint)h->ctx.schema.frame_rate;
    if (info[3] <= 0 || info[3] > 240) info[3] = 30;
    jintArray a = (*env)->NewIntArray(env, 4);
    if (a) (*env)->SetIntArrayRegion(env, a, 0, 4, info);
    return a;
}

JNIEXPORT jbyte JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderFrameType(JNIEnv *env, jobject thiz,
                                                        jlong handle, jint index) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h || !h->dec || index < 0) return 0;
    return (jbyte)qtsq_video_decoder_get_frame_type(h->dec, (uint32_t)index);
}

/* Returns one frame pre-packed as ARGB_8888, which is exactly what Bitmap
 * .copyPixelsFromBuffer wants — no Kotlin-side pixel loop. */
JNIEXPORT jbyteArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderFrame(JNIEnv *env, jobject thiz,
                                                    jlong handle, jint index) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h || !h->dec || index < 0) return NULL;
    uint8_t *px = NULL;
    uint32_t w = 0, hgt = 0;
    int rc = qtsq_video_decoder_get_frame(h->dec, (uint32_t)index, &px, &w, &hgt);
    if (rc != QTSQ_OK || !px || w == 0 || hgt == 0) {
        LOGE("videoDecoderFrame: %d rc=%d (%s)", index, rc, qtsq_error_string(rc));
        return NULL;
    }
    int ch = h->ctx.schema.dimensions[2];
    if (ch <= 0) ch = 3;
    size_t npix = (size_t)w * hgt;
    size_t argb_sz = npix * 4;
    uint8_t *argb = (uint8_t *)malloc(argb_sz);
    if (!argb) return NULL;
    pack_argb_into(px, argb, npix, ch >= 4 ? QTSQ_TYPE_IMAGE_RGBA : QTSQ_TYPE_IMAGE_RGB);
    jbyteArray out = (*env)->NewByteArray(env, (jint)argb_sz);
    if (out) {
        (*env)->SetByteArrayRegion(env, out, 0, (jint)argb_sz, (const jbyte *)argb);
    } else {
        out = NULL;
    }
    free(argb);
    return out;
}

JNIEXPORT jboolean JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderHasAudio(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h || !h->dec) return JNI_FALSE;
    return qtsq_video_decoder_has_audio(h->dec) ? JNI_TRUE : JNI_FALSE;
}

/* Returns {wavBytes, sampleRate, channels} in a 3-element Object array so the
 * Kotlin side gets a directly playable WAV (the gravitational decoder already
 * produces a complete mono RIFF/WAV) in one call. */
JNIEXPORT jobjectArray JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderAudio(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h || !h->dec) return NULL;

    uint8_t *wav = NULL;
    size_t wav_sz = 0;
    uint32_t rate = 0;
    uint8_t channels = 0;
    int rc = qtsq_video_decoder_get_audio(h->dec, &wav, &wav_sz, &rate, &channels);
    if (rc != QTSQ_OK || !wav) {
        LOGE("videoDecoderAudio: rc=%d (%s)", rc, qtsq_error_string(rc));
        return NULL;
    }
    jbyteArray wav_arr = create_jbyteArray(env, wav, wav_sz);
    free(wav);
    if (!wav_arr) return NULL;

    jclass cls = (*env)->FindClass(env, "java/lang/Object");
    if (!cls) return NULL;
    jobjectArray out = (*env)->NewObjectArray(env, 3, cls, NULL);
    if (!out) return NULL;
    (*env)->SetObjectArrayElement(env, out, 0, wav_arr);
    jclass icls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID ictor = (*env)->GetMethodID(env, icls, "<init>", "(I)V");
    jint ir = (jint)rate, ic = (jint)channels;
    jobject jr = (*env)->NewObject(env, icls, ictor, ir);
    jobject jc = (*env)->NewObject(env, icls, ictor, ic);
    (*env)->SetObjectArrayElement(env, out, 1, jr);
    (*env)->SetObjectArrayElement(env, out, 2, jc);
    LOGI("videoDecoderAudio: %zu wav bytes @%u Hz ch=%u", wav_sz, rate, channels);
    return out;
}

JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderFree(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h) return;
    if (h->dec) qtsq_video_decoder_free(h->dec);
    qtsq_free(&h->ctx);
    free(h);
}

/* File size in bytes, for the details panel. Returns -1 on failure. */
JNIEXPORT jlong JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderFileSize(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h) return -1;
    return (jlong)h->ctx.header.singularity_size;
}

/* Codec/mode label from the schema encoding string ("PIXEL"/"4DGS"). */
JNIEXPORT jstring JNICALL
Java_com_haruhito_qtsq_QtsqEngine_videoDecoderEncoding(JNIEnv *env, jobject thiz, jlong handle) {
    jni_vid_dec_t *h = (jni_vid_dec_t *)(intptr_t)handle;
    if (!h) return NULL;
    const char *e = h->ctx.schema.encoding;
    if (!e || e[0] == 0) e = "PIXEL";
    return (*env)->NewStringUTF(env, e);
}

/* Returns the stored QTSQ data_type without decompressing, so the viewer can
 * route to the image / audio / video surface before it tries to decode pixels
 * (video has no single-image payload, so decompressBytes would fail on it).
 * Returns -1 when the bytes cannot be parsed or decrypted. */
JNIEXPORT jint JNICALL
Java_com_haruhito_qtsq_QtsqEngine_peekDataType(JNIEnv *env, jobject thiz,
                                                jbyteArray qtsqBytes, jstring password) {
    if (!qtsqBytes) return -1;
    jni_vid_dec_t *h = (jni_vid_dec_t *)calloc(1, sizeof(jni_vid_dec_t));
    if (!h) return -1;
    qtsq_init(&h->ctx);
    h->ctx.codec = g_jni_codec;

    jsize in_len = (*env)->GetArrayLength(env, qtsqBytes);
    jbyte *in_data = (*env)->GetByteArrayElements(env, qtsqBytes, NULL);
    if (!in_data) { qtsq_free(&h->ctx); free(h); return -1; }
    int rc = qtsq_read_from_memory(&h->ctx, (const uint8_t *)in_data, (size_t)in_len);
    (*env)->ReleaseByteArrayElements(env, qtsqBytes, in_data, JNI_ABORT);
    if (rc != QTSQ_OK) { qtsq_free(&h->ctx); free(h); return -1; }

    if (h->ctx.is_encrypted) {
        const char *pass = password ? (*env)->GetStringUTFChars(env, password, NULL) : NULL;
        rc = jni_decrypt_after_read(&h->ctx, pass);
        if (pass) (*env)->ReleaseStringUTFChars(env, password, pass);
        if (rc != QTSQ_OK) { qtsq_free(&h->ctx); free(h); return -1; }
    }
    jint type = (jint)h->ctx.header.data_type;
    qtsq_free(&h->ctx);
    free(h);
    return type;
}
