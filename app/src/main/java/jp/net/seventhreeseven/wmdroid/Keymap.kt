package jp.net.seventhreeseven.wmdroid

import android.view.KeyEvent

/** Windows virtual keys used by WM hardware buttons, and the phone's keys mapped onto them. */
object Keymap {
    const val VK_RETURN = 0x0D
    const val VK_UP = 0x26
    const val VK_DOWN = 0x28
    const val VK_TSOFT1 = 0x70
    const val VK_TSOFT2 = 0x71
    const val VK_TTALK = 0x72
    const val VK_TEND = 0x73
    const val VK_TVOLUMEUP = 0x75
    const val VK_TVOLUMEDOWN = 0x76
    // Windows Mobile's hardware buttons ("Button 1..4" in Settings > Personal > Buttons, where
    // each one's action is chosen). The skin's Start key is Button 1, App Launch 1 Button 2, App
    // Launch 2 Button 3 and OK Button 4. These values only name the buttons: sendKey turns each into
    // the key that does its action (HwButtons). Button 1 is 0xC5, not VK_APP1 0xC1: the board
    // keyboard driver takes VK 0xC1 as its Fn key, so Win+VK_APP1 can never reach WM.
    const val VK_BUTTON1 = 0xC5
    const val VK_BUTTON2 = 0xC2
    const val VK_BUTTON3 = 0xC3
    const val VK_BUTTON4 = 0xC4
    const val VK_APP1 = VK_BUTTON2
    const val VK_APP2 = VK_BUTTON3

    /** Functions the on-screen keys can have: (label, virtual key). */
    val keyChoices = listOf(
        "Talk (green)" to VK_TTALK,
        "Soft key 1 (left)" to VK_TSOFT1,
        "Soft key 2 (right)" to VK_TSOFT2,
        "End (red)" to VK_TEND,
        "Windows key" to 0x5B,
        "Back" to 0x1B,
        "Action (Enter)" to VK_RETURN,
        "Volume up" to VK_TVOLUMEUP,
        "Volume down" to VK_TVOLUMEDOWN,
        "Start (Button 1)" to VK_BUTTON1,
        "App launch 1 (Button 2)" to VK_BUTTON2,
        "App launch 2 (Button 3)" to VK_BUTTON3,
        "OK (Button 4)" to VK_BUTTON4,
    )

    /** What a volume key does. */
    enum class KeyAction(val id: String, val label: String) {
        PHONE("phone", "Phone volume (not sent to Windows Mobile)"),
        VOLUME("volume", "Windows Mobile volume"),
        DPAD("dpad", "D-pad up / down"),
        SELECT("select", "Select (action)"),
        SIDE("side", "Side key"),
        SOFT("soft", "Soft keys (left / right)"),
        TALK_END("talkend", "Talk / End"),
        LOCK("lock", "Lock the device"),
        ;

        companion object {
            fun of(id: String?, fallback: KeyAction) = entries.firstOrNull { it.id == id } ?: fallback
        }
    }

    /**
     * The guest key for a volume key under [action], [up] telling the two keys apart.
     * Null: leave the key to Android (PHONE) or handle it specially (LOCK).
     */
    fun volumeKey(action: KeyAction, up: Boolean): Int? = when (action) {
        KeyAction.PHONE, KeyAction.LOCK -> null
        KeyAction.VOLUME -> if (up) VK_TVOLUMEUP else VK_TVOLUMEDOWN
        KeyAction.DPAD -> if (up) VK_UP else VK_DOWN
        KeyAction.SELECT -> VK_RETURN
        KeyAction.SIDE -> VK_APP1
        KeyAction.SOFT -> if (up) VK_TSOFT1 else VK_TSOFT2
        KeyAction.TALK_END -> if (up) VK_TTALK else VK_TEND
    }

