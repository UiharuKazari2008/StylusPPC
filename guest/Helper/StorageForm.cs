using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

// The Storage Card page (WMDroid settings > Device > Storage Card...): what the card shows, its
// root folder on the phone, and the phone folders shown in it (add, rename, remove). The phone
// app publishes the setup (storage.ini) and lists phone folders on request (hostdir.txt);
// Apply writes storage_new.ini and Windows Mobile restarts with the new card.
class StorageForm : Form
{
    class Folder { public string Name, Path; }

    readonly float k;
    string mode = "private", root = "";
    bool access;
    readonly List<Folder> folders = new List<Folder>();
    ComboBox modeBox;
    Label rootLabel, accessLabel;
    ListBox list;
    Button allow;
    Panel folderPanel;

    static readonly string[] Modes = { "private", "full", "folders" };
    static readonly string[] ModeNames = { "PocketStylus's own folder", "All of the phone's storage", "Chosen folders" };

    public StorageForm()
    {
        Text = "Storage Card";
        using (Graphics g = CreateGraphics()) k = g.DpiX / 96f;
        Helper.Send("storage refresh");  // the phone rewrites storage.ini (e.g. access just granted)
        Thread.Sleep(600);
        Read();
        Build();
        Menu = new MainMenu();
        var apply = new MenuItem { Text = "Apply" };
        apply.Click += delegate { Apply(); };
        Menu.MenuItems.Add(apply);
        var cancel = new MenuItem { Text = "Cancel" };
        cancel.Click += delegate { Close(); };
        Menu.MenuItems.Add(cancel);
    }

    int S(int px) { return (int)(px * k); }
    int W { get { return Screen.PrimaryScreen.WorkingArea.Width - S(16); } }

    void Read()
    {
        try
        {
            using (var r = new StreamReader(Path.Combine(Helper.Box, "storage.ini"), Encoding.UTF8))
            {
                string l;
                while ((l = r.ReadLine()) != null)
                {
                    if (l.StartsWith("mode=")) mode = l.Substring(5);
                    else if (l.StartsWith("root=")) root = l.Substring(5);
                    else if (l.StartsWith("access=")) access = l.Substring(7) == "1";
                    else if (l.StartsWith("folder="))
                    {
                        string v = l.Substring(7);
                        int t = v.IndexOf('\t');
                        if (t > 0) folders.Add(new Folder { Name = v.Substring(0, t), Path = v.Substring(t + 1) });
                    }
                }
            }
        }
        catch (Exception e) { Helper.Log("storage page: " + e.Message); }
    }

