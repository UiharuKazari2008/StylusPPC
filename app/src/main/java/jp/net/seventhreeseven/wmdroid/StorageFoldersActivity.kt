package jp.net.seventhreeseven.wmdroid

import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import java.io.File

/**
 * Chosen-folders mapping: the card's root folder and each phone folder shown in it, with the name
 * it has on the card (rename, remove, add). Nothing changes until Apply, which restarts Windows
 * Mobile with the new card (result OK).
 */
class StorageFoldersActivity : SettingsPage() {
    private var rootPath = ""
    private val folders = ArrayList<String>()
    private val names = HashMap<String, String>()
    private lateinit var list: LinearLayout
    private lateinit var rootLabel: TextView
    private lateinit var apply: MaterialButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        rootPath = settings.storageRoot
        folders += settings.storageFolders
        names += settings.storageFolderNames
        title("Storage folders")

        section("Card root")
        rootLabel = note("")
        showRoot()
        button("Choose root folder…") {
            if (!StorageBridge.hasAllFilesAccess()) startActivity(StorageBridge.allFilesAccessIntent(this))
            else FolderBrowser(this) { dir -> rootPath = dir.path; showRoot(); changed() }.show()
        }
        button("Use PocketStylus's own folder", outlined = false) { rootPath = ""; showRoot(); changed() }

        section("Folders on the card")
        note("Each appears in the root under its name. Tap a name to rename it.")
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        card.addView(list)
        refresh()
        button("Add folder…") {
            if (!StorageBridge.hasAllFilesAccess()) startActivity(StorageBridge.allFilesAccessIntent(this))
            else FolderBrowser(this) { dir ->
                if (dir.path !in folders) { folders += dir.path; refresh(); changed() }
            }.show()
        }

        apply = MaterialButton(this).apply {
            text = "Apply and restart Windows Mobile"
            isEnabled = false
            setOnClickListener {
                settings.storageRoot = rootPath
                settings.storageFolders = folders.toList()
                settings.storageFolderNames = names.filterKeys { it in folders }
                settings.storageMode = StorageBridge.MODE_FOLDERS
                setResult(RESULT_OK)
                finish()
            }
        }
        show(apply)
    }

    private fun showRoot() {
        rootLabel.text = "New files at the top of the card go to:\n" + rootPath.ifEmpty { "PocketStylus's own folder" }
    }

    private fun changed() { apply.isEnabled = true }

    private fun refresh() {
        list.removeAllViews()
        val shown = StorageBridge.uniqueNames(folders.map { File(it) }, names)
        folders.forEachIndexed { i, path ->
            list.addView(LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(0, dp(6), 0, dp(6))
                addView(LinearLayout(this@StorageFoldersActivity).apply {
                    orientation = LinearLayout.HORIZONTAL
                    addView(EditText(this@StorageFoldersActivity).apply {
                        setText(names[path] ?: shown[i])
                        setSingleLine()
                        hint = File(path).name
                        addTextChangedListener(object : TextWatcher {
                            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                            override fun afterTextChanged(s: Editable?) {
                                // Names can't hold characters Windows file names can't.
                                val n = s.toString().filter { it !in "\\/:*?\"<>|" }.trim()
                                if (n.isEmpty()) names.remove(path) else names[path] = n
                                changed()
                            }
                        })
                    }, LinearLayout.LayoutParams(0, -2, 1f))
                    addView(MaterialButton(this@StorageFoldersActivity, null, com.google.android.material.R.attr.borderlessButtonStyle).apply {
                        text = "Remove"
                        setOnClickListener { folders.remove(path); names.remove(path); refresh(); changed() }
                    })
                })
                addView(TextView(this@StorageFoldersActivity).apply {
                    text = path
                    textSize = 12f
                    alpha = 0.6f
                })
            })
        }
        if (folders.isEmpty()) list.addView(TextView(this).apply { text = "No folders yet."; alpha = 0.6f })
    }
}
