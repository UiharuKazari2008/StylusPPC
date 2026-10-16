using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Camera passthrough: plugs WMDroid's frame generator into NULLCAM.dll, the sample camera driver
// every WM 6.x image ships (docs/research-camera.md). NULLCAM reads HKLM\Drivers\Capture\NullCam
// LibraryName/FunctionName on each pin open, so the Camera in Pictures & Videos, SHCameraCapture
// and DirectShow apps all get the phone's camera from their next start; no reset is needed.
// wmdcam.dll (wmdcam.s, embedded as CameraDll.cs) hands each frame request to the emulator.
//
// Portrait sensor: the emulator offers the camera's sizes in the panel's orientation (240x320
// preview and 480x640 photos on a portrait panel, like a real phone's sensor). pimg takes its sizes
// from the registry below and fails if they disagree with the driver, so they are set to match.
static class Camera
{
    const string Dll = @"\Windows\wmdcam.dll";
    const string Key = @"Drivers\Capture\NullCam";

    public static void Install()
    {
        byte[] want = Convert.FromBase64String(CameraDll.Base64);
        if (!SameFile(Dll, want))
        {
            // In use while a camera app runs: the current copy keeps working until the next start.
            try { using (var f = File.Create(Dll)) f.Write(want, 0, want.Length); }
            catch (Exception e) { Helper.Log("camera dll " + e.Message); return; }
        }
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Key))
        {
            if ((k.GetValue("LibraryName") as string) != Dll) k.SetValue("LibraryName", Dll);
            if ((k.GetValue("FunctionName") as string) != "Fill") k.SetValue("FunctionName", "Fill");
        }
        Orient();
    }

    const string Oem = @"Software\Microsoft\Pictures\Camera\OEM";

    static bool inStep;   // pimg's registry sizes match the sensor

    // From the helper's loop: keep telling the emulator the registry is in step. It turns the
    // driver's formats only after hearing this in the current run (a mismatch crashes pimg).
    public static void Poll()
    {
        if (inStep && regs != IntPtr.Zero) Marshal.WriteInt32(regs, 0x18, 0x200);
    }

    // pimg's photo and video sizes in the sensor's orientation (the emulator's ORIENT register).
    public static void Orient()
    {
        IntPtr regs = Map();
        if (regs == IntPtr.Zero) return;
        bool portrait = Marshal.ReadInt32(regs, 0x18) == 1;
        foreach (string list in new[] { "PictureResolution", "VideoProfile" })
        {
            using (RegistryKey l = Registry.LocalMachine.OpenSubKey(Oem + "\\" + list))
            {
                if (l == null) continue;
                foreach (string item in l.GetSubKeyNames())
                {
                    using (RegistryKey k = Registry.LocalMachine.OpenSubKey(Oem + "\\" + list + "\\" + item, true))
                    {
                        if (k == null) continue;
                        object w = k.GetValue("Width"), h = k.GetValue("Height");
                        if (!(w is int) || !(h is int) || (int)w == (int)h) continue;
                        if (((int)w < (int)h) == portrait) continue;
                        k.SetValue("Width", (int)h);
                        k.SetValue("Height", (int)w);
                    }
                }
            }
        }
        inStep = true;
        Poll();
    }

    static IntPtr regs;

    static IntPtr Map()
    {
        if (regs != IntPtr.Zero) return regs;
        IntPtr p = VirtualAlloc(IntPtr.Zero, 0x1000, 0x2000, 1);   // MEM_RESERVE, PAGE_NOACCESS
        if (p == IntPtr.Zero) return IntPtr.Zero;
        // PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL; the address is 0x5F000000 >> 8
        if (!VirtualCopy(p, new IntPtr(0x5F000000 >> 8), 0x1000, 0x604) || Marshal.ReadInt32(p, 0) != 0x4D414357)
        {
            VirtualFree(p, 0, 0x8000);
            return IntPtr.Zero;
        }
        return regs = p;
    }

    [DllImport("coredll.dll")] static extern IntPtr VirtualAlloc(IntPtr addr, int size, int type, int protect);
    [DllImport("coredll.dll")] static extern bool VirtualCopy(IntPtr dest, IntPtr src, int size, int protect);
    [DllImport("coredll.dll")] static extern bool VirtualFree(IntPtr addr, int size, int type);

    static bool SameFile(string path, byte[] want)
    {
        try
        {
            if (!File.Exists(path)) return false;
            byte[] have;
            using (var f = File.OpenRead(path)) { have = new byte[f.Length]; f.Read(have, 0, have.Length); }
            if (have.Length != want.Length) return false;
            for (int i = 0; i < have.Length; i++) if (have[i] != want[i]) return false;
            return true;
        }
        catch { return false; }
    }
}
