# Microsoft Device Emulator (DE 1.0 / 2.0 / 3.0): virtual hardware reference

Research notes for WMDroid, an Android ARM64 C++ emulator that boots DE ROM images (WM2003SE through 6.5.3).

Confidence tags used below:

- **[SRC]**: read directly from the DE 1.0 shared-source C++ code. File names are relative to the source root.
- **[BIN]**: observed by inspecting data in local files: the user's ROM images, `.dess` files, and the DE 1.0/2.0/3.0 executables (strings and data tables only, nothing executed).
- **[DOC]**: Microsoft documentation or blog posts.
- **[GUESS]**: inference that has not been verified.

DE 2.0 and 3.0 are binary-only. Anything the V1 source says may have changed in 2.0/3.0 unless it is marked [BIN] for those versions.

---

## 0. Source availability

**The full DE 1.0 source was found.** It is the "Shared Source Device Emulator 1.0" release (July 2006, buildable with VS2005).

Mirrors:

| Mirror | Notes |
|---|---|
| https://github.com/gitntel/Microsoft-Device-Emulator | Full extracted tree (`boards/`, `cpus/`, `include/`, `docs/`, `features/`), about 516 files, pushed 2021. This is the copy used for these notes. |
| https://github.com/dbgft/MicrosoftDeviceEmulatorforWinCE | Includes the original `SharedSourceDeviceEmulator10.zip` plus an extracted tree. Its description says "Relicensed as GPL", which is **not** legitimate: only Microsoft can relicense. |
| github.com/klaxons1/..., github.com/davie504/... | Show up in search results but return 404 now. |
| Original MS download | FamilyID `faa8c81d-7316-4461-a0ed-6c95b261ddcd` (dead). Barry Bond's blog post "Shared Source Device Emulator 1.0 has been released", 2006-07-17. |

**License.** The "Microsoft Shared Source Academic License for the Device Emulator, version 1.0" (`license.MD`/`license.rtf`) applies:

- It grants use **solely for academic purposes**: non-commercial teaching, research and personal experimentation *at an accredited educational institution*.
- Section 3(B), "Platform Limitation": derivatives may only run **on a Microsoft Windows OS product**, and the software may only be used to emulate Windows OS products.
- Source redistributions must carry the same license.

**Consequence for WMDroid:** an Android port that copies DE code would violate 3(B). Use this document's facts (register layouts and behaviour are not copyrightable) and write clean-room code. Do not paste DE source.

What is missing from the shared source: the Device Emulator Manager, the ActiveSync-over-DMA plugin (`SERDMAAsPlugin.dll`), the guest-side BSP, and all DE 2.0/3.0 changes (ARMv5 core, battery, LEDs, speakerphone, 128 MB NOR).

**Guest BSP source (PLATFORM\DEVICEEMULATOR).** I could not find a public mirror. It shipped as:

- the "Device Emulator BSP for Windows CE 5.0" download (FamilyID `474b03a6-e87d-455d-bc16-b8cf18ef39b4`, Feb 2006), and
- the CE 6.0 Platform Builder `PLATFORM\DEVICEEMULATOR` tree.

Its OEMAddressTable was recovered from the ROM images instead (section 2.3).

**No open-source DE-board emulator exists.** There is no QEMU fork, "SMDK2410 for WinCE" project, or similar that boots DE images. SkyEye has an S3C2410 model, but it is not DE-compatible: DE uses its own non-S3C devices.

Key source files:

| File | Contents |
|---|---|
| `boards/smdk2410/mappediodevices.h` | MMIO map |
| `boards/smdk2410/board.cpp` | RAM/flash, boot args, loader address mapping, command line |
| `boards/smdk2410/devices.{h,cpp}` | All S3C2410 peripheral models |
| `boards/loadbin_nb0.cpp` | .bin/.nb0 loader |
| `boards/cominterface.{h,cpp}` | DMA transport |
| `boards/FolderSharing.{h,cpp}` | Folder sharing |
| `boards/EmulServ.{h,cpp}` | EmulServ notification device |
| `boards/state.cpp`, `include/state.h` | `.dess` format |
| `cpus/arm/armcpu.cpp`, `cpus/arm/mmu.{h,cpp}` | CPU and CP15 |
| `docs/*.html` | Design notes: LCDDisplay, PeripheralDevices, Mmu, JIT |

---

## 1. CPU

### 1.1 Cores and instruction set

**V1 [SRC] (`cpus/arm/armcpu.cpp`)** emulates ARMv4T: ARM plus Thumb, with BX.

- Optional v5TE extensions are enabled by `/f <0..3>` (`Configuration.ProcessorFeatures`):
  - bit0 `Feature_LoadStoreDouble = 1`: LDRD/STRD.
  - bit1 `Feature_DSP = 2`: QADD/QSUB/QDADD/QDSUB and SMLA<x><y>/SMLAW/SMLAL<x><y>/SMUL...
- In V1, **CLZ, BLX (both forms), and cond=0xF instructions (BLX imm, LDC2/STC2/MCR2/MRC2/CDP2) raise Undefined.**
- PLD is decoded as a NOP.
- BKPT is supported (debugger).
- MCRR/MRRC raise Undefined.
- There is no VFP: CP10/11 raise Undefined in V1.
- A special SWI `0x123456` is a host-syscall hook, enabled only when `ProcessorConfig.ARM.GenerateSyscalls` is set (test config only).
- Abort model is set in `BoardPowerOn`:
  - `PCStoreOffset = 12`: an STR/STM of PC stores PC+12, as on ARM920T.
  - `BaseRestoredAbortModel = 1`.
  - `MemoryBeforeWritebackModel = 1`.

**DE 2.0/3.0 [DOC]** (Device Emulator Command-Line Reference, VS2008):

- `/cpucore ARMv4|ARMv5`, default ARMv4.
- `/cpuoptions` is a combination of T(humb), D(ebug), I(nterworking), M (long multiply), e (DSP). T, D and I are always set.
- `/vfp true|false`, default false.
- [GUESS] ARMv5 mode adds CLZ, BLX and the full v5TE set. The user's 6.5.3 launcher uses `/cpucore ARMv5`.
- **Recommendation:** implement ARMv5TE (ARM + Thumb, interworking LDR-to-PC, CLZ, BLX, QADD family, SMLAxy, LDRD/STRD, PLD as NOP). The WM5-6.5 images are ARMV4I builds, so ARMv4T is the minimum.
- No XScale DSP coprocessor (CP0 accumulator) or iWMMXt is emulated. CP0-CP14 are all `PlaceIllegalCoproc`, which raises Undefined.

### 1.2 CP15 (system control) [SRC] `cpus/arm/mmu.cpp`

The same constants are present in the DE 2.0 and 3.0 executables [BIN].

| CRn (opc2) | Read | Write |
|---|---|---|
| c0 (0) Main ID | **`0x69059201`**: implementer 0x69 'i' (Intel), arch 5 (v5TE), part 0x920, rev 1. Comment: "Intel ... ARM Architecture 5TE, ARM920 core", citing Intel doc 278796 Table 7-4 (XScale). | ignored |
| c0 (1) Cache type | **`0x0B172172`**: ctype 5, separate I/D; each cache 16 KB, 64-way, 32-byte lines (decoded with the ARMv5 format). | ignored |
| c0 (other) | Undefined | Undefined |
| c1 (0) Control | Stored value | See the sanitising rules below this table. |
| c1 (1) Aux control (XScale) | Stored value | Bits other than {0,1,4,5} must be 0 and bit1 (P) must be 1, else Undefined (sic). |
| c2 TTB | Stored value | Low 14 bits must be 0 **and the base must lie in guest RAM**, else Undefined. |
| c3 DACR | Stored value | **Only the value 1 is accepted** (domain 0 = client, all others no access). Anything else raises Undefined. The MMU then just checks PTE domain == 0. |
| c5 FSR / c6 FAR | Read/write | Read/write |
| c7 Cache ops | Undefined on read | See the cache-op list below this table. |
| c8 TLB | Undefined on read | opc2=0, CRm 7/5/6: flush I+D / I / D TLB. opc2=1, CRm 5/6: flush a single I/D entry by VA. |
| c9, c10, c11, c12, c14 | Undefined | Undefined |
| c13 Process ID (FCSE) | Stored value | Must have bits[24:0] = 0. |
| c15 Coprocessor access (XScale) | Stored value | Must have bits[31:14] = 0. In user mode, CP *n* is accessible only if bit *n* is set (checked only for code below VA 0x80000000). |

