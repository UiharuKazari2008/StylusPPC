# Cellular side of the DE phone images: RIL, transport and AT dialect

Research notes for WMDroid. They cover how the GSM "_VR" Device Emulator images talk to a radio, what the Microsoft
Cellular Emulator (CellEmu) did on the host side, and what WMDroid needs to emulate so the guest shows real
signal, operator, calls and SMS bridged from the Android phone. Companion to `research-de-hardware.md`.

Confidence tags:

- **[ROM]**: read directly out of the user's ROM images in this archive. The IMGFS was extracted and registry hives and module
  sections were decompressed with a small read-only script; nothing from the archive was executed. See §8.
- **[DISASM]**: read from an ARM disassembly of a module pulled out of the ROM.
- **[DOC]**: Microsoft documentation (MSDN / Learn archive).
- **[BLOG]**: Microsoft employee blogs. **[FORUM]**: community posts.
- **[SRC]**: open-source code (Linux CAIF, AOSP).
- **[INFERRED]**: reasoned from the above, not observed directly. **[GUESS]**: plausible but unverified.

---

## TL;DR

**Built 2026-10-16:** WM 6 / 6.1 / 6.1.4 now run rilgsm in plain-AT mode against WMDroid's virtual modem on UART0, with
the phone's real network state and outgoing calls handed to the app. Section 11 has what was built and the dialect
corrections found on the way (`+CLCC` needs 9 fields, `ATD` stays open until answered).

1. **There are three radio stacks, depending on the OS version.** [ROM]

   | Images | RIL driver | Transport | Result without CellEmu |
   |---|---|---|---|
   | WM5 (`PPC_USA_GSM_VR`, `SP_USA_GSM_VR`, MSFP `.nb0`) | `fakeril` (built into the guest) | none | Registers by itself on "Fake Network" |
   | WM6, 6 Refresh, 6.1, 6.1.4, 6.5 (Pro and Std) | `rilgsm.dll` (MS sample GSM RIL, Ericsson flavour) | `VEI1:` → `vei.dll` → `MUX1:` → `caifmux.dll` → `COM1:` (UART0) at 115200 8N1 with RTS/CTS | **"No Service"**, because nothing answers on COM1 |
   | WM 6.5.3 DTK (Pro and Std) | `fakeril.dll` (new FakeRIL, built into the guest) | none: `caifmux.dll` and `vei.dll` are *not in the image*, only leftover registry keys | Should register on its own "Fake Network" (see §6) |

2. On WM6–6.5, guest COM1 = UART0 at `0x50000000` (IRQ 28) = DE `/u0` = "Serial port 0" in the DE UI. The CellEmu
   docs say to map its virtual port to Serial port 0. [ROM][DOC]
3. **The wire protocol on COM1 is not raw AT. It is CAIF framing.** CAIF is Ericsson's "Communication CPU to Application CPU
   Interface", the same protocol the Linux kernel had in `net/caif`. Each frame is
   `STX(0x02) | len16 LE | channel | hdr | payload | CRC16 LE`, with CRC-16/CCITT (reflected poly 0x8408, init 0xFFFF, no final XOR).
   AT commands for the RIL travel on one logical channel (VEI1). PPP data travels on others (VEI2..VEI6). [DISASM][SRC][INFERRED]
4. The RIL speaks an **Ericsson-flavoured 27.007 dialect**: `ATV0` numeric result codes, `+CMER=3,,,1` with `+CIEV: 2,<0-5>`
   for signal (there is **no `+CSQ`**), `*EREG` / `+CREG` for registration, `*ECAM` / `*ECAV` for call state, PDU-mode SMS
   (`+CMGF=0`, `+CNMI=2,2,0,1,0`, `+CMT:`). The full init string is in §3.2. [ROM]
5. **Shortcut that skips CAIF:** the RIL's port is just the registry value `HKLM\Software\Microsoft\RIL\ComPort` = `VEI1:`.
   Set it to `COM1:` (the guest agent can do this) and soft-reset. `rilgsm` then talks plain AT directly on UART0. A community
   post reports this works with a real GSM modem. GPRS data is lost on this path. [FORUM][ROM][INFERRED]
6. Signal, operator and "No Service" come **only** from the RIL. There is no DE device for them. Battery is the separate DE
   `IOBattery` device (`battdrvr.dll`), and LED/vibrate is `IONotificationLEDs` (`nleddrvr.dll`). [ROM][DOC]

---

## 1. Radio stack wiring per image family

### 1.1 WM5: in-guest FakeRIL [ROM][BLOG]

- WM5 GSM images contain a `fakeril` module. They do not contain `rilgsm`, `caifmux` or `vei`. [ROM: string scan of
  `PPC_USA_GSM_VR.BIN` and MSFP `pocketpc.nb0`]
- The FakeRIL shows the antenna and "G" icons. Tapping the antenna shows "Fake Network". It loops SMS back from
  +1 425 001 0001 (`0010001`, `0010002`), has special voice numbers (7272470 …), data numbers (7272455 …),
  "always busy" 7272024 and "never answer" 7272773. [BLOG: Barry Bond, Fernando Zandoná]
- Microsoft says the WM 5.0 images **cannot** connect to the Cellular Emulator. [BLOG comment by F. Zandoná, 2007-04-25]
- WMDroid needs nothing on the host side for WM5. There is no host hook either: to bridge real phone status into WM5 you
  would have to patch or replace the RIL inside the guest.

### 1.2 WM6 / 6.1 / 6.1.4 / 6.5: `rilgsm` over CAIF [ROM]

From the decompressed `default.hv` of the WM6 Pro SDK Refresh image. WM6 Std, 6.1 Pro, 6.1.4 Pro/Std and 6.5 Pro/Std have
the same modules.

