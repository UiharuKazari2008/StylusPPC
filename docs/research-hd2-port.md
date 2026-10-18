# Running HTC HD2 software on the DE board

Goal: run apps from the HTC HD2 WM 6.5 ROM (Sense/Manila, HTC toys, camera) on our emulated
SMDK2410 board by porting the software, not by emulating the Qualcomm QSD8250.

ROM: `guest/HTC_HD2_WM65/RUU_signed.nbh` (T-Mobile US 3.14.531.1, CE 5.2 kernel built 2010-07-13).
It is partly cooked: Co0kie HomeTab, MortScript, XTHACK, "Modified_Manila_files".

## Tools (`tools/hd2/`)

| Script | Does |
|---|---|
| `nbh2nb.py <nbh> <out>` | NBH → `os.nb` + partitions (`p0_xip_boot`, `p1_xip`, `p2_imgfs`) + splash |
| `imgfs.py <imgfs.bin> list` / `get <out> <names…\|*>` | IMGFS reader; plain files as-is, ROM modules rebuilt into PE DLLs/EXEs |
| `dsm.py <files> [pattern]` | ROM packages (.dsm): package name + file list |
| `stage.py <files> <out> <pkg-prefix> [extra…]` | copy one package (+extras) to a folder, .rgu → UTF-8 .reg (the app does this itself now: `core/src/htc/romkit.cpp`, below) |
| `peinfo.py <capstone-dir> <out.json> <files…\|@list>` | imports + ISA scan |

Extracted output lives in `guest/HTC_HD2_WM65/extract/` (`files/`, `imgfs_list.txt`,
`packages.txt`, `survey.json`, `de653_dlls.txt` = DLLs in the stock 6.5.3 DE image).

## Formats (verified)

- **NBH (R000FF)**: 9-byte block headers (u32 len, u32 siglen, u8 flag) from file offset 0x17; each
  data block (0x10000) is followed by its signature (0x80). The inner image has an HTCIMAGE header,
  section types at 0x40, offsets at 0xC0 and lengths at 0x140 (0x400 = OS, 0x600 = splash).
- **OS section**: NAND sectors of 0x800 bytes, each followed by an 8-byte tag
  (u32 logical sector, u32 flags). MBR partitions: 0x20 boot XIP, 0x23 XIP, 0x25 IMGFS, 0x04 FAT.
- **IMGFS**: boot sector GUID F8AC2C9D-…, entry size 0x34, sector 0x800, data chunk 0x1000,
  compression `XPH` = [MS-XCA] LZ77+Huffman (the extended-length byte is read *before* the offset
  bits). Header blocks (sig 0x2F5314CE, next-block offset) hold 0x34-byte entries:
  FFFFFEFE module, FFFFF6FE plain file, FFFFF6FD module section, FFFFFEFB long name.
  Names: cch ≤ 4 inline; otherwise the offset points at a name entry, or straight at raw UTF-16 for
  very long names. Data tables are u16 csize, u16 usize, u32 offset per 4K chunk (csize == usize
  means stored).
  The stock DE 6.5.3 image also has IMGFS (sector 0x1000, `XPR`), but behind the NOR FAL, so its
  header chain doesn't map flat; `Probe files` on the live guest is easier.
