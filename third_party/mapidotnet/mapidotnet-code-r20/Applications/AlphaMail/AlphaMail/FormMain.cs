using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using Microsoft.WindowsCE.Forms;
using MAPIdotnet;
using MailGuis;
using TxtView;

namespace AlphaMail
{
    public partial class FormMain : Form
    {
        private InputPanel inputPanel = null;
        private bool suppresInputPanel = true;
        private MAPI mapi = null;
        private IMAPIMsgStore store = null;
        private Settings settings;

        public FormMain()
        {
            InitializeComponent();
            SuspendLayout();

            this.inputPanel = new InputPanel();
            this.inputPanel.EnabledChanged += delegate(object sender, EventArgs args) { if (this.suppresInputPanel) this.inputPanel.Enabled = false; };
            ResumeLayout();

            this.mapi = new MAPI();
            this.settings = new Settings();
        }

        private void FormMain_Resize(object sender, EventArgs e)
        {
            //this.txtView.Height = this.statusBar.Location.Y;
        }

        private void FormMain_Load(object sender, EventArgs e)
        {
            IMAPIMsgStore[] stores = this.mapi.MessageStores;
            if (stores.Length < 1)
            {
                MessageBox.Show("No message store accounts found!");
                this.Close();
            }
            foreach (IMAPIMsgStore store in stores)
            {
                TaggedMenuItem item = new TaggedMenuItem(store);
                item.Text = store.DisplayName;
                item.Click += new EventHandler(click_AccountChange);
                this.menuItemAccounts.MenuItems.Add(item);
            }
            int selIndex = this.settings.MessageStoreIndex;
            if (selIndex >= stores.Length)
                selIndex = 0;
            click_AccountChange(this.menuItemAccounts.MenuItems[selIndex], null);
        }

        private void click_AccountChange(object sender, EventArgs args)
        {
            TaggedMenuItem selItem = (TaggedMenuItem)sender;
            for (int i = 0; i < this.menuItemAccounts.MenuItems.Count; i++)
            {
                MenuItem item = this.menuItemAccounts.MenuItems[i];
                if (item == sender)
                    this.settings.MessageStoreIndex = i;
                else
                    item.Checked = false;
            }
            selItem.Checked = true;
            this.store = (IMAPIMsgStore)selItem.Tag;
            MsgStoreChanged();
        }

        private void MsgStoreChanged()
        {
            List<MenuItem> lowMenuItems = new List<MenuItem>(), highMenuItems = new List<MenuItem>();
            bool doingFolders = false, doneFolders = false;
            for (int i = 0; i < this.menuItemRight.MenuItems.Count; i++)
            {
                MenuItem item = this.menuItemRight.MenuItems[i];
                if (!doingFolders)
                {
                    if (item == this.menuItemSeperatorTop)
                        doingFolders = true;
                    lowMenuItems.Add(item);
                }
                else if (!doneFolders)
                {
                    if (item == this.menuItemConversationView)
                    {
                        doneFolders = true;
                        highMenuItems.Add(item);
                    }
                }
                else
                    highMenuItems.Add(item);
            }
            this.menuItemRight.MenuItems.Clear();

            for (int i = 0; i < lowMenuItems.Count; i++)
                this.menuItemRight.MenuItems.Add(lowMenuItems[i]);

            string defaultFolder = this.settings.DefaultFolder;
            /*IMAPIFolder[] folders = this.store.RootFolder.GetSubFolders((int)this.store.RootFolder.NumSubFolders);
            IMAPIFolder selected = folders[0];
            foreach (IMAPIFolder folder in folders)
            {
                TaggedMenuItem item = new TaggedMenuItem(folder);
                item.Text = folder.DisplayName;
                item.Checked = folder.DisplayName.Equals(defaultFolder);
                if (item.Checked)
                    selected = folder;
                item.Click += new EventHandler(folder_Click);
                //PopulateFolders(item, folder);
                this.menuItemRight.MenuItems.Add(item);
            }*/
            TreeNode n = new TreeNode(this.store.DisplayName);

            this.treeViewFolders.Nodes.Add(n);
            IMAPIFolder rootFolder = this.store.RootFolder.OpenFolder();
            PopulateFolders(n, rootFolder, defaultFolder);
            IMAPIFolder selected = this.treeViewFolders.SelectedNode.Tag as IMAPIFolder;
            this.treeViewFolders.ExpandAll();

            if (selected == null)
            {
                selected = rootFolder.GetSubFolders(1)[0].OpenFolder();
                this.treeViewFolders.SelectedNode = this.treeViewFolders.Nodes[0].Nodes[0];
            }
            if (defaultFolder == this.menuItemConversationView.Text)
                this.menuItemConversationView.Checked = true;
            for (int i = 0; i < highMenuItems.Count; i++)
                this.menuItemRight.MenuItems.Add(highMenuItems[i]);
            FolderChanged(selected);
        }

