using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.WindowsCE.Forms;

// Bluetooth file transfer through the phone (docs/research-bluetooth.md). Device Emulator has no
// Bluetooth radio, so files travel through the mailbox instead:
//  - "Send via Bluetooth" (Start menu, or WMDroid.exe send <file>) copies the file to
//    \Storage Card\.wmdroid\send and posts "share <name>"; the app opens Android's Bluetooth share.
//  - Files the phone receives over Bluetooth are copied by the app into \Storage Card\Bluetooth,
//    which then sends "received <name>"; the helper posts a WM notification for them.
static class Bluetooth
{
    const string Card = @"\Storage Card";
    const string SendDir = Card + @"\.wmdroid\send";
    public const string ReceiveDir = Card + @"\Bluetooth";
    public const string StartMenuLink = @"\Windows\Start Menu\Programs\Send via Bluetooth.lnk";

    // ---------------------------------------------------------------------------------------
    // Sending

    // Hands one file to the app; returns an error for the user, or null.
    public static string Send(string path)
    {
        if (!File.Exists(path)) return "File not found:\n" + path;
        if (!Helper.IsMounted(Card)) return "PocketStylus's storage card is not available.";
        try
        {
            Directory.CreateDirectory(SendDir);
            string name = Path.GetFileName(path);
            string copy = Path.Combine(SendDir, name);
            if (string.Compare(path, copy, true) != 0)
            {
                // Files from ROM are read-only and File.Copy keeps that, which would block the next copy.
                if (File.Exists(copy)) new FileInfo(copy).Attributes = FileAttributes.Normal;
                File.Copy(path, copy, true);
                new FileInfo(copy).Attributes = FileAttributes.Normal;
            }
            Helper.Send("share " + name);
            return null;
        }
        catch (Exception e) { return "Could not send " + Path.GetFileName(path) + ":\n" + e.Message; }
    }

    // ---------------------------------------------------------------------------------------
    // Receiving: one notification lists everything received since it was last dismissed.

    static readonly Guid NotifyClass = new Guid("5D2E4A61-7C3B-4E1A-9B0D-57AD3E0B1A01");
    const int NotifyId = 1;
    const int CmdOpen = 1001, CmdFolder = 1002, CmdDismiss = 1003;

    static readonly List<string> pending = new List<string>();
    static Sink sink;
    static bool shown;

    // received <name>: a file the app put in \Storage Card\Bluetooth.
    public static string Received(string name)
    {
        if (name.Length == 0) return "bad args";
        lock (pending)
        {
            pending.Remove(name);
            pending.Add(name);
        }
        return Notify() ? "ok" : "failed";
    }

    static bool Notify()
    {
        EnsureSink();
        string[] names;
        lock (pending) names = pending.ToArray();
        if (names.Length == 0) { Remove(); return true; }

        bool one = names.Length == 1;
        var html = new System.Text.StringBuilder("<html><body>");
        html.Append(one ? "<b>File received</b><br>" : "<b>" + names.Length + " files received</b><br>");
        for (int i = Math.Max(0, names.Length - 5); i < names.Length; i++) html.Append(Html(names[i])).Append("<br>");
        html.Append("<br>");
        if (one) html.Append("<a href=\"cmd:" + CmdOpen + "\">Open</a> &nbsp; ");
        html.Append("<a href=\"cmd:" + CmdFolder + "\">Show folder</a>");
        html.Append("<br><font size=\"1\">\\Storage Card\\Bluetooth</font></body></html>");

        IntPtr title = Str("Bluetooth"), body = Str(html.ToString());
        IntPtr sk0 = Str(one ? "Open" : "Folder"), sk1 = Str("Dismiss");
        try
        {
            var n = new SHNOTIFICATIONDATA();
            n.cbStruct = Marshal.SizeOf(typeof(SHNOTIFICATIONDATA));
            n.dwID = NotifyId;
            n.npPriority = 0x1B1;  // SHNP_INFORM: show the bubble
            n.csDuration = 15;
            n.hicon = Icon();
            n.clsid = NotifyClass;
            n.hwndSink = sink.Hwnd;
            n.pszHTML = body;
            n.pszTitle = title;
            // Softkeys (SOFTKEYNOTIFY[2]): command to the sink; flags 0 = dismiss the bubble.
            n.sk0Title = sk0; n.sk0Cmd = one ? CmdOpen : CmdFolder;
            n.sk1Title = sk1; n.sk1Cmd = CmdDismiss;
            if (shown) SHNotificationRemove(ref n.clsid, NotifyId);
            int hr = SHNotificationAdd(ref n);
            shown = hr == 0;
            if (hr != 0) Helper.Log("notify failed 0x" + hr.ToString("x8"));
            return hr == 0;
        }
        finally
        {
            Marshal.FreeHGlobal(title); Marshal.FreeHGlobal(body);
            Marshal.FreeHGlobal(sk0); Marshal.FreeHGlobal(sk1);
        }
    }

