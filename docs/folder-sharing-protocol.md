# DE folder sharing (`\Storage Card`): behavioural specification

This is a clean-room behavioural specification of the Device Emulator 1.0 folder-sharing device (`0x500F4000`) and its EmulServ notification (`0x500F5000`). It was derived by observing what the DE 1.0 shared source (`boards/FolderSharing.{h,cpp}`, `boards/EmulServ.{h,cpp}`, mirror `gitntel/Microsoft-Device-Emulator`, branch `main`) does. It contains no code from that source. Section 4.2 of `research-de-hardware.md` gives the overview; this document is the precise contract.

Tags used in this document:

- **[DE]**: what DE 1.0 does.
- **[INFER]**: a deduction about the guest driver (`vcefsd.dll` / `emulserv`). The guest driver source is not in the DE drop. It exists only as a module inside the ROM images.
- **[REC]**: a recommendation for the WMDroid reimplementation.

---

## 1. Registers (word access only)

Any access other than an aligned word at these offsets is fatal in DE.

| Off | Name | Read | Write |
|---|---|---|---|
| 0x0 | ServerPBAddr | Last value written | Guest **physical** address of `ServerPB`. It must lie in guest RAM (bank at 0x30000000 or the RAM extension), otherwise DE aborts when the PB is first used |
| 0x4 | Code | Last code written | Starts the operation (see §2) |
| 0x8 | IOPending | 1 while an operation (or a host-side reconfigure) is outstanding, otherwise 0 | Ignored |
| 0xC | Result | Completion value | Guest may overwrite it. DE never relies on the written value |

Save state stores ServerPBAddr, Code and Result. IOPending is not saved and is treated as 0 on restore.

## 2. Dispatch rules for a write to `Code`

Rules are applied in order. [DE]

| Condition | Effect |
|---|---|
| Sharing disabled (no share configured, or share was removed) | `IOPending = 0`, `Result = 1`. Nothing else happens, for **every** code including 0 and 4 |
| Code ≠ 0 and IOPending ≠ 0 | `Result = 1`. The new operation is **not** started. IOPending is unchanged |
| Code = 0 | PollCompletion (§2.1) |
| Code = 4 or 0x15 | Synchronous: IOPending goes 1 then 0 within the same bus write, and Result is set. ServerPB is not read |
| Code ∈ {5..0x11, 0x13} | IOPending = 1. The operation is queued to a host worker. When it finishes, the out-fields are written into ServerPB **first**, and then `Result` = error code and `IOPending` = 0 |
| Code ∈ {1, 2, 3, 0x12, 0x14} or > 0x15 | DE terminates the emulator ("unsupported hardware"). **[REC]** Return `Result = 1` and `IOPending = 0` instead |

Behaviour of the asynchronous worker:

- It reads `ServerPB` from guest RAM when it runs. It does not snapshot the PB when Code is written. The guest must leave the PB and the data buffer alone until IOPending reads 0. [DE]
- On every async call DE checks that `fStructureSize == 0x42C`. A mismatch is fatal. [DE] **[REC]** Return 0x1F instead.
- **`ServerPB.fResult` is never written by DE.** The status is delivered only through the `Result` register. [INFER] The guest copies it into `fResult` itself if it wants it. **[REC]** Leave fResult untouched.

### 2.1 PollCompletion (Code 0) and IOPending: what the guest relies on

DE behaviour:

- If IOPending is 0 on entry, PollCompletion sets `Result = 0` and returns.
- Otherwise the emulated CPU blocks for up to 1 ms. It wakes early if the operation completes or if a guest interrupt becomes deliverable.
- If it woke because the operation completed, Result is left holding the operation's code.
- Otherwise (timeout or interrupt) it sets `Result = 0`. In that case IOPending is still 1.

Consequences for the guest [INFER]. The guest must use this loop:

1. Write Code.
2. While IOPending reads 1, write Code = 0.
3. Read Result.

The guest **cannot** wait for Result to become non-zero, because success is 0. The guest also must not issue an extra poll after IOPending has reached 0, because that poll would clobber Result to 0. The 1 ms block exists only so that interrupts keep being serviced during slow host I/O. Nothing requires the operation to actually be asynchronous.

