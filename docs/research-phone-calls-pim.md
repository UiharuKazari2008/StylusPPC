# Phone calls (ring, answer, dial, fallback) and PIM mirroring for WMDroid

Scope: WM 6.5.3 Professional guest on the user's Android phone (Galaxy Z Fold7, One UI, targetSdk 36).
The goal has two parts. (1) WM rings for real incoming calls and places real outgoing calls. If the guest does not acknowledge
the ring in time, the stock phone app handles the call. (2) Contacts and calendar sync in both directions, with Android's state
winning conflicts.

Tags: **[ROM]** checked in the 6.5.3 image (extracted modules under the session scratchpad, `fs/`, `fw653/`).
**[DOC]** Android/AOSP documentation; AOSP javadoc was read for `InCallService`, `CallScreeningService` and `TelecomManager`.
**[PRIOR]** earlier WMDroid research docs. **[INFERRED]** reasoning not yet tested on a device.

---

## TL;DR

0. **Built (2026-10-16): outbound calls, see section E.** The guest side ended up on FakeRIL plus a 7-word patch of
   its dial thread, not on a transplanted `rilgsm.dll`; the Android side holds the dialer role when the user allows
   it and hands every call it cannot show to the phone's dialer by ending its in-call process.
   WM 6 / 6.1 / 6.1.4 have no FakeRIL: there the image's own `rilgsm.dll` dials through WMDroid's virtual AT modem on
   COM1, which hands the call to the same `CallBridge` path (`research-cellular.md` section 11).

1. **Recommendation: start as a companion, not as the default dialer.** Hold `ROLE_CALL_SCREENING` and leave Samsung Phone
   as the default dialer. Telecom binds our `CallScreeningService` for each new call and waits up to **5 s** for an answer
   [DOC]. That wait is where the safety check goes:
   - Ring WM, and wait up to about 3 s for the guest to acknowledge.
   - On ack, respond with `setSilenceCall(true)`, so WM rings and the stock UI stays silent.
   - Without an ack, respond "allow", and the stock phone app rings normally.
   - If WMDroid itself hangs, Telecom ignores us after 5 s and the call rings normally.

   The fallback costs no code and covers a hung app as well as a hung guest.
2. **With `ROLE_DIALER` there is no "divert to the main phone app."** Telecom binds only the default dialer's UI
   `InCallService`. It falls back to the preloaded dialer only if our service returns a *null binding*, and the docs say an app must
   never do that. The preloaded dialer is always used for emergency calls [DOC]. So the fallback would have to be **our own Android
   in-call UI** (answer, decline, speaker, mute, keypad). That mode is a later, optional phase (P6).
3. **Call audio never passes through WMDroid.** In both modes the modem and audio HAL route it to the earpiece, speaker or
   Bluetooth. Third-party apps cannot capture or inject cellular call audio. WM's speaker and mute buttons become *routing commands*
   [DOC][INFERRED].
4. **Guest side, recommended: transplant `rilgsm.dll` and emulate the modem in the core.** 6.5.3 has only FakeRIL, which has no
   incoming-call injection hook [ROM]. The WM6–6.5 images ship Microsoft's AT-based `rilgsm.dll`. It imports only `COREDLL`,
   `ossvcs` and `simsec`, all present in 6.5.3 [ROM]. Its call AT dialect is already mapped in [PRIOR] `research-cellular.md`
   §3.4. Copied into 6.5.3 with `ComPort` set to a spare UART, it gives the stock stack:
   - cprog's incoming and call-progress UI, lock-screen answer, caller-ID name lookup, the call log, and missed-call
     notifications;
   - an **in-core ring ack**: the RIL's `AT+CLCC` poll after `RING` arrives within milliseconds, with no mailbox round trip.

   Open risk: whether the 6.5.3 RIL proxy accepts the older driver. `ril.dll` has explicit version-negotiation errors [ROM].
   That needs a 1–2 day spike first.
5. **PIM:** the managed `Microsoft.WindowsMobile.PocketOutlook` assembly **is in the 6.5.3 ROM**. It is a thin wrapper over
   `pimstore.dll` flat exports. It has `RecurrencePattern`, `GetOccurrence`, custom properties (`AddCustomProperty`) and
   `ListChanged` [ROM]. It has **no PIM last-modified stamp**: `LastModified` belongs to `EmailMessage` [ROM]. It also has no
   recurrence-exception list [ROM]. WM-side change detection is therefore content hashes plus `ListChanged`. Android keeps the
   id map and decides every conflict.
6. **Effort:** about 6–8 weeks of focused work for P0–P5: spikes, companion telephony, the AT modem, call log, contacts and
   calendar. Adding the full `ROLE_DIALER` mode adds 2–3 weeks.

---

## A. Android side

### A.1 Two ways to hold calls

| | **Companion** (recommended first) | **Default dialer** (`ROLE_DIALER`) |
|---|---|---|
| How | `ROLE_CALL_SCREENING` → `CallScreeningService`, plus `TelephonyCallback.CallStateListener` (`READ_PHONE_STATE`), `TelecomManager.acceptRingingCall()`/`endCall()` (`ANSWER_PHONE_CALLS`), `placeCall()` (`CALL_PHONE`) | `RoleManager.createRequestRoleIntent(ROLE_DIALER)`; our `InCallService` gets `Call` objects |
| Incoming number | `Call.Details.getHandle()` in `onScreenCall`. Calls from contacts are passed only if we hold `READ_CONTACTS` (we need it for PIM anyway). Restricted/unknown presentations are **not** passed [DOC] | Always |
| Outgoing calls placed on Android | `onScreenCall` fires for outgoing calls too; the response is automatic [DOC] | Yes |
| Call states | Only IDLE/RINGING/OFFHOOK. Outgoing calls go OFFHOOK at dial time, so WM cannot tell alerting from answered. `PreciseCallState` needs a privileged permission [DOC][INFERRED] | Full `Call.STATE_*` (dialing, active, holding, disconnected with cause) |
| Answer / hang up from WM | `acceptRingingCall()` / `endCall()`. Both are deprecated but still documented as working with `ANSWER_PHONE_CALLS`. `endCall` cannot end emergency calls [DOC] | `Call.answer(videoState)`, `reject()`, `disconnect()`, `hold()`, `playDtmfTone()`, conference |
| DTMF, hold, swap from WM | **No API** | Yes |
| Speaker / mute | `AudioManager.setMicrophoneMute`; speaker through `setCommunicationDevice` (unverified for telephony calls) [INFERRED] | `InCallService.setAudioRoute(ROUTE_SPEAKER/EARPIECE/BLUETOOTH/WIRED_HEADSET)`, `setMuted` [DOC] |
| Silencing stock ringing | Only at screening time: `CallResponse.setSilenceCall(true)`. `TelecomManager.silenceRinger()` needs `MODIFY_PHONE_STATE` or the dialer role [DOC] | We own the ringer (`IN_CALL_SERVICE_RINGING` metadata) |
| Fallback when WM doesn't ack | Built in: don't silence; the stock UI rings. Telecom's 5 s screening limit also covers a hung WMDroid [DOC] | **We must ship a complete Android in-call UI.** Telecom falls back to the preloaded dialer only on a null binding (documented as a role violation) and for emergency calls [DOC] |
| Cost to the user | Displaces any existing screening app (Samsung Smart Call / Hiya spam protection) [INFERRED] | WMDroid is fully responsible for every call; a WMDroid bug means missed calls |

