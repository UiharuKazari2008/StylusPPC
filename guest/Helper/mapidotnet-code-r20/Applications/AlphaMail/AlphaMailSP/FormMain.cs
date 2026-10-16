using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using MAPIdotnet;
using MailGuis;
using TxtView;
using MAPIRemote;

namespace AlphaMailSP
{
    public partial class FormMain : Form
    {
        private MAPI mapi;
        private IMAPIMsgStore store = null;
        private Settings settings;
        private IMAPIFolderID deleteFolder = null;
        private IMAPIFolderID inboxFolder = null;
        private IMAPIFolderID draftsFolder = null;

        public FormMain()
        {
            InitializeComponent();
        }

        private void FormMain_Load(object sender, EventArgs e)
        {
            this.txtView = new TxtViewer();
            this.txtView.Size = this.Size;
            this.txtView.Dock = System.Windows.Forms.DockStyle.Fill;
            this.txtView.Location = new System.Drawing.Point(0, 0);
            this.txtView.Name = "txtView";
            this.txtView.TabIndex = 0;
            this.txtView.ItemSelected += new TxtView.ItemSelectedHandler(this.txtView_ItemSelected);
            //this.txtView.Font = new Font(this.txtView.Font.Name, this.txtView.Font.Size - 1, FontStyle.Regular); ;
            this.Controls.Add(txtView);
            this.mapi = new MAPI();
            this.settings = new Settings();
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
            foreach (TaggedMenuItem i in this.menuItemFolders.MenuItems)
            {
                if (i.Tag != null)
                    ((IMAPIFolder)i.Tag).Dispose();
            }

            this.menuItemFolders.MenuItems.Clear();
            this.menuItemMove.MenuItems.Clear();

            string defaultFolderName = this.settings.DefaultFolder;
            IMAPIFolder folder = this.store.RootFolder.OpenFolder();
            this.inboxFolder = this.store.ReceiveFolder;
            TaggedMenuItem defaultFolder = null;
            this.draftsFolder = null;
            this.deleteFolder = this.store.TrashFolder;
            foreach (IMAPIFolderID fId in folder.GetSubFolders(folder.NumSubFolders))
            {
                IMAPIFolder f = fId.OpenFolder();
                TaggedMenuItem item = new TaggedMenuItem(f);
                TaggedMenuItem mvItem = new TaggedMenuItem(fId);
                string folderName = f.DisplayName;
                mvItem.Text = folderName;
#warning The folder contents size should be changed after any event!
                item.Text = folderName + " (" + f.NumSubItems.ToString() + ')';
                if (defaultFolder == null && item.Text.Equals(defaultFolderName))
                {
                    item.Checked = true;
                    defaultFolder = item;
                }
                if (f.DisplayName.Equals("Drafts"))
                    this.draftsFolder = fId;
                item.Click += new EventHandler(folderChange_Click);
                mvItem.Click += new EventHandler(mvItem_Click);
                this.menuItemFolders.MenuItems.Add(item);
                this.menuItemMove.MenuItems.Add(mvItem);
            }
            TaggedMenuItem conv = new TaggedMenuItem();
            conv.Text = "Conversation";
            conv.Click += new EventHandler(folderChange_Click);
            this.menuItemFolders.MenuItems.Add(conv);
            if (defaultFolder != null)
                folderChange_Click(defaultFolder, null);
            else if (conv.Text.Equals(defaultFolderName))
                folderChange_Click(conv, null);
            else
                folderChange_Click(this.menuItemFolders.MenuItems[0], null);
        }

        private void store_MessageEvent(IMAPIMessageID newMessageID, IMAPIMessageID oldMessageID, EEventMask messageFlags)
        {
            if ((messageFlags & EEventMask.fnevObjectCreated) != 0 && 
                !GDIFunctions.IsTopWindow(this.Handle) &&                 
                newMessageID.ParentFolder.Equals(this.inboxFolder))
            {
                this.mapi.DisplayMessage(newMessageID);
            }
        }

        private void folderChange_Click(object sender, EventArgs args)
        {
            TaggedMenuItem selItem = sender as TaggedMenuItem;
            IMAPIFolder folder = selItem.Tag as IMAPIFolder;
            foreach (MenuItem i in this.menuItemFolders.MenuItems)
                i.Checked = false;
            selItem.Checked = true;
            this.settings.DefaultFolder = selItem.Text;
            this.Text = selItem.Text + " - AlphaMail";
            this.menuItemEmptyFolder.Enabled = folder != null && folder.Equals(this.deleteFolder);

            FolderChanged(folder);
        }

