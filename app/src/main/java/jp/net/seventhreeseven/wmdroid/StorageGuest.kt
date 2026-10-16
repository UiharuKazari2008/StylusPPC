package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.os.Environment
import java.io.File

/**
 * The Storage Card page inside Windows Mobile (guest/Helper/StorageForm.cs): the app publishes the
 * card's setup, lists phone folders while the guest browses, and applies the guest's new setup.
 *
 * Mailbox files (UTF-8, in the helper's folder):
 *   storage.ini      app -> guest  mode=, root=, access=0|1, folder=<name>\t<path> lines
 *   hostdir.txt      app -> guest  path=<listed folder>, then "d <name>" per subfolder (or err=<why>)
 *   storage_new.ini  guest -> app  the same format as storage.ini, applied on "storage apply"
 * Messages from the guest: "hostdir <path>" (empty = the phone's storage volumes), "storage apply",
 * "open allfiles" (Android's All files access screen).
 */
object StorageGuest {
    fun write(box: File, ctx: Context, settings: Settings) {
        val names = StorageBridge.uniqueNames(settings.storageFolders.map { File(it) }, settings.storageFolderNames)
        val text = buildString {
            append("mode=").append(settings.storageMode).append("\r\n")
            append("root=").append(settings.storageRoot).append("\r\n")
            append("access=").append(if (StorageBridge.hasAllFilesAccess()) "1" else "0").append("\r\n")
            settings.storageFolders.forEachIndexed { i, p -> append("folder=").append(names[i]).append('\t').append(p).append("\r\n") }
        }
        write(File(box, "storage.ini"), text)
    }

    /** "hostdir <path>": the subfolders of a phone folder (the storage volumes for an empty path). */
    fun listDir(box: File, ctx: Context, path: String) {
        val text = buildString {
            if (!StorageBridge.hasAllFilesAccess()) {
                append("err=Allow All files access for PocketStylus on the phone first.\r\n")
                return@buildString
            }
            if (path.isBlank()) {
                append("path=\r\n")
                val roots = LinkedHashSet<String>()
                roots += Environment.getExternalStorageDirectory().path
                File("/storage").listFiles()?.filter { it.isDirectory && it.canRead() && it.name != "emulated" && it.name != "self" }
                    ?.forEach { roots += it.path }
                for (r in roots) append("d ").append(r).append("\r\n")
                return@buildString
            }
            val dir = File(path)
            val subs = dir.listFiles()?.filter { it.isDirectory && !it.name.startsWith(".") }?.sortedBy { it.name.lowercase() }
            if (subs == null) { append("err=Can't open ").append(path).append("\r\n"); return@buildString }
            append("path=").append(dir.path).append("\r\n")
            for (s in subs) append("d ").append(s.name).append("\r\n")
        }
        write(File(box, "hostdir.txt"), text)
    }

    /** "storage apply": takes storage_new.ini; true if the card changed (Windows Mobile restarts). */
    fun apply(box: File, settings: Settings): Boolean {
        val f = File(box, "storage_new.ini")
        val lines = runCatching { f.readText().removePrefix("﻿").lines() }.getOrNull() ?: return false
        f.delete()
        var mode = settings.storageMode
        var root = settings.storageRoot
        val folders = ArrayList<String>()
        val names = HashMap<String, String>()
        for (raw in lines) {
            val l = raw.trimEnd('\r')
            when {
                l.startsWith("mode=") -> l.removePrefix("mode=").let {
                    if (it in listOf(StorageBridge.MODE_PRIVATE, StorageBridge.MODE_FULL, StorageBridge.MODE_FOLDERS)) mode = it
                }
                l.startsWith("root=") -> root = l.removePrefix("root=").takeIf { it.isEmpty() || File(it).isDirectory } ?: root
                l.startsWith("folder=") -> {
                    val v = l.removePrefix("folder=")
                    val tab = v.indexOf('\t')
                    if (tab > 0) {
                        val path = v.substring(tab + 1)
                        if (File(path).isDirectory && path !in folders) {
                            folders += path
                            val name = v.substring(0, tab).filter { it !in "\\/:*?\"<>|" }.trim()
                            if (name.isNotEmpty() && name != File(path).name) names[path] = name
                        }
                    }
                }
            }
        }
        val changed = mode != settings.storageMode || root != settings.storageRoot ||
            folders != settings.storageFolders || names != settings.storageFolderNames
        if (!changed) return false
        settings.storageMode = mode
        settings.storageRoot = root
        settings.storageFolders = folders
        settings.storageFolderNames = names
        return true
    }

    private fun write(f: File, text: String) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(text)
        tmp.renameTo(f)
    }
}
