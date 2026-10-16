using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Applies the theme the app generated from Android's Material You colours (mailbox folder
// "theme": PNGs + theme.ini), the way Settings > Today applies one: registry values, then the
// colour engine, theme primitives and a WM_SETTINGCHANGE 0xF2 broadcast, live.
// Every value it changes is backed up first, so "theme reset" puts the previous theme back.
// Windows Mobile 5.0 to 6.1.4 have none of 6.5's primitives: there the theme is the phone colour's
// hue as the shell's BaseHue (ApplyHue; docs/research-themes.md section 5).
static class Theme
{
    const string StateKey = @"Software\WMDroid";
    const string BackupKey = @"Software\WMDroid\ThemeBackup";
    const string Missing = "\u0001missing";
    const string ColorKey = @"HKLM\Software\Microsoft\Color";
    const string TodayKey = @"HKCU\Software\Microsoft\Today";

    // Windows Mobile 5 (CE 5.1), 6.0 (5.2.1235), 6.1 (5.2.19202) and 6.1.4 (5.2.20757): BaseHue
    // colour scheme. 6.5 (5.2.21xxx, 6.5.3 = 5.2.23090) has the primitives.
    static bool HueOnly
    {
        get { Version v = Environment.OSVersion.Version; return v.Major == 5 && (v.Minor == 1 || v.Minor == 2 && v.Build < 21000); }
    }

    // theme | theme reset
    public static string Apply(string box, string args)
    {
        if (args == "reset") return Reset();
        string dir = Path.Combine(box, "theme");
        string ini = Path.Combine(dir, "theme.ini");
        if (!File.Exists(ini)) return "no theme";
        string[] lines;
        using (var r = new StreamReader(ini)) lines = r.ReadToEnd().Split('\n');

        string hash = "";
        foreach (string l in lines) if (l.StartsWith("#hash=")) hash = l.Substring(6).Trim();
        // A different stamp there, so a theme an older helper wrote with no effect applies again.
        if (HueOnly && hash.Length > 0) hash = "hue:" + hash;
        int gen;
        using (RegistryKey s = Registry.CurrentUser.CreateSubKey(StateKey))
        {
            if (hash.Length > 0 && hash.Equals(s.GetValue("ThemeHash") as string)) return "unchanged";
            object g = s.GetValue("ThemeGen");
            gen = (g is int ? (int)g : 0) + 1;
        }
        if (HueOnly) return ApplyHue(lines, hash);

        // New file names per generation: registry values always change and nothing in use is overwritten.
        string prefix = @"\Windows\wmdroid_t" + gen + "_";
        foreach (string f in Directory.GetFiles(dir, "*.png"))
            File.Copy(f, prefix + Path.GetFileName(f), true);

        int n = 0;
        foreach (string raw in lines)
        {
            string l = raw.Trim();
            if (l.Length == 0 || l.StartsWith("#")) continue;
            string[] p = l.Split('|');
            if (p.Length != 4) continue;
            object v;
            RegistryValueKind kind;
            switch (p[2])
            {
                case "dword": v = int.Parse(p[3]); kind = RegistryValueKind.DWord; break;
                case "bin": v = Convert.FromBase64String(p[3]); kind = RegistryValueKind.Binary; break;
                default: v = p[3].Replace("%T%", prefix); kind = RegistryValueKind.String; break;
            }
            Write(p[0], p[1], v, kind, true);
            n++;
        }
        using (RegistryKey s = Registry.CurrentUser.CreateSubKey(StateKey))
        {
            s.SetValue("ThemeGen", gen);
            s.SetValue("ThemeHash", hash);
        }
        Refresh();
        DeleteOld(prefix);
        return "ok " + n;
    }

    // Windows Mobile 5.0-6.1.4: aygshell builds its 53 shell and system colours from built-in HSV entries,
    // each recoloured to the hue in Color\BaseHue (0x00VVSSHH, 0..240 scale; S = 0 makes it grey),
    // but only while Today\Skin names a theme: with no Skin it uses the stock colours as they are.
    // shell32 watches BaseHue and recolours at once; #371 and the 0xF2 broadcast are what the
    // Today control panel does after a theme. The accent is MultiColorTheme\BaseColor3 in theme.ini.
    static string ApplyHue(string[] lines, string hash)
    {
        int accent = -1;
        foreach (string raw in lines)
        {
            string[] p = raw.Trim().Split('|');
            if (p.Length == 4 && p[1] == "BaseColor3" && p[2] == "dword") accent = int.Parse(p[3]);
        }
        if (accent < 0) return "no accent";
        int hue = BaseHue(accent);
        // A 6.5-layout override table (written by an older helper) would apply once Skin is set:
        // its white entries are coloured parts here. The stock themes have none.
        Remove(ColorKey, "ThemeColors");
        Remove(ColorKey, "ThemeValues");
        Write(ColorKey, "BaseHue", hue, RegistryValueKind.DWord, true);
        Write(TodayKey, "Skin", @"\Windows\WMDroid.tsk", RegistryValueKind.String, true);
        using (RegistryKey s = Registry.CurrentUser.CreateSubKey(StateKey)) s.SetValue("ThemeHash", hash);
        Refresh();
        DeleteOld(null);  // pictures an older helper copied for 6.5's primitives
        return "ok hue " + (hue & 0xFF);
    }

