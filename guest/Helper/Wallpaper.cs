using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

// The home screen (Titanium, cmhome.dll) and the Start menu draw the theme wallpaper, which shell32
// loads from \Windows\stwater_<SM_CXSCREEN>_<SM_CYSCREEN><ext> (ext taken from the first
// \Windows\stwater*.* file) and caches until WM_SETTINGCHANGE 0xF2 or a screen-size change.
// The theme's copies are RAM files marked read-only/system: clearing the attributes lets us copy over
// them. The Titanium/Start-menu scenes (ConfettiPlus::WallpaperBitmap) cache their own copy keyed by
// HKCU\Software\Microsoft\Today\Wall (or Skin when Wall is unset) and only fetch the picture again from
// shell32 when that key has changed by the time they get WM_SETTINGCHANGE 0xF2.
static class Wallpaper
{
    const string Today = @"Software\Microsoft\Today";
    static int degrees;  // last orientation set through Rotate (0, 90, 180, 270)

    // wallpaper: install \Storage Card\.wmdroid\wall_portrait.jpg and the landscape picture that fits
    // the current orientation, then redraw the home screen and Start menu.
    public static string Apply(string box)
    {
        if (Environment.OSVersion.Version.Major < 5) return ApplyCe42(box);
        int w, h;
        Size(out w, out h);
        string portrait = Path.Combine(box, "wall_portrait.jpg"), landscape = Landscape(box, degrees);
        int p = Install(portrait, Name(w, h)), l = Install(landscape, Name(h, w));
        int done = p + l;
        var sb = System.Windows.Forms.Screen.PrimaryScreen.Bounds;
        // A picture chosen in Windows Mobile is Today\Wall. It is drawn instead of stwater, and it
        // stays there when the theme files already match the phone's wallpaper, so the home screen
        // has to be told to drop it even though nothing was copied.
        bool chosen = (File.Exists(portrait) || File.Exists(landscape)) && UserPicture();
        Helper.Log("wallpaper: portrait " + (p > 0 ? "new" : "same") + ", landscape " + (l > 0 ? "new" : "same") +
            " (" + Path.GetFileName(landscape) + "), screen " + sb.Width + "x" + sb.Height + ", turned " + degrees +
            (chosen ? ", replacing a chosen picture" : ""));
        if (done == 0 && !chosen) return File.Exists(portrait) || File.Exists(landscape) ? "unchanged" : "no pictures";
        Refresh(Name(w, h));
        return done == 0 ? "cleared picture" : "ok " + done;
    }

