using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

// GPS Test: reads the GPS Intermediate Driver (the phone's GPS through WMDroid) and shows the fix,
// a sky view of the satellites in view and their signal strengths.
class GpsTest : Form
{
    [MTAThread]
    static void Main() { Application.Run(new GpsTest()); }

    readonly Timer timer = new Timer();
    readonly MainMenu menu = new MainMenu();
    IntPtr device;
    Bitmap buffer;
    Pos pos;
    string error = "";

    // GPS_POSITION fields we use (gpsapi.h).
    class Pos
    {
        public int valid, fixQuality, fixType, satCount, inView;
        public double lat, lon;
        public float speed, heading, altitude, hdop, vdop, pdop;
        public DateTime utc;
        public int[] used = new int[12], prn = new int[12], elev = new int[12], azim = new int[12], snr = new int[12];
    }

    const int SIZE = 344;
    const int VALID_TIME = 0x1, VALID_LAT = 0x2, VALID_LON = 0x4, VALID_SPEED = 0x8, VALID_HEADING = 0x10,
        VALID_ALT = 0x40, VALID_PDOP = 0x100, VALID_HDOP = 0x200, VALID_VDOP = 0x400, VALID_SATCOUNT = 0x800,
        VALID_INVIEW = 0x2000;

    [DllImport("gpsapi.dll")] static extern IntPtr GPSOpenDevice(IntPtr newLocation, IntPtr stateChange, string name, int flags);
    [DllImport("gpsapi.dll")] static extern int GPSCloseDevice(IntPtr device);
    [DllImport("gpsapi.dll")] static extern int GPSGetPosition(IntPtr device, IntPtr pos, int maxAgeMs, int flags);

    GpsTest()
    {
        Text = "GPS Test";
        BackColor = Color.Black;
        var exit = new MenuItem { Text = "Exit" };
        exit.Click += delegate { Close(); };
        menu.MenuItems.Add(exit);
        Menu = menu;
        device = GPSOpenDevice(IntPtr.Zero, IntPtr.Zero, null, 0);
        if (device == IntPtr.Zero) error = "Could not open the GPS Intermediate Driver";
        timer.Interval = 1000;
        timer.Tick += delegate { Poll(); Invalidate(); };
        timer.Enabled = true;
        Poll();
    }

    protected override void OnClosed(EventArgs e)
    {
        timer.Enabled = false;
        if (device != IntPtr.Zero) GPSCloseDevice(device);
        base.OnClosed(e);
    }

