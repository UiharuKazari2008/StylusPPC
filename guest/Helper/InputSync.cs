using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;
using Microsoft.WindowsCE.Forms;

// Clipboard sync and "Use Android IME over Device IME" (the app's ClipboardBridge.kt / ImeBridge.kt).
//
// Both run on one hidden window with its own message loop: the clipboard needs an owner window that
// answers WM_DESTROYCLIPBOARD (a window that never pumps would hang every program that empties the
// clipboard), and the keyboard watch hears WM_SETTINGCHANGE (SPI_SETSIPINFO) at once besides polling.
//
// Clipboard (app commands "clip", "clip watch 1|0", "clip get"):
//   app -> WM: the app writes clip_in.txt (UTF-8) and sends "clip"; we set CF_UNICODETEXT.
//   WM -> app: while watching, the WM clipboard is hashed once a second; a change we did not make
//   ourselves goes to clip_out.txt (UTF-8) and the app hears "clip". Remembering the hash of what we
//   set (and taking the current clipboard as the baseline when watching starts) prevents ping-pong.
//
// Phone keyboard instead of WM's SIP (app commands "ime watch 1|0", "ime close", "ime diag"):
//   while watching, whenever WM raises its SIP we hide the SIP's window (SipWndClass) but leave the
//   SIP state on, so programs keep their layout above the keyboard area, and send "ime open"; the
//   app shows the phone's keyboard over that area. WM lowering its SIP (focus left the text box, or
//   the SIP button tapped again) sends "ime hide". The app sends "ime close" when the phone's
//   keyboard was dismissed: we lower the SIP (and keep it down for a moment, so a program that
//   re-raises it at once does not bounce the phone's keyboard back). Without a SipWndClass window we
//   fall back to lowering the SIP outright.
//   Typing: while the phone's keyboard is open the app writes what it types as type\<seq>.txt
//   (read every 50 ms here; tokens as for the "type" command, see Type) and we post it to the
//   focused window as an input method would (PostKeybdMessage), not through the emulated keyboard:
//   WM lowers its SIP on every hardware key press.
// Details and limits: docs/clipboard-ime.md.
static class InputSync
{
    const int WM_TIMER = 0x0113, WM_SETTINGCHANGE = 0x001A, WM_DESTROYCLIPBOARD = 0x0307;
    const int SPI_SETSIPINFO = 224;
    const int MSG_CALL = 0x8000 + 0x57;  // WM_APP + n: run a command on the window's thread
    const int TIMER_CLIP = 1, TIMER_SIP = 2;
    const int CLIP_MS = 1000, SIP_MS = 150, TYPE_MS = 50;
    const uint CF_UNICODETEXT = 13;
    const int SIPF_OFF = 0, SIPF_ON = 1;
    const int SW_HIDE = 0, SW_SHOWNA = 8;
    const int MaxClip = 256 * 1024;  // characters

    static Window win;
    static IntPtr hwnd;
    static Thread thread;

    static bool clipWatch;
    static uint clipHash;
    static bool clipKnown;

    static bool imeWatch, imeOpen, sipHiddenByUs;
    static int quietUntil;

