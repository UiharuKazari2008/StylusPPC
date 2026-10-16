# DE IOBattery (0x500FFFF0): what the WM 6.5.3 battery driver reads

Research notes for WMDroid. This doc settles the `IOBattery` guess in `research-de-hardware.md` §4.4 and explains why the
title-bar battery icon ignores `setBattery`.

Tags:

- **[DISASM]**: NDK `llvm-objdump --triple=armv5te` of `battdrvr.dll` and `shell32.exe` from the IMGFS of
  `PPC_USA_GSM_FWVGA_VR.BIN` (6.5.3 Pro). Import ordinals were resolved against `coredll.dll`/`ceddk.dll` export tables
  in the same image's XIP region. Nothing was executed.
- **[OBS]**: observed on the real Microsoft DE 3.0 (`research-status-indicators.md` §5.2).
- **[ROM]**: registry (`default.hv`). **[DOC]**: Microsoft CE docs, recalled. **[INFERRED]**: reasoned from the above.

## TL;DR

1. **Only `battdrvr.dll` touches the device.** A byte scan of the whole image and all 2,407 extracted modules found
   `0x500FFFF0` only in `battdrvr.dll`. Neither the OAL nor `nk.exe` uses it. The driver **only reads** the device: no
   writes, no interrupt and no EmulServ bit. [DISASM]
2. **The current layout is wrong.** The real layout is:

   | Offset | Width | Meaning |
   |---|---|---|
   | +0 | u8 | **0 = AC online**, any non-zero value = on battery (the reverse of our guess) |
   | +1 | u8 | **Percent *used*.** Life = 100 − min(byte, 100) |
   | +2 | u16 LE | **Battery temperature in 0.1 °C**, passed through unchanged to `BatteryTemperature` |

   The real DE default reads as the dword **`0x01180000`**, which is bytes `{00, 00, 18, 01}`: AC online, 100 %, 28.0 °C.
   [OBS][DISASM]
3. **The guest polls every 500 ms** (driver default; the registry does not override it). When the status changes, the driver
   calls `PowerPolicyNotify(PPN_POWERCHANGE)`. The PM then broadcasts `PBT_POWERINFOCHANGE`, and `shell32.exe` redraws the
   title bar. **The emulator only needs to change the bytes**, and the icon follows within about 0.5 s. [DISASM]
4. **Today's bug:** we write `{1, 0, pct, 0}`. The guest reads that as "on battery, 100 %, temperature = pct/10 °C", so the
   icon never moves and AC is never shown.

## 1. Driver structure [DISASM]

`battdrvr.dll` is the stock CE MDD with a small DE PDD. It is unoptimised code, which makes it easy to read.

- **Load address:** `.text` at `0x7B63C000`. `.data` at `0x01E2F000`.
- **IAT entries:**
  - CEDDK: `HalTranslateBusAddress`, `MmMapIoSpace`, `MmUnmapIoSpace`.
  - COREDLL: `CreateThread`, `CeSetThreadPriority`, `WaitForSingleObject`, `PowerPolicyNotify`, `CeSetPowerOnEvent`,
    `OpenDeviceKey`, `RegQueryValueExW`, `GetTickCount`, and others.
  - There is no `InterruptInitialize` and no `KernelIoControl`, so the driver is not interrupt-driven.
- **Exports:** `Init` `0x7B63C6C4`, `IOControl` `0x7B63CAE4`, `PowerUp` `0x7B63CFB4`, `PowerDown` `0x7B63CFA0`, plus the
  usual stream stubs.
- **Registry:** `HKLM\Drivers\BuiltIn\Battery` holds only `Prefix`, `Dll`, `Flags`, `Order` and `IClass`. That means
  `PollInterval`, `PollPriority256` and `PddBufferSize` take the defaults compiled into `Init`. [ROM]

### 1.1 Mapping (lazy, on the first status read), at `0x7B63C160`

```
7b63c178: ldr  r3, =0x500ffff0        ; PA
          ... bl HalTranslateBusAddress(Internal=0, bus 0, PA, &as, &xlat)
7b63c1c8: mov  r2, #4
7b63c1d4: bl   MmMapIoSpace(xlat, 4, FALSE)
7b63c1e4: str  r0, [data+0xA0]        ; g_pBatt
```

The first call to `BatteryPDDGetStatus` triggers it, through the flag at `data+0x8C`, which is initially 1. The same
call sets `g_lastLevel` (`data+0xA4`) to 100.

### 1.2 `BatteryPDDGetStatus(SYSTEM_POWER_STATUS_EX2 *s, BOOL *changed)` at `0x7B63C2B0`

These are every access the driver makes to the device:

