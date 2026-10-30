package moe.tenshii.iosrunner

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException
import java.util.zip.ZipInputStream

data class InstalledApp(
    val directory: File,
    val name: String,
    val executable: String,
    val version: String,
    val minimumOs: String,
    val iconFile: File?,
)

/** Stores imported bundles under the app's private files, one directory per bundle identifier. */
class AppLibrary(context: Context) {
    private val root = File(context.filesDir, "apps").apply { mkdirs() }
    private val staging = File(context.cacheDir, "import")

    fun list(): List<InstalledApp> =
        root.listFiles().orEmpty().filter { it.isDirectory }.mapNotNull { runCatching { describe(it) }.getOrNull() }
            .sortedBy { it.name.lowercase() }

    fun remove(app: InstalledApp) {
        app.directory.deleteRecursively()
    }

    fun find(directoryName: String): InstalledApp? = runCatching { describe(File(root, directoryName)) }.getOrNull()

    @Throws(IOException::class)
    fun import(context: Context, uri: Uri): InstalledApp {
        staging.deleteRecursively()
        staging.mkdirs()
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException("the file could not be opened")
            input.use { extract(ZipInputStream(it)) }
            val bundle = staging.listFiles().orEmpty().firstOrNull { it.isDirectory }
                ?: throw IOException("it has no Payload/*.app folder, so it is not an IPA")
            val identifier = infoPlist(bundle)["CFBundleIdentifier"] as? String
                ?: throw IOException("its Info.plist has no bundle identifier")
            val safeName = identifier.replace(Regex("[^A-Za-z0-9._-]"), "_")
            val destination = File(root, safeName)
            destination.deleteRecursively()
            if (!bundle.renameTo(destination)) throw IOException("the app could not be stored")
            return describe(destination)
        } finally {
            staging.deleteRecursively()
        }
    }

    private fun extract(zip: ZipInputStream) {
        val stagingRoot = staging.canonicalFile
        while (true) {
            val entry = zip.nextEntry ?: break
            val match = BUNDLE_ENTRY.matchEntire(entry.name) ?: continue
            val target = File(stagingRoot, match.groupValues[1]).canonicalFile
            if (!target.path.startsWith(stagingRoot.path + File.separator)) {
                throw IOException("it contains an unsafe path: ${entry.name}")
            }
            if (entry.isDirectory) {
                target.mkdirs()
            } else {
                target.parentFile?.mkdirs()
                target.outputStream().use { zip.copyTo(it) }
            }
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun infoPlist(bundle: File): Map<String, Any?> {
        val file = File(bundle, "Info.plist")
        if (!file.isFile) throw IOException("it has no Info.plist")
        return PropertyList.parse(file.readBytes()) as? Map<String, Any?> ?: throw IOException("its Info.plist is not readable")
    }

    private fun describe(directory: File): InstalledApp {
        val info = infoPlist(directory)
        val executable = info["CFBundleExecutable"] as? String ?: throw IOException("its Info.plist names no executable")
        if (executable.contains('/') || !File(directory, executable).isFile) throw IOException("its executable is missing")
        val icon = directory.listFiles().orEmpty()
            .filter { it.name.startsWith("AppIcon") && it.name.endsWith(".png") }
            .maxByOrNull { it.length() }
        return InstalledApp(
            directory = directory,
            name = info["CFBundleDisplayName"] as? String ?: info["CFBundleName"] as? String ?: directory.name,
            executable = executable,
            version = info["CFBundleShortVersionString"] as? String ?: "?",
            minimumOs = info["MinimumOSVersion"] as? String ?: "?",
            iconFile = icon,
        )
    }

    private companion object {
        val BUNDLE_ENTRY = Regex("""Payload/(.+\.app(?:/.*)?)""")
    }
}
