# Bluetooth for the DE images

Research notes for WMDroid. Main target: WM 6.5.3 Professional `PPC_USA_GSM_FWVGA_VR.BIN`. Companion to
`research-gps-network.md` (serial ports) and `folder-sharing-protocol.md` (the mailbox).

Tags:

- **[ROM]**: read from the running 6.5.3 guest, using the helper's `regdump`/`ls` and a RAM dump with File Explorer
  loaded. Nothing here was disassembled.
- **[OBS]**: tried in the CLI (`/data/local/tmp/wm/bt`, `--load-state test/th.wms --mem 192 --screen 480x854`).
- **[DOC]**: Android or Microsoft documentation, from memory. **[INFERRED]** / **[GUESS]**: not verified.
- **[CODE]**: WMDroid's source as it is today.

---

## TL;DR

| | Finding |
|---|---|
| Today (phase 1) | **Share bridge, done.** Start > *Send via Bluetooth* sends a file through the phone's Bluetooth picker. Files the phone receives over Bluetooth show up in `\Storage Card\Bluetooth` with a WM notification. This needs no guest Bluetooth stack. [CODE][OBS] |
| Guest stack | **Complete Microsoft stack in ROM**: `btd.dll`, the UART transports `bthuart.dll` (H4), `bthcsr.dll` (BCSP) and `bthsc.dll`, the universal loader `bthuniv.dll`, OBEX (`obexsrvr`, `obexinbx`, `obexapi`), `beam.exe`, `wrlsmgr.exe`, A2DP, AG, HID, PBAP, and ActiveSync over BT (`bthasplugin.dll`). It stays idle only because no transport is configured. [ROM] |
| Hook point | Add `HKLM\Software\Microsoft\Bluetooth\Transports\BuiltIn\1` = `bthuart.dll` on a free UART. Then a virtual HCI controller in the core answers it. **No guest code is needed.** [ROM][INFERRED] |
| Hard limit | Android apps get **RFCOMM sockets only** for classic Bluetooth. There is no raw HCI, no classic L2CAP, and no SCO. So the controller has to terminate HCI, L2CAP, SDP and RFCOMM itself, and forward RFCOMM *streams*. Profiles over raw L2CAP (A2DP, HID, PAN/BNEP) or SCO (HFP/HSP audio) **cannot work**. [DOC] |
| What it would add | WM's own Beam UI to real devices (OBEX Object Push), Bluetooth GPS and other SPP gadgets, OBEX FTP, ActiveSync over Bluetooth, and DUN to the rare devices that still offer it. **Receiving** beams stays with phase 1, because Android's own OPP server owns that UUID. |
| Size | **About 4.5k lines, 3–5 weeks** (§5). |
| Recommendation | Keep phase 1. Build phase 2 only if WM-side Beam or SPP devices (GPS, OBD-II, printers) are wanted. If so, start with the "This phone" OPP target (§4.4): it needs no remote device and shows the whole path working. |

---

## 1. Phase 1: the share bridge (implemented)

