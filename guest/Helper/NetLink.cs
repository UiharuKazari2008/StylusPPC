using System;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Text;
using System.Threading;

// Large host traffic, and short lines when the sideband page is not answering.
// The guest is 10.0.2.15; 10.0.2.2 is the phone itself (UserNet). Not the storage-card mailbox.
static class NetLink
{
    const string Host = "10.0.2.2";
    const int Port = 7378;
    const byte Line = 1, Blob = 2;

    static readonly object writeLock = new object();
    static readonly object queueLock = new object();
    static TcpClient client;
    static NetworkStream stream;
    static Thread thread;
    static bool stop;
    static List<string> lines = new List<string>();
    static List<string> blobNames = new List<string>();
    static List<byte[]> blobData = new List<byte[]>();

    public static void Start()
    {
        if (thread != null) return;
        stop = false;
        thread = new Thread(Run);
        thread.IsBackground = true;
        thread.Start();
    }

    public static void Stop()
    {
        stop = true;
        lock (writeLock) { Close(); }
    }

    public static string[] TakeLines()
    {
        lock (queueLock)
        {
            string[] got = lines.ToArray();
            lines.Clear();
            return got;
        }
    }

    // The next blob of this name, or null. A blob is stored before the line that follows it.
    public static byte[] TakeBlob(string name)
    {
        lock (queueLock)
        {
            for (int i = 0; i < blobNames.Count; i++)
            {
                if (blobNames[i] != name) continue;
                byte[] data = blobData[i];
                blobNames.RemoveAt(i);
                blobData.RemoveAt(i);
                return data;
            }
        }
        return null;
    }

    // True only when the bytes were written to a live connection.
    public static bool SendLine(string line)
    {
        if (line == null) return false;
        return Write(Line, Encoding.UTF8.GetBytes(line));
    }

    public static bool SendBlob(string name, byte[] data)
    {
        if (name == null || data == null) return false;
        byte[] nb = Encoding.UTF8.GetBytes(name);
        if (nb.Length > 200) return false;
        byte[] payload = new byte[2 + nb.Length + data.Length];
        payload[0] = (byte)nb.Length;
        payload[1] = (byte)(nb.Length >> 8);
        Buffer.BlockCopy(nb, 0, payload, 2, nb.Length);
        Buffer.BlockCopy(data, 0, payload, 2 + nb.Length, data.Length);
        return Write(Blob, payload);
    }

    static bool Write(byte type, byte[] payload)
    {
        lock (writeLock)
        {
            if (stream == null) return false;
            try
            {
                byte[] hdr = new byte[5];
                int n = payload.Length;
                hdr[0] = (byte)n;
                hdr[1] = (byte)(n >> 8);
                hdr[2] = (byte)(n >> 16);
                hdr[3] = (byte)(n >> 24);
                hdr[4] = type;
                stream.Write(hdr, 0, 5);
                if (n > 0) stream.Write(payload, 0, n);
                return true;
            }
            catch (Exception)
            {
                Close();
                return false;
            }
        }
    }

    static void Run()
    {
        byte[] hdr = new byte[5];
        while (!stop)
        {
            try
            {
                TcpClient c = new TcpClient();
                c.Connect(Host, Port);
                c.NoDelay = true;
                NetworkStream s = c.GetStream();
                lock (writeLock) { Close(); client = c; stream = s; }
                while (!stop && ReadFull(s, hdr, 5))
                {
                    int n = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
                    if (n < 0 || n > 8 * 1024 * 1024) break;
                    byte[] payload = new byte[n];
                    if (n > 0 && !ReadFull(s, payload, n)) break;
                    Accept(hdr[4], payload);
                }
            }
            catch (Exception) { }
            lock (writeLock) { Close(); }
            if (!stop) Thread.Sleep(1000);
        }
    }

    static void Accept(byte type, byte[] payload)
    {
        lock (queueLock)
        {
            if (type == Line)
            {
                lines.Add(Encoding.UTF8.GetString(payload, 0, payload.Length));
                return;
            }
            if (type != Blob || payload.Length < 2) return;
            int nameLen = payload[0] | (payload[1] << 8);
            if (nameLen < 0 || 2 + nameLen > payload.Length) return;
            string name = Encoding.UTF8.GetString(payload, 2, nameLen);
            byte[] data = new byte[payload.Length - 2 - nameLen];
            Buffer.BlockCopy(payload, 2 + nameLen, data, 0, data.Length);
            blobNames.Add(name);
            blobData.Add(data);
        }
    }

    static bool ReadFull(NetworkStream s, byte[] buf, int n)
    {
        int o = 0;
        while (o < n)
        {
            int r = s.Read(buf, o, n - o);
            if (r <= 0) return false;
            o += r;
        }
        return true;
    }

    static void Close()
    {
        try { if (stream != null) stream.Close(); } catch (Exception) { }
        try { if (client != null) client.Close(); } catch (Exception) { }
        stream = null;
        client = null;
    }
}
