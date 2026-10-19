package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * The lower half of an open foldable: a slide-out style hardware keyboard for Windows Mobile,
 * drawn as glass keys like the rest of the skin, or (with [useIme]) the phone's own keyboard,
 * whose text is typed into the guest key by key. The hotkey row (Talk, soft keys, Start, OK,
 * End and arrows) can be shown with either.
 */
class FoldKeyboard(ctx: Context, private val onKey: (vk: Int, down: Boolean) -> Unit) : View(ctx) {
    var useIme = false
        set(v) { field = v; relayout(); if (v) showIme() else hideIme() }
    var hotkeys = true
        set(v) { field = v; relayout() }

    /** [alt]: the blue Fn-layer character; [ch]: a character typed as is (vk = -1). */
    private class Key(val label: String, val vk: Int, val units: Float = 1f, val kind: Int = NORMAL,
                      val alt: Char? = null, val ch: Char? = null) {
        val rect = RectF()
        var pressed = false
        /** What the skin draws (Talk and End get their colours from the virtual key). */
        val skin = SkinLayout.Key(if (kind == TALK || kind == END) vk else 0, 0).apply { id = "$label/$vk/$units".hashCode() or 1 }
    }

    private fun k(c: Char, alt: Char? = null) = Key(c.toString(), c.code, alt = alt)
    private fun typed(c: Char, alt: Char? = null, units: Float = 1f) = Key(c.toString(), -1, units, alt = alt, ch = c)

    // With the phone's keyboard: everything the IME lacks, in one row above it.
    private val hotRowIme = listOf(
        Key("Talk", Keymap.VK_TTALK, 1.3f, TALK), Key("◀ Soft", Keymap.VK_TSOFT1, 1.3f, MOD),
        Key("⊞", 0x5B, 1f, MOD), Key("OK", Keymap.VK_RETURN, 1f, MOD), Key("◀", 0x25, 1f, MOD),
        Key("▲", 0x26, 1f, MOD), Key("▼", 0x28, 1f, MOD), Key("▶", 0x27, 1f, MOD),
        Key("Back", 0x1B, 1f, MOD), Key("Soft ▶", Keymap.VK_TSOFT2, 1.3f, MOD), Key("End", Keymap.VK_TEND, 1.3f, END),
    )
    // With the skin keyboard the slider column has Talk/End/OK/D-pad: only the soft keys here.
    private val hotRowSkin = listOf(
        Key("◀ Soft", Keymap.VK_TSOFT1, 2f, MOD), Key("Back", 0x1B, 1.4f, MOD), Key("Soft ▶", Keymap.VK_TSOFT2, 2f, MOD),
    )
    // A number row over four rows like the Xperia X1's keyboard; blue characters with Fn (the blue
    // dot key). The digits' blue characters are their shifted US symbols (Shift + digit gives the same).
    private val letterRows = listOf(
        listOf(Key("Esc", 0x1B, 1f, MOD), k('1', '!'), k('2', '@'), k('3', '#'), k('4', '$'), k('5', '%'),
            k('6', '^'), k('7', '&'), k('8', '*'), k('9', '('), k('0', ')'), Key("-", 0xBD, 1.3f, alt = '_')),
        // (Digits and their symbols are on the number row: the letters keep only the other marks.)
        listOf(Key("⊞", 0x5B, 1f, MOD), k('Q'), k('W', '"'), k('E'), k('R'), k('T'),
            k('Y'), k('U'), k('I'), k('O', '\''), k('P', '?'), Key("Del", 0x08, 1.3f, MOD)),
        listOf(Key("Tab", 0x09, 1.4f, MOD), k('A'), k('S'), k('D'), k('F'), k('G'),
            k('H', '/'), k('J', '+'), k('K', ':'), k('L', ';'), Key("↵", 0x0D, 1.9f, MOD)),
        listOf(Key("⇧", 0x10, 1.4f, SHIFT), typed('@'), k('Z', '~'), k('X'), k('C'), k('V'),
            k('B'), k('N', '='), k('M'), typed('.', '>'), typed(',', '<'), Key("⇧", 0x10, 1.3f, SHIFT)),
        listOf(Key("●", 0, 1f, FN), Key("Sym", 0, 1.2f, FNLOCK), Key("Ctrl", 0x11, 1.2f, CTRL),
            Key("", 0x20, 4.6f), typed('*'), typed('#'), Key("OK", Keymap.VK_RETURN, 1.3f, MOD)),
    )
    private var keys = emptyList<Key>()
    private val pointerKey = HashMap<Int, Key>()
    private var shift = false
    private var ctrl = false
    private var alt = false
    private var fn = false
    private var fnLock = false

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = relayout()

    /** A finger went down on a key (the skin's key backlight wakes). */
    var onTouchDown: (() -> Unit)? = null

    /**
     * The height the skin keyboard needs in an area [h] high that was sized for the four letter
     * rows: one more row (and gap) on top for the number row at the same key size (the extra rows
     * get [h]'s row height, so the keys keep their size), or [h] when it already fits.
     */
    fun heightFor(h: Int): Int {
        if (useIme || h <= 0) return h
        val d = resources.displayMetrics.density
        val pad = 10 * d
        val gap = 6 * d
        val base = LETTER_ROWS
        val rowH = ((h - 2 * pad - gap * (base - 1)) / base).coerceAtMost(58 * d)
        val need = 2 * pad + letterRows.size * rowH + (letterRows.size - 1) * gap
        return maxOf(h, Math.ceil(need.toDouble()).toInt())
    }

