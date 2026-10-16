using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;
using System.Drawing.Imaging;

namespace TxtView
{
    public partial class TxtViewOld : UserControl
    {
        private Color gradientLeft = Color.White, gradientRight = SystemColors.Control, messageColor = SystemColors.ControlText;
        private TxtViewCollection items;
        private Icon icon = null;
        private Bitmap iconBitmap = null;
        private ImageAttributes attr = null;
        private int iconSize = 16, strHeight = 0, selectedIndex = 0, itemHeight = 0, numFit = 0;
        private bool fullRedraw = true;
        private TxtDrawCaptionArgs customDrawer = null;

        private const int Indent = 20;

        public TxtViewOld()
        {
            InitializeComponent();
            this.items = new TxtViewCollection(this);
            UpdateItemHeight();
        }

        public Color GradientColourLeft
        {
            get { return this.gradientLeft; }
            set
            {
                this.gradientLeft = value;
                this.Invalidate();
            }
        }
        public Color GradientColourRight
        {
            get { return this.gradientRight; }
            set
            {
                this.gradientRight = value;
                this.Invalidate();
            }
        }
        public Color MessageColor
        {
            get { return this.messageColor; }
            set
            {
                this.messageColor = value;
                this.Invalidate();
            }
        }

        public Icon Icon
        {
            get { return this.icon; }
            set
            {
                this.icon = value;
                this.iconBitmap = new Bitmap(this.icon.Width, this.icon.Height);
                Graphics iconGraphics = Graphics.FromImage(this.iconBitmap);
                iconGraphics.DrawIcon(this.icon, 0, 0);
                this.attr = new ImageAttributes();
                Color c = this.iconBitmap.GetPixel(0, 0);
                this.attr.SetColorKey(c, c); 
                Invalidate();
            }
        }

        public delegate void TxtDrawCaptionArgs(Graphics g, Rectangle rect, TxtDisplayItem item, bool selected);

        public TxtDrawCaptionArgs OnCaptionDraw
        {
            get { return this.customDrawer; }
            set
            {
                this.customDrawer = value;
                Invalidate();
            }
        }

        private void DrawItem(Graphics g, Rectangle r, TxtDisplayItem item, bool selected)
        {
            int y = r.Y;
            if (selected)
                g.FillRectangle(new SolidBrush(SystemColors.Highlight), new Rectangle(0, y, r.Width, this.itemHeight));
            if (this.icon != null)
                g.DrawImage(this.iconBitmap, new Rectangle(1, y + 1, this.iconSize, this.iconSize), 0, 0, this.iconBitmap.Width, this.iconBitmap.Height, GraphicsUnit.Pixel, this.attr);
            Font f = new Font(this.Font.Name, this.Font.Size, item.CaptionSyle);
            string caption = item.Caption;
            if (this.customDrawer == null)
                g.DrawString(caption, f, new SolidBrush(selected ? SystemColors.Window : item.CaptionColor), Indent, y + 1);
            else
                this.customDrawer(g, new Rectangle(Indent, y + 1, r.Width, this.strHeight), item, selected);
            int offset = y + this.strHeight + 1;
            g.DrawString(item.Message, this.Font, new SolidBrush(selected ? SystemColors.Window : this.messageColor), Indent, offset);
            offset += this.strHeight + 3;
            g.DrawLine(new Pen(SystemColors.InactiveBorder), 0, offset, r.Width, offset);
        }

        public TxtViewCollection Items { get { return this.items; } }

        private void UpdateItemHeight()
        {
            Graphics g = this.CreateGraphics();
            this.strHeight = (int)g.MeasureString("A", this.Font).Height;
            this.itemHeight = this.strHeight * 2 + 5;
            this.numFit = this.Height / this.itemHeight;
        }

