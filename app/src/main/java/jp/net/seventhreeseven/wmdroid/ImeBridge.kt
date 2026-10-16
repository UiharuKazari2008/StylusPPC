package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.os.FileObserver
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.io.File

/**
 * "Use Android IME over Device IME": when Windows Mobile raises its on-screen keyboard (SIP), the
 * phone's keyboard opens instead.
 *
 * How (a real WM input method would be a COM DLL implementing IInputMethod; see docs/clipboard-ime.md
 * for why this does without one):
 *  - The helper (guest/Helper/InputSync.cs, told "ime watch 1" while this setting is on and the app
 *    is on screen) watches the SIP. When it comes up (a text box asking for it, or WM's keyboard
 *    button) it hides the SIP's window but leaves the SIP on, so programs keep their text above the
 *    keyboard area, and sends "ime open". WM lowering the SIP sends "ime hide".
 *  - "ime open" focuses a hidden 1x1 editor view here and shows the phone's keyboard; its text goes
 *    to the helper as type\<seq>.txt (GuestTyper tokens), which posts it as characters the way a WM
 *    input method does. (Not the emulated keyboard: WM lowers the SIP on every hardware key press.)
 *  - The phone's keyboard being dismissed (IME insets no longer visible) sends "ime close": the
 *    helper lowers the SIP.
 *  - While on, the mailbox's out folder is watched (FileObserver), so "ime open" is acted on at once
 *    rather than at the activity's once-a-second poll.
 * With the fold's own phone-keyboard layout on screen, that keyboard is used instead.
 */
class ImeBridge(
    private val ctx: Context,
    private val settings: Settings,
    private val guest: GuestBridge,
    private val skin: SkinLayout,
) {
    private val ui = Handler(Looper.getMainLooper())
    private val imm get() = ctx.getSystemService(InputMethodManager::class.java)
    private val view = InputView(ctx)
    private var foreground = false
    private var focused = false
    /** What the helper was last told ("ime watch"), null if unknown. */
    private var watching: Boolean? = null
    /** The phone's keyboard is up for Windows Mobile (from "ime open" until it closes either way). */
    private var open = false
    /** It has actually been seen on screen since [open]. */
    private var shown = false
    private var showPending = false

    init {
        // 1x1 and see-through; it only holds the focus for the phone's keyboard.
        view.alpha = 0f
        skin.addView(view, FrameLayout.LayoutParams(1, 1))
        ViewCompat.setOnApplyWindowInsetsListener(view) { _, insets ->
            imeVisible(insets.isVisible(WindowInsetsCompat.Type.ime()))
            insets
        }
    }

    fun resume() {
        foreground = true
        apply()
    }

    fun pause() {
        foreground = false
        // The system takes the keyboard down; the helper puts WM's own back if it was up, and
        // raises "ime open" again on return.
        if (open) close(sendClose = false)
        apply()
    }

    fun onFocus(hasFocus: Boolean) {
        focused = hasFocus
        if (hasFocus && showPending) show()
    }

    /** The setting may have changed. */
    fun apply() {
        val on = foreground && settings.androidIme
        if (!on && open) close(sendClose = false)
        watchMailbox(on)
        if (watching != on) {
            watching = on
            guest.send("ime watch ${if (on) 1 else 0}")
        }
    }

    /** The helper (re)started and has forgotten the watch. */
    fun resend() {
        watching = null
        apply()
    }

    /** "ime open" / "ime hide" from the helper (without the "ime " prefix). */
    fun onGuest(what: String) {
        when (what) {
            "open" -> open()
            "hide" -> if (open) close(sendClose = false)
        }
    }

    // ---- The phone's keyboard ----

    /** The fold's slide-out keyboard is set to the phone's keyboard and on screen: it owns the IME. */
    private fun foldIme() = skin.foldKeyboard.useIme && skin.foldKeyboard.visibility == View.VISIBLE

    private fun open() {
        if (!foreground || !settings.androidIme) return
        if (foldIme()) { skin.foldKeyboard.showIme(); return }
        open = true
        shown = false
        view.active = true
        show()
    }

    private fun show() {
        showPending = false
        if (!open) return
        view.requestFocus()
        imm?.restartInput(view)
        if (!focused) { showPending = true; return }  // shown when the window has the focus
        view.post { if (open) imm?.showSoftInput(view, 0) }  // once the focus change has reached the IMM
        ui.removeCallbacks(visibilityCheck)
        ui.postDelayed(visibilityCheck, CHECK_MS)
    }

    private fun close(sendClose: Boolean) {
        open = false
        shown = false
        showPending = false
        ui.removeCallbacks(visibilityCheck)
        flush()
        view.active = false
        imm?.hideSoftInputFromWindow(view.windowToken, 0)
        view.clearFocus()
        imm?.restartInput(view)
        if (sendClose) guest.send("ime close")
    }

    /** IME visibility from the window insets: dismissed after having been shown = closed by the user. */
    private fun imeVisible(visible: Boolean) {
        if (!open) return
        if (visible) shown = true
        else if (shown) close(sendClose = true)
    }

    /** In case the insets listener is not called (another view consumed them): look every so often. */
    private val visibilityCheck = object : Runnable {
        override fun run() {
            if (!open) return
            ViewCompat.getRootWindowInsets(view)?.let { imeVisible(it.isVisible(WindowInsetsCompat.Type.ime())) }
            if (open) ui.postDelayed(this, CHECK_MS)
        }
    }

    // ---- Typing: tokens to type\<seq>.txt for the helper ----

    private val typeDir = guest.boxFile("type")
    private var seq = System.currentTimeMillis()
    private val pending = StringBuilder()
    private val flushNow = Runnable { flush() }

    private val sink = object : GuestTyper.Sink {
        override fun char(c: Char) = add("%04x".format(c.code))
        override fun key(vk: Int) = add("1%04x".format(vk))
    }

    private fun add(token: String) {
        if (pending.isEmpty()) ui.post(flushNow)  // one file per burst of input
        pending.append(token).append(' ')
    }

    private fun flush() {
        ui.removeCallbacks(flushNow)
        if (pending.isEmpty()) return
        val text = pending.toString()
        pending.setLength(0)
        runCatching {
            typeDir.mkdirs()
            val f = File(typeDir, "%015d.txt".format(seq++))
            val tmp = File(typeDir, "${f.name}.tmp")
            tmp.writeText(text)
            tmp.renameTo(f)
        }
    }

    // ---- Quick reaction to the helper's messages while on ----

    private var observer: FileObserver? = null
    private val pollNow = Runnable { if (observer != null) guest.poll() }

    private fun watchMailbox(on: Boolean) {
        if (on == (observer != null)) return
        observer?.stopWatching()
        observer = null
        if (!on) return
        val dir = guest.boxFile("out").apply { mkdirs() }
        observer = object : FileObserver(dir, FileObserver.MOVED_TO or FileObserver.CLOSE_WRITE) {
            override fun onEvent(event: Int, path: String?) {
                if (path?.endsWith(".msg") != true) return
                ui.removeCallbacks(pollNow)  // one poll per burst of messages
                ui.post(pollNow)
            }
        }.apply { startWatching() }
    }

    /** The focus holder for the phone's keyboard. */
    private inner class InputView(ctx: Context) : View(ctx) {
        var active = false

        init {
            isFocusable = true
            isFocusableInTouchMode = true
        }

        override fun onCheckIsTextEditor() = active

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? =
            if (active) GuestTyper.connection(this, outAttrs, sink) else null
    }

    companion object {
        const val CHECK_MS = 300L
    }
}