c1 (Control) write rules:

- Bit 10 must be 0, and bits[31:16] must be 0 or `0xC000xxxx` (iA/nF clocking bits, which the WM bootloader sets), else Undefined.
- The value is then forced to: C=1, W=1, P=1, D=1, L=1, Z=1, I=1, B=0, RR=0, L4=0.
- Honoured bits are M (MMU), A (alignment) and V (high vectors at 0xFFFF0000).
- When M goes 0→1, DE seeds an ITLB entry so the prefetched next instruction (`mov pc, r0`) still runs at its physical address, and flushes the JIT.

c7 cache operations:

- Instruction-cache invalidates (CRm=5 opc2 0/1) and unified invalidates (CRm=7 opc2 0/1) flush the JIT translation cache: the whole cache, or 32 bytes at the VA in Rd.
- CRm=5 opc2 2 (I-cache set/way) is fatal ("unsupported hardware").
- D-cache clean/invalidate, drain write buffer (opc1=0 CRm=10 opc2=4), BTB invalidate, prefetch and wait-for-interrupt (CRm=0 opc2=4, CRm=8) are all **no-ops**.
- Note that DE does not idle on WFI. See 1.4.

### 1.3 MMU [SRC] `cpus/arm/mmu.{h,cpp}`, `docs/Mmu.html`

- ARMv4/v5 two-level tables:
  - section (1 MB), coarse and fine L2,
  - large (64 KB), small (4 KB with four AP subpages) and tiny (1 KB) pages,
  - XScale extended small page with TEX.
- AP checks assume S=0, R=1.
  - User mode: AP=1 means no access; writes need AP=3.
  - Privileged mode: write needs AP≠0, and reads are always allowed.
- **FCSE:** if VA[31:25] == 0 (and the MMU is on), then VA |= ProcessId. WinCE (≤ 5.x/WM) relies on this heavily, so it is mandatory.
- 64-entry ITLB and 64-entry DTLB.
- **Real TLB caching semantics are required.** The CE kernel handles an LDM/STM that spans two pages by taking two aborts, and the second may evict the first PTE from the page table. The cached TLB entry is what keeps the first page usable. So the emulator must not re-walk tables on every access unless TLB flushes are honoured exactly.
- Fault status codes follow the ARM ARM (5/7 translation, 9/11 domain, 13/15 permission, 1 alignment). FAR holds the VA.
- When the MMU is off, the CPU addresses physical memory directly.

### 1.4 Reset and entry state, idle, power-down [SRC]

**Reset state** (`CpuRaiseResetException`, `BoardLoadImage`):

- SVC mode, ARM state, I=F=1.
- MMU control register = 0, ProcessId = 0.
- **PC = `InitialInstructionPointer`** (the image entry, see 2.2), *not* the reset vector at 0.
- **SP_svc = `0x30010000`** (an arbitrary RAM address).
- All other registers are 0.
- A hard reset (UI or COM `ResetVirtualMachine(true)`) goes through the same path.

**Interrupts:**

- IRQ goes to vector 0x18, or 0xFFFF0018 when V=1.
- FIQ is never raised: `INTMOD != 0` is fatal.

**Idle detection** (`armcpu.cpp` around line 6170). The JIT pattern-matches the OAL idle loop:

```
LDR Rd,[Rn,#imm] ; CMP Rd,... ; B<cond> back to the LDR
```

DE replaces the branch with `WaitForSingleObject(hIdleEvent)`, which is set when an interrupt becomes pending. Host CPU usage stays low without WFI.

**Suspend and power-off hook:**

- `BoardPowerOn` writes this code at **flash physical 0x1004**:
  - `E5801000 str r1,[r0]`
  - `E5823000 str r3,[r2]`
  - `E5845000 str r5,[r4]`
  - `EAFFFFFE b .`
- The guest's OEMPowerOff jumps there through flash VA `0x92001004`.
- When the JIT compiles the `b .` at **guest VA 0x92001010**, it emits "save state and exit" instead.
- The WM5/6.x images already contain the same four words at file offset 0x1004 [BIN].
- In the 6.5.3 image the word at 0x1010 is `EA000000`, not `EAFFFFFE`, and flash is at VA 0x88000000 rather than 0x92000000. DE 3.0 still contains the 0x92001010 constant [BIN]. So **how DE 3.0 detects power-off for 6.x images is unknown** [GUESS: an additional mechanism].
- **Save-state does not store CPU registers or CP15 state.**
  - A restore reloads RAM, flash and devices.
  - It sets PC to the entry point and SP to 0x30010000.
  - It forces GPIO GSTATUS2 bit1 (OFFRST) = 1, so the guest bootloader takes its **resume-from-sleep path**: it reloads DACR, TTB and control from a checksummed block in RAM and jumps to the saved resume address. The 6.5.3 bootloader at file offset 0x10B0-0x1174 does exactly this.
- Recommendation for WMDroid: implement your own full snapshot. You still need GSTATUS2 OFFRST semantics if you want DE-compatible resume.

---

## 2. Physical memory map and image loading

### 2.1 Physical map [SRC] `board.cpp` + `mappediodevices.h`; DE 2/3 [BIN]

The DE 2/3 rows come from the device table extracted from the 2.0/3.0 executables. Device ranges are inclusive of `base+len`.

| PA range | What it is |
|---|---|
| `0x00000000`-`0x01FFFFFF` (V1) / **`0x07FFFFFF` (DE 2.0/3.0)** | NOR flash "bank 0" (`IOAM29LV800BB`). Reads hit memory directly; writes are AMD command cycles (section 3.12). V1 holds 32 MB and DE 2/3 hold **128 MB**, which is why the 96 MB WM6.x NB0s fit. |
| `0x10000000`-`0x1000FFFF` | PCMCIA memory window (PD6710) |
| `0x10010000`-`0x110003DF` | PCMCIA I/O window |
| `0x110003E0`-`0x12000000` | PD6710 controller: index at +0, data at +1 |
| `0x18000000`-`0x1800015C` | CS8900A PacketPage memory |
| `0x19000300`-`0x1900030E` | CS8900A I/O ports |
| **`0x30000000`-`0x33FFFFFF`** | **SDRAM base, 64 MB, always present** |
| **`0x34000000`-`0x3FFFFFFF`** | **RAM extension: (`/memsize` − 64) MB, up to 192 MB** (`/memsize` range 64..256) |
| `0x48000000` | Memory controller (+0x34) |
| `0x49000000` | USB host (DEBUG builds only, non-functional) |
| `0x4A000000` | Interrupt controller (+0x1C) |
| `0x4B000040`, `0x4B000080` | DMA channels 1 and 2 (audio in/out) |
| `0x4C000000` | Clock and power (+0x18) |
| `0x4D000000` | LCD controller (+0x60) |
| `0x4E000000` | NAND controller (+0x14) |
| `0x50000000` / `0x50004000` / `0x50008000` | UART0 / UART1 / UART2 (+0x2C each) |
| **`0x500F0000`** | **DE DMA transport (+0x211C)** |
| **`0x500F4000`** | **DE folder sharing (+0x0C)** |
| **`0x500F5000`** | **DE EmulServ (+0x08)** |
| **`0x500FFFC0`** | **DE 2/3: IOSpeakerPhone (+0x01)** [BIN] |
| **`0x500FFFD0`** | **DE 2/3: IONotificationLEDs (+0x06)** [BIN] |
| **`0x500FFFF0`** | **DE 2/3: IOBattery (+0x03)** [BIN] |
| `0x51000000` | PWM timers (+0x40) |
| `0x52000140` | USB device (DEBUG builds only) |
| `0x53000000` | Watchdog (+0x0C) |
| `0x55000000` | IIS (+0x0C; FIFO at 0x55000010) |
| `0x56000000` | GPIO (+0xC0) |
| `0x57000040` | RTC (RTCCON..BCDYEAR, +0x4C) |
| `0x58000000` | ADC / touch (+0x10) |
| `0x59000020` | SPI1 = keyboard controller (+0x14) |

