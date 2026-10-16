# WM 6.5.3 status indicators: SNAPI values, who writes them, FakeRIL, and physical MMIO from a CF app

Research notes for WMDroid. The goal is to make the guest's title bar and Home screen mirror the real Android phone:
signal bars, operator name, the "No Service" problem, the data-type icon (G/E/3G/H), Wi-Fi, and battery.
Companion to `research-cellular.md` (§6 there covers FakeRIL) and `research-de-hardware.md`.

Confidence tags:

- **[OBS]**: observed on the real Microsoft Device Emulator 3.0 running `PPC_USA_GSM_WVGA_VR.BIN` (WM 6.5.3 Pro, CE 5.2.23090),
  using the probe app in `WM 6.5.3 Professional Emulator\Samples\Probe` (see §6). Registry values were written from a CF 3.5
  app and the title bar was screenshotted after each write.
- **[MANAGED]**: decoded from `SystemState.InitSystemTable` in the device's own `Microsoft.WindowsMobile.Status.dll`
  (`SDK\CF35`, copied from the ROM). The IL was read with `System.Reflection.Metadata`; nothing was executed. This is
  Microsoft's own property → (key, value, bitmask) table, so it is equivalent to `snapi.h`.
- **[DISASM]**: ARM disassembly of a module pulled out of the 6.5.3 ROM (`ossvcs.dll`, `shell32.exe`, `fakeril.dll`).
- **[ROM]**: strings or registry read from the 6.5.3 ROM / `default.hv`.
- **[DOC]**: Microsoft documentation. **[INFERRED]**: reasoned from the above. **[GUESS]**: plausible, not verified.

---

## TL;DR

1. **Writing `HKLM\System\State\...` from a CF app changes the 6.5.3 title bar and Home screen within 1–3 s.** [OBS]
   This works for signal bars, No Service / Searching / Phone off / No SIM, operator name, roaming, the G/E/3G/H icon and
   the Wi-Fi icon. It does **not** work for the battery icon (that comes from the battery driver, not SNAPI).
2. **The bars are driven by `Signal Strength`, not `Signal Strength Raw`, and only these exact values draw bars:**
   `10` = 0 bars, `30` = 1, `60` = 2, `90` = 3, `100` = 4. Any other value draws 0 bars. These are exactly the five levels
   `ossvcs.dll` writes. [OBS][DISASM]
3. **The writer is `ossvcs.dll` (loaded in `shell32.exe`).** It rewrites the whole phone block on every RIL notification
   (registration, signal, radio state, operator). FakeRIL sends **no** periodic notifications (0 changes in 45 s), so
   hand-written values stay put until the next RIL event: boot, flight mode, suspend/resume, a call, or a re-registration.
   Strategy: watch the values and re-apply them after ossvcs (§3). [OBS][DISASM]
4. **FakeRIL does not register by itself after the radio comes back on.** It registers when the phone stack calls
   `RIL_RegisterOnNetwork`, and then answers instantly ("Fake Network", −65 dBm). On the real DE, `RIL_SetEquipmentState(1→2)`
   alone leaves it unregistered ("No Service") indefinitely, and a single `RIL_RegisterOnNetwork(AUTOMATIC)` fixes it at once. A
   Power-button suspend/resume on the real DE also shows "No Service", which recovers by itself within about a minute.
   So WMDroid's persistent "No Service" means the register request never arrives or never completes. The cheap fix is for the
   agent to call `RIL_RegisterOnNetwork` (§4.4). [OBS]
5. **CDMA letters exist and work: `Cellular System Available` 0x02 → "1X", 0x04 → "Ev", but only with `Status` bit 0x100000
   (GprsCoverage) clear.** With GprsCoverage set, the taskbar only knows G/E/3G/H. In WMDroid every data letter (GSM too) is
   hidden behind a "⇄" icon until message 0x480 (wParam 0) is posted to `HHTaskBar` (§2.1). [OBS][DISASM]
6. **VirtualCopy with PAGE_PHYSICAL works from an unsigned CF 3.5 exe on these images.** `CeGetCurrentTrust()` = 2
   (OEM_CERTIFY_TRUST) and policy 4123 (Privileged Applications) = 1, so every app is fully trusted. The DE battery page
   `0x500FF000` was mapped and read successfully. [OBS]
7. **WM 5.0 (Pocket PC Phone, CE 5.1.1700) is different (§7):** its taskbar has only G, E, 1X and Ev (no 3G/H), the Phone
   app shows the operator as a picture (`\Windows\carrierlogo.gif`, a "Mobile Operator" placeholder in ROM), and every
   windowless .NET CF program leaves the spinning wait cursor on screen unless it resets `Cursor.Current`. [OBS][DISASM]

---

## 1. SNAPI registry values (WM 6.x, exact)

All values below come from Microsoft's own table in `Microsoft.WindowsMobile.Status.dll` [MANAGED]. It matches the
`snapi.h` pattern documented on MSDN (example `SN_PHONENOSIM_*` = `HKLM`, `System\State\Phone`, `Status`, bitmask 2) [DOC].
All DWORDs are REG_DWORD and all strings REG_SZ.

### 1.1 `HKLM\System\State\Phone`

| Value | Type | Meaning | Notes |
|---|---|---|---|
| `Status` | DWORD bitmask | see 1.2 | Real DE when registered: `0x009000A0` [OBS] |
| `Signal Strength` | DWORD | **title-bar level**: 10/30/60/90/100 (see 1.4) | Not in the managed table; written by ossvcs and read by the taskbar [OBS][DISASM] |
| `Signal Strength Raw` | DWORD 0–100 | percentage (`SystemProperty.PhoneSignalStrength`) | Real DE: 90 [OBS] |
| `Current Operator Name` | SZ | Home screen operator line | ossvcs **deletes** it when unregistered [OBS] |
| `Cellular System Available` | DWORD bitmask | see 1.3, drives the G/E/3G/H icon | FakeRIL: 0 (it reports system type GSM only) [OBS] |
| `Cellular System Connected` | DWORD bitmask | see 1.3, small "connected" letter on the antenna | |
| `Radio Ready State` | DWORD | copy of RIL `dwReadyState` (0x1F = all ready) | written by ossvcs [OBS] |
| `Extended Status`, `MultiLine Capabilities`, `Active Call Count`, `Caller Name/Number/...`, `Call Start Time`, `Cell Broadcast`, `Incoming Caller *`, `Last Incoming Caller *` | | call state | not needed here |
| `Phone Status Flags` | DWORD | `0x10` default in `default.hv` | not a SNAPI value; no reader found. Ignore it. [ROM] |

