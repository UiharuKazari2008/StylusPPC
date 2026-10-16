using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;
using System.Runtime.InteropServices;

namespace TxtView
{
    public static class GDIFunctions
    {
        // The direction to the GradientFill will follow
        public enum FillDirection
        {
            // The fill goes horizontally
            LeftToRight = Win32Helper.GRADIENT_FILL_RECT_H,
            // The fill goes vertically
            TopToBottom = Win32Helper.GRADIENT_FILL_RECT_V
        }

        public static bool IsTopWindow(IntPtr handle)
        {
            return handle.ToInt32() == GetForegroundWindow().ToInt32();
        }

        [DllImport("coredll.dll")]
        private static extern IntPtr GetForegroundWindow();

        public static bool GradientFill(Graphics gr, Rectangle rc, Color startColor, Color endColor, FillDirection fillDir)
        {
            // Initialize the data to be used in the call to GradientFill.
            Win32Helper.TRIVERTEX[] tva = new Win32Helper.TRIVERTEX[2];
            tva[0] = new Win32Helper.TRIVERTEX(rc.X, rc.Y, startColor);
            tva[1] = new Win32Helper.TRIVERTEX(rc.Right, rc.Bottom, endColor);
            Win32Helper.GRADIENT_RECT[] gra = new Win32Helper.GRADIENT_RECT[] { new Win32Helper.GRADIENT_RECT(0, 1) };

            // Get the hDC from the Graphics object.
            IntPtr hdc = gr.GetHdc();

            // PInvoke to GradientFill.
             bool b = Win32Helper.GradientFill(
                    hdc,
                    tva,
                    (uint)tva.Length,
                    gra,
                    (uint)gra.Length,
                    (uint)fillDir);

            System.Diagnostics.Debug.Assert(b, string.Format(
                "GradientFill failed: {0}",
                System.Runtime.InteropServices.Marshal.GetLastWin32Error()));

            // Release the hDC from the Graphics object.
            gr.ReleaseHdc(hdc);
            return b;
        }

        private static class Win32Helper
        {
            [StructLayout(LayoutKind.Sequential)]
            public struct TRIVERTEX
            {
                public int x;
                public int y;
                public ushort Red;
                public ushort Green;
                public ushort Blue;
                public ushort Alpha;
                public TRIVERTEX(int x, int y, Color color)
                    : this(x, y, color.R, color.G, color.B, color.A)
                { }
                public TRIVERTEX(
                    int x, int y,
                    ushort red, ushort green, ushort blue,
                    ushort alpha)
                {
                    this.x = x;
                    this.y = y;
                    this.Red = (ushort)(red << 8);
                    this.Green = (ushort)(green << 8);
                    this.Blue = (ushort)(blue << 8);
                    this.Alpha = (ushort)(alpha << 8);
                }
            }
            [StructLayout(LayoutKind.Sequential)]
            public struct GRADIENT_RECT
            {
                public uint UpperLeft;
                public uint LowerRight;
                public GRADIENT_RECT(uint ul, uint lr)
                {
                    this.UpperLeft = ul;
                    this.LowerRight = lr;
                }
            }

            [DllImport("coredll.dll", SetLastError = true, EntryPoint = "GradientFill")]
            public extern static bool GradientFill(
                IntPtr hdc,
                TRIVERTEX[] pVertex,
                uint dwNumVertex,
                GRADIENT_RECT[] pMesh,
                uint dwNumMesh,
                uint dwMode);

            public const int GRADIENT_FILL_RECT_H = 0x00000000;
            public const int GRADIENT_FILL_RECT_V = 0x00000001;

            /*[StructLayout(LayoutKind.Sequential)]
            public struct TEXTMETRIC
            {
                public int tmHeight;
                public int tmAscent;
                public int tmDescent;
                public int tmInternalLeading;
                public int tmExternalLeading;
                public int tmAveCharWidth;
                public int tmMaxCharWidth;
                public int tmWeight;
                public int tmOverhang;
                public int tmDigitizedAspectX;
                public int tmDigitizedAspectY;
                public char tmFirstChar;
                public char tmLastChar;
                public char tmDefaultChar;
                public char tmBreakChar;
                public byte tmItalic;
                public byte tmUnderlined;
                public byte tmStruckOut;
                public byte tmPitchAndFamily;
                public byte tmCharSet; 
            }*/

            //[DllImport("coredll.dll", SetLastError = true, EntryPoint = "GetTextMetrics")]
            //public extern static bool GetTextMetrics(
            //    IntPtr hdc,
            //    IntPtr lptm/*TEXTMETRIC*/);
        }
    }
}
