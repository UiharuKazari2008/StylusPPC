using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

// Who is in this phone's InfraWAN room (Settings > Infrared; the room starts as "lobby").
// While this window is open it tells the phone "lobby" through the helper mailbox, and the phone
// writes \Storage Card\.wmdroid\lobby.txt (InfraredBridge.kt). Closing the window sends "lobby stop".
class InfraLobby : Form
{
    [MTAThread]
    static void Main() { Application.Run(new InfraLobby()); }

    readonly Timer timer = new Timer();
    readonly List<string> peers = new List<string>();
    string status = "Looking for the lobby…";
    string room = "";
    string you = "";
    bool up;
    int scroll;
    int beat;
    int linePx = 16;
    bool drag;
    int dragY, dragScroll;

    InfraLobby()
    {
        Text = "InfraLobby";
        BackColor = SystemColors.Window;
        var exit = new MenuItem { Text = "Exit" };
        exit.Click += delegate { Close(); };
        var refresh = new MenuItem { Text = "Refresh" };
        refresh.Click += delegate { Tell("lobby"); Read(); Invalidate(); };
        Menu = new MainMenu();
        Menu.MenuItems.Add(exit);
        Menu.MenuItems.Add(refresh);
        timer.Interval = 1000;
        timer.Tick += delegate { Pulse(); };
        timer.Enabled = true;
        Pulse();
    }

    void Pulse()
    {
        if (beat % 2 == 0) Tell("lobby");
        beat++;
        Read();
        Invalidate();
    }

    protected override void OnClosed(EventArgs e)
    {
        timer.Enabled = false;
        Tell("lobby stop");
        base.OnClosed(e);
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        int max = Math.Max(0, peers.Count - 4);
        if (e.KeyCode == Keys.Up) scroll = Math.Max(0, scroll - 1);
        else if (e.KeyCode == Keys.Down) scroll = Math.Min(max, scroll + 1);
        else { base.OnKeyDown(e); return; }
        Invalidate();
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        drag = true;
        dragY = e.Y;
        dragScroll = scroll;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (!drag) return;
        int max = Math.Max(0, peers.Count - 4);
        int next = dragScroll + (dragY - e.Y) / Math.Max(1, linePx);
        next = Math.Max(0, Math.Min(max, next));
        if (next == scroll) return;
        scroll = next;
        Invalidate();
    }

    protected override void OnMouseUp(MouseEventArgs e) { drag = false; }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        int w = ClientSize.Width;
        int pad = Math.Max(4, w / 40);
        using (Font big = new Font("Tahoma", 10, FontStyle.Bold))
        using (Font font = new Font("Tahoma", 9, FontStyle.Regular))
        using (Brush text = new SolidBrush(SystemColors.WindowText))
        using (Brush dim = new SolidBrush(SystemColors.GrayText))
        using (Brush ok = new SolidBrush(Color.FromArgb(0, 110, 55)))
        {
            // Font.Size is points. Compact Framework's Font has no GetHeight, so scale by DPI.
            float line = font.Size * g.DpiY / 72f + 3;
            linePx = (int)line;
            float y = pad - scroll * line;
            g.DrawString(room.Length > 0 ? room : "InfraWAN", big, text, pad, y);
            y += big.Size * g.DpiY / 72f + 6;
            float wide = w - pad * 2;
            g.DrawString(status, font, up ? ok : dim, new RectangleF(pad, y, wide, line * 3));
            y += line * 3;
            g.DrawString("You", font, dim, pad, y);
            y += line;
            g.DrawString(you.Length > 0 ? you : "This phone", font, text, pad * 2, y);
            y += line + 6;
            g.DrawString(peers.Count == 0 ? "Nobody else is here" : "Also here  " + peers.Count, font, dim, pad, y);
            y += line;
            for (int i = 0; i < peers.Count; i++)
            {
                g.DrawString(peers[i], font, text, pad * 2, y);
                y += line;
            }
        }
    }

    void Read()
    {
        string path = Path.Combine(Box(), "lobby.txt");
        if (!File.Exists(path)) return;
        try
        {
            var next = new List<string>();
            string nextRoom = room, nextStatus = status, nextYou = you;
            bool nextUp = up;
            using (var r = new StreamReader(path, Encoding.UTF8))
            {
                string line;
                while ((line = r.ReadLine()) != null)
                {
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string key = line.Substring(0, eq);
                    string value = line.Substring(eq + 1);
                    if (key == "room") nextRoom = value;
                    else if (key == "up") nextUp = value == "1";
                    else if (key == "status") nextStatus = value;
                    else if (key == "you") nextYou = value;
                    else if (key == "peer" && value.Length > 0) next.Add(value);
                }
            }
            room = nextRoom;
            status = nextStatus;
            you = nextYou;
            up = nextUp;
            peers.Clear();
            peers.AddRange(next);
        }
        catch (Exception) { }
    }

    // The helper's mailbox (\Storage Card\.wmdroid, or whichever volume holds it). Same files
    // GuestBridge.kt reads.
    static string box;

    static string Box()
    {
        if (box != null && Directory.Exists(box)) return box;
        string main = @"\Storage Card\.wmdroid";
        if (Directory.Exists(main)) { box = main; return box; }
        try
        {
            foreach (string d in Directory.GetDirectories(@"\"))
            {
                string b = Path.Combine(d, ".wmdroid");
                if (Directory.Exists(b)) { box = b; return box; }
            }
        }
        catch (Exception) { }
        box = main;
        return box;
    }

    static void Tell(string msg)
    {
        try
        {
            string dir = Path.Combine(Box(), "out");
            Directory.CreateDirectory(dir);
            string name = DateTime.Now.Ticks.ToString("x16");
            string tmp = Path.Combine(dir, name + ".tmp");
            using (var w = new StreamWriter(tmp, false, new UTF8Encoding(false))) w.Write(msg);
            File.Move(tmp, Path.Combine(dir, name + ".msg"));
        }
        catch (Exception) { }
    }
}
