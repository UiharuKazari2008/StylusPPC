using System;
using System.Collections.Generic;
using System.Text;
using System.Windows.Forms;

namespace MailGuis.Settings
{
    public interface ISettingsDialog
    {
        IMainGuiSettings MainGuiSettings { get; }
    }
}
