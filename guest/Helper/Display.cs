using System;
using System.Runtime.InteropServices;

// Turns the WM display (the way G-sensor tools on 2009 phones did): the app passes the phone's
// orientation as degrees counter-clockwise.
static class Display
{
    const int DM_DISPLAYORIENTATION = 0x00800000;
    const int CDS_RESET = 0x40000000;
    const int CDS_TEST = 0x00000002;
    const int DM_DISPLAYQUERYORIENTATION = 0x01000000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public short dmOrientation, dmPaperSize, dmPaperLength, dmPaperWidth, dmScale, dmCopies,
            dmDefaultSource, dmPrintQuality, dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmDisplayOrientation, dmDisplayQuery;
    }

    [DllImport("coredll.dll")]
    static extern int ChangeDisplaySettingsEx(string device, ref DEVMODE mode, IntPtr hwnd, int flags, IntPtr param);

    // rotate 0|90|180|270
    public static string Rotate(string args)
    {
        int deg = int.Parse(args.Trim());
        int dmdo;
        switch (deg)
        {
            case 90: dmdo = 1; break;   // DMDO_90
            case 180: dmdo = 2; break;  // DMDO_180
            case 270: dmdo = 4; break;  // DMDO_270
            default: dmdo = 0; break;
        }
        var dm = new DEVMODE();
        dm.dmDeviceName = "";
        dm.dmFormName = "";
        dm.dmFields = DM_DISPLAYORIENTATION;
        dm.dmDisplayOrientation = dmdo;
        int r = -1;
        foreach (short size in new short[] { 192, (short)Marshal.SizeOf(typeof(DEVMODE)) })
        {
            dm.dmSize = size;
            // CDS_RESET applies the angle again when the driver already claims to be there. A
            // restored session often is: the saved orientation and the panel disagree, and a plain
            // change is ignored, so turning back to the way the phone is held does nothing.
            r = ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, CDS_RESET, IntPtr.Zero);
            if (r == 0) return "ok size=" + size;
        }
        // Report which orientations the display driver offers (DMDO_* bits) to diagnose it.
        var q = new DEVMODE();
        q.dmDeviceName = "";
        q.dmFormName = "";
        q.dmSize = dm.dmSize;
        q.dmFields = DM_DISPLAYQUERYORIENTATION;
        int qr = ChangeDisplaySettingsEx(null, ref q, IntPtr.Zero, CDS_TEST, IntPtr.Zero);
        return "failed " + r + " size=" + dm.dmSize + " query=" + qr + " supported=" + q.dmDisplayQuery;
    }
}
