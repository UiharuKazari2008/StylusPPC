using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Text;
using System.Windows.Forms;

namespace TxtView
{
    public partial class TxtViewer : UserControl
    {
        private int selectedIndex = 0;//, topIndex = 0, prevScrollVal = 0;
        private IItemBuilder builder = null;

        public TxtViewer()
        {
            InitializeComponent();
            this.Font = new Font(this.Font.Name, this.Font.Size, FontStyle.Regular);
        }

        /*public string Text
        {
            get { return this.text; }
            set
            {
                this.text = value;
                if (this.TextChanged != null)
                    this.TextChanged(this, new EventArgs());
            }
        }

        public event EventHandler TextChanged;*/

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            Update();
        }

        public int ItemWidth { get { return this.txtItems.Width; } }

        public IItemBuilder Builder
        {
            get { return this.builder; }
            set 
            { 
                this.builder = value;
                this.builder.Owner = this;
                this.builder.SelectedHandler = DoItemSelected;
                this.vScrollBar1.Maximum = this.builder.NumItems - 1;
                this.txtItems.Controls.Clear();
                this.selectedIndex = 0;
                UpdateItems(this.selectedIndex, true);
            }
        }

        public int SelectedIndex
        {
            get { return this.selectedIndex; }
            set { UpdateItems(value, true); }
        }

        public TxtViewItem SelectedItem { get { return (this.builder.NumItems <= this.selectedIndex) ? null : this.builder[this.selectedIndex]; } }

        public int ViewIndex { get { return this.vScrollBar1.Value; } }

        private void txtItems_KeyDown(object sender, KeyEventArgs e)
        {
            int count = this.builder.NumItems, newIndex;
            bool handled = true;
            switch (e.KeyCode)
            {
                case Keys.Up:
                    if (this.selectedIndex < 1)
                        break;
                    newIndex = this.selectedIndex - 1;
                    UpdateItems(newIndex, true);
                    break;
                case Keys.Down:
                    if (this.selectedIndex + 1 >= count)
                        break;
                    newIndex = this.selectedIndex + 1;
                    UpdateItems(newIndex, true);
                    break;
                case Keys.Enter:
                    DoItemSelected(null, new ItemSelectedEventArgs(this.SelectedItem));
                    break;
                default:
                    handled = false;
                    break;
            }
            e.Handled = handled;
        }
        
        public event ItemSelectedHandler ItemSelected;

        private void DoItemSelected(object sender, ItemSelectedEventArgs args)
        {
            if (this.builder.NumItems <= this.selectedIndex)
                return;
            if (this.ItemSelected != null)
                this.ItemSelected(this, args);
        }

        public override void Refresh()
        {
            this.txtItems.Controls.Clear();
            this.SelectedIndex = 0;
            base.Refresh();
        }

        private void UpdateItems(int newSelected, bool selectedMustBeVisible)
        {
            int scrollVal = this.vScrollBar1.Value, 
                count = this.builder.NumItems,
                loc = 0, i, 
                height = this.txtItems.Height,
                numControls = this.txtItems.Controls.Count;
            bool selectedVisible = false, thisVisible = GDIFunctions.IsTopWindow(this.TopLevelControl.Handle);

            if (selectedMustBeVisible && newSelected < scrollVal)
            {
                int j = 0;
                this.selectedIndex = newSelected;
                TxtViewer.TxtViewItem tempItem = null;
                for (i = this.selectedIndex; j < height; i--)
                {
                    if (i < 0)
                    {
                        this.vScrollBar1.Value = 0;
                        break;
                    }
                    if (tempItem == null)
                    {
                        tempItem = this.builder[i];
                        if (thisVisible)
                            tempItem.Selected = true;
                    }
                    else
                    {
                        this.builder.UpdateItem(tempItem, i);
                        if (thisVisible)
                            tempItem.Selected = false;
                    }
                    tempItem.UpdateHeight();
                    j += tempItem.Height;
                }
                if (i >= 0)
                    this.vScrollBar1.Value = i + 2;
                return;
            }

            int index = scrollVal;
            for (i = 0; loc < height; i++)
            {
                index = i + scrollVal;
                if (!(index < count))
                    break;
                TxtViewItem item;
                if (i < numControls)
                {
                    item = (TxtViewItem)this.txtItems.Controls[i];
                    if (item.Index != index)
                        item = this.builder.UpdateItem(item, index);
                }
                else
                {
                    item = this.builder[index];
                    if (item == null)
                        break;
                    this.txtItems.Controls.Add(item);
                }
                if (index == newSelected)
                {
                    if (thisVisible)
                        item.Selected = true;
                    item.UpdateHeight();
                    if (height - loc >= item.Height)
                        selectedVisible = true;
                }
                else
                {
                    if (thisVisible)
                        item.Selected = false;
                    item.UpdateHeight();
                }
                item.Top = loc;
                loc += item.Height;
            }
            this.vScrollBar1.LargeChange = i;
            while (i < this.txtItems.Controls.Count) // remove excess items
            {
                this.txtItems.Controls.RemoveAt(i);
            }

            this.selectedIndex = newSelected;
            if (!selectedVisible && selectedMustBeVisible)
            {
                this.vScrollBar1.Value = this.selectedIndex;
            }

        }

        private void vScrollBar1_ValueChanged(object sender, EventArgs e)
        {
            UpdateItems(this.selectedIndex, false);
        }

        private void TxtViewer_Resize(object sender, EventArgs e)
        {
            if (this.builder != null)
                UpdateItems(this.selectedIndex, false);
        }

    }
}
