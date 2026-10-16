# Notification LED and vibrator on WM 6.5.3 (DE IONotificationLEDs) and the host API

How the WM 6.5.3 Pro FWVGA image drives the notification light and the vibrator, what WMDroid emulates, and
what the Android side gets. Companion to `research-de-hardware.md` §4.4 and `research-status-indicators.md`.

Confidence tags, as in the other notes: **[OBS]** seen in WMDroid (CLI log), **[DISASM]** ARM disassembly of a
module from the IMGFS, **[ROM]** registry/strings from the image, **[INFERRED]**, **[GUESS]**.

---

## TL;DR

1. **The hardware exists and the image uses it.** `HKLM\Drivers\BuiltIn\NLed` loads `nleddrvr.dll`
   (PDB name `de_nleddrvr.pdb`, the Device Emulator PDD). It maps **6 bytes at PA 0x500FFFD0** and declares
   **2 LEDs**: LED 0 = notification light, LED 1 = vibrator. WMDroid already had the byte array (`DePage::leds`,
   serialized since the first snapshot format), so option (a) of the plan applies: no guest driver is needed.
   [ROM][DISASM][OBS]
2. Register layout, 3 bytes per LED: `+0 OffOnBlink` (0 off, 1 on, 2 blink), `+1 OnTime/100`, `+2 OffTime/100`,
   times in microseconds divided by 100 and **clamped to 255** (so anything above 25.5 ms reads 255). The driver
   writes on time, off time, then the mode byte, with byte stores, and never reads them. [DISASM][OBS]
3. **DE driver bug: the notification light never came on.** The shell's LED request (in `ossvcs.dll`) is
   blink, 62.5 ms on / 1937.5 ms off, `MetaCycleOn = MetaCycleOff = 1`. The DE PDD rejects any
   `MetaCycleOff != 0` with ERROR_INVALID_PARAMETER (87) before touching the hardware. The core now patches that
   one branch in guest RAM (`core/src/board/codepatch.cpp`, "nleddrvr"), after which reminders light the LED.
   [DISASM][OBS]
4. Vibration needs no fix: the shell switches LED 1 on/off (pulses of about 250-500 ms), and every pulse reaches
   the device. [OBS]
5. Host API: `NativeBridge.ledState(handle): Int` (bit0 light, bit1 blink, bit2 vibrate, bits 8..15 on time in
   50 ms units or 0, bits 16..23 vibrator start count). It comes from the core, not from the helper mailbox, so
   there is no mailbox message. §5.

---

## 1. Image configuration [ROM]

`default.hv` (FWVGA 6.5.3):

```
[HKLM\Drivers\BuiltIn\NLed]  Prefix=NLD  Dll=nleddrvr.dll  Flags=8 (DEVFLAGS_NAKEDENTRIES)  Order=0
                             IClass={CBB4F234-F35F-485b-A490-ADC7804A4EF3}
[HKLM\System\Events]         "SYSTEM/NLedAPIsReady"="Notification LED APIs"
```

There is no `TouchVibration` key, so touch feedback vibration (`waveapi.dll`, `gwes.exe`, `HKCU\ControlPanel\TouchVibration`
`TouchVibrateEnabled`) keeps its built-in default. It never fired in the tests. [ROM][OBS]

## 2. The driver (`nleddrvr.dll`, `.text` at 0x7B4A5000, data at 0x01DAF000) [DISASM]

Exports Init/Deinit/Open/Close/IOControl/PowerUp/PowerDown (naked entries). Imports CEDDK ordinals 6/8/9
(HalTranslateBusAddress, MmMapIoSpace, MmUnmapIoSpace) and coredll.

- **Init** (0x7B4A5698): opens `SYSTEM/NLedAPIsReady`, calls the PDD init (0x7B4A50A8), which translates PA
  0x500FFFD0 (literal at 0x7B4A5174) and maps **6 bytes** (pointer at 0x01DAF044), then sets the event.
  It does **not** clear the registers.
- **IOControl** (0x7B4A577C): `0x01050400` = get info (count / supports / settings), `0x01050404` = set
  (input must be exactly 28 bytes, an `NLED_SETTINGS_INFO`). Anything else goes to an optional handler or fails with 50.
- **Get info** (0x7B4A5434): count = **2**. Supports table at 0x7B4A501C:
  LED 0 `{lCycleAdjust 100, fAdjustTotalCycleTime 0, fAdjustOnTime 1, fAdjustOffTime 1, fMetaCycleOn 0, fMetaCycleOff 0}`;
  LED 1 `{lCycleAdjust -1}`, which is how NLED marks a vibrator. Settings come from a copy kept in RAM (0x01DAF05C).
  Confirmed from the guest with `NLedGetDeviceInfo`. [OBS]
