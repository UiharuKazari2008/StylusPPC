using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

// Windows Mobile's hardware Buttons 1-4 (Settings > Personal > Buttons): what the helper still does
// for them. The buttons themselves need no helper: the app sends each one as a key the stock image
// already understands (Keymap.HwButtons): the action's own key where it has one (Start = Win, the
// soft keys = F1/F2, scrolling = the arrows), else Win + the VK_APPn hotkey of a button whose
// assignment in WM is that action (the stock image: Button 2 Rotate, 3 Context Menu, 4 OK/Close).
//
// How WM 6.x stores an assignment (read from cplmain.cpl's Buttons page and shell32.exe):
//   HKLM\Software\Microsoft\Shell\Keys\40Cn  (40 = MOD_WIN, Cn = VK_APPn)
//     Flags, low nibble: the special action (ActionNames below, cplmain's table), 0 = run a program
//     (default value) = "\"program\" args" for a program, "" for an action
//   cplmain keeps the other Flags bits, then releases the key (UnregisterFunc1(MOD_WIN, vk),
//   SHSetAppKeyWndAssoc(vk, NULL)) and posts WM_SETTINGCHANGE(0xE4) to HWND_BROADCAST, on which
//   the shell re-reads the keys.
//
// The helper writes an assignment only for a choice no stock key reaches (Home, Input Panel, an
// action that no button holds any more, WM's own program on Button 1): there WM itself must store
// the action, because only the shell's hotkey performs it. Button 1 is the special case: the board
// keyboard driver (kbdmouse.dll) hard-codes VK 0xC1 = VK_APP1 as its Fn key (both matrix codes that
// give 0xC1, 0x21 and 0x64, are swallowed as Fn, and no Fn or NumLock combination produces it), so
// Win+VK_APP1 can never be pressed and WM's Button 1 (40C1) is unreachable on the stock image. When
// Button 1 needs a stored action, its key moves to 40C5 (VK_APP5, matrix code 0x68), once.
//
// Commands (Helper.Execute):
//   buttons                         WM's assignments, also sent as the "buttons ..." message (Report)
//   buttons sync 1=<a> 2=<a> ...    store these choices (the ones the app has no stock route for;
//                                   "wm" on Button 1 = make WM's own Button 1 pressable). Each one is
//                                   stored only when it differs from the choice stored last time
//                                   (HKCU\Software\WMDroid\Buttons), so changes made in WM's own
//                                   Buttons page stay until the user changes the choice on the phone.
//   button <n> <action>             set Button n to an action (ActionNames), diagnostics
//   button <n> prog <path> [args]   set Button n to a program, diagnostics
//   button <n> press                press Button n's hotkey from inside WM (diagnostics)
static class Buttons
{
    const string KeysRoot = @"Software\Microsoft\Shell\Keys\";
    const string StateKey = @"Software\WMDroid\Buttons";
    const int First = 1, Last = 4;

    // Index = the Flags code (cplmain's table pairs 1,2,4..0xE with the labels; 3 is unused).
    static readonly string[] ActionNames = {
        null, "start", "inputpanel", null, "home", "scrollup", "scrolldown", "scrollleft", "scrollright",
        "none", "okclose", "contextmenu", "rotate", "leftsoft", "rightsoft" };

