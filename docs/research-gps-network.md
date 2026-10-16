# GPS (NMEA over serial) and internet access for the DE images

Research notes for WMDroid. Main target: WM 6.5.3 Professional `PPC_USA_GSM_FWVGA_VR.BIN`. Companion to
`research-de-hardware.md` and `research-cellular.md`.

Tags:

- **[ROM]**: read out of the 6.5.3 image (IMGFS files, `default.hv` hive, XIP `boot.rgu`, module strings). Where it
  says "(disasm)", the claim comes from an ARM disassembly of a module. Nothing was executed. The FWVGA image and the plain
  `PPC_USA_GSM_VR.BIN` image have identical registry keys for everything below.
- **[DE-SRC]**: the DE 1.0 shared source (`gitntel/Microsoft-Device-Emulator`: `boards/smdk2410/devices.cpp`,
  `boards/pcmciadevices.{h,cpp}`, `boards/vpcnet.cpp`). It is academic-license and Windows-only, so the facts below are
  register behaviour, not code. Don't copy it.
- **[DOC]**: Microsoft docs, blogs, or the local `DeviceEmulator.chm`. **[EXT]**: other public sources.
- **[CODE]**: WMDroid's own source as it is today.
- **[INFERRED]** / **[GUESS]**: reasoning that has not been verified.

---

## TL;DR

| | Finding |
|---|---|
| GPS port | **COM1 = UART0** (`0x50000000`, IRQ 28, `serial_smdk2410.dll`). **It is unused on 6.5.3**, because FakeRIL doesn't touch it and `caifmux.dll` is absent. Use it for GPS. [ROM] |
| GPSID | Present: `gpsid.dll`, `gpsapi.dll`, the `GPD0:` device, and the "External GPS" applet in `cplmain.cpl`. The registry has empty `Drivers`/`Multiplexer` keys, so GPSID loads but has no input until configured. [ROM] |
| NMEA | GPSID parses **`$GP` GGA, RMC, GSA, GSV, GLL** (talker must be `GP`). Send one burst per second. No modem lines needed. [ROM] |
| UART model | Already close. It needs level-triggered RX, a thread-safe host feed queue, and (for fidelity) the RX-FIFO-full bit. Baud rate doesn't matter. [CODE][DE-SRC] |
| GPS effort | **About 2–3 days.** |
| NIC | **NE2000 PC Card behind a Cirrus PD6710 PCMCIA controller.** The 6.5.3 image ships `ne2000.dll`, `pcc_smdk2410.dll` and `pcc_serv.dll`. There is no CS8900 driver, no VMINI, and no DMA-transport NIC. [ROM][DE-SRC][DOC] |
| Host side | **libslirp** (BSD/MIT). Its glib dependency builds with the NDK; Termux ships both. Two catches: on Android, DNS needs a small patch or interception, and slirp runs on its own thread. [EXT][INFERRED] |
| Connection Manager | Handles the NE2000 like any network card through `cspnet.dll`. Set "My network card connects to: **The Internet**". The cellular "G/3G" icon will **not** reflect this traffic without extra work (§B3). [ROM][INFERRED] |
| Network effort | **About 1–1.5 weeks** with libslirp. A TLS-upgrading proxy adds 2–5 days. |

---

# A. GPS

## A1. Serial ports in the 6.5.3 BSP

