using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;

// Mirrors the Android phone's radio and Wi-Fi state into the SNAPI values the WM 6.5 title bar and
// Home screen read (HKLM\System\State). The phone stack (ossvcs) rewrites them on every radio event,
// so Apply() runs on each mailbox tick and re-asserts any value that differs.
static class PhoneState
{
    const string PhoneKey = @"System\State\Phone";
    const string HardwareKey = @"System\State\Hardware";

    // Status bits we own; call-related bits are left to the phone stack.
    const int NoSim = 0x02, RadioOff = 0x10, RadioPresent = 0x20, Roaming = 0x200;
    const int Phone1xRttCoverage = 0x10000000;
    const int GprsCoverage = 0x100000, NoService = 0x200000, Searching = 0x400000, Home = 0x800000;
    const int OwnedMask = NoSim | RadioOff | RadioPresent | Roaming | GprsCoverage | NoService | Searching | Home | Phone1xRttCoverage;

    static bool active;
    static int status, signal, signalRaw, available, connected, wifi;
    static string op = "";

    // status svc=0|1 sig=0..4 roam=0|1 data=H|3G|E|G|EV|1X| wifi=0|1 wificonn=0|1 wifisig=0..4 [num=<mccmnc>] op=<name>
    public static string Set(string args)
    {
        bool svc = false, roam = false, wifiOn = false, wifiConn = false;
        int sig = 0;
        string data = "", num = "";
        op = "";
        int opAt = args.IndexOf("op=");
        if (opAt >= 0) { op = args.Substring(opAt + 3).Trim(); args = args.Substring(0, opAt); }
        foreach (string part in args.Split(' '))
        {
            int eq = part.IndexOf('=');
            if (eq <= 0) continue;
            string k = part.Substring(0, eq), v = part.Substring(eq + 1);
            switch (k)
            {
                case "svc": svc = v == "1"; break;
                case "sig": sig = int.Parse(v); break;
                case "roam": roam = v == "1"; break;
                case "data": data = v; break;
                case "wifi": wifiOn = v == "1"; break;
                case "wificonn": wifiConn = v == "1"; break;
                case "num": num = v; break;
            }
        }
        if (sig < 0) sig = 0;
        if (sig > 4) sig = 4;
        status = RadioPresent | (svc ? Home : NoService) | (svc && roam ? Roaming : 0);
        int bit = DataBit(data);
        // The taskbar picks its data-letter table by GprsCoverage: set, it only knows G/E/3G/H; clear, it
        // uses the CDMA table (1X, Ev, ...). So CDMA data must leave GprsCoverage clear; Phone1xRttCoverage
        // is the matching SNAPI bit (the taskbar ignores it). docs/research-status-indicators.md 2.1.
        // WM 5's taskbar reads Phone1xRttCoverage for its 1X letter (section 7).
        if (svc && bit != 0) status |= (bit & 0x26) != 0 ? Phone1xRttCoverage : GprsCoverage;
        signal = svc ? new int[] { 10, 30, 60, 90, 100 }[sig] : 10;
        signalRaw = svc ? sig * 25 : 0;
        // WM 5 has no 3G or H letter: its taskbar shows E for Available bit 0x08, Ev for 0x04, else G
        // (GprsCoverage) or 1X (Phone1xRttCoverage). So UMTS/HSPA (and LTE/NR, sent as H) show E there.
        if (Wm5 && (bit & 0x50) != 0) bit |= 0x08;
        available = svc ? bit : 0;
        connected = available;
        wifi = wifiOn ? 0x03 | (wifiConn ? 0x10 : 0) : 0x01;
        cellularData = available != 0;
        wiredTicks = 0;
        active = true;
        NameOperator(num);
        Apply();
        if (Wm5) CarrierLogo.Show(svc ? op : "");
        return "ok";
    }

    // Windows Mobile 5 (CE 5.1); WM 6.x is CE 5.2.
    internal static readonly bool Wm5 = Environment.OSVersion.Version.Major == 5 && Environment.OSVersion.Version.Minor < 2;

    // WMDroid's internet is the emulated NE2000 card. While it is up, netui tells the taskbar a wired
    // network is connected (HHTaskBar message 0x480, wParam 1), and the taskbar then draws its
    // two-arrow icon instead of any data letter. When the phone reports mobile data, that connection stands for
    // it, so clear the flag (wParam 0); repeat now and then because netui sends 1 again on its events.
    // Fallback only: the system patch stops ethman.dll, the driver behind netui's wired UI
    // (Patch.NoWiredUi), and then nothing sets the flag and nothing needs clearing.
    const int WM_WIREDNETWORK = 0x480;
    static bool cellularData;
    static int wiredTicks;
    static int wiredUi = -1;  // ethman still loads (1), checked once per helper run

