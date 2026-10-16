using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

// Phone calls: Windows Mobile's own Phone app places real calls on the Android phone
// (docs/research-phone-calls-pim.md, section E).
//
// WM 6.5.3 ships FakeRIL as its radio driver. With calls on, the helper lets FakeRIL load again and
// becomes a second RIL client next to the phone stack:
//  - it sees each call cprog places (RIL_GetCallList after every call-control notification) and
//    tells the app ("call dial <id> <number>"), which places the real call;
//  - FakeRIL would "answer" any number after 2.5 s on its own. The core patches its dial thread
//    (core/src/board/codepatch.h, armed by the helper while calls are on) so the call keeps ringing
//    until the app says the real call is active, then the helper opens the gate
//    ("call state <id> active"), so cprog's screen goes Dialing -> Connected (timer) -> Ended
//    with the real call, and cprog writes the call log entry itself;
//  - when the real call ends the helper hangs up WM's call; when WM hangs up, holds or mutes, the
//    app hears it ("call end", "call hold", "call mute", "call speaker").
// If the app does not take a dialled call in time (not running, calls off), WM's call is hung up.
//
// Incoming calls: FakeRIL has a test channel in RIL_DevSpecific (command type 9: "a call comes in
// from <number>", type 22: "the remote party hangs up call <id>"; docs/research-phone-calls-pim.md,
// section F). When the phone rings, the app sends "call ring <number>"; FakeRIL then raises a real
// RIL incoming call, so cprog shows its own incoming-call screen with the ringtone, caller ID from
// Contacts, the call log and the missed-call notice. WM's Answer turns the call active (the app
// answers the real call), Ignore / End remove it (the app rejects or ends the real one); when the
// call is answered or ends on the phone, the helper answers it in WM (RIL_Answer) or lets the
// "remote party" hang up.
//
// All RIL calls and all state live on one worker thread, so a RIL call that never returns cannot stop
// the helper's mailbox loop. The RIL callbacks (a ril.dll thread) only copy their data into a queue:
// calling back into the RIL, or waiting for a lock held by a thread that is inside the RIL, deadlocks
// the proxy.
//
// Mailbox protocol (lines; several may share one message):
//   guest -> app  call ready gate=<0|1> [restart]  RIL client up (gate=1: connect is app-driven;
//                                                  restart: FakeRIL was loaded late, WM must restart
//                                                  once, e.g. with the "patch" command)
//                 call off <reason>                calls unavailable in the guest
//                 call dial <id> <number>          WM placed an outgoing voice call
//                 call ringing <id> <number|->     WM rings for the incoming call the app asked for
//                 call noring <reason>             WM cannot ring (off, busy, noradio, timeout, ...)
//                 call answer <id>                 WM's call was answered in WM (its Answer button)
//                 call end <id>                    WM's call ended (any side; for a ringing call:
//                                                  WM's Ignore)
//                 call hold <id> <0|1>             WM held / retrieved the call
//                 call mute <0|1>                  WM's microphone mute
//                 call speaker <0|1>               WM's speakerphone
//   app -> guest  call state <id> <pending|dialing|ringing|active|held|ended> [cause]
//                 call ring <number|->             the phone rings: ring WM ("-": number withheld)
//                 call answer                      rilgsm images: answer the call ringing through the
//                                                  virtual modem (it was answered on the phone)
//                 call info                        (diagnostics) answered by a "call info ..." line
static class Calls
{
    // The FakeRIL connect gate is the core's (core/src/board/codepatch.h): it finds FakeRIL's
    // dial-thread code by signature and, while armed, holds every dialled call until the gate is
    // opened. The helper only drives the core's code-patch device (physical page 0x5F100000).
    const int PatchPa = 0x5F100000, PatchId = 0x48435450;   // 'PTCH'
    const int RegStatus = 4, RegGate = 8, RegArm = 12;
    const int ST_GATE = 8;   // the gate is in FakeRIL's code now

    const string RilKey = @"Drivers\BuiltIn\RIL";
    const int DEVFLAGS_NOLOAD = 0x4;

    // RIL constants (ril.h)
    const uint RIL_NCLASS_CALLCTRL = 0x00010000, RIL_NCLASS_NETWORK = 0x00040000, RIL_NCLASS_RADIOSTATE = 0x00800000;
    const uint RIL_NOTIFY_REGSTATUSCHANGED = 0x00040001;
    const uint RIL_RESULT_OK = 1;
    const int RIL_CALLDIR_INCOMING = 1, RIL_CALLDIR_OUTGOING = 2;
    const int RIL_CALLSTAT_ACTIVE = 1, RIL_CALLSTAT_ONHOLD = 2, RIL_CALLSTAT_INCOMING = 5, RIL_CALLSTAT_WAITING = 6;
    const int RIL_CALLTYPE_VOICE = 1;
    const uint RIL_OPSELMODE_AUTOMATIC = 1;
    const int CallInfoSize = 1072, AddressOffset = 0x2C;