### 1.2 `Status` bits [MANAGED]

| Bit | SystemProperty | Bit | SystemProperty |
|---|---|---|---|
| `0x00000001` | PhoneSimFull | `0x00001000` | PhoneActiveDataCall |
| `0x00000002` | PhoneNoSim | `0x00002000` | PhoneCallBarring |
| `0x00000004` | PhoneInvalidSim | `0x00004000` | PhoneCallOnHold |
| `0x00000008` | PhoneBlockedSim | `0x00008000` | PhoneConferenceCall |
| `0x00000010` | **PhoneRadioOff** | `0x00010000` | PhoneIncomingCall |
| `0x00000020` | **PhoneRadioPresent** | `0x00020000` | PhoneCallCalling |
| `0x00000040` | PhoneRingerOff | `0x00100000` | **PhoneGprsCoverage** |
| `0x00000080` | PhoneLine1Selected | `0x00200000` | **PhoneNoService** |
| `0x00000100` | PhoneLine2Selected | `0x00400000` | **PhoneSearchingForService** |
| `0x00000200` | **PhoneRoaming** | `0x00800000` | **PhoneHomeService** |
| `0x00000400` | PhoneCallForwardingOnLine1 | `0x10000000` | Phone1xRttCoverage |
| `0x00000800` | PhoneMissedCall | `0x20000000` | PhoneCallTalking |
| | | `0x40000000` | PhoneCallForwardingOnLine2 |

The bits in bold are the ones the indicators need. Leave the call bits (0x400–0x20000, 0x20000000+) to the phone stack.

### 1.3 `Cellular System Available` / `Cellular System Connected` bits [MANAGED]

| Bit | Meaning | Data-slot icon, `Status` **with** GprsCoverage | Data-slot icon, `Status` **without** GprsCoverage |
|---|---|---|---|
| `0x01` | GPRS | **G** | hatched **G** (tapres 22222) |
| `0x02` | 1xRTT | none ("⇄×") | **1X** (tapres 22209) |
| `0x04` | EV-DO | none ("⇄×") | **Ev** (tapres 22215) |
| `0x08` | EDGE | **E** | **E** (tapres 22221) |
| `0x10` | UMTS | **3G** | **3G+** (tapres 22220) |
| `0x20` | EV-DV | none ("⇄×") | **Ev** (tapres 22217, looks the same as EV-DO) |
| `0x40` | HSDPA | **H** | **H** (tapres 22219) |
| `0x80000000` | CSD (Connected only) | | |

The GSM column is [OBS] on the real DE; the CDMA column and the GSM 3G/E/G/H checks were [OBS] in WMDroid (CLI, FWVGA image)
and match the shell32 code [DISASM] (§2.1). **The 1X/Ev letters only appear when `Status` bit 0x100000 (GprsCoverage) is
clear.** `Phone1xRttCoverage` (0x10000000) makes no difference to the title bar either way (Status 0x008000A0 and 0x108000A0
gave the same icons). When several bits are set, the best one wins (0x50 → H; order in §2.1). [OBS] There is no 4G/LTE icon in
6.5.3, so use H (0x40) for LTE/NR. [INFERRED]

### 1.4 Signal level → bars [OBS][DISASM]

`ossvcs.dll` (fn @ 0x02743BEC) computes, from `RILSIGNALQUALITY`:

```
raw   = (clamp(nSignalStrength, nLowSignalStrength, nHighSignalStrength) - nLow) * 100 / (nHigh - nLow)
        (nSignalStrength == 0xFFFFFFFF "unknown" -> uses nLow, i.e. raw 0)
level = raw < LowFloor(25) ? 10 : raw < MedFloor(50) ? 30 : raw < HighFloor(75) ? 60 : raw < VeryHighFloor(90) ? 90 : 100
"Signal Strength" = level, "Signal Strength Raw" = raw
```

The floors can be overridden by optional DWORDs `HKLM\Software\Microsoft\ssupdate\SignalLevelLowFloor`,
`...MedFloor`, `...HighFloor` and `...VeryHighFloor` (that key is absent in the image). [DISASM]
With FakeRIL's −65 dBm and Low/High = −110/−60, raw = 90 and level = 100, which matches what the real DE shows. [OBS]

The taskbar maps **exact** levels: 10→0 bars, 30→1, 60→2, 90→3, 100→4. Values 0, 1–29, 31–59, 61–89 and 91–99 all
draw the antenna with 0 bars. [OBS: sweep of 48 values] Android level 0–4 therefore maps to 10/30/60/90/100.

### 1.5 `HKLM\System\State\Hardware` [MANAGED]

| Value | Bits | Title bar [OBS] |
|---|---|---|
| `WiFi` | `0x01` present, `0x02` power on, `0x04` networks available, `0x08` connecting, `0x10` connected | 0x01: no icon. 0x03/0x07: Wi-Fi antenna. 0x0B: Wi-Fi with ring (connecting). 0x13: Wi-Fi with arrows (connected). **Whenever Wi-Fi is on, its icon takes the data-icon slot instead of G/E/3G/H.** |
| `Bluetooth` | `0x01` power on, `0x02` discoverable, `0x04` A2DP connected, `0x08` present, `0x10` HF control, `0x20` HF audio | no title-bar icon in 6.5.3 |
| `Headset`, `Car Kit`, `Speaker`, `Cradled`, `Camera`, `CameraEnabled`, `ClamshellClosed`, `Handsfree` | DWORD 0/1 | |