    static void Remove()
    {
        if (!shown) return;
        Guid g = NotifyClass;
        SHNotificationRemove(ref g, NotifyId);
        shown = false;
    }

    static void OnCommand(int cmd)
    {
        string[] names;
        lock (pending) { names = pending.ToArray(); pending.Clear(); }
        Remove();
        try
        {
            if (cmd == CmdOpen && names.Length == 1)
                Process.Start(new ProcessStartInfo(Path.Combine(ReceiveDir, names[0]), "") { UseShellExecute = true });
            else if (cmd == CmdOpen || cmd == CmdFolder)
                Process.Start(@"\Windows\fexplore.exe", ReceiveDir);  // unquoted: a quoted path opens nothing
        }
        catch (Exception e) { Helper.Log("bt open " + e.Message); }
    }

    static IntPtr icon;

    // The beaming icon, else File Explorer's: the notification needs one for its tray entry.
    static IntPtr Icon()
    {
        if (icon != IntPtr.Zero) return icon;
        foreach (string f in new[] { @"\Windows\beam.exe", @"\Windows\fexplore.exe" })
        {
            IntPtr small = IntPtr.Zero;
            try { if (ExtractIconEx(f, 0, IntPtr.Zero, out small, 1) > 0 && small != IntPtr.Zero) return icon = small; }
            catch (Exception e) { Helper.Log("bt icon " + e.Message); }
        }
        return IntPtr.Zero;
    }

    static string Html(string s)
    {
        return s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");
    }

    // A NUL-terminated UTF-16 copy in native memory (the CF lacks StringToHGlobalUni).
    static IntPtr Str(string s)
    {
        char[] c = (s + "\0").ToCharArray();
        IntPtr p = Marshal.AllocHGlobal(c.Length * 2);
        Marshal.Copy(c, 0, p, c.Length);
        return p;
    }

    // The notification's window (hwndSink) lives on its own thread, which pumps its messages, so
    // taps on links and softkeys are handled while the mailbox loop sleeps.
    static void EnsureSink()
    {
        if (sink != null) return;
        var ready = new ManualResetEvent(false);
        var t = new Thread(delegate ()
        {
            sink = new Sink();
            ready.Set();
            while (true) { Application.DoEvents(); Thread.Sleep(100); }
        });
        t.IsBackground = true;
        t.Start();
        ready.WaitOne();
    }

    class Sink : MessageWindow
    {
        protected override void WndProc(ref Microsoft.WindowsCE.Forms.Message m)
        {
            if (m.Msg == 0x0111)  // WM_COMMAND from a cmd: link or a softkey
            {
                int cmd = m.WParam.ToInt32() & 0xFFFF;
                if (cmd == CmdOpen || cmd == CmdFolder) { OnCommand(cmd); return; }
                if (cmd == CmdDismiss) { lock (pending) pending.Clear(); Remove(); return; }
            }
            base.WndProc(ref m);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SHNOTIFICATIONDATA
    {
        public int cbStruct, dwID, npPriority, csDuration;
        public IntPtr hicon;
        public int grfFlags;
        public Guid clsid;
        public IntPtr hwndSink, pszHTML, pszTitle, lParam;
        // union { SOFTKEYMENU skm; SOFTKEYNOTIFY rgskn[2]; }: { pszTitle, SOFTKEYCMD { wpCmd, grfFlags } } x2
        public IntPtr sk0Title; public int sk0Cmd, sk0Flags;
        public IntPtr sk1Title; public int sk1Cmd, sk1Flags;
        public IntPtr pszTodaySK, pszTodayExec;
    }

    [DllImport("aygshell.dll")] static extern int SHNotificationAdd(ref SHNOTIFICATIONDATA n);
    [DllImport("aygshell.dll")] static extern int SHNotificationRemove(ref Guid clsid, int id);
    [DllImport("coredll.dll", EntryPoint = "ExtractIconExW")] static extern int ExtractIconEx(string file, int index, IntPtr large, out IntPtr small, int count);
}

// "Send via Bluetooth" without a file argument: pick a file to send.
class SendForm : Form
{
    readonly ListView list;
    readonly Label where;
    readonly MenuItem action;
    string dir;

