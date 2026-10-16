using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;
using System.Windows.Forms;
using TxtView;
using MAPIdotnet;

namespace MailGuis
{
    public class MessageItem : TxtViewer.TxtViewItem, IDisposable
    {
        private IMAPIMessage message;
        private bool newName, updated = false;
        public bool ShowRecipients;
        private MessageBuilder owner;
        private int bannerHeight = 0, selectedHeight = 0, unSelectedHeight = 0, bannerTextHeight = 0;
        private string[] selectedLines = null;
        private string unselectedLine = null, bannerText = "";
        public Color BannerColor;

        public MessageItem(IMAPIMessage message, bool showRecipients, int width, ItemSelectedHandler handler, MessageBuilder owner, int index, Color bannerColor)
            : base(handler, width, index)
        {
            this.BannerColor = bannerColor;
            this.owner = owner; this.ShowRecipients = showRecipients; this.Message = message;
            InitializeComponent();
            this.SelectedChanged += MessageItem_SelectedChanged;
            IndexUpdate();
        }

        public new void Dispose()
        {
            this.SelectedChanged -= MessageItem_SelectedChanged;
            base.Dispose();
        }

        public IMAPIMessage Message
        {
            get { return this.message; }
            set
            {
                this.message = value;
                if (value != null)
                {
                    message.PopulateProperties(EMessageProperties.DeliveryTime | (ShowRecipients ? 0 : EMessageProperties.Sender));
                    CalculateHeight();
                    this.Invalidate();
                }                                          
            }
        }

        public override int Index
        {
            set
            {
                base.Index = value;
                IndexUpdate();
                this.Invalidate();
            }
            get { return base.Index; }
        }

        private void IndexUpdate()
        {
            int thisIndex = this.Index;
            if (thisIndex < 1 || this.owner.Owner.ViewIndex == thisIndex)
            {
                this.newName = true;
                return;
            }
            if (this.message == null)
                return;
            IMAPIContact thisContact;
            if (this.ShowRecipients)
            {
                if (this.message.Recipients.Length != 1)
                {
                    this.newName = true;
                    return;
                }
                thisContact = this.message.Recipients[0];
            }
            else
                thisContact = this.message.Sender;
            if (thisContact == null)
            {
                this.newName = true;
                return;
            }

            IMAPIMessage msg = this.owner.GetMessage(thisIndex - 1);
            if (this.owner.ShowMessageRecips(thisIndex - 1))
            {
                if (msg.Recipients.Length != 1)
                {
                    this.newName = true;
                    return;
                }

                this.newName = !thisContact.Equals(msg.Recipients[0]);
            }
            else
            {
                IMAPIContact sender = msg.Sender;
                if (sender == null)
                    this.newName = true;
                else
                    this.newName = !thisContact.Equals(sender);
            }
        }

        private void CalculateHeight()
        {
            if (this.updated)
                return;
            this.updated = true;
            Graphics gr = this.CreateGraphics();
            Font font = this.owner.BodyFont, bannerFont = this.owner.BannerFont;
            string subject = this.message.Subject;
            string[] words = subject.Split(new char[] { ' ', '\n' });
            StringBuilder b = new StringBuilder();
            int indent = this.owner.Indent, numWords = words.Length, spaceWidth = (int)gr.MeasureString(" ", font).Width, textWidth = this.Width - indent * 2;
            if (!this.ShowRecipients)
            {
                IMAPIContact sender = this.message.Sender;
                this.bannerText = (sender != null) ? "From: " + sender.Name : "From: [blank]";
            }
            else
            {
                IMAPIContact[] contacts = this.message.Recipients;
                if (contacts.Length == 1)
                    this.bannerText = "To: " + contacts[0].Name;
                else if (contacts.Length > 1)
                    this.bannerText = "To: " + contacts[0].Name + " & ...";
                else
                    this.bannerText = "To: [blank]";
            }

            this.bannerTextHeight = (int)gr.MeasureString(this.bannerText, bannerFont).Height;
            int heightOffset = indent * 2;

            // Do selected lines:
            List<string> lines = new List<string>();
            int dotsWidth = (int)gr.MeasureString("...", bannerFont).Width;
            int height = heightOffset, lineHeight = (int)gr.MeasureString(".", font).Height;
            this.unSelectedHeight = 0;
            for (int i = 0, lineWidth = 0; i < numWords; i++)
            {
                string word = words[i];
                SizeF s = gr.MeasureString(word, font);
                if (word == "\r")
                {
                    height += lineHeight;
                    lineWidth = 0;
                    if (b.ToString().Length < 1)
                        b.Append(' ');
                    lines.Add(b.ToString());
                    if (this.unSelectedHeight == 0)
                    {
                        this.unselectedLine = b.ToString() + "...";
                        this.unSelectedHeight = height;
                    }
                    b = new StringBuilder("");
                }
                else
                {
                    int wordWidth = (int)s.Width + spaceWidth;
                    lineWidth += wordWidth;
                    if (this.unSelectedHeight == 0 && (lineWidth + dotsWidth) >= textWidth)
                    {
                        this.unselectedLine = b.ToString() + "...";
                        this.unSelectedHeight = height + lineHeight;
                    }
                    if (lineWidth >= textWidth)
                    {
                        height += lineHeight;
                        lineWidth = wordWidth;
                        lines.Add(b.ToString());
                        b = new StringBuilder("");
                    }
                    b.Append(word);
                    b.Append(' ');
                }
            }
            height += lineHeight;
            lines.Add(b.ToString());
            this.selectedLines = lines.ToArray();
            if (this.selectedLines.Length == 1)
            {
                this.unselectedLine = this.selectedLines[0];
                this.unSelectedHeight = height;
            }
            this.selectedHeight = height;
        }

