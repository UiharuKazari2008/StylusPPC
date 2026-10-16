namespace MailGuis
{
    partial class BackupMessages
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
            this.treeViewMain = new System.Windows.Forms.TreeView();
            this.labelStatus = new System.Windows.Forms.Label();
            this.mainMenu1 = new System.Windows.Forms.MainMenu();
            this.menuItemBackup = new System.Windows.Forms.MenuItem();
            this.menuItemClose = new System.Windows.Forms.MenuItem();
            this.SuspendLayout();
            // 
            // treeViewMain
            // 
            this.treeViewMain.Dock = System.Windows.Forms.DockStyle.Top;
            this.treeViewMain.Location = new System.Drawing.Point(0, 0);
            this.treeViewMain.Name = "treeViewMain";
            this.treeViewMain.Size = new System.Drawing.Size(176, 248);
            this.treeViewMain.TabIndex = 0;
            // 
            // labelStatus
            // 
            this.labelStatus.Dock = System.Windows.Forms.DockStyle.Bottom;
            this.labelStatus.Location = new System.Drawing.Point(0, 163);
            this.labelStatus.Name = "labelStatus";
            this.labelStatus.Size = new System.Drawing.Size(176, 17);
            this.labelStatus.Text = "None added yet...";
            // 
            // mainMenu1
            // 
            this.mainMenu1.MenuItems.Add(this.menuItemBackup);
            this.mainMenu1.MenuItems.Add(this.menuItemClose);
            // 
            // menuItemBackup
            // 
            this.menuItemBackup.Text = "Backup";
            this.menuItemBackup.Click += new System.EventHandler(this.menuItemBackup_Click);
            // 
            // menuItemClose
            // 
            this.menuItemClose.Text = "Close";
            this.menuItemClose.Click += new System.EventHandler(this.menuItemClose_Click);
            // 
            // BackupMessages
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.None;
            this.ClientSize = new System.Drawing.Size(176, 180);
            this.Controls.Add(this.labelStatus);
            this.Controls.Add(this.treeViewMain);
            this.Menu = this.mainMenu1;
            this.Name = "BackupMessages";
            this.Text = "Backup Messages";
            this.Resize += new System.EventHandler(this.BackupMessages_Resize);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.TreeView treeViewMain;
        private System.Windows.Forms.Label labelStatus;
        private System.Windows.Forms.MainMenu mainMenu1;
        private System.Windows.Forms.MenuItem menuItemBackup;
        private System.Windows.Forms.MenuItem menuItemClose;
    }
}