**[REC]** Make the operation synchronous: perform it entirely inside the Code write, so IOPending is already 0 when the guest reads it. PollCompletion should **never modify Result**. That is strictly safer than DE's behaviour, which has a race in which a completion between the timeout and the overwrite can lose an error code. If host I/O can be slow (for example Android SAF), keep the async model: block the poll for at most about 1 ms, or return early on a pending IRQ.

## 3. ServerPB layout (0x42C bytes, little-endian, natural alignment)

| Off | Type | Field | Used by |
|---|---|---|---|
| 0x000 | u16 | fStructureSize = 0x42C | Checked on every async call |
| 0x002 | u16 | fResult | **Never touched by DE** |
| 0x004 | u32 | fFindTransactionID | GetInfo |
| 0x008 | s16 | fIndex | GetInfo |
| 0x00A | u16 | fHandle | Open (out); Read, Write, SetEOF, Close, GetFCBInfo (in) |
| 0x00C | u32 | fFileTimeDate (last-write, DOS) | GetInfo (out), SetAttributes (in) |
| 0x010 | u32 | fSize | Read and Write (in/out), GetInfo, GetFCBInfo, GetSpace (out) |
| 0x014 | u32 | fPosition | Read, Write, SetEOF (in); GetFCBInfo, GetSpace (out) |
| 0x018 | u32 | fDTAPtr: guest **PA** of the data buffer | Read, Write |
| 0x01C | u16 | fFileAttributes | GetInfo, GetFCBInfo (out); SetAttributes (in) |
| 0x01E | u16 | fOpenMode | Open (in), GetFCBInfo (out) |
| 0x020 | u8 | fWildCard | **Never read by DE** |
| 0x022 | u16 | fNameLength: **bytes**, excluding the NUL | All path calls (in); GetInfo, GetFCBInfo (out) |
| 0x024 | u16[256] | fName (UTF-16LE) | All path calls (in); GetInfo, GetFCBInfo (out) |
| 0x224 | u16 | fName2Length (bytes) | Rename (in) |
| 0x226 | u16[256] | fName2 | Rename (in) |
| 0x428 | u32 | fFileCreateTimeDate (creation, DOS) | GetInfo (out only) |

DE never writes `fIndex`, `fFindTransactionID` or `fPosition` back, except for the GetFCBInfo and GetSpace outputs listed below.

## 4. Per-function contract

"Result" means the Result register. Codes are listed in §6. "Path" means fName/fNameLength resolved as described in §5. Any path error is 0x7B and is reported before any file-system access.

