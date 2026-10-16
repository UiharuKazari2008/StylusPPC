# PocketStylus

**Native Pocket PC and Windows Mobile hypervisor for Android.**

PocketStylus boots the original, unmodified Windows Mobile ROM images that Microsoft shipped for its Device Emulator: Windows Mobile 5.0, 6.1.4 and 6.5.3. It runs them full-screen on a modern Android phone, and your phone becomes the Windows Mobile device. Its battery, signal, carrier, Wi-Fi, GPS, camera, speaker, vibration motor, clipboard, keyboard, wallpaper, colour theme and phone line all show up inside Windows Mobile as that device's own hardware.

It was built and tuned on a Galaxy Z Fold7. On a foldable, opening the phone turns it into an Xperia X1-style slider with a landscape screen above a hardware keyboard.

---

## Why this is an accomplishment

Microsoft's Device Emulator (DE) was a Windows-only x86 program, and it was never ported. Running its images anywhere else means rebuilding, from nothing, the whole machine those ROMs expect to wake up on:

- **A full ARM computer, written from scratch.** DE emulates a Samsung SMDK2410 development board: an ARM920T-class CPU, the S3C2410 system-on-chip (interrupt controller, timers, DMA, LCD, touch ADC, UARTs, GPIO, RTC, sleep logic), AMD CFI NOR flash, a PCMCIA bridge with an NE2000 network card, and a set of DE-only devices for battery, LEDs, audio and folder sharing. PocketStylus re-implements every one of them in portable C++ (about 11,000 lines in `core/` and the native app layer). Each register was rebuilt from documentation and from observing the guest. DE's own source code is under an academic, Windows-only licence, so not one line of it is used (see `docs/research-de-hardware.md`).
- **A CPU fast enough to be pleasant.** A naive ARMv5TE interpreter gave about 21 MIPS; Windows Mobile 6.5 needs far more to feel like a phone. The core has an ARM64 JIT with a software TLB, block chaining and inlined fast paths. It runs at 380–420 MIPS under load and peaks around 800. A lockstep `--compare` mode checks it against the interpreter instruction by instruction. Cold boot to the home screen takes about 30 seconds. Resuming a saved session takes under one second.
- **A phone battery is not a desktop PC.** Windows Mobile ticks its scheduler 1,000 times a second, and an emulator that faithfully wakes 1,000 times a second keeps a phone's CPU from ever sleeping. PocketStylus coalesces idle time and replays the owed timer ticks, cutting idle wake-ups from about 1,040 to about 135 per second. It presents only the screen rows that changed, sends performance hints to Android, and steps down when nothing happens.
- **An operating system that never knew Android existed.** To make Windows Mobile believe the phone's hardware is its own, PocketStylus runs a small agent inside the guest: a .NET Compact Framework 3.5 helper, plus a hand-built native ARM bootstrap that installs .NET CF 3.5 silently on releases that lack it. The agent and the app talk through a mailbox on an emulated storage card. To make everything feel native, the project had to reverse-engineer undocumented Windows Mobile behaviour:
  - how the title bar picks the G / E / 3G / H / 1X / Ev icon;
  - how Titanium caches wallpapers;
  - how the WM 5 colour engine recolours the shell;
  - why the lock screen smeared after 25 wallpaper changes;
  - how FakeRIL's dial thread can be held open until a real Android call connects.

  Some fixes needed in-memory patches of Microsoft's own ROM drivers, and one needed a hand-assembled ARM DLL that Windows Mobile loads as its camera driver.
- **Modern-phone niceties on a 2009 OS.** These include Material You colours applied as a native Windows Mobile theme, foldable layouts, phone-shaped screen resolutions the OS was never shipped with, live rotation, snapshot and resume, a gyroscope-driven glass skin, and outgoing calls placed from Windows Mobile's own dialer.

Every feature below was verified by booting real ROMs, both in the app and in a headless runner on the phone (`core/cli`).

---

## Supported Windows Mobile releases

| Release | Images | Notes |
|---|---|---|
| **Windows Mobile 6.5.3 Professional** | FWVGA 480×854 (recommended), WVGA, VGA, WQVGA, QVGA, square | Main target. Titanium home screen, FakeRIL radio, everything below works. |
| **Windows Mobile 6.1.4 Professional** | WVGA 480×800, VGA, WQVGA, QVGA, square | .NET CF 3.5 is installed automatically on first boot. Its radio is PocketStylus's virtual GSM modem. |
| **Windows Mobile 5.0 Pocket PC Phone** | VGA 480×640, QVGA (with and without phone) | Runs at your phone's full height. .NET CF 3.5 is installed automatically. Needs 128 MB of RAM (the default). |

