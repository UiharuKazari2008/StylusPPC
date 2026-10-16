using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

// Android apps in Windows Mobile (the app's Launcher settings, AndroidApps.kt).
//
// "android sync": the app wrote android.ini to the mailbox and one small launcher per app to the
// card's .android folder (guest/AppLink: the app's icon, and tapping it asks the app to open the
// Android app). This keeps Start > Programs > Android Apps in step with it: one shortcut per app,
// named after it, and pinned Android shortcuts as programs of their own in Start > Programs.
// Only shortcuts whose target changed are rewritten.
//
// android.ini (UTF-8): "folder=0|1", "name=<folder name>", then "app<TAB>label<TAB>file" and
// "pin<TAB>label<TAB>file" lines (file: in <card>\.android).
//
// "home today|start": Home on the phone while PocketStylus is its launcher shows Today, or Today
// and then the Start menu.
static class AndroidApps
{
    const string Programs = @"\Windows\Start Menu\Programs";
    const string PinsKey = @"Software\PocketStylus\AndroidPins";  // HKCU: the pinned shortcuts made here
    const string FolderKey = @"Software\PocketStylus";            // HKCU "AndroidFolder": the folder made here

    public static string Command(string verb, string rest)
    {
        if (verb == "home") return Home(rest);
        if (rest == "sync") return Sync();
        return "unknown";
    }

    static string Sync()
    {
        string card = Path.GetDirectoryName(Helper.Box);
        string ini = Path.Combine(Helper.Box, "android.ini");
        if (!File.Exists(ini)) return "no list";
        bool folderOn = true;
        string name = "Android Apps";
        var apps = new List<string[]>();
        var pins = new List<string[]>();
        using (var r = new StreamReader(ini, Encoding.UTF8))
        {
            string line;
            while ((line = r.ReadLine()) != null)
            {
                if (line.StartsWith("folder=")) folderOn = line.Substring(7) != "0";
                else if (line.StartsWith("name=")) name = Clean(line.Substring(5), "Android Apps");
                else
                {
                    string[] f = line.Split('\t');
                    if (f.Length < 3) continue;
                    if (f[0] == "app") apps.Add(f);
                    else if (f[0] == "pin") pins.Add(f);
                }
            }
        }
        int written = 0, removed = 0;
        string target = Path.Combine(card, ".android");

        // The folder: a renamed one goes, the current one holds exactly the listed apps.
        string folder = Path.Combine(Programs, name);
        string old = null;
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(FolderKey))
        {
            old = k.GetValue("AndroidFolder") as string;
            if (old != null && old != folder && Directory.Exists(old)) { try { Directory.Delete(old, true); } catch (Exception) { } }
            if (folderOn) k.SetValue("AndroidFolder", folder);
            else try { k.DeleteValue("AndroidFolder"); } catch (Exception) { }
        }
        if (!folderOn)
        {
            if (Directory.Exists(folder)) { try { Directory.Delete(folder, true); removed++; } catch (Exception) { } }
        }
        else
        {
            Directory.CreateDirectory(folder);
            var keep = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            foreach (string[] a in apps)
            {
                string lnk = Unique(Path.Combine(folder, Clean(a[1], "App")), keep);
                keep[Path.GetFileName(lnk)] = true;
                if (Write(lnk, "\"" + Path.Combine(target, a[2]) + "\"")) written++;
            }
            foreach (string f in Directory.GetFiles(folder, "*.lnk"))
                if (!keep.ContainsKey(Path.GetFileName(f))) { try { File.Delete(f); removed++; } catch (Exception) { } }
        }

        // Pinned shortcuts: programs of their own; the ones made here are remembered in the registry.
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(PinsKey))
        {
            var keep = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            // Names taken by other programs stay theirs: only ours may be rewritten.
            var mine = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            foreach (string v in k.GetValueNames()) mine[v] = true;
            foreach (string[] p in pins)
            {
                string baseName = Clean(p[1], "Shortcut");
                string lnk = Path.Combine(Programs, baseName + ".lnk");
                for (int i = 2; File.Exists(lnk) && !mine.ContainsKey(Path.GetFileName(lnk)) || keep.ContainsKey(Path.GetFileName(lnk)); i++)
                    lnk = Path.Combine(Programs, baseName + " " + i + ".lnk");
                keep[Path.GetFileName(lnk)] = true;
                k.SetValue(Path.GetFileName(lnk), p[2]);
                if (Write(lnk, "\"" + Path.Combine(target, p[2]) + "\"")) written++;
            }
            foreach (string v in k.GetValueNames())
            {
                if (keep.ContainsKey(v)) continue;
                try { File.Delete(Path.Combine(Programs, v)); removed++; } catch (Exception) { }
                k.DeleteValue(v);
            }
        }
        if (written + removed > 0) Helper.Log("android: " + apps.Count + " apps, " + pins.Count + " shortcuts; " + written + " written, " + removed + " removed");
        return "ok apps=" + apps.Count + " pins=" + pins.Count + " written=" + written + " removed=" + removed;
    }

    // A file name from an app's label: no characters Windows forbids, not empty, not too long.
    static string Clean(string label, string fallback)
    {
        var sb = new StringBuilder();
        foreach (char c in label)
            sb.Append(c < 32 || "\\/:*?\"<>|".IndexOf(c) >= 0 ? ' ' : c);
        string s = sb.ToString().Trim().TrimEnd('.');
        if (s.Length > 48) s = s.Substring(0, 48).Trim();
        return s.Length > 0 ? s : fallback;
    }

    // <path>.lnk, or "<path> 2.lnk" and so on when an earlier app of this sync has the name.
    static string Unique(string path, Dictionary<string, bool> taken)
    {
        string lnk = path + ".lnk";
        for (int i = 2; taken.ContainsKey(Path.GetFileName(lnk)); i++) lnk = path + " " + i + ".lnk";
        return lnk;
    }

    // Writes a shortcut unless it already has this target; true when written.
    static bool Write(string lnk, string target)
    {
        string body = target.Length + "#" + target;
        try
        {
            if (File.Exists(lnk))
                using (var r = new StreamReader(lnk, Encoding.ASCII))
                    if (r.ReadToEnd() == body) return false;
        }
        catch (Exception) { }
        Helper.Shortcut(lnk, target);
        return true;
    }

    static string Home(string where)
    {
        IntPtr today = FindWindow("DesktopExplorerWindow", null);
        if (today == IntPtr.Zero) return "no today window";
        // The Start menu (WM 6.5's full-screen one too) closes when Today comes to the front.
        SetForegroundWindow(today);
        if (where != "start") return "ok";
        Thread.Sleep(150);
        keybd_event(0x5B /* VK_LWIN */, 0, 0, 0);
        keybd_event(0x5B, 0, 2 /* KEYEVENTF_KEYUP */, 0);
        return "ok";
    }

    [DllImport("coredll.dll", EntryPoint = "FindWindowW")] static extern IntPtr FindWindow(string cls, string name);
    [DllImport("coredll.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("coredll.dll")] static extern void keybd_event(byte vk, byte scan, int flags, int extra);
}