- **Modules**: the 0xD0-byte header stream is e32_rom (+timestamp, 9 units, subsys at 0x6C) +
  o32_rom × objcnt (dataptr is junk). Writable sections may sit at o32_realaddr ≠ vbase+rva
  ("datamoved"). **Relocations are in romimage's compressed format** (section with o32 flags
  0x42002042, dataptr 0x10000000), not PE blocks. Feeding them to the loader corrupts the DLL
  (ResLoader's DllMain jumped to 0xE59D1004). `imgfs.py` therefore **recovers** fixups: words
  addressed by PC-relative LDRs in code, plus aligned words in data/.pdata, that fall inside the
  image or a moved section. It writes standard HIGHLOW blocks and points moved targets back at
  vbase+rva. This is heuristic; the real decoder (in nk.exe's loader) is still to do.
- **Packages**: .dsm = u32 header size; +0x14 name length, +0x18 name offset; 20-byte records
  (u32 len, u32 flags 0x1000xxxx, u32 name offset, …); names are UTF-16 without terminators.
  Registry comes from `<guid>.rgu` (UTF-16 REGEDIT4).

## CPU: no work needed

The capstone survey of 1,091 native binaries found nothing beyond ARMv5TE except Qualcomm codec
DLLs (`CJpegEncodeEngine/DecodeEngine/DecodeService.dll1`, `mp4encoder.dll`, NEON) and `wmdrm.dll`.
Other "v7" hits are conditional movw/movt misdecodes of data tables, or UPX-packed sections
(TaskManager, World_Clock, MortScript).

## Static dependencies beyond the stock 6.5.3 image

| App | Needs from HD2 |
|---|---|
| Notepad, HTCFlashlight, metalDetector (.NET + SilverMoon/OpenGl.dll), World_Clock, Turbo3D | — |
| Teeter (Labyrinth.exe) | zlibce_m.dll (+ SEN0: driver at runtime) |
| htcCalculator | ResLoader, VerInfou2 (+ htcStyleMetrix, ResProxy registry/service, \Windows bitmaps) |
| Camera | HTCSensorSDK, mdpl, voColorConversion, voJpegDec/Enc (+ htc_camera driver) |
| Album/Recorder/FM/AudioBooster/StreamingPlayer/MP3Trimmer | htcAfe, htcImage, htcStyleMetrix, htcXMLParser (+ codecs) |
| Manila, DigiCompass | libgles_cm (Qualcomm GL ES 1.x), libos, libpanel, lua, mode9 |

The stock image has ddraw, d3dm (+ the d3dmref software rasterizer), gx and imaging, but no GL ES.

## First results on desktop DE 6.5.3 (2026-10-16)

- **Notepad**: runs.
- **htcCalculator**: v1 crashed in ResLoader DllMain (compressed relocs). With recovered relocs plus
  install to \Windows and the .rgu imported, it starts but never shows a window and can't be
  killed (agent hangs). Probably blocked on HTC's ResProxy service (`Resource_Proxy` package),
  which ResLoader uses.
- **HTCFlashlight**: the exe loads `HTCFlashlightLib.dll` and calls `EnableFlashLight` / `SetFlashLightDegree`.
  HTC's library maps a Qualcomm PMIC and the call lands at address 0. PocketStylus replaces that DLL
  (`guest/Flash/flashlight.c`) and drives the phone's camera LED. Mode 0 releases it; any other mode holds it.
- **Turbo3D**: "E: error.not_loaded" message box.

Crash triage: `Probe copy \Windows\System\DumpFiles "\Storage Card\dumps"`, then parse the .kdmp
(CEDS: stream directory at 0x20 with (type, size, rva); 0x8002 exception, 0x8003 modules (field 1 =
base, field 2 = size), 0x8006 contexts). Probe also has `install <dir>` (copy to \Windows + import
.reg) and `runwait <exe> [secs]`.

## Module loading fixes (2026-10-17)

Two loader rules make a rebuilt ROM module run as a RAM DLL (`imgfs.py` applies both):

- **SizeOfImage must cover `.reloc`.** romimage's e32 vsize stops before the relocation section. With
  that size the WM 6.5 loader silently skips relocation, so the DLL runs with its ROM addresses
  (ResLoader's DllMain jumped to 0xE59D1004, zlibce_m wrote to 0x021BE0D8). Checked with
  `Probe loadlib <dll> <export> <its rva> <rva...>`: CE 5.2's LoadLibrary returns a handle, not the
  base, so the base comes from GetProcAddress.
- **`.reloc` flags 0x42000040**, not romimage's 0x42000042 (bit 0x2 marks its compressed format).
- DLLs are also rebased to 0x10000000 (a slot-1 preferred base is unusual for RAM DLLs). The loader
  puts code at 0x78xxxxxx and data in a separate area (0x018xxxxx), and fixes each reference by the
  section it points into.

## Teeter (Labyrinth.exe): running on PocketStylus (2026-10-17)

Teeter runs in PocketStylus with the phone's accelerometer, in the CLI on the Fold7 (480x800
state): level 1 draws, and the ball follows `--accel` tilts on both axes. Steps it needed:

1. **zlibce_m.dll**, rebuilt from IMGFS (above).
2. **SEN0: driver**, `guest/Sensor/htcsensor.c` → `\Windows\HTCSensorService.dll` (the helper installs
   it, HtcCompat.cs). Teeter calls `RegisterDevice(L"SEN", 0, L"HTCSensorService.dll", 1)` itself
   when SEN0: is missing. IOCTLs: 0x0100000F start tilt (in: two DWORDs), 0x01000001 read tilt
   (20-byte SENSORDATA: short TiltX, TiltY, TiltZ, pad; int AngleY, AngleX, flags; out size passed
   as 0), 0x02000001 light. Teeter polls every 50 ms and treats (X, Y, Z) as a gravity vector.
   Frame: 1000 = 1 g, face up (0, 0, -1000), left edge down X < 0, **top edge down Y < 0** (HTC's Y
   runs toward the bottom). Unknown IOCTLs are logged once by the core
   (`motion: SEN0: IOCTL ... not handled`); Teeter also sends 0x010303FF.
3. **Motion device** `core/src/board/motion.{h,cpp}` at PA 0x5F200000 ('MOTN'): X/Y/Z in HTC units,
   OPEN count, reads count as use (wanted for 2 s). App: `MotionBridge.kt` runs the accelerometer
   only while wanted (SENSOR_DELAY_GAME), turns samples into the guest panel's frame (window rotation
   + fold quarter turn) and pauses WM's auto-rotation meanwhile. CLI: `--accel T:ax,ay,az`.
4. **DirectDraw caps.** Teeter calls `IDirectDraw::GetCaps` and refuses ("Initialize Teeter fail.
   Memory is not enough") unless the driver caps include DDSCAPS_VIDEOMEMORY|BACKBUFFER|FLIP,
   DDBLTCAPS_READSYSMEM and DDOVERLAYCAPS_FLIP|ALPHASRC. The stock emulator display driver reports
   only ddsCaps 0x80C8 (front buffer, primary, system memory) with no blit, overlay or flip caps (see
   `guest/Tests/ddcaps.c`). After the check Teeter only uses a plain primary and offscreen surfaces
   with blits to the primary, which the stock driver plus the HEL handle. `guest/DdShim/hdraw.c` →
   `\Windows\HDRAW.dll` wraps IDirectDraw (every method forwards; GetCaps adds those bits).
   `tools/hd2/stage.py --redirect DDRAW.dll=HDRAW.dll` renames the game's import (same length) and
   **strips the Authenticode signature**: CE refuses a badly signed binary but runs an unsigned one.
5. **Screen 480x800.** At 480x854 Teeter's alpha blit overruns its 800-row buffers (data abort at
   Labyrinth+0xE990). It is a WVGA build (`PPC60_WVGA` in its PDB path), so HTC games want Device
   Settings → Screen resolution 480x800.

Debugging that worked: the CLI interpreter trace with `--nojit --trace N --trace-at <pc> --trace-pid
<slot>` (new: only fires in that FCSE slot, e.g. 1e000000, since other EXEs share the low addresses),
plus HTC's DebLogger (`HKLM\SOFTWARE\HTC\DebLogger` OutputType=3 writes `\DebLogger\<exe>.log`,
but zone names are still unknown, so it only logs its header).

