using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.WindowsCE.Forms;

// Discord for Windows Mobile 6 and later. Every request is made here. HTTPS is the phone's own
// WinInet stack; PocketStylus's secure web proxy (port 443) is what speaks modern TLS to Discord.
class DiscordApp : Form
{
    enum Page { Login, Home, Channels, Chat, Options }

    readonly Label hint = new Label();
    readonly Label status = new Label();
    readonly TextBox tokenBox = new TextBox();
    readonly Spinner spinner = new Spinner();
    readonly RowList list = new RowList();
    readonly TextBox input = new TextBox();
    readonly TabStrip tabs = new TabStrip();
    readonly IconStrip favStrip = new IconStrip();
    PictureBox picture;
    readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
    readonly List<Row> messages = new List<Row>();

    InputPanel sip;
    Page page = Page.Login;
    string meId = "";
    string channelId = "";
    string channelName = "";
    string guildId = "";
    string guildName = "";
    string attached;
    Image shown;
    readonly Dictionary<string, Image> pics = new Dictionary<string, Image>();
    readonly Dictionary<string, int> picFails = new Dictionary<string, int>();
    readonly List<string> picOrder = new List<string>();
    readonly List<string> picQueue = new List<string>();
    int picBusy;
    int backTick;
    bool backed;
    bool allowClose;
    int previewGen;
    int polling;
    int waits;
    volatile bool dead;
    bool auto;
    bool signing;
    int homeTab;
    bool favOn;
    int textPt = 9;
    int imageMax = 220;
    int avatarPx = 32;
    int refreshMs = 5000;
    string storedToken = "";
    Page asidePage = Page.Home;
    readonly List<Row> homeAll = new List<Row>();
    readonly List<Row> asideRows = new List<Row>();
    readonly List<Fav> favs = new List<Fav>();

    [MTAThread]
    static void Main() { Application.Run(new DiscordApp()); }