| Guest name | Driver key | Hardware | State on 6.5.3 |
|---|---|---|---|
| **COM1:** | `Drivers\BuiltIn\Serial`: `Dll=serial_smdk2410.dll`, `MemBase=0x50000000`, `MemLen=0x2C`, `Irq=28`, `InterruptBitsShift=0`, `ISTTimeouts=0x200`, `DeviceArrayIndex=0`, `Port="COM1:"`, `FriendlyName="Serial Cable on COM1:"`, `Tsp=unimodem.dll` | UART0 | **Loads and is free.** The only other would-be users are `MUX1`/`MUX2` (`caifmux.dll`, `CommPort=1/2`), and that DLL is not in the image, so those keys fail to load. [ROM] |
| COM3: | `Drivers\BuiltIn\IRDA2410`: `Dll=IRDA2410.Dll`, `IoBase=0x50008000` (UART2), `Irq=9`, `Port="COM3:"` | UART2 | **Dead.** `IRDA2410.dll` is in neither the IMGFS nor the XIP module list. [ROM] |
| (none) | — | UART1 `0x50004000` | No guest driver. It is the OAL debug serial. DE sends it to the console (`/c`). [ROM][DE-SRC] |
| SER6: ("Serial over DMA") | `Drivers\BuiltIn\SERDMA`: `SERDMA.DLL`, DMA channel 6 (`Drivers\BuiltIn\DMA6`, `dmatrans.dll`, Irq 38) | DE DMA transport | Used by ActiveSync. Not a UART. [ROM] |
| PC Card COMx | `Drivers\PCMCIA\Serial`, `...\Modem`, assorted named cards | PCMCIA | Only loads if a serial card is "inserted". Not relevant here. [ROM] |

- DE maps `/u0` to UART0, which is guest COM1. Barry Bond's GPS post configures exactly this: Serial Port 0 → host GPS,
  then in WM Settings → System → GPS → Hardware, choose COM1 at 4800 baud. [DOC]
- The DE help's generic wording ("Serial port 0 … used by the OS for debug output", "Serial port 2 … IRDA") describes
  plain CE images. In the WM images, COM1 is the product port. [DOC][ROM][DE-SRC]
- `Comm\DefaultConnections` also lists "Serial Cable on COM1:" for direct-cable RAS. That is only used on demand. [ROM]

## A2. GPS Intermediate Driver (GPSID) on 6.5.3

What the image contains [ROM]:

```
[Drivers\BuiltIn\GPSID]  Dll=GPSID.dll Prefix=GPD Index=0 Order=16 Keep=1 Flags=2      -> GPD0: (used by gpsapi.dll)
[System\CurrentControlSet\GPS Intermediate Driver]                 (no values)
[System\CurrentControlSet\GPS Intermediate Driver\Drivers]         (no CurrentDriver)
[System\CurrentControlSet\GPS Intermediate Driver\Multiplexer]     (no DriverInterface)
[System\CurrentControlSet\GPS Intermediate Driver\Multiplexer\ActiveDevice]
                         Dll=GPSID.dll Keep=1 Flags=2 Context=0x12344160   (no Prefix/Index)
```

- Modules present: `gpsid.dll`, `gpsapi.dll`, and `cplmain.cpl`, which contains the "GPS Settings" applet (`CPL_GPSID`, with
  help anchors `gpshardware`/`gpsaccess`/`gpsintegration`). This is the Settings → System → **External GPS** page. [ROM]
- Registry value names that `gpsid.dll` reads, from its strings [ROM]:
  - Root: `IsEnabled`, `CurrentLogFile`, `OldLogFile`, `MaxLogFileSize`, `AllowPlugAndPlay`.
  - `Drivers`: `CurrentDriver`.
  - Per-driver: `InterfaceType` (`COMM` / `File` / `Poll`), `CommPort`, `File`, `PollInterval`, `FriendlyName`,
    `InputBufferSize`, `OutputBufferSize`, `RetryWaitOnError`, `SleepOnNoData`, `MinReadSize`, `MaxReadSize`,
    `ReadInterval`, `Iterations`, plus DCB/timeout overrides (`Baud`, `ByteSize`, `Parity`, `StopBits`, `fInX`, `fOutX`,
    `fOutxDsrFlow`, `fOutxCtsFlow`, `fDtrControl`, `fRtsControl`, `fBinary`, `fParity`, `Read/Write*Timeout*`).
  - `Multiplexer`: `DriverInterface`, `MaxBufferSize`. `ActiveDevice`: `Prefix`, `Index` (defaults `COM` / `GPD`).
