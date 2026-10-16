package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton

/** First start: choose which Windows Mobile to boot. */
class SetupActivity : AppCompatActivity() {
    private lateinit var picker: OsPicker

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val settings = Settings(this)
        picker = OsPicker(this, settings)
        val d = resources.displayMetrics.density
        val pad = (16 * d).toInt()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        root.addView(TextView(this).apply {
            text = "Choose a Windows Mobile"
            textSize = 26f
        })
        root.addView(TextView(this).apply {
            text = "Pick the version to boot. You can switch any time in Settings."
            alpha = 0.7f
            setPadding(0, 0, 0, pad)
        })
        root.addView(picker.view)
        root.addView(MaterialButton(this).apply {
            text = "Start"
            setOnClickListener {
                if (!picker.ready()) {
                    Toast.makeText(this@SetupActivity, "Import an image for this version first", Toast.LENGTH_LONG).show()
                    return@setOnClickListener
                }
                picker.save()
                startActivity(Intent(this@SetupActivity, EmulatorActivity::class.java))
                finish()
            }
        })
        setContentView(ScrollView(this).apply { addView(root) })
    }

    override fun onResume() {
        super.onResume()
        picker.rebuild()  // images may have been pushed over adb meanwhile
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        picker.onActivityResult(requestCode, resultCode, data)
    }
}