```
[HKLM\Drivers\BuiltIn\Serial]      Dll=serial_smdk2410.dll  Prefix=COM  Index(DeviceArrayIndex)=0
                                   MemBase=0x50000000 (UART0) Irq=28  Port="COM1:"  FriendlyName="Serial Cable on COM1:"
[HKLM\Drivers\BuiltIn\MUX]         Dll=caifmux.dll  Prefix=MUX  Index=1  Order=1  CommPort=1  BaudRate=115200
[HKLM\Drivers\BuiltIn\VEI1..VEI6]  Dll=vei.dll      Prefix=VEI  Index=1..6  DeviceArrayIndex=0..5  Order=2  MuxIndex=1
[HKLM\Drivers\BuiltIn\RIL]         Dll=rilgsm.dll   Prefix=RIL  Index=1  Order=3
[HKLM\Software\Microsoft\RIL]      ComPort="VEI1:"  DataPort="VEI2:"  LastEquipmentState=2  RILProxyLogNormal=1
[HKLM\Comm\Cellular\RIL]           Contexts=3
[HKLM\System\State\Phone]          "Phone Status Flags"=16
```

- **WM 6.5 (non-.3)** has two muxes: `MUX1` (CommPort=1) and `MUX2` (CommPort=2, i.e. COM2), both at 115200. All VEIs
  still use `MuxIndex=1`. 6.1.4 has a single `MUX`. [ROM]
- The `caifmux.dll` binaries differ between 6.0, 6.1.4 and 6.5 (rebuilds). The framing below was read from the WM6 Refresh build. [ROM]
- Load order: MUX (Order 1) → VEI (Order 2) → RIL (Order 3). [ROM]
- `rilgsm.dll` is the Microsoft sample GSM RIL. Its build path string is
  `c:\wm602\public\cellcore\ril\driver\gsm\response.cpp`, and the version string reads
  "Microsoft Windows CE … 5.2 Build 17740 (Apr 7 2007)". It was compiled with the Ericsson (EMP) radio options:
  `*EVA`, `*EVH`, `*EVD`, `*ELIN`, `*EREG`, `*ECAM`/`*ECAV`, `*EIAD`, `*STKC`/`*STKE`/`*STKR`/`*STKN`/`*STKI`. [ROM]
- `rilgsm` also reads these optional values from `Software\Microsoft\RIL`: `ComPort` (built-in default `"COM2:"`), `BaudRate`,
  `OpenTimeout`, `MaxOpenAttempts`, `ComInitString`, `PreInitString`, `TmpInitString`, `TimeoutCmdInit`,
  `TimeoutWaitForInit`, `TimeoutDTRDrop`, `TimeoutAPIDefault`, `TimeoutCmdOnline`, `TimeoutCmdNoOp`, `DataModeCmdDelay`,
  `MaxDataModeTimeWithoutCmd`, `ConnectResponseDelay`, `EONSEnabled`, `UseCphsPlmnName`, `LastCLIP`, `LastCLIR`. [ROM strings]
- `rilgsm` exports `RIL_*` as the RIL device and also `COM_*`, a virtual serial port the RIL exposes for data. [ROM export names]
- `veim.dll` is unrelated. It is the "Virtual Ethernet Intermediate Miniport" (`Comm\VEM`) used for DE networking. [ROM]

### 1.3 Mapping to DE and to CellEmu

| Layer | Value | Tag |
|---|---|---|
| Guest port the mux opens | `COM1:`, built from the `Com%d:` format with `CommPort=1` | [ROM] |
| Guest UART | UART0 `0x50000000`, IRQ 28 (`Drivers\BuiltIn\Serial`) | [ROM] |
| DE switch | `/u0 <hostport>`, "Serial port 0" in Configure → Peripherals | [DOC: DE help] |
| Host side | CellEmu installs **seven pairs of "XPVCOM" virtual serial ports**. It opens one end and shows the other end's name (usually COM3 or COM4) in the left pane of its status bar. You type that name into DE Serial port 0 (don't pick it from the drop-down) and soft-reset. | [DOC][BLOG] |
| Baud/framing | The guest sets 115200, 8 data bits, no parity, 1 stop bit, **RTS/CTS hardware flow control**, DTR enabled (see §2.3). DE does not care about baud on virtual ports. | [DISASM][ROM] |
| DE-specific transport | None for the radio. The DE DMA transport (`dmatrans.dll`, `SERDMA` = "Serial over DMA") is used only by ActiveSync and debugging. | [ROM] |

Note that the DE help text calls Serial port 0 "the emulator COM1 port, used by the OS for debug output". That is generic CE
wording. In the phone images COM1 belongs to the radio mux. [DOC][ROM]

### 1.4 What CellEmu shows for this link [DOC]

- Its "AT log" tab shows only the AT layer. Lines starting `>>` came from WM, lines starting `<<` came from CellEmu, with a timestamp
  `[Day]::[H]:[M]:[S]`, `dwLen=`, and `<CR>`/`<LF>` printed literally. So CellEmu demultiplexed the CAIF stream and displayed
  the VEI1 AT channel.
- Troubleshooting: "No AT commands are shown" means the DE serial mapping is wrong, or the saved state needs a soft reset. "Network
  name shown on today screen is 35638" is by design: 35638 spells **FKNET** on a phone keypad. So CellEmu reported a numeric
  PLMN (probably MCC 356 / MNC 38) that the RIL could not turn into a name.
- The Data Channels panel lists **five** PPP connections. That matches VEI2..VEI6 being five data channels next to the
  VEI1 AT channel. [INFERRED]
- Behaviour that is not in the AT layer: dial-up server numbers (Busy / Reject / Hang-up-after-connect / No-answer; all other
  numbers auto-answer), several simultaneous MT calls, SMS injection with repeat, 2G/3G switching, "Disconnect GPRS", custom
  event injection, a SIM file system loaded from `USIM.xml`, and "filter DLLs" that each handle a subset of AT commands.

---

## 2. CAIF transport (`caifmux.dll` + `vei.dll`)

### 2.1 Identification

- `caifmux.dll` is a stream driver with prefix `MUX` (`MUX_Init/Open/Read/Write/IOControl/...`). It reads `CommPort`
  and `BaudRate` from `\Drivers\BuiltIn\MUX` and opens `Com%d:`. [ROM]
- `vei.dll` is a stream driver with prefix `VEI`. It opens `MUX1:`. It contains the strings `ATV0`, `ATE0`, `AT+CSCS="UTF-8"`
  and `AT+CR=1` (purpose not confirmed) and reads `Priority256` and `DeviceArrayIndex`. [ROM]
