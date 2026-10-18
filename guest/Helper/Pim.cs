using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using Microsoft.Win32;
using Microsoft.WindowsMobile.PocketOutlook;

// Contacts and calendar sync with Android (docs/research-phone-calls-pim.md, section G).
//
// The helper is the dumb half: it lists Windows Mobile's contacts and appointments and applies the
// changes the app decided on. Android keeps the item map and settles every difference, Android
// winning conflicts. Each synced item carries the app's key in a custom POOM property; the sync
// generation in HKCU\Software\WMDroid\PimGen tells the app whether this is the store it synced last
// (a session restored from an older snapshot has an older generation: Android's state wins then).
//
// Mailbox (files in \Storage Card\.wmdroid, UTF-8):
//   app -> guest  pim dump     writes pim-dump.txt: "gen=<n>", then one record per item
//                 pim apply    reads pim-apply.txt ("gen=<n>", then operations), writes pim-result.txt
//   guest -> app  pim dumped gen=<n> contacts=<n> appointments=<n>
//                 pim applied gen=<n> ok=<n> failed=<n>
//                 pim error <message> / pim busy
// Records: a line "#contact" or "#appointment" (in pim-apply.txt "#put <kind>", "#delete <kind>",
// "#setkey <kind>"; in pim-result.txt "#ok <op> <kind>" / "#fail <op> <kind>"), then "name=value"
// lines: key, oid, and the fields below. Values escape "\" "\n" "\r" as \\ \n \r.
static class Pim
{
    const string KeyProp = "WMDroidKey";

    // Contact fields, by POOM property name (strings unless noted in Get/Set).
    static readonly string[] ContactFields = {
        "FirstName", "MiddleName", "LastName", "Title", "Suffix", "Nickname", "YomiFirstName", "YomiLastName",
        "CompanyName", "YomiCompanyName", "Department", "JobTitle", "OfficeLocation",
        "MobileTelephoneNumber", "HomeTelephoneNumber", "Home2TelephoneNumber", "BusinessTelephoneNumber",
        "Business2TelephoneNumber", "HomeFaxNumber", "BusinessFaxNumber", "PagerNumber", "CarTelephoneNumber",
        "RadioTelephoneNumber", "CompanyTelephoneNumber", "AssistantTelephoneNumber",
        "Email1Address", "Email2Address", "Email3Address", "IM1Address", "IM2Address", "IM3Address", "WebPage",
        "HomeAddressStreet", "HomeAddressCity", "HomeAddressState", "HomeAddressPostalCode", "HomeAddressCountry",
        "BusinessAddressStreet", "BusinessAddressCity", "BusinessAddressState", "BusinessAddressPostalCode", "BusinessAddressCountry",
        "OtherAddressStreet", "OtherAddressCity", "OtherAddressState", "OtherAddressPostalCode", "OtherAddressCountry",
        "Birthday", "Anniversary", "Spouse", "Children", "Body", "Picture",
    };

    // Appointment fields: Start/End are UTC "yyyyMMddTHHmmssZ", or dates "yyyyMMdd" (End exclusive)
    // when AllDay=1; Busy = BusyStatus (0 free, 1 tentative, 2 busy, 3 away); Sensitivity 0 normal,
    // 2 private; Reminder = minutes before, or -1.
    static readonly string[] AppointmentFields = {
        "Subject", "Location", "Body", "Start", "End", "AllDay", "Busy", "Sensitivity", "Reminder", "Categories",
    };

    static volatile bool running;

    public static string Command(string args)
    {
        string verb = args.Trim().ToLower();
        if (verb != "dump" && verb != "apply") return "bad args";
        if (running) { Helper.Send("pim busy"); return "busy"; }
        running = true;
        System.Threading.Thread t = new System.Threading.Thread(delegate() { Run(verb); });
        t.IsBackground = true;
        t.Start();
        return "started";
    }

