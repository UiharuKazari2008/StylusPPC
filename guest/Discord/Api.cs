using System;
using System.IO;
using System.Text;

// Discord REST from the phone. api.discord.com's front is discord.com. The gateway is a web
// socket this runtime does not speak, so an open chat polls /messages instead.
public static class Api
{
    public static string Token = "";

    public static void Use(string token) { Token = token; }

    public static Json Me() { return ArrayOrObject(Get("/api/v10/users/@me"), false); }

    public static Json DmChannels() { return ArrayOrObject(Get("/api/v10/users/@me/channels"), true); }

    public static Json GuildPage(string after)
    {
        string path = "/api/v10/users/@me/guilds?limit=200";
        if (after != null && after.Length > 0) path += "&after=" + Id(after);
        return ArrayOrObject(Get(path), true);
    }

    public static Json GuildChannels(string guildId)
    {
        return ArrayOrObject(Get("/api/v10/guilds/" + Id(guildId) + "/channels"), true);
    }

    public static Json Messages(string channelId, string after)
    {
        string path = "/api/v10/channels/" + Id(channelId) + "/messages?limit=50";
        if (after != null && after.Length > 0) path += "&after=" + Id(after);
        return ArrayOrObject(Get(path), true);
    }

    public static Json SendText(string channelId, string content)
    {
        byte[] body = Encoding.UTF8.GetBytes("{\"content\":" + Json.Quote(content) + "}");
        return Post(channelId, new MemoryStream(body), body.Length, "application/json");
    }

    public static Json SendFile(string channelId, string content, string path)
    {
        FileInfo info = new FileInfo(path);
        if (!info.Exists) throw new IOException("File is missing");
        if (info.Length <= 0) throw new IOException("File is empty");
        if (info.Length > 8 * 1024 * 1024) throw new IOException("File is larger than 8 MB");
        string name = SafeName(Path.GetFileName(path));
        string payload = content != null && content.Length > 0
            ? "{\"content\":" + Json.Quote(content) + ",\"attachments\":[{\"id\":0,\"filename\":" + Json.Quote(name) + "}]}"
            : "{\"attachments\":[{\"id\":0,\"filename\":" + Json.Quote(name) + "}]}";
        string boundary = "WMDiscord" + DateTime.UtcNow.Ticks.ToString("x");
        StringBuilder pre = new StringBuilder();
        pre.Append("--").Append(boundary).Append("\r\n");
        pre.Append("Content-Disposition: form-data; name=\"payload_json\"\r\n");
        pre.Append("Content-Type: application/json\r\n\r\n");
        pre.Append(payload).Append("\r\n");
        pre.Append("--").Append(boundary).Append("\r\n");
        pre.Append("Content-Disposition: form-data; name=\"files[0]\"; filename=\"").Append(name).Append("\"\r\n");
        pre.Append("Content-Type: ").Append(Mime(name)).Append("\r\n\r\n");
        byte[] head = Encoding.UTF8.GetBytes(pre.ToString());
        byte[] tail = Encoding.UTF8.GetBytes("\r\n--" + boundary + "--\r\n");
        using (FileStream file = new FileStream(path, FileMode.Open, FileAccess.Read))
        using (Parts body = new Parts(head, file, file.Length, tail))
            return Post(channelId, body, head.Length + file.Length + tail.Length, "multipart/form-data; boundary=" + boundary);
    }

    public static byte[] Download(string url, int max)
    {
        string host, path;
        int port;
        Split(url, out host, out path, out port);
        // The account token goes only to discord.com. Attachment URLs are already signed.
        Net.Reply r = Net.Exchange("GET", host, port, path, Agent(), null, null, null, 0, 45000, max, "image/jpeg, image/png, image/gif");
        if (r.Truncated) throw new IOException("Reply is too large");
        if (r.Status < 200 || r.Status >= 300) throw new IOException(Problem(r));
        return r.Body;
    }

    static Json Post(string channelId, Stream body, long length, string type)
    {
        int timeout = length > 256 * 1024 ? 120000 : 45000;
        string path = "/api/v10/channels/" + Id(channelId) + "/messages";
        Net.Reply r = ExchangePost(path, body, length, type, timeout);
        if (r.Status == 429 && body != null && body.CanSeek)
        {
            System.Threading.Thread.Sleep(RetryMs(r));
            body.Position = 0;
            r = ExchangePost(path, body, length, type, timeout);
        }
        // A created message can be larger than we keep. Discord already has it.
        return Take(r, true);
    }

    static Net.Reply ExchangePost(string path, Stream body, long length, string type, int timeout)
    {
        return Net.Exchange("POST", "discord.com", 443, path, Agent(), Auth(), type, body, length, timeout, 2 * 1024 * 1024);
    }

    static Json Get(string path)
    {
        Net.Reply r = Net.Exchange("GET", "discord.com", 443, path, Agent(), Auth(), null, null, 0, 45000, 2 * 1024 * 1024);
        if (r.Status == 429) r = WaitAndRetry(path, r);
        return Take(r);
    }

    static Net.Reply WaitAndRetry(string path, Net.Reply first)
    {
        System.Threading.Thread.Sleep(RetryMs(first));
        return Net.Exchange("GET", "discord.com", 443, path, Agent(), Auth(), null, null, 0, 45000, 2 * 1024 * 1024);
    }

    static Json Take(Net.Reply r) { return Take(r, false); }

