using System;
using System.Collections.Generic;
using System.Text;
using System.Windows.Forms;

namespace MailGuis.Settings
{
    public interface IMainGuiSettings
    {
        bool SomethingChanged { get; }
    }
}
