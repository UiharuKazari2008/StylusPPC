package jp.net.seventhreeseven.wmdroid

import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection

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

    /**
     * The phone's keyboard as a text editor for [view]. Composing text (word suggestions,
     * autocorrect) is typed as it changes: a changed tail is backspaced and retyped, so the guest
     * always shows what the keyboard shows.
     */
    fun connection(view: View, outAttrs: EditorInfo, sink: Sink): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(view, false) {
            private var composing = ""

            private fun replaceComposing(text: String) {
                val common = composing.commonPrefixWith(text).length
                repeat(composing.length - common) { sink.key(0x08) }
                text.substring(common).forEach { sink.char(it) }
                composing = text
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                replaceComposing(text.toString())
                return true
            }

            override fun finishComposingText(): Boolean {
                composing = ""
                return true
            }

            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                replaceComposing(text.toString())
                composing = ""
                return true
            }

            override fun deleteSurroundingText(before: Int, after: Int): Boolean {
                composing = ""
                repeat(maxOf(before, 1)) { sink.key(0x08) }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action != KeyEvent.ACTION_DOWN) return true
                when (event.keyCode) {
                    KeyEvent.KEYCODE_DEL -> sink.key(0x08)
                    KeyEvent.KEYCODE_FORWARD_DEL -> sink.key(0x2E)
                    KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> sink.key(0x0D)
                    KeyEvent.KEYCODE_TAB -> sink.key(0x09)
                    KeyEvent.KEYCODE_DPAD_LEFT -> sink.key(0x25)
                    KeyEvent.KEYCODE_DPAD_RIGHT -> sink.key(0x27)
                    KeyEvent.KEYCODE_DPAD_UP -> sink.key(0x26)
                    KeyEvent.KEYCODE_DPAD_DOWN -> sink.key(0x28)
                    KeyEvent.KEYCODE_MOVE_HOME -> sink.key(0x24)
                    KeyEvent.KEYCODE_MOVE_END -> sink.key(0x23)
                    else -> { val ch = event.unicodeChar; if (ch != 0) sink.char(ch.toChar()) }
                }
                return true
            }
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
