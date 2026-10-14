/*
 * QTSQ — Android Keystore JNI Bridge (Phase 2d)
 *
 * Provides C-callable encrypt/decrypt functions that call into the
 * Java KeystoreHelper via JNI. These are registered with the QTSQ
 * keystore at app startup via qtsq_keystore_set_jni_bridge().
 *
 * The key never leaves the Android Keystore hardware; this module only
 * shuffles ciphertext/plaintext between C and Java.
 *
 * Author: Haruhito
 */
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <android/log.h>
#include "qtsq_keystore.h"

#define KSLOG(...) __android_log_print(ANDROID_LOG_ERROR, "QTSQ_KSJNI", __VA_ARGS__)

/* If a Java exception is pending, dump its stack to logcat and clear it so
 * the failure reason is visible instead of a bare -1. */
static void ks_log_clear_exception(JNIEnv *env, const char *where) {
    if ((*env)->ExceptionCheck(env)) {
        KSLOG("%s: Java exception follows:", where);
        (*env)->ExceptionDescribe(env);   /* prints stack trace to logcat */
        (*env)->ExceptionClear(env);
    }
}

/* Bridge callbacks implemented below; forward-declared so nativeInitKeystore
 * can register them with the C keystore. */
int qtsq_jni_encrypt_impl(const uint8_t *plaintext, size_t len,
                          uint8_t *ciphertext, size_t cap, size_t *out_len);
int qtsq_jni_decrypt_impl(const uint8_t *ciphertext, size_t len,
                          uint8_t *plaintext, size_t cap, size_t *out_len);

/* Cached JNI env — set by qtsq_android_init_jni().
 * In a real app this would be obtained from the JavaVM via
 * (*vm)->GetEnv(vm, &env, JNI_VERSION_1_6) on each call. */
static JavaVM *g_jvm = NULL;
static jobject g_global_ctx = NULL; /* application context */

/* Class and method IDs, cached for performance */
static jclass g_helper_class = NULL;
static jmethodID g_encrypt_method = NULL;
static jmethodID g_decrypt_method = NULL;

/* Initialize the JNI bridge. Called from Java (e.g. Application.onCreate)
 * with the application context. */
JNIEXPORT void JNICALL
Java_com_haruhito_qtsq_QtsqEngine_nativeInitKeystore(JNIEnv *env, jobject thiz) {
    /* Get the JavaVM */
    (*env)->GetJavaVM(env, &g_jvm);

    /* Find the KeystoreHelper class */
    jclass local_class = (*env)->FindClass(env, "com/haruhito/qtsq/KeystoreHelper");
    if (!local_class) return;
    g_helper_class = (jclass)(*env)->NewGlobalRef(env, local_class);
    (*env)->DeleteLocalRef(env, local_class);

    /* Find the static encrypt/decrypt methods */
    g_encrypt_method = (*env)->GetStaticMethodID(env, g_helper_class,
        "jniEncrypt", "([BI)[B");
    g_decrypt_method = (*env)->GetStaticMethodID(env, g_helper_class,
        "jniDecrypt", "([BI)[B");

    /* Register the JNI callbacks with the QTSQ keystore */
    qtsq_keystore_set_jni_bridge(
        qtsq_jni_encrypt_impl,
        qtsq_jni_decrypt_impl
    );
}

/* JNI encrypt callback: called from C keystore to encrypt key file bytes.
 * Returns 0 on success, -1 setup failure, -2 Java exception (logged). */
int qtsq_jni_encrypt_impl(const uint8_t *plaintext, size_t len,
                           uint8_t *ciphertext, size_t cap, size_t *out_len) {
    if (!g_jvm || !g_helper_class || !g_encrypt_method) {
        KSLOG("encrypt: bridge not initialized (jvm=%p cls=%p m=%p)",
              (void *)g_jvm, (void *)g_helper_class, (void *)g_encrypt_method);
        return -1;
    }

    JNIEnv *env;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc != JNI_OK) {
        /* Try AttachCurrentThread for background threads */
        rc = (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        if (rc != JNI_OK) return -1;
    }

    /* Create Java byte array from plaintext */
    jbyteArray jplain = (*env)->NewByteArray(env, (jsize)len);
    if (!jplain) {
        ks_log_clear_exception(env, "encrypt/NewByteArray");
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -2;
    }
    (*env)->SetByteArrayRegion(env, jplain, 0, (jsize)len, (const jbyte *)plaintext);

    /* Call KeystoreHelper.jniEncrypt */
    jbyteArray jresult = (jbyteArray)(*env)->CallStaticObjectMethod(
        env, g_helper_class, g_encrypt_method, jplain, (jint)cap);
    (*env)->DeleteLocalRef(env, jplain);

    if (!jresult) {
        ks_log_clear_exception(env, "encrypt/jniEncrypt");   /* ← the real reason lands in logcat */
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -2;
    }

    /* Copy result back to C buffer */
    jsize result_len = (*env)->GetArrayLength(env, jresult);
    if ((size_t)result_len > cap) {
        (*env)->DeleteLocalRef(env, jresult);
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -1;
    }
    (*env)->GetByteArrayRegion(env, jresult, 0, result_len, (jbyte *)ciphertext);
    (*env)->DeleteLocalRef(env, jresult);
    *out_len = (size_t)result_len;

    if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
    return 0;
}

/* JNI decrypt callback: called from C keystore to decrypt key file bytes.
 * Returns 0 on success, -1 setup failure, -2 Java exception (logged). */
int qtsq_jni_decrypt_impl(const uint8_t *ciphertext, size_t len,
                           uint8_t *plaintext, size_t cap, size_t *out_len) {
    if (!g_jvm || !g_helper_class || !g_decrypt_method) {
        KSLOG("decrypt: bridge not initialized");
        return -1;
    }

    JNIEnv *env;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc != JNI_OK) {
        rc = (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        if (rc != JNI_OK) return -1;
    }

    jbyteArray jcipher = (*env)->NewByteArray(env, (jsize)len);
    if (!jcipher) {
        ks_log_clear_exception(env, "decrypt/NewByteArray");
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -2;
    }
    (*env)->SetByteArrayRegion(env, jcipher, 0, (jsize)len, (const jbyte *)ciphertext);

    jbyteArray jresult = (jbyteArray)(*env)->CallStaticObjectMethod(
        env, g_helper_class, g_decrypt_method, jcipher, (jint)cap);
    (*env)->DeleteLocalRef(env, jcipher);

    if (!jresult) {
        ks_log_clear_exception(env, "decrypt/jniDecrypt");
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -2;
    }

    jsize result_len = (*env)->GetArrayLength(env, jresult);
    if ((size_t)result_len > cap) {
        (*env)->DeleteLocalRef(env, jresult);
        if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -1;
    }
    (*env)->GetByteArrayRegion(env, jresult, 0, result_len, (jbyte *)plaintext);
    (*env)->DeleteLocalRef(env, jresult);
    *out_len = (size_t)result_len;

    if (rc == JNI_EDETACHED) (*g_jvm)->DetachCurrentThread(g_jvm);
    return 0;
}