    private val map = buildMap {
        put(KeyEvent.KEYCODE_DPAD_UP, VK_UP)
        put(KeyEvent.KEYCODE_DPAD_DOWN, VK_DOWN)
        put(KeyEvent.KEYCODE_DPAD_LEFT, 0x25)
        put(KeyEvent.KEYCODE_DPAD_RIGHT, 0x27)
        put(KeyEvent.KEYCODE_DPAD_CENTER, VK_RETURN)
        put(KeyEvent.KEYCODE_ENTER, VK_RETURN)
        put(KeyEvent.KEYCODE_BACK, 0x08)
        put(KeyEvent.KEYCODE_DEL, 0x08)
        put(KeyEvent.KEYCODE_FORWARD_DEL, 0x2E)
        put(KeyEvent.KEYCODE_TAB, 0x09)
        put(KeyEvent.KEYCODE_ESCAPE, 0x1B)
        put(KeyEvent.KEYCODE_SPACE, 0x20)
        put(KeyEvent.KEYCODE_SHIFT_LEFT, 0xA0)
        put(KeyEvent.KEYCODE_SHIFT_RIGHT, 0xA1)
        put(KeyEvent.KEYCODE_CTRL_LEFT, 0xA2)
        put(KeyEvent.KEYCODE_CTRL_RIGHT, 0xA3)
        put(KeyEvent.KEYCODE_ALT_LEFT, 0xA4)
        put(KeyEvent.KEYCODE_ALT_RIGHT, 0xA5)
        put(KeyEvent.KEYCODE_CALL, VK_TTALK)
        put(KeyEvent.KEYCODE_ENDCALL, VK_TEND)
        put(KeyEvent.KEYCODE_F1, VK_TSOFT1)
        put(KeyEvent.KEYCODE_F2, VK_TSOFT2)
        put(KeyEvent.KEYCODE_F3, VK_TTALK)
        put(KeyEvent.KEYCODE_F4, VK_TEND)
        put(KeyEvent.KEYCODE_MOVE_HOME, 0x24)
        put(KeyEvent.KEYCODE_MOVE_END, 0x23)
        put(KeyEvent.KEYCODE_PAGE_UP, 0x21)
        put(KeyEvent.KEYCODE_PAGE_DOWN, 0x22)
        put(KeyEvent.KEYCODE_COMMA, 0xBC)
        put(KeyEvent.KEYCODE_PERIOD, 0xBE)
        put(KeyEvent.KEYCODE_MINUS, 0xBD)
        put(KeyEvent.KEYCODE_EQUALS, 0xBB)
        put(KeyEvent.KEYCODE_SEMICOLON, 0xBA)
        put(KeyEvent.KEYCODE_SLASH, 0xBF)
        put(KeyEvent.KEYCODE_APOSTROPHE, 0xDE)
        put(KeyEvent.KEYCODE_LEFT_BRACKET, 0xDB)
        put(KeyEvent.KEYCODE_RIGHT_BRACKET, 0xDD)
        put(KeyEvent.KEYCODE_BACKSLASH, 0xDC)
        for (i in 0..9) put(KeyEvent.KEYCODE_0 + i, '0'.code + i)
        for (i in 0..25) put(KeyEvent.KEYCODE_A + i, 'A'.code + i)
    }

    fun vk(keyCode: Int): Int? = map[keyCode]

    // Windows Mobile Standard (Smartphone) keys (winuserm.h): Home is the Windows key, Back is
    // Escape, Action is Enter, * and # are F8 / F9 (the board types those as Fn + 8 / 9).
    const val VK_THOME = 0x5B
    const val VK_TBACK = 0x1B
    const val VK_TSTAR = 0x77
    const val VK_TPOUND = 0x78

    /**
     * A phone key for Windows Mobile Standard: a flip phone's keypad goes straight to the WM keypad
     * (digits, * #, D-pad + centre, Call / End, soft keys, Back), the rest as on Professional.
     * KEYCODE_MENU is handled by the activity (a tap is the right soft key, a hold the app's settings).
     */
    fun standardVk(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_CLEAR -> VK_TBACK
        KeyEvent.KEYCODE_STAR, KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> VK_TSTAR
        KeyEvent.KEYCODE_POUND -> VK_TPOUND
        KeyEvent.KEYCODE_SOFT_LEFT -> VK_TSOFT1
        KeyEvent.KEYCODE_SOFT_RIGHT -> VK_TSOFT2
        KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_MOVE_HOME -> VK_THOME
        KeyEvent.KEYCODE_NUMPAD_ENTER -> VK_RETURN
        in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 -> '0'.code + (keyCode - KeyEvent.KEYCODE_NUMPAD_0)
        else -> map[keyCode]
    }

