# Phone cameras in the guest (Camera, Pictures & Videos, DirectShow, SHCameraCapture)

Research notes for WMDroid. Main target: WM 6.5.3 Professional `PPC_USA_GSM_FWVGA_VR.BIN` (480x854). Companion to
`research-de-hardware.md` (DE page map), `research-modern-usability.md` §6 (how the IMGFS was decoded) and
`research-status-indicators.md` §5 (physical MMIO from trusted guest code).

Tags:

- **[ROM]**: read out of the 6.5.3 image: IMGFS modules, the `default.hv` hive, package `.rgu` files, and NDK
  `llvm-objdump --triple=armv5te` disassembly of `NULLCAM.dll`. Nothing from the archive was executed.
- **[BIN]**: DE 1.0/2.0/3.0 `DeviceEmulator.exe` (strings only).
- **[DE-SRC]**: DE 1.0 shared source. Facts only, no code.
- **[DOC]**: Microsoft CE/WM documentation, recalled rather than re-fetched.
- **[CODE]**: WMDroid as it is today. **[INFERRED]**: reasoning that has not been tested on a device.

---

## TL;DR

| | Finding |
|---|---|
| Stack in the image | The **complete** WM camera stack is present: `pimg.exe`/`pimgdll.dll` (camera UI inside Pictures & Videos), `aygshell!SHCameraCapture` → `pimgdll!CameraCapture`, `quartz.dll` (Video Capture filter, Capture Graph Builder, Smart Tee, Image Sink, ASF Writer/Mux), `wmvdmoe.dll` (WMV9 encoder DMO) and `imaging.dll` (JPEG). [ROM] |
| Driver | **Yes, a driver ships: Microsoft's sample `NULLCAM.dll`** (`CAM1:` + pin driver `PIN1:`). It renders a synthetic test pattern. It is in every WM 6.0–6.5.3 DE image, Pro and Standard, with identical code. Only the relocation base differs. [ROM] |
| DE hardware | DE has no camera device of any kind. There is no "camera" string in DE 1.0/2.0/3.0. [BIN][DE-SRC] |
| Frame hook | NULLCAM produces every frame by calling **one function pointer**, `DWORD Gen(BYTE *buf, CS_VIDEOINFOHEADER *vih, DWORD mode, BOOL flag, DWORD *extra)`. It defaults to an internal pattern generator at **text+0x4C04** (6.5.3 Pro: `0x7B49DC04`). On every `PIN_Open` it can be **replaced from the registry**: `HKLM\Drivers\Capture\NullCam` `LibraryName` + `FunctionName`. [ROM, disasm] |
| Formats | Everything is RGB565 (`BI_BITFIELDS`, **bottom-up**). Preview and video pins: 160x120 and 176x144 at 30 fps, 320x240 at 15 fps, YV12 176x144 at 15 fps. Still pin: **320x240 and 640x480** only. [ROM] |
| Recommendation | **Option b1: an HLE trap on NULLCAM's generator.** The emulator fills the guest buffer with a downscaled, era-filtered Android frame and returns. No guest code, and every DirectShow, Pictures & Videos and SHCameraCapture client works unchanged. **About 6–8 days.** |

---

## 1. The camera stack in the 6.5.3 image

### 1.1 Modules [ROM]