Not emulated: IIC (`0x54000000`), SDI (`0x5A000000`), SPI0, DMA channels 0 and 3, and the NOR command interface outside bank 0.

**Unmapped behaviour:** any other physical address is treated as I/O and goes to a "no device" handler that returns 0 on read and ignores writes, with a log line. Exception: accessing *known* devices at unknown offsets is mostly **fatal** (`TerminateWithMessage(ID_MESSAGE_UNSUPPORTED_HARDWARE)`). The guest therefore uses only the offsets listed here.

`/memsize` only allocates the extension range. In V1 nothing tells the guest the size, so accesses past the end hit the "no device" handler. [GUESS] The OAL probes RAM by write and readback. The image address tables map VA `0x94000000` → PA `0x34000000` for 192 MB.

### 2.2 Image formats and the loader [SRC] `boards/loadbin_nb0.cpp`, `board.cpp: BoardMapVAToPA`

**.bin (signature `"B000FF\n"`, 7 bytes):**

- Followed by `u32 ImageStart, u32 ImageLength`.
- Records start at file offset 15: `{u32 Address(VA), u32 Length, u32 Checksum, data[Length]}`.
- The last record has `Address == 0`; its **Length field holds the entry-point VA**. Checksums are not verified.
- DE first looks for an 8-byte record holding `"ECEC"` (`0x43454345`) plus a u32 pointer, then the 84-byte record at that pointer: the ROMHDR. It uses `ROMHDR.ulRAMStart` to pick a VA→PA mapping, and every record and the entry point go through it.

| Condition | Mapping | Images |
|---|---|---|
| `ulRAMStart >= 0x92000000` | `PA = VA − 0x92000000` (NOR flash image, sets Board64RamRegion) | — |
| `ulRAMStart >= 0x8C000000` | **CE 4.2 layout:** `PA = VA − 0x4E000000` (VA 0x80000000 → PA 0x32000000; RAM VA 0x8C000000 → PA 0x30000000) | WM2003SE ARMv4 `.bin`: ROM VA 0x80040000 (PA 0x32040000), entry 0x80041000 → PC 0x32041000 [BIN] |
| otherwise ("Bowmore" 64 MB layout) | `PA = VA − 0x50000000` (VA 0x80000000 → PA 0x30000000) | WM5 `PPC_USA.bin`: VA 0x80070000, entry 0x80076CF0 → PC 0x30076CF0 [BIN] |

**.nb0 (raw):**

- Accepted only if the first word has `(w & 0xEA000000) == 0xEA000000`, i.e. it starts with a branch.
- It is copied to **PA = `/r` address, default 0** (NOR flash), and **PC = that address**.
- The MSFP `pocketpc.nb0` (32 MB) and all WM6/6.1/6.5/6.5.3 "`.BIN`" files (96 MB, `0x6000000`, actually raw NB0 [BIN]) boot this way: copied to flash PA 0, PC = 0, MMU off.

**Multi-XIP NB0 structure [BIN].** Checked on WM5 MSFP, WM6, 6.1.4 and 6.5.3:

- Offset 0 holds `B 0x1000` and the `ECEC` header at +0x40 (ROMHDR VA at +0x44, file offset at +0x48).
- File offset 0 is a small XIP **bootloader** linked at VA 0x80021000 / PA 0x30021000. DE's constant `EBOOT_64DRAM_START = 0x30021000` refers to this.
- At 0x1000: `B 0x1014`, then the power-off stub at 0x1004-0x1010. From 0x1014 the code:
  1. invalidates TLB/I/D;
  2. sets WTCON=0, INTMSK=~0, INTSUBMSK, CLKDIVN=3;
  3. sets the iA/nF control bits;
  4. sets LOCKTIME/MPLL;
  5. **reads GSTATUS2 bit1 for wake-from-sleep**;
  6. programs the memory controller (13 words at 0x48000000);
  7. resumes, or
  8. on cold boot copies 256 KB from PA 0 to PA 0x30021000 and jumps there (it checks `PC & 0xFF000000` to see whether it already runs from RAM).
- The OS XIP region starts at file offset 0x30000:
  - WM5 MSFP: ROMHDR physfirst `0x92030000`, so flash VA base is 0x92000000.
  - WM6.x/6.5.3: physfirst `0x88030000`, so flash VA base is 0x88000000.
- WM6.5.3 OS ROMHDR: `ulRAMStart 0x80070000`, `ulRAMEnd 0x83EEF000`.

### 2.3 Guest OEMAddressTable (cached VA → PA, MB) [BIN]

Extracted from the images. Uncached aliases are at VA + 0x20000000.

| VA | WM2003SE (CE 4.2) | WM5 PPC .bin / WM5 MSFP .nb0 | WM6 / 6.1.4 / 6.5.3 |
|---|---|---|---|
| 0x80000000 | PA 0x32000000, 32 | **PA 0x30000000, 64** | **PA 0x30000000, 64** |
| 0x82000000 | PA 0x08000000, 32 | — | — |
| 0x84000000 | PA 0x10000000, 32 (PCMCIA) | same | same |
| 0x86000000 | PA 0x18000000, 32 (CS8900) | same | same |
| 0x88000000 | PA 0x20000000, 32 | PA 0x20000000, 32 | **PA 0x00000000, 96 (NOR)** |
| 0x8A000000 | PA 0x28000000, 32 | PA 0x28000000, 32 | — |
| 0x8C000000 | PA 0x30000000, 32 | PA 0x08000000, 32 | — |
| 0x90800000-0x91A00000 | S3C2410 SFR banks 0x48000000-0x5A000000, 1 MB each | same | same |
| 0x92000000 | PA 0x00000000, 32 (NOR) | PA 0x00000000, 32 (NOR) | — |
| 0x94000000 | PA 0x34000000, 192 (memsize extension) | same | same |

The table sits at image offset 0x11C8 in NB0 images. For 6.5.3 the SFR entries are 0x90800000→0x48000000 ... 0x91A00000→0x5A000000.

DE V1 implements nothing at PA 0x08000000, 0x20000000 or 0x28000000. [GUESS] These are SMDK2410 nGCS1/4/5 leftovers that the guest maps but never touches.

### 2.4 Boot arguments DE writes into RAM [SRC] `board.cpp: BoardWriteGuestArguments`

These are written after the image is loaded and the devices are powered on, and again on every reset.

**(a) CE 5.x+/WM5+ `BSP_ARGS` at PA `0x30020000`.** The guest reads it through `IMAGE_SHARE_ARGS_UA_START`: 0xAC020000 in the CE5 BSP, or [BIN] 0xA0020000/0x80020000 with the 64 MB map.

DE writes only these fields. Offsets are computed from the struct with natural alignment and 4-byte pointers:

| Off | Field | Value |
|---|---|---|
| 0x00 | `OAL_ARGS_HEADER {u32 signature 'ARGS' (0x53475241), u16 oalVersion, u16 bspVersion}` | not written by DE |
| 0x08 | `u8 deviceId[16]` | — |
| 0x18 | `OAL_KITL_ARGS.flags` | DE ORs `OAL_KITL_FLAGS_PASSIVE` (0x2) when `/AllowPassiveKitl` is set |
| 0x1C | `DEVICE_LOCATION` (5 × u32) | — |
| 0x30 | union: serial {baud, data, stop, parity} / ether {u16 mac[3]; u32 ip, mask, route} (20 bytes) | — |
| **0x44** | `u32 ScreenSignature` | **`0xDE12DE34`** |
| **0x48** | `u16 ScreenWidth` | from `/video` or the skin; default 240 |
| **0x4A** | `u16 ScreenHeight` | default 320 |
| **0x4C** | `u16 ScreenBitsPerPixel` | 16, 24 or 32 |
| **0x4E** | `u16 EmulatorFlags` | 1 = soft reset, 0 = cold boot |
| **0x50** | `u8 ScreenOrientation` | `/rotate` angle / 90 (0..3) |
| 0x51 | `u8 Pad[15]` | — |