    /**
     * Buttons 1-4 as keys a stock image already understands, so they work with no guest helper:
     *  - the action's own key: Start = Win, the soft keys = F1/F2, scrolling = the arrows;
     *  - else Win + VK_APPm of a hotkey whose assignment in WM is the action: WM 5 / 6.x ship with
     *    Button 2 Rotate, 3 Context Menu, 4 OK/Close (Shell\Keys\40C2..40C4), so any button can do
     *    those three; "Windows Mobile's own setting" on Buttons 2-4 is just their own hotkey;
     *  - else WM itself must store the action (Home, Input Panel, an action no hotkey holds any more,
     *    WM's own program on Button 1): [needsStore], which the helper writes once (Buttons.cs).
     * Win+VK_APP1 never reaches WM (the keyboard driver's Fn key), so WM's own Button 1 (40C1, stock:
     * Start Menu) is pressed through its action, or through 40C5 once the helper moved it there.
     * [map] is the helper's last "buttons" report: hotkey slot 1..5 (Win+VK_APP1..5) -> action,
     * "prog" or "-" (absent).
     */
    object HwButtons {
        const val STOCK = "1=start 2=rotate 3=contextmenu 4=okclose 5=-"
        private const val VK_LWIN = 0x5B
        private val directKeys = mapOf(
            "start" to VK_LWIN, "leftsoft" to VK_TSOFT1, "rightsoft" to VK_TSOFT2,
            "scrollup" to VK_UP, "scrolldown" to VK_DOWN, "scrollleft" to 0x25, "scrollright" to 0x27)

        fun parse(map: String?): Map<Int, String> {
            val m = mutableMapOf<Int, String>()
            for (part in ((map?.takeIf { it.isNotBlank() }) ?: STOCK).trim().split(' ')) {
                val kv = part.split('=', limit = 2)
                val slot = kv.getOrNull(0)?.toIntOrNull() ?: continue
                if (kv.size == 2 && slot in 1..5) m[slot] = kv[1]
            }
            return m
        }

        /** Button 1..4 for one of the skin's VK_BUTTONn values, or 0. */
        fun button(vk: Int) = when (vk) { VK_BUTTON1 -> 1; VK_BUTTON2 -> 2; VK_BUTTON3 -> 3; VK_BUTTON4 -> 4; else -> 0 }

        // The hotkey slot WM keeps Button n in: 40Cn, Button 1 in 40C5 once moved (40C1 gone).
        private fun ownSlot(n: Int, m: Map<Int, String>) = if (n == 1 && m[1] == "-" && m[5] != "-") 5 else n

        /** What Button [n] does under the Device Settings [choice] ("wm" = WM's own setting). */
        private fun action(n: Int, choice: String, m: Map<Int, String>) =
            if (choice == GuestSettings.BUTTON_WM) m[ownSlot(n, m)] ?: "-" else choice

        /**
         * The virtual key to send for Button [n]: Win + VK_APPm is sent as VK_APPm (Machine::key adds
         * Win), null = nothing ("None").
         */
        fun key(n: Int, choice: String, map: String?): Int? {
            val m = parse(map)
            val slot = ownSlot(n, m)
            if (choice == GuestSettings.BUTTON_WM && slot != 1) return 0xC0 + slot  // WM's own hotkey, whatever it holds
            val a = action(n, choice, m)
            if (a == "none") return null
            directKeys[a]?.let { return it }
            // A hotkey that holds the action (prog: only the button's own); never VK_APP1 (Fn).
            val slots = listOf(slot) + listOf(2, 3, 4, 5).filter { it != slot }
            slots.firstOrNull { it != 1 && m[it] == a && a != "prog" && a != "-" }?.let { return 0xC0 + it }
            return 0xC0 + if (slot == 1) 5 else slot  // stored by the helper (needsStore)
        }

        /** Button [n]'s choice needs the helper to store it in WM (no stock key reaches it). */
        fun needsStore(n: Int, choice: String, map: String?): Boolean {
            val m = parse(map)
            val k = key(n, choice, map) ?: return false
            if (k == VK_LWIN || k == VK_TSOFT1 || k == VK_TSOFT2 || k == VK_UP || k == VK_DOWN || k == 0x25 || k == 0x27) return false
            val slot = k - 0xC0
            val a = action(n, choice, m)
            return if (choice == GuestSettings.BUTTON_WM) m[slot] == "-" else m[slot] != a
        }
    }
}
