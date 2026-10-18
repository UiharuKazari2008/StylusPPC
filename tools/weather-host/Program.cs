using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32;

// Runs on the WM 6.1 emulator: installs the Today item, fetches the forecast the same way the
// helper does (guest/Helper/Weather.cs), then restarts the shell so Today loads the item.
class Program
{
    const string InternetDest = "{436EF144-B4FB-4863-A041-8F905A62C572}";

    static void Main()
    {
        string note = "ok";
        try { Connect(); }
        catch (Exception e) { note = "connect " + e.Message; }
        try { Weather.Install(); }
        catch (Exception e) { note = "install " + e.Message; }
        Exception last = null;
        for (int i = 0; i < 5; i++)
        {
            try { Weather.FetchNow(); last = null; break; }
            catch (Exception e) { last = e; Thread.Sleep(4000); }
        }
        if (last != null) note = "fetch " + last.Message;
        try
        {
            using (var w = new StreamWriter(@"\Storage Card\weather-host.txt", false)) w.Write(note);
        }
        catch (Exception) { }
        try { RestartShell(); }
        catch (Exception e)
        {
            try { using (var w = new StreamWriter(@"\Storage Card\weather-host.txt", true)) w.Write("\r\nshell " + e.Message); }
            catch (Exception) { }
        }
    }

    // The emulated network card is "The Internet", same answer as Patch.EnsureInternet.
    static void Connect()
    {
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"ControlPanel\WiFi\Ethernet"))
            k.SetValue("DestId", InternetDest);
        string xml =
            "<wap-provisioningdoc><characteristic type=\"CM_NetEntries\">" +
            "<characteristic type=\"NE20001\">" +
            "<parm name=\"DestId\" value=\"" + InternetDest + "\"/>" +
            "<parm name=\"Adapter\" value=\"NE20001\"/>" +
            "</characteristic></characteristic></wap-provisioningdoc>";
        IntPtr result;
        DMProcessConfigXML(xml, 1, out result);
        if (result != IntPtr.Zero) LocalFree(result);
    }

    static void RestartShell()
    {
        Kill("shell32.exe");
        Thread.Sleep(1000);
        Process.Start(@"\Windows\shell32.exe", "");
    }

    static int Kill(string name)
    {
        int n = 0;
        IntPtr snap = CreateToolhelp32Snapshot(2 | 0x40000000, 0);
        if (snap == (IntPtr)(-1)) return 0;
        var pe = new PROCESSENTRY32();
        pe.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
        for (bool ok = Process32First(snap, ref pe); ok; ok = Process32Next(snap, ref pe))
        {
            if (string.Compare(pe.szExeFile, name, true) == 0)
            {
                IntPtr h = OpenProcess(1, false, pe.th32ProcessID);
                if (h != IntPtr.Zero) { if (TerminateProcess(h, 0)) n++; CloseHandle(h); }
            }
        }
        CloseToolhelp32Snapshot(snap);
        return n;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct PROCESSENTRY32
    {
        public uint dwSize, cntUsage, th32ProcessID, th32DefaultHeapID, th32ModuleID, cntThreads, th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
        public uint th32MemoryBase, th32AccessKey;
    }

    [DllImport("aygshell.dll", CharSet = CharSet.Unicode)]
    static extern int DMProcessConfigXML(string xml, int flags, out IntPtr result);
    [DllImport("coredll.dll")] static extern IntPtr LocalFree(IntPtr p);
    [DllImport("toolhelp.dll")] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("toolhelp.dll")] static extern bool Process32First(IntPtr snap, ref PROCESSENTRY32 pe);
    [DllImport("toolhelp.dll")] static extern bool Process32Next(IntPtr snap, ref PROCESSENTRY32 pe);
    [DllImport("toolhelp.dll")] static extern bool CloseToolhelp32Snapshot(IntPtr snap);
    [DllImport("coredll.dll")] static extern IntPtr OpenProcess(int access, bool inherit, uint pid);
    [DllImport("coredll.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("coredll.dll")] static extern bool CloseHandle(IntPtr h);
}