| Code | Function | Inputs | Outputs on success (Result = 0 unless noted) | Failure Results |
|---|---|---|---|---|
| 0x00 | PollCompletion | — | See §2.1 | — |
| 0x04 | GetDriveConfig | none (PB not read) | Result = 0 | Disabled: **1**. The intended 0x1F is unreachable because the disabled check in §2 fires first. The guest should treat any non-zero value as "not mounted" |
| 0x05 | Create | Path | No PB output and **no handle**. The new file is created empty with "create-new" semantics and then closed immediately. fOpenMode and fFileAttributes are ignored | Already exists: 0x50. Parent missing: 3. Access: 5. Other: §6 |
| 0x06 | Open | Path, fOpenMode | `fHandle` = lowest free slot 0..39. The file must already exist; Open never creates or truncates. The host file pointer starts at 0 | 40 handles open: 4 (checked first). Bad path: 0x7B. Invalid access nibble or share nibble: **1**. Not found: 2 or 3. Directory or no permission: 5. Share conflict: 0x20. fHandle is unchanged on failure |
| 0x07 | Read | fHandle, fPosition, fSize, fDTAPtr | Reads up to fSize bytes starting at absolute offset fPosition into guest RAM at fDTAPtr. `fSize` = bytes actually read. **At or after EOF this is a short read with Result 0**, possibly 0 bytes. fPosition is **not** updated. The host pointer is left at fPosition + n | Handle not open: 6. fDTAPtr not in RAM: **0x1E**. Handle opened write-only: 5 |
| 0x08 | Write | fHandle, fPosition, fSize, fDTAPtr | Writes fSize bytes from fDTAPtr at absolute offset fPosition, extending the file if needed (the gap is zero-filled). `fSize` = bytes written. fPosition is not updated | Not open: 6. fDTAPtr not in RAM: **0x1D**. Read-only handle: 5. Volume full: 0x27 (see §6 caveat) |
| 0x09 | SetEOF | fHandle, fPosition (new length) | The file is truncated or extended to exactly fPosition bytes. The host pointer is left at fPosition | Not open: 6. Read-only handle: 5 |
| 0x0A | Close | fHandle | The slot is freed | fHandle ≥ 40: 6. Slot < 40 but already free: **0x1F** (DE quirk; the host close fails and the error is unmapped). **[REC]** Return 6 |
| 0x0B | GetSpace | none (fName ignored) | `fSize` = bytes free *to the caller* on the share's volume ÷ 32768. `fPosition` = total bytes ÷ 32768. Each byte count is first clamped to 0xFFFFFFFF, so the maximum reported value is 0x1FFFF units (just under 4 GiB) | Host query failed: §6 |
| 0x0C | MkDir | Path | — | Parent missing: 3. **Already exists: 0x1F** (unmapped host error, not 0x50) |
| 0x0D | RmDir | Path | Removes an empty directory | Not found: 2 or 3. **Not empty: 0x1F** (unmapped). Not a directory or in use: 5 or 0x20 |
| 0x0E | SetAttributes | Path, fFileAttributes, fFileTimeDate | Attributes are set first, from the raw 16-bit value (host semantics; DIRECTORY and unknown bits are ignored by the host). If fFileTimeDate ≠ 0, only the **last-write** time is set to it (DOS local converted to UTC). Creation and access times are untouched. 0 means "do not change the time" | Not found: 2 or 3. Invalid DOS date/time: 0x1F. Setting a time on a **directory** fails with 5 in DE because DE opens it like a file (**[REC]** allow it) |
| 0x0F | Rename | fName/fNameLength → fName2/fName2Length (both are full paths) | Moves or renames a file or directory. Moves across directories are allowed, and so is a copy across host volumes. It never replaces an existing target | **Target exists: 0x1F** (unmapped). Source missing: 2 or 3. Source open by the guest: 0x20. Either path bad: 0x7B |
| 0x10 | Delete | Path | Deletes a file | Directory: 5. Read-only file: 5. File currently open through the share: **0x20**, because Open never grants delete-sharing. Missing: 2 or 3 |
| 0x11 | GetInfo | See §4.1 | See §4.1 | See §4.1 |
| 0x13 | GetFCBInfo | fHandle | `fPosition` = current host file pointer: 0 after Open, otherwise the end of the last Read, Write or SetEOF; 0xFFFFFFFF if ≥ 4 GiB. `fSize` = current length (clamped likewise). `fFileAttributes` = masked attributes. `fOpenMode` = the value passed to Open, all 16 bits. `fName`/`fNameLength` = path relative to the share root, beginning with `\`, canonicalised, in the case **the guest supplied**, NUL-terminated. **Times are not returned** | Not open: 6 |
| 0x15 | GetMaxIOSize | none | **Result = 0x10000**, returned as a value, not an error code | — |

Open mode (`fOpenMode`) fields: [DE]

| Bits | Value | Meaning |
|---|---|---|
| 3:0 access | 0 read, 1 write, 2 read/write | Any other value: Result 1 |
| 7:4 share | 1 deny read+write (exclusive), 2 deny write, 3 deny read, 4 deny none | 0 ("compatibility") or > 4: Result 1 |
| 15:8 | Ignored, but echoed by GetFCBInfo | |

The share bits are enforced between guest handles and against other host processes. Guest handles never allow delete or rename while open.

The guest has no "open always" or "truncate" call. [INFER] Win32 dispositions are composed by the guest driver from GetInfo (exists?), Create (new empty file), Open, and SetEOF(0). This is why Create returns no handle.

### 4.1 GetInfo (0x11): exists / FindFirst / FindNext

The branch is chosen by `fIndex`. **Only −1 is the existence check.** Any other negative value is treated like a positive value, as FindNext. [DE]

| fIndex | fFindTransactionID | Behaviour |
|---|---|---|
| **−1** (stat) | Must be 0xFFFFFFFF, otherwise Result **1** | Path must name one entry. Wildcards are not interpreted, and `fWildCard` is ignored. [INFER] The guest's find logic switches to fIndex 0 when the name contains a wildcard. Outputs: fFileAttributes, fSize, fFileTimeDate, fFileCreateTimeDate. **fName is not written back.** `\` alone names the share root, a directory. Not found: 2. Missing parent: 3 |
| **0** (FindFirst) | 0..39 = slot to own the enumeration; 0xFFFFFFFF = one-shot; anything else gives Result **0x1F** | fName is a full path whose last component may contain `*` or `?` (host wildcard semantics: `*` and `*.*` match everything, `?` matches one character). Entries `.` and `..` are always skipped. The first remaining entry is returned. With a slot ID, any enumeration already in that slot is discarded and replaced, and the entry's **bare name** (no directory, long name, not 8.3) is written to fName/fNameLength, NUL-terminated. With 0xFFFFFFFF the name is **not** written back and nothing is retained. Nothing matches: **2**, or **0x12** if the directory held only `.`/`..`. Directory missing: 3 |
| **≥ 1, or ≤ −2** (FindNext) | Slot used by FindFirst | fName is ignored. Returns the next entry of that slot's enumeration (skipping `.`/`..`) with the name written back. **End of enumeration: 0x12**, and the slot is closed at the same time. Slot ≥ 40, never opened, or already finished: **0x1F** |

Notes on enumeration:

- DE never checks or changes `fIndex` beyond its sign and zero value. [INFER] The guest numbers successive FindNext calls 1, 2, 3 and so on, and allocates transaction IDs 0..39 itself; its find-context table has 40 entries.
- There is no explicit FindClose. A slot is released when its enumeration ends (0x12), when a new FindFirst reuses it, or on reset or reconfigure.
- Order is host directory order, which is unsorted.

Per-entry outputs, identical for stat, FindFirst and FindNext:

| Field | Value |
|---|---|
| fFileAttributes | Host attributes masked to READONLY 0x01, HIDDEN 0x02, SYSTEM 0x04, DIRECTORY 0x10, ARCHIVE 0x20 |
| fSize | 0 for directories; the file length, or 0xFFFFFFFF if ≥ 4 GiB |
| fFileTimeDate | Last-write time (DOS, local) |
| fFileCreateTimeDate | Creation time (DOS, local) |
| fName / fNameLength | FindFirst with a slot, and FindNext only |

Access time is never returned.

## 5. Paths

Path rules: [DE] unless marked otherwise.

- **Format.** fName is UTF-16LE. `fNameLength` is a byte count without the NUL. DE takes `fNameLength/2` characters and also stops at an embedded NUL.
- **Rooting.** The path must start with `\`. `/` is also accepted as a separator because the host accepts it. The path is interpreted relative to the share root. [INFER] The guest FSD strips the mount prefix, so `\Storage Card\a\b.txt` arrives as `\a\b.txt`.
- **Empty or relative names.** A zero-length name, or one without a leading separator, gives 0x7B.
- **Canonicalisation.** The host root and the guest path are joined, and the result is canonicalised: `.` and `..` components are resolved. If the result no longer lies inside the root, comparing case-insensitively, the Result is **0x7B**.
- **Sibling folders.** A sibling folder that shares the root as a prefix (for example `C:\share2` when the root is `C:\share`) is rejected.
- **Length.** If the full host path would exceed 259 characters (MAX_PATH − 1), the Result is 0x7B. Each name buffer holds at most 255 characters plus the NUL.
- **Case.** Lookups are case-insensitive and names keep their case, as on NTFS and in CE. **[REC]** On Android (case-sensitive storage), resolve each component case-insensitively. When creating a file, keep the case the guest supplied. If two host names differ only in case, pick one deterministically.
- **Reserved names.** Host-invalid names (`<>:"|?*` outside wildcards, trailing dots or spaces, DOS device names) give 0x7B. **[REC]** Apply the Windows rules so the guest sees the same behaviour.