```
; AC: helper 0x7b63c1fc
7b63c208: ldrb r3, [g_pBatt, #0]      ; byte0
          cmp r3,#0 ; result = (byte0 == 0) ? 1 : 0     -> "AC online"
; level
7b63c310: ldrb r3, [g_pBatt, #1]      ; byte1
7b63c31c: cmp r3,#100 ; movgt r3,#100
7b63c330: rsb  r3, r3, #100           ; life = 100 - byte1
; temperature
7b63c3b0: ldrh r3, [g_pBatt, #2]      ; u16 at +2
          -> s->BatteryTemperature (+0x2C)
```

The driver then builds the status from these values:

| `SYSTEM_POWER_STATUS_EX2` field (offset) | Value written |
|---|---|
| ACLineStatus (+0) | 1 if byte0 == 0, otherwise 0 |
| BatteryFlag (+1) | life ≥ 65 → `1` HIGH. 20 ≤ life < 65 → `2` LOW. life < 20 → `4` CRITICAL. Add **`8` CHARGING only if AC is online *and* life > the life seen on the previous call** (`0x7B63C35C..0x7B63C378`). `0x80` (no battery) is never set. |
| BatteryLifePercent (+2) | life (0..100) |
| BatteryLifeTime / FullLifeTime (+4/+8) | `0xFFFFFFFF` (unknown) |
| BackupBatteryFlag (+0xD) | `0xFF` (unknown). Backup percent is 0 and backup times are −1 |
| BatteryVoltage (+0x18) | `life*41/1000` (`mul` followed by `__rt_udiv(1000, …)`). This is effectively 0..4 and probably a PDD bug. |
| Current, AvgCurrent, AvgInterval, mAh (+0x1C..+0x28) | 0 |
| BatteryTemperature (+0x2C) | u16 at device +2 |
| BackupBatteryVoltage (+0x30) | 0 |
| BatteryChemistry (+0x34) | 4 = `BATTERY_CHEMISTRY_LION` |

The driver sets `*changed` to 0. Byte 3 is read only as the high byte of the temperature.

**Other PDD entry points:**

- GetLevels (IOCTL `0x29040C`) returns `0x00030003`.
- SupportsChangeNotification (IOCTL `0x290410`) returns 0, so the MDD must poll.
- Status IOCTLs: `0x290400` returns `SYSTEM_POWER_STATUS_EX` (24 bytes) and `0x290404` returns `EX2` (≥56 bytes).

### 1.3 Poll thread, at `0x7B63C5DC` (created by `Init`)

`Init` stores the thread defaults in its context at `data+0x100`:

```
7b63c778: mov r3,#500 ; str r3,[ctx]      ; PollInterval default = 500 ms
7b63c77c: mov r2,#249 ; str r2,[ctx,#4]   ; PollPriority256 default = 249
```

Each value is overridden only if `OpenDeviceKey` + `RegQueryValueExW` succeeds with a 4-byte value. The thread body:

```
CeSetThreadPriority(GetCurrentThread(), 249)
PowerPolicyNotify(PPN_POWERCHANGE=2, 0)            ; once at start
loop until data+0x180 (exit flag):
  r = WaitForSingleObject(hResumeEvt /*data+0x140*/, 500)
  if r == WAIT_OBJECT_0:   resume bookkeeping (time accounting, PDDResume no-op)
  elif r == WAIT_TIMEOUT:
     memset(tmp, 0, 56); GetStatusEx2(tmp, 56, fUpdate=TRUE)   ; 0x7b63d114 -> PDD reads MMIO
     if memcmp(tmp, ctx+8 /*last notified*/, 56) != 0:
        memcpy(ctx+8, tmp, 56)
        PowerPolicyNotify(PPN_POWERCHANGE, 0)        ; 0x7b63c674
  else: exit
```

`PowerUp` (`0x7B63CFB4`) calls `CeSetPowerOnEvent(hResumeEvt)`. Calls with `fUpdate=TRUE` refresh the shared cache
(`data+0x174`) from the hardware. Calls with `fUpdate=FALSE` return that cache, which is at most one poll old.

## 2. Path to the title bar [DISASM][DOC]

1. The poll detects a change and calls `PowerPolicyNotify(PPN_POWERCHANGE)`.
2. `pm.dll` broadcasts `PBT_POWERINFOCHANGE` (POWER_BROADCAST_POWER_INFO) to the queues registered with
   `RequestPowerNotifications`. [DOC]
3. `shell32.exe` handles the message at `0x61030`: `cmp r3,#8` (PBT_POWERINFOCHANGE) → `bl 0x41F00` → `SendMessage(…,
   0x49C, …, &power_info)` to the title-bar window.
4. `0x41F00` calls `GetSystemPowerStatusEx(&s, TRUE)`. It caches AC and flags and raises the low/critical warnings when
   the flag has bit 2 or bit 4 set. These warnings are strings `0x13D4..0x13D7` and the shell can play `Lowbatt.wma`.
5. When `BatteryFlag & 8` is set, the same function starts a **700 ms timer (id 264)** that drives the charging
   animation (`0x42138..0x4216C`). When the bit is clear, it kills the timer.

Writing `HKLM\System\State\Battery\Main` does nothing to the icon (`research-status-indicators.md` §1.6), which matches
this path.

