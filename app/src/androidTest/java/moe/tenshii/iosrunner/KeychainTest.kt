package moe.tenshii.iosrunner

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.KeyStore

@RunWith(AndroidJUnit4::class)
class KeychainTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val directory = File(context.filesDir, "keychains")

    @After
    fun removeTestItems() {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        listOf(FIRST, SECOND).forEach { app ->
            File(directory, "$app.bin").delete()
            store.deleteEntry("iosrunner.keychain.$app")
        }
    }

    private fun item(account: String, secret: String) = JSONObject()
        .put("kSecClass", tagged("kSecClassGenericPassword"))
        .put("kSecAttrAccount", tagged(account))
        .put("kSecValueData", tagged(secret))

    private fun tagged(text: String) = JSONObject().put("s", text)

    private fun call(keychain: Keychain, op: String, query: JSONObject) =
        JSONObject(keychain.call(JSONObject().put("op", op).put("query", query)))

    private fun add(keychain: Keychain, account: String, secret: String) = call(keychain, "add", item(account, secret)).getInt("status")

    private fun find(keychain: Keychain, account: String) = call(keychain, "find", JSONObject().put("kSecAttrAccount", tagged(account)))

    @Test
    fun storesAndReadsBackAcrossInstances() {
        assertEquals(0, add(Keychain(context, FIRST), "user", SECRET))
        val found = find(Keychain(context, FIRST), "user")
        assertEquals(0, found.getInt("status"))
        assertEquals(SECRET, found.getJSONArray("items").getJSONObject(0).getJSONObject("kSecValueData").getString("s"))
    }

    @Test
    fun oneAppCannotFindAnothersItems() {
        add(Keychain(context, FIRST), "user", SECRET)
        assertEquals(NOT_FOUND, find(Keychain(context, SECOND), "user").getInt("status"))
    }

    @Test
    fun anAccessGroupDoesNotReachAnotherApp() {
        add(Keychain(context, FIRST), "user", SECRET)
        val query = JSONObject().put("kSecAttrAccount", tagged("user")).put("kSecAttrAccessGroup", tagged(FIRST))
        assertEquals(NOT_FOUND, call(Keychain(context, SECOND), "find", query).getInt("status"))
    }

    @Test
    fun nothingIsStoredInPlaintext() {
        add(Keychain(context, FIRST), "user", SECRET)
        val stored = String(File(directory, "$FIRST.bin").readBytes(), Charsets.ISO_8859_1)
        assertFalse(stored.contains(SECRET))
        assertFalse(stored.contains("kSecAttrAccount"))
    }

    @Test
    fun anotherAppsFileCannotBeDecrypted() {
        add(Keychain(context, FIRST), "user", SECRET)
        add(Keychain(context, SECOND), "other", "other-secret")
        File(directory, "$FIRST.bin").copyTo(File(directory, "$SECOND.bin"), overwrite = true)
        assertEquals(DECODE_FAILED, find(Keychain(context, SECOND), "user").getInt("status"))
    }

    @Test
    fun duplicatesAreRefusedAndDeleteRemoves() {
        val keychain = Keychain(context, FIRST)
        assertEquals(0, add(keychain, "user", SECRET))
        assertEquals(DUPLICATE, add(keychain, "user", "again"))
        assertEquals(0, call(keychain, "delete", JSONObject().put("kSecAttrAccount", tagged("user"))).getInt("status"))
        assertEquals(NOT_FOUND, find(keychain, "user").getInt("status"))
        assertTrue(File(directory, "$FIRST.bin").exists())
    }

    private companion object {
        const val FIRST = "test.keychain.first"
        const val SECOND = "test.keychain.second"
        const val SECRET = "correct-horse-battery"
        const val NOT_FOUND = -25300
        const val DUPLICATE = -25299
        const val DECODE_FAILED = -26275
    }
}
