using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Tells the app what kind of sound Windows Mobile is likely making, so Android can play it on the
// matching output (ringer, notification or media volume, with Android's silent/vibrate modes):
//   "audio ring"  - a call is ringing
//   "audio media" - a media player is running
//   "audio alert" - otherwise (alerts, reminders, system sounds)
// Sent on change, and again whenever the helper starts.
static class AudioClass
{
    static readonly string[] Players = { "wmplayer.exe", "tcpmp.exe", "coreplayer.exe", "fmradio.exe", "pocketmusic.exe" };
    const int IncomingCall = 0x00010000;  // SN_PHONEINCOMINGCALL_BITMASK in HKLM\System\State\Phone\Status

    static string last = "";
    static int tick;

    public static void Resend() { last = ""; tick = 0; }

    public static void Poll()
    {
        if (tick++ % 4 != 0) return;  // ~every 2 s
        string now = Ringing() ? "ring" : MediaRunning() ? "media" : "alert";
        if (now == last) return;
        last = now;
        Helper.Send("audio " + now);
    }

    static bool Ringing()
    {
        try
        {
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"System\State\Phone"))
            {
                object o = k == null ? null : k.GetValue("Status");
                return o is int && ((int)o & IncomingCall) != 0;
            }
        }
        catch { return false; }
    }

    static bool MediaRunning()
    {
        IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPNOHEAPS, 0);
        if (snap == IntPtr.Zero || snap == new IntPtr(-1)) return false;
        try
        {
            byte[] pe = new byte[PeSize];
            BitConverter.GetBytes(PeSize).CopyTo(pe, 0);
            bool more = Process32First(snap, pe);
            while (more)
            {
                string exe = System.Text.Encoding.Unicode.GetString(pe, 36, 520);
                int nul = exe.IndexOf('\0');
                if (nul >= 0) exe = exe.Substring(0, nul);
                exe = exe.ToLower();
                foreach (string p in Players) if (exe == p) return true;
                more = Process32Next(snap, pe);
            }
            return false;
        }
        finally { CloseToolhelp32Snapshot(snap); }
    }

    // PROCESSENTRY32 on CE: 9 DWORDs, WCHAR szExeFile[260], th32MemoryBase, th32AccessKey.
    const int PeSize = 36 + 520 + 8;
    const uint TH32CS_SNAPPROCESS = 0x2, TH32CS_SNAPNOHEAPS = 0x40000000;

    [DllImport("toolhelp.dll")] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("toolhelp.dll")] static extern bool CloseToolhelp32Snapshot(IntPtr snap);
    [DllImport("toolhelp.dll")] static extern bool Process32First(IntPtr snap, byte[] pe);
    [DllImport("toolhelp.dll")] static extern bool Process32Next(IntPtr snap, byte[] pe);
}