    void Build()
    {
        var p = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        Controls.Add(p);
        int y = S(6);
        p.Controls.Add(new Label { Text = "Storage Card shows", Left = S(8), Top = y, Width = W, Height = S(20) });
        y += S(20);
        modeBox = new ComboBox { Left = S(8), Top = y, Width = W, DropDownStyle = ComboBoxStyle.DropDownList };
        foreach (string n in ModeNames) modeBox.Items.Add(n);
        modeBox.SelectedIndex = Math.Max(0, Array.IndexOf(Modes, mode));
        modeBox.SelectedIndexChanged += delegate { mode = Modes[modeBox.SelectedIndex]; Sync(); };
        p.Controls.Add(modeBox);
        y += S(34);

        accessLabel = new Label { Text = "PocketStylus needs All files access on the phone for this.", Left = S(8), Top = y, Width = W, Height = S(36) };
        p.Controls.Add(accessLabel);
        allow = new Button { Text = "Allow on the phone...", Left = S(8), Top = y + S(36), Width = W, Height = S(28) };
        allow.Click += delegate { Helper.Send("open allfiles"); };
        p.Controls.Add(allow);
        y += S(70);

        folderPanel = new Panel { Left = 0, Top = y, Width = W + S(16), Height = S(420) };
        p.Controls.Add(folderPanel);
        int fy = 0;
        folderPanel.Controls.Add(new Label { Text = "Card root (files at the top of the card)", Left = S(8), Top = fy, Width = W, Height = S(20) });
        fy += S(20);
        rootLabel = new Label { Left = S(8), Top = fy, Width = W, Height = S(36) };
        folderPanel.Controls.Add(rootLabel);
        fy += S(36);
        var change = new Button { Text = "Change...", Left = S(8), Top = fy, Width = (W - S(8)) / 2, Height = S(28) };
        change.Click += delegate { string d = Browse(); if (d != null) { root = d; Sync(); } };
        folderPanel.Controls.Add(change);
        var own = new Button { Text = "PocketStylus's", Left = S(16) + (W - S(8)) / 2, Top = fy, Width = (W - S(8)) / 2, Height = S(28) };
        own.Click += delegate { root = ""; Sync(); };
        folderPanel.Controls.Add(own);
        fy += S(38);
        folderPanel.Controls.Add(new Label { Text = "Phone folders on the card", Left = S(8), Top = fy, Width = W, Height = S(20) });
        fy += S(20);
        list = new ListBox { Left = S(8), Top = fy, Width = W, Height = S(150) };
        folderPanel.Controls.Add(list);
        fy += S(156);
        int bw = (W - S(16)) / 3;
        var add = new Button { Text = "Add...", Left = S(8), Top = fy, Width = bw, Height = S(28) };
        add.Click += delegate
        {
            string d = Browse();
            if (d == null) return;
            foreach (Folder f in folders) if (f.Path == d) return;
            folders.Add(new Folder { Name = System.IO.Path.GetFileName(d), Path = d });
            Sync();
        };
        folderPanel.Controls.Add(add);
        var rename = new Button { Text = "Rename...", Left = S(16) + bw, Top = fy, Width = bw, Height = S(28) };
        rename.Click += delegate
        {
            if (list.SelectedIndex < 0) return;
            Folder f = folders[list.SelectedIndex];
            string n = Ask("Name on the card", f.Name);
            if (n == null) return;
            foreach (char c in "\\/:*?\"<>|") n = n.Replace(c.ToString(), "");
            if (n.Trim().Length > 0) { f.Name = n.Trim(); Sync(); }
        };
        folderPanel.Controls.Add(rename);
        var remove = new Button { Text = "Remove", Left = S(24) + 2 * bw, Top = fy, Width = bw, Height = S(28) };
        remove.Click += delegate { if (list.SelectedIndex >= 0) { folders.RemoveAt(list.SelectedIndex); Sync(); } };
        folderPanel.Controls.Add(remove);
        Sync();
    }

    void Sync()
    {
        bool needsAccess = mode != "private";
        accessLabel.Visible = allow.Visible = needsAccess && !access;
        folderPanel.Visible = mode == "folders";
        rootLabel.Text = root.Length > 0 ? root : "PocketStylus's own folder";
        int sel = list.SelectedIndex;
        list.Items.Clear();
        foreach (Folder f in folders) list.Items.Add(f.Name + "  (" + f.Path + ")");
        if (sel >= 0 && sel < list.Items.Count) list.SelectedIndex = sel;
    }

    void Apply()
    {
        try
        {
            var sb = new StringBuilder();
            sb.Append("mode=").Append(mode).Append("\r\n");
            sb.Append("root=").Append(root).Append("\r\n");
            foreach (Folder f in folders) sb.Append("folder=").Append(f.Name).Append('\t').Append(f.Path).Append("\r\n");
            string path = Path.Combine(Helper.Box, "storage_new.ini");
            using (var w = new StreamWriter(path + ".tmp", false, new UTF8Encoding(false))) w.Write(sb.ToString());
            if (File.Exists(path)) File.Delete(path);
            File.Move(path + ".tmp", path);
            Helper.Send("storage apply");
            Close();
        }
        catch (Exception e) { MessageBox.Show("Couldn't apply: " + e.Message, "Storage Card"); }
    }

