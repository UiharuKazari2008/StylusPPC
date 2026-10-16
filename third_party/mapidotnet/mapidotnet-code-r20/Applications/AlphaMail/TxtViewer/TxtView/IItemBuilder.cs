using System;
using System.Collections.Generic;
using System.Text;

namespace TxtView
{
    public interface IItemBuilder
    {
        TxtViewer.TxtViewItem this[int index] { get; }

        TxtViewer.TxtViewItem UpdateItem(TxtViewer.TxtViewItem item, int index);

        ItemSelectedHandler SelectedHandler { set; }

        int NumItems { get; }

        TxtViewer Owner { set; get; }
    }
}