| Module | Kind | Role |
|---|---|---|
| `NULLCAM.dll` | ROM module (text `0x7B499000`, 22 KB) | Camera driver. Exports `CAM_Init/Deinit/Open/Close/IOControl` and `PIN_Init/Deinit/Open/Close/IOControl`. PDB name `nullcam.pdb`. Debug zones are `Null_CAM`. Imports only `coredll`. Frame timing uses `MMTimer.dll!timeSetEvent`, loaded dynamically. |
| `pimg.exe` | 4 KB stub | Starts `pimgdll.dll` |
| `pimgdll.dll` | Plain PE file (311 KB) | Pictures & Videos, including the camera (`camera\dshowwrapper.cpp`, `dshowpreview.cpp`, `dshowvideo.cpp`). Exports `CameraCapture`, `ShFindCameraUIWindow` and `ShCloseCameraUIWindow`. Builds the graph from `VideoCapture`, `Image Sink Filter`, `Video Encoder DMO`, `Audio Encoder DMO` and `VideoRenderer`. |
| `aygshell.dll` | Module | Exports `SHCameraCapture`, which loads `Pimgdll.dll` and calls `CameraCapture`. That makes `SHCameraCapture` and CF's `CameraCaptureDialog` the same UI as the built-in camera. |
| `quartz.dll` | Module | DirectShow with Video/Audio Capture filters, Capture (FilterGraph) Builder, Smart Tee, Image Sink Filter (`SOFTWARE\Microsoft\DirectShow\ImageSink\Quality\%d\%s`), ASF Writer, ASF Mux and Buffering filter. It reads `VCapName`, default `CAM1:`. |
| `wmvdmoe.dll` | Plain PE file | WMVideo9 Encoder DMO `{d23b90d0-144f-46bd-841d-59e4eb19dc59}`. Input YV12, YUY2 or RGB565; output WMV3. |
| `imaging.dll`, `msdmo.dll`, `mp3dmod.dll`, `wmv9decoder.dll` | | JPEG encode/decode for the Image Sink, DMO host, and playback |
| `camera.lnk` | `#:MSCAMERA` → `pimg.exe -camerakey` | **Not** copied into the Start menu by `initflashfiles.dat`, which only adds `Pictures & Videos.lnk`. The camera opens from inside Pictures & Videos. |

There is no `camera.exe`, `camerashell`, YUV colour-converter filter or OEM camera app. [ROM]

### 1.2 Registry [ROM] (`default.hv`; the values match package `9ba80faf-….rgu`)

```
[HKLM\Drivers\BuiltIn\NullCam]  Prefix="CAM" Dll="NullCam.dll" Order=1 Index=1
                                IClass="{CB998A05-122C-4166-846A-933E4D7E3C86}"   (DEVCLASS_CAMERA_GUID)  -> CAM1:
[HKLM\Software\Microsoft\DirectX\DirectShow\Capture]   Prefix="PIN" Dll="NullCam.dll" Order=1 Index=1
                                IClass="{C9D092D6-827A-45E2-8144-DE1982BFC3A8}"   (pin driver class)      -> PIN1:
[HKLM\Software\Microsoft\Pictures\Camera\OEM]   DisableCamera=0 EnableVideo=1 EnableZoom=1 EnableBrightness=1
      EnableWhiteBalance=1 EnableFlash=0 SelectBigResolution=1 FitWindow=1 IsPortraitScreen=1 PicsInBurst=5
      VideoRecordLimitation="0 15 30" SupportedVideoFormat="*.wma;*.wmv" VideoBufferingDepth=2000000 ...
  \PictureResolution   1: 320x240   2: 640x480
  \VideoProfile        1: "MMS (160x120)"  2: "MMS (176x144)"  3: "PC (320x240)"
  \Zoom                x1..x6 (PhysicalZoom 100..600)      \WhiteBalance  Night/Sunlight/Cloudy/Fluorescent/Incandescent
  \PreviewColorFormatCLSID, \CaptureColorFormatCLSID, \StillColorFormatCLSID   IClass = {e436eb7b-…} = MEDIASUBTYPE_RGB565
  \VideoEncoderCLSID   {d23b90d0-…} WMV9 DMO      \MuxCLSID {4f4ba16c-…} "ASF Writer", VideoFileExt="wmv"
[HKLM\System\Pictures\Camera\OEM]   SoundFile=\Windows\ShutterSound.wav ForceShutterSound=1 ...
  \PictureLocation     Internal="\My Documents\My Pictures\"   External="\DCIM\"
[HKLM\Software\Microsoft\DirectShow\ImageSink\Quality\{0..}\jpg\Quality]   JPEG quality presets
[HKLM\Software\Microsoft\Shell\Rai\:MSCAMERA]   1="pimg.exe -camerakey"
```