## Installing HTC apps from the phone (2026-10-17)

Users bring their own ROM; nothing HTC ships in the APK or the repo. No PC step is needed:

1. **Settings → HTC HD2 apps… → Choose ROM file…** (`HtcAppsActivity.kt`): the HD2 `RUU_signed.nbh`
   (an `os.nb` from `nbh2nb.py` or a raw IMGFS dump also work). The page lists the ROM packages that
   contain an .exe; known-good ones (Teeter) come first, the rest under "Untested".
2. **Install Teeter**: the core's ROM kit (`core/src/htc/romkit.{h,cpp}`, JNI `htcrom_jni.cpp`,
   `HtcRom.kt`) stages the package into the mailbox overlay, `\Storage Card\.wmdroid\htc\<package>\`.
   It is `nbh2nb.py` + `imgfs.py` + `dsm.py` + `stage.py` in C++, reading the NBH in place (NBH block
   index → NAND logical-sector map → IMGFS partition, pread only; ~100 ms on the Fold7):
   - the package's files, ROM modules rebuilt with recovered relocations (byte-identical to
     `imgfs.py`: Labyrinth.exe, zlibce_m, ResLoader, VerInfou2 checked);
   - every staged EXE/DLL's `DDRAW.dll` import renamed to `HDRAW.dll`, Authenticode signature dropped;
   - `deps\<dll>`: ROM DLLs it imports that the guest lacks, followed transitively when the guest's
     list `htc\windows.txt` exists (the helper writes it: `\Windows\*.dll`, first line `# <OS build>`),
     else only the package's own imports;
   - `<package>.reg` (the .rgu as UTF-8) and Start menu shortcuts from the ROM's `initflashfiles.dat`
     (`Directory("…\Games"):-File("Teeter.lnk","\Windows\Teeter.lnk")`);
   - `install.ini` last (`file=`, `dep=`, `reg=`, `link=<shortcut>|<target>`, `missing=`).
