using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32;

// Screen DPI (Device settings, "dpi" in state.ini): Windows Mobile's logical DPI comes from
// HKLM\Drivers\Display\GPE LogicalPixelsX/Y, which GWES reads once at boot. The screen keeps its
// pixel size; 96 dpi draws everything half as large as 192 (four times the content). "0" puts
// the image's own value back. A change is written, flushed and applied with one restart.
static class Dpi
{
    const string GpeKey = @"Drivers\Display\GPE";
    const string StateKey = @"Software\WMDroid";

    // Set after the helper's first check: later checks come with the app's "hello".
    static bool armed;

    // Returns true when a restart is under way (the caller should stop).
    public static bool Check()
    {
        armed = true;
        try
        {
            string want = Helper.Get("dpi", "0");
            int target;
            using (RegistryKey st = Registry.LocalMachine.CreateSubKey(StateKey))
            using (RegistryKey gpe = Registry.LocalMachine.CreateSubKey(GpeKey))
            {
                int reg = Int(gpe.GetValue("LogicalPixelsX"));
                // The image's own DPI, remembered before the first change.
                if (st.GetValue("DpiDefault") == null && reg > 0) st.SetValue("DpiDefault", reg);
                target = want == "0" ? Int(st.GetValue("DpiDefault")) : Int(want);
                if (target < 48 || target > 384) return false;
                int live = Live();
                if (live == target && reg == target)
                {
                    if (st.GetValue("DpiTried") != null) st.DeleteValue("DpiTried");
                    return false;
                }
                // Once per target: if a restart already happened and the screen still isn't at it,
                // don't restart again (no loops); the next different choice tries again.
                if (Int(st.GetValue("DpiTried")) == target && reg == target)
                {
                    Helper.Log("dpi: still " + live + " after the restart for " + target);
                    return false;
                }
                gpe.SetValue("LogicalPixelsX", target);
                gpe.SetValue("LogicalPixelsY", target);
                st.SetValue("DpiTried", target);
                if (live == target) return false;  // registry only (it said otherwise)
            }
            RegFlushKey(HKEY_LOCAL_MACHINE);
            // "patched": the app treats the restarted system as its new boot state (as for Patch).
            Helper.Send("patched dpi " + target);
            Helper.Log("dpi " + Live() + " -> " + target + "; resetting");
            Thread.Sleep(1500);  // let the app read the message
            Patch.SoftReset();
            return true;
        }
        catch (Exception e)
        {
            Helper.Log("dpi: " + e.GetType().Name + " " + e.Message);
            return false;
        }
    }

    // The app said hello (a restored session, perhaps with a new choice in state.ini).
    public static bool Recheck() { return armed && Check(); }

    // The DPI the system draws at now.
    public static int Live()
    {
        IntPtr dc = GetDC(IntPtr.Zero);
        try { return GetDeviceCaps(dc, 88 /* LOGPIXELSX */); }
        finally { ReleaseDC(IntPtr.Zero, dc); }
    }

    static int Int(object v)
    {
        if (v is int) return (int)v;
        try { return v == null ? 0 : int.Parse(v.ToString()); } catch { return 0; }
    }

    static readonly UIntPtr HKEY_LOCAL_MACHINE = new UIntPtr(0x80000002u);
    [DllImport("coredll.dll")] static extern int RegFlushKey(UIntPtr hKey);
    [DllImport("coredll.dll")] static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("coredll.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("coredll.dll")] static extern int GetDeviceCaps(IntPtr dc, int index);
}
