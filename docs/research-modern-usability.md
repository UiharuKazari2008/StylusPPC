# Modern usability: hardware buttons, CPU name, SMS sync, root certificates, clipboard

Research notes for WMDroid. Main target: `Windows Mobile 6.5.3 Professional\Emulation Images\PPC_USA_GSM_FWVGA_VR.BIN`.
The 6.5.3 Standard image has identical BSP offsets. Companion to `research-de-hardware.md` (§3.6 keyboard, §3.8 GPIO),
`research-cellular.md` and `research-status-indicators.md`.

Confidence tags:

- **[ROM]**: read from the 6.5.3 image. This includes registry `.rgu` sources, module sections pulled out of IMGFS, and ARM
  disassembly of those sections (NDK `llvm-objdump`, `--triple=armv5te`). Nothing from the archive was executed. See §6.
- **[BIN]**: read from the installed DE 3.0 `DeviceEmulator.exe`.
- **[SRC]**: DE 1.0 shared source (`gitntel/Microsoft-Device-Emulator`). Facts only, no code (see the licence note in
  `research-de-hardware.md` §0).
- **[DOC]**: Microsoft SDK headers or documentation, recalled rather than re-fetched. **[INFERRED]**: reasoned from the above
  and not tested on a device.

---

## TL;DR

1. **Buttons.** Every skin button sends an IBM AT set-1 code. DE turns it into a VK, then into Samsung matrix codes. Soft 1/2,
   Talk, End, Volume and Record have no matrix key of their own: each is sent as `Fn(0x63)` + a digit key. The guest
   `kbdmouse.dll` then turns `Fn+'1'..'0'` into F1..F10, which are exactly WM's `VK_TSOFT1`/`VK_TSOFT2`/`VK_TTALK`/`VK_TEND`/
   `VK_TVOLUMEUP`/`VK_TVOLUMEDOWN`/`VK_TRECORD`. **Power is not a key.** In DE, AT `0x75` is the soft-reset hotkey. The real power
   button is GPF0/EINT0, which `pwrbtn2410.dll` turns into `SetSystemPowerState(NULL, POWER_STATE_SUSPEND, POWER_FORCE)`.
   §1.3 has a complete table for `machine.cpp`. [ROM][SRC][BIN]
2. **CPU name.** About → Version shows `"%s %s"` of `szProcessCore` and `szProcessorName` from
   `KernelIoControl(IOCTL_PROCESSOR_INFORMATION)`, which today gives **"ARM920T DeviceEmulator"**. The strings are in nk.exe
   `.rdata` in XIP flash. The OAL reads them through three pointers in nk.exe's initialized `.data`, at flash offset
   `0x90E10` on 6.5.3. **Recommendation:** at load, write two UTF-16 strings into unused XIP flash and repoint those words.
   This works the same way on every WM5–6.5.3 image (§2.4). A registry write cannot change this field. [ROM]
3. **SMS sync.** No native compiler is needed. `cemapi.dll` exports the flat MAPI helpers that the managed PocketOutlook
   assembly itself P/Invokes (`#20`–`#54`), plus `HrSetOneProp`/`HrGetOneProp`. A CF helper can open the SMS store, call
   `IMAPIFolder::CreateMessage` in Inbox or Sent Items, and set any property, including `PT_SYSTIME` timestamps, through
   P/Invoke alone. There is no MMS client in the 6.5.3 image. [ROM]
4. **Root certificates.** Adding a root works: write the registry blob, or call `CertAddEncodedCertificateToStore`, or use the
   CertificateStore CSP. **But crypt32 on 6.5.3 has no SHA-2 signature OIDs** (no `sha256RSA`) and schannel stops at TLS 1.0.
   Modern public sites therefore stay unreachable however many roots are added. The only practical use is trusting a
   **private SHA-1/RSA-2048 root** for a TLS-1.0 terminating proxy that WMDroid would run on the Android side. [ROM]
5. **Clipboard.** coredll exports the classic clipboard API, and CF 3.5 has `System.Windows.Forms.Clipboard`. **There is no
   change notification:** no `SetClipboardViewer`, `AddClipboardFormatListener` or `GetClipboardSequenceNumber`. Poll instead.
   [ROM]
6. **Native toolchain.** None is installed: no eVC4, no VS2008 or WM6 SDK, and VS2022 ARM targets Thumb-2 only. The realistic
   option is **cegcc/mingw32ce (`arm-mingw32ce-gcc`) in the existing WSL Ubuntu**. With the findings above, none of the
   features here needs it. (§3.5)

---

## 1. Hardware buttons

### 1.1 The pipeline

