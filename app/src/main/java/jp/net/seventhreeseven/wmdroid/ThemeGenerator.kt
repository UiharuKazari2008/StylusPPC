package jp.net.seventhreeseven.wmdroid

import android.app.WallpaperManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.os.Build
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A Windows Mobile 6.5 theme made from Android's Material You palette: the four base colours and
 * colour overrides the shell derives its palette from, and the glass-style "theme primitives"
 * (selectors, title bars, soft-key bar, task bar, tab pivot) drawn in the same palette.
 *
 * Output goes to a folder the helper reads: PNGs plus theme.ini, one registry value per line as
 * `KEY|NAME|dword|sz|bin|VALUE`. `%T%` in a value stands for the folder the helper copies the
 * pictures to (see docs/research-themes.md for what each value does).
 */
class ThemeGenerator(private val ctx: Context) {
    /**
     * The colour roles the theme uses (ARGB), picked from the phone's palette. Windows Mobile draws
     * white text on its accent and keeps white app surfaces, so the accent is the brightest tone of
     * the phone's colour that still carries white text: tone 60 in dark mode (close to what the
     * system shows), tone 40 in light mode.
     */
    class Palette(
        val primary: Int, val onPrimary: Int, val primaryHi: Int, val primaryLo: Int, val primaryEdge: Int,
        val deep: Int, val tertiary: Int, val outline: Int, val bar: Int, val barLo: Int, val light: Int,
    ) {
        fun key() = listOf(primary, onPrimary, primaryHi, primaryLo, primaryEdge, deep, tertiary, outline, bar, barLo)
            .joinToString("") { "%06X".format(it and 0xFFFFFF) }
    }

    /** Tone table of one system palette: tone(0..1000 step as Android names them). */
    private fun systemTones(name: String): ((Int) -> Int)? {
        if (Build.VERSION.SDK_INT < 31) return null
        val res = ctx.resources
        return { n ->
            val id = res.getIdentifier("system_${name}_$n", "color", "android")
            if (id == 0) 0 else ctx.getColor(id)
        }
    }

    fun palette(): Palette {
        val dark = (ctx.resources.configuration.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK) ==
            android.content.res.Configuration.UI_MODE_NIGHT_YES
        val sys = systemTones("accent1")
        val a1: (Int) -> Int
        val a3: (Int) -> Int
        val n2: (Int) -> Int
        if (sys != null) {
            a1 = sys
            a3 = systemTones("accent3")!!
            n2 = systemTones("neutral2")!!
        } else {
            // Before Material You: tones of the wallpaper's main colour (n = 0 white .. 1000 black).
            val seed = runCatching {
                WallpaperManager.getInstance(ctx).getWallpaperColors(WallpaperManager.FLAG_SYSTEM)?.primaryColor?.toArgb()
            }.getOrNull() ?: 0xFF3F51B5.toInt()
            val hsv = FloatArray(3).also { Color.colorToHSV(seed, it) }
            fun tones(hue: Float, sat: Float): (Int) -> Int = { n ->
                val l = 1f - n / 1000f
                val hsl = floatArrayOf(hue, sat, l)
                androidx.core.graphics.ColorUtils.HSLToColor(hsl)
            }
            a1 = tones(hsv[0], maxOf(0.45f, hsv[1]))
            a3 = tones((hsv[0] + 60) % 360, 0.35f)
            n2 = tones(hsv[0], 0.08f)
        }
        // Material's tonal palettes tone the colour down; the theme keeps its full saturation:
        // the phone's chosen colour (its seed) at fixed lightness steps.
        val seed = seedColor() ?: a1(300)
        val hsl = FloatArray(3).also { androidx.core.graphics.ColorUtils.colorToHSL(seed, it) }
        val sat = maxOf(hsl[1], 0.7f)
        fun hue(l: Float, s: Float = sat) = androidx.core.graphics.ColorUtils.HSLToColor(floatArrayOf(hsl[0], s, l))
        // Title and soft-key bars: a mid tone of the colour (white text stays readable on it).
        val bar = hue(0.36f, minOf(sat, 0.55f))
        val barLo = hue(0.22f, minOf(sat, 0.5f))
        return if (dark) Palette(
            primary = hue(0.64f), onPrimary = Color.WHITE, primaryHi = hue(0.74f), primaryLo = hue(0.56f),
            primaryEdge = hue(0.42f), deep = hue(0.24f), tertiary = a3(200), outline = n2(400), bar = bar,
            barLo = barLo, light = n2(100),
        ) else Palette(
            primary = hue(0.46f), onPrimary = Color.WHITE, primaryHi = hue(0.58f), primaryLo = hue(0.38f),
            primaryEdge = hue(0.28f), deep = hue(0.22f), tertiary = a3(100), outline = n2(300), bar = bar,
            barLo = barLo, light = n2(100),
        )
    }

