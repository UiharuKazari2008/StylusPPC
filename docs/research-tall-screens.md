# Tall and non-standard guest screens for WM 6.5.3 Professional

Research notes for WMDroid's "match the phone's aspect ratio" screen mode. The trigger was glitches when
WM 6.5.3 Professional boots at 480x1024 for the Galaxy Z Fold7 cover screen (about 21:9).

Confidence tags:

- **[ROM]**: observed in this session by read-only inspection of the archive's 6.5.3 Professional `.BIN` images: IMGFS file lists, `default.hv`/`boot.rgu` dumps, `.cpr` layout files, and ARM disassembly of the OAL and display driver.
- **[SRC]**: DE 1.0 shared source.
- **[DOC]**: Microsoft documentation.
- **[WEB]**: other web sources (press, forums).
- **[SHOT]**: the existing WMDroid CLI screenshot `build-cli/tall1024.png`.
- **[MEM]**: general knowledge, not re-verified this session.
- **[INF]**: inference from the above.
- **[GUESS]**: plausible but untested.

---

## TL;DR

1. **WM 6.5.3 supports only 14 fixed screen sizes, and each 6.5.3 image is built for one of them.** [ROM] The shell builds layout file names as `<name>_%dx%d.cpr` and ships only these sizes: 240x320, 320x240, 240x400, 400x240, 320x320, 320x480, 480x320, 480x480, 480x640, 640x480, 480x800, 800x480, **480x854, 854x480**. There is no 480x1024 layout. Several per-image registry values and bitmaps are also tied to the size the image was built for.
2. **FWVGA is 480x854** (not 864) at 192 DPI. [DOC][ROM] The archive's `PPC_USA_GSM_FWVGA_VR.BIN` is a complete FWVGA build. The archive has **no FWVGA skin**, and DE itself rejects heights above 800 [SRC], so this image could never run at its native size under Microsoft's emulator.
3. **The 480x1024 glitches come from the guest, not from WMDroid's LCD path.** [ROM][SHOT][INF]
   - The OAL and the DE display driver handle 480x1024x16 correctly, and it fits the 1 MB frame buffer.
   - The shell's assets do not cover 1024 lines: 800x800 wallpaper and welcome bitmaps, an 800-based dialog work-area rect (`HKLM\System\GWE\dlgctr`), and no `Titanium_/LS_LockScene_/LS_Global_480x1024.cpr`.
   - Result: areas below line ~800 are never painted, or keep stale pixels.
4. **Recommendation:**
   - On the tall cover screen, offer **FWVGA image @ 480x854** (letterboxed) as the "best fit, fully supported" choice, and **WVGA @ 480x800** as the safe fallback.
   - Keep 480x1024 only as an **experimental** mode, on the **FWVGA** image plus a guest tweak pack (section 4.3).
   - Pair every image with its own native size. Never boot an image at another image's size: for example, the VGA, square or WVGA images at 480x1024.

---

## 1. What WM 6.5/6.5.3 supports and how resolution-specific the images are

### 1.1 Official resolutions

- **[DOC] "High DPI Display" (MSDN, WM 6.x OEM docs, 2010).** Professional/Classic resolutions:

  | Resolution | DPI | Orientation |
  |---|---|---|
  | 240x320 | 96 | portrait and landscape |
  | 480x640 | 192 | portrait and landscape |
  | 240x240 | 96 | square |
  | 480x480 | 192 | square |
  | **480x800** | **192** | portrait and landscape |
  | 320x320 | 128 | square |

  Standard (Smartphone): 176x220, 240x320, 320x320, 400x240, 240x400 and 440x240.
- **[DOC] "FWVGA Home Screen on Windows Mobile Standard" (MSDN, 4/8/2010).** "full-width VGA (FWVGA) ... is 480 x 854 pixels in portrait ... 854 x 480 ... in landscape at 192 dpi."
  - It is configured at **build time** with `set DPI=192`, `set RESH=480`, `set RESV=854`.
  - It claims that at FWVGA, apps, the welcome screen, the home screen, backgrounds and themes all display "with no blank margins (black borders or white areas)". That implies blank margins are what you get at sizes that were not built in.
  - The page title says Standard, but the 6.5.3 **Professional** ROM contains the full FWVGA set too [ROM].