- **Set** (0x7B4A5204), `NLED_SETTINGS_INFO {LedNum, OffOnBlink, TotalCycleTime, OnTime, OffTime, MetaCycleOn, MetaCycleOff}`:
  - `on = OnTime / 100`, `off = OffTime / 100` (`__rt_sdiv`).
  - Rejects (error 87): `LedNum >= 2`, `on < 0`, `off < 0`, `OffOnBlink` not 0..2, `MetaCycleOn != 0 && on == 0`,
    and **`MetaCycleOff != 0`** (the `beq` at **0x7B4A52CC** skips the error only when MetaCycleOff is 0).
  - Clamps `on` and `off` to 255. For LED 1 a blink (2) becomes on (1).
  - Writes `base[3n+1] = on`, `base[3n+2] = off`, then `base[3n+0] = OffOnBlink`, and stores the settings with
    TotalCycleTime = OnTime + OffTime.
- **PowerUp/PowerDown** (0x7B4A5974/0x7B4A5960) do nothing to the hardware.

So the light and vibrator are plain levels set by the guest. The host times the blinking itself.

## 3. Who calls NLedSetDevice, and how the settings reach the driver [DISASM][OBS]

`NLedGetDeviceInfo`/`NLedSetDevice` are coredll ordinals 839/840. Importers in IMGFS: `ossvcs.dll`, `shell32.exe`,
`waveapi.dll`, `gwes.exe`, plus `aygshell.dll`/`notify.dll` (info only).

- **`ossvcs.dll`** (the annunciator hosted in shell32) owns both outputs. At start it finds the vibrator
  (lCycleAdjust = -1) and the first other LED (0x0274BD18).
  - LED on (0x0274BF70): increments `LedRefCount` (a registry DWORD) and sets the LED to
    **blink, TotalCycleTime 2 000 000 µs, OnTime 62 500, OffTime 1 937 500, MetaCycleOn 1, MetaCycleOff 1**.
    LED off (0x0274BF08): decrements and switches off at 0.
  - Vibrate on/off (0x0274BE00 / 0x0274BDAC): OffOnBlink 1/0 on the vibrator. The annunciation code
    (0x0273B984, 0x0273C0F8) times it in 100 ms steps or 2000 ms, after checking two shell state values (not decoded).
- **`shell32.exe`** (0x00023FFC) only switches the vibrator off. `waveapi.dll` (0x029D24..) and `gwes.exe`
  are touch feedback (vibrate, `Sleep(n)`, off).
- **Sounds & Notifications**: `notify.dll`'s page edits `HKCU\ControlPanel\Notifications\<class>` `Options` and
  `Duration`. When an event fires, the shell reads them and drives ossvcs:
  - `Options & 0x4` = **Flash light**. Verified: an `SHNotificationAdd` with a test class whose Options = 4 lit the LED. [OBS]
  - `Duration` = **flash light for N minutes**, independent of the bubble. Verified: Duration = 1 turned the LED
    on at t=39 s and off at t=99 s, although the notification had been removed after 5 s. [OBS]
  - Other bits from the image defaults (Default 0xD, text message 9, missed call 4, Reminders 0x4000000D):
    0x1 play sound, 0x2 repeat, 0x8 display message [INFERRED]. The vibrate bit was not identified: 0x8 and 0x10 did
    not vibrate in either profile. [OBS, negative]
- **`CeSetUserNotificationEx`** with `PUN_LED` (reminder dialogs) uses the same ossvcs LED path. Clearing the
  notification turns the LED off. [OBS]

On the stock DE driver every ossvcs LED request fails, so the "flash light" setting has never worked on DE.
With the patch, the LED shows `regs 2/255/255` (blink, both times clamped).

## 4. The fix: one branch in the guest driver

Applied by the core (`core/src/board/codepatch.cpp`, tables `kNled653` / `kNled614`), not by the guest: the code is
found by a byte signature (the literal 0x500FFFD0 plus the code around the branch) in any physical page, just before
code there first runs and again after the page is rewritten (paged back in). Until 2026-10-16 the helper did this
from inside WM (`CodePatch.cs`, kernel mode, fixed VAs, re-checked every 10 s, only while the helper was alive).
The core logs `codepatch: nleddrvr 6.5.3 (accept MetaCycleOff blinks) applied at pa ...`.

WM 6.5.3 (`.text` at 0x7B4A5000):

| VA | Original | Patched | |
|---|---|---|---|
| 0x7B4A5174 | 0x500FFFD0 | same | guard: the driver's literal device address, never written |
| 0x7B4A52CC | 0x0A000004 `beq ok` | 0xEA000004 `b ok` | accept MetaCycleOff != 0 |

