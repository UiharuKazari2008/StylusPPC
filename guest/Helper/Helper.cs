using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// WMDroid guest helper. The Android app serves it on every \Storage Card as 2577\helper.exe, next
// to 2577\autorun.exe, a native bootstrap (guest/Autorun) the shell runs ("install") whenever the
// card mounts: it installs .NET CF 3.5 if the image lacks it, then starts this program with the
// same arguments ("autorun.exe update" below reaches "update" here the same way). It then
//  - installs itself to \Program Files\WMDroid, adds a Start menu entry, a StartUp shortcut and a
//    Settings > System entry,
//  - runs a mailbox loop with the app through \Storage Card\.wmdroid (in\*.cmd from the app,
//    out\*.msg to the app),
//  - shows the WMDroid settings page when started without arguments.
static class Helper
{
    const string MainCard = @"\Storage Card";
    // The card WM mounted the share as: \Storage Card, unless that name was still taken when the
    // card came back (a stray folder in RAM, a program holding the old card): then "Storage Card2".
    static string card = MainCard;
    static string Card { get { return card; } }
    internal static string Box { get { return card + @"\.wmdroid"; } }
    const string InstallDir = @"\Program Files\PocketStylus";
    const string InstalledExe = Patch.SystemExe;
    // The settings/storage pages and "send" run from their own copy: the Compact Framework runs one
    // instance per program file, so with one file an open settings page stopped the helper from
    // starting (the start only brought the page forward) and vice versa.
    const string UiExe = @"\Windows\PocketStylus.exe";

    static bool quit;

    // Who is running: the app compares the size with its own copy and updates an older helper.
    static string Hello()
    {
        long size = 0;
        uint sum = 0;
        try
        {
            byte[] b;
            using (var f = File.OpenRead(Self)) { b = new byte[f.Length]; f.Read(b, 0, b.Length); }
            size = b.Length;
            sum = Fnv(b);
        }
        catch { }
        return "hello " + Environment.OSVersion.Version + " " + Screen.PrimaryScreen.Bounds.Width + "x" +
            Screen.PrimaryScreen.Bounds.Height + " size=" + size + " sum=" + sum.ToString("x8");
    }

    // FNV-1a over the helper's own file, so the app can tell builds of equal size apart.
    static uint Fnv(byte[] b)
    {
        uint h = 2166136261;
        foreach (byte x in b) { h ^= x; h *= 16777619; }
        return h;
    }

    static string Self { get { return System.Reflection.Assembly.GetExecutingAssembly().GetName().CodeBase; } }

    // Started from the card while \Windows holds a different helper build.
    static bool OtherBuildInstalled()
    {
        try
        {
            if (string.Compare(Self, InstalledExe, true) == 0 || !File.Exists(InstalledExe)) return false;
            byte[] a, b;
            using (var f = File.OpenRead(Self)) { a = new byte[f.Length]; f.Read(a, 0, a.Length); }
            using (var f = File.OpenRead(InstalledExe)) { b = new byte[f.Length]; f.Read(b, 0, b.Length); }
            if (a.Length != b.Length) return true;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return true;
            return false;
        }
        catch (Exception) { return false; }
    }

