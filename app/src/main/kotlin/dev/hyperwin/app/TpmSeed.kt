package dev.hyperwin.app

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.spec.GCMParameterSpec

/** The TPM seed never sits in plaintext on disk. Android Keystore wraps it.
 *  The engine still runs in this process: Keystore holds the wrapping key, including
 *  a hardware-backed key when the device has one. It is not a TPM command engine inside the TEE.
 */
object TpmSeed {
    private const val ALIAS = "hyperwin-tpm-seed"
    private const val GCM = "AES/GCM/NoPadding"

    fun wrap(seed: ByteArray): ByteArray {
        require(seed.size == 32)
        val key = key()
        val cipher = Cipher.getInstance(GCM)
        cipher.init(Cipher.ENCRYPT_MODE, key)
        val body = cipher.doFinal(seed)
        val iv = cipher.iv
        return byteArrayOf(iv.size.toByte()) + iv + body
    }

    fun unwrap(blob: ByteArray): ByteArray {
        val ivLen = blob[0].toInt() and 0xff
        val iv = blob.copyOfRange(1, 1 + ivLen)
        val body = blob.copyOfRange(1 + ivLen, blob.size)
        val cipher = Cipher.getInstance(GCM)
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
        return cipher.doFinal(body)
    }

    private fun key() = store().getKey(ALIAS, null) ?: error("missing $ALIAS")

    private fun store(): KeyStore {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (!ks.containsAlias(ALIAS)) {
            val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
            val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
            gen.init(spec)
            gen.generateKey()
        }
        return ks
    }
}