## 6. Error codes and host-error mapping

| Code | Name | Produced when |
|---|---|---|
| 0x00 | OK | |
| 0x01 | Invalid function | Sharing disabled, busy (§2), invalid open mode, GetInfo stat with a transaction ID |
| 0x02 | File not found | |
| 0x03 | Path not found | |
| 0x04 | Too many open files | 40 handles in use |
| 0x05 | Access denied | Includes opening or deleting a directory as a file, a read-only target, and the wrong access direction |
| 0x06 | Invalid handle | fHandle out of range or slot not open (Read, Write, SetEOF, GetFCBInfo; Close only when ≥ 40) |
| 0x12 | No more files | End of enumeration |
| 0x13 | Write protect | |
| 0x1D | Write fault | Write: fDTAPtr outside RAM |
| 0x1E | Read fault | Read: fDTAPtr outside RAM |
| 0x1F | General failure | **Every host error not listed in this table**, plus an invalid transaction slot and the invalid DOS time in SetAttributes |
| 0x20 | Sharing violation | |
| 0x21 | Lock violation | |
| 0x24 | Sharing buffer exceeded | |
| 0x27 | Disk full | **Only** the host "handle disk full" error. The ordinary "disk full" error falls through to 0x1F |
| 0x50 | File exists | Create only. MkDir and Rename report 0x1F instead |
| 0x7B | Invalid name | |

