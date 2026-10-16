using System;
using System.Diagnostics;

// "Send via Bluetooth" in the Start menu: a launcher with its own icon for the helper's send
// command (WMDroid.exe send picks a file and hands it to the phone's Bluetooth share).
static class SendBluetooth
{
    [MTAThread]
    static void Main(string[] args)
    {
        string a = "send";
        if (args.Length > 0) a += " \"" + args[0] + "\"";
        // The pages' own copy: the helper's file is already running (one instance per program file).
        string exe = System.IO.File.Exists(@"\Windows\PocketStylus.exe") ? @"\Windows\PocketStylus.exe" : @"\Windows\WMDroid.exe";
        try { Process.Start(exe, a); } catch (Exception) { }
    }
}
