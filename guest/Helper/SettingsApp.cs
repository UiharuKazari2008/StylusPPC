using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

// WMDroid's settings app inside Windows Mobile (Settings > System > WMDroid). Its pages are built
// from settings.ini, which the phone app writes (GuestSettings.kt), so every setting the phone
// offers here shows up without a helper update. Changes apply live through "set key=value";
// settings marked restart are sent when the app closes with Done (Windows Mobile then restarts).
class SettingsForm : Form
{
    class Entry
    {
        public string Page, Key, Kind, Label, Value, Extra;
        public bool Restart;
    }

    readonly List<Entry> entries = new List<Entry>();
    readonly Dictionary<string, string> pendingRestart = new Dictionary<string, string>();
    readonly float k;  // pixels per 96-dpi pixel
    Timer debounce;
    string debounceKey, debounceValue;

    public SettingsForm()
    {
        Text = "PocketStylus";
        using (Graphics g = CreateGraphics()) k = g.DpiX / 96f;
        ReadCatalogue();
        Menu = new MainMenu();
        var done = new MenuItem { Text = "Done" };
        done.Click += delegate { Finish(); };
        Menu.MenuItems.Add(done);
        var phone = new MenuItem { Text = "On phone" };
        phone.Click += delegate { Helper.Send("open settings"); };
        Menu.MenuItems.Add(phone);
        debounce = new Timer { Interval = 350, Enabled = false };
        debounce.Tick += delegate
        {
            debounce.Enabled = false;
            if (debounceKey != null) Helper.Send("set " + debounceKey + "=" + debounceValue);
            debounceKey = null;
        };
        Build();
    }

    int S(int px) { return (int)(px * k); }

    void ReadCatalogue()
    {
        try
        {
            using (var r = new StreamReader(Path.Combine(Helper.Box, "settings.ini")))
            {
                string line;
                while ((line = r.ReadLine()) != null)
                {
                    string[] f = line.Split('\t');
                    if (f.Length < 7) continue;
                    entries.Add(new Entry { Page = f[0], Key = f[1], Kind = f[2], Label = f[3], Value = f[4], Restart = f[5] == "1", Extra = f[6] });
                }
            }
        }
        catch (Exception e) { Helper.Log("settings app: " + e.Message); }
    }

    void Build()
    {
        if (entries.Count == 0)
        {
            Controls.Add(new Label { Text = "Open PocketStylus on the phone once, then try again.", Left = S(8), Top = S(8), Width = ClientSize.Width - S(16), Height = S(60) });
            return;
        }
        var tabs = new TabControl { Dock = DockStyle.Fill };
        var pages = new Dictionary<string, Panel>();
        var ys = new Dictionary<string, int>();
        foreach (Entry e in entries)
        {
            if (!pages.ContainsKey(e.Page))
            {
                var tp = new TabPage { Text = e.Page };
                var panel = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
                tp.Controls.Add(panel);
                tabs.TabPages.Add(tp);
                pages[e.Page] = panel;
                ys[e.Page] = S(6);
            }
            int y = ys[e.Page];
            Panel p = pages[e.Page];
            int w = Screen.PrimaryScreen.WorkingArea.Width - S(20);
            if (e.Kind == "bool") y = AddBool(p, e, y, w);
            else if (e.Kind == "choice") y = AddChoice(p, e, y, w);
            else if (e.Kind == "slider") y = AddSlider(p, e, y, w);
            else if (e.Kind == "action") y = AddAction(p, e, y, w);
            ys[e.Page] = y;
        }
        // Pages with restart-only settings get an Apply button (Windows Mobile restarts).
        var withApply = new List<string>();
        foreach (Entry e in entries)
            if (e.Restart && !withApply.Contains(e.Page))
            {
                withApply.Add(e.Page);
                Panel p = pages[e.Page];
                int w = Screen.PrimaryScreen.WorkingArea.Width - S(20);
                int y = ys[e.Page] + S(6);
                p.Controls.Add(new Label { Text = "These apply when you tap Apply: Windows Mobile restarts.", Left = S(6), Top = y, Width = w, Height = S(36) });
                var apply = new Button { Text = "Apply", Left = S(6), Top = y + S(38), Width = w, Height = S(30) };
                apply.Click += delegate { Finish(); };
                p.Controls.Add(apply);
            }
        Controls.Add(tabs);
    }

