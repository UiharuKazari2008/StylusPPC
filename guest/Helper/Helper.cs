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
            if (mode == "storage")  // the Storage Card page alone (Standard: the phone's, no buttons there)
            {
                if (Edition.Smartphone) Send("open storage");
                else Application.Run(new StorageForm());
                return;
            }
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

    // Ends every process with this file name, except this one.
    static void EndNamed(string file)
    {
        int self = Process.GetCurrentProcess().Id;
        IntPtr snap = CreateToolhelp32Snapshot(2 /* TH32CS_SNAPPROCESS */ | 0x40000000 /* SNAPNOHEAPS */, 0);
        if (snap == new IntPtr(-1)) return;
        try
        {
            byte[] pe = new byte[564];
            BitConverter.GetBytes(pe.Length).CopyTo(pe, 0);
            for (bool ok = Process32First(snap, pe); ok; ok = Process32Next(snap, pe))
            {
                int pid = BitConverter.ToInt32(pe, 8);
                string exe = Encoding.Unicode.GetString(pe, 36, 520);
                int nul = exe.IndexOf((char)0);
                if (nul >= 0) exe = exe.Substring(0, nul);
                if (pid == self || string.Compare(Path.GetFileName(exe), file, true) != 0) continue;
                IntPtr h = OpenProcess(1 /* PROCESS_TERMINATE */, false, pid);
                if (h == IntPtr.Zero) continue;
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
        }
        finally { CloseToolhelp32Snapshot(snap); }
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
        catch (Exception)
        {
            // A loaded DLL (the shell holds WmpHook) cannot be overwritten. Move it aside so the
            // next load of this path gets the new file; the process that has it mapped keeps the old image.
            try
            {
                string old = dst + ".old";
                try { if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
                File.Move(dst, old);
                File.Copy(src, dst, true);
                return dst;
            }
            catch (Exception) { return File.Exists(dst) ? dst : null; }
        }
    }

    static void Install()
    {
        bool copied = string.Compare(Self, InstalledExe, true) == 0;
        if (!copied)
        {
            try { File.Copy(Self, InstalledExe, true); copied = true; }
            catch (IOException)
            {
                Log("helper: Windows copy is in use");
            }
        }
        try { File.Copy(Self, UiExe, true); }
        catch (Exception) { }  // a settings page is open: it keeps the old copy until next time
        string ui = File.Exists(UiExe) ? UiExe : (copied ? InstalledExe : Self);
        Shortcut(@"\Windows\Start Menu\Programs\PocketStylus.lnk", "\"" + ui + "\"");
        // If Windows still has the previous helper, start this copy after the reboot. That copy
        // replaces the Windows one once the old process is gone.
        Shortcut(@"\Windows\StartUp\PocketStylus.lnk", "\"" + (copied ? InstalledExe : Self) + "\" serve");
        // The other programs come with every helper update (new icons and fixes reach them too).
        string send = CopyProgram("SendBluetooth.exe");
        CopyProgram("GpsTest.exe", true);
        string lobby = CopyProgram("InfraLobby.exe");
        if (lobby != null) Shortcut(@"\Windows\Start Menu\Programs\InfraLobby.lnk", "\"" + lobby + "\"");
        try { DropLeftover(); } catch (Exception) { }
        // Today items are not installed or replaced here. Settings > Today (the "widget" command)
        // is the only way one is added. Copies that were injected before that button existed are
        // removed once, and a later helper start leaves whatever the user installed.
        bool stripped = false;
        try { stripped = ClearInjectedWidgets(); } catch (Exception e) { Log("today " + e.Message); }
        if (stripped)
        {
            EndNamed("shell32.exe");
            try { EnsureShell(); } catch (Exception) { }
        }
        // Text messages on WM 6.5.3 (Sms.cs): MAPIdotnet's native half and its stream types.
        CopyProgram("MAPIlib.dll");
        CopyProgram("OpenNETCF.dll");
        AudioClass.RemoveHook();  // WmpHook.dll did not drive the player (AudioClass.cs)
        Shortcut(Bluetooth.StartMenuLink, send != null ? "\"" + send + "\"" : "\"" + ui + "\" send");
        // (Renamed WMDroid -> PocketStylus: the old entry and shortcuts go.)
        foreach (string old in new string[] { @"\Windows\Start Menu\Programs\WMDroid.lnk", @"\Windows\StartUp\WMDroid.lnk" })
            try { if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
        SettingsEntry(ui);
    }

    // Retired Today plugins (notes, pins, search, and the old weather DLLs).
    // SkyToday.dll and InfraToday4.dll are the optional widgets. DropLeftover does not touch them;
    // ClearInjectedWidgets removes the copies that were injected before the install buttons existed.
    static bool TrashDllName(string path)
    {
        string n = Path.GetFileName(path).ToLower();
        if (n == "infratoday4.dll") return false;
        if (n.EndsWith(".old")) n = n.Substring(0, n.Length - 4);
        if (!n.EndsWith(".dll")) return false;
        foreach (string p in new string[] {
            "psnotes", "pssearch", "psweather", "pspins",
            "wmdnotes", "wmdsearch", "wmdweather", "wmdpins", "infratoday" })
            if (n.StartsWith(p)) return true;
        return false;
    }

    static bool DeleteKey(string path)
    {
        string[] subs;
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(path))
        {
            if (k == null) return false;
            subs = k.GetSubKeyNames();
        }
        foreach (string sub in subs) DeleteKey(path + "\\" + sub);
        try { Registry.LocalMachine.DeleteSubKey(path, false); return true; }
        catch (Exception) { return false; }
    }

    static bool DropTodayKeys()
    {
        bool changed = false;
        const string root = @"Software\Microsoft\Today\Items";
        foreach (string name in new string[] { "Notifications", "Programs", "Search" })
            if (DeleteKey(root + "\\" + name)) changed = true;
        try
        {
            string[] names;
            using (RegistryKey items = Registry.LocalMachine.OpenSubKey(root))
            {
                if (items == null) return changed;
                names = items.GetSubKeyNames();
            }
            foreach (string name in names)
            {
                if (string.Compare(name, "InfraWAN", true) == 0) continue;
                string dll = null;
                using (RegistryKey k = Registry.LocalMachine.OpenSubKey(root + "\\" + name))
                    if (k != null) dll = k.GetValue("DLL") as string;
                if (dll != null && TrashDllName(dll) && DeleteKey(root + "\\" + name)) changed = true;
            }
        }
        catch (Exception) { }
        return changed;
    }

    static int DropPluginFiles()
    {
        int n = 0;
        foreach (string pattern in new string[] {
            "PsNotes*.dll", "PsNotes*.old", "PsSearch*.dll", "PsSearch*.old",
            "PsWeather*.dll", "PsWeather*.old", "PsPins*.dll", "PsPins*.old",
            "WmdNotes*.dll", "WmdNotes*.old", "WmdSearch*.dll", "WmdSearch*.old",
            "WmdWeather*.dll", "WmdWeather*.old", "WmdPins*.dll", "WmdPins*.old",
            "InfraToday*.dll", "InfraToday*.old" })
        {
            string[] found;
            try { found = Directory.GetFiles(@"\Windows", pattern); }
            catch (Exception) { continue; }
            foreach (string path in found)
            {
                if (!TrashDllName(path)) continue;
                try { File.Delete(path); n++; }
                catch (Exception) { }
            }
        }
        foreach (string path in new string[] {
            @"\Storage Card\.wmdroid\notes.txt", @"\Storage Card\.wmdroid\weather.bmp",
            @"\Storage Card2\.wmdroid\notes.txt", @"\Storage Card2\.wmdroid\weather.bmp" })
        {
            try { if (File.Exists(path)) { File.Delete(path); n++; } }
            catch (Exception) { }
        }
        return n;
    }

    // Removes the retired Today plugins. The shell is left running: ending it here kept Today from
    // coming back. InfraWAN and Weather are handled by ClearInjectedWidgets, once.
    static void DropLeftover()
    {
        bool keys = DropTodayKeys();
        int files = DropPluginFiles();
        try
        {
            using (RegistryKey live = Registry.LocalMachine.OpenSubKey(@"Software\Microsoft\Today\Items\Windows Live Search", true))
            {
                object on = live == null ? null : live.GetValue("Enabled");
                if (live != null && !(on is int && (int)on == 1)) live.SetValue("Enabled", 1);
            }
        }
        catch (Exception) { }
        if (keys || files > 0)
        {
            Log("today: removed leftover items");
            RegFlushKey(HKLM);
        }
    }

    static readonly UIntPtr HKCU = new UIntPtr(0x80000001u);

    static bool WidgetsOptIn()
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\WMDroid"))
            {
                object v = k == null ? null : k.GetValue("TodayOptIn");
                return v is int && (int)v == 1;
            }
        }
        catch (Exception) { return false; }
    }

    // The Weather and InfraWAN items shipped before there was an install button. Today still lists
    // them, and their DLLs (and the .old copies a replace leaves behind) are what breaks the screen.
    // Once, remove the items and the files. After this, only the settings button adds one.
    static bool ClearInjectedWidgets()
    {
        if (WidgetsOptIn()) return false;
        bool removed = false;
        if (DeleteKey(@"Software\Microsoft\Today\Items\Weather")) removed = true;
        if (DeleteKey(@"Software\Microsoft\Today\Items\InfraWAN")) removed = true;
        foreach (string path in new string[] {
            @"\Windows\SkyToday.dll", @"\Windows\SkyToday.dll.old", @"\Windows\SkyToday.dll.dead",
            @"\Windows\InfraToday.dll", @"\Windows\InfraToday.dll.old",
            @"\Windows\InfraToday4.dll", @"\Windows\InfraToday4.dll.old", @"\Windows\InfraToday4.dll.dead" })
        {
            if (RemoveTodayFile(path)) removed = true;
        }
        bool gone = true;
        foreach (string path in new string[] {
            @"\Windows\SkyToday.dll", @"\Windows\InfraToday.dll", @"\Windows\InfraToday4.dll" })
            if (File.Exists(path)) gone = false;
        using (RegistryKey w = Registry.LocalMachine.OpenSubKey(@"Software\Microsoft\Today\Items\Weather"))
            if (w != null) gone = false;
        using (RegistryKey i = Registry.LocalMachine.OpenSubKey(@"Software\Microsoft\Today\Items\InfraWAN"))
            if (i != null) gone = false;
        if (gone)
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\WMDroid"))
                    if (k != null) k.SetValue("TodayOptIn", 1, RegistryValueKind.DWord);
                RegFlushKey(HKCU);
            }
            catch (Exception e) { Log("today opt-in " + e.Message); }
        }
        if (removed)
        {
            Log("today: removed the injected widgets");
            RegFlushKey(HKLM);
        }
        return removed;
    }

    static bool RemoveTodayFile(string path)
    {
        try
        {
            if (!File.Exists(path)) return false;
            File.Delete(path);
            return true;
        }
        catch (Exception)
        {
            try
            {
                string dead = path + ".away";
                if (File.Exists(dead)) File.Delete(dead);
                File.Move(path, dead);
                return true;
            }
            catch (Exception) { return false; }
        }
    }

    static bool ProcessNamed(string file)
    {
        IntPtr snap = CreateToolhelp32Snapshot(2 /* TH32CS_SNAPPROCESS */ | 0x40000000 /* SNAPNOHEAPS */, 0);
        if (snap == new IntPtr(-1)) return false;
        try
        {
            byte[] pe = new byte[564];
            BitConverter.GetBytes(pe.Length).CopyTo(pe, 0);
            for (bool ok = Process32First(snap, pe); ok; ok = Process32Next(snap, pe))
            {
                string exe = Encoding.Unicode.GetString(pe, 36, 520);
                int nul = exe.IndexOf((char)0);
                if (nul >= 0) exe = exe.Substring(0, nul);
                if (string.Compare(Path.GetFileName(exe), file, true) == 0) return true;
            }
        }
        finally { CloseToolhelp32Snapshot(snap); }
        return false;
    }

    // The cleanup used to end the shell, and Today then never came back. Start it if it is gone.
    static void EnsureShell()
    {
        if (ProcessNamed("shell32.exe")) return;
        try
        {
            Process.Start(@"\Windows\shell32.exe", "");
            Log("today: shell was not running");
        }
        catch (Exception e) { Log("today: shell " + e.Message); }
    }

    static readonly UIntPtr HKLM = new UIntPtr(0x80000002u);
    [DllImport("coredll.dll")] static extern int RegFlushKey(UIntPtr hKey);

    // Settings > System. Group is the category: 0 Personal, 1 System, 2 Connections.
    /** "widget weather" / "widget infrawan": the settings button. Does not run on its own. */
    public static string InstallWidget(string which)
    {
        which = which == null ? "" : which.Trim().ToLower();
        bool changed;
        string name;
        if (which == "weather") { changed = Weather.Install(); name = "Weather"; }
        else if (which == "infrawan") { changed = Today.Install(); name = "InfraWAN"; }
        else return "bad args";
        if (changed) EndNamed("shell32.exe");
        return changed ? "installed " + name : "same " + name;
    }

    public static void SettingsEntry(string exe)
    {
        try { Registry.LocalMachine.DeleteSubKey(@"ControlPanel\WMDroid"); } catch (Exception) { }
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"ControlPanel\PocketStylus"))
        {
            k.SetValue("", "PocketStylus");
            k.SetValue("Group", 1);
            k.SetValue("Redirect", exe);
            k.SetValue("Icon", exe + ",0");
        }
        try
        {
            string dir = @"\Windows\Start Menu\Settings\System";
            Directory.CreateDirectory(dir);
            Shortcut(dir + @"\PocketStylus.lnk", "\"" + exe + "\"");
        }
        catch (Exception e) { Log("settings link " + e.Message); }
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

    // A long command (filing a lot of texts) is still moving: the watchdog restarts a helper that
    // has not stepped for 45 s, and that restart pulls the storage card out from under it.
    internal static void Beat() { beat = Environment.TickCount; }

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
        // Watchdog: a step that never returns (a driver call, a window that stopped answering) would
        // leave the app's commands waiting. A slow command is given three minutes and a pulse, so a
        // slowed guest is not restarted and the storage card stays in. A wedged loop still restarts
        // after 45 s. The stuck command was already taken out of the mailbox, so it isn't repeated.
        Step("start");
        var dog = new Thread(delegate ()
        {
            bool slowLogged = false;
            while (!quit)
            {
                Thread.Sleep(5000);
                if (quit) break;
                int stuck = Environment.TickCount - beat;
                // Only while work is actually slow: an idle helper already answers, and a pulse
                // every 5 s would fill the mailbox when the app is not reading it.
                if (stage.StartsWith("cmd ") || stuck >= 15000)
                {
                    // A pulse on the link. A mailbox file here fills up while the app is not reading.
                    if (!Link.PostStat("alive")) Send("alive");
                }
                // A command may run for minutes when the guest itself is slowed. Between commands,
                // 45 s still means the loop is wedged.
                int limit = stage.StartsWith("cmd ") ? 180000 : 45000;
                if (stuck < 45000) slowLogged = false;
                else if (stuck < limit && !slowLogged)
                {
                    slowLogged = true;
                    Log("slow " + (stuck / 1000) + " s at " + stage);
                }
                if (stuck < limit) continue;
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
        NetLink.Start();  // 10.0.2.2:7378, for anything the sideband page does not carry (NetLink.cs)
        string hello = Hello();
        Log(hello);
        Send(hello);
        try { Camera.Install(); } catch (Exception e) { Log("camera " + e.Message); }  // camera passthrough (Camera.cs)
        try { InputSync.RemoveBlank(); } catch (Exception e) { Log("ime " + e.Message); }  // the blank keyboard is gone (InputSync.cs)
        try { HtcCompat.Install(); } catch (Exception e) { Log("htc " + e.Message); }  // G-sensor driver + DirectDraw shim for HTC apps (HtcCompat.cs)
        try { DropLeftover(); } catch (Exception) { }
        try { EnsureShell(); } catch (Exception) { }
        Standby.Disable();  // WM's own auto-off would stop everything, mailbox included (Standby.cs)
        bool patchChecked = false;
        while (!quit)
        {
            // A clock step (the phone's time changed, or the guest was frozen while the RTC moved)
            // asks the app for a fresh status snapshot. The ask is a command; the snapshot is status.
            WatchClock();
            Step("cmd");
            try { DrainCmd(); }
            catch (Exception e) { Log("cmd " + e.Message); }
            Step("status");
            try { Link.Pump(); DrainStatus(); }
            catch (Exception e) { Log("status " + e.Message); }
            Step("card");
            CheckCard();
            Step("net");
            try { DrainNet(); }
            catch (Exception e) { Log("net " + e.Message); }
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
            try { Weather.Tick(); } catch (Exception) { }  // today's forecast, while the Today item is on (Weather.cs)
            Thread.Sleep(IsMounted(Card) ? 500 : 3000);
        }
        Calls.Stop();  // close the RIL client, or this process (and its mutex) outlives the update
        InputSync.Stop();  // clipboard/keyboard window's message loop (InputSync.cs)
        NetLink.Stop();
    }

    static long wall0;
    static int tick0;
    static bool clockSet;

    // Wall clock against the tick count. The RTC follows the phone; the tick count does not move
    // while the guest is frozen. Forward needs 15 s so a slow guest (the loop still finishes)
    // is not a jump. Backward is always a clock set.
    static void WatchClock()
    {
        long wall = DateTime.UtcNow.Ticks;
        int tick = Environment.TickCount;
        if (!clockSet) { wall0 = wall; tick0 = tick; clockSet = true; return; }
        long step = wall - wall0 - (long)unchecked(tick - tick0) * 10000L;
        wall0 = wall;
        tick0 = tick;
        if (step > 150000000L || step < -20000000L)
        {
            Log("time jump " + (step / 10000000L) + " s");
            if (!Link.PostCmd("sync")) Send("sync");
        }
    }

    // Command pipe. "result ..." is the app answering; everything else runs, and the short answer
    // goes back on the same pipe.
    static void DrainCmd()
    {
        foreach (string line in Link.TakeCmd())
        {
            if (line == null || line.Length == 0 || line.StartsWith("result ")) continue;
            Step("cmd " + (line.Length > 40 ? line.Substring(0, 40) : line));
            string answer;
            try { answer = Execute(line); }
            catch (Exception e) { Log("cmd " + e.Message); continue; }
            int sp = line.IndexOf(' ');
            string verb = sp > 0 ? line.Substring(0, sp) : line;
            Send("result " + verb + " " + answer);
        }
    }

    // TCP link. Status lines are applied and not answered. A blob is already queued ahead of its line.
    static void DrainNet()
    {
        foreach (string line in NetLink.TakeLines())
        {
            if (line == null || line.Length == 0 || line.StartsWith("result ")) continue;
            if (line.StartsWith("status ")) { Execute(line); continue; }
            Step("cmd " + (line.Length > 40 ? line.Substring(0, 40) : line));
            string answer;
            try { answer = Execute(line); }
            catch (Exception e) { Log("cmd " + e.Message); continue; }
            int sp = line.IndexOf(' ');
            string verb = sp > 0 ? line.Substring(0, sp) : line;
            Send("result " + verb + " " + answer);
        }
    }

    // Status pipe. Applied, not answered, so a stream of signal updates cannot fill the command pipe.
    static void DrainStatus()
    {
        foreach (string line in Link.TakeStat())
        {
            if (line == null || line.Length == 0) continue;
            Step("status " + (line.Length > 40 ? line.Substring(0, 40) : line));
            try { Execute(line); }
            catch (Exception e) { Log("status " + e.Message); }
        }
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
            case "pull":
            {
                // pull <guest path> <name>: copy one file out of Windows onto the storage card.
                string[] a = SplitN(rest, 2);
                if (a.Length < 2 || a[1].IndexOf('\\') >= 0 || a[1].IndexOf('/') >= 0) return "bad args";
                string dest = Path.Combine(Box, a[1]);
                try
                {
                    if (File.Exists(dest)) File.Delete(dest);
                    File.Copy(a[0], dest);
                    return "ok " + new FileInfo(dest).Length;
                }
                catch (Exception e) { return e.GetType().Name + " " + e.Message; }
            }
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
            case "tlsca": return TlsCa.Command(rest);  // secure web proxy root certificate (TlsCa.cs)
            case "tlsask": return TlsAsk.Command(rest);  // the proxy's certificate warning dialog (TlsAsk.cs)
            case "received": return Bluetooth.Received(rest);
            case "irda": return Irda.Command(rest);  // infrared transport choice (Irda.cs)
            case "widget": return InstallWidget(rest);  // Today item the user asked for (Today.cs, Weather.cs)
            case "clip": case "ime": case "type": return InputSync.Command(verb.ToLower(), rest);  // clipboard sync, phone keyboard (InputSync.cs)
            case "media": return AudioClass.Command(rest);  // play / pause / next / prev / stop (AudioClass.cs)
            case "vol": return AudioClass.SetVolumes(rest);  // phone media / ringtone / notification volume (AudioClass.cs)
            case "rotate": return Wallpaper.Rotate(Box, rest);
            case "button": case "buttons": return Buttons.Command(verb.ToLower(), rest);  // hardware Buttons 1-4 (Buttons.cs)
            case "htcinstall": return HtcCompat.InstallStaged();  // apps the app staged from an HTC ROM (HtcCompat.cs)
            case "htcuninstall": return HtcCompat.UninstallStaged();  // undo them on an OS that should not have them
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

    // Messages to the app. The serve loop and the watchdog both send.
    public static void Send(string msg)
    {
        if (msg == null || msg.Length == 0) return;
        // Short lines go on the sideband. TCP is the path when that page is not answering.
        if (msg.Length <= 200 && Link.PostCmd(msg)) return;
        if (!NetLink.SendLine(msg)) Log("send waiting for the link");
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
