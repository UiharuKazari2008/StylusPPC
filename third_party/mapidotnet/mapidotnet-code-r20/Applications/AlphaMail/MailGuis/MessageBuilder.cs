using System;
using System.Drawing;
using TxtView;
using MAPIdotnet;

namespace MailGuis
{
    public abstract class MessageBuilder : IItemBuilder
    {
        protected ItemSelectedHandler handler = null;
        private int largeBannerTextIndent, smallBannerTextIndent, indent;
        private Color bannerColor, selectedBannerColor;
        protected TxtViewer owner = null;
        private Font bannerFont, bodyFont;
        protected NewMessageHandler messageEvent;

        public delegate void NewMessageHandler();

        public MessageBuilder(TxtViewer owner, int indent, int largeBannerHeight, int smallBannerHeight, NewMessageHandler messageEvent)
        {
            this.owner = owner; this.indent = indent; this.largeBannerTextIndent = largeBannerHeight; this.smallBannerTextIndent = smallBannerHeight;
            this.bannerFont = owner.Font; this.bodyFont = owner.Font; this.messageEvent = messageEvent;
            this.bannerColor = Color.FromArgb(167, 212, 255);
            //this.bannerColor = Color.FromArgb(153, 204, 255);
            this.selectedBannerColor = Color.Red;
        }
        public MessageBuilder(TxtViewer owner, int indent, int largeBannerHeight, int smallBannerHeight, ItemSelectedHandler handler, NewMessageHandler messageEvent)
            : this(owner, indent, largeBannerHeight, smallBannerHeight, messageEvent)
        { this.handler = handler; }

        public TxtViewer Owner
        {
            get { return this.owner; }
            set { this.owner = value; }
        }

        //public IMAPIMessage GetMessage(int index) { return GetMessage(index); }

        public ItemSelectedHandler SelectedHandler { set { this.handler = value; } }

        public Color BannerColor
        {
            get { return this.bannerColor; }
            set
            {
                this.bannerColor = value;
                CallParameterChanged();
            }
        }
        public Color SelectedBannerColor
        {
            get { return this.selectedBannerColor; }
            set
            {
                this.selectedBannerColor = value;
                CallParameterChanged();
            }
        }

        public Font BannerFont
        {
            get { return this.bannerFont; }
            set
            {
                this.bannerFont = value;
                CallParameterChanged();
            }
        }
        public Font BodyFont
        {
            get { return this.bodyFont; }
            set
            {
                this.bodyFont = value;
                CallParameterChanged();
            }
        }
        public int LargeBannerTextIndent
        {
            get { return this.largeBannerTextIndent; }
            set
            {
                this.largeBannerTextIndent = value;
                CallParameterChanged();
            }
        }
        public int SmallBannerTextIndent
        {
            get { return this.smallBannerTextIndent; }
            set
            {
                this.smallBannerTextIndent = value;
                CallParameterChanged();
            }
        }

        public int Indent
        {
            get { return this.indent; }
            set
            {
                this.indent = value;
                CallParameterChanged();
            }
        }

        public event EventHandler ParameterChanged;
        protected void CallParameterChanged()
        {
            if (this.ParameterChanged != null)
                this.ParameterChanged(this, new EventArgs());
            this.owner.Invalidate();
        }

        public abstract IMAPIMessage GetMessage(int index);

        public abstract bool ShowMessageRecips(int index);

        public abstract TxtViewer.TxtViewItem UpdateItem(TxtViewer.TxtViewItem i, int index);

        public abstract int NumItems { get; }

        public abstract TxtViewer.TxtViewItem this[int index] { get; }
    }
}
