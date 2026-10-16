using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Mirrors the phone's screen brightness into Windows Mobile's backlight settings (the values the
// Backlight settings page and OEM backlight drivers use), then tells the driver to re-read them.
static class Backlight
{
    // backlight <0-100>
    public static string Set(string args)
    {
        int pct = Math.Max(0, Math.Min(100, int.Parse(args.Trim())));
        int level = (pct * 10 + 50) / 100;  // WM brightness levels 0..10
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"ControlPanel\Backlight"))
        {
            k.SetValue("Brightness", level);
            k.SetValue("ACBrightness", level);
        }
        IntPtr ev = CreateEvent(IntPtr.Zero, false, false, "BackLightChangeEvent");
        if (ev != IntPtr.Zero)
        {
            EventModify(ev, 3 /* EVENT_SET */);
            CloseHandle(ev);
        }
        return "ok " + level;
    }

    [DllImport("coredll.dll")] static extern IntPtr CreateEvent(IntPtr attr, bool manual, bool initial, string name);
    [DllImport("coredll.dll")] static extern bool EventModify(IntPtr ev, int func);
    [DllImport("coredll.dll")] static extern bool CloseHandle(IntPtr h);
}