- `pimgdll` also reads the following [ROM strings; semantics DOC/INFERRED]:
  - `System\Pictures\Camera\OEM\OEMCameraCaptureDLL` / `OEMCameraCapture`: the OEM replacement for the capture dialog.
  - `CameraEnabled` under `System\State\Hardware`.
  - `CameraPowerName`, `CameraStandbyPowerLevel`, `UnloadCaptureFilter`, `PreviewWidth/Height`, `\VideoCaptureFrameInterval`,
    `\PreviewFrameInterval`, `\CSCCLSID` and `\AudioCaptureFilter`.
  - None of these is set, so the defaults apply.
- `HKLM\Drivers\Capture\NullCam` is **absent**, so NULLCAM uses its built-in pattern generator. [ROM]

**Answer to Q1:** the stack and a driver are both present. On stock DE the camera app should open and show NULLCAM's
moving test pattern. Photos (JPEG via `imaging.dll`) and WMV video should save normally. [ROM; INFERRED: it was not run]

### 1.3 Inside NULLCAM [ROM, disasm of the 6.5.3 Pro build; offsets are from the start of `.text`]

**Pins and formats.** The PDD constructor (`text+0x1690..0x1AAC`) fills three pin descriptors. The data ranges live in `.data`
(`CS_DATARANGE_VIDEO`, 0x538 bytes each; `CS_VIDEOINFOHEADER` at +0xD0). `.data` starts at `0x01DAB000` in 6.5.3 Pro.

| Pin (GUID) | Formats, in the order the pin offers them |
|---|---|
| Capture `PINNAME_VIDEO_CAPTURE` {fb6c4281-…} | 160x120 RGB565 at 30 fps, 176x144 RGB565 at 30 fps, 320x240 RGB565 at 15 fps, 176x144 YV12 at 15 fps |
| Preview `PINNAME_VIDEO_PREVIEW` {fb6c4282-…} | The same four formats |
| Still `PINNAME_VIDEO_STILL` {fb6c428a-…} | 320x240 RGB565 (`.data+0x16D8`), **640x480 RGB565** (`.data+0x11A0`). These two ranges are used by the still pin only. |

- RGB565 entries use `biCompression = BI_BITFIELDS | 0x8000` (`BI_SRCPREROTATE`) and a **positive `biHeight`, so they are bottom-up DIBs**.
- YV12 has `biHeight = -144` (top-down).
- Frame intervals (`AvgTimePerFrame`) are 666666 or 333333 in 100 ns units.

**Frame production:**

- `FillBuffer` (`text+0x354C`) is called from the timer path and from the still path (`text+0x3D8C`, `+0x3E6C`).
- It enters the pin's critical section and calls the pin's generator pointer `pin->gen` (pin object +0x5A0) with these arguments:
  - `r0` = destination buffer (`CS_STREAM_HEADER.Data`)
  - `r1` = `&pin->format.VideoInfoHeader` (pin+0xF0)
  - `r2` = 4 (pattern mode)
  - `r3` = 0 (flag)
  - `[sp]` = pointer to an adapter property value (adapter+0x404, default 750; [INFERRED] the VideoProcAmp brightness)
- **The return value becomes `DataUsed`**. The built-in generator returns `biSizeImage`, or `-1` on failure.
- The function then sets `PresentationTime`, `PictureNumber` and the `OptionsFlags` key-frame bit, and leaves the critical section.

**Default generator** at `text+0x4C04`:

- It draws an animated grey/gradient pattern for 16-bpp output and has a separate YV12 path at `text+0x4984`.
- It masks `0x8000` off `biCompression` and reads `biWidth/biHeight/biBitCount/biSizeImage` from `r1`.
- Prologue signature (identical in all seven images checked):
  `e1a0c00d e92d000f e92d5ff0 e28db028 e24dd03c e1a08003 e54b8060`.
- The literal-pool word just before it equals `entry + 0x98C`.

**Registry plug-in.** `PIN_Open` (`text+0x14BC`) allocates the 0x5B0-byte pin object, then calls `text+0x3B54`, which:

1. calls `RegOpenKeyEx(HKLM, "Drivers\Capture\NullCam")`;
2. reads `LibraryName` and `FunctionName` (both must be REG_SZ);
3. calls `LoadLibrary(LibraryName)`, stored at pin+0x5A4, then `GetProcAddress(…, FunctionName)`, stored at pin+0x5A0;
4. falls back to the built-in generator if any step fails.

