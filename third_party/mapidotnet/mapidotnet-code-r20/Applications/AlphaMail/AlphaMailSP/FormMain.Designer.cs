using TxtView;

namespace AlphaMailSP
{
    partial class FormMain
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

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
            this.menuItemNew = new System.Windows.Forms.MenuItem();
            this.menuItemMenu = new System.Windows.Forms.MenuItem();
            this.menuItemDelete = new System.Windows.Forms.MenuItem();
            this.menuItemReply = new System.Windows.Forms.MenuItem();
            this.menuItemFolders = new System.Windows.Forms.MenuItem();
            this.menuItemAccounts = new System.Windows.Forms.MenuItem();
            this.menuItemMore = new System.Windows.Forms.MenuItem();
            this.menuItemEmptyFolder = new System.Windows.Forms.MenuItem();
            this.menuItemForward = new System.Windows.Forms.MenuItem();
            this.menuItemMove = new System.Windows.Forms.MenuItem();
            this.menuItemMarkAs = new System.Windows.Forms.MenuItem();
            this.menuItemDrafts = new System.Windows.Forms.MenuItem();
            this.menuItem2 = new System.Windows.Forms.MenuItem();
            this.menuItemNavigate = new System.Windows.Forms.MenuItem();
            this.menuItemFwd10 = new System.Windows.Forms.MenuItem();
            this.menuItemBack10 = new System.Windows.Forms.MenuItem();
            this.menuItemStart = new System.Windows.Forms.MenuItem();
            this.menuItemTools = new System.Windows.Forms.MenuItem();
            this.menuItemBackup = new System.Windows.Forms.MenuItem();
            this.menuItem1 = new System.Windows.Forms.MenuItem();
            this.menuItemExit = new System.Windows.Forms.MenuItem();
            this.SuspendLayout();
            // 
            // mainMenu1
            // 
            this.mainMenu1.MenuItems.Add(this.menuItemNew);
            this.mainMenu1.MenuItems.Add(this.menuItemMenu);
            // 
            // menuItemNew
            // 
            this.menuItemNew.Text = "New";
            this.menuItemNew.Click += new System.EventHandler(this.menuItemNew_Click);
            // 
            // menuItemMenu
            // 
            this.menuItemMenu.MenuItems.Add(this.menuItemDelete);
            this.menuItemMenu.MenuItems.Add(this.menuItemReply);
            this.menuItemMenu.MenuItems.Add(this.menuItemFolders);
            this.menuItemMenu.MenuItems.Add(this.menuItemAccounts);
            this.menuItemMenu.MenuItems.Add(this.menuItemMore);
            this.menuItemMenu.MenuItems.Add(this.menuItem2);
            this.menuItemMenu.MenuItems.Add(this.menuItemNavigate);
            this.menuItemMenu.MenuItems.Add(this.menuItemTools);
            this.menuItemMenu.MenuItems.Add(this.menuItem1);
            this.menuItemMenu.MenuItems.Add(this.menuItemExit);
            this.menuItemMenu.Text = "Menu";
            this.menuItemMenu.Popup += new System.EventHandler(this.menuItemMenu_Popup);
            // 
            // menuItemDelete
            // 
            this.menuItemDelete.Text = "Delete";
            this.menuItemDelete.Click += new System.EventHandler(this.menuItemDelete_Click);
            // 
            // menuItemReply
            // 
            this.menuItemReply.Text = "Reply";
            this.menuItemReply.Click += new System.EventHandler(this.menuItemReply_Click);
            // 
            // menuItemFolders
            // 
            this.menuItemFolders.Text = "Folders";
            // 
            // menuItemAccounts
            // 
            this.menuItemAccounts.Text = "Change Account";
            // 
            // menuItemMore
            // 
            this.menuItemMore.MenuItems.Add(this.menuItemEmptyFolder);
            this.menuItemMore.MenuItems.Add(this.menuItemForward);
            this.menuItemMore.MenuItems.Add(this.menuItemMove);
            this.menuItemMore.MenuItems.Add(this.menuItemMarkAs);
            this.menuItemMore.MenuItems.Add(this.menuItemDrafts);
            this.menuItemMore.Text = "More...";
            this.menuItemMore.Popup += new System.EventHandler(this.menuItemMore_Popup);
            // 
            // menuItemEmptyFolder
            // 
            this.menuItemEmptyFolder.Text = "Empty Folder";
            this.menuItemEmptyFolder.Click += new System.EventHandler(this.menuItemEmptyFolder_Click);
            // 
            // menuItemForward
            // 
            this.menuItemForward.Text = "Forward";
            this.menuItemForward.Click += new System.EventHandler(this.menuItemForward_Click);
            // 
            // menuItemMove
            // 
            this.menuItemMove.Text = "Move";
            // 
            // menuItemMarkAs
            // 
            this.menuItemMarkAs.Text = "Mark as";
            this.menuItemMarkAs.Click += new System.EventHandler(this.menuItemMarkAs_Click);
            // 
            // menuItemDrafts
            // 
            this.menuItemDrafts.Text = "Drafts...";
            // 
            // menuItem2
            // 
            this.menuItem2.Text = "-";
            // 
            // menuItemNavigate
            // 
            this.menuItemNavigate.MenuItems.Add(this.menuItemFwd10);
            this.menuItemNavigate.MenuItems.Add(this.menuItemBack10);
            this.menuItemNavigate.MenuItems.Add(this.menuItemStart);
            this.menuItemNavigate.Text = "Navigate";
            // 
            // menuItemFwd10
            // 
            this.menuItemFwd10.Text = "Forward 10";
            this.menuItemFwd10.Click += new System.EventHandler(this.menuItemFwd10_Click);
            // 
            // menuItemBack10
            // 
            this.menuItemBack10.Text = "Back 10";
            this.menuItemBack10.Click += new System.EventHandler(this.menuItemBack10_Click);
            // 
            // menuItemStart
            // 
            this.menuItemStart.Text = "&Start";
            this.menuItemStart.Click += new System.EventHandler(this.menuItemStart_Click);
            // 
            // menuItemTools
            // 
            this.menuItemTools.MenuItems.Add(this.menuItemBackup);
            this.menuItemTools.Text = "Tools";
            // 
            // menuItemBackup
            // 
            this.menuItemBackup.Text = "Backup";
            this.menuItemBackup.Click += new System.EventHandler(this.menuItemBackup_Click);
            // 
            // menuItem1
            // 
            this.menuItem1.Text = "-";
            // 
            // menuItemExit
            // 
            this.menuItemExit.Text = "Exit";
            this.menuItemExit.Click += new System.EventHandler(this.menuItemExit_Click);
            // 
            // FormMain
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Inherit;
            this.ClientSize = new System.Drawing.Size(320, 186);
            this.Menu = this.mainMenu1;
            this.Name = "FormMain";
            this.Text = "AlphaMail";
            this.Load += new System.EventHandler(this.FormMain_Load);
            this.GotFocus += new System.EventHandler(this.FormMain_GotFocus);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.MainMenu mainMenu1;
        private System.Windows.Forms.MenuItem menuItemNew;
        private System.Windows.Forms.MenuItem menuItemMenu;
        private System.Windows.Forms.MenuItem menuItemDelete;
        private System.Windows.Forms.MenuItem menuItemReply;
        private System.Windows.Forms.MenuItem menuItemFolders;
        private System.Windows.Forms.MenuItem menuItemAccounts;
        private TxtView.TxtViewer txtView;
        private System.Windows.Forms.MenuItem menuItem2;
        private System.Windows.Forms.MenuItem menuItemTools;
        private System.Windows.Forms.MenuItem menuItemBackup;
        private System.Windows.Forms.MenuItem menuItem1;
        private System.Windows.Forms.MenuItem menuItemExit;
        private System.Windows.Forms.MenuItem menuItemForward;
        private System.Windows.Forms.MenuItem menuItemMove;
        private System.Windows.Forms.MenuItem menuItemMarkAs;
        private System.Windows.Forms.MenuItem menuItemEmptyFolder;
        private System.Windows.Forms.MenuItem menuItemMore;
        private System.Windows.Forms.MenuItem menuItemDrafts;
        private System.Windows.Forms.MenuItem menuItemNavigate;
        private System.Windows.Forms.MenuItem menuItemFwd10;
        private System.Windows.Forms.MenuItem menuItemBack10;
        private System.Windows.Forms.MenuItem menuItemStart;
    }
}