    [MTAThread]
    static void Main(string[] args)
    {
        string mode = args.Length > 0 ? args[0].ToLower() : "";
        // The Compact Framework starts every program with the wait cursor on its main thread and
        // takes it away when the first form shows. The helper's own modes show no form, and WM 5
        // then keeps drawing the spinning wait cursor over the Today screen for as long as the
        // helper runs (from the first boot on, so also in every saved state).
        if (mode == "serve" || mode == "install" || mode == "update" || mode == "uninstall") Cursor.Current = Cursors.Default;
        // Started from the card (install, update): that card, whatever name WM gave it.
        try
        {
            string d = Path.GetDirectoryName(Self);
            if (string.Compare(Path.GetFileName(d), "2577", true) == 0) card = Path.GetDirectoryName(d);
        }
        catch (Exception) { }
        try
        {
            // The card always carries the app's current helper. An installed helper of another build
            // (e.g. an older one stuck where the mailbox can't reach it, so the app's "update" never
            // arrives) is replaced the way an update does it.
            if (mode == "install" && OtherBuildInstalled()) { Log("install: replacing the installed helper"); mode = "update"; }
            if (mode == "install") { Install(); Process.Start(InstalledExe, "serve"); return; }
            if (mode == "uninstall") return;
            if (mode == "serve")
            {
                Serve(args.Length > 1 && args[1] == "takeover");
                // Leaving for an update: end the process even if a thread lingers (RIL, camera), or
                // it keeps the mutex and its file and the new helper can neither install nor serve.
                if (quit) Process.GetCurrentProcess().Kill();
                return;
            }
            if (mode == "send")
            {
                // Send via Bluetooth (Bluetooth.cs): the given file, else one picked here.
                if (args.Length < 2) { Application.Run(new SendForm()); return; }
                string error = Bluetooth.Send(args[1]);
                if (error != null) MessageBox.Show(error, "Send via Bluetooth");
                return;
            }
            if (mode == "storage") { Application.Run(new StorageForm()); return; }  // the Storage Card page alone
            if (mode == "update")
            {
                // Started from the card by the old helper, which is exiting: replace it. Whatever
                // goes wrong installing, a helper must keep serving (else nothing reaches WM until
                // a reset): the installed copy if it is there, else this one from the card.
                // Older helpers don't always exit: if it still holds the mutex, end it.
                if (!WaitHelperGone(15000)) Log("update: old helper still running; ending it");
                // Also ends an older settings page running from the helper's file, which would
                // keep the new helper from starting (one instance per program file).
                EndOtherHelpers();
                WaitHelperGone(5000);
                try { Install(); } catch (Exception e) { Log("update install " + e.Message); }
                try { Process.Start(File.Exists(InstalledExe) ? InstalledExe : Self, "serve takeover"); }
                catch (Exception e)
                {
                    Log("update start " + e.Message);
                    Process.Start(Self, "serve takeover");
                }
                return;
            }
            Application.Run(new SettingsForm());
        }
        catch (Exception e) { Log("fatal " + e.Message); }
    }

    // True once no helper holds the mailbox mutex (waits up to ms).
    static bool WaitHelperGone(int ms)
    {
        int end = Environment.TickCount + ms;
        while (true)
        {
            IntPtr m = CreateMutex(IntPtr.Zero, false, "WMDroidHelper");
            bool held = Marshal.GetLastWin32Error() == 183;
            CloseHandle(m);
            if (!held) return true;
            if (Environment.TickCount - end > 0) return false;
            Thread.Sleep(500);
        }
    }