- Microsoft's documented layout [DOC]:
  - `Drivers\CurrentDriver` names a subkey.
  - That subkey holds `InterfaceType="COMM"`, `CommPort="COMn:"` and `FriendlyName`.
  - `Multiplexer\DriverInterface` is the virtual port that raw-NMEA applications open. It must start with `COM` or `GPD`;
    Microsoft suggests `GPDx` or `COM10+`.
  - Applications that use `GPSGetPosition` don't care about `DriverInterface`.
- `Drivers\State\Gps` is **not** referenced by `gpsid.dll`, so nothing needs setting there. [ROM]

**Configuration to apply.** Either the user does it in External GPS (Hardware: COM1, any baud; Programs: COM4; Access:
"Manage GPS automatically"), or the guest Helper does it with its existing `regset` command:

```
HKLM\System\CurrentControlSet\GPS Intermediate Driver            IsEnabled      dword 1
HKLM\System\CurrentControlSet\GPS Intermediate Driver\Drivers    CurrentDriver  sz    "WMDroid"
HKLM\System\CurrentControlSet\GPS Intermediate Driver\Drivers\WMDroid
                                                                 InterfaceType  sz    "COMM"
                                                                 CommPort       sz    "COM1:"
                                                                 FriendlyName   sz    "WMDroid phone GPS"
HKLM\System\CurrentControlSet\GPS Intermediate Driver\Multiplexer
                                                                 DriverInterface sz   "COM4:"   (raw NMEA port for old apps)
HKLM\...\Multiplexer\ActiveDevice                                Prefix sz "COM", Index dword 4   [INFERRED: must match DriverInterface]
```

GPSID reads this when it starts, so do a soft reset afterwards. The CPL can refresh it live instead. The RAM hive survives
soft resets and WMDroid snapshots, but a cold boot from ROM loses it, so the Helper should re-apply it at boot. [INFERRED]

## A3. What to send, and what the UART model needs

**NMEA content** [ROM: `gpsid.dll` contains the tag list `GP`, `GLL`, `GSV`, `GSA`, `RMC`, `GGA` and `%02X`]:

- **The talker must be `GP`.** Android's raw NMEA (`OnNmeaMessageListener`) usually emits `$GN…`, `$GL…` or `$GA…`, so
  either rewrite the talker and checksum, or (better) **synthesize sentences** from `Location` plus `GnssStatus`.
- Always send a correct `*hh` checksum and end lines with CRLF. [INFERRED: `%02X` suggests GPSID checks or produces checksums]
- One burst per second, in this order: `GGA`, `GSA`, `GSV` × n, `RMC` (about 400–600 bytes). [INFERRED]

| Sentence | Feeds `GPS_POSITION` fields |
|---|---|
| **RMC** | UTC time+date, status A/V, lat/lon, speed (knots), course, magnetic variation |
| **GGA** | Fix quality (0/1), satellites used, HDOP, altitude MSL, geoid separation (use `Location.getMslAltitudeMeters()` on API 34+, else 0) |
| **GSA** | Mode A, fix type 1/2/3, up to 12 used PRNs, PDOP/HDOP/VDOP |
| **GSV** | Satellites in view (4 per sentence): PRN, elevation, azimuth, SNR (`GnssStatus.getCn0DbHz`) |
| GLL | Optional |

Notes on the fields:

- Android gives accuracy in metres, not DOP. A workable approximation is HDOP ≈ `accuracy / 5`, VDOP from
  `verticalAccuracyMeters`, and PDOP = √(H² + V²). [INFERRED]
- Map non-GPS satellites into the 33–99 PRN range, or report only GPS PRNs (1–32). Old parsers choke on 3-digit PRNs. [INFERRED]
- With no fix, keep sending `RMC …,V,…`, `GGA` quality 0 and `GSA` fix 1, so apps show "searching" instead of "no device".

**Modem-status lines:** none needed. GPSID's DCB defaults don't use DSR/CTS flow control (they can be overridden per driver, see above).
The model already reports CTS (`umstat=1`). DE drives UART0 DTR from GPH6 and reads DSR on GPH7 (active-low). Mirror that
only if some app insists on it. [CODE][DE-SRC]

