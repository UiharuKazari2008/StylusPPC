# Windows Mobile 6.5.3 themes, and a Material You theme generated from Android

Scope: how `.tsk` themes work in the 6.5.3 Professional FWVGA image (`PPC_USA_GSM_FWVGA_VR.BIN`, 480x854, 192 DPI), what a theme controls, and how WMDroid can generate a theme from the Android Material You palette and apply it live through the guest helper.

Tags:
- **[ROM]**: verified in this image: the IMGFS files, the default hive, the first-boot provxml, or ARM disassembly of the modules.
- **[DOC]**: public Microsoft or Android documentation, or long-standing community knowledge.
- **[INFERRED]**: a deduction that has not been tested in the guest yet.

Evidence lives in the session scratchpad:
- `themes/<name>/` holds the six unpacked `.tsk` files.
- `fs/` holds the IMGFS files.
- `status/all/` holds the module sections.
- `thm_dis/*_b.txt` holds the annotated disassembly of aygshell, cplmain, gwes, shell32 and cmhome.
- `fw653/default_tree.txt` is the HKLM/HKCR dump.

---

## 1. How 6.5.3 themes work

### 1.1 The `.tsk` file

- **A `.tsk` is a CAB file** that wceload installs. [ROM]
  - `HKCR\tdyskin\Shell\Open\Command` is `wceload.exe "%1" /silent /nodelete /safe`.
  - `HKCR\.tsk` maps to `tskfile`, and its Open command is `ctlpnl.exe cplmain,13`, so tapping a `.tsk` opens Today settings.
  - `AppInstall\ExtensionOptions\.tsk = 3`.
- **Contents.** Every built-in theme has the same layout [ROM: `themes/*/`]:
  - `_setup.xml`, a `wap-provisioningdoc` with an `Install` block, `FileOperation` and `Registry` characteristics.
  - A legacy `MSCE` header (`*.000`).
  - Two JPEGs extracted to `%CE2%` (`\Windows`): `stwater_480_854.jpg` and `stwater_854_480.jpg`.
- **Most theme art is not inside the `.tsk`.** The built-in themes point the registry at PNGs already in ROM `\Windows`, such as `classicblue_softkeybar.png` and `black_taskbar.png`. [ROM]
- **File-name whitelist.** `HKLM\AppInstall\TSKAllowed` limits which file names a `.tsk` may install. The list covers `stwater_480_854.*`, `stwater_854_480.*`, `tdywater_*`, `softkeybar.png`, `taskbar.png`, `menuSelector.png`, `msgboxBorder.png`, `PushButton*.png`, `bubble_*.png`, `CheckBox*ThemeImage.png`, and so on. Allowed types are png/bmp for all of them, and jpg/gif only for wallpapers, `softkeybar` and `taskbar`. [ROM]
  - A third-party `.tsk` therefore ships files under these generic names.
  - The whitelist binds only the wceload path. The helper writes files directly, so it is not affected.
- **Where themes live.** Six themes ship in `\Windows`: Classic Blue, DVF green, Isaac Mizrahi purple, Rock and Republic black, Ron Arad orange and Vera Wang red. [ROM: fs]
  - The Today CPL enumerates `*.tsk` in two folders. One is `\windows`, and the other is very likely `\My Documents`. [ROM: cplmain `FindFirstFileW` loops at 0x2046868 and 0x20469a0, string `\windows`][DOC]
- **First boot applies Classic Blue.** `mxipcold_wpc_2.provxml` writes `HKCU\Software\Microsoft\Today\Skin = \Windows\Classic Blue.tsk` plus the full Classic Blue registry set. [ROM]

### 1.2 Registry that defines the active theme [ROM: `_setup.xml` and provxml]

