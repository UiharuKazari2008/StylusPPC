using System;
using System.Collections.Generic;
using System.Text;
using System.Windows.Forms;

namespace MailGuis
{
    public class TaggedMenuItem : MenuItem
    {
        private object tag = null;

        public TaggedMenuItem()
            : base()
        { }

        public TaggedMenuItem(object tag)
            : base()
        { Tag = tag; }

        public object Tag
        {
            set
            {
                this.tag = value;
                //this.Text = value.ToString();
            }
            get { return this.tag; }
        }
    }
}
