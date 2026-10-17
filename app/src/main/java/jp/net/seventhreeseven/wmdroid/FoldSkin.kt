package jp.net.seventhreeseven.wmdroid

import android.graphics.Rect
import android.graphics.RectF

/**
 * Geometry of the open-foldable layout (after the Xperia X1's slide-out design): the window is
 * landscape with the fold across the middle. The upper half holds the landscape display with a
 * key column on its right (Talk / OK, the square D-pad, End / Start); the lower half holds the
 * [FoldKeyboard]. No frame or hinge is drawn: SkinLayout draws the backdrop and every key in the
 * same glass style as on the cover screen.
 */
class FoldSkin {
    val screen = Rect()        // where the display goes
    val keyboard = Rect()      // keyboard view (lower half)
    val pad = RectF()          // square D-pad
    val padCentre = RectF()

    val talk = SkinLayout.Key(Keymap.VK_TTALK, 0).apply { label = "✆" }
    val ok = SkinLayout.Key(Keymap.VK_BUTTON4, 0).apply { label = "OK" }
    val end = SkinLayout.Key(Keymap.VK_TEND, 0).apply { label = "✆" }
    val start = SkinLayout.Key(Keymap.VK_BUTTON1, 0).apply { label = "⊞" }
    val keys = listOf(talk, ok, end, start)

    /**
     * [creaseTop]..[creaseBottom]: the fold in view coordinates; [aspect]: display width / height;
     * [topInset]: below the lock button. Like the cover screen the display runs edge to edge: from
     * the left edge, down to just above the fold (~48% of the height); keys fill the rest of the row.
     * [fill]: use that whole place, so a shorter guest picture is not pulled in from the edges.
     */
    fun layout(w: Int, h: Int, creaseTop: Int, creaseBottom: Int, aspect: Float, topInset: Float, fill: Boolean = false) {
        val m = h * 0.015f
        val y0 = topInset + m
        val bottom = minOf(h * 0.48f, creaseTop - m)
        var sh = bottom - y0
        val maxW = w * 0.8f
        var sw = if (fill) maxW else sh * aspect
        if (!fill && sw > maxW) { sw = maxW; sh = sw / aspect }
        screen.set(0, y0.toInt(), sw.toInt(), (y0 + sh).toInt())
        // Key column to the right of the display.
        val col = RectF(sw + 2 * m, y0, w - 2 * m, y0 + sh)
        val g = col.width() * 0.05f
        val rowH = col.height() * 0.2f
        talk.rect.set(col.left, col.top, col.centerX() - g / 2, col.top + rowH)
        ok.rect.set(col.centerX() + g / 2, col.top, col.right, col.top + rowH)
        end.rect.set(col.left, col.bottom - rowH, col.centerX() - g / 2, col.bottom)
        start.rect.set(col.centerX() + g / 2, col.bottom - rowH, col.right, col.bottom)
        val side = minOf(col.width(), col.height() - 2 * rowH - 4 * g)
        pad.set(col.centerX() - side / 2, col.centerY() - side / 2, col.centerX() + side / 2, col.centerY() + side / 2)
        val c = side * 0.19f
        padCentre.set(pad.centerX() - c, pad.centerY() - c, pad.centerX() + c, pad.centerY() + c)
        // Lower half: the keyboard.
        keyboard.set((2 * m).toInt(), (creaseBottom + m).toInt(), (w - 2 * m).toInt(), (h - m).toInt())
    }
}
