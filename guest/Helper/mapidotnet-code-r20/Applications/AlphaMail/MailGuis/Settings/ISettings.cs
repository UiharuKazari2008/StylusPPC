using System;
using System.Collections.Generic;
using System.Text;

namespace MailGuis.Settings
{
    public interface ISettings
    {
        ISettingsDialog OpenSettingsDialog();
    }
}