        private void FolderChanged(IMAPIFolder folder) // if folder == null, conversation view
        {
            if (folder != null)
            {
                bool showRecips = folder.Equals(this.store.SentMailFolder) || folder.DisplayName.Equals("Drafts");

                this.txtView.Builder = new ItemBuilder(this.txtView, folder.FolderID, showRecips, 2, 0, -1, MessageEvent);
                this.txtView.Invalidate();
            }
            else // conversation view
            {
                this.txtView.Builder = new ConversationItemBuilder(this.txtView, this.store.ReceiveFolder, this.store.SentMailFolder, 2, 0, -1, MessageEvent);
                this.txtView.Invalidate();
            }
        }

        private void txtView_ItemSelected(object sender, TxtView.ItemSelectedEventArgs args)
        {
            //Class1.Serialize(((MessageItem)args.Item).Message);

            this.mapi.DisplayMessage(((MessageItem)args.Item).Message.MessageID);
        }

        private void menuItemBackup_Click(object sender, EventArgs e)
        {
            BackupMessages backuper = new BackupMessages(new IMAPIMsgStore[] { this.store });
            backuper.ShowDialog();
        }

        private void menuItemDelete_Click(object sender, EventArgs e)
        {
            MessageItem msgItem = this.txtView.SelectedItem as MessageItem;
            if (msgItem == null)
                return;
            IMAPIMessageID msgId = msgItem.Message.MessageID;
            IMAPIFolderID parentFolderId = msgId.ParentFolder;
            IMAPIFolder parentFolder = parentFolderId.OpenFolder();
            if (parentFolderId.Equals(this.deleteFolder)) // If not trash folder then we just move to trash
                parentFolder.DeleteMessage(msgId);
            else 
            {
                parentFolder.CopyMessages(new IMAPIMessageID[] { msgId }, this.deleteFolder.OpenFolder(), true);
            }
        }

        private void menuItemExit_Click(object sender, EventArgs e)
        {
            this.Close();
            Application.Exit();
        }

        private void menuItemReply_Click(object sender, EventArgs e)
        {
            MessageItem item = this.txtView.SelectedItem as MessageItem;
            if (item == null)
                return;
            bool showRecips = item.ShowRecipients;
            IMAPIMessage msg = item.Message.MessageID.OpenMessage();
            if (showRecips)
            {
                IMAPIContact[] recips = msg.Recipients;
                if (recips.Length > 0)
                    this.store.DisplayComposeDialog(recips);
            }
            else
                this.store.DisplayComposeDialog(new IMAPIContact[] { msg.Sender }, "");
        }

        private void menuItemForward_Click(object sender, EventArgs e)
        {
            MessageItem item = this.txtView.SelectedItem as MessageItem;
            if (item == null)
                return;
            this.store.DisplayComposeDialog("", item.Message.Subject);
        }

        private void menuItemMenu_Popup(object sender, EventArgs e)
        {
            MessageItem item = this.txtView.SelectedItem as MessageItem;
            bool isItem = item != null;
            this.menuItemDelete.Enabled = isItem;
            this.menuItemReply.Enabled = isItem;
            this.menuItemMove.Enabled = isItem;
            if (!isItem)
                return;
            if (item.ShowRecipients)
                this.menuItemReply.Text = "Write new";
            else
                this.menuItemReply.Text = "Reply";
            this.menuItemDrafts.MenuItems.Clear();
        }

        private void menuItemMore_Popup(object sender, EventArgs e)
        {
            MessageItem item = this.txtView.SelectedItem as MessageItem;
            bool isItem = item != null;
            this.menuItemMarkAs.Enabled = isItem;
            this.menuItemForward.Enabled = isItem;
            if (!isItem)
                return;
            IMAPIMessage msg = item.Message;
            if ((msg.Flags & EMessageFlags.MSGFLAG_READ) == 0)
                this.menuItemMarkAs.Text = "Mark as Read";
            else
                this.menuItemMarkAs.Text = "Mark as Unread";
            menuItemDrafts_Popup(this, null);
        }

