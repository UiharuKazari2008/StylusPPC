using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;
using Microsoft.WindowsCE.Forms;

namespace TxtView
{
    public partial class TxtViewer
    {
        public abstract partial class TxtViewItem : UserControl
        {
            private bool selected;
            public ItemSelectedHandler ParentHandler = null;
            private int index = 0;

            public TxtViewItem()
            {
                InitializeComponent();
                Selected = false;
            }

            public TxtViewItem(ItemSelectedHandler handler, int width, int index)
            {
                InitializeComponent();
                this.Width = width;
                this.ParentHandler = handler;
                this.index = index;
            }

            //~TxtViewItem() { }

            public virtual int Index
            {
                get { return this.index; }
                set { this.index = value; }
            }

            /*public new void Dispose()
            {
                this.ParentHandler = null;
                this.ItemSelected = null;
                base.Dispose();
            }*/

            public abstract void UpdateHeight();

            public new virtual int Height
            {
                get { return base.Height; }
                protected set { base.Height = value; }
            }

            public bool Selected
            {
                get { return this.selected; }
                set
                {
                    if (this.selected != value)
                    {
                        this.selected = value;
                        this.Invalidate();
                        if (this.SelectedChanged != null)
                            this.SelectedChanged(this, new EventArgs());
                    }
                }
            }

            public event EventHandler SelectedChanged;

            public event ItemSelectedHandler ItemSelected;

            protected void DoItemSelected()
            {
                ItemSelectedEventArgs args = new ItemSelectedEventArgs(this);
                if (this.ParentHandler != null)
                    this.ParentHandler(this, args);
                if (this.ItemSelected != null)
                    this.ItemSelected(this, args);
            }

            protected override void OnMouseDown(MouseEventArgs e)
            {
                DoItemSelected();
                base.OnMouseDown(e);
            }

            protected override void OnKeyPress(KeyPressEventArgs e)
            {
                bool handled = true;
                switch (e.KeyChar)
                {
                    case '\r': // select
                        DoItemSelected();
                        break;
                    default:
                        handled = false;
                        break;
                }
                e.Handled = handled;
                base.OnKeyPress(e);
            }
        }

        /*private void InitializeComponent()
        {
            this.SuspendLayout();
            // 
            // TxtViewer
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Inherit;
            this.BackColor = System.Drawing.SystemColors.Window;
            this.Name = "TxtViewer";
            this.ResumeLayout(false);

        }*/
    }

    public class ItemSelectedEventArgs : EventArgs
    {
        private TxtViewer.TxtViewItem item;
        public ItemSelectedEventArgs(TxtViewer.TxtViewItem item) { this.item = item; }
        public TxtViewer.TxtViewItem Item { get { return this.item; } }
    }

    public delegate void ItemSelectedHandler(object sender, ItemSelectedEventArgs args);

}