**Requirements for `ROLE_DIALER`** (AOSP `InCallService` javadoc) [DOC]:
- An activity handling `android.intent.action.DIAL`, category DEFAULT, with and without a `tel:` data scheme, and a dial pad.
- A service with `android:permission="android.permission.BIND_INCALL_SERVICE"`, `exported="true"`, an intent filter for
  `android.telecom.InCallService`, `<meta-data android:name="android.telecom.IN_CALL_SERVICE_UI" android:value="true"/>`,
  and optionally `IN_CALL_SERVICE_RINGING` (then we play the ringtone).
- "Should never return a null binding."
- Incoming UI: a notification on a high-importance channel with `setFullScreenIntent()` (heads-up while the phone is in use,
  full screen otherwise), using `Notification.CallStyle`.

Telecom still writes `CallLog.Calls`, so the dialer does not own the call log. The preloaded dialer always handles emergency calls.
An FGS of type `phoneCall` is the right type while a call is up; it needs `FOREGROUND_SERVICE_PHONE_CALL` plus the dialer role
(or `MANAGE_OWN_CALLS`) [DOC, recalled; verify on targetSdk 36].

**Companion requirements:**
- `CallScreeningService` with `android:permission="android.permission.BIND_SCREENING_SERVICE"` and an intent filter for
  `android.telecom.CallScreeningService`.
- Request the role through `RoleManager.createRequestRoleIntent(ROLE_CALL_SCREENING)`.
- Permissions: `READ_PHONE_STATE`, `READ_CONTACTS`, `ANSWER_PHONE_CALLS`, `CALL_PHONE` and `READ_CALL_LOG` (for the call-log mirror).

A third option, a companion `InCallService` (full call control while the stock dialer stays default), needs
`MANAGE_ONGOING_CALLS` plus a `CompanionDeviceManager` association with a watch [DOC]. A third-party app with no real watch
almost certainly cannot get it. Not pursued [INFERRED].

### A.2 Audio

Cellular voice is routed by the modem and audio HAL. The `VOICE_CALL`, `VOICE_UPLINK` and `VOICE_DOWNLINK` audio sources need
`CAPTURE_AUDIO_OUTPUT`, which is signature/privileged [DOC]. So:
- WMDroid never carries call audio, and the emulator's audio device must not try to.
- WM's ringtone is the only call sound WM produces. It plays through WMDroid's normal audio output (an Android `AudioTrack`).
  During a ring it should use `USAGE_NOTIFICATION_RINGTONE` attributes so it follows the ringer volume and DND [INFERRED].
- Stop guest audio output (or duck it) once a call is active.

### A.3 The ring/ack state machine (companion mode)

```
onScreenCall(details)                                      [Telecom: must respond within 5 s]
  if outgoing → respond(allow); tell guest "call log/outgoing mirror"; return
  t0 = now
  ensureGuestRunning()            # see A.4; may take 0..N ms
  guest.ring(number, presentation)  # AT path: +CRING/+CLIP into the emulated UART (core)
  wait until (ack) or (now - t0 > ACK_BUDGET = 3000 ms)
  ack → respond(silence=true); WMDroid shows a CallStyle/full-screen notification "Incoming call (Windows Mobile)"
        that opens EmulatorActivity; WM's ringtone plays
  no ack → respond(allow); stock phone rings; guest (if it comes up later) shows the call as a mirror only
TelephonyCallback: RINGING→OFFHOOK (answered anywhere) → guest "connected"; →IDLE → guest "disconnected"
guest ATA  → acceptRingingCall();  guest ATH/CHUP while ringing → endCall() (rejects);  while active → endCall()
```

Ack definition, best first:
1. **AT route:** the guest RIL reacts to `RING`/`+CRING` with `AT+CLCC` (§3.4 of `research-cellular.md`). The core sees it on
   the UART in under 100 ms. This proves the CPU runs, the serial driver works and the RIL processed the event. A second
   level of ack, "cprog visible", is not needed.
2. **Helper route:** the helper answers `ring` over the mailbox after its UI is up. Today's mailbox polls are 500 ms in the guest
   (`Helper.cs`) and 1 s in the app (`EmulatorActivity` 1 s tick). The worst case is about 1.5–2 s, which is still inside budget.
   Poll faster (100 ms) while a call is pending.

Keep the guest's ringtone and the Android notification in sync: if the call is answered or ended on Android, stop WM's ring at once
with `NO CARRIER` / `*ECAV …,0`.

**Ack budget:** 3000 ms if the session is live or paused in memory. If the session is hibernated, attempt the resume only when the
measured restore time is under about 2 s; otherwise respond "allow" at once and bring WM up as a mirror. Telecom's hard limit is
5 s including our bind time [DOC].

Edge cases:
- **Call waiting:** a second `onScreenCall` arrives while OFFHOOK. Map it to `+CCWA`. Answering it needs hold/swap, which has no
  companion API, so in companion mode allow it and let the stock UI handle it.
- **Hidden numbers** are never passed to the screening service [DOC]. A RINGING `CallStateListener` event without an
  `onScreenCall` means "unknown caller". Ring WM with no `+CLIP`, but WM can no longer silence the stock UI, so both ring.
  Acceptable and documented.

