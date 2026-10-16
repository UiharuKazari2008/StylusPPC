package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.app.AlertDialog
import android.os.Environment
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import java.io.File

/**
 * Picks any folder on the phone (including Download, Android, the storage root) using plain file
 * access. Android's own picker hides some folders "for your privacy"; with All files access the
 * app can browse everything itself.
 */
class FolderBrowser(private val activity: Activity, private val onPicked: (File) -> Unit) {
    private val root = Environment.getExternalStorageDirectory()
    private var current = root
    private lateinit var title: TextView
    private lateinit var adapter: ArrayAdapter<String>
    private var entries = listOf<File>()
    private lateinit var dialog: AlertDialog

    fun show() {
        val pad = (16 * activity.resources.displayMetrics.density).toInt()
        title = TextView(activity).apply { setPadding(pad, pad / 2, pad, pad / 2) }
        adapter = ArrayAdapter(activity, android.R.layout.simple_list_item_1, mutableListOf())
        val list = ListView(activity).apply {
            this.adapter = this@FolderBrowser.adapter
            setOnItemClickListener { _, _, pos, _ ->
                current = if (pos == 0 && current != root) current.parentFile ?: root else entries[pos - offset()]
                refresh()
            }
        }
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(title)
            addView(list, LinearLayout.LayoutParams(-1, (activity.resources.displayMetrics.heightPixels * 0.55).toInt()))
        }
        dialog = AlertDialog.Builder(activity)
            .setTitle("Choose a folder")
            .setView(content)
            .setPositiveButton("Use this folder") { _, _ -> onPicked(current) }
            .setNegativeButton("Cancel", null)
            .create()
        refresh()
        dialog.show()
    }

    private fun offset() = if (current != root) 1 else 0

    private fun refresh() {
        title.text = current.path
        entries = (current.listFiles { f -> f.isDirectory } ?: emptyArray())
            .sortedBy { it.name.lowercase() }
        adapter.clear()
        if (current != root) adapter.add("⬆  ..")
        adapter.addAll(entries.map { "📁  ${it.name}" })
        adapter.notifyDataSetChanged()
    }
}
