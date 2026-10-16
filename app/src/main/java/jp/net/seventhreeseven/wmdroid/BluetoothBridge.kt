package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.database.ContentObserver
import android.net.Uri
import android.os.Handler
import android.provider.MediaStore
import android.util.Log
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Bluetooth file transfer through the phone (docs/research-bluetooth.md). Device Emulator has no
 * Bluetooth radio, so Windows Mobile's files travel through the helper's mailbox instead:
 *  - Sending: "Send via Bluetooth" in the guest copies a file to \Storage Card\.wmdroid\send and
 *    posts "share <name>"; [share] hands it to Android's Bluetooth sender (its device picker), or to
 *    the share sheet on phones without one.
 *  - Receiving: files the phone receives over Bluetooth (MediaStore Downloads rows owned by the
 *    Bluetooth app, saved in Download/) are copied to \Storage Card\Bluetooth, an overlay of
 *    [receiveDir], and announced to the helper with "received <name>", which shows a notification.
 *    Seeing other apps' downloads needs All files access.
 */
class BluetoothBridge(
    private val ctx: Context,
    private val settings: Settings,
    private val guest: GuestBridge,
    private val ui: Handler,
) {
    private val sendDir = guest.boxFile("send")
    private val worker = Executors.newSingleThreadExecutor()
    private val queued = AtomicBoolean(false)
    private val observer = object : ContentObserver(ui) {
        override fun onChange(selfChange: Boolean) = scan()
    }

    /** Watches for received files while the session exists. */
    fun start() {
        sendDir.mkdirs()
        cleanSendDir(maxAgeMs = DAY_MS)
        DOWNLOADS?.let { ctx.contentResolver.registerContentObserver(it, true, observer) }
        scan()
    }

    fun stop() {
        ctx.contentResolver.unregisterContentObserver(observer)
    }

    // ---- Sending ----

    /** Opens Android's Bluetooth sender for a file the helper put in the mailbox's send folder. */
    fun share(activity: Activity, name: String): Boolean {
        val file = File(sendDir, name)
        if (File(name).name != name || !file.isFile) {
            Log.w(TAG, "share: no such file $name")
            return false
        }
        cleanSendDir(maxAgeMs = HOUR_MS, keep = file)
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.files", file)
        val type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase())
            ?: "application/octet-stream"
        val send = Intent(Intent.ACTION_SEND).apply {
            setType(type)
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = ClipData.newRawUri(name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        // Straight to the Bluetooth device picker when the phone's Bluetooth app takes this type.
        // (com.android.bluetooth/.opp.BluetoothOppLauncherActivity on AOSP and Samsung.)
        val targets = activity.packageManager.queryIntentActivities(send, 0)
        val bt = targets.firstOrNull { it.activityInfo.packageName == "com.android.bluetooth" }
            ?: targets.firstOrNull { it.activityInfo.packageName.contains("bluetooth", ignoreCase = true) }
        val direct = bt?.let { Intent(send).setClassName(it.activityInfo.packageName, it.activityInfo.name) }
        for (intent in listOfNotNull(direct, Intent.createChooser(send, "Send $name"))) {
            try {
                activity.startActivity(intent)
                return true
            } catch (e: ActivityNotFoundException) {
                Log.w(TAG, "share: $e")
            } catch (e: SecurityException) {
                Log.w(TAG, "share: $e")
            }
        }
        return false
    }

    /** Old copies go; Bluetooth reads a file some time after the share starts, so recent ones stay. */
    private fun cleanSendDir(maxAgeMs: Long, keep: File? = null) {
        val cutoff = System.currentTimeMillis() - maxAgeMs
        sendDir.listFiles()?.forEach { if (it != keep && it.lastModified() < cutoff) it.delete() }
    }

    // ---- Receiving ----

    /** Looks for newly received files (coalesced; runs on a worker thread). */
    fun scan() {
        if (DOWNLOADS == null || !settings.bluetoothReceive || !StorageBridge.hasAllFilesAccess()) return
        if (!queued.compareAndSet(false, true)) return
        worker.execute {
            queued.set(false)
            runCatching { scanNow() }.onFailure { Log.w(TAG, "scan: $it") }
        }
    }

    private fun scanNow() {
        val DOWNLOADS = DOWNLOADS ?: return
        val cr = ctx.contentResolver
        val baseline = settings.bluetoothBaseline
        if (baseline < 0) {
            // First use: files received before the feature was on stay where they are.
            settings.bluetoothBaseline = cr.query(DOWNLOADS, arrayOf(MediaStore.MediaColumns._ID), null, null,
                "${MediaStore.MediaColumns._ID} DESC")?.use { if (it.moveToFirst()) it.getLong(0) else 0L } ?: return
            return
        }
        val copied = settings.bluetoothCopied
        val found = ArrayList<Pair<Long, String>>()
        // Rows still being received are pending, which queries leave out until they complete.
        cr.query(
            DOWNLOADS,
            arrayOf(MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME),
            "${MediaStore.MediaColumns._ID} > ? AND ${MediaStore.MediaColumns.OWNER_PACKAGE_NAME} LIKE ?",
            arrayOf(baseline.toString(), "%bluetooth%"),
            "${MediaStore.MediaColumns._ID} ASC",
        )?.use { c ->
            while (c.moveToNext()) {
                val id = c.getLong(0)
                if (id.toString() !in copied) found += id to (c.getString(1) ?: "file$id")
            }
        }
        if (found.isEmpty()) return
        val dir = receiveDir(ctx)
        // Copies land next to the guest's folder (same volume, so the rename is atomic) and appear whole.
        val partDir = File(dir.parentFile, "Bluetooth.part").apply { mkdirs() }
        val done = copied.toMutableSet()
        for ((id, name) in found) {
            val out = unique(dir, guestName(name))
            val tmp = File(partDir, out.name)
            try {
                cr.openInputStream(ContentUris.withAppendedId(DOWNLOADS, id))!!.use { input ->
                    tmp.outputStream().use { input.copyTo(it) }
                }
                if (!tmp.renameTo(out)) throw java.io.IOException("rename failed")
                Log.i(TAG, "received over Bluetooth: ${out.name}")
                ui.post { guest.send("received ${out.name}") }
            } catch (e: Exception) {
                tmp.delete()
                Log.w(TAG, "copy $name: $e")
                if (e is java.io.FileNotFoundException) done += id.toString()  // deleted meanwhile
                continue
            }
            done += id.toString()
        }
        // Only ids above the baseline matter; keep the most recent ones.
        settings.bluetoothCopied = done.sortedByDescending { it.toLongOrNull() ?: 0L }.take(KEEP_IDS).toSet()
    }

    /** A name Windows Mobile accepts (no path or reserved characters, at most 200 characters). */
    private fun guestName(name: String): String {
        val clean = name.map { if (it < ' ' || it in "\\/:*?\"<>|") '_' else it }.joinToString("")
            .trim().trimEnd('.').ifEmpty { "file" }
        if (clean.length <= 200) return clean
        val ext = clean.substringAfterLast('.', "").take(10)
        return clean.take(200 - ext.length - 1) + if (ext.isEmpty()) "" else ".$ext"
    }

    /** "name.ext", else "name (2).ext", ... (the guest's file system ignores case). */
    private fun unique(dir: File, name: String): File {
        val taken = dir.list()?.map { it.lowercase() }?.toSet() ?: emptySet()
        if (name.lowercase() !in taken) return File(dir, name)
        val base = name.substringBeforeLast('.')
        val ext = if (name.contains('.')) "." + name.substringAfterLast('.') else ""
        var n = 2
        while ("$base ($n)$ext".lowercase() in taken) n++
        return File(dir, "$base ($n)$ext")
    }

    companion object {
        const val TAG = "WMDroidBluetooth"
        // MediaStore.Downloads (and OWNER_PACKAGE_NAME) are API 29: receiving is off on Android 8/9.
        private val DOWNLOADS: Uri? =
            if (android.os.Build.VERSION.SDK_INT >= 29) MediaStore.Downloads.EXTERNAL_CONTENT_URI else null
        private const val HOUR_MS = 3_600_000L
        private const val DAY_MS = 24 * HOUR_MS
        private const val KEEP_IDS = 500

        /** Host folder shown as \Storage Card\Bluetooth (in every storage mode). */
        fun receiveDir(ctx: Context): File = File(ctx.getExternalFilesDir(null), "Bluetooth").apply { mkdirs() }
    }
}