    // COLORREF (0x00BBGGRR) -> BaseHue: hue | saturation << 8 | value << 16, each 0..240.
    static int BaseHue(int c)
    {
        int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        int max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), d = max - min;
        double h = 0;
        if (d > 0)
        {
            if (max == r) h = (double)(g - b) / d;
            else if (max == g) h = 2 + (double)(b - r) / d;
            else h = 4 + (double)(r - g) / d;
            if (h < 0) h += 6;
        }
        int hh = (int)(h * 40 + 0.5) % 240;
        int s = max == 0 ? 0 : d * 240 / max;
        int v = max * 240 / 255;
        return hh | s << 8 | v << 16;
    }

    // Puts back every value the theme changed, then refreshes.
    static string Reset()
    {
        int n = 0;
        using (RegistryKey b = Registry.CurrentUser.OpenSubKey(BackupKey))
        {
            if (b == null) return "nothing to reset";
            foreach (string name in b.GetValueNames())
            {
                int bar = name.LastIndexOf('|');
                if (bar <= 0) continue;
                object v = b.GetValue(name);
                string path = name.Substring(0, bar), value = name.Substring(bar + 1);
                if (Missing.Equals(v)) Delete(path, value);
                else Write(path, value, v, v is int ? RegistryValueKind.DWord : v is byte[] ? RegistryValueKind.Binary : RegistryValueKind.String, false);
                n++;
            }
        }
        Registry.CurrentUser.DeleteSubKeyTree(BackupKey);
        using (RegistryKey s = Registry.CurrentUser.CreateSubKey(StateKey)) s.SetValue("ThemeHash", "");
        Refresh();
        DeleteOld(null);
        return "ok " + n;
    }

    static RegistryKey Root(string path, out string sub)
    {
        sub = path.Substring(5);
        return path.StartsWith("HKCU") ? Registry.CurrentUser : Registry.LocalMachine;
    }

    static void Write(string path, string name, object v, RegistryValueKind kind, bool backup)
    {
        string sub;
        RegistryKey root = Root(path, out sub);
        using (RegistryKey k = root.CreateSubKey(sub))
        {
            if (backup)
            {
                using (RegistryKey b = Registry.CurrentUser.CreateSubKey(BackupKey))
                {
                    string id = path + "|" + name;
                    if (b.GetValue(id) == null)
                    {
                        object old = k.GetValue(name);
                        b.SetValue(id, old ?? Missing);
                    }
                }
            }
            k.SetValue(name, v, kind);
        }
    }

    // Deletes a value, backing it up first like Write.
    static void Remove(string path, string name)
    {
        string sub;
        RegistryKey root = Root(path, out sub);
        object old;
        using (RegistryKey k = root.OpenSubKey(sub)) old = k == null ? null : k.GetValue(name);
        if (old == null) return;
        using (RegistryKey b = Registry.CurrentUser.CreateSubKey(BackupKey))
        {
            string id = path + "|" + name;
            if (b.GetValue(id) == null) b.SetValue(id, old);
        }
        Delete(path, name);
    }

    static void Delete(string path, string name)
    {
        string sub;
        RegistryKey root = Root(path, out sub);
        using (RegistryKey k = root.OpenSubKey(sub, true))
            if (k != null) try { k.DeleteValue(name); } catch (ArgumentException) { }
    }

    // What Settings > Today does after writing a theme: colour engine, primitives, broadcast.
    static void Refresh()
    {
        try { ReloadThemeColors(); } catch (Exception e) { Helper.Log("theme #371 " + e.Message); }
        // Before 6.5, coredll has no UpdateThemePrimitives (and there is nothing to update).
        if (!HueOnly) try { UpdateThemePrimitives(); } catch (Exception e) { Helper.Log("theme primitives " + e.Message); }
        PostMessage(new IntPtr(0xFFFF), 0x001A /* WM_SETTINGCHANGE */, new IntPtr(0xF2), IntPtr.Zero);
    }

    // Removes earlier generations' pictures (keep = the current prefix, null = all).
    static void DeleteOld(string keep)
    {
        foreach (string f in Directory.GetFiles(@"\Windows", "wmdroid_t*_*.png"))
            if (keep == null || !f.StartsWith(keep, StringComparison.OrdinalIgnoreCase))
                try { File.Delete(f); } catch (Exception) { }
    }

    [DllImport("aygshell.dll", EntryPoint = "#371")] static extern int ReloadThemeColors();
    [DllImport("coredll.dll")] static extern void UpdateThemePrimitives();
    [DllImport("coredll.dll")] static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);
}
