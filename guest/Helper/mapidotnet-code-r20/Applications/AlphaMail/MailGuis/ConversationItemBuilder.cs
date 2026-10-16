using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;
using System.Windows.Forms;
using MAPIdotnet;
using TxtView;

namespace MailGuis
{
    public class ConversationItemBuilder : MessageBuilder
    {
        private IMAPIFolder inboxFolder, sentFolder;
        private MessageContainer[] conversationMessages;
        private int inboxIndex = 0, sentIndex = 0, index = 0;
        private Color sentColor = Color.FromArgb(219, 219, 219);


        public ConversationItemBuilder(TxtViewer owner, IMAPIFolderID inboxFolder, IMAPIFolderID sentFolder, int indent, int largeBannerHeight, int smallBannerHeight, NewMessageHandler messageHandler)
            : base(owner, indent, largeBannerHeight, smallBannerHeight, messageHandler)
        {
            this.inboxFolder = inboxFolder.OpenFolder();
            this.sentFolder = sentFolder.OpenFolder();
            this.inboxFolder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            this.sentFolder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            this.conversationMessages = new MessageContainer[this.inboxFolder.NumSubItems + this.sentFolder.NumSubItems];
            this.inboxFolder.ParentStore.MessageEvent += new MessageEventHandler(folder_MessageEvent);
            this.inboxFolder.ParentStore.EventNotifyMask |= EEventMask.fnevObjectCreated | 
                EEventMask.fnevObjectMoved |
                EEventMask.fnevObjectCopied |
                EEventMask.fnevObjectDeleted;
            this.sentFolder.ParentStore.MessageEvent += new MessageEventHandler(folder_MessageEvent);
            this.sentFolder.ParentStore.EventNotifyMask |= EEventMask.fnevObjectCreated |
                EEventMask.fnevObjectMoved |
                EEventMask.fnevObjectDeleted |
                EEventMask.fnevObjectCopied;
        }

        private struct MessageContainer
        {
            public bool showRecips;
            public IMAPIMessage message;
        }

        private void folder_MessageEvent(IMAPIMessageID newMessageID, IMAPIMessageID oldMessageID, EEventMask messageFlags)
        {
            bool refresh = false;
            if (newMessageID != null)
            {
                if (newMessageID.ParentFolder.Equals(this.sentFolder.FolderID) ||
                    newMessageID.ParentFolder.Equals(this.inboxFolder.FolderID))
                    refresh = true;
            }
            if (!refresh && oldMessageID != null)
            {
                if (oldMessageID.ParentFolder.Equals(this.sentFolder.FolderID) ||
                    oldMessageID.ParentFolder.Equals(this.inboxFolder.FolderID))
                    refresh = true;
            }
            if (!refresh)
                return;

            this.inboxIndex = this.sentIndex = this.index = 0;
            this.inboxFolder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            this.sentFolder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            this.conversationMessages = new MessageContainer[this.inboxFolder.NumSubItems + this.sentFolder.NumSubItems];
            if (GDIFunctions.IsTopWindow(this.owner.TopLevelControl.Handle))
                this.Owner.Refresh();
            else if (this.messageEvent != null)
                this.messageEvent();
        }

        private void FillConversation(int index)
        {
            int inboxLen = this.inboxFolder.NumSubItems, sentLen = this.sentFolder.NumSubItems;
            while (this.index <= index)
            {
                int i = this.index++;
                IMAPIMessage sentMsg = null, msg = null;
                bool showRecip;

                if (this.inboxIndex >= inboxLen)
                    showRecip = true;
                else
                {
                    this.inboxFolder.SeekMessages(inboxLen - 1 - this.inboxIndex);
                    msg = this.inboxFolder.GetNextMessages(1)[0];
                    if (this.sentIndex >= sentLen)
                        showRecip = false;
                    else
                    {
                        this.sentFolder.SeekMessages(sentLen - 1 - this.sentIndex);
                        sentMsg = this.sentFolder.GetNextMessages(1)[0];
                        showRecip = msg.SystemDeliveryTime < sentMsg.SystemDeliveryTime;
                    }
                }
                if (showRecip)
                {
                    msg = sentMsg;
                    this.sentIndex++;
                }
                else
                    this.inboxIndex++;
                this.conversationMessages[i].message = msg;
                this.conversationMessages[i].showRecips = showRecip;
            }
        }

        public override IMAPIMessage GetMessage(int index)
        {
            if (this.conversationMessages[index].message == null)
                FillConversation(index);
            return this.conversationMessages[index].message;
        }

        public override bool ShowMessageRecips(int index)
        {
            if (this.conversationMessages[index].message == null)
                FillConversation(index);
            return this.conversationMessages[index].showRecips;
        }

        public override TxtViewer.TxtViewItem this[int index]
        {
            get 
            {
                return new MessageItem(GetMessage(index), this.conversationMessages[index].showRecips, this.owner.ItemWidth, this.handler, this, index, this.conversationMessages[index].showRecips ? this.sentColor : this.BannerColor); 
            }
        }

        public override int NumItems { get { return this.conversationMessages.Length; } }

        public override TxtViewer.TxtViewItem UpdateItem(TxtViewer.TxtViewItem i, int index)
        {
            MessageItem item = (MessageItem)i;
            item.Width = this.owner.ItemWidth;
            IMAPIMessage msg = GetMessage(index);
            bool showRecip = this.conversationMessages[index].showRecips;
            item.ShowRecipients = showRecip;
            item.BannerColor = showRecip ? this.sentColor : this.BannerColor;
            item.Message = msg;
            item.Index = index;
            return item;
        }

    }
}
