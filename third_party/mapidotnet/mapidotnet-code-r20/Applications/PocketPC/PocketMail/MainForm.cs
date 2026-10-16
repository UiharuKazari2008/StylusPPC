using System;
using System.Linq;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using System.IO;
using MAPIdotnet;
using Microsoft.WindowsMobile.PocketOutlook;
using OpenNETCF.Runtime.InteropServices.ComTypes;
using OpenNETCF.Drawing;
using OpenNETCF.Drawing.Imaging;

namespace PocketMail
{
    public partial class MainForm : Form
    {
        private MAPI mapi;
        private IMAPIMsgStore[] stores;

        public MainForm()
        {
            InitializeComponent();
            this.mapi = new MAPI();
            this.stores = this.mapi.MessageStores;
            PopulateFolderTree();
        }

        private void PopulateFolderTree()
        {
            this.treeViewMain.Nodes.Clear();
            // Populate message stores
            for (int i = 0, length = this.stores.Length; i < length; i++)
            {
                IMAPIMsgStore store = this.stores[i];
                TreeNode n = new TreeNode(store.ToString());
                this.treeViewMain.Nodes.Add(n);
                // Recursively populate store folders
                PopulateFolders(store.RootFolder.OpenFolder(), n);
                // For this we'll register for every event
                /*store.MessageEvent += new MessageEventHandler(store_MessageEvent);
                store.NewMessage += new NewMessageEventHandler(store_NewMessage);
                store.FolderEvent += new FolderEventHandler(store_FolderEvent);*/
#warning Uncomment below line to receive events
                //store.EventNotifyMask = EEventMask.fnevNewMail | EEventMask.fnevObjectCreated | EEventMask.fnevObjectModified | EEventMask.fnevObjectMoved | EEventMask.fnevObjectCopied;
                //n.Expand();
            }
            this.treeViewMain.Focus();
        }

        private void PopulateFolders(IMAPIFolder folder, TreeNode node)
        {
            IMAPIFolderID[] subFolders = folder.GetSubFolders((int)folder.NumSubFolders);
            foreach (IMAPIFolderID fId in subFolders)
            {
                IMAPIFolder f = fId.OpenFolder();
                TreeNode newNode = new TreeNode(f.ToString() + " (" + f.NumSubItems.ToString() + " sub items)");
                newNode.Tag = fId;
                node.Nodes.Add(newNode);
                PopulateFolders(f, newNode);
            }
        }

        private void tabControl1_SelectedIndexChanged(object o, EventArgs e)
        {
            switch (this.tabControl1.SelectedIndex)
            {
                case 0:
                    this.treeViewMessages.Nodes.Clear();
                    this.treeViewMain.Focus();
                    break;

                case 1:
                    {
                        IMAPIFolderID fId = this.treeViewMain.SelectedNode.Tag as IMAPIFolderID;
                        if (fId == null)
                        {
                            this.tabControl1.SelectedIndex = 0;
                            return;
                        }
                        this.treeViewMessages.Nodes.Clear();
                        this.treeViewMessages.Nodes.Add("Populating nodes now...");
                        this.treeViewMessages.Focus();
                        this.Refresh();

                        IMAPIFolder folder = fId.OpenFolder();
                        this.treeViewMessages.Tag = folder;
                        // May as well show latest messages first:
                        folder.SortMessagesByDeliveryTime(TableSortOrder.TABLE_SORT_DESCEND);
                        // Get all the messages
                        IMAPIMessage[] messages = folder.GetNextMessages(folder.NumSubItems);

                        TreeNode[] nodes = new TreeNode[messages.Length];
                        for (int i = 0, length = messages.Length; i < length; i++)
                        {
                            IMAPIMessage msg = messages[i];
                            TreeNode node = new TreeNode(i.ToString());
                            node.Tag = msg;
                            msg.PopulateProperties(EMessageProperties.DeliveryTime | EMessageProperties.Sender | EMessageProperties.Subject);

                            // Sender:
                            IMAPIContact sender = msg.Sender;
                            TreeNode n;
                            if (sender != null)
                            {
                                n = new TreeNode("Sender: " + sender.FullAddress);
                                n.Tag = sender;
                                node.Nodes.Add(n);
                            }

                            // Subject:
                            node.Nodes.Add("Subject: " + msg.Subject);

                            // Delivery Date:
                            DateTime delivery = msg.LocalDeliveryTime;
                            if (delivery != null)
                            {
                                n = new TreeNode("Delivery date: " + delivery.ToString("H:mm d/MM/yy"));
                                n.Tag = delivery;
                                node.Nodes.Add(n);
                            }

                            node.Nodes.Add("Body: " + msg.Body);

                            // Recipients:
                            IMAPIContact[] recipients = msg.Recipients;
                            if (recipients.Length > 0)
                            {
                                TreeNode recipNode = new TreeNode("Recipients");
                                foreach (IMAPIContact recipient in recipients)
                                {
                                    n = new TreeNode(recipient.Name + " (" + recipient.FullAddress + ')');
                                    n.Tag = recipient;
                                    recipNode.Nodes.Add(n);
                                }
                                node.Nodes.Add(recipNode);
                            }

                            // Attachments:
                            IMAPIAttachment[] attachments = msg.Attachments;
                            if (attachments.Length > 0)
                            {
                                TreeNode attachNode = new TreeNode("Attachments");
                                foreach (IMAPIAttachment attachment in attachments)
                                {
                                    n = new TreeNode(attachment.FileName + " (" + attachment.Size.ToString() + " bytes)");
                                    n.Tag = attachment;
                                    attachNode.Nodes.Add(n);
                                }
                                node.Nodes.Add(attachNode);
                            }

                            nodes[i] = node;
                        }
                        this.treeViewMessages.Nodes.Add("Done populating, adding now...");
                        this.Refresh();
                        SuspendLayout();
                        this.treeViewMessages.Nodes.Clear();
                        this.treeViewMessages.Nodes.AddRange(nodes);
                        ResumeLayout();

                        if (this.treeViewMessages.Nodes.Count > 0)
                            this.treeViewMessages.SelectedNode = this.treeViewMessages.Nodes[0];
                        this.treeViewMessages.Focus();
                    }
                    break;
            }
        }

