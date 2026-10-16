using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Windows Mobile's own auto-standby ("Turn off device if not used for N minutes") suspends the whole
// system: nothing runs, so the app's mailbox commands pile up until something presses the power
// button. Android already decides when the phone sleeps (and the app locks it with WM's display),
// so the helper switches WM's suspend timeouts off. The display timeouts are left alone.
static class Standby
{
    const string Timeouts = @"System\CurrentControlSet\Control\Power\Timeouts";

    public static void Disable()
    {
        try
        {
            bool changed = false;
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Timeouts))
            {
                foreach (string name in new string[] { "BattSuspendTimeout", "ACSuspendTimeout" })
                {
                    object v = k.GetValue(name);
                    Helper.Log("standby: " + name + " was " + (v == null ? "unset" : v.ToString()));
                    if (v is int && (int)v == 0) continue;
                    k.SetValue(name, 0);
                    changed = true;
                }
            }
            if (!changed) return;
            // The power manager re-reads its timeouts when this event is set.
            IntPtr e = CreateEvent(IntPtr.Zero, false, false, "PowerManager/ReloadActivityTimeouts");
            if (e != IntPtr.Zero)
            {
                EventModify(e, 3 /* EVENT_SET */);
                CloseHandle(e);
            }
            Helper.Log("standby: WM auto-off disabled");
        }
        catch (Exception x) { Helper.Log("standby " + x.Message); }
    }

    [DllImport("coredll.dll")] static extern IntPtr CreateEvent(IntPtr attr, bool manual, bool initial, string name);
    [DllImport("coredll.dll")] static extern bool EventModify(IntPtr h, int func);
    [DllImport("coredll.dll")] static extern bool CloseHandle(IntPtr h);
}
