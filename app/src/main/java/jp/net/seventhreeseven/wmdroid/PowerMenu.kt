package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

/**
 * The lock button's long-press panel: two rows of icons under [anchor], drawn in [layer]
 * (a full-window overlay; a tap outside the panel closes it). Items that need a yes turn
 * the panel into the question.
 */
class PowerMenu(private val ctx: Context, private val layer: FrameLayout, private val anchor: () -> android.graphics.RectF) {
    /** [confirm]: (question, explanation, yes-button) asked first; [warn] tints it as destructive. */
    class Item(
        val title: String,
        val hint: String? = null,
        val icon: String = "dot",
        val warn: Boolean = false,
        val confirm: Triple<String, String, String>? = null,
        val action: () -> Unit,
    )

    /** Called whenever the panel opens or closes (e.g. to route Back to [close]). */
    var onOpenChanged: ((Boolean) -> Unit)? = null

    val isOpen get() = layer.visibility == View.VISIBLE

    init {
        layer.visibility = View.GONE
        layer.isClickable = true
        layer.setOnClickListener { close() }
    }

    private fun dp(v: Float) = (v * ctx.resources.displayMetrics.density).toInt()

    /** Items in two rows. Null entries are skipped. */
    fun show(items: List<Item?>) {
        val list = items.filterNotNull()
        val card = card()
        val cols = ((list.size + 1) / 2).coerceAtLeast(1)
        val cell = dp(76f)
        var i = 0
        repeat(2) { row ->
            if (i >= list.size) return@repeat
            val n = if (row == 0) cols else list.size - cols
            val line = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
            repeat(n) {
                val item = list[i++]
                line.addView(iconCell(item), LinearLayout.LayoutParams(0, -2, 1f))
            }
            repeat(cols - n) { line.addView(View(ctx), LinearLayout.LayoutParams(0, 0, 1f)) }
            card.addView(line, LinearLayout.LayoutParams(-1, -2))
        }
        layer.removeAllViews()
        val a = anchor()
        // Under the lock button, but always inside the window (the lock can sit at the right edge).
        val w = (cols * cell + dp(8f)).coerceAtMost(
            (if (layer.width > 0) layer.width else ctx.resources.displayMetrics.widthPixels) - dp(16f))
        val room = if (layer.width > 0) layer.width else ctx.resources.displayMetrics.widthPixels
        layer.addView(card, FrameLayout.LayoutParams(w, -2, Gravity.TOP or Gravity.START).apply {
            topMargin = a.bottom.toInt() + dp(6f)
            leftMargin = a.left.toInt().coerceIn(dp(8f), maxOf(dp(8f), room - w - dp(8f)))
        })
        layer.visibility = View.VISIBLE
        card.alpha = 0f
        card.animate().alpha(1f).setDuration(120).start()
        onOpenChanged?.invoke(true)
    }

    fun close() {
        if (!isOpen) return
        layer.visibility = View.GONE
        layer.removeAllViews()
        onOpenChanged?.invoke(false)
    }