    static void Run(string verb)
    {
        try
        {
            using (OutlookSession s = new OutlookSession())
            {
                if (verb == "dump") Dump(s);
                else Apply(s);
            }
        }
        catch (Exception e)
        {
            Helper.Log("pim " + verb + ": " + e.GetType().Name + " " + e.Message);
            Helper.Send("pim error " + verb + " " + e.GetType().Name + " " + e.Message.Replace('\n', ' '));
        }
        finally { running = false; }
    }

    // ---- Dump ----

    static void Dump(OutlookSession s)
    {
        int t0 = Environment.TickCount, nc = 0, na = 0;
        byte[] body;
        var ms = new MemoryStream();
        using (StreamWriter w = new StreamWriter(ms, new UTF8Encoding(false)))
        {
            w.Write("gen=" + Gen + "\n");
            foreach (Contact c in s.Contacts.Items) { WriteRecord(w, "#contact", c, ContactFields); nc++; }
            foreach (Appointment a in s.Appointments.Items) { WriteRecord(w, "#appointment", a, AppointmentFields); na++; }
            w.Flush();
            body = ms.ToArray();
        }
        Helper.Log("pim: dumped " + nc + " contacts, " + na + " appointments in " + (Environment.TickCount - t0) + " ms");
        string note = "pim dumped gen=" + Gen + " contacts=" + nc + " appointments=" + na;
        // The bytes go out before the line, on the same TCP connection, so the app has them when the line arrives.
        if (!NetLink.SendBlob("pim-dump", body) || !NetLink.SendLine(note))
            Helper.Log("pim: dump waiting for the link");
    }

    static void WriteRecord(StreamWriter w, string head, PimItem item, string[] fields)
    {
        w.Write(head + "\n");
        string key = KeyOf(item);
        if (key.Length > 0) w.Write("key=" + Escape(key) + "\n");
        w.Write("oid=" + OidOf(item) + "\n");
        foreach (string f in fields)
        {
            string v;
            try { v = Get(item, f); } catch (Exception) { v = ""; }
            if (v.Length > 0) w.Write(f + "=" + Escape(v) + "\n");
        }
    }

    // ---- Apply ----

    class Op
    {
        public string Verb, Kind;
        public Dictionary<string, string> Values = new Dictionary<string, string>();
    }