`BSP_ARGS_VERSION = 256`. The WM6.5.3 image contains `0xDE12DE34` [BIN].

V1 validation, which DE 3.0 still enforces per the user's notes: width and height 64..800 and even; depth 16/24/32; `W*H*(bpp/8) <= 1 MB`. The 1 MB frame-buffer limit comes from config.bib.

**(b) CE 4.2 `BOOT_ARGS` at PA `0x301FF000`** (top of the 1 MB frame-buffer reservation at 0x30100000).

- Written only when the image is not in the 64 MB layout and the entry is not 0x30021000, i.e. WM2003SE.
- The struct is the CEPC/SMDK2410 `bootarg.h` layout. Computed offsets:
  - `dwSig` at 0x04 = `0x544F4F42` ("BOOT")
  - `cxPhysicalScreen` at 0x72
  - `cyPhysicalScreen` at 0x74
  - `bppScreen` at 0x78
  - `softReset` (u8) at 0x80
- For the CE 4.2 BSP the frame buffer is at PA **0x30100000** (from `platform.reg` LCDPhysicalFrameBase).

### 2.5 Memory sizes summary

| Item | Value |
|---|---|
| RAM | 64 MB at 0x30000000 plus 0-192 MB extension at 0x34000000 |
| NOR | 32 MB (V1) or 128 MB (DE 2/3) at 0 |
| NAND | Optional (`/flash`): Samsung 64 MB-class, 4096 blocks × 32 pages × (512 + 8) bytes |
| Default initial SP | 0x30010000 |

---

## 3. S3C2410 peripherals as modelled by DE [SRC] `boards/smdk2410/devices.{h,cpp}`

General rules:

- Most devices are word-access only. Byte or half access to word-only devices is fatal.
- Writes to read-only registers are ignored.
- Unknown offsets are fatal.
- All device access happens under one global I/O lock.

### 3.1 Interrupt controller `0x4A000000`

Registers (word access):

| Offset | Register | Behaviour |
|---|---|---|
| 0x00 | SRCPND | Write 1 to clear |
| 0x04 | INTMOD | **Must be 0** |
| 0x08 | INTMSK | Reset 0xFFFFFFFF |
| 0x0C | PRIORITY | Reads 0x7F; writes are fatal |
| 0x10 | INTPND | Write 1 to clear |
| 0x14 | INTOFFSET | — |
| 0x18 | SUBSRCPND | Write 1 to clear |
| 0x1C | INTSUBMSK | Reset 0x7FF |

Standard S3C2410 bit numbers:

| Bit | Source | Bit | Source |
|---|---|---|---|
| 0 | EINT0 | 16 | LCD |
| 1 | EINT1 | 17 | DMA0 |
| 2 | EINT2 | 18 | DMA1 |
| 3 | EINT3 | 19 | DMA2 |
| 4 | EINT4_7 | 20 | DMA3 |
| 5 | EINT8_23 | 21 | SDI |
| 7 | nBATT_FLT | 22 | SPI0 |
| 8 | TICK | 23 | UART1 |
| 9 | WDT | 25 | USBD |
| 10 | TIMER0 | 26 | USBH |
| 11 | TIMER1 | 27 | IIC |
| 12 | TIMER2 | 28 | UART0 |
| 13 | TIMER3 | 29 | SPI1 |
| 14 | TIMER4 | 30 | RTC |
| 15 | UART2 | 31 | ADC |

SUBSRCPND bits:

| Bit | Source | Bit | Source |
|---|---|---|---|
| 0 | RXD0 | 6 | RXD2 |
| 1 | TXD0 | 7 | TXD2 |
| 2 | ERR0 | 8 | ERR2 |
| 3 | RXD1 | 9 | TC (pen up/down) |
| 4 | TXD1 | 10 | ADC |
| 5 | ERR1 | | |

Sub-sources roll up into the main register:

- bits 0-2 → UART0 (28)
- bits 3-5 → UART1 (23)
- bits 6-8 → UART2 (15)
- TC/ADC → ADC (31)

The arbiter is implemented exactly as in S3C2410 manual figure 14-1, with rotation (`ArbiterPriorities = {0x543210, 0x514320, 0x521430, 0x532140}`). Only one INTPND bit is set at a time. INTOFFSET is the bit number. The CPU IRQ line is asserted while INTPND ≠ 0.

When INTPND is cleared to 0, DE re-ORs level-triggered sources into SRCPND:

- GPIO EINT lines set to level mode,
- PWM timers 2/3/4 with AutoReload = 0 whose interrupt has not been serviced, and
- UART RX data-ready (into SUBSRCPND).

A write to SRCPND re-asserts EINT1 if the keyboard queue is non-empty.

**DE interrupt assignments** (GPIO comment table plus device code):

| Line | Use |
|---|---|
| EINT0 | Power button (GPF0; DE also clears GPFDAT bit0 while pressed) |
| EINT1 | Keyboard (SPI1) |
| EINT2 | Reset button (DE soft reset) |
| EINT3 | PCMCIA card-status change |
| EINT8 | PCMCIA card IRQ (level) |
| EINT9 | CS8900 |
| **EINT10** | **DMA transport** (SYSINTR_DMATRANS) |
| **EINT11** | **EmulServ** |
| TIMER4 | OS tick |
| TIMER3 | Touch-panel sampling |
| TIMER2 | Implemented, used by the guest [GUESS: profiler or audio] |
| DMA1 / DMA2 | Audio in / out |
| ADC (TC sub-interrupt) | Pen down/up |
| UART0/1/2 | Serial |

### 3.2 PWM timers `0x51000000`

Registers:

| Offset | Register | Offset | Register |
|---|---|---|---|
| 0x00 | TCFG0 | 0x24 | TCNTB2 |
| 0x04 | TCFG1 | 0x28 | TCMPB2 |
| 0x08 | TCON | 0x2C | TCNTO2 |
| 0x0C | TCNTB0 | 0x30 | TCNTB3 |
| 0x10 | TCMPB0 | 0x34 | TCMPB3 |
| 0x14 | TCNTO0 (read is fatal) | 0x38 | TCNTO3 |
| 0x18 | TCNTB1 | 0x3C | TCNTB4 |
| 0x1C | TCMPB1 | 0x40 | TCNTO4 |
| 0x20 | TCNTO1 (read is fatal) | | |

- **Only timers 2, 3 and 4 run.** Starting timer 0 or 1 is fatal.
- A timer starts on a 0→1 edge of its TCON start bit and loads TCNTBx then. The manual-update bit is not needed.
- On expiry DE raises INT_TIMERx. If auto-reload is set it re-arms with the same period.
- TCNTOx is computed from host elapsed time. It latches 0 while an auto-reload timer's interrupt is pending, so the count never appears to go backwards.
- **Clock model:** `tick_rate = 203 MHz / 4 / prescaler / divider`.
  - That is PCLK = 50.75 MHz.
  - Prescaler1 (TCFG0[15:8]) is used *as is*, not +1; a prescaler of 0 counts as 1.
  - The divider comes from MUXx: 0 → /2, 1 → /4, 2 → /8, 3 → /16.
- **OS tick:** Timer4, prescaler1 = 200, MUX4 = 1 (/4) gives 63.4375 ticks/ms. The OAL writes TCNTB4 = 63 (or 15 with a different divider) for a 1 ms tick. `OEM_COUNT_1MS = 63`.
- A due time of exactly 1 ms is shortened to 0.975 ms to dodge a Windows timer quirk.

### 3.3 UARTs `0x50000000` / `0x50004000` / `0x50008000`

Standard S3C2410 offsets:

| Offset | Register | Offset | Register |
|---|---|---|---|
| 0x00 | ULCON | 0x18 | UFSTAT |
| 0x04 | UCON | 0x1C | UMSTAT |
| 0x08 | UFCON | 0x20 | UTXH |
| 0x0C | UMCON | 0x24 | URXH |
| 0x10 | UTRSTAT | 0x28 | UBRDIV |
| 0x14 | UERSTAT | | |

Mapping:

- **UART0 = guest COM1** (`/u0`).
- **UART1 = debug serial** (`/u1`). With no host port it goes to the emulator console: TX to stdout, RX from the console when `/c` is used.
- **UART2 = guest COM3** (`/u2`). [DOC] The GPS post says Serial Port 0 = COM1.

Behaviour:

- TX completes instantly: UTRSTAT always reports TX empty and UFSTAT reports the TX FIFO empty.
- The TX sub-interrupt is raised after each host write completes, and once at power-on.
- RX uses an 8 KB queue. UFSTAT RX count saturates at 15 and sets the full bit. UTRSTAT bit0 = data ready, which acts as a **level** RXD sub-interrupt.
- Reading UERSTAT clears it.
- UMSTAT reports CTS/DSR. UART0 DTR is driven by GPH6 and DSR is read back on GPH7 (active-low).
- Baud: `UBRDIV ≈ 3145728/baud − 1`. The table includes 326 = 9600, 162 = 19200, 81 = 38400, 53 = 57600 and 26 = 115200; nearest match wins.

### 3.4 LCD controller `0x4D000000`

DE uses **the real S3C2410 LCD registers, with arbitrary geometry**. There is no separate display device.

Register offsets:

| Offset | Register | Offset | Register |
|---|---|---|---|
| 0x00 | LCDCON1 | 0x28 | BLUEVAL |
| 0x04 | LCDCON2 | 0x2C | DE rotation request (see below) |
| 0x08 | LCDCON3 | 0x30-0x48 | Test mode: fatal |
| 0x0C | LCDCON4 | 0x4C | DITHMODE |
| 0x10 | LCDCON5 | 0x50 | TPAL |
| 0x14 | LCDSADDR1 | 0x54 | LCDINTPND |
| 0x18 | LCDSADDR2 | 0x58 | LCDSRCPND |
| 0x1C | LCDSADDR3 | 0x5C | LCDINTMSK (reset 3) |
| 0x20 | REDVAL | 0x60 | LPCSEL (reset 4) |
| 0x24 | GREENVAL | | |

How DE uses the registers:

- **Width = LCDCON3.HOZVAL (bits 18:8) + 1. Height = LCDCON2.LINEVAL (bits 23:14) + 1.**
- **Bits per pixel from LCDCON1.BPPMODE (bits 4:1):** 12 → 16 bpp **RGB565**, 13 → 24 bpp, 14 → 32 bpp. Anything else is forced back to 16.
  - 24 and 32 bpp are blitted as Windows BI_RGB, i.e. little-endian B, G, R(, X) bytes.
- **Frame-buffer PA = (LCDSADDR1.LCDBASEU[20:0] << 1) | (LCDSADDR1.LCDBANK[29:21] << 22).**
  - Pixels are copied **linearly with stride = width × Bpp**. PAGEWIDTH and OFFSIZE are ignored; OFFSIZE is asserted 0.
  - The frame buffer must be in RAM. Otherwise DE draws a flicker pattern.
- LCDCON1.ENVID (bit0) shows or hides the window.
- **DE extension:** writing 0..3 to offset **0x2C** (a test-mode register on real hardware) requests a screen rotation of value × 90°.
- Geometry may not change while ENVID = 1 (fatal). WinCE never reconfigures at runtime.
- No LCD interrupts are generated. The host polls the frame buffer about 12 times per second (V1 sleeps 82 ms) and repaints only when something changed.

**How the guest picks the resolution.** The OAL's `InitDisplay()` reads `BSP_ARGS` (or the CE 4.2 `BOOT_ARGS`) and programs LCDCON1-3 and LCDSADDR. The display driver reads the size back from the LCD registers. The emulator only has to honour the registers. This is how 480x800 works, and why the limit is ≤ 800 and ≤ 1 MB of frame buffer.

### 3.5 ADC / touchscreen `0x58000000`

Registers: ADCCON 0x00 (reset 0x3FC4), ADCTSC 0x04 (reset 0x58), ADCDLY 0x08 (reset 0xFF), ADCDAT0 0x0C, ADCDAT1 0x10.

Pen events:

1. Pen down or up sets ADCDAT0/1.UPDOWN (bit15: 0 = down) and XY_PST (copied from ADCTSC).
2. DE then raises the **TC sub-interrupt** (SUBSRCPND bit9, which rolls up to INT_ADC).

Conversions:

- Writing ADCCON with ENABLE_START = 1 completes the conversion immediately: ECFLG (bit15) = 1, ENABLE_START cleared.
- **ADCDAT0.XPDATA receives the *Y* sample and ADCDAT1.YPDATA receives the *X* sample.** This swap is intentional.
- Sample scaling (empirical, CE 4.2 calibration), computed only while pen down and XY_PST ≠ 0:
  - `SampleX = 90 + x*875/ScreenWidth`
  - `SampleY = 920 − y*870/ScreenHeight`
- No ADC interrupt is raised for conversions. The guest driver polls, sampling on Timer3 interrupts.

### 3.6 Keyboard: SPI1 `0x59000020`

The SMDK2410 keyboard sits behind a microcontroller on SPI1. Registers:

| Offset | Register | Notes |
|---|---|---|
| 0x00 | SPCON | Keyboard enabled only when SMOD (bits 6:5) = 01 (interrupt mode); otherwise keys are dropped |
| 0x04 | SPSTA | Reset 1; REDY |
| 0x08 | SPPIN | Reset 2 |
| 0x0C | SPPRE | — |
| 0x10 | SPTDAT | Writing sets REDY |
| 0x14 | SPRDAT | — |

Protocol:

- A 16-entry queue of 8-bit codes. Each enqueue raises **EINT1** (SRCPND bit1 directly).
- The guest writes `0xFF` to SPTDAT, then reads SPRDAT to pop one code. If more codes remain, EINT1 is raised again. An empty queue reads 0.
- Code format: **Samsung matrix scancode | 0x80 for key-up**. The table is `VKtoSCTable[256]` in `boards/smdk2410/vktoscan.c`, generated by inverting the CE driver's scancode→VK table. Examples:

| VK | Code | VK | Code |
|---|---|---|---|
| VK_BACK | 0x69 | 'A' | 0x0D |
| VK_RETURN | 0x5A | '0' | 0x49 |
| VK_SPACE | 0x6E | '1' | 0x31 |
| VK_LEFT | 0x6D | VK_LSHIFT (0xA0) | 0x12 |
| VK_UP | 0x6C | VK_RSHIFT (0xA1) | 0x62 |
| VK_RIGHT | 0x6F | VK_LCONTROL | 0x19 |
| VK_DOWN | 0x6A | VK_LMENU | 0x01 |
| VK_ESCAPE | 0x29 | VK 0xC0-0xC5 (WinCE app/soft keys) | 0x63..0x68 |
| VK_TAB | 0x0B | | |

- Keys with no direct code are sent as `Fn` (VK 0xC0, or `/funckey`) + key + `Fn` up, using `g_rgvkMapFn`. Examples: F1-F10 → Fn+'1'..'0', Home → Fn+Left. NumLock-dependent keypad keys use `g_rgvkMapNumLock`.
- **Skin buttons** carry IBM AT set-1 scancodes. DE maps them through `ScanCodeToVKeyTable` (`boards/scancodemapping.cpp`, 0x00-0x76) to a VK, then through `VKtoSCTable` as above.
- VK 0xC6 (EMUL_RESET) triggers a soft reset.
- Soft keys use `/funckey 193`.

### 3.7 RTC `0x57000040`

Offsets are relative to 0x57000040:

| Offset | Register |
|---|---|
| 0x00 | RTCCON |
| 0x10-0x2C | Alarm registers and RTCRST: ignored, so no alarm interrupt ever fires |
| 0x30 | BCDSEC |
| 0x34 | BCDMIN |
| 0x38 | BCDHOUR |
| 0x3C | BCDDATE |
| 0x40 | BCDDAY |
| 0x44 | BCDMON |
| 0x48 | BCDYEAR (year − 2000) |

- Reads return host local time plus a saved offset.
- Writes to the BCD registers are latched. When RTCCON goes from non-zero to 0, the new offset is computed.
- TICNT (0x57000044) is **not** implemented; it falls at offset 4, which is fatal.

### 3.8 GPIO `0x56000000`