    int AddBool(Panel p, Entry e, int y, int w)
    {
        var c = new CheckBox { Text = e.Label, Checked = e.Value == "1", Left = S(6), Top = y, Width = w, Height = S(26) };
        c.CheckStateChanged += delegate { Changed(e, c.Checked ? "1" : "0", false); };
        p.Controls.Add(c);
        return y + S(28);
    }

    int AddChoice(Panel p, Entry e, int y, int w)
    {
        p.Controls.Add(new Label { Text = e.Label, Left = S(6), Top = y, Width = w, Height = S(20) });
        y += S(20);
        var cb = new ComboBox { Left = S(6), Top = y, Width = w, DropDownStyle = ComboBoxStyle.DropDownList };
        var values = new List<string>();
        foreach (string opt in e.Extra.Split(';'))
        {
            int eq = opt.IndexOf('=');
            if (eq <= 0) continue;
            values.Add(opt.Substring(0, eq));
            cb.Items.Add(opt.Substring(eq + 1));
        }
        int sel = values.IndexOf(e.Value);
        if (sel >= 0) cb.SelectedIndex = sel;
        cb.SelectedIndexChanged += delegate { if (cb.SelectedIndex >= 0) Changed(e, values[cb.SelectedIndex], false); };
        p.Controls.Add(cb);
        return y + S(32);
    }

    int AddSlider(Panel p, Entry e, int y, int w)
    {
        int min = 0, max = 100;
        try
        {
            int c = e.Extra.IndexOf(':');
            if (c > 0) { min = int.Parse(e.Extra.Substring(0, c)); max = int.Parse(e.Extra.Substring(c + 1)); }
            else max = int.Parse(e.Extra);
        }
        catch { }
        int v = 0;
        try { v = int.Parse(e.Value); } catch { }
        var label = new Label { Text = e.Label + ": " + v, Left = S(6), Top = y, Width = w, Height = S(20) };
        p.Controls.Add(label);
        y += S(20);
        var t = new TrackBar { Minimum = min, Maximum = max, Value = Math.Min(max, Math.Max(min, v)), Left = S(2), Top = y, Width = w + S(4), Height = S(30), TickFrequency = Math.Max(1, (max - min) / 10) };
        t.ValueChanged += delegate
        {
            label.Text = e.Label + ": " + t.Value;
            Changed(e, t.Value.ToString(), true);
        };
        p.Controls.Add(t);
        return y + S(34);
    }

    // A button that sends its command to the phone app (e.g. "open storage").
    int AddAction(Panel p, Entry e, int y, int w)
    {
        var b = new Button { Text = e.Label, Left = S(6), Top = y + S(4), Width = w, Height = S(30) };
        string command = e.Value;
        // "local:storage" opens the Storage Card page here; anything else goes to the phone app.
        b.Click += delegate
        {
            if (command == "local:storage") { using (var f = new StorageForm()) f.ShowDialog(); }
            else Helper.Send(command);
        };
        p.Controls.Add(b);
        return y + S(40);
    }

    // Live settings go to the phone at once (sliders once they stop moving); restart ones wait for Done.
    void Changed(Entry e, string value, bool slider)
    {
        e.Value = value;
        if (e.Restart) { pendingRestart[e.Key] = value; return; }
        if (slider)
        {
            debounceKey = e.Key;
            debounceValue = value;
            debounce.Enabled = false;
            debounce.Enabled = true;
            return;
        }
        Helper.Send("set " + e.Key + "=" + value);
    }

    void Finish()
    {
        if (debounceKey != null) Helper.Send("set " + debounceKey + "=" + debounceValue);
        foreach (KeyValuePair<string, string> kv in pendingRestart) Helper.Send("set " + kv.Key + "=" + kv.Value);
        Close();
    }
}
