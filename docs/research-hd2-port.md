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
| `stage.py <files> <out> <pkg-prefix> [extra…]` | copy one package (+extras) to a folder, .rgu → UTF-8 .reg |
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
- **HTCFlashlight**: null function call (PC=0). Likely a missing HTC service or a dynamically loaded
  DLL; not yet analysed.
- **Turbo3D**: "E: error.not_loaded" message box.

Crash triage: `Probe copy \Windows\System\DumpFiles "\Storage Card\dumps"`, then parse the .kdmp
(CEDS: stream directory at 0x20 with (type, size, rva); 0x8002 exception, 0x8003 modules (field 1 =
base, field 2 = size), 0x8006 contexts). Probe also has `install <dir>` (copy to \Windows + import
.reg) and `runwait <exe> [secs]`.

## Teeter sensor protocol (from Labyrinth.exe's embedded SensorSDK)

CreateFile("SEN0:"); on failure it activates the HTCSensorService driver and retries.
DeviceIoControl 0x0100000F (8-byte input) starts tilt; 0x01000001 reads tilt data (out buffer
passed with size 0); 0x02000001 reads the light sensor. Named events HTC_GSENSOR_SERVICESTART/STOP.
On the HD2 the stack is HTCSensorSDK.dll → HTCSensorService.dll (SEN stream driver) →
GSensor.dll (ECS driver, I2C). Plan: our own SEN driver backed by a board device fed from the
phone's accelerometer, like `camera.cpp`.

## Next steps

1. Decode romimage's compressed relocations properly (or validate the heuristic on more DLLs).
2. Install HTC services in the order their packages expect (HTC_Framework, Resource_Proxy,
   HTC_Util, htcDevService), then retry Calculator, Flashlight and Album.
3. SEN0: driver + accelerometer device → Teeter, auto-rotate, compass toys.
4. GL ES: pass-through `libgles_cm.dll`/`libEGL` to the host GPU → Manila / Sense home.