    private fun relayout() {
        val rows = buildList {
            if (hotkeys && useIme) add(hotRowIme)
            if (!useIme) addAll(letterRows)
        }
        keys = rows.flatten()
        if (width == 0 || height == 0 || rows.isEmpty()) { invalidate(); return }
        val d = resources.displayMetrics.density
        val pad = 10 * d
        val gap = 6 * d
        // With the phone's keyboard, the hotkey row sits at the top; the keyboard covers the rest.
        val rowH = if (useIme) 52 * d else ((height - 2 * pad - gap * (rows.size - 1)) / rows.size).coerceAtMost(58 * d)
        val total = rows.size * rowH + (rows.size - 1) * gap
        // The keyboard sits at the bottom edge, like a slide-out's; the phone's keyboard gets the top row.
        var y = if (useIme) pad else height - pad - total
        val maxUnits = rows.maxOf { r -> r.sumOf { it.units.toDouble() }.toFloat() }
        for (r in rows) {
            val units = r.sumOf { it.units.toDouble() }.toFloat()
            val unitW = (width - 2 * pad - gap * (r.size - 1)) / maxOf(units, maxUnits * 0.9f)
            val rowW = units * unitW + gap * (r.size - 1)
            var x = (width - rowW) / 2
            for (k in r) {
                k.rect.set(x, y, x + k.units * unitW, y + rowH)
                x += k.units * unitW + gap
            }
            y += rowH + gap
        }
        text.textSize = rowH * 0.36f
        invalidate()
    }

    /** Draws one key (SkinLayout's glass keys, so the keyboard matches the rest of the skin). */
    var painter: ((Canvas, SkinLayout.Key) -> Unit)? = null

    override fun onDraw(c: Canvas) {
        val paint = painter ?: return
        for (k in keys) {
            val s = k.skin
            s.rect.set(k.rect)
            s.pressed = k.pressed || latched(k)
            s.label = when {
                k.kind == FN -> "●"
                k.label.length == 1 && k.label[0].isLetter() && !shift -> k.label.lowercase()
                k.label.isEmpty() -> " "
                else -> k.label
            }
            s.alt = k.alt?.toString()
            paint(c, s)
        }
    }

    private fun latched(k: Key) = (k.kind == SHIFT && shift) || (k.kind == CTRL && ctrl) || (k.kind == ALT && alt) ||
        (k.kind == FN && fn) || (k.kind == FNLOCK && fnLock)

    override fun onTouchEvent(e: MotionEvent): Boolean {
        val i = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val k = keys.firstOrNull { it.rect.contains(e.getX(i), e.getY(i)) } ?: return true
                pointerKey[e.getPointerId(i)] = k
                k.pressed = true
                onTouchDown?.invoke()
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                press(k)
                invalidate()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val k = pointerKey.remove(e.getPointerId(i)) ?: return true
                k.pressed = false
                release(k)
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        return true
    }

    /** Modifiers latch for the next key (tap Shift, then a letter); other keys go down and up. */
    private fun press(k: Key) {
        when (k.kind) {
            SHIFT -> { shift = !shift; return }
            CTRL -> { ctrl = !ctrl; return }
            ALT -> { alt = !alt; return }
            FN -> { fn = !fn; invalidate(); return }
            FNLOCK -> { fnLock = !fnLock; fn = false; invalidate(); return }
        }
        // The blue layer (and plain typed characters) go in as characters, with Shift if needed.
        val ch = if ((fn || fnLock) && k.alt != null) k.alt else k.ch
        if (ch != null) { typer.char(ch); return }
        if (ctrl) onKey(0x11, true)
        if (alt) onKey(0x12, true)
        if (shift && k.kind == NORMAL) onKey(0x10, true)
        onKey(k.vk, true)
    }

    private fun release(k: Key) {
        if (k.kind == SHIFT || k.kind == CTRL || k.kind == ALT || k.kind == FN || k.kind == FNLOCK) return
        val wasChar = ((fn || fnLock) && k.alt != null) || k.ch != null
        if (!wasChar) {
            onKey(k.vk, false)
            if (shift && k.kind == NORMAL) onKey(0x10, false)
            if (alt) onKey(0x12, false)
            if (ctrl) onKey(0x11, false)
        }
        if (k.kind == NORMAL) { shift = false; ctrl = false; alt = false; fn = false }
    }

    fun releaseAll() {
        pointerKey.values.toSet().forEach { it.pressed = false; release(it) }
        pointerKey.clear()
        invalidate()
    }

    // ---- The phone's keyboard ----

    fun showIme() {
        if (!useIme || !isAttachedToWindow) return
        requestFocus()
        context.getSystemService(InputMethodManager::class.java)?.showSoftInput(this, 0)
    }

    fun hideIme() {
        context.getSystemService(InputMethodManager::class.java)?.hideSoftInputFromWindow(windowToken, 0)
    }

    override fun onCheckIsTextEditor() = useIme

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!useIme) return null
        return GuestTyper.connection(this, outAttrs, typer)  // shared with ImeBridge (GuestTyper.kt)
    }

    /** Characters go in key by key on a US layout (GuestTyper.HardwareKeys). */
    private val typer = GuestTyper.HardwareKeys(onKey)

    companion object {
        private const val NORMAL = 0
        private const val MOD = 1
        private const val SHIFT = 2
        private const val CTRL = 3
        private const val ALT = 4
        private const val TALK = 5
        private const val END = 6
        private const val FN = 7
        private const val FNLOCK = 8
        /** The letter rows (without the number row) the keyboard's area is sized for. */
        private const val LETTER_ROWS = 4
    }
}