| Key | Values |
|---|---|
| `HKCU\Software\Microsoft\Today` | `Skin` (path of the active .tsk), `UseStartImage=1`, `DPI=192`, `MultiColorThemeEnabled=1`, optional `Wall` (user picture) |
| `HKLM\Software\Microsoft\Color\MultiColorTheme` | `BaseColor1..4` (DWORD COLORREF `0x00BBGGRR`) |
| `HKLM\Software\Microsoft\Color` | `ThemeColors` and `ThemeValues`, each binary 280 bytes = 70 DWORDs (per-entry override flag and value). Read-only by themes: `DefSHColor`, `SHColor` (41 entries), `BaseHue` |
| `HKLM\System\GWE` | `DefSysColor` / `SysColor` (29 COLORREFs). Themes do not write these directly |
| `HKCU\Software\Microsoft\Today\{SoftKeyBar, Toolbar, TaskBar, ScrollBar, Bubble, Toast, MessageBox, Menu, ListView, TabView, UpDownControl, BubbleTile, Magnifier, PropertySheet, CutCopyPasteControl, PushButton, CheckBox, RadioButton, ComboBox}` | `<Part>ThemeImage` = full path to a PNG, plus `…TopMargin/BottomMargin/LeftMargin/RightMargin` (or `…Padding`), `…Tiled`, and some `…Height` values. These are 9-slice "theme primitives" |

- **70 = 29 + 41.** `DefSysColor` has 29 entries and `DefSHColor` has 41. aygshell `memset`s a 280-byte table before it reads them. [ROM]
  - That the 70 entries run SysColor[0..28] first, then SHColor[0..40], is [INFERRED].
  - **Correction from WM5 (§5.2):** WM5's aygshell reads `DefSHColor` into entries 0..40 and `DefSysColor` into 41..69, so the order is **SHColor first**. 6.5 most likely keeps it: entry 54 = SysColor 13 (COLOR_HIGHLIGHT = the accent), 55 = 14 (HIGHLIGHTTEXT, white), 46 = 5 (COLOR_WINDOW, white). [INFERRED for 6.5]
- **TabView colours** such as `BackgroundColor=0x40000006` look like "palette index" references (flag `0x40000000` + index). [INFERRED]

### 1.3 Engine modules in the image [ROM]

| Module | Role |
|---|---|
| `cplmain.cpl` (Today CPL, applet 13) | Theme picker. Its apply sequence is below |
| `aygshell.dll` | Colour engine. Ordinal **#371** (0x2f068bc) reads `Today\Skin`, `ThemeColors/ThemeValues`, `DefSHColor`, `DefSysColor`, `MultiColorTheme\BaseColorN` and `BaseHue`, builds the palette and its shared cache (`AYGSHELL_COLOR_CACHE_FILE`), then calls `commctrl.dll#82`, which is the only importer of coredll `SetSysColors`. #371 reads no register arguments (void) |
| `gwes.exe` | Draws the theme primitives. It is the only module that reads `TaskBarThemeImage`, `MenuBorderThemeImage`, `PushButton*`, `ScrollBar*`, `MessageBox*`, `PropertySheetPivot*` and `SoftKeyBarThemeImage` |
| `shell32.exe` | At boot it calls `UpdateThemePrimitives` (coredll export 2720) after `ColdInitPhase2Done`. It registers a registry-change callback (`ossvcs#346`) on `HKCU\Software\Microsoft\Today` value `Skin`. The callback calls `aygshell#371` and then `InvalidateRect`. It also handles `WM_SETTINGCHANGE` wParam 0xF2 (reload, then `InvalidateRect`) and 0xF3 |
| `cmhome.dll` (Titanium) | Watches `Today\Wall` and `Today\Skin` (`ossvcs#336`). It loads the wallpaper from `\Windows\stwater_%d_%d%s`, using `GetSystemMetrics` width and height. It handles 0xF2 |

### 1.4 What the Today CPL does when you pick a theme [ROM: cplmain 0x2047bc0–0x2048064]

1. Delete `Color\ThemeColors`, `ThemeValues`, `BaseHue` and `Today\MultiColorThemeEnabled`. Delete the keys `Color\MultiColorTheme` and all `Today\<Part>` subkeys.
2. `ShellExecuteEx(lpClass="tdyskin", lpFile=<tsk>)`, which runs wceload. Wait up to 30 s.
3. Read `UseStartImage` and `DPI`.
4. Write `Today\Wall`, or delete it when no picture is chosen, and write `Today\Skin = <tsk path>` (0x204655c).
5. Call **`UpdateThemePrimitives()`** (coredll).
6. Call **`PostMessage(HWND_BROADCAST, WM_SETTINGCHANGE (0x1A), 0xF2, 0)`**.

