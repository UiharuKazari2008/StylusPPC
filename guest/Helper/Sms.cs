#if CF2
using System;
// Texts on WM 6.5.3 go through MAPIdotnet. WM 2003's messaging store is not that API.
static class Sms
{
    public static string Command(string args)
    {
        Helper.Log("sms: unsupported on this version");
        return "unsupported";
    }
    public static void Poll() { }
}
#else
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using MAPIdotnet;

// Text messages between Windows Mobile 6.5.3 and the phone (SmsBridge.kt), through WM's messaging
// store with MAPIdotnet (third_party/mapidotnet; native half MAPIlib.dll from tools/make-mapilib.py).
// WM 6 / 6.1 / 6.1.4 do not use this: their texts go through the virtual modem as real SMS.
// Found in the CLI: CEMAPI commits a new message on SetProps (SaveChanges answers E_NOTIMPL);
// PocketOutlook's SmsMessage.Send bypasses the store (nothing in Outbox or Sent Items), while
// Messaging's own Send goes through the SMS store's Outbox, where the watcher sees it.
//
// Mailbox:
//   app -> guest  sms deliver         files the messages in sms-in.txt (records "#sms", id, from, time
//                                     = Unix ms, text) into the SMS Inbox as unread texts
//                 sms result <file> <0|1>   the phone sent (1) or could not send a message
//                 sms pictures        moves MMS pictures from the mailbox's mms folder to My Pictures\MMS
//   guest -> app  sms delivered <id> ...    filed
//                 sms sent <file>     WM sent a text: sms-out-<n>.txt holds "to=" and "text="
// The helper watches the SMS store's Outbox and Sent Items while "sms=1" is in state.ini; what it
// has passed on is remembered in \Application Data\PocketStylus\sms-seen.txt.
static class Sms
{
    const string SeenFile = @"\Application Data\PocketStylus\sms-seen.txt";
    static System.Threading.Thread watcher;
    static int outSeq;
    // Entry ids of messages this process filed (the watcher must not send them).
    static Dictionary<string, bool> filed = new Dictionary<string, bool>();

    public static string Command(string args)
    {
        string[] p = args.Split(' ');
        switch (p[0])
        {
            case "deliver": return Deliver();
            case "pictures": return Pictures();
            case "result": Helper.Log("sms: " + args); return "ok";
            case "watch": Start(); return "watching";
            case "list":  // diagnostics: message stores and their folders' item counts
            {
                var sb = new StringBuilder();
                using (MAPI mapi = new MAPI())
                    foreach (IMAPIMsgStore st in mapi.MessageStores)
                    {
                        sb.Append(st.DisplayName).Append(':');
                        foreach (IMAPIFolderID fid in st.OpenFolder(st.RootFolder).GetSubFolders(30))
                        {
                            IMAPIFolder f = st.OpenFolder(fid);
                            sb.Append(' ').Append(f.DisplayName).Append('=').Append(f.NumSubItems);
                        }
                        sb.Append(" | ");
                    }
                return sb.ToString();
            }
            case "diag":  // diagnostics: is MAPIlib.dll there and loadable?
            {
                string dll = p.Length > 1 ? args.Substring(5) : @"\Windows\MAPIlib.dll";
                IntPtr h = LoadLibrary(dll);
                int err = System.Runtime.InteropServices.Marshal.GetLastWin32Error();
                return "exists=" + File.Exists(dll) + " onetcf=" + File.Exists(@"\Windows\OpenNETCF.dll") + " load=" + h + " err=" + err;
            }
            case "testsend":  // diagnostics: WM sends a text through its own SMS stack, as Messaging does
                if (p.Length < 3) return "bad args";
                using (Microsoft.WindowsMobile.PocketOutlook.OutlookSession s = new Microsoft.WindowsMobile.PocketOutlook.OutlookSession())
                    new Microsoft.WindowsMobile.PocketOutlook.SmsMessage(p[1], args.Substring(p[0].Length + p[1].Length + 2)).Send();
                return "sent";
        }
        return "bad args";
    }

    /** Called from the mailbox loop: watches WM's outgoing texts while the app has texts on. */
    public static void Poll()
    {
        if (watcher == null && !Patch.ModemRil && Helper.Get("sms", "0") == "1") Start();
    }

    static void Start()
    {
        if (watcher != null) return;
        watcher = new System.Threading.Thread(Watch);
        watcher.IsBackground = true;
        watcher.Start();
    }

    static IMAPIMsgStore SmsStore(MAPI mapi)
    {
        foreach (IMAPIMsgStore s in mapi.MessageStores)
            if (string.Compare(s.DisplayName, "SMS", true) == 0) return s;
        return null;
    }

    // ---- Incoming ----