    delegate void RilResultCb(uint code, int cmdId, IntPtr data, uint cb, uint param);
    delegate void RilNotifyCb(uint code, IntPtr data, uint cb, uint param);
    static RilResultCb resultCb;   // kept alive: native code holds these
    static RilNotifyCb notifyCb;

    class RilEvent
    {
        public bool IsResult;
        public uint Code;
        public int CmdId;
        public byte[] Data;
    }

    class WmCall
    {
        public int Id, Status;
        public string Number;
        public bool Acked, Ending;
        public bool Incoming;      // rung in by the app ("call ring"), not dialled in WM
        public int Since;          // TickCount when seen / last acked
        public int Timeout;        // ms to wait for the app before hanging up
    }

    // The incoming call asked for with "call ring", until FakeRIL lists it (it waits 1 s first).
    static bool ringWait;
    static int ringAsked;
    static string ringNumber;

    static readonly List<RilEvent> events = new List<RilEvent>();  // filled by the RIL thread
    static IntPtr ril = IntPtr.Zero;
    static int lastInit = -100000, lastRegister = -100000, lastGate = -100000;
    static bool loadTried, loadedLate, gateOk, refresh = true, haveMute;
    static bool mute, speaker;
    static readonly Dictionary<int, string> pending = new Dictionary<int, string>();  // cmd id -> what
    static readonly Dictionary<string, int> issuedAt = new Dictionary<string, int>();
    static readonly Dictionary<int, WmCall> calls = new Dictionary<int, WmCall>();
    static readonly List<string> outbox = new List<string>();
    static string lastState = "";

    // "calls=1" in state.ini (the app's setting), re-read every few seconds.
    static bool enabled;
    static int lastSettings = -100000;

    // The worker thread that owns the RIL; commands from the app are handed to it.
    static System.Threading.Thread worker;
    static int heartbeat;
    static bool stuckLogged;
    static readonly List<string[]> commands = new List<string[]>();

    // Called from the helper's mailbox loop (every 500 ms): starts the worker and passes its messages
    // on (only this thread writes the mailbox).
    public static void Poll()
    {
        if (worker == null)
        {
            heartbeat = Environment.TickCount;
            worker = new System.Threading.Thread(Run);
            worker.IsBackground = true;
            worker.Start();
        }
        else if (Environment.TickCount - heartbeat > 15000 && !stuckLogged)
        {
            stuckLogged = true;  // a RIL call that never returns stops calls only, not the helper
            Helper.Log("calls: worker stuck");
        }
        Flush();
    }

    static volatile bool stopping;

    // The helper is exiting (update): close the RIL client so the process can end.
    public static void Stop()
    {
        if (worker == null) return;
        stopping = true;
        worker.Join(3000);
        Flush();
    }

    static void Run()
    {
        while (!stopping)
        {
            heartbeat = Environment.TickCount;
            stuckLogged = false;
            try
            {
                Tick();
                string[][] todo;
                lock (commands) { todo = commands.ToArray(); commands.Clear(); }
                foreach (string[] p in todo) Helper.Log("calls: " + string.Join(" ", p) + " -> " + Do(p));
            }
            catch (Exception e) { Helper.Log("calls " + e.GetType().Name + " " + e.Message); }
            System.Threading.Thread.Sleep(250);
        }
        if (ril != IntPtr.Zero) { RIL_Deinitialize(ril); ril = IntPtr.Zero; }
    }