    static void Apply(OutlookSession s)
    {
        int t0 = Environment.TickCount;
        int gen = -1;
        List<Op> ops = new List<Op>();
        Op cur = null;
        byte[] incoming = null;
        for (int i = 0; i < 150 && incoming == null; i++)
        {
            incoming = NetLink.TakeBlob("pim-apply");
            if (incoming == null) System.Threading.Thread.Sleep(20);
        }
        if (incoming == null) { Helper.Log("pim: no apply bytes"); Helper.Send("pim error apply no bytes"); return; }
        foreach (string line in ReadLines(incoming))
        {
            if (line.StartsWith("#"))
            {
                string[] p = line.Substring(1).Split(' ');
                cur = new Op { Verb = p[0], Kind = p.Length > 1 ? p[1] : "" };
                ops.Add(cur);
                continue;
            }
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            string k = line.Substring(0, eq), v = Unescape(line.Substring(eq + 1));
            if (cur == null) { if (k == "gen") gen = int.Parse(v); }
            else cur.Values[k] = v;
        }
        // Items by key and by item id, for both kinds.
        Dictionary<string, PimItem> byKey = new Dictionary<string, PimItem>();
        Dictionary<string, PimItem> byOid = new Dictionary<string, PimItem>();
        foreach (Contact c in s.Contacts.Items) Index(c, "contact", byKey, byOid);
        foreach (Appointment a in s.Appointments.Items) Index(a, "appointment", byKey, byOid);

        int ok = 0, failed = 0;
        byte[] body = new byte[0];
        var result = new MemoryStream();
        using (StreamWriter w = new StreamWriter(result, new UTF8Encoding(false)))
        {
            foreach (Op op in ops)
            {
                string key, oid;
                op.Values.TryGetValue("key", out key);
                op.Values.TryGetValue("oid", out oid);
                try
                {
                    PimItem item = null;
                    if (key != null) byKey.TryGetValue(op.Kind + "|" + key, out item);
                    if (item == null && oid != null) byOid.TryGetValue(op.Kind + "|" + oid, out item);
                    string[] fields = op.Kind == "contact" ? ContactFields : AppointmentFields;
                    if (op.Verb == "delete")
                    {
                        if (item != null) Delete(item);
                        w.Write("#ok delete " + op.Kind + "\n");
                        if (key != null) w.Write("key=" + Escape(key) + "\n");
                    }
                    else if (op.Verb == "put" || op.Verb == "setkey")
                    {
                        if (item == null && op.Verb == "setkey") throw new Exception("no item " + oid);
                        // new Contact() is not in the store: its id is a sentinel and Update() does not
                        // insert it, so the next dump still sees nothing. AddNew() does.
                        bool fresh = item == null;
                        if (fresh) item = op.Kind == "contact" ? (PimItem)s.Contacts.Items.AddNew() : s.Appointments.Items.AddNew();
                        try
                        {
                            if (op.Verb == "put")
                                foreach (string f in fields)
                                {
                                    string v;
                                    op.Values.TryGetValue(f, out v);
                                    // One field POOM refuses does not cost the item (the app sees it in the result).
                                    try { Set(item, f, v ?? ""); }
                                    catch (Exception e) { Helper.Log("pim: " + op.Kind + " " + key + " " + f + ": " + e.GetType().Name); }
                                }
                            if (key != null) SetKey(item, key);
                            if (item is Contact) ((Contact)item).Update();
                            else ((Appointment)item).Update();
                        }
                        catch (Exception)
                        {
                            if (fresh) try { Delete(item); } catch (Exception) { }
                            throw;
                        }
                        WriteRecord(w, "#ok " + op.Verb + " " + op.Kind, item, fields);
                    }
                    else continue;
                    ok++;
                }
                catch (Exception e)
                {
                    failed++;
                    Helper.Log("pim: " + op.Verb + " " + op.Kind + " " + key + ": " + e.GetType().Name + " " + e.Message);
                    w.Write("#fail " + op.Verb + " " + op.Kind + "\n");
                    if (key != null) w.Write("key=" + Escape(key) + "\n");
                    w.Write("error=" + Escape(e.GetType().Name + " " + e.Message) + "\n");
                }
            }
            w.Flush();
            body = result.ToArray();
        }
        if (gen >= 0) Gen = gen;
        Helper.Log("pim: applied " + ok + " (" + failed + " failed) in " + (Environment.TickCount - t0) + " ms");
        string note = "pim applied gen=" + Gen + " ok=" + ok + " failed=" + failed;
        if (!NetLink.SendBlob("pim-result", body) || !NetLink.SendLine(note))
            Helper.Log("pim: result waiting for the link");
    }

    static void Index(PimItem item, string kind, Dictionary<string, PimItem> byKey, Dictionary<string, PimItem> byOid)
    {
        string key = KeyOf(item);
        if (key.Length > 0) byKey[kind + "|" + key] = item;
        byOid[kind + "|" + OidOf(item)] = item;
    }

    static void Delete(PimItem item)
    {
        if (item is Contact) ((Contact)item).Delete();
        else ((Appointment)item).Delete();
    }

    // ---- Keys and generation ----

    static string KeyOf(PimItem item)
    {
        try
        {
            if (!item.Properties.Contains(KeyProp)) return "";
            object v = item.Properties[KeyProp];
            return v as string ?? "";
        }
        catch (Exception) { return ""; }
    }

    static void SetKey(PimItem item, string key)
    {
        if (!item.Properties.Contains(KeyProp)) item.Properties.Add(KeyProp, typeof(string));
        item.Properties[KeyProp] = key;
    }

    static string OidOf(PimItem item)
    {
        try { return item.ItemId == null ? "0" : item.ItemId.ToString(); } catch (Exception) { return "0"; }
    }

    static int Gen
    {
        get
        {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\WMDroid"))
            {
                object v = k == null ? null : k.GetValue("PimGen");
                return v is int ? (int)v : 0;
            }
        }
        set
        {
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\WMDroid")) k.SetValue("PimGen", value);
        }
    }