        private void PopulateFolders(TreeNode item, IMAPIFolder folder, string defaultFolder)
        {
            int numSubs = folder.NumSubFolders;
            if (numSubs < 1)
                return;
            IMAPIFolderID[] subs = folder.GetSubFolders(numSubs);
            foreach (IMAPIFolderID fId in subs)
            {
                IMAPIFolder f = fId.OpenFolder();
                TreeNode i = new TreeNode(f.DisplayName);
                item.Nodes.Add(i);
                i.Tag = f;
                i.Checked = f.DisplayName.Equals(defaultFolder);
                if (i.Checked)
                {
                    //selectedF = f;
                    this.treeViewFolders.SelectedNode = i;
                }
                //i.Click += folder_Click;
                PopulateFolders(i, f, defaultFolder);
            }
        }

        private void treeViewFolders_AfterSelect(object sender, TreeViewEventArgs e)
        //private void folder_Click(object sender, EventArgs args)
        {
            //TaggedMenuItem selItem = (TaggedMenuItem)sender;

            TreeNode selItem = e.Node;
            if (selItem == null)
                return;
            //TreeNode selItem = sender as TreeNode;
            bool doingFolders = false;
            /*for (int i = 0; i < this.menuItemRight.MenuItems.Count; i++)
            {
                TreeNode item = this.menuItemRight.MenuItems[i];
                if (!doingFolders)
                {
                    if (item == this.menuItemSeperatorTop)
                        doingFolders = true;
                }
                else
                {
                    if (item == this.menuItemConversationView)
                        break;
                    if (item.MenuItems.Count < 1)
                        item.Checked = item == selItem;
                }
            }*/
            /*IMAPIFolder selFolder = (IMAPIFolder)selItem.Tag;
            this.settingsOld.DefaultFolder = selFolder.DisplayName;
            FolderChanged(selFolder);*/
        }

        private void menuItemConversationView_Click(object sender, EventArgs e)
        {
            bool doingFolders = false;
            for (int i = 0; i < this.menuItemRight.MenuItems.Count; i++)
            {
                MenuItem item = this.menuItemRight.MenuItems[i];
                if (!doingFolders)
                {
                    if (item == this.menuItemSeperatorTop)
                        doingFolders = true;
                }
                else
                {
                    if (item == this.menuItemConversationView)
                        break;
                    item.Checked = false;
                }
            }
            this.settings.DefaultFolder = this.menuItemConversationView.Text;
            FolderChanged(null);
        }

        private void FolderChanged(IMAPIFolder folder) // if folder == null, conversation view
        {
            if (folder != null)
            {
                this.statusBar.Text = folder.NumSubItems.ToString() + " messages in " + folder.DisplayName;
                //this.conversations = new MailConversation(this.store.ReceiveFolder, this.store.SentMailFolder, 20, 0);
                //folder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
                //IMAPIMessage[] messages = folder.GetNextMessages((int)folder.NumSubItems);
                
                bool showRecips = folder.Equals(this.store.SentMailFolder) || folder.DisplayName.Equals("Drafts");

                this.txtView.Builder = new ItemBuilder(this.txtView, folder.FolderID, showRecips, 3, 2, 0, null);
                this.txtView.Invalidate();
            }
            else // conversation view
            {

            }
        }

        private void menuItemBackup_Click(object sender, EventArgs e)
        {
            BackupMessages backuper = new BackupMessages(new IMAPIMsgStore[] { this.store });
            backuper.ShowDialog();
        }

        private void menuItemSettings_Click(object sender, EventArgs e)
        {
            //this.suppresInputPanel = false;
            //this.settings.OpenSettingsDialog();
            //this.suppresInputPanel = true;
        }

        private void menuItemLeft_Click(object sender, EventArgs e)
        {
            this.store.DisplayComposeDialog(new IMAPIContact[] { (this.txtView.SelectedItem as MessageItem).Message.Sender, (this.txtView.SelectedItem as MessageItem).Message.Sender });
        }

        private void buttonShowFolders_Click(object sender, EventArgs e)
        {
            this.treeViewFolders.Visible = !this.treeViewFolders.Visible;
        }



        

    }
}