Each release keeps its own saved sessions and its own device settings, so you can switch between them without losing anything.

**Professional or Standard?** "Professional" (Pocket PC) is the touchscreen edition. "Standard" (Smartphone) has no touchscreen and is driven entirely by keypad, so it isn't offered: PocketStylus is built around touch. Very minor releases (6.0, 6.1, 5.0 MSFP) were dropped in favour of the newest build in each family.

---

## Getting started

1. Install `PocketStylus-1.0.apk` from the release page (Android 10 or newer, ARM64).
2. **Get the ROM images.** They are Microsoft's and are not included in PocketStylus. Download the [Windows Mobile Emulation Images Archive](https://archive.org/details/windows-mobile-emulation-images-archive.-7z) from the Internet Archive and extract it with 7-Zip. The images PocketStylus uses are in each release's `Emulation Images` folder under `ARM-based/`:
   - `Windows Mobile 6.5.3 Professional/Emulation Images`
   - `Windows Mobile 6.1.4 Professional/Emulation Images`
   - `Windows Mobile 5.0 (Pocket PC)/Emulation Images`

   (They originally shipped with the *Windows Mobile 6.5.3 Developer Tool Kit*, the *Windows Mobile 6 SDK* image packages and the *Windows Mobile 5.0 SDK*.)
3. Open PocketStylus. In the OS list:
   - tap **Select ROM folder…** on a release and pick its `Emulation Images` folder, which imports every image that release knows; or
   - open the release and tap **Select ROM image…** next to one missing image.
4. Choose the image and tap **Save and restart device**. The first boot takes about 30–70 seconds. After that, PocketStylus resumes where you left off.

Developers can also push images over adb with `python tools/push-images.py <profile|all>`.

---

## Features

Each section below expands to show how the feature works.

<details>
<summary><b>The emulator core: a from-scratch SMDK2410 with an ARM64 JIT</b></summary>

- `core/src/arm`: ARMv5TE + Thumb CPU with banked modes, CP15, and an MMU with the FCSE process-ID remap. It uses a two-level software TLB with a generation counter, so a full flush is a single increment; WM 6.5 flushes about 600 times a second.
- `core/src/arm/jit.cpp`: an ARM64 dynamic recompiler. It translates guest blocks to host code, chains them, keeps guest registers in host registers, inlines register-shift operations, and takes the I/O check only on the slow path. WM 6.5 makes every system call by jumping to a faulting address (about 1,500 prefetch aborts a second), so abort handling avoids C++ exceptions entirely. Self-modifying code is detected per 4 KB physical page.
- `--compare` runs the interpreter and the JIT in lockstep on a deterministic clock and stops at the first difference.
- `core/src/board`: the S3C2410 peripherals, AMD CFI NOR flash, PCMCIA + NE2000, DE's battery / LED / audio / folder-share devices, and the camera device. The memory map is read from the ROM's own `OEMAddressTable`.
- B000FF `.bin` images are loaded into RAM and started at their entry point; `.nb0` images boot from flash.
</details>

<details>
<summary><b>Snapshots, quick resume, Savepoint and restarts</b></summary>

