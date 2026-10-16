package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.net.Uri
import java.io.File

/**
 * ROM images live in the app's external files dir, one folder per profile:
 *   Android/data/<pkg>/files/images/<profileId>/<fileName>
 * Images can be imported with the system file picker or pushed with adb (tools/push-images.ps1).
 */
object ImageStore {
    fun root(ctx: Context): File = File(ctx.getExternalFilesDir(null), "images").apply { mkdirs() }

    fun dir(ctx: Context, profile: OsProfile): File = File(root(ctx), profile.id).apply { mkdirs() }

    fun find(ctx: Context, profile: OsProfile, variant: ImageVariant): File? {
        val d = dir(ctx, profile)
        return d.listFiles()?.firstOrNull { it.name.equals(variant.fileName, ignoreCase = true) && it.length() > 0 }
    }

    fun available(ctx: Context, profile: OsProfile): List<ImageVariant> =
        profile.variants.filter { find(ctx, profile, it) != null }

    /** Copies a picked file into the profile folder under the variant's canonical name. */
    fun import(ctx: Context, profile: OsProfile, variant: ImageVariant, uri: Uri): File {
        val out = File(dir(ctx, profile), variant.fileName)
        val tmp = File(out.path + ".part")
        ctx.contentResolver.openInputStream(uri)!!.use { input ->
            tmp.outputStream().use { input.copyTo(it, 1 shl 20) }
        }
        if (!tmp.renameTo(out)) throw IllegalStateException("could not store image")
        return out
    }
}