**UART model** (`core/src/board/s3c2410.{h,cpp}` `Uart`) [CODE] compared with DE [DE-SRC]:

1. **Make RX level-triggered.** Today `feed()` raises the RXD sub-interrupt once per byte (an edge). DE re-asserts the RX
   sub-source whenever data is pending after INTPND is cleared. Without that, any bytes the guest's ISR doesn't drain wait
   for the next byte or for the driver's `ISTTimeouts` = 512 ms poll. Fix: when SUBSRCPND RXDn is cleared, or on
   `IntController::update()`, re-set it while `!rx.empty()` and RX interrupts are enabled.
2. **Thread safety.** `feed()` touches `std::deque` and `IntController` directly. Location callbacks run on an Android
   thread, so queue the bytes under a mutex (like `inMu` in `jni.cpp`) and drain them on the emulation thread.
3. Set UFSTAT bit 8 (RX FIFO full) when 16 or more bytes are pending. Keep UTRSTAT bit 0, UERSTAT = 0, and TX instant.
4. **Baud doesn't matter**, because UBRDIV is ignored. Optionally pace delivery to about 1 KB/s to look like 9600 baud. Not
   required.
5. UART0 `onTx` in `jni.cpp` currently logs to logcat. GPSID doesn't write to the GPS port, but when GPS is active, route
   TX to a sink.

## A4. Steps and effort (about 2–3 days)

1. Core (0.5 d): UART RX level re-assert, RX-full bit, and `Machine::uartInput(idx, bytes)` with a mutexed queue drained in
   the run loop. JNI: `NativeBridge.uartWrite(handle, 0, byte[])`.
2. App (1 d):
   - Manifest: `ACCESS_FINE_LOCATION` and `ACCESS_COARSE_LOCATION`, with a runtime request.
   - `LocationManager.requestLocationUpdates(GPS_PROVIDER, 1000 ms)` plus `registerGnssStatusCallback`.
   - An `NmeaSynth` that emits GGA/GSA/GSV/RMC every second, with or without a fix.
   - A Settings toggle, "Phone GPS on COM1".
3. Guest (0.5 d): Helper applies the A2 keys at boot and soft-resets if they changed. Alternatively, document the
   External GPS steps.
