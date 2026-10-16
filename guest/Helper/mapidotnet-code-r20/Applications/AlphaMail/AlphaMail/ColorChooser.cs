using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;

namespace AlphaMail
{
    public partial class ColorChooser : UserControl
    {
        private Color color;
        private bool suppressEvents = false;

        public ColorChooser()
        {
            InitializeComponent();
            this.Color = Color.Red;
        }

        public Color Color
        {
            get { return this.color; }
            set
            {
                this.suppressEvents = true;
                this.color = value;
                float h, s, l;
                RGBToHSL(this.color, out h, out s, out l);
                this.trackBarH.Value = (int)h;
                this.trackBarS.Value = (int)(s * ((float)this.trackBarS.Maximum));
                this.trackBarL.Value = (int)(l * ((float)this.trackBarL.Maximum));
                this.panelShow.BackColor = this.color;
                this.suppressEvents = false;
            }
        }

        private void trackBar_ValueChanged(object sender, EventArgs e)
        {
            if (this.suppressEvents)
                return;

            this.color = HSLToRGB((float)this.trackBarH.Value,
                ((float)this.trackBarS.Value) / this.trackBarS.Maximum,
                ((float)this.trackBarL.Value) / this.trackBarL.Maximum);
            this.panelShow.BackColor = this.color;

            if (this.ColorChanged != null)
                this.ColorChanged(this, new ColorChangedEventArgs(this.color));
        }

        public event ColorChangedEventHandler ColorChanged;

        public delegate void ColorChangedEventHandler(object sender, ColorChangedEventArgs args);
        public class ColorChangedEventArgs : EventArgs
        {
            Color color;
            public ColorChangedEventArgs(Color color) : base()
            { this.color = color; }

            public Color Color { get { return this.color; } }
        }

        public static void RGBToHSL(Color inColor, out float hue, out float saturation, out float luminance)
        {
            float red = (float)inColor.R;
            float green = (float)inColor.G;
            float blue = (float)inColor.B;
            float minval = red;
            if (green < minval) minval = green;
            if (blue < minval) minval = blue;
            float maxval = red;
            if (green > maxval) maxval = green;
            if (blue > maxval) maxval = blue;

            float mdiff = maxval - minval;
            float msum = maxval + minval;

            luminance = msum / 510.0f;
            saturation = 0.0f;
            hue = 0.0f;

            if (maxval == minval)
            {
                saturation = 0.0f;
                hue = 0.0f;
            }
            else
            {
                float rnorm = (maxval - red) / mdiff;
                float gnorm = (maxval - green) / mdiff;
                float bnorm = (maxval - blue) / mdiff;

                if (luminance <= 0.5f)
                {
                    saturation = (mdiff / msum);
                }
                else
                {
                    saturation = (mdiff / (510.0f - msum));
                }

                if (red == maxval) hue = 60.0f * (6.0f + bnorm - gnorm);
                if (green == maxval) hue = 60.0f * (2.0f + rnorm - bnorm);
                if (blue == maxval) hue = 60.0f * (4.0f + gnorm - rnorm);
                if (hue > 360.0f) hue = hue - 360.0f;
            }
        }

        private static byte ToRGB1(float rm1, float rm2, float rh)
        {
            if (rh > 360.0f)
                rh -= 360.0f;
            else if (rh < 0.0f)
                rh += 360.0f;

            if (rh < 60.0f)
                rm1 = rm1 + (rm2 - rm1) * rh / 60.0f;
            else if (rh < 180.0f)
                rm1 = rm2;
            else if (rh < 240.0f)
                rm1 = rm1 + (rm2 - rm1) * (240.0f - rh) / 60.0f;

            return (byte)(rm1 * 255);
        }

        public static Color HSLToRGB(float hue, float saturation, float luminance)
        {
            int Red = (int)(luminance * 255.0f);
            int Green = (int)(luminance * 255.0f);
            int Blue = (int)(luminance * 255.0f);
            if (saturation != 0.0)
            {
                float rm1, rm2;

                if (luminance <= 0.5f)
                {
                    rm2 = luminance + luminance * saturation;
                }
                else
                {
                    rm2 = luminance + saturation - luminance * saturation;
                }
                rm1 = 2.0f * luminance - rm2;
                Red = ToRGB1(rm1, rm2, hue + 120.0f);
                Green = ToRGB1(rm1, rm2, hue);
                Blue = ToRGB1(rm1, rm2, hue - 120.0f);
            }

            return Color.FromArgb(Red, Green, Blue);
        }
    }
}
