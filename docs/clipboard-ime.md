# Clipboard sync and "Use Android IME over Device IME"

Two input features shared between the phone app and the guest helper. Code:
`app/.../ClipboardBridge.kt`, `app/.../ImeBridge.kt`, `app/.../GuestTyper.kt` (shared with `FoldKeyboard`),
`guest/Helper/InputSync.cs`. Settings: Hardware > "Sync Clipboard" (`sync.clipboard`, default on) and
Design > "Use Android IME over Device IME" (`ime.android`, default off), both in the `GuestSettings`
catalogue and live (`EmulatorActivity.applyLiveSettings`).

Tags: **[OBS]** tried in the CLI (`/data/local/tmp/wm/clip`, `--load-state test/th.wms --mem 192 --screen
480x854`, Word Mobile with a text file from the card); **[INFERRED]** not verified.

## Mailbox protocol

| Direction | Message | Meaning |
|---|---|---|
| app -> guest | `clip watch 1` / `clip watch 0` | Report WM clipboard changes (sent on resume / pause / setting change). Turning it on takes the current WM clipboard as the baseline, so old WM text never overwrites the phone's clip. |
| app -> guest | `clip` | Set WM's clipboard from `clip_in.txt` (UTF-8, `\n` becomes `\r\n`). |
| app -> guest | `clip get` | Diagnostics: the WM clipboard text (escaped, first 200 chars). |
| guest -> app | `clip` | WM's clipboard changed; the text is in `clip_out.txt` (UTF-8, `\r\n` becomes `\n`). |
| app -> guest | `ime watch 1` / `ime watch 0` | Watch WM's SIP (only while the setting is on and the app is on screen). Off restores WM's own keyboard if it was up. |
| guest -> app | `ime open` | WM raised its SIP: show the phone's keyboard. |
| guest -> app | `ime hide` | WM lowered its SIP (focus left the text box, SIP button tapped again): hide the phone's keyboard. |
| app -> guest | `ime close` | The phone's keyboard was dismissed: lower WM's SIP (and keep it down for 0.8 s). |
| app -> guest | `type/<seq>.txt` (file, not a command) | Typed input while the phone's keyboard is open; read by the helper every 50 ms. Tokens are hex, space separated: `xxxx` = a UTF-16 character (WM_CHAR), `1xxxx` = press virtual key `xxxx` (Back, Enter, Tab, arrows, Delete, Home/End). |
| app -> guest | `type <tokens>` | Same, through the normal mailbox (diagnostics). |
| app -> guest | `ime diag` / `ime show` | Diagnostics: SIP state and window; raise the SIP as a text box would. |

## Clipboard