- "VEI" is the CAIF service name for the AT channel. Linux CAIF `cfveil.c` = "VEI layer", control service
  `CFCTRL_SRV_VEI = 1`. The frame layout and CRC below match Linux `cfserl.c` and `cffrml.c` exactly. [SRC][DISASM]

### 2.2 Frame format [DISASM caifmux 0x01c61bec (writer), 0x01c61b68 (CRC), 0x01c61da0 (reader)]

```
+------+--------+--------+---------+-------------+---------+--------+--------+
| 0x02 | len_lo | len_hi | channel | svc hdr (n) | payload | crc_lo | crc_hi |
+------+--------+--------+---------+-------------+---------+--------+--------+
  STX    len = 2 + 1 + n + payload_len        CRC over channel..payload
```

- **STX 0x02** is sent when the per-port flag `[obj+0x7f]` is set. The constructor sets it to 1, so STX is on by default.
  The reader is a state machine that hunts for 0x02 (state 3), then reads the 2-byte length (state 2), then the body, then checks the CRC.
- **Length** is 16-bit little-endian. It counts the two length bytes plus channel, header and payload, but **not** the CRC or the STX.
  That is the same number Linux writes ("CAIF packet + 2 for FCS"). [DISASM][SRC]
- **CRC** is table-driven CRC-16/CCITT reflected (poly 0x8408; table[1] = 0x1189), init 0xFFFF, no final XOR. It is computed over the
  bytes after the length field through the end of the payload and appended little-endian. This is Linux `crc_ccitt(0xffff, …)`. [DISASM][SRC]
- **Header size n**: message type 1/2 has 1 service-header byte (for VEI, per Linux: `0x00` = payload, `0x80` = flow-on,
  `0x81` = flow-off, `0x82` = set-pin). Message type 3 has 4 bytes: a flag byte built from bits 7/2/1/0, then three bytes,
  used for control-channel traffic. [DISASM][SRC][INFERRED]
- **Channel byte**: 8 logical channels at most. On receive, a channel byte > 0x80 is matched against `(local_id | 0x80)`.
  Bytes ≤ 0x80 go to the control path. On transmit, bit 7 is set or cleared depending on the message type
  (`and 0x7f` / `orr 0x80`). The exact semantics of bit 7 are **not resolved**. [DISASM, partial]
- Max payload 0xFA0 (4000 bytes). Linux CAIF caps frames at 4096. Reads are done in 0x200-byte chunks. [DISASM][SRC]
- **Control/link setup**: at start-up `caifmux` loops over channels 0..7 and sends a type-3 control message with a 1-byte
  payload. It then waits on an event and treats `WAIT_TIMEOUT` (0x102) as "no answer", so the host side has to answer.
  In Linux, control channel 0 carries `LINK_SETUP(0)` / `LINK_DESTROY(1)` / `ENUM(3)` …, responses have bit 0x20 set, and a link-setup
  request is `cmd, (chtype<<4)|linktype, (prio<<3)|phyid, endpoint&3`. The response echoes these and appends the
  assigned link id. **Whether Microsoft's caifmux uses the same control encoding is not verified.** [SRC][GUESS]

**Recommended next step (high value, cheap):** log raw UART0 TX/RX bytes in WMDroid while a WM6–6.5 image boots. The
first frames caifmux sends answer the open control-channel questions directly. Another source of real traffic is the
real DE + CellEmu on a PC (WM6 SDK) with a serial sniffer (for example com0com plus a logging bridge).

### 2.3 COM1 settings the guest applies [DISASM caifmux 0x01c627c0..0x01c628d4]

- `CreateFile(COM1:, GENERIC_READ|GENERIC_WRITE)`, then `GetCommState`/`SetCommState` with
  DCBlength=0x1C, BaudRate = registry (115200), ByteSize=8, Parity=NONE, StopBits=ONE, XonChar=0x11, XoffChar=0x13, and flags:
  `fBinary=1, fParity=1, fOutxCtsFlow=1, fOutxDsrFlow=0, fDtrControl=ENABLE, fRtsControl=HANDSHAKE`, with the high bits kept.
- `SetCommMask(EV_RXCHAR|EV_RLSD|EV_BREAK|EV_ERR|EV_RING)` (0x1E1).
- `SetCommTimeouts{ReadInterval=MAXDWORD, ReadTotalMult=0, ReadTotalConst=0, WriteTotalMult=0, WriteTotalConst=100}`.
- **Consequence for WMDroid:** because `fOutxCtsFlow=1`, the S3C2410 serial driver may hold TX until CTS is asserted.
  UART0's UMSTAT must report CTS active (and DSR active via GPH7, see the hardware doc §3.3), or the guest may never transmit. [INFERRED]

---

## 3. AT dialect spoken by `rilgsm` (WM6–6.5)

All strings below are verbatim from the decompressed `rilgsm.dll` (WM6 Refresh). `<cr>` is 0x0D. [ROM]

### 3.1 Result-code style

- The init string sends `V0` and `E0`, so final results are **numeric**: `0`=OK, `1`=CONNECT, `2`=RING, `3`=NO CARRIER,
  `4`=ERROR, `6`=NO DIALTONE, `7`=BUSY, `8`=NO ANSWER, each followed by `<cr>`. Information lines stay text. In V0 mode they are
  normally sent as `<text><cr><lf>`. [ROM: the parser holds `"4\r"`, `"\r\nOK\r\n"`, `"\r\nERROR\r\n"` and `"\r\n> "`,
  so it accepts both styles. The numeric codes themselves are standard V.25ter. [INFERRED]]
- A host emulator should track `V`/`E` state and answer accordingly. The very first `AT<cr>` probe arrives before `ATV0`, so
  it gets a verbose `\r\nOK\r\n` reply (V1 is the power-on default). [INFERRED]
- `+CMEE=1` gives numeric errors: `+CME ERROR: <n>` and `+CMS ERROR: <n>`.

### 3.2 Initialisation sequence

The RIL keeps the init list as `|`-separated items and sends each one as `AT%s<cr>`:

```
V0|E0|S0=0|Q0|X3|&C1|&D1|+CMEE=1|+CRC=1|+CR=1|+CMUT=0|+CMER=3,,,1|*EREG=2|
+CSSN=0,1;+COLP=0;+CCWA=1;+CSCS="UTF-8";+CMGF=0|+CGREG=1|+CUSD=1|+CGEREP=1,0|
+CPMS="SM","SM","SM"|*ECAM=1|*STKC=1,"0901FF7F00000010"|
```

Two other lists exist: `V0|E0|+CRC=1|+CMUT=0|` (a short re-init) and the list above starting at `+CMUT=0|…` (a re-init
without the V/E/S/Q/X part). Other start-up commands seen: `AT+CPIN?`, `AT+CPBS="SM"`,
`AT+CNMI=2,2,0,1,0`, `AT+CLIP=%u|+CLIR=%u|`, `AT+CGMI;+CGMM;+CGMR;+CGSN`, `AT+CIMI`, `AT+CNUM`, `AT+CFUN?`, `AT+CFUN=1`,
`AT+CIND?`, `AT+CREG?`, `AT+COPS=3,%u;+COPS?`, `AT+CSMS?;+CPMS?`, `AT+CSCA?`, and capability probes (`AT+CHLD=?`,
`AT+CBST=?`, `AT+CRLP=?`, `AT+CFUN=?`, `AT+CPBS=?`, `AT+CPBR=?`, `AT+CPBF=?`, `AT+CSMS=?`, `AT+CPMS=?`, `AT+CMGL=?`,
`AT+CPOL=?`, `AT+CLCK=?`, `AT+CPWD=?`, `AT+CCFC=?`, `AT+CHSD;+CHSN=?`, `AT+CGDCONT=?`, `AT+CGQREQ=?`,
`AT+CGQMIN=?`, `AT+CGSMS=?`, `AT+DS=?`). [ROM]

Practical rule: answer `0` (OK) to anything you don't model. Return plausible `=?` ranges for the capability probes so the RIL
caches sane device caps. Ericsson `*E…` and `*STK…` commands can simply be OK'd. [INFERRED]

### 3.3 Unsolicited result codes the RIL parses

Prefix dispatch table at `rilgsm` VA 0x01a5dbec..0x01a5dc48, in this order: [ROM][DISASM]

`+CGEV:`, `+CHSR:`, `+CUSD:`, `+CDSI:`, `+CBMI:`, `+CMTI:`, `+CDS:`, `+CBM:`, `+CMT:`, `+CCCM:`, `+CSSI:`, `+CSSU:`, `+CCWA:`,
`+COLP:`, `+CLIP:`, `+CR:`, `+CRING:`, `+CGREG:`, `+CREG:`, `*STKN:`, `*STKI:`, `+CIEV:`, `*EREG:`, `*ECAV:`.
Plain `RING` arrives as numeric `2` in V0 mode. With `+CRC=1` the modem sends `+CRING: VOICE` instead.

### 3.4 What each indicator needs