- **[ROM] The shell's resolution set (all 6.5.3 Pro images contain every layout file).** `Titanium_*.cpr` (home screen), `LS_LockScene_*.cpr` and `LS_Global_*.cpr` (lock screen) exist for exactly the 14 sizes in the TL;DR. `SlidingPanel_*.cpr` exists only for 240x320, 320x240 and 320x320.
  - `shell32.exe` contains the format string `%dx%d`, the extension `.cpr` and the path `\Windows\%s`. `cmhome.dll` reads `CPRFileName="Titanium"` from `HKLM\Software\Microsoft\CHome\ExtensibilityXML`.
  - So the file name is the base name plus the current screen size. There is no 480x1024 (or any other) file.
- **[ROM] DPI-specific resources.** Only `shellres.192.dll`, `shellresapps.96.dll` and `shellresapps.192.dll` exist alongside the neutral DLLs. 128 DPI exists only for the 320x320 build.

### 1.2 Which archive image is which (6.5.3 Professional) [ROM]

Read from each image's `HKLM\System\GWE\dlgctr`, its wallpaper size and its `Drivers\Display\GPE\LogicalPixelsX`.

| Image | Native size | DPI | `dlgctr` (portrait work rect) | Wallpaper | Matching skin in archive |
|---|---|---|---|---|---|
| `PPC_USA_GSM_VR.BIN` | 240x320 | 96 | 0,18,240,286 | 320x320 | Pocket PC Phone (240x320) |
| `PPC_USA_GSM_WQVGA_VR.BIN` | 240x400 | 96 | 0,18,240,366 | 400x400 | 240x400 |
| `PPC_USA_GSM_QVGA_VR.BIN` | **320x320** (despite the name) | 128 | 0,25,320,275 | 320x320 | QVGA Square 320x320 |
| `PPC_USA_GSM_VGA_SQUARE_VR.BIN` | 480x480 | 192 | 0,36,480,412 | 480x480 | VGA Square |
| `PPC_USA_GSM_VGA_VR.BIN` | 480x640 | 192 | 0,36,480,572 | — | VGA |
| `PPC_USA_GSM_WVGA_VR.BIN` | 480x800 | 192 | 0,36,480,732 | 800x800 | 480x800 |
| `PPC_USA_GSM_FWVGA_VR.BIN` | **480x854** | 192 | 0,36,480,786 | 854x854 | **none** |

The archive's 6.5.3 skins: 240x320, 240x240, 240x400, 320x320, 480x480, 480x640 and 480x800. There is no 854 skin. WMDroid's `OsProfiles.kt` currently offers neither the FWVGA image nor the 320x320 image.

### 1.3 What actually differs between the WVGA and FWVGA builds [ROM]

The IMGFS file lists are identical (1184 files each). The differences:

- **Bitmaps resized to the build's screen:**
  - Wallpapers `Grass/Leaf/Spring/Water.jpg`: 800x800 vs 854x854, i.e. square at the long side, so rotation never shows a gap.
  - `welcomehead.192.png`: 854x854 in FWVGA.
  - Dialer backgrounds `dlrback_port.png`: 480x696 vs 480x750 (= screen − 104).
  - Dialer buttons, the five `.tsk` themes, `blankbanner/complete/myinfo.192.gif`.
- **`HKLM\System\GWE`:**
  - `dlgctr` = RECT(0,36,480,H−68): 732 vs 786.
  - `dlgctrL` = RECT(0,36,longside,412): 800 vs 854.
