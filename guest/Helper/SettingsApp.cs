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
        var phone = new MenuItem { Text = "More" };  // the phone app's own settings pages
        phone.Click += delegate { Helper.Send("open settings"); };
        Menu.MenuItems.Add(phone);
        debounce = new Timer { Interval = 350, Enabled = false };
        debounce.Tick += delegate
        {
            debounce.Enabled = false;
            if (debounceKey != null) Helper.Send("set " + debounceKey + "=" + debounceValue);
            debounceKey = null;
        };
        if (Edition.Smartphone) BuildKeypad(done, phone);
        else Build();
    }

    // ---------------------------------------------------------------------------------------
    // Windows Mobile Standard (Smartphone): no touch screen, and the Compact Framework has no
    // TabControl, Button or TrackBar there (NotSupportedException). One page at a time, every
    // setting a ComboBox (a keypad spinner: Up/Down move between settings, Left/Right change the
    // value), pages and actions in the right soft key's menu; Done (left soft key) applies the
    // restart settings as the Pro version's Apply does.
    MenuItem more, phoneItem;
    readonly List<string> pageNames = new List<string>();
    readonly List<Control> pageControls = new List<Control>();

    void BuildKeypad(MenuItem done, MenuItem phone)
    {
        AutoScroll = true;
        Menu.MenuItems.Clear();
        Menu.MenuItems.Add(done);
        more = new MenuItem { Text = "Menu" };
        Menu.MenuItems.Add(more);
        phoneItem = phone;
        if (entries.Count == 0)
        {
            Controls.Add(new Label { Text = "Open PocketStylus on the phone once, then try again.", Left = S(4), Top = S(4), Width = ClientSize.Width - S(8), Height = S(60) });
            more.MenuItems.Add(phone);
            return;
        }
        foreach (Entry e in entries) if (!pageNames.Contains(e.Page)) pageNames.Add(e.Page);
        ShowPage(pageNames[0]);
    }

    // The right soft key's menu: the pages, then this page's actions (there are no buttons on
    // Standard), then "On phone".
    void PageMenu(string name)
    {
        more.MenuItems.Clear();
        foreach (string pg in pageNames)
        {
            string target = pg;
            var item = new MenuItem { Text = pg, Checked = pg == name };
            item.Click += delegate { ShowPage(target); };
            more.MenuItems.Add(item);
        }
        bool first = true;
        foreach (Entry e in entries)
        {
            if (e.Page != name || e.Kind != "action") continue;
            if (first) more.MenuItems.Add(new MenuItem { Text = "-" });
            first = false;
            string command = e.Value == "local:storage" ? "open storage" : e.Value;  // the phone's page
            var item = new MenuItem { Text = e.Label };
            item.Click += delegate { Helper.Send(command); };
            more.MenuItems.Add(item);
        }
        more.MenuItems.Add(new MenuItem { Text = "-" });
        more.MenuItems.Add(phoneItem);
    }

    void ShowPage(string name)
    {
        Text = "PocketStylus: " + name;
        PageMenu(name);
        SuspendLayout();
        foreach (Control c in pageControls) { Controls.Remove(c); c.Dispose(); }
        pageControls.Clear();
        AutoScrollPosition = new Point(0, 0);
        int w = ClientSize.Width - S(8), y = S(2);
        bool restart = false, actions = false;
        Control first = null;
        foreach (Entry e in entries)
        {
            if (e.Page != name) continue;
            if (e.Kind == "action") { actions = true; continue; }
            var cb = new ComboBox { Left = S(4), Width = w, DropDownStyle = ComboBoxStyle.DropDownList };
            var values = new List<string>();
            if (e.Kind == "bool")
            {
                values.Add("0"); cb.Items.Add("Off");
                values.Add("1"); cb.Items.Add("On");
            }
            else if (e.Kind == "choice")
            {
                foreach (string opt in e.Extra.Split(';'))
                {
                    int eq = opt.IndexOf('=');
                    if (eq <= 0) continue;
                    values.Add(opt.Substring(0, eq));
                    cb.Items.Add(opt.Substring(eq + 1));
                }
            }
            else if (e.Kind == "slider") SliderSteps(e, values, cb);
            else continue;
            if (e.Restart) restart = true;
            y = AddLine(new Label { Text = e.Label, Left = S(4), Top = y, Width = w, Height = S(18) }, y) - S(4);
            cb.Top = y;
            int sel = values.IndexOf(e.Value);
            if (sel >= 0) cb.SelectedIndex = sel;
            Entry entry = e;
            cb.SelectedIndexChanged += delegate { if (cb.SelectedIndex >= 0) Changed(entry, values[cb.SelectedIndex], entry.Kind == "slider"); };
            y = AddLine(cb, y);
            if (first == null) first = cb;
        }
        string note = (restart ? "Some of these apply with Done: Windows Mobile restarts. " : "") + (actions ? "More in Menu." : "");
        if (note.Length > 0) AddLine(new Label { Text = note, Left = S(4), Top = y + S(4), Width = w, Height = S(34) }, y);
        ResumeLayout();
        if (first != null) first.Focus();
    }

    int AddLine(Control c, int y)
    {
        // Keep the focused line on screen: the form scrolls with the keypad focus.
        if (c is ComboBox) c.GotFocus += delegate { ScrollTo(c); };
        Controls.Add(c);
        pageControls.Add(c);
        return y + c.Height + S(4);
    }

    void ScrollTo(Control c)
    {
        int scrolled = -AutoScrollPosition.Y;
        int top = c.Top - S(20), bottom = c.Bottom + S(4);
        if (top < 0) AutoScrollPosition = new Point(0, Math.Max(0, scrolled + top));
        else if (bottom > ClientSize.Height) AutoScrollPosition = new Point(0, scrolled + bottom - ClientSize.Height);
    }

    // A slider as about a dozen steps between its ends, plus its current value.
    static void SliderSteps(Entry e, List<string> values, ComboBox cb)
    {
        int min = 0, max = 100, v = 0;
        try
        {
            int c = e.Extra.IndexOf(':');
            if (c > 0) { min = int.Parse(e.Extra.Substring(0, c)); max = int.Parse(e.Extra.Substring(c + 1)); }
            else max = int.Parse(e.Extra);
        }
        catch { }
        try { v = int.Parse(e.Value); } catch { }
        var steps = new List<int>();
        int step = Math.Max(1, (max - min) / 12);
        for (int x = min; x < max; x += step) steps.Add(x);
        steps.Add(max);
        if (v >= min && v <= max && !steps.Contains(v)) { steps.Add(v); steps.Sort(); }
        foreach (int x in steps) { values.Add(x.ToString()); cb.Items.Add(x.ToString()); }
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

// Which Windows Mobile edition this is: Standard (Smartphone, keypad only) lacks the Compact
// Framework's touch controls (Button, TabControl, TrackBar, ListBox...), so forms build a keypad UI.
static class Edition
{
    static int smartphone = -1;

    public static bool Smartphone
    {
        get
        {
            if (smartphone < 0)
            {
                try { smartphone = Microsoft.WindowsCE.Forms.SystemSettings.Platform == Microsoft.WindowsCE.Forms.WinCEPlatform.Smartphone ? 1 : 0; }
                catch (Exception) { smartphone = 0; }
            }
            return smartphone == 1;
        }
    }
}
