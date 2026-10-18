package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File

/**
 * Apps from an HTC HD2 ROM the user owns (docs/research-hd2-port.md). The core reads the ROM
 * (HtcRom) and stages a package as \Storage Card\.wmdroid\htc\<package>\; the guest helper
 * installs it into \Windows with its registry and Start menu shortcut (guest/Helper/HtcCompat.cs),
 * right away when Windows Mobile runs, else at its next start. HTC's WVGA games need 480×800.
 * Result OK = restart Windows Mobile (the screen size changed).
 */
class HtcAppsActivity : SettingsPage() {
    private val prefs by lazy { getSharedPreferences("wmdroid.htc", MODE_PRIVATE) }
    private var pfd: ParcelFileDescriptor? = null
    private var rom = 0L
    private lateinit var romNote: TextView
    private lateinit var apps: LinearLayout
    private val box by lazy { File(filesDir, "overlay/.wmdroid") }  // GuestBridge's mailbox (\Storage Card\.wmdroid)
    private val htcDir by lazy { File(box, "htc") }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title("HTC HD2 apps")

        section("ROM")
        note("Install apps such as Teeter from an HTC HD2 Windows Mobile 6.5 ROM you own: its RUU_signed.nbh " +
            "(from HTC's ROM update), or an os.nb or IMGFS dump. PocketStylus includes no HTC software.")
        romNote = note("")
        button("Choose ROM file…") {
            @Suppress("DEPRECATION")
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), REQ_ROM)
        }

        section("Screen")
        if (settings.screenRes == "480x800") note("Screen resolution is 480×800, as HTC's WVGA apps expect.")
        else {
            note("HTC's games are made for 480×800 (WVGA). At other sizes they can draw past their buffers and crash " +
                "(Teeter does at 480×854). Screen resolution is now " +
                (if (settings.screenRes == Settings.SCREEN_AUTO) "the phone's shape" else settings.screenRes.replace("x", "×")) + ".")
            button("Use 480×800 and restart Windows Mobile") {
                settings.screenRes = "480x800"
                setResult(RESULT_OK)
                finish()
            }
        }

        section("Apps in this ROM")
        apps = card
        show()
        prefs.getString(KEY_ROM, null)?.let { openRom(Uri.parse(it)) } ?: showRom("No ROM chosen.")
    }

    override fun onDestroy() {
        closeRom()
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        if (rom != 0L) refreshList()
    }

    private fun showRom(text: String) { romNote.text = text }

    private fun closeRom() {
        if (rom != 0L) HtcRom.close(rom)
        rom = 0L
        pfd?.close()
        pfd = null
    }

    private fun openRom(uri: Uri) {
        closeRom()
        showRom("Reading the ROM…")
        apps.removeAllViews()
        Thread {
            val err = arrayOfNulls<String>(1)
            val fd = runCatching { contentResolver.openFileDescriptor(uri, "r") }.getOrNull()
            val h = if (fd != null) HtcRom.open(fd.fd, err) else 0L
            val list = if (h != 0L) HtcRom.list(h) else emptyList()
            runOnUiThread {
                if (isDestroyed) { if (h != 0L) HtcRom.close(h); fd?.close(); return@runOnUiThread }
                pfd = fd
                rom = h
                if (h == 0L) {
                    fd?.close()
                    pfd = null
                    showRom(if (fd == null) "The ROM file can't be opened any more; choose it again." else "Not a usable ROM: ${err[0]}")
                } else {
                    packages = list
                    showRom("${uri.lastPathSegment?.substringAfterLast('/') ?: "ROM"}: ${list.size} packages.")
                    refreshList()
                }
            }
        }.start()
    }

    private var packages: List<HtcRom.Package> = emptyList()

    private fun refreshList() {
        apps.removeAllViews()
        card = apps
        val withApps = packages.filter { it.exes.isNotEmpty() }
        val (works, others) = withApps.partition { p -> WORKING.any { p.name.startsWith(it, ignoreCase = true) } }
        if (works.isNotEmpty()) group("Run on PocketStylus")
        for (p in works) appRow(p)
        if (others.isNotEmpty()) {
            group("Untested")
            note("Many HTC apps need HTC services, Sense (Manila) or OpenGL ES, which PocketStylus doesn't have yet. " +
                "They install the same way but may not start.")
        }
        for (p in others) appRow(p)
        if (withApps.isEmpty()) note("No apps found in this ROM.")
    }

    private fun appRow(p: HtcRom.Package) {
        val dir = File(htcDir, p.name)
        val state = when {
            dir.listFiles()?.any { it.name.startsWith("done-") } == true -> "installed in Windows Mobile"
            File(dir, "install.ini").exists() -> "waiting for Windows Mobile to install it"
            else -> null
        }
        button("Install ${p.title}" + if (state != null) " (again)" else "") { install(p) }
        note(p.exes.joinToString(", ") + (if (state != null) " · $state" else ""))
    }

    private fun install(p: HtcRom.Package) {
        val h = rom
        if (h == 0L) return
        Toast.makeText(this, "Preparing ${p.title}…", Toast.LENGTH_SHORT).show()
        Thread {
            val dir = File(htcDir, p.name)
            dir.deleteRecursively()
            htcDir.mkdirs()
            val have = File(htcDir, "windows.txt").takeIf { it.exists() }?.readText()
            val log = HtcRom.stage(h, p.name, dir.path, have)
            val ok = !log.contains("error:")
            if (ok) {
                // Ask a running helper now; a later start or session also installs it (HtcCompat.InstallStaged).
                val inDir = File(box, "in").apply { mkdirs() }
                val name = "h%013d".format(System.currentTimeMillis())
                val tmp = File(inDir, "$name.tmp")
                tmp.writeText("htcinstall")
                tmp.renameTo(File(inDir, "$name.cmd"))
            } else dir.deleteRecursively()
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                refreshList()
                if (ok) Toast.makeText(this, "${p.title} goes into Windows Mobile when it runs; look in Start.",
                    Toast.LENGTH_LONG).show()
                else MaterialAlertDialogBuilder(this).setTitle("${p.title} can't be installed")
                    .setMessage(log.lines().filter { it.isNotBlank() }.takeLast(12).joinToString("\n"))
                    .setPositiveButton("OK", null).show()
            }
        }.start()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_ROM || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        runCatching { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        prefs.edit().putString(KEY_ROM, uri.toString()).apply()
        openRom(uri)
    }

    companion object {
        private const val REQ_ROM = 61
        private const val KEY_ROM = "rom"
        /** Packages known to run (docs/research-hd2-port.md). */
        private val WORKING = listOf("Teeter_")
    }
}