- **Skin → AT code.** All 6.5.3 PPC skins in the archive (`Skins\*\*.xml`) define these buttons. [ROM: skin XML]

  | Skin button | AT set-1 | | Skin button | AT set-1 |
  |---|---|---|---|---|
  | Soft Key 1 | `0x3B` (F1) | | Up/Down/Left/Right | `0x48`/`0x50`/`0x4B`/`0x4D` |
  | Soft Key 2 | `0x3C` (F2) | | Enter (D-pad centre) | `0x1C` |
  | Talk | `0x3D` (F3), press-and-hold | | Rocker Up / Down | `0x48` / `0x50` (same as the arrows) |
  | End | `0x3E` (F4), press-and-hold | | Record | `0x44` (F10) |
  | Power | `0x75`, press-and-hold | | App Launch 1 / 2 | `DOWN:0x5B 0x70 UP:0x5B` / `… 0x71 …` |

  The Smartphone skins add **Volume Up `0x40` (F6)**, **Volume Down `0x41` (F7)**, Back `0x01` (Esc) and Home `0x5B` (LWin).
  No skin has a camera key. Some Smartphone skins send Soft Key 1–4 as `LWin + 0x70..0x73`.
- **AT → VK (DE).** DE 3.0 uses a 0x76-entry table, at file offset `0x6E68` in `DeviceEmulator.exe` [BIN]. It differs from the
  DE 1.0 source in one way: AT `0x47/49/4F/51/52/53` give Home/PgUp/End/PgDn/Ins/Del, not numpad keys. AT `0x70..0x74` →
  `VK_APP1..5` (0xC1..0xC5). AT `0x75` → `EMUL_RESET` (0xC6), which DE handles itself: soft reset, nothing sent to the guest,
  and skin auto-repeat skips it. [SRC `scancodemapping.cpp`, `skinregionbutton.cpp`, `devices.cpp`][BIN]
- **VK → Samsung code (DE).** DE looks the VK up in `VKtoSCTable`. If there is no direct code it uses `g_rgvkMapFn` and sends
  **`0x63` (Fn) down, key, `0xE3` (Fn up)** around *each* transition, both down and up. Keys in the NumLock table get a
  NumLock toggle first. [SRC]
- **Samsung code → VK (guest).** `kbdmouse.dll` (pdb `KbdS3C2410Us.pdb`) is the driver in `HKLM\HARDWARE\DEVICEMAP\KEYBD`
  (`Irq`=1, `IOBase`=`0xB1600000`, `SSPBase`=`0xB1900000`). Its tables [ROM]:
  - matrix table: `S001+0x168`, 0x72 bytes, indexed by Samsung code;
  - Fn table: `S000+0x98`, 21 `{vk, vkGenerated}` DWORD pairs: `'1'..'0'`→F1..F10, `-`→NumLock, `=`→Cancel, `P`→Insert,
    `[`→Pause, `]`→Scroll, `;`/`'`→Snapshot, ←→Home, ↑→PgUp, ↓→PgDn, →→End;
  - NumLock table: `S000+0x20`, 15 pairs.

  Code `0x63` has no entry in the matrix table, so the driver handles it as the Fn modifier in code. **The guest has no F11+
  path**, so F16–F24 (`VK_TSPEAKERPHONETOGGLE`, `VK_TFLIP`, `VK_TPOWER`, `VK_KEYLOCK`…) cannot come from the keyboard device.

### 1.2 What WM does with the VKs [DOC][ROM]

`tpcshell.h`/`winuserm.h` define:

| Constant | Value |
|---|---|
| `VK_TSOFT1` | F1 `0x70` |
| `VK_TSOFT2` | F2 `0x71` |
| `VK_TTALK` | F3 `0x72` |
| `VK_TEND` | F4 `0x73` |
| `VK_TVOLUMEUP` | F6 `0x75` |
| `VK_TVOLUMEDOWN` | F7 `0x76` |
| `VK_TSTAR` | F8 |
| `VK_TPOUND` | F9 |
| `VK_TRECORD` | F10 `0x79` |
| `VK_TSPEAKERPHONETOGGLE` | F16 |
| `VK_TFLIP` | F17 |
| `VK_TPOWER` | F18 `0x81` |
| `VK_KEYLOCK` | F22 `0x85` |
| `VK_THOME` | `VK_LWIN` `0x5B` |
| `VK_TBACK` | `VK_ESCAPE` |
| `VK_TACTION` | `VK_RETURN` `0x0D` |
| `VK_APP1..6` | `0xC1..0xC6` |
| `VK_OFF` | `0xDF` |

- **App buttons.** PPC app buttons arrive as Win + `VK_APPn`. The shell binds them in `HKLM\Software\Microsoft\Shell\Keys\40C1..40C4`
  (`40` = MOD_WIN; the image defines "Button 1".."Button 4"). A camera key can be bound the same way: Win + `VK_APP5`, plus a
  `Keys\40C5` key pointing at `\Windows\camera.lnk`, which exists in the ROM together with `pimg.exe` and `NULLCAM.dll`.
  [ROM .rgu][INFERRED]