- Ports A-H: CON, DAT and UP registers at the standard offsets (0x00-0x78).
- Other registers: MISCCR 0x80, DCLKCON 0x84, EXTINT0/1/2 0x88/0x8C/0x90, EINTFLT2/3 0x9C/0xA0, **EINTMASK 0xA4**, **EINTPEND 0xA8** (write 1 to clear), **GSTATUS2 0xB4**, **GSTATUS3 0xB8**.
- EINT4-23 trigger type comes from EXTINTn (3 bits per line):
  - 1 = high level (re-asserted after INTPND is cleared),
  - 4-7 = rising/both edges,
  - 0 and 2-3 = mostly ignored.
- Pending lines are routed to EINT4_7 or EINT8_23.
- **GSTATUS2 bit1 (OFFRST)** is set on save-state restore (resume) and cleared on hard reset. The bootloader branches on it.
- Power button: GPFDAT bit0 reads 0 while "pressed" (released after two GPFDAT reads). EINT0 is raised only if GPFCON[1:0] = 2.
- The UDA1341 audio codec L3 bus on GPB is not modelled. DE has **no codec emulation**.

### 3.9 IIS audio `0x55000000` and DMA `0x4B000040` / `0x4B000080`

**IIS** (word access): IISCON 0x00 (reads always OR in TX/RX FIFO ready, 0x80/0x40), IISMOD 0x04, IISPSR 0x08, IISFCON 0x0C, IISFIFO 0x10. These are only stored.

**Audio format:** fixed host format, **44.1 kHz, 16-bit, stereo PCM**, regardless of IISMOD/IISPSR.

**DMA2 = playback.** On DMASKTRIG (0x20) ON_OFF, DE requires all of the following, otherwise it is fatal:

- DISRC in RAM, DISRCC = 0 (increment, AHB);
- DIDST = 0x55000010, DIDSTC = 3 (fixed, APB);
- DCON: SERVMODE = 0, HWSRCSEL = 0 (IIS), SWHW_SEL = 1, RELOAD = 0, DSZ = 1 (halfword), TC > 0.

Then:

- Buffer bytes = TC × 2.
- DE queues it to waveOut and raises **INT_DMA2** when the host finishes the block.
- A new DISRC written while ON queues the next buffer (double-buffering).

**DMA1 = record.** The mirror image of DMA2: DISRC = 0x55000010, DISRCC = 3, HWSRCSEL = 2, destination in RAM. DE raises **INT_DMA1**.

**DMA register layout** per channel: DISRC 0, DISRCC 4, DIDST 8, DIDSTC 0xC, DCON 0x10, DSTAT 0x14 (= TC), DCSRC 0x18, DCDST 0x1C, DMASKTRIG 0x20.

### 3.10 Clock/power `0x4C000000`, memory controller `0x48000000`, watchdog `0x53000000`

- **Clock/power:** LOCKTIME 0, MPLLCON 4, UPLLCON 8, CLKCON 0xC, CLKSLOW 0x10, CLKDIVN 0x14. Stored only. Setting CLKCON.PowerOff does nothing; the power-off path is the code stub in 1.4.
- **Memory controller:** BWSCON 0, BANKCON0-7 0x04-0x20, REFRESH 0x24, BANKSIZE 0x28, MRSRB6/7 0x2C/0x30. Stored only.
- **Watchdog:** writing WTCON with `(v & 0x21) == 0x21` triggers a **soft reset** (EINT2 plus `EmulatorFlags = 1`). WTDAT and WTCNT are ignored. This is how the OAL's reboot IOCTL works.

### 3.11 NAND `0x4E000000` (only with `/flash`)

Registers: NFCONF 0, NFCMD 4 (byte), NFADDR 8 (byte), NFDATA 0xC (byte), NFSTAT 0x10 (bit0 is always ready).

Commands:

| Code | Command | Behaviour |
|---|---|---|
| 0xFF | Reset | — |
| 0x90 | Read ID | Returns 0xEC (Samsung), then 0x76 |
| 0x00 | Read | — |
| 0x50 | Read spare | Starts at byte 512 |
| 0x60 / 0xD0 | Erase | V1 erases only *one 520-byte page* at the row address; the guest FMD tolerates this |
| 0x80 / 0x10 | Program | Reading after 0x10/0xD0 returns 0 (success) |

- Address bytes shift in from the top: `NFADDR = (NFADDR >> 8) | (b << 24)`. Page index = NFADDR >> 8.
- Geometry: 4096 blocks × 32 pages × 520 bytes. Page info (8 bytes) sits at offset 512; 0xFFFFFFFF means good and unused.
- A pre-formatted image is embedded as resource `formatted_flash.bin`.
- The `/flash` file format: `u32 0` (version/type), `u32 blocks` (4096), `u32 bytesPerBlock` (16640), `u32 (bytesPerPage << 16) | pagesPerBlock`, then an LZ block (section 5).

### 3.12 NOR flash command interface (bank 0) — `IOAM29LV800BB`

AMD-style 16-bit command cycles, in byte addresses:

- Unlock: `0xAAAA` written at `0xAAAA` (0x5555<<1), then `0x5555` at `0x5554` (0x2AAA<<1).
- Command at 0xAAAA:
  - 0x9090 autoselect: word 0 reads `0x225B0001` (manufacturer 0x01, device 0x225B);
  - 0xA0A0 program the next halfword;
  - 0x8080 erase: unlock again, then 0x3030 at the sector address erases it (sector sizes 16K/8K/8K/32K, then 64K);
  - 0xF0F0 reset.
- With `/n`, DE also writes an EBOOT config block at flash 0xF0000 (signature 0x12345678) containing the MAC at 0xF0016.

### 3.13 CS8900A Ethernet `0x19000300` (I/O) and `0x18000000` (PacketPage memory)

I/O ports:

| Offset | Port | Offset | Port |
|---|---|---|---|
| 0x0 | RX/TX data 0 | 0x8 | ISQ (0x0304 = RX OK) |
| 0x2 | data 1 | 0xA | PacketPage pointer |
| 0x4 | TxCMD | 0xC | PacketPage data 0 |
| 0x6 | TxLength | 0xE | PacketPage data 1 |

PacketPage registers DE implements:

| PP offset | Register | Behaviour |
|---|---|---|
| 0x0000 | EISA ID | 0x630E |
| 0x0002 | Product ID | 0 |
| 0x0022 | Interrupt number | must be 0 |
| 0x002C / 0x002E | Memory base | — |
| 0x0102 / 0x0106 | RxCFG / TxCFG | ignored |
| 0x0104 | RxCTL | — |
| 0x0112 | LineCTL | — |
| 0x0114 | SelfCTL | ignored |
| 0x0116 | BusCTL | bit 0x8000 = IRQ enable |
| 0x0136 | SelfST | 0x80 |
| 0x0138 | BusST | 0x100 = ready for TX |
| 0x0150-0x0156 | Multicast hash | — |
| 0x0158-0x015C | Individual address (MAC) | — |

- Interrupt line **EINT9**.
- Received frames are read from data port 0 as `{u16 status, u16 length, data...}`.
- The host side uses the Virtual PC network driver, VMNetSrv.

### 3.14 PCMCIA (Cirrus PD6710) and NE2000

- Controller at 0x110003E0: index register at +0, data at +1. Chip revision reads 0x83.
- Card I/O windows map to the I/O range at 0x10010000.
- The NE2000 card (`/p`) raises **EINT8**.

---

## 4. DE-specific virtual devices

### 4.1 DMA transport `0x500F0000` (ActiveSync, debugging, custom host↔guest channels) [SRC] `boards/cominterface.{h,cpp}`

Register map:

| Offset | Register |
|---|---|
| 0x0000-0x0FFF | InputBuffer: host → guest packet |
| 0x1000-0x1FFF | OutputBuffer: guest → host packet |
| 0x2000-0x200C | Flags[0..3], one per physical channel. Reads return `(Flags & ~1) \| channelHasData` |
| 0x2010 | InputDataLength |
| 0x2014 | InputDataPtr |
| 0x2018 | InputStatus |
| 0x201C | OutputDataLength |
| 0x2020 | OutputDataPtr |
| 0x2024 | OutputStatus |
| 0x2080 + 0x20·n (n = 0..3) | Per-channel block (see below) |
| 0x2100 | CurrentVirtualChannel |
| 0x2104 | VirtualOperation |
| 0x2108 | VirtualStatus (read only) |
| 0x210C | VirtualChannelIndexF; reads return "channel has data or address-connected" |
| 0x2110 | VirtualChannelIndexR |
| 0x2114 | VirtualChannelIndexW |