    static void Tick()
    {
        if (Patch.ModemRil)
        {
            // rilgsm images: Windows Mobile dials through its own RIL, and the app's virtual modem
            // hands those calls to the phone directly (core/src/board/modem.cpp). No RIL client here,
            // and the RIL driver's load flags are not touched.
            if (lastState != "modem") { lastState = "modem"; Queue("call ready modem"); }
            if (Helper.Get("ril.trace", "0") == "1") TraceRil();  // diagnostics: log what the RIL reports
            return;
        }
        if (Environment.TickCount - lastSettings > 5000)
        {
            lastSettings = Environment.TickCount;
            enabled = Helper.Get("calls", "0") == "1";
            // FakeRIL loads at boot while calls are on. patch.fakeril=on (also written by the app with
            // calls on, so Patch.cs leaves it alone) keeps it loading when calls are off.
            if (enabled) SetNoLoad(false);
            else if (Helper.Get("patch.fakeril", "off") == "off") SetNoLoad(true);
        }
        if (ril == IntPtr.Zero)
        {
            if (!enabled)
            {
                if (lastState != "off") { lastState = "off"; Queue("call off disabled"); }
                return;
            }
            if (Environment.TickCount - lastInit < 10000) return;
            lastInit = Environment.TickCount;
            if (!Init()) return;
        }
        // The RIL stays open once FakeRIL is loaded (it stays loaded until the next restart).
        if (Environment.TickCount - lastGate > 2000)
        {
            lastGate = Environment.TickCount;
            gateOk = SyncGate(enabled);
        }
        string state = enabled ? "ready gate=" + (gateOk ? 1 : 0) + (loadedLate ? " restart" : "") : "off disabled";
        if (state != lastState) { lastState = state; Queue("call " + state); }

        Drain();
        // The call list after each call-control notification, and every second during a call (FakeRIL
        // does not notify every change); WM's mute (no notification at all) likewise.
        if ((refresh || calls.Count > 0 || ringWait) && CanIssue("calls"))
        {
            refresh = false;
            Issue(RIL_GetCallList(ril), "calls");
        }
        if (calls.Count > 0 && CanIssue("mute")) Issue(RIL_GetAudioMuting(ril), "mute");
        if (ringWait && Environment.TickCount - ringAsked > 8000)
        {
            ringWait = false;
            Helper.Log("calls: FakeRIL did not ring");
            Queue("call noring timeout");
        }
        // A call the app has not taken in time (or with calls off) is hung up, so WM never waits
        // forever on a call nothing places (or rings for a call nothing answers).
        foreach (WmCall c in calls.Values)
        {
            if (c.Ending || (enabled && Environment.TickCount - c.Since < c.Timeout)) continue;
            Helper.Log("calls: phone did not take call " + c.Id + "; hanging up");
            c.Ending = true;
            if (c.Incoming) RemoteEnd(c.Id);
            else Hangup();
        }
        if (calls.Count > 0 || speaker) PollSpeaker();
    }

    // Diagnostics (state.ini ril.trace=1): a passive RIL client that logs every notification and
    // result with its first words, to see what the radio driver tells the phone stack.
    static void TraceRil()
    {
        if (ril == IntPtr.Zero)
        {
            if (Environment.TickCount - lastInit < 10000) return;
            lastInit = Environment.TickCount;
            resultCb = OnResult;
            notifyCb = OnNotify;
            IntPtr h;
            int hr = RIL_Initialize(1, resultCb, notifyCb, 0x00FF0000 /* all classes */, 0, out h);
            Helper.Log("ril trace: RIL_Initialize 0x" + hr.ToString("X8"));
            if (hr < 0) return;
            ril = h;
        }
        TraceTapi();
        if (Environment.TickCount - lastGate > 1000)
        {
            lastGate = Environment.TickCount;
            int id = RIL_GetCallList(ril);
            if (id > 0) pending[id] = "trace calls";
            if (!numberAsked) { numberAsked = true; Helper.Log("ril trace: subscriber numbers cmd " + RIL_GetSubscriberNumbers(ril)); }
        }
        RilEvent[] batch;
        lock (events) { batch = events.ToArray(); events.Clear(); }
        foreach (RilEvent e in batch)
        {
            if (e.IsResult && (e.Data == null || e.Data.Length == 0) && quietEmpty++ % 10 != 0) continue;  // empty call list
            var sb = new StringBuilder(e.IsResult ? "ril result " + e.CmdId + " 0x" : "ril notify 0x");
            sb.Append(e.Code.ToString("X8"));
            for (int i = 0; e.Data != null && i + 4 <= e.Data.Length && i < 0x40; i += 4)
                sb.Append(" " + BitConverter.ToUInt32(e.Data, i).ToString("X"));
            if (e.Data != null && e.Data.Length >= 0x430) sb.Append(" \"" + ReadString(e.Data, AddressOffset, 64) + "\"");
            Helper.Log(sb.ToString());
        }
    }

    // TAPI side of the trace: every line opened with monitor privilege, each message logged
    // (LINE_CALLSTATE = 2: param1 state, param2 detail, e.g. the disconnect mode).
    static int quietEmpty;
    static bool numberAsked;
    static IntPtr lineApp = IntPtr.Zero;
    static bool tapiTried;

