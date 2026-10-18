using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// Pieces HTC's own apps (from an HD2 ROM the user copies in) expect from an HTC phone
// (docs/research-hd2-port.md):
//   \Windows\HTCSensorService.dll  the G-sensor stream driver SEN0: (guest/Sensor/htcsensor.c,
//                                  SensorDll.cs). HTC apps register it themselves when SEN0: is
//                                  missing; it reads the emulator's motion device.
//   \Windows\HDRAW.dll             DirectDraw with the display caps HTC games check for
//                                  (guest/DdShim/hdraw.c, HdrawDll.cs); an HTC game's DDRAW.dll
//                                  import is renamed to it when the game is staged.
//   \Windows\HTCFlashlightLib.dll  the camera LED for HTC Flashlight (guest/Flash/flashlight.c,
//                                  FlashDll.cs). HTC's own library needs a Qualcomm chip, so a
//                                  staged copy of it is replaced with this one.
//
// Apps from the user's HTC ROM: the app (HtcAppsActivity, core/src/htc/romkit.h) stages a ROM
// package as \Storage Card\.wmdroid\htc\<package>\ with install.ini written last; InstallStaged
// installs each staged folder once per OS build (done-<build>.txt), the way the ROM had it:
//   file=<name>      -> \Windows\<name>
//   dep=<name>       deps\<name> -> \Windows\<name>, only if \Windows lacks it
//   reg=<name>       REGEDIT4 import
//   link=<dst>|<src> Start menu shortcut (copy of the ROM's \Windows\<x>.lnk, from initflashfiles.dat)
// It runs only on "htcinstall" (the app sends it after staging) and only on WM 6.5. The app needs
// to know which DLLs \Windows has to stage only the missing ones: htc\windows.txt.
static class HtcCompat
{
    public static void Install()
    {
        // Back on: the stuck Start button came from booting the FWVGA image at 480x800
        // (GuestStates.resolve now boots the WVGA image there), not from these DLLs, which nothing
        // loads until an HTC app runs.
        // HD2 software is WM 6.5 only, and this runs once per release of these two DLLs: the
        // registry remembers the version written. Staged ROM apps install only when the app asks
        // ("htcinstall"), never at boot: every OS shares the card's htc folder.
        if (!Is65) return;
        string want = SensorDll.Base64.Length + "/" + HdrawDll.Base64.Length + "/" + FlashDll.Base64.Length;
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(MarkKey))
        {
            if (want == k.GetValue("dlls") as string && File.Exists(@"\Windows\HTCSensorService.dll")
                && File.Exists(@"\Windows\HDRAW.dll") && File.Exists(@"\Windows\HTCFlashlightLib.dll"))
                return;
            Put(@"\Windows\HTCSensorService.dll", SensorDll.Base64);
            Put(@"\Windows\HDRAW.dll", HdrawDll.Base64);
            Put(@"\Windows\HTCFlashlightLib.dll", FlashDll.Base64);
            k.SetValue("dlls", want);
        }
    }

    const string MarkKey = @"Software\PocketStylus\HtcCompat";

    /** WM 6.5 (CE 5.2 build 21000 and later; 6.1.4 is 5.2.20xxx), the release HD2 software is for. */
    static bool Is65 { get { return Environment.OSVersion.Version.Build >= 21000; } }

    static string Build { get { return Environment.OSVersion.Version.ToString(); } }

    /** Installs the staged HTC packages this OS build hasn't installed yet; a summary for the mailbox. */
    public static string InstallStaged()
    {
        if (!Is65) return "HTC HD2 apps need Windows Mobile 6.5";
        string root = Helper.Box + @"\htc";
        if (!Directory.Exists(root)) return "none staged";
        WriteWindowsList(root);
        var sb = new StringBuilder();
        foreach (string dir in Directory.GetDirectories(root))
        {
            if (!File.Exists(dir + @"\install.ini") || File.Exists(dir + @"\done-" + Build + ".txt")) continue;
            sb.Append(Path.GetFileName(dir)).Append(": ").Append(InstallOne(dir)).Append("; ");
        }
        return sb.Length > 0 ? sb.ToString() : "nothing new";
    }

    /**
     * "htcuninstall": takes every staged package's files, dependencies and shortcuts back out of this
     * OS's \Windows (ROM files cannot be deleted, so only copies go) and forgets that this build
     * installed them. For an OS that installed them by mistake.
     */
    public static string UninstallStaged()
    {
        string root = Helper.Box + @"\htc";
        if (!Directory.Exists(root)) return "none staged";
        int gone = 0, kept = 0;
        foreach (string dir in Directory.GetDirectories(root))
        {
            if (!File.Exists(dir + @"\install.ini")) continue;
            string[] lines;
            using (var r = new StreamReader(dir + @"\install.ini", Encoding.UTF8)) lines = r.ReadToEnd().Replace("\r", "").Split('\n');
            foreach (string line in lines)
            {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = line.Substring(0, eq), v = line.Substring(eq + 1);
                string path = key == "file" || key == "dep" ? @"\Windows\" + v : key == "link" ? v.Split('|')[0] : null;
                if (path == null || !File.Exists(path)) continue;
                try { File.Delete(path); gone++; } catch (Exception) { kept++; }
            }
            try { File.Delete(dir + @"\done-" + Build + ".txt"); } catch (Exception) { }
        }
        Helper.Log("htcuninstall: " + gone + " removed, " + kept + " kept (ROM or in use)");
        return gone + " removed, " + kept + " kept";
    }

    static string InstallOne(string dir)
    {
        var report = new StringBuilder();
        var links = new StringBuilder();
        int files = 0, deps = 0, errors = 0;
        bool exe = false;
        string[] lines;
        using (var r = new StreamReader(dir + @"\install.ini", Encoding.UTF8)) lines = r.ReadToEnd().Replace("\r", "").Split('\n');
        foreach (string line in lines)
        {
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            string key = line.Substring(0, eq), v = line.Substring(eq + 1);
            try
            {
                switch (key)
                {
                    case "file":
                        // HTC's flashlight library talks to a Qualcomm chip and jumps to address 0 here.
                        if (v.ToLower() == "htcflashlightlib.dll")
                        {
                            Put(@"\Windows\HTCFlashlightLib.dll", FlashDll.Base64);
                            files++;
                            break;
                        }
                        File.Copy(dir + "\\" + v, @"\Windows\" + v, true);
                        files++;
                        if (v.ToLower().EndsWith(".exe")) exe = true;
                        break;
                    case "dep":
                        if (File.Exists(@"\Windows\" + v)) break;
                        File.Copy(dir + @"\deps\" + v, @"\Windows\" + v, false);
                        deps++;
                        break;
                    case "reg":
                        ImportReg(dir + "\\" + v, report);
                        break;
                    case "missing":
                        report.Append("missing ").Append(v).Append("\r\n");
                        break;
                }
            }
            catch (Exception e) { errors++; report.Append(key).Append(' ').Append(v).Append(": ").Append(e.Message).Append("\r\n"); }
        }
        // Shortcuts last: they copy \Windows\<x>.lnk, installed above.
        foreach (string line in lines)
        {
            if (!line.StartsWith("link=")) continue;
            string[] p = line.Substring(5).Split('|');
            if (p.Length != 2) continue;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(p[0]));
                File.Copy(p[1], p[0], true);
                string where = p[0].Replace(@"\Windows\Start Menu\Programs\", "");
                links.Append(links.Length > 0 ? ", " : "").Append("Start > ").Append(where.Replace("\\", " > ").Replace(".lnk", ""));
            }
            catch (Exception e) { errors++; report.Append("link ").Append(p[0]).Append(": ").Append(e.Message).Append("\r\n"); }
        }
        string summary = files + " files, " + deps + " dependencies" + (errors > 0 ? ", " + errors + " errors" : "");
        try
        {
            using (var w = new StreamWriter(dir + @"\done-" + Build + ".txt", false, new UTF8Encoding(false)))
                w.Write(summary + "\r\n" + report);
        }
        catch (Exception e) { Helper.Log("htcinstall done " + e.Message); }
        Helper.Log("htcinstall " + Path.GetFileName(dir) + ": " + summary);
        Tell(Path.GetFileName(dir), links.ToString(), exe, errors);
        return summary;
    }

    // Tells the user where the app went, without holding up the mailbox.
    static void Tell(string package, string links, bool exe, int errors)
    {
        string name = package;
        int us = name.IndexOf('_');
        if (us > 0) name = name.Substring(0, us);
        string text = errors > 0 ? name + " was installed with errors (see " + Helper.Box + @"\htc\" + package + ")."
            : name + " is installed." + (links.Length > 0 ? "\r\n\r\n" + links : "");
        var screen = Screen.PrimaryScreen.Bounds;
        if (exe && Math.Min(screen.Width, screen.Height) == 480 && Math.Max(screen.Width, screen.Height) != 800)
            text += "\r\n\r\nHTC games are made for 480x800 and can crash at other sizes: PocketStylus Device Settings > Screen resolution > 480x800.";
        var t = new Thread(delegate ()
        {
            try { MessageBox.Show(text, "HTC apps", MessageBoxButtons.OK, MessageBoxIcon.Asterisk, MessageBoxDefaultButton.Button1); }
            catch { }
        });
        t.IsBackground = true;
        t.Start();
    }

    // htc\windows.txt: the DLLs \Windows has (ROM and RAM), so the app stages only missing ones.
    static void WriteWindowsList(string root)
    {
        string path = root + @"\windows.txt", head = "# " + Build;
        try
        {
            if (File.Exists(path)) using (var r = new StreamReader(path)) if (r.ReadLine() == head) return;
            var sb = new StringBuilder(head + "\r\n");
            foreach (string f in Directory.GetFiles(@"\Windows", "*.dll")) sb.Append(Path.GetFileName(f).ToLower()).Append("\r\n");
            using (var w = new StreamWriter(path, false, new UTF8Encoding(false))) w.Write(sb.ToString());
        }
        catch (Exception e) { Helper.Log("htc windows.txt " + e.Message); }
    }

    // REGEDIT4: strings, dword:, hex:, hex(2): expand_sz, hex(7): multi_sz, "name"=- deletes
    // (as Probe's install, WM 6.5.3 Professional Emulator/Samples/Probe).
    static void ImportReg(string path, StringBuilder report)
    {
        RegistryKey key = null;
        string pending = "";
        string[] lines;
        using (var r = new StreamReader(path, true)) lines = r.ReadToEnd().Replace("\r", "").Split('\n');
        foreach (string raw in lines)
        {
            string line = pending + raw.Trim();
            pending = "";
            if (line.EndsWith("\\") && !line.EndsWith("\"")) { pending = line.Substring(0, line.Length - 1); continue; }
            if (line.Length == 0 || line[0] == ';' || line.StartsWith("REGEDIT")) continue;
            if (line[0] == '[')
            {
                if (key != null) key.Close();
                string p = line.Substring(1, line.IndexOf(']') - 1);
                int s = p.IndexOf('\\');
                string hive = s < 0 ? p : p.Substring(0, s), sub = s < 0 ? "" : p.Substring(s + 1);
                RegistryKey root = hive == "HKEY_CURRENT_USER" ? Registry.CurrentUser : hive == "HKEY_CLASSES_ROOT" ? Registry.ClassesRoot : Registry.LocalMachine;
                key = root.CreateSubKey(sub);
                continue;
            }
            if (key == null) continue;
            string name; int i;
            if (line[0] == '@') { name = ""; i = 1; }
            else
            {
                var sb = new StringBuilder();
                for (i = 1; i < line.Length && line[i] != '"'; i++) { if (line[i] == '\\') i++; sb.Append(line[i]); }
                name = sb.ToString(); i++;
            }
            if (i >= line.Length || line[i] != '=') continue;
            string v = line.Substring(i + 1);
            try
            {
                if (v == "-") key.DeleteValue(name, false);
                else if (v.StartsWith("\""))
                {
                    var sb = new StringBuilder();
                    for (int k = 1; k < v.Length && v[k] != '"'; k++) { if (v[k] == '\\') k++; sb.Append(v[k]); }
                    key.SetValue(name, sb.ToString());
                }
                else if (v.StartsWith("dword:")) key.SetValue(name, (int)Convert.ToUInt32(v.Substring(6), 16));
                else if (v.StartsWith("hex"))
                {
                    int c = v.IndexOf(':');
                    string kind = v.Substring(0, c);
                    string[] parts = v.Substring(c + 1).Trim(',').Split(',');
                    byte[] b = new byte[parts.Length];
                    for (int k = 0; k < parts.Length; k++) b[k] = Convert.ToByte(parts[k].Trim(), 16);
                    if (kind == "hex(7)") key.SetValue(name, Encoding.Unicode.GetString(b, 0, b.Length).TrimEnd('\0').Split('\0'));
                    else if (kind == "hex(2)") key.SetValue(name, Encoding.Unicode.GetString(b, 0, b.Length).TrimEnd('\0'));
                    else key.SetValue(name, b);
                }
            }
            catch (Exception e) { report.Append("reg ").Append(name).Append(": ").Append(e.Message).Append("\r\n"); }
        }
        if (key != null) key.Close();
    }

    static void Put(string path, string base64)
    {
        byte[] want = Convert.FromBase64String(base64);
        if (SameFile(path, want)) return;
        try { using (var f = File.Create(path)) f.Write(want, 0, want.Length); }
        catch (Exception e) { Helper.Log("htc " + Path.GetFileName(path) + " " + e.Message); }
    }

    static bool SameFile(string path, byte[] want)
    {
        try
        {
            if (!File.Exists(path)) return false;
            byte[] have;
            using (var f = File.OpenRead(path)) { have = new byte[f.Length]; f.Read(have, 0, have.Length); }
            if (have.Length != want.Length) return false;
            for (int i = 0; i < have.Length; i++) if (have[i] != want[i]) return false;
            return true;
        }
        catch { return false; }
    }
}