3. The guest helper (`guest/Helper/HtcCompat.cs`, `InstallStaged`) installs every staged folder that
   has `install.ini` but no `done-<OS build>.txt`: files → `\Windows`, deps only if `\Windows` lacks
   them, REGEDIT4 import (Probe's parser), shortcuts; then it writes `done-<build>.txt` and shows
   "Teeter is installed. Start > Games > Teeter" (plus a 480x800 hint at other sizes). It runs at
   helper start, on `hello` (every session start) and on the `htcinstall` command the app drops in
   `in\h<ms>.cmd`, so it also works when the session restarts before the command is read.
   Installing again (the app re-stages and deletes the done marker) repairs a cold-booted OS.
4. **Screen 480x800**: the page warns when the OS isn't at 480x800 and offers "Use 480×800 and restart
   Windows Mobile"; Device Settings labels the choice "480×800 (WVGA; HTC games)".

Shell test of the kit: `sh tools/build-htcrom.sh` → `/data/local/tmp/wm/htcrom <rom> list [text]` /
`stage <package-prefix> <out> [windows.txt]`. CLI end-to-end test (2026-10-17, `hd2/inst/run.sh` on
the Fold7, state `hd2/s800.wms`, which had no Teeter): `htcrom stage` into the overlay, helper
`update` → at start it installed 56 files + zlibce_m (aygshell skipped, present), the registry key
and Start > Games > Teeter, wrote `windows.txt` (507 DLLs); `run \Windows\Labyrinth.exe` then played
with `--accel` tilts. The app page itself was compiled; its UI flow is not yet tried on a phone.

## Next steps

1. Decode romimage's compressed relocations properly (the heuristic has worked on every DLL tried).
2. Install HTC services in the order their packages expect (HTC_Framework, Resource_Proxy,
   HTC_Util, htcDevService), then retry Calculator and Album. Flashlight's LED is the phone's camera flash.
3. Other G-sensor users: auto-rotate (HTCSensorSDK), compass (needs a magnetometer IOCTL).
4. GL ES: pass-through `libgles_cm.dll`/`libEGL` to the host GPU → Manila / Sense home.