WM 6.1.4 has the same rejection in an optimised build of the driver: `ldr r3, [r7, #0x18]; cmp r3, #0;
bne fail` (page offset 0x218, 0x1A000023 -> 0xE1A00000 nop), same literal at page offset 0x134. WM 5's image has no
DE NLED driver that uses 0x500FFFD0.

With MetaCycleOff accepted, the driver handles the request like any blink and the meta cycle is ignored, which
is what the host does anyway. The fix no longer needs the helper (or a storage card).

Alternatives not taken: a replacement NLED driver (needs native code) or an HLE trap in the core (needs guest
VA→PA lookups in device.exe's context). Neither is simpler than this patch.

## 5. Host API

Core: `wm::NotifyLeds` (`core/src/board/nled.{h,cpp}`), member `DePage::nled`, fed by `DePage::write` on every
write to the LED bytes. It is reset (all off) on the board's soft reset and on the OAL's MMU-off reboot, because
the guest driver does not clear the bytes at Init. **No new snapshot state:** the register bytes were already in
`DEPG`, and the latch and counter are transient.

JNI: `NativeBridge.ledState(handle: Long): Int`, constants `LED_LIGHT = 1`, `LED_BLINK = 2`, `LED_VIBRATE = 4`.

| Bits | Meaning |
|---|---|
| 0 | Notification light on (steady or blinking) |
| 1 | Light is blinking |
| 2 | Vibrator on now, **or switched on since the previous `ledState` call** (latched, cleared by the call) |
| 8..15 | Blink on time in 50 ms units when the register holds a real value (< 255; anything shorter than 50 ms reads 1). **0 = unknown**, which is the case for the shell's pattern (clamped to 255) |
| 16..23 | Number of vibrator off→on transitions, mod 256 |

Notes for the host side:
- Poll from **one place** (for example every frame or every 100 ms), because each call consumes the vibrate latch.
  A pulse shorter than the poll interval still shows as one poll with bit 2 set. A changed bits 16..23 value also
  shows a pulse that was missed.
- When blinking with an unknown on time, draw WM's own pattern: **a short flash (about 60 ms) every 2 s**
  (ossvcs' 62.5 ms / 1937.5 ms). A steady light (bit 0 without bit 1) only comes from direct `NLedSetDevice` callers.
- Vibration patterns are pulses of about 250-500 ms. Switching the Android vibrator on while bit 2 is set and off
  when it clears follows them closely enough.
- No mailbox message is involved, and the guest helper has no part in it (the core patches the driver).

## 6. Verification (CLI on the phone, `test/th.wms`, 192 MB, 480x854)

Runner: `/data/local/tmp/wm/led/run1.sh` drops helper mailbox commands at set times. The probe is
`\Storage Card\LedTest.exe` (session scratchpad `nled/LedTest`, not shipped; it calls NLedGetDeviceInfo/NLedSetDevice,
CeSetUserNotificationEx and SHNotificationAdd).

| Test | CLI log | Result |
|---|---|---|
| `NLedGetDeviceInfo` | | count 2; LED 0 cycleAdjust 100; LED 1 cycleAdjust -1 |
| set LED 0 blink 10 000 / 20 000 µs | `regs 2/100/200`, `ledState 0x103` | blink, on time 1 unit |
| set LED 0 on, then off | `ledState 0x1`, then `0` | |
| ossvcs pattern (meta 1/1) on the stock driver | no write | `NLedSetDevice` → FALSE, error 87 |
| vibrator pulse 40 ms | `ledState 0x10004` then `0x10000` | latched by the 250 ms poll |
| reminder (`PUN_LED\|PUN_DIALOG`) on the stock driver | nothing until the clear, then a write that leaves it off | LED request rejected |
| same after the new helper's patch (`codepatch nled 2`) | `regs 2/255/255`, `ledState 0x3`; off when cleared | **works** |
| SHNotification class Options = 4, Duration = 1 | blink at t=39 s, off at t=99 s | flash light for 1 minute |
| ShellOverrides `Mode` = 1, then a reminder | 5 pulses about 250-500 ms apart, `ledState 0x10004` … `0x50000` | vibrator path. The exact trigger was not pinned down (it happened at the time the probe started in two runs, but not in a third run with different timing) |

Not tested: the Android UI side (the main session draws the dot and calls Vibrator), touch-feedback vibration
(disabled in this image), phone ring and missed-call LED (no incoming calls in WMDroid yet), and the other WM releases
(the addresses were checked on `PPC_USA_GSM_FWVGA_VR.BIN` only; on images where the words differ the patch does
nothing).
