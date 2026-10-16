package jp.net.seventhreeseven.wmdroid

import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.DynamicColors
import com.google.android.material.color.MaterialColors
import com.google.android.material.materialswitch.MaterialSwitch

/**
 * The look shared by WMDroid's settings screens: a title, sections as Material You cards holding
 * groups of controls, and an optional button pinned at the bottom.
 */
abstract class SettingsPage : AppCompatActivity() {
    protected lateinit var settings: Settings
    protected lateinit var root: LinearLayout
    /** The card currently being filled. */
    protected lateinit var card: LinearLayout

    private val d get() = resources.displayMetrics.density
    protected fun dp(v: Int) = (v * d).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(24))
        }
    }

    protected fun title(t: String) = root.addView(TextView(this).apply {
        text = t
        textSize = 28f
        setPadding(dp(8), dp(16), dp(8), dp(4))
    })

    protected fun section(title: String) {
        root.addView(TextView(this).apply {
            text = title
            textSize = 15f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            setTextColor(MaterialColors.getColor(root, com.google.android.material.R.attr.colorPrimary))
            setPadding(dp(8), dp(20), dp(8), dp(8))
        })
        card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(12))
        }
        root.addView(MaterialCardView(this).apply {
            radius = dp(20).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            setCardBackgroundColor(MaterialColors.getColor(root, com.google.android.material.R.attr.colorSurfaceContainer))
            addView(card)
        }, LinearLayout.LayoutParams(-1, -2))
    }

    /** A sub-heading inside the current card. */
    protected fun group(title: String) {
        if (card.childCount > 0) card.addView(View(this).apply {
            setBackgroundColor(MaterialColors.getColor(root, com.google.android.material.R.attr.colorOutlineVariant))
        }, LinearLayout.LayoutParams(-1, maxOf(1, dp(1) / 2)).apply { setMargins(0, dp(10), 0, dp(4)) })
        card.addView(TextView(this).apply {
            text = title
            textSize = 13f
            alpha = 0.75f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            setPadding(0, dp(8), 0, dp(4))
        })
    }

    protected fun note(t: String) = TextView(this).apply {
        text = t
        textSize = 13f
        alpha = 0.65f
        setPadding(0, dp(4), 0, dp(6))
    }.also { card.addView(it) }

    protected fun label(t: String) = card.addView(TextView(this).apply {
        text = t
        setPadding(0, dp(8), 0, 0)
    })

    protected fun switch(t: String, on: Boolean, live: ((Boolean) -> Unit)? = null) = MaterialSwitch(this).apply {
        text = t
        isChecked = on
        minHeight = dp(48)
        if (live != null) setOnCheckedChangeListener { _, v -> live(v) }
    }.also { card.addView(it, LinearLayout.LayoutParams(-1, -2)) }

    protected fun spinner(t: String?, items: List<String>, selected: Int, live: ((Int) -> Unit)? = null): Spinner {
        if (t != null) label(t)
        return Spinner(this).apply {
            adapter = ArrayAdapter(this@SettingsPage, android.R.layout.simple_spinner_dropdown_item, items)
            setSelection(selected.coerceIn(0, maxOf(0, items.size - 1)), false)
            if (live != null) onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
                override fun onItemSelected(p: android.widget.AdapterView<*>?, v: View?, pos: Int, id: Long) = live(pos)
                override fun onNothingSelected(p: android.widget.AdapterView<*>?) {}
            }
        }.also { card.addView(it, LinearLayout.LayoutParams(-1, -2)) }
    }

    /** A slider with its value shown next to the label; [live] gets each value the user picks. */
    protected fun slider(t: String, min: Int, max: Int, value: Int, unit: String = "", live: ((Int) -> Unit)? = null): SeekBar {
        val l = TextView(this).apply { setPadding(0, dp(8), 0, 0) }
        card.addView(l)
        fun show(v: Int) { l.text = "$t: $v$unit" }
        show(value)
        return SeekBar(this).apply {
            this.min = min
            this.max = max
            progress = value
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, v: Int, fromUser: Boolean) {
                    show(v)
                    if (fromUser) live?.invoke(v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
        }.also { card.addView(it, LinearLayout.LayoutParams(-1, -2)) }
    }

    protected fun button(t: String, outlined: Boolean = true, onClick: () -> Unit) =
        MaterialButton(this, null, if (outlined) com.google.android.material.R.attr.materialButtonOutlinedStyle
        else com.google.android.material.R.attr.borderlessButtonStyle).apply {
            text = t
            setOnClickListener { onClick() }
        }.also { card.addView(it, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) }) }

    /** Shows the page: the cards scroll, [bottom] (if any) stays pinned below them. */
    protected fun show(bottom: View? = null) {
        val screen = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(ScrollView(this@SettingsPage).apply { addView(root) }, LinearLayout.LayoutParams(-1, 0, 1f))
            if (bottom != null) addView(bottom, LinearLayout.LayoutParams(-1, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(dp(16), dp(8), dp(16), dp(12))
            })
        }
        ViewCompat.setOnApplyWindowInsetsListener(screen) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(screen)
    }
}
