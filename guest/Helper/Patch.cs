using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32;

// The system patch: on the first boot of a fresh image the helper moves itself into \Windows,
// removes the emulator-only parts WMDroid replaces, then soft-resets. The app saves the patched,
// booted state and starts from it afterwards, so it acts as the modified ROM.
static class Patch
{
    // Bump when Apply() changes; images patched with an older version are patched again.
    public const int Version = 7;

    // Patches below this version changed driver settings and need a restart to take effect.
    // (v4: states booted before the PCMCIA network card existed need their drivers reloaded.)
    const int RestartBelow = 4;

    const string StateKey = @"Software\WMDroid";
    public const string SystemExe = @"\Windows\WMDroid.exe";

    public static int Installed
    {
        get
        {
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(StateKey))
            {
                object v = k == null ? null : k.GetValue("Patch");
                return v is int ? (int)v : 0;
            }
        }
    }

    // Returns true when a reset is under way (the caller should stop).
    public static bool Check() { return Check(false); }

    public static bool Check(bool force)
    {
        if (!force && (Installed >= Version || Helper.Get("patch.auto", "1") == "0")) return false;
        try
        {
            bool restart = force || Installed < RestartBelow;
            needRestart = false;
            Apply();
            restart |= needRestart;
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(StateKey)) k.SetValue("Patch", Version);
            RegFlushKey(HKEY_LOCAL_MACHINE);
            if (!restart)
            {
                Helper.Log("patch " + Version + " applied");
                return false;
            }
            Helper.Send("patched " + Version);
            Helper.Log("patch " + Version + " applied; resetting");
            Thread.Sleep(1500);  // let the app read the message
            SoftReset();
            return true;
        }
        catch (Exception e)
        {
            Helper.Log("patch failed: " + e.Message);
            Helper.Send("patch failed " + e.Message);
            return false;
        }
    }

    static void Apply()
    {
        Step("helper", delegate
        {
            // The helper lives in \Windows and starts with the system, card or not.
            string self = System.Reflection.Assembly.GetExecutingAssembly().GetName().CodeBase;
            if (string.Compare(self, SystemExe, true) != 0) File.Copy(self, SystemExe, true);
            Helper.Shortcut(@"\Windows\StartUp\PocketStylus.lnk", "\"" + SystemExe + "\" serve");
            Helper.Shortcut(@"\Windows\Start Menu\Programs\PocketStylus.lnk", "\"" + SystemExe + "\"");
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"ControlPanel\PocketStylus"))
            {
                k.SetValue("Group", 2);
                k.SetValue("Redirect", SystemExe);
                k.SetValue("Icon", SystemExe + ",0");
            }
        });
        Step("radio", ModemRadio);
        Step("fakeril", delegate
        {
            // FakeRIL off: the app supplies the phone's real network state, and FakeRIL's
            // "Fake Network" events would keep overwriting it. DEVFLAGS_NOLOAD keeps the driver
            // registered but unloaded; the original flags are kept to undo it.
            if (Helper.Get("patch.fakeril", "off") != "off" || ModemRil) return;
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\RIL", true))
            {
                if (k == null) return;
                object f = k.GetValue("Flags");
                int flags = f is int ? (int)f : 0;
                using (RegistryKey st = Registry.LocalMachine.CreateSubKey(StateKey))
                    if (st.GetValue("RilFlags") == null) st.SetValue("RilFlags", flags);
                k.SetValue("Flags", flags | 0x4);
            }
        });
        Step("apps", InstallApps);
        Step("net", EnsureInternet);
        Step("ethman", NoWiredUi);
        Step("gps", delegate
        {
            // The GPS Intermediate Driver reads the phone's GPS from COM1 and shares it
            // (GPS API, and raw NMEA on COM4: for older programs).
            // On rilgsm images COM1 is the radio (ModemRadio), so the GPS is not set up there.
            if (ModemRil) return;
            const string Gps = @"System\CurrentControlSet\GPS Intermediate Driver";
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Gps)) k.SetValue("IsEnabled", 1);
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Gps + @"\Drivers")) k.SetValue("CurrentDriver", "WMDroid");
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Gps + @"\Drivers\PocketStylus"))
            {
                k.SetValue("InterfaceType", "COMM");
                k.SetValue("CommPort", "COM1:");
                k.SetValue("FriendlyName", "Phone GPS (PocketStylus)");
            }
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Gps + @"\Multiplexer")) k.SetValue("DriverInterface", "COM4:");
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Gps + @"\Multiplexer\ActiveDevice"))
            {
                k.SetValue("Prefix", "COM");
                k.SetValue("Index", 4);
            }
        });
    }

    delegate void Action0();

    static bool needRestart;  // a step changed drivers that load at boot
    static int modemRil = -1;

    // WM 6 / 6.1 / 6.1.4 (and 6.5 before 6.5.3) have Microsoft's sample GSM RIL (rilgsm.dll) instead
    // of FakeRIL. It talks AT commands to a radio, and the app's virtual modem on COM1 is that radio
    // (core/src/board/modem.cpp): the real network state and calls come through the RIL itself.
    public static bool ModemRil
    {
        get
        {
            if (modemRil < 0)
                using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\RIL"))
                {
                    string dll = k == null ? null : k.GetValue("Dll") as string;
                    modemRil = dll != null && string.Compare(dll, "rilgsm.dll", true) == 0 ? 1 : 0;
                }
            return modemRil == 1;
        }
    }

    // rilgsm opens Software\Microsoft\RIL ComPort, VEI1: in the image: a channel of the CAIF mux
    // (caifmux.dll on COM1, vei.dll), which nothing on this board answers. Point it at COM1: itself
    // and keep the mux and its channels from loading, so the port is free and plain AT reaches the
    // modem (docs/research-cellular.md sections 1.2 and 4). Takes effect at the next boot.
    static void ModemRadio()
    {
        if (!ModemRil) return;
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"Software\Microsoft\RIL"))
        {
            if (!"COM1:".Equals(k.GetValue("ComPort") as string)) { k.SetValue("ComPort", "COM1:"); needRestart = true; }
        }
        string[] mux = { "MUX", "MUX1", "MUX2", "VEI1", "VEI2", "VEI3", "VEI4", "VEI5", "VEI6" };
        foreach (string name in mux)
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\" + name, true))
            {
                if (k == null) continue;
                object f = k.GetValue("Flags");
                int flags = f is int ? (int)f : 0;
                if ((flags & 0x4) == 0) { k.SetValue("Flags", flags | 0x4); needRestart = true; }  // DEVFLAGS_NOLOAD
            }
        // Patch 5 unloaded every RIL ("fakeril" step); this one must load.
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\RIL", true))
        {
            object f = k.GetValue("Flags");
            int flags = f is int ? (int)f : 0;
            if ((flags & 0x4) != 0) { k.SetValue("Flags", flags & ~0x4); needRestart = true; }
        }
        Helper.Log("radio: rilgsm on COM1 (virtual modem)" + (needRestart ? "; restart needed" : ""));
    }

    // The NE2000 card stands for the phone's own internet (mobile data or Wi-Fi), not a cable. The
    // Ethernet Manager (Drivers\BuiltIn\Ethman, ethman.dll, 5.8 KB) only drives netui's wired-network
    // UI: it watches the adapters (GetAdaptersInfo, address-change notifications) and calls netui's
    // AddNetUISystrayIcon / UpdateConnectionStatus, which posts 0x480 (wParam 1) to the taskbar, and
    // the taskbar then shows its two-arrow "wired" icon in place of the G/3G/H letter or Wi-Fi icon
    // (docs/research-status-indicators.md 2.1). Connection Manager does not use it. Not loading it
    // keeps the title bar showing the phone's state with no helper repainting it. Takes effect at
    // the next boot; the original flags are kept to undo it.
    static void NoWiredUi()
    {
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\Ethman", true))
        {
            if (k == null) return;
            object f = k.GetValue("Flags");
            int flags = f is int ? (int)f : 0;
            if ((flags & 0x4) != 0) return;  // DEVFLAGS_NOLOAD already
            using (RegistryKey st = Registry.LocalMachine.CreateSubKey(StateKey))
                if (st.GetValue("EthmanFlags") == null) st.SetValue("EthmanFlags", flags);
            k.SetValue("Flags", flags | 0x4);
            needRestart = true;
        }
    }

    // ethman.dll is not loaded (NoWiredUi took effect or the image has none): nothing claims a wired network.
    public static bool WiredUiOff
    {
        get
        {
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Drivers\BuiltIn\Ethman"))
            {
                object f = k == null ? null : k.GetValue("Flags");
                return k == null || (f is int && ((int)f & 0x4) != 0);
            }
        }
    }

    // Connection Manager's "The Internet" destination.
    const string InternetDest = "{436EF144-B4FB-4863-A041-8F905A62C572}";

    // The emulated NE2000 card is the device's internet: tell Connection Manager it connects to
    // "The Internet" (not "Work"), so IE and other programs use it without asking. Connection
    // Manager can put its own default back when it restarts, so the helper re-applies this at start.
    public static void EnsureInternet()
    {
        string xml =
            "<wap-provisioningdoc><characteristic type=\"CM_NetEntries\">" +
            "<characteristic type=\"NE20001\">" +
            "<parm name=\"DestId\" value=\"" + InternetDest + "\"/>" +
            "<parm name=\"Adapter\" value=\"NE20001\"/>" +
            "</characteristic></characteristic></wap-provisioningdoc>";
        IntPtr result;
        int hr = DMProcessConfigXML(xml, 1 /* CFGFLAG_PROCESS */, out result);
        if (result != IntPtr.Zero) LocalFree(result);
        if (hr != 0) Helper.Log("network provisioning 0x" + hr.ToString("X8"));
        NoNetPrompt();
    }

    // When a wired adapter comes up, ethman.dll hands it to netui.dll (AddNetUISystrayIcon), and
    // netui asks "Establishing Connection: This network card connects me to: The Internet / Work"
    // (a notification, CLSID {8DDF46E7-56ED-4750-9E58-AFC6CE486D03}, ID 0x1002) unless
    // HKLM\ControlPanel\WiFi\Ethernet holds the answer: DestId, the destination GUID as a string.
    // The answer is one value for every wired card, so it also covers a re-inserted card;
    // CM_NetEntries above does not stop the question. WM 5, 6.1.4 and 6.5.3 all work this way
    // (docs/research-gps-network.md B3). On a fresh image netui asks a few seconds before the helper
    // can run, so a question already waiting is taken down as well; "The Internet" is also its
    // default answer. Runs at every helper start; logs only when it changes something.
    public static void NoNetPrompt()
    {
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"ControlPanel\WiFi\Ethernet"))
        {
            if (string.Compare(k.GetValue("DestId") as string, InternetDest, true) != 0)
            {
                k.SetValue("DestId", InternetDest);
                RegFlushKey(HKEY_LOCAL_MACHINE);  // WM 5 loses unflushed values on a reset
                Helper.Log("net: network card connects to The Internet");
            }
        }
        Guid netui = new Guid("8DDF46E7-56ED-4750-9E58-AFC6CE486D03");
        if (SHNotificationRemove(ref netui, 0x1002) == 0) Helper.Log("net: removed the network card question");
    }

    [DllImport("aygshell.dll")] static extern int SHNotificationRemove(ref Guid clsid, int id);

    // Guest programs that ship with the helper: copied into \Windows with a Start menu entry.
    static void InstallApps()
    {
        string dir = Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().GetName().CodeBase);
        string[][] apps = { new string[] { "GpsTest.exe", "GPS Test" } };
        foreach (string[] a in apps)
        {
            string src = Path.Combine(@"\Storage Card\2577", a[0]);
            if (!File.Exists(src)) src = Path.Combine(dir, a[0]);
            if (!File.Exists(src)) continue;
            string dst = Path.Combine(@"\Windows", a[0]);
            File.Copy(src, dst, true);
            Helper.Shortcut(@"\Windows\Start Menu\Programs\" + a[1] + ".lnk", "\"" + dst + "\"");
        }
    }

    // One patch step; a failing step is reported and the others still run.
    static void Step(string name, Action0 a)
    {
        try { a(); }
        catch (Exception e)
        {
            Helper.Log("patch " + name + ": " + e.GetType().Name + " " + e.Message);
            Helper.Send("patch step " + name + " failed: " + e.GetType().Name + " " + e.Message);
        }
    }

    public static void SoftReset()
    {
        // IOCTL_HAL_REBOOT: the OAL reboots through the watchdog, which the emulator turns into
        // the board's reset button (the power manager's reset path jumps to 0 with the MMU on).
        int ret;
        KernelIoControl(0x0101003C, IntPtr.Zero, 0, IntPtr.Zero, 0, out ret);
    }

    static readonly UIntPtr HKEY_LOCAL_MACHINE = new UIntPtr(0x80000002u);
    [DllImport("coredll.dll")] static extern int RegFlushKey(UIntPtr hKey);
    [DllImport("aygshell.dll", CharSet = CharSet.Unicode)]
    static extern int DMProcessConfigXML(string xml, int flags, out IntPtr result);

    // Runs a provisioning document; returns the result XML (queries) or the error code.
    public static string Provision(string xml)
    {
        IntPtr result;
        int hr = DMProcessConfigXML(xml, 1, out result);
        string text = result != IntPtr.Zero ? Marshal.PtrToStringUni(result) : "";
        if (result != IntPtr.Zero) LocalFree(result);
        return "hr=0x" + hr.ToString("X8") + " " + text;
    }
    [DllImport("coredll.dll")] static extern IntPtr LocalFree(IntPtr p);
    [DllImport("coredll.dll")]
    static extern bool KernelIoControl(int code, IntPtr inBuf, int inSize, IntPtr outBuf, int outSize, out int returned);
}