- **`HKLM\Software\Microsoft\MediaPlayer\Skins`:** `wmp_WPC_player_480x800.skn` vs `_480x854.skn` (portrait and landscape).
- **Two resolution-specific packages** (different `.rgu` GUIDs in `[Software\Microsoft\...\Packages]`): `18e5b098…`/`cfaf1396…` in WVGA vs `60850960…`/`b246f5ba…` in FWVGA. `mxipcold_wpc_2.provxml` differs by 3 bytes.
- **Identical in both:** `boot.rgu`, the display driver, the OAL, `LogicalPixelsX/Y = 192`, and all `.cpr` files.

The `.cpr` layouts encode the WM 6.5 chrome at 192 DPI: a 36 px top taskbar and a 68 px bottom menu (softkey) bar. That is 18/34 px at 96 DPI, which matches the WM 6.5 geometry change reported by developers [WEB codeproject/Zebra].

- Home form height = H − 104 (696 at 800, 750 at 854).
- Lock screen: ink area height H − 399; pane at top H − 244.

---

## 2. How real WM phones handled tall or high-resolution screens

| Device | Screen | OS | Notes |
|---|---|---|---|
| HTC Touch HD, Touch Pro2 | 480x800 | 6.1 (6.5 ROMs later) | [MEM] TouchFLO 3D / Manila replaced most of the Today screen |
| **HTC HD2** | 4.3" 480x800 | 6.5 | [WEB] Sense on top of 6.5 |
| Toshiba TG01 | 4.1" 480x800 | 6.1, then 6.5 | [MEM] |
| Toshiba TG02 / KDDI IS02 / K01 | 4.1" 480x800 | 6.5.3 | [WEB] |
| Sony Ericsson Xperia X1 / X2 | 3.0" / 3.2" 480x800 | 6.1 / 6.5 | [MEM]/[WEB] Xperia Panels |
| Samsung Omnia II (i8000), Omnia Pro | 3.7" 480x800 | 6.1 / 6.5 | [WEB] TouchWiz |
| Acer M900, neoTouch S200 | 3.8" 480x800 | 6.1 / 6.5 | [WEB]/[MEM] |
| Sharp Willcom 03, Hybrid W-ZERO3 | 480x800 | 6.1 | [MEM] |

- **No shipped WM 6.x device with 480x854 or anything taller turned up.** [WEB, negative result] Searches only found Android phones (Motorola Droid/Milestone) at 854. FWVGA support in 6.5.3 appears to have been groundwork that no OEM used.
- **How OEMs did it** [DOC][ROM][INF]:
  - The resolution is a **build-time** choice (`RESH/RESV/DPI`). The build pulls in the matching packages: themes, wallpapers, dialer art, the `dlgctr` registry values and the WMP skin.
  - OEMs therefore stayed on the officially supported sizes. Several also replaced the shell (Sense, TouchWiz, Xperia Panels), which carried its own layouts.
  - Nobody shipped a size without a `Titanium_WxH.cpr`.
- **Community hacks** [WEB][MEM]:
  - **`HKLM\Drivers\Display\GPE\LogicalPixelsX/Y` edits**, e.g. 192 → 128 or 96 on WVGA to fit "more" on screen. This changes DPI, not resolution. Reported results: misdrawn buttons and menus, wrong-size icons, because there is no `.128` resource DLL. RealVGA and WVGAFix tools did the same on 6.1.
  - **Cooked ROMs** swapped the resolution-specific packages and `.cpr`/`.png` sets. The XDA 6.5 guide mentions "Original CPR's" per resolution and a "Titanium CPR Helper".
  - Nobody (as far as found) ran a 6.5 shell at a height the build didn't contain. Without the files, Titanium/lock screen layouts are missing and bitmaps leave white or stale areas.
  - `HKLM\System\GDI\Rotation\Angle` only rotates; it doesn't add sizes.

---

## 3. Causes of drawing and repaint glitches at non-standard heights

### 3.1 Ruled out: the OAL, the display driver and the frame buffer [ROM][SRC]

