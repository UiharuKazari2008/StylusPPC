package jp.net.seventhreeseven.wmdroid

import android.content.Context
import org.json.JSONObject

/**
 * A skin as made in tools/skin-designer.html: up to four layouts (the cover screen, the cover
 * screen with the D-pad on, the open foldable, and the open foldable with the phone's own
 * keyboard), each a list of items placed in fractions of
 * the window, so one skin fits any phone of that shape.
 *
 * JSON: {"version":1, "backdrop":"silver", "opacity":40,
 *        "front"|"frontDpad"|"inside" (or "foldable")|"insideIme" (or "foldableIme"):
 *            {"canvas":{"w","h"}, "crease":50,
 *            "items":[{"id","type":"screen|key|dpad|lock|keyboard","action","label","x","y","w","h"}]}}
 */
class SkinSpec(
    val backdrop: String?,
    val opacity: Int?,
    val front: Layout?,
    val frontDpad: Layout?,
    val foldable: Layout?,
    /** The open foldable with the phone's own keyboard: its "keyboard" item is the hotkey row. */
    val foldableIme: Layout?,
) {
    class Layout(val items: List<Item>)

    class Item(
        val id: String, val type: String, val action: String?, val label: String?,
        val x: Float, val y: Float, val w: Float, val h: Float,
    )

    companion object {
        /** Parses a skin; throws IllegalArgumentException with a readable reason when it is not one. */
        fun parse(json: String): SkinSpec {
            val o = try { JSONObject(json) } catch (e: Exception) { throw IllegalArgumentException("not valid JSON: ${e.message}") }
            fun layout(vararg names: String): Layout? {
                val l = names.firstNotNullOfOrNull { o.optJSONObject(it) } ?: return null
                val arr = l.optJSONArray("items") ?: throw IllegalArgumentException("a layout has no items")
                val items = (0 until arr.length()).map { i ->
                    val it = arr.getJSONObject(i)
                    Item(
                        it.optString("id", "item$i"), it.optString("type", "key"),
                        it.optString("action").ifEmpty { null }, it.optString("label").ifEmpty { null },
                        it.optDouble("x", 0.0).toFloat(), it.optDouble("y", 0.0).toFloat(),
                        it.optDouble("w", 0.0).toFloat(), it.optDouble("h", 0.0).toFloat(),
                    )
                }
                if (items.none { it.type == "screen" }) throw IllegalArgumentException("a layout has no screen")
                return Layout(items)
            }
            val spec = SkinSpec(
                o.optString("backdrop").ifEmpty { null },
                if (o.has("opacity")) o.optInt("opacity") else null,
                layout("front"), layout("frontDpad", "front-dpad", "front_dpad"), layout("foldable", "inside"),
                layout("foldableIme", "foldable-ondevkeyboard", "insideIme", "inside-ime"),
            )
            if (spec.front == null && spec.frontDpad == null && spec.foldable == null)
                throw IllegalArgumentException("no front, frontDpad or inside layout")
            return spec
        }

        /** The user's imported skin, else the built-in one (assets/skins/default.json). */
        fun load(ctx: Context, settings: Settings): SkinSpec? {
            settings.customSkin.takeIf { it.isNotBlank() }?.let { runCatching { return parse(it) } }
            return runCatching { parse(ctx.assets.open("skins/default.json").bufferedReader().readText()) }.getOrNull()
        }

        /** Windows virtual key for a designer action (0 = none). */
        fun actionVk(action: String?): Int = when (action) {
            "talk" -> Keymap.VK_TTALK
            "end" -> Keymap.VK_TEND
            "soft1" -> Keymap.VK_TSOFT1
            "soft2" -> Keymap.VK_TSOFT2
            "start" -> Keymap.VK_BUTTON1
            "back" -> 0x1B
            "ok" -> Keymap.VK_BUTTON4
            "volup" -> Keymap.VK_TVOLUMEUP
            "voldown" -> Keymap.VK_TVOLUMEDOWN
            "app1" -> Keymap.VK_APP1
            "app2" -> Keymap.VK_APP2
            "up" -> Keymap.VK_UP
            "down" -> Keymap.VK_DOWN
            "left" -> 0x25
            "right" -> 0x27
            else -> 0
        }

        /** Default text on a key without a label (none: the plain glass mark). */
        fun actionLabel(action: String?): String? = when (action) {
            "ok" -> "OK"
            "start" -> "⊞"
            "back" -> "↩"
            "volup" -> "+"
            "voldown" -> "−"
            "up" -> "▲"
            "down" -> "▼"
            "left" -> "◀"
            "right" -> "▶"
            else -> null
        }
    }
}
