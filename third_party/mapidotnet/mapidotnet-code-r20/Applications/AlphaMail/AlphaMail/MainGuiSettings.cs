using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;
using MailGuis.Settings;

namespace AlphaMail
{
    public partial class MainGuiSettings : UserControl, IMainGuiSettings
    {
        private bool somethingChanged = false;

        public MainGuiSettings() : base()
        {
            InitializeComponent();
        }

        private void itemChanged(object sender, EventArgs args)
        {
            this.somethingChanged = true;
        }

        public bool SomethingChanged { get { return this.somethingChanged; } }
    }
}
