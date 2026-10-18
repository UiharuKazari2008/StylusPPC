using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
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
    static string lastTitle = "";
    static string lastMeta = "";
    static int lastSentPos = -1;
    static int lastErr;
    static int tick;
    static int nudgeAt;
    static uint playerPid;
    static string playerExe = "";
    static IntPtr playerHwnd;
    static bool hookInstalled;
    // {E7C4A11E-6B3D-4F2A-9C15-8D0E5F6A7B21} — guest/WmpHook/wmp.c
    const string HookClsid = "{E7C4A11E-6B3D-4F2A-9C15-8D0E5F6A7B21}";
    // Kept for the EnumWindows call: a delegate the GC collected mid-enumeration skips windows.
    static EnumProc enumProc;

    public static void Resend() { last = ""; lastTitle = ""; lastMeta = ""; lastSentPos = -1; tick = 0; lastMedia = -2; lastRing = -2; lastNote = -2; }

    // The speaker slider is the system volume (ControlPanel\Volume and SoundCategories\System).
    // The wave device is not that slider: with nothing playing it reads 0, which is the wrong
    // "media" level. Ringtone is SoundCategories\Ring. Reported as 0–100.
    static int lastMedia = -2, lastRing = -2, lastNote = -2;

    static void PollVolume() { }

    /** Volume levels are not synced. This does not read or write either side. */
    public static string SetVolumes(string rest)
    {
        return "ok";
    }

    static int WavePercent()
    {
        uint v;
        if (waveOutGetVolume(IntPtr.Zero, out v) != 0) return -1;
        return (int)(((v & 0xFFFF) * 100) / 0xFFFF);
    }

    static void SetWave(int pct)
    {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        uint c = (uint)(pct * 0xFFFF / 100);
        waveOutSetVolume(IntPtr.Zero, c | (c << 16));
    }

    static int CatPercent(string cat)
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"ControlPanel\SoundCategories\" + cat))
            {
                if (k == null) return -1;
                object o = k.GetValue("InitVol");
                if (!(o is int)) return -1;
                uint v = (uint)(int)o;
                if (v == 0) return 0;
                // A full slider is 0xFFFFFFFF. Older images store 0–100, or a 16-bit wave volume.
                // Dividing those by 0xFFFFFFFF reports 0 and the phone's sliders are then muted.
                if (v <= 100) return (int)v;
                if (v <= 0xFFFF) return (int)(v * 100 / 0xFFFF);
                return (int)((ulong)v * 100 / 0xFFFFFFFFUL);
            }
        }
        catch { return -1; }
    }

    static void SetCat(string cat, int pct)
    {
        SetDword(@"ControlPanel\SoundCategories\" + cat, "InitVol", pct);
        SendMessage((IntPtr)0xFFFF, 0x1A, 0, "SoundCategories");  // WM_SETTINGCHANGE, so the shell uses it
    }

    /** The other slider in the volume bubble: HKCU\ControlPanel\Volume\Volume. */
    static void SetMaster(int pct)
    {
        SetDword(@"ControlPanel\Volume", "Volume", pct);
        SendMessage((IntPtr)0xFFFF, 0x1A, 0, "Volume");
    }

    static int MasterPercent()
    {
        return DwordPercent(@"ControlPanel\Volume", "Volume");
    }

    // RegistryKey.SetValue drops a dword whose high bit is set (anything over half volume) and the
    // slider then shows 0. Write the four bytes ourselves.
    static void SetDword(string subkey, string name, int pct)
    {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int v = unchecked((int)((ulong)pct * 0xFFFFFFFFUL / 100UL));
        IntPtr hk;
        int disp;
        const uint HKCU = 0x80000001;
        const int KEY_WRITE = 0x20006;
        if (RegCreateKeyEx(HKCU, subkey, 0, null, 0, KEY_WRITE, IntPtr.Zero, out hk, out disp) != 0) return;
        RegSetValueEx(hk, name, 0, 4, ref v, 4);  // REG_DWORD
        RegCloseKey(hk);
    }

    static int DwordPercent(string subkey, string name)
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(subkey))
            {
                if (k == null) return -1;
                object o = k.GetValue(name);
                if (!(o is int)) return -1;
                uint v = (uint)(int)o;
                if (v == 0) return 0;
                if (v <= 100) return (int)v;
                if (v <= 0xFFFF) return (int)(v * 100 / 0xFFFF);
                return (int)((ulong)v * 100 / 0xFFFFFFFFUL);
            }
        }
        catch { return -1; }
    }

    public static void Poll()
    {
        bool scan = tick++ % 4 == 0;  // process list ~every 2 s; the player plug-in is read every tick
        // A ring is checked every pass so it reaches the phone's ringtone output as it starts.
        // Anything else that is not a player is a notification (the phone's notification output).
        string now;
        if (Ringing()) now = "ring";
        else if (scan) now = MediaRunning() ? "media" : "notify";
        else now = last == "media" ? "media" : "notify";
        if (now != last)
        {
            last = now;
            Helper.Send("audio " + now);
            if (now != "media") { lastMeta = ""; lastSentPos = -1; }
        }
        PollVolume();
        if (last == "media" && playerExe == "wmplayer.exe")
        {
            if (!ReadMeta() && scan)
            {
                string title = PlayerTitle();
                if (title != lastTitle)
                {
                    lastTitle = title;
                    Helper.Send("audio title " + title);
                }
            }
        }
        else if (scan)
        {
            string title = last == "media" ? PlayerTitle() : "";
            if (title != lastTitle)
            {
                lastTitle = title;
                Helper.Send("audio title " + title);
            }
        }
    }

    /** Play, pause, next, prev, stop, seek. The phone's media controls are off. */
    public static string Command(string what)
    {
        return "off";
    }

    /** Drops WmpHook.dll and its Media Player registration. The shell loads that list. */
    public static void RemoveHook()
    {
        if (hookInstalled) return;
        hookInstalled = true;
        try { Registry.LocalMachine.DeleteSubKeyTree(@"Software\Microsoft\MediaPlayer\UIPlugins\" + HookClsid); }
        catch (Exception) { }
        try { Registry.ClassesRoot.DeleteSubKeyTree(@"CLSID\" + HookClsid); }
        catch (Exception) { }
        foreach (string path in new string[] { @"\Windows\WmpHook.dll", @"\Windows\WmpHook.dll.old" })
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception)
            {
                try { File.Move(path, path + ".dead"); }
                catch (Exception e) { Helper.Log("wmp hook " + e.Message); }
            }
        }
    }

    static void WriteCmd(string what)
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\WMDroid\Media"))
                if (k != null) k.SetValue("cmd", what);
        }
        catch (Exception e) { Helper.Log("wmp cmd " + e.Message); }
    }

    static int ReadSeq()
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\WMDroid\Media"))
            {
                object o = k == null ? null : k.GetValue("seq");
                return o is int ? (int)o : 0;
            }
        }
        catch { return 0; }
    }

    /** True once the plug-in has published. Sends track, artist, album and position when they move. */
    static bool ReadMeta()
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\WMDroid\Media"))
            {
                if (k == null) return false;
                object seq = k.GetValue("seq");
                if (!(seq is int) || (int)seq == 0) return false;
                object err = k.GetValue("err");
                if (err is int && (int)err != 0 && (int)err != lastErr)
                {
                    lastErr = (int)err;
                    Helper.Log("wmp err " + lastErr.ToString("X8"));
                }
                int state = k.GetValue("state") is int ? (int)k.GetValue("state") : 0;
                int pos = k.GetValue("pos") is int ? (int)k.GetValue("pos") : 0;
                int dur = k.GetValue("dur") is int ? (int)k.GetValue("dur") : 0;
                if (pos < 0) pos = 0;
                if (dur < 0) dur = 0;
                string title = k.GetValue("title") as string ?? "";
                string artist = k.GetValue("artist") as string ?? "";
                string album = k.GetValue("album") as string ?? "";
                if (title.Length == 0) title = lastTitle;
                string word = (state == 3 || state == 4 || state == 5 || state == 6) ? "playing"
                    : state == 2 ? "paused" : "stopped";
                string sig = word + "\t" + title + "\t" + artist + "\t" + album + "\t" + dur;
                bool moved = lastSentPos < 0 || pos - lastSentPos >= 1000 || lastSentPos - pos >= 1000;
                if (sig != lastMeta || moved)
                {
                    lastMeta = sig;
                    lastSentPos = pos;
                    Helper.Send("audio meta " + word + " " + pos + " " + dur + " " + title + "\t" + artist + "\t" + album);
                }
                return true;
            }
        }
        catch { return false; }
    }

    /** Asks a player that is already open to look at UIPlugins again. Harmless if it ignores it. */
    static void Nudge()
    {
        int now = Environment.TickCount;
        if (unchecked(now - nudgeAt) < 5000) return;
        nudgeAt = now;
        uint msg = RegisterWindowMessage("WMPlayer_PluginAddRemove");
        if (msg != 0) PostMessage((IntPtr)0xFFFF, (int)msg, 0, 0);
    }

    static string PlayerTitle()
    {
        playerHwnd = IntPtr.Zero;
        foundTitle = "";
        if (playerPid == 0) return "";
        wantPid = playerPid;
        if (enumProc == null) enumProc = OnWindow;
        EnumWindows(enumProc, IntPtr.Zero);
        return foundTitle;
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
        playerPid = 0;
        playerExe = "";
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
                foreach (string p in Players)
                    if (exe == p)
                    {
                        playerPid = BitConverter.ToUInt32(pe, 8);  // th32ProcessID
                        playerExe = exe;
                        return true;
                    }
                more = Process32Next(snap, pe);
            }
            return false;
        }
        finally { CloseToolhelp32Snapshot(snap); }
    }

    // PROCESSENTRY32 on CE: 9 DWORDs, WCHAR szExeFile[260], th32MemoryBase, th32AccessKey.
    const int PeSize = 36 + 520 + 8;
    const uint TH32CS_SNAPPROCESS = 0x2, TH32CS_SNAPNOHEAPS = 0x40000000;

    static uint wantPid;
    static string foundTitle = "";
    delegate int EnumProc(IntPtr hwnd, IntPtr lp);

    static int OnWindow(IntPtr hwnd, IntPtr lp)
    {
        uint pid;
        GetWindowThreadProcessId(hwnd, out pid);
        if (pid != wantPid || !IsWindowVisible(hwnd)) return 1;
        StringBuilder sb = new StringBuilder(256);
        GetWindowText(hwnd, sb, 256);
        string t = sb.ToString();
        if (t.Length > foundTitle.Length) { foundTitle = t; playerHwnd = hwnd; }
        return 1;
    }

    const int WM_KEYDOWN = 0x100, WM_KEYUP = 0x101;

    [DllImport("coredll")] static extern bool EnumWindows(EnumProc proc, IntPtr lp);
    [DllImport("coredll")] static extern int waveOutGetVolume(IntPtr device, out uint volume);
    [DllImport("coredll")] static extern int waveOutSetVolume(IntPtr device, uint volume);
    [DllImport("coredll.dll", CharSet = CharSet.Unicode)]
    static extern int RegCreateKeyEx(uint hKey, string sub, int reserved, string cls, int options, int sam, IntPtr sec, out IntPtr hk, out int disp);
    [DllImport("coredll.dll", CharSet = CharSet.Unicode)]
    static extern int RegSetValueEx(IntPtr hk, string name, int reserved, int type, ref int data, int cb);
    [DllImport("coredll.dll")] static extern int RegCloseKey(IntPtr hk);
    [DllImport("coredll", CharSet = CharSet.Unicode)] static extern int SendMessage(IntPtr hwnd, int msg, int wParam, string lParam);
    [DllImport("coredll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("coredll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int n);
    [DllImport("coredll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("coredll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("coredll")] static extern bool PostMessage(IntPtr hwnd, int msg, int wparam, int lparam);
    [DllImport("coredll", CharSet = CharSet.Unicode)] static extern uint RegisterWindowMessage(string name);

    [DllImport("toolhelp.dll")] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("toolhelp.dll")] static extern bool CloseToolhelp32Snapshot(IntPtr snap);
    [DllImport("toolhelp.dll")] static extern bool Process32First(IntPtr snap, byte[] pe);
    [DllImport("toolhelp.dll")] static extern bool Process32Next(IntPtr snap, byte[] pe);
}