### A.4 What must wake when a call arrives

| Layer | Problem | Fix |
|---|---|---|
| App process | Process may be frozen (cached-app freezer) or dead | Telecom's bind of `CallScreeningService` (or `InCallService`) starts or unfreezes the process [DOC] |
| Native session | Today the session lives in `EmulatorActivity` (`handle`, `NativeBridge.pause`, `hibernate()` destroys it and keeps `resume.wms`) | Move session ownership to a service-scoped singleton so a service can `resume()`, or reload `resume.wms` without an Activity. The surface attaches later. **Needed for either mode** [INFERRED from code] |
| Keep running | Starting an FGS from the background is restricted | Exemptions that apply: the user turns off battery optimisation for the app; `SYSTEM_ALERT_WINDOW` (on Android 15+ it also needs a visible overlay) [DOC]. Ask for the battery-optimisation exemption during call setup. With `ROLE_DIALER`, the `phoneCall` FGS type applies |
| UI | Background activity start is blocked | Use a full-screen-intent notification. A notification tap is an allowed start [DOC]. `USE_FULL_SCREEN_INTENT` is user-grantable on Android 14+ |
| CPU limit | `setCpuLimit(CPU_SCREEN_OFF/BACKGROUND)` throttles the guest | Lift it to full while a call is pending or active |
| WM's own suspend | If WM itself is in `POWER_STATE_SUSPEND`, UART data does not wake it. WMDroid normally reports AC power, which usually prevents idle suspend [INFERRED] | The core detects the guest power-off/idle state and pulses EINT0 (power button) before `RING`, which also turns WM's screen on |
| Helper | The agent loop exits when `\Storage Card` disappears during suspend ([PRIOR] status-indicators §4.4) | Must be fixed for any helper-based ack |

---

## B. Guest side (WM 6.5.3)

What the ROM provides [ROM]:
- `phone.dll` exports only `PhoneOpenCallLog`, `PhoneGetCallLogEntry`, `PhoneCloseCallLog`, `PhoneSeekCallLog`, `PhoneMakeCall`,
  `PhoneAddSpeedDial`, `PhoneShowCallLog`, `PhoneSendDTMFStart`/`Stop` and `PhoneIsEmergencyNumber`. There is **no
  `PhoneAddCallLogEntry`**.
- The call log database is `clog.db`, owned by `tpcutil.dll`. Its writer is an ordinal-only export. `tpcutil` also exports
  `CIDFindContact`, which is caller-ID lookup in POOM.
- `cprog.exe` posts `MS_PHONE_MISSEDCALL`; `ossvcs.dll` keeps `Missed Call Count`.
- The RIL proxy is `ril.dll`, exporting `RIL_Dial`, `RIL_Answer`, `RIL_Hangup`, `RIL_ManageCalls`, `RIL_GetCallList`,
  `RIL_SendDTMF`, `RIL_Set/GetAudioMuting`, `RIL_Set/GetAudioDevices`, `RIL_DevSpecific` and others. It does driver version
  negotiation (strings such as "Incompatible versions of RIL Proxy and Driver", "Downgrade RIL Driver").
- TAPI (`tapi.dll`) and `cellcore.dll` (the cellular TSP and ExTAPI `line*`) sit between cprog and the RIL. **cprog's call UI
  only exists if a RIL driver produces call notifications.**

### B.1 Option (a): re-enable FakeRIL and drive it

