using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// The classic Today screen item (not Titanium) that shows how many people are in the InfraWAN room.
// InfraToday.dll is ordinal 240, InitializeCustomItem (guest/Today/today.c). Settings > Today lists
// it as InfraWAN; turning it off there stops the count. A tap opens InfraLobby.
static class Today
{
    const string DllPath = @"\Windows\InfraToday4.dll";
    const string Key = @"Software\Microsoft\Today\Items\InfraWAN";

    /** Replaces the DLL when the user already installed this item. Does not add the item. */
    public static bool Update()
    {
        if (!File.Exists(DllPath)) return false;
        bool replaced = Put(DllPath, TodayDll.Base64);
        if (replaced) Helper.Log("today: InfraToday.dll updated");
        return replaced;
    }

    public static bool Install()
    {
        bool replaced = Put(DllPath, TodayDll.Base64);
        bool fresh;
        using (RegistryKey k = Registry.LocalMachine.CreateSubKey(Key))
        {
            fresh = k.GetValue("Enabled") == null;
            k.SetValue("DLL", DllPath);
            k.SetValue("Type", 4);             // custom item
            k.SetValue("Options", 0);          // no options dialog (ordinal 241 is not exported)
            k.SetValue("Selectability", 1);    // listed in Settings > Today
            if (fresh)
            {
                k.SetValue("Enabled", 1);
                k.SetValue("Flags", 0);
                k.SetValue("Order", 100);
            }
        }
        RegFlushKey(HKLM);
        if (replaced) Helper.Log("today: InfraToday.dll installed");
        return replaced;
    }

    static bool Put(string path, string base64)
    {
        byte[] want = Convert.FromBase64String(base64);
        if (Same(path, want)) return false;
        try
        {
            using (var f = File.Create(path)) f.Write(want, 0, want.Length);
            return true;
        }
        catch (Exception)
        {
            // The shell has the old DLL mapped. Move it aside so this path can be written; the
            // running shell keeps the old image until it is restarted (CopyProgram in Helper.cs).
            string old = path + ".old";
            try { if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
            File.Move(path, old);
            using (var f = File.Create(path)) f.Write(want, 0, want.Length);
            return true;
        }
    }

    static bool Same(string path, byte[] want)
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

    static readonly UIntPtr HKLM = new UIntPtr(0x80000002u);
    [DllImport("coredll.dll")] static extern int RegFlushKey(UIntPtr hKey);
}