Per-channel block at 0x2080 + 0x20·n:

| +Offset | Register | Behaviour |
|---|---|---|
| +0x00 | GlobalRegister | The guest writes 1 to probe the device, then **0x101 (bit 8) to enable interrupts** |
| +0x04 | FlagsRegister | Guest PA of its Flags word |
| +0x08 | IOInputRegister | Any write pulls one queued host packet into InputBuffer: InputDataLength = bytes, InputStatus = 0 (or 0xFFFFFFFF if none). Then re-raises the IRQ if more packets are queued |
| +0x0C | IOOutputRegister | Any write pushes OutputBuffer[0..OutputDataLength) to the host. OutputStatus: 0 ok, 0xFFFFFFFE queue full, 0xFFFFFFFD virtual channel disconnected, 0xFFFFFFFF closed |
| +0x10 | IRQRegister | — |
| +0x14 | IRQAcknowledgeRegister | A write lowers EINT10 |

VirtualOperation values:

| Value | Operation |
|---|---|
| 1 | DeviceAttach(CurrentVirtualChannel) |
| 2 | DeviceDetach |
| 4 | IsChannelInUse |
| 8 | NewVirtualChannel; VirtualStatus = the new index |
| 16 | NewAddressChannel |

Other details:

- Interrupt: **EINT10**, raised when a host packet arrives and GlobalRegister bit 8 is set (channel 0's register is used for virtual channels).
- Packets are at most 4096 bytes. Each direction has a 64-entry queue; when full, the oldest packet is dropped (non-virtual channels).
- Host-side channel numbers are 4..7, mapping to physical 0..3. Virtual channels are ≥ 0x80000000: `NEW_VIRTUAL_CHANNEL` = 0x80000000 and `NEW_ADDR_CHANNEL` = 0x80000001, and 0x80000001-0x80000FFF is reserved.
- The "address service" lets the guest publish named channels; ActiveSync uses this.
- The ActiveSync-over-DMA pieces (guest `DMATrans` driver, host `SERDMAAsPlugin.dll`) are **not** in the source. [GUESS] For WMDroid, expose these channels to a host-side bridge.

### 4.2 Folder sharing `0x500F4000` (`\Storage Card`) [SRC] `boards/FolderSharing.{h,cpp}`

Registers (word access):

| Offset | Register | Behaviour |
|---|---|---|
| 0x0 | ServerPBAddr | **Guest PA** of the parameter block |
| 0x4 | Code | Writing starts operation `Code` |
| 0x8 | IOPending | Read only: 1 while busy |
| 0xC | Result | Error code or return value |

Protocol:

1. The guest fills `ServerPB` and writes `Code`.
2. For asynchronous calls (5-0x13) it repeatedly writes `Code = 0` (PollCompletion). DE blocks up to 1 ms or until an IRQ is pending.
3. When `IOPending == 0`, the guest reads `Result`. The out-parameters are written into ServerPB.

If sharing is disabled, any Code returns Result = 1.

Function codes:

| Code | Function | Code | Function |
|---|---|---|---|
| 0x00 | PollCompletion | 0x0C | MkDir |
| 0x04 | GetDriveConfig (0 = enabled, 0x1F = disabled) | 0x0D | RmDir |
| 0x05 | Create | 0x0E | SetAttributes |
| 0x06 | Open | 0x0F | Rename (fName → fName2) |
| 0x07 | Read | 0x10 | Delete |
| 0x08 | Write | 0x11 | GetInfo (FindFirst/FindNext/exists) |
| 0x09 | SetEOF | 0x13 | GetFCBInfo |
| 0x0A | Close | 0x15 | GetMaxIOSize (returns 0x10000 in Result) |
| 0x0B | GetSpace (fSize = free/32K, fPosition = total/32K) | | |

Codes 1-3, 0x12 and 0x14 are unused.

GetInfo semantics by `fIndex`: 0 = FindFirst using fName (may contain wildcards); > 0 = FindNext within `fFindTransactionID`; < 0 = existence check.

Error codes are DOS-style: 0 OK, 1 invalid function, 2 file not found, 3 path not found, 4 too many open files, 5 access denied, 6 invalid handle, 0x11, 0x12 no more files, 0x13, 0x1D, 0x1E, 0x1F general failure, 0x20 sharing violation, 0x21, 0x24, 0x27 disk full, 0x50 file exists, 0x7B invalid name.

Other details:

- Up to 40 open handles; the invalid handle is 0xFFFF.
- Open modes: access 0/1/2 (R/W/RW) | share 0x10 deny RW, 0x20 deny W, 0x30 deny R, 0x40 deny none.
- Times are DOS date/time packed as `(date << 16) | time`, local time.
- Attributes are the Win32 bits RO, HIDDEN, SYSTEM, DIRECTORY and ARCHIVE.
- Paths are UTF-16, relative to the share root, and DE rejects any path that escapes it.

`ServerPB` layout (`sizeof` must equal `fStructureSize` or DE aborts). Offsets are computed with natural alignment and a 4-byte pointer:

| Off | Type | Field |
|---|---|---|
| 0x000 | u16 | fStructureSize (= 0x42C, i.e. 1068) |
| 0x002 | u16 | fResult |
| 0x004 | u32 | fFindTransactionID |
| 0x008 | s16 | fIndex |
| 0x00A | u16 | fHandle |
| 0x00C | u32 | fFileTimeDate |
| 0x010 | u32 | fSize |
| 0x014 | u32 | fPosition |
| 0x018 | u32 | fDTAPtr (**guest PA** of the data buffer) |
| 0x01C | u16 | fFileAttributes |
| 0x01E | u16 | fOpenMode |
| 0x020 | u8 | fWildCard |
| 0x022 | u16 | fNameLength (bytes, excluding NUL) |
| 0x024 | wchar[256] | fName |
| 0x224 | u16 | fName2Length |
| 0x226 | wchar[256] | fName2 |
| 0x428 | u32 | fFileCreateTimeDate |

When sharing is enabled or disabled (including at cold boot), DE raises **EmulServ bit `0x40000000`** (`EmulServ_FolderSharingChanged`). The guest driver (`vcefsd`) then mounts or unmounts `\Storage Card`. The 2008 docs call it "\Storage File", which is wrong.

### 4.3 EmulServ `0x500F5000` [SRC] `boards/EmulServ.{h,cpp}`

| Offset | Register | Behaviour |
|---|---|---|
| 0x0 | InterruptMask | Read/write. The guest writes 0xFFFFFFFF to enable |
| 0x4 | InterruptPending | Reading returns the pending bits, clears them and lowers the IRQ |

- Interrupt: **EINT11** (`SYSINTR_EMULSERV`).
- The only bit defined in V1 is `0x40000000` = folder sharing changed. [GUESS] DE 2/3 may add bits, for example for rotation.

### 4.4 DE 2.0/3.0 additions: battery, notification LED, speakerphone [BIN]

The new devices (`IOBattery`, `IONotificationLEDs`, `IOSpeakerPhone`) appear only in the DE 2.0 and 3.0 executables' MMIO tables. The constants also appear in the WM6.5.3 image.

| Device | PA | Size | Known behaviour |
|---|---|---|---|
| IOBattery | 0x500FFFF0 | 4 bytes (0x500FFFF0-0x500FFFF3) | `/battery` (AC vs battery) and `/batterycharge 0-100`. [GUESS] Bytes are something like {ACLineStatus, BatteryFlag, LifePercent, ...}. Not verified. |
| IONotificationLEDs | 0x500FFFD0 | 7 bytes (0x500FFFD0-0x500FFFD6) | [DOC] Notification LED and vibrate. The skin shows the LED as a button with `isNotificationLED="true"`. Register semantics unknown. |
| IOSpeakerPhone | 0x500FFFC0 | 2 bytes | `/speakerphone n`: bit0 car kit, bit1 headset, bit2 speakerphone. Semantics unknown. |

**Recommendation:** map these as plain read/write byte arrays, initialised to:

- AC power, 100% charge,
- LEDs off,
- no speakerphone.

Log guest accesses to these ranges to learn the protocol. Reading the battery or NLED driver in the WM6 image, or disassembling the DE 3.0 handlers, would settle the semantics.

### 4.5 Serial port mapping

- `/u0` → UART0 (guest COM1, e.g. GPS).
- `/u1` → UART1 (debug/KITL serial; console with `/c`).
- `/u2` → UART2 (COM3).

---

## 5. `.dess` saved-state format [SRC] `boards/state.cpp`, `include/state.h`; versions [BIN]

All values are little-endian.

Multi-character constants are written as MSVC ints, so `'SSED'` is stored as the bytes `"DESS"`. Every tag shows up byte-reversed in the file.

```
u32 'SSED'   -> bytes "DESS"
i32 version  -> V1 writes 12, accepts 11..12. Local files: WM5 sessions = 11 (DE 1.0), WM6 sessions = 17 (DE 2.0/3.0)
u32 'SAMS'   -> bytes "SMAS" (platform)
u8  directory[256]   (zero padding)
--- Config section ('CONF', bytes "FNOC") ---
  KeyboardSelector(u8) KeyboardToWindows(bool) HostOnlyRouting(bool) FlashEnabled(bool)
  String SkinXMLFile; String FlashStateFile; LANGID LangID (u16)
  String UARTNames[0..2]; u32 InitialInstructionPointer; i32 ScreenHeight, ScreenWidth, ScreenBitsPerPixel
  String ROMImageName; u32 ProcessorFeatures; bool NetworkingEnabled; bool PCMCIACardInserted; bool fCreateConsoleWindow
  String VMIDName; GUID VMID; i32 RotationMode; bool IsZoomed; bool fShowSkin; i32 ToolTipState
  u8 NE2000MACAddress[6]; bool IsAlwaysOnTop; String FolderShareName; u8 SuggestedMacNE2000[6]; u8 SuggestedMacCS8900[6]
  u32 PhysicalMemoryExtensionSize(MB); bool SpecifiedNE2000Mac; bool SpecifiedCS8900Mac; bool UseDefaultSaveState
  bool Board64RamRegion; i32 DefaultRotationMode; u32 FuncKeyCode; u8 pad[55]
  (String = u32 byteLen incl. NUL, then UTF-16 data; 0xFFFFFFFF = NULL)
--- Board ---
  LZ(FlashBank0, 32 MB in V1)   LZ(RAM 64 MB)   [LZ(extension RAM) if memsize > 64]
--- Devices, in ascending MMIO address order ---
  'AM29' u16 CMD
  (PCMCIA and CS8900Memory save nothing)
  'NETW' CS8900 state
  'MEMC' ...
  'INTC' SRCPND INTMOD INTMASK INTPND PRIORITY INTOFFSET SUBSRCPND(u16) INTSUBMSK(u16)
  'DMAC' ×2 (9 × u32 each)
  'CLKP'
  'LCDC' LCD registers, then 'UWIN' window/skin state
  'FLSH' NAND regs + optional LZ(flash)
  'UART' ×3
  'DMAT' LZ(in) LZ(out) Flags... + channel records + address service
  'FLDS'
  'IEMS' u32 mask
  'PWMT'
  'IIIS'
  'GPIO'
  'RTCC'
  'ADCV'
  'SPI1' + 'ISPI' + keyboard queue
LZ block = u32 'ZMEM' (zlib compress2 level 1) or 'UMEM' (raw), u32 storedLength, data[storedLength]
```

- Field sizes follow MSVC x86: bool = 1 byte, no padding between `Write`s because each field is written separately.
- The CONF header verified in a local version-17 file: `"FNOC"`, then bytes `a5 00 00 00` (KeyboardSelector = 0xA5 and three bools), then the skin string length 0xB4.
- **Version 17 (DE 2.0/3.0) is not documented.** It likely adds 128 MB flash, battery/LED/speakerphone state and maybe more CONF fields. Parse defensively by walking the tags. [GUESS]
- **There is no CPU register or CP15 block.** Restore depends on the guest's sleep/resume path (section 1.4).

---

## 6. Practical boot recipe for WMDroid (synthesised)

1. **Allocate memory.**
   - RAM: 64 MB at PA 0x30000000, plus an extension at 0x34000000 (use 192 MB to match `/memsize 256`).
   - NOR: 128 MB at PA 0, zero- or 0xFF-filled. [GUESS] The real initial NOR contents beyond the image are unverified.
2. **Load the image.**
   - `.bin`: load records with the ROMHDR-based VA→PA rule (2.2); PC = mapped entry point.
   - Raw NB0 (all WM6.x "`.BIN`" files and the MSFP `.nb0`): copy to PA 0; PC = 0.
3. **Set CPU state.** SVC mode, I=F=1, MMU off, SP = 0x30010000. CP15 ID = 0x69059201, cache type = 0x0B172172. Accept the iA/nF bits in c1.
4. **Write boot arguments.** `BSP_ARGS` screen fields at PA 0x30020000+0x44 (signature 0xDE12DE34, W, H, bpp, flags = 0, orientation). For WM2003SE also write `BOOT_ARGS` at 0x301FF000.
5. **Implement devices.** Interrupt controller, PWM timers 2-4 (63.4375 ticks/ms at prescaler 200 and /4), GPIO (GSTATUS2 = 0 on cold boot), UART0-2, LCD (read geometry and frame buffer from the registers), ADC/touch, SPI1 keyboard, RTC, clock/memory controller stubs, watchdog soft reset, IIS/DMA audio. Then folder sharing, EmulServ, DMA transport and the DE 2/3 stubs.
6. **Make unknown-device accesses safe.** Unknown MMIO should read 0 and ignore writes, and log the access rather than abort.
7. **Detect idle.** Use the LDR/CMP/B-back idle loop to sleep the host thread until an IRQ is pending.

---

## Sources

Code and binary data:

- DE 1.0 shared source mirror: https://github.com/gitntel/Microsoft-Device-Emulator (`boards/`, `cpus/arm/`, `include/`, `docs/`)
- Mirror with the original zip: https://github.com/dbgft/MicrosoftDeviceEmulatorforWinCE
- Local data analysis (ROM headers, OEMAddressTables, bootloader words, `.dess` headers, DE executable device tables) was done with scratch scripts on the files under `ARM-based/` and `Emulators/` in this archive.

Microsoft documentation and posts:

- Device Emulator Command-Line Reference (VS2008): https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2008/aa188169(v=vs.90)
- Barry Bond's blog index: https://learn.microsoft.com/en-us/archive/blogs/barrybo/
- DE V2 performance internals (TLB, interrupt polling, memcpy): https://learn.microsoft.com/en-us/archive/blogs/barrybo/deviceemulator-v2-how-did-we-get-a-40-improvement-in-performance
- Notification LED in DE 2.0: https://learn.microsoft.com/en-us/archive/blogs/barrybo/notification-led-in-the-deviceemulator-v2-0-how-to-customize-the-led-with-a-skin
- GPS and the emulator (serial port 0 = COM1): https://learn.microsoft.com/en-us/archive/blogs/barrybo/gps-and-the-deviceemulator-part-2-using-gps-receiver-hardware-from-the-emulator
- OEM info string "Microsoft DeviceEmulator": https://learn.microsoft.com/en-us/archive/blogs/barrybo/detecting-whether-your-application-is-running-on-the-deviceemulator-or-not
- CE 5.0 BSP and the 2 × 32 MB RAM split (comments): https://learn.microsoft.com/en-us/archive/blogs/msdn/mikehall/armv4i-emulator-working-with-ce-5-0-platform-builder
- DE 2.0 CTP announcement: https://learn.microsoft.com/en-us/archive/blogs/barrybo/now-available-device-emulator-2-0-community-tech-preview
- OSNews release note: https://www.osnews.com/story/15204/shared-source-microsoft-device-emulator-10-release/
