using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

// Infrared test tool (docs/research-irda.md): drives Windows Mobile's own IrDA stack through
// AF_IRDA sockets (System.Net.IrDA), so the IrLAP / IrLMP / TinyTP path can be checked between two
// emulated devices without tapping through the Beam UI. No window; it logs to
// \Storage Card\.wmdroid\irtest.log (each CLI instance has its own .wmdroid overlay).
//   IrTest.exe discover          list the devices in range (up to 10 tries)
//   IrTest.exe listen            accept one connection on the "PocketStylusTest" service and echo a summary
//   IrTest.exe send [bytes]      connect to the first device's service and send a test pattern
class IrTest
{
    const string Service = "PocketStylusTest";
    static string logPath = @"\Storage Card\.wmdroid\irtest.log";

    static void Log(string s)
    {
        try
        {
            using (var w = new StreamWriter(logPath, true)) w.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " " + s);
        }
        catch { }
    }

    static int Main(string[] args)
    {
        if (!Directory.Exists(Path.GetDirectoryName(logPath))) logPath = @"\irtest.log";
        string mode = args.Length > 0 ? args[0] : "discover";
        Log("start " + mode);
        try
        {
            if (mode == "listen") Listen();
            else if (mode == "send") Send(args.Length > 1 ? int.Parse(args[1]) : 4000);
            else Discover(10);
        }
        catch (Exception e) { Log("error " + e.GetType().Name + ": " + e.Message); }
        Log("end " + mode);
        return 0;
    }

    static IrDADeviceInfo[] Discover(int tries)
    {
        var c = new IrDAClient();
        for (int i = 0; i < tries; i++)
        {
            int t0 = Environment.TickCount;
            IrDADeviceInfo[] d = c.DiscoverDevices(8);
            var sb = new StringBuilder();
            foreach (IrDADeviceInfo x in d)
                sb.Append(" [").Append(x.DeviceName).Append(" ").Append(BitConverter.ToString(x.DeviceID)).Append(" hints=").Append((int)x.Hints).Append("]");
            Log("discover " + i + ": " + d.Length + " device(s) in " + (Environment.TickCount - t0) + " ms" + sb);
            if (d.Length > 0) return d;
            Thread.Sleep(1000);
        }
        return new IrDADeviceInfo[0];
    }

    static void Listen()
    {
        var l = new IrDAListener(Service);
        l.Start();
        Log("listening on " + Service);
        IrDAClient c = l.AcceptIrDAClient();
        Log("accepted from " + c.RemoteMachineName);
        Stream s = c.GetStream();
        byte[] buf = new byte[4096];
        int total = 0, sum = 0, n;
        // A 4-byte length, then the data (the CF's IrDAClient cannot half-close its socket).
        int want = 0;
        for (int i = 0; i < 4; i++) { int b = s.ReadByte(); if (b < 0) { Log("closed before the length"); return; } want |= b << (8 * i); }
        int t0 = Environment.TickCount;
        while (total < want && (n = s.Read(buf, 0, Math.Min(buf.Length, want - total))) > 0)
        {
            for (int i = 0; i < n; i++) sum = (sum * 31 + buf[i]) & 0x7FFFFFFF;
            total += n;
        }
        int ms = Environment.TickCount - t0;
        Log("received " + total + " bytes sum=" + sum + " in " + ms + " ms");
        byte[] reply = Encoding.ASCII.GetBytes("ok " + total + " " + sum);
        try { s.Write(reply, 0, reply.Length); } catch (Exception e) { Log("reply " + e.Message); }
        Thread.Sleep(500);
        c.Close();
        l.Stop();
    }

    static void Send(int count)
    {
        IrDADeviceInfo[] d = Discover(10);
        if (d.Length == 0) { Log("nobody in range"); return; }
        var c = new IrDAClient();
        int t0 = Environment.TickCount;
        c.Connect(new IrDAEndPoint(d[0].DeviceID, Service));
        Log("connected to " + d[0].DeviceName + " in " + (Environment.TickCount - t0) + " ms");
        Stream s = c.GetStream();
        byte[] data = new byte[count];
        int sum = 0;
        for (int i = 0; i < count; i++) { data[i] = (byte)(i * 7 + (i >> 8)); sum = (sum * 31 + data[i]) & 0x7FFFFFFF; }
        t0 = Environment.TickCount;
        s.Write(BitConverter.GetBytes(count), 0, 4);
        for (int o = 0; o < count; o += 1024) s.Write(data, o, Math.Min(1024, count - o));
        Log("sent " + count + " bytes sum=" + sum + " in " + (Environment.TickCount - t0) + " ms");
        byte[] buf = new byte[256];
        int n = s.Read(buf, 0, buf.Length);
        Log("reply: " + (n > 0 ? Encoding.ASCII.GetString(buf, 0, n) : "(none)") + " after " + (Environment.TickCount - t0) + " ms");
        c.Close();
    }
}