`PIN_Close` → `text+0x33EC` calls `FreeLibrary`. The key is re-read on **every pin open**, so no reset is needed after changing it.

**Per-image addresses** (the code is identical, only the base moves):

| Image | NULLCAM `.text` | Generator |
|---|---|---|
| 6.5.3 Pro (FWVGA, VGA, VR; same build) | `0x7B499000` | `0x7B49DC04` |
| 6.5.3 Std WVGA | `0x7B8AC000` | `0x7B8B0C04` |
| 6.5 Pro WVGA | `0x7B55D000` | `0x7B561C04` |
| 6.1.4 Pro 480x800 | `0x022B2000` | `0x022B6C04` |
| 6 Pro (SDK Refresh) VGA | `0x01AD1000` | `0x01AD5C04` |

---

## 2. Options for getting phone frames into the guest

### (b1) HLE trap on NULLCAM's generator: **recommended**

The emulator intercepts execution at the generator entry and does the generator's job in host code:

1. **Trap.** When a block is translated (JIT) or fetched (interpreter) at a PC whose words match the signature above,
   mark it as an HLE trap. Use a signature check rather than a per-image table, so every WM6.x image works. A table
   lookup by PC can serve as a fast pre-filter. [CODE: `Jit::compile(pc)` and `Cpu::run()` are the two insertion points.
   `Cpu::translate(va, write, user, pa, fsr)` already exists.]
2. **On a hit**, read the arguments and the format:
   - read `r0` (buffer VA) and `r1` (VIH VA);
   - fetch `biWidth`, `biHeight` (sign = orientation), `biBitCount`, `biCompression & ~0x8000` and `biSizeImage`;
   - only handle RGB565 (16 bpp, BI_BITFIELDS) and YV12. Anything else falls through to the original code.
3. **Write the frame.**
   - Translate each 4 KB page of the buffer with the current page tables, as a write from user mode.
   - **If any page fails to translate** (CE commits buffers lazily), don't trap this call; let the original generator run. It
     touches every page, and the next frame will succeed.
   - Otherwise copy the pre-converted frame page by page into guest RAM, set `r0 = biSizeImage`, `PC = LR`, and leave the
     JIT block.
4. **Fallbacks.** If the camera bridge is off, or permission is denied, don't trap: the user gets the stock test pattern.
   Alternatively, write a "no camera" card.

What it gives and what it costs:

- **Pros:** no guest code, no Helper step, no toolchain, works from first boot. Every client of the stack gets real
  pixels: the Pictures & Videos camera, `SHCameraCapture`, CF `CameraCaptureDialog`, and third-party DirectShow apps.
  JPEG encoding, file naming and WMV recording stay with the guest, so they are authentic. The trap is stateless, so it is
  snapshot-safe.
- **Cons:** sizes are limited to NULLCAM's table (stills ≤ 640x480; see §2 "Bigger stills"), and there is a single camera
  (front/back is chosen on the host). The PC-trap mechanism is new to the core, but it is small and reusable.

### (b2) Registry plug-in DLL plus a hypercall: fallback if a PC trap is awkward

- The Helper copies `wmdcam.dll` to `\Windows` and sets `HKLM\Drivers\Capture\NullCam`
  `LibraryName="\Windows\wmdcam.dll"`, `FunctionName="Fill"`.
- `Fill` is about 10 ARM instructions with **no imports**:
  1. touch one byte per 4 KB of the buffer, to commit the pages;
  2. execute a reserved coprocessor op (for example `mcr p7,0,r0,c0,c0,0`), which WMDroid intercepts in
     `Cpu::coprocessor()` before raising undefined. Today any non-CP15/CP14 op is undefined [CODE];
  3. return `r0`.
- A ~150-line Python script can emit the PE: machine `0x1C0`, one export, one `.text` section. No cegcc needed.
- **Trust:** the image is one-tier with unsigned code allowed (`research-status-indicators.md` §5.1 [OBS]), so `device.exe`
  will load it. [INFERRED]
- **Effort:** about the same as b1, plus 1–2 days for the PE generator and the Helper plumbing. It is cleaner if the JIT
  should stay free of PC traps. It doesn't work before the Helper has run.

### (a) A real WMDroid camera driver and MMIO/DMA device

