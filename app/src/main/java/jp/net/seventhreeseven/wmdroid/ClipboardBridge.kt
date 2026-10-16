package jp.net.seventhreeseven.wmdroid

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.util.Log
import java.io.File

/**
 * "Sync Clipboard": the phone's clipboard and Windows Mobile's, both ways (text only).
 *
 *  - phone -> WM: when the phone's clip changes while WMDroid is on screen, and when it gets the
 *    focus back, the text goes to the mailbox as clip_in.txt (UTF-8, so nothing needs escaping) and
 *    the helper hears "clip"; it sets CF_UNICODETEXT (guest/Helper/InputSync.cs).
 *  - WM -> phone: the helper hashes WM's clipboard once a second while told "clip watch 1"; a change
 *    comes as clip_out.txt plus a "clip" message, which becomes the phone's primary clip.
 *
 * No ping-pong: both sides remember what they last set. Android only lets the focused app read the
 * clipboard, so reads happen only with window focus, and only when the clip's timestamp changed
 * (each read of another app's clip shows the system's "pasted" notice; we add no UI of our own).
 */
class ClipboardBridge(private val ctx: Context, private val settings: Settings, private val guest: GuestBridge) {
    private val cm = ctx.getSystemService(ClipboardManager::class.java)
    private var foreground = false
    private var focused = false
    /** What the helper was last told ("clip watch"), null if unknown. */
    private var watching: Boolean? = null
    /** The text last synced either way. */
    private var lastText: String? = null
    /** Timestamp of the phone's clip last looked at. */
    private var lastStamp = Long.MIN_VALUE

    private val listener = ClipboardManager.OnPrimaryClipChangedListener { toGuest() }

    fun resume() {
        foreground = true
        cm?.addPrimaryClipChangedListener(listener)
        apply()
    }

    fun pause() {
        foreground = false
        cm?.removePrimaryClipChangedListener(listener)
        tell(false)
    }

    /** Window focus: the clipboard can be read only now. */
    fun onFocus(hasFocus: Boolean) {
        focused = hasFocus
        if (hasFocus) toGuest()
    }

    /** The setting may have changed. */
    fun apply() {
        tell(foreground && settings.syncClipboard)
        toGuest()
    }

    /** The helper (re)started: it has forgotten the watch, and its clipboard may be empty. */
    fun resend() {
        watching = null
        lastStamp = Long.MIN_VALUE
        lastText = null
        apply()
    }

    private fun tell(on: Boolean) {
        if (watching == on) return
        watching = on
        guest.send("clip watch ${if (on) 1 else 0}")
    }

    /** The phone's clip to Windows Mobile, if it is new text. */
    private fun toGuest() {
        if (!foreground || !focused || !settings.syncClipboard) return
        val cm = cm ?: return
        val desc = runCatching { cm.primaryClipDescription }.getOrNull() ?: return
        if (desc.timestamp == lastStamp) return
        lastStamp = desc.timestamp
        if (!desc.hasMimeType("text/*")) return
        val text = runCatching { cm.primaryClip?.getItemAt(0)?.text?.toString() }.getOrNull() ?: return
        if (text.isEmpty() || text == lastText) return
        lastText = text
        write(guest.boxFile(CLIP_IN), text.take(MAX_CHARS))
        guest.send("clip")
    }

    /** "clip" from the helper: Windows Mobile's new clipboard text is in clip_out.txt. */
    fun fromGuest() {
        if (!settings.syncClipboard) return
        val text = runCatching { guest.boxFile(CLIP_OUT).readText() }.getOrNull()?.removePrefix("﻿") ?: return
        if (text.isEmpty() || text == lastText) return
        lastText = text
        val cm = cm ?: return
        runCatching { cm.setPrimaryClip(ClipData.newPlainText("Windows Mobile", text)) }
            .onFailure { Log.w(TAG, "clipboard: ${it.message}") }
        // Our own clip: the listener will see it, and the timestamp says it is not news.
        if (focused) runCatching { cm.primaryClipDescription?.timestamp }.getOrNull()?.let { lastStamp = it }
    }

    private fun write(f: File, text: String) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(text)
        tmp.renameTo(f)
    }

    companion object {
        const val TAG = "WMDroidClip"
        const val CLIP_IN = "clip_in.txt"
        const val CLIP_OUT = "clip_out.txt"
        const val MAX_CHARS = 256 * 1024
    }
}