The DE image has no WLAN adapter, so nothing else writes `WiFi`. It was absent before the test. [OBS][INFERRED]

### 1.6 Battery, connections, other [MANAGED][DOC][OBS]

- `HKLM\System\State\Battery\Main` and `\Backup`: the high word is strength (0 VeryLow, 21 Low, 41 Medium, 61 High, 81 VeryHigh) and
  the low word is state flags (0 Normal, 1 NotPresent, 2 Charging, 4 Low, 8 Critical). Real DE: `Main = 0x00510000`.
  **Writing `Main` does not change the title-bar battery icon** [OBS]. The icon follows the battery driver
  (`battdrvr.dll` → DE `IOBattery`), so keep driving it from the WMDroid battery device.
- `HKLM\System\State\Connections\{Cellular,Network,Desktop,Bluetooth,Modem,Proxy,Vpn,Unknown}`: `Count` (DWORD),
  `Descriptions` (SZ), `Adapters` (SZ), plus the overall `HKLM\System\State\Connections\Count`. Setting `Cellular\Count=1` had no
  visible title-bar effect. [OBS]
- `HKLM\System\State\Connectivity\InternetSharing` bits 0x01..0x20, and `HKLM\System\State\Lock`. Not needed.

---

## 2. What the 6.5.3 shell shows for each state [OBS]

Taken with `Signal Strength = 100`, unless noted.

| `Status` | Title-bar antenna | Home operator line |
|---|---|---|
| `0x009000A0` (Home + GprsCoverage + Line1 + RadioPresent) | 4 bars | `Current Operator Name` |
| `0x002000A0` (NoService) | antenna + **!** | **"No Service"** |
| `0x004000A0` (Searching) | antenna + **…** | **"Searching..."** |
| `0x000000B0` (RadioOff) | antenna + **×** | **"Phone off"** |
| `0x000000A2` (NoSim, Home bit **clear**) | SIM card with × | **"No SIM"** |
| `0x009000A2` (NoSim **with** Home bit) | 4 bars (NoSim is ignored) | operator |
| `0x002000A4` / `0x002000A8` (Invalid / Blocked SIM + NoService) | antenna + ! | "No Service" |
| `0x00900080` (RadioPresent clear) | antenna with small × | |
| `0x00000000` | antenna + × | |
| `0x009002A0` (Roaming) | 4 bars **plus a roaming triangle** left of the data slot | operator |

- **Data slot** (left of the antenna): Wi-Fi icon if `WiFi & 0x02`, else G/E/3G/H (or 1X/Ev, §2.1) from `Cellular System Available`, else the
  "⇄×" no-data icon. Setting the same bit in `Cellular System Connected` adds a small G/H at the top-left of the antenna. The
  `Status` 0x1000 bit (ActiveDataCall) and `Connections\Cellular\Count` had no visible effect.
- Changes show up within about 1.5 s (the taskbar uses registry change notifications, not polling).
- `shell32.exe` also has OEM hooks `LoadOemRssiIcon` / `LoadOemDataIcon` (via `Security\ResOver\Bitmaps` `FuncDll`, absent here) and
  a `ConsistentRoamingIconSlot` setting. These are not needed. [ROM]

### 2.1 How the 6.5.3 taskbar picks the data-slot icon (CDMA included) [DISASM][OBS]

Source: `shell32.exe` from the 6.5.3 image (`.text` at 0x11000, data at 0xD8000). The taskbar does not read the SNAPI value names;
it asks `ossvcs` for property IDs, and ossvcs' table (at 0x02717FB8) maps them: 0x00060013 = `Phone\Status`,
0x00760013 = `Cellular System Available`, 0x007D0013 = `Cellular System Connected`, 0x007B0013 = `Hardware\WiFi`,
0x000C0013 = `Active Call Count`. The collector at 0x5A3B0 copies `Status` into its state block, and the data-slot function at
0x5BCEC picks the icon in this order:

1. `WiFi & 0x10` (connected) → Wi-Fi icons (§1.5).
2. A "wired network" flag set by message **0x480 (WM_USER+0x80) to the `HHTaskBar` window** with wParam ≠ 0, or
   `HKLM\System\State\Hardware\Cradled` ≠ 0 → the generic **⇄** icon (shellres 4684). The 0x480 handler is case 18 of the window
   procedure's switch at 0x47AD8 (flag at 0xDC394). `netui.dll` posts 0x480 with wParam 1 / 0 (two neighbouring methods of one
   class, 0x7BF14DA8 / 0x7BF14DC8); netui is where the wired-network UI lives (`SinkWiredNetUI`, `WZC_CONNECTION_BUBBLE_WIRED.HTM`).
3. Otherwise the letter from `Cellular System Available`:
   - `Status & 0x100000` (GprsCoverage) **set**: a 4-entry GSM table (0x40 H, 0x10 3G, 0x08 E, 0x01 G), icons from `tapres.dll`
     (22218, 22216, 22214, 22208).
   - **clear**: a 7-entry table, tried in this order: 0x40 H, 0x20 Ev, 0x10 3G+, 0x04 Ev, 0x02 1X, 0x08 E, 0x01 hatched G
     (`tapres.dll` 22219, 22217, 22220, 22215, 22209, 22221, 22222). For 0x02/0x04/0x20 the OEM `LoadOemDataIcon` hook is called
     if one is installed (none in this image).
   - No match → the "⇄×" no-data icon.

