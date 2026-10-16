namespace PocketMail
{
    partial class MainForm
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;
        private System.Windows.Forms.MainMenu mainMenu1;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.mainMenu1 = new System.Windows.Forms.MainMenu();
            this.menuItem1 = new System.Windows.Forms.MenuItem();
            this.menuItemExit = new System.Windows.Forms.MenuItem();
            this.menuItemAttachImage = new System.Windows.Forms.MenuItem();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPageFolders = new System.Windows.Forms.TabPage();
            this.treeViewMain = new System.Windows.Forms.TreeView();
            this.tabPageMessages = new System.Windows.Forms.TabPage();
            this.treeViewMessages = new System.Windows.Forms.TreeView();
            this.tabPageAttachment = new System.Windows.Forms.TabPage();
            this.contextMenuFolders = new System.Windows.Forms.ContextMenu();
            this.menuItemCreateFolder = new System.Windows.Forms.MenuItem();
            this.label1 = new System.Windows.Forms.Label();
            this.textBoxNewFolder = new System.Windows.Forms.TextBox();
            this.menuItemDeleteFolder = new System.Windows.Forms.MenuItem();
            this.tabControl1.SuspendLayout();
            this.tabPageFolders.SuspendLayout();
            this.tabPageMessages.SuspendLayout();
            this.SuspendLayout();
            // 
            // mainMenu1
            // 
            this.mainMenu1.MenuItems.Add(this.menuItem1);
            // 
            // menuItem1
            // 
            this.menuItem1.MenuItems.Add(this.menuItemExit);
            this.menuItem1.MenuItems.Add(this.menuItemAttachImage);
            this.menuItem1.Text = "Menu";
            this.menuItem1.Popup += new System.EventHandler(this.menuItem1_Popup);
            // 
            // menuItemExit
            // 
            this.menuItemExit.Text = "Exit";
            this.menuItemExit.Click += new System.EventHandler(this.menuItemExit_Click);
            // 
            // menuItemAttachImage
            // 
            this.menuItemAttachImage.Text = "Attachment image";
            this.menuItemAttachImage.Click += new System.EventHandler(this.menuItemAttachImage_Click);
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPageFolders);
            this.tabControl1.Controls.Add(this.tabPageMessages);
            this.tabControl1.Controls.Add(this.tabPageAttachment);
            this.tabControl1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.tabControl1.Location = new System.Drawing.Point(0, 0);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(240, 268);
            this.tabControl1.TabIndex = 0;
            this.tabControl1.SelectedIndexChanged += new System.EventHandler(this.tabControl1_SelectedIndexChanged);
            // 
            // tabPageFolders
            // 
            this.tabPageFolders.Controls.Add(this.textBoxNewFolder);
            this.tabPageFolders.Controls.Add(this.label1);
            this.tabPageFolders.Controls.Add(this.treeViewMain);
            this.tabPageFolders.Location = new System.Drawing.Point(0, 0);
            this.tabPageFolders.Name = "tabPageFolders";
            this.tabPageFolders.Size = new System.Drawing.Size(240, 245);
            this.tabPageFolders.Text = "Folders";
            // 
            // treeViewMain
            // 
            this.treeViewMain.ContextMenu = this.contextMenuFolders;
            this.treeViewMain.Dock = System.Windows.Forms.DockStyle.Top;
            this.treeViewMain.Location = new System.Drawing.Point(0, 0);
            this.treeViewMain.Name = "treeViewMain";
            this.treeViewMain.Size = new System.Drawing.Size(240, 224);
            this.treeViewMain.TabIndex = 0;
            // 
            // tabPageMessages
            // 
            this.tabPageMessages.Controls.Add(this.treeViewMessages);
            this.tabPageMessages.Location = new System.Drawing.Point(0, 0);
            this.tabPageMessages.Name = "tabPageMessages";
            this.tabPageMessages.Size = new System.Drawing.Size(240, 245);
            this.tabPageMessages.Text = "Messages";
            // 
            // treeViewMessages
            // 
            this.treeViewMessages.Dock = System.Windows.Forms.DockStyle.Fill;
            this.treeViewMessages.Location = new System.Drawing.Point(0, 0);
            this.treeViewMessages.Name = "treeViewMessages";
            this.treeViewMessages.Size = new System.Drawing.Size(240, 245);
            this.treeViewMessages.TabIndex = 0;
            // 
            // tabPageAttachment
            // 
            this.tabPageAttachment.Location = new System.Drawing.Point(0, 0);
            this.tabPageAttachment.Name = "tabPageAttachment";
            this.tabPageAttachment.Size = new System.Drawing.Size(232, 242);
            this.tabPageAttachment.Text = "Attachment";
            // 
            // contextMenuFolders
            // 
            this.contextMenuFolders.MenuItems.Add(this.menuItemCreateFolder);
            this.contextMenuFolders.MenuItems.Add(this.menuItemDeleteFolder);
            // 
            // menuItemCreateFolder
            // 
            this.menuItemCreateFolder.Text = "Create Folder";
            this.menuItemCreateFolder.Click += new System.EventHandler(this.menuItemCreateFolder_Click);
            // 
            // label1
            // 
            this.label1.Location = new System.Drawing.Point(3, 226);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(78, 20);
            this.label1.Text = "New Folder:";
            // 
            // textBoxNewFolder
            // 
            this.textBoxNewFolder.Location = new System.Drawing.Point(87, 225);
            this.textBoxNewFolder.Name = "textBoxNewFolder";
            this.textBoxNewFolder.Size = new System.Drawing.Size(153, 21);
            this.textBoxNewFolder.TabIndex = 2;
            this.textBoxNewFolder.Text = "NewFolder";
            // 
            // menuItemDeleteFolder
            // 
            this.menuItemDeleteFolder.Text = "Delete Folder";
            this.menuItemDeleteFolder.Click += new System.EventHandler(this.menuItemDeleteFolder_Click);
            // 
            // MainForm
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Dpi;
            this.AutoScroll = true;
            this.ClientSize = new System.Drawing.Size(240, 268);
            this.Controls.Add(this.tabControl1);
            this.Menu = this.mainMenu1;
            this.Name = "MainForm";
            this.Text = "PocketMail";
            this.tabControl1.ResumeLayout(false);
            this.tabPageFolders.ResumeLayout(false);
            this.tabPageMessages.ResumeLayout(false);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPageFolders;
        private System.Windows.Forms.TabPage tabPageMessages;
        private System.Windows.Forms.TreeView treeViewMain;
        private System.Windows.Forms.TreeView treeViewMessages;
        private System.Windows.Forms.MenuItem menuItem1;
        private System.Windows.Forms.MenuItem menuItemExit;
        private System.Windows.Forms.MenuItem menuItemAttachImage;
        private System.Windows.Forms.TabPage tabPageAttachment;
        private System.Windows.Forms.ContextMenu contextMenuFolders;
        private System.Windows.Forms.MenuItem menuItemCreateFolder;
        private System.Windows.Forms.TextBox textBoxNewFolder;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.MenuItem menuItemDeleteFolder;


    }
}