        private void menuItemNew_Click(object sender, EventArgs e)
        {
            this.store.DisplayComposeDialog();
        }

        private void mvItem_Click(object sender, EventArgs e)
        {
            // check if same folder
            MessageItem msgItem = this.txtView.SelectedItem as MessageItem;
            if (msgItem == null)
                return;
            IMAPIMessage msg = msgItem.Message;
            TaggedMenuItem item = (TaggedMenuItem)sender;
            IMAPIFolderID destFolder = (IMAPIFolderID)item.Tag;
            IMAPIMessageID msgId = msg.MessageID;
            IMAPIFolderID parentFolder = msgId.ParentFolder;
            if (destFolder.Equals(parentFolder))
            {
                MessageBox.Show("Can't copy to same folder!");
                return;
            }
            parentFolder.OpenFolder().CopyMessages(new IMAPIMessageID[] { msgId }, destFolder.OpenFolder(), true);
        }

        private void menuItemMarkAs_Click(object sender, EventArgs e)
        {
            MessageItem item = this.txtView.SelectedItem as MessageItem;
            if (item == null)
                return;
            IMAPIMessage msg = item.Message;
            EMessageFlags flags = msg.Flags;
            bool read = (flags & EMessageFlags.MSGFLAG_READ) != 0;
            msg.Flags = read ? flags & ~EMessageFlags.MSGFLAG_READ : flags | EMessageFlags.MSGFLAG_READ;
            item.Invalidate();
        }

        private void menuItemEmptyFolder_Click(object sender, EventArgs e)
        {
            try
            {
                this.deleteFolder.OpenFolder().EmptyFolder();
            }
            catch { }
            this.txtView.Invalidate();
        }


        private bool updatePending = false;
        private void MessageEvent()
        {
            this.updatePending = true;
        }

        private void FormMain_GotFocus(object sender, EventArgs e)
        {
            if (this.updatePending)
                this.txtView.Refresh();
        }

        private void menuItemDrafts_Popup(object sender, EventArgs e)
        {
            foreach (MenuItem item in this.menuItemDrafts.MenuItems)
            {
                TaggedMenuItem i = item as TaggedMenuItem;
                if (i == null)
                    continue;
                ((IMAPIMessage)i.Tag).Dispose();
            }
            if (this.draftsFolder == null)
            {
                MenuItem i = new MenuItem();
                i.Text = "<Can't find folder>";
                this.menuItemDrafts.MenuItems.Add(i);
                return;
            }
            IMAPIFolder f = this.draftsFolder.OpenFolder();
            f.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            IMAPIMessage[] msgs = f.GetNextMessages(10);
            int len = msgs.Length;
            if (len < 1)
            {
                MenuItem i = new MenuItem();
                i.Text = "<None>";
                this.menuItemDrafts.MenuItems.Add(i);
                return;
            }
            for (int i = 0; i < len; i++)
            {
                IMAPIMessage msg = msgs[i];
                TaggedMenuItem item = new TaggedMenuItem(msg);
                string text = "";
                if (msg.Recipients.Length > 0)
                    text = "To: " + msg.Recipients[0] + ' ';
                text += '\"' + msg.Subject + '\"';
                item.Text = text;
                item.Click += new EventHandler(draftItem_Click);
                this.menuItemDrafts.MenuItems.Add(item);
            }
        }

        private void draftItem_Click(object sender, EventArgs args)
        {
            TaggedMenuItem item = (TaggedMenuItem)sender;
            IMAPIMessage msg = (IMAPIMessage)item.Tag;
            this.store.DisplayComposeDialog(msg.Recipients, msg.Subject);
            try
            {
                msg.MessageID.ParentFolder.OpenFolder().DeleteMessage(msg.MessageID);
            }
            catch { }
        }

        private void menuItemStart_Click(object sender, EventArgs e)
        {
            try
            {
                this.txtView.SelectedIndex = 0;
            }
            catch { }
        }

        private void menuItemFwd10_Click(object sender, EventArgs e)
        {
            try
            {
                this.txtView.SelectedIndex += 10;
            }
            catch { }
        }

        private void menuItemBack10_Click(object sender, EventArgs e)
        {
            try
            {
                this.txtView.SelectedIndex -= 10;
            }
            catch { }
        }
    }
}