- WM side: one hidden `MessageWindow` with its own message loop owns the clipboard (a window that does
  not pump would hang any program emptying the clipboard, since WM_DESTROYCLIPBOARD is sent to the
  owner). `OpenClipboard(hwnd)` / `EmptyClipboard` / `SetClipboardData(CF_UNICODETEXT, LocalAlloc(LMEM_FIXED))`;
  reading uses `IsClipboardFormatAvailable` first so an empty or non-text clipboard is never opened. WM
  6.5 has no `GetClipboardSequenceNumber` (not in the ROM's coredll), so the helper hashes (FNV-1a) the
  text once a second. **[OBS]**
- Ping-pong: the helper stores the hash of what it set; the app stores the last text synced either way.
  **[OBS]** setting from the app, then `clip watch 0/1`, produced no `clip` back.
- Android rules: the clip is read only with window focus (`onWindowFocusChanged`) and only when
  `ClipDescription.timestamp` changed, so the system's "pasted from your clipboard" notice appears once per
  new clip, not on every return. Writing (`setPrimaryClip`) needs no focus; on Android 13+ the system shows
  its own "copied" preview when WM copies something. WMDroid adds no UI of its own. Only text clips
  (`text/*`) are synced, up to 256 K characters.
- **[OBS]** Phone -> WM: text with `✓` and `日本` set and read back exactly; pasted into Word Mobile with
  Ctrl+V (WM's fonts lack those glyphs and draw boxes). WM -> phone: Ctrl+A, Ctrl+C in Word wrote
  `clip_out.txt` with the document text and sent `clip` within a second.
- Limits: text only; a copy made in WM while the app is in the background is not reported (the watch is
  off; nobody can use WM then anyway).

## Phone keyboard instead of WM's SIP

### Why not a real input method

A WM input method is an in-process COM server (`IInputMethod`: Select/Deselect/Showing/Hiding/GetInfo/
ReceiveSipInfo/RegisterCallback/GetImData/SetImData/UserOptionsDlg plus `IClassFactory`,
`DllGetClassObject`, registration under HKCR\CLSID with `IsSIPInputMethod`, and becoming the current IM).
The camera route (`wmdcam.s` + `tools/make-wmdcam.py`, NDK clang + a hand-made PE with relocations) could
build it, but it means a COM vtable, a window class and window procedure for the SIP panel in ARM
assembly, and switching the user's selected input method. Its real gains (programs see a SIP of the right
height; `IIMCallback::SendString` for Unicode) are reached more simply below, so it was not written.

### What it does

1. The helper polls `SipGetInfo` (SIPF_ON) every 150 ms while `ime watch 1`, and also reacts at once to
   `WM_SETTINGCHANGE(SPI_SETSIPINFO)`.
2. When the SIP comes up it **hides the SIP's window** (`FindWindow("SipWndClass")`, `ShowWindow(SW_HIDE)`)
   but leaves the SIP on, and sends `ime open`. Programs therefore keep their content above the SIP area,
   where the phone's keyboard now sits. **[OBS]** Word Mobile's toolbar moved up and the SIP area stayed
   blank. If no `SipWndClass` window exists it falls back to `SipShowIM(SIPF_OFF)`.
3. The app focuses a hidden 1x1 editor view and shows the phone's keyboard. A `FileObserver` on the
   mailbox's `out` folder (only while the setting is on) makes the app react at once instead of at its
   once-a-second poll.
4. Typed text goes as `type/<seq>.txt` and the helper posts it with `PostKeybdMessage(-1, ...)`, the way a SIP
   input method types. Composing text (suggestions, autocorrect) is typed as it changes; a changed tail is
   backspaced and retyped. Any Unicode character works (e.g. `é`, `あ`, if the WM font has it). **[OBS]**
   `Hi⏎A⌫B←C` and `éあ x` came out as expected in Word.
   Not the emulated keyboard: **[OBS]** WM lowers its SIP on every hardware key press (Word, both Ctrl+V
   and a plain `C`), which would close the phone's keyboard after the first key. `PostKeybdMessage` input
   leaves it up.
5. Tapping WM's keyboard button raises the SIP, so it opens the phone's keyboard; tapping it again lowers
   the SIP and the helper sends `ime hide`. **[OBS]**
6. The phone's keyboard being dismissed (Back, the IME's own hide key) is seen through the window insets
   (`WindowInsetsCompat.Type.ime()` visibility, with a 300 ms recheck while open) and sends `ime close`.
   **[OBS]** for the guest side (`ime close` lowered the SIP and Word's layout returned).
7. App leaves the screen: `ime watch 0` (WM's own keyboard reappears if it was up); on return
   `ime watch 1` and, if the SIP is still up, `ime open` reopens the phone's keyboard.
8. Fold mode with the slide-out keyboard set to the phone's keyboard: that keyboard owns the IME; `ime open`
   only makes sure it is shown.

### Limits

- WM's own SIP may flash for up to ~150 ms before it is hidden (less when WM_SETTINGCHANGE arrives).
- The SIP area WM reserves is its own keyboard's height, not the phone keyboard's. With the phone's
  keyboard taller than that area (likely on the cover screen, where it also covers the skin's keys), the
  bottom lines of a text box can be under it. **[INFERRED]**
- WM does not tell when a text box loses focus unless the program lowers the SIP; then `ime hide` closes the
  phone's keyboard, otherwise it stays until dismissed.
- A key from a physical keyboard (or the skin's buttons) goes through the emulated keyboard: WM lowers
  the SIP and the phone's keyboard closes. Intended for a physical keyboard; for the skin's soft keys it
  may be surprising.
- Typing goes through files in the mailbox: about 50-100 ms latency per burst.
- Characters WM's fonts lack show as boxes; emoji become two UTF-16 units.
- Untested in the app itself (needs the APK on the phone): insets-based dismiss detection with
  `SOFT_INPUT_ADJUST_NOTHING`, Samsung Keyboard's composing behaviour, window-focus timing on resume.