    static string Deliver()
    {
        string path = Path.Combine(Helper.Box, "sms-in.txt");
        if (!File.Exists(path)) return "nothing";
        var delivered = new StringBuilder();
        using (MAPI mapi = new MAPI())
        {
            IMAPIMsgStore store = SmsStore(mapi);
            if (store == null) return "no SMS store";
            MAPIFolder inbox = (MAPIFolder)store.OpenFolder(store.ReceiveFolder);
            MAPIFolder sent = (MAPIFolder)store.OpenFolder(store.SentMailFolder);
            // A whole history in one pass blocks the mailbox long enough for the watchdog to kill
            // this process and for the app to pull the storage card. A short batch, then the app
            // sends the next.
            int filed = 0;
            foreach (Dictionary<string, string> m in ReadRecords(path))
            {
                if (filed >= 8) break;
                string id, from, text, time, box, readFlag;
                m.TryGetValue("id", out id); m.TryGetValue("from", out from);
                m.TryGetValue("text", out text); m.TryGetValue("time", out time);
                m.TryGetValue("box", out box); m.TryGetValue("read", out readFlag);
                Helper.Beat();
                try
                {
                    long ms = time != null ? long.Parse(time) : 0;
                    ulong ft = (ulong)((ms + 11644473600000L) * 10000L);
                    MAPIFolder folder = box == "out" ? sent : inbox;
                    cemapi.IMessage msg = folder.Raw.CreateMessage();
                    msg.SetProps(new cemapi.PropTags[] {
                        cemapi.PropTags.PR_MESSAGE_CLASS, cemapi.PropTags.PR_SUBJECT, cemapi.PropTags.PR_SENDER_EMAIL_ADDRESS,
                        cemapi.PropTags.PR_SENDER_NAME, cemapi.PropTags.PR_SENDER_ADDRTYPE, cemapi.PropTags.PR_MESSAGE_DELIVERY_TIME,
                        cemapi.PropTags.PR_MESSAGE_FLAGS },
                        new object[] { "IPM.SMStext", text ?? "", from ?? "", from ?? "", "SMS", ft, readFlag == "1" ? 1u : 0u });
                    msg.SaveChanges(0);
                    Remember(msg);
                    msg.Release();
                    delivered.Append(' ').Append(id);
                    filed++;
                }
                catch (Exception e) { Helper.Log("sms: deliver " + id + ": " + e.Message); filed++; }
            }
        }
        File.Delete(path);
        if (delivered.Length > 0) Helper.Send("sms delivered" + delivered);
        return "ok";
    }

    // MMS pictures, video and sound (sms pictures): from the mailbox's mms folder into
    // \My Documents\My Pictures\MMS, where Pictures & Videos shows them.
    static string Pictures()
    {
        string src = Path.Combine(Helper.Box, "mms");
        if (!Directory.Exists(src)) return "nothing";
        string dst = @"\My Documents\My Pictures\MMS";
        Directory.CreateDirectory(dst);
        int n = 0;
        foreach (string f in Directory.GetFiles(src))
        {
            try
            {
                string to = Path.Combine(dst, Path.GetFileName(f));
                if (File.Exists(to)) File.Delete(to);
                File.Copy(f, to);
                File.Delete(f);
                n++;
            }
            catch (Exception e) { Helper.Log("sms: picture " + f + ": " + e.Message); }
        }
        return n + " filed";
    }

    // ---- Outgoing: WM's Outbox and Sent Items ----

    static void Watch()
    {
        var seen = LoadSeen();
        bool first = seen.Count == 0;
        Helper.Log("sms: watching WM's Outbox and Sent Items (" + seen.Count + " known)");
        while (true)
        {
            try
            {
                using (MAPI mapi = new MAPI())
                {
                    IMAPIMsgStore store = SmsStore(mapi);
                    if (store != null)
                    {
                        bool changed = false;
                        foreach (IMAPIFolderID fid in new IMAPIFolderID[] { store.OutboxFolder, store.SentMailFolder })
                        {
                            IMAPIFolder folder = store.OpenFolder(fid);
                            folder.SeekMessages(0);
                            IMAPIMessage[] msgs;
                            while ((msgs = folder.GetNextMessages(20)).Length > 0)
                                foreach (IMAPIMessage m in msgs)
                                {
                                    string key = Hex(m.MessageID.AsByteArray);
                                    if (seen.ContainsKey(key) || filed.ContainsKey(key)) { seen[key] = true; continue; }
                                    seen[key] = true;
                                    changed = true;
                                    // The first look only learns what is already there (no old texts resent).
                                    // A message filed from the phone's history carries its original time, so it
                                    // is not sent again. A text Windows Mobile just composed is recent.
                                    if (!first && IsFresh(m)) Report(m);
                                }
                        }
                        if (changed || first) SaveSeen(seen);
                        if (first) Helper.Log("sms: first look done");
                        first = false;
                    }
                }
            }
            catch (Exception e) { Helper.Log("sms: watch " + e.GetType().Name + " " + e.Message); }
            System.Threading.Thread.Sleep(3000);
        }
    }