    /** One icon with its name under it. */
    private fun iconCell(item: Item) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        setPadding(dp(4f), dp(8f), dp(4f), dp(6f))
        background = ripple()
        contentDescription = if (item.hint != null) "${item.title}. ${item.hint}" else item.title
        setOnClickListener {
            val q = item.confirm
            val panel = (parent as? View)?.parent as? LinearLayout
            if (q == null) { close(); item.action() } else if (panel != null) ask(panel, q, item.action)
        }
        addView(ImageView(ctx).apply {
            setImageBitmap(glyph(item.icon, if (item.warn) WARN else TEXT))
        }, LinearLayout.LayoutParams(dp(28f), dp(28f)))
        addView(TextView(ctx).apply {
            text = item.title
            textSize = 11f
            gravity = Gravity.CENTER
            maxLines = 2
            setTextColor(if (item.warn) WARN else TEXT)
            setPadding(0, dp(4f), 0, 0)
        }, LinearLayout.LayoutParams(-1, -2))
    }

    /** A small line icon, drawn once per show. */
    private fun glyph(kind: String, color: Int): Bitmap {
        val s = dp(28f)
        val bmp = Bitmap.createBitmap(s, s, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val p = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.color = color
            style = Paint.Style.STROKE
            strokeWidth = dp(1.7f).toFloat().coerceAtLeast(2f)
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        val u = s.toFloat()
        fun line(x1: Float, y1: Float, x2: Float, y2: Float) = c.drawLine(x1 * u, y1 * u, x2 * u, y2 * u, p)
        fun circle(cx: Float, cy: Float, r: Float) = c.drawCircle(cx * u, cy * u, r * u, p)
        when (kind) {
            "save" -> {
                line(0.22f, 0.72f, 0.78f, 0.72f)
                line(0.5f, 0.18f, 0.5f, 0.62f)
                line(0.32f, 0.46f, 0.5f, 0.64f)
                line(0.68f, 0.46f, 0.5f, 0.64f)
            }
            "suspend" -> {
                circle(0.5f, 0.52f, 0.28f)
                line(0.5f, 0.12f, 0.5f, 0.34f)
            }
            "pause" -> {
                p.style = Paint.Style.FILL
                c.drawRoundRect(0.28f * u, 0.18f * u, 0.42f * u, 0.82f * u, 2f, 2f, p)
                c.drawRoundRect(0.58f * u, 0.18f * u, 0.72f * u, 0.82f * u, 2f, 2f, p)
            }
            "resume" -> {
                p.style = Paint.Style.FILL
                val path = Path()
                path.moveTo(0.32f * u, 0.16f * u)
                path.lineTo(0.32f * u, 0.84f * u)
                path.lineTo(0.82f * u, 0.5f * u)
                path.close()
                c.drawPath(path, p)
            }
            "reset" -> {
                val oval = android.graphics.RectF(0.22f * u, 0.22f * u, 0.78f * u, 0.78f * u)
                c.drawArc(oval, 40f, 280f, false, p)
                line(0.72f, 0.18f, 0.78f, 0.38f)
                line(0.58f, 0.28f, 0.78f, 0.38f)
            }
            "cold" -> {
                line(0.5f, 0.16f, 0.5f, 0.84f)
                line(0.18f, 0.38f, 0.82f, 0.72f)
                line(0.82f, 0.38f, 0.18f, 0.72f)
                circle(0.5f, 0.5f, 0.08f)
                p.style = Paint.Style.FILL
                c.drawCircle(0.5f * u, 0.5f * u, 0.06f * u, p)
            }
            "off" -> {
                circle(0.5f, 0.54f, 0.26f)
                p.strokeWidth = dp(2.2f).toFloat()
                line(0.5f, 0.12f, 0.5f, 0.42f)
            }
            "settings" -> {
                line(0.18f, 0.28f, 0.82f, 0.28f)
                line(0.18f, 0.5f, 0.82f, 0.5f)
                line(0.18f, 0.72f, 0.82f, 0.72f)
                p.style = Paint.Style.FILL
                c.drawCircle(0.34f * u, 0.28f * u, 0.07f * u, p)
                c.drawCircle(0.66f * u, 0.5f * u, 0.07f * u, p)
                c.drawCircle(0.42f * u, 0.72f * u, 0.07f * u, p)
            }
            "max" -> {
                line(0.22f, 0.42f, 0.22f, 0.22f)
                line(0.22f, 0.22f, 0.42f, 0.22f)
                line(0.78f, 0.42f, 0.78f, 0.22f)
                line(0.78f, 0.22f, 0.58f, 0.22f)
                line(0.22f, 0.58f, 0.22f, 0.78f)
                line(0.22f, 0.78f, 0.42f, 0.78f)
                line(0.78f, 0.58f, 0.78f, 0.78f)
                line(0.78f, 0.78f, 0.58f, 0.78f)
            }
            "audio" -> {
                p.style = Paint.Style.FILL
                val path = Path()
                path.moveTo(0.16f * u, 0.38f * u)
                path.lineTo(0.34f * u, 0.38f * u)
                path.lineTo(0.52f * u, 0.2f * u)
                path.lineTo(0.52f * u, 0.8f * u)
                path.lineTo(0.34f * u, 0.62f * u)
                path.lineTo(0.16f * u, 0.62f * u)
                path.close()
                c.drawPath(path, p)
                p.style = Paint.Style.STROKE
                c.drawArc(android.graphics.RectF(0.4f * u, 0.32f * u, 0.72f * u, 0.68f * u), -50f, 100f, false, p)
                c.drawArc(android.graphics.RectF(0.4f * u, 0.2f * u, 0.88f * u, 0.8f * u), -50f, 100f, false, p)
            }
            else -> circle(0.5f, 0.5f, 0.16f)
        }
        return bmp
    }

    /** Swaps the panel's items for a yes/no question. */
    private fun ask(card: LinearLayout, q: Triple<String, String, String>, action: () -> Unit) {
        card.removeAllViews()
        val pad = dp(18f)
        card.addView(TextView(ctx).apply {
            text = q.first
            textSize = 16f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(TEXT)
            setPadding(pad, dp(10f), pad, dp(4f))
        })
        card.addView(TextView(ctx).apply {
            text = q.second
            textSize = 13f
            setTextColor(HINT)
            setPadding(pad, 0, pad, dp(8f))
        })
        val buttons = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
            setPadding(0, 0, dp(6f), 0)
        }
        fun button(label: String, color: Int, onClick: () -> Unit) = buttons.addView(TextView(ctx).apply {
            text = label
            textSize = 15f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(color)
            setPadding(dp(14f), dp(10f), dp(14f), dp(10f))
            background = ripple()
            setOnClickListener { onClick() }
        })
        button("Cancel", TEXT) { close() }
        button(q.third, WARN) { close(); action() }
        card.addView(buttons, LinearLayout.LayoutParams(-1, -2))
    }

    /** Dark glass: a slight top-to-bottom gradient, rounded, with a thin light border. */
    private fun card() = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(6f), 0, dp(6f))
        elevation = dp(12f).toFloat()
        isClickable = true  // taps on the panel itself do not close it
        background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            intArrayOf(0xF22A2D33.toInt(), 0xF2141619.toInt())).apply {
            cornerRadius = dp(8f).toFloat()
            setStroke(maxOf(1, dp(1f)), 0x4DFFFFFF)
        }
    }

    private fun ripple() = RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), null, ColorDrawable(Color.WHITE))

    companion object {
        private val TEXT = 0xFFF2F3F5.toInt()
        private val HINT = 0x99FFFFFF.toInt()
        private val WARN = 0xFFFF8A80.toInt()

        /** The cover shown while paused from the menu: dimmed, a play button, and a tap anywhere resumes. */
        fun pausedCover(ctx: Context, onResume: () -> Unit): View {
            val d = ctx.resources.displayMetrics.density
            val play = ImageView(ctx).apply {
                setImageResource(android.R.drawable.ic_media_play)
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(0x33FFFFFF)
                    setStroke(maxOf(1, d.toInt()), 0x99FFFFFF.toInt())
                }
                val p = (20 * d).toInt()
                setPadding(p, p, p, p)
            }
            val column = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                addView(play, LinearLayout.LayoutParams((84 * d).toInt(), (84 * d).toInt()))
                addView(TextView(ctx).apply {
                    text = "Paused"
                    textSize = 22f
                    setTextColor(TEXT)
                    setPadding(0, (14 * d).toInt(), 0, 0)
                })
                addView(TextView(ctx).apply {
                    text = "Tap to resume"
                    textSize = 14f
                    setTextColor(HINT)
                })
            }
            return FrameLayout(ctx).apply {
                setBackgroundColor(0xB3000000.toInt())
                visibility = View.GONE
                isClickable = true
                contentDescription = "Paused. Tap to resume."
                setOnClickListener { onResume() }
                addView(column, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
            }
        }
    }
}