- **OAL `InitDisplay`** (nk.exe, VA 0x8806F5EC):
  - Reads `BSP_ARGS` at 0xA0020044. If the signature isn't `0xDE12DE34`, it defaults to 240x320.
  - Programs LCDCON2 `LINEVAL = H−1` (10 bits, so **H ≤ 1024**) and LCDCON3 `HOZVAL = W−1`.
  - Programs LCDSADDR1 = PA **0x33F00000** (the top 1 MB of the 64 MB RAM), LCDSADDR2 = end address and LCDSADDR3 PAGEWIDTH = W.
  - Then clears W×H×Bpp bytes at 0xA3F00000.
  - **It does no range checking.**
- **Display driver** `DeviceEmulator_lcd.dll` (export name `DDI.dll`; `HALInit` means DirectDraw HAL):
  - Reads the same `BSP_ARGS`.
  - Computes fbsize = W×H×bpp/8 and reserves max(fbsize, 2 MB) of virtual space.
  - `VirtualCopy`s fbsize bytes from `LCDVirtualFrameBase` (0xA3F00000; registry `Drivers\Display\S3C2410\CONFIG`).
  - There is no 800 limit anywhere.
- **RAM:** the OS RAM ends at PA 0x33EEF000, and **exactly 1 MB** (0x33F00000–0x34000000) is reserved for the frame buffer.
  - 480x1024x2 = 960 KB fits; 480x854x2 = 801 KB fits.
  - **Any 24/32 bpp mode above 1 MB overruns past 0x34000000.** The OAL memset and the driver's VirtualCopy run off the end of the 64 MB static map, into the next mapping. So even 480x800 must stay at 16 bpp. WMDroid's `maxH = min(1024, 1MB / (2W))` is the right cap for 16 bpp only.
- **Height limit:** DE 1.0 rejects W or H > 800 in `boards_config.cpp` [SRC], and DE 3.0 does too [MEM]. That is an emulator policy, not a guest limit.

### 3.2 Real causes (guest shell side)

1. **Missing layout files** [ROM][INF].
   - At 480x1024, `shell32`/`cmhome` look for `\Windows\Titanium_480x1024.cpr`, and `lockscreen.dll` for `LS_LockScene_480x1024.cpr` / `LS_Global_480x1024.cpr`. None exist.
   - The home or lock scene then fails to load, or never paints its form. Whatever was in the frame buffer stays.
   - The fallback behaviour was not disassembled. **[GUESS]** It is "no scene", not "nearest size".
2. **Bitmaps sized for the build's long side** [ROM][SHOT].
   - Wallpapers, `welcomehead.192.png`, `.tsk` theme backgrounds and dialer backgrounds are 800 (WVGA) or 854 (FWVGA) tall.
   - `build-cli/tall1024.png` (WVGA image at 480x1024, 300 s after boot) shows the result: the orange "Windows" welcome image ends at y = 800 and rows 800–956 are white, while the taskbar (0–36) and softkey bar (956–1024) are drawn correctly at the true size.
3. **`HKLM\System\GWE\dlgctr`/`dlgctrL`** [ROM][INF].
   - These hard-code the dialog/work rectangle for the build's size. WVGA: (0,36,480,732). At 1024, full-screen dialogs and message boxes are sized and centred for 732.
   - **[INF]** This leaves a 224 px band (732–956) that nothing owns until the desktop repaints. It shows as stale content and is a likely source of the "repaint" artefacts.
   - The value is undocumented. Its meaning is inferred from its value always equalling (0, taskbar, W, H − menubar).
4. **Dialer and WMP layouts.** The dialer background is 480x(H−104) art. The WMP skin is `wmp_WPC_player_480x800.skn`; `wmp_WPC_pushed_480xY.gif` suggests partial height-agnostic support. [ROM]
5. **Third-party and CF apps** that assume 640/800 or hard-code bar heights [WEB codeproject/Zebra]. These are worse at 1024 than at 854.
6. **Minor emulator-side effects** [SRC][INF].
   - DE's touch mapping `SampleY = 920 − y·870/H` gives under one ADC step per line at H = 1024 (about 1.18 px quantisation). Harmless.
   - DirectDraw/GAPI apps get the full 480x1024 primary.
   - **[GUESS]** Any app that assumes ≤ 800 lines, or a 1 MB video heap with room for a back buffer, may fail to create offscreen surfaces. At 1024 only 64 KB of the 1 MB reservation remains.