| Direction | Guest | App |
|---|---|---|
| Send | `WMDroid.exe send [file]`. The Start menu link *Send via Bluetooth* opens a file picker; with a path it sends at once. The file is copied to `\Storage Card\.wmdroid\send\` and the helper posts `share <name>` (guest/Helper/Bluetooth.cs). | `GuestBridge` → `EmulatorActivity.onGuestShare` → `BluetoothBridge.share`. A FileProvider URI (`${applicationId}.files`, `res/xml/file_paths.xml`) goes in `ACTION_SEND`, sent straight to `com.android.bluetooth/.opp.BluetoothOppLauncherActivity` (that opens the device picker), with the share sheet as fallback. While the picker is up the guest is suspended without locking (`ownUi`). |
| Receive | `received <name>` → one SHNotificationAdd bubble lists everything not yet dismissed, with *Open* / *Show folder* links and softkeys (*Folder* / *Dismiss* for several files). The sink is a `MessageWindow` on its own pumping thread. | A `ContentObserver` on `MediaStore.Downloads` (plus a scan at start and on each helper hello) finds rows with `owner_package_name LIKE '%bluetooth%'` above a baseline id taken at first use. They are copied into `getExternalFilesDir()/Bluetooth`, which every storage mode overlays as `\Storage Card\Bluetooth`, and then `received` is sent. Copied ids are remembered (`bt.copied`). Needs All files access, toggled in Settings > Phone integration > Bluetooth. |

Facts behind the choices:

- **File Explorer has no extension point usable from .NET.** aygshell reads `HKCR\<class>\Shellex\ContextMenuHandlers`
  (the ROM uses it for "Set As Ringtone", CLSID `{E88B4A18-6F77-4bb6-8E0D-DCD4B1F8F1DA}`, on audio classes) and
  `HKLM\Software\Microsoft\Shell\Extensions\ContextMenus\<app>\<context>`, which has only `ActiveSync` and `Contacts`
  here. Both need a native in-proc COM server (IContextMenu). That could be done the way `wmdcam.dll` is
  (`tools/make-wmdcam.py`), but it would also need an import table, so it is a separate task. [ROM]
- **On this phone (Galaxy Z Fold7) received files land in `Download/`**, not a Bluetooth folder, with
  `owner_package_name=com.android.bluetooth`. MediaStore is therefore a better test than any folder name. Rows being
  received are `IS_PENDING` and left out of queries until complete. [OBS][DOC]
- `com.android.bluetooth/.opp.BluetoothOppLauncherActivity` resolves `ACTION_SEND` for `image/*`, `text/plain`,
  `application/octet-stream` and zip on this phone (`cmd package query-activities`). Other packages with "blue" in the name
  (e.g. a messaging app) do not contain "bluetooth" and are skipped. The manifest declares a `<queries>` entry for
  `ACTION_SEND */*`. [OBS]
- **`fexplore.exe <folder>` opens that folder only when the path is unquoted**, even with spaces. A quoted path shows
  nothing. [OBS]
- The ROM file `File.Copy` keeps READONLY, so the send copy clears attributes, both before an overwrite and after the copy. [OBS]

Verified in the CLI [OBS]: the Start menu link is created on install; `send` with a path copies the file and posts
`share bth.gif`; the picker lists folders and files, and navigates with the D-pad and Enter; one `received` shows
"File received" with *Open* / *Dismiss*; two show "2 files received" with *Folder* / *Dismiss*; the left softkey sends
WM_COMMAND 1002 to the sink, which opens File Explorer at `\Storage Card\Bluetooth`. The Android side compiles. The
share intent and the MediaStore watcher have not been run on the phone yet, because that needs the new APK installed.

## 2. What the guest already has

From the 6.5.3 guest [ROM]:

| Area | Contents |
|---|---|
| Stack | `btd.dll` (HCI/L2CAP/SDP/RFCOMM in one), `btdrt.dll`, `bthutil.dll`, `sdputil.dll`, `sdpready.exe`, `bthlink.dll` |
| Transports | `bthuniv.dll` (universal loader; `Bluetooth\Transports\BthUnivPresent=1`). `Transports\PnP` has eight entries, all **without a port name**: `bthamb` 115200; `bthuart` 460800 (flags 0x80000001), 115200 (flags 4), 7200 (flags 4), 115200 (resetdelay 5000, flags 0); `bthcsr` 115200 and 921600 (flags 1); `bthsc` 9600. `Transports\BuiltIn` **does not exist**, so nothing is ever opened. |
| Profiles | `Bluetooth\Services`: SPP `1101`, DUN `1103`, HSP `1108`, A2DP sink side `110B`, HFP `111E`, HID `1124`, plus a vendor UUID. Also `[pan]` (ActivateOnBoot=0), `[Hid]`, `[AudioGateway]` (BTAGPBModule=`BthAGPhonebook.dll`), `[Serial]\Ports` |
| OBEX | `Software\Microsoft\Obex`: `IsEnabled=1`, `protocols=bth,irda`. Services: the inbox `{0000…}` (`obexinbx.dll`, OPP, with an SDP record) and PBAP (`bthpbap.dll`). `beam.exe` and the Start "Beam" icon are present |
| UI | `wrlsmgr.exe` (Wireless Manager), the Bluetooth Settings applet |
| Class of device | `Bluetooth\sys\COD = 0x1A0114` (phone / smartphone, with object transfer, networking and telephony bits) |

Serial ports (see `research-gps-network.md` §A1) [ROM]:

| UART | Guest | Use |
|---|---|---|
| UART0 `0x50000000` | COM1, `Drivers\BuiltIn\Serial`, `serial_smdk2410.dll`, Irq 28 | GPS (planned) |
| UART1 `0x50004000` | no driver | OAL debug output: **not usable**, because the kernel writes text to it |
| UART2 `0x50008000` | `Drivers\BuiltIn\IRDA2410` (COM3, `IRDA2410.Dll`, which is missing from the image, so the key fails) | **Free, recommended for HCI** |

## 3. Phase 2 design: a virtual HCI controller

```
 WM apps (Beam, File Explorer, GPS apps, ActiveSync)
   │ Winsock AF_BTH / OBEX / virtual COM
 btd.dll  (HCI host, L2CAP, SDP, RFCOMM)
   │ bthuart.dll: H4 over COM4: (serial_smdk2410.dll on UART2)
 ─── emulated UART2 ────────────────────────────────────────────
 core: VirtualHci  (HCI controller + L2CAP/SDP/RFCOMM endpoint)
   │ JNI: device list, "open RFCOMM to <addr,uuid>", byte pipes
 app:  BluetoothRadio.kt (BluetoothAdapter / BluetoothSocket)
   │
 phone's Bluetooth → nearby devices
