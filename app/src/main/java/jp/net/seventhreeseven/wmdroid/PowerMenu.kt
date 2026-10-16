package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
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
 * The lock button's long-press panel: a small dark-glass list under [anchor], drawn in
 * [layer] (a full-window overlay; a tap outside the panel closes it). Items that need a yes
 * turn the panel into the question.
 */
class PowerMenu(private val ctx: Context, private val layer: FrameLayout, private val anchor: () -> android.graphics.RectF) {
    /** [confirm]: (question, explanation, yes-button) asked first; [warn] tints it as destructive. */
    class Item(
        val title: String,
        val hint: String? = null,
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

    /** Items; null adds a divider. */
    fun show(items: List<Item?>) {
        val card = card()
        for (item in items) {
            if (item == null) {
                card.addView(View(ctx).apply { setBackgroundColor(0x26FFFFFF) },
                    LinearLayout.LayoutParams(-1, maxOf(1, dp(1f))).apply { setMargins(0, dp(4f), 0, dp(4f)) })
                continue
            }
            card.addView(LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(18f), dp(9f), dp(18f), dp(9f))
                background = ripple()
                setOnClickListener {
                    val q = item.confirm
                    if (q == null) { close(); item.action() } else ask(card, q, item.action)
                }
                addView(TextView(ctx).apply {
                    text = item.title
                    textSize = 16f
                    setTextColor(if (item.warn) WARN else TEXT)
                })
                if (item.hint != null) addView(TextView(ctx).apply {
                    text = item.hint
                    textSize = 12f
                    setTextColor(HINT)
                })
            }, LinearLayout.LayoutParams(-1, -2))
        }
        layer.removeAllViews()
        val a = anchor()
        // Under the lock button, but always inside the window (the lock can sit at the right edge).
        val w = dp(264f)
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
