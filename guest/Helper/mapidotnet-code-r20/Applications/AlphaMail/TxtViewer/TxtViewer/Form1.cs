using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.IO;
using System.Windows.Forms;

namespace TxtViewer
{
    public partial class Form1 : Form
    {
        private int i = 10;

        public Form1()
        {
            InitializeComponent();
            for (int i = 0; i < 13; i++)
            {
                this.txtView1.AddItem(new TxtView.TxtView.TxtViewItem());
            }
        }

        private void menuItem1_Click(object sender, EventArgs e)
        {
            //this.txtView1.Icon = this.Icon;
        }

        private void menuItem2_Click(object sender, EventArgs e)
        {
            //this.txtView1.Items.Add(new TxtView.TxtDisplayItem("Bob" + i.ToString(), "Jimbo was here minta fag nut zaz sdafgsdgsdfgksdjfhgkljshdfgkjhsdkgljhsrthewirutywrt98356235235kjhkljhlkj235325", FontStyle.Regular, Color.Red));
            i++;
        }

        private void txtView1_ItemSelected(object sender, TxtView.ItemSelectedEventArgs args)
        {
            //MessageBox.Show(args.Item.Message, args.Item.Caption);
        }

        private void button1_Click(object sender, EventArgs e)
        {

        }

        private void txtView1_ItemSelected_1(object sender, TxtView.ItemSelectedEventArgs args)
        {
            MessageBox.Show("Item selected");
        }


    }
}