    class Window : MessageWindow
    {
        protected override void WndProc(ref Message m)
        {
            try
            {
                if (m.Msg == WM_TIMER && (int)m.WParam == TIMER_CLIP) ClipPoll();
                else if (m.Msg == WM_TIMER && (int)m.WParam == TIMER_SIP) SipPoll();
                else if (m.Msg == WM_SETTINGCHANGE && (int)m.WParam == SPI_SETSIPINFO) SipPoll();
                else if (m.Msg == MSG_CALL) { m.Result = IntPtr.Zero; callResult = Run(callVerb, callArgs); return; }
            }
            catch (Exception e) { Helper.Log("input " + e.Message); }
            base.WndProc(ref m);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Thread and commands

    public static void Start()
    {
        if (thread != null) return;
        var ready = new ManualResetEvent(false);
        thread = new Thread(delegate ()
        {
            win = new Window();
            hwnd = win.Hwnd;
            SetTimer(hwnd, TIMER_CLIP, CLIP_MS, IntPtr.Zero);
            ready.Set();
            MSG msg;
            while (GetMessage(out msg, IntPtr.Zero, 0, 0))
            {
                TranslateMessage(ref msg);
                DispatchMessage(ref msg);
            }
        });
        thread.IsBackground = true;
        thread.Start();
        ready.WaitOne(5000, false);
    }

    /// Ends the window's loop (the helper is exiting, e.g. for an update).
    public static void Stop()
    {
        if (hwnd != IntPtr.Zero) PostMessage(hwnd, 0x0012 /* WM_QUIT */, IntPtr.Zero, IntPtr.Zero);
    }

    static string callVerb, callArgs, callResult;
    static readonly object callLock = new object();

    /// Mailbox commands "clip ...", "ime ..." and "type ..." (Helper.Execute); run on the window's thread.
    public static string Command(string verb, string args)
    {
        if (verb == "type") return Type(args);  // posts to the focused window: any thread will do
        if (hwnd == IntPtr.Zero) Start();
        lock (callLock)
        {
            callVerb = verb; callArgs = args; callResult = "no window";
            SendMessage(hwnd, MSG_CALL, IntPtr.Zero, IntPtr.Zero);
            return callResult;
        }
    }

    static string Run(string verb, string args)
    {
        if (verb == "clip")
        {
            if (args == "") return SetFromApp();
            if (args.StartsWith("watch ")) { SetClipWatch(args.EndsWith("1")); return "ok"; }
            if (args == "get") { string t = ReadClip(); return t == null ? "busy" : "len=" + t.Length + " " + Escape(t, 200); }
            return "bad args";
        }
        if (verb == "ime")
        {
            if (args.StartsWith("watch ")) { SetImeWatch(args.EndsWith("1")); return "ok"; }
            if (args == "close") { CloseFromApp(); return "ok"; }
            if (args == "diag")
            {
                IntPtr w = SipWindow();
                return "on=" + SipOn() + " wnd=" + w.ToInt32().ToString("x") + " visible=" + (w != IntPtr.Zero && IsWindowVisible(w)) +
                    " watch=" + imeWatch + " open=" + imeOpen;
            }
            if (args == "show") { SipShowIM(SIPF_ON); return "ok"; }  // diagnostics: what a text box does
            return "bad args";
        }
        return "unknown";
    }

    // ---------------------------------------------------------------------------------------
    // Clipboard

    static void SetClipWatch(bool on)
    {
        if (on && !clipWatch)
        {
            // Whatever WM holds now is not news: only later changes go to the phone.
            string t = ReadClip();
            if (t != null) { clipHash = Hash(t); clipKnown = true; }
        }
        clipWatch = on;
    }

    static void ClipPoll()
    {
        if (!clipWatch) return;
        string t = ReadClip();
        if (t == null) return;  // open elsewhere: next time
        uint h = Hash(t);
        if (clipKnown && h == clipHash) return;
        clipHash = h;
        clipKnown = true;
        if (t.Length == 0) return;  // emptied: keep the phone's clipboard
        string path = Path.Combine(Helper.Box, "clip_out.txt"), tmp = path + ".tmp";
        using (var w = new StreamWriter(tmp, false, new UTF8Encoding(false))) w.Write(t.Replace("\r\n", "\n"));
        if (File.Exists(path)) File.Delete(path);
        File.Move(tmp, path);
        Helper.Send("clip");
    }

    static string SetFromApp()
    {
        string text;
        using (var r = new StreamReader(Path.Combine(Helper.Box, "clip_in.txt"), Encoding.UTF8)) text = r.ReadToEnd();
        if (text.Length > MaxClip) text = text.Substring(0, MaxClip);
        // Windows line ends for WM's edit boxes.
        text = text.Replace("\r\n", "\n").Replace("\n", "\r\n");
        if (!SetClip(text)) return "failed " + Marshal.GetLastWin32Error();
        clipHash = Hash(text);  // so the poll does not send it back
        clipKnown = true;
        return "ok";
    }

    static bool Open()
    {
        for (int i = 0; i < 10; i++)
        {
            if (OpenClipboard(hwnd)) return true;
            Thread.Sleep(20);
        }
        return false;
    }

    /// The WM clipboard's text ("" if it has none), or null if it could not be opened.
    static string ReadClip()
    {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return "";
        if (!Open()) return null;
        try
        {
            IntPtr h = GetClipboardData(CF_UNICODETEXT);
            if (h == IntPtr.Zero) return "";
            string t = Marshal.PtrToStringUni(h);
            return t.Length > MaxClip ? t.Substring(0, MaxClip) : t;
        }
        finally { CloseClipboard(); }
    }

    static bool SetClip(string text)
    {
        IntPtr mem = LocalAlloc(0 /* LMEM_FIXED */, (text.Length + 1) * 2);
        if (mem == IntPtr.Zero) return false;
        Marshal.Copy(text.ToCharArray(), 0, mem, text.Length);
        Marshal.WriteInt16(mem, text.Length * 2, 0);
        if (!Open()) { LocalFree(mem); return false; }
        bool ok;
        try
        {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, mem) != IntPtr.Zero;  // the clipboard owns mem now
            if (!ok) LocalFree(mem);
        }
        finally { CloseClipboard(); }
        return ok;
    }

