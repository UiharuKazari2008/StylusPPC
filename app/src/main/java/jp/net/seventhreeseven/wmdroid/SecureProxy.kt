package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.security.MessageDigest
import kotlin.concurrent.thread

/**
 * Secure web proxy (core/src/net/tlsproxy.h, docs/research-tls-proxy.md). Windows Mobile's TLS is
 * from 2009 (TLS 1.0, no SNI, old roots), so almost no HTTPS site accepts it. With the proxy on, the
 * guest's HTTPS goes to the native core, which answers with a certificate signed by a private root CA
 * and connects to the real site with modern TLS, validated against Android's certificate store.
 *
 * This class turns the proxy on or off for the session ("Secure Web Proxy" in GuestSettings) and
 * keeps Windows Mobile's root store in step through the helper (guest/Helper/TlsCa.cs): the root's
 * certificate goes into the mailbox and "tlsca install" adds it; turning the proxy off sends
 * "tlsca remove". The CA lives in noBackupFilesDir/tls: app-private and never backed up. Its private
 * key stays there; only the certificate is handed to the guest.
 *
 * Certificate warnings: when a site's certificate fails the check, the proxy holds the browser's
 * request and asks; [tick] passes the question to the helper ("tlsask", guest/Helper/TlsAsk.cs),
 * which shows a Windows Mobile "Security Warning" box, and [onGuest] passes the answer back. "Yes"
 * lets that site with that certificate through for the rest of the session (in memory only).
 */
class SecureProxy(private val ctx: Context, private val guest: GuestBridge, private val currentHandle: () -> Long) {
    private val dir = File(ctx.noBackupFilesDir, "tls")
    private val main = Handler(Looper.getMainLooper())
    private var handle = 0L
    private var applied: Boolean? = null
    private var thumb: String? = null
    private val asked = HashMap<Int, Long>()  // certificate warning id -> when it went to the helper (uptime ms)

    /** A new native session: applies [on] to it. UI thread. */
    fun attach(handle: Long, on: Boolean) {
        this.handle = handle
        applied = null
        asked.clear()
        NativeBridge.tlsForget()  // "continue anyway" choices last one session
        apply(on)
    }

    /**
     * Every second while the session runs (EmulatorActivity's tick). Certificate warnings from the
     * proxy go to the helper's dialog. The polling itself tells the proxy that someone answers: when
     * it stops (app in the background, proxy off) the proxy shows its error page at once. With the
     * helper not reading its mailbox ([helperUp] false) a warning is declined at once, so the browser
     * gets the error page instead of waiting for a dialog that never shows.
     */
    fun tick(helperUp: Boolean) {
        if (applied != true || handle == 0L) return
        val now = SystemClock.uptimeMillis()
        for (p in NativeBridge.tlsPrompts()) {
            val id = p.substringBefore(' ').toIntOrNull() ?: continue
            Log.i(TAG, "certificate warning ${p.substringBefore('\n')}")
            if (helperUp) {
                guest.send("tlsask $p")
                asked[id] = now
            } else {
                NativeBridge.tlsAnswer(id, false)
            }
        }
        if (!helperUp && asked.isNotEmpty()) {  // the helper stopped while a dialog was due
            for (id in asked.keys) NativeBridge.tlsAnswer(id, false)
            asked.clear()
        }
        asked.entries.removeAll { now - it.value > 120_000 }
    }

    /** The helper's answer, "tlsask <id> yes|no|timeout" without the prefix. */
    fun onGuest(line: String) {
        val p = line.split(' ')
        val id = p.getOrNull(0)?.toIntOrNull() ?: return
        asked.remove(id)
        NativeBridge.tlsAnswer(id, p.getOrNull(1) == "yes")
    }

    /** The setting changed (or is re-applied): UI thread; does nothing if it is already so. */
    fun apply(on: Boolean) {
        val hd = handle
        if (hd == 0L || applied == on) return
        applied = on
        if (!on) {
            NativeBridge.setTlsProxy(hd, null)
            guest.send("tlsca remove")
            return
        }
        thread(name = "SecureProxy") {
            // The first time this makes the CA (two RSA keys); later it loads it once per process.
            val der = runCatching { NativeBridge.tlsCa(dir.path) }.getOrNull()
            main.post {
                // The session may have ended (or restarted) while the CA was loading.
                if (handle != hd || currentHandle() != hd || applied != true) return@post
                if (der == null) {
                    Log.w(TAG, "secure web proxy unavailable (no CA)")
                    return@post
                }
                NativeBridge.setTlsProxy(hd, dir.path)
                thumb = MessageDigest.getInstance("SHA-1").digest(der).joinToString("") { "%02X".format(it) }
                guest.boxFile(CA_FILE).writeBytes(der)
                sendInstall()
            }
        }
    }

    /** The helper (re)started: make sure WM's root store matches the setting. */
    fun onHello() {
        when (applied) {
            true -> sendInstall()
            false -> guest.send("tlsca remove")
            null -> {}
        }
    }

    private fun sendInstall() {
        val t = thumb ?: return  // still making the CA: sent once it is ready
        guest.send("tlsca install $t")
    }

    companion object {
        private const val TAG = "SecureProxy"
        /** The root certificate (DER) in the helper's mailbox, as TlsCa.cs reads it. */
        const val CA_FILE = "pocketstylus-ca.cer"
    }
}