    static void ShowCellularLetter()
    {
        if (wiredUi < 0) wiredUi = Patch.WiredUiOff ? 0 : 1;
        if (wiredUi == 0) return;
        if (!cellularData || wiredTicks++ % 20 != 0) return;  // Apply runs every 0.5 s: about every 10 s
        IntPtr bar = FindWindow("HHTaskBar", null);
        if (bar != IntPtr.Zero) PostMessage(bar, WM_WIREDNETWORK, IntPtr.Zero, IntPtr.Zero);
    }

    [DllImport("coredll.dll", EntryPoint = "FindWindowW")] static extern IntPtr FindWindow(string cls, string name);
    [DllImport("coredll.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);

    static int DataBit(string d)
    {
        switch (d)
        {
            case "G": return 0x01;
            case "1X": return 0x02;
            case "EV": return 0x04;
            case "E": return 0x08;
            case "3G": return 0x10;
            case "H": return 0x40;
            default: return 0;
        }
    }


    // rilgsm images: rilgsm names the network from its own 2007 table or from
    // Software\Microsoft\RIL\OperatorNames (value name = MCC+MNC). The phone's operator name goes
    // there, and the app has the virtual modem announce the registration again so the RIL re-reads it.
    static string namedNumeric = "";

    static void NameOperator(string numeric)
    {
        if (!Patch.ModemRil || numeric.Length < 5 || op.Length == 0 || numeric + "=" + op == namedNumeric) return;
        try
        {
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"Software\Microsoft\RIL\OperatorNames"))
                if (!op.Equals(k.GetValue(numeric) as string)) { k.SetValue(numeric, op); Helper.Send("radio refresh"); }
            namedNumeric = numeric + "=" + op;
        }
        catch (Exception e) { Helper.Log("operator name " + e.Message); }
    }

    public static void Apply()
    {
        if (!active) return;
        try
        {
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(PhoneKey))
            {
                int cur = Dword(k, "Status");
                // rilgsm images: radio, signal and operator come through the RIL from the virtual
                // modem (Patch.ModemRil); only the data-letter values are the helper's.
                int owned = Patch.ModemRil ? GprsCoverage | Phone1xRttCoverage : OwnedMask;
                int want = (cur & ~owned) | (status & owned);
                if (cur != want) k.SetValue("Status", want);
                if (!Patch.ModemRil) Put(k, "Signal Strength", signal);
                if (!Patch.ModemRil) Put(k, "Signal Strength Raw", signalRaw);
                Put(k, "Cellular System Available", available);
                Put(k, "Cellular System Connected", connected);
                if (op.Length > 0 && (status & Home) != 0 && !Patch.ModemRil)
                {
                    if (!op.Equals(k.GetValue("Current Operator Name") as string)) k.SetValue("Current Operator Name", op);
                }
            }
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(HardwareKey)) Put(k, "WiFi", wifi);
            ShowCellularLetter();
        }
        catch (Exception e) { Helper.Log("status " + e.Message); }
    }

    static int Dword(RegistryKey k, string name)
    {
        object o = k.GetValue(name);
        return o is int ? (int)o : 0;
    }

    static void Put(RegistryKey k, string name, int v)
    {
        object o = k.GetValue(name);
        if (!(o is int) || (int)o != v) k.SetValue(name, v);
    }

    // Locks the device the way the Start menu's Lock item does.
    [DllImport("aygshell.dll")] static extern int SHDeviceLockAndPrompt();

    // The shell locks on VK_KEYLOCK (F22), which the keyboard driver can't produce but an
    // injected key event can. (SHDeviceLockAndPrompt returns success from here but shows nothing.)
    public static string Lock()
    {
        keybd_event(0x85, 0, 0, 0);
        keybd_event(0x85, 0, 2 /* KEYEVENTF_KEYUP */, 0);
        return "ok";
    }

    [DllImport("coredll.dll")] static extern void keybd_event(byte vk, byte scan, int flags, int extra);
}