    static Json Take(Net.Reply r, bool sentAnyway)
    {
        if (r.Truncated && sentAnyway && r.Status >= 200 && r.Status < 300) return new Json();
        if (r.Truncated) throw new IOException("Reply is too large");
        if (r.Status >= 200 && r.Status < 300)
        {
            if (r.Body.Length == 0) return new Json();
            try { return Json.Parse(r.Text); }
            catch (FormatException) { throw new IOException("Unexpected reply"); }
        }
        throw new IOException(Problem(r));
    }

    static Json ArrayOrObject(Json j, bool array)
    {
        if (array && j.kind != Json.Kind.Array) throw new IOException("Unexpected reply");
        if (!array && j.kind != Json.Kind.Object) throw new IOException("Unexpected reply");
        return j;
    }

    public static string Problem(Net.Reply r)
    {
        string text = r.Text ?? "";
        if (text.IndexOf("could not connect securely") >= 0 || text.IndexOf("Secure connection blocked") >= 0)
            return "Secure Web Proxy could not open a modern connection to Discord.";
        try
        {
            Json err = Json.Parse(text);
            string message = err.Str("message");
            if (message.Length > 0) return r.Status + " " + message;
        }
        catch (FormatException) { }
        if (text.Length == 0) return "HTTP " + r.Status;
        return r.Status + " " + Clip(text, 160);
    }

    static int RetryMs(Net.Reply r)
    {
        string raw = "";
        try { raw = Json.Parse(r.Text).Str("retry_after"); }
        catch (FormatException) { }
        if (raw.Length == 0) raw = r.Header("Retry-After");
        int whole = 0, frac = 0, digits = 0, i = 0;
        while (i < raw.Length && raw[i] >= '0' && raw[i] <= '9') { whole = whole * 10 + (raw[i] - '0'); i++; }
        if (i < raw.Length && raw[i] == '.')
        {
            i++;
            while (i < raw.Length && raw[i] >= '0' && raw[i] <= '9' && digits < 3)
            {
                frac = frac * 10 + (raw[i] - '0');
                digits++;
                i++;
            }
        }
        while (digits < 3) { frac *= 10; digits++; }
        int ms = whole * 1000 + frac;
        if (ms < 250) ms = 250;
        if (ms > 20000) ms = 20000;
        return ms;
    }

    static string Auth()
    {
        if (Token.Length == 0) throw new IOException("Sign in first");
        return Token;
    }

    static string Agent() { return "WMDiscord/1.0 (Windows Mobile 6)"; }

    public static string Id(string id)
    {
        if (id == null || id.Length == 0) throw new IOException("Missing id");
        for (int i = 0; i < id.Length; i++)
            if (id[i] < '0' || id[i] > '9') throw new IOException("Bad id");
        return id;
    }

    public static void Split(string url, out string host, out string path, out int port)
    {
        Uri u;
        try { u = new Uri(url); }
        catch (Exception) { throw new IOException("Bad file address"); }
        if (u.Scheme != "https") throw new IOException("Only HTTPS is used");
        host = u.Host;
        path = u.PathAndQuery;
        if (path == null || path.Length == 0) path = "/";
        port = u.Port;
    }

    static string SafeName(string name)
    {
        if (name == null || name.Length == 0) return "file";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < name.Length; i++)
        {
            char c = name[i];
            if (c == '"' || c == '\\' || c == '\r' || c == '\n' || c == '/' || c == ':') b.Append('_');
            else b.Append(c);
        }
        return b.Length == 0 ? "file" : b.ToString();
    }

    public static string Mime(string name)
    {
        string n = name == null ? "" : name.ToLower();
        if (n.EndsWith(".jpg") || n.EndsWith(".jpeg")) return "image/jpeg";
        if (n.EndsWith(".png")) return "image/png";
        if (n.EndsWith(".gif")) return "image/gif";
        if (n.EndsWith(".bmp")) return "image/bmp";
        if (n.EndsWith(".webp")) return "image/webp";
        if (n.EndsWith(".txt")) return "text/plain";
        if (n.EndsWith(".pdf")) return "application/pdf";
        return "application/octet-stream";
    }

    static string Clip(string s, int n)
    {
        s = s.Replace('\r', ' ').Replace('\n', ' ').Trim();
        if (s.Length <= n) return s;
        return s.Substring(0, n);
    }

    // head, then a file, then tail, without holding the file in memory.
    sealed class Parts : Stream
    {
        readonly byte[] head, tail;
        readonly Stream mid;
        readonly long midLen, total;
        long pos;

        public Parts(byte[] head, Stream mid, long midLen, byte[] tail)
        {
            this.head = head;
            this.mid = mid;
            this.midLen = midLen;
            this.tail = tail;
            total = head.Length + midLen + tail.Length;
        }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { return total; } }
        public override long Position
        {
            get { return pos; }
            set { throw new NotSupportedException(); }
        }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (count <= 0 || pos >= total) return 0;
            int n = 0;
            if (pos < head.Length)
            {
                int take = (int)Math.Min(count, head.Length - pos);
                Buffer.BlockCopy(head, (int)pos, buffer, offset, take);
                pos += take; offset += take; count -= take; n += take;
            }
            long midEnd = head.Length + midLen;
            if (count > 0 && pos < midEnd)
            {
                int take = (int)Math.Min(count, midEnd - pos);
                int got = mid.Read(buffer, offset, take);
                if (got <= 0) return n;
                pos += got; offset += got; count -= got; n += got;
            }
            if (count > 0 && pos >= midEnd && pos < total)
            {
                int tailOff = (int)(pos - midEnd);
                int take = Math.Min(count, tail.Length - tailOff);
                Buffer.BlockCopy(tail, tailOff, buffer, offset, take);
                pos += take;
                n += take;
            }
            return n;
        }
    }
}
