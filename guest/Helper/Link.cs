using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Pipes on their own page at 0x5F300000 (Sideband, core/src/board/de_devices.h). Not COM1 or COM4.
//   +0x000 command/response (CMD1)
//   +0x400 status (STA1)
//   +0x800 bulk (BLK1): contact and calendar bytes, 1792 a chunk
static class Link
{
    const uint Pa = 0x5F300000;
    const int CmdAt = 0;
    const int StatAt = 0x400;
    const int BlkAt = 0x800;
    const int BulkCap = 1792;
    const int CmdId = unchecked((int)0x31444D43);  // 'CMD1'
    const int StatId = unchecked((int)0x31415453); // 'STA1'
    const int BlkId = unchecked((int)0x314B4C42);  // 'BLK1'
    static MemoryStream held = new MemoryStream();
    static bool heldLast;
    static readonly object ioLock = new object();
    static readonly object mapLock = new object();
    static IntPtr regs;
    static bool logged;

    static IntPtr Map()
    {
        if (regs != IntPtr.Zero) return regs;
        lock (mapLock)
        {
            if (regs != IntPtr.Zero) return regs;
            IntPtr p = VirtualAlloc(IntPtr.Zero, 0x1000, 0x2000, 1);  // MEM_RESERVE, PAGE_NOACCESS
            if (p == IntPtr.Zero) return Fail("alloc");
            // PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL. Address is the page >> 8 (Camera.cs).
            if (!VirtualCopy(p, new IntPtr(Pa >> 8), 0x1000, 0x604))
            {
                int err = Marshal.GetLastWin32Error();
                VirtualFree(p, 0, 0x8000);
                return Fail("copy " + err);
            }
            int cmd = Marshal.ReadInt32(p, CmdAt), stat = Marshal.ReadInt32(p, StatAt), blk = Marshal.ReadInt32(p, BlkAt);
            if (cmd != CmdId || stat != StatId || blk != BlkId)
            {
                VirtualFree(p, 0, 0x8000);
                return Fail("id " + cmd.ToString("x8") + " " + stat.ToString("x8") + " " + blk.ToString("x8"));
            }
            return regs = p;
        }
    }

    static IntPtr Fail(string why)
    {
        if (!logged) { logged = true; Helper.Log("link " + why); }
        return IntPtr.Zero;
    }

    static string[] Take(int at)
    {
        IntPtr r = Map();
        if (r == IntPtr.Zero) return new string[0];
        var lines = new List<string>();
        for (int k = 0; k < 16; k++)
        {
            int n = Marshal.ReadInt32(r, at + 4);
            if (n <= 0) break;
            if (n > 240) n = 240;
            var raw = new byte[n];
            for (int i = 0; i < n; )
            {
                int w = Marshal.ReadInt32(r, at + 0x20 + (i & ~3));
                int pos = i & ~3;
                for (int b = 0; b < 4 && pos + b < n; b++) raw[pos + b] = (byte)((w >> (8 * b)) & 0xFF);
                i = pos + 4;
            }
            Marshal.WriteInt32(r, at + 4, 0);  // consume, so the next read is the next line
            lines.Add(Encoding.UTF8.GetString(raw, 0, n));
        }
        return lines.ToArray();
    }

    static bool Post(int at, string line)
    {
        IntPtr r = Map();
        if (r == IntPtr.Zero || line == null || line.Length == 0) return r != IntPtr.Zero && line != null;
        for (int i = 0; i < line.Length && i < 240; i++)
        {
            char c = line[i];
            if (c < 32 || c > 126) continue;
            Marshal.WriteInt32(r, at + 8, c);
        }
        Marshal.WriteInt32(r, at + 8, (int)'\n');
        return true;
    }

    public static string[] TakeCmd() { return Take(CmdAt); }
    public static string[] TakeStat() { return Take(StatAt); }
    public static bool PostCmd(string line) { return Post(CmdAt, line); }
    public static bool PostStat(string line) { return Post(StatAt, line); }

    // Pull every host→guest chunk already waiting. The serve loop calls this so a long transfer
    // does not fill the window while the helper is busy with something else.
    public static void Pump()
    {
        lock (ioLock) PumpLocked();
    }

    static void PumpLocked()
    {
        IntPtr r = Map();
        if (r == IntPtr.Zero || heldLast) return;
        for (int k = 0; k < 64; k++)
        {
            int st = Marshal.ReadInt32(r, BlkAt + 4);
            if ((st & 1) == 0) return;
            int n = Marshal.ReadInt32(r, BlkAt + 8);
            if (n < 0) n = 0;
            if (n > BulkCap) n = BulkCap;
            var buf = new byte[n];
            for (int i = 0; i < n; )
            {
                int w = Marshal.ReadInt32(r, BlkAt + 0x20 + (i & ~3));
                int at = i & ~3;
                for (int b = 0; b < 4 && at + b < n; b++) buf[at + b] = (byte)((w >> (8 * b)) & 0xFF);
                i = at + 4;
            }
            Marshal.WriteInt32(r, BlkAt + 4, 0);
            if (n > 0) held.Write(buf, 0, n);
            if ((st & 2) != 0) { heldLast = true; return; }
        }
    }

    // The bytes the host pushed, once the last chunk has arrived. Null if it does not, in time.
    public static byte[] RecvBulk(int timeoutMs)
    {
        if (Map() == IntPtr.Zero) return null;
        int end = Environment.TickCount + timeoutMs;
        while (unchecked(end - Environment.TickCount) > 0)
        {
            lock (ioLock)
            {
                PumpLocked();
                if (heldLast)
                {
                    byte[] got = held.ToArray();
                    held = new MemoryStream();
                    heldLast = false;
                    return got;
                }
            }
            Thread.Sleep(20);
        }
        return null;
    }

    // Guest → host. False when this build has no bulk window.
    public static bool SendBulk(byte[] data)
    {
        IntPtr r = Map();
        if (r == IntPtr.Zero || data == null) return false;
        int off = 0;
        do
        {
            int n = data.Length - off;
            if (n > BulkCap) n = BulkCap;
            bool last = off + n >= data.Length;
            for (int spin = 0; spin < 250; spin++)
            {
                int queued;
                lock (ioLock) queued = Marshal.ReadInt32(r, BlkAt + 0x10);
                if (queued < 48) break;
                Thread.Sleep(20);
            }
            lock (ioLock)
            {
                for (int i = 0; i < n; i += 4)
                {
                    int w = 0;
                    for (int b = 0; b < 4 && i + b < n; b++) w |= (data[off + i + b] & 0xFF) << (8 * b);
                    Marshal.WriteInt32(r, BlkAt + 0x20 + i, w);
                }
                Marshal.WriteInt32(r, BlkAt + 0x0C, n | (last ? unchecked((int)0x80000000) : 0));
            }
            off += n;
            if (data.Length == 0) break;
        } while (off < data.Length);
        return true;
    }

    [DllImport("coredll.dll")] static extern IntPtr VirtualAlloc(IntPtr addr, int size, int type, int protect);
    [DllImport("coredll.dll", SetLastError = true)] static extern bool VirtualCopy(IntPtr dest, IntPtr src, int size, int protect);
    [DllImport("coredll.dll")] static extern bool VirtualFree(IntPtr addr, int size, int type);
}