FakeRIL handles outgoing calls itself: any number fake-connects, and special numbers (7272024 "always busy", 7272773 "never
answer", …) change the behaviour ([PRIOR] `research-cellular.md` §1.1). Strings in the 6.5.3 binary include numbers 7272xxx,
7273432, 7274386–7274391 and 7279000–7279201 [ROM].

What could inject an incoming call [ROM, strings and imports]:
- **No automation channel.** Winsock imports are only `socket`/`bind`/`sendto`/`setsockopt` (fake packet data), with no
  `recv`/`listen`. There is no message queue or named event API in its strings.
- **Extension points:**
  - `RIL_DevSpecific`: the only action string is "Switch Network".
  - `\windows\FakeRadioATSupport.dll` with `InitFakeRilATSupport`/`ReceiveCmd`: absent and its interface is undocumented.
  - `Comm\Cellular\RIL\RemoteCalltype` `CalltypeDLL`/`CalltypeFunction`: a native hook for call-type classification only.
- **Possible test hook:** one of the special numbers may trigger a callback ("incoming call"). This is unverified.

Re-enabling FakeRIL also brings back its "Fake Network" status events, the reason it was disabled (`Patch.cs` sets
`DEVFLAGS_NOLOAD`).

**Verdict:** useful only for outgoing calls plus a CF RIL client. The helper would `RIL_Initialize` with a callback (CF can
P/Invoke `ril.dll`, as the earlier `Probe ril` work did [PRIOR]), see cprog's dial, forward it to Android, and later
`RIL_Hangup` the fake call. Incoming calls are not possible unless the spike in P0 finds a callback number. Not recommended as
the main path.

### B.2 Option (b): a WMDroid RIL driver

**(b1) Transplanted `rilgsm.dll` plus an AT modem in the core. Recommended.** [ROM][PRIOR][INFERRED]
- `rilgsm.dll` is in the user's WM6/6.1/6.1.4/6.5 images (extracted copies exist for WM6 and 6.1.4). Its only imports are
  `COREDLL`, `ossvcs`, `simsec` and itself, all in 6.5.3. Its call commands include `ATD`, `ATA`, `ATH`, `AT+CLCC`,
  `AT+CHLD=0/1x/2x/3/4` and `+VTS`.
- Install from the user's own 6.5 image, copied by the helper into `\Windows`; device.exe loads drivers from RAM on these
  fully-trusted images ([PRIOR] trust level 2):
  1. Set `Drivers\BuiltIn\RIL\Dll=rilgsm.dll` and clear `DEVFLAGS_NOLOAD`.
  2. Set `Software\Microsoft\RIL\ComPort` to a UART port, and keep `DataPort` unused (no GPRS: Android networking already reaches
     the guest over NE2000).
  3. Soft-reset.
- **UART conflict:** COM1/UART0 is already WMDroid's GPS port (`GpsBridge.kt`; [PRIOR] gps-network §B). UART2 (`0x50008000`) has
  a dead `COM3:` key: `IRDA2410.Dll`, which is absent [ROM]. Re-point that key to `serial_smdk2410.dll` (keep `IoBase`/`Irq`, add
  `MemBase`/`MemLen`/`DeviceArrayIndex=2` like the COM1 key) and use `ComPort=COM3:`. Alternatively move GPS there. Needs a
  UART2 instance in `s3c2410.cpp`.
- Core work: the "EmuModem" from [PRIOR] `research-cellular.md` §9, Mode B (plain AT, no CAIF).
  - Answer the init list (§3.2), SIM/registration/operator/`+CIND` (§3.4), and the call set: `+CRING: VOICE`, `+CLIP`, `*ECAV`
    states, `+CLCC`, `ATA`/`*EVA`, `ATH`/`*EVH`/`+CHLD`, `ATD…;`, `+VTS`, `+CMUT`.
  - Expose a small JNI surface: `modemRing(number)`, `modemCallState(id, state)`, plus callbacks `onDial(number)`, `onAnswer()`,
    `onHangup()`, `onDtmf(c)`, `onMute(b)`.
- Bonus: signal, operator and SMS (`+CMT`) then flow through the real stack. The SNAPI-writing workaround becomes optional.
- **Risk:** the `ril.dll` proxy–driver version check. Spike first (P0). If the proxy rejects it, check whether `ril.dll` and
  `cellcore.dll` from the same 6.5 image can be transplanted as a set. This is riskier because cprog and ossvcs were built
  against the 6.5.3 versions.

**(b2) A native RIL driver written from scratch.** The driver↔proxy contract (`RIL_IOControl` IOCTL codes, RILVERSIONEX, the
notification queue) is private (`public\cellcore\ril` in Platform Builder). It would have to be reverse-engineered from
`ril.dll` and `fakeril.dll`. It also needs the cegcc/mingw32ce toolchain, which is not installed ([PRIOR] modern-usability
§3.5).

Minimum surface for cprog:
- equipment state and ready state (0x1F);
- `GetDevCaps`, SIM ready/locked state, subscriber number;
- registration, current operator, signal;
- `GetCallList`, `Dial`, `Answer`, `Hangup`, `ManageCalls`, `SendDTMF`, audio muting/devices/gain;
- CallerId/CallWaiting settings;
- notifications: RING, CALLERID, CONNECT, DISCONNECT, CALLSTATECHANGED, REGSTATUSCHANGED, SIGNALQUALITY,
  RADIOEQUIPMENTSTATECHANGED, RADIOPRESENCECHANGED;
- `RIL_E_NOTSUPPORTED` for everything else.

Effort: 3–6 weeks, and the RE risk is high. Use only if (b1) fails.

**(b3) A shim around FakeRIL** (our DLL loads `fakeril.dll` and filters IOCTLs) needs the same private IOCTL knowledge as (b2).
It offers no advantage over (b2).

### B.3 Option (c): no RIL

The helper (CF 3.5) draws its own incoming-call form (Answer / Ignore / Mute / Speaker) and plays the ringtone from
`HKCU\ControlPanel\Sounds\RingTone0`. It writes SNAPI `Status` bits `PhoneIncomingCall 0x10000`, `PhoneCallTalking 0x20000000`,
`PhoneCallCalling 0x20000` and `PhoneMissedCall 0x800`, plus `Incoming Caller Number/Name` ([PRIOR] status-indicators §1.2).
Outgoing calls are captured by a "Dial" helper UI or a Talk-key hook, not by cprog.

Drawbacks:
- ossvcs may overwrite the call bits, although with FakeRIL off there is no RIL to trigger rewrites.
- No cprog UI, no call log, no Today/lock-screen call scene.
- Every number typed into WM's own dialer goes nowhere.

Effort: 4–6 days. A reasonable stop-gap only.

### B.4 Call log and missed calls

- With a RIL (a or b), cprog and `tpcutil` write `clog.db` and raise missed-call notifications themselves (`MS_PHONE_MISSEDCALL`,
  `Missed Call Count`, `PhoneMissedCall` bit) [ROM][INFERRED]. One more advantage of (b1).
- **History import** (Android `CallLog.Calls`, needs `READ_CALL_LOG`, into WM) is needed in any design for parity with calls
  handled while WM was down. No public writer exists [ROM]. Route: P/Invoke the CEDB/EDB API in coredll
  (`CeOpenDatabaseInSession`, `CeSeekDatabaseEx`, `CeWriteRecordProps`; all present [ROM]) on `clog.db`.
  1. Find which volume holds it with `CeFindFirstDatabaseEx`.
  2. Learn the record schema by dumping entries that cprog wrote for test calls placed through the new RIL.
  3. Dedupe by (time, number, type).

  Effort: 2–3 days [INFERRED].
- Missed calls handled only on Android (no ack): write the log entry. Optionally bump `Missed Call Count` (HKCU
  `System\State\Phone`) and post `SHNotificationAdd` with our own CLSID.

---

## C. PIM 2-way sync (Android authoritative)

### C.1 Android sources [DOC]

**Contacts (`ContactsContract`):**
- Work on **RawContacts** (`_ID`, `CONTACT_ID`, `ACCOUNT_TYPE/NAME`, `VERSION`, `DELETED`, `DIRTY`) and their **Data** rows by
  MIME type: StructuredName (with `PHONETIC_*`, which maps to WM Yomi fields; useful for Japanese names), Phone(TYPE),
  Email(TYPE), StructuredPostal, Organization, Event(BIRTHDAY/ANNIVERSARY), Note, Website, Nickname, Im, Photo, GroupMembership.
- Stable id: the aggregate `Contacts.LOOKUP_KEY`, which survives re-aggregation. Keep the raw contact ids beside it.
- Change detection:
  - `RawContacts.VERSION`, which increments on any change to the raw contact or its data;
  - `Contacts.CONTACT_LAST_UPDATED_TIMESTAMP` for a cheap "since" query;
  - `ContactsContract.DeletedContacts` (with `CONTACT_DELETED_TIMESTAMP`) for deletions;
  - a `ContentObserver` on `ContactsContract.AUTHORITY_URI` as the trigger (coarse; debounce about 2 s).
- **Do not touch `DIRTY`.** It belongs to the account's own sync adapter (Google/Samsung). Our writes as a normal app set it, so
  Google uploads our edits.
- WM-created contacts go to a user-chosen account, by default the main Google account.

**Calendar (`CalendarContract`):**
- `Events` columns: `_ID`, `CALENDAR_ID`, `DTSTART`, `DTEND`/`DURATION`, `EVENT_TIMEZONE`, `ALL_DAY`, `RRULE`/`RDATE`/`EXDATE`,
  `ORIGINAL_ID`/`ORIGINAL_INSTANCE_TIME` (exceptions), `STATUS`, `AVAILABILITY`, `ACCESS_LEVEL`, `DELETED`, `DIRTY`.
  `Reminders` and `Attendees` are child tables.
- **No modification timestamp or version column**, so keep a per-event content hash in the map. Trigger with a `ContentObserver`
  on `CalendarContract.CONTENT_URI`.
- Only write to calendars with `CALENDAR_ACCESS_LEVEL >= CAL_ACCESS_CONTRIBUTOR`.
- Limit scope to selected calendars and a window (e.g. −90 days … +2 years). Recurring masters are always included.

### C.2 WM sources [ROM]

- `GAC_Microsoft.WindowsMobile.PocketOutlook_v1_0_0_0` is in the ROM. It P/Invokes `pimstore.dll` (Do* flat exports such as
  `DoGetItemFromOid`, `DoGetRecurrencePattern`, `DoGetAppointmentOccurrence`, `DoPutPicture`) and `cemapi.dll`.
- Usable surface:
  - `OutlookSession.Contacts/Appointments.Items` (`Restrict`, `Find`, `Sort`);
  - `PimItem.ItemId` (the CEOID, stable while the item lives);
  - `PimItem.Properties` with `AddCustomProperty`: store `WMDroid.Key` on every synced item;
  - `RecurrencePattern` (RecurrenceType, Interval, DayOfWeekMask, DayOfMonth, Instance, MonthOfYear, PatternEndDate,
    NoEndDate, Occurrences, `RecurringTimeZone`) and `GetOccurrence(DateTime)`, which creates or edits a single exception;
  - `ListChanged` events.
- **Missing:** a PIM `LastModified` (the property exists only on `EmailMessage`) and an exception enumerator (no
  `Exceptions` member).
  - Reading WM-made exceptions needs native `IRecurrencePattern::get_Exceptions`. CF 3.5 COM interop can do that:
    `[ComImport]` interfaces plus a `CoCreateInstance` P/Invoke for `IPOutlookApp` from `pimstore.dll`, which exports
    `DllGetClassObject` [ROM][INFERRED].
  - Fallback: treat WM edits to single occurrences as unsupported in v1. WM edits the series, or Android does.
- **WM change tracking:** none exposed for third-party apps.
  - Use a canonical-field **hash** per item, computed in the helper.
  - Use `ListChanged` (local and remote POOM notifications while the helper runs) as the trigger.
  - Detect deletes by diffing the OID set on each scan.
  - Not recommended: EDB change tracking (`CeAddSyncPartner`, `CeGetChangedRecords`, `CeMarkRecord`; exported [ROM]) on
    `pim.vol`, which `pimstore` owns. It is undocumented for POOM databases and could disturb ActiveSync-style replication state.

### C.3 Sync design

**Id map lives on Android.** Room table `pim_map`:

| Column | Meaning |
|---|---|
| `kind` | `C` (contact) or `E` (event) |
| `android_key` | lookup key / event id |
| `wm_oid` | WM item id |
| `android_ver` | VERSION, or event hash |
| `wm_hash` | WM item hash at last sync |
| `epoch` | guest store epoch |

**Guest store epoch.** WMDroid can restart from `boot.wms` ("power off" path) or cold boot, which loses guest PIM changes. The
helper keeps a GUID in `HKCU\Software\WMDroid\PimEpoch`, created when it first sees an empty store. If the epoch differs from
the map's, Android treats WM as untrusted:
- full re-push, Android wins;
- **no** WM→Android deletions;
- WM items without a key are offered as "new" only after user confirmation.

**Sync cycle** (triggered by an observer, guest `ListChanged`, guest resume, or every 15 min while running):
1. **Android delta.** Raw contacts whose `VERSION` changed, plus `DeletedContacts` since the last stamp. Events whose hash
   changed or that disappeared.
2. **WM digest.** The helper returns `oid|key|hash` for every item: one line each, about 60 bytes. 1,000 contacts is about
   60 KB, one file.
3. **Decide on Android, Android-wins:**
   - Android changed (whether or not WM also changed): push Android to WM. With "field merge" on, first apply WM-only changes
     to fields Android did not change, then push the merged item.
   - Only WM changed: pull the WM item and update the Android raw contact or event, touching only the mapped fields. Unmapped
     Android data (extra phones, IM, groups, attendees) is never deleted.
   - New on WM (no key): create on Android, then write the key back to WM.
   - Deleted on Android: delete on WM.
   - Deleted on WM: delete on Android only if Android is unchanged since the last sync and the epoch matches. Otherwise recreate
     on WM.
   - **Mass-delete guard:** if more than 10% or more than 20 items vanish on WM in one cycle, ask the user.
4. **Batching over the mailbox.** Write `in\pim-<seq>.xml` with 100 items per file. CF 3.5 has `System.Xml` and no JSON. Send
   one `pim apply <file>` command and receive `pim result <file>` with the new OIDs and hashes. The 0.5–1.5 s mailbox latency
   then matters only per batch.
5. **Commit.** Update the map only after the guest result arrives (idempotent; a key on the WM item makes retries safe).

**Initial import:** Android → WM only, after clearing WM's PIM (the user confirms). Typical sizes: 500–2,000 contacts and a
few thousand events. POOM saves are roughly 20–50 ms each in the guest, so allow about 1–3 min with a progress notification
[INFERRED]. Photos are optional: scale to 96×96 JPEG and send through `Contact.Picture` (`DoPutPicture`) in a second pass.

**Contact field mapping:**
- WM has fixed slots: Mobile, Home, Home2, Business, Business2, HomeFax, BusinessFax, Pager, Car, Radio, Company, Assistant;
  Email1–3; IM1–3; WebPage; Home/Business/Other address; First/Middle/Last/Suffix/Title/Nickname; Yomi First/Last/Company;
  Company, Department, JobTitle, OfficeLocation; Birthday, Anniversary; Spouse, Children; Categories; Body (note); Picture;
  per-contact ringtone.
- Fill slots by Android TYPE, in a deterministic order.
- Overflow Android numbers do not go to WM. They are never removed from Android because WM lacks them.
- Groups map to WM Categories, one way, Android → WM.

**Calendar mapping:**
- Subject/Location/Body ↔ TITLE/EVENT_LOCATION/DESCRIPTION.
- BusyStatus ↔ AVAILABILITY. Sensitivity ↔ ACCESS_LEVEL.
- The first ALERT reminder ↔ `ReminderSet` + `ReminderMinutesBeforeStart`.
- Calendar name → Category.
- **Times:** send instants as UTC. The helper converts with the guest zone, which WMDroid already syncs (`tz` command). All-day
  events go as date-only (Android stores UTC midnight, WM local midnight).
- **RRULE ↔ RecurrencePattern:**
  - DAILY;INTERVAL ↔ Daily.
  - WEEKLY;BYDAY ↔ Weekly + DayOfWeekMask.
  - MONTHLY;BYMONTHDAY ↔ Monthly.
  - MONTHLY;BYDAY=nXX ↔ MonthNth + Instance (−1 → 5).
  - YEARLY;BYMONTH;BYMONTHDAY ↔ Yearly.
  - YEARLY;BYMONTH;BYDAY=nXX ↔ YearNth.
  - COUNT ↔ Occurrences. UNTIL ↔ PatternEndDate.
  - Any other rule (several BYMONTHDAY, BYSETPOS, HOURLY, RDATE): expand into single read-only WM appointments within the window,
    keyed `e:<id>:<begin>`. WM edits to them are ignored or turned into Android exceptions.
- **Exceptions:**
  - Android exception rows (`ORIGINAL_ID` + `ORIGINAL_INSTANCE_TIME`) → WM `GetOccurrence(t)` then modify.
  - Cancelled instances and `EXDATE` → `GetOccurrence(t).Delete()`.
- **Time zone drift:** WM evaluates a recurrence in the device zone. An event whose `EVENT_TIMEZONE` differs from the phone's zone
  can drift by an hour around DST. Expand such series in v1.
- **Attendees: not synced in v1.** Adding WM Recipients turns the item into a meeting request and can send mail.

Caller ID becomes a side benefit: cprog resolves names through `CIDFindContact` against the mirrored POOM contacts [ROM].

---

## D. Phased plan

| Phase | Work | Effort |
|---|---|---|
| **P0 spikes** | (1) Copy `rilgsm.dll` from the user's WM 6.5 image into 6.5.3, set up UART2 as `COM3:`, and log the AT traffic in the core. Does the proxy accept the driver? (2) `CallScreeningService` prototype on the Z Fold7: bind latency, `setSilenceCall` behaviour, what Samsung's UI does when WMDroid answers through `acceptRingingCall`. (3) Measure resume time from paused and from `resume.wms`. (4) Optional: dial each FakeRIL test number and watch for RING | 3–4 days |
| **P1 Android telephony (companion)** | Role request UI and permissions; `CallScreeningService` + `CallStateListener`; `CallCoordinator` state machine with the ack budget; session ownership moved to a service; battery-optimisation exemption; full-screen CallStyle notification; answer/hang-up/dial bridge; emergency-number guard (WM dial of an emergency number opens Android `ACTION_DIAL`, never blocked) | 5–7 days |
| **P2 Guest calls via the AT modem** (b1) | UART2 device; AT engine with call states, CLIP, CLCC, DTMF, mute, init/SIM/registration answers; in-core ack; helper patch step to install `rilgsm` and its registry. If P0 fails: B.3 stop-gap (4–6 days) now, (b2) later (3–6 weeks) | 6–10 days |
| **P3 Call log** | Check cprog logging and missed-call UI; CEDB import of Android call history with dedupe | 2–4 days |
| **P4 Contacts sync** | Map, epoch, digest/apply protocol, field mapping, initial import, mass-delete guard, photos | 6–9 days |
| **P5 Calendar sync** | Window, hashing, RRULE mapping, exceptions, all-day/TZ, reminders; optional COM interop for WM exceptions | 8–12 days |
| **P6 (optional) Full default dialer** | `InCallService` + DIAL activity + complete Android fallback in-call UI (`CallStyle`, keypad, speaker/BT routing, hold/swap); full call states to WM | 10–15 days |

Order of value: P0 → P1 + P2 (WM rings, with a safe fallback) → P4 (caller names) → P3 → P5 → P6.

## Open questions to settle in P0

- Does 6.5.3 `ril.dll` accept the 6.5 `rilgsm.dll`? Which RIL version does each report?
- Does Samsung's in-call UI come to the front when `acceptRingingCall()` answers while WMDroid is in the foreground?
- How long does `NativeBridge.loadState(resume.wms)` take on the Z Fold7? This decides whether hibernated sessions can ring.
- Does `AudioManager.setCommunicationDevice(speaker)` affect a Telecom-managed cellular call from a non-dialer app?

## E. Outbound calls as built (2026-10-16)

Goal: dial in Windows Mobile's own Phone app (cprog), and a real cellular call goes out on the Android phone. WM's call
screen is the only UI. Its End / Mute / Hold (and Speaker, see E.5) act on the real call. Call audio stays on the
phone's earpiece and microphone; it never passes through the emulator.

### E.1 Design choice: FakeRIL + a connect gate, not an AT modem

| Option | Why not / why |
|---|---|
| B.2(b1) `rilgsm.dll` + AT modem on UART2 | Needs a Microsoft binary from *another* image (WM 6.1.4/6.5) on the phone, rebuilt into a loadable PE from IMGFS sections. It also needs a UART2 serial driver that the 6.5.3 image does not configure, and passing the unverified proxy/driver version check. Many unknowns, each a possible dead end. **Kept as the long-term option** (incoming calls with a real `RING`, SMS). |
| B.2(b2) own RIL driver | Private proxy↔driver contract; weeks of RE. No. |
| B.3 no RIL | WM's own call screen and call log would not be used, and that is the point. No. |
| **B.1 FakeRIL + helper RIL client + gate patch** | **Chosen.** Everything is already in the 6.5.3 image. cprog, TAPI/cellcore, the call screen with its timer, Mute/Hold, the call log, and caller ID all run unchanged. The one missing piece is *when* the call connects. That is solved with a 7-word in-memory patch of FakeRIL's dial thread (applied by the core since 2026-10-16). |

FakeRIL's voice-call progress (read from `fakeril.dll`, .text at 0x7B4B7000) [DISASM]:
- The dial thread waits 500 ms, then sets the call **ALERTING** (`RIL_CALLSTAT` 4).
- It classifies the number (fn 0x7B4D7F68) against its built-in test numbers: 7272773 → never answer (30 s), 7272024 → busy,
  7274391 / 7273432 / the 7272470 group → special sessions.
- For a normal number it waits `[obj+0x1ED0]` seconds and polls the hang-up event every 50 ms (loop at 0x7B4DC82C–0x7B4DC868).
  The constructor sets that field to the constant 2. It then sets the call **ACTIVE**.

**The gate** (`guest/Helper/Calls.cs`):
- The patch replaces the elapsed-time test in that loop with "has the word at 0x7B4DC838 changed since this call started?".
  That word is the old pre-loop test, which the patch now jumps over.
- So the call stays ALERTING (WM shows "Dialing...") until the helper bumps the word, when the app reports the real call
  as active. FakeRIL then turns the call ACTIVE within 50 ms (WM shows "Connected" with the timer).
- The counter lives inside FakeRIL's code page, so no shared memory is needed.
- Since 2026-10-16 the core applies the patch (`core/src/board/codepatch.cpp`, table `kGate653`): it finds the code by
  signature in physical memory just before it runs (and again when the page is paged back in), drops the page's
  translated blocks, and bumps the counter itself. The helper only drives the core's code-patch device (PA 0x5F100000,
  mapped with VirtualCopy): ARM = 1 while calls are on (0 removes the patch, so FakeRIL connects by itself again),
  GATE = connect now, STATUS bit 3 = the gate is in FakeRIL's code. No kernel mode in the guest any more.