    /** The colour the phone's theme is built from (Android 12+ theme settings), if it says. */
    private fun seedColor(): Int? = runCatching {
        val json = android.provider.Settings.Secure.getString(ctx.contentResolver, "theme_customization_overlay_packages")
            ?: return@runCatching null
        val o = org.json.JSONObject(json)
        val hex = o.optString("android.theme.customization.system_palette")
            .ifEmpty { o.optString("android.theme.customization.accent_color") }
        if (hex.isEmpty()) null else (hex.toLong(16) or 0xFF000000L).toInt()
    }.getOrNull()

    /** Writes the theme into [dir]; returns the palette key (unchanged key = nothing to do). */
    fun write(dir: File, pal: Palette = palette()): String {
        dir.mkdirs()
        dir.listFiles()?.forEach { it.delete() }
        png(dir, "taskbar.png", taskBar(pal))
        png(dir, "softkeybar.png", bar(8, 68, pal, 0.5f))
        png(dir, "pivot.png", bar(8, 64, pal, 0.5f))
        png(dir, "menusel.png", selector(172, 72, pal))
        png(dir, "listsel.png", selector(474, 72, pal))
        png(dir, "title.png", title(389, 46, pal))

        val key = key(pal)
        val lines = mutableListOf("#hash=$key")
        fun dw(k: String, n: String, v: Int) = lines.add("$k|$n|dword|$v")
        fun sz(k: String, n: String, v: String) = lines.add("$k|$n|sz|$v")
        fun image(k: String, n: String, file: String, t: Int, b: Int, l: Int, r: Int) {
            sz(k, n, "%T%$file")
            dw(k, "${n}TopMargin", t); dw(k, "${n}BottomMargin", b)
            dw(k, "${n}LeftMargin", l); dw(k, "${n}RightMargin", r)
            dw(k, "${n}Tiled", 0)
        }
        val today = "HKCU\\Software\\Microsoft\\Today"
        val mct = "HKLM\\Software\\Microsoft\\Color\\MultiColorTheme"
        // COLORREF is 0x00BBGGRR.
        fun ref(c: Int) = (Color.blue(c) shl 16) or (Color.green(c) shl 8) or Color.red(c)
        dw(mct, "BaseColor1", ref(pal.deep))
        dw(mct, "BaseColor2", ref(pal.onPrimary))
        dw(mct, "BaseColor3", ref(pal.primary))
        dw(mct, "BaseColor4", ref(pal.tertiary))
        // Per-entry overrides of the 70-entry palette (flags, then values), as the built-in themes set them.
        val flags = IntArray(70)
        val values = IntArray(70)
        fun set(i: Int, c: Int) { flags[i] = 1; values[i] = ref(c) }
        // White as in the built-ins: these are surfaces (46 = message box body, 16/37 = calendar).
        for (i in intArrayOf(16, 37, 38, 45, 46, 55, 66)) set(i, Color.WHITE)
        set(54, pal.primary)
        set(47, pal.deep)
        set(58, pal.outline)
        lines.add("HKLM\\Software\\Microsoft\\Color|ThemeColors|bin|${b64(flags)}")
        lines.add("HKLM\\Software\\Microsoft\\Color|ThemeValues|bin|${b64(values)}")
        dw(today, "MultiColorThemeEnabled", 1)

        image("$today\\TaskBar", "TaskBarThemeImage", "taskbar.png", 0, 0, 0, 0)
        image("$today\\SoftKeyBar", "SoftKeyBarThemeImage", "softkeybar.png", 12, 12, 2, 2)
        image("$today\\PropertySheet", "PropertySheetPivotBackgroundThemeImage", "pivot.png", 12, 12, 2, 2)
        image("$today\\Menu", "MenuSelectorThemeImage", "menusel.png", 40, 16, 15, 15)
        image("$today\\ListView", "ListViewSelectorThemeImage", "listsel.png", 40, 16, 15, 15)
        image("$today\\MessageBox", "MessageBoxCaptionThemeImage", "title.png", 13, 4, 13, 13)
        image("$today\\Bubble", "BubbleTitleThemeImage", "title.png", 15, 2, 15, 15)
        image("$today\\Toast", "ToastTitleThemeImage", "title.png", 15, 2, 15, 15)

        File(dir, "theme.ini").writeText(lines.joinToString("\r\n", postfix = "\r\n"))
        return key
    }

    fun key(pal: Palette) = pal.key() + VERSION