```

### 3.1 Guest provisioning (helper, about 100 lines)

1. Re-point the dead IRDA2410 key at the working serial driver on UART2. Copy the `Serial` key, then set
   `MemBase=0x50008000`, the UART2 Irq, `DeviceArrayIndex=2` and `Port=COM4:` (avoiding COM3 keeps old IrDA references
   harmless). [INFERRED: the Irq numbering in the `Serial` key is the raw interrupt index, 28 = UART0, so UART2 would be 15;
   check against `serial_smdk2410.dll` before relying on it.]
2. `HKLM\Software\Microsoft\Bluetooth\Transports\BuiltIn\1`: `driver=bthuart.dll`, `name=COM4:`, `baud=115200`,
   `flags=0`, `resetdelay=0`. [DOC: the documented CE values; unverified on this image]
3. Soft reset (Patch.cs already handles one-time reset patches). Wireless Manager should then show Bluetooth, and
   *Turn on* sends HCI_Reset.

Alternative with no UART: a small RAM DLL implementing the CE HCI transport interface (`HCI_ReadHciParameters`,
`HCI_StartHardware`, `HCI_OpenConnection`, `HCI_ReadPacket`, `HCI_WritePacket`, …) over an MMIO mailbox, like
`wmdcam.dll`. [DOC, names from memory] It avoids H4 framing and the serial driver's timing, but it means writing ARM code. The UART path
reuses ROM code unchanged, so prefer it.

### 3.2 Core: `VirtualHci` (C++, about 3k lines)

**HCI controller** (about 900 lines). H4 framing on UART2 TX/RX, using the RX level-trigger work planned for GPS.

- Commands btd issues on bring-up: Reset; Read_Local_Version; Read_Local_Supported_Features/Commands; Read_BD_ADDR (a
  fixed locally administered address); Read_Buffer_Size; Write_Scan_Enable; Write_Class_of_Device; Write_Local_Name;
  Set_Event_Filter; Write_Page/Inquiry_Scan_Activity; Write_Authentication_Enable; Write_Encryption_Mode; link policy.
  Advertise BT 2.0 without SSP. The 6.5 stack predates Secure Simple Pairing, so it uses legacy PIN pairing. [INFERRED]
- Inquiry → Inquiry_Result (address, CoD) from the app's device list, then Inquiry_Complete. Remote_Name_Request →
  the Android name.
- Create_Connection → Connection_Complete at once. The ACL is virtual; real links open per RFCOMM channel (§3.3).
  Disconnect → close all pipes for that handle.
- Security: Link_Key_Request → answer with a stored per-device dummy key, or a negative reply so that WM asks for a PIN.
  PIN_Code_Request_Reply → call `BluetoothDevice.createBond()`, which shows **Android's** pairing UI, and report success
  once BOND_BONDED. Authentication_Requested / Set_Connection_Encryption → succeed.
- ACL data with Number_Of_Completed_Packets flow control.

**L2CAP endpoint** (about 500 lines). Signalling on CID 1: Connection Req/Rsp, Configure (accept the MTU), Disconnection,
Echo, and Information (no extended features). Accept **PSM 1 (SDP)** and **PSM 3 (RFCOMM)**. Refuse everything else with
"PSM not supported": AVDTP 0x19, HID 0x11/0x13, BNEP 0x0F.

**SDP server, for the remote side** (about 600 lines). When WM queries a remote device, build records from
`BluetoothDevice.getUuids()` (refreshed with `fetchUuidsWithSdp()`). Each RFCOMM-based UUID gets a stable fake server
channel (e.g. OPP 0x1105 → 12, FTP 0x1106 → 10, SPP 0x1101 → 1, DUN 0x1103 → 2, PBAP 0x112F → 19, MAP 0x1132 → 16,
other UUIDs from 20 up). Each record carries the ServiceClassIDList, a ProtocolDescriptorList (L2CAP, RFCOMM ch, plus
OBEX for OBEX profiles), the BluetoothProfileDescriptorList and ServiceName. Implement ServiceSearchAttribute well,
because btd and beam use it; the other two PDUs can be simple.

**RFCOMM mux (TS 07.10)** (about 900 lines). Handle SABM/UA/DM/DISC on DLCI 0 and on data DLCIs; PN (accept the frame size,
enable credit-based flow control); MSC/RLS/RPN echo; UIH data with credits; FCS. On SABM for channel *n*, ask the app to
open `createRfcommSocketToServiceRecord(uuid[n])`. Answer UA when it connects, or DM if it fails. Bytes then flow both
ways, and DISC closes the socket. Back-pressure: grant credits only while the app-side write queue is short.

**Snapshot.** Connections cannot survive a state save. Serialize "radio on, paired list", and on restore drop the links
(HCI Disconnection_Complete) so that btd recovers. Use the optional trailer, as other new devices do.

### 3.3 App: `BluetoothRadio.kt` (about 800 lines with JNI)

- Permissions: `BLUETOOTH_CONNECT`, plus `BLUETOOTH_SCAN` with `neverForLocation`. Ask for them only when WM turns Bluetooth on.
- Device list = bonded devices plus `startDiscovery()` results while WM inquires (stop after the inquiry length).
  `getAddress()` of the phone itself is hidden, which is why the controller uses its own address.
- RFCOMM client pipes: one thread per socket. Use secure sockets for bonded devices and insecure sockets for OPP pushes
  to devices that allow them.
- RFCOMM servers (WM as server): `listenUsingRfcommWithServiceRecord(name, uuid)` **only for UUIDs Android does not
  serve itself**. That means SPP and vendor UUIDs, not OPP, PBAP, MAP or HFP. An accepted socket becomes an incoming
  Connection_Request, then L2CAP, then SABM toward the guest's channel (read from the guest's own SDP server through
  the controller's SDP client).
- Reflection on `createRfcommSocket(int channel)` is a non-SDK API and is blocked at targetSdk 36, so channel numbers
  stay virtual and UUIDs do the routing. [DOC]

### 3.4 OBEX: pass through, with one exception

OBEX runs end to end between WM and the remote device over the RFCOMM pipe, so it does not need to be terminated. The exception is
**"This phone (WMDroid)"**, a fake device always present in inquiry results. Its OPP (and optionally FTP) channel is
served by the app itself. A small OBEX server (CONNECT, PUT with Name/Length/Body, DISCONNECT; about 300 lines) saves into
`Download/` through MediaStore. That gives WM's real Beam UI a target with no second device, and it makes a good first milestone.

## 4. What it enables, and what it cannot

| Use | Path | Works? |
|---|---|---|
| Beam a contact, picture or file (Contacts, Pictures, File Explorer *Beam*) to a nearby phone or PC | OPP client over RFCOMM | **Yes** |
| Beam to the phone itself | "This phone" OPP target (§3.4) | **Yes** (phase 1 already covers this) |
| Receive beams from other devices | Remote → OPP on the phone | **No.** Android's OPP owns the record; phase 1 delivers the files anyway |
| Bluetooth GPS mice, OBD-II (ELM327), BT serial printers, scanners | SPP client → virtual COM (`Bluetooth\Serial\Ports`) | **Yes**, the strongest new capability |
| OBEX FTP browsing of other devices | FTP client | Yes, if the remote device offers FTP (few modern phones do) |
| ActiveSync over Bluetooth | `bthasplugin.dll` → the PC's BT serial port + WMDC | Possible but niche. WMDC on Windows 10/11 is fragile |
| DUN (WM dials through another phone) | DUN client | Rare. Modern phones dropped DUN; NAT networking already exists |
| Headsets and car kits (HFP/HSP) | RFCOMM control + **SCO audio** | **No.** Apps cannot open SCO; WM audio already plays on the phone |
| Stereo headphones (A2DP) | L2CAP AVDTP | **No** (and audio already routes through Android) |
| Keyboards and mice (HID) | L2CAP 0x11/0x13 | **No.** Phone-paired keyboards already reach WM through the key map |
| PAN networking | BNEP | **No.** Networking already goes through NAT |

## 5. Size estimate

| Piece | Lines | Time |
|---|---|---|
| Guest provisioning (UART2 COM4, transport key, reset) | ~100 C# | 1 d |
| UART2 H4 plumbing + RX level trigger (shared with GPS) | ~150 C++ | 1 d |
| HCI controller | ~900 C++ | 4–5 d |
| L2CAP endpoint | ~500 C++ | 2 d |
| SDP server/client and record synthesis | ~600 C++ | 3 d |
| RFCOMM mux + credits | ~900 C++ | 4–5 d |
| JNI + `BluetoothRadio.kt` (discovery, bonding, pipes, permissions) | ~800 Kotlin/C++ | 3–4 d |
| "This phone" OBEX OPP server | ~300 Kotlin | 1–2 d |
| Snapshot handling, testing against real devices | — | 3–5 d |
| **Total** | **~4.5k** | **3–5 weeks** |

Main risks: btd's exact bring-up command set and timeouts (log every HCI command first); legacy PIN pairing against
Android bonding (whether WM accepts a link key that was never really negotiated); and RFCOMM flow control with slow
remote devices. A test path that needs no hardware: run the "This phone" OPP target in the CLI with a host-side fake
`BluetoothRadio`.