    // ---- Fields ----

    static string Get(PimItem item, string f)
    {
        Contact c = item as Contact;
        if (c != null)
        {
            if (f == "WebPage") return c.WebPage == null ? "" : c.WebPage.ToString();
            if (f == "Birthday") return DateText(c.Birthday);
            if (f == "Anniversary") return DateText(c.Anniversary);
            if (f == "Picture")
            {
                // Contact.Picture is a read-only Image. JPEG so the sync can compare it.
                System.Drawing.Image img = c.Picture;
                if (img == null) return "";
                using (MemoryStream ms = new MemoryStream())
                {
                    img.Save(ms, System.Drawing.Imaging.ImageFormat.Jpeg);
                    return Convert.ToBase64String(ms.ToArray());
                }
            }
            string v = (string)typeof(Contact).GetProperty(f).GetValue(c, null) ?? "";
            return f == "Body" ? v.Replace("\r\n", "\n") : v;
        }
        Appointment a = (Appointment)item;
        switch (f)
        {
            case "Subject": return a.Subject ?? "";
            case "Location": return a.Location ?? "";
            case "Body": return (a.Body ?? "").Replace("\r\n", "\n");
            case "Categories": return a.Categories ?? "";
            case "AllDay": return a.AllDayEvent ? "1" : "";
            case "Start": return a.AllDayEvent ? a.Start.ToString("yyyyMMdd") : Utc(a.Start);
            case "End":
                if (!a.AllDayEvent) return Utc(a.End);
                // All-day ends are kept as the next day's date (exclusive), whichever way POOM stores them.
                DateTime e = a.End.TimeOfDay == TimeSpan.Zero ? a.End : a.End.Date.AddDays(1);
                return e.ToString("yyyyMMdd");
            case "Busy": return ((int)a.BusyStatus).ToString();
            case "Sensitivity": return ((int)a.Sensitivity).ToString();
            case "Reminder": return a.ReminderSet ? a.ReminderMinutesBeforeStart.ToString() : "-1";
        }
        return "";
    }

