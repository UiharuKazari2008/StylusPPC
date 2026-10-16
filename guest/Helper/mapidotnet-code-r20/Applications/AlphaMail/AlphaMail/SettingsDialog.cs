using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using MailGuis.Settings;

namespace AlphaMail
{
    public partial class SettingsDialog : Form, ISettingsDialog
    {
        public SettingsDialog() : base()
        {
            InitializeComponent();
        }

        public IMainGuiSettings MainGuiSettings { get { return this.mainGuiSettings; } }


    }
}