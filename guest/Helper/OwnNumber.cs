using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// The device's own phone number on FakeRIL images (WM 5, WM 6.5.3).
//
// Commands (Helper.Execute):
//   number probe          what the RIL reports: RIL_GetSubscriberNumbers and the SIM's MSISDN file (EF 6F40)
static class OwnNumber
{
    delegate void RilResultCb(uint code, int cmdId, IntPtr data, uint cb, uint param);
    delegate void RilNotifyCb(uint code, IntPtr data, uint cb, uint param);
    static RilResultCb resultCb;
    static RilNotifyCb notifyCb;
    static IntPtr ril;
    static readonly AutoResetEvent done = new AutoResetEvent(false);
    static int waitId;
    static uint lastCode;
    static byte[] lastData;

    public static string Command(string rest)
    {
        try
        {
            if (rest == "probe") return Probe();
            if (rest.StartsWith("sim ")) return WriteSim(rest.Substring(4).Trim()) + " | " + Probe();
            if (rest == "radio") return Radio();
            return "bad args";
        }
        catch (Exception e) { return "failed " + e.GetType().Name + " " + e.Message; }
    }

    static bool Open()
    {
        if (ril != IntPtr.Zero) return true;
        resultCb = OnResult;
        notifyCb = delegate { };
        IntPtr h;
        int hr = RIL_Initialize(1, resultCb, notifyCb, 0, 0, out h);
        if (hr < 0) { Helper.Log("number: RIL_Initialize 0x" + hr.ToString("X8")); return false; }
        ril = h;
        return true;
    }

    static void OnResult(uint code, int cmdId, IntPtr data, uint cb, uint param)
    {
        var copy = new byte[cb];
        if (cb > 0 && data != IntPtr.Zero) Marshal.Copy(data, copy, 0, (int)cb);
        lock (done) { waitId = cmdId; lastCode = code; lastData = copy; }
        done.Set();
    }

    // Issues a RIL request and waits for its result (FakeRIL answers after a simulated delay).
    static bool Call(int id, out uint code, out byte[] data)
    {
        code = 0; data = null;
        if (id <= 0) return false;
        int until = Environment.TickCount + 5000;
        while (Environment.TickCount - until < 0)
        {
            lock (done) if (waitId == id) { code = lastCode; data = lastData; return true; }
            done.WaitOne(200, false);
        }
        return false;
    }

    static string Probe()
    {
        if (!Open()) return "no ril";
        var sb = new StringBuilder();
        uint code; byte[] d;
        done.Reset();
        if (Call(RIL_GetSubscriberNumbers(ril), out code, out d))
        {
            sb.Append("subscriber code=").Append(code).Append(" bytes=").Append(d.Length);
            for (int o = 0; o + 1064 <= d.Length; o += 1064)
                sb.Append(" [").Append(Str(d, o + 0x18)).Append(" / ").Append(Str(d, o + 0x218)).Append(']');
        }
        else sb.Append("subscriber timeout");
        // READ RECORD (RIL_SIMCMD_READRECORD = 2) of EF MSISDN, record 1, 0x20 bytes
        var p = new int[] { 24, 0x0F, 0x6F40, 1, 4, 0x20 };
        done.Reset();
        if (Call(RIL_SendRestrictedSimCmd(ril, 2, p, IntPtr.Zero, 0), out code, out d))
        {
            sb.Append(" | 6F40 code=").Append(code).Append(' ');
            foreach (byte b in d) sb.Append(b.ToString("x2"));
        }
        else sb.Append(" | 6F40 timeout");
        return sb.ToString();
    }

    // EF MSISDN record 1 (GSM 11.11 10.5.5): alpha identifier (21 bytes here, FF padded), BCD length,
    // TON/NPI, 10 BCD bytes, capability and extension ids.
    static byte[] MsisdnRecord(string number, string alpha)
    {
        var r = new byte[35];
        for (int i = 0; i < r.Length; i++) r[i] = 0xFF;
        for (int i = 0; i < alpha.Length && i < 21; i++) r[i] = (byte)alpha[i];
        bool intl = number.StartsWith("+");
        var digits = new StringBuilder();
        foreach (char c in number) if ((c >= '0' && c <= '9') || c == '*' || c == '#') digits.Append(c);
        int n = Math.Min(digits.Length, 20);
        r[21] = (byte)(1 + (n + 1) / 2);
        r[22] = (byte)(intl ? 0x91 : 0x81);
        for (int i = 0; i < n; i++)
        {
            char c = digits[i];
            int v = c == '*' ? 0xA : c == '#' ? 0xB : c - '0';
            int at = 23 + i / 2;
            r[at] = (byte)(i % 2 == 0 ? (0xF0 | v) : ((r[at] & 0x0F) | (v << 4)));
        }
        return r;
    }

    static string WriteSim(string number)
    {
        if (!Open()) return "no ril";
        byte[] rec = MsisdnRecord(number, "My number");
        IntPtr buf = Marshal.AllocHGlobal(rec.Length);
        try
        {
            Marshal.Copy(rec, 0, buf, rec.Length);
            var p = new int[] { 24, 0x0F, 0x6F40, 1, 4, rec.Length };   // UPDATE RECORD, record 1, absolute
            uint code; byte[] d;
            done.Reset();
            if (!Call(RIL_SendRestrictedSimCmd(ril, 5, p, buf, rec.Length), out code, out d)) return "6F40 write timeout";
            var sb = new StringBuilder("6F40 write code=" + code + " ");
            foreach (byte b in d) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    // Radio off and on again (RIL_EQSTATE_MINIMUM 1 -> FULL 2), then registration.
    static string Radio()
    {
        if (!Open()) return "no ril";
        uint code; byte[] d;
        var sb = new StringBuilder();
        done.Reset();
        sb.Append("off ").Append(Call(RIL_SetEquipmentState(ril, 1), out code, out d) ? code.ToString() : "timeout");
        Thread.Sleep(2000);
        sb.Append(" on ").Append(Call(RIL_SetEquipmentState(ril, 2), out code, out d) ? code.ToString() : "timeout");
        Thread.Sleep(2000);
        sb.Append(" register ").Append(Call(RIL_RegisterOnNetwork(ril, 1, IntPtr.Zero), out code, out d) ? code.ToString() : "timeout");
        return sb.ToString();
    }

    static string Str(byte[] d, int o)
    {
        var s = new StringBuilder();
        for (int i = o; i + 1 < d.Length && i < o + 512; i += 2)
        {
            char c = (char)(d[i] | d[i + 1] << 8);
            if (c == 0) break;
            s.Append(c);
        }
        return s.ToString();
    }

    [DllImport("ril.dll")] static extern int RIL_Initialize(uint index, RilResultCb result, RilNotifyCb notify, uint classes, uint param, out IntPtr hRil);
    [DllImport("ril.dll")] static extern int RIL_GetSubscriberNumbers(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_SetEquipmentState(IntPtr h, int state);
    [DllImport("ril.dll")] static extern int RIL_RegisterOnNetwork(IntPtr h, int mode, IntPtr op);
    [DllImport("ril.dll")] static extern int RIL_SendRestrictedSimCmd(IntPtr h, int command, int[] parameters, IntPtr data, int size);
}