- **Evidence that the modules handle these keys.** Counts of `cmp rX,#imm` in the disassembly [ROM heuristic]:

  | Module | F3 | F4 | F6/F7 | F18 (`VK_TPOWER`) | F22 (`VK_KEYLOCK`) | `VK_OFF` |
  |---|---|---|---|---|---|---|
  | `shell32.exe` | yes | yes | 2/2 | — | yes | yes |
  | `cprog.exe` (phone) | 4 | yes | yes | yes | yes | — |
  | `gwes.exe` | — | 3 | — | 7 | yes | — |
  | `aygshell.dll` | yes | — | — | yes | yes | yes |

  So Volume (F6/F7) is very likely handled by the shell (volume bubble) on Pro too. [INFERRED]
- **End press-and-hold.** aygshell has `KeyLockOnly`, `KeyLockConfirm`, `SkipKeyLockConfirm` and exports
  `SHDeviceLockAndPrompt` (#398). `lockscreen.dll` exports `StartScreenLock`. A short End press hangs up or goes to Home.
  WM Pro builds commonly lock the device on a long End press, but the ROM contains no registry switch that proves it for this
  image. **[INFERRED] Verify** on the desktop DE with `wmctl hold` on the End button.
- **Lock / power from the emulator.**
  1. **Power button = GPIO, not a key.** Clear GPFDAT bit0, raise EINT0 if GPFCON[1:0] = 2, and release after two GPFDAT
     reads (`research-de-hardware.md` §3.8). `pwrbtn2410.dll`'s IST samples the button, `Sleep(200)`, samples again, then
     calls `SetSystemPowerState(NULL, 0x00200000 /*SUSPEND*/, 0x1000 /*FORCE*/)`. If that function is missing it calls
     `PowerOffSystem()` instead. A second path does the same with `0x00800000` (RESET), which is the reset button. [ROM
     disasm `pwrbtn2410.dll` @`0x7B4703DC`/`0x7B470614`]. On resume the WM 6.5 lock screen appears when lock is enabled.
     DE's own skin "Power" button (AT `0x75`) only soft-resets, so **do not copy DE here**. Wire Android POWER (or an on-screen
     power button) to GPF0. Resume handling depends on the open question in `research-de-hardware.md` §1.4: how DE 3.0 detects
     power-off for 6.x images.
  2. **Lock without suspending:** the helper P/Invokes `aygshell!SHDeviceLockAndPrompt()` (documented; WM6+). [DOC][ROM export]
  3. **Screen-off or suspend from the helper:** `coredll!SetSystemPowerState(null, POWER_STATE_SUSPEND, POWER_FORCE)` (#1582),
     or `keybd_event(VK_OFF,0,0,0)` / `KEYEVENTF_KEYUP` (#833). `keybd_event(VK_TPOWER)` is another possibility, since gwes
     clearly special-cases F18, but its effect is untested. [ROM exports][INFERRED]

### 1.3 Complete table for `Machine::key()` (AT set-1 in, Samsung codes out)

Format: `AT → VK → codes`. **Fn** means send `0x63`, then the code (with `|0x80` on release), then `0xE3`, for both press and
release. This is what DE does. Today `machine.cpp` holds Fn down from press to release, which also works but leaves Fn
latched if a second key arrives during a hold. [ROM matrix table + BIN AT table]

**Phone and hardware buttons**

| Button | AT | VK | Samsung sequence |
|---|---|---|---|
| Soft key 1 | 3B | F1 = VK_TSOFT1 | Fn + `31` |
| Soft key 2 | 3C | F2 = VK_TSOFT2 | Fn + `39` |
| Talk (green) | 3D | F3 = VK_TTALK | Fn + `2F` |
| End (red) | 3E | F4 = VK_TEND | Fn + `37` |
| Volume up | 40 | F6 = VK_TVOLUMEUP | Fn + `47` |
| Volume down | 41 | F7 = VK_TVOLUMEDOWN | Fn + `4F` |
| `*` (VK_TSTAR) | 42 | F8 | Fn + `57` |
| `#` (VK_TPOUND) | 43 | F9 | Fn + `41` |
| Record | 44 | F10 = VK_TRECORD | Fn + `49` |
| F5 | 3F | F5 | Fn + `3F` |
| D-pad ↑ ↓ ← → | 48 50 4B 4D | VK_UP/DOWN/LEFT/RIGHT | `6C` `6A` `6D` `6F` |
| Action / Enter | 1C | VK_RETURN = VK_TACTION | `5A` |
| Home / Start (Win) | 5B | VK_LWIN = VK_THOME | `60` (RWIN `5C` → `5F`) |
| Back (Smartphone) | 01 | VK_ESCAPE = VK_TBACK | `29` |
| App 1..5 | 70..74 | VK_APP1..5 | `64` `65` `66` `67` `68` (`21` is a second APP1 code), wrapped in LWin `60`/`E0` |
| Power | 75 | — | **No key.** Pulse GPF0/EINT0 (§1.2) |
| Soft reset | — | — | EINT2 (DE `BoardReset(soft)`), separate UI action |

**Keyboard**

| Keys | AT | Samsung |
|---|---|---|
| Esc, `1`–`0` | 01, 02–0B | `29`, `31 39 2F 37 3F 47 4F 57 41 49` |
| `-` `=` Bksp Tab | 0C 0D 0E 0F | `51 59 69 0B` |
| `Q`–`P` | 10–19 | `2B 33 3B 43 3A 42 4A 52 4B 53` |
| `[` `]` Enter | 1A 1B 1C | `5B 6B 5A` |
| Ctrl (L/R) | 1D | `19` (DE 3 gives VK_RCONTROL; VKtoSC folds L/R to `19`) |
| `A`–`L` | 1E–26 | `0D 2D 35 3D 45 4D 55 44 4C` |
| `;` `'` `` ` `` | 27 29 28 | `54 5C 09` |
| LShift, RShift | 2A, 36 | `12`, `62` |
| `Z`–`M` | 2C–32 | `0C 0E 2E 36 3E 46 4E` |
| `,` `.` `/` `\` | 33 34 35 (numpad /), — | `56 5E 5D 0A` |
| Alt (L/R) | 38 | `01` (VK_LMENU/RMENU fold to `01`) |
| Space | 39 | `6E` |
| Delete | 53 | `2A` |
| CapsLock | — | `2C` |
| Pause | — | `70` (or Fn + `5B`) |
| Home PgUp End PgDn | 47 49 4F 51 | Fn + `6D`, Fn + `6C`, Fn + `6F`, Fn + `6A` |
| Insert | 52 | Fn + `53` |
| NumLock | 45 | Fn + `51` |
| Numpad 0–9 `*` `+` `-` `.` | 60–6F etc. | NumLock-mode keys (`M U I O J K L 7 8 9` …). Avoid them; send the digits instead |
| AT `2B` | 2B | **VK_BACK** in DE's table, not `#` |
| F11, F12, Apps (5D) | 57, 58, 5D | dropped (no guest code) |

**Fixes this implies for `Keymap.kt` / `kKeys`** [INFERRED]

- `kKeys` has no Fn entries, so F1–F4 (soft keys, Talk, End) are currently dropped. Add them.
- `KEYCODE_POUND → 0x2B` produces Backspace. Send `#` as Shift+`3` (`12`,`2F`) and `*` as Shift+`8` (`12`,`57`) so text fields
  get the characters. F8/F9 are the Smartphone keypad VKs.
- `KEYCODE_VOLUME_*` currently sends ↑/↓. That matches the PPC skin's "Rocker", but it is not volume. Use Fn + `47` / Fn +
  `4F` for real WM volume, or keep the volume keys on the Android side.
- On Pro, `KEYCODE_BACK` → Esc (`29`) closes dialogs more naturally than Backspace.

---

## 2. CPU name in Settings → System → About

### 2.1 Where the string comes from [ROM disasm `cplmain.cpl`]

The About applet lives in `cplmain.cpl` (S000 VA `0x02031000`). Around `0x2049834` it does:

```
KernelIoControl(0x01010064 /*IOCTL_PROCESSOR_INFORMATION*/, NULL, 0, &pi /*0x240 bytes*/, 0x240, &ret);
StringCchPrintfW(buf, 100, L"%s %s", pi.szProcessCore, pi.szProcessorName);
SetDlgItemTextW(hDlg, 569, buf);
```

The label text `Processor:` and the format `Processor: %1!s!` are in `shellres.dll.0409.mui` and `tshres.dll.0409.mui`.
No registry value overrides the processor line.

The same function also reads `HKLM\Software\Microsoft\Shell\DeviceBeta`, value `About` (REG_SZ), and **appends it to the OS
version line** (control 564). That is a supported registry hook for adding custom About text such as
"WMDroid on Galaxy Z Fold7", but it cannot change the CPU field. [ROM]

### 2.2 The OAL side [ROM disasm nk.exe]

- `IOCTL_PROCESSOR_INFORMATION` dispatches to `0x880734F0`. Its entry in the IOCTL table is at flash `0x333D4`.
- The handler copies three strings, each length-checked:

  | `PROCESSOR_INFO` field | Offset | Max size | Source pointer (RAM) |
  |---|---|---|---|
  | `szProcessCore` | +0x02 | ≤ 80 bytes (39 chars + NUL) | `0x800764A0` |
  | `szProcessorName` | +0x54 | ≤ 80 bytes | `0x800764A4` |
  | `szVendor` | +0x16E | ≤ 200 bytes | `0x800764A8` |

- It also sets `dwInstructionSet` from `0x80096E70` and `dwClockSpeed` from `0x8007649C`. The clock value is `0x03066230`
  = 50,750,000, which is not displayed.
- If a string is too long, the IOCTL fails and About shows nothing. Keep within the limits.
- The pointers live in nk.exe's initialized `.data`. Copy entry 0 copies flash `0x88090970` to RAM `0x80076000` (0x600 bytes),
  so the pointer words are at **flash offsets `0x90E10` / `0x90E14` / `0x90E18`** and hold `0x88033494`, `0x880334A4` and
  `0x880334C4`.
- The strings themselves are UTF-16 in nk.exe `.rdata` (XIP, uncompressed). The next fields follow them directly, so the slots
  are tight:

  | Flash offset | String | Room |
  |---|---|---|
  | `0x33494` | `ARM920T` | 16 bytes |
  | `0x334A4` | `DeviceEmulator` | 32 bytes |
  | `0x334C4` | `Microsoft` | 20 bytes |
  | `0x334D8` | `Microsoft DeviceEmulator` | OEM info (`SPI_GETOEMINFO`) |
  | `0x3350C` | `PocketPC` | platform type |

  The `.data` words at `0x90E1C..0x90E28` point at the last two (platform name/manufacturer, OEM info, platform type).
  **Do not change OEM info.** Software, possibly including our own helper, detects the emulator with it.
- **The same layout exists in every DE image** [ROM scan]: a single pointer triple `[core, name, vendor]` → "ARM920T",
  "DeviceEmulator", "Microsoft":

  | Image | Pointer triple at flash offset |
  |---|---|
  | WM5 MSFP | `0x8C760` (flash VA `0x92xxxxxx`) |
  | WM6 / 6 Std | `0x8EDC8` |
  | 6.1 | `0x8FDC8` |
  | 6.1.4 / 6.5 | `0x90E08` |
  | 6.5.3 | `0x90E10` |

  To find it generically: locate UTF-16 `"ARM920T\0"` in the OS XIP region, then the one DWORD in flash whose value is its VA.
- **Free XIP flash for new strings:** the OS ROMHDR `physlast` is `0x882A3C30`, and the XIP partition (MBR at `0x20000`,
  type `0x22`) runs to `0x2E0000`. Flash `0x2A3C30..0x2E0000` is all `0xFF` (~240 KB). It is read-only XIP space that the
  FAL/IMGFS never writes. [ROM][INFERRED that nothing reads it]

### 2.3 Options compared

| Option | Verdict |
|---|---|
| **Flash patch at load:** write strings at e.g. flash `0x2A4000` (VA `0x882A4000`), repoint `0x90E10`/`0x90E14` (and `0x90E18` for the vendor) | **Recommended.** Deterministic and runs before boot. No guest code is involved. Survives soft reset because the kernel re-copies `.data`. The 6.5.3 core slot is 16 bytes, so in-place overwrite cannot hold "Qualcomm". |
| PC hook at `0x880734F0` in the emulator | Works, but the JIT would need a per-image breakpoint mechanism. Fragile. |
| Write the RAM pointers after boot | Racy, and RAM strings would need a safe home. No benefit over the flash patch. |
| Registry write by the helper | **Does not work** for the CPU line, because cplmain asks the OAL directly. Only the `DeviceBeta\About` suffix is possible. |

**What to write.** About shows `core + " " + name`. Use core = SoC vendor, name = marketing name: `"Qualcomm"` +
`"Snapdragon 8 Elite"` gives "Processor: Qualcomm Snapdragon 8 Elite". Vendor = `"Qualcomm"`.

**Where the Android values come from.**
- `Build.SOC_MANUFACTURER` / `Build.SOC_MODEL` (API 31+) give "Qualcomm" / "SM8750". Android exposes no marketing name, so
  WMDroid needs a small lookup table (SM8750 → "Snapdragon 8 Elite").
- Snapshots: the copied pointers live in RAM, so a restored snapshot keeps the old strings until the next boot. `imageCrc` is
  taken from the file, so patch the in-memory flash only.

---

## 3. SMS (and MMS) into WM Messaging

### 3.1 What the ROM offers [ROM]

The managed `Microsoft.WindowsMobile.PocketOutlook.dll` (CF GAC, in the ROM) does all its MAPI work through **ordinal exports
of `cemapi.dll`**. Their signatures were decoded from the assembly's P/Invoke declarations:

| Ordinal | Signature |
|---|---|
| #20 | `DoLogon(out IntPtr session)` |
| #23 | `DoGetMsgStoresTable(IntPtr session, out IntPtr table)` |
| #27 | `DoGetNextIdInTable(IntPtr table, out IntPtr entryId, out uint cb)` |
| #24 | `DoOpenAccount(IntPtr session, IntPtr entryId, uint cb, out IntPtr store)` |
| #29 | `DoGetFolder(IntPtr store, uint folderPropTag, out IntPtr folder)` |
| #30 | `DoCreateMessage(IntPtr folder, uint flags, out IntPtr msg)`: calls `IMAPIFolder::CreateMessage` (vtable +0x4C) |
| #39 | `DoSetStringProp(IntPtr obj, uint tag, string val)`: one-prop `SetProps` (vtable +0x20) |
| #42 | `DoSetLongProperty(IntPtr obj, uint tag, uint val)`: one-prop `SetProps` |
| #22 | `DoSetMessageRecipients(IntPtr msg, string to, string toNames, string cc, string ccNames, string bcc, string bccNames)` |
| #35 | `DoSetMessageBody(IntPtr msg, string body)` |
| #31 | `DoMoveMessage(IntPtr src, IntPtr dst, IntPtr msg, uint flags)` |
| #32 | `DoSubmitMessage(IntPtr msg)` |
| #43 | `DoGetContentsTable(IntPtr container, out IntPtr table)` |
| #38 | `DoGetStringProp(IntPtr obj, uint tag, out IntPtr str, out IntPtr toFree)` |
| #41 | `DoGetLongProperty` |
| #40 | `DoGetDateProperty` |
| #46 | `DoMapiFreeBuffer(ref IntPtr)` |
| #21 | `DoLogoff(ref IntPtr)` |
| #50 | `RegisterForMapiNativeEvent(...)` |
| #48 | `SendSMSMessage(string to, string text, bool report)` |

`cemapi.dll` also exports by **name**: `MAPIInitialize`, `MAPILogonEx`, `HrSetOneProp`, `HrGetOneProp`, `MAPIAllocateBuffer`,
`MAPIFreeBuffer`, `FreeProws`, `MAPI_GetStoreByName`, `MailSwitchToFolder`, `MailSyncMessages`. `tmailutl.dll #20` is
`AddAttachment(string body, int numFiles, IntPtr msg)`.

### 3.2 Plan: P/Invoke only, no native code [INFERRED, untested]

1. **Open the SMS store.**
   - `MAPIInitialize(IntPtr.Zero)`, then `DoLogon`.
   - Walk `DoGetMsgStoresTable` / `DoGetNextIdInTable`. For each entry, `DoOpenAccount`, then read `PR_DISPLAY_NAME`
     (`0x3001001F`) with `DoGetStringProp`. Pick the store named **"SMS"**.
2. **Open the folder.** `DoGetFolder(store, PR_CE_IPM_INBOX_ENTRYID or PR_IPM_SENTMAIL_ENTRYID, …)`. Take the folder tags from
   `cemapi.h`/`mapitags.h` (Inbox is the CE-specific `PR_CE_IPM_INBOX_ENTRYID`), or read them out of the managed assembly's
   constants.
3. **Create and fill the message.** `DoCreateMessage(folder, 0, out msg)`, then set:
   - `PR_MESSAGE_CLASS` = `"IPM.SMStext"`;
   - `PR_SUBJECT` and/or body = text;
   - `PR_SENDER_EMAIL_ADDRESS` / `PR_SENDER_NAME` (received) or recipients via `DoSetMessageRecipients` (sent);
   - `PR_MESSAGE_FLAGS` = `MSGFLAG_READ` for history;
   - `PR_MSG_STATUS` = `MSGSTATUS_RECTYPE_SMS`;
   - **`PR_MESSAGE_DELIVERY_TIME` / `PR_CLIENT_SUBMIT_TIME` as `PT_SYSTIME`** through `HrSetOneProp(msg, IntPtr spropvalue)`.
     Build the 16-byte `SPropValue` with `Marshal.AllocHGlobal`: tag (4), pad (4), FILETIME (8).
4. **Learn the exact property set** the native SMS stack writes. 6.5.3 FakeRIL can deliver a message to itself
   (`research-cellular.md` §6). Then read every property of a real received SMS with `HrGetOneProp` or the contents table and
   copy the schema. Threading in tmail groups messages by sender address.
5. **Commit.** `DoSetStringProp` and the others call only `SetProps`; nothing in the flat API calls `SaveChanges`. The
   PocketOutlook API never saves explicitly either, which suggests that CE MAPI commits on `SetProps`. **Risk:** if messages
   do not persist, call `IMAPIProp::SaveChanges` (vtable +0x10) through CF COM interop. Declare a `[ComImport]
   InterfaceIsIUnknown` `IMessage`, and get the object by declaring our own `DoCreateMessage` P/Invoke with
   `[MarshalAs(UnmanagedType.Interface)] out IMessage`.
6. **Unread counters.** The Home "unread SMS" count (SNAPI `System\State\Messages\SMS\Unread\Count`) is maintained by the
   messaging stack. A raw insert may not bump it until tmail recounts. For live incoming messages, also post
   `SHNotificationAdd` (aygshell #155) and consider `MAPI_RefreshRuleClients`. [INFERRED]
7. **WM → Android.** `MessageInterceptor` sees only incoming messages, so it cannot catch what the user sends. Watch the SMS
   store's Outbox/Sent Items with `RegisterForMapiNativeEvent` or by polling `DoGetContentsTable`. Then hand the message to
   Android `SmsManager`, but only after the user opts in. On 6.5.3 the RIL is the in-guest FakeRIL, so nothing reaches a modem.

**MMS.** The 6.5.3 image has **no MMS client**: no composer or transport module, only `IPM.MMS` references in
tmail/htmlnote/cmhome. Options:
- insert MMS as `IPM.SMStext` with the text plus a note, and save media to `\Storage Card`; or
- insert as `IPM.Note` into a local mail account with attachments (`tmailutl #20 AddAttachment`).

The ROM gives no evidence on how tmail would render a raw `IPM.MMS` item, so treat that route as unknown.

### 3.3 Effort

- Helper side: about 300–500 lines of C# P/Invoke, with no native DLL.
- Android side: read `content://sms` (needs `READ_SMS`; WMDroid is not the default SMS app, so it can read but not write) and
  stream it through the shared folder as JSON.

### 3.4 Documented alternatives that do not work

- `SmsMessage.Send` / `SmsAccount` only send.
- `MessageInterceptor` only intercepts incoming radio SMS.
- Writing `pim.vol`/`cemail.vol` directly (EDB) bypasses MAPI notifications and is fragile.

### 3.5 Native ARM toolchain status

**Nothing is installed** [checked]:
- no eVC4 or VS2008 Smart Device tools under the archive or `Program Files (x86)`;
- VS2022 is present, but its ARM32 compiler emits Thumb-2 for desktop Windows on ARM, which ARMv4/v5 WM cannot run;
- the Android NDK clang can target `armv5te`, but only as ELF, and its `arm-windows` target is Thumb-2 as well.

If native code is ever needed (e.g. `IMAPIProp::SaveChanges`, or a MAPI advise sink if COM interop fails), the practical
route is **cegcc/mingw32ce** (`arm-mingw32ce-gcc`, GCC + binutils for PE/ARM CE, links against `coredll`). Build it with the
`cegcc-build` scripts inside the WSL Ubuntu that is already installed, and add a `tools/build-native.sh` beside
`build-wsl.sh`. Write in C or C++ with no exceptions, and call COM vtables by hand. The outputs are not GPL-encumbered.
Converting NDK-clang ELF objects to PE by hand is possible but not worth it.

---

## 4. Root certificate updates

### 4.1 Where roots live [ROM]

- **Read-only ROM roots.** `sysroots.p7b` is an XIP file at flash VA `0x8829C248`, a PKCS#7 bundle of 25,734 bytes. crypt32
  has `\ROM` and `Root` strings side by side, so it most likely merges this bundle into "Root" [INFERRED]. It holds
  **29 roots, all 2009-era**:
  - 21 `sha1RSA`, 5 `md5RSA`, 3 `md2RSA` (counts by signature algorithm);
  - RSA keys of 1000–2048 bits;
  - includes VeriSign G3/G5, Thawte, GeoTrust, DigiCert Global/EV, GlobalSign R1, Go Daddy/Starfield Class 2, Baltimore,
    Entrust 2048, AddTrust, Comodo AAA, Equifax;
  - no ISRG, no Amazon, no ECC roots.
- **Writable stores.** Registry `HKLM\Comm\Security\SystemCertificates\<Store>\Certificates\<SHA1 hex, upper>\Blob`, with
  "Root", "CA" and "MY" under `SystemCertificates`. The ROM `.rgu` files populate the SPC, Privileged and Unprivileged
  Execution Trust Authorities stores this way.
- **Blob format** (seen in the ROM `.rgu` files). It is the standard serialized store element: a sequence of
  `{DWORD propId; DWORD encoding (=1); DWORD cb; BYTE data[cb]}`:
  - prop `3` = SHA-1 hash (20 bytes);
  - optional props (the code-signing stores add `0x8000`, a 4-byte role mask);
  - a final element `0x20` (`CERT_CERT_PROP_ID` / file element type 32) holding the DER certificate.

  A root entry needs only `3` + `0x20`.

### 4.2 Ways for the helper to add a root

1. **CryptoAPI (preferred):** `crypt32!CertOpenStore(CERT_STORE_PROV_SYSTEM_W=10, 0, 0,
   CERT_SYSTEM_STORE_LOCAL_MACHINE, "Root")` + `CertAddEncodedCertificateToStore(h, X509_ASN_ENCODING, der, len,
   CERT_STORE_ADD_REPLACE_EXISTING, IntPtr.Zero)`. Both are exported by name; `CertOpenSystemStoreW` is not. crypt32 has a
   `Comm\Security\Crypto\UI\RootStorePrompt` setting, so a confirmation prompt is possible. [ROM]
2. **CertificateStore CSP:** `Microsoft.WindowsMobile.Configuration.ConfigurationManager.ProcessConfiguration` (assembly in
   ROM), or `aygshell!DMProcessConfigXML` (#2021), with
   `<characteristic type="CertificateStore"><characteristic type="ROOT"><characteristic type="<SHA1>"><parm
   name="EncodedCertificate" value="<base64>"/>…`. Documented, and handles security roles. [DOC][ROM: cplmain uses this CSP]
3. **Raw registry Blob write:** works, but it skips crypt32's caches and checks. Use it only if 1 and 2 are blocked.

### 4.3 Algorithms and practical value [ROM]

- **crypt32's OID table** (~151 OIDs) contains:
  - `md2/md4/md5/sha1` with RSA, `sha1DSA`, RSA, DSA, DH, 3DES/RC2/RC4/DES, PKCS#7/#9/#12, and the usual X.509 extensions;
  - **no `1.2.840.113549.1.1.11/12/13` (sha256/384/512RSA)**, no `2.16.840.1.101.3.4.2.*` hashes, and no ECC.
- `rsaenh.dll` *does* implement SHA-256/384/512 and AES-128/192/256 as CAPI algorithms, but the chain engine cannot map
  certificate signature OIDs to them. Result: **any chain signed with SHA-2 fails verification on 6.5.3**, and nearly every
  CA has issued SHA-2 certificates since about 2016. RSA-4096 keys are fine for the CSP; the hash is the blocker.
- **schannel** offers SSL 2.0/3.0, PCT 1.0 and **TLS 1.0 only**. Its ciphers are RC4, DES, 3DES and AES-128/256 with RSA key
  exchange and an MD5/SHA-1 handshake hash. There is no ECDHE and no GCM.
- **Value.** Updating roots alone gains nothing for public sites. It becomes useful together with a **WMDroid-side TLS
  bridge**: a local proxy that speaks TLS 1.2/1.3 to the internet and TLS 1.0 (`TLS_RSA_WITH_AES_128_CBC_SHA`) to the guest,
  presenting leaf certificates from a **private root** with sha1RSA signatures and RSA-2048 keys. The helper installs that
  root once through option 1 or 2. mbedTLS 2.x can be built with TLS 1.0 enabled for the guest side. That makes IE Mobile
  HTTPS usable. Explain the MITM trade-off to the user. [INFERRED]

---

## 5. Clipboard bridge

- **coredll exports** [ROM]:

  | Function | Ordinal |
  |---|---|
  | `OpenClipboard` | #668 |
  | `CloseClipboard` | #669 |
  | `GetClipboardOwner` | #670 |
  | `SetClipboardData` | #671 |
  | `GetClipboardData` | #672 |
  | `RegisterClipboardFormatW` | #673 |
  | `CountClipboardFormats` | #674 |
  | `EnumClipboardFormats` | #675 |
  | `EmptyClipboard` | #677 |
  | `IsClipboardFormatAvailable` | #678 |
  | `GetOpenClipboardWindow` | #680 |
  | `GetClipboardDataAlloc` | #681 |

  **Not present:** `SetClipboardViewer`/`ChangeClipboardChain` (no `WM_DRAWCLIPBOARD`), `AddClipboardFormatListener` and
  `GetClipboardSequenceNumber`.
- **Simplest API:** CF 3.5 `System.Windows.Forms.Clipboard.SetDataObject(text)` / `GetDataObject()` (present in the ROM's
  `System.Windows.Forms`) for text.
- **P/Invoke version:**
  - `OpenClipboard(hwnd)`. Use a real window such as `Microsoft.WindowsCE.Forms.MessageWindow.Hwnd`: on Win32, a NULL owner
    makes `SetClipboardData` fail after `EmptyClipboard`, so avoid it.
  - `GetClipboardData(CF_UNICODETEXT = 13)` returns a pointer that can be read directly with `Marshal.PtrToStringUni`. CE has
    no `GlobalLock` step.
  - To write: `EmptyClipboard`, then `SetClipboardData(13, p)` with `p` from `Marshal.AllocHGlobal` (LocalAlloc on CF). The
    system owns `p` afterwards, so do not free it. Then `CloseClipboard`. [DOC][INFERRED]
- **Change detection:** poll every 0.5–1 s. Compare `GetClipboardOwner()` and a hash of the `CF_UNICODETEXT` contents. Skip
  while `GetOpenClipboardWindow() != 0`.
- **Avoiding echo loops:** remember the hash of the last value written from Android and do not send it back.
- **Android side:** `ClipboardManager.addPrimaryClipChangedListener`. Android 10+ only lets the foreground app read the
  clipboard, which WMDroid normally is.
- **Transport:** the existing shared-folder channel (`clip-in.txt` / `clip-out.txt`, UTF-8).

---

## 6. Method notes (reproducible)

- **The 6.5.3 `.BIN` is a raw NB0.**
  - The OS XIP TOC is at file offset `0x30040` (`romscan.py <img> 0x30000`).
  - The IMGFS header (GUID `f8ac2c9d-e3d4-2b4d-bd30-916ed84f31dc`, compression `XPR`) is at file offset **`0x310000`**.
- **IMGFS offsets are logical.** Every 64 KB flash block carries 60 KB of data, so `phys = 0x310000 + (L / 0xF000) * 0x10000 +
  L % 0xF000`.
- **IMGFS structure.**
  - Header blocks are 4 KB, signature `0x2F5314CE`, then a next-block pointer, then 0x34-byte dirents:
    - `FEFEFFFF` module;
    - `FEF6FFFF` file;
    - `FDF6FFFF` section;
    - `FBFEFFFF` long name. Names over 24 characters are stored raw at the pointer.
  - Data tables are runs of `{u16 compSize, u16 realSize, u32 offset}` per 4 KB chunk; `comp == real` means stored raw.
  - Compression is **MS-XCA plain LZ77 ("Xpress")**.
  - A module's header stream is `e32_rom` (0x70 bytes) plus `o32_rom` entries. Sections are `S000..`, placed at `o32_realaddr`.
- The scratch scripts used for this note (parser, disassembly wrapper, key-table generator) were kept out of the project.
  `romscan.py` could gain an `--imgfs` mode based on the above.