    static uint Hash(string s)
    {
        uint h = 2166136261;
        foreach (char c in s) { h ^= c; h *= 16777619; }
        return h;
    }

    static string Escape(string s, int max)
    {
        if (s.Length > max) s = s.Substring(0, max) + "...";
        return s.Replace("\r", "\\r").Replace("\n", "\\n");
    }

    // ---------------------------------------------------------------------------------------
    // Phone keyboard instead of WM's SIP

    static IntPtr SipWindow() { return FindWindow("SipWndClass", null); }

    /// Drops the blank keyboard this project briefly installed (wmdime.dll). It is not a keyboard anyone picks.
    public static void RemoveBlank()
    {
        byte[] ours = new Guid("{C3A91E40-7B2D-4F58-9A16-5E0D8C4B71A2}").ToByteArray();
        try
        {
            byte[] cur = new byte[16];
            if (SipGetCurrentIM(cur))
            {
                bool match = true;
                for (int i = 0; i < 16; i++) if (cur[i] != ours[i]) { match = false; break; }
                if (match)
                {
                    byte[] prev = null;
                    using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\WMDroid\Ime"))
                        if (k != null) prev = k.GetValue("Prev") as byte[];
                    if (prev == null || prev.Length != 16)
                        prev = new Guid("{42429667-ae04-11d0-a4f8-00aa00a749b9}").ToByteArray();
                    SipSetCurrentIM(prev);
                }
            }
        }
        catch (Exception e) { Helper.Log("ime " + e.Message); }
        try { Registry.CurrentUser.DeleteSubKeyTree(@"CLSID\{C3A91E40-7B2D-4F58-9A16-5E0D8C4B71A2}"); } catch { }
        try { Registry.ClassesRoot.DeleteSubKeyTree(@"CLSID\{C3A91E40-7B2D-4F58-9A16-5E0D8C4B71A2}"); } catch { }
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"ControlPanel\Sip\Installed", true))
                if (k != null) k.DeleteValue("{C3A91E40-7B2D-4F58-9A16-5E0D8C4B71A2}", false);
        }
        catch { }
        try { Registry.CurrentUser.DeleteSubKeyTree(@"Software\WMDroid\Ime"); } catch { }
        try { if (File.Exists(@"\Windows\wmdime.dll")) File.Delete(@"\Windows\wmdime.dll"); } catch { }
    }

    static bool SipOn()
    {
        var si = new SIPINFO();
        si.cbSize = Marshal.SizeOf(typeof(SIPINFO));
        return SipGetInfo(ref si) && (si.fdwFlags & SIPF_ON) != 0;
    }

    static void SetImeWatch(bool on)
    {
        if (on == imeWatch) return;
        imeWatch = on;
        if (on)
        {
            SetTimer(hwnd, TIMER_SIP, SIP_MS, IntPtr.Zero);
            SipPoll();
            return;
        }
        KillTimer(hwnd, TIMER_SIP);
        SetOpen(false);
        // Off (setting off, or the app left the screen): WM's own keyboard is back where it was.
        IntPtr w = SipWindow();
        if (sipHiddenByUs && w != IntPtr.Zero && SipOn()) ShowWindow(w, SW_SHOWNA);
        sipHiddenByUs = false;
    }

    /// The phone's keyboard is (thought to be) open: typed text is read every TYPE_MS meanwhile.
    static void SetOpen(bool open)
    {
        if (open == imeOpen) return;
        if (!open) TypePoll();  // what was typed before it closed
        imeOpen = open;
        if (imeWatch) SetTimer(hwnd, TIMER_SIP, open ? TYPE_MS : SIP_MS, IntPtr.Zero);
    }

    static void SipPoll()
    {
        if (!imeWatch) return;
        if (imeOpen) TypePoll();
        bool on = SipOn();
        if (on && Environment.TickCount - quietUntil < 0)
        {
            SipShowIM(SIPF_OFF);  // the phone's keyboard was just closed: keep WM's down too
            return;
        }
        IntPtr w = SipWindow();
        if (on)
        {
            if (w != IntPtr.Zero)
            {
                if (IsWindowVisible(w)) { ShowWindow(w, SW_HIDE); sipHiddenByUs = true; }
            }
            else SipShowIM(SIPF_OFF);  // no SIP window to hide: lower it outright
            if (!imeOpen) { SetOpen(true); Helper.Send("ime open"); }
        }
        else if (imeOpen && w != IntPtr.Zero)
        {
            // WM lowered its keyboard (focus moved on, or the SIP button): the phone's goes too.
            SetOpen(false);
            sipHiddenByUs = false;
            Helper.Send("ime hide");
        }
    }

    static void CloseFromApp()
    {
        SetOpen(false);
        quietUntil = Environment.TickCount + 800;
        // Lowering the SIP hides its window anyway; the next SipShowIM(on) shows it again.
        if (SipOn()) SipShowIM(SIPF_OFF);
        sipHiddenByUs = false;
    }

    // Typing from the phone's keyboard. Tokens (hex, space separated): a UTF-16 code unit is typed as
    // a character (WM_CHAR); 1xxxx presses virtual key xxxx (Back, Enter, Tab, arrows...).
    // This goes the way a SIP input method types (PostKeybdMessage), not through the emulated
    // keyboard: WM lowers its SIP on hardware key presses, which would close the phone's keyboard
    // after every key.
    static string Type(string args)
    {
        int n = 0;
        foreach (string p in args.Split(' ', '\r', '\n'))
        {
            if (p.Length == 0) continue;
            uint v;
            try { v = Convert.ToUInt32(p, 16); } catch { return "bad args"; }
            if (v >= 0x10000) PressKey(v & 0xFFFF);
            else
            {
                uint[] ch = { v }, st = { KeyStateDownFlag };
                PostKeybdMessage(new IntPtr(-1), 0, KeyStateDownFlag, 1, st, ch);
                st[0] = KeyStatePrevDownFlag | KeyShiftNoCharacterFlag;
                PostKeybdMessage(new IntPtr(-1), 0, st[0], 1, st, ch);
            }
            n++;
        }
        return "ok " + n;
    }

    static void PressKey(uint vk)
    {
        uint c = vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B ? vk : 0;  // keys that also type a character
        uint[] ch = { c }, st = { KeyStateDownFlag };
        PostKeybdMessage(new IntPtr(-1), vk, KeyStateDownFlag, c != 0 ? 1u : 0u, st, ch);
        st[0] = KeyStatePrevDownFlag | KeyShiftNoCharacterFlag;
        PostKeybdMessage(new IntPtr(-1), vk, st[0], c != 0 ? 1u : 0u, st, ch);
    }

    // The app writes what the phone's keyboard typed as type\<seq>.txt (Type's tokens); read quickly
    // while the phone's keyboard is open (the mailbox's in\ is only read twice a second).
    static void TypePoll()
    {
        string dir = Path.Combine(Helper.Box, "type");
        if (!Directory.Exists(dir)) return;
        string[] files = Directory.GetFiles(dir, "*.txt");
        if (files.Length == 0) return;
        Array.Sort(files);
        foreach (string f in files)
        {
            string text;
            using (var r = new StreamReader(f)) text = r.ReadToEnd();
            File.Delete(f);
            Type(text);
        }
    }

    const uint KeyStateDownFlag = 0x0080, KeyStatePrevDownFlag = 0x0040, KeyShiftNoCharacterFlag = 0x00010000;

    // ---------------------------------------------------------------------------------------

    [StructLayout(LayoutKind.Sequential)]
    struct SIPINFO
    {
        public int cbSize, fdwFlags;
        public int vdLeft, vdTop, vdRight, vdBottom;    // rcVisibleDesktop
        public int sipLeft, sipTop, sipRight, sipBottom; // rcSipRect
        public int dwImDataSize;
        public IntPtr pvImData;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public int message; public IntPtr wParam, lParam; public int time, x, y; }

    [DllImport("coredll.dll")] static extern bool SipGetInfo(ref SIPINFO si);
    [DllImport("coredll.dll")] static extern bool SipGetCurrentIM(byte[] clsid);
    [DllImport("coredll.dll")] static extern bool SipSetCurrentIM(byte[] clsid);
    [DllImport("coredll.dll")] static extern bool SipShowIM(int flags);
    [DllImport("coredll.dll")] static extern IntPtr FindWindow(string cls, string title);
    [DllImport("coredll.dll")] static extern bool IsWindowVisible(IntPtr w);
    [DllImport("coredll.dll")] static extern bool ShowWindow(IntPtr w, int cmd);
    [DllImport("coredll.dll")] static extern IntPtr SetTimer(IntPtr w, int id, int ms, IntPtr proc);
    [DllImport("coredll.dll")] static extern bool KillTimer(IntPtr w, int id);
    [DllImport("coredll.dll")] static extern bool GetMessage(out MSG m, IntPtr w, int min, int max);
    [DllImport("coredll.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("coredll.dll")] static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("coredll.dll")] static extern bool PostMessage(IntPtr w, int msg, IntPtr wp, IntPtr lp);
    [DllImport("coredll.dll")] static extern IntPtr SendMessage(IntPtr w, int msg, IntPtr wp, IntPtr lp);
    [DllImport("coredll.dll", SetLastError = true)] static extern bool OpenClipboard(IntPtr owner);
    [DllImport("coredll.dll")] static extern bool CloseClipboard();
    [DllImport("coredll.dll")] static extern bool EmptyClipboard();
    [DllImport("coredll.dll")] static extern bool IsClipboardFormatAvailable(uint format);
    [DllImport("coredll.dll")] static extern IntPtr GetClipboardData(uint format);
    [DllImport("coredll.dll", SetLastError = true)] static extern IntPtr SetClipboardData(uint format, IntPtr mem);
    [DllImport("coredll.dll")] static extern IntPtr LocalAlloc(int flags, int bytes);
    [DllImport("coredll.dll")] static extern IntPtr LocalFree(IntPtr mem);
    [DllImport("coredll.dll")]
    static extern bool PostKeybdMessage(IntPtr hwnd, uint vk, uint flags, uint count, uint[] shiftStates, uint[] chars);
}
