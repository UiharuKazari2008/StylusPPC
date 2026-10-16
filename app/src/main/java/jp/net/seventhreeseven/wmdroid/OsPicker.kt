package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import android.widget.Toast
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import kotlin.concurrent.thread

/**
 * The OS + screen picker shared by first-run setup and Settings. Shows every profile with the
 * state of its images and lets the user import a missing one.
 */
class OsPicker(private val activity: Activity, private val settings: Settings) {
    private val ctx = activity
    val view = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
    private var pendingImport: Pair<OsProfile, ImageVariant>? = null
    private var pendingFolder: OsProfile? = null

    var selectedProfile: OsProfile? = settings.profile
    var selectedVariant: ImageVariant? = settings.variant

    init { rebuild() }

    fun rebuild() {
        view.removeAllViews()
        for (profile in OsProfiles.all) view.addView(card(profile))
    }

    private fun dp(v: Int) = (v * ctx.resources.displayMetrics.density).toInt()

    private fun card(profile: OsProfile): View {
        val selected = selectedProfile?.id == profile.id
        val card = MaterialCardView(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(-1, -2).apply { setMargins(0, dp(6), 0, dp(6)) }
            isCheckable = true
            isChecked = selected
            strokeWidth = if (selected) dp(2) else dp(1)
            setOnClickListener {
                selectedProfile = profile
                val avail = ImageStore.available(ctx, profile)
                selectedVariant = avail.firstOrNull() ?: profile.variants.first()
                rebuild()
            }
        }
        val col = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(12), dp(16), dp(12))
        }
        col.addView(TextView(ctx).apply {
            text = profile.title
            textSize = 17f
            setTypeface(typeface, Typeface.BOLD)
        })
        val avail = ImageStore.available(ctx, profile)
        col.addView(TextView(ctx).apply {
            text = "${profile.subtitle} · " +
                if (avail.isEmpty()) "no image yet" else "${avail.size} of ${profile.variants.size} images ready"
            alpha = 0.7f
        })
        if (selected) {
            val group = RadioGroup(ctx).apply { setPadding(0, dp(8), 0, 0) }
            for (v in profile.variants) {
                val have = ImageStore.find(ctx, profile, v) != null
                val row = LinearLayout(ctx).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }
                val rb = RadioButton(ctx).apply {
                    text = v.label + if (have) "" else "  (missing)"
                    isEnabled = have
                    isChecked = have && selectedVariant?.id == v.id
                    layoutParams = LinearLayout.LayoutParams(0, -2, 1f)
                    setOnClickListener { selectedVariant = v; rebuild() }
                }
                row.addView(rb)
                if (!have) row.addView(MaterialButton(ctx, null, com.google.android.material.R.attr.borderlessButtonStyle).apply {
                    text = "Select ROM image…"
                    setOnClickListener { startImport(profile, v) }
                })
                group.addView(row)
            }
            col.addView(group)
            col.addView(TextView(ctx).apply {
                text = "File: ${profile.archiveFolder}/Emulation Images/<name>\n" +
                    "or push to ${ImageStore.dir(ctx, profile).path}"
                textSize = 11f
                alpha = 0.6f
                setPadding(0, dp(6), 0, 0)
            })
        }
        // Every release, picked or not: point at its "Emulation Images" folder and every image this
        // release knows (matched by file name) is copied in at once.
        if (avail.size < profile.variants.size) col.addView(MaterialButton(ctx, null, com.google.android.material.R.attr.borderlessButtonStyle).apply {
            text = if (avail.isEmpty()) "Select ROM folder…" else "Add missing images from a folder…"
            setOnClickListener { startFolderImport(profile) }
        })
        card.addView(col)
        return card
    }

    private fun startFolderImport(profile: OsProfile) {
        pendingFolder = profile
        activity.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_IMPORT_DIR)
    }

    /** Imports every file in the picked folder (and one level of subfolders) named like one of [profile]'s images. */
    private fun importFolder(profile: OsProfile, tree: Uri): Int {
        val cr = ctx.contentResolver
        val wanted = profile.variants.filter { ImageStore.find(ctx, profile, it) == null }
        var count = 0
        fun scan(docId: String, depth: Int) {
            val children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
            cr.query(children, arrayOf(android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
                while (c.moveToNext()) {
                    val id = c.getString(0)
                    val name = c.getString(1) ?: continue
                    if (c.getString(2) == android.provider.DocumentsContract.Document.MIME_TYPE_DIR) {
                        if (depth < 2) scan(id, depth + 1)
                        continue
                    }
                    val v = wanted.firstOrNull { it.fileName.equals(name, ignoreCase = true) } ?: continue
                    if (ImageStore.find(ctx, profile, v) != null) continue
                    ImageStore.import(ctx, profile, v, android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, id))
                    count++
                }
            }
        }
        scan(android.provider.DocumentsContract.getTreeDocumentId(tree), 0)
        return count
    }

    private fun startImport(profile: OsProfile, variant: ImageVariant) {
        pendingImport = profile to variant
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        activity.startActivityForResult(i, REQ_IMPORT)
    }

    /** Forward from Activity.onActivityResult. */
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQ_IMPORT_DIR && resultCode == Activity.RESULT_OK) {
            val tree = data?.data ?: return
            val profile = pendingFolder ?: return
            Toast.makeText(ctx, "Looking for ${profile.title} images…", Toast.LENGTH_SHORT).show()
            thread {
                val msg = try {
                    val n = importFolder(profile, tree)
                    if (n > 0) {
                        selectedProfile = profile
                        selectedVariant = ImageStore.available(ctx, profile).firstOrNull()
                    }
                    if (n == 0) "No ${profile.title} images in that folder (looking for " +
                        profile.variants.joinToString { it.fileName } + ")" else "Imported $n image${if (n == 1) "" else "s"}"
                } catch (e: Exception) {
                    "Import failed: ${e.message}"
                }
                activity.runOnUiThread {
                    Toast.makeText(ctx, msg, Toast.LENGTH_LONG).show()
                    rebuild()
                }
            }
            return
        }
        if (requestCode != REQ_IMPORT || resultCode != Activity.RESULT_OK) return
        val uri: Uri = data?.data ?: return
        val (profile, variant) = pendingImport ?: return
        Toast.makeText(ctx, "Importing ${variant.fileName}…", Toast.LENGTH_SHORT).show()
        thread {
            val msg = try {
                ImageStore.import(ctx, profile, variant, uri)
                selectedProfile = profile
                selectedVariant = variant
                "Imported ${variant.label}"
            } catch (e: Exception) {
                "Import failed: ${e.message}"
            }
            activity.runOnUiThread {
                Toast.makeText(ctx, msg, Toast.LENGTH_LONG).show()
                rebuild()
            }
        }
    }

    /** True if the current selection can boot. */
    fun ready(): Boolean {
        val p = selectedProfile ?: return false
        val v = selectedVariant ?: return false
        return ImageStore.find(ctx, p, v) != null
    }

    fun save() {
        settings.profileId = selectedProfile?.id
        settings.variantId = selectedVariant?.id
    }

    companion object {
        const val REQ_IMPORT = 41
        const val REQ_IMPORT_DIR = 42
    }
}