    static void TraceTapi()
    {
        if (!tapiTried)
        {
            tapiTried = true;
            int devs, ver = 0x00020000;
            int[] p = { 24, 0, 0, 2 /* LINEINITIALIZEEXOPTION_USEEVENT */, 0, 0 };
            int r = lineInitializeEx(out lineApp, IntPtr.Zero, IntPtr.Zero, "PocketStylus", out devs, ref ver, p);
            Helper.Log("tapi: lineInitializeEx " + r + " devices " + devs);
            for (int i = 0; r == 0 && i < devs; i++)
            {
                IntPtr line;
                int api;
                byte[] ext = new byte[16];
                int n = lineNegotiateAPIVersion(lineApp, i, 0x00010003, 0x00020002, out api, ext);
                int o = lineOpen(lineApp, i, out line, api, 0, 0, 2 /* MONITOR */, 4 /* interactive voice */, IntPtr.Zero);
                Helper.Log("tapi: line " + i + " api 0x" + api.ToString("X") + " (" + n + ") open -> 0x" + o.ToString("X"));
            }
        }
        if (lineApp == IntPtr.Zero) return;
        int[] m = new int[6];
        while (lineGetMessage(lineApp, m, 0) == 0)
            Helper.Log("tapi msg " + m[1] + " call=" + m[0].ToString("X") + " p1=0x" + m[3].ToString("X") + " p2=0x" + m[4].ToString("X") + " p3=0x" + m[5].ToString("X"));
    }

    [DllImport("coredll.dll")] static extern int lineInitializeEx(out IntPtr app, IntPtr inst, IntPtr cb, string name, out int devs, ref int ver, int[] parms);
    [DllImport("coredll.dll")] static extern int lineOpen(IntPtr app, int dev, out IntPtr line, int ver, int extVer, int inst, int priv, int media, IntPtr parms);
    [DllImport("coredll.dll")] static extern int lineNegotiateAPIVersion(IntPtr app, int dev, int lo, int hi, out int api, byte[] ext);
    [DllImport("coredll.dll")] static extern int lineGetMessage(IntPtr app, int[] msg, int timeout);

    // Opens the RIL; FakeRIL is loaded first if the system booted without it.
    static bool Init()
    {
        resultCb = OnResult;
        notifyCb = OnNotify;
        // The RIL proxy accepts clients whether or not a driver is loaded, so look at the driver.
        if (!RilLoaded())
        {
            if (loadTried) return false;
            // Booted with FakeRIL off (DEVFLAGS_NOLOAD): load it now.
            loadTried = true;
            IntPtr dev = ActivateDeviceEx(RilKey, IntPtr.Zero, 0, IntPtr.Zero);
            Helper.Log("calls: loading FakeRIL: " + (dev != IntPtr.Zero ? "ok" : "failed " + Marshal.GetLastWin32Error()));
            // The phone stack (cprog, cellcore, ossvcs) only binds to a RIL present at boot: until
            // the next restart WM says "Phone off" and cannot dial.
            loadedLate = dev != IntPtr.Zero;
            if (!RilLoaded())
            {
                if (lastState != "off noril") { lastState = "off noril"; Queue("call off noril"); }
                return false;
            }
        }
        IntPtr h;
        const uint classes = RIL_NCLASS_CALLCTRL | RIL_NCLASS_NETWORK | RIL_NCLASS_RADIOSTATE;
        int hr = RIL_Initialize(1, resultCb, notifyCb, classes, 0, out h);
        Helper.Log("calls: RIL_Initialize 0x" + hr.ToString("X8"));
        if (hr < 0)
        {
            string why = "ril 0x" + hr.ToString("X8");
            if (lastState != why) { lastState = why; Queue("call off " + why); }
            return false;
        }
        ril = h;
        Helper.Log("calls: RIL client up");
        Issue(RIL_GetRegistrationStatus(ril), "reg");
        return true;
    }

    // Is the RIL driver loaded? device.exe lists each loaded driver under Drivers\Active with its key.
    static bool rilLoaded;  // drivers are not unloaded here, so once seen it stays true

    static bool RilLoaded()
    {
        if (rilLoaded) return true;
        using (RegistryKey active = Registry.LocalMachine.OpenSubKey(@"Drivers\Active"))
        {
            if (active == null) return false;
            foreach (string n in active.GetSubKeyNames())
                using (RegistryKey k = active.OpenSubKey(n))
                {
                    string key = k == null ? null : k.GetValue("Key") as string;
                    if (key != null && string.Compare(key, RilKey, true) == 0) return rilLoaded = true;
                }
        }
        return false;
    }

