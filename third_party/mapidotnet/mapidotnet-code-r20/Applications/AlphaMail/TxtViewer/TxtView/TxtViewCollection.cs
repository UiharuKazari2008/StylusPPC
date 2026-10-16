using System;
using System.Collections.Generic;
using System.Text;

namespace TxtView
{
    public partial class TxtViewOld
    {
        public class TxtViewCollection
        {
            private List<TxtDisplayItem> items;
            private TxtViewOld parent;

            public TxtViewCollection(TxtViewOld parent) { this.items = new List<TxtDisplayItem>(); this.parent = parent; }

            public int Count { get { return this.items.Count; } }

            public TxtDisplayItem this[int i] { get { return this.items[i]; } }

            public void Add(TxtDisplayItem item)
            {
                this.items.Add(item);
                this.parent.NumItemsChanged();
            }

            public void Remove(int index)
            {
                this.items.RemoveAt(index);
                this.parent.NumItemsChanged();
            }

            public void Clear() { this.items.Clear(); this.parent.NumItemsChanged(); }
        }
    }
}
