package jp.net.seventhreeseven.wmdroid

import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.text.SpannableStringBuilder
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * Typing into Windows Mobile from the phone's keyboard (the IME), shared by FoldKeyboard (the
 * fold's slide-out keyboard set to the phone's keyboard) and ImeBridge ("Use Android IME over
 * Device IME"). The input connection turns the IME's text and keys into [Sink] calls; the sink
 * decides how they reach the guest.
 */
object GuestTyper {
    /** Where typed input goes: characters, and virtual keys (Back, Enter, arrows...). */
    interface Sink {
        fun char(c: Char)
        fun key(vk: Int)
    }

    /**
     * Through the emulated hardware keyboard, key by key on a US layout (what Windows Mobile's
     * keyboard driver expects); characters the layout lacks are dropped.
     */
    class HardwareKeys(private val onKey: (vk: Int, down: Boolean) -> Unit) : Sink {
        override fun char(c: Char) {
            when {
                c in 'a'..'z' -> tap(c.uppercaseChar().code)
                c in 'A'..'Z' -> tap(c.code, shifted = true)
                c in '0'..'9' -> tap(c.code)
                c == '\n' -> tap(0x0D)
                c == '\t' -> tap(0x09)
                else -> GuestTyper.US[c]?.let { (vk, s) -> tap(vk, s) }
            }
        }

        override fun key(vk: Int) = tap(vk)

        private fun tap(vk: Int, shifted: Boolean = false) {
            if (shifted) onKey(0x10, true)
            onKey(vk, true)
            onKey(vk, false)
            if (shifted) onKey(0x10, false)
        }
    }

    /** Bumped when Windows Mobile's caret may have moved without us (a tap on its screen). */
    @Volatile private var caretEpoch = 0

    /** A tap on the guest screen: what the keyboard thinks is before the cursor no longer holds. */
    fun caretMoved() { caretEpoch++ }

    /** The phone's keyboard as a text editor for [view], typing through [sink]. */
    fun connection(view: View, outAttrs: EditorInfo, sink: Sink): InputConnection {
        // Multi-line: the keyboard shows a plain Enter key (not Done/Go) and sends it to us.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
            InputType.TYPE_TEXT_FLAG_MULTI_LINE
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_ACTION_NONE
        outAttrs.initialSelStart = 0
        outAttrs.initialSelEnd = 0
        return Connection(view, sink)
    }

    /**
     * Keeps what has been typed into the guest since the keyboard connected, as a text box would,
     * so the keyboard sees the text before its cursor. Keyboards edit that text, not just append:
     * autocorrect and suggestions replace a word, backspacing into a finished word re-opens it
     * (setComposingRegion), the space bar slides the cursor. Each edit is played into the guest at
     * its caret as arrows, Backspaces and characters. Text from before the connection (or a tap
     * on the guest screen) is unknown: the keyboard sees an empty box and only deletes into it.
     */
    private class Connection(private val view: View, private val sink: Sink) : BaseInputConnection(view, false) {
        private val text = SpannableStringBuilder()
        private var cursor = 0
        /** The composing region (the word being typed or re-edited), or -1. */
        private var cs = -1
        private var ce = -1
        private var batch = 0
        private var epoch = caretEpoch
        private val imm = view.context.getSystemService(InputMethodManager::class.java)

        override fun getEditable(): Editable {
            sync()
            Selection.setSelection(text, cursor)
            return text
        }

        /** The guest's caret moved under us: forget the text and have the keyboard re-read it. */
        private fun sync() {
            if (epoch == caretEpoch) return
            epoch = caretEpoch
            reset()
        }

        private fun reset() {
            text.clear()
            cursor = 0
            cs = -1
            changed()
        }

        /** Tells the keyboard where the cursor and composing word are now (outside batch edits). */
        private fun changed() {
            if (batch == 0) imm?.updateSelection(view, cursor, cursor, cs, if (cs >= 0) ce else -1)
        }

        /** Enter and Tab can arrive as characters, which WM only beeps at. */
        private fun type(c: Char) = when (c) {
            '\n', '\r' -> sink.key(0x0D)
            '\t' -> sink.key(0x09)
            else -> sink.char(c)
        }

        private fun moveTo(pos: Int) {
            while (cursor > pos) { sink.key(0x25); cursor-- }
            while (cursor < pos) { sink.key(0x27); cursor++ }
        }

        /** Replaces text[start, end) with [new], leaving the caret after it. */
        private fun replace(start: Int, end: Int, new: String) {
            moveTo(end)
            val old = text.substring(start, end)
            val common = old.commonPrefixWith(new).length
            repeat(old.length - common) { sink.key(0x08) }
            new.substring(common).forEach { type(it) }
            text.replace(start, end, new)
            cursor = start + new.length
            if (text.length > MAX_TEXT) reset()
        }

        /** Where new text goes: over the composing word, else at the caret. */
        private fun target() = if (cs >= 0) cs to ce else cursor to cursor

        override fun setComposingText(t: CharSequence, newCursorPosition: Int): Boolean {
            sync()
            val (s, e) = target()
            replace(s, e, t.toString())
            if (t.isEmpty()) cs = -1 else { cs = s; ce = s + t.length }
            changed()
            return true
        }

        override fun commitText(t: CharSequence, newCursorPosition: Int): Boolean {
            sync()
            val (s, e) = target()
            replace(s, e, t.toString())
            cs = -1
            changed()
            return true
        }

        override fun setComposingRegion(start: Int, end: Int): Boolean {
            sync()
            val s = minOf(start, end).coerceIn(0, text.length)
            val e = maxOf(start, end).coerceIn(0, text.length)
            if (s == e) cs = -1 else { cs = s; ce = e }
            changed()
            return true
        }

        override fun finishComposingText(): Boolean {
            cs = -1
            changed()
            return true
        }

        override fun deleteSurroundingText(before: Int, after: Int): Boolean {
            sync()
            cs = -1
            // Some keyboards ask for nothing on an empty box: still one Backspace.
            val b = if (before == 0 && after == 0) 1 else before
            repeat(after) { sink.key(0x2E) }
            text.delete(cursor, minOf(text.length, cursor + after))
            repeat(b) { sink.key(0x08) }
            val from = maxOf(0, cursor - b)
            text.delete(from, cursor)
            cursor = from
            changed()
            return true
        }

        override fun deleteSurroundingTextInCodePoints(before: Int, after: Int): Boolean {
            sync()
            // In UTF-16 units over the text we know; past it, one unit per code point.
            var b = 0
            repeat(before) { b += if (cursor - b >= 2 && Character.isLowSurrogate(text[cursor - b - 1])) 2 else 1 }
            var a = 0
            repeat(after) { a += if (cursor + a + 1 < text.length && Character.isHighSurrogate(text[cursor + a])) 2 else 1 }
            return deleteSurroundingText(b, a)
        }

        // The keyboard moving the cursor (space bar slide): arrow keys. Selections are not mirrored.
        override fun setSelection(start: Int, end: Int): Boolean {
            sync()
            cs = -1
            moveTo(end.coerceIn(0, text.length))
            changed()
            return true
        }

        override fun beginBatchEdit(): Boolean { batch++; return true }

        override fun endBatchEdit(): Boolean {
            if (batch > 0 && --batch == 0) changed()
            return batch > 0
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            if (event.action != KeyEvent.ACTION_DOWN) return true
            sync()
            cs = -1
            when (event.keyCode) {
                KeyEvent.KEYCODE_DEL -> {
                    sink.key(0x08)
                    if (cursor > 0) text.delete(--cursor, cursor + 1)
                }
                KeyEvent.KEYCODE_FORWARD_DEL -> {
                    sink.key(0x2E)
                    if (cursor < text.length) text.delete(cursor, cursor + 1)
                }
                KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> replace(cursor, cursor, "\n")
                KeyEvent.KEYCODE_TAB -> replace(cursor, cursor, "\t")
                // Left/right within the known text are followed; anything else leaves it unknown.
                KeyEvent.KEYCODE_DPAD_LEFT -> { sink.key(0x25); if (cursor > 0) cursor-- else reset() }
                KeyEvent.KEYCODE_DPAD_RIGHT -> { sink.key(0x27); if (cursor < text.length) cursor++ else reset() }
                KeyEvent.KEYCODE_DPAD_UP -> { sink.key(0x26); reset() }
                KeyEvent.KEYCODE_DPAD_DOWN -> { sink.key(0x28); reset() }
                KeyEvent.KEYCODE_MOVE_HOME -> { sink.key(0x24); reset() }
                KeyEvent.KEYCODE_MOVE_END -> { sink.key(0x23); reset() }
                else -> { val ch = event.unicodeChar; if (ch != 0) replace(cursor, cursor, ch.toChar().toString()) }
            }
            changed()
            return true
        }

        // The Enter key when the keyboard shows it as an action (Done, Go, Send...).
        override fun performEditorAction(actionCode: Int): Boolean {
            sync()
            cs = -1
            replace(cursor, cursor, "\n")
            changed()
            return true
        }

        companion object {
            /** Past this the text is dropped (offsets the keyboard holds would no longer match). */
            const val MAX_TEXT = 4000
        }
    }

    /** Characters on a US keyboard that need a VK other than their own code (and maybe Shift). */
    internal val US: Map<Char, Pair<Int, Boolean>> = mapOf(
        ' ' to (0x20 to false), '.' to (0xBE to false), ',' to (0xBC to false), '/' to (0xBF to false),
        ';' to (0xBA to false), '\'' to (0xDE to false), '[' to (0xDB to false), ']' to (0xDD to false),
        '\\' to (0xDC to false), '-' to (0xBD to false), '=' to (0xBB to false),
        '!' to ('1'.code to true), '@' to ('2'.code to true), '#' to ('3'.code to true), '$' to ('4'.code to true),
        '%' to ('5'.code to true), '^' to ('6'.code to true), '&' to ('7'.code to true), '*' to ('8'.code to true),
        '(' to ('9'.code to true), ')' to ('0'.code to true), '_' to (0xBD to true), '+' to (0xBB to true),
        ':' to (0xBA to true), '"' to (0xDE to true), '<' to (0xBC to true), '>' to (0xBE to true),
        '?' to (0xBF to true), '{' to (0xDB to true), '}' to (0xDD to true), '|' to (0xDC to true),
        '`' to (0xC0 to false), '~' to (0xC0 to true),
    )
}
