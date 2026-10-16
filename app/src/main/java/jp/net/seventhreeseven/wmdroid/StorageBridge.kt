package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.Settings as AndroidSettings
import java.io.File

/**
 * Phone storage passthrough: decides which host folders the guest sees as \Storage Card.
 *  - private: a folder owned by the app (no permission needed)
 *  - full:    all of the phone's shared storage
 *  - folders: chosen folders, each shown as a top-level folder of \Storage Card
 * The last two need Android's "All files access", because the emulator core uses plain file paths.
 */
object StorageBridge {
    const val MODE_PRIVATE = "private"
    const val MODE_FULL = "full"
    const val MODE_FOLDERS = "folders"

    fun privateDir(ctx: Context): File = File(ctx.getExternalFilesDir(null), "StorageCard").apply { mkdirs() }

    // Android 8-10 have no "All files access": the Storage permission (legacy storage on 10) gives
    // the same plain-path access, and shared storage is writable once it is granted.
    fun hasAllFilesAccess(): Boolean =
        if (android.os.Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager()
        else Environment.getExternalStorageDirectory().canWrite()

    fun allFilesAccessIntent(ctx: Context) =
        if (android.os.Build.VERSION.SDK_INT >= 30)
            Intent(AndroidSettings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${ctx.packageName}"))
        else  // App info, where Permissions > Storage is granted.
            Intent(AndroidSettings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${ctx.packageName}"))

    /** (guest folder name, host path) pairs; a single "" name maps the share root itself. */
    fun mounts(ctx: Context, settings: Settings): List<Pair<String, String>> {
        val fallback = listOf("" to privateDir(ctx).path)
        return when (settings.storageMode) {
            MODE_FULL ->
                if (hasAllFilesAccess()) listOf("" to Environment.getExternalStorageDirectory().path) else fallback
            // A real root folder (files can be saved at the top of the card); the chosen folders
            // appear in it as subfolders (see folderOverlays).
            MODE_FOLDERS -> if (!hasAllFilesAccess()) fallback else listOf("" to folderRoot(ctx, settings).path)
            else -> fallback
        }
    }

    /** Chosen-folders mode's root: the folder picked for it, else WMDroid's own. */
    fun folderRoot(ctx: Context, settings: Settings): File =
        settings.storageRoot.takeIf { it.isNotEmpty() }?.let { File(it) }?.takeIf { it.isDirectory } ?: privateDir(ctx)

    /** Chosen-folders mode: each folder shown as a subfolder at the card's root (by its name). */
    fun folderOverlays(ctx: Context, settings: Settings): List<Pair<String, String>> {
        if (settings.storageMode != MODE_FOLDERS || !hasAllFilesAccess()) return emptyList()
        val root = folderRoot(ctx, settings).canonicalPath
        val dirs = settings.storageFolders.map { File(it) }.filter { it.isDirectory && it.canonicalPath != root }
        val names = settings.storageFolderNames
        return uniqueNames(dirs, names).zip(dirs.map { it.path })
    }

    /** Names on the card: the given name, else the folder's own; duplicates get " (2)" etc. */
    fun uniqueNames(dirs: List<File>, names: Map<String, String> = emptyMap()): List<String> {
        val seen = HashMap<String, Int>()
        return dirs.map { d ->
            val base = names[d.path]?.takeIf { it.isNotBlank() } ?: d.name.ifEmpty { "Storage" }
            val n = seen.merge(base.lowercase(), 1, Int::plus)!!
            if (n == 1) base else "$base ($n)"
        }
    }

    /** Maps a folder picked with ACTION_OPEN_DOCUMENT_TREE to a file path on primary storage. */
    fun treeUriToPath(uri: Uri): String? {
        val id = runCatching { DocumentsContract.getTreeDocumentId(uri) }.getOrNull() ?: return null
        val parts = id.split(":", limit = 2)
        if (parts.size != 2) return null
        val root = if (parts[0] == "primary") Environment.getExternalStorageDirectory().path else "/storage/${parts[0]}"
        return if (parts[1].isEmpty()) root else "$root/${parts[1]}"
    }
}