- Words are checked before they are written, so other FakeRIL builds are left alone: `gate=0`, and calls then connect after
  FakeRIL's own 2.5 s. (WM 5's FakeRIL has a similar wait loop at a different layout; not patched.)

**FakeRIL must load at boot.**
- `Patch.cs` sets `DEVFLAGS_NOLOAD` on it (the "Fake Network" fights with PhoneState). With calls on, the helper clears that flag,
  and the app writes `patch.fakeril=on` so `Patch.cs` leaves it alone.
- `ActivateDeviceEx` can load FakeRIL at run time, and the helper's RIL client works then. **cprog, cellcore and ossvcs only
  bind to a RIL present at boot** [OBS, CLI], though: WM says "Phone off" and offers to turn the phone on.
- So enabling calls costs **one restart**. The helper reports `call ready gate=1 restart`, the app asks, and then sends the
  existing `patch` command (soft reset; `onGuestPatched` re-saves `boot.wms`).
- FakeRIL registers only when asked (status-indicators §4.3). The helper calls `RIL_RegisterOnNetwork` when the
  registration status is not home (2) or roaming (5).
- The title bar and Home screen keep showing the phone's real operator and signal: PhoneState re-asserts SNAPI every 500 ms.
  Verified: "T-Mobile" on Home and on the call screen while FakeRIL is registered on "Fake Network".