Codes 0x11 (not same device) are defined but never produced.

**[REC]** Reproduce the table faithfully. The guest maps these codes back to Win32 errors, and its behaviour around 0x1F versus 0x50 is unverified. If a deviation is wanted, return 0x50 for "already exists" in MkDir and Rename, and 0x27 for any disk-full condition.

## 7. Data buffer, sizes and limits

- **fDTAPtr range check.** fDTAPtr is translated as a guest physical address. **DE checks only the first byte** and assumes `fSize` bytes are contiguous in guest RAM, with no upper bound check.
  - [INFER] The guest uses a physically contiguous bounce buffer of at most GetMaxIOSize (64 KiB) and splits larger requests itself.
  - **[REC]** Reject the request if `[fDTAPtr, fDTAPtr + fSize)` leaves RAM: 0x1E for Read, 0x1D for Write. Also cap fSize at 0x10000.
- **32-bit offsets.** fPosition and fSize are u32, so offsets at or beyond 4 GiB cannot be addressed. Sizes and positions are reported as 0xFFFFFFFF when the true value is ≥ 4 GiB.
- **Limits.** At most 40 open handles and 40 enumeration slots exist. Both tables are global to the device.
- **Handle ownership.** The guest does not see host handles. A handle is just a slot index from 0 to 39. The "invalid handle" sentinel 0xFFFF is only internal to DE.

## 8. Time encoding

- **Packing.** `value = (dosDate << 16) | dosTime`.
  - dosTime: bits 15–11 hour, 10–5 minute, 4–0 second/2.
  - dosDate: bits 15–9 year − 1980, 8–5 month, 4–0 day.
- **Time zone.** The value is in the **host's local time zone**. DE converts UTC to host-local on output, and host-local to UTC on input.
- **Field meanings.**
  - `fFileTimeDate` = last-write (modify) time.
  - `fFileCreateTimeDate` = creation time.
  - Times outside 1980–2107 come out as **0**.
- **Input.** SetAttributes treats 0 as "leave unchanged". It can set only the modify time.
- **Granularity.** Precision is 2 seconds.
- **[REC]** Use the time zone the guest clock runs in, which is normally the same as the host's. Treat 0 as "unknown".

## 9. Lifecycle, EmulServ notification and mounting

### 9.1 EmulServ device (`0x500F5000`, IRQ EINT11)

On the S3C2410, EINT11 is EINTPEND bit 11 under the INT_EINT8_23 main source. [DE]

| Off | Read | Write |
|---|---|---|
| 0x0 InterruptMask | Mask value | Stores the mask. If (mask & pending) ≠ 0, EINT11 is asserted **immediately** |
| 0x4 InterruptPending | Returns the pending bits, **clears all of them** and deasserts EINT11 | Ignored |

- Raising an event ORs its bit into pending, and asserts EINT11 if the bit is unmasked.
- Pending bits stay latched while masked, so an event raised before the guest driver loads is delivered as soon as the driver unmasks.
- The only bit is `0x40000000` = folder sharing changed. The guest's `kFServerChangedServiceMask` must match it.
- The mask is saved in state; pending is not.
- Neither register is cleared by a guest soft reset.

### 9.2 When DE raises `0x40000000`

All rows are [DE].

