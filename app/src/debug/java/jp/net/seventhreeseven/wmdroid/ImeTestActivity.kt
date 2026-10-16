package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.TextView

/**
 * Debug builds only: the phone's keyboard typing through GuestTyper with no guest. A fake WM edit
 * box (text and caret) takes the keys; every InputConnection call is logged (tag ImeTest), then
 * "guest" (the fake box, | = caret) and "kbd" (what the connection tells the keyboard), with
 * MISMATCH when they differ. Tapping the screen is a tap on the guest (GuestTyper.caretMoved).
 *   adb shell am start -n jp.net.seventhreeseven.wmdroid/.ImeTestActivity [--ez suggest true]
 */
class ImeTestActivity : Activity() {
    private val guest = StringBuilder()
    private var caret = 0
    private lateinit var shown: TextView

    private val sink = object : GuestTyper.Sink {
        override fun char(c: Char) {
            Log.d(TAG, "  -> char ${"%04x".format(c.code)} '$c'")
            guest.insert(caret++, c)
        }

        override fun key(vk: Int) {
            Log.d(TAG, "  -> key ${"%02x".format(vk)}")
            when (vk) {
                0x08 -> if (caret > 0) guest.deleteCharAt(--caret)
                0x2E -> if (caret < guest.length) guest.deleteCharAt(caret)
                0x0D -> guest.insert(caret++, '\n')
                0x09 -> guest.insert(caret++, '\t')
                0x25 -> if (caret > 0) caret--
                0x27 -> if (caret < guest.length) caret++
                0x24 -> caret = guest.lastIndexOf("\n", caret - 1) + 1
                0x23 -> caret = guest.indexOf("\n", caret).let { if (it < 0) guest.length else it }
                else -> Log.d(TAG, "  (key not modelled)")
            }
        }
    }

    /** After each keyboard call: the fake box against what the keyboard is told. */
    private fun check(c: InputConnection) {
        val g = guest.substring(0, caret) + "|" + guest.substring(caret)
        val k = "${c.getTextBeforeCursor(10000, 0)}|${c.getTextAfterCursor(10000, 0)}"
        shown.text = g.replace("\n", "⏎\n")
        Log.d(TAG, "  guest=\"${g.replace("\n", "⏎")}\" kbd=\"${k.replace("\n", "⏎")}\"")
        // The keyboard's view only covers text typed since the last tap; it must end the guest's.
        val gb = guest.substring(0, caret)
        val kb = k.substringBefore('|')
        if (!gb.endsWith(kb)) Log.d(TAG, "  MISMATCH before cursor")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        shown = TextView(this).apply { textSize = 22f; setTextColor(Color.BLACK); setPadding(32, 32, 32, 32) }
        val input = Input(this)
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.WHITE)
            addView(shown, LinearLayout.LayoutParams(-1, -2))
            addView(input, LinearLayout.LayoutParams(-1, 0, 1f))
        })
        input.requestFocus()
        input.postDelayed({
            getSystemService(InputMethodManager::class.java).showSoftInput(input, 0)
        }, 500)
    }

    private inner class Input(ctx: Context) : View(ctx) {
        init { isFocusable = true; isFocusableInTouchMode = true }

        override fun onCheckIsTextEditor() = true

        override fun onTouchEvent(e: MotionEvent): Boolean {
            if (e.actionMasked == MotionEvent.ACTION_DOWN) {
                Log.d(TAG, "tap (caretMoved)")
                GuestTyper.caretMoved()
            }
            return true
        }

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            val inner = GuestTyper.connection(this, outAttrs, sink)
            // --ez suggest true: as a keyboard that ignores NO_SUGGESTIONS (suggestions, autocorrect).
            if (intent.getBooleanExtra("suggest", false)) outAttrs.inputType =
                (outAttrs.inputType and android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS.inv()) or
                    android.text.InputType.TYPE_TEXT_FLAG_AUTO_CORRECT
            return object : InputConnectionWrapper(inner, false) {
                private fun <T> call(what: String, f: () -> T): T {
                    Log.d(TAG, what)
                    return f().also { check(inner) }
                }
                override fun setComposingText(t: CharSequence, p: Int) = call("setComposingText \"$t\"") { super.setComposingText(t, p) }
                override fun commitText(t: CharSequence, p: Int) = call("commitText \"$t\"") { super.commitText(t, p) }
                override fun finishComposingText() = call("finishComposingText") { super.finishComposingText() }
                override fun deleteSurroundingText(b: Int, a: Int) = call("deleteSurroundingText $b $a") { super.deleteSurroundingText(b, a) }
                override fun deleteSurroundingTextInCodePoints(b: Int, a: Int) = call("deleteSurroundingTextInCodePoints $b $a") { super.deleteSurroundingTextInCodePoints(b, a) }
                override fun setComposingRegion(s: Int, e: Int) = call("setComposingRegion $s $e") { super.setComposingRegion(s, e) }
                override fun setSelection(s: Int, e: Int) = call("setSelection $s $e") { super.setSelection(s, e) }
                override fun commitCorrection(c: android.view.inputmethod.CorrectionInfo?) = call("commitCorrection $c") { super.commitCorrection(c) }
                override fun performEditorAction(a: Int) = call("performEditorAction $a") { super.performEditorAction(a) }
                override fun sendKeyEvent(e: KeyEvent) = if (e.action != KeyEvent.ACTION_DOWN) super.sendKeyEvent(e)
                    else call("sendKeyEvent ${KeyEvent.keyCodeToString(e.keyCode)} meta=${e.metaState}") { super.sendKeyEvent(e) }
            }
        }
    }

    companion object { const val TAG = "ImeTest" }
}