### E.2 Guest: the helper as a second RIL client

`Calls.cs` runs on its own worker thread, so a RIL call that never returns cannot stop the mailbox loop. It does this:
- `RIL_Initialize(1, …, CALLCTRL|NETWORK|RADIOSTATE)`. The RIL proxy accepts clients even with no driver loaded, so the
  helper first checks that `Drivers\Active` lists `Drivers\BuiltIn\RIL`. Reading FakeRIL's code page while the driver is
  unloaded killed the helper.
- The RIL callbacks run on a ril.dll thread. They only copy their data into a queue. An earlier version took a lock there
  and deadlocked against the main thread calling into the RIL.
- FakeRIL answers one request at a time after a simulated delay. Polling 8 times a second built a backlog, with replies
  arriving seconds late. So there is one request of each kind in flight, at most once a second, and a request with no reply
  is forgotten after 5 s.
- `RIL_GetCallList` runs after every call-control notification, and every second while a call exists. `RILCALLINFO` is
  1072 bytes here: id +8, direction +0xC, status +0x10, type +0x14, number at +0x2C. `RIL_GetAudioMuting` runs every
  second during a call (cprog's mute raises no notification).
- On helper exit (`update`) it calls `RIL_Deinitialize`. Without that, the old helper process (and its mutex) outlived the
  update.

Mailbox lines (several may share one message):

| Direction | Line | Meaning |
|---|---|---|
| guest → app | `call ready gate=<0/1> [restart]` | RIL client up; `restart`: FakeRIL was loaded late |
| | `call off <reason>` | calls unavailable (`disabled`, `noril`, `ril 0x…`) |
| | `call dial <id> <number>` | cprog placed an outgoing voice call |
| | `call end <id>` | WM's call is gone (its End, or our hang-up) |
| | `call hold <id> <0/1>`, `call mute <0/1>`, `call speaker <0/1>` | WM controls |
| app → guest | `call state <id> pending\|dialing\|active\|held\|ended [cause]` | real call progress |
| | `call info` | diagnostics, answered with a `call info …` line |

**Safety in the guest:**
- A dialled call the app does not acknowledge within 10 s is hung up (60 s after `pending`, which the app sends while
  Android asks for a permission). With calls off it is hung up at once. So WM never waits forever.
- The hang-up is `RIL_Hangup` (outbound only: at most one call).

### E.3 Android: `CallBridge` (main process) and `WmInCallService` (process `:call`)

- **Placing.** `TelecomManager.placeCall(tel:…, extras)` with:
  - a token, both top level and in `EXTRA_OUTGOING_CALL_EXTRAS`;
  - a fixed `EXTRA_PHONE_ACCOUNT_HANDLE` (the default outgoing account, or the only call-capable one), so a dual-SIM phone does
    not stop at a SIM picker.

  `CALL_PHONE` is asked for at enable time, or during the first dial (WM then gets `pending`). **Emergency numbers**
  (`TelephonyManager.isEmergencyNumber`, with a fallback list) open the phone's own dialer with the number filled in, and
  WM's call ends. They are never blocked or hidden.
- **With the dialer role** (Settings → Phone calls → "Use Windows Mobile's call screen", via
  `RoleManager.createRequestRoleIntent(ROLE_DIALER)`; giving it back goes through Android's default-apps page):
  - Telecom binds `WmInCallService` for every call. It keeps a call only if it carries WMDroid's token (or matches the number
    of the call being placed, in case Telecom drops extras), is outgoing, and the emulator is linked.
  - Call progress goes to WM: CONNECTING/DIALING → `dialing`, ACTIVE → `active`, HOLDING → `held`, DISCONNECTED →
    `ended <cause>`.
  - WM's End → `call.disconnect()`. Mute → `setMuted`. Speaker → `setAudioRoute(ROUTE_SPEAKER / ROUTE_WIRED_OR_EARPIECE)`.
    Hold → `hold()` / `unhold()`.
  - While WM shows a call, WMDroid holds a `PROXIMITY_SCREEN_OFF_WAKE_LOCK`, as a phone app does.
  - The mailbox is read every 250 ms during calls, so WM shows "Connected" within about 1 s of the real answer.
- **Hand-off to the phone's dialer.** Every case below is handled by ending the `:call` process.
  [AOSP `InCallController`: the default dialer's connection is wrapped in an `EmergencyInCallServiceConnection`; when that
  sub-connection dies, it connects the system dialer's InCallService for the ongoing calls. Not yet verified on One UI.]
  The main process sees the link die and ends WM's call (`ended handoff`); the real call goes on in the stock UI.

  | Case | Who notices |
  |---|---|
  | A call WM did not place (another app, an incoming call) | the in-call service |
  | WMDroid leaves the screen (`onStop`) during a WM call | CallBridge sends `HANDOFF` |
  | The main process dies | `linkToDeath` in the call process |
  | A SIM choice is needed | the in-call service |
  | The `:call` process crashes | Telecom does the same thing by itself |
- **Without the role.** `placeCall` shows the phone's dialer, and WM mirrors the call from `TelephonyCallback.CallStateListener`:
  - off-hook → `active` (there is no finer state without the role; WM's timer starts at dial time);
  - idle → `ended`. An idle report that comes before the first off-hook is ignored for 30 s, because registering reports
    the current, still idle, state.
  - WM's End → `TelecomManager.endCall()` (deprecated, needs `ANSWER_PHONE_CALLS`).
  - Mute and speaker → `AudioManager` (best effort).
  - Hold is not available.
- **DialActivity** (required for the role) passes `ACTION_DIAL` (with and without `tel:`) to the system dialer package, so
  tel: links keep opening the usual dial pad.
- **After a call**, Telecom unbinds everything and nothing of WMDroid stays in the call path. Without the role WMDroid is
  never in the call path at all.

### E.4 Verified in the CLI (wmcore-cli on the Fold7, `/data/local/tmp/wm/calls/`)

- Talk → dial 5551234 → Talk gives `call dial 1 5551234`. WM shows "Dialing..." and stays there, 5 s and more, until
  `call state 1 active`. Then "Connected: 00:01 … 00:10". `call state 1 ended` gives "Call ended: 00:13".
- WM's Mute gives `call mute 1` (and the mic-off icon). Hold / Unhold give `call hold 1 1` / `call hold 1 0` (and "On hold").
  End gives `call end 1`.
- Call History lists the outgoing call to 555-1234.
- With no acknowledgement, the call is hung up after 10 s ("Call ended 00:00").
- Turning calls on at run time loads FakeRIL, but WM reports "Phone off" until the soft reset. After the reset FakeRIL loads
  at boot, the helper registers it, and dialling works.

### E.5 Open items

- **WM's "Speaker On" does nothing with FakeRIL.** The button does not change, and SNAPI `Hardware\Speaker` does not change.
  - FakeRIL's `RIL_GetAudioDevices` returns E_NOTIMPL. `RIL_SetAudioDevices` (proxy function 0x030000BC) is queued.
  - emulserv's `SPK1:` driver (DE speakerphone device at 0x500FFFC0, bit 2 = speakerphone) writes `Speaker` when set,
    but cprog never reaches it here.
  - The helper reports `call speaker` from SNAPI already, and CallBridge routes it, so only the cprog → ossvcs path is
    missing. Next step: find which ossvcs ordinal cprog calls for IDC_SPEAKER.
- **DTMF from WM's keypad during a call** is not forwarded: FakeRIL takes `RIL_SendDTMF` without a notification. `CallLink.DTMF`
  exists on the Android side. A gate-style patch of FakeRIL's DTMF handler could feed it.
- **Android → WM mute and speaker changes** (from a headset, say) are not mirrored back into cprog.
- **Incoming calls (next):** the same design extends. `WmInCallService` already gets incoming calls with the role, and today
  hands them off at once. FakeRIL has no incoming-call hook ([B.1]); its special-number sessions (fn 0x7B4DC13C) are where a
  second gate would go. Otherwise switch the guest to B.2(b1) for a real `RING`. The Android half (role, hand-off,
  link) stays as it is.

## Sources

- [ROM] 6.5.3 FWVGA image modules: export tables of `phone.dll`, `fakeril.dll`, `ril.dll`, `tapi.dll`, `cellcore.dll`, `tpcutil.dll`,
  `pimstore.dll`; string and import scans of `fakeril.dll`, `cprog.exe`, `ossvcs.dll`, `tpcutil.dll`, the PocketOutlook GAC assembly,
  WM 6.1.4 `rilgsm.dll`; `default.hv` serial keys (`fw653/default_tree.txt`); coredll EDB export names counted in the `.BIN`.
- [DOC] AOSP javadoc: `telecomm/java/android/telecom/InCallService.java`, `CallScreeningService.java`, `TelecomManager.java`
  (`endCall`, `acceptRingingCall`, `silenceRinger`); developer.android.com "Restrictions on starting activities from the background"
  and "Restrictions on starting a foreground service from the background". ContactsContract/CalendarContract column semantics are from
  the reference documentation (not re-fetched).
- [PRIOR] `docs/research-cellular.md` (§1.1, §3, §9), `docs/research-status-indicators.md` (§1, §4), `docs/research-modern-usability.md`
  (§3.5), `docs/research-gps-network.md` (COM1 = GPS), `guest/Helper/Patch.cs`, `EmulatorActivity.kt` (suspend/hibernate), `GuestBridge.kt`.