    // FakeRIL loads at boot only while calls are on.
    static void SetNoLoad(bool noLoad)
    {
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(RilKey, true))
        {
            if (k == null) return;
            object f = k.GetValue("Flags");
            int flags = f is int ? (int)f : 0;
            int want = noLoad ? flags | DEVFLAGS_NOLOAD : flags & ~DEVFLAGS_NOLOAD;
            if (want != flags) { k.SetValue("Flags", want); Helper.Log("calls: RIL Flags " + flags + " -> " + want); }
        }
    }

    // ---- RIL callbacks (ril.dll thread): copy and queue, nothing else ----

    static void OnResult(uint code, int cmdId, IntPtr data, uint cb, uint param)
    {
        try { Enqueue(true, code, cmdId, data, cb); } catch (Exception) { }
    }

    static void OnNotify(uint code, IntPtr data, uint cb, uint param)
    {
        try { Enqueue(false, code, 0, data, cb); } catch (Exception) { }
    }

    static void Enqueue(bool result, uint code, int cmdId, IntPtr data, uint cb)
    {
        byte[] copy = null;
        if (data != IntPtr.Zero && cb > 0 && cb < 0x100000)
        {
            copy = new byte[cb];
            Marshal.Copy(data, copy, 0, (int)cb);
        }
        lock (events) events.Add(new RilEvent { IsResult = result, Code = code, CmdId = cmdId, Data = copy });
    }

    // ---- Worker thread ----

    static void Issue(int hr, string what)
    {
        if (hr > 0) { pending[hr] = what; issuedAt[what] = Environment.TickCount; }
        else Helper.Log("calls: " + what + " failed 0x" + hr.ToString("X8"));
    }

    // FakeRIL answers one request at a time after a simulated delay, so polls must not pile up:
    // one request of a kind at a time (a lost answer is forgotten after 5 s), at most once a second.
    static bool CanIssue(string what)
    {
        int t;
        if (!issuedAt.TryGetValue(what, out t)) return true;
        bool waiting = false;
        foreach (string w in pending.Values) if (w == what) { waiting = true; break; }
        if (waiting && Environment.TickCount - t < 5000) return false;
        if (waiting) ForgetPending(what);
        return Environment.TickCount - t >= 1000;
    }

    static void ForgetPending(string what)
    {
        var stale = new List<int>();
        foreach (KeyValuePair<int, string> kv in pending) if (kv.Value == what) stale.Add(kv.Key);
        foreach (int id in stale) pending.Remove(id);
    }

    static void Drain()
    {
        RilEvent[] batch;
        lock (events)
        {
            if (events.Count == 0) return;
            batch = events.ToArray();
            events.Clear();
        }
        foreach (RilEvent e in batch)
        {
            if (!e.IsResult)
            {
                if ((e.Code & 0x00FF0000) == RIL_NCLASS_CALLCTRL) refresh = true;
                else if (e.Code == RIL_NOTIFY_REGSTATUSCHANGED && e.Data != null && e.Data.Length >= 4)
                    CheckRegistration(BitConverter.ToInt32(e.Data, 0));
                continue;
            }
            string what;
            if (!pending.TryGetValue(e.CmdId, out what)) continue;
            pending.Remove(e.CmdId);
            bool ok = e.Code == RIL_RESULT_OK;
            if (what == "calls") { if (ok) OnCallList(e.Data); }  // errors: keep what we know
            else if (what == "mute") { if (ok && e.Data != null && e.Data.Length >= 4) OnMute(BitConverter.ToInt32(e.Data, 0) != 0); }
            else if (what == "reg") { if (ok && e.Data != null && e.Data.Length >= 4) CheckRegistration(BitConverter.ToInt32(e.Data, 0)); }
            else if (what == "ring" && !ok && ringWait)
            {
                ringWait = false;
                Helper.Log("calls: ring -> 0x" + e.Code.ToString("X"));
                Queue("call noring ril");
            }
            else Helper.Log("calls: " + what + " -> " + (ok ? "ok" : "0x" + e.Code.ToString("X")));
        }
    }

    static void OnMute(bool m)
    {
        if (calls.Count == 0) return;
        if (haveMute && m == mute) return;
        haveMute = true;
        mute = m;
        Queue("call mute " + (m ? 1 : 0));
    }

    // FakeRIL only registers when asked (docs/research-status-indicators.md 4.3); cprog refuses to dial
    // without service. The title bar keeps showing the phone's real network (PhoneState).
    static void CheckRegistration(int regStatus)
    {
        if (regStatus == 2 /* RIL_REGSTAT_HOME */ || regStatus == 5 /* RIL_REGSTAT_ROAMING */) return;
        if (Environment.TickCount - lastRegister < 30000) return;
        lastRegister = Environment.TickCount;
        Helper.Log("calls: registration " + regStatus + "; registering");
        Issue(RIL_RegisterOnNetwork(ril, RIL_OPSELMODE_AUTOMATIC, IntPtr.Zero), "register");
    }

    // RILCALLINFO[] (1072 bytes each in this RIL): id +8, direction +0xC, status +0x10, type +0x14,
    // RILADDRESS +0x1C with the number at +0x2C.
    static void OnCallList(byte[] data)
    {
        var seen = new Dictionary<int, bool>();
        int off = 0;
        while (data != null && off + CallInfoSize <= data.Length)
        {
            int size = BitConverter.ToInt32(data, off);
            if (size < 0x40 || off + size > data.Length) break;
            int id = BitConverter.ToInt32(data, off + 8), dir = BitConverter.ToInt32(data, off + 0xC);
            int status = BitConverter.ToInt32(data, off + 0x10), type = BitConverter.ToInt32(data, off + 0x14);
            string number = ReadString(data, off + AddressOffset, 256);
            off += size;
            if (type != RIL_CALLTYPE_VOICE) continue;
            seen[id] = true;
            WmCall c;
            if (!calls.TryGetValue(id, out c))
            {
                if (dir != RIL_CALLDIR_OUTGOING)
                {
                    if (!ringWait) { Helper.Log("calls: ignoring incoming call " + id); continue; }
                    // The call the app rang in: the app has 10 s to confirm it ("call state <id> ringing").
                    ringWait = false;
                    c = new WmCall { Id = id, Status = status, Number = number, Incoming = true, Since = Environment.TickCount, Timeout = 10000 };
                    calls[id] = c;
                    Helper.Log("calls: WM rings for " + ringNumber + " (call " + id + ", status " + status + ")");
                    Queue("call ringing " + id + " " + ringNumber);
                    continue;
                }
                c = new WmCall { Id = id, Status = status, Number = number, Since = Environment.TickCount, Timeout = 10000 };
                calls[id] = c;
                Helper.Log("calls: WM dials " + number + " (call " + id + ", status " + status + ")");
                Queue("call dial " + id + " " + number);
                continue;
            }
            if (status != c.Status)
            {
                Helper.Log("calls: call " + id + " status " + c.Status + " -> " + status);
                // Answered: WM's Answer, or ours for a call answered on the phone (the app ignores that echo).
                if (c.Incoming && (c.Status == RIL_CALLSTAT_INCOMING || c.Status == RIL_CALLSTAT_WAITING) && status == RIL_CALLSTAT_ACTIVE)
                    Queue("call answer " + id);
                if (status == RIL_CALLSTAT_ONHOLD) Queue("call hold " + id + " 1");
                else if (c.Status == RIL_CALLSTAT_ONHOLD && status == RIL_CALLSTAT_ACTIVE) Queue("call hold " + id + " 0");
                c.Status = status;
            }
        }
        var gone = new List<int>();
        foreach (int id in calls.Keys) if (!seen.ContainsKey(id)) gone.Add(id);
        foreach (int id in gone)
        {
            calls.Remove(id);
            Helper.Log("calls: call " + id + " ended in WM");
            Queue("call end " + id);
        }
        if (calls.Count == 0) haveMute = false;
    }

    static string ReadString(byte[] b, int offset, int maxChars)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < maxChars && offset + 2 * i + 1 < b.Length; i++)
        {
            char ch = (char)BitConverter.ToUInt16(b, offset + 2 * i);
            if (ch == 0) break;
            if (ch > ' ') sb.Append(ch);
        }
        return sb.ToString();
    }

    // ---- Commands from the app (queued by the mailbox loop, run on the worker) ----

    public static string Command(string args)
    {
        string[] p = args.Split(' ');
        if (p.Length >= 3 && p[0] == "state" || p.Length >= 2 && p[0] == "ring" || p[0] == "info" || p[0] == "answer")
        {
            lock (commands) commands.Add(p);
            return "ok";
        }
        return "bad args";
    }

    static string Do(string[] p)
    {
        if (p[0] == "state") return State(p);
        if (p[0] == "ring") return Ring(p[1]);
        if (p[0] == "answer") return AnswerModemCall();
        Queue("call info " + Info());
        return "ok";
    }

    // rilgsm images: the call ringing through the virtual modem was answered on the phone. cprog keeps
    // the caller's number only when it answers itself (the modem switching the call to active, or
    // another RIL client's RIL_Answer, leave the call screen saying "Unknown"), so this presses WM's
    // Talk key, as the incoming-call bubble's Answer does (AT+CHLD=2 to the modem).
    static string AnswerModemCall()
    {
        if (!Patch.ModemRil) return "not a modem image";
        const byte VK_TTALK = 0x72;
        keybd_event(VK_TTALK, 0, 0, 0);
        keybd_event(VK_TTALK, 0, 2 /* KEYEVENTF_KEYUP */, 0);
        return "ok";
    }

    [DllImport("coredll.dll")] static extern void keybd_event(byte vk, byte scan, int flags, int extra);

    // The phone rings: FakeRIL's test channel makes the call come in (it waits 1 s, then raises
    // RIL_NOTIFY_RING and lists the call; cprog shows its incoming-call screen). "-": no caller ID.
    static string Ring(string number)
    {
        string why = null;
        if (Patch.ModemRil) why = "modem";
        else if (!enabled) why = "off";
        else if (ril == IntPtr.Zero) why = "noril";
        else if (loadedLate) why = "restart";   // cprog does not know FakeRIL until WM restarts
        else if (calls.Count > 0 || ringWait) why = "busy";
        if (why != null) { Queue("call noring " + why); return "no: " + why; }
        bool hidden = number == "-" || number.Length == 0;
        string digits = hidden ? "" : number.TrimStart('+');
        // FakeRIL's command 9 (fn 0x7B4DBBF0): cbSize, type, WCHAR number[260], address type,
        // presentation allowed, auto-hang-up seconds (0: never), line (1-based).
        byte[] b = new byte[0x220];
        BitConverter.GetBytes(b.Length).CopyTo(b, 0);
        BitConverter.GetBytes(9).CopyTo(b, 4);
        for (int i = 0; i < digits.Length && i < 255; i++) BitConverter.GetBytes(digits[i]).CopyTo(b, 8 + 2 * i);
        BitConverter.GetBytes(number.StartsWith("+") ? 1 : 0).CopyTo(b, 0x210);  // RIL_ADDRTYPE_INTERNATIONAL
        BitConverter.GetBytes(hidden ? 0 : 1).CopyTo(b, 0x214);
        BitConverter.GetBytes(1).CopyTo(b, 0x21C);
        int hr = RIL_DevSpecific(ril, b, b.Length);
        if (hr <= 0) { Queue("call noring ril"); return "DevSpecific 0x" + hr.ToString("X8"); }
        pending[hr] = "ring";
        ringWait = true;
        ringAsked = Environment.TickCount;
        ringNumber = hidden ? "-" : number;
        refresh = true;
        return "ok";
    }

    // FakeRIL's command 22: the remote party hangs up call <id> (WM shows a missed call when it
    // was still ringing, else "Call ended").
    static bool RemoteEnd(int id)
    {
        if (ril == IntPtr.Zero) return false;
        byte[] b = new byte[12];
        BitConverter.GetBytes(b.Length).CopyTo(b, 0);
        BitConverter.GetBytes(22).CopyTo(b, 4);
        BitConverter.GetBytes(id).CopyTo(b, 8);
        int hr = RIL_DevSpecific(ril, b, b.Length);
        Issue(hr, "remote end");
        refresh = true;
        return hr > 0;
    }

    static string State(string[] p)
    {
        int id;
        try { id = int.Parse(p[1]); } catch { return "bad id"; }
        string state = p[2];
        WmCall c;
        if (!calls.TryGetValue(id, out c)) return "no call " + id;
        switch (state)
        {
            case "pending":   // the app is asking for a permission or the role: give it a minute
                c.Acked = true; c.Since = Environment.TickCount; c.Timeout = 60000;
                return "ok";
            case "dialing":   // the real call is being placed; it may ring for a long time
            case "ringing":   // the phone rings for WM's incoming call until it is answered or ends
            case "held":
                c.Acked = true; c.Since = Environment.TickCount; c.Timeout = int.MaxValue;
                return "ok";
            case "active":
                c.Acked = true; c.Timeout = int.MaxValue;
                if (c.Status == RIL_CALLSTAT_ACTIVE || c.Status == RIL_CALLSTAT_ONHOLD) return "ok already";
                if (c.Incoming)
                {
                    // Answered on the phone (or a headset): WM's ringing call connects too.
                    Issue(RIL_Answer(ril), "answer");
                    refresh = true;
                    return "ok";
                }
                return OpenGate() ? "ok" : "ok no gate";  // without the gate FakeRIL connects by itself after 2.5 s
            case "ended":
                if (c.Ending) return "ok already";
                c.Ending = true;
                Helper.Log("calls: phone call " + id + " ended" + (p.Length > 3 ? " (" + p[3] + ")" : ""));
                if (c.Incoming) return RemoteEnd(id) ? "ok" : "remote end failed";
                return Hangup() ? "ok" : "hangup failed";
        }
        return "bad state";
    }

    static string Info()
    {
        IntPtr r = PatchRegs();
        var sb = new StringBuilder("gate=" + (gateOk ? 1 : 0) + (r == IntPtr.Zero ? " nopatchdev" :
            " status=" + Marshal.ReadInt32(r, RegStatus).ToString("X") + " counter=" + Marshal.ReadInt32(r, RegGate)) + " ril=" + (ril != IntPtr.Zero));
        foreach (WmCall c in calls.Values)
            sb.Append(" [" + c.Id + " " + c.Number + " status=" + c.Status + (c.Incoming ? " in" : "") + (c.Acked ? " acked" : "") + (c.Ending ? " ending" : "") + "]");
        return sb.ToString();
    }

    // Ends WM's call(s). Outbound-only there is at most one call to end, and RIL_Hangup ends all.
    static bool Hangup()
    {
        if (ril == IntPtr.Zero) return false;
        int hr = RIL_Hangup(ril);
        Issue(hr, "hangup");
        refresh = true;
        return hr > 0;
    }

    static void Queue(string line) { lock (outbox) outbox.Add(line); }

    static void Flush()
    {
        string msg;
        lock (outbox)
        {
            if (outbox.Count == 0) return;
            msg = string.Join("\n", outbox.ToArray());
            outbox.Clear();
        }
        Helper.Send(msg);
    }

    // WM's speakerphone. emulserv.dll's SPK1: driver (the DE speakerphone device at 0x500FFFC0) writes
    // HKLM\System\State\Hardware "Speaker" whenever the speakerphone is switched. Not yet seen to
    // change in the CLI: cprog's "Speaker On" button does nothing with FakeRIL (whose
    // RIL_GetAudioDevices is E_NOTIMPL), so this is in place for when that path is found.
    static void PollSpeaker()
    {
        bool on = false;
        using (RegistryKey k = Registry.LocalMachine.OpenSubKey(@"System\State\Hardware"))
        {
            object v = k == null ? null : k.GetValue("Speaker");
            on = v is int && (int)v != 0;
        }
        if (on != speaker) { speaker = on; Queue("call speaker " + (on ? 1 : 0)); }
    }

    // ---- The FakeRIL connect gate (the core's code-patch device) ----

    static IntPtr patchRegs;

    static IntPtr PatchRegs()
    {
        if (patchRegs != IntPtr.Zero) return patchRegs;
        IntPtr p = VirtualAlloc(IntPtr.Zero, 0x1000, 0x2000, 1);   // MEM_RESERVE, PAGE_NOACCESS
        if (p == IntPtr.Zero) return IntPtr.Zero;
        // PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL; the address is PatchPa >> 8
        if (!VirtualCopy(p, new IntPtr(PatchPa >> 8), 0x1000, 0x604) || Marshal.ReadInt32(p, 0) != PatchId)
        {
            VirtualFree(p, 0, 0x8000);
            return IntPtr.Zero;
        }
        return patchRegs = p;
    }

    // Armed while calls are on: the core then holds each call FakeRIL dials until OpenGate. Disarmed,
    // FakeRIL connects calls by itself again. True when the gate is in FakeRIL's code (the core puts
    // it in when FakeRIL's dial code first runs, so this may only turn true with the first call).
    static bool SyncGate(bool on)
    {
        IntPtr r = PatchRegs();
        if (r == IntPtr.Zero) return false;
        if ((Marshal.ReadInt32(r, RegArm) != 0) != on)
        {
            Marshal.WriteInt32(r, RegArm, on ? 1 : 0);
            Helper.Log("calls: FakeRIL connect gate " + (on ? "armed" : "disarmed"));
        }
        return on && (Marshal.ReadInt32(r, RegStatus) & ST_GATE) != 0;
    }

    // The real call is active: WM's dialling call connects now.
    static bool OpenGate()
    {
        IntPtr r = PatchRegs();
        if (r == IntPtr.Zero || (Marshal.ReadInt32(r, RegStatus) & ST_GATE) == 0) return false;
        Marshal.WriteInt32(r, RegGate, 1);
        return true;
    }

    [DllImport("ril.dll")] static extern int RIL_Initialize(uint index, RilResultCb result, RilNotifyCb notify, uint classes, uint param, out IntPtr hRil);
    [DllImport("ril.dll")] static extern int RIL_Deinitialize(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_GetCallList(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_GetSubscriberNumbers(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_Hangup(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_Answer(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_DevSpecific(IntPtr h, byte[] parms, int size);
    [DllImport("ril.dll")] static extern int RIL_GetAudioMuting(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_GetRegistrationStatus(IntPtr h);
    [DllImport("ril.dll")] static extern int RIL_RegisterOnNetwork(IntPtr h, uint mode, IntPtr operatorNames);
    [DllImport("coredll.dll", SetLastError = true)] static extern IntPtr ActivateDeviceEx(string key, IntPtr regEnts, int count, IntPtr clientInfo);
    [DllImport("coredll.dll")] static extern IntPtr VirtualAlloc(IntPtr addr, int size, int type, int protect);
    [DllImport("coredll.dll")] static extern bool VirtualCopy(IntPtr dest, IntPtr src, int size, int protect);
    [DllImport("coredll.dll")] static extern bool VirtualFree(IntPtr addr, int size, int type);
}
