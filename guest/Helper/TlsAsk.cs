using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Certificate warnings from the secure web proxy (core/src/net/tlsproxy.cpp, docs/research-tls-proxy.md).
// When a site's certificate fails Android's check, the proxy holds the browser's request and the
// app sends (Helper.Execute):
//   tlsask <id> <seconds> <host>
//   <the message, several lines>
// The helper shows the message as a Windows Mobile warning box, "Security Warning" with Yes / No
// (No is the default), and answers "tlsask <id> yes", "tlsask <id> no", or "tlsask <id> timeout"
// when nobody answered within <seconds>: the box then closes itself, because the proxy has stopped
// waiting and shown its error page. One box at a time; others wait their turn within their own time.
// The mailbox loop never waits for the box (it runs on its own thread), so the watchdog stays quiet.
static class TlsAsk
{
    const string Caption = "Security Warning";

    class Ask { public string id, host, text; public int deadline; }

    static readonly Queue<Ask> queue = new Queue<Ask>();
    static bool running;

    public static string Command(string rest)
    {
        int nl = rest.IndexOf('\n');
        string head = (nl >= 0 ? rest.Substring(0, nl) : rest).Trim();
        string text = nl >= 0 ? rest.Substring(nl + 1).Replace("\r", "").Trim() : "";
        string[] p = head.Split(' ');
        if (p.Length < 2 || text.Length == 0) return "bad args";
        int secs;
        try { secs = int.Parse(p[1]); }
        catch (Exception) { return "bad args"; }
        var a = new Ask();
        a.id = p[0];
        a.host = p.Length > 2 ? p[2] : "";
        a.text = text.Replace("\n", "\r\n");
        a.deadline = Environment.TickCount + secs * 1000;
        lock (queue)
        {
            queue.Enqueue(a);
            if (!running)
            {
                running = true;
                var t = new Thread(Run);
                t.IsBackground = true;
                t.Start();
            }
        }
        return "asking";
    }

    static void Run()
    {
        while (true)
        {
            Ask a;
            lock (queue)
            {
                if (queue.Count == 0) { running = false; return; }
                a = queue.Dequeue();
            }
            string answer;
            try
            {
                int left = a.deadline - Environment.TickCount;
                answer = left < 2000 ? "timeout" : Show(a, left);
            }
            catch (Exception e) { Helper.Log("tlsask " + e.Message); answer = "no"; }
            Helper.Log("tlsask " + a.id + " " + a.host + ": " + answer);
            Helper.Send("tlsask " + a.id + " " + answer);
        }
    }

    // The warning box until it is answered or `ms` have passed (then it is closed as "timeout").
    static string Show(Ask a, int ms)
    {
        bool done = false, expired = false;
        var closer = new Thread(delegate ()
        {
            int end = Environment.TickCount + ms;
            while (!done && end - Environment.TickCount > 0) Thread.Sleep(250);
            // MessageBox has no time limit: press its No for the user, retrying until it is gone.
            for (int i = 0; !done && i < 20; i++)
            {
                expired = true;
                IntPtr w = OurBox();
                if (w != IntPtr.Zero) PostMessage(w, WM_COMMAND, IDNO, 0);
                Thread.Sleep(500);
            }
        });
        closer.IsBackground = true;
        closer.Start();
        int r = MessageBox(IntPtr.Zero, a.text, Caption,
            MB_YESNO | MB_ICONEXCLAMATION | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
        done = true;
        if (r == IDYES) return "yes";
        return expired ? "timeout" : "no";
    }

    // This process's warning box: a top-level window with the caption (IE may have its own).
    static IntPtr OurBox()
    {
        uint self = (uint)Process.GetCurrentProcess().Id;
        var title = new StringBuilder(64);
        for (IntPtr w = FindWindow(null, Caption); w != IntPtr.Zero; w = GetWindow(w, GW_HWNDNEXT))
        {
            title.Length = 0;
            GetWindowText(w, title, title.Capacity);
            if (title.ToString() != Caption) continue;
            uint pid;
            GetWindowThreadProcessId(w, out pid);
            if (pid == self) return w;
        }
        return IntPtr.Zero;
    }

    const uint MB_YESNO = 0x4, MB_ICONEXCLAMATION = 0x30, MB_DEFBUTTON2 = 0x100, MB_SETFOREGROUND = 0x10000, MB_TOPMOST = 0x40000;
    const int IDYES = 6, IDNO = 7, WM_COMMAND = 0x111;
    const uint GW_HWNDNEXT = 2;
    [DllImport("coredll.dll", EntryPoint = "MessageBoxW")] static extern int MessageBox(IntPtr owner, string text, string caption, uint type);
    [DllImport("coredll.dll", EntryPoint = "FindWindowW")] static extern IntPtr FindWindow(string cls, string name);
    [DllImport("coredll.dll")] static extern IntPtr GetWindow(IntPtr w, uint cmd);
    [DllImport("coredll.dll", EntryPoint = "GetWindowTextW")] static extern int GetWindowText(IntPtr w, StringBuilder text, int max);
    [DllImport("coredll.dll")] static extern uint GetWindowThreadProcessId(IntPtr w, out uint pid);
    [DllImport("coredll.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(IntPtr w, int msg, int wParam, int lParam);
}