**Guest side** (native DLL replacing `NullCam.dll` in `Drivers\BuiltIn`):

- **Stream interfaces:** `CAM_*` and `PIN_*`.
- **`IOCTL_CS_PROPERTY` property sets:** `CSPROPSETID_Pin`, `CSPROPSETID_Connection`, `PROPSETID_VIDCAP_VIDEOPROCAMP`,
  `…CAMERACONTROL`, `…VIDEOCONTROL` and `…DROPPEDFRAMES`.
- **Streams and buffers:** `IOCTL_STREAM_INSTANTIATE`, `IOCTL_CS_BUFFERS` (alloc/enqueue/dequeue of `CS_STREAM_HEADER`),
  and MsgQueue notifications to the capture filter.
- **Power:** IOCTLs for the camera power name / D-states.

That is the CE6/WM6 camera MDD/PDD contract. Its MDD source lives in Platform Builder (MS licence) and is not in
mingw32ce. [DOC]

**Toolchain:** cegcc / `arm-mingw32ce` in WSL is not installed. It builds CE DLLs, but you would hand-write
`cs.h`/`csmedia.h`/`camera.h` equivalents and debug against `quartz`'s Video Capture filter without a debugger.
Realistic, but **3–5 weeks** and high risk.

**Host side:** a device at a free DE-page address. Used today:

- `0x500F0000–0x500F211B` (DMA transport)
- `0x500F4000–0x500F400B` (folder sharing)
- `0x500F5000–0x500F5007` (EmulServ)
- `0x500F6000` (planned WMDroid status device, `research-status-indicators.md` §5)
- `0x500FFFC0/D0/F0` (DE 2/3 byte devices)

**Free pages:** `0x500F7000–0x500FEFFF`. Use, for example, **`0x500F8000`**: registers plus a "copy frame to guest PA"
command, with the driver passing the PA of an `AllocPhysMem` buffer.

- **Free EINTs:** 4–7 and 12–23. Polling from the driver's frame timer avoids needing a SYSINTR at all.

**Only worth it** for stills above 640x480, two guest-visible cameras, or YUY2/hardware-like behaviour. None of that is
needed for the user's goals.

### (c) User-mode only: no driver involvement

- WMDroid takes a photo or clip with the phone's camera (an overlay button, or a Helper menu command over the mailbox).
- The Android app applies the downscale and era filter and writes a JPEG straight into the shared folder
  (`\Storage Card\DCIM\…`), or the Helper copies it to `\My Documents\My Pictures`.
- Pictures & Videos then shows it. CF code can also map an MMIO page directly (§5 of the status doc), so a CF viewfinder
  app is possible.
- **Limits:** `SHCameraCapture`, `CameraCaptureDialog` and the built-in Camera keep showing NULLCAM's test pattern,
  because they always go through DirectShow → `CAM1:`. A CF app can't feed DirectShow.
- **Effort:** 1–2 days. It is a nice extra ("import phone photo", which comes almost free with storage passthrough), not
  a camera bridge.

### Bigger stills (optional extension to b1/b2)

- The still pin's two ranges (`.data+0x11A0` and `+0x16D8`) are used by no other pin.
- Rewrite them in `device.exe`'s copy of `.data` from a second trap at `CAM_Open`/`PIN_Open` (`text+0x1034` /
  `text+0x14BC`, which run before any format query). Change `SampleSize`, `biWidth/biHeight/biSizeImage`,
  `ConfigCaps.InputSize/Min/MaxOutputSize/MaxCroppingSize` and the bitrate, for example to 1280x960 and 1600x1200.