| Event | Device effect | Interrupt |
|---|---|---|
| Cold boot with a share configured (and accepted) | Root validated (must exist and be a directory; trailing separator stripped; made absolute). Enabled before the guest runs | **Yes.** Latched until the guest unmasks |
| Cold boot without a share | Disabled. All codes return 1 | No |
| Restore from saved state, share setting unchanged | Re-enabled silently | **No.** The restored guest is already mounted |
| Restore from saved state, share enabled↔disabled changed | — | Yes |
| Runtime: disabled → enabled | Enabled | Yes |
| Runtime: enabled → disabled | The in-flight operation (if any) completes with **0x1F**. All handles and find slots are closed. Further codes return 1 | Yes |
| Runtime: enabled → different folder | All handles and find slots are closed silently. The guest keeps its mount, and stale handles now give 6 or 0x1F | **No** |
| Guest reset while enabled | All handles and find slots are closed | **Yes**, so the rebooted guest re-mounts |
| Guest reset while disabled | Nothing | No |

While a runtime reconfigure is queued, IOPending reads 1, so guest calls during that window get Result 1 (busy). [DE]

### 9.3 What the guest expects for `\Storage Card` to mount

The guest-side sequence is [INFER], based on the DE comments and the register design:

1. **Driver load.** The `emulserv` driver hooks SYSINTR_EMULSERV (EINT11) and writes `0xFFFFFFFF` to InterruptMask.
2. **Interrupt.** If the boot event is latched, EINT11 fires at once.
3. **Dispatch.** The driver reads InterruptPending, which also clears it. When bit `0x40000000` is set, it signals `vcefsd`.
4. **Configure.** `vcefsd` writes ServerPBAddr (once) and issues **GetDriveConfig (4)**.
   - Result 0: it registers or mounts the volume as `\Storage Card`, typically followed by GetSpace and a root FindFirst `\*`.
   - Result non-zero: it dismounts, and open guest handles become invalid.
5. **Ongoing use.** File operations then follow §4. Large I/O is chunked to GetMaxIOSize.

**Verified (2026-10-16, disassembly of `emulserv.dll` and `vcefsd.dll` from the WM 6.5.3 image; WM 5 and 6.1.4 behave the same).** Step 3 is not a signal to vcefsd. emulserv's change handler maps `0x500F4000` itself, writes **Code = 0** (PollCompletion) and reads **Result**: Result 1 means "no share", and it calls `DeactivateDevice` on the block device it activated (the storage manager dismounts the card, the shell runs the card's autorun `uninstall`). Any other value means "share present": if it holds no device handle it calls `ActivateDevice` on its `VCEFSD` block device, the storage manager mounts it, and vcefsd's `FSD_MountDisk` issues GetDriveConfig and `FSDMGR_RegisterVolume(L"Storage Card")` (the shell then runs autorun `install`). A notification while the card is mounted and the share present does nothing. So PollCompletion must answer 1 / 0 exactly as DE does: WMDroid once left the previous operation's Result in place, and a card taken out and put back stayed unmounted (the last failed call had left 1). vcefsd itself only polls while IOPending reads 1, so with synchronous operations it never writes Code 0. If a folder named `\Storage Card` exists when the card mounts (e.g. a program created `\Storage Card\...` in RAM while the card was away), the card mounts as `\Storage Card2`.

**[REC] Minimum WMDroid sequence:**

- At power-on with a share configured, set EmulServ pending |= `0x40000000` before releasing the CPU. Do not assert the IRQ until the mask is written.
- Answer GetDriveConfig with 0.
- On a guest reset while enabled, re-latch the bit.
- On hot enable or disable, latch the bit and assert the IRQ if it is unmasked.
- For a hot change of folder, consider raising a disable notification and then an enable notification, so the guest drops stale handles. DE does not do this.
- Skip the interrupt only when restoring a snapshot taken with the same share state.

## 10. Deviations recommended for WMDroid (summary)

1. Complete operations synchronously, and never let PollCompletion overwrite Result (§2.1).
2. Unknown codes, a bad fStructureSize, or a PB outside RAM give Result 1 or 0x1F instead of aborting the emulator.
3. Bounds-check `fDTAPtr + fSize` and cap transfers at 64 KiB.
4. Close of a free slot returns 6.
5. Allow SetAttributes time changes on directories.
6. Resolve path components case-insensitively on case-sensitive host storage.
7. Keep everything else, including error codes, bit-exact with DE.