    static void Set(PimItem item, string f, string v)
    {
        Contact c = item as Contact;
        if (c != null)
        {
            if (f == "WebPage")
            {
                // POOM throws on a null web page: an empty one is only cleared if there is one.
                if (v.Length > 0) c.WebPage = new Uri(v.IndexOf(':') > 0 ? v : "http://" + v);
                else
                {
                    Uri old = null;
                    try { old = c.WebPage; } catch (Exception) { }
                    if (old != null) try { c.WebPage = new Uri("http://"); } catch (Exception) { }
                }
                return;
            }
            if (f == "Birthday") { c.Birthday = ParseDate(v); return; }
            if (f == "Anniversary") { c.Anniversary = ParseDate(v); return; }
            if (f == "Picture")
            {
                // The property is read-only; SetPicture takes a file (PocketOutlook).
                if (v.Length == 0) return;
                string path = Path.Combine(Path.GetTempPath(), "wmdroid-contact.jpg");
                byte[] jpeg = Convert.FromBase64String(v);
                using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write)) fs.Write(jpeg, 0, jpeg.Length);
                c.SetPicture(path);
                try { File.Delete(path); } catch (Exception) { }
                return;
            }
            if (f == "Body") v = v.Replace("\r\n", "\n").Replace("\n", "\r\n");
            PropertyInfo p = typeof(Contact).GetProperty(f);
            if ((string)p.GetValue(c, null) != v) p.SetValue(c, v, null);
            return;
        }
        Appointment a = (Appointment)item;
        switch (f)
        {
            case "Subject": a.Subject = v; break;
            case "Location": a.Location = v; break;
            case "Body": a.Body = v.Replace("\r\n", "\n").Replace("\n", "\r\n"); break;
            case "Categories": a.Categories = v; break;
            case "AllDay": a.AllDayEvent = v == "1"; break;
            // Start and End come as a pair: End is set after Start (POOM keeps the duration otherwise).
            case "Start": a.Start = ParseWhen(v); break;
            case "End": a.End = ParseWhen(v); break;
            case "Busy": a.BusyStatus = (BusyStatus)(v.Length > 0 ? int.Parse(v) : 2); break;
            case "Sensitivity": a.Sensitivity = (Sensitivity)(v.Length > 0 ? int.Parse(v) : 0); break;
            case "Reminder":
                int m = v.Length > 0 ? int.Parse(v) : -1;
                a.ReminderSet = m >= 0;
                if (m >= 0) a.ReminderMinutesBeforeStart = m;
                break;
        }
    }

    // POOM's "no date" is 1 Jan 4501.
    static readonly DateTime NoDate = new DateTime(4501, 1, 1);

    static string DateText(DateTime d)
    {
        // Unset dates read back as 1 Jan 1601 (a zero FILETIME) or 1 Jan 4501.
        return d.Year >= 4500 || d.Year <= 1601 ? "" : d.ToString("yyyy-MM-dd");
    }

    static DateTime ParseDate(string v)
    {
        if (v.Length < 10) return NoDate;
        return new DateTime(int.Parse(v.Substring(0, 4)), int.Parse(v.Substring(5, 2)), int.Parse(v.Substring(8, 2)));
    }

    static string Utc(DateTime local)
    {
        return local.ToUniversalTime().ToString("yyyyMMdd'T'HHmmss'Z'");
    }

    // "yyyyMMdd" (a local date) or "yyyyMMddTHHmmssZ" (UTC) -> POOM's local time.
    static DateTime ParseWhen(string v)
    {
        DateTime d = new DateTime(int.Parse(v.Substring(0, 4)), int.Parse(v.Substring(4, 2)), int.Parse(v.Substring(6, 2)));
        if (v.Length < 15) return d;
        d = d.AddHours(int.Parse(v.Substring(9, 2))).AddMinutes(int.Parse(v.Substring(11, 2))).AddSeconds(int.Parse(v.Substring(13, 2)));
        return new DateTime(d.Ticks, DateTimeKind.Utc).ToLocalTime();
    }

    static List<string> ReadLines(string path)
    {
        byte[] b;
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read))
        {
            b = new byte[fs.Length];
            int n = 0;
            while (n < b.Length) { int g = fs.Read(b, n, b.Length - n); if (g <= 0) break; n += g; }
        }
        return ReadLines(b);
    }

    /** UTF-8 lines. A 4-byte sequence (an emoji) is skipped: the reader in this runtime throws on it. */
    static List<string> ReadLines(byte[] b)
    {
        List<string> lines = new List<string>();
        StringBuilder sb = new StringBuilder();
        int i = 0;
        while (i < b.Length)
        {
            int c = b[i];
            if (c == '\n') { lines.Add(sb.ToString()); sb.Length = 0; i++; continue; }
            if (c == '\r') { i++; continue; }
            if (c < 0x80) { sb.Append((char)c); i++; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < b.Length) { sb.Append((char)(((c & 0x1F) << 6) | (b[i + 1] & 0x3F))); i += 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < b.Length) { sb.Append((char)(((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F))); i += 3; }
            else if ((c & 0xF8) == 0xF0 && i + 3 < b.Length) i += 4;
            else i++;
        }
        if (sb.Length > 0) lines.Add(sb.ToString());
        return lines;
    }

    static string Escape(string v)
    {
        return v.Replace("\\", "\\\\").Replace("\n", "\\n").Replace("\r", "\\r");
    }

    static string Unescape(string v)
    {
        if (v.IndexOf('\\') < 0) return v;
        StringBuilder sb = new StringBuilder(v.Length);
        for (int i = 0; i < v.Length; i++)
        {
            char ch = v[i];
            if (ch == '\\' && i + 1 < v.Length)
            {
                char n = v[++i];
                sb.Append(n == 'n' ? '\n' : n == 'r' ? '\r' : n);
            }
            else sb.Append(ch);
        }
        return sb.ToString();
    }
}
