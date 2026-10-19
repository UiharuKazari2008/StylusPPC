using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

// HTTPS from this phone. HttpWebRequest waits in Connection Manager before any packet leaves
// (guest/Helper/Weather.cs), so this uses WinInet, the same stack as Internet Explorer.
// PocketStylus's secure web proxy accepts that TLS 1.0 on port 443 and opens modern TLS to the site.
// The host has no Discord code: the request, the token and the body stay in Windows Mobile.
public static class Net
{
    public sealed class Reply
    {
        public int Status;
        public byte[] Body = new byte[0];
        public string Headers = "";
        public bool Truncated;
        public string Text { get { return Encoding.UTF8.GetString(Body, 0, Body.Length); } }

        public string Header(string name)
        {
            string key = name.ToLower();
            int i = 0;
            while (i < Headers.Length)
            {
                int end = Headers.IndexOf("\r\n", i);
                if (end < 0) end = Headers.Length;
                string line = Headers.Substring(i, end - i);
                int colon = line.IndexOf(':');
                if (colon > 0 && line.Substring(0, colon).Trim().ToLower() == key)
                    return line.Substring(colon + 1).Trim();
                if (end >= Headers.Length) break;
                i = end + 2;
            }
            return "";
        }
    }

    static readonly object Gate = new object();
    static IntPtr session;

    public static Reply Exchange(string method, string host, int port, string path, string userAgent,
        string authorization, string contentType, Stream body, long length, int timeoutMs, int maxBody)
    {
        return Exchange(method, host, port, path, userAgent, authorization, contentType, body, length, timeoutMs, maxBody, "*/*");
    }

    public static Reply Exchange(string method, string host, int port, string path, string userAgent,
        string authorization, string contentType, Stream body, long length, int timeoutMs, int maxBody, string accept)
    {
        if (host == null || host.Length == 0) throw new IOException("Missing host");
        if (path == null || path.Length == 0) path = "/";
        if (port <= 0 || port > 65535) throw new IOException("Bad port");
        if (length < 0 || length > int.MaxValue) throw new IOException("Body is too large");
        lock (Gate)
        {
            IntPtr conn = IntPtr.Zero, req = IntPtr.Zero;
            try
            {
                IntPtr ses = Session();
                conn = InternetConnect(ses, host, (short)port, null, null, 3, 0, IntPtr.Zero);
                if (conn == IntPtr.Zero) throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
                uint flags = 0x80000000 | 0x04000000 | 0x00000100; // reload, no cache write, pragma no-cache
                if (port == 443) flags |= 0x00800000; // secure
                req = HttpOpenRequest(conn, method, path, "HTTP/1.1", null, IntPtr.Zero, flags, IntPtr.Zero);
                if (req == IntPtr.Zero) throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
                Timeout(req, timeoutMs);
                if (userAgent != null && userAgent.Length > 0) Header(req, "User-Agent: " + userAgent);
                if (authorization != null && authorization.Length > 0) Header(req, "Authorization: " + authorization);
                Header(req, "Accept: " + (accept == null || accept.Length == 0 ? "*/*" : accept));
                Header(req, "Connection: close");
                if (contentType != null && contentType.Length > 0) Header(req, "Content-Type: " + contentType);
                // WinInet otherwise waits on Expect: 100-continue, which some fronts never answer.
                if (body != null) Header(req, "Expect:");
                bool ok;
                if (body == null || length == 0)
                    ok = HttpSendRequest(req, null, 0, null, 0);
                else
                {
                    MemoryStream ms = body as MemoryStream;
                    if (ms != null && length <= 256 * 1024)
                    {
                        byte[] buf = ms.ToArray();
                        ok = HttpSendRequest(req, null, 0, buf, buf.Length);
                    }
                    else
                    {
                        INTERNET_BUFFERS ib = new INTERNET_BUFFERS();
                        ib.dwStructSize = Marshal.SizeOf(typeof(INTERNET_BUFFERS));
                        ib.dwBufferTotal = (int)length;
                        ok = HttpSendRequestEx(req, ref ib, IntPtr.Zero, 0, IntPtr.Zero);
                        if (ok)
                        {
                            WriteAll(req, body, length);
                            ok = HttpEndRequest(req, IntPtr.Zero, 0, 0);
                        }
                    }
                }
                Reply reply = new Reply();
                reply.Status = StatusOf(Query(req, 19));
                reply.Headers = Query(req, 22);
                if (!ok && reply.Status == 0) throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
                reply.Body = ReadAll(req, maxBody, reply);
                return reply;
            }
            finally
            {
                if (req != IntPtr.Zero) InternetCloseHandle(req);
                if (conn != IntPtr.Zero) InternetCloseHandle(conn);
            }
        }
    }

    static IntPtr Session()
    {
        if (session != IntPtr.Zero) return session;
        session = InternetOpen("WMDiscord/1.0", 1, null, null, 0);
        if (session == IntPtr.Zero) throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
        Timeout(session, 45000);
        return session;
    }

    static void WriteAll(IntPtr req, Stream body, long length)
    {
        byte[] chunk = new byte[8192];
        long left = length;
        while (left > 0)
        {
            int want = (int)Math.Min(chunk.Length, left);
            int got = 0;
            while (got < want)
            {
                int n = body.Read(chunk, got, want - got);
                if (n <= 0) throw new IOException("File ended early");
                got += n;
            }
            int off = 0;
            while (off < got)
            {
                int n;
                bool wrote;
                if (off == 0) wrote = InternetWriteFile(req, chunk, got, out n);
                else
                {
                    byte[] rest = new byte[got - off];
                    Buffer.BlockCopy(chunk, off, rest, 0, rest.Length);
                    wrote = InternetWriteFile(req, rest, rest.Length, out n);
                }
                if (!wrote || n <= 0) throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
                off += n;
            }
            left -= got;
        }
    }