`shellres.192.dll` also has animated letter sets with traffic arrows (G 4762, E 4787, 1X 4813, 3G 4838, Ev 4863 and 4888, H 4913,
25 frames each; tables at shell32 0xD9244) and small antenna-corner letters 4938–4945 (G, E, 1X, ⇄, Ev, Ev, H, 3G). Those are used
only when `Status & 0x1000` (ActiveDataCall) is set together with `Cellular System Connected`; the descriptor table at 0x171D0 maps
the same bits (0x40 H, 0x20 Ev, 0x10 3G, 0x04 Ev, 0x02 1X, 0x08 E, 0x01 G, 0x80000000 ⇄). `HKLM\Software\Microsoft\Shell\CellularDataIconOptions`
(DWORD, absent; read at 0x5A340) switches between the corner-letter and the in-slot animated style. Not needed by WMDroid.

**WMDroid consequence [OBS]:** WMDroid's internet is the emulated NE2000 card, so the 0x480 flag is set in a booted guest and the
data slot shows ⇄ for every `Cellular System Available` value, GSM letters included (seen in all test shots until the flag was
cleared). Posting `0x480, wParam 0` to `HHTaskBar` (`FindWindow("HHTaskBar")` + `PostMessage`) clears it, and the letters then
stay. In a 150 s run nothing set the flag again. The helper re-posts it about every 10 s while it reports mobile data.

Verified in WMDroid with the helper's own `status` command (top 40 px of 480x854 screenshots): `data=1X` → **1X**,
`data=EV` → **Ev** (Status 0x108000A0, Available/Connected 4), `data=3G/H/G/E` → **3G/H/G/E**, `data=EV roam=1` → roaming
triangle + **Ev**, `data=1X wifi=1 wificonn=1` → Wi-Fi icon, `data=` (none) → "⇄×".

---

## 3. Who writes these values, and a robust strategy

### 3.1 Writers [DISASM][OBS]

- **`ossvcs.dll`**, the "SSUpdate" thread hosted by `shell32.exe`. It imports `RIL.DLL`/`CELLCORE.DLL` and holds the strings
  `Status`, `Signal Strength`, `Signal Strength Raw`, `Current Operator Name`, `Cellular System Available/Connected`,
  `Radio Ready State`. It writes them **only in response to RIL notifications**, not on a timer.
- Observed rewrite events:
  - **Radio off/on** (`RIL_NOTIFY_RADIOEQUIPMENTSTATECHANGED` 0x00800001 and `REGSTATUSCHANGED` 0x00040001). Hand-written values were replaced
    by `Status=0x00200020`, `Signal Strength=10`, `Raw=0`, operator deleted, cellular bits 0.
  - **Registration** (`REGSTATUSCHANGED` 3→2, `GPRSREGSTATUSCHANGED` 0x00040004 → 2). Values were replaced by `0x00900020`, 100/90,
    "Fake Network". The Line1 bit 0x80 that was set at boot was not set again.
  - **Suspend/resume**: "No Service", then re-registration after about a minute or less.
  - Likely also calls and SIM events. FakeRIL has a `SignalRestorationTimeout` value. [GUESS]
- In 45 s of 500 ms polling while idle, **nothing** rewrote the hand-written values. FakeRIL's signal is static. [OBS]
- `HKLM\System\State\Hardware\WiFi` is written by `netui.dll`/WZC only when a WLAN adapter exists. There is none on DE. [ROM][INFERRED]
- Battery values are written by ossvcs from power notifications. They are cosmetic only (§1.6).

### 3.2 Recommended approach (in the guest agent)

1. **Keep FakeRIL registered** so the phone stack, TAPI and the phone app are consistent. At agent start, and after each resume, call
   `RIL_GetRegistrationStatus`. If the status is not 2 (home) or 5 (roaming), call `RIL_RegisterOnNetwork(h, RIL_OPSELMODE_AUTOMATIC=1, NULL)`
   (§4.4). The probe's `reg` mode does exactly this.
2. **Overlay the Android state** with a read-modify-write of `Status`. Only touch the mask
   `0x00F00232` (RadioPresent 0x20, RadioOff 0x10, NoSim 0x02, Roaming 0x200, GprsCoverage 0x100000, NoService 0x200000,
   Searching 0x400000, Home 0x800000) and keep the call bits. Then write `Signal Strength` (10/30/60/90/100), `Signal Strength Raw` (0–100),
   `Current Operator Name`, `Cellular System Available`/`Connected` and `Hardware\WiFi`.
3. **Re-assert after ossvcs.** Subscribe to changes of those values with `Microsoft.WindowsMobile.Status.RegistryState`
   (`new RegistryState(@"HKEY_LOCAL_MACHINE\System\State\Phone", "Status")`, `.Changed`), or with native
   `RegistryNotifyCallback`/`RegistryNotifyWindow` (aygshell, `regext.h`). When a value differs from the desired value, rewrite it after a
   short debounce (100–300 ms) so ossvcs finishes its batch first. Compare before writing to avoid feedback loops. Expect a brief
   flash of "Fake Network" or 4 bars at each RIL event. A slow poll (2–5 s) as a backstop is cheap.
4. Android → SNAPI mapping:

   | Android | SNAPI |
   |---|---|
   | `ServiceState.STATE_IN_SERVICE` (+`getRoaming()`) | Home 0x800000 \| RadioPresent 0x20 (\| Roaming 0x200) |
   | `STATE_OUT_OF_SERVICE` / `STATE_EMERGENCY_ONLY` | NoService 0x200000 \| 0x20, or Searching 0x400000 while searching |
   | `STATE_POWER_OFF` / airplane mode | RadioOff 0x10 \| 0x20 |
   | SIM absent (`SIM_STATE_ABSENT`) | NoSim 0x02, with Home **cleared** |
   | `SignalStrength.getLevel()` 0..4 | `Signal Strength` = {10,30,60,90,100}[level]; `Raw` = level*25 (or from dBm) |
   | `getNetworkOperatorName()` | `Current Operator Name` |
   | data network type GPRS/EDGE/UMTS/HSxPA/LTE/NR | Available = 0x01/0x08/0x10/0x40/0x40/0x40 and GprsCoverage 0x100000; Connected = the same bit when mobile data is the active network |
   | CDMA data 1xRTT / EV-DO (EVDO_0/A/B, eHRPD) | Available = 0x02 / 0x04 with GprsCoverage **clear** (Phone1xRttCoverage 0x10000000 is optional); §1.3, §2.1 |
   | mobile data in use (WMDroid) | post 0x480 wParam 0 to `HHTaskBar`, or the NE2000's ⇄ hides the letter (§2.1) |
   | Wi-Fi enabled / scanning results / connecting / connected | `WiFi` = 0x01 \| 0x02 [\| 0x04] [\| 0x08 or 0x10] (0x01 alone hides the icon) |