// WM 5's Phone app (cprog) never shows the operator as text: its branding area draws the picture
// \Windows\carrierlogo.gif, which the ROM ships as a red "Mobile Operator" placeholder (220x34). A file
// written over it in \Windows shadows the ROM copy, so the helper draws the phone's operator name the
// same way; deleting the RAM copy brings the ROM placeholder back. docs/research-status-indicators.md 7.
static class CarrierLogo
{
    const string Path = @"\Windows\carrierlogo.gif";
    const int W = 220, H = 34;
    static string shown;  // what this helper run last wrote ("" = the ROM placeholder)

    public static void Show(string op)
    {
        if (op == shown) return;
        try
        {
            // The name drawn last time, so a restart doesn't rewrite the same picture.
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"Software\WMDroid"))
            {
                if (shown == null && op == (k.GetValue("CarrierLogo") as string ?? "") && (op.Length == 0 || System.IO.File.Exists(Path)))
                {
                    shown = op;
                    return;
                }
                if (op.Length == 0)
                {
                    if (System.IO.File.Exists(Path)) try { System.IO.File.Delete(Path); } catch (Exception) { }  // the ROM copy stays
                }
                else
                    using (var f = System.IO.File.Create(Path)) Gif(Draw(op), f);
                k.SetValue("CarrierLogo", op);
            }
            shown = op;
        }
        catch (Exception e) { Helper.Log("carrier logo " + e.Message); }
    }

    // The name in the placeholder's red and size, shrunk to fit; true = text pixel.
    static bool[] Draw(string text)
    {
        var px = new bool[W * H];
        using (var bmp = new System.Drawing.Bitmap(W, H))
        using (var g = System.Drawing.Graphics.FromImage(bmp))
        {
            g.Clear(System.Drawing.Color.White);
            float size = 10;
            System.Drawing.Font font = null;
            System.Drawing.SizeF sz;
            while (true)
            {
                font = new System.Drawing.Font("Tahoma", size, System.Drawing.FontStyle.Regular);
                sz = g.MeasureString(text, font);
                if (sz.Width <= W - 4 || size <= 5) break;
                font.Dispose();
                size -= 0.5f;
            }
            using (font)
            using (var brush = new System.Drawing.SolidBrush(System.Drawing.Color.Black))
                g.DrawString(text, font, brush, 2, Math.Max(0, (H - sz.Height) / 2));
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    System.Drawing.Color c = bmp.GetPixel(x, y);
                    px[y * W + x] = c.R + c.G + c.B < 384;
                }
        }
        return px;
    }

    // GIF89a, 256-colour table (0 = transparent background, 1 = text), LZW without compression: every pixel
    // is a 9-bit literal and a clear code follows every 254 of them, so the code size never grows.
    static void Gif(bool[] px, System.IO.Stream s)
    {
        var o = new System.IO.MemoryStream();
        Bytes(o, "GIF89a");
        Word(o, W); Word(o, H);
        o.WriteByte(0xF7); o.WriteByte(0); o.WriteByte(0);  // global table of 256 entries
        for (int i = 0; i < 256; i++)
        {
            if (i == 1) { o.WriteByte(197); o.WriteByte(39); o.WriteByte(13); }
            else { o.WriteByte(255); o.WriteByte(255); o.WriteByte(255); }
        }
        o.Write(new byte[] { 0x21, 0xF9, 4, 1, 0, 0, 0, 0 }, 0, 8);  // graphic control: index 0 transparent
        o.WriteByte(0x2C); Word(o, 0); Word(o, 0); Word(o, W); Word(o, H); o.WriteByte(0);
        o.WriteByte(8);  // LZW minimum code size
        var data = new System.IO.MemoryStream();
        int acc = 0, bits = 0, run = 0;
        Action<int> code = delegate (int c)
        {
            acc |= c << bits;
            bits += 9;
            while (bits >= 8) { data.WriteByte((byte)acc); acc >>= 8; bits -= 8; }
        };
        code(256);
        foreach (bool p in px)
        {
            if (run == 254) { code(256); run = 0; }
            code(p ? 1 : 0);
            run++;
        }
        code(257);
        if (bits > 0) data.WriteByte((byte)acc);
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
        s.Write(all, 0, all.Length);
    }

    static void Word(System.IO.Stream o, int v) { o.WriteByte((byte)v); o.WriteByte((byte)(v >> 8)); }

    static void Bytes(System.IO.Stream o, string a) { foreach (char c in a) o.WriteByte((byte)c); }
}
