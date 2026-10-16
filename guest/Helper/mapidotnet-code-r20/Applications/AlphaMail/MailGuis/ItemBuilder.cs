using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;
using System.Windows.Forms;
using MAPIdotnet;
using TxtView;

namespace MailGuis
{
    public class ItemBuilder : MessageBuilder
    {
        private IMAPIFolder folder;
        private bool showRecips;

        public ItemBuilder(TxtViewer owner, IMAPIFolderID folder, bool showRecips, int indent, int largeBannerHeight, int smallBannerHeight, NewMessageHandler messageHandler)
            : this(owner, folder, showRecips, null, indent, largeBannerHeight, smallBannerHeight, messageHandler)
        { }

        public ItemBuilder(TxtViewer owner, IMAPIFolderID folder, bool showRecips, ItemSelectedHandler handler, int indent, int largeBannerHeight, int smallBannerHeight, NewMessageHandler messageHandler)
            : base(owner, indent, largeBannerHeight, smallBannerHeight, handler, messageHandler)
        {
            this.folder = folder.OpenFolder();
            this.folder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            this.showRecips = showRecips;
            this.folder.ParentStore.MessageEvent += new MessageEventHandler(folder_MessageEvent);
            this.folder.ParentStore.EventNotifyMask |= EEventMask.fnevObjectCreated |
                EEventMask.fnevObjectMoved |
                EEventMask.fnevObjectDeleted |
                EEventMask.fnevObjectCopied;
        }

        private void folder_MessageEvent(IMAPIMessageID newMessageID, IMAPIMessageID oldMessageID, EEventMask messageFlags)
        {
            bool refresh = false;
            if (newMessageID != null)
            {
                if (newMessageID.ParentFolder.Equals(this.folder.FolderID))
                    refresh = true;
            }
            if (!refresh && oldMessageID != null)
            {
                if (oldMessageID.ParentFolder.Equals(this.folder.FolderID))
                    refresh = true;
            }
            if (!refresh)
                return;
            this.folder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
            if (GDIFunctions.IsTopWindow(this.owner.TopLevelControl.Handle))
                this.Owner.Refresh();
            else if (this.messageEvent != null)
                this.messageEvent();
        }

        public override IMAPIMessage GetMessage(int index)
        {
            this.folder.SeekMessages(this.folder.NumSubItems - 1 - index);
            IMAPIMessage[] msgs = this.folder.GetNextMessages(1);
            return (msgs.Length == 1) ? msgs[0] : null;
        }

        public override bool ShowMessageRecips(int index)
        {
            return this.showRecips;
        }

        public override TxtViewer.TxtViewItem this[int index]
        {
            get 
            {
                TxtViewer.TxtViewItem item = new MessageItem(GetMessage(index), this.showRecips, this.owner.ItemWidth, this.handler, this, index, this.BannerColor);
                item.Width = this.owner.Width;
                return item;
            }
        }

        public override int NumItems { get { return this.folder.NumSubItems; } }

        public override TxtViewer.TxtViewItem UpdateItem(TxtViewer.TxtViewItem i, int index)
        {
            MessageItem item = (MessageItem)i;
            item.Width = this.owner.ItemWidth;
            item.Message = GetMessage(index);
            item.Index = index;
            return item;
        }

        

        

        

    }
}