5. Alternatives considered:
   - **Configuring FakeRIL instead** (so ossvcs writes the right values itself). FakeRIL has the knobs `OperatorSignalQuality`,
     `OperatorLongName/ShortName`, `OperatorStatus`, `HomeOperator`, `OperatorSystemType` and `SIMInserted` inside a
     `FakeRILCSP` characteristic. That CSP is **not registered** in the 6.5.3 hive (no CLSID/InProcServer for `fakeril.dll`), and the
     storage format (`Operators.dat`, `FakeRadio.dat`) is unknown. Not recommended without more reverse engineering. [ROM][INFERRED]
   - Replacing the RIL driver needs native code (no toolchain here). A custom RIL would make every layer consistent and is the long-term option.

---

## 4. FakeRIL on 6.5.3 and the "No Service" problem

### 4.1 Image configuration [ROM]

`default.hv` (IMGFS, logical base sector 0x2C0) contains only:

```
[HKLM\Drivers\BuiltIn\RIL]   Dll=fakeril.dll  Prefix=RIL  Order=3  Index=1
[HKLM\Software\Microsoft\RIL] LastEquipmentState=2 (RIL_EQSTATE_FULL)  NetworkType=1  ComPort="VEI1:"  DataPort="VEI2:" (dead)
[HKLM\Comm\Cellular\RIL]      Contexts=3
[HKLM\System\State\Phone]     "Phone Status Flags"=0x10
```

There is no `FakeRIL`, `OperatorNames` or `Automation\Phone` key, and no `*.sim` or `*.dat` file in IMGFS. `MUX1`/`MUX2`/`VEI1..6` point to DLLs
that are not in the image. Registry and file names FakeRIL references in its strings: `Software\Microsoft\RIL`
(`DataModeCmdDelay`, `Timeout*`, `ModemRunTimeDetection`, `NetAdapter`, `RILAPIDelay`, `RILAPIDelayVariance`, `SignalRestorationTimeout`,
`NetworkType`, `Features`, `LastEquipmentState`), `Software\Microsoft\RIL\OperatorNames`, `Comm\Cellular\RIL\RemoteCalltype`,
`Automation\Phone`, `System\State\Hardware`, `\windows\FakeRadioATSupport.dll` (absent), `\release\Fakeril.sim`, and the files `FakeRadio.dat`,
`FakeSIM.dat`, `Operators.dat`, `SupSvcs.dat`, `CallBarring.dat` and `LockFac.dat`. Built-in operator: long name "Fake Network", short name "FN 0",
numeric "66660". [ROM]

### 4.2 What FakeRIL reports when registered (real DE, `Probe ril`) [OBS]

| Call | Result |
|---|---|
| `RIL_GetEquipmentState` | `{cb=0x14, params=7, dwEqState=2 FULL, dwRadioSupport=2, dwReadyState=0x1F}` |
| `RIL_GetRegistrationStatus` / `GetGPRSRegistrationStatus` | 2 (home) / 2 |
| `RIL_GetSignalQuality` | SS −65, Min −113, Max −51, BER 0xFFFFFFFF (unknown), Low −110, High −60 |
| `RIL_GetCurrentOperator` | "Fake Network" / "FN 0" / "66660" |
| `RIL_GetCurrentSystemType` | 0x08 (GSM). So `Cellular System Available` = 0 and no G icon on the stock DE |
| Notification at RIL_Initialize | 0x00800002 (radio presence) = 1 |

Resulting SNAPI state: `Status=0x009000A0`, `Signal Strength=100`, `Raw=90`, operator "Fake Network", `Radio Ready State=0x1F`.

### 4.3 Reproducing "No Service" on the real DE [OBS]

- `RIL_SetEquipmentState(1)` followed by `(2)` from an app: the notifications are REGSTATUS→1, GPRSREG→1, EQSTATE{1,1,0}, then EQSTATE{2,2,0}
  and {.., ready 0x1F}. FakeRIL then **stays unregistered**: regstat 1, signal unknown, `GetCurrentOperator` fails (0x8100065A), system type 0.
  ossvcs writes `Status=0x00200020`, `Signal Strength=10`, `Raw=0` and deletes the operator, and the Home screen shows "No Service". It was still like
  this after more than 60 s.
- A single `RIL_RegisterOnNetwork(AUTOMATIC)` gives REGSTATUS 3 (searching) then 2 (home) and GPRSREG 2 within about 1 s, and the indicators come back.
- Pressing the Power button twice (suspend/resume) shows "No Service", and it recovers within about 60 s without help. The phone stack issues the
  register request on resume.

**Conclusion:** FakeRIL has nothing that WMDroid must feed, such as a COM port, a timer device or a SIM device. It needs the stack to call
`RIL_RegisterOnNetwork` after the radio is at full power. A WMDroid guest stuck on "No Service" is therefore in one of these states:
(a) the register call never happened, for example because a suspend/resume or radio power transition went differently, or the boot-time
path was skipped; (b) it happened while FakeRIL was not ready, with `dwReadyState` ≠ 0x1F; or (c) FakeRIL's internal registration lag
(`OperatorRegistrationLag`) uses a wait or timer that never fires in WMDroid. [INFERRED]

### 4.4 Diagnosing and fixing in WMDroid

Run the probe in the WMDroid guest through the same shared folder and agent. Run one instance at a time:

1. `Probe dump wmdroid` records `Status`, `Radio Ready State`, `Signal Strength`, `CeGetCurrentTrust` and the policies.
2. `Probe ril` records equipment state, regstat and signal.
   - `dwEqState`≠2 means the radio is off or minimal. Check `LastEquipmentState` and whether WMDroid suspended the guest.
   - eq 2, ready 0x1F, regstat 1 means case (a). Go to step 3.
   - `dwReadyState`≠0x1F means FakeRIL/SIM init is stuck. Compare with §4.2.
3. `Probe reg 10`. If it reaches regstat 2 at once, the agent workaround (§3.2 step 1) fixes it. Separately, look at WMDroid's
   power/suspend emulation (power button, idle suspend, resume interrupt). If it stays at 3 (searching), the internal delay never
   completes, which is case (c). Look at WMDroid's OS timer / `Sleep` / wait-timeout emulation.

Side finding: the existing agent (`Samples\Agent`) exits when `\Storage Card` disappears during suspend. Its loop is
`while (Directory.Exists(@"\Storage Card"))`, so after one Power-button cycle `wmctl run`/`ping` time out until the agent is started again.
Making it wait for the card instead of exiting would fix this.

---

## 5. Physical MMIO from a .NET CF 3.5 app (VirtualAlloc + VirtualCopy)

### 5.1 Trust model on the DE 6.5.3 image [OBS][DOC]

- `CeGetCurrentTrust()` returns **2 = OEM_CERTIFY_TRUST** (fully trusted) in an unsigned CF exe started by the agent.
- `HKLM\Security\Policies\Policies` on the image includes `00001006` (4102 Unsigned Applications) = 1 (allowed),
  `0000101b` (4123 Privileged Applications) = 1 (one-tier: every app that runs is privileged), and `0000101a` (4122 Unsigned Prompt) = 1, with `00001001` = 1.
  The DE/SDK image is effectively unlocked.
- WM 6.5.3 runs on the CE 5.2 kernel, where `VirtualCopy` can be called from trusted user-mode code. On CE 6+ it is kernel-only. [DOC][INFERRED]

### 5.2 Verified call sequence [OBS]

The page containing `0x500FFFF0` (the DE `IOBattery` block) was mapped: `VirtualAlloc` → `0x01AB0000`, `VirtualCopy` → TRUE, and
`[0x500FFFF0]` read `0x01180000`. `VirtualFree` → TRUE.

```csharp
const uint MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000;
const uint PAGE_NOACCESS = 0x01, PAGE_READWRITE = 0x04, PAGE_NOCACHE = 0x200, PAGE_PHYSICAL = 0x400;
[DllImport("coredll.dll", SetLastError = true)] static extern IntPtr VirtualAlloc(IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);
[DllImport("coredll.dll", SetLastError = true)] static extern bool   VirtualCopy(IntPtr lpvDest, IntPtr lpvSrc, uint cbSize, uint fdwProtect);
[DllImport("coredll.dll", SetLastError = true)] static extern bool   VirtualFree(IntPtr lpAddress, uint dwSize, uint dwFreeType);

uint pa = 0x500F6000;                                   // must be page aligned (low 12 bits zero)
IntPtr va = VirtualAlloc(IntPtr.Zero, 0x1000, MEM_RESERVE, PAGE_NOACCESS);   // reserve only
bool ok = VirtualCopy(va, (IntPtr)(int)(pa >> 8), 0x1000,                       // PAGE_PHYSICAL => source is phys >> 8
                      PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL);
int v = Marshal.ReadInt32(va, 0x10);                    // 32-bit access at pa + 0x10
Marshal.WriteInt32(va, 0x10, 0x12345678);
VirtualFree(va, 0, MEM_RELEASE);
```

Rules:

- With `PAGE_PHYSICAL`, `lpvSrc` is the physical address **shifted right by 8**. The physical address must be page aligned.
- `cbSize` must be a multiple of the page size, and the destination must be a reserved (not committed) region of at least that size.
- Use `PAGE_NOCACHE` for device registers.
- Map once and keep the mapping. Don't call VirtualCopy per access.
- `Marshal.ReadInt32`/`WriteInt32` on aligned offsets give 32-bit accesses. [INFERRED]
- `0x500F6000` has no device on the real DE. Only exercise it on WMDroid, where the device exists.

---

## 6. Probe tool and how the data was obtained

- `WM 6.5.3 Professional Emulator\Samples\Probe\Probe.cs` (built with `.\wmbuild.ps1 Samples\Probe` → `Shared\Probe.exe`). Modes:
  `dump`, `set`, `del`, `watch`, `phys`, `ril`, `reg`, `eq` and `files`. The usage is in the file header. Run it with
  `.\wmctl.ps1 run Probe.exe <mode> <args>`, one instance at a time. Output is appended to `Shared\probe.txt`.
- SNAPI table: `Microsoft.WindowsMobile.Status.dll` IL, decoded read-only with a `System.Reflection.Metadata` dumper (session scratchpad `ildump\ildump.cs`).
- `ossvcs.dll`, `shell32.exe`, `cprog.exe`, `cmhome.dll`, `netui.dll` and others were extracted from the 6.5.3 WVGA image with `imgfs_dump.py` and
  disassembled with `armlite.py` (scratchpad). `ossvcs` `.text` loads at 0x02715000. The signal function is at 0x02743BEC and the floor loader at 0x02743384.
- §2.1 (CDMA letters): `shell32.exe`, `ossvcs.dll`, `netui.dll` and the icon DLLs (`shellres.192.dll`, `tapres.192.dll`) were
  disassembled with llvm-objdump and their resource sections listed and rendered with a small PE-resource walker. The WMDroid
  checks ran in `wmcore-cli` on the phone from `hb/boot.wms` with a test helper; values were written with temporary debug commands
  and then with the final `status` command, and the title bar was cropped from `--shot` frames.