        private void menuItemExit_Click(object sender, EventArgs e)
        {
            Application.Exit();
        }

        private void menuItem1_Popup(object sender, EventArgs e)
        {
            if (this.tabControl1.SelectedIndex != 1)
                return;
            IMAPIAttachment a = this.treeViewMessages.SelectedNode.Tag as IMAPIAttachment;
            this.menuItemAttachImage.Enabled = a != null;
        }

        static public IBitmapImage CreateThumbnail(IStream stream, Size size)
        {
            IBitmapImage imageBitmap;
            ImageInfo ii;
            IImage image;
            ImagingFactory factory = new ImagingFactoryClass();
            factory.CreateImageFromStream(stream, out image);
            image.GetImageInfo(out ii);
            factory.CreateBitmapFromImage(
                image,
                (uint)size.Width,
                (uint)size.Height,
                ii.PixelFormat,
                InterpolationHint.InterpolationHintDefault,
                out imageBitmap);
            return imageBitmap;
        }

        private void menuItemAttachImage_Click(object sender, EventArgs e)
        {
            if (this.tabControl1.SelectedIndex != 1)
                return;
            IMAPIAttachment a = this.treeViewMessages.SelectedNode.Tag as IMAPIAttachment;
            if (a == null)
                return;
            int i = a.FileName.LastIndexOf('.');
            string type = a.FileName.Substring(i + 1);
            this.tabPageAttachment.Controls.Clear();
            try
            {
                switch (type)
                {
                    case "txt":
                        Stream s = a.OpenAttachment(false);
                        TextBox t = new TextBox();
                        t.Dock = DockStyle.Fill;
                        t.Multiline = true;
                        t.ReadOnly = true;
                        t.ScrollBars = ScrollBars.Both;
                        this.tabPageAttachment.Controls.Add(t);
                        t.Text = (new StreamReader(s)).ReadToEnd();
                        break;
                    case "jpg":
                    case "jpeg":
                        PictureBox pb = new PictureBox();
                        pb.Size = new Size(this.tabPageAttachment.Width, this.tabPageAttachment.Height);
                        pb.Location = new Point(0, 0);
                        IBitmapImage imageBitmap;
                        ImageInfo ii;
                        IImage image;
                        Size imgSize, viewSize = pb.Size; ;
                        ImagingFactory factory = new ImagingFactoryClass();
                        factory.CreateImageFromStream(a.OpenAttachmentNative(false), out image);
                        image.GetImageInfo(out ii);
                        image.GetPhysicalDimension(out imgSize);
                        // Have to fudge image size as all of .NET's ones don't hold the aspect ratio
                        float imgAspect = (float)imgSize.Height / (float)imgSize.Width;
                        float viewAspect = (float)viewSize.Height / (float)viewSize.Width;
                        if (imgAspect > viewAspect)
                            imgSize = new Size((int)((float)viewSize.Height / imgAspect), viewSize.Height);
                        else
                            imgSize = new Size(viewSize.Width, (int)(viewSize.Width * imgAspect));
                        factory.CreateBitmapFromImage(
                            image,
                            (uint)imgSize.Width,
                            (uint)imgSize.Height,
                            ii.PixelFormat,
                            InterpolationHint.InterpolationHintDefault,
                            out imageBitmap);

                        pb.Image = ImageUtils.IBitmapImageToBitmap(imageBitmap);
                        this.tabPageAttachment.Controls.Add(pb);
                        break;
                }
                this.tabControl1.SelectedIndex = 2;
            }
            catch {  }
        }

        private void menuItemCreateFolder_Click(object sender, EventArgs e)
        {
            IMAPIFolderID fId = this.treeViewMain.SelectedNode.Tag as IMAPIFolderID;
            if (fId == null)
                return;
            IMAPIFolder f = fId.OpenFolder();
            IMAPIFolder newF = f.CreateFolder(this.textBoxNewFolder.Text, true);
            PopulateFolderTree();
        }

        private void menuItemDeleteFolder_Click(object sender, EventArgs e)
        {
            IMAPIFolderID fId = this.treeViewMain.SelectedNode.Tag as IMAPIFolderID;
            if (fId == null)
                return;
            IMAPIFolder f = fId.OpenFolder();
            IMAPIFolderID parentId = f.ParentFolder;
            if (parentId == null)
                return;
            DeleteFolderResult res = parentId.OpenFolder().DeleteFolder(fId, false, false);
            if (res != DeleteFolderResult.Successful)
                MessageBox.Show("Error deleting: " + res.ToString());
            PopulateFolderTree();
        }

    }
}