    static bool IsFresh(IMAPIMessage m)
    {
        try
        {
            DateTime when = m.LocalDeliveryTime;
            return when.Year < 2000 || when > DateTime.Now.AddMinutes(-10);
        }
        catch (Exception) { return true; }
    }

    // So the outbox watcher does not treat a message we just filed as one Windows Mobile sent.
    static void Remember(cemapi.IMessage msg)
    {
        try
        {
            byte[] id = msg.GetProps(new cemapi.PropTags[] { cemapi.PropTags.PR_ENTRYID })[0].AsBinary;
            var seen = LoadSeen();
            string hex = Hex(id);
            seen[hex] = true;
            filed[hex] = true;
            SaveSeen(seen);
        }
        catch (Exception) { }
    }

    static void Report(IMAPIMessage m)
    {
        var to = new StringBuilder();
        foreach (IMAPIContact r in m.Recipients)
        {
            if (to.Length > 0) to.Append(';');
            to.Append(Number(r.FullAddress));
        }
        string text = m.Subject;
        try { if (!string.IsNullOrEmpty(m.Body) && m.Body.Length > (text ?? "").Length) text = m.Body; } catch (Exception) { }
        foreach (string number in to.ToString().Split(';'))
        {
            if (number.Length == 0) continue;
            string name = "sms-out-" + (++outSeq) + ".txt";
            using (StreamWriter w = new StreamWriter(Path.Combine(Helper.Box, name), false, new UTF8Encoding(false)))
                w.Write("to=" + Escape(number) + "\ntext=" + Escape(text ?? "") + "\n");
            Helper.Log("sms: WM sent a text to " + number);
            Helper.Send("sms sent " + name);
        }
    }

    // "Name <+15551234>" or "+15551234" -> the number.
    static string Number(string address)
    {
        if (address == null) return "";
        int lt = address.LastIndexOf('<'), gt = address.LastIndexOf('>');
        if (lt >= 0 && gt > lt) address = address.Substring(lt + 1, gt - lt - 1);
        return address.Trim();
    }

    // ---- Files ----

    static List<Dictionary<string, string>> ReadRecords(string path)
    {
        var list = new List<Dictionary<string, string>>();
        Dictionary<string, string> cur = null;
        using (StreamReader r = new StreamReader(path, Encoding.UTF8))
        {
            string line;
            while ((line = r.ReadLine()) != null)
            {
                if (line.StartsWith("#")) { cur = new Dictionary<string, string>(); list.Add(cur); continue; }
                int eq = line.IndexOf('=');
                if (cur != null && eq > 0) cur[line.Substring(0, eq)] = Unescape(line.Substring(eq + 1));
            }
        }
        return list;
    }

    static Dictionary<string, bool> LoadSeen()
    {
        var d = new Dictionary<string, bool>();
        try
        {
            using (StreamReader r = new StreamReader(SeenFile))
            {
                string l;
                while ((l = r.ReadLine()) != null) if (l.Length > 0) d[l] = true;
            }
        }
        catch (Exception) { }
        return d;
    }

    static void SaveSeen(Dictionary<string, bool> seen)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SeenFile));
            using (StreamWriter w = new StreamWriter(SeenFile, false))
                foreach (string k in seen.Keys) w.WriteLine(k);
        }
        catch (Exception e) { Helper.Log("sms: seen " + e.Message); }
    }

    static string Hex(byte[] b)
    {
        var sb = new StringBuilder(b.Length * 2);
        foreach (byte x in b) sb.Append(x.ToString("X2"));
        return sb.ToString();
    }

    [System.Runtime.InteropServices.DllImport("coredll.dll", SetLastError = true)] static extern IntPtr LoadLibrary(string path);

    static string Escape(string v) { return v.Replace("\\", "\\\\").Replace("\n", "\\n").Replace("\r", "\\r"); }

    static string Unescape(string v)
    {
        if (v.IndexOf('\\') < 0) return v;
        var sb = new StringBuilder(v.Length);
        for (int i = 0; i < v.Length; i++)
        {
            char c = v[i];
            if (c == '\\' && i + 1 < v.Length) { char n = v[++i]; sb.Append(n == 'n' ? '\n' : n == 'r' ? '\r' : n); }
            else sb.Append(c);
        }
        return sb.ToString();
    }
}
#endif