- Screenshot sweeps covered the 48 `Signal Strength` values, 25 `Status`/cellular/Wi-Fi/BT combinations, and the Home-screen operator line. They ran on
  the real DE through `wmctl.ps1`. Every value was restored afterwards, and the emulator was stopped (it was cold-booted, so nothing persists).

## 7. Windows Mobile 5.0 Pocket PC Phone (`PPC_USA_GSM_VGA_VR.BIN`, CE 5.1.1700)

Checked in WMDroid's CLI (`wm5gsmvga.bin --mem 128 --screen 480x854`, the user's saved state and cold boots), with
`shell32.exe`, `cprog.exe`, `ossvcs.dll`, `netui.dll` and `fakeril.dll` disassembled. The WM 5 XIP modules are compressed
in ROM (section flag 0x2000; each 4 KB page is a block with a 16-byte header, not XPRESS or deflate), so their code was
read from the running guest instead: a small CF tool (`SetKMode` + `SetProcPermissions(~0)` + `ReadProcessMemory`, exe
slots from the toolhelp `th32MemoryBase`) wrote the decompressed pages to the card. Addresses below are slot-relative
(shell32 `.text` 0x11000, cprog 0x11000) or absolute for DLLs (ossvcs `.text` 0x02E21000, netui 0x024B1000).

### 7.1 Data letter in the title bar [DISASM][OBS]

The WM 5 taskbar does not read the registry itself. At start-up it subscribes to eight ossvcs properties (array at
shell32 0x12CBC: 0x2B0013, 0x60013 `Status`, 0x80013 `Signal Strength`, 0x750013 `Signal Strength Raw`, 0xC0013,
0x9001F `Current Operator Name`, 0x310013, 0x760013 `Cellular System Available`); ossvcs (same property table layout as
6.5.3, entries at 0x02E21CD0) reads them from `HKLM\System\State\Phone`, so plain registry writes reach the taskbar. On
each notification the collector at 0x33E4C rebuilds a 7-DWORD state (0x4E7F8): word 0 maps `Status` bits (GprsCoverage
0x100000 → 0x2000, Phone1xRttCoverage 0x10000000 → 0x8000, ActiveDataCall 0x1000 → 0x20, ...), word 6 is the low half
of `Cellular System Available`. There is no `Cellular System Connected` and no `Hardware\WiFi` in WM 5's taskbar.

The data-slot function (0x343FC) picks, in order:

1. 0x480 flag with lParam ≠ 0 (0x4E814), set by netui → connection icon.
2. Active data call (`Status` 0x1000), or the 0x480 lParam = 0 flag (0x4E818, the wired network, the same message
   as 6.5.3: netui posts 0x480 wParam 1/0 lParam 0 to `HHTaskBar`), or `Hardware\Cradled` (taskbar message 0x485) → the
   two-arrow connection icon (tap: "Connectivity: Network Card Connected"). Posting 0x480 wParam 0 lParam 0 clears it.
3. Otherwise a letter from tapres: Available & 0x08 → **E** (0x56C6); Available & 0x04 → **Ev** (0x56C7); `Status`
   GprsCoverage → **G** (0x56C0); `Status` Phone1xRttCoverage → **1X** (0x56C1); none → "⇄×".

