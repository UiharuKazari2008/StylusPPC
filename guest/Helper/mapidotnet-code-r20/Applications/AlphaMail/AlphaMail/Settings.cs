using System;
using System.Collections.Generic;
using System.Text;
using MailGuis.Settings;
using System.Xml;
using System.IO;

namespace AlphaMail
{
    public class Settings: ISettings
    {
        private XmlDocument xmlDoc;

        public Settings()
        {
            /*this.xmlDoc = new XmlDocument();
            XmlDeclaration dec = this.doc.CreateXmlDeclaration("1.0", null, "yes");
            this.doc.PrependChild(dec);*/
            /*try
            {
//#error here
                StreamReader in_xml = File.OpenText(fileName);
                membersXml.Load(in_xml);
                in_xml.Close();
            }
            catch
            {
                // File doesn't exist so setup stuff

                return false;
            }*/
        }

        public ISettingsDialog OpenSettingsDialog() 
        { 
            SettingsDialog settings = new SettingsDialog();
            settings.ShowDialog();
            return settings;
        }
    }
}