aygshell, shell32, cmhome and gwes all compare the wParam against 242 (0xF2). aygshell's subclass proc debounces it with a 100 ms `SetTimer(id 242)`. [ROM]

**So a theme applies live, with no soft reset needed.** [ROM: the shipped CPL does exactly this]

On a soft reset, shell32 re-runs `UpdateThemePrimitives`, and the colours come back from the registry, so the theme persists. [ROM]

---

## 2. What a theme controls at 480x854

### 2.1 Colours

All six themes write `BaseColor2 = #FFFFFF`. The other three base colours (RGB):

| Theme | Base1 (deep/secondary) | Base3 (accent) | Base4 (light contrast) |
|---|---|---|---|
| Classic Blue | #182375 | #2D5EEF | #BBB6FD |
| DVF green | #F255A2 | #2BA173 | #FFB9C4 |
| Isaac Mizrahi purple | #9951DB | #EC2093 | #FFC600 |
| Rock and Republic black | #000000 | #5F0100 | #FFFFFF |
| Ron Arad orange | #553102 | #F96314 | #FDD5A6 |
| Vera Wang red | #4E4E4E | #ED2117 | #FFC600 |

[ROM]

- **Base3 is the theme's named colour** (highlight and selection). Base2 is the text drawn on it. Base4 is often a contrasting tint (tertiary-like). [INFERRED from the table]
- **ThemeColors/ThemeValues overrides used by the built-ins.** [ROM: `thm_tools/dec.py`]
  - #FFFFFF at entries 16, 37, 38, 45, 46, 55 and 66.
  - Entry 54 = Base3 (Classic Blue darkens it slightly to #2D5ECF).
  - Entry 47 = Base1 and entry 58 = a muted tint (Isaac, Ron Arad and Vera Wang).
  - **No built-in theme overrides the window background or text colours.** Entries 5 and 8 under the SysColor-first layout are never set, so apps keep white surfaces even in "black" themes.

### 2.2 Images

Sizes are those in the FWVGA ROM. All PNGs are 8-bit RGBA, non-interlaced. [ROM: fs]

| Registry part | Size (WxH) | Slice margins T/B/L/R | Differs between themes? |
|---|---|---|---|
| TaskBarThemeImage (top title/status bar) | 480x52 (Vera Wang: 60x60) | 0/0/0/0 | yes |
| TaskBarThemeHighlight | 120x52 | 24/24/40/40 | yes |
| SoftKeyBarThemeImage | 8x68 (stretched) | 12/12/2/2 | yes |
| SoftKeyBarThemeHighlight / SipHighlight | 180x52 / 120x52 | 24/24/60/60, 24/24/40/40 | no (`black_*`) |
| MenuBorderThemeImage | 118x101, `…Height=444` | pad 13/21/22/43 | yes |
| MenuSelectorThemeImage | 172x72 | 18/18/6/6 | yes |
| MenuUp/DownArrowBackground | 289x67 | 0/11/15/36, 11/0/15/36 | yes |
| ListViewSelectorThemeImage | 474x72, MaxHeight 94, HeaderHeight 64 | 18/18/6/17 | yes |
| MessageBoxBorder / BubbleBorder | 104x101 | 11/21/13/20 | yes |
| MessageBoxCaption / BubbleTitle / ToastTitle | 389x46 | — | yes |
| ToastBorderThemeImage | 472x230 | 15/0/16/16 | yes |
| PropertySheetPivotBackground (tab pivot) | 8x64 | — | yes |
| ScrollBar thumb / track | 26x70 / 26x120 | — | no |
| UpDown pushed, CutCopyPaste* | 54x44, 60x60 | — | yes |
| BubbleTile* icons | 48x48 | — | yes |
| PushButton*, CheckBox*, RadioButton*, ComboBox* | 60x60, 40x40, 40x40, 64x64 | — | no (shared) |
| **Wallpaper** `\Windows\stwater_480_854.jpg` and `stwater_854_480.jpg` | 480x854 and 854x480, baseline JPEG | — | yes |

- **Titanium honours only the wallpaper.** `Titanium_480x854.cpr` draws `ConfettiPlus::WallpaperBitmap` at 480x854 (Top −36), and its text colours are hard-coded `#FFFFFFFF`. The rest of the home screen does not follow the theme. [ROM]
- **The Start menu background** is the same `stwater` image when `UseStartImage=1`. [ROM: aygshell/shell32 `\Windows\stwater*.*`][DOC]

---

## 3. Plan: a Material You theme, applied live

### 3.1 Android side (new `ThemeGenerator.kt`)

1. **Read the palette.**
   - **API 31+:** `getColor(android.R.color.system_accent1_N / accent2_N / accent3_N / neutral1_N / neutral2_N)`. N 0…1000 corresponds to tone 100…0: `_600` = tone 40 and `_100` = tone 90. [DOC]
   - **API 27–30 fallback:** `WallpaperManager.getWallpaperColors(FLAG_SYSTEM)` (no permission needed), then expand to tones with HCT.
   - Choose light or dark from `Configuration.uiMode`.
2. **Map roles to WM elements** (light scheme; dark changes only the bar images).

   | WM target | Material role | Source |
   |---|---|---|
   | BaseColor3, ThemeValues[54] | primary | `system_accent1_600` (tone 40, ≥4.5:1 against white) |
   | BaseColor2, white overrides (16, 37, 38, 45, 46, 55, 66) | onPrimary | `system_accent1_0` |
   | BaseColor1, ThemeValues[47] | secondary, deep | `system_accent2_700` (tone 30); try `accent1_800` |
   | BaseColor4 | tertiaryContainer | `system_accent3_100` |
   | ThemeValues[58] | outlineVariant | `system_neutral2_300` |
   | TaskBar image 480x52 | surface (dark: surfaceContainerHighest) | `neutral1_900`→`neutral1_800` gradient, white glyphs stay legible |
   | SoftKeyBar 8x68 | surfaceContainer, tinted | `neutral1_900` → `accent1_800` vertical gradient |
   | Menu/ListView selector, TaskBar/SoftKey highlight | primary / primaryContainer | `accent1_600` fill, 1 px `accent1_400` edge, rounded |
   | Menu/MessageBox borders, captions, toast | surface + primary header | `neutral1_10` body, `accent1_600` caption |
   | Pivot background 8x64 | secondaryContainer | `accent2_100`, or `accent2_800` in dark |
   | Wallpaper 480x854 / 854x480 | primary→tertiary gradient | `accent1_700`→`accent3_500`, optional noise; JPEG q≈85, baseline |
   | Optional, experimental: SysColor overrides 13/14 (HIGHLIGHT/HIGHLIGHTTEXT), 5/8 (WINDOW/WINDOWTEXT), 15 (BTNFACE), 17 (GRAYTEXT) | primary/onPrimary, surface/onSurface, surfaceContainer, outline | Off by default. Built-ins never change 5/8 |

3. **Generate images procedurally** with `Bitmap.Config.ARGB_8888` → PNG, at exactly the sizes in §2.2, keeping the built-in slice margins.
   - The **MVP** covers colours, wallpaper, TaskBar, SoftKeyBar, the Menu and ListView selectors, and the pivot background.
   - For every other part, point the registry at the existing ROM files (`\Windows\classicblue_*.png`, `black_*.png`, `default_*.png`). Referencing them in place means nothing from Microsoft is copied or redistributed.
4. **Write the theme** into the shared folder as `\Storage Card\wmdroid-theme\g<N>\`: the PNG/JPG files plus `theme.xml`.
   - `theme.xml` is a `wap-provisioningdoc` with the same `Registry` characteristics as `_setup.xml`.
   - `ThemeColors`/`ThemeValues` are written as `datatype="binary"` base64, as the ROM themes do.
   - Image paths use the final `\Windows\…` names.
5. **Trigger.** Send `theme g<N>` to the helper.
   - Do this when the helper first answers `ping`, from `WallpaperManager.addOnColorsChangedListener`, and on an uiMode change.
   - Skip the send when the hash of the palette and mode is unchanged.
   - Put it behind a Settings toggle: "Match Android colours".

### 3.2 Guest helper (new `Theme.cs`, verb `theme`)

1. **Copy the PNGs** to `\Windows\wmdroid_g<N>_<part>.png`.
   - Each generation gets new names, so registry values always change and no file that gwes holds open is overwritten.
2. **Replace the wallpaper.**
   - Copy the wallpaper to a temp file, then `DeleteFile` and `MoveFile` it to `\Windows\stwater_480_854.jpg` (and `_854_480`). Retry a few times on a sharing violation.
   - Delete `HKCU\…\Today\Wall`, as the CPL does when no picture is chosen.
3. **Write the registry.**
   - `DMProcessConfigXML(theme.xml)` (the existing `Patch.Provision`).
   - Always write every `Today\<Part>` key, `MultiColorTheme\BaseColor1..4`, `Color\ThemeColors/ThemeValues`, and `Today\MultiColorThemeEnabled=1`, `UseStartImage=1`, `DPI=192`.
   - Then `RegFlushKey`.
4. **Write `Skin` last** with a changed value: alternate `\Windows\WMDroid-a.tsk` and `\Windows\WMDroid-b.tsk`.
   - This fires shell32's and cmhome's `Skin` notifications, which recompute colours via #371 and redraw. [ROM]
5. **Apply belt-and-braces:**
   - `[DllImport("aygshell.dll", EntryPoint="#371")] static extern int ReloadThemeColors();`, then call it.
   - `[DllImport("coredll.dll")] static extern void UpdateThemePrimitives();` (the export is named), then call it.
   - `PostMessage((IntPtr)0xFFFF, 0x1A, (IntPtr)0xF2, IntPtr.Zero)`.
6. **Delete the previous generation's files**, then reply `ok`.

**Phase 2 (optional):** also emit a real `WMDroid.tsk` (an uncompressed MSCF CAB with `_setup.xml` and the whitelisted generic names from §1.1). Re-selecting it in Settings › Today then reproduces the theme. It also means `Skin` points at a real file.

### 3.3 Validation steps (CLI with `--share` and `--shot`; do not touch the user's live session)

1. **Confirm the palette layout.**
   - `regdump` `HKLM\Software\Microsoft\Color` and `HKLM\System\GWE` after applying each built-in theme from the CPL.
   - Then write distinctive test values into ThemeValues[i] and read `GetSysColor(0..28)` from the helper. This settles the SysColor-first index layout.
2. **Check that the steps are sufficient.** Apply the generated theme with steps 1–5 above and take screenshots of the Start menu, a menu, a list view, a message box and Titanium. Then remove step 5 and check that the `Skin` change alone is enough.

---

## 4. Risks

- **A soft reset should not be needed.** [ROM: the CPL path]
  - Apps that cached `GetSysColorBrush` brushes, or their own copies of colours, may keep old colours until they restart. [INFERRED]
  - If the `stwater` file stays locked, the wallpaper changes only after Titanium reloads it.
- **Formats.** Use RGBA PNG for primitives, as every ROM primitive is.
  - JPEG is accepted for `softkeybar`, `taskbar`, `scrollbarTrack` and wallpapers. GIF and BMP are accepted for wallpapers. [ROM: TSKAllowed]
  - Use baseline (SOF0) JPEG, as the ROM themes do. Progressive JPEG is untested.
- **Sizes.** Primitives are 9-sliced by the margins, so the sizes are flexible but must be larger than the margins.
  - The wallpaper must exist as `stwater_<SM_CXSCREEN>_<SM_CYSCREEN>` for both orientations (480_854 and 854_480), or Titanium falls back. [ROM: cmhome]
- **Contrast.** Base2 (white) is drawn on Base3, so keep primary at tone ≤ 40–50.
  - Dark-mode SysColor overrides (WINDOW/WINDOWTEXT) are untested territory: built-in themes never change them, and many WM apps hard-code black text.
- **Unknown semantics.**
  - The exact BaseColor → SHColor derivation (aygshell 0x2f06404/0x2f0665c plus `commctrl#77`, probably HLS/luma maths).
  - `BaseHue`, and `Restriction=2` under `Color` and `Today`.
  - The meaning of WM_SETTINGCHANGE 0xF3.
  - The UpdateThemePrimitives signature (assumed void).
- **Selecting another theme** in Settings › Today deletes all theme keys first (§1.4). A WMDroid theme is then lost until the app pushes again. The Phase 2 `.tsk` fixes this.
- **Persistence.** Everything lives in the registry and the RAM object store, so it survives soft reset and saved states, and is lost on hard reset.

---

## 5. Windows Mobile 5.0 to 6.1.4: the BaseHue colour scheme

Scope: WM5 Pocket PC Phone VGA (`PPC_USA_GSM_VGA_VR.BIN`, CE 5.1.1700), plus WM 6.0 (5.2.1235), 6.1 (5.2.19202) and 6.1.4 (5.2.20757).

- None of these has the 6.5 theme primitives. coredll has no `UpdateThemePrimitives` export (GetProcAddress returns 0 on WM5; the helper's P/Invoke fails with "Can't find an Entry Point" on all four). [ROM]
- There is no `MultiColorTheme` key. [ROM: registry dumps]

Evidence lives in the session scratchpad under `wm5/`:
- `m/` holds the modules, dumped from the running guest because the XIP sections are compressed.
- `*.ann` holds the annotated disassembly.
- `guava/` holds the unpacked `Guava Bubbles.tsk`.
- `d/card/p1.txt.out` holds the registry dump.
- The tools are `wm5x.py`, `wm5imp.py`, `wm5ann.py` and `probe5/` (a CF probe run through the helper's `run` verb).

### 5.1 Registry (stock values) [ROM: live registry dumps]

| Value | WM5 | WM 6.0 | WM 6.1 / 6.1.4 |
|---|---|---|---|
| `HKLM\Software\Microsoft\Color\BaseHue` | `0x00B0D589` | `0x00B0D57B` | `0x00778D52` |
| `HKCU\Software\Microsoft\Today\Skin` | unset ("Windows Default") | unset | `\Windows\Windows Mobile 6.1.tsk` |
| Per-skin key | none | none | `HKCU\…\Today\Windows Mobile 6.1.tsk` with `COLOR_TITLEFLATCENTER` |
| `.tsk` files in `\Windows` | Guava Bubbles | + Windows Mobile Green | + Windows Mobile 6.1 |

- **The `Color` key holds** `BaseHue`, `SHColor`, `DefSHColor` (41 COLORREFs) and `Restriction=2`. `HKLM\System\GWE` holds `SysColor` and `DefSysColor` (29).
- **The background pictures** are `\Windows\Default_stwater*.gif`. 6.1 adds `stwater.jpg`, which comes from its theme.
- **BaseHue format.** `BaseHue` is `0x00VVSSHH`: hue, saturation and value, each on a 0..240 scale.
  - The WM5 blue `#1575BC` gives H = 137, S = 213, V = 176, which is exactly `0x00B0D589`.
  - When S = 0, the scheme turns grey.

### 5.2 How aygshell builds the colours [ROM: WM5 aygshell #371 = 0x326A5E8]

#371 is the same ordinal as on 6.5. shell32 calls it too. It runs these steps:

1. **Get the hue.** It reads `BaseHue`. If BaseHue is missing, it derives one from shell colour 0x10000005 (SHColor[5]) through `commctrl#77`, then writes it back. Grey falls back to hue 137.
2. **Load the overrides.** It reads `ThemeColors` (flags) and `ThemeValues` (values), each 280 bytes = 70 DWORDs.
   - **If `Today\Skin` is unset or empty**, it replaces both with `DefSHColor` and `DefSysColor`, with every flag set. The stock colours are then used exactly, and BaseHue has no effect.
3. **Recolour the entries.** It walks two tables: 36 shell entries (index 0x10000000+n, n ≤ 40) and 17 system entries (0x40000000+n).
   - Each entry has a built-in HSV colour.
   - A flagged entry takes its `ThemeValues` value.
   - Any other entry keeps its own S and V but takes the BaseHue hue. If BaseHue is grey, the entry becomes a luminance grey.
   - A value with the entry's name under `HKCU\…\Today\<skin file name>`, such as `COLOR_TITLEFLATCENTER` on 6.1, then overrides the result.
   - For some entries it also deletes old WM2003-style values named `"<n>"` under `Color`.
4. **Apply.** It passes the 53 colours to `commctrl#82` (the SetSysColors path), then redraws the gradients for the hue.

### 5.3 What the Today CPL does when you pick a theme [ROM: WM5 cplmain 0x25149F4–0x2514CC4]

1. **Delete old colour values.** It deletes `Color\"0"…"40"`, except `"19"`.
2. **Delete the old pictures.** It calls `aygshell#329(1)`, which runs a FindFirstFile loop over `\Windows\stwater*.*` and `\Windows\tdywater*.*`.
3. **Install the theme.** For a `.tsk`, it calls `ShellExecuteEx(class "tdyskin")` (wceload) and waits up to 30 s.
   - Guava Bubbles' `_setup` writes `Color "5"` and `"14"` = `#B32C2C`, `Today\UseStartImage=1`, and `\Windows\stwater.gif`.
4. **Write the Today values.** It writes `Today\UseStartImage=1` and `DPI`.
5. **Write the skin.** It writes `Today\Skin = <tsk path>` and writes or deletes `Wall`.
6. **Set the hue.** For a theme, it deletes `BaseHue`, so aygshell re-derives it from the theme's colour 5. For "Windows Default", it writes `BaseHue = 0x10189`.
7. **Broadcast.** It sends `PostMessage(HWND_BROADCAST, WM_SETTINGCHANGE, 0xF2, 0)`.

**Applying is live.** shell32 registers `ossvcs#346` on `HKLM\SOFTWARE\Microsoft\Color` value `BaseHue`, and its callback calls `aygshell#371`. shell32's 0xF2 handler then invalidates and redraws. [ROM: shell32 0x16194, 0x42D24]

### 5.4 What the helper does on these versions (`Theme.ApplyHue`)

The helper takes this path when the version is CE 5.1, or CE 5.2 with a build below 21000.

1. **Hue.** It reads the accent, `MultiColorTheme\BaseColor3`, from the same `theme.ini`, so ThemeGenerator is unchanged. It converts the accent to a `BaseHue`.
2. **Overrides.** It removes `Color\ThemeColors` and `ThemeValues`.
   - An older helper wrote them in the 6.5 layout. Once a Skin is set they would apply, turning coloured parts white.
3. **Registry.** It writes `BaseHue`, and `Today\Skin = \Windows\WMDroid.tsk`. Skin has to be non-empty, but the file does not need to exist.
4. **Refresh.** It calls `aygshell#371` and broadcasts 0xF2. It skips `UpdateThemePrimitives`.
5. **Backup.** Each changed value is backed up as on 6.5. `theme reset` restores the old BaseHue and Skin (or deletes Skin), and the stock colours come back live.
6. **Stamp.** The stored stamp is `hue:<hash>`, so a theme that an older helper wrote here without effect is applied again.

**Verified with the CLI and the user's theme** (accent `#ED595B`, giving hue 239):

| Version | Result |
|---|---|
| WM5 | Live recolour of the title bar, soft-key bar, Today separators, the Start menu, Settings tabs, the selection highlight and the "ok" button |
| WM5, after a soft reset (`--reset`) | The scheme is still applied |
| WM 6.0, 6.1, 6.1.4 | Bars and the Start menu recolour, and reset restores teal or green |
| Old helper on 6.x (for comparison) | Only the Start-menu highlight changed, through ThemeValues[54] = COLOR_HIGHLIGHT |

**Not themed:**
- The Today background picture: WM5's `Default_stwater*.gif`, or 6.1's `stwater.jpg`. On WM5, shell32 also builds `%s_%d_%d%s` names and loads them with `SHLoadImageFile`, as on 6.5.
- The `.tsk` that a real theme installs. Choosing a theme in Settings › Today replaces ours, as on 6.5.