4. Test (0.5–1 d): the WM6 SDK GPS sample (C#, uses `gpsapi`), GPSID logging (`CurrentLogFile`), and Google Maps Mobile
   ("Windows Mobile GPS" mode, or COM4).
5. Fallback, with no UART work at all: GPSID `InterfaceType="File"` reading NMEA from `\Storage Card`, like FakeGPS. It is
   playback-only, so it's not suitable for live position. [DOC][INFERRED]

---

# B. Internet access

## B1. Network hardware and guest drivers

**What DE emulates** [DE-SRC][DOC]:

| Device | Where | Guest driver in 6.5.3 | Use |
|---|---|---|---|
| **NE2000 PC Card** (`/p`) | PCMCIA slot behind a Cirrus **PD6710**. Controller at PA `0x110003E0` (index) / `0x110003E1` (data). Card I/O at PA `0x11000000 + port`. Attribute/common memory at PA `0x10000000+`. | **Yes**: `ne2000.dll`, `pcc_smdk2410.dll`, `pcc_serv.dll` | **The one to implement.** DE help: "Enable NE2000 PCMCIA Network Adapter … Either the OS or KITL can use the NE2000 for general networking." |
| CS8900A (`/n`) | PA `0x18000000` / `0x19000300`, EINT9 | **No** driver in the image | DE help: "Windows Mobile OS images cannot use the CS8900 card for general networking because they do not include CS8900 drivers." |
| "VPC network" | Host side only (VMNetSrv virtual switch, `vpcnet.cpp`) | — | Bridges NE2000/CS8900 frames to a host NIC. Not a guest device. |
| VMINI | — | Not in the image | `VMINI1` is the KITL virtual-miniport name. It appears only as FakeRIL's default `NetAdapter` (§B3). |
| DMA transport | `0x500F0000` | `dmatrans.dll` + `SERDMA` | Used by ActiveSync (+ DTPT pass-through `RNDISFN1` 169.254.2.x). Internet through it would need a full ActiveSync host. **Not recommended.** |

**Guest registry** [ROM]:

```
[Drivers\BuiltIn\PCCARD]       Dll=pcc_serv.dll Order=0
[Drivers\BuiltIn\PCC_SMDK2410] Dll=pcc_smdk2410.dll ControllerFnInt=3 FnIrq=36 ForceFnSysIntr=1 CSCIrq=3 PollTimeout=60000
   \WindowEntry1  memory window base 0x10000000, 4 KB..8 MB
   \WindowEntry2  I/O window 0x11000000..0x1100FFFF, granularity 1
[Drivers\PCMCIA\Detect\60]     Dll=NE2000.DLL Entry=DetectNE2000      (card is matched by CIS → DetectNE2000)
[Drivers\PCMCIA\NE2000]        Dll=ndis.dll Prefix=NDS Miniport=NE2000
[Comm\NE2000]                  ImagePath=ne2000.dll Group=NDIS   \Linkage Route=["NE20001"]
[Comm\NE20001\Parms]           BusType=8 (PCMCIA) IoBaseAddress=0x300 InterruptNumber=3 Transceiver=3 CardType=1 EightBitIO=1
[Comm\Tcpip\Parms]             EnableDHCP=1 TcpWindowSize=32768
[Drivers\BuiltIn\Ethman]       ethman.dll (ETM)   -- adapter watcher → netui systray / connection status
```

`ne2000.dll` is the stock CE NDIS miniport ("Novell 2000 Adapter", `NdisMRegisterIoPortRange`, `CardRequestConfiguration`). [ROM]

**The interface the guest expects.** This is behaviour, summarised from DE [DE-SRC]. Everything is byte or halfword access.

*PD6710 (ExCA) at index/data `0x110003E0/E1`:*

| Index | Behaviour |
|---|---|
| 0x00 | Revision: read **0x83** |
| 0x01 | Status: CD1, CD2 = 1 when a card is present; READY = 1 when power-control bits 0x90 are both set |
| 0x02 | Power control. 0x90 set → card power-on; clear → power-off |
| 0x03 | Interrupt/general control. Card IRQ select is non-zero when card interrupts are wanted |
| 0x04 | Card status change. Read-to-clear; bit 3 = card detect changed |
| 0x05 | Status-change interrupt config. If card-detect is enabled, insertion raises **EINT3** |
| 0x06 | Window enable: I/O map 0 and 1 enable bits |
| 0x07 | I/O window control |
| 0x08–0x0F | I/O map 0/1 start/end (expects 0x300–0x31F/0x33F) |
| 0x10–0x15 | Memory map 0 (0..0xFFFF, offset 0) |
| 0x16 | Read 1 (5 V card) |
| 0x1F | Chip info: first read after reset or write returns top bits `11`, later reads `00` |
| others ≤ 0x3F | Store and ignore |

*Card I/O:* an access at PA `0x11000000 + p` reaches the card when `p` lies inside an enabled I/O map. The card sees
offset `p − mapStart`, so the NE2000 lives at `0x11000300`.

*Card attribute memory (window at `0x10000000`):*

- CIS tuples at even offsets (byte i at 2i). It is a standard NE2000 CIS: function ID 6 (network), config base 0x3F8,
  config entries for I/O 0x300/0x320/0x340/0x360 (length 0x20), manufacturer ID 0x8A01. Any equivalent CIS that
  `DetectNE2000` accepts will do.
- FCSR at 0x3FA (bit 1 = interrupt pending, bit 0 = ack, which clears it). Writes to 0x3F8/0x3F9 are ignored.
- Card RAM is also visible at 0x4000–0x7FFF. Unknown halfword reads return 0 (the driver's RAM probe).

*NE2000 (DP8390):*

- Registers 0x00–0x0F in 3 pages selected by CR.PS.
- Remote-DMA data port at **+0x10**: 8-bit, because `EightBitIO=1`, or 16-bit when DCR.WTS is set.
- Reset at **+0x1F**. A read or write puts the NIC in stopped state with ISR.RST set.
- Buffer RAM is 16 KB at NIC addresses 0x4000–0x7FFF (pages 0x40–0x7F).
- PROM at NIC addresses 0–31: each MAC byte is doubled, and bytes 14/15 = `'W','W'` (marks a 16-bit card).
- Reading ISR while CR.STP is set and RST is clear sets RST. The driver's init polls for this.

*Receive:*

1. Put a 4-byte header {status = 1, next page, length LE (including the header)} at CURR, then the frame, wrapping PSTOP → PSTART.
2. If the ring would overrun BNRY, raise OVW instead.
3. Otherwise set CURR = next page and raise PRX.
4. Filtering: directed, plus broadcast if RCR.AB, plus multicast via the MAR hash if RCR.AM.

*Transmit:* CR.TXP sends TBCR bytes from page TPSR (when DCR.LS = 1 and TCR is not loopback), then raises PTX.
Remote DMA end raises RDC.

*Interrupt:* **EINT8, level-triggered**, asserted while `(ISR & IMR) != 0`. It reaches the CPU through EINT8_23 (IRQ 5).
The PD6710's card IRQ select value doesn't matter.

**WMDroid today** [CODE]:

- `machine.cpp` maps a logging `Probe` (a plain RegFile) at 0x10000000 (32 MB) and at 0x18000000.
- `Gpio::raiseExt()` is edge-only. A **level EINT8** path, re-asserted while the card IRQ is high, is needed.
- Nothing NIC-related is in snapshots yet.

## B2. Host side on Android (no root)

The emulated NIC exchanges raw Ethernet frames. The host needs ARP, DHCP, DNS and TCP/UDP NAT in user mode.

| Option | Pros | Cons | Effort |
|---|---|---|---|
| **libslirp 4.9.x** (BSD-3/MIT) | Full ARP/DHCP/DNS-forward/TCP/UDP/ICMP NAT, host-forward and guest-forward hooks. Proven as QEMU's user net. Its API is frame-in/frame-out plus poll and timer callbacks. | Needs **glib** (LGPL-2.1: ship it as a `.so`) plus libffi, pcre2, libiconv and zlib. **Termux builds both with the NDK** (`packages/libslirp`: 4.9.5, depends on glib), so it is known to build. | 1–2 d to cross-build (meson cross-file, or reuse Termux recipes), plus 1–2 d to integrate |
| lwIP-based splice (tun2socks pattern: lwIP (BSD) terminates guest TCP; host sockets do the real I/O) | No glib; small | You write ARP, a DHCP server, DNS and UDP NAT yourself. More glue and edge cases. | 1–2 weeks |
| Custom minimal NAT (ARP + DHCP + DNS proxy + UDP map + own TCP state machine) | No dependencies; total control | Getting TCP right (windows, retransmit, FIN/RST, MSS) takes time. Reference code like NetGuard is GPLv3. | 2–3 weeks |

**Recommendation: libslirp.**

Android-specific notes [INFERRED unless tagged]:

- **DNS.** libslirp finds the host resolver through `/etc/resolv.conf`, which Android lacks. Termux patches around this
  [EXT]. Either patch `get_dns_addr`, or answer UDP/53 to the virtual DNS (10.0.2.3) in WMDroid via
  `android_res_nquery` (API 29+) or `getaddrinfo` before frames reach slirp.
- **Threads.** Run slirp on its own thread (`slirp_pollfds_fill` / `poll` / `slirp_pollfds_poll`, with timers through
  `SlirpCb`). Hand frames between the emulation thread and slirp through mutexed queues plus an eventfd wakeup. Slirp is
  not thread-safe.
- **Permissions.** Needs the `INTERNET` permission, which the manifest doesn't have yet. ICMP echo works through Android's
  unprivileged ping sockets.
- **Snapshots.** The NIC registers and ring buffer can be serialised. Slirp's TCP state can't, so on restore just reset the
  NAT and let guest connections time out.
- **Defaults.** Guest 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3, assigned by slirp's DHCP. The image already has
  `EnableDHCP=1`. [ROM]

## B3. Connection Manager, and whether the cellular data path can carry it

**Network card path** [ROM][DOC][INFERRED]:

1. `ethman.dll` sees `NE20001` come up and get a DHCP address.
2. `netui` shows it under Settings → Connections → Network Cards.
3. The NIC connection service provider is `cspnet.dll` (`Comm\ConnMgr\Providers\{f792e23c-dc19-4668-9be4-f8688b4c18d6}`,
   `WakeOnIncoming = WiFi, Ethernet, DTPT`). It reads per-adapter `SrcId`/`DestId` (`CM_NetEntries`) to decide whether
   the card reaches **The Internet** (`{436EF144-B4FB-4863-A041-8F905A62C572}`) or **Work**
   (`{A1182988-0D73-439e-87AD-2A5B369F808B}`).
4. Set "My network card connects to: The Internet", either in the UI or with a `CM_NetEntries` provisioning XML from the
   Helper. IE Mobile, GMM and other ConnMgr clients then use it. ConnMgr prefers it over GPRS while it is up.

**Cellular path on 6.5.3** [ROM, partly from disassembly]:

- 6.5.3 packet data is NDIS-based. `wwan.dll` (`Drivers\BuiltIn\WWN`) creates a virtual Ethernet adapter `WWAN<n>`
  through `veim.dll` (`Comm\VEM`, "Virtual Ethernet Intermediate Miniport") for each RIL context. `cspwwan.dll` is its
  ConnMgr provider. There is no PPP over a serial data port here.
- FakeRIL fakes the GPRS context and contains the strings `WWAN`, `192.168.1.1` and `NdisGprsAuthInfo`. From the disassembly:
  1. It reads `HKLM\FAKERIL\NetAdapter`, with ASCII default `"VMINI1"`.
  2. It watches `SIO_ADDRESS_LIST_CHANGE`. When an adapter reports IP **192.168.1.1** (the fake WWAN context), it looks up
     an adapter whose name **starts with `"PASS\" + NetAdapter`** using `GetAdaptersInfo`.
  3. It then calls `CreateIpForwardEntry`/`SetIpForwardEntry`. One route is a default route (metric 60) via that real
     adapter's gateway. The other sends the real adapter's subnet to the WWAN interface (metric 10).
  4. In other words, it was built to carry fake-GPRS traffic over a real NIC on Microsoft's test setup, where VMINI1 sat
     under a passthrough driver named `PASS`.
- **In this image no adapter is named `PASS\…`**, so the trick does not fire and the "G" connection carries no real
  traffic. [INFERRED]
- **Possible experiment [GUESS]:** set `Comm\NE2000\Linkage\Route` to `"PASS\NE20001"`, so that NDIS would open the
  nested key `Comm\PASS\NE20001\Parms`, and set `HKLM\FAKERIL\NetAdapter = "NE20001"`. If NDIS accepts that instance
  name, connecting "GPRS" in the guest would re-route through the NE2000 and the cellular icon would match real
  connectivity. Try it only after the NIC works, and keep the plain network-card setup as the default.
- The reliable way to make the cellular icon real is a WMDroid replacement RIL DLL dropped in by the Helper. That is the
  same idea as `research-cellular.md` §6, and it is out of scope here.

## B4. Plan and effort (about 1–1.5 weeks)

1. **PD6710 + NE2000 device** (2–4 d):
   - Replace the `0x10000000` Probe with a PCMCIA bus device (controller, I/O window, attribute memory).
   - Implement the DP8390 core as specified in §B1, plus a level EINT8 in `Gpio` and EINT3 on card insertion.
   - Add an "Insert network card" setting, a MAC address (locally administered, persisted), and snapshot support.
   - Bring-up: the guest should show "NE2000 Compatible Ethernet Driver" with an APIPA address before any host side exists.
2. **NAT** (2–4 d): cross-build glib + libslirp for arm64-v8a (and x86_64 for the emulator), wire the frame queues, fix
   DNS, add the `INTERNET` permission.
3. **Guest config** (0.5 d): the Helper provisions `CM_NetEntries` DestId = Internet for the NE2000 adapter.
4. **Test** (1–2 d): IE Mobile on plain-HTTP sites, `ping`, DNS, large downloads (ring overflow / BNRY handling),
   suspend/resume and snapshot restore.
5. **Optional: the B3 "PASS\" experiment** (0.5 d).

**TLS.** The 6.5.3 `schannel.dll` offers only **SSL 2.0, SSL 3.0 and TLS 1.0**, with AES-128/256 available
("Microsoft Enhanced RSA and AES Cryptographic Provider"). There is no TLS 1.1/1.2, and the root store is from 2009. [ROM]
SNI support and SHA-2 certificate validation are uncertain. [INFERRED] Most modern HTTPS endpoints will refuse it. Options:

- **(a) TLS-upgrading HTTP proxy in the app (recommended first).** Set a ConnMgr/IE proxy to a slirp host-forward
  address (e.g. 10.0.2.2:8080). The guest speaks plain HTTP. The proxy fetches upstream over modern TLS (OkHttp) and can
  rewrite `https://` links to `http://`, WebOne-style. No guest certificates are needed. 2–3 d.
- **(b) MITM proxy for apps that insist on https.** Generate a per-install CA, install its `.cer` into the guest root store
  through the Helper, terminate guest TLS 1.0 (RSA/AES-CBC/SHA1, so the server side must allow legacy) and re-originate
  TLS 1.3 upstream. Bind it to the in-process NAT only. 3–5 d.
- **(c) App-specific backends**, such as the user's StaticForge proxy, speaking HTTP to the guest. No work beyond (a).

---

## Method and scratch artefacts

These are in the session scratchpad, not in the repo:

- `imgfs_dump.py` (IMGFS + Xpress extractor) on `PPC_USA_GSM_FWVGA_VR.BIN` → `fw653/`.
- `hvtree.py` (CE hive → text) and `regsel.py` (key and regex selection) for the registry.
- `xipfile.py` / `tools/romscan.py` for the XIP region (`boot.rgu`, module list).
- `strs.py` for module strings, and `armlite.py` for the FakeRIL disassembly (code VA base `0x7B4B7000`, routines around
  `0x7B4D2E38`–`0x7B4D33E8`).

## Sources

- Barry Bond, "GPS and the DeviceEmulator, Part 2" (Serial Port 0 → COM1, External GPS hardware port COM1, 4800 baud): https://learn.microsoft.com/en-us/archive/blogs/barrybo/gps-and-the-deviceemulator-part-2-using-gps-receiver-hardware-from-the-emulator
- GPS Intermediate Driver Multiplexer registry settings: https://learn.microsoft.com/en-us/previous-versions/ms889973(v=msdn.10)
- Example GPS Intermediate Driver registry settings: https://msdn2.microsoft.com/en-us/library/bb202104(v=msdn.10)
- GPSID input source registry settings (Compact 2013): https://msdn.microsoft.com/en-us/library/ee486820.aspx
- GPSID architecture: https://learn.microsoft.com/en-us/previous-versions/bb201942(v=msdn.10)
- DE help (`DeviceEmulator.chm`, local): Network tab (NE2000 vs CS8900 text), Peripherals tab, `/p` `/n` `/u0..2` switches
- DE 1.0 shared source: https://github.com/gitntel/Microsoft-Device-Emulator (`boards/pcmciadevices.*`, `boards/smdk2410/devices.cpp`, `boards/vpcnet.*`)
- libslirp: https://gitlab.freedesktop.org/slirp/libslirp ; Termux recipes `packages/libslirp/build.sh` (4.9.5, depends on glib) and `packages/glib/build.sh` in https://github.com/termux/termux-packages
- Building glib for Android (NDK notes): https://discourse.gnome.org/t/build-glib-for-android/8696
