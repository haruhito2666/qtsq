package com.haruhito.qtsq

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Phase 2d: Android Keystore bridge for QTSQ device key encryption.
 *
 * Generates an AES-256 key in the AndroidKeyStore (hardware-backed on
 * devices that support it). The key never leaves the secure enclave.
 * Used to encrypt/decrypt QTSQ device secret key files before they're
 * written to app-private file storage.
 *
 * Called from native code via JNI (see qtsq_keystore_set_jni_bridge).
 */
object KeystoreHelper {
    private const val KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "qtsq_device_key_v1"
    private const val GCM_TAG_BITS = 128
    private const val GCM_IV_LEN = 12

    private val keyStore: KeyStore by lazy {
        KeyStore.getInstance(KEYSTORE).apply { load(null) }
    }

    /** Get or create the AndroidKeyStore AES-256 key. */
    private fun getOrCreateKey(): SecretKey {
        keyStore.getEntry(KEY_ALIAS, null)?.let { entry ->
            return (entry as KeyStore.SecretKeyEntry).secretKey
        }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        val spec = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .setUserAuthenticationRequired(false)
            .build()
        gen.init(spec)
        return gen.generateKey()
    }

    /** Encrypt plaintext bytes. Output format: [IV(12)][ciphertext+tag]. */
    fun encrypt(plaintext: ByteArray): ByteArray {
        val key = getOrCreateKey()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key)
        val iv = cipher.iv
        val ct = cipher.doFinal(plaintext)
        return iv + ct
    }

    /** Decrypt bytes produced by encrypt(). Input: [IV(12)][ciphertext+tag]. */
    fun decrypt(ciphertext: ByteArray): ByteArray {
        require(ciphertext.size > GCM_IV_LEN) { "ciphertext too short" }
        val key = getOrCreateKey()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        val iv = ciphertext.copyOfRange(0, GCM_IV_LEN)
        val ct = ciphertext.copyOfRange(GCM_IV_LEN, ciphertext.size)
        val spec = GCMParameterSpec(GCM_TAG_BITS, iv)
        cipher.init(Cipher.DECRYPT_MODE, key, spec)
        return cipher.doFinal(ct)
    }

    // ── JNI callbacks ──

    @JvmStatic
    fun jniEncrypt(plaintext: ByteArray, cap: Int): ByteArray {
        return encrypt(plaintext)
    }

    @JvmStatic
    fun jniDecrypt(ciphertext: ByteArray, cap: Int): ByteArray {
        return decrypt(ciphertext)
    }
}
