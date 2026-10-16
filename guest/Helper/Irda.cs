using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// Infrared (docs/research-irda.md). Windows Mobile's IrDA stack is complete in every image: the
// protocol (irdastk.dll), the serial-IR miniport irsir.dll bound as Comm\Irsir1 (Parms\Port = 2,
// so it opens "COM2:"), IrCOMM on COM3: and OBEX beaming. Only the port driver is missing: DE's
// Drivers\BuiltIn\IRDA2410 key names IRDA2410.Dll, which is not in the image. The stock serial
// driver serial_smdk2410.dll has the same BSP's IR UART as DeviceArrayIndex 1, so the key is
// pointed at it on UART2 (0x50008000, IRQ 15); after one restart irsir opens COM2: at boot and the
// emulator core (core/src/board/irda.cpp) is the transceiver on the other end.
//
// When a program starts an infrared operation the core tells the app, which asks here ("irda ask"):
// a page offers the two ways out of the emulator, nearby phones over Bluetooth LE or the InfraWAN
// relay over the Internet, and the choice goes back as "irda use ble|wan|cancel remember=0|1".
static class Irda
{
    const string PortKey = @"Drivers\BuiltIn\IRDA2410";
    const string StateKey = @"Software\WMDroid";
    const int UartBase = 0x50008000, UartIrq = 15;
    const int SetupVersion = 2;  // bump when Provision() changes (2: OBEX over IrDA, own device address)

    // ---- Port provisioning (once per image, one restart) ----