    private fun b64(v: IntArray): String {
        val b = ByteBuffer.allocate(v.size * 4).order(ByteOrder.LITTLE_ENDIAN)
        v.forEach { b.putInt(it) }
        return android.util.Base64.encodeToString(b.array(), android.util.Base64.NO_WRAP)
    }

    private fun png(dir: File, name: String, b: Bitmap) {
        File(dir, name).outputStream().use { b.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    private fun vgrad(c: Canvas, r: RectF, top: Int, bottom: Int, path: Path? = null) {
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        if (path != null) c.drawPath(path, paint) else c.drawRect(r, paint)
        paint.shader = null
    }

    /** Title/status bar: tinted near-black, faint gloss on top, accent hairline at the bottom. */
    private fun taskBar(p: Palette): Bitmap {
        val b = Bitmap.createBitmap(480, 52, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        vgrad(c, RectF(0f, 0f, 480f, 52f), p.bar, p.barLo)
        vgrad(c, RectF(0f, 0f, 480f, 24f), 0x1CFFFFFF, 0x06FFFFFF)
        paint.color = p.primary
        c.drawRect(0f, 50f, 480f, 52f, paint)
        return b
    }

    /** Soft-key bar and tab pivot strip: the 6.5 two-tone glass, in the tinted neutrals. */
    private fun bar(w: Int, h: Int, p: Palette, split: Float): Bitmap {
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        val mid = h * split
        vgrad(c, RectF(0f, 0f, w.toFloat(), mid), blend(p.bar, Color.WHITE, 0.18f), p.bar)
        vgrad(c, RectF(0f, mid, w.toFloat(), h.toFloat()), p.barLo, blend(p.barLo, Color.BLACK, 0.4f))
        paint.color = blend(p.primary, p.bar, 0.35f)
        c.drawRect(0f, 0f, w.toFloat(), 1.5f, paint)
        return b
    }

    /** Menu and list selection: a rounded glass bar in the primary colour. */
    private fun selector(w: Int, h: Int, p: Palette): Bitmap {
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        val r = RectF(3f, 3f, w - 3f, h - 5f)
        val rad = 9f
        // Soft shadow under it.
        paint.style = Paint.Style.FILL
        paint.color = 0x40000000
        c.drawRoundRect(RectF(r.left + 1, r.top + 3, r.right - 1, r.bottom + 3), rad, rad, paint)
        val body = Path().apply { addRoundRect(r, rad, rad, Path.Direction.CW) }
        vgrad(c, r, p.primaryHi, p.primaryLo, body)
        // Gloss: the upper half lighter, with a soft hot spot in the middle.
        c.save()
        c.clipPath(body)
        val top = RectF(r.left, r.top, r.right, r.top + r.height() * 0.5f)
        vgrad(c, top, 0x66FFFFFF, 0x22FFFFFF)
        paint.shader = android.graphics.RadialGradient(r.centerX(), r.top, r.width() * 0.45f,
            0x40FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP)
        c.drawRect(top, paint)
        paint.shader = null
        c.restore()
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 2f
        paint.color = p.primaryEdge
        c.drawRoundRect(r, rad, rad, paint)
        paint.strokeWidth = 1f
        paint.color = 0x55FFFFFF
        c.drawRoundRect(RectF(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2), rad - 2, rad - 2, paint)
        paint.style = Paint.Style.FILL
        return b
    }

    /** Message box / notification caption: rounded top corners, primary glass, dark rim. */
    private fun title(w: Int, h: Int, p: Palette): Bitmap {
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        val r = RectF(1f, 1f, w - 1f, h.toFloat())
        val rad = 13f
        val shape = Path().apply {
            addRoundRect(r, floatArrayOf(rad, rad, rad, rad, 0f, 0f, 0f, 0f), Path.Direction.CW)
        }
        vgrad(c, r, p.primary, p.primaryLo, shape)
        c.save()
        c.clipPath(shape)
        vgrad(c, RectF(r.left, r.top, r.right, r.top + h * 0.45f), 0x3CFFFFFF, 0x10FFFFFF)
        c.restore()
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 2f
        paint.color = p.primaryEdge
        c.drawPath(shape, paint)
        paint.style = Paint.Style.FILL
        paint.color = p.light
        c.drawRect(0f, h - 2f, w.toFloat(), h.toFloat(), paint)
        return b
    }

    private fun blend(a: Int, b: Int, t: Float) = Color.rgb(
        (Color.red(a) + (Color.red(b) - Color.red(a)) * t).toInt(),
        (Color.green(a) + (Color.green(b) - Color.green(a)) * t).toInt(),
        (Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t).toInt(),
    )

    companion object {
        /** Bump when the drawing changes, so guests re-apply. */
        private const val VERSION = "v4"
    }
}
