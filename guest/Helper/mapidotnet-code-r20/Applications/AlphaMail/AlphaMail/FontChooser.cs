using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;

namespace AlphaMail
{
    public partial class FontChooser : UserControl
    {
        private Font font;
        private bool suppressEvents = false;

        public FontChooser()
        {
            InitializeComponent();
            this.textBoxName.TextChanged += new EventHandler(UpdateFont);
            this.numericUpDownSize.ValueChanged += new EventHandler(UpdateFont);
            this.Font = base.Font;
        }

        public FontChooser(Font font)
            : base()
        {
            this.Font = font;
        }

        void UpdateFont(object sender, EventArgs e)
        {
            if (this.suppressEvents)
                return;
            try
            {
                this.font = new Font(this.textBoxName.Text, (float)this.numericUpDownSize.Value, this.font.Style);
            }
            catch (ArgumentException)
            {
                MessageBox.Show("Invalid value", "Invalid font name or size", MessageBoxButtons.OK, MessageBoxIcon.Exclamation, MessageBoxDefaultButton.Button1);
            }
            this.textBoxName.Text = this.font.Name;
            this.numericUpDownSize.Value = (decimal)this.font.Size;
            if (this.FontChanged != null)
                this.FontChanged(this, new FontChangeEventArgs(this.font));
        }

        public override Font Font
        {
            set
            {
                this.suppressEvents = true;
                this.font = value;
                this.textBoxName.Text = this.font.Name;
                this.numericUpDownSize.Value = (decimal)this.font.Size;
                this.suppressEvents = false;
            }
            get { return this.font; }
        }

        public event FontChangeEventHandler FontChanged;

        public delegate void FontChangeEventHandler(object sender, FontChangeEventArgs args);
        public class FontChangeEventArgs : EventArgs
        {
            private Font font;

            public FontChangeEventArgs(Font font)
                : base()
            { this.font = font; }

            public Font Font { get { return this.font; } }
        }
    }
}
