package jp.net.seventhreeseven.wmdroid

import android.content.Context
import org.json.JSONObject

/**
 * A skin as made in tools/skin-designer.html: the cover screen, the cover screen with the D-pad
 * on, the open foldable, the open foldable with the phone's own keyboard, Foldable Max (the
 * screen fills the open phone), Foldable Max with that keyboard, and the Standard keypad. Each
 * layout is a list of items placed in fractions of the window, so one skin fits any phone of
 * that shape.
 *
 * The "standard" layout is the keypad phone for Windows Mobile Standard (no touch screen): soft
 * keys, Home, Back, Talk / End, the D-pad with its action button and the number pad.
 *
 * A "dock" item is the rectangle the Android app dock is drawn in. Layouts without one keep the
 * automatic placement.
 *
 * JSON: {"version":1, "backdrop":"silver", "opacity":40,
 *        "front"|"frontDpad"|"inside" (or "foldable")|"insideIme" (or "foldableIme")|
 *        "insideMax" (or "foldableMax")|"insideMaxIme" (or "foldableMaxIme")|"standard":
 *            {"canvas":{"w","h"}, "crease":50,
 *            "items":[{"id","type":"screen|key|dpad|lock|keyboard|led|rect|image|dock","action","label",
 *                      "x","y","w","h","radius","color","image"}]}}
 */
class SkinSpec(
    val backdrop: String?,
    val opacity: Int?,
    val front: Layout?,
    val frontDpad: Layout?,
    val foldable: Layout?,
    /** The open foldable with the phone's own keyboard: its "keyboard" item is the hotkey row. */
    val foldableIme: Layout?,
    /** Windows Mobile Standard's keypad phone (a skin without one gets the built-in skin's). */
    val standard: Layout? = null,
    /** Open foldable, maximized: the screen takes most of the window. The hinge is not drawn. */
    val foldableMax: Layout? = null,
    /** Maximized, with the phone's own keyboard: the screen fills the area above that keyboard. */
    val foldableMaxIme: Layout? = null,
) {
    class Layout(val items: List<Item>)

    class Item(
        val id: String, val type: String, val action: String?, val label: String?,
        val x: Float, val y: Float, val w: Float, val h: Float,
        /** Corner radius as a fraction of the item's shorter side, 0..0.5. */
        val radius: Float = 0f,
        /** Fill for a rect, #RRGGBB or #AARRGGBB. */
        val color: Int = 0xFF8A9098.toInt(),
        /** A logo, as a data URL or raw base64. */
        val image: String? = null,
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
                        it.optDouble("radius", 0.0).toFloat().coerceIn(0f, 0.5f),
                        colorOf(it.optString("color")),
                        it.optString("image").ifEmpty { null },
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
                layout("standard", "keypad"),
                layout("foldableMax", "insideMax", "foldable-max", "inside-max"),
                layout("foldableMaxIme", "insideMaxIme", "foldable-max-keyboard", "inside-max-ime"),
            )
            if (spec.front == null && spec.frontDpad == null && spec.foldable == null && spec.standard == null)
                throw IllegalArgumentException("no front, frontDpad, inside or standard layout")
            return spec
        }

        /** The user's imported skin, else the built-in one (assets/skins/default.json). */
        fun load(ctx: Context, settings: Settings): SkinSpec? {
            val builtIn = runCatching { parse(ctx.assets.open("skins/default.json").bufferedReader().readText()) }.getOrNull()
            settings.customSkin.takeIf { it.isNotBlank() }?.let {
                runCatching {
                    val own = parse(it)
                    // Layouts added after a skin was made (the Standard keypad, Foldable Max) stay
                    // the built-in ones until that skin defines its own.
                    return SkinSpec(
                        own.backdrop, own.opacity, own.front, own.frontDpad, own.foldable, own.foldableIme,
                        own.standard ?: builtIn?.standard,
                        own.foldableMax ?: builtIn?.foldableMax,
                        own.foldableMaxIme ?: builtIn?.foldableMaxIme,
                    )
                }
            }
            return builtIn
        }

        /** #RRGGBB or #AARRGGBB. Anything else is the default border grey. */
        fun colorOf(s: String?): Int {
            val h = s?.removePrefix("#") ?: return 0xFF8A9098.toInt()
            val n = runCatching { h.toLong(16) }.getOrNull() ?: return 0xFF8A9098.toInt()
            return when (h.length) {
                6 -> (0xFF000000 or n).toInt()
                8 -> n.toInt()
                else -> 0xFF8A9098.toInt()
            }
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
            // Windows Mobile Standard's keypad (the "standard" layout).
            "home" -> Keymap.VK_THOME
            "action" -> Keymap.VK_RETURN
            "star" -> Keymap.VK_TSTAR
            "pound" -> Keymap.VK_TPOUND
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" -> action[0].code
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
            "home" -> "⊞"
            "action" -> "●"
            "star" -> "*"
            "pound" -> "#"
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" -> action
            else -> null
        }

        /** The letters (or symbol) printed beside a number key's digit. */
        fun actionAlt(action: String?): String? = when (action) {
            "1" -> "∞"
            "2" -> "ABC"
            "3" -> "DEF"
            "4" -> "GHI"
            "5" -> "JKL"
            "6" -> "MNO"
            "7" -> "PQRS"
            "8" -> "TUV"
            "9" -> "WXYZ"
            "0" -> "+"
            "star" -> "⇧"
            "pound" -> "␣"
            else -> null
        }
    }
}