    DiscordApp()
    {
        Text = "Discord";
        KeyPreview = true;
        MinimizeBox = false;
        BackColor = SystemColors.Window;
        hint.Text = "Sent from Windows Mobile. Turn on Secure Web Proxy in PocketStylus so HTTPS works. Paste your account token. It stays on this phone.";
        hint.ForeColor = SystemColors.WindowText;
        status.Visible = false;
        status.ForeColor = Color.DarkRed;
        status.BackColor = SystemColors.Window;
        tokenBox.Multiline = false;
        tokenBox.PasswordChar = '*';
        tokenBox.MaxLength = 500;
        input.Multiline = true;
        input.ScrollBars = ScrollBars.Vertical;
        input.AcceptsReturn = false;
        input.KeyDown += delegate(object sender, KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Enter) { e.Handled = true; Send(); }
        };
        input.TextChanged += delegate { if (page == Page.Chat) list.StickBottom(); };
        list.Images = Lookup;
        list.Chosen += delegate { if (page != Page.Chat) Open(); };
        tabs.Picked += delegate { homeTab = tabs.Index; ApplyTab(); };
        favStrip.Images = Lookup;
        favStrip.Picked += delegate { OpenFav(favStrip.Selected); };
        Controls.Add(hint);
        Controls.Add(status);
        Controls.Add(tabs);
        Controls.Add(favStrip);
        Controls.Add(tokenBox);
        Controls.Add(spinner);
        Controls.Add(list);
        Controls.Add(input);
        try
        {
            picture = new PictureBox();
            picture.SizeMode = PictureBoxSizeMode.StretchImage;
            picture.BackColor = Color.Black;
            Controls.Add(picture);
        }
        catch (Exception) { picture = null; }
        try
        {
            sip = new InputPanel();
            sip.EnabledChanged += delegate { Relayout(); };
        }
        catch (Exception) { sip = null; }
        string saved = LoadPrefs();
        auto = saved.Length >= 10;
        signing = auto;
        list.TextPt = textPt;
        list.ImageMax = imageMax;
        list.AvatarPx = avatarPx;
        GoTo(Page.Login);
        timer.Tick += delegate { Poll(); };
        ApplyRefresh();
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        Relayout();
        if (auto) BeginInvoke(new ThreadStart(SignIn));
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        Relayout();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        // Smartphone OK / Back is Escape. The same places the title-bar OK goes back from.
        if (e.KeyCode == Keys.Escape)
        {
            e.Handled = true;
            if (!GoBack()) CloseApp();
            return;
        }
        if (!signing && page != Page.Login)
        {
            if (e.KeyCode == Keys.Up) { e.Handled = true; NavVert(-1); return; }
            if (e.KeyCode == Keys.Down) { e.Handled = true; NavVert(1); return; }
            if ((e.KeyCode == Keys.Left || e.KeyCode == Keys.Right) && !Typing())
            {
                e.Handled = true;
                NavHoriz(e.KeyCode == Keys.Right ? 1 : -1);
                return;
            }
            if (e.KeyCode == Keys.Enter && !(page == Page.Chat && input.Focused))
            {
                e.Handled = true;
                if (favOn && favStrip.Visible) OpenFav(favStrip.Selected);
                else if (page == Page.Chat) ReadMessage();
                else Open();
                return;
            }
        }
        else if (!signing && page == Page.Login && e.KeyCode == Keys.Enter)
        {
            e.Handled = true;
            SignIn();
            return;
        }
        base.OnKeyDown(e);
    }

    bool Typing()
    {
        return (page == Page.Chat && input.Focused) || (page == Page.Login && tokenBox.Focused);
    }

    void NavVert(int dir)
    {
        if (favOn && favStrip.Visible)
        {
            if (dir > 0)
            {
                favOn = false;
                favStrip.Keyed = false;
                list.Focus();
            }
            return;
        }
        if (dir < 0 && page == Page.Chat && input.Focused)
        {
            list.Focus();
            return;
        }
        if (dir < 0 && favStrip.Visible && list.AtFirst())
        {
            favOn = true;
            favStrip.Keyed = true;
            favStrip.Move(0);
            return;
        }
        if (dir > 0 && page == Page.Chat && !input.Focused && list.AtLast())
        {
            input.Focus();
            return;
        }
        list.Move(dir);
        if (!input.Focused) list.Focus();
    }

    void NavHoriz(int dir)
    {
        if (favOn && favStrip.Visible)
        {
            favStrip.Move(dir);
            return;
        }
        if (page == Page.Home)
        {
            int next = homeTab + dir;
            if (next < 0 || next > 1 || next == homeTab) return;
            homeTab = next;
            favOn = false;
            favStrip.Keyed = false;
            tabs.Index = next;
            tabs.Invalidate();
            ApplyTab();
            return;
        }
        if (page == Page.Options) CycleOption(dir);
    }

    void FocusList()
    {
        if (signing || page == Page.Login || !list.Visible) return;
        if (page == Page.Chat && input.Focused) return;
        try { list.Focus(); } catch (Exception) { }
    }

    // Compact Framework does not let a form override WndProc. The title-bar OK still posts
    // WM_CLOSE, and canceling OnClosing does not stop the shell from destroying the window.
    delegate IntPtr WndHook(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    WndHook wndHook;
    IntPtr oldProc;

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (oldProc != IntPtr.Zero) return;
        try
        {
            wndHook = HookWnd;
            oldProc = SetWindowLong(Handle, -4, Marshal.GetFunctionPointerForDelegate(wndHook));
        }
        catch (Exception) { oldProc = IntPtr.Zero; }
    }

    IntPtr HookWnd(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (!allowClose && msg == 0x0010 && page != Page.Login && page != Page.Home)
        {
            try { BeginInvoke(new ThreadStart(delegate { GoBack(); })); } catch (Exception) { }
            return IntPtr.Zero;
        }
        return CallWindowProc(oldProc, hWnd, msg, wParam, lParam);
    }

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        if (!allowClose && page != Page.Login && page != Page.Home)
        {
            e.Cancel = true;
            GoBack();
            return;
        }
        base.OnClosing(e);
    }

    bool GoBack()
    {
        int now = Environment.TickCount;
        if (backed && unchecked(now - backTick) < 400) return page != Page.Login && page != Page.Home;
        if (page == Page.Options)
        {
            backed = true;
            backTick = now;
            BackOptions();
            return true;
        }
        if (page == Page.Chat || page == Page.Channels)
        {
            backed = true;
            backTick = now;
            Back();
            return true;
        }
        return false;
    }

    void CloseApp()
    {
        allowClose = true;
        Close();
    }

    protected override void OnClosed(EventArgs e)
    {
        dead = true;
        timer.Enabled = false;
        timer.Dispose();
        ClearPicture();
        lock (pics)
        {
            foreach (Image im in pics.Values) if (im != null) im.Dispose();
            pics.Clear();
        }
        base.OnClosed(e);
    }

    void GoTo(Page next)
    {
        page = next;
        favOn = false;
        favStrip.Keyed = false;
        ForgetFailures();
        bool entry = next == Page.Login && !signing;
        hint.Visible = entry;
        tokenBox.Visible = entry;
        list.Visible = next != Page.Login;
        input.Visible = next == Page.Chat;
        if (picture != null) picture.Visible = next == Page.Chat && shown != null;
        Menu = BuildMenu();
        Relayout();
        try { BeginInvoke(new ThreadStart(FocusList)); } catch (Exception) { }
    }

    MainMenu BuildMenu()
    {
        MainMenu menu = new MainMenu();
        if (page == Page.Login)
        {
            if (signing) return menu;
            menu.MenuItems.Add(Item("Sign in", delegate { SignIn(); }));
            menu.MenuItems.Add(Item("Paste", delegate { PasteInto(tokenBox); }));
            return menu;
        }
        if (page == Page.Options)
        {
            menu.MenuItems.Add(Item("Back", delegate { BackOptions(); }));
            return menu;
        }
        menu.MenuItems.Add(page == Page.Chat ? Item("Send", delegate { Send(); }) : Item("Open", delegate { Open(); }));
        MenuItem more = new MenuItem();
        more.Text = "Menu";
        if (page == Page.Home || page == Page.Channels || page == Page.Chat)
            more.MenuItems.Add(Item("Favorite", delegate { ToggleFav(); }));
        if (page == Page.Chat)
        {
            more.MenuItems.Add(Item("Paste", delegate { PasteInto(input); }));
            more.MenuItems.Add(Item("Attach", delegate { Attach(); }));
            more.MenuItems.Add(Item("Remove file", delegate { attached = null; ShowPendingFile(); }));
            more.MenuItems.Add(Item("Read", delegate { ReadMessage(); }));
            more.MenuItems.Add(Item("Save file", delegate { SaveFile(); }));
        }
        more.MenuItems.Add(Item("Refresh", delegate { RefreshView(); }));
        more.MenuItems.Add(Item("Settings", delegate { ShowOptions(); }));
        if (page != Page.Home) more.MenuItems.Add(Item("Back", delegate { Back(); }));
        more.MenuItems.Add(Item("Sign out", delegate { SignOut(); }));
        menu.MenuItems.Add(more);
        return menu;
    }

    static MenuItem Item(string text, EventHandler click)
    {
        MenuItem item = new MenuItem();
        item.Text = text;
        item.Click += click;
        return item;
    }

    void Relayout()
    {
        int w = ClientSize.Width;
        int h = ClientSize.Height;
        if (w < 20 || h < 20) return;
        if (sip != null && sip.Enabled)
        {
            int cut = sip.Bounds.Height;
            if (cut > h / 2) cut = h / 2;
            if (cut > 0) h -= cut;
        }
        int top = 0;
        if (status.Visible)
        {
            status.Bounds = new Rectangle(2, 0, Math.Max(1, w - 4), 36);
            top = 36;
        }
        if (signing)
        {
            tabs.Visible = false;
            favStrip.Visible = false;
            hint.Visible = false;
            tokenBox.Visible = false;
            list.Visible = false;
            input.Visible = false;
            int s = 64;
            spinner.Bounds = new Rectangle((w - s) / 2, Math.Max(0, (h - s) / 2), s, s);
            spinner.Spin(true);
            return;
        }
        spinner.Spin(false);
        if (page == Page.Login)
        {
            tabs.Visible = false;
            favStrip.Visible = false;
            hint.Bounds = new Rectangle(4, top + 4, Math.Max(1, w - 8), 72);
            tokenBox.Bounds = new Rectangle(4, top + 84, Math.Max(1, w - 8), 24);
            return;
        }
        int boxH = 0;
        if (page == Page.Chat)
        {
            boxH = h / 5;
            if (boxH < 64) boxH = 64;
            if (boxH > 112) boxH = 112;
        }
        int picW = 0;
        int picH = 0;
        if (page == Page.Chat && picture != null && shown != null && shown.Width > 0)
            Contain(shown.Width, shown.Height, w, 120, out picW, out picH);
        input.Bounds = new Rectangle(0, h - boxH, w, boxH);
        if (picture != null)
        {
            picture.Bounds = new Rectangle(0, h - boxH - picH, picW, picH);
            picture.Visible = picH > 0;
        }
        bool home = page == Page.Home;
        tabs.Visible = home;
        tabs.Index = homeTab;
        if (home)
        {
            tabs.Bounds = new Rectangle(0, top, w, 32);
            top += 32;
        }
        List<Fav> shownFav = VisibleFavs();
        bool icons = shownFav.Count > 0 && page != Page.Login && page != Page.Options && page != Page.Chat;
        favStrip.Visible = icons;
        if (icons)
        {
            int favH = avatarPx + 12;
            if (favH < 40) favH = 40;
            favStrip.Icon = avatarPx;
            favStrip.Bounds = new Rectangle(0, top, w, favH);
            favStrip.Show(shownFav);
            top += favH;
        }
        list.Bounds = new Rectangle(0, top, w, Math.Max(1, h - top - boxH - picH));
        if (page == Page.Chat) list.StickBottom();
    }

    void ShowError(string text)
    {
        status.Text = text;
        status.Visible = true;
        Relayout();
    }

    void HideStatus()
    {
        if (!status.Visible) return;
        status.Text = "";
        status.Visible = false;
        Relayout();
    }

    void SignIn()
    {
        string raw = tokenBox.Text;
        if ((raw == null || raw.Length < 10) && storedToken.Length >= 10) raw = storedToken;
        tokenBox.Text = "";
        signing = true;
        HideStatus();
        GoTo(Page.Login);
        Go(delegate { DoSignIn(raw); }, "Signing in…");
    }

    void DoSignIn(string raw)
    {
        string token = Clean(raw);
        if (token.Length < 10) throw new IOException("Paste your account token first.");
        storedToken = token;
        SavePrefs(token);
        Api.Use(token);
        Exception last = null;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                Json me = Api.Me();
                string id = me.Str("id");
                string global = me.Str("global_name");
                string name = global.Length > 0 ? global : me.Str("username");
                List<Row> rows = HomeRows(id, Api.DmChannels(), AllGuilds());
                bool kept = SavePrefs(token);
                Ui(delegate
                {
                    meId = id;
                    ShowHome(rows, name);
                    if (!kept) ShowError("Signed in. The token could not be saved on this phone.");
                });
                return;
            }
            catch (Exception ex)
            {
                last = ex;
                string m = ex.Message == null ? "" : ex.Message;
                if (m.StartsWith("401") || m.IndexOf("account token") >= 0) throw;
                if (attempt < 2) System.Threading.Thread.Sleep(1200);
            }
        }
        if (last != null) throw last;
    }

    void LoadHome()
    {
        Go(delegate
        {
            Json me = Api.Me();
            string id = me.Str("id");
            string global = me.Str("global_name");
            string name = global.Length > 0 ? global : me.Str("username");
            List<Row> rows = HomeRows(id, Api.DmChannels(), AllGuilds());
            Ui(delegate { meId = id; ShowHome(rows, name); });
        }, "Working…");
    }

    void ShowHome(List<Row> rows, string name)
    {
        signing = false;
        channelId = "";
        previewGen++;
        ClearPicture();
        messages.Clear();
        homeAll.Clear();
        if (rows != null) homeAll.AddRange(rows);
        SyncFavAvatars();
        GoTo(Page.Home);
        ApplyTab();
        Text = name.Length > 0 ? name : "Discord";
        HideStatus();
    }

    void ApplyTab()
    {
        List<Row> view = new List<Row>();
        string kind = homeTab == 0 ? "dm" : "guild";
        for (int i = 0; i < homeAll.Count; i++)
            if (homeAll[i].Kind == kind) view.Add(homeAll[i]);
        if (view.Count == 0) view.Add(Head(homeTab == 0 ? "No direct messages" : "No servers"));
        list.Show(view, false);
        Relayout();
        try { BeginInvoke(new ThreadStart(FocusList)); } catch (Exception) { }
    }

    void ApplyRefresh()
    {
        if (refreshMs < 2000) { timer.Enabled = false; return; }
        timer.Interval = refreshMs;
        timer.Enabled = true;
    }

    List<Fav> VisibleFavs()
    {
        List<Fav> view = new List<Fav>();
        for (int i = 0; i < favs.Count; i++)
        {
            Fav f = favs[i];
            if (page == Page.Home && homeTab == 0 && f.Kind == "dm") view.Add(f);
            else if (page == Page.Home && homeTab == 1 && f.Kind == "guild") view.Add(f);
            else if (page == Page.Channels && f.Kind == "channel" && f.Guild == guildId) view.Add(f);
        }
        return view;
    }

    static string AvatarIn(List<Row> rows, string id)
    {
        if (rows == null) return "";
        for (int i = 0; i < rows.Count; i++)
            if (rows[i].Id == id && rows[i].AvatarUrl.Length > 0) return rows[i].AvatarUrl;
        return "";
    }

    string AvatarFor(string id)
    {
        if (id == null || id.Length == 0) return "";
        string url = AvatarIn(homeAll, id);
        if (url.Length > 0) return url;
        return AvatarIn(list.Export(), id);
    }

    void SyncFavAvatars()
    {
        bool changed = false;
        for (int i = 0; i < favs.Count; i++)
        {
            string url = AvatarFor(favs[i].Id);
            if (url.Length == 0 || url == favs[i].AvatarUrl) continue;
            favs[i].AvatarUrl = url;
            changed = true;
        }
        if (changed) SavePrefs(storedToken);
    }

    void OpenFav(Fav f)
    {
        if (f == null || f.Id.Length == 0) return;
        if (f.Kind == "guild") OpenGuild(f.Id, f.Title);
        else
        {
            if (f.Kind == "channel" && f.Guild.Length > 0) guildId = f.Guild;
            OpenChannel(f.Id, f.Title, f.Kind == "channel" && f.Guild.Length > 0);
        }
    }

    void ToggleFav()
    {
        string kind = "";
        string id = "";
        string title = "";
        string avatar = "";
        string guild = "";
        if (page == Page.Chat)
        {
            kind = guildId.Length > 0 ? "channel" : "dm";
            id = channelId;
            title = channelName;
            guild = guildId;
            avatar = AvatarFor(id);
        }
        else
        {
            Row row = list.Selected;
            if (row == null || (row.Kind != "dm" && row.Kind != "guild" && row.Kind != "channel")) return;
            kind = row.Kind;
            id = row.Id;
            title = row.Title;
            avatar = row.AvatarUrl;
            guild = kind == "channel" ? guildId : "";
        }
        if (id.Length == 0) return;
        for (int i = 0; i < favs.Count; i++)
        {
            if (favs[i].Kind == kind && favs[i].Id == id)
            {
                favs.RemoveAt(i);
                SavePrefs(storedToken);
                HideStatus();
                Relayout();
                return;
            }
        }
        Fav f = new Fav();
        f.Kind = kind;
        f.Id = id;
        f.Title = title;
        f.AvatarUrl = avatar;
        f.Guild = guild;
        favs.Add(f);
        SavePrefs(storedToken);
        HideStatus();
        Relayout();
    }

    void ShowOptions()
    {
        if (page != Page.Options)
        {
            asidePage = page;
            asideRows.Clear();
            asideRows.AddRange(list.Export());
        }
        GoTo(Page.Options);
        List<Row> rows = new List<Row>();
        rows.Add(Opt("text", "Text size", TextName()));
        rows.Add(Opt("avatar", "Avatar size", AvatarName()));
        rows.Add(Opt("image", "Image size", ImageName()));
        rows.Add(Opt("refresh", "Refresh", RefreshName()));
        list.Show(rows, false);
        Text = "Settings";
        HideStatus();
    }

    void BackOptions()
    {
        Page dest = asidePage == Page.Options ? Page.Home : asidePage;
        GoTo(dest);
        if (dest == Page.Home) ApplyTab();
        else list.Show(asideRows, dest == Page.Chat);
        if (dest == Page.Chat) Text = channelName.Length > 0 ? channelName : "Discord";
        else if (dest == Page.Channels) Text = guildName.Length > 0 ? guildName : "Discord";
    }

    void CycleOption() { CycleOption(1); }

    static int Step(int value, int[] choices, int dir)
    {
        int i = 0;
        for (; i < choices.Length; i++) if (choices[i] == value) break;
        if (i >= choices.Length) i = 0;
        i += dir < 0 ? -1 : 1;
        if (i < 0) i = choices.Length - 1;
        if (i >= choices.Length) i = 0;
        return choices[i];
    }

    void CycleOption(int dir)
    {
        Row row = list.Selected;
        if (row == null) return;
        if (row.Id == "text") textPt = Step(textPt, new int[] { 8, 10, 12 }, dir);
        else if (row.Id == "avatar")
        {
            avatarPx = Step(avatarPx, new int[] { 24, 32, 48 }, dir);
            list.AvatarPx = avatarPx;
        }
        else if (row.Id == "image")
        {
            imageMax = Step(imageMax, new int[] { 120, 220, 360 }, dir);
            list.ImageMax = imageMax;
            lock (pics)
            {
                foreach (Image im in pics.Values) if (im != null) im.Dispose();
                pics.Clear();
                picOrder.Clear();
                picFails.Clear();
            }
        }
        else if (row.Id == "refresh")
        {
            refreshMs = Step(refreshMs, new int[] { 0, 5000, 15000, 30000, 60000 }, dir);
            ApplyRefresh();
        }
        list.TextPt = textPt;
        SavePrefs(storedToken);
        ShowOptions();
    }

    string TextName()
    {
        if (textPt <= 8) return "Small";
        if (textPt >= 12) return "Large";
        return "Medium";
    }

    string AvatarName()
    {
        if (avatarPx <= 24) return "Small";
        if (avatarPx >= 48) return "Large";
        return "Medium";
    }

    string ImageName()
    {
        if (imageMax <= 120) return "Small";
        if (imageMax >= 360) return "Large";
        return "Medium";
    }

    string RefreshName()
    {
        if (refreshMs <= 0) return "Off";
        if (refreshMs < 10000) return "5 seconds";
        if (refreshMs < 20000) return "15 seconds";
        if (refreshMs < 45000) return "30 seconds";
        return "1 minute";
    }

    static Row Opt(string id, string name, string value)
    {
        Row row = new Row();
        row.Kind = "opt";
        row.Id = id;
        row.Title = name + ": " + value;
        return row;
    }

    void RefreshView()
    {
        if (page == Page.Home) LoadHome();
        else if (page == Page.Channels) LoadChannels();
        else if (page == Page.Chat) ReloadChat();
    }

    void Back()
    {
        previewGen++;
        ClearPicture();
        messages.Clear();
        channelId = "";
        if (page == Page.Chat && guildId.Length > 0) LoadChannels();
        else { guildId = ""; LoadHome(); }
    }

    void SignOut()
    {
        previewGen++;
        storedToken = "";
        favs.Clear();
        DeleteSession();
        SavePrefs("");
        Api.Use("");
        tokenBox.Text = "";
        attached = null;
        channelId = "";
        guildId = "";
        messages.Clear();
        ClearPicture();
        Text = "Discord";
        GoTo(Page.Login);
        HideStatus();
    }

    void PasteInto(TextBox box)
    {
        string text = ReadClipboard();
        if (text.Length == 0)
        {
            ShowError("The clipboard is empty.");
            return;
        }
        text = text.Replace("\r\n", "\n").Replace('\r', '\n');
        if (box == tokenBox) box.Text = text;
        else box.Text = (box.Text == null || box.Text.Length == 0) ? text : box.Text + text;
        box.Focus();
        box.SelectionStart = box.Text.Length;
        HideStatus();
    }

    void Open()
    {
        if (page == Page.Options) { CycleOption(); return; }
        Row row = list.Selected;
        if (row == null || row.Kind == "head") return;
        if (page == Page.Home && row.Kind == "dm") OpenChannel(row.Id, row.Title, false);
        else if (page == Page.Home && row.Kind == "guild") OpenGuild(row.Id, row.Title);
        else if (page == Page.Channels && row.Kind == "channel") OpenChannel(row.Id, row.Title, true);
    }

    void OpenGuild(string id, string name)
    {
        Go(delegate
        {
            List<Row> rows = ChannelRows(Api.GuildChannels(id));
            Ui(delegate
            {
                guildId = id;
                guildName = name;
                channelId = "";
                Fill(rows);
                SyncFavAvatars();
                GoTo(Page.Channels);
                Text = name.Length > 0 ? name : "Discord";
                HideStatus();
            });
        }, "Opening…");
    }

    void LoadChannels()
    {
        string id = guildId;
        string name = guildName;
        Go(delegate
        {
            List<Row> rows = ChannelRows(Api.GuildChannels(id));
            Ui(delegate
            {
                if (guildId != id) return;
                channelId = "";
                Fill(rows);
                SyncFavAvatars();
                GoTo(Page.Channels);
                Text = name.Length > 0 ? name : "Discord";
                HideStatus();
            });
        }, "Opening…");
    }

    void OpenChannel(string id, string name, bool fromGuild)
    {
        Go(delegate
        {
            Json msgs = Api.Messages(id, "");
            Ui(delegate
            {
                if (!fromGuild) guildId = "";
                channelId = id;
                channelName = name;
                attached = null;
                messages.Clear();
                GoTo(Page.Chat);
                ApplyMessages(msgs, false);
                Text = name.Length > 0 ? name : "Discord";
                HideStatus();
            });
        }, "Opening…");
    }

    void ReloadChat()
    {
        string id = channelId;
        Go(delegate
        {
            Json msgs = Api.Messages(id, "");
            Ui(delegate
            {
                if (page != Page.Chat || channelId != id) return;
                messages.Clear();
                ApplyMessages(msgs, false);
                HideStatus();
            });
        }, "Working…");
    }

    void Send()
    {
        if (page != Page.Chat || channelId.Length == 0) return;
        string text = input.Text == null ? "" : input.Text.Trim();
        string file = attached;
        if (text.Length == 0 && (file == null || file.Length == 0))
        {
            ShowError("Write a message first.");
            return;
        }
        if (text.Length > 2000)
        {
            ShowError("Messages can be at most 2000 characters.");
            return;
        }
        string id = channelId;
        Go(delegate
        {
            if (file != null && file.Length > 0) Api.SendFile(id, text, file);
            else Api.SendText(id, text);
            Json msgs = Api.Messages(id, "");
            Ui(delegate
            {
                if (page != Page.Chat || channelId != id) return;
                input.Text = "";
                attached = null;
                messages.Clear();
                ApplyMessages(msgs, false);
                HideStatus();
            });
        }, "Sending…");
    }

    void Attach()
    {
        if (page != Page.Chat) return;
        try
        {
            using (OpenFileDialog dlg = new OpenFileDialog())
            {
                dlg.Filter = "Images (*.jpg;*.png;*.gif;*.bmp)|*.jpg;*.jpeg;*.png;*.gif;*.bmp|All files (*.*)|*.*";
                dlg.FilterIndex = 1;
                try { dlg.InitialDirectory = @"\My Documents"; }
                catch (Exception) { }
                if (dlg.ShowDialog() != DialogResult.OK) return;
                attached = dlg.FileName;
                ShowPendingFile();
            }
        }
        catch (Exception) { ShowError("Could not open the file list."); }
    }

    void ReadMessage()
    {
        Row row = list.Selected;
        if (row == null || row.Kind != "message") return;
        string text = row.Full.Length > 0 ? row.Full : "(no text)";
        if (text.Length > 1500) text = text.Substring(0, 1500);
        MessageBox.Show(text, "Message");
    }

    void SaveFile()
    {
        Row row = list.Selected;
        if (row == null || row.FileUrl.Length == 0)
        {
            ShowError("This message has no file.");
            return;
        }
        string url = row.FileUrl;
        string name = row.FileName.Length > 0 ? row.FileName : "file";
        Go(delegate
        {
            byte[] bytes = Api.Download(url, 8 * 1024 * 1024);
            string dest = UniquePath(SafeFile(name));
            using (FileStream fs = new FileStream(dest, FileMode.Create, FileAccess.Write))
                fs.Write(bytes, 0, bytes.Length);
            Ui(delegate { HideStatus(); });
        }, "Saving…");
    }

    void Poll()
    {
        if (dead || page != Page.Chat || channelId.Length == 0) return;
        if (Interlocked.CompareExchange(ref polling, 1, 0) != 0) return;
        string id = channelId;
        string after = Newest();
        Go(delegate
        {
            try
            {
                Json arr = Api.Messages(id, after);
                Ui(delegate { if (page == Page.Chat && channelId == id) ApplyMessages(arr, true); });
            }
            finally { Interlocked.Exchange(ref polling, 0); }
        }, null);
    }

    void ShowPendingFile()
    {
        ClearPicture();
        if (page != Page.Chat) { Relayout(); return; }
        if (attached == null || attached.Length == 0) { HideStatus(); Relayout(); return; }
        string name = Path.GetFileName(attached);
        if (IsImage("", name))
        {
            try
            {
                FileInfo info = new FileInfo(attached);
                if (info.Length > 0 && info.Length <= 1536 * 1024)
                {
                    byte[] bytes = new byte[info.Length];
                    using (FileStream fs = new FileStream(attached, FileMode.Open, FileAccess.Read))
                    {
                        int got = 0;
                        while (got < bytes.Length)
                        {
                            int n = fs.Read(bytes, got, bytes.Length - got);
                            if (n <= 0) break;
                            got += n;
                        }
                    }
                    ShowImage(bytes);
                }
            }
            catch (Exception) { }
        }
        Relayout();
    }

    void ShowImage(byte[] bytes)
    {
        Image copy = Fit(bytes, 320);
        if (copy == null) return;
        Image old = shown;
        shown = copy;
        if (picture != null) picture.Image = copy;
        if (old != null) old.Dispose();
        Relayout();
    }

    void ClearPicture()
    {
        Image old = shown;
        shown = null;
        if (picture != null) { picture.Image = null; picture.Visible = false; }
        if (old != null) old.Dispose();
    }

    void ApplyMessages(Json arr, bool append)
    {
        int before = list.Count;
        bool follow = !append || before == 0 || list.AtBottom();
        int added = 0;
        foreach (Json m in arr.Items)
        {
            string id = m.Str("id");
            if (id.Length == 0 || Contains(id)) continue;
            messages.Add(MessageRow(m));
            added++;
        }
        if (append && added == 0) return;
        messages.Sort(delegate(Row a, Row b) { return CmpId(a.Id, b.Id); });
        if (messages.Count > 80) messages.RemoveRange(0, messages.Count - 80);
        list.Show(messages, follow);
    }

    void Fill(List<Row> rows) { list.Show(rows, false); }

    bool Contains(string id)
    {
        for (int i = 0; i < messages.Count; i++)
            if (messages[i].Id == id) return true;
        return false;
    }

    string Newest()
    {
        string max = "";
        for (int i = 0; i < messages.Count; i++)
            if (CmpId(messages[i].Id, max) > 0) max = messages[i].Id;
        return max;
    }

    List<Json> AllGuilds()
    {
        List<Json> all = new List<Json>();
        string after = "";
        for (int n = 0; n < 10; n++)
        {
            Json arr = Api.GuildPage(after);
            if (arr.Items.Count == 0) break;
            string max = after;
            foreach (Json g in arr.Items)
            {
                all.Add(g);
                string id = g.Str("id");
                if (CmpId(id, max) > 0) max = id;
            }
            if (arr.Items.Count < 200 || max.Length == 0 || max == after) break;
            after = max;
        }
        return all;
    }

    static List<Row> HomeRows(string self, Json dms, List<Json> guilds)
    {
        List<Json> chats = new List<Json>();
        foreach (Json ch in dms.Items)
        {
            int type = ch.Int("type", -1);
            if (type == 1 || type == 3) chats.Add(ch);
        }
        chats.Sort(delegate(Json a, Json b) { return CmpId(b.Str("last_message_id"), a.Str("last_message_id")); });
        guilds.Sort(delegate(Json a, Json b)
        {
            return string.Compare(a.Str("name"), b.Str("name"), StringComparison.OrdinalIgnoreCase);
        });
        List<Row> rows = new List<Row>();
        rows.Add(Head("Direct messages"));
        if (chats.Count == 0) rows.Add(Head("No direct messages"));
        foreach (Json ch in chats)
        {
            Row row = new Row();
            row.Kind = "dm";
            row.Id = ch.Str("id");
            row.Title = DmTitle(self, ch);
            row.AvatarUrl = DmAvatar(self, ch);
            rows.Add(row);
        }
        rows.Add(Head("Servers"));
        if (guilds.Count == 0) rows.Add(Head("No servers"));
        foreach (Json g in guilds)
        {
            Row row = new Row();
            row.Kind = "guild";
            row.Id = g.Str("id");
            row.Title = g.Str("name");
            if (row.Title.Length == 0) row.Title = "Server";
            string icon = g.Str("icon");
            if (icon.Length > 0) row.AvatarUrl = "https://cdn.discordapp.com/icons/" + row.Id + "/" + icon + ".jpg?size=64";
            rows.Add(row);
        }
        return rows;
    }

    static List<Row> ChannelRows(Json arr)
    {
        List<Json> cats = new List<Json>();
        List<Json> text = new List<Json>();
        foreach (Json c in arr.Items)
        {
            int type = c.Int("type", -1);
            if (type == 4) cats.Add(c);
            else if (type == 0 || type == 5) text.Add(c);
        }
        cats.Sort(ByPos);
        text.Sort(ByPos);
        List<Row> rows = new List<Row>();
        AddChannels(rows, text, "");
        foreach (Json cat in cats)
        {
            string id = cat.Str("id");
            bool any = false;
            foreach (Json c in text) if (c.Str("parent_id") == id) any = true;
            if (!any) continue;
            string name = cat.Str("name");
            rows.Add(Head(name.Length > 0 ? name : "Category"));
            AddChannels(rows, text, id);
        }
        foreach (Json c in text)
        {
            string parent = c.Str("parent_id");
            if (parent.Length == 0) continue;
            bool known = false;
            foreach (Json cat in cats) if (cat.Str("id") == parent) known = true;
            if (known) continue;
            Row row = new Row();
            row.Kind = "channel";
            row.Id = c.Str("id");
            row.Title = c.Str("name");
            if (row.Title.Length == 0) row.Title = "channel";
            rows.Add(row);
        }
        if (rows.Count == 0) rows.Add(Head("No text channels"));
        return rows;
    }

    static void AddChannels(List<Row> rows, List<Json> text, string parent)
    {
        foreach (Json c in text)
        {
            string got = c.Str("parent_id");
            if (parent.Length == 0)
            {
                if (got.Length != 0) continue;
            }
            else if (got != parent) continue;
            Row row = new Row();
            row.Kind = "channel";
            row.Id = c.Str("id");
            row.Title = c.Str("name");
            if (row.Title.Length == 0) row.Title = "channel";
            rows.Add(row);
        }
    }

    static int ByPos(Json a, Json b) { return a.Int("position", 0) - b.Int("position", 0); }

    static string DmTitle(string self, Json ch)
    {
        string name = ch.Str("name");
        if (name.Length > 0) return name;
        StringBuilder b = new StringBuilder();
        foreach (Json u in ch.Get("recipients").Items)
        {
            if (u.Str("id") == self) continue;
            string g = u.Str("global_name");
            string one = g.Length > 0 ? g : u.Str("username");
            if (one.Length == 0) continue;
            if (b.Length > 0) b.Append(", ");
            b.Append(one);
        }
        return b.Length > 0 ? b.ToString() : "Direct message";
    }

    static string DmAvatar(string self, Json ch)
    {
        string icon = ch.Str("icon");
        if (icon.Length > 0) return "https://cdn.discordapp.com/channel-icons/" + ch.Str("id") + "/" + icon + ".jpg?size=64";
        foreach (Json u in ch.Get("recipients").Items)
        {
            if (u.Str("id") == self) continue;
            return UserAvatar(u);
        }
        return "";
    }

    static string UserAvatar(Json user)
    {
        string id = user.Str("id");
        if (id.Length == 0) return "";
        string hash = user.Str("avatar");
        if (hash.Length > 0) return "https://cdn.discordapp.com/avatars/" + id + "/" + hash + ".jpg?size=64";
        return "https://cdn.discordapp.com/embed/avatars/" + (DefaultIndex(id) % 6) + ".png";
    }

    static int DefaultIndex(string id)
    {
        long n = 0;
        for (int i = 0; i < id.Length; i++)
        {
            if (id[i] < '0' || id[i] > '9') break;
            n = n * 10 + (id[i] - '0');
        }
        return (int)((n >> 22) % 6);
    }

    static Row MessageRow(Json msg)
    {
        Json user = msg.Get("author");
        string global = user.Str("global_name");
        string author = global.Length > 0 ? global : user.Str("username");
        if (author.Length == 0) author = "Someone";
        string content = msg.Str("content");
        if (content.Length == 0 && msg.Get("embeds").Items.Count > 0)
        {
            Json embed = msg.Get("embeds").Items[0];
            content = embed.Str("title");
            string desc = embed.Str("description");
            if (desc.Length > 0) content = content.Length > 0 ? content + " " + desc : desc;
        }
        string files = "";
        string image = "";
        int imageW = 0;
        int imageH = 0;
        string fileUrl = "";
        string fileName = "";
        foreach (Json a in msg.Get("attachments").Items)
        {
            string fn = a.Str("filename");
            if (files.Length > 0) files += ", ";
            files += fn.Length > 0 ? fn : "file";
            if (fileUrl.Length == 0)
            {
                fileUrl = a.Str("url");
                fileName = fn;
            }
            if (image.Length == 0 && IsImage(a.Str("content_type"), fn))
            {
                string proxy = a.Str("proxy_url");
                image = proxy.Length > 0 ? proxy : a.Str("url");
                imageW = a.Int("width", 0);
                imageH = a.Int("height", 0);
            }
        }
        if (image.Length == 0)
        {
            foreach (Json e in msg.Get("embeds").Items)
            {
                Json im = e.Get("image");
                string u = im.Str("proxy_url");
                if (u.Length == 0) u = im.Str("url");
                if (u.Length == 0) { im = e.Get("thumbnail"); u = im.Str("proxy_url"); }
                if (u.Length == 0) u = im.Str("url");
                if (u.Length > 0)
                {
                    image = u;
                    imageW = im.Int("width", 0);
                    imageH = im.Int("height", 0);
                    break;
                }
            }
        }
        string full = author;
        if (content.Length > 0) full += "\r\n" + content;
        if (files.Length > 0) full += "\r\n" + files;
        Row row = new Row();
        row.Kind = "message";
        row.Id = msg.Str("id");
        row.Author = author;
        row.Body = content;
        row.Title = author;
        row.Full = full;
        row.AvatarUrl = UserAvatar(user);
        row.ImageUrl = image;
        row.ImageW = imageW;
        row.ImageH = imageH;
        row.FileUrl = fileUrl;
        row.FileName = fileName;
        row.FileNote = files;
        return row;
    }

    // Discord's media proxy scales when width and height are added. The signature (ex, is, hm) stays.
    static string Thumb(string url)
    {
        if (url == null || url.Length == 0) return "";
        int hash = url.IndexOf('#');
        string sized = hash >= 0 ? url.Substring(0, hash) : url;
        string frag = hash >= 0 ? url.Substring(hash) : "";
        int q = sized.IndexOf('?');
        string path = q >= 0 ? sized.Substring(0, q) : sized;
        string query = q >= 0 ? sized.Substring(q + 1) : "";
        StringBuilder b = new StringBuilder();
        int i = 0;
        while (i <= query.Length)
        {
            int amp = query.IndexOf('&', i);
            if (amp < 0) amp = query.Length;
            if (amp > i)
            {
                string pair = query.Substring(i, amp - i);
                if (!pair.StartsWith("width=") && !pair.StartsWith("height="))
                {
                    if (b.Length > 0) b.Append('&');
                    b.Append(pair);
                }
            }
            if (amp >= query.Length) break;
            i = amp + 1;
        }
        if (b.Length > 0) b.Append('&');
        b.Append("width=360");
        return path + "?" + b.ToString() + frag;
    }

    static bool IsImage(string type, string name)
    {
        string t = type == null ? "" : type.ToLower();
        if (t.StartsWith("image/")) return t.IndexOf("svg") < 0 && t.IndexOf("webp") < 0;
        string n = name == null ? "" : name.ToLower();
        return n.EndsWith(".jpg") || n.EndsWith(".jpeg") || n.EndsWith(".png") || n.EndsWith(".gif") || n.EndsWith(".bmp");
    }

    static string OneLine(string s, int cap)
    {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.Length && b.Length < cap; i++)
        {
            char c = s[i];
            b.Append(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
        }
        if (b.Length >= cap && s.Length > cap) b.Append("...");
        return b.ToString();
    }

    static Row Head(string title)
    {
        Row row = new Row();
        row.Kind = "head";
        row.Title = title;
        return row;
    }

    static string Clean(string raw)
    {
        if (raw == null) raw = "";
        string head = raw.Trim();
        if (head.Length >= 4 && head.Substring(0, 4).ToLower() == "bot ")
            throw new IOException("Use your account token. A bot token will not sign in.");
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < raw.Length; i++)
            if (raw[i] > ' ') b.Append(raw[i]);
        return b.ToString();
    }

    static int CmpId(string a, string b)
    {
        if (a == null) a = "";
        if (b == null) b = "";
        if (a.Length != b.Length) return a.Length - b.Length;
        return string.CompareOrdinal(a, b);
    }

    static string SafeFile(string name)
    {
        StringBuilder b = new StringBuilder();
        if (name != null)
        {
            for (int i = 0; i < name.Length; i++)
            {
                char c = name[i];
                if (c == '\\' || c == '/' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
                    b.Append('_');
                else b.Append(c);
            }
        }
        string s = b.ToString().Trim();
        return s.Length == 0 ? "file" : s;
    }

    static string UniquePath(string name)
    {
        string dir = @"\My Documents";
        try { if (!Directory.Exists(dir)) dir = @"\"; }
        catch (Exception) { dir = @"\"; }
        string dest = Path.Combine(dir, name);
        if (!File.Exists(dest)) return dest;
        string stem = Path.GetFileNameWithoutExtension(name);
        string ext = Path.GetExtension(name);
        for (int i = 2; i < 50; i++)
        {
            dest = Path.Combine(dir, stem + " " + i + ext);
            if (!File.Exists(dest)) return dest;
        }
        return Path.Combine(dir, stem + " " + DateTime.Now.Ticks.ToString("x") + ext);
    }

    static bool HasBox(string root)
    {
        string box = Path.Combine(root, ".wmdroid");
        if (Directory.Exists(box)) return true;
        try { return File.Exists(Path.Combine(box, "state.ini")); }
        catch (Exception) { return false; }
    }

    static string DataDir()
    {
        string[] known = new string[] { @"\Storage Card", @"\Storage Card2", @"\SD Card" };
        for (int i = 0; i < known.Length; i++)
        {
            if (!HasBox(known[i])) continue;
            string dir = Path.Combine(known[i], ".wmdroid\\discord");
            try
            {
                if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
                return dir;
            }
            catch (Exception) { }
        }
        try
        {
            string[] roots = Directory.GetDirectories(@"\");
            for (int i = 0; i < roots.Length; i++)
            {
                if (!HasBox(roots[i])) continue;
                string dir = Path.Combine(roots[i], ".wmdroid\\discord");
                if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
                return dir;
            }
        }
        catch (Exception) { }
        string[] dirs = new string[] { @"\My Documents\Discord", @"\Application Data\Discord" };
        for (int i = 0; i < dirs.Length; i++)
        {
            try
            {
                if (!Directory.Exists(dirs[i])) Directory.CreateDirectory(dirs[i]);
                return dirs[i];
            }
            catch (Exception) { }
        }
        return @"\Windows";
    }

    static string SessionPath() { return Path.Combine(DataDir(), "session.txt"); }

    static string[] SessionFiles()
    {
        List<string> paths = new List<string>();
        paths.Add(SessionPath());
        try
        {
            string[] roots = Directory.GetDirectories(@"\");
            for (int i = 0; i < roots.Length; i++)
                paths.Add(Path.Combine(roots[i], ".wmdroid\\discord\\session.txt"));
        }
        catch (Exception) { }
        paths.Add(@"\My Documents\Discord\session.txt");
        paths.Add(@"\Application Data\Discord\session.txt");
        paths.Add(@"\Windows\discord-session.txt");
        return paths.ToArray();
    }

    static string[] TokenFiles()
    {
        List<string> paths = new List<string>();
        paths.Add(Path.Combine(DataDir(), "token.txt"));
        paths.Add(@"\Storage Card\.wmdroid\discord\token.txt");
        paths.Add(@"\Storage Card2\.wmdroid\discord\token.txt");
        paths.Add(@"\My Documents\Discord\token.txt");
        paths.Add(@"\Application Data\Discord\token.txt");
        return paths.ToArray();
    }

    static string ReadTokenFile()
    {
        string[] paths = TokenFiles();
        for (int i = 0; i < paths.Length; i++)
        {
            if (!File.Exists(paths[i])) continue;
            string text = ReadFileText(paths[i]).Trim();
            if (text.Length >= 10) return text;
        }
        return "";
    }

    static void WriteTokenFile(string token)
    {
        if (token == null || token.Length == 0) return;
        byte[] b = Encoding.ASCII.GetBytes(token);
        string[] paths = TokenFiles();
        for (int i = 0; i < paths.Length; i++)
        {
            try
            {
                string dir = Path.GetDirectoryName(paths[i]);
                if (dir == null || dir.Length == 0) continue;
                if (!Directory.Exists(dir))
                {
                    string parent = Path.GetDirectoryName(dir);
                    if (parent == null || parent.Length == 0 || !Directory.Exists(parent)) continue;
                    Directory.CreateDirectory(dir);
                }
                using (FileStream fs = new FileStream(paths[i], FileMode.Create, FileAccess.Write, FileShare.Read))
                {
                    fs.Write(b, 0, b.Length);
                    fs.Flush();
                }
            }
            catch (Exception) { }
        }
    }

    string LoadPrefs()
    {
        string token = ReadTokenFile();
        string[] paths = SessionFiles();
        bool prefs = false;
        for (int i = 0; i < paths.Length; i++)
        {
            if (!File.Exists(paths[i])) continue;
            string got = ReadPrefs(paths[i], !prefs);
            if (!prefs) prefs = true;
            if (token.Length < 10 && got.Length >= 10) token = got;
        }
        if (token.Length >= 10) WriteTokenFile(token);
        storedToken = token;
        return token;
    }

    string ReadPrefs(string path, bool apply)
    {
        string token = "";
        List<Fav> loaded = new List<Fav>();
        try
        {
            string[] lines = ReadFileText(path).Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            for (int n = 0; n < lines.Length; n++)
            {
                string line = lines[n];
                if (line.StartsWith("token=")) token = line.Substring(6);
                else if (!apply) continue;
                else if (line.StartsWith("text=")) textPt = ParseInt(line.Substring(5), textPt);
                else if (line.StartsWith("image=")) imageMax = ParseInt(line.Substring(6), imageMax);
                else if (line.StartsWith("avatar=")) avatarPx = ParseInt(line.Substring(7), avatarPx);
                else if (line.StartsWith("refresh=")) refreshMs = ParseInt(line.Substring(8), refreshMs);
                else if (line.StartsWith("fav="))
                {
                    Fav f = ParseFav(line.Substring(4));
                    if (f != null) loaded.Add(f);
                }
            }
        }
        catch (Exception) { return token; }
        if (apply)
        {
            favs.Clear();
            favs.AddRange(loaded);
            if (textPt < 8 || textPt > 16) textPt = 9;
            if (imageMax < 80 || imageMax > 480) imageMax = 220;
            if (avatarPx < 24 || avatarPx > 48) avatarPx = 32;
            if (refreshMs < 0) refreshMs = 5000;
        }
        return token;
    }

    static int ParseInt(string s, int fallback)
    {
        int n = 0;
        if (s == null || s.Length == 0) return fallback;
        int i = 0;
        if (s[0] == '-') i = 1;
        if (i >= s.Length) return fallback;
        for (; i < s.Length; i++)
        {
            if (s[i] < '0' || s[i] > '9') return fallback;
            n = n * 10 + (s[i] - '0');
        }
        return s[0] == '-' ? -n : n;
    }

    static Fav ParseFav(string line)
    {
        string[] p = line.Split('\t');
        if (p.Length < 2 || p[0].Length == 0 || p[1].Length == 0) return null;
        Fav f = new Fav();
        f.Kind = p[0];
        f.Id = p[1];
        f.Title = p.Length > 2 ? p[2] : "";
        f.AvatarUrl = p.Length > 3 ? p[3] : "";
        f.Guild = p.Length > 4 ? p[4] : "";
        return f;
    }

    bool SavePrefs(string token)
    {
        string path = SessionPath();
        try
        {
            using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read))
            using (StreamWriter w = new StreamWriter(fs, new UTF8Encoding(false)))
            {
                if (token != null && token.Length > 0) w.WriteLine("token=" + token);
                w.WriteLine("text=" + textPt);
                w.WriteLine("image=" + imageMax);
                w.WriteLine("avatar=" + avatarPx);
                w.WriteLine("refresh=" + refreshMs);
                for (int i = 0; i < favs.Count; i++)
                {
                    Fav f = favs[i];
                    w.WriteLine("fav=" + f.Kind + "\t" + f.Id + "\t" + OneLine(f.Title, 40) + "\t" + f.AvatarUrl + "\t" + f.Guild);
                }
                w.Flush();
                fs.Flush();
            }
            if (token != null && token.Length >= 10) WriteTokenFile(token);
            return File.Exists(path);
        }
        catch (Exception) { return false; }
    }

    static string ReadFileText(string path)
    {
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            int n = fs.Length > 262144 ? 262144 : (int)fs.Length;
            if (n <= 0) return "";
            byte[] b = new byte[n];
            int got = 0;
            while (got < n)
            {
                int r = fs.Read(b, got, n - got);
                if (r <= 0) break;
                got += r;
            }
            if (got >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
                return Encoding.UTF8.GetString(b, 3, got - 3);
            return Encoding.UTF8.GetString(b, 0, got);
        }
    }

    static void DeleteSession()
    {
        string[] paths = SessionFiles();
        for (int i = 0; i < paths.Length; i++)
        {
            try { if (File.Exists(paths[i])) File.Delete(paths[i]); }
            catch (Exception) { }
        }
        string[] tokens = TokenFiles();
        for (int i = 0; i < tokens.Length; i++)
        {
            try { if (File.Exists(tokens[i])) File.Delete(tokens[i]); }
            catch (Exception) { }
        }
    }

    static string ReadClipboard()
    {
        const int CF_UNICODETEXT = 13;
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return "";
        if (!OpenClipboard(IntPtr.Zero)) return "";
        try
        {
            IntPtr h = GetClipboardData(CF_UNICODETEXT);
            if (h == IntPtr.Zero) return "";
            string t = Marshal.PtrToStringUni(h);
            return t == null ? "" : t;
        }
        catch (Exception) { return ""; }
        finally { CloseClipboard(); }
    }

    [DllImport("coredll.dll", EntryPoint = "SetWindowLongW")] static extern IntPtr SetWindowLong(IntPtr hWnd, int index, IntPtr value);
    [DllImport("coredll.dll", EntryPoint = "CallWindowProcW")] static extern IntPtr CallWindowProc(IntPtr prev, IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("coredll.dll")] static extern bool OpenClipboard(IntPtr owner);
    [DllImport("coredll.dll")] static extern bool CloseClipboard();
    [DllImport("coredll.dll")] static extern bool IsClipboardFormatAvailable(int format);
    [DllImport("coredll.dll")] static extern IntPtr GetClipboardData(int format);

    static bool IsIcon(string url)
    {
        return url.IndexOf("/avatars/") >= 0 || url.IndexOf("/embed/avatars/") >= 0 || url.IndexOf("/icons/") >= 0 || url.IndexOf("/channel-icons/") >= 0;
    }

    void ForgetFailures()
    {
        lock (pics)
        {
            picFails.Clear();
            List<string> drop = new List<string>();
            foreach (KeyValuePair<string, Image> kv in pics)
                if (kv.Value == null) drop.Add(kv.Key);
            for (int i = 0; i < drop.Count; i++) pics.Remove(drop[i]);
        }
    }

    Image Lookup(string url, out bool pending)
    {
        pending = false;
        if (url == null || url.Length == 0) return null;
        lock (pics)
        {
            if (pics.ContainsKey(url))
            {
                Image got = pics[url];
                if (got != null) return got;
            }
            int fails = picFails.ContainsKey(url) ? picFails[url] : 0;
            if (fails >= 3) return null;
        }
        pending = true;
        Want(url);
        return null;
    }

    void Want(string url)
    {
        if (dead || url == null || url.Length == 0) return;
        bool start = false;
        lock (pics)
        {
            if (picQueue.Contains(url)) return;
            if (pics.ContainsKey(url) && pics[url] != null) return;
            int fails = picFails.ContainsKey(url) ? picFails[url] : 0;
            if (fails >= 3) return;
            if (IsIcon(url)) picQueue.Insert(0, url);
            else picQueue.Add(url);
            if (picBusy == 0) { picBusy = 1; start = true; }
        }
        if (!start) return;
        Thread worker = new Thread(LoadPics);
        worker.IsBackground = true;
        worker.Start();
    }

    void LoadPics()
    {
        try
        {
            while (!dead)
            {
                string url;
                lock (pics)
                {
                    if (picQueue.Count == 0) return;
                    url = picQueue[0];
                    picQueue.RemoveAt(0);
                }
                Image img = FetchPic(url);
                if (dead) { if (img != null) img.Dispose(); return; }
                List<Image> doomed = new List<Image>();
                lock (pics)
                {
                    if (img == null)
                    {
                        int n = picFails.ContainsKey(url) ? picFails[url] + 1 : 1;
                        picFails[url] = n;
                        if (n < 3 && !picQueue.Contains(url)) picQueue.Add(url);
                        else pics[url] = null;
                    }
                    else
                    {
                        if (picFails.ContainsKey(url)) picFails.Remove(url);
                        pics[url] = img;
                        picOrder.Remove(url);
                        picOrder.Add(url);
                        TrimPics(doomed);
                    }
                }
                Ui(delegate
                {
                    for (int i = 0; i < doomed.Count; i++) if (doomed[i] != null) doomed[i].Dispose();
                    list.Invalidate();
                    favStrip.Invalidate();
                });
            }
        }
        finally
        {
            bool again = false;
            lock (pics)
            {
                if (dead || picQueue.Count == 0) picBusy = 0;
                else again = true;
            }
            if (again)
            {
                Thread worker = new Thread(LoadPics);
                worker.IsBackground = true;
                worker.Start();
            }
        }
    }

    void TrimPics(List<Image> doomed)
    {
        while (picOrder.Count > 48)
        {
            string old = picOrder[0];
            picOrder.RemoveAt(0);
            if (!pics.ContainsKey(old)) continue;
            Image gone = pics[old];
            pics.Remove(old);
            if (gone != null) doomed.Add(gone);
        }
    }

    Image FetchPic(string url)
    {
        try
        {
            bool icon = IsIcon(url);
            string fetch = icon ? url : Thumb(url);
            byte[] bytes = ReadCache(fetch);
            if (bytes == null)
            {
                bytes = Api.Download(fetch, icon ? 1024 * 1024 : 512 * 1024);
                if (!IsImage(bytes)) return null;
                WriteCache(fetch, bytes);
            }
            return Fit(bytes, icon ? 96 : imageMax);
        }
        catch (Exception) { return null; }
    }

    static bool IsImage(byte[] b)
    {
        if (b == null || b.Length < 32) return false;
        if (b[0] == 0xFF && b[1] == 0xD8) return true;
        if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) return true;
        if (b[0] == (byte)'G' && b[1] == (byte)'I' && b[2] == (byte)'F') return true;
        if (b[0] == (byte)'B' && b[1] == (byte)'M') return true;
        return false;
    }

    static string CacheDir()
    {
        string dir = Path.Combine(DataDir(), "cache");
        try { if (!Directory.Exists(dir)) Directory.CreateDirectory(dir); }
        catch (Exception) { }
        return dir;
    }

    static string CacheName(string url)
    {
        int cut = url.IndexOf('?');
        string path = cut >= 0 ? url.Substring(0, cut) : url;
        int hash = path.IndexOf('#');
        if (hash >= 0) path = path.Substring(0, hash);
        uint h = 2166136261;
        for (int i = 0; i < path.Length; i++) { h ^= path[i]; h *= 16777619; }
        return h.ToString("x8") + ".img";
    }

    static byte[] ReadCache(string url)
    {
        try
        {
            string path = Path.Combine(CacheDir(), CacheName(url));
            if (!File.Exists(path)) return null;
            FileInfo info = new FileInfo(path);
            if (info.Length < 32 || info.Length > 1024 * 1024) return null;
            byte[] b = new byte[(int)info.Length];
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                int got = 0;
                while (got < b.Length)
                {
                    int n = fs.Read(b, got, b.Length - got);
                    if (n <= 0) break;
                    got += n;
                }
                if (got != b.Length) return null;
            }
            if (!IsImage(b))
            {
                try { File.Delete(path); } catch (Exception) { }
                return null;
            }
            return b;
        }
        catch (Exception) { return null; }
    }

    static void WriteCache(string url, byte[] bytes)
    {
        if (!IsImage(bytes)) return;
        try
        {
            string dir = CacheDir();
            string path = Path.Combine(dir, CacheName(url));
            using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read))
            {
                fs.Write(bytes, 0, bytes.Length);
                fs.Flush();
            }
            string[] files = Directory.GetFiles(dir, "*.img");
            if (files.Length <= 400) return;
            int extra = files.Length - 320;
            for (int n = 0; n < extra; n++)
            {
                int oldest = -1;
                DateTime when = DateTime.MaxValue;
                for (int i = 0; i < files.Length; i++)
                {
                    if (files[i] == null) continue;
                    DateTime t;
                    try { t = new FileInfo(files[i]).LastWriteTime; }
                    catch (Exception) { t = DateTime.MinValue; }
                    if (oldest < 0 || t < when) { oldest = i; when = t; }
                }
                if (oldest < 0) break;
                try { File.Delete(files[oldest]); } catch (Exception) { }
                files[oldest] = null;
            }
        }
        catch (Exception) { }
    }

    static Image Fit(byte[] bytes, int maxEdge)
    {
        using (MemoryStream ms = new MemoryStream(bytes))
        {
            Bitmap raw = new Bitmap(ms);
            Bitmap copy = null;
            try
            {
                int w = raw.Width, h = raw.Height;
                if (w < 1 || h < 1) return null;
                int edge = w > h ? w : h;
                int nw = w, nh = h;
                if (edge > maxEdge)
                {
                    nw = w >= h ? maxEdge : Math.Max(1, w * maxEdge / h);
                    nh = h > w ? maxEdge : Math.Max(1, h * maxEdge / w);
                }
                copy = new Bitmap(nw, nh);
                using (Graphics g = Graphics.FromImage(copy))
                    g.DrawImage(raw, new Rectangle(0, 0, nw, nh), new Rectangle(0, 0, w, h), GraphicsUnit.Pixel);
                int orient = JpegOrientation(bytes);
                if (orient >= 2 && orient <= 8)
                {
                    Bitmap turned = Spin(copy, orient);
                    if (turned != null)
                    {
                        copy.Dispose();
                        copy = turned;
                    }
                }
                Bitmap done = copy;
                copy = null;
                return done;
            }
            finally
            {
                raw.Dispose();
                if (copy != null) copy.Dispose();
            }
        }
    }

    static void Contain(int srcW, int srcH, int maxW, int maxH, out int dw, out int dh)
    {
        if (srcW < 1) srcW = 1;
        if (srcH < 1) srcH = 1;
        if (maxW < 1) maxW = 1;
        if (maxH < 1) maxH = 1;
        dw = maxW;
        dh = (int)((long)srcH * dw / srcW);
        if (dh > maxH)
        {
            dh = maxH;
            dw = (int)((long)srcW * dh / srcH);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    static Bitmap Spin(Bitmap src, int orient)
    {
        int w = src.Width, h = src.Height;
        if (w < 1 || h < 1) return null;
        bool turn = orient >= 5;
        Bitmap dst = new Bitmap(turn ? h : w, turn ? w : h);
        try
        {
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int dx = x, dy = y;
                    switch (orient)
                    {
                        case 2: dx = w - 1 - x; break;
                        case 3: dx = w - 1 - x; dy = h - 1 - y; break;
                        case 4: dy = h - 1 - y; break;
                        case 5: dx = y; dy = x; break;
                        case 6: dx = h - 1 - y; dy = x; break;
                        case 7: dx = h - 1 - y; dy = w - 1 - x; break;
                        case 8: dx = y; dy = w - 1 - x; break;
                    }
                    dst.SetPixel(dx, dy, src.GetPixel(x, y));
                }
            }
            return dst;
        }
        catch (Exception)
        {
            dst.Dispose();
            return null;
        }
    }

    static int JpegOrientation(byte[] b)
    {
        if (b == null || b.Length < 4 || b[0] != 0xFF || b[1] != 0xD8) return 1;
        int i = 2;
        while (i + 4 < b.Length)
        {
            if (b[i] != 0xFF) return 1;
            while (i < b.Length && b[i] == 0xFF) i++;
            if (i >= b.Length) return 1;
            int marker = b[i++];
            if (marker == 0xD9 || marker == 0xDA) return 1;
            if (i + 1 >= b.Length) return 1;
            int len = (b[i] << 8) | b[i + 1];
            if (len < 2 || i + len > b.Length) return 1;
            if (marker == 0xE1 && len > 16
                && b[i + 2] == (byte)'E' && b[i + 3] == (byte)'x' && b[i + 4] == (byte)'i' && b[i + 5] == (byte)'f'
                && b[i + 6] == 0 && b[i + 7] == 0)
                return TiffOrientation(b, i + 8, i + len);
            i += len;
        }
        return 1;
    }

    static int TiffOrientation(byte[] b, int start, int end)
    {
        if (end - start < 8) return 1;
        bool le = b[start] == (byte)'I' && b[start + 1] == (byte)'I';
        if (!le && !(b[start] == (byte)'M' && b[start + 1] == (byte)'M')) return 1;
        if (Tiff16(b, start + 2, end, le) != 42) return 1;
        int ifd = start + Tiff32(b, start + 4, end, le);
        if (ifd < start || ifd + 2 > end) return 1;
        int n = Tiff16(b, ifd, end, le);
        for (int e = 0; e < n; e++)
        {
            int ent = ifd + 2 + e * 12;
            if (ent + 12 > end) break;
            if (Tiff16(b, ent, end, le) == 0x0112) return Tiff16(b, ent + 8, end, le);
        }
        return 1;
    }

    static int Tiff16(byte[] b, int o, int end, bool le)
    {
        if (o < 0 || o + 1 >= end) return 0;
        return le ? b[o] | (b[o + 1] << 8) : (b[o] << 8) | b[o + 1];
    }

    static int Tiff32(byte[] b, int o, int end, bool le)
    {
        if (o < 0 || o + 3 >= end) return 0;
        if (le) return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);
        return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3];
    }

    void Go(ThreadStart job, string busy)
    {
        bool wait = busy != null;
        if (wait)
        {
            waits++;
            Cursor.Current = Cursors.WaitCursor;
        }
        Thread thread = new Thread(delegate
        {
            try { job(); }
            catch (Exception e) { Fail(e); }
            finally
            {
                if (wait) Ui(delegate
                {
                    if (waits > 0) waits--;
                    if (waits == 0) Cursor.Current = Cursors.Default;
                });
            }
        });
        thread.IsBackground = true;
        thread.Start();
    }

    void Fail(Exception e)
    {
        string message = e.Message;
        if (message == null || message.Length == 0) message = "Something went wrong";
        Ui(delegate
        {
            signing = false;
            if (page == Page.Login || message.StartsWith("401"))
            {
                Text = "Discord";
                GoTo(Page.Login);
            }
            ShowError(message);
        });
    }

    void Ui(ThreadStart work)
    {
        if (dead) return;
        try
        {
            if (IsDisposed) return;
            if (InvokeRequired) Invoke(new ThreadStart(work));
            else work();
        }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { }
    }

    // Drawn here, not a ListBox: Windows Mobile Standard throws on ListBox (SettingsApp.cs, Edition).
    // Rows are as tall as their wrapped text and pictures, so scroll is in pixels.
    sealed class RowList : Control
    {
        public delegate Image ImageLookup(string url, out bool pending);
        readonly List<Row> rows = new List<Row>();
        int[] heights = new int[0];
        int selected = -1;
        int scroll;
        public event EventHandler Chosen;
        public ImageLookup Images;
        public int TextPt = 9;
        public int ImageMax = 220;
        public int AvatarPx = 32;
        int dragY;
        int dragScroll;
        bool dragging;
        bool dragged;

        public RowList()
        {
            BackColor = SystemColors.Window;
            TabStop = true;
        }

        public int Count { get { return rows.Count; } }
        public int TopIndex
        {
            get
            {
                int y = 0;
                for (int i = 0; i < rows.Count; i++)
                {
                    if (y + HeightOf(i) > scroll) return i;
                    y += HeightOf(i);
                }
                return 0;
            }
        }
        public Row Selected { get { return selected >= 0 && selected < rows.Count ? rows[selected] : null; } }

        public void Show(List<Row> next, bool end)
        {
            rows.Clear();
            if (next != null) rows.AddRange(next);
            if (rows.Count == 0) { selected = -1; scroll = 0; heights = new int[0]; Invalidate(); return; }
            if (end) selected = rows.Count - 1;
            else
            {
                selected = 0;
                for (int i = 0; i < rows.Count; i++)
                    if (rows[i].Kind != "head") { selected = i; break; }
            }
            Remeasure();
            if (end) scroll = MaxScroll();
            else scroll = 0;
            Invalidate();
        }

        public void Remeasure()
        {
            heights = new int[rows.Count];
            if (ClientSize.Width < 8) return;
            Graphics g;
            try { g = CreateGraphics(); }
            catch (Exception) { return; }
            using (g)
            using (Font font = new Font("Tahoma", TextPt, FontStyle.Regular))
            using (Font bold = new Font("Tahoma", TextPt, FontStyle.Bold))
            {
                for (int i = 0; i < rows.Count; i++)
                    heights[i] = Layout(rows[i], g, font, bold, ClientSize.Width, 0, false, false);
            }
            if (scroll > MaxScroll()) scroll = MaxScroll();
            Invalidate();
        }

        public bool AtBottom() { return scroll >= MaxScroll() - 4; }

        public void StickBottom()
        {
            Remeasure();
            scroll = MaxScroll();
            if (rows.Count > 0) selected = rows.Count - 1;
            Invalidate();
        }

        int HeightOf(int i) { return i >= 0 && i < heights.Length && heights[i] > 0 ? heights[i] : 36; }

        int Total()
        {
            int t = 0;
            for (int i = 0; i < rows.Count; i++) t += HeightOf(i);
            return t;
        }

        int MaxScroll()
        {
            int n = Total() - ClientSize.Height;
            return n > 0 ? n : 0;
        }

        int RowAt(int contentY)
        {
            int y = 0;
            for (int i = 0; i < rows.Count; i++)
            {
                int h = HeightOf(i);
                if (contentY < y + h) return i;
                y += h;
            }
            return rows.Count - 1;
        }

        void Reveal(int index)
        {
            int top = 0;
            for (int i = 0; i < index && i < rows.Count; i++) top += HeightOf(i);
            int h = HeightOf(index);
            if (top < scroll) scroll = top;
            if (top + h > scroll + ClientSize.Height) scroll = top + h - ClientSize.Height;
            if (scroll < 0) scroll = 0;
            if (scroll > MaxScroll()) scroll = MaxScroll();
        }

        public void Move(int dir)
        {
            if (rows.Count == 0 || dir == 0) return;
            int next = selected < 0 ? (dir > 0 ? 0 : rows.Count - 1) : selected + dir;
            while (next >= 0 && next < rows.Count && rows[next].Kind == "head") next += dir;
            if (next < 0 || next >= rows.Count) return;
            selected = next;
            Reveal(selected);
            Invalidate();
        }

        public bool AtFirst()
        {
            for (int i = 0; i < rows.Count; i++)
            {
                if (rows[i].Kind == "head") continue;
                return selected <= i;
            }
            return true;
        }

        public bool AtLast()
        {
            for (int i = rows.Count - 1; i >= 0; i--)
            {
                if (rows[i].Kind == "head") continue;
                return selected >= i;
            }
            return true;
        }

        void Pick(int index, bool activate)
        {
            if (index < 0 || index >= rows.Count) return;
            selected = index;
            Reveal(index);
            Invalidate();
            if (activate && Chosen != null) Chosen(this, EventArgs.Empty);
        }

        public List<Row> Export()
        {
            List<Row> copy = new List<Row>();
            copy.AddRange(rows);
            return copy;
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            Focus();
            dragging = true;
            dragged = false;
            dragY = e.Y;
            dragScroll = scroll;
            Capture = true;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (!dragging) return;
            int dy = dragY - e.Y;
            if (!dragged && (dy > 10 || dy < -10)) dragged = true;
            if (!dragged) return;
            int next = dragScroll + dy;
            if (next < 0) next = 0;
            int max = MaxScroll();
            if (next > max) next = max;
            if (next == scroll) return;
            scroll = next;
            Invalidate();
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            Capture = false;
            dragging = false;
            if (dragged) return;
            Pick(RowAt(e.Y + scroll), true);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Up) { Move(-1); e.Handled = true; }
            else if (e.KeyCode == Keys.Down) { Move(1); e.Handled = true; }
            else if (e.KeyCode == Keys.Enter) { if (Chosen != null) Chosen(this, EventArgs.Empty); e.Handled = true; }
            else base.OnKeyDown(e);
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            bool end = AtBottom();
            Remeasure();
            if (end) scroll = MaxScroll();
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            using (Font font = new Font("Tahoma", TextPt, FontStyle.Regular))
            using (Font bold = new Font("Tahoma", TextPt, FontStyle.Bold))
            {
                int y = -scroll;
                int w = ClientSize.Width;
                for (int i = 0; i < rows.Count; i++)
                {
                    int h = HeightOf(i);
                    if (y + h >= 0 && y < ClientSize.Height)
                        Layout(rows[i], e.Graphics, font, bold, w, y, true, i == selected);
                    y += h;
                }
            }
        }

        int Layout(Row row, Graphics g, Font font, Font bold, int width, int top, bool draw, bool on)
        {
            int lh = (int)Math.Ceiling(g.MeasureString("Ag", font).Height);
            if (lh < 12) lh = 12;
            int gap = row.Kind == "message" ? 12 : 4;
            int avatar = row.AvatarUrl.Length > 0 ? AvatarPx : 0;
            int textX = avatar > 0 ? avatar + 10 : 6;
            int textW = width - textX - 6;
            if (textW < 24) textW = 24;
            int y = 4;
            if (row.Kind == "head")
            {
                y = lh + 8;
                if (draw)
                {
                    using (Brush hi = new SolidBrush(SystemColors.Highlight))
                    using (Brush ink = new SolidBrush(on ? SystemColors.HighlightText : SystemColors.GrayText))
                    {
                        if (on) g.FillRectangle(hi, 0, top, width, y);
                        g.DrawString(row.Title, font, ink, 6, top + 2);
                    }
                }
                return y;
            }
            if (row.Kind != "message")
            {
                int h = Math.Max(avatar + 8, lh + 16);
                if (draw)
                {
                    using (Brush hi = new SolidBrush(SystemColors.Highlight))
                    using (Brush ink = new SolidBrush(on ? SystemColors.HighlightText : SystemColors.WindowText))
                    {
                        if (on) g.FillRectangle(hi, 0, top, width, h);
                        DrawAvatar(g, row.AvatarUrl, 4, top + (h - AvatarPx) / 2, AvatarPx, on);
                        g.DrawString(row.Title, font, ink, textX, top + (h - lh) / 2);
                    }
                }
                return h;
            }
            string wrapped = "";
            int bodyH = 0;
            if (row.Body.Length > 0)
            {
                wrapped = WrapText(g, row.Body, font, textW);
                int lines = 1;
                for (int c = 0; c < wrapped.Length; c++) if (wrapped[c] == '\n') lines++;
                bodyH = lines * lh + 2;
            }
            bool pending = false;
            Image pic = null;
            if (row.ImageUrl.Length > 0 && Images != null) pic = Images(row.ImageUrl, out pending);
            int picW = 0;
            int picH = 0;
            if (pic != null || pending)
            {
                int srcW = pic != null ? pic.Width : row.ImageW;
                int srcH = pic != null ? pic.Height : row.ImageH;
                if (srcW < 1 || srcH < 1) { srcW = 4; srcH = 3; }
                Contain(srcW, srcH, textW, ImageMax, out picW, out picH);
            }
            int chipH = row.FileNote.Length > 0 ? lh + 10 : 0;
            int hMsg = 4 + lh + 2 + bodyH + (picH > 0 ? picH + 6 : 0) + chipH + gap;
            if (hMsg < avatar + gap) hMsg = avatar + gap;
            if (!draw) return hMsg;
            using (Brush hi = new SolidBrush(SystemColors.Highlight))
            using (Brush ink = new SolidBrush(on ? SystemColors.HighlightText : SystemColors.WindowText))
            using (Brush plate = new SolidBrush(Color.FromArgb(226, 228, 232)))
            using (Brush chip = new SolidBrush(Color.FromArgb(232, 234, 238)))
            using (Pen edge = new Pen(Color.FromArgb(180, 184, 192)))
            {
                if (on) g.FillRectangle(hi, 0, top, width, hMsg);
                DrawAvatar(g, row.AvatarUrl, 4, top + 4, AvatarPx, on);
                g.DrawString(row.Author.Length > 0 ? row.Author : row.Title, bold, ink, textX, top + 2);
                int cy = top + 4 + lh + 2;
                if (bodyH > 0) g.DrawString(wrapped, font, ink, textX, cy);
                cy += bodyH;
                if (pic != null && picH > 0)
                {
                    g.DrawImage(pic, new Rectangle(textX, cy, picW, picH), new Rectangle(0, 0, pic.Width, pic.Height), GraphicsUnit.Pixel);
                    cy += picH + 6;
                }
                else if (pending)
                {
                    g.FillRectangle(plate, textX, cy, picW, picH);
                    cy += picH + 6;
                }
                if (chipH > 0)
                {
                    g.FillRectangle(chip, textX, cy, textW, lh + 6);
                    g.DrawRectangle(edge, textX, cy, textW - 1, lh + 5);
                    g.DrawString(row.FileNote, font, ink, new RectangleF(textX + 4, cy + 2, textW - 8, lh + 2));
                }
            }
            return hMsg;
        }

        static string WrapText(Graphics g, string text, Font font, int width)
        {
            string[] lines = text.Replace("\r\n", "\n").Replace("\r", "\n").Split('\n');
            StringBuilder result = new StringBuilder();
            for (int li = 0; li < lines.Length; li++)
            {
                if (li > 0) result.Append('\n');
                string[] words = lines[li].Split(' ');
                string cur = "";
                for (int w = 0; w < words.Length; w++)
                {
                    string word = words[w];
                    if (word.Length == 0) continue;
                    if (g.MeasureString(word, font).Width > width)
                    {
                        if (cur.Length > 0) { result.Append(cur); result.Append('\n'); cur = ""; }
                        StringBuilder piece = new StringBuilder();
                        for (int c = 0; c < word.Length; c++)
                        {
                            piece.Append(word[c]);
                            if (piece.Length > 1 && g.MeasureString(piece.ToString(), font).Width > width)
                            {
                                piece.Length = piece.Length - 1;
                                result.Append(piece.ToString());
                                result.Append('\n');
                                piece.Length = 0;
                                piece.Append(word[c]);
                            }
                        }
                        cur = piece.ToString();
                        continue;
                    }
                    string trial = cur.Length == 0 ? word : cur + " " + word;
                    if (cur.Length > 0 && g.MeasureString(trial, font).Width > width)
                    {
                        result.Append(cur);
                        result.Append('\n');
                        cur = word;
                    }
                    else cur = trial;
                }
                result.Append(cur);
            }
            return result.ToString();
        }

        void DrawAvatar(Graphics g, string url, int x, int y, int size, bool on)
        {
            if (url == null || url.Length == 0 || size < 1) return;
            bool pending;
            Image img = Images != null ? Images(url, out pending) : null;
            Rectangle box = new Rectangle(x, y, size, size);
            if (img != null)
            {
                g.DrawImage(img, box, new Rectangle(0, 0, img.Width, img.Height), GraphicsUnit.Pixel);
                return;
            }
            using (Brush fill = new SolidBrush(on ? SystemColors.HighlightText : Color.FromArgb(196, 200, 206)))
                g.FillRectangle(fill, box);
        }
    }

    sealed class Row
    {
        public string Kind = "";
        public string Id = "";
        public string Title = "";
        public string Author = "";
        public string Body = "";
        public string Full = "";
        public string AvatarUrl = "";
        public string ImageUrl = "";
        public int ImageW;
        public int ImageH;
        public string FileUrl = "";
        public string FileName = "";
        public string FileNote = "";
        public override string ToString() { return Title; }
    }

    sealed class Fav
    {
        public string Kind = "";
        public string Id = "";
        public string Title = "";
        public string AvatarUrl = "";
        public string Guild = "";
    }

    sealed class TabStrip : Control
    {
        public int Index;
        public event EventHandler Picked;

        public TabStrip() { BackColor = SystemColors.Control; }

        protected override void OnPaint(PaintEventArgs e)
        {
            int w = ClientSize.Width;
            int h = ClientSize.Height;
            string[] names = new string[] { "Messages", "Servers" };
            using (Font font = new Font("Tahoma", 9, FontStyle.Bold))
            using (Brush hi = new SolidBrush(SystemColors.Highlight))
            using (Brush ink = new SolidBrush(SystemColors.ControlText))
            using (Brush on = new SolidBrush(SystemColors.HighlightText))
            using (Pen line = new Pen(SystemColors.ControlDark))
            {
                for (int i = 0; i < 2; i++)
                {
                    Rectangle box = new Rectangle(i * w / 2, 0, w / 2, h);
                    if (i == Index) e.Graphics.FillRectangle(hi, box);
                    Brush color = i == Index ? on : ink;
                    SizeF size = e.Graphics.MeasureString(names[i], font);
                    e.Graphics.DrawString(names[i], font, color, box.X + (box.Width - size.Width) / 2, (h - size.Height) / 2);
                }
                e.Graphics.DrawLine(line, 0, h - 1, w, h - 1);
            }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            int next = e.X < ClientSize.Width / 2 ? 0 : 1;
            if (next == Index) return;
            Index = next;
            Invalidate();
            if (Picked != null) Picked(this, EventArgs.Empty);
        }
    }

    sealed class IconStrip : Control
    {
        public delegate Image ImageLookup(string url, out bool pending);
        public ImageLookup Images;
        public event EventHandler Picked;
        public int Icon = 48;
        public bool Keyed;
        readonly List<Fav> items = new List<Fav>();
        int cursor = -1;
        int offset;
        int dragX;
        int dragOffset;
        bool dragging;
        bool dragged;
        public Fav Selected;

        public IconStrip() { BackColor = SystemColors.Window; }

        public void Show(List<Fav> next)
        {
            bool same = next.Count == items.Count;
            if (same)
            {
                for (int i = 0; i < next.Count; i++)
                    if (next[i].Id != items[i].Id) { same = false; break; }
            }
            items.Clear();
            items.AddRange(next);
            if (!same) { offset = 0; cursor = items.Count > 0 ? 0 : -1; }
            if (cursor >= items.Count) cursor = items.Count - 1;
            Selected = cursor >= 0 ? items[cursor] : null;
            int max = MaxOffset();
            if (offset > max) offset = max;
            Invalidate();
        }

        public void Move(int dir)
        {
            if (items.Count == 0) { cursor = -1; Selected = null; Invalidate(); return; }
            if (cursor < 0 || cursor >= items.Count) cursor = 0;
            else cursor += dir;
            if (cursor < 0) cursor = 0;
            if (cursor >= items.Count) cursor = items.Count - 1;
            Selected = items[cursor];
            int cell = Cell();
            int x = cursor * cell;
            if (x < offset) offset = x;
            if (x + cell > offset + ClientSize.Width) offset = x + cell - ClientSize.Width;
            if (offset < 0) offset = 0;
            int max = MaxOffset();
            if (offset > max) offset = max;
            Invalidate();
        }

        int Cell()
        {
            int n = Icon;
            if (n < 32) n = 32;
            return n + 8;
        }

        int MaxOffset()
        {
            int n = items.Count * Cell() - ClientSize.Width;
            return n > 0 ? n : 0;
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            dragging = true;
            dragged = false;
            dragX = e.X;
            dragOffset = offset;
            Capture = true;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (!dragging) return;
            int dx = dragX - e.X;
            if (!dragged && (dx > 10 || dx < -10)) dragged = true;
            if (!dragged) return;
            int next = dragOffset + dx;
            if (next < 0) next = 0;
            int max = MaxOffset();
            if (next > max) next = max;
            if (next == offset) return;
            offset = next;
            Invalidate();
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            Capture = false;
            dragging = false;
            if (dragged || items.Count == 0) return;
            int index = (e.X + offset) / Cell();
            if (index < 0 || index >= items.Count) return;
            cursor = index;
            Selected = items[index];
            if (Picked != null) Picked(this, EventArgs.Empty);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            int cell = Cell();
            int size = Icon;
            if (size < 24) size = 24;
            if (size > cell - 4) size = cell - 4;
            int y = (ClientSize.Height - size) / 2;
            if (y < 0) y = 0;
            using (Brush plate = new SolidBrush(Color.FromArgb(210, 214, 220)))
            {
                for (int i = 0; i < items.Count; i++)
                {
                    int x = i * cell - offset;
                    if (x > ClientSize.Width || x + cell < 0) continue;
                    Fav f = items[i];
                    bool pending = false;
                    Image img = f.AvatarUrl.Length > 0 && Images != null ? Images(f.AvatarUrl, out pending) : null;
                    Rectangle box = new Rectangle(x + (cell - size) / 2, y, size, size);
                    if (img != null)
                        e.Graphics.DrawImage(img, box, new Rectangle(0, 0, img.Width, img.Height), GraphicsUnit.Pixel);
                    else
                        e.Graphics.FillRectangle(plate, box);
                    if (Keyed && i == cursor)
                    {
                        using (Pen ring = new Pen(SystemColors.Highlight))
                            e.Graphics.DrawRectangle(ring, box.X - 2, box.Y - 2, box.Width + 3, box.Height + 3);
                    }
                }
            }
        }
    }

    sealed class Spinner : Control
    {
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
        int step;

        public Spinner()
        {
            BackColor = SystemColors.Window;
            timer.Interval = 120;
            timer.Tick += delegate { step = (step + 1) % 8; Invalidate(); };
        }

        public void Spin(bool on)
        {
            Visible = on;
            timer.Enabled = on;
            if (on) Invalidate();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) timer.Dispose();
            base.Dispose(disposing);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            int cx = ClientSize.Width / 2;
            int cy = ClientSize.Height / 2;
            for (int i = 0; i < 8; i++)
            {
                double ang = i * 45 * Math.PI / 180.0;
                int x = cx + (int)(Math.Cos(ang) * 14) - 3;
                int y = cy + (int)(Math.Sin(ang) * 14) - 3;
                int shade = 48 + ((i + 8 - step) % 8) * 24;
                using (Brush brush = new SolidBrush(Color.FromArgb(shade, shade, shade)))
                    e.Graphics.FillEllipse(brush, x, y, 6, 6);
            }
        }
    }
}
