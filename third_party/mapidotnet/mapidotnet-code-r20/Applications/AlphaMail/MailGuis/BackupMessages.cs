using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using System.Xml;
using MAPIdotnet;

namespace MailGuis
{
    public partial class BackupMessages : Form
    {
        private IMAPIMsgStore[] stores;
        private XmlDocument doc = null;
        private int messagesAdded = 0, foldersAdded = 0;

        public BackupMessages(IMAPIMsgStore[] stores)
        {
            InitializeComponent();
            this.stores = stores;
            for (int i = 0, num = stores.Length; i < num; i++)
            {
                IMAPIMsgStore store = stores[i];
                IMAPIFolder f = store.OpenFolder(store.RootFolder);
                TreeNode item = new TreeNode(store.DisplayName);
                item.Tag = f;
                this.treeViewMain.Nodes.Add(item);
                PopulateFolders(item);
            }
            this.treeViewMain.ExpandAll();
        }

        private void PopulateFolders(TreeNode item)
        {
            IMAPIFolder f = (IMAPIFolder)item.Tag;
            IMAPIFolderID[] subs = f.GetSubFolders((int)f.NumSubFolders);
            foreach (IMAPIFolderID subId in subs)
            {
                IMAPIFolder sub = subId.OpenFolder();
                TreeNode i = new TreeNode(sub.DisplayName);
                i.Tag = sub;
                item.Nodes.Add(i);
                PopulateFolders(i);
            }
        }

        private ElementAdder doer = null;
        private delegate void ElementAdder(XmlElement current, IMAPIFolder folder);

        private void menuItemBackup_Click(object sender, EventArgs e)
        {
            if (this.treeViewMain.SelectedNode == null || this.doer != null)
                return;
            Cursor.Current = Cursors.WaitCursor;
            IMAPIFolder folder = (IMAPIFolder)this.treeViewMain.SelectedNode.Tag;
            this.foldersAdded = this.messagesAdded = 0;

            this.doc = new XmlDocument();
            XmlDeclaration dec = this.doc.CreateXmlDeclaration("1.0", null, "yes");
            this.doc.PrependChild(dec);
            
            //this.doer = AddAsElement;
            //this.doer.BeginInvoke(this.doc.DocumentElement, folder, new AsyncCallback(finishedCallback), null);
            XmlElement rootEl = this.doc.CreateElement("Folder");
            this.doc.AppendChild(rootEl);
            rootEl.SetAttribute("Name", folder.DisplayName);
            AddAsElement(rootEl, folder);
            Cursor.Current = Cursors.Default;
            /*rootEl = this.doc.CreateElement("Date");
            this.doc.AppendChild(rootEl);
            rootEl.SetAttribute("CreationDate", DateTime.Now.ToShortDateString());
            rootEl.SetAttribute("CreationTime", DateTime.Now.ToShortTimeString());*/
            finishedCallback(null);
        }

        private void finishedCallback(IAsyncResult r)
        {
            //this.doer.EndInvoke(r);
            string fileName = null;
            try
            {
                SaveFileDialog s = new SaveFileDialog();
                s.Filter = "XML Document|*.xml";
                if (s.ShowDialog() != DialogResult.OK)
                    MessageBox.Show("Invalid location selected");
                else
                    fileName = s.FileName;
            }
            catch // probably in SmartPhone
            {
                fileName = "\\Storage Card\\AlphaMailBackup.xml";
            }
            XmlTextWriter out_xml = new XmlTextWriter(fileName, null);
            out_xml.Formatting = Formatting.Indented;
            this.doc.Save(out_xml);
            out_xml.Close();
            MessageBox.Show("Backup saved to \"" + fileName + "\"");

            this.doer = null;
        }

        private void messageAdded(object s, EventArgs e) { itemAdded(true); }
        private void folderAdded(object s, EventArgs e) { itemAdded(false); }
        private void itemAdded(bool message)
        {
            if (message)
                this.messagesAdded++;
            else
                this.foldersAdded++;
            this.labelStatus.Text = this.messagesAdded.ToString() + " messages, " + this.foldersAdded.ToString() + " folders";
        }

        private void AddAsElement(XmlElement current, IMAPIFolder folder)
        {
            // do messages and then do sub folders
            folder.SeekMessages(0);
            IMAPIMessage[] messages = folder.GetNextMessages((int)folder.NumSubItems);
            int num = messages.Length;
            if (num > 0)
            {
                XmlElement messageItems = this.doc.CreateElement("Messages");
                current.AppendChild(messageItems);
                for (int i = 0; i < num; i++)
                {
                    IMAPIMessage m = messages[i];
                    m.PopulateProperties(EMessageProperties.DeliveryTime | EMessageProperties.Sender | EMessageProperties.Status | EMessageProperties.Subject);
                    XmlElement mItem = this.doc.CreateElement('M' + i.ToString());
                    messageItems.AppendChild(mItem);
                    try { mItem.SetAttribute("SystemDeliveryTime", m.SystemDeliveryTime.ToString()); }
                    catch { }
                    try { mItem.SetAttribute("Flags", m.Flags.ToString()); }
                    catch { }
                    XmlElement sender = this.doc.CreateElement("Sender");
                    mItem.AppendChild(sender);
                    if (m.Sender != null)
                    {
                        try { sender.SetAttribute("Full", m.Sender.FullAddress); }
                        catch { }
                        try { sender.SetAttribute("Name", m.Sender.Name); }
                        catch { }
                    }
                    try { mItem.SetAttribute("Status", m.Status.ToString()); }
                    catch { }
                    try { mItem.SetAttribute("Subject", m.Subject); }
                    catch { }
                    try
                    {
                        IMAPIContact[] recips = m.Recipients;
                        if (recips.Length > 0)
                        {
                            XmlElement rItems = this.doc.CreateElement("Recipients");
                            mItem.AppendChild(rItems);
                            for (int j = 0; j < recips.Length; j++)
                            {
                                XmlElement rItem = this.doc.CreateElement('R' + j.ToString());
                                rItems.AppendChild(rItem);
                                rItem.SetAttribute("Full", recips[j].FullAddress);
                                rItem.SetAttribute("Name", recips[j].Name);
                            }
                        }
                    }
                    catch { }
                    this.Invoke(new EventHandler(messageAdded), new object[] { this, null });
                }
            }

            // do sub folders
            int n = (int)folder.NumSubFolders;
            IMAPIFolderID[] subFolders = folder.GetSubFolders(n);
            if (subFolders.Length > 0)
            {
                XmlElement subsItem = this.doc.CreateElement("SubFolders");
                current.AppendChild(subsItem);
                for (int i = 0; i < subFolders.Length; i++)
                {
                    IMAPIFolder f = subFolders[i].OpenFolder();
                    XmlElement fItem = this.doc.CreateElement("Folder");
                    subsItem.AppendChild(fItem);
                    fItem.SetAttribute("Name", f.DisplayName);
                    AddAsElement(fItem, f);
                    this.Invoke(new EventHandler(folderAdded), new object[] { this, null });
                }
            }
        }

        private void menuItemClose_Click(object sender, EventArgs e)
        {
            this.Close();
        }

        private void BackupMessages_Resize(object sender, EventArgs e)
        {
            this.treeViewMain.Height = this.labelStatus.Top;
        }
    }
}