    // Ends every other process running the installed helper's file (a stuck or lingering helper).
    static void EndOtherHelpers()
    {
        string name = Path.GetFileName(InstalledExe);
        int self = Process.GetCurrentProcess().Id;
        IntPtr snap = CreateToolhelp32Snapshot(2 /* TH32CS_SNAPPROCESS */ | 0x40000000 /* SNAPNOHEAPS */, 0);
        if (snap == new IntPtr(-1)) return;
        try
        {
            // PROCESSENTRY32: nine DWORDs, WCHAR szExeFile[260], th32MemoryBase, th32AccessKey.
            byte[] pe = new byte[564];
            BitConverter.GetBytes(pe.Length).CopyTo(pe, 0);
            for (bool ok = Process32First(snap, pe); ok; ok = Process32Next(snap, pe))
            {
                int pid = BitConverter.ToInt32(pe, 8);
                string exe = Encoding.Unicode.GetString(pe, 36, 520);
                int nul = exe.IndexOf((char)0);
                if (nul >= 0) exe = exe.Substring(0, nul);
                if (pid == self || string.Compare(Path.GetFileName(exe), name, true) != 0) continue;
                IntPtr h = OpenProcess(0, false, pid);
                if (h == IntPtr.Zero) continue;
                Log("ending helper process " + pid.ToString("x"));
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
        }
        finally { CloseToolhelp32Snapshot(snap); }
    }

    // "mem": the helper's managed heap, the device's free RAM, this process's threads and the
    // number of processes, to spot leaks over long runs.
    static string Mem()
    {
        var ms = new MEMORYSTATUS();
        ms.length = Marshal.SizeOf(ms);
        GlobalMemoryStatus(ref ms);
        int threads = 0, procs = 0, self = Process.GetCurrentProcess().Id;
        IntPtr snap = CreateToolhelp32Snapshot(2 | 4 /* processes, threads */ | 0x40000000 /* no heaps */, 0);
        if (snap != new IntPtr(-1))
        {
            try
            {
                byte[] pe = new byte[564];
                BitConverter.GetBytes(pe.Length).CopyTo(pe, 0);
                for (bool ok = Process32First(snap, pe); ok; ok = Process32Next(snap, pe)) procs++;
                byte[] te = new byte[36];  // THREADENTRY32: owner process at +12
                BitConverter.GetBytes(te.Length).CopyTo(te, 0);
                for (bool ok = Thread32First(snap, te); ok; ok = Thread32Next(snap, te))
                    if (BitConverter.ToInt32(te, 12) == self) threads++;
            }
            finally { CloseToolhelp32Snapshot(snap); }
        }
        return "gc=" + GC.GetTotalMemory(false) / 1024 + "K free=" + ms.availPhys / 1024 + "K load=" + ms.memoryLoad +
            "% threads=" + threads + " procs=" + procs;
    }

    struct MEMORYSTATUS { public int length, memoryLoad; public uint totalPhys, availPhys, totalPageFile, availPageFile, totalVirtual, availVirtual; }
    [DllImport("coredll.dll")] static extern void GlobalMemoryStatus(ref MEMORYSTATUS ms);
    [DllImport("toolhelp.dll")] static extern bool Thread32First(IntPtr snap, byte[] te);
    [DllImport("toolhelp.dll")] static extern bool Thread32Next(IntPtr snap, byte[] te);
    [DllImport("toolhelp.dll")] static extern IntPtr CreateToolhelp32Snapshot(int flags, int pid);
    [DllImport("toolhelp.dll")] static extern bool CloseToolhelp32Snapshot(IntPtr snap);
    [DllImport("toolhelp.dll")] static extern bool Process32First(IntPtr snap, byte[] pe);
    [DllImport("toolhelp.dll")] static extern bool Process32Next(IntPtr snap, byte[] pe);
    [DllImport("coredll.dll")] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("coredll.dll")] static extern bool TerminateProcess(IntPtr h, int code);

    // ---------------------------------------------------------------------------------------
    // Installation

    // Copies a program from the card's 2577 folder to \Windows; returns its path, or null.
    static string CopyProgram(string name) { return CopyProgram(name, false); }

    static string CopyProgram(string name, bool onlyIfInstalled)
    {
        string src = Path.Combine(Card + @"\2577", name), dst = Path.Combine(@"\Windows", name);
        try
        {
            if (!File.Exists(src) || (onlyIfInstalled && !File.Exists(dst))) return File.Exists(dst) ? dst : null;
            File.Copy(src, dst, true);
            return dst;
        }
        catch (Exception) { return File.Exists(dst) ? dst : null; }  // in use: keep the old one
    }