## 3. Caching and refresh

- **No interrupt or doorbell is needed, and none exists.** The guest sees new bytes on the next 500 ms poll. Only a change
  in the computed 56-byte status produces a notification. Any change to AC, level or temperature counts.
- **While the guest is suspended, the poll thread does not run.** After resume, the next poll comes 500 ms later.
- **Torn reads are harmless.** The guest reads byte0, byte1 and the u16 in three separate loads, so a write that lands
  mid-poll gives one mixed sample. The next poll corrects it. Writing all 4 bytes together, for example under the
  machine lock, is enough.
- **The CHARGING flag is edge-triggered, not level-triggered.** It is set only on the poll where life rose since the
  previous poll. On a steady "80 %, plugged in", the guest reports AC=1 and flag `0x01`, with no charging animation.
  Whenever the level steps up (79 → 80), the flag becomes `0x09` for one poll. The next poll clears it, which triggers
  another notification, so the animation runs for about 0.5–1 s. The real DE behaves the same way, because this is the
  same driver.
  - The hardware has no bit that holds "charging" on. The core now patches the `ble` at `0x7B63C368` into a NOP so
    that AC online always sets CHARGING (also at 100 % on AC). See "Thresholds" below.

## 4. What `setBattery` should do

The bytes at `0x500FFFF0..F3` are a little-endian dword:

```
b[0] = ac ? 0 : 1;                          // 0 = AC online (inverted!)
b[1] = 100 - clamp(percent, 0, 100);        // percent USED
b[2] = tempDeciC & 0xFF;                    // u16 LE, 0.1 degC
b[3] = tempDeciC >> 8;                      // default 280 (28.0 degC, the real DE value)
```

- Android's `BatteryManager.EXTRA_TEMPERATURE` is also in tenths of a degree Celsius, so it can be passed straight
  through. If it is unavailable, keep 280.
- Make the power-on default `{0x00, 0x00, 0x18, 0x01}` to match the real DE.
- No interrupt, EmulServ bit or other side effect is required.

| Case | Bytes (+0..+3) | Dword read at +0 | Guest sees |
|---|---|---|---|
| 57 %, discharging | `01 2B 18 01` | `0x01182B01` | AC 0, flag `0x02` LOW, life 57 |
| 80 %, charging | `00 14 18 01` | `0x01181400` | AC 1, flag `0x01` HIGH. `0x09` (+CHARGING) for one poll whenever the level has just risen |
| 100 %, AC, full | `00 00 18 01` | `0x01180000` | AC 1, flag `0x01` HIGH, life 100 (identical to real DE) |

**Thresholds the guest applies:**

- Life < 20 gives CRITICAL, and life 20–64 gives LOW. The shell shows its warnings from these flags.
- Forwarding the phone's real level therefore made 6.5.3 raise low-battery and critical-battery bubbles below 65 % and
  20 %, which is the real-device behaviour but far too early for a phone.
- **Fixed in the core** (`core/src/board/codepatch.cpp`, tables `kBatt653` / `kBatt614`; until 2026-10-16 the guest
  helper's `CodePatch.cs` did it from inside WM): LOW below 20 % (`cmp life,#65` -> `#20`, twice), CRITICAL below
  10 % (`cmp life,#20` -> `#10`), CHARGING whenever on AC. The code is found by byte signature in physical memory
  before it runs, so the fix does not depend on the helper. WM 6.1.4's battdrvr is an optimised build with the same
  65 % / 20 % thresholds and the same one-poll CHARGING rule (`cmp r2,#0x41` / `cmp r2,#0x14`, `bls` over
  `mov r4,#8`); it gets the same fix. WM 5 (`wm5gsmvga.bin`) never reads the DE battery device (0 reads in a boot),
  so there is nothing to fix there.

## 5. Reproduction

- **Extraction:** `battdrvr.dll` came from the IMGFS (`imgfs_dump.py`, see `research-camera.md` §Reproduction). Its e32
  values: `.text` RVA `0x1000` at `0x7B63C000` (size `0x196C`), `.data` at `0x01E2F000`. The literal `0x500FFFF0` is at
  `0x7B63C1F8`.
- **Import ordinals** were resolved from the export tables of the XIP modules:
  - The XIP TOC is at `0x880D57E4`, which gives coredll e32 at `0x880D5C90` and ceddk e32 at `0x880D6628`.
  - CEDDK 6/8/9 = HalTranslateBusAddress/MmMapIoSpace/MmUnmapIoSpace.
  - COREDLL 1764 = PowerPolicyNotify, 1688 = CeSetPowerOnEvent, 715 = GetSystemPowerStatusEx, 1585 =
    RequestPowerNotifications.
- **Shell side:** in `shell32.exe` (vbase `0x10000`), the `GetSystemPowerStatusEx` IAT slot is `0xD8560`, its thunk is
  `0xD5094`, and the only caller is `0x41F18`.
