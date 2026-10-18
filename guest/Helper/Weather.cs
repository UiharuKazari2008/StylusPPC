using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;

// Today's weather on the classic Today screen (WM 6.1, not Titanium). SkyToday.dll is ordinal 240,
// InitializeCustomItem (guest/Weather/weather.c). It only draws \Storage Card\.wmdroid\weather.txt.
// This class, inside Windows Mobile, asks the public IP where the phone is and fetches today's
// forecast, then writes that file. Settings > Today lists it as Weather.
static class Weather
{
    const string DllPath = @"\Windows\SkyToday.dll";
    const string Key = @"Software\Microsoft\Today\Items\Weather";
    const int FreshMs = 30 * 60 * 1000;
    const int RetryMs = 2 * 60 * 1000;

    static int due;
    static bool busy;

    /** Replaces the DLL when the user already installed this item. Does not add the item. */
    public static bool Update()
    {
        if (!File.Exists(DllPath)) return false;
        bool replaced = Put(DllPath, WeatherDll.Base64);
        if (replaced) Note("SkyToday.dll updated");
        return replaced;
    }

    public static bool Install()
    {
        bool replaced = Put(DllPath, WeatherDll.Base64);
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
                k.SetValue("Order", 0);
            }
        }
        RegFlushKey(HKLM);
        if (replaced) Note("SkyToday.dll installed");
        return replaced;
    }

    // The helper's serve loop. A fetch runs on its own thread, and only while the item is on.
    public static void Tick()
    {
        if (busy) return;
        int now = Environment.TickCount;
        if (due != 0 && unchecked(now - due) < 0) return;
        if (!ItemOn()) { due = unchecked(now + 60000); return; }
        busy = true;
        new Thread(FetchThread).Start();
    }

    static void FetchThread()
    {
        try
        {
            FetchNow();
            due = unchecked(Environment.TickCount + FreshMs);
        }
        catch (Exception e)
        {
            Note("fetch " + e.GetType().Name + " " + e.Message);
            if (!HaveOk()) WriteStatus(Short(e.Message));
            due = unchecked(Environment.TickCount + RetryMs);
        }
        busy = false;
    }

    // One lookup: IP for the city, then today's temperature, condition, high and low.
    public static void FetchNow()
    {
        try { FetchBody(); }
        catch (Exception e)
        {
            if (!HaveOk()) WriteStatus(Short(e.Message));
            throw;
        }
    }

    static void FetchBody()
    {
        if (!HaveOk()) WriteStatus("Looking up...");
        string geo = HttpGet("http://ip-api.com/json/?fields=status,message,countryCode,city,lat,lon");
        if (Field(geo, "status") != "success") throw new IOException(Field(geo, "message") ?? "No location");
        string lat = Field(geo, "lat");
        string lon = Field(geo, "lon");
        if (lat == null || lon == null || lat.Length == 0 || lon.Length == 0) throw new IOException("No location");
        string cc = Field(geo, "countryCode");
        bool f = cc == "US" || cc == "LR" || cc == "MM";
        string wx = HttpGet("http://api.open-meteo.com/v1/forecast?latitude=" + lat + "&longitude=" + lon +
            "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&temperature_unit=" +
            (f ? "fahrenheit" : "celsius") + "&timezone=auto&forecast_days=1");
        string cur = After(wx, "\"current\":{");
        string temp = RoundNum(Field(cur, "temperature_2m"));
        string high = RoundNum(Field(wx, "temperature_2m_max"));
        string low = RoundNum(Field(wx, "temperature_2m_min"));
        if (temp.Length == 0) throw new IOException("No forecast");
        int code = Atoi(RoundNum(Field(cur, "weather_code")));
        string city = Field(geo, "city");
        if (city == null || city.Length == 0) city = "Here";
        Write("ok=1\r\nplace=" + Clean(city, 40) + "\r\ntemp=" + temp + "\r\nunit=" + (f ? "F" : "C") +
            "\r\nhigh=" + high + "\r\nlow=" + low + "\r\ntext=" + Clean(Condition(code), 32) + "\r\n");
        Note(city + " " + temp + (f ? "F" : "C"));
    }

    static bool ItemOn()
    {
        try
        {
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey(Key))
            {
                if (k == null) return false;
                object v = k.GetValue("Enabled");
                return v is int && (int)v != 0;
            }
        }
        catch (Exception) { return false; }
    }

    static string Condition(int code)
    {
        switch (code)
        {
            case 0: return "Clear";
            case 1: return "Mainly clear";
            case 2: return "Partly cloudy";
            case 3: return "Overcast";
            case 45:
            case 48: return "Fog";
            case 51: return "Light drizzle";
            case 53:
            case 55: return "Drizzle";
            case 56:
            case 57: return "Freezing drizzle";
            case 61: return "Light rain";
            case 63: return "Rain";
            case 65: return "Heavy rain";
            case 66:
            case 67: return "Freezing rain";
            case 71: return "Light snow";
            case 73: return "Snow";
            case 75: return "Heavy snow";
            case 77: return "Snow grains";
            case 80: return "Light showers";
            case 81: return "Showers";
            case 82: return "Heavy showers";
            case 85:
            case 86: return "Snow showers";
            case 95:
            case 96:
            case 99: return "Thunderstorm";
            default: return "Unknown";
        }
    }

    // A raw socket, not HttpWebRequest. On WM, HttpWebRequest waits in Connection Manager
    // before any packet leaves, so a NIC that already has an address still never fetches.
    static string HttpGet(string url)
    {
        Uri u = new Uri(url);
        IPAddress ip = null;
        foreach (IPAddress a in Dns.GetHostEntry(u.Host).AddressList)
            if (a.AddressFamily == AddressFamily.InterNetwork) { ip = a; break; }
        if (ip == null) throw new IOException("No address");
        Note("get " + u.Host);
        Socket sock = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        try
        {
            sock.Connect(new IPEndPoint(ip, u.Port));
            byte[] req = Encoding.ASCII.GetBytes("GET " + u.PathAndQuery + " HTTP/1.0\r\nHost: " + u.Host +
                "\r\nUser-Agent: PocketStylus\r\nConnection: close\r\n\r\n");
            int sent = 0;
            while (sent < req.Length) sent += sock.Send(req, sent, req.Length - sent, SocketFlags.None);
            byte[] buf = new byte[65536];
            int n = 0;
            while (n < buf.Length)
            {
                int r;
                try { r = sock.Receive(buf, n, buf.Length - n, SocketFlags.None); }
                catch (SocketException) { break; }
                if (r <= 0) break;
                n += r;
            }
            string all = Encoding.UTF8.GetString(buf, 0, n);
            int body = all.IndexOf("\r\n\r\n");
            if (body < 0) throw new IOException("Bad reply");
            int line = all.IndexOf("\r\n");
            string status = line > 0 ? all.Substring(0, line) : all;
            if (status.IndexOf(" 200") < 0) throw new IOException(status);
            return all.Substring(body + 4);
        }
        catch (SocketException e)
        {
            throw new IOException("socket " + e.ErrorCode);
        }
        finally { try { sock.Close(); } catch (Exception) { } }
    }

    static string After(string json, string marker)
    {
        if (json == null) return "";
        int i = json.IndexOf(marker);
        return i < 0 ? "" : json.Substring(i);
    }

    static string Field(string json, string key)
    {
        if (json == null) return null;
        string pat = "\"" + key + "\"";
        int i = json.IndexOf(pat);
        if (i < 0) return null;
        i += pat.Length;
        while (i < json.Length && (json[i] == ' ' || json[i] == '\t')) i++;
        if (i >= json.Length || json[i] != ':') return null;
        i++;
        while (i < json.Length && (json[i] == ' ' || json[i] == '\t')) i++;
        if (i >= json.Length) return null;
        if (json[i] == '"')
        {
            i++;
            StringBuilder sb = new StringBuilder();
            while (i < json.Length && json[i] != '"')
            {
                if (json[i] == '\\' && i + 1 < json.Length)
                {
                    i++;
                    char c = json[i];
                    if (c == 'n' || c == 'r' || c == 't') sb.Append(' ');
                    else sb.Append(c);
                }
                else sb.Append(json[i]);
                i++;
            }
            return sb.ToString();
        }
        int e = i;
        while (e < json.Length && json[e] != ',' && json[e] != '}' && json[e] != ']') e++;
        return json.Substring(i, e - i).Trim();
    }

    static int Atoi(string s)
    {
        if (s == null || s.Length == 0) return 0;
        int n = 0, i = 0, sign = 1;
        if (s[0] == '-') { sign = -1; i++; }
        for (; i < s.Length; i++)
        {
            if (s[i] < '0' || s[i] > '9') break;
            n = n * 10 + (s[i] - '0');
        }
        return sign < 0 ? -n : n;
    }

    static string RoundNum(string raw)
    {
        if (raw == null) return "";
        int i = 0;
        while (i < raw.Length && raw[i] != '-' && raw[i] != '.' && (raw[i] < '0' || raw[i] > '9')) i++;
        if (i >= raw.Length) return "";
        int sign = 1;
        if (raw[i] == '-') { sign = -1; i++; }
        int whole = 0;
        bool any = false;
        while (i < raw.Length && raw[i] >= '0' && raw[i] <= '9') { any = true; whole = whole * 10 + (raw[i] - '0'); i++; }
        if (i < raw.Length && raw[i] == '.')
        {
            i++;
            if (i < raw.Length && raw[i] >= '0' && raw[i] <= '9' && raw[i] >= '5') whole++;
        }
        if (!any) return "";
        if (sign < 0) whole = -whole;
        return whole.ToString();
    }

    static string Clean(string s, int cap)
    {
        if (s == null) return "";
        StringBuilder b = new StringBuilder();
        foreach (char c in s)
        {
            if (c == '\r' || c == '\n' || c == '=') b.Append(' ');
            else b.Append(c);
            if (b.Length >= cap) break;
        }
        return b.ToString().Trim();
    }

    static string Short(string m)
    {
        if (m == null || m.Length == 0) return "No network";
        string l = m.ToLower();
        if (l.IndexOf("resolve") >= 0 || l.IndexOf("connect") >= 0 || l.IndexOf("timed out") >= 0 || l.IndexOf("timeout") >= 0)
            return "No network";
        return m;
    }

    static string Dir()
    {
        if (Directory.Exists(@"\Storage Card")) return @"\Storage Card\.wmdroid";
        if (Directory.Exists(@"\Storage Card2")) return @"\Storage Card2\.wmdroid";
        return @"\Storage Card\.wmdroid";
    }

    static bool HaveOk()
    {
        try
        {
            using (var r = new StreamReader(Path.Combine(Dir(), "weather.txt")))
            {
                string line;
                while ((line = r.ReadLine()) != null)
                    if (line.StartsWith("ok=1")) return true;
            }
            return false;
        }
        catch (Exception) { return false; }
    }

    static void WriteStatus(string status)
    {
        Write("ok=0\r\nstatus=" + Clean(status, 48) + "\r\n");
    }

    static void Write(string body)
    {
        string dir = Dir();
        Directory.CreateDirectory(dir);
        string dest = Path.Combine(dir, "weather.txt");
        string tmp = dest + ".tmp";
        byte[] bytes = Encoding.UTF8.GetBytes(body);
        using (var f = File.Create(tmp)) f.Write(bytes, 0, bytes.Length);
        try { if (File.Exists(dest)) File.Delete(dest); } catch (Exception) { }
        File.Move(tmp, dest);
    }

    static void Note(string s)
    {
        try
        {
            string dir = Dir();
            Directory.CreateDirectory(dir);
            using (var w = new StreamWriter(Path.Combine(dir, "helper.log"), true))
                w.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " weather " + s);
        }
        catch (Exception) { }
    }

    // Same replace-if-different copy as Today.Put (guest/Helper/Today.cs).
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