**Not** a cause: the 1 MB limit at 16 bpp, LCD stride (OFFSIZE = 0, PAGEWIDTH = W), `LogicalPixels`, or rotation (the driver rotates in software via `SYSTEM\GDI\ROTATION`; landscape 1024x480 fits HOZVAL's 11 bits).

---

## 4. Recommendation for WMDroid

### 4.1 Image/size pairs to offer (6.5.3 Professional)

| Window shape (portrait h/w) | Offer | Why |
|---|---|---|
| ≥ 1.70 (Fold7 cover, most phones) | **FWVGA image @ 480x854**, fit to width, letterbox the rest | Tallest fully supported layout. Every asset, `dlgctr`, `.cpr` and WMP skin matches. On a 1080-wide cover: 2.25× scale → 1080x1922. |
| fallback | WVGA image @ 480x800 | The layout Microsoft actually shipped on devices and tested in DE |
| 1.25–1.45 | VGA image @ 480x640 | |
| ~1.0–1.2 (Fold7 main, near-square) | VGA_SQUARE image @ 480x480 (or VGA @ 640x480 when the window is landscape) | |
| experimental "fill screen" | **FWVGA image** @ 480xH, H ≤ 1024, plus the tweak pack below | Start from 854 assets: only 170 uncovered lines vs 224. Mark it experimental. |

Rules for the app (`EmulatorActivity.screenSize`, `OsProfiles`):

- **Add the FWVGA variant:** `ImageVariant("fwvga", "FWVGA 480×854", "PPC_USA_GSM_FWVGA_VR.BIN", 480, 854)`. Optionally also `PPC_USA_GSM_QVGA_VR.BIN` as "Square 320×320".
- **Replace "phone aspect at the image's width"** with "choose the supported size nearest the window aspect, then select the matching image". Do not stretch an image to another image's height. Today the code would boot, for example, the 480x480 square image at 480x1024.
- **Enforce bpp = 16 whenever W×H×Bpp > 1 MB.** In practice, always use 16 for 480-wide.
- **Scale non-integer with filtering, preserving aspect.** Fill the letterbox with black, or with the guest's top/bottom bar colour.
- **Optionally pick the 6.5.3 Standard WVGA image too.** The archive has `SP_USA_GSM_WVGA_VR.bin`; the app does not offer it.

### 4.2 Guest tweaks that are safe at native sizes

None are needed. At native sizes the image is already consistent. **Do not change `LogicalPixelsX/Y`**: there are no 128-DPI resources for 480-wide builds.

### 4.3 Tweak pack for the experimental 480xH mode (FWVGA base) [GUESS: built from ROM evidence, not yet tested]

Apply via the WMDroid agent on first boot, then soft reset.

1. **`HKLM\System\GWE`**: RECTs as four little-endian int32s.
   - `dlgctr` = (0, 36, 480, H−68). For H = 1024: `hex:00000000 24000000 e0010000 bc030000`.
   - `dlgctrL` = (0, 36, H, 412). For H = 1024: `hex:00000000 24000000 00040000 9c010000`.
2. **Layout files**: copy into `\Windows` as new RAM files. The shell reads `\Windows\<name>`, and new files can be added alongside the ROM files.
   - `Titanium_480xH.cpr` (from `_480x854`):
     - Form `Height` = H−104
     - `Background` `Height` = H
     - `HomeNav` `Height` = H−104
   - `LS_Global_480xH.cpr`: `Background` `Height` = H.
   - `LS_LockScene_480xH.cpr` (from `_480x854`):
     - Form `Height` = H
     - `LetterRecognition`/`AppText`/`GestureText` `Height` = H−399
     - `Overlay` `Height` = H−395
     - `Letter` `Top` += (H−854)/2
     - `PaneView` `Top` = H−244
     - The `LS_InkingArea_192_480x*.png` it references is not in the image anyway.
   - Landscape (`*_Hx480.cpr`) needs every right-anchored X shifted by (H−854). The 800→854 diff shows dozens of `Left`/`Width` edits. Either generate the files with a script or lock the experimental mode to portrait.
   - **[GUESS]** `cmhome.dll` also reads `HKLM\Software\Microsoft\cHome\QA\CPRFile`, which may allow pointing the home screen at an explicit `.cpr` path. Untested.
3. **Wallpaper**: set an H×H (1024x1024) JPEG as the Today/Home background so the `ConfettiPlus::WallpaperBitmap` covers the screen. Built-in themes stay 854 tall.
4. **Known remaining gaps**: the first-boot welcome image (854x854), dialer backgrounds (480x750), the WMP skin (480x854) and third-party apps.

### 4.4 Verification

Not done this session. Per instructions, I didn't execute anything from the archive.

Suggested headless check with the existing WSL runner, matching `build-cli/restest.sh`, after copying the FWVGA image to `~/wm/fw.bin`:

```
wmcore-cli-x86_64 fw.bin --mem 256 --screen 480x854  --seconds 300 --dump fw854.ppm
wmcore-cli-x86_64 fw.bin --mem 256 --screen 480x1024 --seconds 300 --dump fw1024.ppm
```

Expected:

- 854: full-screen home with no white band.
- 1024 without tweaks: bitmaps end at 854 and the home layout is missing.
- 1024 with the tweak pack: home and lock screen fill the screen.

---

## Sources

- Microsoft, "FWVGA Home Screen on Windows Mobile Standard" — https://learn.microsoft.com/en-us/previous-versions/ff599695(v=msdn.10)
- Microsoft, "High DPI Display" (WM 6.x resolution/DPI table) — https://learn.microsoft.com/en-us/previous-versions/bb431852(v=msdn.10)
- Microsoft, "High DPI Display" (WM 5.0 SDK) — https://learn.microsoft.com/en-us/previous-versions/ms889979(v=msdn.10)
- CodeProject, "Windows Mobile 6.5 Changed Screen Geometry" (taskbar 18 px / menu bar 34 px at 96 DPI) — https://www.codeproject.com/Articles/580718/Windows-Mobile-6-5-Changed-Screen-Geometry
- Zebra developer forum on fixed WM 6.5.3 bar sizes — https://developer.zebra.com/thread/25200
- pocketmagic.net, "Changing the Pocket PC screen resolution" (LogicalPixelsX/Y) — https://pocketmagic.net/?p=115
- computerbase.de, "Win Mobile 6.5 Auflösung ändern" (distorted UI after DPI edits) — https://www.computerbase.de/forum/threads/win-mobile-6-5-aufloesung-aendern.1355664/latest
- Pocketnow, "How to change your screen resolution" (WVGAFix) — https://pocketnow.com/how-to-change-your-screen-resolution
- XDA, "A guide to Windows Mobile 6.5" (per-resolution CPRs) — https://forum.xda-developers.com/t/a-guide-to-windows-mobile-6-5.504324/post-4426781
- HTC HD2 (Wikipedia) — https://en.wikipedia.org/wiki/HTC_HD2
- Xperia X2 480x800, WM 6.5 — https://itpro.com/614665/sony-ericsson-announces-windows-mobile-65-based-xperia-x2
- Toshiba K01/IS02 — https://m.gsmarena.com/toshiba_k01-3161.php ; https://www.slashgear.com/kddi-toshiba-is02-aka-tg02-hits-japan-in-june-3079613
- Acer M900 480x800 — https://www.phonescoop.com/articles/article.php?a=3977
- DE 1.0 shared source `boards/config.cpp` (64..800 and 1 MB checks), via `docs/research-de-hardware.md`
- ROM inspection scripts (session scratchpad): `imgfs_dump.py`, `hvtree.py`, `xipfile.py`, `armlite.py`, `immscan.py`, `imgdim.py`