    void Poll()
    {
        if (device == IntPtr.Zero) return;
        IntPtr p = Marshal.AllocHGlobal(SIZE + 32);
        try
        {
            byte[] zero = new byte[SIZE + 32];
            Marshal.Copy(zero, 0, p, zero.Length);
            Marshal.WriteInt32(p, 0, 1);     // GPS_VERSION_1
            Marshal.WriteInt32(p, 4, SIZE);
            int r = GPSGetPosition(device, p, 5000, 0);
            if (r != 0) { error = "GPSGetPosition error " + r; return; }
            error = "";
            var q = new Pos();
            q.valid = Marshal.ReadInt32(p, 8);
            if ((q.valid & VALID_TIME) != 0)
            {
                try
                {
                    q.utc = new DateTime(Marshal.ReadInt16(p, 16), Marshal.ReadInt16(p, 18), Marshal.ReadInt16(p, 22),
                        Marshal.ReadInt16(p, 24), Marshal.ReadInt16(p, 26), Marshal.ReadInt16(p, 28));
                }
                catch (ArgumentException) { }
            }
            q.lat = Double(p, 32);
            q.lon = Double(p, 40);
            q.speed = Float(p, 48);
            q.heading = Float(p, 52);
            q.altitude = Float(p, 64);
            q.fixQuality = Marshal.ReadInt32(p, 72);
            q.fixType = Marshal.ReadInt32(p, 76);
            q.pdop = Float(p, 84);
            q.hdop = Float(p, 88);
            q.vdop = Float(p, 92);
            q.satCount = Marshal.ReadInt32(p, 96);
            for (int i = 0; i < 12; i++) q.used[i] = Marshal.ReadInt32(p, 100 + i * 4);
            q.inView = Math.Min(12, Marshal.ReadInt32(p, 148));
            for (int i = 0; i < 12; i++)
            {
                q.prn[i] = Marshal.ReadInt32(p, 152 + i * 4);
                q.elev[i] = Marshal.ReadInt32(p, 200 + i * 4);
                q.azim[i] = Marshal.ReadInt32(p, 248 + i * 4);
                q.snr[i] = Marshal.ReadInt32(p, 296 + i * 4);
            }
            pos = q;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    static double Double(IntPtr p, int off)
    {
        byte[] b = new byte[8];
        Marshal.Copy(new IntPtr(p.ToInt32() + off), b, 0, 8);
        return BitConverter.ToDouble(b, 0);
    }

    static float Float(IntPtr p, int off)
    {
        byte[] b = new byte[4];
        Marshal.Copy(new IntPtr(p.ToInt32() + off), b, 0, 4);
        return BitConverter.ToSingle(b, 0);
    }

    bool Used(int prn)
    {
        if (pos == null) return false;
        for (int i = 0; i < Math.Min(12, pos.satCount); i++) if (pos.used[i] == prn) return true;
        return false;
    }

    protected override void OnPaintBackground(PaintEventArgs e) { }

    protected override void OnPaint(PaintEventArgs e)
    {
        int w = ClientSize.Width, h = ClientSize.Height;
        if (buffer == null || buffer.Width != w || buffer.Height != h)
        {
            if (buffer != null) buffer.Dispose();
            buffer = new Bitmap(w, h);
        }
        using (Graphics g = Graphics.FromImage(buffer)) Draw(g, w, h);
        e.Graphics.DrawImage(buffer, 0, 0);
    }

    void Draw(Graphics g, int w, int h)
    {
        g.Clear(Color.Black);
        bool wide = w > h;
        using (var big = new Font("Tahoma", 11, FontStyle.Bold))
        using (var small = new Font("Tahoma", 8, FontStyle.Regular))
        using (var white = new SolidBrush(Color.White))
        using (var gray = new SolidBrush(Color.Gray))
        using (var green = new SolidBrush(Color.FromArgb(80, 220, 100)))
        using (var amber = new SolidBrush(Color.FromArgb(240, 190, 60)))
        using (var line = new Pen(Color.FromArgb(70, 70, 80)))
        {
            int pad = w / 40;
            // Fix and readouts.
            string fix = pos == null ? "Waiting for GPS" :
                pos.fixType == 2 ? "3D fix" : pos.fixType == 1 ? "2D fix" : "Searching";
            if (pos != null) fix += "   " + pos.satCount + " used / " + pos.inView + " in view";
            g.DrawString(error.Length > 0 ? error : fix, big, error.Length > 0 ? amber : (pos != null && pos.fixType > 0 ? green : amber), pad, pad);
            int y = pad + (int)(big.Size * 3.2f);
            string[] rows = Readouts();
            int textW = wide ? w / 2 : w;
            for (int i = 0; i < rows.Length; i++)
            {
                int col = wide ? 0 : (i % 2) * (w / 2);
                int row = wide ? i : i / 2;
                g.DrawString(rows[i], small, white, pad + col, y + row * (int)(small.Size * 2.4f));
            }
            int textRows = wide ? rows.Length : (rows.Length + 1) / 2;
            int top = y + textRows * (int)(small.Size * 2.4f) + pad;

            // Sky view: horizon outer ring, 30 and 60 degrees inside; north up.
            int barsH = h / 5;
            int skyArea = wide ? Math.Min(w / 2, h - pad * 2) : Math.Min(w - pad * 2, h - top - barsH - pad * 2);
            int r = Math.Max(20, skyArea / 2 - pad);
            int cx = wide ? w * 3 / 4 : w / 2;
            int cy = wide ? h / 2 : top + r + pad;
            for (int k = 1; k <= 3; k++)
            {
                int rr = r * k / 3;
                g.DrawEllipse(line, cx - rr, cy - rr, rr * 2, rr * 2);
            }
            g.DrawLine(line, cx - r, cy, cx + r, cy);
            g.DrawLine(line, cx, cy - r, cx, cy + r);
            g.DrawString("N", small, gray, cx - 4, cy - r - (int)(small.Size * 1.8f));
            g.DrawString("S", small, gray, cx - 4, cy + r);
            g.DrawString("E", small, gray, cx + r + 2, cy - small.Size);
            g.DrawString("W", small, gray, cx - r - (int)(small.Size * 1.6f), cy - small.Size);
            if (pos != null)
            {
                int dot = Math.Max(6, r / 9);
                for (int i = 0; i < pos.inView; i++)
                {
                    if (pos.prn[i] == 0) continue;
                    double dist = r * (90 - Math.Max(0, Math.Min(90, pos.elev[i]))) / 90.0;
                    double a = pos.azim[i] * Math.PI / 180;
                    int sx = cx + (int)(dist * Math.Sin(a)), sy = cy - (int)(dist * Math.Cos(a));
                    Brush b = Used(pos.prn[i]) ? green : pos.snr[i] > 0 ? amber : gray;
                    g.FillEllipse(b, sx - dot, sy - dot, dot * 2, dot * 2);
                    g.DrawString(pos.prn[i].ToString(), small, white, sx + dot, sy - dot);
                }
            }

            // Signal bars.
            if (pos != null && pos.inView > 0)
            {
                int bx = pad, bw = (wide ? w / 2 : w) - pad * 2;
                int by = wide ? h - pad - barsH : h - pad - barsH;
                if (wide) by = Math.Max(top, by);
                int n = pos.inView, slot = bw / Math.Max(1, n);
                for (int i = 0; i < n; i++)
                {
                    int s = Math.Max(0, Math.Min(50, pos.snr[i]));
                    int bh = (barsH - (int)(small.Size * 2)) * s / 50;
                    Brush b = Used(pos.prn[i]) ? green : s > 0 ? amber : gray;
                    g.FillRectangle(b, bx + i * slot + 2, by + barsH - (int)(small.Size * 2) - bh, slot - 4, bh);
                    g.DrawString(pos.prn[i].ToString(), small, white, bx + i * slot + 2, by + barsH - (int)(small.Size * 1.8f));
                }
            }
        }
    }

    string[] Readouts()
    {
        if (pos == null) return new string[0];
        return new string[] {
            "Lat  " + (Has(VALID_LAT) ? Deg(pos.lat, "N", "S") : "-"),
            "Lon  " + (Has(VALID_LON) ? Deg(pos.lon, "E", "W") : "-"),
            "Alt  " + (Has(VALID_ALT) ? pos.altitude.ToString("0") + " m" : "-"),
            "Speed  " + (Has(VALID_SPEED) ? (pos.speed * 1.852).ToString("0.0") + " km/h" : "-"),
            "Heading  " + (Has(VALID_HEADING) ? pos.heading.ToString("0") + "°" : "-"),
            "HDOP  " + (Has(VALID_HDOP) ? pos.hdop.ToString("0.0") : "-") + "  VDOP  " + (Has(VALID_VDOP) ? pos.vdop.ToString("0.0") : "-"),
            "UTC  " + (Has(VALID_TIME) && pos.utc.Year > 2000 ? pos.utc.ToString("HH:mm:ss") : "-"),
            "PDOP  " + (Has(VALID_PDOP) ? pos.pdop.ToString("0.0") : "-"),
        };
    }

    bool Has(int field) { return (pos.valid & field) != 0; }

    static string Deg(double v, string pos, string neg)
    {
        return Math.Abs(v).ToString("0.00000") + "° " + (v >= 0 ? pos : neg);
    }
}
