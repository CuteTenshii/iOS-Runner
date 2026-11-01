package moe.tenshii.iosrunner

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.security.GeneralSecurityException
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * One app's Keychain: its items, encrypted under a key that only Android Keystore holds.
 *
 * [appId] comes from the app this host launched, never from a guest request, so no request can name another app's
 * key or file. Access groups are stored but never widen a lookup.
 */
class Keychain(context: Context, private val appId: String) {
    private val file = AtomicFile(File(File(context.filesDir, DIRECTORY).apply { mkdirs() }, "$appId.bin"))
    private val alias = "$KEY_PREFIX$appId"
    private var items: JSONArray? = null

    fun call(request: JSONObject): String {
        val query = request.optJSONObject("query") ?: JSONObject()
        return when (request.getString("op")) {
            "add" -> add(query)
            "find" -> find(query, request.optBoolean("all"))
            "update" -> update(query, request.optJSONObject("changes") ?: JSONObject())
            "delete" -> delete(query)
            else -> status(STATUS_PARAM)
        }
    }

    private fun add(attributes: JSONObject): String {
        val all = load() ?: return status(STATUS_DECODE)
        if (matching(all, identity(attributes)).isNotEmpty()) return status(STATUS_DUPLICATE)
        all.put(attributes)
        return save(all)
    }

    private fun find(query: JSONObject, wantsAll: Boolean): String {
        val all = load() ?: return status(STATUS_DECODE)
        val found = matching(all, query)
        if (found.isEmpty()) return status(STATUS_NOT_FOUND)
        val result = JSONArray()
        (if (wantsAll) found else found.take(1)).forEach { result.put(all.getJSONObject(it)) }
        return JSONObject().put("status", STATUS_OK).put("items", result).toString()
    }

    private fun update(query: JSONObject, changes: JSONObject): String {
        val all = load() ?: return status(STATUS_DECODE)
        val found = matching(all, query)
        if (found.isEmpty()) return status(STATUS_NOT_FOUND)
        found.forEach { index ->
            val item = all.getJSONObject(index)
            changes.keys().forEach { key -> item.put(key, changes.get(key)) }
        }
        return save(all)
    }

    private fun delete(query: JSONObject): String {
        val all = load() ?: return status(STATUS_DECODE)
        val found = matching(all, query)
        if (found.isEmpty()) return status(STATUS_NOT_FOUND)
        found.sortedDescending().forEach { all.remove(it) }
        return save(all)
    }

    private fun matching(all: JSONArray, query: JSONObject): List<Int> = (0 until all.length()).filter { index ->
        val item = all.getJSONObject(index)
        query.keys().asSequence().all { key -> key in NOT_MATCHED || item.opt(key)?.toString() == query.get(key).toString() }
    }

    /** What makes an item unique: adding a second item with these same values is a duplicate. */
    private fun identity(attributes: JSONObject): JSONObject {
        val out = JSONObject()
        IDENTITY.forEach { key -> attributes.opt(key)?.let { out.put(key, it) } }
        return out
    }

    private fun load(): JSONArray? {
        items?.let { return it }
        val loaded = try {
            if (file.baseFile.exists()) JSONArray(String(decrypt(file.readFully()), Charsets.UTF_8)) else JSONArray()
        } catch (_: GeneralSecurityException) {
            null
        } catch (_: IOException) {
            null
        }
        items = loaded
        return loaded
    }

    private fun save(all: JSONArray): String {
        val stream = file.startWrite()
        return try {
            stream.write(encrypt(all.toString().toByteArray(Charsets.UTF_8)))
            file.finishWrite(stream)
            status(STATUS_OK)
        } catch (_: GeneralSecurityException) {
            file.failWrite(stream)
            status(STATUS_IO)
        } catch (_: IOException) {
            file.failWrite(stream)
            status(STATUS_IO)
        }
    }

    private fun encrypt(plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        cipher.updateAAD(appId.toByteArray())
        return cipher.iv + cipher.doFinal(plain)
    }

    private fun decrypt(sealed: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(TAG_BITS, sealed, 0, IV_BYTES))
        cipher.updateAAD(appId.toByteArray())
        return cipher.doFinal(sealed, IV_BYTES, sealed.size - IV_BYTES)
    }

    private fun secretKey(): SecretKey {
        val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        generator.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(KEY_BITS)
                .build(),
        )
        return generator.generateKey()
    }

    private fun status(code: Int) = JSONObject().put("status", code).toString()

    private companion object {
        const val DIRECTORY = "keychains"
        const val KEY_PREFIX = "iosrunner.keychain."
        const val KEYSTORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val KEY_BITS = 256
        const val TAG_BITS = 128
        const val IV_BYTES = 12
        const val STATUS_OK = 0
        const val STATUS_PARAM = -50
        const val STATUS_IO = -36
        const val STATUS_DUPLICATE = -25299
        const val STATUS_NOT_FOUND = -25300
        const val STATUS_DECODE = -26275
        val NOT_MATCHED = setOf("kSecAttrAccessGroup", "kSecAttrAccessible", "kSecAttrAccessControl", "kSecAttrSynchronizable", "kSecValueData")
        val IDENTITY = listOf("kSecClass", "kSecAttrService", "kSecAttrAccount", "kSecAttrServer", "kSecAttrPort", "kSecAttrProtocol", "kSecAttrPath", "kSecAttrAuthenticationType", "kSecAttrSecurityDomain")
    }
}