    // Today\Wall set: the user picked a picture, which hides the theme wallpaper.
    static bool UserPicture()
    {
        try
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(Today))
            {
                if (k == null) return false;
                string v = k.GetValue("Wall") as string;
                return v != null && v.Length > 0;
            }
        }
        catch (Exception) { return false; }
    }

    // rotate N [sync]: turn the display. "sync" is sent only while Android wallpaper sync is on.
    // Then the landscape picture for N is put in place first, and Today is refreshed when the
    // picture it last showed this way round is not the one now in place (also 90 <-> 270, which
    // keeps the size). Without sync the theme's own picture is already right, and a Today refresh
    // would only flash the home screen.
    public static string Rotate(string box, string args)
    {
        string[] parts = args.Trim().Split(' ');
        int deg = int.Parse(parts[0]);
        bool sync = false;
        for (int i = 1; i < parts.Length; i++) if (parts[i] == "sync") sync = true;
        int w, h;
        Size(out w, out h);
        bool landscape = deg == 90 || deg == 270;
        bool changed = sync && landscape && Install(Landscape(box, deg), Name(h, w)) > 0;
        degrees = deg;
        string r = Display.Rotate(deg.ToString());
        if (!sync) return r;
        if (r.StartsWith("ok") && (changed || UserPicture() || ShownHere() != Hash(CurrentFile())))
        {
            Thread.Sleep(500);  // let the shell finish the turn first
            Refresh(Name(w, h));
        }
        return r;
    }

    // What the home screen last showed in portrait and in landscape (FNV of the picture file).
    static uint shownPortrait, shownLandscape;

    static bool IsLandscape()
    {
        var b = System.Windows.Forms.Screen.PrimaryScreen.Bounds;
        return b.Width > b.Height;
    }

    static string CurrentFile()
    {
        int w, h;
        Size(out w, out h);
        return IsLandscape() ? Name(h, w) : Name(w, h);
    }

    static uint ShownHere() { return IsLandscape() ? shownLandscape : shownPortrait; }

    static uint Hash(string path)
    {
        try
        {
            if (!File.Exists(path)) return 0;
            uint x = 2166136261;
            using (FileStream f = File.OpenRead(path))
            {
                byte[] b = new byte[16384];
                int n;
                while ((n = f.Read(b, 0, b.Length)) > 0)
                    for (int i = 0; i < n; i++) { x ^= b[i]; x *= 16777619; }
            }
            return x;
        }
        catch (Exception) { return 0; }
    }

    static void Size(out int w, out int h)
    {
        var b = System.Windows.Forms.Screen.PrimaryScreen.Bounds;
        w = Math.Min(b.Width, b.Height);
        h = Math.Max(b.Width, b.Height);
    }

    static string Name(int w, int h) { return @"\Windows\stwater_" + w + "_" + h + ".jpg"; }

    static string Landscape(string box, int deg)
    {
        string turned = Path.Combine(box, deg == 270 ? "wall_landscape_270.jpg" : "wall_landscape_90.jpg");
        return File.Exists(turned) ? turned : Path.Combine(box, "wall_landscape.jpg");
    }

    // Copies src over dst (the theme's read-only copy included), retrying while it is in use.
    // Returns 0 when dst already holds the same picture: then nothing may be refreshed. The lock
    // screen (in shell32.exe) loads its own copy of the wallpaper after every refresh and never frees
    // the old one (about 800 KB at 480x854). The app sends "wallpaper" on every fold, unfold and
    // start, so refreshing an unchanged picture used up shell32's 32 MB slot after a few dozen
    // locks; the lock screen then could not allocate its back buffer and its slide-in animation
    // left trails (and it kept showing an old wallpaper).
    static int Install(string src, string dst)
    {
        if (!File.Exists(src)) return 0;
        if (SameFile(src, dst)) return 0;
        for (int i = 0; i < 10; i++)
        {
            SetFileAttributes(dst, 0x80);  // FILE_ATTRIBUTE_NORMAL; fails harmlessly if dst is missing
            if (CopyFile(src, dst, false)) return 1;
            Thread.Sleep(300);
        }
        return 0;
    }

    static bool SameFile(string a, string b)
    {
        try
        {
            if (!File.Exists(b)) return false;
            using (FileStream fa = File.OpenRead(a), fb = File.OpenRead(b))
            {
                if (fa.Length != fb.Length) return false;
                byte[] ba = new byte[16384], bb = new byte[16384];
                int n;
                while ((n = fa.Read(ba, 0, ba.Length)) > 0)
                {
                    if (fb.Read(bb, 0, n) != n) return false;
                    for (int i = 0; i < n; i++) if (ba[i] != bb[i]) return false;
                }
                return true;
            }
        }
        catch (Exception) { return false; }  // unreadable: copy as before
    }

    static void Refresh(string portrait)
    {
        // First let shell32 drop its cached picture, then change the scenes' cache key and tell them.
        PostMessage(new IntPtr(0xFFFF), 0x001A /* WM_SETTINGCHANGE */, new IntPtr(0xF2), IntPtr.Zero);
        Thread.Sleep(1000);
        // Two key changes, always ending with no Wall (key = Skin): Wall = the picture, then gone.
        // Each change makes the scenes fetch the picture from shell32 again, and ending unset means
        // the cache key never names one orientation's file, so after a turn the shell's own
        // size-change reload shows the other orientation's new picture too. (Alternating one change
        // per refresh left Wall set every other time, and a landscape Today kept the old picture.)
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(Today))
            k.SetValue("Wall", portrait);
        PostMessage(new IntPtr(0xFFFF), 0x001A, new IntPtr(0xF2), IntPtr.Zero);
        Thread.Sleep(1000);
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(Today))
            if (k.GetValue("Wall") != null) k.DeleteValue("Wall");
        PostMessage(new IntPtr(0xFFFF), 0x001A, new IntPtr(0xF2), IntPtr.Zero);
        // Only this orientation's picture is on screen now; the other is refreshed on the next turn.
        uint shown = Hash(CurrentFile());
        if (IsLandscape()) shownLandscape = shown; else shownPortrait = shown;
    }

    // WM 2003 SE draws \Windows\stwater.gif (and stwater*.gif) behind the Start menu and
    // \Windows\tdywater.gif on Today. Today\Wall is the picture the user picked; the shell
    // copies that role onto \Windows\tdycust.gif. It wants GIF, so the JPEG from the phone
    // is rewritten as a 256-colour GIF.
    static string ApplyCe42(string box)
    {
        int w, h;
        Size(out w, out h);
        string portrait = Path.Combine(box, "wall_portrait.jpg");
        string src = File.Exists(portrait) ? portrait : Landscape(box, degrees);
        if (!File.Exists(src)) return "no pictures";
        string cust = @"\Windows\tdycust.gif";
        int n = 0;
        try
        {
            using (Bitmap bmp = new Bitmap(src))
            {
                if (WriteGif(bmp, cust)) n++;
                if (WriteGif(bmp, @"\Windows\stwater.gif")) n++;
                if (WriteGif(bmp, @"\Windows\tdywater.gif")) n++;
                if (WriteGif(bmp, @"\Windows\stwater_" + w + "_" + h + ".gif")) n++;
            }
        }
        catch (Exception e)
        {
            Helper.Log("wallpaper ce42 " + e.Message);
            return "failed";
        }
        using (RegistryKey k = Registry.CurrentUser.CreateSubKey(Today))
        {
            k.SetValue("Wall", cust);
            k.SetValue("Watermark", @"\Windows\stwater.gif");
        }
        PostMessage(new IntPtr(0xFFFF), 0x001A /* WM_SETTINGCHANGE */, new IntPtr(0xF2), IntPtr.Zero);
        PostMessage(new IntPtr(0xFFFF), 0x001A, IntPtr.Zero, IntPtr.Zero);
        return n > 0 ? "ok " + n : "failed";
    }

    // GIF89a, 3-3-2 palette. LZW stays at 9-bit codes: a clear follows every 254 pixels.
    static bool WriteGif(Bitmap bmp, string path)
    {
        SetFileAttributes(path, 0x80);
        int w = bmp.Width, h = bmp.Height;
        MemoryStream o = new MemoryStream();
        byte[] magic = Encoding.ASCII.GetBytes("GIF89a");
        o.Write(magic, 0, 6);
        GifWord(o, w); GifWord(o, h);
        o.WriteByte(0xF7); o.WriteByte(0); o.WriteByte(0);
        for (int i = 0; i < 256; i++)
        {
            o.WriteByte((byte)(((i >> 5) & 7) * 255 / 7));
            o.WriteByte((byte)(((i >> 2) & 7) * 255 / 7));
            o.WriteByte((byte)((i & 3) * 255 / 3));
        }
        o.WriteByte(0x2C);
        GifWord(o, 0); GifWord(o, 0); GifWord(o, w); GifWord(o, h);
        o.WriteByte(0);
        o.WriteByte(8);
        MemoryStream data = new MemoryStream();
        GifBits bits = new GifBits(data);
        bits.Write(256);
        int run = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                Color c = bmp.GetPixel(x, y);
                int idx = ((c.R >> 5) << 5) | ((c.G >> 5) << 2) | (c.B >> 6);
                if (run == 254) { bits.Write(256); run = 0; }
                bits.Write(idx);
                run++;
            }
        bits.Write(257);
        bits.Flush();
        byte[] d = data.ToArray();
        for (int i = 0; i < d.Length; i += 255)
        {
            int n = Math.Min(255, d.Length - i);
            o.WriteByte((byte)n);
            o.Write(d, i, n);
        }
        o.WriteByte(0);
        o.WriteByte(0x3B);
        byte[] all = o.ToArray();
        using (FileStream f = File.Create(path)) f.Write(all, 0, all.Length);
        return true;
    }

    static void GifWord(Stream s, int v) { s.WriteByte((byte)v); s.WriteByte((byte)(v >> 8)); }

    class GifBits
    {
        MemoryStream data;
        int acc, bits;
        public GifBits(MemoryStream d) { data = d; }
        public void Write(int c)
        {
            acc |= c << bits;
            bits += 9;
            while (bits >= 8) { data.WriteByte((byte)acc); acc >>= 8; bits -= 8; }
        }
        public void Flush() { if (bits > 0) data.WriteByte((byte)acc); }
    }

    [DllImport("coredll.dll", SetLastError = true)] static extern bool SetFileAttributes(string name, uint attr);
    [DllImport("coredll.dll", SetLastError = true)] static extern bool CopyFile(string src, string dst, bool failIfExists);
    [DllImport("coredll.dll")] static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);
}