    // ---- Phone folder browser ----

    // Lists one phone folder through the phone app (null = failed, with a message shown).
    List<string> List(string path, out string shown)
    {
        shown = path;
        string f = Path.Combine(Helper.Box, "hostdir.txt");
        try { if (File.Exists(f)) File.Delete(f); } catch { }
        Helper.Send("hostdir " + path);
        for (int i = 0; i < 40 && !File.Exists(f); i++) Thread.Sleep(100);
        if (!File.Exists(f)) { MessageBox.Show("The phone didn't answer.", "Storage Card"); return null; }
        var dirs = new List<string>();
        using (var r = new StreamReader(f, Encoding.UTF8))
        {
            string l;
            while ((l = r.ReadLine()) != null)
            {
                if (l.StartsWith("err=")) { MessageBox.Show(l.Substring(4), "Storage Card"); return null; }
                if (l.StartsWith("path=")) shown = l.Substring(5);
                else if (l.StartsWith("d ")) dirs.Add(l.Substring(2));
            }
        }
        return dirs;
    }

    // Returns the chosen phone folder, or null.
    string Browse()
    {
        string current = "";
        string result = null;
        var dlg = new Form { Text = "Choose a phone folder" };
        var label = new Label { Left = S(4), Top = S(4), Width = W + S(8), Height = S(36) };
        var box = new ListBox { Left = S(4), Top = S(42), Width = W + S(8), Height = S(330) };
        dlg.Controls.Add(label);
        dlg.Controls.Add(box);
        List<string> items = null;
        Action0 load = null;
        load = delegate
        {
            string shown;
            List<string> got = List(current, out shown);
            if (got == null) return;
            items = got;
            current = shown;
            label.Text = current.Length > 0 ? current : "Phone storage";
            box.Items.Clear();
            if (current.Length > 0) box.Items.Add("[..]");
            foreach (string d in items) box.Items.Add(current.Length > 0 ? d : d);
        };
        box.SelectedIndexChanged += delegate { };
        EventHandler open = delegate
        {
            int i = box.SelectedIndex;
            if (i < 0) return;
            if (current.Length > 0 && i == 0)
            {
                // Up a level; the storage volumes when at one's top.
                string parent = Path.GetDirectoryName(current);
                current = (parent == null || parent == "/storage" || parent == "/storage/emulated" || parent == "/") ? "" : parent.Replace('\\', '/');
            }
            else
            {
                string name = items[current.Length > 0 ? i - 1 : i];
                current = current.Length > 0 ? current.TrimEnd('/') + "/" + name : name;
            }
            load();
        };
        var menu = new MainMenu();
        var choose = new MenuItem { Text = "Choose" };
        choose.Click += delegate { if (current.Length > 0) { result = current; dlg.Close(); } };
        menu.MenuItems.Add(choose);
        var openItem = new MenuItem { Text = "Open" };
        openItem.Click += open;
        menu.MenuItems.Add(openItem);
        dlg.Menu = menu;
        box.KeyDown += delegate(object s, KeyEventArgs e) { if (e.KeyCode == Keys.Enter) open(s, e); };
        box.DoubleClick += open;
        load();
        dlg.ShowDialog();
        return result;
    }

    delegate void Action0();

    // A one-line text prompt; null if cancelled.
    string Ask(string title, string value)
    {
        string result = null;
        var dlg = new Form { Text = title };
        var tb = new TextBox { Text = value, Left = S(8), Top = S(12), Width = W };
        dlg.Controls.Add(tb);
        var menu = new MainMenu();
        var ok = new MenuItem { Text = "OK" };
        ok.Click += delegate { result = tb.Text; dlg.Close(); };
        menu.MenuItems.Add(ok);
        var cancel = new MenuItem { Text = "Cancel" };
        cancel.Click += delegate { dlg.Close(); };
        menu.MenuItems.Add(cancel);
        dlg.Menu = menu;
        dlg.ShowDialog();
        return result;
    }
}
