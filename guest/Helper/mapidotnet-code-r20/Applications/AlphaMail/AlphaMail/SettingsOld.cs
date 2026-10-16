using System;
using System.Collections.Generic;
using System.Text;
using Microsoft.Win32;

namespace MailGuis
{
    internal class SettingsOld
    {
        private enum Bools : int
        {
            ShowDate = 0x01,
            ShowSize = 0x02,
        }

        private RegistryKey key;
        private Bools bools;
        
        private const string RegKeys = "Software\\AlphaMail",
            RegValueMsgStore = "MsgStoreIndex",
            RegValueDefaultFolder = "DefaultFolder",
            RegValueBools = "BoolVals";

        public SettingsOld()
        {
            this.key = Registry.CurrentUser.CreateSubKey(RegKeys);
            this.bools = (Bools)((int)this.key.GetValue(RegValueBools, 0));
        }

        private void UpdateBools() { this.key.SetValue(RegValueBools, (int)this.bools); }

        public int MessageStoreIndex
        {
            get { return (int)this.key.GetValue(RegValueMsgStore, 0); }
            set { this.key.SetValue(RegValueMsgStore, value); }
        }

        public string DefaultFolder
        {
            get { return (string)this.key.GetValue(RegValueDefaultFolder, "Inbox"); }
            set { this.key.SetValue(RegValueDefaultFolder, value); }
        }

        public bool ShowDate
        {
            get { return (this.bools & Bools.ShowDate) != 0; }
            set 
            { 
                this.bools = (this.bools & (~Bools.ShowDate)) | (value ? Bools.ShowDate : 0);
                UpdateBools();
            }
        }
    }
}