        private void TxtView_KeyPress(object sender, KeyPressEventArgs e)
        {
            bool handled = true;
            switch (e.KeyChar)
            {
                case '\r': // select
                    DoItemSelected(this.selectedIndex);
                    break;
                default:
                    handled = false;
                    break;
            }
            e.Handled = handled;
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.Handled)
                return;
            int count = this.items.Count, newIndex;
            bool handled = true;
            switch (e.KeyCode)
            {
                case Keys.Up:
                    if (this.selectedIndex < 1)
                        break;
                    newIndex = this.selectedIndex - 1;
                    ChangeSelectedIndex(newIndex);
                    break;
                case Keys.Down:
                    if (this.selectedIndex + 1 >= count)
                        break;
                    newIndex = this.selectedIndex + 1;
                    ChangeSelectedIndex(newIndex);
                    break;
                default:
                    handled = false;
                    break;
            }
            e.Handled = handled;
        }

        private void NumItemsChanged()
        {
            int count = this.items.Count, newMax = (count > 1) ? count - 1 : 0;
            if (this.vScrollBar1.Value >= count)
                this.vScrollBar1.Value = newMax;
            this.vScrollBar1.Maximum = newMax;
            if (this.selectedIndex >= count)
                this.selectedIndex = newMax;
            Invalidate();
        }

        public event ItemSelectedHandler ItemSelected;

        private void DoItemSelected(int index)
        {
            //if (this.ItemSelected != null)
                //this.ItemSelected.Invoke(this, new ItemSelectedEventArgs(this.items[index], index)); 
        }

        public int SelectedIndex
        {
            get { return this.selectedIndex; }
            set { ChangeSelectedIndex(value); }
        }

        private bool ChangeSelectedIndex(int newIndex)
        {
            int count = this.items.Count;
            if (newIndex >= count)
                return false;
            int scrollVal = this.vScrollBar1.Value;
            if (scrollVal >= this.selectedIndex && newIndex < scrollVal)
            {
                this.selectedIndex = newIndex;
                this.vScrollBar1.Value -= this.numFit;
                Invalidate(); // redraw whole thing
            }
            else if (newIndex >= (scrollVal + this.numFit) && this.selectedIndex <= (scrollVal + this.numFit))
            {
                this.selectedIndex = newIndex;
                this.vScrollBar1.Value += this.numFit;
                Invalidate(); // redraw whole thing
            }
            else // just sel index changed
            {
                this.fullRedraw = false;
                Invalidate(new Rectangle(0, (this.selectedIndex - scrollVal) * this.itemHeight, this.Width, this.itemHeight));
                Invalidate(new Rectangle(0, (newIndex - scrollVal) * this.itemHeight, this.Width, this.itemHeight));
                this.selectedIndex = newIndex;
            }
            return true;
        }

        private void vScrollBar1_ValueChanged(object sender, EventArgs e)
        {
            Invalidate(); // redraw whole thing
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (!(e.X > this.Width - this.vScrollBar1.Width))
            {
                int i = e.Y / this.itemHeight + this.vScrollBar1.Value;
                if (ChangeSelectedIndex(i))
                {
                    Update();
                    DoItemSelected(i);
                }
            }
            base.OnMouseDown(e);
        }
        
        protected override void OnPaint(PaintEventArgs e)
        {
            Rectangle r;
            Graphics g = e.Graphics;
            if (this.fullRedraw)
                r = this.ClientRectangle;
            else
            {
                r = e.ClipRectangle;
                this.fullRedraw = true;
            }
            GradientFill.Fill(g, r, Color.White, SystemColors.Control, GradientFill.FillDirection.LeftToRight);
            int count = this.items.Count, scrollVal = this.vScrollBar1.Value, numItems = r.Height / this.itemHeight, startItem = r.Y / this.itemHeight + scrollVal;
            if (this.ClientRectangle == r)
                numItems++;
            for (int i = 0, j = r.Y; i < numItems; i++)
            {
                int item = i + startItem;
                if (item >= count)
                    break;
                DrawItem(g, new Rectangle(0, j, r.Width, this.itemHeight), this.items[item], item == this.selectedIndex);
                j += this.itemHeight;
            }
            base.OnPaint(e);
        }

        private void TxtView_Resize(object sender, EventArgs e)
        {
            this.UpdateItemHeight();
            vScrollBar1_ValueChanged(null, null);
        }






    }
}