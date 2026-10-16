using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

namespace MailGuis
{
    public class WinCE
    {
        [Flags]
        private enum InfoID : uint
        {
            NLED_COUNT_INFO_ID = 0,
            NLED_SUPPORTS_INFO_ID = 1,
            NLED_SETTINGS_INFO_ID = 2
        };

        [DllImport("coredll.dll")]
        private static extern uint NLedGetDeviceInfo(uint nId, IntPtr pOutput);

        [DllImport("coredll.dll")]
        private static extern uint NLedSetDevice(uint nId, IntPtr pInput);

        public class NLED_SUPPORTS_INFO
        {
            public uint LedNum; // 0
            public int lCycleAdjust; // 4
            public bool fAdjustTotalCycleTime; // 8
            public bool fAdjustOnTime; // 12
            public bool fAdjustOffTime; // 16
            public bool fMetaCycleOn; // 20
            public bool fMetaCycleOff; // 24
            public NLED_SUPPORTS_INFO(IntPtr p)
            {
                this.LedNum = (uint)Marshal.ReadInt32(p);
                this.lCycleAdjust = Marshal.ReadInt32(p, 4);
                this.fAdjustTotalCycleTime = Marshal.ReadInt32(p, 8) != 0;
                this.fAdjustOnTime = Marshal.ReadInt32(p, 12) != 0;
                this.fAdjustOffTime = Marshal.ReadInt32(p, 16) != 0;
                this.fMetaCycleOn = Marshal.ReadInt32(p, 20) != 0;
                this.fMetaCycleOff = Marshal.ReadInt32(p, 24) != 0;
            }
            public static int SizeOf { get { return 28; } }
        }

        public class NLED_SETTINGS_INFO
        {
            public uint LedNum = 0;      // 0           // @FIELD   LED number, 0 is first LED
            public int OffOnBlink = 0;   // 4          // @FIELD   0 == off, 1 == on, 2 == blink
            public int TotalCycleTime = 0;// 8         // @FIELD   total cycle time of a blink in microseconds
            public int OnTime = 0;        // 12         // @FIELD   on time of a cycle in microseconds
            public int OffTime = 0;       // 16         // @FIELD   off time of a cycle in microseconds
            public int MetaCycleOn = 0;    // 20       // @FIELD   number of on blink cycles
            public int MetaCycleOff = 0;   // 24        // @FIELD   number of off blink cycles
            public NLED_SETTINGS_INFO() { }
            public NLED_SETTINGS_INFO(IntPtr p)
            {
                this.LedNum = (uint)Marshal.ReadInt32(p);
                this.OffOnBlink = Marshal.ReadInt32(p, 4);
                this.TotalCycleTime = Marshal.ReadInt32(p, 8);
                this.OnTime = Marshal.ReadInt32(p, 12);
                this.OffTime = Marshal.ReadInt32(p, 16);
                this.MetaCycleOn = Marshal.ReadInt32(p, 20);
                this.MetaCycleOff = Marshal.ReadInt32(p, 24);
            }
            public static int SizeOf { get { return 28; } }
            public IntPtr AllocHGlobal()
            {
                IntPtr p = Marshal.AllocHGlobal(SizeOf);
                Marshal.WriteInt32(p, (int)this.LedNum);
                Marshal.WriteInt32(p, 4, this.OffOnBlink);
                Marshal.WriteInt32(p, 8, this.TotalCycleTime);
                Marshal.WriteInt32(p, 12, this.OnTime);
                Marshal.WriteInt32(p, 16, this.OnTime);
                Marshal.WriteInt32(p, 20, this.MetaCycleOn);
                Marshal.WriteInt32(p, 24, this.MetaCycleOff);
                return p;
            }
        };

        public static int LedNum
        {
            get
            {
                IntPtr p = Marshal.AllocHGlobal(4);
                uint b = NLedGetDeviceInfo((uint)InfoID.NLED_COUNT_INFO_ID, p);
                int v = Marshal.ReadInt32(p);
                Marshal.FreeHGlobal(p);
                return v;
            }
        }

        public static NLED_SUPPORTS_INFO LedInfo(int ledNum)
        {
            IntPtr p = Marshal.AllocHGlobal(NLED_SUPPORTS_INFO.SizeOf);
            Marshal.WriteInt32(p, ledNum);
            uint b = NLedGetDeviceInfo((uint)InfoID.NLED_SUPPORTS_INFO_ID, p);
            NLED_SUPPORTS_INFO s;
            if (b == 0)
                s = null;
            else
                s = new NLED_SUPPORTS_INFO(p);
            Marshal.FreeHGlobal(p);
            return s;
        }

        public static NLED_SETTINGS_INFO LedSettings(int ledNum)
        {
            IntPtr p = Marshal.AllocHGlobal(NLED_SETTINGS_INFO.SizeOf);
            Marshal.WriteInt32(p, ledNum);
            uint b = NLedGetDeviceInfo((uint)InfoID.NLED_SETTINGS_INFO_ID, p);
            NLED_SETTINGS_INFO s;
            if (b == 0)
                s = null;
            else
                s = new NLED_SETTINGS_INFO(p);
            Marshal.FreeHGlobal(p);
            return s;
        }

        public static void SetLed(NLED_SETTINGS_INFO settings)
        {
            //IntPtr p = Marshal.AllocHGlobal(NLED_SETTINGS_INFO.SizeOf);
            IntPtr p = settings.AllocHGlobal();
            uint b = NLedSetDevice((uint)InfoID.NLED_SETTINGS_INFO_ID, p);
            Marshal.FreeHGlobal(p);
        }
    }
}