    static byte[] ReadAll(IntPtr req, int max, Reply reply)
    {
        if (max < 1) max = 1;
        MemoryStream ms = new MemoryStream();
        byte[] buf = new byte[4096];
        while (true)
        {
            int room = max - (int)ms.Length;
            if (room <= 0)
            {
                int extra;
                if (!InternetReadFile(req, buf, 1, out extra))
                    throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
                if (extra > 0) reply.Truncated = true;
                break;
            }
            int want = Math.Min(buf.Length, room);
            int read;
            if (!InternetReadFile(req, buf, want, out read))
                throw new IOException(WinInetMessage(Marshal.GetLastWin32Error()));
            if (read <= 0) break;
            ms.Write(buf, 0, read);
        }
        return ms.ToArray();
    }

    static void Header(IntPtr req, string line)
    {
        if (line.EndsWith("\r\n") == false) line += "\r\n";
        HttpAddRequestHeaders(req, line, line.Length, 0x20000000 | 0x80000000);
    }

    static void Timeout(IntPtr handle, int ms)
    {
        InternetSetOption(handle, 2, ref ms, 4);
        InternetSetOption(handle, 5, ref ms, 4);
        InternetSetOption(handle, 6, ref ms, 4);
        InternetSetOption(handle, 7, ref ms, 4);
        InternetSetOption(handle, 8, ref ms, 4);
    }

    static string Query(IntPtr req, int level)
    {
        byte[] buf = new byte[128];
        int len = buf.Length;
        int idx = 0;
        if (!HttpQueryInfo(req, (uint)level, buf, ref len, ref idx))
        {
            if (Marshal.GetLastWin32Error() != 122 || len <= 0 || len > 65536) return "";
            buf = new byte[len];
            idx = 0;
            if (!HttpQueryInfo(req, (uint)level, buf, ref len, ref idx)) return "";
        }
        if (len < 0 || len > buf.Length) len = buf.Length;
        string s = Encoding.Unicode.GetString(buf, 0, len);
        return s.TrimEnd('\0');
    }

    static int StatusOf(string s)
    {
        int n = 0, i = 0;
        if (s == null) return 0;
        while (i < s.Length && (s[i] < '0' || s[i] > '9')) i++;
        for (; i < s.Length && s[i] >= '0' && s[i] <= '9'; i++) n = n * 10 + (s[i] - '0');
        return n;
    }

    public static string WinInetMessage(int err)
    {
        if (err == 12045 || err == 12037 || err == 12038 || err == 12044 || err == 12157)
            return "HTTPS failed (" + err + "). Turn on Secure Web Proxy in PocketStylus so it can install its certificate.";
        if (err == 12007) return "Could not find the server. Check the network.";
        if (err == 12002) return "The server took too long to answer.";
        if (err == 12029 || err == 12031)
            return "Could not connect (" + err + "). Check the network and that Secure Web Proxy is on.";
        return "Network error " + err;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct INTERNET_BUFFERS
    {
        public int dwStructSize;
        public IntPtr Next;
        public IntPtr lpcszHeader;
        public int dwHeadersLength;
        public int dwHeadersTotal;
        public IntPtr lpvBuffer;
        public int dwBufferLength;
        public int dwBufferTotal;
        public int dwOffsetLow;
        public int dwOffsetHigh;
    }

    [DllImport("wininet.dll", EntryPoint = "InternetOpenW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr InternetOpen(string agent, int access, string proxy, string bypass, int flags);

    [DllImport("wininet.dll", EntryPoint = "InternetConnectW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr InternetConnect(IntPtr session, string server, short port, string user, string pass, int service, int flags, IntPtr context);

    [DllImport("wininet.dll", EntryPoint = "HttpOpenRequestW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr HttpOpenRequest(IntPtr connect, string verb, string path, string version, string referrer, IntPtr accept, uint flags, IntPtr context);

    [DllImport("wininet.dll", EntryPoint = "HttpAddRequestHeadersW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool HttpAddRequestHeaders(IntPtr request, string headers, int length, uint modifiers);

    [DllImport("wininet.dll", EntryPoint = "HttpSendRequestW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool HttpSendRequest(IntPtr request, string headers, int headerChars, byte[] optional, int optionalLength);

    [DllImport("wininet.dll", EntryPoint = "HttpSendRequestExW", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool HttpSendRequestEx(IntPtr request, ref INTERNET_BUFFERS input, IntPtr output, uint flags, IntPtr context);

    [DllImport("wininet.dll", SetLastError = true)]
    static extern bool InternetWriteFile(IntPtr request, byte[] buffer, int length, out int written);

    [DllImport("wininet.dll", EntryPoint = "HttpEndRequestW", SetLastError = true)]
    static extern bool HttpEndRequest(IntPtr request, IntPtr buffers, int flags, int context);

    [DllImport("wininet.dll", EntryPoint = "HttpQueryInfoW", SetLastError = true)]
    static extern bool HttpQueryInfo(IntPtr request, uint level, byte[] buffer, ref int length, ref int index);

    [DllImport("wininet.dll", SetLastError = true)]
    static extern bool InternetReadFile(IntPtr request, byte[] buffer, int length, out int read);

    [DllImport("wininet.dll", SetLastError = true)]
    static extern bool InternetSetOption(IntPtr handle, int option, ref int value, int length);

    [DllImport("wininet.dll", SetLastError = true)]
    static extern bool InternetCloseHandle(IntPtr handle);
}