So WM 5 has **no 3G or H letter** (0x10 / 0x40 are ignored, and GprsCoverage then shows G: the user's "G on LTE").
The helper adds bit 0x08 for 3G/H on WM 5 (CE 5.1), so UMTS/HSPA/LTE/NR show **E**, the best letter there. The app
sends the same `data=` values for every OS. Seen in screenshots: `data=H` → E, `data=3G roam=1` → roaming triangle + E,
`data=EV` → Ev, `data=1X` → 1X, `data=G` → G. WM 6.5.3 with the same helper still shows H / Ev / 3G / E.

### 7.2 Operator name [OBS][DISASM]

- Title-bar bubble (tap the antenna) and SNAPI: `Current Operator Name`, as on 6.5.3 (the helper's value shows).
- **Phone app (cprog):** the top line is not text. cprog's skin (control table at 0x11740) draws `bmpNormal` =
  `\Windows\carrierlogo.gif`, which the ROM ships as a 220x34 GIF with red (197,39,13) "Mobile Operator" text on a
  transparent background; that is the "mobile operator" the user saw. The string is in no resource. cprog also writes
  `Status`/`Current Operator Name` itself through ossvcs ordinals #68/#66 on RIL registration events.
- A file written to `\Windows\carrierlogo.gif` shadows the ROM file and cprog shows it. The helper (WM 5 only) draws the
  operator name the same way (Tahoma, same red, shrunk to fit 220 px) into a 2-colour GIF89a (uncompressed 9-bit LZW)
  whenever the operator changes, remembers it in `HKLM\Software\WMDroid\CarrierLogo`, and deletes the RAM copy when
  there is no operator (the ROM placeholder comes back). Verified: "Verizon", "T-Mobile Wi-Fi Calling".
- cprog loads the picture once, when its window is first created; a later change shows after the next WM restart
  (`WM_SETTINGCHANGE` 0x3002 broadcast does not reload it).
- FakeRIL's `Software\Microsoft\RIL\OperatorNames` (value = numeric code, data = name) is only used when FakeRIL builds
  operator entries from SIM data (fn 0x0163200C), not for `RIL_GetCurrentOperator`, so it cannot rename "Fake Network".

### 7.3 The stuck wait cursor [OBS]

The .NET Compact Framework starts every program with `IDC_WAIT` (0x7F02) set on its main thread and resets it when the
first form is shown. The helper's `serve` / `install` / `update` modes never show a form, and WM 5 keeps drawing the
four-colour wait cursor in the middle of the screen for as long as such a thread lives: from the first boot on (the
StartUp shortcut starts `WMDroid.exe serve`), and therefore in every saved state. `GetCursor()` on the helper's mailbox
thread returned 0x7F02; `Cursor.Current = Cursors.Default` (or `SetCursor(NULL)`) **on that thread** removed it at once.
Calling `SetCursor` from another process does not help. Neither `wceload /noui` (the native bootstrap's CF install) nor
shell32's `ShellExecuteEx` (it saves and restores the caller's cursor, 0x30D5C/0x30EF0) leaves it behind. Fix: the helper
sets `Cursor.Current = Cursors.Default` at the start of those modes. Cold boot from the ROM with the old helper: cursor at
90/150/195 s; with the fix: none. A saved state that has the cursor loses it when the new helper takes over (the old
helper process ends).

## 8. What works with no guest helper (hardware paths) [OBS][DISASM]

Checked 2026-10-16 in `wmcore-cli` on the phone (6.5.3 FWVGA from `hb/boot.wms`, 6.1.4 WVGA cold boot with the virtual
modem, WM5 GSM VGA from a copy of the user's state), with the helper removed (card `helper.exe` deleted, then `update`,
so the helper exits and nothing restarts it) and an app pause (`--pause`) afterwards.

| | 6.5.3 / WM5 (FakeRIL images) | 6.1.4 (rilgsm + virtual modem, `core/src/board/modem.cpp`) |
|---|---|---|
| Signal bars, operator, No Service, roaming | **Helper** (`status`). No radio path exists: FakeRIL simulates its own network inside the guest and the patch unloads it. With FakeRIL unloaded nothing rewrites the values, so the last ones stay correct across helper death and pauses; after a guest reboot ossvcs writes "Phone off" (`Status 0x00200010`) until the helper writes again. | **Modem** (RIL): `Status 0x009000A0`, operator from `+COPS`/CPHS, `+CIEV` bars, roaming (`0x001002A0`, Home bit cleared) all written by ossvcs with no `status` command. Android level 1 currently maps to 0 bars (`+CIEV: 2,2` = −98 dBm → raw 24 < 25). |
| G/E/3G/H letter | Helper | **Helper**: rilgsm has no input for the radio technology (its `+CGREG?` parser reads only `<n>,<stat>`; no `*ERINFO`/AcT), so `Cellular System Available` stays 0 |
| Wi-Fi icon | Helper: no DE image has a WLAN adapter, and WZC needs a miniport that answers 802.11 OIDs (the NE2000 driver is Microsoft's) | Helper |
| "⇄" wired icon over the data slot | **Gone**: the patch (v7, `Patch.NoWiredUi`) stops `ethman.dll` | same |

**`ethman.dll` = the 0x480 sender.** `Drivers\BuiltIn\Ethman` (Prefix ETM, Flags 2) is a 5.8 KB driver that imports only
`GetAdaptersInfo`, Winsock address-change ioctls and netui's `AddNetUISystrayIcon` / `RemoveNetUISystrayIcon` /
`UpdateConnectionStatus`. Those netui methods (vtable at netui 0x7BF02BD0, slots 0x7BF14DA8 / 0x7BF14DC8) post `0x480`
wParam 1 / 0 to `HHTaskBar`. Connection Manager does not use it. With Flags |= `DEVFLAGS_NOLOAD` (4) and one restart, the
data slot shows the SNAPI letter (H after `status data=H`) and stays that way with the helper gone; the helper's 0x480
repost is now only a fallback for images where the patch did not run.

**Buttons 1-4.** `kbdmouse.dll` (matrix table at its data section +0x168: codes 0x21 and 0x64 → VK 0xC1, 0x65..0x68 →
0xC2..0xC5) compares every VK with 0xC1 in code (0x7B3DB7BC) and keeps it as its Fn state, so Win+VK_APP1 can never be
produced; no Fn (21 pairs) or NumLock (15 pairs) entry generates 0xC1 either. The stock Shell\Keys of WM5, 6.1.4 and 6.5.3
are Button 1 Start (1), 2 Rotate (0xC), 3 Context Menu (0xB), 4 OK/Close (0xA). The app therefore sends each button as a
stock key (`Keymap.HwButtons`): Win for Start, F1/F2 for the soft keys, arrows for scrolling, Win+VK_APP2..4 for Rotate,
Context Menu and OK/Close (from any button), the button's own hotkey for "Windows Mobile's own setting". Verified with no
helper: Win → Start menu, F1 → Contacts / Phone, Win+VK_APP2 → rotate and back, Win+VK_APP4 → closes Contacts / Phone,
Win+VK_APP3 after the helper stored Home in 40C3 → Today. Only choices with no stock key (Home, Input Panel, an action no
hotkey holds any more, WM's own program on Button 1) need the helper to store them once (Buttons.cs `buttons sync`); Button 1
then moves to 40C5.

## Sources

- Using the State and Notifications Broker in Native Code (snapi.h layout, PhoneNoSim bitmask 2, battery word layout): https://learn.microsoft.com/en-us/previous-versions/bb154479(v=msdn.10)
- Security Policy Settings (4102 unsigned apps, 4122 unsigned prompt, 4123 privileged apps): https://learn.microsoft.com/en-us/previous-versions/aa455966(v=msdn.10) and https://learn.microsoft.com/en-us/previous-versions/bb416355(v=msdn.10)
- Security Policies for Windows Mobile 5.0 and 6: https://technet.microsoft.com/en-us/library/cc182305.aspx
- Background on SNAPI signal values (percent vs raw): https://social.msdn.microsoft.com/Forums/silverlight/en-US/e5f6bee6-e6bc-4f68-bc14-7e07690cbb87/wm-50-snapi-how-is-phonesignalstrength-actually-derived?forum=vssmartdevicesvbcs ; https://www.42gears.com/nl/?p=190
- RIL structure and constant names (`RILSIGNALQUALITY`, `RIL_EQSTATE_*`, `RIL_REGSTAT_*`, `RIL_NOTIFY_*`, `RIL_SYSTEMTYPE_*`) are from `ril.h` as remembered and checked against the observed data. Treat the names as [INFERRED] and the values as [OBS].