    static void Install()
    {
        if (string.Compare(Self, InstalledExe, true) != 0)
        {
            try { File.Copy(Self, InstalledExe, true); }
            catch (IOException) { } // already running from there
        }
        try { File.Copy(Self, UiExe, true); }
        catch (Exception) { }  // a settings page is open: it keeps the old copy until next time
        string ui = File.Exists(UiExe) ? UiExe : InstalledExe;
        Shortcut(@"\Windows\Start Menu\Programs\PocketStylus.lnk", "\"" + ui + "\"");
        Shortcut(@"\Windows\StartUp\PocketStylus.lnk", "\"" + InstalledExe + "\" serve");
        // The other programs come with every helper update (new icons and fixes reach them too).
        string send = CopyProgram("SendBluetooth.exe");
        CopyProgram("GpsTest.exe", true);
        // Text messages on WM 6.5.3 (Sms.cs): MAPIdotnet's native half and its stream types.
        CopyProgram("MAPIlib.dll");
        CopyProgram("OpenNETCF.dll");
        Shortcut(Bluetooth.StartMenuLink, send != null ? "\"" + send + "\"" : "\"" + ui + "\" send");
        // Settings > System entry (WM shows ControlPanel items that redirect to a program).
        // (Renamed WMDroid -> PocketStylus: the old entry and shortcuts go.)
        try { Registry.LocalMachine.DeleteSubKey(@"ControlPanel\WMDroid"); } catch (Exception) { }
        foreach (string old in new string[] { @"\Windows\Start Menu\Programs\WMDroid.lnk", @"\Windows\StartUp\WMDroid.lnk" })
            try { if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"ControlPanel\PocketStylus"))
        {
            k.SetValue("Group", 2);
            k.SetValue("Redirect", ui);
            k.SetValue("Icon", ui + ",0");
        }
    }

    public static void Shortcut(string path, string target)
    {
        try
        {
            string body = target.Length + "#" + target;
            using (var w = new StreamWriter(path, false, Encoding.ASCII)) w.Write(body);
        }
        catch (Exception e) { Log("shortcut " + path + ": " + e.Message); }
    }

    // ---------------------------------------------------------------------------------------
    // Mailbox

    // What the mailbox loop is doing, and when it last moved on (the watchdog below).
    static volatile string stage = "start";
    static volatile int beat = Environment.TickCount;

    static void Step(string what) { stage = what; beat = Environment.TickCount; }

    // ---------------------------------------------------------------------------------------
    // The card. The app takes it out and puts it back (e.g. to restart a silent helper), and the
    // user can switch it off. While it is away nothing may create "\Storage Card\..." (that makes
    // a folder of that name in RAM, and WM then mounts the returning card as "\Storage Card2",
    // where neither the mailbox nor the card's autorun are looked for).

    // A mounted volume, not a folder in RAM: CE marks mount points directory + temporary.
    internal static bool IsMounted(string path)
    {
        uint a = GetFileAttributes(path);
        return a != 0xFFFFFFFF && (a & 0x110) == 0x110;
    }

    static bool cardWasThere = true;
    static int remountAsked;  // TickCount of the last "remount" request (0 = none yet)

    // Once per mailbox pass: where the card is, and what to do when it went or came back.
    static void CheckCard()
    {
        if (!IsMounted(MainCard))
        {
            RemoveStrayCard();
            string found = MainCard;
            try
            {
                foreach (string d in Directory.GetDirectories(@"\"))
                    if (IsMounted(d) && Directory.Exists(Path.Combine(d, ".wmdroid"))) { found = d; break; }
            }
            catch (Exception) { }
            card = found;
        }
        else card = MainCard;
        bool there = IsMounted(card);
        if (there && !cardWasThere)
        {
            Log("card back" + (card != MainCard ? " as " + card : ""));
            Send(Hello());  // the app resends what the helper keeps in memory
        }
        cardWasThere = there;
        if (there && strayRemoved) { strayRemoved = false; Log("removed a stray " + MainCard + " folder from RAM"); }
        // Mounted under another name while \Storage Card is free (the stray folder is gone): the
        // app re-inserts the card, which brings the usual name back. At most every 5 minutes.
        if (there && card != MainCard && GetFileAttributes(MainCard) == 0xFFFFFFFF &&
            (remountAsked == 0 || Environment.TickCount - remountAsked > 300000))
        {
            remountAsked = Environment.TickCount;
            Log("card is " + card + "; asking the app to re-insert it");
            Send("remount");
        }
    }

    // A "\Storage Card" folder in RAM holding nothing but the helper's own .wmdroid (an older
    // helper logged while the card was away): it keeps the card's name taken, so it goes.
    static void RemoveStrayCard()
    {
        uint a = GetFileAttributes(MainCard);
        if (a == 0xFFFFFFFF || (a & 0x100) != 0) return;  // missing, or the card itself
        try
        {
            if (Directory.GetFiles(MainCard).Length > 0) return;
            foreach (string d in Directory.GetDirectories(MainCard))
                if (string.Compare(Path.GetFileName(d), ".wmdroid", true) != 0) return;
            Directory.Delete(MainCard, true);
            strayRemoved = true;
        }
        catch (Exception) { }
    }

    static bool strayRemoved;

    static void Serve(bool takeover)
    {
        IntPtr mutex = CreateMutex(IntPtr.Zero, false, "WMDroidHelper");
        if (Marshal.GetLastWin32Error() == 183)
        {
            if (!takeover) return; // already running
            // Started by a stuck helper that is about to end itself: wait for its mutex to go.
            for (int i = 0; i < 20 && Marshal.GetLastWin32Error() == 183; i++)
            {
                CloseHandle(mutex);
                Thread.Sleep(1000);
                mutex = CreateMutex(IntPtr.Zero, false, "WMDroidHelper");
            }
            if (Marshal.GetLastWin32Error() == 183) { Log("takeover: the old helper still runs"); return; }
        }
        if (takeover) Log("serving (took over)");
        // Watchdog: if one step of the loop blocks (a driver call, a window that stopped answering),
        // the app's commands would wait forever. Log where it stuck, start a fresh helper and end
        // this one; the stuck command was already taken out of the mailbox, so it isn't repeated.
        Step("start");
        var dog = new Thread(delegate ()
        {
            while (!quit)
            {
                Thread.Sleep(5000);
                int stuck = Environment.TickCount - beat;
                if (quit || stuck < 45000) continue;
                Log("stuck " + stuck / 1000 + " s at " + stage + "; restarting");
                Send("stuck " + stage);
                // Through the card's copy, the same way as an update: this file is running (one
                // instance per program file), and the updater waits for this process to end.
                try { Process.Start(Card + @"\2577\autorun.exe", "update"); }
                catch (Exception e) { Log("restart " + e.Message); continue; }
                Process.GetCurrentProcess().Kill();
            }
        });
        dog.IsBackground = true;
        dog.Start();
        try { Patch.NoNetPrompt(); } catch (Exception e) { Log("net " + e.Message); }  // answer netui's network card question first (Patch.cs)
        Send(Hello());
        try { Camera.Install(); } catch (Exception e) { Log("camera " + e.Message); }  // camera passthrough (Camera.cs)
        Standby.Disable();  // WM's own auto-off would stop everything, mailbox included (Standby.cs)
        bool patchChecked = false;
        while (!quit)
        {
            Step("card");
            CheckCard();
            Step("mailbox");
            try
            {
                string dir = Path.Combine(Box, "in");
                if (Directory.Exists(dir))
                {
                    string[] cmds = Directory.GetFiles(dir, "*.cmd");
                    Array.Sort(cmds);
                    foreach (string f in cmds)
                    {
                        string text;
                        using (var r = new StreamReader(f)) text = r.ReadToEnd().Trim();
                        File.Delete(f);
                        Step("cmd " + Path.GetFileNameWithoutExtension(f) + " " + (text.Length > 40 ? text.Substring(0, 40) : text));
                        Send("result " + Path.GetFileNameWithoutExtension(f) + " " + Execute(text));
                    }
                }
            }
            catch (Exception e) { Log("serve " + e.Message); }
            Step("patch");
            if (!patchChecked && !quit)
            {
                // After the first mailbox pass (so an "update" from the app wins): patch the system
                // once per image, which resets the guest; otherwise report readiness.
                patchChecked = true;
                bool irdaPort = Irda.Provision();  // infrared port driver (Irda.cs); the patch restart covers it
                if (Patch.Check()) return;
                if (Dpi.Check()) return;  // Screen DPI setting: one restart when it changed (Dpi.cs)
                if (irdaPort && Irda.Restart()) return;
                try { Patch.EnsureInternet(); } catch (Exception e) { Log("net " + e.Message); }
                Send("ready patch=" + Patch.Installed);
            }
            Step("phone state");
            PhoneState.Apply();
            Step("calls");
            Calls.Poll();  // phone calls from WM's Phone app (Calls.cs)
            Sms.Poll();  // WM 6.5.3: texts WM sends (Sms.cs)
            Step("audio");
            AudioClass.Poll();
            Step("camera");
            try { Camera.Poll(); } catch (Exception e) { Log("camera " + e.Message); }  // portrait sensor: registry in step (Camera.cs)
            Step("idle");
            Thread.Sleep(IsMounted(Card) ? 500 : 3000);
        }
        Calls.Stop();  // close the RIL client, or this process (and its mutex) outlives the update
        InputSync.Stop();  // clipboard/keyboard window's message loop (InputSync.cs)
    }

    // Commands from the app.
    static string Execute(string line)
    {
        string verb = line, rest = "";
        int sp = line.IndexOf(' ');
        if (sp > 0) { verb = line.Substring(0, sp); rest = line.Substring(sp + 1).Trim(); }
        switch (verb.ToLower())
        {
            case "ping": return "pong";
            case "sleep": Thread.Sleep(int.Parse(rest) * 1000); return "ok";  // diagnostics: a stuck command (watchdog)
            case "backlight": return Backlight.Set(rest);
            case "wallpaper": return Wallpaper.Apply(Box);
            case "theme": return Theme.Apply(Box, rest);
            case "rm":
                try { File.Delete(rest); return "ok"; }
                catch (Exception e) { return e.GetType().Name + " " + e.Message + " attr=" + (File.Exists(rest) ? new FileInfo(rest).Attributes.ToString() : "missing"); }
            case "ls": return string.Join(" | ", Directory.GetFiles(rest.Length > 0 ? rest : Box));
            case "df":
            {
                // df <path>: free / total bytes as Windows Mobile sees them (diagnostics)
                ulong avail, total, free;
                if (!GetDiskFreeSpaceEx(rest.Length > 0 ? rest : Card, out avail, out total, out free)) return "failed";
                return "free=" + avail + " total=" + total;
            }
            case "hello":
                Send(Hello());
                AudioClass.Resend();
                if (Dpi.Recheck()) { quit = true; return "resetting"; }  // a new Screen DPI choice (Dpi.cs)
                return "ok";
            case "dpi": return "live=" + Dpi.Live() + " want=" + Get("dpi", "0");  // diagnostics
            case "card": return card + " attr=" + GetFileAttributes(card).ToString("x") + " main=" + GetFileAttributes(MainCard).ToString("x");  // diagnostics
            case "mem": return Mem();  // diagnostics: memory, threads, processes (long-run checks)
            case "play": return PlaySound(rest, IntPtr.Zero, 0x20001) ? "ok" : "failed";  // SND_FILENAME | SND_ASYNC
            case "patch": return Patch.Check(true) ? "resetting" : "failed";
            case "provision": return Patch.Provision(rest);
            case "update":
            {
                // The app has a newer helper on the card: hand over to it and exit.
                Process.Start(Card + @"\2577\autorun.exe", "update");
                quit = true;
                return "ok";
            }
            case "tz": return SetTimeZone(rest);
            case "status": return PhoneState.Set(rest);
            case "lock": return PhoneState.Lock();
            case "call": return Calls.Command(rest);  // call state <id> <state> (Calls.cs)
            case "pim": return Pim.Command(rest);  // contacts and calendar sync: pim dump / pim apply (Pim.cs)
            case "sms": return Sms.Command(rest);  // text messages (Sms.cs)
            case "number": return OwnNumber.Command(rest);  // the device's own phone number (OwnNumber.cs)
            case "received": return Bluetooth.Received(rest);
            case "irda": return Irda.Command(rest);  // infrared transport choice (Irda.cs)
            case "clip": case "ime": case "type": return InputSync.Command(verb.ToLower(), rest);  // clipboard sync, phone keyboard (InputSync.cs)
            case "rotate": return Wallpaper.Rotate(Box, rest);
            case "button": case "buttons": return Buttons.Command(verb.ToLower(), rest);  // hardware Buttons 1-4 (Buttons.cs)
            case "android": case "home": return AndroidApps.Command(verb.ToLower(), rest);  // Android apps in Start, Home key (AndroidApps.cs)
            case "run":
            {
                string exe = rest, a = "";
                if (rest.StartsWith("\"")) { int q = rest.IndexOf('"', 1); exe = rest.Substring(1, q - 1); a = rest.Substring(q + 1).Trim(); }
                else { int s = rest.IndexOf(' '); if (s > 0) { exe = rest.Substring(0, s); a = rest.Substring(s + 1); } }
                return "ok pid " + Process.Start(exe, a).Id;
            }
            case "regset":
            {
                // regset HKLM\Path\Key Name dword|sz Value
                string[] p = SplitN(rest, 4);
                if (p.Length < 4) return "bad args";
                RegistryKey root = p[0].StartsWith("HKCU") ? Registry.CurrentUser : Registry.LocalMachine;
                using (RegistryKey k = root.CreateSubKey(p[0].Substring(p[0].IndexOf('\\') + 1)))
                {
                    if (p[2] == "dword") k.SetValue(p[1], int.Parse(p[3]));
                    else k.SetValue(p[1], p[3]);
                }
                return "ok";
            }
            case "regdump":
            {
                // regdump HKCU\Path\Key: values of one key, for diagnostics
                RegistryKey root = rest.StartsWith("HKCU") ? Registry.CurrentUser : Registry.LocalMachine;
                using (RegistryKey k = root.OpenSubKey(rest.Substring(rest.IndexOf('\\') + 1)))
                {
                    if (k == null) return "no key";
                    var sb = new StringBuilder();
                    foreach (string n in k.GetValueNames()) sb.Append(n).Append('=').Append(k.GetValue(n)).Append("; ");
                    foreach (string n in k.GetSubKeyNames()) sb.Append('[').Append(n).Append("] ");
                    return sb.ToString();
                }
            }
            default: return "unknown";
        }
    }

    // tz <bias minutes> <daylight bias> <standard name>|<daylight name>
    // Bias follows Windows: UTC = local + bias. Without transition rules the zone has no DST switch;
    // the app resends the current offset whenever the phone's offset changes.
    static string SetTimeZone(string args)
    {
        string[] p = SplitN(args, 3);
        if (p.Length < 2) return "bad args";
        var tz = new TIME_ZONE_INFORMATION();
        tz.Bias = int.Parse(p[0]);
        tz.DaylightBias = int.Parse(p[1]);
        string[] names = p.Length > 2 ? p[2].Split('|') : new string[0];
        tz.StandardName = names.Length > 0 ? names[0] : "Phone time";
        tz.DaylightName = names.Length > 1 ? names[1] : tz.StandardName;
        tz.StandardDate = new byte[16];
        tz.DaylightDate = new byte[16];
        return SetTimeZoneInformation(ref tz) ? "ok" : "failed " + Marshal.GetLastWin32Error();
    }

    // Split on spaces into at most n parts (the Compact Framework lacks Split(char[], int)).
    static string[] SplitN(string s, int n)
    {
        var parts = new System.Collections.Generic.List<string>();
        int start = 0;
        while (parts.Count < n - 1)
        {
            int sp = s.IndexOf(' ', start);
            if (sp < 0) break;
            parts.Add(s.Substring(start, sp - start));
            start = sp + 1;
        }
        parts.Add(s.Substring(start));
        return parts.ToArray();
    }

    // Messages to the app.
    public static void Send(string msg)
    {
        try
        {
            if (!IsMounted(Card)) return;  // no card: nobody to tell (see CheckCard)
            string dir = Path.Combine(Box, "out");
            Directory.CreateDirectory(dir);
            string name = DateTime.Now.Ticks.ToString("x16") + "-" + Environment.TickCount.ToString("x8");
            string tmp = Path.Combine(dir, name + ".tmp");
            using (var w = new StreamWriter(tmp, false, new UTF8Encoding(false))) w.Write(msg);
            File.Move(tmp, Path.Combine(dir, name + ".msg"));
        }
        catch (Exception e) { Log("send " + e.Message); }
    }

    // Current settings, written by the app as key=value lines.
    public static string Get(string key, string fallback)
    {
        try
        {
            using (var r = new StreamReader(Path.Combine(Box, "state.ini")))
            {
                string line;
                while ((line = r.ReadLine()) != null)
                {
                    int eq = line.IndexOf('=');
                    if (eq > 0 && line.Substring(0, eq) == key) return line.Substring(eq + 1);
                }
            }
        }
        catch { }
        return fallback;
    }

    public static void Log(string s)
    {
        try
        {
            if (!IsMounted(Card)) return;  // never create \Storage Card in RAM (see CheckCard)
            Directory.CreateDirectory(Box);
            using (var w = new StreamWriter(Path.Combine(Box, "helper.log"), true)) w.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " " + s);
        }
        catch { }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TIME_ZONE_INFORMATION
    {
        public int Bias;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string StandardName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] StandardDate;
        public int StandardBias;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DaylightName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] DaylightDate;
        public int DaylightBias;
    }
    [DllImport("coredll.dll", SetLastError = true)] static extern bool SetTimeZoneInformation(ref TIME_ZONE_INFORMATION tz);
    [DllImport("coredll.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("coredll.dll", SetLastError = true)] static extern IntPtr CreateMutex(IntPtr attr, bool owner, string name);
    [DllImport("coredll.dll")] static extern bool PlaySound(string file, IntPtr module, int flags);
    [DllImport("coredll.dll", EntryPoint = "GetFileAttributesW")] static extern uint GetFileAttributes(string path);
    [DllImport("coredll.dll")]
    static extern bool GetDiskFreeSpaceEx(string dir, out ulong availToCaller, out ulong total, out ulong free);
}

// The WMDroid settings app is in SettingsApp.cs.