        public override void UpdateHeight()
        {
            this.bannerHeight = this.bannerTextHeight + (this.newName ? (this.owner.LargeBannerTextIndent * 2) : (this.owner.SmallBannerTextIndent * 2));
            this.Height = this.bannerHeight + (this.Selected ? this.selectedHeight : this.unSelectedHeight);
            this.updated = false;
        }

        private void MessageItem_SelectedChanged(object sender, EventArgs args)
        {
            bool selected = this.Selected;
            if (selected)
            {
                EMessageFlags flags = this.message.Flags;
                if ((flags & EMessageFlags.MSGFLAG_READ) == 0) // set as read
                    this.message.Flags = flags | EMessageFlags.MSGFLAG_READ;
            }

            UpdateHeight();
            this.Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            Graphics g = e.Graphics;
            Rectangle r = e.ClipRectangle;

            this.updated = false;
            bool selected = this.Selected;
            int indent = this.owner.Indent;
            Brush textBrush = new SolidBrush(SystemColors.WindowText);

            if (r.Y < bannerHeight) // redraw banner
            {
                Font bannerFont = this.owner.BannerFont;
                if ((this.message.Flags & EMessageFlags.MSGFLAG_READ) == 0)
                    bannerFont = new Font(bannerFont.Name, bannerFont.Size, FontStyle.Bold);
                if (selected)
                    bannerFont = new Font(bannerFont.Name, bannerFont.Size, FontStyle.Italic | bannerFont.Style);
                //Color bannerColor = selected ? this.owner.SelectedBannerColor : this.BannerColor;
                //Color bannerColor = selected ? Color.FromArgb(this.BannerColor.R - 50, this.BannerColor.G - 50, this.BannerColor.B - 50) : this.BannerColor;
                //Color bannerColor = selected ? Color.FromArgb((this.BannerColor.R > 205) ? 255 : this.BannerColor.R + 50, this.BannerColor.G, this.BannerColor.B) : this.BannerColor;
                Color bannerColor = this.BannerColor;
                Brush bannerBrush = new SolidBrush(bannerColor);
                DateTime time = this.message.LocalDeliveryTime, local = DateTime.Now;
                string date;
                if (time.Equals(new DateTime(0))) // hasn't been delivered yet!
                    date = "";
                else if (time.Date.Equals(local.Date) || selected)
                {
                    date = time.ToString("h:mm ");
                    if (time.Hour > 11)
                        date += "pm";
                    else
                        date += "am";
                }
                else
                    date = time.ToString("d/M/yy");
                int dateWidth = (int)g.MeasureString(date, bannerFont).Width; int bannerOffset = bannerHeight / 2 + indent, bannerWidth = this.Width - bannerHeight - 2 * indent;
                g.FillEllipse(bannerBrush, newName ? indent : bannerWidth - dateWidth + indent, indent, bannerHeight, bannerHeight);
                g.FillEllipse(bannerBrush, bannerWidth + indent, indent, bannerHeight, bannerHeight);
                g.FillRectangle(bannerBrush, newName ? bannerOffset : bannerOffset + bannerWidth - dateWidth, indent, newName ? bannerWidth : dateWidth, bannerHeight + 1);
                if (!newName)
                    g.DrawLine(new Pen(bannerColor), bannerOffset, bannerOffset, bannerWidth - dateWidth + indent, bannerOffset);

                int textIndent = this.newName ? this.owner.LargeBannerTextIndent : this.owner.SmallBannerTextIndent;
                if (this.newName)
                    g.DrawString(this.bannerText, bannerFont, textBrush, new Rectangle(bannerOffset + 2, indent + textIndent, bannerWidth - dateWidth, bannerHeight));
                
                g.DrawString(date, bannerFont, textBrush, new Rectangle(bannerOffset + bannerWidth - dateWidth, indent + textIndent, dateWidth + 2, bannerHeight));
            }
            if (r.Y + r.Height >= bannerHeight + indent && this.selectedLines != null)
            {
                Font font = this.owner.BodyFont;
                int width = this.Width - indent * 2, offset = bannerHeight + indent * 2;
                if (selected)
                {
                    for (int i = 0, num = this.selectedLines.Length; i < num; i++)
                    {
                        int lineHeight = (int)g.MeasureString(this.selectedLines[i], font).Height;
                        g.DrawString(this.selectedLines[i], font, textBrush, new RectangleF(indent, offset, width, lineHeight));
                        offset += lineHeight;
                    }
                }
                else
                    g.DrawString(this.unselectedLine, font, textBrush, new RectangleF(indent, offset, width, this.Height - bannerHeight));
            }
        }


        private void InitializeComponent()
        {
            this.SuspendLayout();
            // 
            // MessageItem
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Inherit;
            this.Name = "MessageItem";
            //this.Size = new System.Drawing.Size(228, 100);
            this.ResumeLayout(false);

        }
    }
}