| Guest UI effect | What to send | Tag |
|---|---|---|
| **Signal bars** | `+CIEV: 2,<0..5>`. The handler only acts on indicator index **2** (Ericsson's `"signal"`). It maps value 1..5 to −113/−98/−82/−66/−51 dBm and 0 to "unknown". It reports MinSS −113, MaxSS −51, LowSS −110, HighSS −60 (`RIL_NOTIFY_SIGNALQUALITY`). Answer the solicited `AT+CIND?` with an Ericsson-style list where the 2nd value is signal, e.g. `+CIND: 5,4,0,0,1,0,0,0,0,0,0,0` (battchg, signal, batterywarning, chargerconnected, service, sounder, message, call, roam, smsfull, callsetup, callheld). **There is no `AT+CSQ` in this RIL.** | [DISASM 0x01a5bd48][BLOG RIL log example][GUESS on the full CIND layout] |
| **Registered / operator** | Solicited: `+CREG: 2,1,"<lac>","<ci>"` (`AT+CREG?`). Unsolicited: `*EREG: <stat>[,"<lac>","<ci>"]` (enabled by `*EREG=2`) and/or `+CREG: <stat>…`. stat 1 = home, 5 = roaming, 2 = searching, 3 = denied, 0 = not registered. | [ROM][GUESS on exact *EREG fields; see Ericsson AT manual] |
| **Operator name** | The RIL sends `AT+COPS=3,<fmt>;+COPS?` for each format. Reply `+COPS: 0,0,"Long Name"`, `+COPS: 0,1,"Short"`, `+COPS: 0,2,"MCCMNC"`. If only a numeric code comes back and it is not in the RIL's built-in PLMN table (which has names like "AT&T", "Rogers AT&T", "NUEVATEL") or in `Software\Microsoft\RIL\OperatorNames`, the Today screen shows the number. That is the CellEmu "35638" case. | [ROM][DOC] |
| **SIM ready** | `AT+CPIN?` → `+CPIN: READY`. `AT+CIMI` → IMSI. `AT+CNUM` → `+CNUM: "","<number>",129`. `+CRSM` handles SIM file reads (EONS/CPHS/phonebook). Answer them or return an error. | [ROM] |
| **Radio on** | `AT+CFUN?` → `+CFUN: 1`. `AT+CFUN=0` / `AT+CGATT=0;+CFUN=0` turn the radio off for flight mode. | [ROM] |
| **Incoming call** | `+CRING: VOICE` (`+CRC=1`), then `+CLIP: "<num>",<type>` (129 national, 145 with `+`). Also `*ECAV: <id>,5,1,…` for an incoming/waiting call (`*ECAM=1` enables call-state events). The RIL polls `AT+CLCC` → `+CLCC: <id>,1,4,0,0,"<num>",129`. Answer is `ATA` or `AT*EVA`. Hang-up is `ATH` / `AT*EVH` / `AT+CHLD=…`. | [ROM][GUESS on *ECAV fields and which answer variant is used] |
| **Outgoing call** | `ATD<num>;` (or `*EVD="<num>"`). Reply OK, then `*ECAV: <id>,1,1` (calling), `,6,` (alerting), `,3,` (active), and `3` (NO CARRIER) / `*ECAV: <id>,0,…` when it ends. `AT+CLCC` must reflect the state. | [ROM][GUESS] |
| **Incoming SMS** | PDU mode (`+CMGF=0`) with `+CNMI=2,2,0,1,0` routes the message directly: `+CMT: ,<tpdu_len><cr><lf><hex SMSC+TPDU><cr><lf>`. The RIL may ack with `AT+CNMA=<n>`. Reply OK. Alternative: store it and send `+CMTI: "SM",<idx>`, then serve `AT+CMGR=<idx>`. | [ROM][27.005] |
| **Outgoing SMS** | `AT+CMGS=<tpdu_len><cr>` → modem sends `<cr><lf>> ` → guest sends hex PDU + Ctrl-Z → `+CMGS: <mr>` + OK. `AT+CMMS=1` may come first. | [ROM][27.005] |
| **GPRS** | `+CGDCONT`, `+CGATT`, `+CGACT`, `+CGPADDR`, `+CGDATA=`, `*EIAD=<cid>,1`, `+CGREG`, `+CGEV:`. PPP runs on DataPort `VEI2:` (another CAIF channel). | [ROM] |

No full real CellEmu ↔ WM AT transcript was found online. The MSDN page shows only screenshots of the AT log tab. Use §2's
"log the UART" step to capture one.

---

## 4. Plain-AT shortcut (skip CAIF) [FORUM][ROM][INFERRED]

The MSDN forum answer quoted on toughdev.com (2008) said the emulator's default `HKLM\Software\Microsoft\RIL\ComPort` is a
virtual port (`VEI1:`). Setting it to `COM1:` and mapping DE Serial port 0 to a real GSM modem gave network, signal strength
and working calls. For WMDroid:

1. Use the existing guest agent (or a provisioning XML / `.reg` run once) to set
   `HKLM\Software\Microsoft\RIL\ComPort = "COM1:"`. Optionally also set `BaudRate=115200` under the same key.
2. If `caifmux` turns out to open COM1 at boot (not just on first `VEI` open), stop it from claiming the port. Either point
   `HKLM\Drivers\BuiltIn\MUX\CommPort` (6.5: `MUX1`, `MUX2`) at an unused port number, or delete those keys. [GUESS: the forum post
   only mentions ComPort, which suggests the open is lazy]
3. Soft reset. The hive registry survives a soft reset because RAM is kept. A cold boot from the ROM loses it.
4. WMDroid's UART0 back end is then a plain AT responder (§3). GPRS data stops working, because DataPort `VEI2:` is gone. You could
   point `DataPort` at `COM1:` as well, but data mode would then share the AT port (`ATD*99#` → PPP on the same port).

The full-fidelity path is to implement CAIF on the host side (§2) behind UART0. That keeps the ROM untouched, gives
simultaneous AT and data, and works on fresh images.

---

## 5. Battery and other DE devices [ROM][DOC]

- Phone status (bars, "No Service", operator, roaming) is produced only by the RIL → RIL proxy (`ril.dll`) → CellTSP/TAPI →
  SNAPI (`HKLM\System\State\Phone`). No DE device feeds it.
- **Battery**: `HKLM\Drivers\BuiltIn\Battery` = `battdrvr.dll` (Prefix BAT, IClass `{DD176277-CD34-4980-91EE-67DBEF3D8913}`).
  It is backed by the DE 2.0/3.0 `IOBattery` MMIO block at `0x500FFFF0` (`/battery`, `/batterycharge`). See `research-de-hardware.md` §4.4.
  On the AT side, CIND index 1 is `battchg`, but `rilgsm` ignores every `+CIEV` index except 2, so the modem does not drive battery.
  [DISASM]
- **Notification LED / vibrate**: `HKLM\Drivers\BuiltIn\NLed` = `nleddrvr.dll` (Prefix NLD), backed by DE `IONotificationLEDs`.
  It is used for ring and new-message alerts. Mapping it to the Android vibrator/LED is worth doing alongside calls and SMS.
- Ring tones and SMS sounds play through the normal IIS audio path. CellEmu carries **no call audio**. `IOSpeakerPhone` (DE 2/3)
  only reports speaker state.
- ActiveSync uses the DMA transport (`dmatrans.dll` DMA0..9, `SERDMA` on DMA channel 6). The radio does not.

---

## 6. WM 6.5.3: built-in FakeRIL [ROM]

- 6.5.3 Pro and Std images contain `fakeril.dll` (378 KB code, `fakeril.pdb`), registered as `Drivers\BuiltIn\RIL`. They
  do **not** contain `rilgsm.dll`, `caifmux.dll` or `vei.dll`. The `MUX1`/`MUX2`/`VEI1..6` keys and `ComPort="VEI1:"` are still in
  the hive but are dead. `Software\Microsoft\RIL\NetworkType=1` is new.
- FakeRIL strings include the network names "Fake Network", "Fake Roaming Network", "Fake Denied Network", "Fake Network Alt 1/2",
  "Fake Network UMTS" and "Fake Network GSM Compact"; the number "This is a Fake Number"; and state files `FakeRadio.dat`, `FakeSIM.dat`,
  `Operators.dat`, `SupSvcs.dat`, `CallBarring.dat`, `LockFac.dat`, `\release\Fakeril.sim`, `%04X_%04X.sim`. It has registry
  values `FakeRILCSP` (with sections Radio/Network/Operator/LocalOperators/SIMFileSystem/SIMProperties/CallForwarding/…),
  `RILAPIDelay`, `RILAPIDelayVariance`, `SignalRestorationTimeout`, `OperatorSignalQuality`, `ModemRunTimeDetection`, `ExternalFRCsp`,
  `NetAdapter`/`VMINI1`/`WWAN`/`192.168.1.1` (fake packet data over the DE NIC), and an optional plug-in
  `\windows\FakeRadioATSupport.dll` (`InitFakeRilATSupport`, `ReceiveCmd`). That plug-in is **not** in the image.
- **Implication:** on 6.5.3 there is no host-side radio port to emulate. The guest simulates its own network. If WMDroid shows
  "No Service" on 6.5.3, the cause is inside the guest, for example a timer, RTC or registry state that FakeRIL waits on, or
  `LastEquipmentState`. It is **not** a missing CellEmu. Check `HKLM\System\State\Phone` values and the RIL proxy log (§7)
  with the guest agent. Bridging real phone status into 6.5.3 would need either the FakeRIL CSP/registry knobs (`FakeRILCSP`,
  `OperatorSignalQuality`, operator list), live-edited through the agent, or a replacement RIL driver dropped in by the agent.
  [INFERRED]

---

### 6.1 The device's own number on 6.5.3 FakeRIL [OBS][DISASM] (2026-10-16)

- **Where WM shows it:** Phone > Menu > Options (Sounds tab header): "Number: +1 (425) 001-0001". It comes from
  `RIL_GetSubscriberNumbers`, which FakeRIL answers with `14250010001` / `14250010002`, description "This is a Fake Number".
  Seen only while FakeRIL is loaded (calls on, `patch.fakeril=on`); with FakeRIL unloaded there is no number at all.
- **Source:** FakeRIL's default SIM file table in its `.data` section (S001, VA 0x01E09AD8 in the 6.5.3 build): header
  `10 7F 00 00 03 00 00 00 02 00 00 00 02 00 00 00 02 00 00 00 23 00 00 00` (DF 7F10, EF type, 2 records of 0x23 = 35 bytes),
  then two EF MSISDN (6F40) records: 21-byte alpha "This is a Fake Number", `07 91 41 52 00 01 00 F1|F2 FF..` (BCD 14250010001/2).
  At load FakeRIL copies it into a heap SIM store (the live one; seen in a RAM dump next to an untouched `.data` copy).
  The ROM sections are compressed, so the bytes are not in `fwvga.bin`. There are no `FakeRadio.dat`/`FakeSIM.dat` files
  (FakeRIL keeps them in `\release`, DE's KITL share, absent here), so every FakeRIL load starts from the default.
  `HKLM\Software\Microsoft\SimMgr\SimFile` can name a SIM image file instead (default `\release\Fakeril.sim`; its format is
  FakeRIL's own, not decoded).
- **Writing it from a RIL client works at the RIL level:** `RIL_SendRestrictedSimCmd(UPDATERECORD 5, 6F40, record 1,
  absolute, 35 bytes)` returns 90 00, and `RIL_GetSubscriberNumbers` reports the new number at once (helper diagnostic
  `number sim +1...`, guest/Helper/OwnNumber.cs).
- **But the display does not follow:** celltsp.dll reads the subscriber numbers once (function 0x7B6FF9D4, guarded by its
  globals+0xEB "have numbers" flag) and drops them only on `RIL_NOTIFY_RADIOPRESENCECHANGED` = 0 or
  `RIL_NOTIFY_SIM_DATACHANGE` (0x00400008) with data 0xFFFFFFFC (MSISDNs) / 0xFFFFFFFF (all files). Both come only from the
  radio driver; FakeRIL sends neither after an UPDATE RECORD, and a radio off/on cycle (`RIL_SetEquipmentState` 1 -> 2) does
  not clear the cache. Reloading FakeRIL clears it but also resets the SIM to the default.
- **Consequence:** the number must already be in FakeRIL's SIM when FakeRIL starts: i.e. FakeRIL's `.data` default table must
  hold it before FakeRIL's init copies it. That is a host-side job (find the 48-byte signature above in guest RAM when the page
  holding it is in place, before FakeRIL's SIM init runs, and rewrite the two BCD numbers), or a decoded `SimFile` image on
  host-provided storage. A guest helper starts too late for either.

## 7. Debugging aids inside the guest

- `RILProxyLogNormal=1` is already set in the WM6–6.5.3 hives. Logging also needs `rillog.dll` in `\Windows`, and output goes
  to debug output (OutputDebugString / CeLog). Setting `RILProxyLogDetail=1` too gives command, response and notification lines such as
  `RILProxy : N : SIGNALQUALITY - SignalStrength = -113, MinSS = -113, MaxSS = -51 …`. [BLOG][ROM]
- `rilgsm` has debug strings `RilDrv: Sending cmd: %a` and `RilDrv: Accumulated response: %a`. Whether a retail image emits
  them, and where (UART1 debug serial or KITL), is not verified. [ROM]

---

## 8. Open-source references

- **No open-source re-implementation of Microsoft's Cellular Emulator was found**, and no WM RIL AT transcripts were found either. The
  WM5 sample RIL source (`public\cellcore\ril\driver`) is in the WM AKU / CE 6.0 Platform Builder trees, not on public GitHub.
  [FORUM: CodeProject "Oldie but Goldie" names the path and the Siemens/TI/TTPCom variants]
- **Linux CAIF** (`net/caif/` in the kernel up to 6.x: `cfserl.c` STX/length, `cffrml.c` length + CRC-CCITT FCS, `cfmuxl.c`
  1-byte link id, `cfctrl.c` control channel, `cfveil.c` AT channel). This is the closest public spec for caifmux framing. GPL, so
  read it for the protocol but don't copy it into WMDroid's code.
  e.g. https://raw.githubusercontent.com/torvalds/linux/v6.6/net/caif/cfserl.c
- **Android emulator modem** (`android_modem.c`, `sms.c`, `sim_card.c`): an AT responder covering
  `+CREG/+CGREG/+COPS/+CSQ/+CLCC/+CMGS/+CMT`, plus PDU helpers. Apache-2.0.
  https://android.googlesource.com/platform/external/qemu/+/refs/tags/android-4.4_r1/telephony/android_modem.c
- **Cuttlefish modem_simulator** (C++): network/call/SMS/SIM/STK/data services with `pdu_parser.cpp`. Apache-2.0.
  https://android.googlesource.com/device/google/cuttlefish/+/refs/heads/main/host/commands/modem_simulator/
- celersms **AT-Emulator** (Java, GSM AT set): https://github.com/celersms/AT-Emulator

---

## 9. Suggested WMDroid design

1. **UART0 back end** = an "EmuModem" object. Assert CTS/DSR in UMSTAT. Feed RX through the existing 8 KB queue and its interrupts.
2. **Mode A (default for WM6–6.5): CAIF host.** Deframe STX/len/CRC, answer control-channel link setup for channels 0..7,
   and route channel → VEI1 (AT engine) and VEI2..6 (PPP engines, later). Confirm the control bytes with a UART capture first (§2.2).
3. **Mode B: plain AT on UART0** for images where the agent has set `ComPort=COM1:` (§4). This mode is also useful for bring-up.
4. **AT engine**: V0/V1- and E-aware, Ericsson extensions OK'd, state for CFUN/CPIN/CREG/COPS/CIND/CLCC/SMS store.
   Drive unsolicited codes from Android:

   | Android source | Guest output |
   |---|---|
   | `SignalStrength.getLevel()` 0..4 | `+CIEV: 2,<level+1>`, or 0 when out of service |
   | `ServiceState` (state, roaming) | `*EREG:` / `+CREG:` stat 1/5/2/0 |
   | `getNetworkOperatorName()` / `getNetworkOperator()` | `+COPS` replies in long/short/numeric format |
   | `TelephonyCallback.CallStateListener` (ringing + number if permitted) | `+CRING: VOICE`, `+CLIP`, `*ECAV`. Answer/hang-up from the guest can call `TelecomManager.acceptRingingCall()` / `endCall()`; this needs permissions and user opt-in. |
   | `SMS_RECEIVED` intent `pdus` extra (raw 3GPP SMS-DELIVER with SMSC) | `+CMT: ,<len>` + hex. The PDU format already matches `+CMGF=0`. |
   | Guest `AT+CMGS` PDU | Decode, then optionally `SmsManager.sendTextMessage()`. Off by default; ask the user. |
   | IMSI/MSISDN (restricted on modern Android) | A synthetic IMSI with the real MCC/MNC from `getSimOperator()`, and a configurable number |

5. Battery stays on the `IOBattery` device (`BatteryManager`), not on the modem.
6. WM5 and WM 6.5.3 use FakeRIL, so there is no host radio path; bridging there needs in-guest work (§1.1, §6).

---

## 10. How the ROM data was obtained (reproducible, read-only)

- DE flash images (`.BIN`, 96 MB for WM6+) are laid out in 64 KB blocks: 15 data sectors of 4 KB, plus a last sector of 8-byte
  `{logical_sector, flags}` entries, ending with the signature `47 8A 43 3B E6 2A D9 A9 31 67 39 09 53 67 BF 12`.
- IMGFS pointers are relative to logical sector 0x260 (WM6 Refresh), 0x2B0 (6.1.4) or 0x2C0 (6.5.3).
- IMGFS dirents are 0x34 bytes: file `0xFFFFFEFE`, name `0xFFFFFEFB`, section `0xFFFFF6FD`. Data indexes are arrays of
  `{u16 comp, u16 raw, u32 ptr}` 4 KB chunks, compressed with Xpress (MS-XCA plain LZ77).
- CE hive cells: key `0xC0……` with the name length at +0x18 and the name at +0x1C. Value `0xD0……` with `{u16 type, u16 datalen, u16 namelen}` at +0x10.
- The scripts used (`imgfs_dump.py`, `hvtree.py`, `armlite.py`) are in the session scratchpad and can be moved into `tools/` if wanted.

## 11. What WMDroid built (2026-10-16): virtual modem for WM 6 / 6.1 / 6.1.4

Facts below were observed in the CLI on the user's three images (`wm6vga.bin`, `wm61wvga.bin`, `wm614wvga.bin`); rilgsm's
retail build prints `RilDrv: Sending cmd / Accumulated response` on UART1, which the CLI shows as `[uart1]`. [OBSERVED]

**Path chosen: plain AT (section 4), not CAIF.** The system patch (`guest/Helper/Patch.cs`, step "radio", since patch 6; only when
`Drivers\BuiltIn\RIL\Dll` is `rilgsm.dll`) sets `Software\Microsoft\RIL\ComPort = COM1:`, gives `Drivers\BuiltIn\MUX`,
`MUX1`, `MUX2` and `VEI1..6` `DEVFLAGS_NOLOAD`, clears NOLOAD on the RIL driver (patch 5's "fakeril" step had unloaded
every RIL; it now skips rilgsm images), and restarts once. After that restart rilgsm opens COM1 and runs its init list
(section 3.2) against `core/src/board/modem.cpp`, attached to UART0 (`Uart::peer`). GPRS through the RIL is gone, which is
fine: the internet is the NE2000 card. GPS on COM1 is not set up on these images (the port is the radio's).

**Corrections and additions to section 3, found the hard way:**

- **`+CLCC` needs 9 fields.** rilgsm's parser drops an entry whose `"<number>",<type>` is not followed by
  `,<alpha>,<priority>`; it then returns an *empty* call list, and CellTSP ends the connected call at once with
  `LINEDISCONNECTMODE_UNAVAIL` ("Call ended 00:00"), without sending any AT command. Answer
  `+CLCC: 1,0,0,0,0,"5550123",129,"",0`. A line that stops after `<mpty>` also parses. [DISASM rilgsm `+CLCC` parser][OBSERVED]
- **`ATD<n>;` must stay open until the far end answers.** `RIL_Dial` attaches `RIL_NOTIFY_CONNECT` to the dial command's
  success, so an immediate `OK` reads as "answered" (WM: "Cannot complete the call…"). The modem sends `*ECAV: <id>,1,1`
  at once, `OK` (`0<cr>`) after `*ECAV: <id>,3,1` when the app says the real call is active, and `NO CARRIER` (`3<cr>`)
  after `*ECAV: <id>,0,1,8,16` if it ends first. Any byte from the guest during the open dial aborts it (V.250); rilgsm
  does that when WM's End is pressed while dialing. [DISASM `RIL_Dial`][OBSERVED]
- **`*ECAV: <ccid>,<ccstatus>,<calltype>[,<processid>,<exitcause>,"<number>",<type>]`** is parsed as Ericsson documents it:
  ccstatus 1/2 → CPI "new outgoing" (sets direction outgoing), 3 → connected, 6 → new incoming, 0 → disconnected (exit
  cause 16 → normal, 17 → busy), 7 → busy; the debug line "Parse CPI: sending notification type N" prints the *calltype*.
  The notification is `RIL_NOTIFY_CALLPROGRESSINFO` (0x1000B) with a `RILCALLINFO` (0x430 bytes) whose `dwStatus` holds the
  CPI status when param bit 0x80 is set. rilgsm reports CPI support unconditionally (`RIL_GetDevCaps(0x21)` → 1), so
  CellTSP runs in CPI mode. [DISASM]
- **Operator names.** rilgsm only asks `AT+COPS=3,2;+COPS?` (numeric) and names the network from its 2007 table or from
  `HKLM\Software\Microsoft\RIL\OperatorNames` (value name = MCC+MNC, data = name), else shows the digits (the CellEmu
  "35638" case). It re-reads on any `*EREG:` (same status is enough). The helper writes the phone's name there
  (`Status.cs` `NameOperator`, status line `num=`), sends `radio refresh`, and the app has the modem repeat `*EREG`.
  The modem also serves CPHS ONString/ONShort (`+CRSM` 192/176 on 6F14/6F18); rilgsm reads them but the registry is what
  worked. Signal: `+CIEV: 2,<bars+1>`; roaming: `*EREG: 5` (WM shows the roaming triangle). [OBSERVED]

**Own number.** `AT+CNUM` answers `+CNUM: "","<number>",<129|145>` with `PhoneNumber.own()` (Phone calls settings
"My phone number", else `SubscriptionManager.getPhoneNumber` / `line1Number`, permission `READ_PHONE_NUMBERS` asked only
with calls on). rilgsm returns it from `RIL_GetSubscriberNumbers` (checked with the trace client); CellTSP caches it as its TAPI address
when the radio starts, and Phone > Menu > Options shows it in the page title ("Phone - +1 (555) 123-4567"). Because of
that cache a number that arrives later (permission granted, setting changed, restored state) shows after WM's next
restart. [OBSERVED]

**App side.** `PhoneStatus` → `NativeBridge.modemNetwork` every status tick; `modemEvents` is polled by `CallBridge`
(same `dial/end/hold/mute` verbs as the helper's `call …` lines), and `CallBridge.state()` answers a modem call with
`modemCallState` instead of the mailbox. If the app does not take a dial within 10 s (60 s while it asks for a
permission) the modem ends it with NO CARRIER. The helper's own RIL client (`Calls.cs`) stays off on these images; its
`ril.trace=1` state.ini switch logs RIL notifications, call lists and TAPI call states to `helper.log` (that is how the
`+CLCC` problem was found).

**CLI.** `--trace-modem`, `--modem-net T:"svc=1 sig=3 roam=0 num=310260 op=Name"`, `--modem-call T:id:state`
(pending/dialing/active/held/ended), `--modem-stub` (answer after 3 s), `--modem-refresh T`; modem events are logged as
`modem events: dial 1 5550123`.

## Sources

- Cellular Emulator overview: https://learn.microsoft.com/en-us/previous-versions/bb158495(v=msdn.10)
- Cellular Emulator UI (status bar COM port, AT log format, Call Manager, Data Channels): https://learn.microsoft.com/en-us/previous-versions/bb158487(v=msdn.10)
- Cellular Emulator configuration (USIM.xml, filter DLLs): https://learn.microsoft.com/en-us/previous-versions/bb158493(v=msdn.10)
- Cellular Emulator installation (ships with the 6.5.3 DTK, virtual COM ports): https://learn.microsoft.com/en-us/previous-versions/bb158494(v=msdn.10)
- Cellular Emulator troubleshooting (seven XPVCOM pairs, 35638 = FKNET): https://learn.microsoft.com/en-us/previous-versions/bb158499(v=msdn.10)
- Cellular Emulator data connections: https://learn.microsoft.com/en-us/previous-versions/bb158505(v=msdn.10)
- F. Zandoná, "WM 6 SDK and Cellular Emulator (Can you hear me now?)" (WM6 images have no signal; WM5 = FakeRIL; WM5 cannot use CellEmu): https://learn.microsoft.com/en-us/archive/blogs/fzandona/wm-6-sdk-and-cellular-emulator-can-you-hear-me-now
- D. Glover, connecting DE v2 to CellEmu (type the port name, soft reset): https://learn.microsoft.com/en-us/archive/blogs/dglover/how-to-connect-the-windows-mobile-device-emulator-v2-to-the-wm6-sdk-cellular-emulator
- B. Bond, "Fake phone calls in the DeviceEmulator" (FakeRIL numbers): https://learn.microsoft.com/en-us/archive/blogs/barrybo/fake-phone-calls-in-the-deviceemulator-pdc-2005-followup
- F. Zandoná, DeviceEmulator phone numbers: https://learn.microsoft.com/en-us/archive/blogs/fzandona/windows-mobile-deviceemulator-phone-numbers
- RIL proxy logging (RILProxyLogNormal/Detail, SIGNALQUALITY example values): https://learn.microsoft.com/en-us/archive/blogs/cenet/ril-proxy-logging-the-most-important-debugging-tool-for-investigating-radio-and-ril-issues
- RIL registry settings (CE 6.0: DataPort, Contexts, EONS/CPHS): https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ee497232(v=winembedded.60)
- ToughDev, "Connect WM6 SDK Emulators to GSM Modem" (ComPort VEI1: → COM1:): https://www.toughdev.com/content/2008/11/connect-wm6-sdk-emulators-to-gsm-modem/
- B4X forum thread on the emulator and CellEmu: https://b4x.com/android/forum/threads/windows-mobile-emulator-and-cellular-emulator.4715
- DevX, "Using the Cellular Emulator in the Windows Mobile 6 SDK": https://www.devx.com/wireless-zone/40981/
- Linux CAIF sources (v6.6): https://github.com/torvalds/linux/tree/v6.6/net/caif and https://github.com/torvalds/linux/blob/v6.6/include/net/caif/cfctrl.h
- DE 3.0 help (`DeviceEmulator.chm`, local): `/u0`..`/u2` mapping, Peripherals tab