- The whole machine (RAM, flash, CPU, devices) serialises to a `.wms` file. `boot.wms` is saved when the first boot reaches the home screen, and `resume.wms` whenever you leave the app, so the next start resumes in under a second.
- New device state is added as optional trailers after the flash section, so sessions saved by older builds keep loading.
- **Power menu → Savepoint** writes a resume point without shutting down. **Reset** performs a real warm reboot: the host resets the CPU into the ROM's boot code with RAM intact, exactly as the OAL's `IOCTL_HAL_REBOOT` does. **Cold boot** discards the current release's saves only.
- A hung guest (detected when the battery driver's polling stops while the CPU is busy) is never saved over a good session.
</details>

<details>
<summary><b>Power and efficiency</b></summary>

- **Idle coalescing:** when the guest was idle for the last slice, the host sleeps to the end of the next 16 ms slice instead of waking for every 1 ms tick, then delivers the owed PWM timer ticks one by one. Guest time stays exact, and idle wake-ups drop from about 1,040 to about 135 per second.
- Audio DMA keeps exact timing. Coalescing continues while music plays if the guest is at least half idle.
- The OpenGL presenter uploads only the rows that changed since the last frame and asks for 60 Hz with `ANativeWindow_setFrameRate`.
- Android performance hints: a CPU boost on rotations and layout changes; nice 0 and no hints after a second of idling.
- Background modes: keep running (music keeps playing at full speed), suspend, or hibernate to a save after a timer.
</details>

<details>
<summary><b>Display: phone-shaped screens, rotation and screen-off</b></summary>

- Windows Mobile reads its screen size from the bootloader's arguments, which PocketStylus writes. Each release boots at your phone's own shape (for example 480×854) even when Microsoft only shipped 480×640 or 480×800 images.
- **Auto-rotate** follows the accelerometer, with delay and sensitivity sliders. The helper turns Windows Mobile with `ChangeDisplaySettingsEx`, and the wallpaper stays put.
- **Lock Android on screen off:** when Windows Mobile turns its display off, the phone locks too, using a device-admin lock. Opening the app or unlocking the phone wakes Windows Mobile again, with the power button if it was in standby.
- Upscaling filters: smooth, sharp pixels or enhanced.
</details>

<details>
<summary><b>Foldables: the Xperia X1 slider</b></summary>

- Built on `androidx.window`: when the phone opens flat, the layout switches to Windows Mobile in landscape across the top of the inner screen, with the keyboard below.
- The keyboard is either the skin's own X1-style keyboard (number row, Fn layer, Sym lock, Ctrl) or the phone's own keyboard with a row of Windows Mobile keys above it.
- **Show Mid-Display Hinge:** draws a drop shadow, a classic plastic hinge, a modern crease, or a black void at the fold, with thickness, location and opacity sliders.
- The cover screen decides the boot resolution, so the session stays the same whether the phone is opened or closed.
</details>

<details>
<summary><b>Skins: glass keys, gyro shine and key backlights</b></summary>

- The skin is drawn on Android with Vista/WM 6.5-era glass keys: Start (with the Windows flag), soft keys, OK, the App Launch keys (marked with one and two dots), a D-pad and a long lock key.
- **Gyro Key Shine:** virtual lights are fixed in the room. Each frame they are rotated into the phone's frame using the game rotation vector, and each key gets the mirror reflection of each light on its curved surface. The highlights move like real reflections when you tilt the phone.
- **Key Backlights:** the glyphs dim while unlit and glow when you touch, with inner and outer glow, a timeout, a colour (including your theme's), and **Dirtiness**, which adds the uneven, side-lit light of a rubber-membrane keypad.
- Skins are JSON files in four layouts: front, front with D-pad, inside (fold), and inside with the phone's keyboard. Design one in `tools/skin-designer.html` and import it under Settings → Skin.
</details>

<details>
<summary><b>Material You theme and wallpaper sync</b></summary>

- The app reads the phone's Material You seed colour, generates a saturated palette, and the helper applies it as a real Windows Mobile theme:
  - **WM 6.5:** the theme's colour table and generated Titanium primitives, then the colour engine and a `WM_SETTINGCHANGE` broadcast.
  - **WM 5.0–6.1.4:** `BaseHue` plus a theme `Skin` entry, which make the built-in colour engine recolour the whole shell live.
  - **Theme reset** restores the original theme from a backup.
- **Wallpaper sync:** the slice of your phone's wallpaper that sits behind the Windows Mobile screen becomes Windows Mobile's own wallpaper, so the two line up. There is a separate landscape picture for the open foldable. The Today screen's own picture cache is refreshed after every turn.
- The backdrop behind the skin is the live Android wallpaper, which the system draws exactly as on the home screen, with optional blur and opacity.
</details>

<details>
<summary><b>Status bar: battery, signal, carrier, data type, Wi-Fi</b></summary>

- **Battery:** DE's battery device reports the phone's real charge, charging state and temperature. In-memory patches fix the thresholds in Microsoft's `battdrvr.dll`, which otherwise warned "Main battery low" at 65%.
- **Signal, carrier, roaming and data type:** written to `HKLM\System\State\Phone` from Android's telephony state.
- **Network Indicator Type:** shows GSM letters (G / E / 3G / H) or CDMA letters (1X / Ev). A wired-network override that hid the letters is cleared by the helper.
- The Wi-Fi icon and signal follow the phone's Wi-Fi.
- See `docs/research-status-indicators.md` for the title bar's full icon logic.
</details>

<details>
<summary><b>Internet, storage card and GPS</b></summary>

- **Internet:** an emulated NE2000 PCMCIA card on PocketStylus's own user-mode NAT (guest 10.0.2.15, gateway .2, DNS .3). The helper provisions Connection Manager so Internet Explorer and apps just work.
- **Storage card:** a DE folder-share device mounts a phone folder (or several chosen folders, overlaid into one card root) as `\Storage Card`. Folders are managed from the Storage Card page in Windows Mobile's settings, using a folder browser for the phone's storage.
- **GPS:** the phone's location as an NMEA receiver on COM1 (GGA, GSA, GSV and RMC once a second), where Windows Mobile's GPS Intermediate Driver reads it. Location is used only while a program has the port open.
</details>

<details>
<summary><b>Sound, notification light and vibration</b></summary>

- The audio DMA honours auto-reload; Windows Media Player used to stop after 40 seconds without it.
- A loudness lift compensates for Windows Mobile's quiet mixer.
- The helper reports whether a sound is media, an alert or a ringtone, and the app reopens its audio stream with the matching Android usage, so ringtones follow the ringer volume and Do Not Disturb.
- **Notification light and vibration:** DE's notification-LED device drives a glowing dot on the skin and the phone's vibrator. A patch fixes Microsoft's LED driver, which rejected the shell's blink pattern.
</details>

<details>
<summary><b>Camera passthrough with a "2009" filter</b></summary>

- The helper installs `wmdcam.dll`, a hand-assembled ARM DLL, as the plug-in for Windows Mobile's NullCam driver. It talks to a camera device at PA 0x5F000000, which fills frames from the phone's Camera2 stream: preview 320×240, photos 640×480, video 176×144.
- The sensor is presented in portrait and follows Windows Mobile's rotation.
- The optional **2009 filter** degrades the image towards a period phone camera (noise, softness, colour cast and compression), with a strength slider.
</details>

<details>
<summary><b>Virtual GSM modem (Windows Mobile 6.1.4)</b></summary>

- WM 6.1.4 ships Microsoft's real GSM radio driver (`rilgsm.dll`), which expects a modem on the serial port. PocketStylus emulates that modem (`core/src/board/modem.cpp`) and speaks the AT dialect the driver uses. The helper points the driver at COM1 once.
- The modem reports your phone's real signal bars, registration, roaming, operator name and own number. Calls dialled in Windows Mobile reach the modem as `ATD`, and the app places them through Android.
- Your number comes from the SIM (permission "read phone numbers", asked only when calls are on), or from **My phone number** in the Phone calls settings. Windows Mobile reads it when the radio starts, and shows it in Phone → Menu → Options.
</details>

<details>
<summary><b>Phone calls from Windows Mobile's own dialer</b></summary>

- Turning calls on lets WM 6.5's FakeRIL load. The helper becomes a second RIL client, sees each call the Phone app dials, and the app places the real call through Android Telecom.
- A small in-memory patch holds FakeRIL's dial thread, so Windows Mobile's call screen goes Dialing → Connected exactly when the real call does. Ending, holding or muting on either side is mirrored to the other.
- With **Use Windows Mobile's call screen** (the default-phone-app role), calls stay entirely inside Windows Mobile. Without it, Android's call screen opens and PocketStylus comes back to the front.
- Emergency numbers always go to the phone's own dialer.
</details>

<details>
<summary><b>Bluetooth, clipboard, keyboard and hardware buttons</b></summary>

- **Send via Bluetooth** in Windows Mobile opens Android's share sheet with the file. Files the phone receives over Bluetooth appear on the storage card.
- **Clipboard sync** works both ways.
- **Use Android IME over Device IME:** tapping a Windows Mobile text field opens the phone's keyboard, and what you type is sent as Windows Mobile key events.
- **Hardware buttons:** the skin's Start, App Launch 1 and 2, and OK keys are Windows Mobile's Buttons 1–4, each assignable in Device Settings.
</details>

<details>
<summary><b>Hardware-side fixes: ROM patches, buttons and the storage card</b></summary>

- Fixes to Microsoft's drivers are applied from outside the guest by the emulator core (`core/src/board/codepatch.cpp`). Each driver is recognised by byte signature, not by fixed address, so the right patch applies on each release and survives everything that happens inside Windows Mobile. They cover:
  - the battery thresholds that warned "Main battery low" at 65%;
  - the notification LED rejecting the shell's blink pattern;
  - the FakeRIL dial gate.
- Buttons 1–4 are sent as keys the stock images already understand, so they work even without the helper.
- The storage card's presence check answers exactly as Device Emulator's does, so Windows Mobile always remounts `\Storage Card` after the card is re-inserted.
</details>

<details>
<summary><b>Screen DPI</b></summary>

- Device Settings → **Screen DPI** sets Windows Mobile's logical DPI. Windows Mobile reads it at boot, so changing it restarts Windows Mobile once:
  - Image default / High (192) is right for most phones.
  - Medium (128) and Classic (96) fit much more on screen on 6.1.4 and 5.0, but lose some title-bar icons.
  - WM 6.5's home screen needs 192.
</details>

<details>
<summary><b>Inside Windows Mobile: the helper, its settings app and self-repair</b></summary>

- `guest/Helper` is a .NET CF 3.5 program, `WMDroid.exe`, started from the storage card's autorun. It installs itself, adds a StartUp entry, and serves the app's mailbox (`\Storage Card\.wmdroid\in` and `out`).
- **PocketStylus settings inside Windows Mobile** (Settings → System → PocketStylus) are generated from the same catalogue as the Android Device Settings page and apply live.
- **Self-update:** each build carries a hash. An outdated helper hands over to the new one, which ends any leftovers.
- **Watchdog:** if a step of the helper's loop blocks for 45 seconds, it logs where it stuck and restarts itself.
- **Auto-standby** is switched off inside Windows Mobile, because Android decides when the phone sleeps.
- **.NET CF 3.5 bootstrap** (`guest/Autorun`): a 6.6 KB native ARM program, built with the Android NDK's clang and wrapped as a Windows CE executable. On releases without .NET CF 3.5 (WM 5–6.1.4) it installs Microsoft's redistributable silently with `wceload /noui`, then starts the helper.
</details>

---

## Building from source

Requirements: Windows with Android Studio (its JDK and SDK, NDK 29), and an ARM64 Android phone over adb for testing.

```powershell
# Guest programs (C# helper and tools; uses the .NET 3.5 compiler and the CF 3.5 reference assemblies)
powershell -File tools\build-guest.ps1
python tools\make-autorun.py          # native .NET CF bootstrap (NDK clang)
# App
.\tools\build-apk.ps1                 # debug build + install
.\tools\build-apk.ps1 -Release -NoInstall   # signed release (needs key.properties, see below)
```

Release signing reads `key.properties` at the project root (git-ignored):

```
storeFile=C:/path/to/release.jks
storePassword=…
keyAlias=…
keyPassword=…
```

### Headless runner

`core/cli` builds a command-line emulator for the phone. It's used for bring-up, regression tests and every investigation in `docs/`:

```bash
sh tools/build-cli.sh                 # builds with the NDK and pushes to /data/local/tmp/wm
adb shell "cd /data/local/tmp/wm && ./wmcore-cli fwvga.bin --mem 192 --screen 480x854 --seconds 60 --shot 55:home.ppm"
```

Useful options:
- `--load-state` resumes a saved session; `--compare` runs the JIT against the interpreter.
- `--key`, `--tap`, `--drag` and `--power` send input; `--put T:file:text` writes a mailbox command at time T.
- `--reset` does a warm reboot; `--dump-ram`, `--jit-map` and `--burst` help with debugging.

## Project layout

```
core/src/arm/     CPU, MMU, JIT, disassembler
core/src/board/   SMDK2410 board: S3C2410, flash, PCMCIA/NE2000, DE devices, camera, snapshot
core/src/net/     user-mode NAT
core/cli/         headless runner
app/              Android app (Kotlin UI + JNI: GL presenter, AAudio, sensors, bridges)
guest/Helper/     the in-guest helper (C#, .NET CF 3.5)
guest/Autorun/    native .NET CF bootstrap (C, Windows CE ARM)
guest/*/          small guest programs (Send via Bluetooth, GPS test)
tools/            build scripts, skin designer, ROM scanner, image pusher
docs/             research notes: hardware, status bar, themes, calls, camera, GPS, Bluetooth, …
```

## Legal

Windows Mobile, Windows CE and the Device Emulator images are Microsoft's. PocketStylus includes no ROM images: you supply your own. The APK bundles Microsoft's freely redistributable .NET Compact Framework 3.5 CAB (`NETCFv35.wm.armv4i.cab`), which is installed into releases that lack it, under Microsoft's redistribution terms. PocketStylus was written without using Device Emulator source code.