    // Writes the port's driver key; true when it changed (a restart loads the driver). Runs before
    // the system patch, whose own restart then covers it on a fresh image.
    public static bool Provision()
    {
        if (Helper.Get("irda.port", "1") == "0") return false;
        try
        {
            int com = IrsirPort();
            if (com <= 0) return false;  // no serial-IR miniport in this image
            bool changed = false;
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(PortKey))
            {
                changed |= Set(k, "Dll", "serial_smdk2410.dll");
                changed |= Set(k, "Prefix", "COM");
                changed |= Set(k, "Index", com);
                changed |= Set(k, "MemBase", UartBase);
                changed |= Set(k, "MemLen", 0x2C);
                changed |= Set(k, "Irq", UartIrq);
                changed |= Set(k, "InterruptBitsShift", 6);    // UART2's sub-interrupt bits
                changed |= Set(k, "ISTTimeouts", 0x200);
                changed |= Set(k, "DeviceArrayIndex", 1);     // the driver's IR UART (0 = COM1's DTR/DSR UART)
                changed |= Set(k, "Port", "COM" + com + ":");
                changed |= Set(k, "FriendlyName", "Infrared Port (PocketStylus)");
            }
            // The OBEX server listens on IrDA only when ObexServiceMask bit 0 is set (obexsrvr.dll
            // reads it next to "protocols"); the images ship 2 (Bluetooth only), so nothing could
            // receive a beam over infrared even with "Receive all incoming beams" on.
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"Software\Microsoft\Obex"))
            {
                object m = k.GetValue("ObexServiceMask");
                int mask = m is int ? (int)m : 0;
                if ((mask & 1) == 0) { k.SetValue("ObexServiceMask", mask | 1); changed = true; }
            }
            // IrLAP's 32-bit device address (Comm\IrDA DevAddr, read when the stack starts). Every
            // copy of an image starts with the same one; on a shared medium (an InfraWAN room) two
            // devices with one address can't tell each other apart, so each device draws its own once.
            using (RegistryKey st = Registry.LocalMachine.CreateSubKey(StateKey))
            using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"Comm\IrDA"))
            {
                if (st.GetValue("IrdaAddr") == null)
                {
                    int addr = new Random(Environment.TickCount ^ (int)DateTime.Now.Ticks).Next(0x100, 0x7FFFFFFF);
                    k.SetValue("DevAddr", addr);
                    st.SetValue("IrdaAddr", addr);
                    changed = true;
                }
            }
            if (changed)
            {
                RegFlushKey(HKEY_LOCAL_MACHINE);
                Helper.Log("irda: infrared port on UART2 as COM" + com + ": (after a restart)");
            }
            return changed;
        }
        catch (Exception e)
        {
            Helper.Log("irda: " + e.GetType().Name + " " + e.Message);
            return false;
        }
    }

    // The port was just set up and nothing else restarted the system: restart once. Returns true
    // when a restart is under way (the caller should stop).
    public static bool Restart()
    {
        try
        {
            using (RegistryKey st = Registry.LocalMachine.CreateSubKey(StateKey))
            {
                // Once: if the key keeps changing (something else rewrites it), don't loop.
                // Once per version of these settings: if they keep changing (something else rewrites
                // them), don't loop.
                object v = st.GetValue("IrdaTried");
                if (v is int && (int)v >= SetupVersion) { Helper.Log("irda: settings changed again; no second restart"); return false; }
                st.SetValue("IrdaTried", SetupVersion);
            }
            RegFlushKey(HKEY_LOCAL_MACHINE);
            Helper.Send("patched irda");  // the app treats the restarted system as its new boot state
            Helper.Log("irda: resetting to load the infrared port");
            Thread.Sleep(1500);  // let the app read the message
            Patch.SoftReset();
            return true;
        }
        catch (Exception e)
        {
            Helper.Log("irda: " + e.GetType().Name + " " + e.Message);
            return false;
        }
    }

    // The COM port number irsir.dll opens (Comm\Irsir1\Parms Port), 0 without the miniport.
    static int IrsirPort()
    {
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"Comm\Irsir1\Parms"))
        {
            object v = k == null ? null : k.GetValue("Port");
            return v is int ? (int)v : 0;
        }
    }

    static bool Set(RegistryKey k, string name, object value)
    {
        object old = k.GetValue(name);
        if (old != null && old.ToString() == value.ToString() && old.GetType() == value.GetType()) return false;
        k.SetValue(name, value);
        return true;
    }

    // ---- Commands from the app ----

    // irda ask [last]     show the transport choice (last: the previous choice, preselected)
    // irda pick <choice>  answer an open question as if tapped (tests)
    // irda status <text>  the app's transport state, shown on the open page
    public static string Command(string args)
    {
        string verb = args, rest = "";
        int sp = args.IndexOf(' ');
        if (sp > 0) { verb = args.Substring(0, sp); rest = args.Substring(sp + 1).Trim(); }
        switch (verb)
        {
            case "ask": Ask(rest); return "ok";
            case "pick":
            {
                ChooserForm f = open;
                if (f == null) return "no question";
                f.Pick(rest, false);
                return "ok";
            }
            case "status": status = rest; return "ok";
            case "port": return "com" + IrsirPort() + " tried=" + TriedFlag();
            case "obex": return Obex(rest);
            default: return "unknown";
        }
    }

    // obex [stop|start|refresh|restart]: the OBEX server service (Services\OBEX, OBX0:), for diagnostics.
    static string Obex(string how)
    {
        IntPtr h = CreateFile("OBX0:", 0, 0, IntPtr.Zero, 3 /* OPEN_EXISTING */, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) return "open failed " + Marshal.GetLastWin32Error();
        try
        {
            string r = "";
            int got;
            if (how == "stop" || how == "restart" || how == "")
                r += "stop=" + DeviceIoControl(h, IOCTL_SERVICE_STOP, IntPtr.Zero, 0, IntPtr.Zero, 0, out got, IntPtr.Zero) + "/" + Marshal.GetLastWin32Error() + " ";
            if (how == "start" || how == "restart" || how == "")
                r += "start=" + DeviceIoControl(h, IOCTL_SERVICE_START, IntPtr.Zero, 0, IntPtr.Zero, 0, out got, IntPtr.Zero) + "/" + Marshal.GetLastWin32Error() + " ";
            if (how == "refresh")
                r += "refresh=" + DeviceIoControl(h, IOCTL_SERVICE_REFRESH, IntPtr.Zero, 0, IntPtr.Zero, 0, out got, IntPtr.Zero) + "/" + Marshal.GetLastWin32Error();
            return r.Trim();
        }
        finally { CloseHandle(h); }
    }

    const int IOCTL_SERVICE_START = 0x01040004, IOCTL_SERVICE_STOP = 0x01040008, IOCTL_SERVICE_REFRESH = 0x0104000C;
    [DllImport("coredll.dll", SetLastError = true)]
    static extern IntPtr CreateFile(string name, int access, int share, IntPtr sec, int disposition, int flags, IntPtr template);
    [DllImport("coredll.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, int code, IntPtr inBuf, int inSize, IntPtr outBuf, int outSize, out int returned, IntPtr overlapped);
    [DllImport("coredll.dll")] static extern bool CloseHandle(IntPtr h);

    static object TriedFlag()
    {
        using (RegistryKey st = Registry.LocalMachine.OpenSubKey(StateKey))
            return st == null ? null : st.GetValue("IrdaTried");
    }

    static volatile ChooserForm open;
    static volatile string status = "";

    static void Ask(string last)
    {
        ChooserForm f = open;
        if (f != null) { f.Raise(); return; }
        var t = new Thread(delegate ()
        {
            try
            {
                var form = new ChooserForm(last);
                open = form;
                Application.Run(form);
            }
            catch (Exception e) { Helper.Log("irda ask: " + e.Message); Helper.Send("irda use cancel remember=0"); }
            finally { open = null; }
        });
        t.IsBackground = true;
        t.Start();
    }

    delegate void Act();

    // The question page: full-screen like the system's own Beam pages, two large choices.
    class ChooserForm : Form
    {
        readonly float k;
        readonly CheckBox remember;
        readonly Label note;
        bool answered;
        readonly System.Windows.Forms.Timer poll = new System.Windows.Forms.Timer();

        public ChooserForm(string last)
        {
            Text = "Infrared";
            using (Graphics g = CreateGraphics()) k = g.DpiX / 96f;
            int w = Screen.PrimaryScreen.WorkingArea.Width - S(16), y = S(8);
            var head = new Label { Text = "A program is starting an infrared transfer.", Left = S(8), Top = y, Width = w, Height = S(36) };
            head.Font = new Font(head.Font.Name, head.Font.Size, FontStyle.Bold);
            Controls.Add(head);
            y += S(38);
            Controls.Add(new Label { Text = "Reach other devices through:", Left = S(8), Top = y, Width = w, Height = S(20) });
            y += S(24);
            Button ble = Choice("Nearby phones (Bluetooth LE)", "ble", y, w, last);
            y += S(46);
            Controls.Add(new Label { Text = "PocketStylus phones close by, as if their IR ports faced each other.", Left = S(16), Top = y, Width = w - S(8), Height = S(30) });
            y += S(34);
            Choice("InfraWAN (Internet)", "wan", y, w, last);
            y += S(46);
            Controls.Add(new Label { Text = "Everyone in your InfraWAN room, wherever they are (set the server in the phone's PocketStylus settings).", Left = S(16), Top = y, Width = w - S(8), Height = S(44) });
            y += S(48);
            remember = new CheckBox { Text = "Remember my choice", Left = S(8), Top = y, Width = w, Height = S(24) };
            Controls.Add(remember);
            y += S(30);
            note = new Label { Text = "", Left = S(8), Top = y, Width = w, Height = S(36) };
            Controls.Add(note);
            Menu = new MainMenu();
            var cancel = new MenuItem { Text = "Cancel" };
            cancel.Click += delegate { Pick("cancel", false); };
            Menu.MenuItems.Add(cancel);
            if (ble != null) ble.Focus();
            poll.Interval = 500;
            poll.Tick += delegate { if (note.Text != status) note.Text = status; };
            poll.Enabled = true;
        }

        int S(int px) { return (int)(px * k); }

        Button Choice(string text, string value, int y, int w, string last)
        {
            var b = new Button { Text = text, Left = S(8), Top = y, Width = w, Height = S(40) };
            if (value == last) b.Font = new Font(b.Font.Name, b.Font.Size, FontStyle.Bold);
            b.Click += delegate { Pick(value, remember.Checked); };
            Controls.Add(b);
            return b;
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            Raise();
        }

        public void Raise()
        {
            if (InvokeRequired) { BeginInvoke(new Act(Raise)); return; }
            BringToFront();
            SetForegroundWindow(Handle);
        }

        // Thread-safe: the mailbox thread answers through here for tests ("irda pick").
        public void Pick(string choice, bool keep)
        {
            if (InvokeRequired) { BeginInvoke(new Act(delegate { Pick(choice, keep); })); return; }
            if (answered) return;
            answered = true;
            if (choice != "ble" && choice != "wan") choice = "cancel";
            Helper.Send("irda use " + choice + " remember=" + (keep && choice != "cancel" ? 1 : 0));
            Helper.Log("irda: chose " + choice + (keep ? " (remembered)" : ""));
            poll.Enabled = false;
            Close();
        }

        protected override void OnClosed(EventArgs e)
        {
            if (!answered) { answered = true; Helper.Send("irda use cancel remember=0"); }
            base.OnClosed(e);
        }
    }

    static readonly UIntPtr HKEY_LOCAL_MACHINE = new UIntPtr(0x80000002u);
    [DllImport("coredll.dll")] static extern int RegFlushKey(UIntPtr hKey);
    [DllImport("coredll.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
}