    static string SlotPath(int vk) { return KeysRoot + "40" + vk.ToString("X2"); }
    static bool Exists(int vk)
    {
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(SlotPath(vk))) return k != null;
    }

    // Where WM keeps Button n: 40Cn; Button 1 in 40C5 once it has been moved there (Move1).
    static int Vk(int n) { return n == 1 && !Exists(0xC1) && Exists(0xC5) ? 0xC5 : 0xC0 + n; }

    public static string Command(string verb, string rest)
    {
        try
        {
            if (verb == "buttons")
            {
                if (rest.StartsWith("sync")) return Sync(rest.Substring(4).Trim());
                return Report();
            }
            // button <n> ...
            int sp = rest.IndexOf(' ');
            int b;
            if (sp < 0 || !TryButton(rest.Substring(0, sp), out b)) return "bad args";
            string what = rest.Substring(sp + 1).Trim();
            if (what == "press") { Press(Vk(b)); return "ok"; }
            if (b == 1) Move1();
            string err = Set(Vk(b), what);
            if (err != null) return err;
            Notify(Vk(b));
            Report();
            return "ok " + Describe(Vk(b));
        }
        catch (Exception e) { return "failed " + e.Message; }
    }

    // "buttons 1=start 2=rotate 3=contextmenu 4=okclose 5=-": the assignment of each hotkey slot
    // Win+VK_APP1..5 (40C1..40C5; "-" = none, "prog" = a program), for the app's routing
    // (Keymap.HwButtons). Sent to the app too, so it knows them while the helper is gone.
    public static string Report()
    {
        var sb = new StringBuilder();
        for (int slot = 1; slot <= 5; slot++)
        {
            if (sb.Length > 0) sb.Append(' ');
            string d = Describe(0xC0 + slot);
            sb.Append(slot).Append('=').Append(d.StartsWith("prog:") ? "prog" : d);
        }
        string map = sb.ToString();
        Helper.Send("buttons " + map);
        return map;
    }

    static bool TryButton(string s, out int n)
    {
        n = 0;
        try { n = int.Parse(s); } catch { return false; }
        return n >= First && n <= Last;
    }

    // "start" etc., "prog:<command line>", "flags:<n>" for a code cplmain doesn't offer, "-" if absent.
    static string Describe(int vk)
    {
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(SlotPath(vk)))
        {
            if (k == null) return "-";
            int code = Flags(k) & 0xF;
            if (code == 0) return "prog:" + (k.GetValue("") as string ?? "");
            return code < ActionNames.Length && ActionNames[code] != null ? ActionNames[code] : "flags:" + code;
        }
    }

    static int Flags(RegistryKey k)
    {
        object v = k.GetValue("Flags");
        return v is int ? (int)v : 0;
    }

    // Button 1 onto VK_APP5: copies 40C1 (Name "Button 1", Icon, Flags, ResetFlags, program) to 40C5
    // unless 40C5 exists, and deletes 40C1 so WM's Buttons page lists Button 1-4 once (sorted by key:
    // Button 2, 3, 4, then 1). Only done when Button 1 needs a stored action (see the top). A hard
    // reset brings 40C1 back.
    static void Move1()
    {
        using (RegistryKey old = Registry.LocalMachine.OpenSubKey(SlotPath(0xC1)))
        {
            if (old == null) return;
            if (!Exists(0xC5))
                using (RegistryKey k = Registry.LocalMachine.CreateSubKey(SlotPath(0xC5)))
                    foreach (string name in old.GetValueNames()) k.SetValue(name, old.GetValue(name));
        }
        Registry.LocalMachine.DeleteSubKey(SlotPath(0xC1), false);
        try { UnregisterFunc1(8 /* MOD_WIN */, 0xC1); } catch { }
        Notify(0xC5);
        Helper.Log("buttons: Button 1 moved to 40C5");
    }

    // Writes an assignment like cplmain does; null on success, else the error to report.
    static string Set(int vk, string what)
    {
        int code = 0;
        string command = "";
        if (what.StartsWith("prog "))
        {
            string path = what.Substring(5).Trim();
            if (path.Length == 0) return "bad args";
            command = path.StartsWith("\"") ? path : "\"" + path + "\"";
        }
        else
        {
            code = Array.IndexOf(ActionNames, what);
            if (code <= 0) return "unknown action " + what;
        }
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(SlotPath(vk)))
        {
            k.SetValue("Flags", (Flags(k) & ~0xF) | code);
            k.SetValue("", command);
        }
        return null;
    }

    // Releases the key from whoever holds it and tells the shell to re-read the assignments.
    static void Notify(int vk)
    {
        try { UnregisterFunc1(8 /* MOD_WIN */, vk); } catch { }
        try { SHSetAppKeyWndAssoc((byte)vk, IntPtr.Zero); } catch { }
        PostMessage((IntPtr)0xFFFF, 0x1A /* WM_SETTINGCHANGE */, 0xE4, 0);
    }

    // "button <n> press": presses a hotkey's Win+VK inside Windows Mobile (for testing).
    static void Press(int vk)
    {
        keybd_event(0x5B /* VK_LWIN */, 0, 0, 0);
        keybd_event((byte)vk, 0, 0, 0);
        keybd_event((byte)vk, 0, 2 /* KEYEVENTF_KEYUP */, 0);
        keybd_event(0x5B, 0, 2, 0);
    }

    static string Sync(string args)
    {
        var done = new StringBuilder();
        using (RegistryKey state = Registry.CurrentUser.CreateSubKey(StateKey))
        {
            foreach (string pair in args.Split(' '))
            {
                int eq = pair.IndexOf('=');
                int n;
                if (eq <= 0 || !TryButton(pair.Substring(0, eq), out n)) continue;
                string want = pair.Substring(eq + 1);
                if (want == (state.GetValue(n.ToString()) as string)) continue;  // stored before: WM's own setting may have changed since
                if (n == 1) Move1();
                if (want != "wm")
                {
                    string err = Set(Vk(n), want);
                    if (err != null) { done.Append(' ').Append(n).Append(':').Append(err); continue; }
                    Notify(Vk(n));
                }
                state.SetValue(n.ToString(), want);
                done.Append(' ').Append(n).Append('=').Append(want);
            }
        }
        Report();
        return done.Length == 0 ? "ok unchanged" : "ok" + done;
    }

    [DllImport("coredll.dll")] static extern bool PostMessage(IntPtr hwnd, int msg, int wParam, int lParam);
    [DllImport("coredll.dll")] static extern bool UnregisterFunc1(int modifiers, int vk);
    [DllImport("aygshell.dll")] static extern bool SHSetAppKeyWndAssoc(byte vk, IntPtr hwnd);
    [DllImport("coredll.dll")] static extern void keybd_event(byte vk, byte scan, int flags, int extra);
}