    public SendForm()
    {
        Text = "Bluetooth";
        int w = Screen.PrimaryScreen.WorkingArea.Width, h = Screen.PrimaryScreen.WorkingArea.Height, top;
        using (var g = CreateGraphics()) top = (int)g.MeasureString("Ag", Font).Height + 8;
        where = new Label { Left = 4, Top = 4, Width = w - 8, Height = top - 4 };
        list = new ListView
        {
            Left = 0, Top = top, Width = w, Height = h - top, View = View.Details,
            HeaderStyle = ColumnHeaderStyle.None, FullRowSelect = true,
            Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right | AnchorStyles.Bottom,
        };
        list.Columns.Add("Name", w * 2 / 3, HorizontalAlignment.Left);
        list.Columns.Add("Size", w - w * 2 / 3 - 8, HorizontalAlignment.Right);
        list.ItemActivate += delegate { Go(); };
        list.SelectedIndexChanged += delegate { UpdateAction(); };
        Controls.Add(where);
        Controls.Add(list);
        action = new MenuItem { Text = "Send", Enabled = false };
        action.Click += delegate { Go(); };
        var cancel = new MenuItem { Text = "Cancel" };
        cancel.Click += delegate { Close(); };
        Menu = new MainMenu();
        Menu.MenuItems.Add(action);
        Menu.MenuItems.Add(cancel);
        Browse(Directory.Exists(@"\My Documents") ? @"\My Documents" : @"\");
    }

    void Browse(string path)
    {
        dir = path;
        where.Text = path;
        list.BeginUpdate();
        list.Items.Clear();
        if (path != @"\") Add("..", "", true);
        try
        {
            string[] dirs = Directory.GetDirectories(path), files = Directory.GetFiles(path);
            Array.Sort(dirs, StringComparer.OrdinalIgnoreCase);
            Array.Sort(files, StringComparer.OrdinalIgnoreCase);
            foreach (string d in dirs) Add(Path.GetFileName(d) + @"\", "", true);
            foreach (string f in files) Add(Path.GetFileName(f), FormatSize(new FileInfo(f).Length), false);
        }
        catch (Exception e) { Add("(" + e.Message + ")", "", false); }
        list.EndUpdate();
        UpdateAction();
    }

    void Add(string name, string size, bool folder)
    {
        var item = new ListViewItem(new[] { name, size });
        item.Tag = folder;
        list.Items.Add(item);
    }

    static string FormatSize(long n)
    {
        if (n < 1024) return n + " B";
        if (n < 1024 * 1024) return (n + 1023) / 1024 + " KB";
        return (n / (1024.0 * 1024)).ToString("0.0") + " MB";
    }

    ListViewItem Selected { get { return list.SelectedIndices.Count > 0 ? list.Items[list.SelectedIndices[0]] : null; } }

    void UpdateAction()
    {
        ListViewItem s = Selected;
        action.Enabled = s != null;
        action.Text = s != null && (bool)s.Tag ? "Open" : "Send";
    }

    void Go()
    {
        ListViewItem s = Selected;
        if (s == null) return;
        string name = s.Text;
        if ((bool)s.Tag)
        {
            if (name == "..")
            {
                string up = Path.GetDirectoryName(dir);
                Browse(string.IsNullOrEmpty(up) ? @"\" : up);
            }
            else Browse(Path.Combine(dir, name.TrimEnd('\\')));
            return;
        }
        string error = Bluetooth.Send(Path.Combine(dir, name));
        if (error != null) { MessageBox.Show(error, Text); return; }
        Close();
    }
}