- Add `PictureResolution\3..` for those sizes through the Helper.
- 1280x960 RGB565 is 2.4 MB per buffer. Larger sizes risk `device.exe`'s 32 MB slot.
- **Effort:** +1–2 days. [INFERRED: pimg's still path and the Image Sink should accept any size the pin offers]

---

## 3. Formats and the Android side

**What the guest asks for** (exactly what the trap must produce) [ROM]:

| Use | Pin | Size | Format | Rate |
|---|---|---|---|---|
| Viewfinder (photo mode) | Preview | 320x240 (pimg default for photos [INFERRED]) | RGB565, bottom-up | 15 fps |
| Viewfinder and recording (video mode) | Preview + Capture | 160x120, 176x144, 320x240 (the VideoProfile choice) | RGB565, bottom-up (YV12 176x144 possible) | 30/30/15 fps |
| Photo | Still | 320x240 or 640x480 (`SelectBigResolution=1`) | RGB565 → guest JPEG | single |

- **Stride** is `width*2`; every size is even.
- **Bottom-up** means guest row 0 is the image's last row. With libyuv, write to the last row with a negative stride.
- **YV12** is Y, then V, then U planes, top-down.
- 176x144 (11:9) is centre-cropped from 4:3. Everything else is 4:3.
- **2009 reference:** stock WM6.5 phones shipped 2–5 MP sensors (2048x1536–2592x1944), but this Microsoft camera UI with
  NULLCAM tops out at VGA stills. That fits the "era" look; bigger stills are the optional extension above.

**Android pipeline** [INFERRED design]:

1. **CameraX setup.**
   - Use `ImageAnalysis` with `OUTPUT_IMAGE_FORMAT_YUV_420_888` and `STRATEGY_KEEP_ONLY_LATEST`.
   - `ResolutionSelector`: 4:3 aspect, target **640x480**, closest-higher fallback.
   - Camera2Interop AE target FPS `[15,30]`.
   - `CameraSelector.DEFAULT_BACK_CAMERA` or `DEFAULT_FRONT_CAMERA`, from a WMDroid setting or overlay toggle, also exposed
     to the Helper's settings page. pimg has no camera-switch UI.
   - `ImageCapture` isn't needed unless stills above VGA are added.
2. **Lifecycle.**
   - Bind lazily on the first trap hit; unbind after about 2 s without hits (pimg tears its graph down on idle and standby).
   - Request `CAMERA` at that point. Add `<uses-permission CAMERA>` and `uses-feature … required=false`.
   - Android's privacy indicator will show while the guest camera is open.
3. **Conversion** on the analysis thread (C++, libyuv BSD-3 or own NEON):
   - `Android420ToI420`, then centre-crop to the guest aspect;
   - rotate by `imageInfo.rotationDegrees`, plus a user rotation offset because pimg is portrait-aware
     (`IsPortraitScreen=1`; orientation must be verified on device);
   - mirror the front camera;
   - `I420Scale` (box filter) to the last size the guest requested;
   - era filter;
   - `I420ToRGB565` with a negative height (bottom-up), or keep YV12.
   - Publish into a mutex-protected latest-frame slot keyed by (w, h, format). The trap only does a memcpy (≤ 600 KB, well
     under 1 ms).
   - On a size change, the first call converts synchronously from the last I420 frame.

**"Era" filter** (toggle plus strength; applied after scaling, so it costs little at ≤ 640x480):

| Stage | 2009 phone trait | Suggested default |
|---|---|---|
| Softness | Cheap fixed-focus plastic lens, heavy ISP noise reduction | Downscale to 0.6× and back up bilinear, or a 3x3 Gaussian with σ ≈ 0.8 px. Then a light unsharp mask (amount 0.4, r = 1) for the halo look. |
| Noise | Small 1/4" CMOS | Luma: Gaussian with σ = 2 + 4·(1 − Y) on a 0..255 scale, new each frame. Chroma: σ ≈ 5, generated at half resolution and upsampled (blotchy). |
| Tone | Low dynamic range | Black level lifted to about 10, highlights clipped (input ≥ 235 → 255), mild S-curve |
| Colour | Weak AWB | Saturation × 0.85. Default cast slightly warm/green (R +4, G +3, B −8). Optionally follow the guest's White Balance choice. |
| Vignetting | Small lens | Corners −30 %, quadratic |
| Temporal | Slow sensor readout | Frame rate capped at the guest's 15/30 fps. Optional 20 % blend with the previous frame for motion smear. |
| Quantisation | | RGB565 banding and the guest's JPEG quality (`ImageSink\Quality`) are applied by the guest anyway. Don't add them on the host. |

With the filter **off**, use a clean box downscale plus a mild sharpen.

---

## 4. Recommended plan (b1): about 6–8 days

1. **Core: HLE trap mechanism** (1 d).
   - A trap table of {signature, handler}, checked at JIT compile time and in the interpreter's fetch path.
   - A JIT exit reason `HLE` that runs the handler and resumes at `LR`.
   - Verify with `wmcore-cli --compare`: the trap must behave identically in both engines.
2. **Core: `CameraBridge`** (1 d).
   - Parse the VIH; translate and write the buffer page by page; handle bottom-up RGB565 and YV12; use the "page not
     present → fall through" rule.
   - Keep a mutexed latest-frame slot, an `onActive(w,h,fmt)` callback to JNI, and activity timestamps.
   - Log the 5th argument while moving pimg's brightness slider to confirm what it means.
3. **App: CameraX pipeline and native conversion** (1.5–2 d): permission flow, lazy bind/unbind, rotation/mirror,
   libyuv build, and the frame slot via JNI.
4. **App: era filter** (1–1.5 d), in C++. Settings: Camera bridge on/off, Back/Front, Era filter on/off plus strength,
   Rotation offset.
5. **Helper** (0.5 d): optionally copy `\Windows\camera.lnk` to `\Windows\Start Menu\Programs\Camera.lnk`; a front/back
   command over the mailbox.
6. **Test** (1–1.5 d):
   - Pictures & Videos → Camera: preview, both still sizes, and a WMV recording at all three profiles with audio (the
     mic path from the audio bridge).
   - A CF `CameraCaptureDialog` sample.
   - Saving to `\Storage Card\DCIM`, which appears on the phone.
   - Snapshot/restore while the camera is open.
   - Standard and 6.1.4/6.0 images, using the same signature.

**Optional extras:**

- larger stills via the `.data` patch (+1–2 d);
- (c) "import phone photo" (+1 d);
- (b2) if the PC trap turns out awkward (+1–2 d).

Defer (a) unless two guest-visible cameras or native YUY2 pins become requirements.

**Open questions to check on a device:**

- that the stock DE camera app runs with NULLCAM's pattern before any work starts. Run WMDroid as-is and open Pictures & Videos → Camera;
- which preview size pimg picks in photo mode;
- the orientation pimg expects on a portrait screen;
- whether `DataUsed` larger than the actual frame matters for YV12. It doesn't for RGB565, since that is a fixed size.

---

## Method and scratch artefacts

These live in the session scratchpad (`cam/`, `camtools/`), not in the repo:

- `imgfs_dump.py <img> <out> nullcam.dll …` extracted the modules and `default.hv`.
- `hvtree.py` + `regsel.py -g` produced the registry dump.
- `camtools/hdr2.py` parses `e32_rom`/`o32_rom` headers.
- `camtools/dr.py` decodes the `CS_DATARANGE_VIDEO` structures.
- `dis.sh` (NDK clang `.incbin` + `llvm-objdump --triple=armv5te --adjust-vma`) disassembled `NULLCAM.dll.S000` at `0x7B499000`.
- Cross-image checks used the same extractor on `PPC_USA_GSM_VGA_VR`, `PPC_USA_GSM_VR` (6.5.3 Pro),
  `SP_USA_GSM_WVGA_VR` (6.5.3 Std), `PPC_USA_GSM_480x800` (6.1.4), `PPC_USA_GSM_VGA_VR` (6 SDK Refresh) and
  `PPC_USA_GSM_WVGA_VR` (6.5).

## Sources

- WM6/CE6 camera driver model (MDD/PDD, `DEVCLASS_CAMERA_GUID`, pin driver, `CS_DATARANGE_VIDEO`, `IOCTL_CS_*`), recalled:
  https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ee497949(v=winembedded.60) (Camera Driver Development Concepts)
- `SHCameraCapture` / `CameraCaptureDialog`: https://learn.microsoft.com/en-us/previous-versions/bb431794(v=msdn.10)
- CameraX ImageAnalysis / ResolutionSelector: https://developer.android.com/media/camera/camerax/analyze
- libyuv (BSD-3): https://chromium.googlesource.com/libyuv/libyuv
- DE 1.0 shared source (no camera device): https://github.com/gitntel/Microsoft-Device-Emulator
