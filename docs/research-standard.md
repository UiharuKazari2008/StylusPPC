# Windows Mobile Standard (Smartphone) support

Issue [#2](https://github.com/UiharuKazari2008/StylusPPC/issues/2): run the keypad-only Standard
editions, mainly for Android flip phones with a hardware keypad.

Everything below was checked in the headless runner on the phone (`wmcore-cli`, private copy in
`/data/local/tmp/wm/std/`, images there as `sp653qvga.bin` etc., walk-through script `verify.sh`).
The app side (profiles, keypad skin, key mapping) compiles into the APK but was **not** run on a
device yet.

## 1. What works

| | 6.5.3 Standard | 6.1.4 Standard | 5.0 Smartphone |
|---|---|---|---|
| Cold boot to Home | ✅ ~20 s (no card), ~50 s with the helper | ✅ | ✅ ~13 s |
| Keys (soft keys, D-pad, Action, Home, Back, digits, * #, Talk, End) | ✅ | ✅ | ✅ |
| Start menu, soft-key menus, opening programs | ✅ | ✅ | ✅ |
| Dialing (digits on Home open the dialer, Talk calls) | ✅ FakeRIL call screen | ✅ through the virtual modem (`ATD5551234;`, call screen, Last Call) | ✅ FakeRIL call screen |
| Guest helper (CF 3.5, mailbox, theme, status, patches) | ✅ (CF 3.5 in ROM) | ✅ (bootstrap installs CF 3.5) | ✅ (bootstrap installs CF 3.5) |
| PocketStylus settings app in WM | ✅ keypad version | ✅ | ✅ |
| Lock (`lock` command) | ✅ "Unlock" soft key | ✅ | ✅ |

Screens (left to right: Home, Start (left soft key), D-pad moved, right soft key menu, Action opens
the program, Home, digits 5551234, Talk, End, settings app, Done, lock):
`docs/images/std-653-walkthrough.png`, `std-614-walkthrough.png`, `std-5-walkthrough.png`;
the keypad settings app (Down, Right changes a value, Menu lists the pages, Done):
`docs/images/std-614-settings.png`; a 6.1.4 call through the virtual modem: `std-614-call.png`.

## 2. Keys

The Standard images use the same SMDK2410 keyboard driver (`kbdmouse.dll`, matrix scancodes,
Fn = VK 0xC1 = code 0x64) as the Professional ones, so **no core change was needed**: the existing
`Machine::key` VK -> matrix table already covers every Smartphone key (`winuserm.h` VK_T* names).
F-keys go out as Fn + digit, which is how * (F8) and # (F9) arrive.

| WM Standard key | VK | Board code(s) | Skin action | Android key (flip phone) |
|---|---|---|---|---|
| Left soft key (VK_TSOFT1) | 0x70 F1 | Fn+'1' (0x64,0x31) | `soft1` | KEYCODE_SOFT_LEFT, F1 |
| Right soft key (VK_TSOFT2) | 0x71 F2 | Fn+'2' | `soft2` | KEYCODE_SOFT_RIGHT, F2, MENU tap |
| Talk / Send (VK_TTALK) | 0x72 F3 | Fn+'3' | `talk` | KEYCODE_CALL, F3 |
| End (VK_TEND) | 0x73 F4 | Fn+'4' | `end` | KEYCODE_ENDCALL, F4 (Android usually keeps ENDCALL) |
| Home (VK_THOME) | 0x5B LWin | 0x60 | `home` | KEYCODE_MOVE_HOME (KEYCODE_HOME is Android's) |
| Back (VK_TBACK) | 0x1B Esc | 0x29 | `back` | KEYCODE_BACK, KEYCODE_CLEAR |
| Action (VK_TACTION) | 0x0D Enter | 0x5A | D-pad centre / `action` | DPAD_CENTER, ENTER, NUMPAD_ENTER |
| Up / Down / Left / Right | 0x26 / 0x28 / 0x25 / 0x27 | 0x6C / 0x6A / 0x6D / 0x6F | D-pad | DPAD_* |
| 0-9 (VK_TNUM*) | '0'-'9' | 0x49, 0x31, 0x39, ... | `0`-`9` | KEYCODE_0-9, NUMPAD_0-9 |
| * (VK_TSTAR) | 0x77 F8 | Fn+'8' | `star` | KEYCODE_STAR, NUMPAD_MULTIPLY |
| # (VK_TPOUND) | 0x78 F9 | Fn+'9' | `pound` | KEYCODE_POUND |
| Volume up / down | 0x75 / 0x76 F6 / F7 | Fn+'6' / Fn+'7' | `volup` / `voldown` | per the volume key settings |

Android's KEYCODE_MENU on a Standard profile: a tap is the right soft key (WM's "Menu"), a hold opens
PocketStylus's settings (phones without a touch screen have no other way to reach them).
Letters (KEYCODE_A-Z) still go through as on Professional, so a QWERTY phone types directly.

Touch on the WM screen for Standard (`EmulatorActivity.keypadTouch`): a tap on the soft key bar
(bottom 9 %) presses that soft key, a swipe is the D-pad (one press per tenth of the screen; content
follows the finger), any other tap is Action.

## 3. Screen sizes

Each Standard image has home-screen layouts only for its own size family, so the Professional trick
(any phone shape from one image) does not carry over. Tested:

| Image | Size | Result |
|---|---|---|
| 6.5.3 `SP_USA_GSM_QVGA_VR.bin` (96 dpi) | 240x320, 320x240, 320x320 | ✅ (same as the LS / SQUARE images) |
| | 240x400, 240x426 | ❌ "The layout you have selected cannot be loaded" on Home |
| 6.5.3 `SP_USA_GSM_WVGA_VR.bin` (192 dpi) | 480x800, 480x640 | ✅ Home / Start; ❌ in-call screen draws garbled (also at 480x800) |
| | 480x854 | Home leaves a grey band above the soft key bar |
| 6.1.4 `SP_USA_GSM_QVGA_VR.bin` | 240x320, 320x240, 320x320 | ✅ |
| | 240x400 | ❌ Home layout error |
| 5.0 `SP_USA_GSM_QVGA_VR.bin` | 240x320, 320x240, 240x400 | ✅ |

`OsProfile.screens` lists the offered sizes per release and the image each needs; Device Settings ->
Screen resolution shows them (plus "Phone shape (auto)"). Auto picks, from the QVGA image, the size
whose shape is nearest the skin's display area (`OsProfiles.standardScreen`); with the keypad skin
that is 240x320. DPI stays the image's own (the DPI setting is hidden and sent as 0).

## 4. The helper on Standard: two-tier security

The Standard images are **two-tier**: `HKLM\Security\Policies\Policies\0000101b` (4123, Privileged
Applications) = 0, so the unsigned CF helper ran with normal trust and every protected write failed
(`UnauthorizedAccessException`: status keys, patches, IrDA, shortcuts in `\Windows`, calls). The
Professional images are one-tier (= 1, docs/research-status-indicators.md §5.1).

Fix (core, `Machine::oneTierPolicy`): for the first 3 minutes after every (re)boot the board scans
guest RAM once a second for the hive's value cell (`04 00 04 00 08 xx` + UTF-16 `0000101b` + DWORD)
and sets 0 -> 1 in place. The cell appears about 15 s into boot (6.5.3, 6.1.4) or after the first
helper reset (5.0); the helper starts after the home screen, so it is privileged from its first run.
The patched hive is what WM flushes, so the value persists through soft resets and in saved states.
Professional images have the value at 1 already, so nothing changes there. (An SDK-signed helper
would be the "proper" alternative, but needs the WM SDK's privileged test certificate.)

Guest program changes (`guest/Helper`, edition test `Edition.Smartphone` in SettingsApp.cs):

- **Settings app**: the Compact Framework on Smartphone has no TabControl, Button or TrackBar
  (`NotSupportedException`). The keypad version shows one page at a time; every setting is a
  ComboBox spinner (Up/Down between settings, Left/Right change the value), sliders become ~12
  steps, the right soft key's Menu lists the pages and the page's actions, Done applies the restart
  settings.
- **Storage Card page** (buttons, list box): on Standard the request goes to the phone's page.
- **IrDA transport question**: soft keys (Bluetooth LE / Menu: InfraWAN, Remember, Cancel) instead
  of buttons.
- **FakeRIL** is never unloaded on Standard (the "calls off" patch step): without a radio Standard
  opens a "SIM Failure" message at every start.

## 5. App changes

- `OsProfiles.kt`: `wm653std` (QVGA + WVGA images), `wm614std`, `wm5sp` (QVGA), `touch = false`,
  `screens`. Images are imported like the others (Select ROM folder... on each release's
  `Emulation Images`), or `python tools/push-images.py wm653std`.
- Keypad skin: `SkinSpec` "standard" layout (also accepted as "keypad"); the built-in skin
  (`assets/skins/default.json`) has one: soft keys with Home and Back between them, Talk / D-pad /
  End, and a 3x4 number pad with letters. A user skin without one gets the built-in keypad.
  `SkinLayout.keypad` picks it while keys are on; number keys draw their letters beside the digit.
  `tools/skin-designer.html` has a "Standard keypad" tab and the new actions (home, action, star,
  pound, 0-9).
- Device Settings / WM settings app (`GuestSettings.entriesFor`): Standard hides rotation, the fold
  slider and hinge, the Pro Buttons 1-4 and DPI; Hardware Keys becomes Keypad / None (None = the
  phone's own keypad, display fitted alone).
- `EmulatorActivity`: no auto-rotate and no X1 slider on Standard (the open foldable keeps the cover
  layout), key mapping per §2, touch per §2, no Suspend in the power menu for 5.0 Smartphone (as for
  5.0 Pocket PC).

## 6. Not done / open

- Not run in the app on a phone yet: the keypad skin's look, the flip-phone key mapping (MENU
  long-press, SOFT_LEFT/RIGHT) and auto resolution need a check on a device.
- 6.1.4 Standard calls go through the virtual modem like 6.1.4 Pro (helper "radio" step, then a
  restart): in the CLI they need `--modem-net` (the app's network feed) and `--modem-stub`, else
  "Phone Error". Incoming calls / SMS on Standard were not tested.
- WVGA image: in-call screen garbled (any size). 480x854 not offered (grey band).
- Landscape on Standard: offered only as a fixed 320x240 boot size; live rotation is off.
- Standard's own Settings has no PocketStylus entry (Pro's ControlPanel key is a Pocket PC thing):
  the settings app is in Start > All Programs > PocketStylus; on the phone, hold Menu or use the lock
  button's power menu.
- 6.5 / 6.1 / 6.0 Standard and the 176x220 images: not offered.

## 7. Test commands

```sh
# on the phone, /data/local/tmp/wm/std: card.sh makes a mailbox + card programs
sh verify.sh v653 sp653qvga.bin 240x320     # full walk-through, shots in v653/
./wmcore-cli sp653qvga.bin --mem 128 --screen 240x320 --seconds 90 \
  --key 35:0x70 --shot 38:start.ppm --key 39:0x28 --key 42:0x0D --key 47:0x1B --key 50:0x5B \
  --key 55:0x35 --key 59:0x77 --key 59.5:0x78 --shot 61:dial.ppm
```
